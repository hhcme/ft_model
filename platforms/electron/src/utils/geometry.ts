import type { Batch, BatchBounds, Bounds } from '../app/types';

/** Compute axis-aligned bounding box for each batch. */
export function computeBatchBounds(batches: Batch[]): (BatchBounds | null)[] {
  return batches.map((batch) => {
    const verts = batch.vertices;
    if (!verts || verts.length === 0) return null;
    let minX = Infinity, minY = Infinity;
    let maxX = -Infinity, maxY = -Infinity;
    for (const v of verts) {
      const x = v[0], y = v[1];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
    return { minX, minY, maxX, maxY };
  });
}

/** Test if a bounding box overlaps the viewport. */
export function isVisible(
  bb: BatchBounds | null,
  vp: Bounds,
): boolean {
  if (!bb) return false;
  return !(
    bb.maxX < vp.minX || bb.minX > vp.maxX ||
    bb.maxY < vp.minY || bb.minY > vp.maxY
  );
}

/** Fit viewport to data using IQR-based outlier rejection. */
export function fitViewToBounds(
  batches: Batch[],
  batchBoundsList: (BatchBounds | null)[],
  canvasWidth: number,
  canvasHeight: number,
  bounds?: Bounds,
): { centerX: number; centerY: number; zoom: number } {
  if (bounds && bounds.minX < bounds.maxX && bounds.minY < bounds.maxY &&
      [bounds.minX, bounds.maxX, bounds.minY, bounds.maxY].every(Number.isFinite)) {
    const w = bounds.maxX - bounds.minX;
    const h = bounds.maxY - bounds.minY;
    return {
      centerX: (bounds.minX + bounds.maxX) / 2,
      centerY: (bounds.minY + bounds.maxY) / 2,
      zoom: Math.min((canvasWidth * 0.9) / w, (canvasHeight * 0.9) / h),
    };
  }

  const validEntries: { bb: BatchBounds; verts: number; idx: number }[] = [];
  for (let i = 0; i < batchBoundsList.length; i++) {
    const bb = batchBoundsList[i];
    const batch = batches[i];
    if (!bb || !batch?.vertices?.length) continue;
    if (![bb.minX, bb.maxX, bb.minY, bb.maxY].every(Number.isFinite)) continue;
    validEntries.push({ bb, verts: batch.vertices.length, idx: i });
  }
  if (validEntries.length === 0) return { centerX: 0, centerY: 0, zoom: 1 };

  let totalVerts = 0;
  for (const entry of validEntries) totalVerts += entry.verts;

  const sampleSize = Math.min(totalVerts, 10000);
  const sampleInterval = Math.max(1, Math.floor(totalVerts / sampleSize));

  const samplesX: number[] = [];
  const samplesY: number[] = [];
  let globalIdx = 0;
  for (const entry of validEntries) {
    const batch = batches[entry.idx];
    for (const v of batch.vertices) {
      if (globalIdx % sampleInterval === 0 &&
          Number.isFinite(v[0]) && Number.isFinite(v[1])) {
        samplesX.push(v[0]);
        samplesY.push(v[1]);
      }
      globalIdx++;
    }
  }

  if (samplesX.length === 0) return { centerX: 0, centerY: 0, zoom: 1 };

  samplesX.sort((a, b) => a - b);
  samplesY.sort((a, b) => a - b);
  const n = samplesX.length;

  const q1x = samplesX[Math.floor(n * 0.25)];
  const q3x = samplesX[Math.floor(n * 0.75)];
  const q1y = samplesY[Math.floor(n * 0.25)];
  const q3y = samplesY[Math.floor(n * 0.75)];
  const q05x = samplesX[Math.floor(n * 0.05)];
  const q95x = samplesX[Math.floor(n * 0.95)];
  const q05y = samplesY[Math.floor(n * 0.05)];
  const q95y = samplesY[Math.floor(n * 0.95)];
  const q01x = samplesX[Math.floor(n * 0.01)];
  const q99x = samplesX[Math.floor(n * 0.99)];
  const q01y = samplesY[Math.floor(n * 0.01)];
  const q99y = samplesY[Math.floor(n * 0.99)];
  const medianX = samplesX[Math.floor(n * 0.5)];
  const medianY = samplesY[Math.floor(n * 0.5)];

  const iqrX = q3x - q1x;
  const iqrY = q3y - q1y;
  const splitX = iqrX > 0 && ((q1x - q05x) > Math.max(iqrX * 2.5, 1) ||
    (q95x - q3x) > Math.max(iqrX * 2.5, 1));
  const splitY = iqrY > 0 && ((q1y - q05y) > Math.max(iqrY * 2.5, 1) ||
    (q95y - q3y) > Math.max(iqrY * 2.5, 1));

  let minX = q1x - iqrX * 0.1;
  let maxX = q3x + iqrX * 0.1;
  let minY = q1y - iqrY * 0.1;
  let maxY = q3y + iqrY * 0.1;
  if (!splitX && !splitY) {
    const broadW = q99x - q01x;
    const broadH = q99y - q01y;
    if (broadW > 0 && broadH > 0) {
      minX = q01x - broadW * 0.02;
      maxX = q99x + broadW * 0.02;
      minY = q01y - broadH * 0.02;
      maxY = q99y + broadH * 0.02;
    }
  }

  const w = (maxX - minX) || 1;
  const h = (maxY - minY) || 1;
  const zoom = Math.min(
    (canvasWidth * 0.9) / w,
    (canvasHeight * 0.9) / h,
  );

  return { centerX: medianX, centerY: medianY, zoom };
}
