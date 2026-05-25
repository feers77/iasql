# Medium article drafts / Borradores de artículo

> Two ready-to-publish drafts (English + Español). Replace the demo numbers if you re-run
> on different hardware. Repo: https://github.com/feers77/iasql

---

## EN — *I taught PostgreSQL to compile its own wiki with an LLM*

**TL;DR** — IA-SQL is a PostgreSQL extension that implements Andrej Karpathy's "LLM Wiki"
pattern *inside the database*. You `INSERT` raw documents; a background worker calls an
external LLM to compile them into a maintained, cross-referenced Markdown wiki, and a
nightly audit checks that wiki against the sources for hallucinations. It's MIT-licensed and
on GitHub.

### The problem with "chat with your documents"

The default way to put private knowledge into an LLM is **RAG**: chop documents into chunks,
embed them, and at query time retrieve a few chunks and ask the model to improvise. It
works, but it has a structural flaw — the model re-discovers your domain *from scratch on
every question*. There is no accumulation. Ask something that requires connecting ideas
scattered across ten documents and you're betting on a similarity search returning exactly
the right fragments.

### Karpathy's "compiler" metaphor

Andrej Karpathy proposed flipping this around with a metaphor from software engineering:

- Raw documents are **source code** (immutable).
- The LLM is a **compiler**.
- A maintained Markdown wiki is the **compiled binary**.

The expensive work moves from *query time* to *ingest time*. When a new document arrives,
the model reads it once, decides which wiki pages it affects, and rewrites them —
consolidating, resolving contradictions, adding cross-references. Knowledge **compounds**.

### Why put it inside PostgreSQL?

Because Postgres already has everything a compile loop needs: **background workers** for
async work, **SPI** to run SQL from C, triggers, transactions, `SKIP LOCKED` job queues,
GUCs for live-tunable config, and `pg_cron` for scheduling. The database stops being a
passive store and becomes something that *metabolises* information.

One deliberate choice: **the LLM stays external** (any OpenAI-compatible endpoint — Ollama,
llama.cpp, vLLM, or a SaaS API). Embedding heavy inference inside a backend would tie the
model's crashes to the database's uptime. IA-SQL is the orchestrator in the engine; the
model is a swappable backend.

### How it works

```
INSERT into raw_documents (Layer 1, append-only)
   → AFTER INSERT trigger enqueues a job (never blocks the insert)
   → background worker: claim (FOR UPDATE SKIP LOCKED) → call LLM → write
   → compiled_pages + entity_graph (Layer 2, owned by the LLM)
   → pg_cron nightly → lint each page against its sources → hallucination_flags
```

The compiler's behaviour lives in Layer 3 — system prompts exposed as PostgreSQL GUCs you
can change live with `ALTER SYSTEM SET … ; SELECT pg_reload_conf();`.

### A real run

Feeding three short documents about PostgreSQL, background workers, and IA-SQL produced
three coherent pages with synthesised content and a real knowledge graph:

```
postgresql_architecture  --generated_by-->     karpathy_llm_wiki_pattern
ia_sql                   --uses_component-->    postgresql_architecture
ia_sql                   --implements_pattern-->karpathy_llm_wiki_pattern
```

Then I injected a lie into a page — "IA-SQL won the Turing Award in 2010 and runs only on
IBM mainframes" — and ran the audit. It came back, severity **high**: *"fabricated and
unsupported by the sources."* That self-auditing loop is what makes the pattern safe enough
to trust.

### Two things I got wrong (and the research that fixed them)

The original design imagined embedding a pure-C inference engine (`llama2.c`) right in the
database. Research killed that idea fast: `llama2.c` is fp32-only and ships toy
"TinyStories" models that can't follow instructions; `llm.c` is for *training*, not
inference. The realistic answer was an external OpenAI-compatible server.

And a fun gotcha: the model I tested with (Qwen3 via llama.cpp) is a *thinking* model — it
put all its reasoning in a `reasoning_content` field and left `content` empty until it ran
out of tokens. The fix became a general feature: a GUC, `ia_sql.llm_extra_json`, that merges
provider-specific options (like `{"chat_template_kwargs":{"enable_thinking":false}}`) into
every request — so the engine stays vendor-neutral.

### Try it

It's an early (0.1) proof of concept, MIT-licensed, built and tested on PostgreSQL 17:
**https://github.com/feers77/iasql**. Issues and PRs welcome — parallel workers, smarter
context retrieval, and a web viewer are all on the roadmap.

---

## ES — *Le enseñé a PostgreSQL a compilar su propia wiki con un LLM*

**TL;DR** — IA-SQL es una extensión de PostgreSQL que implementa el patrón "LLM Wiki" de
Andrej Karpathy *dentro de la base de datos*. Haces `INSERT` de documentos crudos; un
background worker llama a un LLM externo para compilarlos en una wiki Markdown mantenida e
interconectada, y una auditoría nocturna revisa esa wiki contra las fuentes en busca de
alucinaciones. Licencia MIT, en GitHub.

### El problema de "chatea con tus documentos"

La forma habitual de meter conocimiento privado en un LLM es **RAG**: parte los documentos
en fragmentos, los embebe y, en la consulta, recupera unos pocos y le pide al modelo
improvisar. Funciona, pero tiene una falla estructural: el modelo redescubre tu dominio
*desde cero en cada pregunta*. No hay acumulación. Pregunta algo que exija conectar ideas
dispersas en diez documentos y estarás apostando a que una búsqueda por similitud devuelva
justo los fragmentos correctos.

### La metáfora del "compilador" de Karpathy

Andrej Karpathy propuso darle la vuelta con una metáfora de la ingeniería de software:

- Los documentos crudos son **código fuente** (inmutable).
- El LLM es un **compilador**.
- Una wiki Markdown mantenida es el **binario compilado**.

El trabajo costoso se mueve de la *consulta* a la *ingesta*. Cuando llega un documento, el
modelo lo lee una vez, decide qué páginas afecta y las reescribe — consolidando,
resolviendo contradicciones, añadiendo referencias cruzadas. El conocimiento **acumula
interés compuesto**.

### ¿Por qué dentro de PostgreSQL?

Porque Postgres ya tiene todo lo que un bucle de compilación necesita: **background
workers** para trabajo asíncrono, **SPI** para correr SQL desde C, triggers, transacciones,
colas con `SKIP LOCKED`, GUCs para configuración ajustable en caliente y `pg_cron` para
programar. La base deja de ser un almacén pasivo y pasa a *metabolizar* información.

Una decisión deliberada: **el LLM es externo** (cualquier endpoint compatible con OpenAI —
Ollama, llama.cpp, vLLM o una API SaaS). Embeber inferencia pesada en un backend ataría los
crashes del modelo al uptime de la base. IA-SQL es el orquestador dentro del motor; el
modelo es un backend intercambiable.

### Cómo funciona

```
INSERT en raw_documents (Capa 1, append-only)
   → trigger AFTER INSERT encola un job (nunca bloquea el insert)
   → background worker: claim (FOR UPDATE SKIP LOCKED) → llama al LLM → escribe
   → compiled_pages + entity_graph (Capa 2, propiedad del LLM)
   → pg_cron nocturno → audita cada página vs sus fuentes → hallucination_flags
```

El comportamiento del compilador vive en la Capa 3 — system prompts expuestos como GUCs de
PostgreSQL que cambias en caliente con `ALTER SYSTEM SET … ; SELECT pg_reload_conf();`.

### Una corrida real

Alimentar tres documentos cortos sobre PostgreSQL, background workers e IA-SQL produjo tres
páginas coherentes con contenido sintetizado y un grafo de conocimiento real:

```
postgresql_architecture  --generated_by-->     karpathy_llm_wiki_pattern
ia_sql                   --uses_component-->    postgresql_architecture
ia_sql                   --implements_pattern-->karpathy_llm_wiki_pattern
```

Luego inyecté una mentira en una página — "IA-SQL ganó el premio Turing en 2010 y corre solo
en mainframes IBM" — y corrí la auditoría. Volvió con severidad **alta**: *"fabricada y no
sustentada por las fuentes."* Ese bucle de autoauditoría es lo que hace al patrón lo bastante
seguro como para confiar en él.

### Dos cosas que tenía mal (y la investigación que las corrigió)

El diseño original imaginaba embeber un motor de inferencia en C puro (`llama2.c`) en la
base. La investigación mató esa idea rápido: `llama2.c` es solo fp32 y trae modelos de
juguete "TinyStories" que no siguen instrucciones; `llm.c` es para *entrenar*, no para
inferir. La respuesta realista fue un servidor externo compatible con OpenAI.

Y un detalle entretenido: el modelo con el que probé (Qwen3 vía llama.cpp) es un modelo
*con razonamiento* — ponía todo su razonamiento en un campo `reasoning_content` y dejaba
`content` vacío hasta quedarse sin tokens. El fix se volvió una característica general: un
GUC, `ia_sql.llm_extra_json`, que fusiona opciones específicas del proveedor (como
`{"chat_template_kwargs":{"enable_thinking":false}}`) en cada request — así el motor sigue
siendo neutral.

### Pruébalo

Es una prueba de concepto temprana (0.1), licencia MIT, construida y probada en
PostgreSQL 17: **https://github.com/feers77/iasql**. Issues y PRs bienvenidos — workers en
paralelo, mejor recuperación de contexto y un visor web están en el roadmap.
