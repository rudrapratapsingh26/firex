import { fileURLToPath } from 'node:url';

import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

/* The build is served by firex-server from dashboard/dist, on the site LAN,
 * with no CDN: fonts are bundled from @fontsource. In development the API and
 * the live socket are proxied to a gateway on localhost:8080. */
export default defineConfig({
  root: fileURLToPath(new URL('.', import.meta.url)),
  plugins: [react()],
  build: { outDir: 'dist', emptyOutDir: true },
  server: {
    port: 5173,
    proxy: { '/api': { target: 'http://localhost:8080', ws: true } },
  },
});
