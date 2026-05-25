#!/usr/bin/env python3
"""
Generate a STATIC version of the landing page for GitHub Pages.

Single source of truth: the LANDING template in app.py. This produces site/index.html
where:
  - GitHub link and internal nav point to absolute URLs (the live demo for /wiki, /graph),
  - the live stats are fetched client-side from the demo's /stats.json (with a graceful
    fallback to "—" if the demo is offline).

Usage:  python3 viewer/build_static_landing.py   ->   site/index.html
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app  # noqa: E402  (LANDING + GITHUB live here)

DEMO = os.environ.get("IASQL_DEMO_URL", "https://iasql.dev.feres.cl")

FETCH = """
<script>
fetch("%s/stats.json").then(function(r){return r.json();}).then(function(s){
  var m={pages:"st-pages",docs:"st-docs",edges:"st-edges"};
  for(var k in m){var el=document.getElementById(m[k]); if(el&&s[k]!=null) el.textContent=s[k];}
}).catch(function(){});
</script>
""" % DEMO


def build():
    h = app.LANDING
    h = h.replace("__GH__", app.GITHUB)
    h = h.replace("__PAGES__", "—").replace("__DOCS__", "—").replace("__EDGES__", "—")
    h = h.replace('href="/wiki"', 'href="%s/wiki"' % DEMO)
    h = h.replace('href="/graph"', 'href="%s/graph"' % DEMO)
    h = h.replace("</body></html>", FETCH + "</body></html>")
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "site")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "index.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write(h)
    print("wrote", out, "(%d bytes)" % len(h))

    base = "https://feers77.github.io/iasql/"
    with open(os.path.join(out_dir, "robots.txt"), "w") as f:
        f.write("User-agent: *\nAllow: /\nSitemap: %ssitemap.xml\n" % base)
    with open(os.path.join(out_dir, "sitemap.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
                '  <url><loc>%s</loc><changefreq>weekly</changefreq>'
                '<priority>1.0</priority></url>\n</urlset>\n' % base)
    print("wrote robots.txt + sitemap.xml")


if __name__ == "__main__":
    build()
