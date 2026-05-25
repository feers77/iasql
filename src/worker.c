/*
 * worker.c — the IA-SQL dispatcher background worker.
 *
 * One long-lived worker polls the ia_wiki.jobs queue and processes jobs one at
 * a time. Each job is claimed in its own transaction (FOR UPDATE SKIP LOCKED so
 * the design is safe even with multiple workers later), then processed, then the
 * result is written in a second transaction. This keeps transactions short and
 * never blocks the client that inserted the document.
 *
 * Phase 3: the "compile" step is a clearly-marked stub. Phase 4 replaces it with
 * a real call to an external OpenAI-compatible LLM.
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
#include "utils/wait_event.h"

#include "ia_sql.h"

/* ------------------------------------------------------------------ */
/* SQL helpers                                                         */
/* ------------------------------------------------------------------ */

/* True if the ia_wiki schema (CREATE EXTENSION) has been installed yet. */
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

/* Copy a (possibly NULL) text column out of the current SPI tuple into ctx. */
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

/* Insert-or-update one wiki page, merging provenance. */
static void
upsert_page(const char *entity, const char *title, const char *markdown,
			const char *summary, const char *doc_id_text)
{
	Oid			argtypes[5] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID};
	Datum		values[5];
	char		nulls[5] = {' ', ' ', ' ', ' ', ' '};

	values[0] = CStringGetTextDatum(entity);
	if (title)		values[1] = CStringGetTextDatum(title);		else nulls[1] = 'n';
	if (markdown)	values[2] = CStringGetTextDatum(markdown);	else nulls[2] = 'n';
	if (summary)	values[3] = CStringGetTextDatum(summary);	else nulls[3] = 'n';
	if (doc_id_text) values[4] = CStringGetTextDatum(doc_id_text); else nulls[4] = 'n';

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

/* Mark a job done. */
static void
mark_done(int64 job_id)
{
	Oid			argtypes[1] = {INT8OID};
	Datum		values[1] = {Int64GetDatum(job_id)};

	SPI_execute_with_args(
		"UPDATE ia_wiki.jobs SET status='done', finished_at=now() WHERE job_id=$1",
		1, argtypes, values, NULL, false, 0);
}

/* Record telemetry for one compilation. */
static void
log_processing(const char *doc_id_text, int64 job_id, const char *model, int latency_ms)
{
	Oid			argtypes[4] = {TEXTOID, INT8OID, TEXTOID, INT4OID};
	Datum		values[4];
	char		nulls[4] = {' ', ' ', ' ', ' '};

	if (doc_id_text) values[0] = CStringGetTextDatum(doc_id_text); else nulls[0] = 'n';
	values[1] = Int64GetDatum(job_id);
	values[2] = CStringGetTextDatum(model);
	values[3] = Int32GetDatum(latency_ms);

	SPI_execute_with_args(
		"INSERT INTO ia_wiki.processing_log (doc_id, job_id, model, latency_ms) "
		"VALUES ($1::uuid, $2, $3, $4)",
		4, argtypes, values, nulls, false, 0);
}

/* On error: requeue (if attempts remain) or mark error. */
static void
mark_error(int64 job_id, const char *message)
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

/* ------------------------------------------------------------------ */
/* Job processing                                                      */
/* ------------------------------------------------------------------ */

/*
 * Claim and process the next pending job. Returns true if a job was handled
 * (so the caller keeps looping), false if the queue is empty.
 */
static bool
process_one_job(MemoryContext jobctx)
{
	int64		job_id = 0;
	char	   *doc_id = NULL;
	char	   *kind = NULL;
	char	   *content = NULL;
	bool		claimed = false;

	/* ---- Transaction 1: claim a pending job and fetch its document ---- */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	if (!schema_present())
	{
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;			/* extension not installed yet */
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
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	if (!claimed)
		return false;

	/* ---- Transaction 2: process + write result (with error handling) ---- */
	PG_TRY();
	{
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());

		if (kind && strcmp(kind, "ingest") == 0)
		{
			/* ===== Phase 3 STUB compiler (replaced by the LLM in Phase 4) ===== */
			StringInfoData md;
			char		entity[64];
			char	   *title;
			char	   *summary;

			snprintf(entity, sizeof(entity), "stub-%lld", (long long) job_id);

			initStringInfo(&md);
			appendStringInfoString(&md,
				"**(stub compilation — Phase 3, no LLM yet)**\n\n");
			appendStringInfoString(&md, content ? content : "(empty document)");

			title = pnstrdup(content ? content : "(empty)",
							 content ? Min(60, (int) strlen(content)) : 7);
			summary = pnstrdup(content ? content : "(empty)",
							   content ? Min(160, (int) strlen(content)) : 7);

			upsert_page(entity, title, md.data, summary, doc_id);
			log_processing(doc_id, job_id, "(stub)", 0);
			pfree(md.data);
		}
		/* 'lint' handled in Phase 5 */

		mark_done(job_id);

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(jobctx);
		ed = CopyErrorData();
		FlushErrorState();
		AbortOutOfAnyTransaction();

		/* Record the failure in a fresh transaction. */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		mark_error(job_id, ed->message);
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();

		ereport(LOG,
				(errmsg("ia_sql: job %lld failed: %s",
						(long long) job_id, ed->message)));
		FreeErrorData(ed);
	}
	PG_END_TRY();

	return true;
}

/* Process every pending job, then return. */
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

/* Reset jobs left 'running' by a previous (crashed) worker back to 'pending'. */
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

	ereport(LOG, (errmsg("ia_sql dispatcher shutting down")));
	proc_exit(0);
}
