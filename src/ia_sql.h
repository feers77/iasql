/*
 * ia_sql.h — shared declarations for the IA-SQL extension.
 *
 * IA-SQL implements Andrej Karpathy's "LLM Wiki" pattern inside PostgreSQL:
 * documents are append-only ground truth, a background worker calls an external
 * OpenAI-compatible LLM to compile a maintained Markdown wiki, and a lint pass
 * audits the wiki against its sources.
 */
#ifndef IA_SQL_H
#define IA_SQL_H

#include "postgres.h"

/* ---- Configuration (GUCs), defined in ia_sql.c ---- */
extern char  *ia_sql_database;            /* DB the worker connects to        */
extern bool   ia_sql_enabled;             /* master on/off switch             */
extern int    ia_sql_poll_interval_ms;    /* worker poll cadence              */
extern char  *ia_sql_llm_base_url;        /* OpenAI-compatible base URL        */
extern char  *ia_sql_llm_api_key;         /* optional bearer key              */
extern char  *ia_sql_llm_model;           /* model name                       */
extern int    ia_sql_llm_timeout_ms;      /* HTTP timeout                     */
extern double ia_sql_llm_temperature;     /* sampling temperature             */
extern int    ia_sql_llm_max_tokens;      /* max completion tokens            */
extern char  *ia_sql_wiki_system_prompt;  /* Layer 3: compiler directive      */
extern char  *ia_sql_lint_system_prompt;  /* Layer 3: auditor directive       */
extern int    ia_sql_max_attempts;        /* retries before a job errors out  */

/* ---- Background worker entry point (worker.c) ---- */
extern PGDLLEXPORT void ia_sql_worker_main(Datum main_arg);

#endif							/* IA_SQL_H */
