import { CadController } from './CadController';
import { attachGestures } from './GestureAdapter';

/**
 * Custom element `<cad-canvas>` — a self-contained CAD viewer widget.
 *
 * Features:
 *   - Creates an HTMLCanvasElement inside shadow DOM
 *   - Initialises the WASM CadEngine on first connection
 *   - Runs a requestAnimationFrame render loop
 *   - Handles viewport resize via ResizeObserver
 *   - Attaches mouse/touch gesture handling
 *
 * Public API:
 *   - `controller` — the underlying CadController
 *   - `loadFile(url)` — fetch and display a DXF/DWG
 *   - `loadBuffer(data)` — display from a raw byte array
 *   - `ready` — Promise that resolves once the engine is initialised
 */
export class CadCanvas extends HTMLElement {
  /** The underlying CadController driving the WASM engine. */
  readonly controller = new CadController();

  private canvas: HTMLCanvasElement | null = null;
  private detachGestures: (() => void) | null = null;
  private rafId = 0;
  private resizeObserver: ResizeObserver | null = null;
  private readyResolve: (() => void) | null = null;
  private _ready = new Promise<void>((resolve) => {
    this.readyResolve = resolve;
  });

  /** Promise that resolves once the WASM engine is fully initialised. */
  get ready(): Promise<void> {
    return this._ready;
  }

  // ---- Custom element lifecycle ----

  constructor() {
    super();
    // Attach shadow DOM so consumer styles don't leak in
    const shadow = this.attachShadow({ mode: 'open' });

    const style = document.createElement('style');
    style.textContent = `
      :host { display: block; width: 100%; height: 100%; }
      canvas { display: block; width: 100%; height: 100%; }
    `;
    shadow.appendChild(style);

    const canvas = document.createElement('canvas');
    canvas.style.touchAction = 'none'; // prevent browser gesture interception
    shadow.appendChild(canvas);
    this.canvas = canvas;
  }

  async connectedCallback(): Promise<void> {
    if (!this.canvas) return;

    // Set initial pixel size from layout
    const rect = this.getBoundingClientRect();
    this.canvas.width = rect.width * devicePixelRatio;
    this.canvas.height = rect.height * devicePixelRatio;

    try {
      await this.controller.initialize(this.canvas);
    } catch (err) {
      console.error('[cad-canvas] Failed to initialise WASM engine:', err);
      return;
    }

    // Gesture handling
    this.detachGestures = attachGestures(this.canvas, this.controller);

    // Resize observer
    this.resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const { width, height } = entry.contentRect;
        if (width > 0 && height > 0) {
          const pw = Math.round(width * devicePixelRatio);
          const ph = Math.round(height * devicePixelRatio);
          if (this.canvas) {
            this.canvas.width = pw;
            this.canvas.height = ph;
          }
          this.controller.resize(pw, ph);
        }
      }
    });
    this.resizeObserver.observe(this);

    // Start render loop
    this.startRenderLoop();

    if (this.readyResolve) {
      this.readyResolve();
    }
  }

  disconnectedCallback(): void {
    // Stop render loop
    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
      this.rafId = 0;
    }

    // Detach gesture handlers
    if (this.detachGestures) {
      this.detachGestures();
      this.detachGestures = null;
    }

    // Disconnect resize observer
    if (this.resizeObserver) {
      this.resizeObserver.disconnect();
      this.resizeObserver = null;
    }

    // Destroy WASM engine
    this.controller.destroy();

    // Reset ready promise so a re-insertion re-initialises
    this._ready = new Promise<void>((resolve) => {
      this.readyResolve = resolve;
    });
  }

  // ---- Public API ----

  /** Fetch a file by URL and load it into the viewer. */
  async loadFile(url: string): Promise<void> {
    await this.ready;
    await this.controller.loadFile(url);
  }

  /** Load a DXF/DWG from a raw byte buffer. */
  async loadBuffer(data: Uint8Array): Promise<void> {
    await this.ready;
    this.controller.loadBuffer(data);
  }

  // ---- Internal ----

  private startRenderLoop(): void {
    const loop = () => {
      if (this.controller.isReady) {
        this.controller.renderFrame();
      }
      this.rafId = requestAnimationFrame(loop);
    };
    this.rafId = requestAnimationFrame(loop);
  }
}

// Register the custom element
customElements.define('cad-canvas', CadCanvas);

// Re-export via separate declare so TS consumers can import the class
export default CadCanvas;
