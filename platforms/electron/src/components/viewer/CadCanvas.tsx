import { useRef, useEffect, useMemo } from 'react';
import type { DrawData, Viewport, Measurement, MeasurePoint } from '../../app/types';
import { computeBatchBounds, computeOutlierResistantBounds } from '../../utils/geometry';
import {
  renderGrid, renderBatches, renderTexts, renderMeasurements,
  renderBorder, renderPaper, withWorldClip, beginRenderFrame,
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
}

export default function CadCanvas({
  drawData, viewport, layerVisible, measurements, measurePoints,
  measurePreview, measureMode, theme = 'dark', onPan, onZoom, onFit, onResize, onCanvasReady,
}: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rafRef = useRef(0);
  const needsRender = useRef(true);
  const propsRef = useRef({ drawData, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode });
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
    propsRef.current = { drawData, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode };
    needsRender.current = true;
  }, [drawData, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode]);

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
      d.dragging = true; d.x = e.clientX; d.y = e.clientY; e.preventDefault();
    };
    const onMouseMove = (e: MouseEvent) => {
      if (!d.dragging) return;
      onPan(e.clientX - d.x, e.clientY - d.y);
      d.x = e.clientX; d.y = e.clientY; e.preventDefault();
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
    window.addEventListener('mouseup', () => { d.dragging = false; });
    cv.addEventListener('wheel', onWheel, opts);
    cv.addEventListener('dblclick', onFit);
    cv.addEventListener('touchstart', onTouchStart, opts);
    cv.addEventListener('touchmove', onTouchMove, opts);
    cv.addEventListener('touchend', () => { d.touchDist = 0; });
    return () => {
      cv.removeEventListener('mousedown', onMouseDown);
      window.removeEventListener('mousemove', onMouseMove);
      cv.removeEventListener('wheel', onWheel);
      cv.removeEventListener('dblclick', onFit);
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
    };
    rafRef.current = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(rafRef.current);
  }, []);

  return <canvas ref={canvasRef} style={{ display: 'block', width: '100%', height: '100%', cursor: 'grab', touchAction: 'none' }} />;
}
