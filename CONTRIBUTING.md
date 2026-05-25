# Contributing / Cómo contribuir

## English

Thanks for your interest! IA-SQL is an early proof of concept and contributions are very
welcome.

**Getting started**

```bash
sudo apt install -y build-essential libcurl4-openssl-dev postgresql-server-dev-17
git clone https://github.com/feers77/iasql.git && cd iasql
make && sudo make install
```

Then enable the worker (`shared_preload_libraries = 'pg_cron,ia_sql'`), restart PostgreSQL,
`CREATE EXTENSION ia_sql;`, and point it at any OpenAI-compatible endpoint (Ollama is the
easiest for local dev). See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Guidelines**

- Match the existing C style (PostgreSQL conventions: tabs, `PG_TRY`, `palloc`).
- Keep the LLM provider-agnostic — provider quirks belong in `ia_sql.llm_extra_json`, not in
  the code.
- Validate a clean install: `CREATE EXTENSION` on a fresh database must succeed.
- Documentation is **bilingual (English + Spanish)** — please update both.

**Good first issues**

Parallel workers, smarter context selection, a read-only web viewer, packaging for the
PostgreSQL APT repos. See the Roadmap in the README.

## Español

¡Gracias por tu interés! IA-SQL es una prueba de concepto temprana y las contribuciones son
muy bienvenidas.

**Para empezar**

```bash
sudo apt install -y build-essential libcurl4-openssl-dev postgresql-server-dev-17
git clone https://github.com/feers77/iasql.git && cd iasql
make && sudo make install
```

Luego habilita el worker (`shared_preload_libraries = 'pg_cron,ia_sql'`), reinicia
PostgreSQL, `CREATE EXTENSION ia_sql;` y apúntalo a cualquier endpoint compatible con OpenAI
(Ollama es lo más fácil para desarrollo local). Ver [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Lineamientos**

- Sigue el estilo C existente (convenciones de PostgreSQL: tabs, `PG_TRY`, `palloc`).
- Mantén el LLM agnóstico del proveedor — las particularidades van en
  `ia_sql.llm_extra_json`, no en el código.
- Valida un install limpio: `CREATE EXTENSION` en una base nueva debe funcionar.
- La documentación es **bilingüe (inglés + español)** — actualiza ambos.

**Buenas primeras tareas**

Workers en paralelo, selección de contexto más inteligente, un visor web de solo lectura,
empaquetado para los repos APT de PostgreSQL. Ver el Roadmap en el README.
