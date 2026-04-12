import { defineConfig } from 'vite';
import { resolve } from 'path';

/**
 * Vite config for the CAD viewer demo.
 *
 * The WASM module is built separately by Emscripten and placed in
 * the public output directory. Vite serves it as a static asset.
 */
export default defineConfig({
  root: resolve(__dirname),
  publicDir: resolve(__dirname, 'public'),

  build: {
    outDir: resolve(__dirname, 'dist'),
    emptyOutDir: true,
  },

  server: {
    // Allow loading files from the project root during development
    fs: {
      allow: [resolve(__dirname, '..'), resolve(__dirname)],
    },
  },

  assetsInclude: ['**/*.wasm'],
});
