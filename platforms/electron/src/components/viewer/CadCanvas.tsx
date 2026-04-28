import { useRef, useEffect, useMemo } from 'react';
import type { DrawData, Viewport, Measurement, MeasurePoint } from '../../app/types';
import type { SelectionState } from '../../hooks/useSelection';
import { computeBatchBounds, computeOutlierResistantBounds } from '../../utils/geometry';
import { screenToWorld } from '../../utils/transforms';
import {
  renderGrid, renderBatches, renderTexts, renderMeasurements,
  renderBorder, renderPaper, withWorldClip, beginRenderFrame,
  renderSingleBatchLines, renderSingleBatchLinestrip,
} from '../../utils/renderer';

interface Props {
  drawData: DrawData | null;
  viewport: Viewport;
  layerVisible: Map<string, boolean>;
  measurements: Measurement[];
  measurePoints: MeasurePoint[];
  measurePreview: MeasurePoint | null;
  measureMode: 'dist' | 'area' | null;
  theme?: 'dark' | 'light';
  onPan: (dx: number, dy: number) => void;
  onZoom: (factor: number, pivotX: number, pivotY: number) => void;
  onFit: () => void;
  onResize: (w: number, h: number) => void;
  onCanvasReady?: (canvas: HTMLCanvasElement) => void;
  onMeasureClick?: (wx: number, wy: number) => void;
  onMeasureMove?: (wx: number, wy: number) => void;
  onMeasureFinish?: () => void;
  selection?: SelectionState;
  onSelect?: (screenX: number, screenY: number) => void;
}

export default function CadCanvas({
  drawData, viewport, layerVisible, measurements, measurePoints,
  measurePreview, measureMode, theme = 'dark',
  onPan, onZoom, onFit, onResize, onCanvasReady,
  onMeasureClick, onMeasureMove, onMeasureFinish,
  selection, onSelect,
}: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rafRef = useRef(0);
  const needsRender = useRef(true);
  const propsRef = useRef({ drawData, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode, onMeasureClick, onMeasureMove, onMeasureFinish, selection, onSelect });
  const dragRef = useRef({ dragging: false, x: 0, y: 0, touchDist: 0, tapTime: 0 });
  const boundsRef = useRef<{ bb: ReturnType<typeof computeBatchBounds>; ob: ReturnType<typeof computeOutlierResistantBounds> } | null>(null);

  // Memoize batch/outlier bounds, store in ref so the rAF loop sees current values
  const batchBounds = useMemo(() => computeBatchBounds(drawData?.batches ?? []), [drawData]);
  const outlierBounds = useMemo(
    () => computeOutlierResistantBounds(drawData?.batches ?? [], batchBounds),
    [drawData, batchBounds],
  );
  useEffect(() => { boundsRef.current = { bb: batchBounds, ob: outlierBounds }; }, [batchBounds, outlierBounds]);

  useEffect(() => {
    propsRef.current = { drawData, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode, onMeasureClick, onMeasureMove, onMeasureFinish, selection, onSelect };
    needsRender.current = true;
  }, [drawData, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode, onMeasureClick, onMeasureMove, onMeasureFinish, selection, onSelect]);

  // Canvas resize observer
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const observer = new ResizeObserver(() => {
      const dpr = window.devicePixelRatio;
      const rect = canvas.getBoundingClientRect();
      canvas.width = rect.width * dpr;
      canvas.height = rect.height * dpr;
      onResize(canvas.width, canvas.height);
    });
    observer.observe(canvas);
    return () => observer.disconnect();
  }, [onResize]);

  useEffect(() => { if (canvasRef.current) onCanvasReady?.(canvasRef.current); }, [onCanvasReady]);

  // Mouse / touch event listeners
  useEffect(() => {
    const cv = canvasRef.current;
    if (!cv) return;
    const d = dragRef.current;

    const onMouseDown = (e: MouseEvent) => {
      if (e.button !== 0) return;
      const p = propsRef.current;
      if (p.measureMode && p.onMeasureClick) {
        // Measurement mode: click to add point
        const r = cv.getBoundingClientRect();
        const [wx, wy] = screenToWorld(e.clientX - r.left, e.clientY - r.top, p.viewport);
        p.onMeasureClick(wx, wy);
        e.preventDefault();
        return;
      }
      d.dragging = true; d.x = e.clientX; d.y = e.clientY;
      d.tapTime = Date.now();
      e.preventDefault();
    };
    const onMouseUp = (e: MouseEvent) => {
      const wasDragging = d.dragging;
      const dt = Date.now() - d.tapTime;
      d.dragging = false;
      // Short click without significant drag = select
      if (wasDragging && dt < 200 && Math.abs(e.clientX - d.x) < 5 && Math.abs(e.clientY - d.y) < 5) {
        const p = propsRef.current;
        if (!p.measureMode && p.onSelect) {
          p.onSelect(e.clientX, e.clientY);
        }
      }
    };
    const onMouseMove = (e: MouseEvent) => {
      const p = propsRef.current;
      if (d.dragging) {
        onPan(e.clientX - d.x, e.clientY - d.y);
        d.x = e.clientX; d.y = e.clientY; e.preventDefault();
      } else if (p.measureMode && p.onMeasureMove) {
        // Update measurement preview on hover
        const r = cv.getBoundingClientRect();
        const [wx, wy] = screenToWorld(e.clientX - r.left, e.clientY - r.top, p.viewport);
        p.onMeasureMove(wx, wy);
      }
    };
    const onContextMenu = (e: MouseEvent) => {
      e.preventDefault();
      const p = propsRef.current;
      if (p.measureMode === 'area' && p.onMeasureFinish) {
        p.onMeasureFinish();
      }
    };
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const r = cv.getBoundingClientRect();
      onZoom(e.deltaY > 0 ? 0.9 : 1.1, e.clientX - r.left, e.clientY - r.top);
    };
    const onTouchStart = (e: TouchEvent) => {
      if (e.touches.length === 1) {
        const now = Date.now();
        if (now - d.tapTime < 300) { onFit(); d.tapTime = 0; e.preventDefault(); return; }
        d.tapTime = now; d.x = e.touches[0].clientX; d.y = e.touches[0].clientY;
      } else if (e.touches.length === 2) {
        const dx = e.touches[1].clientX - e.touches[0].clientX;
        const dy = e.touches[1].clientY - e.touches[0].clientY;
        d.touchDist = Math.hypot(dx, dy);
      }
      e.preventDefault();
    };
    const onTouchMove = (e: TouchEvent) => {
      if (e.touches.length === 1) {
        onPan(e.touches[0].clientX - d.x, e.touches[0].clientY - d.y);
        d.x = e.touches[0].clientX; d.y = e.touches[0].clientY;
      } else if (e.touches.length === 2 && d.touchDist > 0) {
        const dx = e.touches[1].clientX - e.touches[0].clientX;
        const dy = e.touches[1].clientY - e.touches[0].clientY;
        const r = cv.getBoundingClientRect();
        onZoom(Math.hypot(dx, dy) / d.touchDist,
          (e.touches[0].clientX + e.touches[1].clientX) / 2 - r.left,
          (e.touches[0].clientY + e.touches[1].clientY) / 2 - r.top);
        d.touchDist = Math.hypot(dx, dy);
      }
      e.preventDefault();
    };
    const opts = { passive: false } as const;
    cv.addEventListener('mousedown', onMouseDown);
    window.addEventListener('mousemove', onMouseMove);
    window.addEventListener('mouseup', onMouseUp);
    cv.addEventListener('wheel', onWheel, opts);
    cv.addEventListener('dblclick', onFit);
    cv.addEventListener('contextmenu', onContextMenu);
    cv.addEventListener('touchstart', onTouchStart, opts);
    cv.addEventListener('touchmove', onTouchMove, opts);
    cv.addEventListener('touchend', () => { d.touchDist = 0; });
    return () => {
      cv.removeEventListener('mousedown', onMouseDown);
      window.removeEventListener('mousemove', onMouseMove);
      window.removeEventListener('mouseup', onMouseUp);
      cv.removeEventListener('wheel', onWheel);
      cv.removeEventListener('dblclick', onFit);
      cv.removeEventListener('contextmenu', onContextMenu);
      cv.removeEventListener('touchstart', onTouchStart);
      cv.removeEventListener('touchmove', onTouchMove);
    };
  }, [onPan, onZoom, onFit]);

  // rAF render loop — runs continuously, only draws when dirty
  useEffect(() => {
    const loop = () => {
      rafRef.current = requestAnimationFrame(loop);
      if (!needsRender.current) return;
      needsRender.current = false;
      const canvas = canvasRef.current;
      if (!canvas || canvas.width === 0) return;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      const p = propsRef.current;
      const dd = p.drawData, vp = p.viewport;
      const av = dd?.views?.find((v) => v.id === dd?.activeViewId);
      const b = boundsRef.current;
      const bb = b?.bb ?? [];
      const ob = b?.ob;
      const isLight = theme === 'light';
      beginRenderFrame(vp.zoom);
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = isLight ? '#ffffff' : '#1e1e2e';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      if (av?.paperMode !== true) renderGrid(ctx, vp, theme);
      if (!dd?.batches?.length) return;
      renderPaper(ctx, av, vp);
      const pb = av?.presentationBounds ?? dd.presentationBounds ?? dd.bounds;
      const rel = !av?.source || av.source === 'layout' || av.source === 'vport' || av.source === 'paperSpaceFallback';
      // Only draw dashed border for non-paper views; paper views already have renderPaper border
      if (!av?.paperMode) renderBorder(ctx, rel ? pb : ob ?? pb, vp, theme);
      const artB = av?.source === 'vport' ? undefined : rel ? pb : ob ?? pb;
      withWorldClip(ctx, av?.clipBounds, vp, () => {
        renderBatches(ctx, dd.batches, bb, vp, p.layerVisible, av?.clipBounds, artB, av?.paperMode === true || isLight);
        if (dd.texts?.length) renderTexts(ctx, dd.texts, vp, p.layerVisible, av?.clipBounds, av?.paperMode === true || isLight);
      });
      renderMeasurements(ctx, p.measurements, p.measurePoints, p.measurePreview, vp, p.measureMode);
      // Selection highlight overlay — uses inline transform constants
      if (p.selection) {
        const s = p.selection;
        const hc = s.highlightColor;
        const col = `rgb(${hc[0] * 255 | 0},${hc[1] * 255 | 0},${hc[2] * 255 | 0})`;
        ctx.save();
        ctx.lineWidth = 2;
        ctx.strokeStyle = col;
        const sz = vp.zoom;
        const shw = vp.canvasWidth * 0.5;
        const shh = vp.canvasHeight * 0.5;
        const scx = vp.centerX;
        const scy = vp.centerY;
        if (s.selectedBatchIndex !== null && dd.batches[s.selectedBatchIndex]) {
          const batch = dd.batches[s.selectedBatchIndex];
          if (batch.topology === 'lines') {
            renderSingleBatchLines(ctx, batch, sz, shw, shh, scx, scy);
          } else if (batch.topology === 'linestrip') {
            renderSingleBatchLinestrip(ctx, batch, sz, shw, shh, scx, scy);
          }
        }
        if (s.selectedTextIndex !== null && dd.texts?.[s.selectedTextIndex]) {
          const t = dd.texts[s.selectedTextIndex];
          const thw = (t.rectWidth ?? t.height * 6) * 0.5;
          const thh = (t.rectHeight ?? t.height) * 0.5;
          const sx1 = (t.x - thw - scx) * sz + shw;
          const sy1 = -(t.y + thh - scy) * sz + shh;
          const sx2 = (t.x + thw - scx) * sz + shw;
          const sy2 = -(t.y - thh - scy) * sz + shh;
          ctx.strokeRect(Math.min(sx1, sx2), Math.min(sy1, sy2), Math.abs(sx2 - sx1), Math.abs(sy2 - sy1));
        }
        ctx.restore();
      }
    };
    rafRef.current = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(rafRef.current);
  }, []);

  return <canvas ref={canvasRef} style={{ display: 'block', width: '100%', height: '100%', cursor: measureMode ? 'crosshair' : 'grab', touchAction: 'none' }} />;
}
