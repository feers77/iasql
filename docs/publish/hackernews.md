# Hacker News submission / Envío a Hacker News

> **You must submit this yourself** (HN has no submission API; it needs your logged-in
> account). The links below pre-fill the form — open one while logged in to HN, then click
> "submit". HN is English-only; post the English comment. (ES translation kept for reference.)

## Title (74 chars, under HN's 80 limit)

```
Show HN: IA-SQL – Postgres compiles your documents into a wiki with an LLM
```

## One-click submit links (pre-filled)

Recommended: submit the **GitHub repo** as the URL (HN likes code), then paste the first
comment with the live demo link. Alternatively submit the **live demo**.

- **Repo (recommended):**
  https://news.ycombinator.com/submitlink?u=https%3A%2F%2Fgithub.com%2Ffeers77%2Fiasql&t=Show%20HN%3A%20IA-SQL%20%E2%80%93%20Postgres%20compiles%20your%20documents%20into%20a%20wiki%20with%20an%20LLM

- **Live demo:**
  https://news.ycombinator.com/submitlink?u=https%3A%2F%2Fiasql.dev.feres.cl&t=Show%20HN%3A%20IA-SQL%20%E2%80%93%20Postgres%20compiles%20your%20documents%20into%20a%20wiki%20with%20an%20LLM

(If a link 404s, just go to https://news.ycombinator.com/submit and paste the title + URL.)

## First comment — EN (post this right after submitting)

> Author here. IA-SQL is a small PostgreSQL extension (C, MIT) that implements Karpathy's
> "LLM Wiki" pattern *inside the database*. Instead of RAG (retrieve chunks at query time),
> it works at ingest time: you INSERT documents into an append-only table, an AFTER INSERT
> trigger enqueues a job (O(1), never blocks), and a background worker claims it with
> FOR UPDATE SKIP LOCKED, calls an external OpenAI-compatible LLM, and UPSERTs synthesised
> Markdown pages + a typed entity graph. A pg_cron job then audits pages against their
> sources and records hallucination flags.
>
> Design choices I'd love feedback on: the LLM is deliberately external (Ollama / llama.cpp
> / vLLM / OpenAI) so heavy inference can't take the database down; the worker does
> claim → HTTP outside any transaction → write, with bounded retries; provider quirks
> (e.g. Qwen3's thinking mode) are absorbed by a single GUC that merges extra JSON into each
> request, keeping the engine vendor-neutral.
>
> It's an early 0.1 PoC built/tested on PostgreSQL 17. Live demo:
> https://iasql.dev.feres.cl — code: https://github.com/feers77/iasql. Architecture notes
> in docs/ARCHITECTURE.md. Happy to answer anything.

## Primer comentario — ES (referencia)

> Soy el autor. IA-SQL es una extensión chica de PostgreSQL (C, MIT) que implementa el
> patrón "LLM Wiki" de Karpathy *dentro de la base de datos*. En vez de RAG (recuperar
> fragmentos en la consulta), trabaja en la ingesta: insertas documentos en una tabla
> append-only, un trigger AFTER INSERT encola un job (O(1), no bloquea), y un background
> worker lo toma con FOR UPDATE SKIP LOCKED, llama a un LLM externo compatible con OpenAI y
> hace UPSERT de páginas Markdown sintetizadas + un grafo de entidades. Un job de pg_cron
> audita las páginas contra sus fuentes y registra alucinaciones. El LLM es externo a
> propósito; el worker hace claim → HTTP fuera de transacción → escritura, con reintentos.
> Demo: https://iasql.dev.feres.cl — código: https://github.com/feers77/iasql

## Tips

- Best time: weekday mornings US Eastern. Submit, then post the first comment immediately.
- Don't ask for upvotes (against HN rules). Reply to every comment in the first hours.
- "Show HN" rules: it must be something people can try — the live demo + repo both qualify.
