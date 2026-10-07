FROM node:24-alpine

WORKDIR /app
COPY --chown=node:node package.json ./
COPY --chown=node:node src ./src
COPY --chown=node:node frontend ./frontend

ENV NODE_ENV=production HOST=0.0.0.0 PORT=8790
USER node
EXPOSE 8790
HEALTHCHECK --interval=15s --timeout=4s --start-period=5s --retries=4 CMD wget -qO- http://127.0.0.1:8790/api/health >/dev/null || exit 1
CMD ["node", "src/server.js"]
