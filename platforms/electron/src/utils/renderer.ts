import type { Batch, BatchBounds, Bounds, TextEntity, ViewDefinition, Viewport } from '../app/types';
import { worldToScreen, getViewportWorldBounds } from './transforms';
import { cleanMText, parseRichMText, type RichTextLine } from './textUtils';

type Ctx = CanvasRenderingContext2D;

/** Draw background grid lines. */
export function renderGrid(ctx: Ctx, vp: Viewport): void {
  const worldWidth = vp.canvasWidth / vp.zoom;
  const targetScreenPx = 80;
  const targetWorldSpacing = targetScreenPx / vp.zoom;

  const pow = Math.pow(10, Math.floor(Math.log10(Math.max(targetWorldSpacing, 1e-10))));
  let spacing: number;
  if (targetWorldSpacing < 2 * pow) spacing = 2 * pow;
  else if (targetWorldSpacing < 5 * pow) spacing = 5 * pow;
  else spacing = 10 * pow;

  const screenSpacing = spacing * vp.zoom;
  if (screenSpacing < 20 || screenSpacing > 300) return;

  const wb = getViewportWorldBounds(vp);
  const startX = Math.floor(wb.minX / spacing) * spacing;
  const startY = Math.floor(wb.minY / spacing) * spacing;
  const endX = Math.ceil(wb.maxX / spacing) * spacing;
  const endY = Math.ceil(wb.maxY / spacing) * spacing;

  if ((endX - startX) / spacing > 200 || (endY - startY) / spacing > 200) return;

  const alpha = Math.max(0.03, Math.min(0.12, 0.12 - Math.abs(screenSpacing - 80) / 2000));
  ctx.strokeStyle = `rgba(255,255,255,${alpha})`;
  ctx.lineWidth = 1;
  ctx.beginPath();

  for (let wx = startX; wx <= endX; wx += spacing) {
    const [sx] = worldToScreen(wx, 0, vp);
    ctx.moveTo(sx, 0);
    ctx.lineTo(sx, vp.canvasHeight);
  }
  for (let wy = startY; wy <= endY; wy += spacing) {
    const [, sy] = worldToScreen(0, wy, vp);
    ctx.moveTo(0, sy);
    ctx.lineTo(vp.canvasWidth, sy);
  }
  ctx.stroke();
}

function boostColor(r: number, g: number, b: number): [number, number, number] {
  if ((r + g + b) / 3 < 30) {
    return [Math.max(r, 60), Math.max(g, 60), Math.max(b, 60)];
  }
  return [r, g, b];
}

function adaptColor(r: number, g: number, b: number, paperMode = false): [number, number, number] {
  if (!paperMode) return boostColor(r, g, b);
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const brightness = (r + g + b) / 3;
  if (brightness > 210 && max - min < 70) return [24, 28, 34];
  if (brightness > 170 && max - min < 45) return [54, 60, 70];
  return [r, g, b];
}

function validBounds(bounds: Bounds | undefined): bounds is Bounds {
  return !!bounds && !bounds.isEmpty &&
    bounds.minX < bounds.maxX && bounds.minY < bounds.maxY &&
    [bounds.minX, bounds.maxX, bounds.minY, bounds.maxY].every(Number.isFinite);
}

function intersectsBounds(a: BatchBounds, b: Bounds): boolean {
  return !(a.maxX < b.minX || a.minX > b.maxX || a.maxY < b.minY || a.minY > b.maxY);
}

function expandBounds(bounds: Bounds, padRatio: number): Bounds {
  const w = bounds.maxX - bounds.minX;
  const h = bounds.maxY - bounds.minY;
  const pad = Math.max(w, h) * padRatio;
  return {
    minX: bounds.minX - pad,
    minY: bounds.minY - pad,
    maxX: bounds.maxX + pad,
    maxY: bounds.maxY + pad,
  };
}

function pointInBounds(x: number, y: number, bounds: Bounds): boolean {
  return x >= bounds.minX && x <= bounds.maxX && y >= bounds.minY && y <= bounds.maxY;
}

function segmentIntersectsBounds(
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  bounds: Bounds,
): boolean {
  const minX = Math.min(x0, x1);
  const maxX = Math.max(x0, x1);
  const minY = Math.min(y0, y1);
  const maxY = Math.max(y0, y1);
  return !(maxX < bounds.minX || minX > bounds.maxX || maxY < bounds.minY || minY > bounds.maxY);
}

function isArtifactSegment(
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  presentationBounds?: Bounds,
): boolean {
  if (![x0, y0, x1, y1].every(Number.isFinite)) return true;
  if (!validBounds(presentationBounds)) return false;

  const w = presentationBounds.maxX - presentationBounds.minX;
  const h = presentationBounds.maxY - presentationBounds.minY;
  const diag = Math.hypot(w, h);
  if (!Number.isFinite(diag) || diag <= 0) return false;

  const padded = expandBounds(presentationBounds, 0.12);
  const intersectsPresentation = segmentIntersectsBounds(x0, y0, x1, y1, padded);

  const len = Math.hypot(x1 - x0, y1 - y0);
  if (!Number.isFinite(len)) return true;
  const inside0 = pointInBounds(x0, y0, padded);
  const inside1 = pointInBounds(x1, y1, padded);
  if (inside0 && inside1) return false;
  if (!intersectsPresentation) {
    // Both endpoints outside the padded presentation bounds and segment
    // doesn't cross it. Only filter if the segment is significantly
    // longer than the drawing extent (likely a flying line).
    return len > diag * 0.05;
  }

  // One endpoint inside, one outside — typical flying-line pattern where
  // DWG decoding produces one valid coordinate and one garbage coordinate.
  // Filter if the segment is much longer than the drawing diagonal.
  if (len > diag * 1.0) return true;
  if (len > diag * 0.5) {
    const farPad = expandBounds(presentationBounds, 0.4);
    return !pointInBounds(x0, y0, farPad) || !pointInBounds(x1, y1, farPad);
  }
  return false;
}

function horizontalTextAlign(align?: number): CanvasTextAlign {
  // DXF/DWG horizontal justification:
  // 0=Left, 1=Center, 2=Right, 3=Aligned, 4=Middle, 5=Fit
  if (align === 2 || align === 3 || align === 5) return 'right';
  if (align === 1 || align === 4) return 'center';
  return 'left';
}

function mtextHorizontalAlign(align?: number): CanvasTextAlign {
  if (align === 2 || align === 5 || align === 8) return 'center';
  if (align === 3 || align === 6 || align === 9) return 'right';
  return 'left';
}

function verticalTextBaseline(align?: number): CanvasTextBaseline {
  // DXF/DWG vertical justification:
  // 0=Baseline, 1=Bottom, 2=Middle, 3=Top
  // align value in our export is h_just (horizontal)
  // For TEXT, align 4 = "Middle" (centered vertically and horizontally)
  if (align === 4) return 'middle';
  return 'bottom';
}

function mtextVerticalOffset(align: number | undefined, totalHeight: number): number {
  if (align === 4 || align === 5 || align === 6) return -totalHeight * 0.5;
  if (align === 7 || align === 8 || align === 9) return -totalHeight;
  return 0;
}

function canvasFont(
  runHeight: number,
  fontFamily?: string,
  bold?: boolean,
  italic?: boolean,
): string {
  const fallback = "-apple-system, 'PingFang SC', 'Microsoft YaHei', 'Noto Sans SC', sans-serif";
  const style = italic ? 'italic ' : '';
  const weight = bold ? '700 ' : '';
  if (!fontFamily) return `${style}${weight}${runHeight}px ${fallback}`;
  const safeFamily = fontFamily.replace(/['"\\;]/g, '').trim();
  if (!safeFamily) return `${style}${weight}${runHeight}px ${fallback}`;
  return `${style}${weight}${runHeight}px '${safeFamily}', ${fallback}`;
}

function wrapRichTextLines(
  ctx: Ctx,
  lines: RichTextLine[],
  maxWidth: number,
  baseHeight: number,
): RichTextLine[] {
  if (!Number.isFinite(maxWidth) || maxWidth <= baseHeight * 2) return lines;
  const wrapped: RichTextLine[] = [];

  const measure = (run: RichTextLine[number], text: string) => {
    const scale = Number.isFinite(run.heightScale) ? Math.max(0.12, Math.min(8, run.heightScale ?? 1)) : 1;
    const runHeight = Math.max(3, baseHeight * scale);
    ctx.font = canvasFont(runHeight, run.fontFamily, run.bold, run.italic);
    return { width: ctx.measureText(text).width, runHeight };
  };

  for (const line of lines) {
    let current = [] as RichTextLine;
    current.indent = line.indent;
    let currentWidth = 0;
    const indent = Number.isFinite(line.indent) ? Math.max(0, line.indent ?? 0) : 0;
    const availableWidth = Math.max(baseHeight * 2, maxWidth - indent);

    const pushCurrent = () => {
      if (current.length > 0) {
        wrapped.push(current);
        current = [] as RichTextLine;
        current.indent = line.indent;
        currentWidth = 0;
      }
    };

    for (const run of line) {
      const tokens = run.text.split(/(\s+)/).filter((token) => token.length > 0);
      for (const token of tokens) {
        const isSpace = /^\s+$/.test(token);
        if (isSpace && current.length === 0) continue;
        const { width, runHeight } = measure(run, token);
        const gap = current.length > 0 ? runHeight * 0.02 : 0;
        if (!isSpace && current.length > 0 && currentWidth + gap + width > availableWidth) {
          pushCurrent();
        }
        current.push({ ...run, text: token });
        currentWidth += (currentWidth > 0 ? gap : 0) + width;
      }
    }
    pushCurrent();
    if (line.length === 0) wrapped.push([] as RichTextLine);
  }

  return wrapped.length > 0 ? wrapped : lines;
}

/** Draw paper background/plot window for layout views. */
export function renderPaper(ctx: Ctx, view: ViewDefinition | undefined, vp: Viewport): void {
  if (!view || (view.type !== 'layout' && !view.paperMode)) return;
  const paper = validBounds(view.paperBounds) ? view.paperBounds :
    validBounds(view.presentationBounds) ? view.presentationBounds :
    validBounds(view.bounds) ? view.bounds : undefined;
  if (!paper) return;

  const [x0, y0] = worldToScreen(paper.minX, paper.maxY, vp);
  const [x1, y1] = worldToScreen(paper.maxX, paper.minY, vp);
  const left = Math.min(x0, x1);
  const top = Math.min(y0, y1);
  const width = Math.abs(x1 - x0);
  const height = Math.abs(y1 - y0);
  if (width < 1 || height < 1) return;

  ctx.save();
  const shadowOffset = Math.max(6, 10 * vp.dpr);
  ctx.fillStyle = 'rgba(0,0,0,0.32)';
  ctx.fillRect(left + shadowOffset, top + shadowOffset, width, height);
  ctx.fillStyle = '#f8f8f4';
  ctx.fillRect(left, top, width, height);
  ctx.strokeStyle = 'rgba(20,20,20,0.62)';
  ctx.lineWidth = Math.max(1, vp.dpr);
  ctx.strokeRect(left, top, width, height);

  const plot = validBounds(view.plotWindow) ? view.plotWindow : undefined;
  if (plot) {
    const [px0, py0] = worldToScreen(plot.minX, plot.maxY, vp);
    const [px1, py1] = worldToScreen(plot.maxX, plot.minY, vp);
    ctx.strokeStyle = 'rgba(20,20,20,0.72)';
    ctx.lineWidth = Math.max(1, vp.dpr);
    ctx.strokeRect(
      Math.min(px0, px1),
      Math.min(py0, py1),
      Math.abs(px1 - px0),
      Math.abs(py1 - py0),
    );
  }
  ctx.restore();
}

export function withWorldClip(
  ctx: Ctx,
  bounds: Bounds | undefined,
  vp: Viewport,
  draw: () => void,
): void {
  if (!validBounds(bounds)) {
    draw();
    return;
  }

  const [x0, y0] = worldToScreen(bounds.minX, bounds.maxY, vp);
  const [x1, y1] = worldToScreen(bounds.maxX, bounds.minY, vp);
  const left = Math.min(x0, x1);
  const top = Math.min(y0, y1);
  const width = Math.abs(x1 - x0);
  const height = Math.abs(y1 - y0);
  if (width < 1 || height < 1) return;

  ctx.save();
  ctx.beginPath();
  ctx.rect(left, top, width, height);
  ctx.clip();
  draw();
  ctx.restore();
}

/** Render geometry batches. Returns render stats. */
export function renderBatches(
  ctx: Ctx,
  batches: Batch[],
  boundsList: (BatchBounds | null)[],
  vp: Viewport,
  layerVisible: Map<string, boolean>,
  clipBounds?: Bounds,
  presentationBounds?: Bounds,
  paperMode = false,
): { visible: number; drawn: number; culled: number; hidden: number } {
  const wb = getViewportWorldBounds(vp);
  const margin = 200 / vp.zoom;
  const dpr = vp.dpr;
  let visible = 0, drawn = 0, culled = 0, hidden = 0;

  for (let bi = 0; bi < batches.length; bi++) {
    const batch = batches[bi];
    if (!batch.vertices?.length) continue;

    const bb = boundsList[bi];
    if (bb && (bb.maxX < wb.minX || bb.minX > wb.maxX ||
               bb.maxY < wb.minY || bb.minY > wb.maxY)) {
      culled++;
      continue;
    }
    if (bb && validBounds(clipBounds) && !intersectsBounds(bb, clipBounds)) {
      culled++;
      continue;
    }

    const name = batch.layerName;
    if (name && layerVisible.has(name) && !layerVisible.get(name)) {
      hidden++;
      continue;
    }

    visible++;
    drawn += batch.vertices.length;

    const [dr, dg, db] = adaptColor(...batch.color, paperMode);
    ctx.strokeStyle = `rgb(${dr},${dg},${db})`;
    const worldLineWidth = Number.isFinite(batch.lineWidth) ? (batch.lineWidth ?? 1) : 1;
    if (paperMode) {
      const paperCssWidth = worldLineWidth <= 0.12 ? 0.8 : worldLineWidth <= 0.2 ? 1.0 : 1.25;
      ctx.lineWidth = Math.max(1, Math.min(8 * dpr, paperCssWidth * dpr));
    } else {
      ctx.lineWidth = Math.max(1, Math.min(8, worldLineWidth * dpr));
    }
    const dash = batch.linePattern
      ?.filter((v) => Number.isFinite(v) && Math.abs(v) > 1e-6)
      .map((v) => Math.abs(v)) ?? [];
    ctx.setLineDash(dash.length >= 2 ? dash.map((v) => Math.max(1, v * vp.zoom)) : []);

    if (batch.topology === 'triangles') {
      renderTriangles(ctx, batch, vp, dr, dg, db);
    } else if (batch.topology === 'lines') {
      renderLines(ctx, batch, vp, presentationBounds);
    } else {
      renderLinestrip(ctx, batch, vp, wb, margin, presentationBounds);
    }
    ctx.setLineDash([]);
  }
  return { visible, drawn, culled, hidden };
}

function renderTriangles(
  ctx: Ctx, batch: Batch, vp: Viewport,
  r: number, g: number, b: number,
): void {
  ctx.fillStyle = `rgb(${r},${g},${b})`;
  ctx.beginPath();
  for (let i = 0; i < batch.vertices.length; i += 3) {
    const v0 = batch.vertices[i];
    const v1 = batch.vertices[i + 1];
    const v2 = batch.vertices[i + 2];
    const [sx0, sy0] = worldToScreen(v0[0], v0[1], vp);
    const [sx1, sy1] = worldToScreen(v1[0], v1[1], vp);
    const [sx2, sy2] = worldToScreen(v2[0], v2[1], vp);
    ctx.moveTo(sx0, sy0);
    ctx.lineTo(sx1, sy1);
    ctx.lineTo(sx2, sy2);
    ctx.closePath();
  }
  ctx.fill();
}

function renderLines(ctx: Ctx, batch: Batch, vp: Viewport, presentationBounds?: Bounds): void {
  ctx.beginPath();
  let hasPath = false;
  for (let i = 0; i < batch.vertices.length; i += 2) {
    const v0 = batch.vertices[i];
    const v1 = batch.vertices[i + 1];
    if (!v0 || !v1) continue;
    if (isArtifactSegment(v0[0], v0[1], v1[0], v1[1], presentationBounds)) continue;
    const [sx0, sy0] = worldToScreen(v0[0], v0[1], vp);
    const [sx1, sy1] = worldToScreen(v1[0], v1[1], vp);
    ctx.moveTo(sx0, sy0);
    ctx.lineTo(sx1, sy1);
    hasPath = true;
  }
  if (hasPath) ctx.stroke();
}

function renderLinestrip(
  ctx: Ctx, batch: Batch, vp: Viewport,
  wb: Bounds, margin: number, presentationBounds?: Bounds,
): void {
  const breaks = batch.breaks;
  if (breaks?.length) {
    ctx.beginPath();
    let hasPath = false;
    for (let ei = 0; ei < breaks.length; ei++) {
      const startIdx = breaks[ei];
      const endIdx = (ei + 1 < breaks.length) ? breaks[ei + 1] : batch.vertices.length;
      if (endIdx - startIdx < 2) continue;

      // Compute entity AABB for culling (not just first/last vertex)
      let eMinX = Infinity, eMinY = Infinity, eMaxX = -Infinity, eMaxY = -Infinity;
      for (let i = startIdx; i < endIdx; i++) {
        const x = batch.vertices[i][0], y = batch.vertices[i][1];
        if (x < eMinX) eMinX = x; if (x > eMaxX) eMaxX = x;
        if (y < eMinY) eMinY = y; if (y > eMaxY) eMaxY = y;
      }
      if (eMaxX < wb.minX - margin || eMinX > wb.maxX + margin) continue;
      if (eMaxY < wb.minY - margin || eMinY > wb.maxY + margin) continue;

      let penDown = false;
      for (let i = startIdx + 1; i < endIdx; i++) {
        const prev = batch.vertices[i - 1];
        const v = batch.vertices[i];
        if (isArtifactSegment(prev[0], prev[1], v[0], v[1], presentationBounds)) {
          penDown = false;
          continue;
        }
        if (!penDown) {
          const [sx0, sy0] = worldToScreen(prev[0], prev[1], vp);
          ctx.moveTo(sx0, sy0);
          penDown = true;
        }
        const [sx, sy] = worldToScreen(v[0], v[1], vp);
        ctx.lineTo(sx, sy);
        hasPath = true;
      }
    }
    if (hasPath) ctx.stroke();
  } else {
    ctx.beginPath();
    let hasPath = false;
    let penDown = false;
    for (let i = 1; i < batch.vertices.length; i++) {
      const prev = batch.vertices[i - 1];
      const v = batch.vertices[i];
      if (isArtifactSegment(prev[0], prev[1], v[0], v[1], presentationBounds)) {
        penDown = false;
        continue;
      }
      if (!penDown) {
        const [sx0, sy0] = worldToScreen(prev[0], prev[1], vp);
        ctx.moveTo(sx0, sy0);
        penDown = true;
      }
      const [sx, sy] = worldToScreen(v[0], v[1], vp);
      ctx.lineTo(sx, sy);
      hasPath = true;
    }
    if (hasPath) ctx.stroke();
  }
}

/** Render text entities. */
export function renderTexts(
  ctx: Ctx,
  texts: TextEntity[],
  vp: Viewport,
  layerVisible: Map<string, boolean>,
  clipBounds?: Bounds,
  paperMode = false,
): void {
  for (const txt of texts) {
    if (txt.layerName && layerVisible.has(txt.layerName) && !layerVisible.get(txt.layerName)) continue;
    if (validBounds(clipBounds) &&
        (txt.x < clipBounds.minX || txt.x > clipBounds.maxX ||
         txt.y < clipBounds.minY || txt.y > clipBounds.maxY)) continue;

    const [sx, sy] = worldToScreen(txt.x, txt.y, vp);
    if (sx < -500 || sx > vp.canvasWidth + 500 || sy < -500 || sy > vp.canvasHeight + 500) continue;

    const screenHeight = txt.height * vp.zoom;
    if (screenHeight < 3) continue;

    ctx.save();
    ctx.translate(sx, sy);
    if (txt.rotation) ctx.rotate(-txt.rotation);
    ctx.scale(txt.widthFactor || 1, 1);
    const isMText = txt.kind === 'mtext';
    ctx.textAlign = 'left';
    ctx.textBaseline = isMText ? 'top' : verticalTextBaseline(txt.align);
    let richLines = parseRichMText(txt.text);
    if (richLines.length === 0) {
      const clean = cleanMText(txt.text);
      richLines = clean
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean)
        .map((line) => [{ text: line }] as RichTextLine);
    }
    if (richLines.length === 0) {
      ctx.restore();
      continue;
    }
    if (isMText && Number.isFinite(txt.rectWidth) && (txt.rectWidth ?? 0) > 0) {
      // Apply widthFactor to the wrapping width so text wraps at the correct
      // visual boundary. ctx.scale(widthFactor) later stretches glyphs, but
      // measureText works in pre-scale space, so divide by widthFactor.
      const wf = txt.widthFactor && txt.widthFactor > 0 ? txt.widthFactor : 1;
      richLines = wrapRichTextLines(ctx, richLines, (txt.rectWidth ?? 0) * vp.zoom / wf, screenHeight);
    }

    const measuredLines = richLines.map((line) => {
      const lineMaxScale = Math.max(
        1,
        ...line.map((run) => Number.isFinite(run.heightScale) ? (run.heightScale ?? 1) : 1),
      );
      const measuredRuns = line
        .map((run) => {
          const runText = run.text.trim();
          if (!runText) return null;
          const runScale = Number.isFinite(run.heightScale) ? (run.heightScale ?? 1) : 1;
          const runHeight = Math.max(3, screenHeight * Math.max(0.12, Math.min(8, runScale)));
          ctx.font = canvasFont(runHeight, run.fontFamily, run.bold, run.italic);
          return { run, runText, runHeight, width: ctx.measureText(runText).width };
        })
        .filter((run): run is {
          run: NonNullable<RichTextLine[number]>;
          runText: string;
          runHeight: number;
          width: number;
        } => !!run);
      const lineWidth = measuredRuns.reduce((sum, run, index) =>
        sum + run.width + (index + 1 < measuredRuns.length ? run.runHeight * 0.08 : 0), 0);
      // AutoCAD default line spacing factor is 1.167 (at least);
      // if rectHeight is available, derive per-line spacing from it.
      const lineSpacing = (isMText && Number.isFinite(txt.rectHeight) && (txt.rectHeight ?? 0) > 0 && measuredLines)
        ? 1.167
        : 1.15;
      const lineHeight = screenHeight * lineMaxScale * lineSpacing;
      return { measuredRuns, lineWidth, lineHeight, indent: line.indent };
    }).filter((line) => line.measuredRuns.length > 0);

    const totalHeight = measuredLines.reduce((sum, line) => sum + line.lineHeight, 0);
    let baselineY = isMText ? mtextVerticalOffset(txt.align, totalHeight) : 0;
    const align = isMText ? mtextHorizontalAlign(txt.align) : horizontalTextAlign(txt.align);
    for (const line of measuredLines) {
      const y = baselineY;
      const indent = isMText && Number.isFinite(line.indent)
        ? (line.indent ?? 0) * vp.zoom
        : 0;
      let x = align === 'center' ? -line.lineWidth * 0.5 : align === 'right' ? -line.lineWidth : 0;
      x += indent;
      for (let ri = 0; ri < line.measuredRuns.length; ri++) {
        const { run, runText, runHeight, width: w } = line.measuredRuns[ri];
        ctx.font = canvasFont(runHeight, run.fontFamily, run.bold, run.italic);
        const [dr, dg, db] = adaptColor(...(run.color ?? txt.color), paperMode);
        ctx.fillStyle = `rgb(${dr},${dg},${db})`;
        ctx.fillText(runText, x, y);
        if (run.underline) {
          ctx.save();
          ctx.strokeStyle = `rgb(${dr},${dg},${db})`;
          ctx.lineWidth = Math.max(1, runHeight * 0.035);
          const underlineY = y + (isMText ? runHeight * 0.92 : runHeight * 0.08);
          ctx.beginPath();
          ctx.moveTo(x, underlineY);
          ctx.lineTo(x + w, underlineY);
          ctx.stroke();
          ctx.restore();
        }
        x += w + (ri + 1 < line.measuredRuns.length ? runHeight * 0.08 : 0);
      }
      baselineY += line.lineHeight;
    }
    ctx.restore();
  }
}

/** Render measurement overlays. */
export function renderMeasurements(
  ctx: Ctx,
  measurements: { type: 'dist' | 'area'; points: [number, number][]; value: number }[],
  currentPoints: [number, number][],
  preview: [number, number] | null,
  vp: Viewport,
  mode: 'dist' | 'area' | null,
): void {
  const allMeasurements = measurements;
  if (allMeasurements.length === 0 && currentPoints.length === 0 && !preview) return;

  // Completed measurements
  for (const m of allMeasurements) {
    if (m.type === 'dist') {
      const [sx0, sy0] = worldToScreen(m.points[0][0], m.points[0][1], vp);
      const [sx1, sy1] = worldToScreen(m.points[1][0], m.points[1][1], vp);
      ctx.strokeStyle = '#00ff88';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(sx0, sy0);
      ctx.lineTo(sx1, sy1);
      ctx.stroke();
      const mx = (sx0 + sx1) / 2;
      const my = (sy0 + sy1) / 2;
      ctx.fillStyle = '#00ff88';
      ctx.font = '12px sans-serif';
      ctx.textBaseline = 'bottom';
      ctx.fillText(m.value.toFixed(2), mx, my - 4);
    } else if (m.type === 'area' && m.points.length >= 3) {
      ctx.fillStyle = 'rgba(0,255,136,0.1)';
      ctx.strokeStyle = '#00ff88';
      ctx.lineWidth = 2;
      ctx.beginPath();
      const [sx0, sy0] = worldToScreen(m.points[0][0], m.points[0][1], vp);
      ctx.moveTo(sx0, sy0);
      for (let i = 1; i < m.points.length; i++) {
        const [sx, sy] = worldToScreen(m.points[i][0], m.points[i][1], vp);
        ctx.lineTo(sx, sy);
      }
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
      const cx = m.points.reduce((s, p) => s + p[0], 0) / m.points.length;
      const cy = m.points.reduce((s, p) => s + p[1], 0) / m.points.length;
      const [scx, scy] = worldToScreen(cx, cy, vp);
      ctx.fillStyle = '#00ff88';
      ctx.font = '12px sans-serif';
      ctx.textBaseline = 'middle';
      ctx.fillText(m.value.toFixed(2), scx, scy);
    }
  }

  // In-progress measurement
  if (mode && currentPoints.length > 0) {
    ctx.strokeStyle = 'rgba(0,255,136,0.6)';
    ctx.setLineDash([4, 4]);
    ctx.lineWidth = 2;
    ctx.beginPath();
    const [sx0, sy0] = worldToScreen(currentPoints[0][0], currentPoints[0][1], vp);
    ctx.moveTo(sx0, sy0);
    for (let i = 1; i < currentPoints.length; i++) {
      const [sx, sy] = worldToScreen(currentPoints[i][0], currentPoints[i][1], vp);
      ctx.lineTo(sx, sy);
    }
    if (preview) {
      const [sx, sy] = worldToScreen(preview[0], preview[1], vp);
      ctx.lineTo(sx, sy);
    }
    ctx.stroke();
    ctx.setLineDash([]);
  }

  // Crosshairs
  for (const p of currentPoints) {
    const [sx, sy] = worldToScreen(p[0], p[1], vp);
    ctx.strokeStyle = '#00ff88';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(sx - 6, sy); ctx.lineTo(sx + 6, sy);
    ctx.moveTo(sx, sy - 6); ctx.lineTo(sx, sy + 6);
    ctx.stroke();
  }
}

/** Draw a subtle border rectangle from DrawData.bounds. */
export function renderBorder(ctx: Ctx, bounds: Bounds | undefined, vp: Viewport): void {
  if (!bounds || bounds.minX >= bounds.maxX || bounds.minY >= bounds.maxY) return;

  const [sx0, sy0] = worldToScreen(bounds.minX, bounds.maxY, vp);
  const [sx1, sy1] = worldToScreen(bounds.maxX, bounds.minY, vp);

  // Light fill — subtle "paper" background
  ctx.fillStyle = 'rgba(255,255,255,0.015)';
  ctx.fillRect(sx0, sy0, sx1 - sx0, sy1 - sy0);

  // Border line
  ctx.strokeStyle = 'rgba(255,255,255,0.15)';
  ctx.lineWidth = 1;
  ctx.setLineDash([8, 4]);
  ctx.strokeRect(sx0, sy0, sx1 - sx0, sy1 - sy0);
  ctx.setLineDash([]);
}
