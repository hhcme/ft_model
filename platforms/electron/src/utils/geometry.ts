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
): { centerX: number; centerY: number; zoom: number } {
  const validEntries: { bb: BatchBounds; verts: number; idx: number }[] = [];
  for (let i = 0; i < batchBoundsList.length; i++) {
    const bb = batchBoundsList[i];
    if (!bb) continue;
    const batch = batches[i];
    if (!batch?.vertices?.length) continue;
    validEntries.push({ bb, verts: batch.vertices.length, idx: i });
  }
  if (validEntries.length === 0) {
    return { centerX: 0, centerY: 0, zoom: 1 };
  }

  let totalVerts = 0;
  for (const e of validEntries) totalVerts += e.verts;

  const sampleSize = Math.min(totalVerts, 10000);
  const sampleInterval = Math.max(1, Math.floor(totalVerts / sampleSize));

  const samplesX: number[] = [];
  const samplesY: number[] = [];

  let globalIdx = 0;
  for (const e of validEntries) {
    const batch = batches[e.idx];
    const verts = batch.vertices;
    for (let j = 0; j < verts.length; j++) {
      if (globalIdx % sampleInterval === 0) {
        samplesX.push(verts[j][0]);
        samplesY.push(verts[j][1]);
      }
      globalIdx++;
    }
  }

  if (samplesX.length === 0) return { centerX: 0, centerY: 0, zoom: 1 };

  const n = samplesX.length;
  const sortedX = [...samplesX].sort((a, b) => a - b);
  const sortedY = [...samplesY].sort((a, b) => a - b);

  const q1x = sortedX[Math.floor(n * 0.25)];
  const q3x = sortedX[Math.floor(n * 0.75)];
  const q1y = sortedY[Math.floor(n * 0.25)];
  const q3y = sortedY[Math.floor(n * 0.75)];
  const iqrX = q3x - q1x;
  const iqrY = q3y - q1y;
  const medianX = sortedX[Math.floor(n * 0.5)];
  const medianY = sortedY[Math.floor(n * 0.5)];

  const padX = iqrX * 0.1;
  const padY = iqrY * 0.1;
  const minX = q1x - padX;
  const maxX = q3x + padX;
  const minY = q1y - padY;
  const maxY = q3y + padY;

  const w = (maxX - minX) || 1;
  const h = (maxY - minY) || 1;
  const margin = 0.9;
  const zoom = Math.min(
    (canvasWidth * margin) / w,
    (canvasHeight * margin) / h,
  );

  return { centerX: medianX, centerY: medianY, zoom };
}
