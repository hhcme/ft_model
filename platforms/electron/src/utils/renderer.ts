import type { Batch, BatchBounds, Bounds, TextEntity, Viewport } from '../app/types';
import { worldToScreen, getViewportWorldBounds } from './transforms';
import { cleanMText } from './textUtils';

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

/** Render geometry batches. Returns render stats. */
export function renderBatches(
  ctx: Ctx,
  batches: Batch[],
  boundsList: (BatchBounds | null)[],
  vp: Viewport,
  layerVisible: Map<string, boolean>,
): { visible: number; drawn: number; culled: number; hidden: number } {
  const wb = getViewportWorldBounds(vp);
  const margin = 100 / vp.zoom;
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

    const name = batch.layerName;
    if (name && layerVisible.has(name) && !layerVisible.get(name)) {
      hidden++;
      continue;
    }

    visible++;
    drawn += batch.vertices.length;

    const [dr, dg, db] = boostColor(...batch.color);
    ctx.strokeStyle = `rgb(${dr},${dg},${db})`;
    ctx.lineWidth = Math.max(1, dpr);

    if (batch.topology === 'triangles') {
      renderTriangles(ctx, batch, vp, dr, dg, db);
    } else if (batch.topology === 'lines') {
      renderLines(ctx, batch, vp);
    } else {
      renderLinestrip(ctx, batch, vp, wb, margin);
    }
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

function renderLines(ctx: Ctx, batch: Batch, vp: Viewport): void {
  ctx.beginPath();
  for (let i = 0; i < batch.vertices.length; i += 2) {
    const v0 = batch.vertices[i];
    const v1 = batch.vertices[i + 1];
    const [sx0, sy0] = worldToScreen(v0[0], v0[1], vp);
    const [sx1, sy1] = worldToScreen(v1[0], v1[1], vp);
    ctx.moveTo(sx0, sy0);
    ctx.lineTo(sx1, sy1);
  }
  ctx.stroke();
}

function renderLinestrip(
  ctx: Ctx, batch: Batch, vp: Viewport,
  wb: Bounds, margin: number,
): void {
  const breaks = batch.breaks;
  if (breaks?.length) {
    ctx.beginPath();
    let hasPath = false;
    for (let ei = 0; ei < breaks.length; ei++) {
      const startIdx = breaks[ei];
      const endIdx = (ei + 1 < breaks.length) ? breaks[ei + 1] : batch.vertices.length;
      if (endIdx - startIdx < 2) continue;

      const vFirst = batch.vertices[startIdx];
      const vLast = batch.vertices[endIdx - 1];
      if (vFirst[0] < wb.minX - margin && vLast[0] < wb.minX - margin) continue;
      if (vFirst[0] > wb.maxX + margin && vLast[0] > wb.maxX + margin) continue;
      if (vFirst[1] < wb.minY - margin && vLast[1] < wb.minY - margin) continue;
      if (vFirst[1] > wb.maxY + margin && vLast[1] > wb.maxY + margin) continue;

      const [sx0, sy0] = worldToScreen(vFirst[0], vFirst[1], vp);
      ctx.moveTo(sx0, sy0);
      for (let i = startIdx + 1; i < endIdx; i++) {
        const v = batch.vertices[i];
        const [sx, sy] = worldToScreen(v[0], v[1], vp);
        ctx.lineTo(sx, sy);
      }
      hasPath = true;
    }
    if (hasPath) ctx.stroke();
  } else {
    const v0 = batch.vertices[0];
    const [sx0, sy0] = worldToScreen(v0[0], v0[1], vp);
    ctx.beginPath();
    ctx.moveTo(sx0, sy0);
    for (let i = 1; i < batch.vertices.length; i++) {
      const v = batch.vertices[i];
      const [sx, sy] = worldToScreen(v[0], v[1], vp);
      ctx.lineTo(sx, sy);
    }
    ctx.stroke();
  }
}

/** Render text entities. */
export function renderTexts(
  ctx: Ctx,
  texts: TextEntity[],
  vp: Viewport,
  layerVisible: Map<string, boolean>,
): void {
  for (const txt of texts) {
    if (txt.layerName && layerVisible.has(txt.layerName) && !layerVisible.get(txt.layerName)) continue;

    const [sx, sy] = worldToScreen(txt.x, txt.y, vp);
    if (sx < -500 || sx > vp.canvasWidth + 500 || sy < -500 || sy > vp.canvasHeight + 500) continue;

    const screenHeight = txt.height * vp.zoom;
    if (screenHeight < 3) continue;

    const clean = cleanMText(txt.text);
    if (!clean) continue;

    const [dr, dg, db] = boostColor(...txt.color);
    ctx.fillStyle = `rgb(${dr},${dg},${db})`;
    ctx.font = `${screenHeight}px -apple-system, 'PingFang SC', 'Microsoft YaHei', sans-serif`;
    ctx.textBaseline = 'bottom';

    ctx.save();
    ctx.translate(sx, sy);
    if (txt.rotation) ctx.rotate(-txt.rotation);
    ctx.scale(txt.widthFactor || 1, 1);
    ctx.fillText(clean, 0, 0);
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
