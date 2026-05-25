<!-- Language / Idioma: **English** · [Español](README.es.md) -->

# IA-SQL

**Turn PostgreSQL into a self-compiling knowledge base.**
An in-database implementation of Andrej Karpathy's *"LLM Wiki"* pattern: you `INSERT`
raw documents, and a background worker uses an external LLM to **compile** them into a
maintained, cross-referenced Markdown wiki — then keeps auditing that wiki against the
sources for hallucinations.

> Status: **0.1 — working proof of concept.** Built and tested on PostgreSQL 17.
> [Español aquí →](README.es.md)

🌐 **Landing: https://feers77.github.io/iasql** · **Live wiki demo: https://iasql.dev.feres.cl**
📖 **New here? Start with the [tutorial by user profile](docs/TUTORIAL.md).**

---

## The idea

Most "chat with your documents" systems use **RAG**: at query time they retrieve a few
text chunks and ask the model to improvise an answer. The model re-discovers your domain
from scratch on every question and never accumulates understanding.

Karpathy's **LLM Wiki** pattern flips this around, borrowing a metaphor from software:

| Software        | Knowledge base                                  |
|-----------------|-------------------------------------------------|
| Source code     | Raw documents (immutable ground truth)          |
| Compiler        | The LLM                                          |
| Compiled binary | A maintained Markdown wiki (synthesised, linked) |

The expensive work happens at **ingest time**, not query time. When a new document
arrives, the LLM reads it once, decides which entities/pages it affects, and rewrites
those pages — consolidating, resolving contradictions, and adding cross-references.
Knowledge **compounds**: each document makes the whole wiki better.

**IA-SQL puts this loop inside PostgreSQL.** The database is no longer a passive store; it
metabolises new information asynchronously and audits its own consistency, while normal
queries keep running.

## How it works

```
INSERT INTO ia_wiki.raw_documents (content)         -- Layer 1: append-only ground truth
        │  AFTER INSERT trigger (O(1): enqueue + NOTIFY, never blocks)
        ▼
   ia_wiki.jobs  (pending)
        │  ia_sql dispatcher (background worker)
        │   claim FOR UPDATE SKIP LOCKED  →  call external LLM  →  write result
        ▼
   ia_wiki.compiled_pages + entity_graph             -- Layer 2: the wiki (LLM-owned)
        ▲
        │  pg_cron nightly  →  ia_wiki.enqueue_lint()
        ▼
   ia_wiki.hallucination_flags                       -- self-audit against Layer 1
```

The three layers of the pattern map to:

- **Layer 1 — Ground truth:** `ia_wiki.raw_documents`, append-only (UPDATE/DELETE are
  blocked by a trigger) so the wiki can always be recompiled from scratch.
- **Layer 2 — The wiki:** `ia_wiki.compiled_pages` (Markdown) and `ia_wiki.entity_graph`
  (typed relations), fully owned and rewritten by the LLM.
- **Layer 3 — Directives:** the compiler/auditor system prompts, exposed as PostgreSQL
  **GUCs** (`ia_sql.wiki_system_prompt`, `ia_sql.lint_system_prompt`) — tunable live with
  `ALTER SYSTEM SET … ; SELECT pg_reload_conf();`.

### Why PostgreSQL (and why the LLM stays *external*)

PostgreSQL's process model, **background workers**, **SPI**, **GUCs** and triggers make it
an ideal host for an asynchronous compile loop. The heavy model inference, however, runs
in a **separate, configurable OpenAI-compatible service** (local Ollama / llama.cpp, or a
SaaS API). This keeps the database stable — a crash or OOM in a model never takes Postgres
down — and lets you pick any model. IA-SQL is the *orchestrator in the engine*; the LLM is
a swappable backend.

## Requirements

- PostgreSQL 17 (with `postgresql-server-dev-17`)
- A C toolchain (`gcc`/`make`) and `libcurl` (`libcurl4-openssl-dev`)
- `pg_cron` (optional, for scheduled audits)
- An OpenAI-compatible chat-completions endpoint (Ollama, llama.cpp server, vLLM, OpenAI,
  etc.) serving an **instruction-following** model

## Install

```bash
git clone https://github.com/feers77/iasql.git
cd iasql
make
sudo make install
```

Enable the background worker and (optionally) pg_cron, then create the extension:

```sql
-- postgresql.conf
shared_preload_libraries = 'pg_cron,ia_sql'   -- restart required
```
```sql
CREATE EXTENSION ia_sql;     -- creates schema ia_wiki + tables + functions
```

Point IA-SQL at your LLM:

```sql
ALTER SYSTEM SET ia_sql.llm_base_url = 'http://localhost:11434/v1';  -- e.g. Ollama
ALTER SYSTEM SET ia_sql.llm_model    = 'qwen2.5';
SELECT pg_reload_conf();
```

## Usage

```sql
-- 1. Feed it documents (Layer 1). The wiki compiles asynchronously.
INSERT INTO ia_wiki.raw_documents (source, content)
VALUES ('notes', 'PostgreSQL is an extensible, process-based RDBMS …');

-- 2. Read the compiled wiki (Layer 2).
SELECT * FROM ia_wiki.pages;                       -- listing
SELECT markdown_body FROM ia_wiki.compiled_pages WHERE page_entity = 'postgresql';
SELECT * FROM ia_wiki.entity_graph;                -- the knowledge graph

-- 3. Audit for hallucinations (on demand or via pg_cron).
SELECT ia_wiki.enqueue_lint(20);
SELECT * FROM ia_wiki.hallucination_flags WHERE NOT resolved;

-- Observability
SELECT * FROM ia_wiki.jobs ORDER BY job_id DESC;          -- queue
SELECT * FROM ia_wiki.processing_log ORDER BY id DESC;    -- tokens / latency
```

## Configuration (GUCs)

| GUC | Default | Purpose |
|-----|---------|---------|
| `ia_sql.enabled` | `on` | Master on/off switch for the worker |
| `ia_sql.database` | `iasql` | Database the worker connects to (postmaster-level) |
| `ia_sql.poll_interval_ms` | `1000` | Worker poll cadence |
| `ia_sql.llm_base_url` | `http://localhost:11434/v1` | OpenAI-compatible base URL |
| `ia_sql.llm_api_key` | `''` | Bearer key (superuser-only, hidden) |
| `ia_sql.llm_model` | `qwen2.5` | Model name |
| `ia_sql.llm_timeout_ms` | `120000` | HTTP timeout |
| `ia_sql.llm_temperature` | `0.2` | Sampling temperature |
| `ia_sql.llm_max_tokens` | `4096` | Max completion tokens |
| `ia_sql.llm_extra_json` | `{}` | Extra JSON merged into each request (see below) |
| `ia_sql.wiki_system_prompt` | *(built-in)* | Layer-3 compiler directive |
| `ia_sql.lint_system_prompt` | *(built-in)* | Layer-3 auditor directive |
| `ia_sql.max_attempts` | `3` | Retries before a job is marked `error` |

### Provider compatibility

IA-SQL speaks the standard `/chat/completions` API, so it works with Ollama, llama.cpp's
server, vLLM, OpenAI, and compatible gateways. Provider-specific options go through
`ia_sql.llm_extra_json`, which is merged into every request body. Examples:

```sql
-- Qwen3 "thinking" models: disable reasoning so the answer is plain JSON
ALTER SYSTEM SET ia_sql.llm_extra_json = '{"chat_template_kwargs":{"enable_thinking":false}}';

-- Force JSON output where supported
ALTER SYSTEM SET ia_sql.llm_extra_json = '{"response_format":{"type":"json_object"}}';
```

For a hosted API, set `ia_sql.llm_api_key` and the corresponding `ia_sql.llm_base_url`.

## Security notes

- The worker runs as a PostgreSQL background worker; only install extensions you trust.
- `ia_sql.llm_api_key` is `SUPERUSER_ONLY` and not shown to regular users.
- Documents and compiled pages are sent to your configured LLM endpoint — point it at an
  endpoint you trust with your data.

## Roadmap

- Parallel workers (`RegisterDynamicBackgroundWorker`) for higher ingest throughput
- Smarter context retrieval (graph- or embedding-guided page selection)
- Optional token-streaming for interactive use (`shm_mq`)
- Full re-bootstrap (recompile the whole wiki from Layer 1)
- A read-only web viewer for the wiki

## Credits & license

- Concept: Andrej Karpathy's **LLM Wiki** pattern.
- Bundles [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
- Licensed under the **MIT License** — see [LICENSE](LICENSE).
