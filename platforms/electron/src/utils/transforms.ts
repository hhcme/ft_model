import type { Viewport } from '../app/types';

export function worldToScreen(
  wx: number, wy: number, vp: Viewport,
): [number, number] {
  const sx = (wx - vp.centerX) * vp.zoom + vp.canvasWidth / 2;
  const sy = -(wy - vp.centerY) * vp.zoom + vp.canvasHeight / 2;
  return [sx, sy];
}

export function screenToWorld(
  sx: number, sy: number, vp: Viewport,
): [number, number] {
  const wx = (sx - vp.canvasWidth / 2) / vp.zoom + vp.centerX;
  const wy = -(sy - vp.canvasHeight / 2) / vp.zoom + vp.centerY;
  return [wx, wy];
}

export function getViewportWorldBounds(vp: Viewport) {
  const [minX, minY] = screenToWorld(0, vp.canvasHeight, vp);
  const [maxX, maxY] = screenToWorld(vp.canvasWidth, 0, vp);
  return { minX, maxX, minY, maxY };
}
