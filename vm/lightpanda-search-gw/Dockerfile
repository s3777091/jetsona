# Node web-search gateway. Fetches DuckDuckGo Lite (static HTML) + cheerio parse.
# No browser/Chromium needed (DDG Lite is server-rendered; LightPanda dropped the
# results <table>, so a direct fetch is both simpler and reliable).
FROM node:20-slim

WORKDIR /app

RUN npm init -y && npm install cheerio@1

COPY server.js ./

ENV NODE_ENV=production \
    PORT=9233

EXPOSE 9233
CMD ["node", "server.js"]