#!/usr/bin/env python3
"""
IA-SQL viewer — landing page + read-only web UI for the compiled wiki.

OPTIONAL component. The core of IA-SQL is the PostgreSQL extension; this is a convenience
landing + read-only viewer (e.g. for a public demo). It connects as a least-privilege
reader role and renders Markdown client-side with marked.js.

Routes: / (landing), /wiki (page list), /page?e=<entity>, /graph, /health
Config via environment (see viewer/iasql-viewer.service):
  PGHOST, PGPORT, PGDATABASE, IASQL_READER_USER, IASQL_READER_PASSWORD,
  VIEWER_BIND (default 127.0.0.1), VIEWER_PORT (default 8000)
"""
import html
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import psycopg2

GITHUB = "https://github.com/feers77/iasql"

DB = dict(
    host=os.environ.get("PGHOST", "127.0.0.1"),
    port=os.environ.get("PGPORT", "5432"),
    dbname=os.environ.get("PGDATABASE", "iasql"),
    user=os.environ.get("IASQL_READER_USER", "iasql_reader"),
    password=os.environ.get("IASQL_READER_PASSWORD", ""),
    connect_timeout=5,
)


def connect():
    return psycopg2.connect(**DB)


# --- shared chrome for inner pages (wiki/page/graph) -------------------------
INNER = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<script src="https://cdn.jsdelivr.net/npm/marked/marked.min.js"></script>
<style>
 :root{{color-scheme:light dark}}
 *{{box-sizing:border-box}}
 body{{font:16px/1.65 ui-sans-serif,system-ui,sans-serif;max-width:860px;margin:0 auto;padding:1.5rem;color:#1b1b2b}}
 @media(prefers-color-scheme:dark){{body{{color:#e7e7ee;background:#0f1020}}}}
 a{{color:#5b5bd6}} header a{{text-decoration:none;font-weight:700;font-size:1.1rem}}
 nav{{margin:.4rem 0 1.6rem;opacity:.85;font-size:.95rem}}
 .card{{border:1px solid #8884;border-radius:12px;padding:1rem 1.1rem;margin:.6rem 0}}
 .card:hover{{border-color:#5b5bd6aa}}
 .muted{{opacity:.65;font-size:.92em}} .tag{{font-size:.78em;background:#5b5bd622;color:#5b5bd6;border-radius:999px;padding:.1rem .55rem}}
 code,pre{{background:#8881;border-radius:6px}} pre{{padding:1rem;overflow:auto}}
 table{{border-collapse:collapse;width:100%}} td,th{{border:1px solid #8883;padding:.45rem .6rem;text-align:left}}
 #md h1,#md h2,#md h3{{line-height:1.25}}
</style></head><body>
<header><a href="/">⬡ IA-SQL</a> <span class="muted">self-compiling knowledge base</span></header>
<nav><a href="/wiki">Pages</a> · <a href="/graph">Knowledge graph</a> · <a href="{gh}">GitHub ★</a></nav>
{body}
</body></html>"""


def page_inner(title, body):
    return INNER.format(title=title, body=body, gh=GITHUB)


# --- landing -----------------------------------------------------------------
LANDING = r"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>IA-SQL — a self-compiling knowledge base inside PostgreSQL</title>
<meta name="description" content="IA-SQL turns PostgreSQL into a self-compiling knowledge base: insert documents, an LLM compiles them into a maintained, audited Markdown wiki. Open source, MIT.">
<style>
 :root{--bg:#0b0b16;--fg:#ececf3;--mut:#a6a6c0;--acc:#7c7cf0;--acc2:#22d3ee;--card:#15162a;--line:#2a2b45;color-scheme:dark}
 *{box-sizing:border-box;margin:0;padding:0}
 html.es .en{display:none} html:not(.es) .es{display:none}
 body{font:17px/1.7 ui-sans-serif,system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
 a{color:var(--acc);text-decoration:none} a:hover{text-decoration:underline}
 .wrap{max-width:920px;margin:0 auto;padding:0 1.25rem}
 .top{display:flex;justify-content:space-between;align-items:center;padding:1.1rem 0}
 .brand{font-weight:800;letter-spacing:.3px;font-size:1.15rem}
 .langbtn{background:transparent;border:1px solid var(--line);color:var(--mut);border-radius:999px;padding:.3rem .8rem;cursor:pointer;font:inherit;font-size:.85rem}
 .hero{text-align:center;padding:3.5rem 0 2.5rem;background:radial-gradient(900px 380px at 50% -10%,#4f46e533,transparent)}
 .badge{display:inline-block;border:1px solid var(--line);background:#ffffff08;color:var(--mut);border-radius:999px;padding:.3rem .9rem;font-size:.82rem;margin-bottom:1.3rem}
 h1{font-size:clamp(2.6rem,7vw,4.2rem);line-height:1.02;letter-spacing:-1.5px;background:linear-gradient(120deg,#fff,#a5b4fc 60%,#67e8f9);-webkit-background-clip:text;background-clip:text;color:transparent}
 .tag{font-size:clamp(1.05rem,2.6vw,1.35rem);color:var(--mut);max-width:640px;margin:1.1rem auto 0}
 .cta{display:flex;gap:.8rem;justify-content:center;flex-wrap:wrap;margin:2rem 0 1rem}
 .btn{display:inline-flex;align-items:center;gap:.5rem;padding:.8rem 1.3rem;border-radius:12px;font-weight:600;border:1px solid var(--line)}
 .btn.primary{background:linear-gradient(120deg,#6366f1,#22d3ee);color:#0b0b16;border:0} .btn.primary:hover{text-decoration:none;filter:brightness(1.07)}
 .btn.ghost{background:#ffffff06;color:var(--fg)} .btn.ghost:hover{text-decoration:none;border-color:var(--acc)}
 .stats{display:flex;gap:2rem;justify-content:center;flex-wrap:wrap;margin-top:1.6rem;color:var(--mut);font-size:.95rem}
 .stats b{color:var(--fg);font-size:1.5rem;display:block;font-variant-numeric:tabular-nums}
 section{padding:2.6rem 0;border-top:1px solid var(--line)}
 h2{font-size:1.7rem;letter-spacing:-.5px;margin-bottom:1.1rem}
 .lead{color:var(--mut);max-width:680px;margin-bottom:1.4rem}
 .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:1rem}
 .cell{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1.1rem}
 .cell h3{font-size:1.05rem;margin-bottom:.4rem} .cell p{color:var(--mut);font-size:.95rem}
 .pipe{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1.1rem 1.3rem;overflow:auto}
 .pipe pre{font:13px/1.7 ui-monospace,monospace;color:#cdd0ff}
 .analogy{display:grid;grid-template-columns:repeat(3,1fr);gap:.8rem;margin:.4rem 0 1rem}
 .analogy .cell{text-align:center} .analogy .k{font-size:.8rem;color:var(--mut)} .analogy .v{font-weight:700;margin-top:.2rem}
 .arrow{text-align:center;color:var(--acc);font-size:1.3rem}
 pre.code{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1.1rem;overflow:auto;font:13.5px/1.7 ui-monospace,monospace}
 .kw{color:#c792ea}.fn{color:#82aaff}.str{color:#c3e88d}.cm{color:#7f849c}
 footer{border-top:1px solid var(--line);padding:2rem 0 3rem;color:var(--mut);font-size:.9rem;text-align:center}
 @media(max-width:560px){.analogy{grid-template-columns:1fr}}
</style></head><body>
<div class="wrap">
 <div class="top">
  <span class="brand">⬡ IA-SQL</span>
  <button class="langbtn" onclick="var h=document.documentElement;h.classList.toggle('es');localStorage.setItem('lang',h.classList.contains('es')?'es':'en')">
   <span class="en">Español</span><span class="es">English</span>
  </button>
 </div>
</div>

<div class="hero"><div class="wrap">
 <span class="badge">Open source · MIT · PostgreSQL 17</span>
 <h1>IA-SQL</h1>
 <p class="tag"><span class="en">Turn PostgreSQL into a <b>self-compiling knowledge base</b>. Insert documents; an LLM compiles them into a maintained, cross-referenced, self-audited Markdown wiki.</span><span class="es">Convierte PostgreSQL en una <b>base de conocimiento que se autocompila</b>. Inserta documentos; un LLM los compila en una wiki Markdown mantenida, interconectada y autoauditada.</span></p>
 <div class="cta">
  <a class="btn primary" href="__GH__"><span>★</span> <span class="en">View on GitHub</span><span class="es">Ver en GitHub</span></a>
  <a class="btn ghost" href="/wiki"><span class="en">Browse the live wiki →</span><span class="es">Explora la wiki en vivo →</span></a>
 </div>
 <div class="stats">
  <div><b>__PAGES__</b><span class="en">pages compiled</span><span class="es">páginas compiladas</span></div>
  <div><b>__DOCS__</b><span class="en">source documents</span><span class="es">documentos fuente</span></div>
  <div><b>__EDGES__</b><span class="en">graph relations</span><span class="es">relaciones del grafo</span></div>
 </div>
</div></div>

<section><div class="wrap">
 <h2><span class="en">Not RAG. Compiled knowledge.</span><span class="es">No es RAG. Es conocimiento compilado.</span></h2>
 <p class="lead"><span class="en">RAG re-discovers your domain from scratch on every question. IA-SQL implements Andrej Karpathy's <i>LLM Wiki</i> pattern: do the work once, at ingest time, and let knowledge compound. A software metaphor:</span><span class="es">RAG redescubre tu dominio desde cero en cada pregunta. IA-SQL implementa el patrón <i>LLM Wiki</i> de Andrej Karpathy: haz el trabajo una vez, en la ingesta, y deja que el conocimiento acumule. Una metáfora del software:</span></p>
 <div class="analogy">
  <div class="cell"><div class="k"><span class="en">source code</span><span class="es">código fuente</span></div><div class="v"><span class="en">raw documents</span><span class="es">documentos crudos</span></div></div>
  <div class="cell"><div class="k"><span class="en">compiler</span><span class="es">compilador</span></div><div class="v">the LLM</div></div>
  <div class="cell"><div class="k"><span class="en">compiled binary</span><span class="es">binario compilado</span></div><div class="v"><span class="en">a maintained wiki</span><span class="es">una wiki mantenida</span></div></div>
 </div>
</div></div>

<section><div class="wrap">
 <h2><span class="en">How it works</span><span class="es">Cómo funciona</span></h2>
 <div class="pipe"><pre><span class="en">INSERT into raw_documents          # Layer 1: append-only ground truth
  -> AFTER INSERT trigger enqueues a job (O(1), never blocks)
  -> background worker: claim (SKIP LOCKED) -> call external LLM -> write
  -> compiled_pages + entity_graph  # Layer 2: the wiki (owned by the LLM)
  -> pg_cron nightly lint -> audit pages vs sources -> hallucination_flags</span><span class="es">INSERT en raw_documents            # Capa 1: ground truth append-only
  -> trigger AFTER INSERT encola un job (O(1), nunca bloquea)
  -> background worker: claim (SKIP LOCKED) -> llama al LLM externo -> escribe
  -> compiled_pages + entity_graph  # Capa 2: la wiki (propiedad del LLM)
  -> lint nocturno (pg_cron) -> audita páginas vs fuentes -> hallucination_flags</span></pre></div>
</div></div>

<section><div class="wrap">
 <h2><span class="en">Why it's nice</span><span class="es">Por qué está bueno</span></h2>
 <div class="grid">
  <div class="cell"><h3><span class="en">Async by design</span><span class="es">Asíncrono por diseño</span></h3><p><span class="en">A PostgreSQL background worker compiles in the background; your INSERT returns instantly.</span><span class="es">Un background worker de PostgreSQL compila en segundo plano; tu INSERT retorna al instante.</span></p></div>
  <div class="cell"><h3><span class="en">Immutable truth</span><span class="es">Verdad inmutable</span></h3><p><span class="en">Sources are append-only, so the whole wiki can be recompiled from scratch anytime.</span><span class="es">Las fuentes son append-only, así la wiki puede recompilarse desde cero cuando quieras.</span></p></div>
  <div class="cell"><h3><span class="en">Self-auditing</span><span class="es">Autoauditoría</span></h3><p><span class="en">A nightly lint checks every claim against the sources and flags hallucinations.</span><span class="es">Un lint nocturno contrasta cada afirmación con las fuentes y marca alucinaciones.</span></p></div>
  <div class="cell"><h3><span class="en">Provider-agnostic</span><span class="es">Agnóstico de proveedor</span></h3><p><span class="en">Any OpenAI-compatible endpoint: Ollama, llama.cpp, vLLM, OpenAI. All via GUCs.</span><span class="es">Cualquier endpoint compatible con OpenAI: Ollama, llama.cpp, vLLM, OpenAI. Todo por GUCs.</span></p></div>
 </div>
</div></div>

<section><div class="wrap">
 <h2><span class="en">It's just SQL</span><span class="es">Es solo SQL</span></h2>
 <pre class="code"><span class="cm">-- <span class="en">feed a document; the wiki compiles itself</span><span class="es">alimenta un documento; la wiki se compila sola</span></span>
<span class="kw">INSERT INTO</span> ia_wiki.raw_documents (content)
<span class="kw">VALUES</span> (<span class="str">'PostgreSQL is an extensible, process-based RDBMS...'</span>);

<span class="cm">-- <span class="en">read the compiled knowledge (no model call at query time)</span><span class="es">lee el conocimiento compilado (sin llamar al modelo en la consulta)</span></span>
<span class="kw">SELECT</span> markdown_body <span class="kw">FROM</span> ia_wiki.compiled_pages
 <span class="kw">WHERE</span> page_entity = <span class="str">'postgresql'</span>;</pre>
</div></div>

<footer><div class="wrap">
 <p><span class="en">Built on the <i>LLM Wiki</i> pattern by Andrej Karpathy · MIT licensed</span><span class="es">Basado en el patrón <i>LLM Wiki</i> de Andrej Karpathy · Licencia MIT</span></p>
 <p style="margin-top:.5rem"><a href="__GH__">GitHub</a> · <a href="/wiki"><span class="en">Live wiki</span><span class="es">Wiki en vivo</span></a> · <a href="/graph"><span class="en">Knowledge graph</span><span class="es">Grafo</span></a></p>
</div></footer>
<script>if(localStorage.getItem('lang')==='es')document.documentElement.classList.add('es');</script>
</body></html>"""


def render_landing():
    pages = docs = edges = 0
    try:
        with connect() as c, c.cursor() as cur:
            cur.execute("SELECT count(*) FROM ia_wiki.compiled_pages")
            pages = cur.fetchone()[0]
            cur.execute("SELECT count(*) FROM ia_wiki.entity_graph")
            edges = cur.fetchone()[0]
            cur.execute("SELECT count(DISTINCT doc_id) FROM ia_wiki.processing_log")
            docs = cur.fetchone()[0]
    except Exception:
        pass
    return (LANDING.replace("__GH__", GITHUB)
            .replace("__PAGES__", str(pages))
            .replace("__DOCS__", str(docs))
            .replace("__EDGES__", str(edges)))


def render_index():
    with connect() as c, c.cursor() as cur:
        cur.execute("SELECT page_entity, title, summary, n_sources FROM ia_wiki.pages")
        rows = cur.fetchall()
    items = []
    for ent, title, summary, n in rows:
        items.append(
            '<div class="card"><a href="/page?e={e}"><b>{t}</b></a>'
            ' <span class="tag">{n} src</span><br><span class="muted">{s}</span></div>'.format(
                e=html.escape(ent, quote=True), t=html.escape(title or ent),
                s=html.escape(summary or ""), n=n))
    body = "<h1>Compiled pages</h1>" + ("".join(items) or "<p>No pages yet.</p>")
    return page_inner("IA-SQL wiki", body)


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
    return page_inner(html.escape(title or entity) + " · IA-SQL", body)


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
    return page_inner("Knowledge graph · IA-SQL", body)


class Handler(BaseHTTPRequestHandler):
    server_version = "ia_sql-viewer/0.2"

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
                return self._send(200, render_landing())
            if u.path == "/wiki":
                return self._send(200, render_index())
            if u.path == "/graph":
                return self._send(200, render_graph())
            if u.path == "/page":
                e = (parse_qs(u.query).get("e") or [""])[0]
                page = render_page(e)
                return self._send(404, page_inner("Not found", "<h1>404</h1>")) \
                    if page is None else self._send(200, page)
            return self._send(404, page_inner("Not found", "<h1>404</h1>"))
        except Exception as exc:
            self._send(503, page_inner("Error",
                                       "<h1>Temporarily unavailable</h1>"
                                       "<p class='muted'>%s</p>" % html.escape(str(exc))))

    def log_message(self, *a):
        pass


def main():
    bind = os.environ.get("VIEWER_BIND", "127.0.0.1")
    port = int(os.environ.get("VIEWER_PORT", "8000"))
    httpd = ThreadingHTTPServer((bind, port), Handler)
    print("ia_sql viewer on http://%s:%d" % (bind, port), flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
