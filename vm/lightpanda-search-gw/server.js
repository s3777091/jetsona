// Web gateway for the Jetson firmware's agent tools (Bearer <GATEWAY_TOKEN>):
//
//   GET /search?q=<query>
//   -> {"query":"...","results":[{"title","url","snippet"}, ...]}
//
//   GET /fetch?url=<http(s) url>          (the firmware's web_open tool)
//   -> {"url":"...","source":"lightpanda"|"fetch","title":"...","text":"..."}
//
// /search source = Bing HTML results (li.b_algo). Bing is server-rendered and
// lenient enough to serve results to a server IP from the VM (DuckDuckGo Lite
// started rate-limiting/blocking the VM IP mid-deploy, so it was unreliable).
// LightPanda drops DDG's results <table>, so search needs no browser at all.
//
// /fetch is where the `lightpanda` container earns its keep: pages are loaded
// through its CDP endpoint (puppeteer-core) so client-side-rendered content is
// visible; anything LightPanda chokes on (beta, incomplete Web APIs) falls back
// to a plain fetch + cheerio text extraction.
//
// Runs in the `xiaozhi_default` docker network. The Jetson firmware GETs this.
const http = require('http');
const url = require('url');
const cheerio = require('cheerio');
const puppeteer = require('puppeteer-core');

const TOKEN = process.env.GATEWAY_TOKEN || 'changeme';
const PORT   = parseInt(process.env.PORT || '9233', 10);
// CDP endpoint of the pre-existing lightpanda container on xiaozhi_default.
const LIGHTPANDA_CDP = process.env.LIGHTPANDA_CDP || 'ws://lightpanda:9222';
const FETCH_MAX_CHARS = parseInt(process.env.FETCH_MAX_CHARS || '6000', 10);
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

// ---- /fetch: read a page's text (LightPanda CDP, plain-fetch fallback) ----

function squashWhitespace(s) {
  return (s || '').replace(/[ \t ]+/g, ' ').replace(/\s*\n\s*/g, '\n').trim();
}

function withTimeout(promise, ms, label) {
  let timer;
  const guard = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timeout ${ms}ms`)), ms);
  });
  return Promise.race([promise, guard]).finally(() => clearTimeout(timer));
}

async function fetchViaLightpanda(target) {
  // Connect per request: LightPanda sessions are cheap, and a stale shared
  // connection would break every request after a container restart.
  const browser = await withTimeout(
    puppeteer.connect({ browserWSEndpoint: LIGHTPANDA_CDP }), 5000, 'CDP connect');
  try {
    const context = await browser.createBrowserContext();
    const page = await context.newPage();
    await withTimeout(page.goto(target, { waitUntil: 'load' }), 25000, 'goto');
    // textContent, not innerText: LightPanda does no layout, so the
    // layout-aware innerText is unreliable there.
    const got = await page.evaluate(() => ({
      title: document.title || '',
      text: document.body ? document.body.textContent : '',
    }));
    await context.close();
    return { title: squashWhitespace(got.title), text: squashWhitespace(got.text) };
  } finally {
    // disconnect(), not close(): the shared lightpanda container must survive.
    try { await browser.disconnect(); } catch (_) {}
  }
}

async function fetchPlain(target) {
  const r = await fetch(target, {
    headers: { 'User-Agent': UA, 'Accept-Language': 'en,vi;q=0.8' },
    redirect: 'follow',
    signal: AbortSignal.timeout(25000),
  });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const html = await r.text();
  const $ = cheerio.load(html);
  $('script, style, noscript, svg, iframe, nav, footer, header').remove();
  const title = $('title').first().text();
  const main = $('main, article, [role=main]').first();
  const text = (main.length ? main : $('body')).text();
  return { title: squashWhitespace(title), text: squashWhitespace(text) };
}

async function fetchPage(target) {
  try {
    const got = await fetchViaLightpanda(target);
    // A JS-heavy page can "load" into an empty shell on a partial engine;
    // treat a near-empty body as a miss so the fallback gets its chance.
    if (got.text && got.text.length >= 200) return { source: 'lightpanda', ...got };
    const plain = await fetchPlain(target);
    return plain.text.length > got.text.length
      ? { source: 'fetch', ...plain }
      : { source: 'lightpanda', ...got };
  } catch (e) {
    console.error('lightpanda fetch failed, falling back:', (e && e.message) || e);
    return { source: 'fetch', ...(await fetchPlain(target)) };
  }
}

const srv = http.createServer(async (req, res) => {
  const parsed = url.parse(req.url, true);
  const auth = req.headers.authorization || '';
  const tok = auth.startsWith('Bearer ') ? auth.slice(7) : (parsed.query.token || '');
  if (tok !== TOKEN) { res.writeHead(401); res.end('unauthorized'); return; }
  if (parsed.pathname === '/healthz') {
    res.writeHead(200); res.end('ok'); return;
  }
  if (parsed.pathname === '/fetch') {
    const target = (parsed.query.url || '').toString().trim();
    if (!/^https?:\/\//i.test(target)) {
      res.writeHead(400); res.end('missing/invalid url'); return;
    }
    try {
      const page = await fetchPage(target);
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({
        url: target,
        source: page.source,
        title: page.title,
        text: page.text.slice(0, FETCH_MAX_CHARS),
      }));
    } catch (e) {
      console.error('fetch error:', (e && e.stack) || e);
      res.writeHead(502, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ error: String((e && e.message) || e) }));
    }
    return;
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