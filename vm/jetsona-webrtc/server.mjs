// Custom Next.js server.
//
// Why custom (not `next start`): the App Router route handlers are HTTP-only and
// cannot accept a WebSocket upgrade. The input bridge is a raw WebSocket, so we
// attach a `ws` WebSocketServer to the same HTTP server and proxy /input upgrades
// to the upstream jetsona_webrtc_input.py on the Jetson (INPUT_UPSTREAM).
//
// This same process therefore owns, at the site root:
//   GET  /                        -> the WebRTC client page (Next.js)
//   POST /media/jetsona/whep      -> WHEP proxy (Next.js route handler)
//   WS   /input?token=...         -> input WebSocket proxy (this file)
//
// Optional HTTP Basic auth (BASIC_USER / BASIC_PASS) is enforced here for both
// the HTTP request stream and the WS upgrade, so the whole site is protected by
// a single config.

import { createServer } from 'node:http';
import { parse } from 'node:url';
import next from 'next';
import { WebSocket, WebSocketServer } from 'ws';

const dev = process.env.NODE_ENV !== 'production';
const hostname = process.env.HOST || '0.0.0.0';
const port = parseInt(process.env.PORT || '3000', 10);

const inputUpstream = process.env.INPUT_UPSTREAM || '';
const inputToken = process.env.JETSONA_INPUT_TOKEN || '';
const basicUser = process.env.BASIC_USER || '';
const basicPass = process.env.BASIC_PASS || '';
const authEnabled = Boolean(basicUser && basicPass);

const app = next({ dev, hostname, port });
const handle = app.getRequestHandler();

function checkBasicAuth(header) {
  if (!header || !header.startsWith('Basic ')) return false;
  try {
    const [user, pass] = Buffer.from(header.slice(6), 'base64').toString('utf8').split(':');
    // Constant-time-ish comparison to avoid timing leaks on the password.
    const same = (a, b) => {
      if (a.length !== b.length) return false;
      let diff = 0;
      for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
      return diff === 0;
    };
    return same(user, basicUser) && same(pass, basicPass);
  } catch {
    return false;
  }
}

function unauthorized(res) {
  res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="jetsona"' });
  res.end('Unauthorized');
}

await app.prepare();
const server = createServer((req, res) => {
  if (authEnabled && !checkBasicAuth(req.headers.authorization)) {
    return unauthorized(res);
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

  if (authEnabled && !checkBasicAuth(req.headers.authorization)) {
    socket.write('HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm="jetsona"\r\n\r\n');
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
  } catch (err) {
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
  console.log(`> basic auth:     ${authEnabled ? 'on' : 'off'}`);
});