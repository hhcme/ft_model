import { useCallback, useRef, useState } from 'react';
import type { Batch, TextEntity, Viewport } from '../app/types';
import { screenToWorld } from '../utils/transforms';

export interface SelectionState {
  selectedBatchIndex: number | null;
  selectedTextIndex: number | null;
  highlightColor: [number, number, number];
}

const HIGHLIGHT_COLOR: [number, number, number] = [0, 1, 0.53]; // #00ff88

/** Minimum distance in world units to consider a hit. */
const PICK_TOLERANCE_WORLD = 2.0;

/** Distance from point to line segment (world coords). */
function distToSegment(px: number, py: number, ax: number, ay: number, bx: number, by: number): number {
  const dx = bx - ax, dy = by - ay;
  const lenSq = dx * dx + dy * dy;
  if (lenSq < 1e-12) return Math.hypot(px - ax, py - ay);
  let t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
  t = Math.max(0, Math.min(1, t));
  return Math.hypot(px - (ax + t * dx), py - (ay + t * dy));
}

export function useSelection() {
  const [selection, setSelection] = useState<SelectionState>({
    selectedBatchIndex: null,
    selectedTextIndex: null,
    highlightColor: HIGHLIGHT_COLOR,
  });
  const selectionRef = useRef(selection);
  selectionRef.current = selection;

  const pick = useCallback((
    screenX: number, screenY: number,
    viewport: Viewport,
    batches: Batch[],
    texts: TextEntity[],
  ) => {
    const [wx, wy] = screenToWorld(screenX, screenY, viewport);
    const tolerance = PICK_TOLERANCE_WORLD / viewport.zoom;

    let bestDist = tolerance;
    let bestBatch = -1;
    let bestText = -1;

    // Check batches
    for (let bi = 0; bi < batches.length; bi++) {
      const v = batches[bi].vertices;
      const topo = batches[bi].topology;

      if (topo === 'lines') {
        for (let i = 0; i + 3 < v.length; i += 4) {
          const d = distToSegment(wx, wy, v[i], v[i + 1], v[i + 2], v[i + 3]);
          if (d < bestDist) { bestDist = d; bestBatch = bi; bestText = -1; }
        }
      } else if (topo === 'linestrip') {
        for (let i = 0; i + 3 < v.length; i += 2) {
          const d = distToSegment(wx, wy, v[i], v[i + 1], v[i + 2], v[i + 3]);
          if (d < bestDist) { bestDist = d; bestBatch = bi; bestText = -1; }
        }
      } else { // triangles — point-in-triangle or closest vertex
        for (let i = 0; i + 5 < v.length; i += 6) {
          const d1 = Math.hypot(wx - v[i], wy - v[i + 1]);
          const d2 = Math.hypot(wx - v[i + 2], wy - v[i + 3]);
          const d3 = Math.hypot(wx - v[i + 4], wy - v[i + 5]);
          const d = Math.min(d1, d2, d3);
          if (d < bestDist) { bestDist = d; bestBatch = bi; bestText = -1; }
        }
      }
    }

    // Check texts
    for (let ti = 0; ti < texts.length; ti++) {
      const t = texts[ti];
      const hw = (t.rectWidth ?? t.height * 6) * 0.5;
      const hh = (t.rectHeight ?? t.height) * 0.5;
      if (wx >= t.x - hw && wx <= t.x + hw && wy >= t.y - hh && wy <= t.y + hh) {
        const d = Math.hypot(wx - t.x, wy - t.y);
        if (d < bestDist) { bestDist = d; bestText = ti; bestBatch = -1; }
      }
    }

    if (bestBatch >= 0) {
      setSelection({ selectedBatchIndex: bestBatch, selectedTextIndex: null, highlightColor: HIGHLIGHT_COLOR });
      return { type: 'batch' as const, index: bestBatch, layerName: batches[bestBatch].layerName };
    }
    if (bestText >= 0) {
      setSelection({ selectedBatchIndex: null, selectedTextIndex: bestText, highlightColor: HIGHLIGHT_COLOR });
      return { type: 'text' as const, index: bestText, layerName: texts[bestText].layerName };
    }

    // Clicked empty space — deselect
    setSelection({ selectedBatchIndex: null, selectedTextIndex: null, highlightColor: HIGHLIGHT_COLOR });
    return null;
  }, []);

  const clear = useCallback(() => {
    setSelection({ selectedBatchIndex: null, selectedTextIndex: null, highlightColor: HIGHLIGHT_COLOR });
  }, []);

  return { selection, selectionRef, pick, clear };
}
