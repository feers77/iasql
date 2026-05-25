<!-- Idioma / Language: [English](TUTORIAL.md) · **Español** -->

# Tutorial de IA-SQL — por perfil de usuario

Este tutorial recorre cada tipo de usuario de IA-SQL de principio a fin. Asume que la
extensión está instalada (`make && sudo make install`) y que PostgreSQL tiene
`shared_preload_libraries = 'pg_cron,ia_sql'`.

Hay cuatro perfiles:

| Perfil | Quién | Puede |
|--------|-------|-------|
| **Admin / DBA** | superusuario | instalar, configurar el LLM, gestionar roles, programar auditorías |
| **Ingestor** | proveedor de contenido | insertar documentos (Capa 1) |
| **Lector** | consumidor del conocimiento | leer la wiki compilada (Capa 2) |
| **Auditor** | calidad / cumplimiento | correr auditorías, revisar y resolver flags |

Las conexiones usan `psql`. Reemplaza `HOST` por tu servidor (p.ej. una IP de Tailscale) y
usa la contraseña de cada rol.

---

## 1. Admin / DBA

**Instala la extensión y habilita el worker.**

```sql
CREATE EXTENSION ia_sql;     -- crea el esquema ia_wiki, tablas, funciones
SELECT ia_wiki.version();    -- chequeo -> "IA-SQL 0.1"
```

**Apunta IA-SQL a tu LLM** (cualquier endpoint compatible con OpenAI):

```sql
ALTER SYSTEM SET ia_sql.llm_base_url = 'http://localhost:11434/v1';   -- Ollama, llama.cpp, vLLM, OpenAI…
ALTER SYSTEM SET ia_sql.llm_model    = 'qwen2.5';
-- Ajustes específicos del proveedor (se fusionan en cada request):
ALTER SYSTEM SET ia_sql.llm_extra_json = '{"chat_template_kwargs":{"enable_thinking":false}}'; -- Qwen3
-- Para una API hospedada:
-- ALTER SYSTEM SET ia_sql.llm_api_key = 'sk-...';
SELECT pg_reload_conf();
```

**Ajusta el comportamiento del compilador** (directivas de la Capa 3) en caliente, sin caídas:

```sql
ALTER SYSTEM SET ia_sql.wiki_system_prompt = 'Eres un compilador de wiki meticuloso …';
SELECT pg_reload_conf();
```

**Crea los roles de usuario** (ver `sql/roles.example.sql`; cambia las contraseñas):

```sql
\i sql/roles.example.sql
```

**Programa la auditoría nocturna** (requiere pg_cron):

```sql
SELECT cron.schedule('ia_sql_lint', '0 3 * * *', 'SELECT ia_wiki.enqueue_lint(20)');
```

**Monitorea el motor.**

```sql
SELECT * FROM pg_stat_activity WHERE backend_type = 'ia_sql';   -- ¿worker vivo?
SELECT status, count(*) FROM ia_wiki.jobs GROUP BY status;      -- salud de la cola
SELECT job_id, model, prompt_tokens, completion_tokens, latency_ms
  FROM ia_wiki.processing_log ORDER BY id DESC LIMIT 10;        -- costo / latencia
```

Pausa/reanuda el worker cuando quieras:
`ALTER SYSTEM SET ia_sql.enabled = off; SELECT pg_reload_conf();`

---

## 2. Ingestor — alimenta la base de conocimiento

Solo haces una cosa: insertar documentos crudos. La compilación ocurre automáticamente en
segundo plano; tu `INSERT` retorna de inmediato.

```bash
psql "host=HOST user=iasql_ingestor dbname=iasql"
```
```sql
INSERT INTO ia_wiki.raw_documents (source, content) VALUES
  ('reunion-2026-05-20', 'Decisión: estandarizamos en PostgreSQL 17 para todos los servicios …');

-- Observa cómo se compila tu documento:
SELECT job_id, kind, status FROM ia_wiki.jobs ORDER BY job_id DESC LIMIT 5;
```

Notas:
- El ground truth es **append-only**: no puedes `UPDATE`/`DELETE` en `raw_documents` (por
  diseño). Para corregir información, inserta un documento nuevo con la corrección — el
  compilador la reconcilia.
- No puedes leer ni modificar la wiki; eso es trabajo del Lector y del motor.

---

## 3. Lector — usa la wiki compilada

```bash
psql "host=HOST user=iasql_reader dbname=iasql"
```
```sql
-- Navega las páginas
SELECT page_entity, title, summary, last_compiled FROM ia_wiki.pages;

-- Lee una página
SELECT markdown_body FROM ia_wiki.compiled_pages WHERE page_entity = 'postgresql_architecture';

-- Explora el grafo de conocimiento
SELECT source_entity, relation, target_entity, weight FROM ia_wiki.entity_graph;

-- Encuentra páginas relacionadas con un concepto
SELECT target_entity, relation FROM ia_wiki.entity_graph WHERE source_entity = 'ia_sql';
```

La wiki está precompilada, así que las lecturas son SQL plano y rápido — sin llamada al
modelo en el momento de la consulta.

---

## 4. Auditor — mantén la wiki honesta

El auditor contrasta las páginas compiladas contra sus documentos fuente y gestiona los
flags resultantes.

```bash
psql "host=HOST user=iasql_auditor dbname=iasql"
```
```sql
-- Dispara una auditoría de las N páginas más antiguas (o confía en el job nocturno de pg_cron)
SELECT ia_wiki.enqueue_lint(20);

-- Revisa los hallazgos abiertos
SELECT id, page_entity, severity, description
  FROM ia_wiki.hallucination_flags
  WHERE NOT resolved ORDER BY severity DESC, id;

-- Resuelve un hallazgo después de actuar sobre él
UPDATE ia_wiki.hallucination_flags SET resolved = true WHERE id = 42;
```

Cada flag vincula una afirmación de una página con la ausencia de evidencia que la sustente
en la Capa 1, con una severidad (`low` / `medium` / `high`). Un flag de severidad alta
suele significar que el compilador introdujo una afirmación no sustentada — alimenta un
documento correctivo (como Ingestor) y la página se reescribirá.

---

## De punta a punta, en un minuto

```sql
-- como ingestor
INSERT INTO ia_wiki.raw_documents (content)
VALUES ('La capital de Francia es París. La Torre Eiffel se inauguró en 1889.');
-- espera unos segundos…
-- como lector
SELECT markdown_body FROM ia_wiki.compiled_pages;     -- una página compilada sobre París/Francia
-- como auditor
SELECT ia_wiki.enqueue_lint(5);
SELECT * FROM ia_wiki.hallucination_flags;            -- vacío para hechos bien fundados
```
