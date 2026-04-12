import type {
  CadWasmModule,
  DrawingInfo,
  LayerInfo,
  ViewportState,
  WasmCadEngine,
} from './types';
import { loadWasmModule } from './WasmLoader';

/**
 * High-level TypeScript facade wrapping the WASM CadEngine.
 *
 * Usage:
 * ```ts
 * const ctrl = new CadController();
 * await ctrl.initialize(canvas);
 * await ctrl.loadFile('/drawings/part.dxf');
 * ctrl.renderFrame();
 * ```
 */
export class CadController {
  private engine: WasmCadEngine | null = null;
  private module: CadWasmModule | null = null;

  // ------------------------------------------------------------------ //
  //  Lifecycle
  // ------------------------------------------------------------------ //

  /**
   * Load the WASM module, create a CadEngine, and initialise it against
   * the given HTML canvas element.
   */
  async initialize(
    canvas: HTMLCanvasElement,
    wasmPath?: string,
  ): Promise<void> {
    this.module = await loadWasmModule(wasmPath);
    this.engine = new this.module.CadEngine();

    const width = canvas.clientWidth || canvas.width;
    const height = canvas.clientHeight || canvas.height;

    const rc = this.engine.initialize(canvas, width, height);
    if (rc !== 0) {
      throw new Error(`CadEngine.initialize() failed with code ${rc}`);
    }
  }

  /** Whether the engine has been initialised and is ready to use. */
  get isReady(): boolean {
    return this.engine !== null;
  }

  // ------------------------------------------------------------------ //
  //  File loading
  // ------------------------------------------------------------------ //

  /** Load a DXF/DWG from a raw byte buffer. */
  loadBuffer(data: Uint8Array): void {
    this.requireEngine();
    this.engine!.loadBuffer(data);
  }

  /** Fetch a remote file and load it into the engine. */
  async loadFile(url: string): Promise<void> {
    this.requireEngine();
    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(`Failed to fetch "${url}": ${response.status} ${response.statusText}`);
    }
    const arrayBuffer = await response.arrayBuffer();
    this.engine!.loadBuffer(new Uint8Array(arrayBuffer));
  }

  /** Close the current file and free associated resources. */
  closeFile(): void {
    this.requireEngine();
    this.engine!.closeFile();
  }

  // ------------------------------------------------------------------ //
  //  Rendering
  // ------------------------------------------------------------------ //

  /** Render a single frame. */
  renderFrame(): void {
    this.requireEngine();
    this.engine!.renderFrame();
  }

  /** Notify the engine that the viewport size has changed. */
  resize(width: number, height: number): void {
    this.requireEngine();
    this.engine!.resize(width, height);
  }

  // ------------------------------------------------------------------ //
  //  View controls
  // ------------------------------------------------------------------ //

  /** Pan the camera by (dx, dy) in screen-pixel units. */
  pan(dx: number, dy: number): void {
    this.requireEngine();
    this.engine!.pan(dx, dy);
  }

  /**
   * Zoom by `factor` around screen point (pivotX, pivotY).
   * factor > 1 zooms in, factor < 1 zooms out.
   */
  zoom(factor: number, pivotX: number, pivotY: number): void {
    this.requireEngine();
    this.engine!.zoom(factor, pivotX, pivotY);
  }

  /** Fit the drawing to fill the viewport. */
  fitToExtents(): void {
    this.requireEngine();
    this.engine!.fitToExtents();
  }

  // ------------------------------------------------------------------ //
  //  Queries
  // ------------------------------------------------------------------ //

  getDrawingInfo(): DrawingInfo {
    this.requireEngine();
    return this.engine!.getDrawingInfo();
  }

  getViewportState(): ViewportState {
    this.requireEngine();
    return this.engine!.getViewportState();
  }

  getLayers(): LayerInfo[] {
    this.requireEngine();
    return this.engine!.getLayers();
  }

  setLayerVisibility(name: string, visible: boolean): void {
    this.requireEngine();
    this.engine!.setLayerVisibility(name, visible);
  }

  // ------------------------------------------------------------------ //
  //  Cleanup
  // ------------------------------------------------------------------ //

  /** Shutdown the engine and release all WASM-side resources. */
  destroy(): void {
    if (this.engine) {
      this.engine.shutdown();
      this.engine.delete();
      this.engine = null;
    }
    this.module = null;
  }

  // ------------------------------------------------------------------ //
  //  Internal helpers
  // ------------------------------------------------------------------ //

  private requireEngine(): asserts this is { engine: WasmCadEngine } {
    if (!this.engine) {
      throw new Error('CadController is not initialized. Call initialize() first.');
    }
  }
}
