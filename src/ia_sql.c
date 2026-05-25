/*
 * ia_sql.c — module entry point: GUC configuration, background-worker
 * registration, and small SQL-callable helpers.
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "ia_sql.h"

PG_MODULE_MAGIC;

/* ------------------------------------------------------------------ */
/* Default Layer-3 directives. Tunable live via ALTER SYSTEM + reload. */
/* ------------------------------------------------------------------ */
#define DEFAULT_WIKI_PROMPT \
"You are IA-SQL, a relentless knowledge-base compiler embedded in a database. " \
"Given a NEW SOURCE DOCUMENT and the CURRENT WIKI CONTEXT, synthesise the " \
"knowledge into a maintained wiki. Consolidate, resolve contradictions by " \
"citing sources, and never invent facts absent from the provided context. " \
"Detect the key entities/concepts and (re)write a hierarchical Markdown page " \
"for each. Reply with ONLY a JSON object of this exact shape, no prose, no " \
"code fences: {\"pages\":[{\"entity\":\"slug\",\"title\":\"...\"," \
"\"markdown\":\"...\",\"summary\":\"...\"}],\"edges\":[{\"source\":\"slug\"," \
"\"target\":\"slug\",\"relation\":\"...\",\"weight\":0.0}]}. " \
"Write page content in the same language as the source document."

#define DEFAULT_LINT_PROMPT \
"You are IA-SQL's auditor. Given a compiled WIKI PAGE and its SOURCE " \
"DOCUMENTS (ground truth), verify every claim in the page against the " \
"sources. Flag any statement that is unsupported, contradicted, or " \
"fabricated. Reply with ONLY a JSON object, no prose, no code fences: " \
"{\"flags\":[{\"severity\":\"low|medium|high\",\"description\":\"...\"}]}. " \
"If the page is fully supported, reply {\"flags\":[]}."

/* GUC backing storage */
char  *ia_sql_database = NULL;
bool   ia_sql_enabled = true;
int    ia_sql_poll_interval_ms = 1000;
char  *ia_sql_llm_base_url = NULL;
char  *ia_sql_llm_api_key = NULL;
char  *ia_sql_llm_model = NULL;
int    ia_sql_llm_timeout_ms = 120000;
double ia_sql_llm_temperature = 0.2;
int    ia_sql_llm_max_tokens = 4096;
char  *ia_sql_llm_extra_json = NULL;
char  *ia_sql_wiki_system_prompt = NULL;
char  *ia_sql_lint_system_prompt = NULL;
int    ia_sql_max_attempts = 3;

void _PG_init(void);

static void
ia_sql_define_gucs(void)
{
	DefineCustomStringVariable("ia_sql.database",
		"Database the IA-SQL worker connects to / BD a la que conecta el worker.",
		NULL, &ia_sql_database, "iasql",
		PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("ia_sql.enabled",
		"Enable the IA-SQL background compiler / Habilita el compilador en segundo plano.",
		NULL, &ia_sql_enabled, true,
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("ia_sql.poll_interval_ms",
		"Worker poll interval / Intervalo de sondeo del worker.",
		NULL, &ia_sql_poll_interval_ms, 1000, 100, 600000,
		PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomStringVariable("ia_sql.llm_base_url",
		"OpenAI-compatible base URL / URL base OpenAI-compatible.",
		NULL, &ia_sql_llm_base_url, "http://192.168.1.83:11435/v1",
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("ia_sql.llm_api_key",
		"Bearer API key, optional / Clave API Bearer, opcional.",
		NULL, &ia_sql_llm_api_key, "",
		PGC_SUSET, GUC_SUPERUSER_ONLY, NULL, NULL, NULL);

	DefineCustomStringVariable("ia_sql.llm_model",
		"Model name / Nombre del modelo.",
		NULL, &ia_sql_llm_model, "qwen3",
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("ia_sql.llm_timeout_ms",
		"LLM HTTP timeout / Tiempo límite HTTP del LLM.",
		NULL, &ia_sql_llm_timeout_ms, 120000, 1000, 1800000,
		PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomRealVariable("ia_sql.llm_temperature",
		"Sampling temperature / Temperatura de muestreo.",
		NULL, &ia_sql_llm_temperature, 0.2, 0.0, 2.0,
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("ia_sql.llm_max_tokens",
		"Max completion tokens / Máx. tokens de salida.",
		NULL, &ia_sql_llm_max_tokens, 4096, 16, 131072,
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("ia_sql.llm_extra_json",
		"Extra JSON merged into each request, for provider-specific options "
		"(e.g. {\"chat_template_kwargs\":{\"enable_thinking\":false}}) / JSON extra "
		"fusionado en cada request, para opciones del proveedor.",
		NULL, &ia_sql_llm_extra_json, "{}",
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("ia_sql.wiki_system_prompt",
		"Compiler directive, Layer 3 / Directiva del compilador, Capa 3.",
		NULL, &ia_sql_wiki_system_prompt, DEFAULT_WIKI_PROMPT,
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("ia_sql.lint_system_prompt",
		"Auditor directive / Directiva del auditor.",
		NULL, &ia_sql_lint_system_prompt, DEFAULT_LINT_PROMPT,
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("ia_sql.max_attempts",
		"Max job attempts before erroring / Máx. intentos antes de marcar error.",
		NULL, &ia_sql_max_attempts, 3, 1, 100,
		PGC_SIGHUP, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("ia_sql");
}

static void
ia_sql_register_worker(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 5;	/* seconds; restart if it crashes */
	snprintf(worker.bgw_name, BGW_MAXLEN, "ia_sql dispatcher");
	snprintf(worker.bgw_type, BGW_MAXLEN, "ia_sql");
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "ia_sql");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "ia_sql_worker_main");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&worker);
}

void
_PG_init(void)
{
	ia_sql_define_gucs();

	/* The dispatcher can only be registered from a preloaded library. */
	if (!process_shared_preload_libraries_in_progress)
		return;

	ia_sql_register_worker();
}

/* SQL: ia_wiki.version() -> text  (proves the module is linked) */
PG_FUNCTION_INFO_V1(ia_sql_version);
Datum
ia_sql_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text("IA-SQL 0.1"));
}
