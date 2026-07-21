// Wake-on-LAN status proxy.
//
// Forwards GET to the Jetson's WoL endpoint, which pings the target PC on the
// LAN and reports online/offline. Used by the WoL panel to show whether the PC
// is up and to detect when a wake attempt has succeeded.

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function GET(): Promise<Response> {
  const upstream = process.env.WOL_UPSTREAM;
  const token = process.env.WOL_TOKEN || '';
  if (!upstream) {
    return json(502, { ok: false, error: 'WOL_UPSTREAM not configured' });
  }
  if (!token) {
    return json(502, { ok: false, error: 'WOL_TOKEN not configured' });
  }
  try {
    const res = await fetch(`${upstream.replace(/\/$/, '')}/status`, {
      headers: { 'X-WoL-Token': token },
      signal: AbortSignal.timeout(4000),
      cache: 'no-store',
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