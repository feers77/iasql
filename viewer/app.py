#!/usr/bin/env python3
"""
IA-SQL viewer — a tiny, read-only web UI for the compiled wiki.

OPTIONAL component. The core of IA-SQL is the PostgreSQL extension; this is just a
convenience read-only viewer (e.g. for a public demo). It connects as a least-privilege
reader role and renders Markdown client-side with marked.js.

Config via environment (see viewer/iasql-viewer.service):
  PGHOST (default 127.0.0.1), PGPORT (5432), PGDATABASE (iasql),
  IASQL_READER_USER, IASQL_READER_PASSWORD,
  VIEWER_BIND (default 127.0.0.1), VIEWER_PORT (default 8000)
"""
import html
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import psycopg2

DB = dict(
    host=os.environ.get("PGHOST", "127.0.0.1"),
    port=os.environ.get("PGPORT", "5432"),
    dbname=os.environ.get("PGDATABASE", "iasql"),
    user=os.environ.get("IASQL_READER_USER", "iasql_reader"),
    password=os.environ.get("IASQL_READER_PASSWORD", ""),
    connect_timeout=5,
)

PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<script src="https://cdn.jsdelivr.net/npm/marked/marked.min.js"></script>
<style>
 :root{{color-scheme:light dark}}
 body{{font:16px/1.6 system-ui,sans-serif;max-width:860px;margin:0 auto;padding:1.5rem}}
 header a{{text-decoration:none;font-weight:600}} nav{{margin:.5rem 0 1.5rem;opacity:.8}}
 .card{{border:1px solid #8884;border-radius:10px;padding:1rem;margin:.6rem 0}}
 .muted{{opacity:.65;font-size:.9em}} code,pre{{background:#8881;border-radius:6px}}
 pre{{padding:1rem;overflow:auto}} table{{border-collapse:collapse;width:100%}}
 td,th{{border:1px solid #8883;padding:.4rem .6rem;text-align:left}}
 .tag{{font-size:.8em;background:#8882;border-radius:999px;padding:.1rem .5rem}}
</style></head><body>
<header><a href="/">IA-SQL · wiki</a> <span class="muted">self-compiling knowledge base</span></header>
<nav><a href="/">Pages</a> · <a href="/graph">Knowledge graph</a> ·
 <a href="https://github.com/feers77/iasql">GitHub</a></nav>
{body}
</body></html>"""


def connect():
    return psycopg2.connect(**DB)


def render_index():
    with connect() as c, c.cursor() as cur:
        cur.execute("SELECT page_entity, title, summary, n_sources, last_compiled "
                    "FROM ia_wiki.pages")
        rows = cur.fetchall()
    items = []
    for ent, title, summary, n, when in rows:
        items.append(
            '<div class="card"><a href="/page?e={e}"><b>{t}</b></a>'
            ' <span class="tag">{n} src</span><br><span class="muted">{s}</span></div>'.format(
                e=html.escape(ent, quote=True), t=html.escape(title or ent),
                s=html.escape(summary or ""), n=n))
    body = "<h1>Compiled pages</h1>" + ("".join(items) or "<p>No pages yet.</p>")
    return PAGE.format(title="IA-SQL wiki", body=body)


def render_page(entity):
    with connect() as c, c.cursor() as cur:
        cur.execute("SELECT title, markdown_body, last_compiled FROM ia_wiki.compiled_pages "
                    "WHERE page_entity=%s", (entity,))
        row = cur.fetchone()
        cur.execute("SELECT relation, target_entity FROM ia_wiki.entity_graph "
                    "WHERE source_entity=%s ORDER BY relation", (entity,))
        edges = cur.fetchall()
    if not row:
        return None
    title, md, when = row
    links = "".join('<li class="muted">{r} → <a href="/page?e={e}">{e}</a></li>'.format(
        r=html.escape(rel), e=html.escape(tgt, quote=True)) for rel, tgt in edges)
    md_js = json.dumps(md or "").replace("</", "<\\/")
    body = ("<article><h1>{t}</h1><div class='muted'>compiled {w}</div>"
            "<div id='md'></div>{rel}"
            "<script>document.getElementById('md').innerHTML=marked.parse({md});</script>"
            "</article>").format(
        t=html.escape(title or entity), w=html.escape(str(when)),
        rel=("<h3>Related</h3><ul>" + links + "</ul>") if links else "", md=md_js)
    return PAGE.format(title=html.escape(title or entity) + " · IA-SQL", body=body)


def render_graph():
    with connect() as c, c.cursor() as cur:
        cur.execute("SELECT source_entity, relation, target_entity, weight "
                    "FROM ia_wiki.entity_graph ORDER BY source_entity")
        rows = cur.fetchall()
    trs = "".join("<tr><td><a href='/page?e={s}'>{s}</a></td><td>{r}</td>"
                  "<td><a href='/page?e={t}'>{t}</a></td><td>{w:.2f}</td></tr>".format(
                      s=html.escape(s, quote=True), r=html.escape(r),
                      t=html.escape(t, quote=True), w=w) for s, r, t, w in rows)
    body = ("<h1>Knowledge graph</h1><table><tr><th>source</th><th>relation</th>"
            "<th>target</th><th>weight</th></tr>" + (trs or "") + "</table>")
    return PAGE.format(title="Knowledge graph · IA-SQL", body=body)


class Handler(BaseHTTPRequestHandler):
    server_version = "ia_sql-viewer/0.1"

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        try:
            if u.path == "/health":
                return self._send(200, "ok", "text/plain")
            if u.path == "/":
                return self._send(200, render_index())
            if u.path == "/graph":
                return self._send(200, render_graph())
            if u.path == "/page":
                e = (parse_qs(u.query).get("e") or [""])[0]
                page = render_page(e)
                return self._send(404, PAGE.format(title="Not found", body="<h1>404</h1>")) \
                    if page is None else self._send(200, page)
            return self._send(404, PAGE.format(title="Not found", body="<h1>404</h1>"))
        except Exception as exc:  # keep the demo resilient
            self._send(503, PAGE.format(title="Error",
                                        body="<h1>Temporarily unavailable</h1>"
                                             "<p class='muted'>%s</p>" % html.escape(str(exc))))

    def log_message(self, *a):  # quiet
        pass


def main():
    bind = os.environ.get("VIEWER_BIND", "127.0.0.1")
    port = int(os.environ.get("VIEWER_PORT", "8000"))
    httpd = ThreadingHTTPServer((bind, port), Handler)
    print("ia_sql viewer on http://%s:%d" % (bind, port), flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
