<!-- Idioma / Language: [English](README.md) · **Español** -->

# IA-SQL

**Convierte PostgreSQL en una base de conocimiento que se autocompila.**
Una implementación, dentro de la base de datos, del patrón *"LLM Wiki"* de Andrej
Karpathy: tú haces `INSERT` de documentos crudos y un *background worker* usa un LLM
externo para **compilarlos** en una wiki Markdown mantenida e interconectada — y luego
audita esa wiki contra las fuentes en busca de alucinaciones.

> Estado: **0.1 — prueba de concepto funcional.** Construido y probado en PostgreSQL 17.
> [English here →](README.md)

🌐 **Landing: https://feers77.github.io/iasql** · **Demo de la wiki en vivo: https://iasql.dev.feres.cl**
📖 **¿Recién llegas? Empieza por el [tutorial por perfil de usuario](docs/TUTORIAL.es.md).**

---

## La idea

La mayoría de los sistemas de "chatea con tus documentos" usan **RAG**: en el momento de
la consulta recuperan unos fragmentos de texto y le piden al modelo improvisar una
respuesta. El modelo redescubre tu dominio desde cero en cada pregunta y nunca acumula
comprensión.

El patrón **LLM Wiki** de Karpathy le da vuelta a esto, con una metáfora del software:

| Software           | Base de conocimiento                                     |
|--------------------|---------------------------------------------------------|
| Código fuente      | Documentos crudos (verdad base inmutable)               |
| Compilador         | El LLM                                                   |
| Binario compilado  | Una wiki Markdown mantenida (sintetizada, enlazada)     |

El trabajo costoso ocurre en el **momento de la ingesta**, no en el de la consulta. Cuando
llega un documento nuevo, el LLM lo lee una vez, decide qué entidades/páginas afecta y
reescribe esas páginas — consolidando, resolviendo contradicciones y añadiendo
referencias cruzadas. El conocimiento **acumula interés compuesto**: cada documento mejora
toda la wiki.

**IA-SQL pone este bucle dentro de PostgreSQL.** La base de datos deja de ser un almacén
pasivo: metaboliza información nueva de forma asíncrona y audita su propia coherencia,
mientras las consultas normales siguen corriendo.

## Cómo funciona

```
INSERT INTO ia_wiki.raw_documents (content)         -- Capa 1: ground truth append-only
        │  trigger AFTER INSERT (O(1): encola + NOTIFY, nunca bloquea)
        ▼
   ia_wiki.jobs  (pending)
        │  ia_sql dispatcher (background worker)
        │   claim FOR UPDATE SKIP LOCKED  →  llama al LLM externo  →  escribe resultado
        ▼
   ia_wiki.compiled_pages + entity_graph             -- Capa 2: la wiki (propiedad del LLM)
        ▲
        │  pg_cron nocturno  →  ia_wiki.enqueue_lint()
        ▼
   ia_wiki.hallucination_flags                       -- autoauditoría contra la Capa 1
```

Las tres capas del patrón se mapean a:

- **Capa 1 — Ground truth:** `ia_wiki.raw_documents`, append-only (UPDATE/DELETE bloqueados
  por trigger) para poder recompilar la wiki desde cero.
- **Capa 2 — La wiki:** `ia_wiki.compiled_pages` (Markdown) y `ia_wiki.entity_graph`
  (relaciones tipadas), propiedad del LLM y reescritas por él.
- **Capa 3 — Directivas:** los *system prompts* del compilador/auditor, expuestos como
  **GUCs** de PostgreSQL (`ia_sql.wiki_system_prompt`, `ia_sql.lint_system_prompt`) —
  ajustables en caliente con `ALTER SYSTEM SET … ; SELECT pg_reload_conf();`.

### Por qué PostgreSQL (y por qué el LLM es *externo*)

El modelo de procesos de PostgreSQL, sus **background workers**, **SPI**, **GUCs** y
triggers lo hacen un anfitrión ideal para un bucle de compilación asíncrono. La inferencia
pesada, en cambio, corre en un **servicio externo configurable, compatible con OpenAI**
(Ollama / llama.cpp local, o una API SaaS). Así la base de datos se mantiene estable —un
crash u OOM del modelo nunca tumba a Postgres— y puedes elegir cualquier modelo. IA-SQL es
el *orquestador dentro del motor*; el LLM es un backend intercambiable.

## Requisitos

- PostgreSQL 17 (con `postgresql-server-dev-17`)
- Toolchain C (`gcc`/`make`) y `libcurl` (`libcurl4-openssl-dev`)
- `pg_cron` (opcional, para auditorías programadas)
- Un endpoint de chat-completions compatible con OpenAI (Ollama, servidor de llama.cpp,
  vLLM, OpenAI, etc.) sirviendo un modelo **que siga instrucciones**

## Instalación

```bash
git clone https://github.com/feers77/iasql.git
cd iasql
make
sudo make install
```

Habilita el worker y (opcional) pg_cron, luego crea la extensión:

```sql
-- postgresql.conf
shared_preload_libraries = 'pg_cron,ia_sql'   -- requiere reinicio
```
```sql
CREATE EXTENSION ia_sql;     -- crea el esquema ia_wiki + tablas + funciones
```

Apunta IA-SQL a tu LLM:

```sql
ALTER SYSTEM SET ia_sql.llm_base_url = 'http://localhost:11434/v1';  -- p.ej. Ollama
ALTER SYSTEM SET ia_sql.llm_model    = 'qwen2.5';
SELECT pg_reload_conf();
```

## Uso

```sql
-- 1. Aliméntalo con documentos (Capa 1). La wiki se compila asíncronamente.
INSERT INTO ia_wiki.raw_documents (source, content)
VALUES ('notas', 'PostgreSQL es un RDBMS extensible basado en procesos …');

-- 2. Lee la wiki compilada (Capa 2).
SELECT * FROM ia_wiki.pages;                       -- listado
SELECT markdown_body FROM ia_wiki.compiled_pages WHERE page_entity = 'postgresql';
SELECT * FROM ia_wiki.entity_graph;                -- el grafo de conocimiento

-- 3. Audita alucinaciones (a demanda o vía pg_cron).
SELECT ia_wiki.enqueue_lint(20);
SELECT * FROM ia_wiki.hallucination_flags WHERE NOT resolved;

-- Observabilidad
SELECT * FROM ia_wiki.jobs ORDER BY job_id DESC;          -- cola
SELECT * FROM ia_wiki.processing_log ORDER BY id DESC;    -- tokens / latencia
```

## Configuración (GUCs)

| GUC | Default | Propósito |
|-----|---------|-----------|
| `ia_sql.enabled` | `on` | Interruptor maestro del worker |
| `ia_sql.database` | `iasql` | BD a la que conecta el worker (nivel postmaster) |
| `ia_sql.poll_interval_ms` | `1000` | Cadencia de sondeo del worker |
| `ia_sql.llm_base_url` | `http://localhost:11434/v1` | URL base compatible con OpenAI |
| `ia_sql.llm_api_key` | `''` | Clave Bearer (solo superusuario, oculta) |
| `ia_sql.llm_model` | `qwen2.5` | Nombre del modelo |
| `ia_sql.llm_timeout_ms` | `120000` | Tiempo límite HTTP |
| `ia_sql.llm_temperature` | `0.2` | Temperatura de muestreo |
| `ia_sql.llm_max_tokens` | `4096` | Máx. tokens de salida |
| `ia_sql.llm_extra_json` | `{}` | JSON extra fusionado en cada request (ver abajo) |
| `ia_sql.wiki_system_prompt` | *(interno)* | Directiva del compilador (Capa 3) |
| `ia_sql.lint_system_prompt` | *(interno)* | Directiva del auditor (Capa 3) |
| `ia_sql.max_attempts` | `3` | Reintentos antes de marcar `error` |

### Compatibilidad de proveedores

IA-SQL habla la API estándar `/chat/completions`, así que funciona con Ollama, el servidor
de llama.cpp, vLLM, OpenAI y gateways compatibles. Las opciones específicas de cada
proveedor van por `ia_sql.llm_extra_json`, que se fusiona en cada cuerpo de request.
Ejemplos:

```sql
-- Modelos Qwen3 "thinking": desactiva el razonamiento para que la respuesta sea JSON puro
ALTER SYSTEM SET ia_sql.llm_extra_json = '{"chat_template_kwargs":{"enable_thinking":false}}';

-- Forzar salida JSON donde se soporte
ALTER SYSTEM SET ia_sql.llm_extra_json = '{"response_format":{"type":"json_object"}}';
```

Para una API hospedada, configura `ia_sql.llm_api_key` y el `ia_sql.llm_base_url`
correspondiente.

## Notas de seguridad

- El worker corre como background worker de PostgreSQL; instala solo extensiones de confianza.
- `ia_sql.llm_api_key` es `SUPERUSER_ONLY` y no se muestra a usuarios normales.
- Los documentos y las páginas compiladas se envían a tu endpoint LLM configurado —
  apúntalo a un endpoint en el que confíes con tus datos.

## Hoja de ruta

- Workers en paralelo (`RegisterDynamicBackgroundWorker`) para más throughput de ingesta
- Recuperación de contexto más inteligente (selección de páginas por grafo o embeddings)
- Streaming de tokens opcional para uso interactivo (`shm_mq`)
- Re-bootstrap completo (recompilar toda la wiki desde la Capa 1)
- Un visor web de solo lectura de la wiki

## Créditos y licencia

- Concepto: el patrón **LLM Wiki** de Andrej Karpathy.
- Incluye [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
- Licenciado bajo la **Licencia MIT** — ver [LICENSE](LICENSE).
