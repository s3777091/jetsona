# Node web gateway: /search (Bing HTML + cheerio, no browser needed) and
# /fetch (page reader driving the existing `lightpanda` container over CDP via
# puppeteer-core, with a plain fetch+cheerio fallback for pages the beta
# engine can't handle).
FROM node:20-slim

WORKDIR /app

RUN npm init -y && npm install cheerio@1 puppeteer-core@23

COPY server.js ./

ENV NODE_ENV=production \
    PORT=9233

EXPOSE 9233
CMD ["node", "server.js"]