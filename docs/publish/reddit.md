# Reddit post drafts / Borradores para Reddit

> Pick the subreddit that fits, paste title + body. Be upfront that it's a 0.1 PoC and that
> you're the author (most subs require the [P]/OC disclosure). Repo:
> https://github.com/feers77/iasql

---

## EN — r/PostgreSQL

**Title:** IA-SQL: a PostgreSQL extension that compiles documents into a self-maintained
wiki using an LLM (background worker + pg_cron)

**Body:**

I built a small MIT-licensed extension that implements Karpathy's "LLM Wiki" pattern inside
Postgres. Instead of RAG (retrieve chunks at query time), it does the work at *ingest* time:

- You `INSERT` into an append-only `raw_documents` table (Layer 1 / ground truth).
- An `AFTER INSERT` trigger enqueues a job (O(1), never blocks the insert).
- A background worker claims jobs with `FOR UPDATE SKIP LOCKED`, calls an external
  OpenAI-compatible LLM, and UPSERTs synthesised Markdown pages + a typed entity graph
  (Layer 2).
- A `pg_cron` job audits pages against their sources and records hallucination flags.

The LLM stays external (Ollama / llama.cpp / vLLM / OpenAI) on purpose — no heavy inference
in a backend. Config is all GUCs (`ALTER SYSTEM SET ia_sql.* …`). Built and tested on PG 17.

It's an early proof of concept; I'd love feedback on the worker/transaction design (claim →
HTTP outside any txn → write, with bounded retries). Repo + architecture doc:
https://github.com/feers77/iasql

---

## EN — r/LocalLLaMA

**Title:** [P] IA-SQL — point your local Ollama/llama.cpp at PostgreSQL and it compiles your
docs into a maintained wiki (not RAG)

**Body:**

Weekend-ish project: a Postgres extension that turns any OpenAI-compatible local server into
a "knowledge compiler". You drop documents into a table and a background worker asks your
local model to merge them into a maintained Markdown wiki with cross-references — Karpathy's
"LLM Wiki" idea, but living inside the database.

Why you might care if you run models locally:
- Works with Ollama, llama.cpp server, vLLM — anything speaking `/chat/completions`.
- Provider quirks handled via one GUC, e.g. for Qwen3 thinking models:
  `ia_sql.llm_extra_json = '{"chat_template_kwargs":{"enable_thinking":false}}'`.
- A nightly "lint" pass re-checks the compiled wiki against the source docs and flags
  hallucinations (caught a planted "won the Turing Award in 2010" at severity high).

MIT, tested with Qwen3 served by llama.cpp. Feedback welcome on prompt/JSON robustness:
https://github.com/feers77/iasql

---

## ES — r/programacion

**Título:** IA-SQL: una extensión de PostgreSQL que compila documentos en una wiki
automantenida usando un LLM (open source, MIT)

**Cuerpo:**

Hice una extensión chica (MIT) que implementa el patrón "LLM Wiki" de Karpathy dentro de
Postgres. En vez de RAG (recuperar fragmentos en la consulta), hace el trabajo en la
*ingesta*:

- Haces `INSERT` en una tabla `raw_documents` append-only (Capa 1 / verdad base).
- Un trigger `AFTER INSERT` encola un job (O(1), no bloquea el insert).
- Un background worker toma jobs con `FOR UPDATE SKIP LOCKED`, llama a un LLM externo
  compatible con OpenAI y hace UPSERT de páginas Markdown sintetizadas + un grafo de
  entidades (Capa 2).
- Un job de `pg_cron` audita las páginas contra sus fuentes y registra alucinaciones.

El LLM es externo a propósito (Ollama / llama.cpp / vLLM / OpenAI). Toda la config son GUCs.
Probado en PG 17. Es una prueba de concepto temprana; agradezco feedback sobre el diseño del
worker (claim → HTTP fuera de transacción → escritura, con reintentos):
https://github.com/feers77/iasql

---

## Posting tips / Consejos

- r/PostgreSQL & r/programming: lead with the engineering (worker, SKIP LOCKED, txn design).
- r/LocalLLaMA: lead with "use your local model", show the Qwen3 thinking-mode fix.
- Disclose you're the author and that it's 0.1. Link the README and ARCHITECTURE.md.
- Cross-post a day apart, not all at once, and reply to comments.
