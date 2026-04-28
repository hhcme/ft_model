/** Frame-level text measurement cache. Key = font + "|" + text. */
export class TextMeasureCache {
  private cache = new Map<string, number>();
  private lastZoom = -1;

  /** Clear cache when viewport zoom changes (font sizes depend on zoom). */
  beginFrame(zoom: number): void {
    if (Math.abs(zoom - this.lastZoom) > 1e-9) {
      this.cache.clear();
      this.lastZoom = zoom;
    }
  }

  /** Measure text width, using cached result when available. */
  measure(ctx: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D, font: string, text: string): number {
    const key = font + '|' + text;
    const cached = this.cache.get(key);
    if (cached !== undefined) return cached;
    ctx.font = font;
    const width = ctx.measureText(text).width;
    this.cache.set(key, width);
    return width;
  }

  /** Force full invalidation. */
  invalidate(): void {
    this.cache.clear();
    this.lastZoom = -1;
  }
}

/** Shared singleton — one per Canvas context lifecycle. */
export const textMeasureCache = new TextMeasureCache();
