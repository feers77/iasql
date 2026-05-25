-- ============================================================================
-- IA-SQL  ·  ia_sql--0.1.sql   (schema portion / parte de esquema)
-- ----------------------------------------------------------------------------
-- EN: Implements Andrej Karpathy's "LLM Wiki" pattern inside PostgreSQL.
--     Raw documents are the immutable "source code" (ground truth); an LLM acts
--     as a "compiler" that synthesises a maintained Markdown wiki; a lint step
--     audits the wiki against the sources.
-- ES: Implementa el patrón "LLM Wiki" de Andrej Karpathy dentro de PostgreSQL.
--     Los documentos crudos son el "código fuente" inmutable (ground truth); un
--     LLM actúa como "compilador" que sintetiza una Wiki Markdown mantenida; un
--     paso de lint audita la Wiki contra las fuentes.
--
-- NOTE/NOTA: C functions (GUCs, background worker, helpers) are declared in the
--   companion sections added by the C extension build. This file is also usable
--   stand-alone for schema testing.
-- ============================================================================

CREATE SCHEMA IF NOT EXISTS ia_wiki;
COMMENT ON SCHEMA ia_wiki IS 'IA-SQL: LLM Wiki knowledge base (3 layers).';

-- ----------------------------------------------------------------------------
-- LAYER 1 / CAPA 1 — Ground Truth (append-only)
-- EN: The immutable source of truth. Inserts only; never updated or deleted,
--     so the wiki can always be recompiled from scratch (re-bootstrap).
-- ES: La verdad base inmutable. Solo inserciones; nunca se actualiza ni borra,
--     para poder recompilar la Wiki desde cero (re-bootstrap).
-- ----------------------------------------------------------------------------
CREATE TABLE ia_wiki.raw_documents (
    doc_id      uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    source      text,                                   -- EN: origin label / ES: etiqueta de origen
    content     text NOT NULL,                          -- EN: raw text / ES: texto crudo
    metadata    jsonb NOT NULL DEFAULT '{}',
    inserted_at timestamptz NOT NULL DEFAULT now()
);
COMMENT ON TABLE ia_wiki.raw_documents IS 'Layer 1 / Capa 1: append-only ground truth.';

-- Append-only guard / Guarda append-only
CREATE OR REPLACE FUNCTION ia_wiki.guard_append_only() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'ia_wiki.raw_documents is append-only (no UPDATE/DELETE) '
                    '/ es append-only (sin UPDATE/DELETE)';
END $$;

CREATE TRIGGER raw_documents_append_only
    BEFORE UPDATE OR DELETE ON ia_wiki.raw_documents
    FOR EACH ROW EXECUTE FUNCTION ia_wiki.guard_append_only();

-- ----------------------------------------------------------------------------
-- JOB QUEUE / COLA DE TRABAJOS
-- EN: Decouples ingestion from compilation. The trigger enqueues; the worker
--     consumes. 'ingest' compiles a new doc into the wiki; 'lint' audits a page.
-- ES: Desacopla la ingesta de la compilación. El trigger encola; el worker
--     consume. 'ingest' compila un doc nuevo; 'lint' audita una página.
-- ----------------------------------------------------------------------------
CREATE TABLE ia_wiki.jobs (
    job_id      bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    doc_id      uuid REFERENCES ia_wiki.raw_documents(doc_id),
    kind        text NOT NULL DEFAULT 'ingest' CHECK (kind IN ('ingest','lint')),
    status      text NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending','running','done','error')),
    attempts    int  NOT NULL DEFAULT 0,
    error       text,
    payload     jsonb NOT NULL DEFAULT '{}',
    created_at  timestamptz NOT NULL DEFAULT now(),
    started_at  timestamptz,
    finished_at timestamptz
);
CREATE INDEX jobs_pending_idx ON ia_wiki.jobs (job_id) WHERE status = 'pending';
COMMENT ON TABLE ia_wiki.jobs IS 'Async job queue consumed by the ia_sql background worker.';

-- ----------------------------------------------------------------------------
-- LAYER 2 / CAPA 2 — The Wiki (LLM-owned, mutable)
-- ----------------------------------------------------------------------------
CREATE TABLE ia_wiki.compiled_pages (
    page_entity    text PRIMARY KEY,                    -- EN: stable entity key / ES: clave de entidad
    title          text,
    markdown_body  text,
    summary        text,
    source_doc_ids uuid[] NOT NULL DEFAULT '{}',        -- EN: provenance / ES: procedencia
    last_compiled  timestamptz NOT NULL DEFAULT now()
);
COMMENT ON TABLE ia_wiki.compiled_pages IS 'Layer 2 / Capa 2: synthesised Markdown wiki pages.';

CREATE TABLE ia_wiki.entity_graph (
    source_entity text NOT NULL,
    target_entity text NOT NULL,
    relation      text NOT NULL DEFAULT 'related',
    weight        real NOT NULL DEFAULT 1.0,
    PRIMARY KEY (source_entity, target_entity, relation)
);
COMMENT ON TABLE ia_wiki.entity_graph IS 'Layer 2 / Capa 2: typed relations between wiki entities.';

CREATE TABLE ia_wiki.processing_log (
    id                bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    doc_id            uuid,
    job_id            bigint,
    model             text,
    prompt_tokens     int,
    completion_tokens int,
    latency_ms        int,
    created_at        timestamptz NOT NULL DEFAULT now()
);
COMMENT ON TABLE ia_wiki.processing_log IS 'Per-compilation telemetry (model, tokens, latency).';

-- ----------------------------------------------------------------------------
-- LINT / AUDITORÍA — hallucination flags
-- EN: The self-healing audit: claims unsupported by the ground truth are flagged.
-- ES: La auditoría autocurativa: se marcan afirmaciones no sustentadas por las fuentes.
-- ----------------------------------------------------------------------------
CREATE TABLE ia_wiki.hallucination_flags (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    page_entity      text,
    severity         text CHECK (severity IN ('low','medium','high')),
    description      text,
    evidence_doc_ids uuid[] NOT NULL DEFAULT '{}',
    detected_at      timestamptz NOT NULL DEFAULT now(),
    resolved         boolean NOT NULL DEFAULT false
);
COMMENT ON TABLE ia_wiki.hallucination_flags IS 'Lint findings: unsupported/contradictory claims.';

-- ----------------------------------------------------------------------------
-- ENQUEUE TRIGGER / TRIGGER DE ENCOLADO  (O(1), never blocks the INSERT)
-- EN: On a new document, enqueue an 'ingest' job and NOTIFY the worker.
-- ES: Ante un documento nuevo, encola un job 'ingest' y NOTIFY al worker.
-- ----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ia_wiki.enqueue_ingest() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO ia_wiki.jobs (doc_id, kind) VALUES (NEW.doc_id, 'ingest');
    PERFORM pg_notify('ia_sql_jobs', NEW.doc_id::text);
    RETURN NEW;
END $$;

CREATE TRIGGER raw_documents_enqueue
    AFTER INSERT ON ia_wiki.raw_documents
    FOR EACH ROW EXECUTE FUNCTION ia_wiki.enqueue_ingest();

-- ----------------------------------------------------------------------------
-- CONVENIENCE VIEW / VISTA DE CONVENIENCIA
-- ----------------------------------------------------------------------------
CREATE VIEW ia_wiki.pages AS
    SELECT page_entity, title, summary, last_compiled,
           COALESCE(array_length(source_doc_ids, 1), 0) AS n_sources
    FROM ia_wiki.compiled_pages
    ORDER BY last_compiled DESC;
COMMENT ON VIEW ia_wiki.pages IS 'Human-friendly listing of compiled wiki pages.';

-- ----------------------------------------------------------------------------
-- C-LANGUAGE FUNCTIONS / FUNCIONES EN C  (provided by the ia_sql module)
-- ----------------------------------------------------------------------------
CREATE FUNCTION ia_wiki.version() RETURNS text
    AS 'MODULE_PATHNAME', 'ia_sql_version' LANGUAGE C STRICT;
COMMENT ON FUNCTION ia_wiki.version() IS 'IA-SQL engine version / versión del motor.';

-- ----------------------------------------------------------------------------
-- LINT SCHEDULING / PROGRAMACIÓN DEL LINT
-- EN: Enqueue 'lint' jobs for the N least-recently-compiled pages. The worker
--     audits each page against its source documents (Layer 1) and records any
--     unsupported claim in ia_wiki.hallucination_flags. Schedule with pg_cron.
-- ES: Encola jobs 'lint' para las N páginas compiladas hace más tiempo. El worker
--     audita cada página contra sus documentos fuente (Capa 1) y registra toda
--     afirmación no sustentada en ia_wiki.hallucination_flags. Programar con pg_cron.
-- ----------------------------------------------------------------------------
CREATE FUNCTION ia_wiki.enqueue_lint(sample integer DEFAULT 20)
RETURNS integer LANGUAGE plpgsql AS $$
DECLARE
    n integer;
BEGIN
    INSERT INTO ia_wiki.jobs (kind, payload)
    SELECT 'lint', jsonb_build_object('page_entity', page_entity)
    FROM ia_wiki.compiled_pages
    ORDER BY last_compiled ASC
    LIMIT sample;
    GET DIAGNOSTICS n = ROW_COUNT;
    PERFORM pg_notify('ia_sql_jobs', 'lint');
    RETURN n;
END $$;
COMMENT ON FUNCTION ia_wiki.enqueue_lint(integer) IS
    'Enqueue lint/audit jobs for the N oldest pages / Encola jobs de auditoría para las N páginas más antiguas.';
