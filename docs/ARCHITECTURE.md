# Architecture / Arquitectura

*(English first, Español abajo.)*

## English

### Code layout

```
ia_sql.control            extension manifest
sql/ia_sql--0.1.sql       schema (3 layers) + PL/pgSQL triggers + C function decls
src/ia_sql.c              _PG_init: GUCs + background-worker registration; ia_wiki.version()
src/worker.c              the dispatcher background worker (queue loop + SQL writes)
src/http_client.c/.h      libcurl POST wrapper
src/llm.c/.h              build chat requests, parse responses -> pages/edges/flags
src/vendor/cJSON.*        bundled JSON library (MIT)
sql/roles.example.sql     optional least-privilege roles
```

### Control flow

1. `_PG_init()` runs in the postmaster (the module is in `shared_preload_libraries`). It
   defines all `ia_sql.*` GUCs and registers one static background worker.
2. The worker (`ia_sql_worker_main`) connects to `ia_sql.database`, then loops:
   `WaitLatch(timeout = poll_interval)` → process all pending jobs → repeat. It handles
   `SIGHUP` (reload config) and `SIGTERM` (clean shutdown), and requeues jobs left
   `running` by a previous crash.
3. An `AFTER INSERT` trigger on `raw_documents` enqueues a job and `pg_notify`s — it is
   `O(1)` and never blocks the inserting transaction. (It is `SECURITY DEFINER` so a
   least-privileged ingestor needs no rights on `ia_wiki.jobs`.)

### Job processing (the important part)

Each job is handled in **three steps with short transactions**, so the slow network call
never holds a transaction or row lock open:

- **Tx1 — claim:** `UPDATE … WHERE job_id = (SELECT … WHERE status='pending' ORDER BY job_id
  FOR UPDATE SKIP LOCKED LIMIT 1)` marks the row `running` and returns the document plus the
  current wiki context. `SKIP LOCKED` makes the design correct even with multiple workers.
  Then **commit** — the lock is released.
- **HTTP (no transaction):** `ia_llm_ingest()` builds the chat request, calls the LLM, and
  parses the JSON answer into `pages[]` / `edges[]`. Strings are copied into a per-job
  `MemoryContext` that survives between transactions; cJSON trees are created and freed
  without any `ereport()` in between, so error unwinding never leaks them.
- **Tx2 — write:** UPSERT each page (`compiled_pages`) and edge (`entity_graph`), append the
  telemetry row (`processing_log`), and mark the job `done`. Wrapped in `PG_TRY/PG_CATCH`;
  on failure the job is requeued (up to `ia_sql.max_attempts`) or marked `error`.

The lint path is analogous: gather a page and its source documents, call `ia_llm_lint()`,
and write `hallucination_flags`.

### Why the LLM is external

Heavy inference inside a regular backend would couple the model's stability to the
database: a segfault or OOM can trigger a server-wide restart. IA-SQL keeps inference in a
**separate OpenAI-compatible service**, so the database only ever does cheap orchestration.
Provider quirks are absorbed by `ia_sql.llm_extra_json` (merged into each request), keeping
the engine vendor-neutral.

### Extending

- **New job kinds:** add a branch in `process_one_job()` and an enqueue path.
- **New model output fields:** extend `LlmResult` and the parser in `llm.c`.
- **Parallelism (roadmap):** the claim already uses `SKIP LOCKED`; a pool of dynamic
  workers (`RegisterDynamicBackgroundWorker`) can be added without changing the SQL.

---

## Español

### Estructura del código

```
ia_sql.control            manifiesto de la extensión
sql/ia_sql--0.1.sql       esquema (3 capas) + triggers PL/pgSQL + decls de funciones C
src/ia_sql.c              _PG_init: GUCs + registro del background worker; ia_wiki.version()
src/worker.c              el background worker dispatcher (loop de cola + escrituras SQL)
src/http_client.c/.h      wrapper de POST con libcurl
src/llm.c/.h              arma requests de chat, parsea respuestas -> páginas/aristas/flags
src/vendor/cJSON.*        librería JSON incluida (MIT)
sql/roles.example.sql     roles opcionales de mínimo privilegio
```

### Flujo de control

1. `_PG_init()` corre en el postmaster (el módulo está en `shared_preload_libraries`).
   Define todos los GUCs `ia_sql.*` y registra un background worker estático.
2. El worker (`ia_sql_worker_main`) conecta a `ia_sql.database` y entra en bucle:
   `WaitLatch(timeout = poll_interval)` → procesa todos los jobs pendientes → repite.
   Maneja `SIGHUP` (recarga config) y `SIGTERM` (apagado limpio), y re-encola los jobs que
   quedaron `running` por un crash previo.
3. Un trigger `AFTER INSERT` en `raw_documents` encola un job y hace `pg_notify` — es `O(1)`
   y nunca bloquea la transacción de inserción. (Es `SECURITY DEFINER`, así un ingestor de
   mínimo privilegio no necesita permisos sobre `ia_wiki.jobs`.)

### Procesamiento de jobs (lo importante)

Cada job se maneja en **tres pasos con transacciones cortas**, para que la llamada de red
lenta nunca mantenga abierta una transacción o un lock de fila:

- **Tx1 — claim:** `UPDATE … WHERE job_id = (SELECT … WHERE status='pending' ORDER BY job_id
  FOR UPDATE SKIP LOCKED LIMIT 1)` marca la fila `running` y devuelve el documento más el
  contexto actual de la wiki. `SKIP LOCKED` hace correcto el diseño incluso con varios
  workers. Luego **commit** — se libera el lock.
- **HTTP (sin transacción):** `ia_llm_ingest()` arma el request, llama al LLM y parsea el
  JSON a `pages[]` / `edges[]`. Las cadenas se copian a un `MemoryContext` por-job que
  sobrevive entre transacciones; los árboles cJSON se crean y liberan sin ningún
  `ereport()` en medio, así el desenrollado de errores nunca los filtra.
- **Tx2 — escritura:** UPSERT de cada página (`compiled_pages`) y arista (`entity_graph`),
  inserta la telemetría (`processing_log`) y marca el job `done`. Envuelto en
  `PG_TRY/PG_CATCH`; ante fallo el job se re-encola (hasta `ia_sql.max_attempts`) o se marca
  `error`.

El camino de lint es análogo: junta una página y sus documentos fuente, llama a
`ia_llm_lint()` y escribe `hallucination_flags`.

### Por qué el LLM es externo

Inferencia pesada dentro de un backend normal acoplaría la estabilidad del modelo a la
base de datos: un segfault u OOM puede provocar un reinicio de todo el servidor. IA-SQL
mantiene la inferencia en un **servicio externo compatible con OpenAI**, así la base solo
hace orquestación barata. Las particularidades de cada proveedor se absorben con
`ia_sql.llm_extra_json` (fusionado en cada request), manteniendo el motor neutral.

### Cómo extender

- **Nuevos tipos de job:** agrega una rama en `process_one_job()` y un camino de encolado.
- **Nuevos campos de salida del modelo:** extiende `LlmResult` y el parser en `llm.c`.
- **Paralelismo (roadmap):** el claim ya usa `SKIP LOCKED`; un pool de workers dinámicos
  (`RegisterDynamicBackgroundWorker`) se puede agregar sin cambiar el SQL.
