/**
 * TypeScript type definitions matching the C++ CAD engine types
 * exposed via the Embind/WASM bridge.
 */

/** 3D vector — primary coordinate type (2D is just z=0). */
export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

/** 2D vector convenience type. */
export interface Vec2 {
  x: number;
  y: number;
}

/** 3D axis-aligned bounding box. */
export interface Bounds3d {
  min: Vec3;
  max: Vec3;
}

/** Current viewport state returned by the engine. */
export interface ViewportState {
  center: Vec3;
  zoom: number;
  width: number;
  height: number;
}

/** Information about a loaded drawing. */
export interface DrawingInfo {
  filename: string;
  extents: Bounds3d;
  layerCount: number;
  entityCount: number;
  acadVersion: string;
}

/** Layer metadata. */
export interface LayerInfo {
  name: string;
  isVisible: boolean;
  isFrozen: boolean;
  isLocked: boolean;
  entityCount: number;
}

/** RGBA color (8-bit per channel). */
export interface Color {
  r: number;
  g: number;
  b: number;
  a: number;
}

/**
 * WASM-side CadEngine instance shape.
 * This mirrors the Embind bindings in cad_wasm_bridge.cpp.
 */
export interface WasmCadEngine {
  initialize(canvas: unknown, width: number, height: number): number;
  loadBuffer(data: Uint8Array): void;
  loadFile(filepath: string): number;
  renderFrame(): void;
  pan(dx: number, dy: number): void;
  zoom(factor: number, pivotX: number, pivotY: number): void;
  fitToExtents(): void;
  resize(width: number, height: number): void;
  closeFile(): void;
  shutdown(): void;
  getDrawingInfo(): DrawingInfo;
  getViewportState(): ViewportState;
  getLayers(): LayerInfo[];
  setLayerVisibility(name: string, visible: boolean): void;
  delete(): void;
}

/** Shape of the WASM module created by Emscripten. */
export interface CadWasmModule {
  CadEngine: new () => WasmCadEngine;
  HEAPU8: Uint8Array;
}

/** Factory function exported by the generated WASM module loader. */
export type CadWasmFactory = (moduleOverrides?: Partial<CadWasmModule>) => Promise<CadWasmModule>;
