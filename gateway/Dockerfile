# firex-server: gateway API, ingest, provisioning and the dashboard.
# Build from the repository root:  docker build -f gateway/Dockerfile -t firex-server .
# Runs on a Raspberry Pi 4/5 (linux/arm64) or any linux/amd64 host.

FROM node:22-bookworm-slim AS dashboard
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY dashboard ./dashboard
RUN npm run dashboard:build

FROM node:22-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production \
    FIREX_DATA_DIR=/data \
    FIREX_DASHBOARD_DIR=/app/dashboard/dist
COPY package.json package-lock.json ./
RUN npm ci --omit=dev && npm cache clean --force
# TypeScript runs directly with Node's type stripping (docs/adr/0005): the file
# in the image is the file in the repository.
COPY ledger/src ./ledger/src
COPY gateway/server ./gateway/server
COPY gateway/sim ./gateway/sim
COPY --from=dashboard /app/dashboard/dist ./dashboard/dist
RUN mkdir -p /data && chown node:node /data
VOLUME /data
USER node
EXPOSE 8080 8081
HEALTHCHECK --interval=10s --timeout=3s --start-period=20s --retries=6 \
  CMD node -e "fetch('http://127.0.0.1:8080/api/healthz').then(r => process.exit(r.ok ? 0 : 1), () => process.exit(1))"
CMD ["node", "--experimental-strip-types", "--disable-warning=ExperimentalWarning", "gateway/server/main.ts"]
