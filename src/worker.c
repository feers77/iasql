/*
 * worker.c — the IA-SQL dispatcher background worker.
 *
 * One long-lived worker polls the ia_wiki.jobs queue and processes jobs one at
 * a time:
 *   Tx1  claim a pending job (FOR UPDATE SKIP LOCKED) + read its document and
 *        the current wiki context, then COMMIT (so no transaction is held open
 *        during the slow network call);
 *   HTTP call the external OpenAI-compatible LLM (no transaction);
 *   Tx2  write the compiled pages/edges, telemetry, and mark the job done.
 * Failures are recorded on the job row with a bounded retry.
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "ia_sql.h"
#include "http_client.h"
#include "llm.h"

/* ------------------------------------------------------------------ */
/* Small SQL helpers (run inside an open transaction with SPI)         */
/* ------------------------------------------------------------------ */

static bool
schema_present(void)
{
	bool		present = false;

	if (SPI_execute("SELECT to_regclass('ia_wiki.jobs') IS NOT NULL", true, 1) == SPI_OK_SELECT
		&& SPI_processed == 1)
	{
		bool		isnull;
		Datum		d = SPI_getbinval(SPI_tuptable->vals[0],
									  SPI_tuptable->tupdesc, 1, &isnull);

		present = !isnull && DatumGetBool(d);
	}
	return present;
}

static char *
copy_col(MemoryContext ctx, HeapTuple tup, TupleDesc td, int col)
{
	char	   *v = SPI_getvalue(tup, td, col);
	char	   *out = NULL;

	if (v != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(ctx);

		out = pstrdup(v);
		MemoryContextSwitchTo(old);
	}
	return out;
}

static void
upsert_page(const char *entity, const char *title, const char *markdown,
			const char *summary, const char *doc_id_text)
{
	Oid			argtypes[5] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID};
	Datum		values[5];
	char		nulls[5] = {' ', ' ', ' ', ' ', ' '};

	values[0] = CStringGetTextDatum(entity);
	if (title)		 values[1] = CStringGetTextDatum(title);		else nulls[1] = 'n';
	if (markdown)	 values[2] = CStringGetTextDatum(markdown);		else nulls[2] = 'n';
	if (summary)	 values[3] = CStringGetTextDatum(summary);		else nulls[3] = 'n';
	if (doc_id_text) values[4] = CStringGetTextDatum(doc_id_text);	else nulls[4] = 'n';

	SPI_execute_with_args(
		"INSERT INTO ia_wiki.compiled_pages "
		"  (page_entity, title, markdown_body, summary, source_doc_ids, last_compiled) "
		"VALUES ($1, $2, $3, $4, "
		"        CASE WHEN $5 IS NULL THEN '{}'::uuid[] ELSE ARRAY[$5::uuid] END, now()) "
		"ON CONFLICT (page_entity) DO UPDATE SET "
		"  title = EXCLUDED.title, "
		"  markdown_body = EXCLUDED.markdown_body, "
		"  summary = EXCLUDED.summary, "
		"  source_doc_ids = (SELECT ARRAY(SELECT DISTINCT e FROM unnest("
		"      ia_wiki.compiled_pages.source_doc_ids || EXCLUDED.source_doc_ids) e)), "
		"  last_compiled = now()",
		5, argtypes, values, nulls, false, 0);
}

static void
upsert_edge(const char *src, const char *tgt, const char *rel, double weight)
{
	Oid			argtypes[4] = {TEXTOID, TEXTOID, TEXTOID, FLOAT8OID};
	Datum		values[4];
	char		nulls[4] = {' ', ' ', ' ', ' '};

	values[0] = CStringGetTextDatum(src);
	values[1] = CStringGetTextDatum(tgt);
	if (rel) values[2] = CStringGetTextDatum(rel); else nulls[2] = 'n';
	values[3] = Float8GetDatum(weight);

	SPI_execute_with_args(
		"INSERT INTO ia_wiki.entity_graph (source_entity, target_entity, relation, weight) "
		"VALUES ($1, $2, COALESCE($3, 'related'), $4) "
		"ON CONFLICT (source_entity, target_entity, relation) "
		"DO UPDATE SET weight = EXCLUDED.weight",
		4, argtypes, values, nulls, false, 0);
}

static void
mark_done(int64 job_id)
{
	Oid			argtypes[1] = {INT8OID};
	Datum		values[1] = {Int64GetDatum(job_id)};

	SPI_execute_with_args(
		"UPDATE ia_wiki.jobs SET status='done', finished_at=now() WHERE job_id=$1",
		1, argtypes, values, NULL, false, 0);
}

static void
log_processing(const char *doc_id_text, int64 job_id, const char *model,
			   int prompt_tokens, int completion_tokens, int latency_ms)
{
	Oid			argtypes[6] = {TEXTOID, INT8OID, TEXTOID, INT4OID, INT4OID, INT4OID};
	Datum		values[6];
	char		nulls[6] = {' ', ' ', ' ', ' ', ' ', ' '};

	if (doc_id_text) values[0] = CStringGetTextDatum(doc_id_text); else nulls[0] = 'n';
	values[1] = Int64GetDatum(job_id);
	values[2] = CStringGetTextDatum(model);
	values[3] = Int32GetDatum(prompt_tokens);
	values[4] = Int32GetDatum(completion_tokens);
	values[5] = Int32GetDatum(latency_ms);

	SPI_execute_with_args(
		"INSERT INTO ia_wiki.processing_log "
		"  (doc_id, job_id, model, prompt_tokens, completion_tokens, latency_ms) "
		"VALUES ($1::uuid, $2, $3, $4, $5, $6)",
		6, argtypes, values, nulls, false, 0);
}

static void
insert_flag(const char *page_entity, const char *severity, const char *description)
{
	Oid			argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		values[3];
	char		nulls[3] = {' ', ' ', ' '};

	values[0] = CStringGetTextDatum(page_entity);
	if (severity) values[1] = CStringGetTextDatum(severity); else nulls[1] = 'n';
	values[2] = CStringGetTextDatum(description);

	SPI_execute_with_args(
		"INSERT INTO ia_wiki.hallucination_flags (page_entity, severity, description) "
		"VALUES ($1, CASE WHEN $2 IN ('low','medium','high') THEN $2 ELSE 'medium' END, $3)",
		3, argtypes, values, nulls, false, 0);
}

static void
mark_error_spi(int64 job_id, const char *message)
{
	Oid			argtypes[3] = {INT8OID, INT4OID, TEXTOID};
	Datum		values[3];
	char		nulls[3] = {' ', ' ', ' '};

	values[0] = Int64GetDatum(job_id);
	values[1] = Int32GetDatum(ia_sql_max_attempts);
	if (message) values[2] = CStringGetTextDatum(message); else nulls[2] = 'n';

	SPI_execute_with_args(
		"UPDATE ia_wiki.jobs SET "
		"  status = CASE WHEN attempts >= $2 THEN 'error' ELSE 'pending' END, "
		"  error = $3, "
		"  finished_at = CASE WHEN attempts >= $2 THEN now() ELSE NULL END "
		"WHERE job_id = $1",
		3, argtypes, values, nulls, false, 0);
}

/* Record a job failure in its own transaction. */
static void
record_job_error(int64 job_id, const char *message)
{
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	mark_error_spi(job_id, message);
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
}

/* ------------------------------------------------------------------ */
/* Job processing                                                      */
/* ------------------------------------------------------------------ */

static bool
process_one_job(MemoryContext jobctx)
{
	int64		job_id = 0;
	char	   *doc_id = NULL;
	char	   *kind = NULL;
	char	   *content = NULL;
	char	   *wiki_ctx = NULL;
	bool		claimed = false;

	/* ---- Tx1: claim a job, read its document and the wiki context ---- */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	if (!schema_present())
	{
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	if (SPI_execute(
			"WITH c AS ("
			"  SELECT job_id FROM ia_wiki.jobs WHERE status='pending' "
			"  ORDER BY job_id FOR UPDATE SKIP LOCKED LIMIT 1) "
			"UPDATE ia_wiki.jobs j SET status='running', started_at=now(), "
			"       attempts=attempts+1 "
			"FROM c WHERE j.job_id=c.job_id "
			"RETURNING j.job_id, j.doc_id::text, j.kind, "
			"  (SELECT content FROM ia_wiki.raw_documents r WHERE r.doc_id=j.doc_id)",
			false, 1) == SPI_OK_UPDATE_RETURNING && SPI_processed == 1)
	{
		HeapTuple	tup = SPI_tuptable->vals[0];
		TupleDesc	td = SPI_tuptable->tupdesc;
		bool		isnull;

		job_id = DatumGetInt64(SPI_getbinval(tup, td, 1, &isnull));
		doc_id = copy_col(jobctx, tup, td, 2);
		kind = copy_col(jobctx, tup, td, 3);
		content = copy_col(jobctx, tup, td, 4);
		claimed = true;

		if (kind && strcmp(kind, "ingest") == 0 &&
			SPI_execute(
				"SELECT COALESCE(string_agg('- ' || page_entity || ': ' "
				"  || COALESCE(summary,''), E'\n'), '') "
				"FROM (SELECT page_entity, summary FROM ia_wiki.compiled_pages "
				"      ORDER BY last_compiled DESC LIMIT 50) s",
				true, 1) == SPI_OK_SELECT && SPI_processed == 1)
			wiki_ctx = copy_col(jobctx, SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	if (!claimed)
		return false;

	/* ---- ingest: call the LLM, then write the result ---- */
	if (kind && strcmp(kind, "ingest") == 0)
	{
		LlmResult	r;
		char	   *err = NULL;
		TimestampTz t0;
		int			latency_ms;
		bool		ok;

		MemoryContextSwitchTo(jobctx);
		t0 = GetCurrentTimestamp();
		ok = ia_llm_ingest(doc_id, content, wiki_ctx, &r, &err);
		latency_ms = (int) ((GetCurrentTimestamp() - t0) / 1000);

		if (!ok)
		{
			record_job_error(job_id, err ? err : "ingest failed");
			ereport(LOG, (errmsg("ia_sql: ingest job %lld failed: %s",
								 (long long) job_id, err ? err : "?")));
			return true;
		}

		PG_TRY();
		{
			int			i;

			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());

			for (i = 0; i < r.n_pages; i++)
				upsert_page(r.pages[i].entity, r.pages[i].title,
							r.pages[i].markdown, r.pages[i].summary, doc_id);
			for (i = 0; i < r.n_edges; i++)
				upsert_edge(r.edges[i].source, r.edges[i].target,
							r.edges[i].relation, r.edges[i].weight);

			log_processing(doc_id, job_id, ia_sql_llm_model,
						   r.prompt_tokens, r.completion_tokens, latency_ms);
			mark_done(job_id);

			SPI_finish();
			PopActiveSnapshot();
			CommitTransactionCommand();

			ereport(LOG, (errmsg("ia_sql: ingest job %lld -> %d page(s), %d edge(s), "
								 "%dms, tokens %d/%d",
								 (long long) job_id, r.n_pages, r.n_edges,
								 latency_ms, r.prompt_tokens, r.completion_tokens)));
		}
		PG_CATCH();
		{
			ErrorData  *ed;

			MemoryContextSwitchTo(jobctx);
			ed = CopyErrorData();
			FlushErrorState();
			AbortOutOfAnyTransaction();
			record_job_error(job_id, ed->message);
			ereport(LOG, (errmsg("ia_sql: writing ingest job %lld failed: %s",
								 (long long) job_id, ed->message)));
			FreeErrorData(ed);
		}
		PG_END_TRY();
	}
	/* ---- lint: audit a page against its sources ---- */
	else if (kind && strcmp(kind, "lint") == 0)
	{
		LlmResult	r;
		char	   *err = NULL;
		char	   *page_entity = NULL;
		char	   *page_md = NULL;
		char	   *sources = NULL;
		bool		ok;
		bool		have_page = false;

		/* The lint job's target page_entity travels in payload->>'page_entity'. */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		if (SPI_execute_with_args(
				"SELECT cp.page_entity, cp.markdown_body, "
				"  COALESCE(string_agg(rd.content, E'\n---\n'), '') "
				"FROM ia_wiki.jobs j "
				"JOIN ia_wiki.compiled_pages cp ON cp.page_entity = j.payload->>'page_entity' "
				"LEFT JOIN ia_wiki.raw_documents rd ON rd.doc_id = ANY(cp.source_doc_ids) "
				"WHERE j.job_id = $1 GROUP BY cp.page_entity, cp.markdown_body",
				1, (Oid[]){INT8OID}, (Datum[]){Int64GetDatum(job_id)}, NULL,
				true, 1) == SPI_OK_SELECT && SPI_processed == 1)
		{
			page_entity = copy_col(jobctx, SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
			page_md = copy_col(jobctx, SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2);
			sources = copy_col(jobctx, SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3);
			have_page = true;
		}
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();

		if (!have_page)
		{
			/* nothing to audit (page gone) — just close the job */
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());
			mark_done(job_id);
			SPI_finish();
			PopActiveSnapshot();
			CommitTransactionCommand();
			return true;
		}

		MemoryContextSwitchTo(jobctx);
		ok = ia_llm_lint(page_entity, page_md, sources, &r, &err);
		if (!ok)
		{
			record_job_error(job_id, err ? err : "lint failed");
			return true;
		}

		PG_TRY();
		{
			int			i;

			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());
			for (i = 0; i < r.n_flags; i++)
				insert_flag(page_entity, r.flags[i].severity, r.flags[i].description);
			mark_done(job_id);
			SPI_finish();
			PopActiveSnapshot();
			CommitTransactionCommand();
			ereport(LOG, (errmsg("ia_sql: lint job %lld -> %d flag(s) on '%s'",
								 (long long) job_id, r.n_flags, page_entity)));
		}
		PG_CATCH();
		{
			ErrorData  *ed;

			MemoryContextSwitchTo(jobctx);
			ed = CopyErrorData();
			FlushErrorState();
			AbortOutOfAnyTransaction();
			record_job_error(job_id, ed->message);
			FreeErrorData(ed);
		}
		PG_END_TRY();
	}
	else
	{
		/* unknown kind: close it out so the queue does not stall */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		mark_done(job_id);
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	return true;
}

static void
process_pending_jobs(void)
{
	MemoryContext jobctx = AllocSetContextCreate(TopMemoryContext,
												 "ia_sql job",
												 ALLOCSET_DEFAULT_SIZES);

	while (!ShutdownRequestPending && process_one_job(jobctx))
		MemoryContextReset(jobctx);

	MemoryContextDelete(jobctx);
}

static void
requeue_stale_jobs(void)
{
	PG_TRY();
	{
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		if (schema_present())
			SPI_execute("UPDATE ia_wiki.jobs SET status='pending' WHERE status='running'",
						false, 0);
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		FlushErrorState();
		AbortOutOfAnyTransaction();
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------ */
/* Worker entry point                                                  */
/* ------------------------------------------------------------------ */
void
ia_sql_worker_main(Datum main_arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(ia_sql_database, NULL, 0);
	ia_http_global_init();

	ereport(LOG, (errmsg("ia_sql dispatcher started (db=%s)", ia_sql_database)));

	requeue_stale_jobs();

	while (!ShutdownRequestPending)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ia_sql_poll_interval_ms,
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (ia_sql_enabled)
			process_pending_jobs();
	}

	ia_http_global_cleanup();
	ereport(LOG, (errmsg("ia_sql dispatcher shutting down")));
	proc_exit(0);
}
