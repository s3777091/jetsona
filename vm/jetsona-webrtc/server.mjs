// Custom Next.js server.
//
// Why custom (not `next start`): the App Router route handlers are HTTP-only and
// cannot accept a WebSocket upgrade. The input bridge is a raw WebSocket, so we
// attach a `ws` WebSocketServer to the same HTTP server and proxy /input upgrades
// to the upstream jetsona_webrtc_input.py on the Jetson (INPUT_UPSTREAM).
//
// This same process therefore owns, at the site root:
//   GET  /                        -> the console page (Next.js)
//   GET  /login                   -> the sign-in page (Next.js, public)
//   POST /api/auth/login          -> sign in, sets the session cookie (this file)
//   POST /api/auth/logout         -> clears it (this file)
//   GET  /api/auth/verify         -> 204/401, for Caddy `forward_auth` (this file)
//   POST /media/jetsona/whep      -> WHEP proxy (Next.js route handler)
//   WS   /input?token=...         -> input WebSocket proxy (this file)
//
// Auth is a signed, HttpOnly session cookie rather than HTTP Basic: Basic has no
// sign-out, cannot be styled, and re-prompts natively on every 401. Sessions are
// stateless (HMAC over user + expiry), so nothing needs to survive a restart
// except the secret.

import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import { createServer } from 'node:http';
import { parse } from 'node:url';
import next from 'next';
import { WebSocket, WebSocketServer } from 'ws';

const dev = process.env.NODE_ENV !== 'production';
const hostname = process.env.HOST || '0.0.0.0';
const port = parseInt(process.env.PORT || '3000', 10);

const inputUpstream = process.env.INPUT_UPSTREAM || '';
const inputToken = process.env.JETSONA_INPUT_TOKEN || '';

// AUTH_* is the current spelling; BASIC_* is still read so an existing
// /etc/jetsona-webrtc.env keeps working untouched.
const authUser = process.env.AUTH_USER || process.env.BASIC_USER || '';
const authPass = process.env.AUTH_PASS || process.env.BASIC_PASS || '';
const authEnabled = Boolean(authUser && authPass);

// A stable SESSION_SECRET keeps sessions valid across restarts. Without one we
// derive from the password (so changing the password signs everybody out) and
// fall back to a random per-process key if auth is off entirely.
const sessionSecret =
  process.env.SESSION_SECRET ||
  (authPass ? createHmac('sha256', 'jetsona/session/v1').update(authPass).digest('hex') : null) ||
  randomBytes(32).toString('hex');

const SESSION_COOKIE = 'jetsona_session';
const SESSION_TTL_MS = 7 * 24 * 60 * 60 * 1000;

const app = next({ dev, hostname, port });
const handle = app.getRequestHandler();

// --- Session ---------------------------------------------------------------

const b64url = (input) => Buffer.from(input).toString('base64url');

function signSession(user, expiresAt) {
  const payload = b64url(JSON.stringify({ u: user, e: expiresAt }));
  const mac = createHmac('sha256', sessionSecret).update(payload).digest('base64url');
  return `${payload}.${mac}`;
}

function verifySession(value) {
  if (!value) return null;
  const dot = value.lastIndexOf('.');
  if (dot < 1) return null;
  const payload = value.slice(0, dot);
  const mac = value.slice(dot + 1);
  const expected = createHmac('sha256', sessionSecret).update(payload).digest('base64url');
  if (!safeEqual(mac, expected)) return null;
  try {
    const { u, e } = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8'));
    if (typeof e !== 'number' || Date.now() > e) return null;
    return { user: u };
  } catch {
    return null;
  }
}

/** Length-safe constant-time compare for same-charset strings. */
function safeEqual(a, b) {
  const bufA = Buffer.from(String(a));
  const bufB = Buffer.from(String(b));
  if (bufA.length !== bufB.length) return false;
  return timingSafeEqual(bufA, bufB);
}

/** Credentials are compared as digests so the compare is always fixed-length. */
function credentialsMatch(user, pass) {
  const digest = (value) => createHmac('sha256', sessionSecret).update(String(value)).digest();
  return (
    timingSafeEqual(digest(user), digest(authUser)) &&
    timingSafeEqual(digest(pass), digest(authPass))
  );
}

function readCookie(req, name) {
  const header = req.headers.cookie;
  if (!header) return null;
  for (const part of header.split(';')) {
    const eq = part.indexOf('=');
    if (eq < 0) continue;
    if (part.slice(0, eq).trim() === name) return decodeURIComponent(part.slice(eq + 1).trim());
  }
  return null;
}

function isSecureRequest(req) {
  return (req.headers['x-forwarded-proto'] || '').split(',')[0].trim() === 'https';
}

function sessionCookie(req, value, maxAgeSec) {
  const attrs = [
    `${SESSION_COOKIE}=${value}`,
    'Path=/',
    'HttpOnly',
    'SameSite=Lax',
    `Max-Age=${maxAgeSec}`,
  ];
  if (isSecureRequest(req)) attrs.push('Secure');
  return attrs.join('; ');
}

function isAuthed(req) {
  if (!authEnabled) return true;
  return Boolean(verifySession(readCookie(req, SESSION_COOKIE)));
}

// Paths reachable without a session. `/_next/static` must be public or the login
// page would render unstyled; those bundles carry no privileged data.
function isPublicPath(pathname) {
  return (
    pathname === '/login' ||
    pathname === '/favicon.ico' ||
    pathname.startsWith('/api/auth/') ||
    pathname.startsWith('/_next/static/')
  );
}

// --- Brute-force throttle --------------------------------------------------
// In-memory and per-IP: enough to make online guessing impractical without
// pulling in a store. Resets on restart, which is acceptable here.

const attempts = new Map();
const MAX_ATTEMPTS = 8;
const ATTEMPT_WINDOW_MS = 10 * 60 * 1000;

function clientIp(req) {
  const forwarded = (req.headers['x-forwarded-for'] || '').split(',')[0].trim();
  return forwarded || req.socket.remoteAddress || 'unknown';
}

function throttled(ip) {
  const entry = attempts.get(ip);
  if (!entry) return false;
  if (Date.now() - entry.first > ATTEMPT_WINDOW_MS) {
    attempts.delete(ip);
    return false;
  }
  return entry.count >= MAX_ATTEMPTS;
}

function recordFailure(ip) {
  const entry = attempts.get(ip);
  if (!entry || Date.now() - entry.first > ATTEMPT_WINDOW_MS) {
    attempts.set(ip, { count: 1, first: Date.now() });
    return;
  }
  entry.count += 1;
}

// --- HTTP ------------------------------------------------------------------

function sendJson(res, status, body, headers = {}) {
  res.writeHead(status, {
    'Content-Type': 'application/json',
    'Cache-Control': 'no-store',
    ...headers,
  });
  res.end(JSON.stringify(body));
}

function readBody(req, limit = 4096) {
  return new Promise((resolve, reject) => {
    let data = '';
    req.on('data', (chunk) => {
      data += chunk;
      if (data.length > limit) {
        reject(new Error('payload too large'));
        req.destroy();
      }
    });
    req.on('end', () => resolve(data));
    req.on('error', reject);
  });
}

async function handleLogin(req, res) {
  const ip = clientIp(req);
  if (throttled(ip)) {
    return sendJson(res, 429, { ok: false, error: 'Quá nhiều lần thử. Đợi vài phút rồi thử lại.' });
  }
  let username = '';
  let password = '';
  try {
    const parsed = JSON.parse(await readBody(req));
    username = String(parsed.username ?? '');
    password = String(parsed.password ?? '');
  } catch {
    return sendJson(res, 400, { ok: false, error: 'Dữ liệu không hợp lệ.' });
  }

  if (!credentialsMatch(username, password)) {
    recordFailure(ip);
    return sendJson(res, 401, { ok: false, error: 'Sai tài khoản hoặc mật khẩu.' });
  }

  attempts.delete(ip);
  const token = signSession(authUser, Date.now() + SESSION_TTL_MS);
  sendJson(res, 200, { ok: true }, { 'Set-Cookie': sessionCookie(req, token, SESSION_TTL_MS / 1000) });
}

await app.prepare();

const server = createServer(async (req, res) => {
  const { pathname } = parse(req.url, true);

  if (pathname === '/api/auth/login' && req.method === 'POST') {
    if (!authEnabled) return sendJson(res, 500, { ok: false, error: 'Chưa cấu hình tài khoản.' });
    return handleLogin(req, res);
  }

  if (pathname === '/api/auth/logout' && req.method === 'POST') {
    return sendJson(res, 200, { ok: true }, { 'Set-Cookie': sessionCookie(req, '', 0) });
  }

  // Probe endpoint for Caddy `forward_auth`, so routes proxied around this
  // server (e.g. the /pc relay) can reuse the same session.
  if (pathname === '/api/auth/verify') {
    if (isAuthed(req)) {
      res.writeHead(204, { 'Cache-Control': 'no-store' });
      return res.end();
    }
    return sendJson(res, 401, { ok: false });
  }

  if (!isPublicPath(pathname) && !isAuthed(req)) {
    // Navigations get bounced to the form; anything else gets a plain 401 so
    // fetch() callers see a status instead of a login page body.
    const accept = req.headers.accept || '';
    if (req.method === 'GET' && accept.includes('text/html')) {
      const next = encodeURIComponent(req.url || '/');
      res.writeHead(302, { Location: `/login?next=${next}`, 'Cache-Control': 'no-store' });
      return res.end();
    }
    return sendJson(res, 401, { ok: false, error: 'unauthorized' });
  }

  handle(req, res, parse(req.url, true));
});

const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) => {
  const { pathname, query } = parse(req.url, true);

  if (pathname !== '/input') {
    // No other WebSocket endpoint is expected. Drop it.
    socket.destroy();
    return;
  }

  if (!isAuthed(req)) {
    socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
    socket.destroy();
    return;
  }

  if (inputToken && query.token !== inputToken) {
    socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
    socket.destroy();
    return;
  }

  if (!inputUpstream) {
    socket.write('HTTP/1.1 502 Bad Gateway\r\n\r\n');
    socket.destroy();
    return;
  }

  wss.handleUpgrade(req, socket, head, (ws) => proxyInput(ws));
});

function proxyInput(ws) {
  let upstream;
  try {
    upstream = new WebSocket(inputUpstream);
  } catch {
    ws.close(1011, 'upstream invalid');
    return;
  }

  // The browser may send input frames the instant its socket opens, before the
  // upstream connection is ready. Buffer client frames and flush once the
  // upstream is OPEN so nothing is dropped during that race.
  const pending = [];
  const flush = () => {
    while (pending.length && upstream.readyState === WebSocket.OPEN) {
      upstream.send(pending.shift());
    }
  };

  ws.on('message', (data) => {
    if (upstream.readyState === WebSocket.OPEN) upstream.send(data);
    else pending.push(data);
  });
  upstream.on('message', (data) => {
    if (ws.readyState === WebSocket.OPEN) ws.send(data);
  });
  upstream.on('open', flush);

  const teardown = () => {
    try { upstream.close(); } catch {}
    try { ws.close(); } catch {}
  };
  ws.on('close', () => upstream.close());
  ws.on('error', teardown);
  upstream.on('close', () => ws.close());
  upstream.on('error', teardown);
}

server.listen(port, hostname, () => {
  console.log(`> jetsona-webrtc ready on http://${hostname}:${port} (${dev ? 'dev' : 'prod'})`);
  console.log(`> WHEP upstream:  ${process.env.WHEP_UPSTREAM || '(unset)'}`);
  console.log(`> input upstream: ${inputUpstream || '(unset)'}`);
  console.log(`> auth:           ${authEnabled ? `session cookie (user ${authUser})` : 'DISABLED'}`);
});
