import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { resolve } from 'path';

export default defineConfig({
  plugins: [react()],
  root: resolve(__dirname),
  publicDir: resolve(__dirname, 'public'),

  build: {
    outDir: resolve(__dirname, 'dist'),
    emptyOutDir: true,
  },

  server: {
    proxy: {
      '/parse': 'http://localhost:2415',
    },
    fs: {
      allow: [resolve(__dirname, '..'), resolve(__dirname)],
    },
  },

  assetsInclude: ['**/*.wasm'],
});
