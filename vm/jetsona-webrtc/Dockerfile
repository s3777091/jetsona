# syntax=docker/dockerfile:1
# Jetsona WebRTC client + WHEP/input proxy (Next.js custom server).
#
# Multi-stage: build inside the image, ship only runtime artifacts.
# The container runs with network_mode: host on the VM (see docker-compose.yml)
# because the WHEP (mediamtx :8889) and input (sshd tunnel :46001) upstreams
# bind to the host's 127.0.0.1 and are not reachable from a bridge network.

FROM node:22-bookworm-slim AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
# postcss.config.mjs is what wires Tailwind v4 into the Next build; without it
# `next build` silently emits a stylesheet with no utilities in it.
COPY next.config.mjs tsconfig.json postcss.config.mjs server.mjs ./
COPY src ./src
RUN npm run build

FROM node:22-bookworm-slim AS run
WORKDIR /app
ENV NODE_ENV=production
COPY --from=build /app/package.json ./
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/.next ./.next
COPY --from=build /app/next.config.mjs ./
COPY --from=build /app/server.mjs ./
COPY --from=build /app/tsconfig.json ./
EXPOSE 9100
CMD ["node", "server.mjs"]
