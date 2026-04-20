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

/** Fit viewport to data — batch-level outlier filtering. */
export function fitViewToBounds(
  _batches: Batch[],
  batchBoundsList: (BatchBounds | null)[],
  canvasWidth: number,
  canvasHeight: number,
  _bounds?: Bounds,
): { centerX: number; centerY: number; zoom: number } {
  // Collect batch centers
  type Entry = { bb: BatchBounds; cx: number; cy: number };
  const entries: Entry[] = [];
  for (const bb of batchBoundsList) {
    if (!bb || bb.minX >= bb.maxX || bb.minY >= bb.maxY) continue;
    entries.push({ bb, cx: (bb.minX + bb.maxX) / 2, cy: (bb.minY + bb.maxY) / 2 });
  }
  if (entries.length === 0) return { centerX: 0, centerY: 0, zoom: 1 };

  // Median center of all batches
  const sortedCX = entries.map(e => e.cx).sort((a, b) => a - b);
  const sortedCY = entries.map(e => e.cy).sort((a, b) => a - b);
  const medCX = sortedCX[Math.floor(sortedCX.length / 2)];
  const medCY = sortedCY[Math.floor(sortedCY.length / 2)];

  // Distance of each batch from median, then median distance
  const dists = entries.map(e => Math.hypot(e.cx - medCX, e.cy - medCY));
  const sortedDists = [...dists].sort((a, b) => a - b);
  const medDist = sortedDists[Math.floor(sortedDists.length / 2)];

  // Tighter filter: 5x median distance
  const threshold = Math.max(medDist * 5, 1);

  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  let count = 0;
  for (let i = 0; i < entries.length; i++) {
    if (dists[i] > threshold) continue;
    const bb = entries[i].bb;
    if (bb.minX < minX) minX = bb.minX;
    if (bb.maxX > maxX) maxX = bb.maxX;
    if (bb.minY < minY) minY = bb.minY;
    if (bb.maxY > maxY) maxY = bb.maxY;
    count++;
  }

  if (count === 0 || minX >= maxX || minY >= maxY) {
    return { centerX: medCX, centerY: medCY, zoom: 1 };
  }

  const w = maxX - minX;
  const h = maxY - minY;
  const zoom = Math.min(
    (canvasWidth * 0.92) / w,
    (canvasHeight * 0.92) / h,
  );

  // Center = median of batch centers (robust to outlier batches)
  return {
    centerX: medCX,
    centerY: medCY,
    zoom,
  };
}
