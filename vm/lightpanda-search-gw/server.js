// Web-search gateway for the Jetson firmware's WebSearchTool.
//
//   GET /search?q=<query>   with header  Authorization: Bearer <GATEWAY_TOKEN>
//   -> {"query":"...","results":[{"title","url","snippet"}, ...]}
//
// Source = Bing HTML results (li.b_algo). Bing is server-rendered and lenient
// enough to serve results to a server IP from the VM (DuckDuckGo Lite started
// rate-limiting/blocking the VM IP mid-deploy, so it was unreliable). LightPanda
// was the original plan but its renderer drops DDG's results <table>; Bing's
// div-based layout parses fine with cheerio directly, so no browser is needed.
// The existing `lightpanda` container is left untouched for any future JS use.
//
// Runs in the `xiaozhi_default` docker network. The Jetson firmware GETs this.
const http = require('http');
const url = require('url');
const cheerio = require('cheerio');

const TOKEN = process.env.GATEWAY_TOKEN || 'changeme';
const PORT   = parseInt(process.env.PORT || '9233', 10);
const UA     = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
               '(KHTML, like Gecko) Chrome/120.0 Safari/537.36';

function b64urlDecode(s) {
  // Bing's `u` param is base64url without padding; restore padding.
  s = s.replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  try { return Buffer.from(s, 'base64').toString('utf-8'); } catch (_) { return ''; }
}

function unwrapBingHref(href) {
  // Organic results come back as https://www.bing.com/ck/a?!&&p=...&u=a1<base64url>
  // where the base64url (after a leading 2-char marker like "a1") is the real URL.
  try {
    const uu = new URL(href);
    if (uu.hostname.endsWith('bing.com') && uu.pathname === '/ck/a') {
      const u = uu.searchParams.get('u');
      if (u && /^a[0-9]/.test(u)) {
        const real = b64urlDecode(u.slice(2));
        if (/^https?:\/\//.test(real)) return real;
      }
    }
  } catch (_) {}
  return href;
}

async function search(query) {
  const u = 'https://www.bing.com/search?q=' + encodeURIComponent(query) +
            '&setlang=en&count=20';
  const r = await fetch(u, { headers: { 'User-Agent': UA, 'Accept-Language': 'en' }, redirect: 'follow' });
  if (!r.ok) throw new Error(`Bing HTTP ${r.status}`);
  const html = await r.text();
  const $ = cheerio.load(html);
  const out = [];
  $('li.b_algo').each((_, el) => {
    const $li = $(el);
    const $a = $li.find('h2 a').first();
    const title = $a.text().trim();
    const raw = $a.attr('href') || '';
    if (!title) return;
    const href = unwrapBingHref(raw);
    if (!/^https?:/i.test(href)) return;
    const snippet = $li.find('.b_caption p').first().text().trim()
      || $li.find('p').first().text().trim();
    out.push({ title, url: href, snippet });
  });
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