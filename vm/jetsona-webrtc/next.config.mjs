/** @type {import('next').NextConfig} */
const nextConfig = {
  // App runs at the site root (no basePath). WHEP and the input WebSocket are
  // served by this same Next.js process: /media/jetsona/whep (route handler)
  // and /input (custom ws server in server.mjs).
  reactStrictMode: true,
  // The WHEP proxy streams SDP; never cache it.
  async headers() {
    return [
      {
        source: '/media/jetsona/whep',
        headers: [{ key: 'Cache-Control', 'value': 'no-store' }],
      },
    ];
  },
};

export default nextConfig;