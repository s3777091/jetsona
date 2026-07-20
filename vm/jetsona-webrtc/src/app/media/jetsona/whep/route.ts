// WHEP (WebRTC-HTTP Egress Protocol) proxy.
//
// The browser POSTs an SDP offer here; we forward it to the upstream RTSP->WebRTC
// bridge (MediaMTX / mediamtx WHEP endpoint on the Jetson, reached via the tunnel)
// and pipe the SDP answer straight back. The original client waited for ICE
// gathering to complete before sending, so a single request/response is enough —
// no SSE trickle channel is needed.

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function POST(request: Request): Promise<Response> {
  const upstream = process.env.WHEP_UPSTREAM;
  if (!upstream) {
    return new Response('WHEP upstream not configured (WHEP_UPSTREAM)', { status: 502 });
  }

  const sdp = await request.text();
  try {
    const upstreamRes = await fetch(upstream, {
      method: 'POST',
      headers: { 'Content-Type': 'application/sdp' },
      body: sdp,
      // MediaMTX can take a moment on the first offer; don't let the edge time
      // it out prematurely.
      signal: AbortSignal.timeout(15000),
    });

    const body = await upstreamRes.text();
    return new Response(body, {
      status: upstreamRes.status,
      headers: {
        'Content-Type': upstreamRes.headers.get('Content-Type') || 'application/sdp',
        'Cache-Control': 'no-store',
      },
    });
  } catch (error) {
    return new Response(`WHEP upstream error: ${(error as Error).message}`, { status: 502 });
  }
}