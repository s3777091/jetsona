// Web-search gateway for the Jetson firmware's WebSearchTool.
//
//   GET /search?q=<query>   with header  Authorization: Bearer <GATEWAY_TOKEN>
//   -> {"query":"...","results":[{"title","url","snippet"}, ...]}
//
// Why direct fetch (not LightPanda): DuckDuckGo Lite is *static server-rendered
// HTML* — no JS needed — and LightPanda's renderer drops the results <table>
// (verified: it loads the page but returns 0 `a.result-link`, only 2 anchors).
// So we fetch lite.duckduckgo.com directly and parse with cheerio. The existing
// `lightpanda` container is left untouched; it stays available for any future
// JS-heavy scraping the agent might need.
//
// Runs in the `xiaozhi_default` docker network (harmless; keeps it next to the
// rest of the stack). The Jetson firmware GETs this endpoint.
const http = require('http');
const url = require('url');
const cheerio = require('cheerio');

const TOKEN = process.env.GATEWAY_TOKEN || 'changeme';
const PORT   = parseInt(process.env.PORT || '9233', 10);
const UA     = 'Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 ' +
               '(KHTML, like Gecko) Chrome/120.0 Safari/537.36';

function unwrapDdgHref(href) {
  // DDG sometimes wraps organic/redirect links as
  // //duckduckgo.com/l/?uddg=<encoded-url>; unwrap to the real target.
  if (!href) return '';
  const m = href.match(/[?&]uddg=([^&]+)/);
  if (m) {
    try { return decodeURIComponent(m[1]); } catch (_) { return href; }
  }
  if (href.startsWith('//')) return 'https:' + href;
  return href;
}

async function search(query) {
  const u = 'https://lite.duckduckgo.com/lite/?q=' + encodeURIComponent(query);
  const r = await fetch(u, { headers: { 'User-Agent': UA, 'Accept': 'text/html' }, redirect: 'follow' });
  if (!r.ok) throw new Error(`DDG lite HTTP ${r.status}`);
  const html = await r.text();
  const $ = cheerio.load(html);
  const out = [];
  // DDG Lite: each result row has an <a class="result-link">; the snippet sits
  // in a sibling <td class="result-snippet"> within the same result <tr>.
  $('a.result-link').each((_, el) => {
    const $a = $(el);
    const title = $a.text().trim();
    const href = unwrapDdgHref($a.attr('href') || '');
    if (!title || !/^https?:/i.test(href)) return;
    // Walk up to the enclosing result row, then find its snippet.
    const $row = $a.closest('tr');
    const snippet = $row.find('td.result-snippet').text().trim()
      || $row.find('.result-snippet').text().trim();
    out.push({ title, url: href, snippet });
  });
  // De-dupe by url (DDG sometimes lists the same domain twice).
  const seen = new Set();
  const dedup = [];
  for (const r of out) {
    if (seen.has(r.url)) continue;
    seen.add(r.url);
    dedup.push(r);
  }
  return dedup.slice(0, 8);
}

const srv = http.createServer(async (req, res) => {
  const parsed = url.parse(req.url, true);
  const auth = req.headers.authorization || '';
  const tok = auth.startsWith('Bearer ') ? auth.slice(7) : (parsed.query.token || '');
  if (tok !== TOKEN) { res.writeHead(401); res.end('unauthorized'); return; }
  if (parsed.pathname === '/healthz') {
    res.writeHead(200); res.end('ok'); return;
  }
  if (parsed.pathname !== '/search') {
    res.writeHead(404); res.end('not found'); return;
  }
  const q = (parsed.query.q || '').toString().trim();
  if (!q) { res.writeHead(400); res.end('missing q'); return; }
  try {
    const results = await search(q);
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({ query: q, results }));
  } catch (e) {
    console.error('search error:', e && e.stack || e);
    res.writeHead(502, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({ error: String((e && e.message) || e), results: [] }));
  }
});

srv.listen(PORT, '0.0.0.0', () => {
  console.log(`lightpanda-search-gw listening on :${PORT}`);
});