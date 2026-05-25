-- ============================================================================
-- IA-SQL — example role setup (optional, deployment-specific)
-- IA-SQL — configuración de roles de ejemplo (opcional, según despliegue)
-- ----------------------------------------------------------------------------
-- EN: Run as a superuser AFTER `CREATE EXTENSION ia_sql;`. These least-privilege
--     roles map to the user profiles in the tutorial. CHANGE the passwords.
-- ES: Ejecutar como superusuario DESPUÉS de `CREATE EXTENSION ia_sql;`. Estos
--     roles de mínimo privilegio mapean a los perfiles del tutorial. CAMBIA las
--     contraseñas.
-- ============================================================================

-- Ingestor — feeds documents (Layer 1) / alimenta documentos (Capa 1)
CREATE ROLE iasql_ingestor LOGIN PASSWORD 'changeme';
GRANT USAGE ON SCHEMA ia_wiki TO iasql_ingestor;
GRANT INSERT, SELECT ON ia_wiki.raw_documents TO iasql_ingestor;
GRANT SELECT ON ia_wiki.jobs TO iasql_ingestor;   -- watch compilation status

-- Reader — consumes the compiled wiki (Layer 2) / consume la wiki (Capa 2)
CREATE ROLE iasql_reader LOGIN PASSWORD 'changeme';
GRANT USAGE ON SCHEMA ia_wiki TO iasql_reader;
GRANT SELECT ON ia_wiki.compiled_pages, ia_wiki.entity_graph,
                ia_wiki.pages, ia_wiki.processing_log TO iasql_reader;

-- Auditor — reviews/resolves hallucination flags / revisa/resuelve flags
CREATE ROLE iasql_auditor LOGIN PASSWORD 'changeme';
GRANT USAGE ON SCHEMA ia_wiki TO iasql_auditor;
GRANT SELECT ON ia_wiki.raw_documents, ia_wiki.compiled_pages,
                ia_wiki.hallucination_flags TO iasql_auditor;
GRANT UPDATE ON ia_wiki.hallucination_flags TO iasql_auditor;  -- set resolved
GRANT EXECUTE ON FUNCTION ia_wiki.enqueue_lint(integer) TO iasql_auditor;

-- The Admin/DBA profile is a superuser (e.g. postgres): installs the extension,
-- sets GUCs (ALTER SYSTEM), schedules pg_cron. No extra grants needed.
-- El perfil Admin/DBA es un superusuario (p.ej. postgres): instala la extensión,
-- fija GUCs (ALTER SYSTEM), programa pg_cron. No requiere grants extra.
