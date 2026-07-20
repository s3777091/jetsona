import WebRtcClient from '@/components/WebRtcClient';

// force-dynamic so the server-injected token is never cached.
export const dynamic = 'force-dynamic';

export default function Page() {
  // JETSONA_INPUT_TOKEN is read at request time on the server. When unset we
  // render the literal placeholder so an external reverse proxy can still
  // substitute it (preserving the original deploy model as a fallback).
  const inputToken = process.env.JETSONA_INPUT_TOKEN || '__JETSONA_INPUT_TOKEN__';
  return <WebRtcClient inputToken={inputToken} />;
}