import type { Batch, BatchBounds, Bounds, DrawData } from '../app/types';

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
      return candidate;
    }
  }
  return undefined;
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

  // Core percentiles
  const q25x = pct(samplesX, 0.25), q75x = pct(samplesX, 0.75);
  const q25y = pct(samplesY, 0.25), q75y = pct(samplesY, 0.75);
  const q10x = pct(samplesX, 0.10), q90x = pct(samplesX, 0.90);
  const q10y = pct(samplesY, 0.10), q90y = pct(samplesY, 0.90);
  const q05x = pct(samplesX, 0.05), q95x = pct(samplesX, 0.95);
  const q05y = pct(samplesY, 0.05), q95y = pct(samplesY, 0.95);

  const iqrX = q75x - q25x;
  const iqrY = q75y - q25y;
  // Minimum gap to detect a density split (outlier boundary)
  const minGapX = Math.max(iqrX * 1.5, 1);
  const minGapY = Math.max(iqrY * 1.5, 1);

  // Adaptive expansion: start from IQR core, expand outward
  // checking for density gaps at each expansion level.
  let loX = q25x, hiX = q75x;
  let loY = q25y, hiY = q75y;

  // Expand to Q10–Q90 if no gap
  if (q25x - q10x < minGapX) loX = q10x;
  if (q90x - q75x < minGapX) hiX = q90x;
  if (q25y - q10y < minGapY) loY = q10y;
  if (q90y - q75y < minGapY) hiY = q90y;

  // Expand to Q05–Q95 if no gap
  if (loX === q10x && q10x - q05x < minGapX) loX = q05x;
  if (hiX === q90x && q95x - q90x < minGapX) hiX = q95x;
  if (loY === q10y && q10y - q05y < minGapY) loY = q05y;
  if (hiY === q90y && q95y - q90y < minGapY) hiY = q95y;

  // Add a small margin (5% of range) for visual breathing room
  const rangeX = (hiX - loX) || 1;
  const rangeY = (hiY - loY) || 1;

  const w = rangeX * 1.1;
  const h = rangeY * 1.1;
  const zoom = Math.min(
    (canvasWidth * 0.9) / w,
    (canvasHeight * 0.9) / h,
  );

  // Center on the IQR core (Q25+Q75)/2 — robust against asymmetric outliers
  return {
    centerX: (q25x + q75x) / 2,
    centerY: (q25y + q75y) / 2,
    zoom,
  };
}
