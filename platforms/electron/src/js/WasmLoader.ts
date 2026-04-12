import type { CadWasmModule } from './types';

let cachedModule: CadWasmModule | null = null;

/**
 * Dynamically imports and initializes the CAD engine WASM module.
 *
 * The WASM file is expected to be served from the same origin at a
 * path relative to the page (e.g. `/cad_engine.js` / `/cad_engine.wasm`).
 *
 * The module is cached after the first successful load so that repeated
 * calls return the same instance without re-fetching.
 */
export async function loadWasmModule(
  wasmPath = '/cad_engine.js',
): Promise<CadWasmModule> {
  if (cachedModule) {
    return cachedModule;
  }

  // Emscripten generates a JS wrapper that exposes a factory function.
  // The default export is a function returning a Promise<Module>.
  //
  // Vite treats dynamic-import targets as entry chunks, so we use
  // a raw URL import that resolves at runtime.
  const factoryImport: unknown = await import(/* @vite-ignore */ wasmPath);

  // Emscripten generated modules export a default function
  const factory = (factoryImport as { default?: () => Promise<CadWasmModule> }).default
    ?? (typeof factoryImport === 'function' ? factoryImport : null);

  if (!factory) {
    throw new Error(
      `WASM module at "${wasmPath}" did not export a factory function.`,
    );
  }

  const module = await (factory as () => Promise<CadWasmModule>)();

  cachedModule = module;
  return module;
}

/**
 * Force-clear the cached module (useful for hot-reload scenarios).
 */
export function resetWasmCache(): void {
  cachedModule = null;
}
