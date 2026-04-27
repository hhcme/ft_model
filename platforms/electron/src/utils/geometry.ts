import type { Batch, BatchBounds, Bounds, DrawData } from '../app/types';

// Entity modifier bitmask (mirrors C++ EntityModifier)
const MOD_EXCLUDE_BOUNDING = 0x0010;

/** Compute axis-aligned bounding box for each batch. */
export function computeBatchBounds(batches: Batch[]): (BatchBounds | null)[] {
  return batches.map((batch) => {
    if (batch.bounds && !batch.bounds.isEmpty &&
        batch.bounds.minX < batch.bounds.maxX && batch.bounds.minY < batch.bounds.maxY &&
        [batch.bounds.minX, batch.bounds.maxX, batch.bounds.minY, batch.bounds.maxY].every(Number.isFinite)) {
      return {
        minX: batch.bounds.minX,
        minY: batch.bounds.minY,
        maxX: batch.bounds.maxX,
        maxY: batch.bounds.maxY,
      };
    }
    const verts = batch.vertices;
    if (!verts || verts.length === 0) return null;
    let minX = Infinity, minY = Infinity;
    let maxX = -Infinity, maxY = -Infinity;
    for (let i = 0; i < verts.length; i += 2) {
      const x = verts[i], y = verts[i + 1];
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

/** Compute the percentile value from a sorted array. */
function pct(sorted: number[], p: number): number {
  return sorted[Math.min(Math.floor(sorted.length * p), sorted.length - 1)];
}

/**
 * Adaptive percentile-based outlier-resistant fitView.
 *
 * Strategy: use density-weighted boundary expansion from the IQR core.
 * 1. Start with the IQR core (Q25–Q75) as the central region.
 * 2. Expand outward through Q10–Q90 and Q05–Q95, checking density continuity.
 * 3. If a large gap appears (density drops sharply), stop expansion on that side.
 * 4. Center on the IQR core's geometric center (robust to asymmetric outliers).
 */
export function getPreferredViewBounds(drawData: DrawData): Bounds | undefined {
  const active = drawData.views?.find((v) => v.id === drawData.activeViewId);
  // For layout or viewport views with real presentation data, trust C++ bounds.
  // For "finiteGeometryFallback" (raw model-space aggregation), prefer the
  // front-end percentile fallback which handles asymmetric outliers better.
  const isReliableSource = !active?.source || active.source === 'layout' ||
    active.source === 'vport' || active.source === 'paperSpaceFallback';
  if (isReliableSource) {
    const candidate = active?.presentationBounds ?? active?.bounds ??
      drawData.presentationBounds ?? drawData.bounds;
    if (candidate && !candidate.isEmpty &&
        candidate.minX < candidate.maxX && candidate.minY < candidate.maxY &&
        [candidate.minX, candidate.maxX, candidate.minY, candidate.maxY].every(Number.isFinite)) {
      // Safety check: verify that at least some batch geometry actually falls
      // within these bounds. A layout view may have paper-space bounds that
      // don't contain any of the model-space vertex data.
      if (drawData.batches?.length) {
        const hasGeometryInView = drawData.batches.some((b) => {
          if (!b.vertices?.length) return false;
          const bb = b.bounds;
          if (!bb || bb.isEmpty) return false;
          return bb.maxX >= candidate.minX && bb.minX <= candidate.maxX &&
                 bb.maxY >= candidate.minY && bb.minY <= candidate.maxY;
        });
        if (!hasGeometryInView) return undefined;
      }
      return candidate;
    }
  }
  return undefined;
}

/**
 * Compute outlier-resistant bounds from batch vertices using RANSAC.
 *
 * Strategy: randomly sample 2 points → fit axis-aligned bounding box → count
 * inliers (points within adaptive threshold). Iterate, keep best consensus set.
 * Refit with all inliers from the best model.
 */
export function computeOutlierResistantBounds(
  batches: Batch[],
  batchBoundsList: (BatchBounds | null)[],
): Bounds | undefined {
  const validEntries: { bb: BatchBounds; verts: number; idx: number }[] = [];
  for (let i = 0; i < batchBoundsList.length; i++) {
    const bb = batchBoundsList[i];
    const batch = batches[i];
    if (!bb || !batch?.vertices?.length) continue;
    if (![bb.minX, bb.maxX, bb.minY, bb.maxY].every(Number.isFinite)) continue;
    // Skip helper entities (POINT/RAY/XLINE) from bounds calculation
    if ((batch.modifiers ?? 0) & MOD_EXCLUDE_BOUNDING) continue;
    validEntries.push({ bb, verts: batch.vertices.length / 2, idx: i });
  }
  if (validEntries.length === 0) return undefined;

  let totalVerts = 0;
  for (const entry of validEntries) totalVerts += entry.verts;

  // Collect sample points
  const sampleSize = Math.min(totalVerts, 10000);
  const sampleInterval = Math.max(1, Math.floor(totalVerts / sampleSize));

  const pts: [number, number][] = [];
  let globalIdx = 0;
  for (const entry of validEntries) {
    const batch = batches[entry.idx];
    const verts = batch.vertices;
    for (let fi = 0; fi < verts.length; fi += 2) {
      if (globalIdx % sampleInterval === 0 &&
          Number.isFinite(verts[fi]) && Number.isFinite(verts[fi + 1])) {
        pts.push([verts[fi], verts[fi + 1]]);
      }
      globalIdx++;
    }
  }

  if (pts.length === 0) return undefined;
  if (pts.length <= 4) {
    // Too few points for RANSAC, use simple bounds
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const [x, y] of pts) {
      if (x < minX) minX = x; if (x > maxX) maxX = x;
      if (y < minY) minY = y; if (y > maxY) maxY = y;
    }
    return { minX, minY, maxX, maxY, isEmpty: false };
  }

  // Compute adaptive inlier threshold from IQR
  const xs = pts.map(p => p[0]).sort((a, b) => a - b);
  const ys = pts.map(p => p[1]).sort((a, b) => a - b);
  const iqrX = pct(xs, 0.75) - pct(xs, 0.25);
  const iqrY = pct(ys, 0.75) - pct(ys, 0.25);
  const threshold = Math.max(iqrX + iqrY, Math.max(
    pct(xs, 0.95) - pct(xs, 0.05),
    pct(ys, 0.95) - pct(ys, 0.05),
  ) * 0.25, 10);

  const n = pts.length;
  const maxIterations = 100;
  const minInlierRatio = 0.6;

  let bestInliers: [number, number][] = [];
  let bestCount = 0;

  for (let iter = 0; iter < maxIterations; iter++) {
    // Sample 2 random distinct points
    const i1 = Math.floor(Math.random() * n);
    let i2 = Math.floor(Math.random() * n);
    if (i2 === i1) i2 = (i2 + 1) % n;
    const [x1, y1] = pts[i1];
    const [x2, y2] = pts[i2];

    // Fit AABB from 2 points
    const loX = Math.min(x1, x2);
    const hiX = Math.max(x1, x2);
    const loY = Math.min(y1, y2);
    const hiY = Math.max(y1, y2);

    // Count inliers: points within threshold of the bounding box
    const inliers: [number, number][] = [];
    for (const [px, py] of pts) {
      const dx = px < loX ? loX - px : px > hiX ? px - hiX : 0;
      const dy = py < loY ? loY - py : py > hiY ? py - hiY : 0;
      if (dx + dy <= threshold) inliers.push([px, py]);
    }

    if (inliers.length > bestCount) {
      bestCount = inliers.length;
      bestInliers = inliers;
      if (bestCount >= n * minInlierRatio) break; // good enough
    }
  }

  if (bestInliers.length === 0) return undefined;

  // Refit with all inliers from best model
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const [x, y] of bestInliers) {
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
  }

  // Expand by 60% on each side to include drawing border entities and
  // annotation margins that fall outside the dense content core.
  const rangeX = (maxX - minX) || 1;
  const rangeY = (maxY - minY) || 1;
  return {
    minX: minX - rangeX * 0.6,
    minY: minY - rangeY * 0.6,
    maxX: maxX + rangeX * 0.6,
    maxY: maxY + rangeY * 0.6,
    isEmpty: false,
  };
}

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

  const ob = computeOutlierResistantBounds(batches, batchBoundsList);
  if (!ob) return { centerX: 0, centerY: 0, zoom: 1 };

  const w = ob.maxX - ob.minX;
  const h = ob.maxY - ob.minY;
  const zoom = Math.min(
    (canvasWidth * 0.9) / w,
    (canvasHeight * 0.9) / h,
  );

  // Center on the same outlier-resistant bounds used for zoom. Centering on
  // the IQR core can push dense sheet-edge tables/title blocks off screen.
  return {
    centerX: (ob.minX + ob.maxX) / 2,
    centerY: (ob.minY + ob.maxY) / 2,
    zoom,
  };
}
