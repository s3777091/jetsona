# jetsona-webrtc (Next.js)

WebRTC remote client for the Jetsona firmware. Replaces the old single-file
`index.html`. This Next.js app owns **both** the page and the server-side
proxying that used to live in the VM's reverse proxy:

| Path                       | What                                       | Owner            |
| -------------------------- | ------------------------------------------ | ---------------- |
| `GET /`                    | WebRTC client page (WHEP video + input)    | Next.js page     |
| `POST /media/jetsona/whep` | WHEP SDP offer/answer proxy                | Next.js route    |
| `WS /input?token=…`        | Raw WebSocket input (mouse/keyboard) proxy | `server.mjs` (ws)|

The app runs at the site **root** (no `/jetsona` basePath).

## Environment

Copy `.env.example` to `.env` and fill in:

| Var                  | Required | Example                          | Notes                                              |
| -------------------- | -------- | -------------------------------- | -------------------------------------------------- |
| `WHEP_UPSTREAM`      | yes      | `http://127.0.0.1:8889/jetsona/whep` | MediaMTX WHEP endpoint (Jetson, via tunnel)    |
| `INPUT_UPSTREAM`     | yes      | `ws://127.0.0.1:46001`           | `jetsona_webrtc_input.py` (Jetson, via tunnel)     |
| `JETSONA_INPUT_TOKEN`| no       | `s3cret`                         | Token sent as `?token=` on `/input`. If unset, the page renders the literal `__JETSONA_INPUT_TOKEN__` placeholder (so an external proxy can still substitute it) and the WS upgrade skips token checks. |
| `BASIC_USER` / `BASIC_PASS` | no  | `jetsona` / `…`                 | Optional HTTP Basic auth over the whole site (page + WHEP + WS upgrade). |
| `PORT`               | no       | `3000`                           | Listen port.                                       |
| `HOST`               | no       | `0.0.0.0`                        | Listen host.                                       |

## Develop

```bash
npm install
npm run dev      # node server.mjs (dev mode)
```

## Production

```bash
npm install
npm run build
npm run start:sh # NODE_ENV=production node server.mjs
```

`npm run start` uses `cross-env` for Windows; `npm run start:sh` is the plain
POSIX form used by the systemd unit on the VM.

## Deploy (VM 36.50.27.142)

Runs in Docker, fronted by Caddy (TLS for `ai.protexa.cloud`).

1. Ship the source to `/opt/jetsona-webrtc` on the VM.
2. `/etc/jetsona-webrtc.env` holds `WHEP_UPSTREAM`, `INPUT_UPSTREAM`,
   `JETSONA_INPUT_TOKEN`, `BASIC_USER`, `BASIC_PASS`, `HOST=127.0.0.1`, `PORT=9100`.
3. `docker compose up -d --build` builds the image and starts the container with
   `network_mode: host` (required so the container can reach the host-loopback
   mediamtx `:8889` and ssh-tunnel `:46001` upstreams).
4. Caddy reverse-proxies `ai.protexa.cloud /` to `127.0.0.1:9100` (Caddy passes
   WebSocket upgrades through automatically).

Port `9100` is chosen to avoid the many ports already in use on the VM. The old
single-file `index.html` at `/var/www/jetsona` and the old `/jetsona/*` Caddy
block are removed — the app now owns the site root.