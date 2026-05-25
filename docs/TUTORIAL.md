<!-- Language / Idioma: **English** · [Español](TUTORIAL.es.md) -->

# IA-SQL Tutorial — by user profile

This tutorial walks each kind of user through IA-SQL end to end. It assumes the extension
is installed (`make && sudo make install`) and PostgreSQL has
`shared_preload_libraries = 'pg_cron,ia_sql'`.

There are four profiles:

| Profile | Who | Can |
|---------|-----|-----|
| **Admin / DBA** | superuser | install, configure the LLM, manage roles, schedule audits |
| **Ingestor** | content provider | insert documents (Layer 1) |
| **Reader** | knowledge consumer | read the compiled wiki (Layer 2) |
| **Auditor** | quality / compliance | run audits, review & resolve hallucination flags |

Connections below use `psql`. Replace `HOST` with your server (e.g. a Tailscale IP) and use
each role's password.

---

## 1. Admin / DBA

**Install the extension and enable the worker.**

```sql
CREATE EXTENSION ia_sql;     -- creates schema ia_wiki, tables, functions
SELECT ia_wiki.version();    -- sanity check -> "IA-SQL 0.1"
```

**Point IA-SQL at your LLM** (any OpenAI-compatible endpoint):

```sql
ALTER SYSTEM SET ia_sql.llm_base_url = 'http://localhost:11434/v1';   -- Ollama, llama.cpp, vLLM, OpenAI…
ALTER SYSTEM SET ia_sql.llm_model    = 'qwen2.5';
-- Provider-specific tweaks (merged into every request):
ALTER SYSTEM SET ia_sql.llm_extra_json = '{"chat_template_kwargs":{"enable_thinking":false}}'; -- Qwen3
-- For a hosted API:
-- ALTER SYSTEM SET ia_sql.llm_api_key = 'sk-...';
SELECT pg_reload_conf();
```

**Tune the compiler's behaviour** (Layer 3 directives) live, without downtime:

```sql
ALTER SYSTEM SET ia_sql.wiki_system_prompt = 'You are a meticulous wiki compiler …';
SELECT pg_reload_conf();
```

**Create the user roles** (see `sql/roles.example.sql`; change the passwords):

```sql
\i sql/roles.example.sql
```

**Schedule the nightly audit** (requires pg_cron):

```sql
SELECT cron.schedule('ia_sql_lint', '0 3 * * *', 'SELECT ia_wiki.enqueue_lint(20)');
```

**Monitor the engine.**

```sql
SELECT * FROM pg_stat_activity WHERE backend_type = 'ia_sql';   -- worker alive?
SELECT status, count(*) FROM ia_wiki.jobs GROUP BY status;      -- queue health
SELECT job_id, model, prompt_tokens, completion_tokens, latency_ms
  FROM ia_wiki.processing_log ORDER BY id DESC LIMIT 10;        -- cost / latency
```

Pause/resume the worker any time: `ALTER SYSTEM SET ia_sql.enabled = off; SELECT pg_reload_conf();`

---

## 2. Ingestor — feed the knowledge base

You only do one thing: insert raw documents. Compilation happens automatically in the
background; your `INSERT` returns immediately.

```bash
psql "host=HOST user=iasql_ingestor dbname=iasql"
```
```sql
INSERT INTO ia_wiki.raw_documents (source, content) VALUES
  ('meeting-2026-05-20', 'Decision: we will standardise on PostgreSQL 17 for all services …');

-- Watch your document get compiled:
SELECT job_id, kind, status FROM ia_wiki.jobs ORDER BY job_id DESC LIMIT 5;
```

Notes:
- Ground truth is **append-only**: you cannot `UPDATE`/`DELETE` `raw_documents` (by design).
  To correct information, insert a new document that states the correction — the compiler
  will reconcile it.
- You cannot read or modify the wiki; that is the Reader's and the engine's job.

---

## 3. Reader — use the compiled wiki

```bash
psql "host=HOST user=iasql_reader dbname=iasql"
```
```sql
-- Browse pages
SELECT page_entity, title, summary, last_compiled FROM ia_wiki.pages;

-- Read a page
SELECT markdown_body FROM ia_wiki.compiled_pages WHERE page_entity = 'postgresql_architecture';

-- Explore the knowledge graph
SELECT source_entity, relation, target_entity, weight FROM ia_wiki.entity_graph;

-- Find pages related to a concept
SELECT target_entity, relation FROM ia_wiki.entity_graph WHERE source_entity = 'ia_sql';
```

The wiki is precompiled, so reads are plain, fast SQL — no model call at query time.

---

## 4. Auditor — keep the wiki honest

The auditor checks compiled pages against their source documents and manages the resulting
flags.

```bash
psql "host=HOST user=iasql_auditor dbname=iasql"
```
```sql
-- Trigger an audit of the N oldest pages (or rely on the nightly pg_cron job)
SELECT ia_wiki.enqueue_lint(20);

-- Review open findings
SELECT id, page_entity, severity, description
  FROM ia_wiki.hallucination_flags
  WHERE NOT resolved ORDER BY severity DESC, id;

-- Resolve a finding after acting on it
UPDATE ia_wiki.hallucination_flags SET resolved = true WHERE id = 42;
```

Each flag links a claim in a page to the absence of supporting evidence in Layer 1, with a
severity (`low` / `medium` / `high`). A high-severity flag usually means the compiler
introduced an unsupported statement — feed a corrective document (as an Ingestor) and the
page will be rewritten.

---

## End-to-end, in one minute

```sql
-- as ingestor
INSERT INTO ia_wiki.raw_documents (content)
VALUES ('The capital of France is Paris. The Eiffel Tower opened in 1889.');
-- wait a few seconds…
-- as reader
SELECT markdown_body FROM ia_wiki.compiled_pages;     -- a compiled page about Paris/France
-- as auditor
SELECT ia_wiki.enqueue_lint(5);
SELECT * FROM ia_wiki.hallucination_flags;            -- should be empty for grounded facts
```
