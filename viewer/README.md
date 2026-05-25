# IA-SQL viewer (optional) / visor de IA-SQL (opcional)

**EN:** A tiny, dependency-light, **read-only** web UI for the compiled wiki — handy for a
public demo. It is *not* part of the core extension; the database is the product. It
connects as a least-privilege reader role and renders Markdown client-side (marked.js).

**ES:** Una UI web mínima, de pocas dependencias y **solo lectura** para la wiki compilada
— útil para un demo público. *No* es parte de la extensión central; el producto es la base
de datos. Conecta con un rol lector de mínimo privilegio y renderiza Markdown en el cliente.

### Run / Ejecutar

```bash
sudo apt install -y python3-psycopg2
export PGHOST=127.0.0.1 PGDATABASE=iasql
export IASQL_READER_USER=iasql_reader IASQL_READER_PASSWORD=...   # see sql/roles.example.sql
export VIEWER_BIND=127.0.0.1 VIEWER_PORT=8000
python3 app.py
```

Or install the systemd unit `iasql-viewer.service` (adjust paths) and put it behind a
reverse proxy. Routes: `/` (pages), `/page?e=<entity>`, `/graph`, `/health`.
