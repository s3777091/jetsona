// Wake-on-LAN trigger proxy.
//
// The browser POSTs here; we forward to the Jetson's WoL endpoint
// (WOL_UPSTREAM, reached via the reverse SSH tunnel on the VM's loopback)
// with the shared token in X-WoL-Token. The Jetson is the only device on the
// same LAN as the target PC, so it is the one that actually emits the magic
// packet.
//
// Auth: this route inherits the site-wide HTTP Basic auth enforced in
// server.mjs (the page already authenticated the browser), plus the token
// between the VM and the Jetson.

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function POST(): Promise<Response> {
  const upstream = process.env.WOL_UPSTREAM;
  const token = process.env.WOL_TOKEN || '';
  if (!upstream) {
    return json(502, { ok: false, error: 'WOL_UPSTREAM not configured' });
  }
  if (!token) {
    return json(502, { ok: false, error: 'WOL_TOKEN not configured' });
  }
  try {
    const res = await fetch(`${upstream.replace(/\/$/, '')}/wake`, {
      method: 'POST',
      headers: { 'X-WoL-Token': token },
      signal: AbortSignal.timeout(5000),
    });
    const body = await res.text();
    return new Response(body, {
      status: res.status,
      headers: { 'Content-Type': res.headers.get('Content-Type') || 'application/json', 'Cache-Control': 'no-store' },
    });
  } catch (error) {
    return json(502, { ok: false, error: `WoL upstream error: ${(error as Error).message}` });
  }
}

function json(status: number, obj: unknown): Response {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  });
}