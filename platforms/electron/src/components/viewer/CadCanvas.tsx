import { useRef, useEffect, useCallback, useMemo } from 'react';
import type { DrawData, Viewport, Measurement, MeasurePoint } from '../../app/types';
import { computeBatchBounds, computeOutlierResistantBounds } from '../../utils/geometry';
import {
  renderGrid,
  renderBatches,
  renderTexts,
  renderMeasurements,
  renderBorder,
  renderPaper,
  withWorldClip,
} from '../../utils/renderer';

interface Props {
  drawData: DrawData | null;
  viewport: Viewport;
  layerVisible: Map<string, boolean>;
  measurements: Measurement[];
  measurePoints: MeasurePoint[];
  measurePreview: MeasurePoint | null;
  measureMode: 'dist' | 'area' | null;
  onPan: (dx: number, dy: number) => void;
  onZoom: (factor: number, pivotX: number, pivotY: number) => void;
  onFit: () => void;
  onResize: (w: number, h: number) => void;
  onWorldClick?: (wx: number, wy: number) => void;
  onCanvasReady?: (canvas: HTMLCanvasElement) => void;
}

export default function CadCanvas({
  drawData, viewport, layerVisible,
  measurements, measurePoints, measurePreview, measureMode,
  onPan, onZoom, onFit, onResize, onWorldClick, onCanvasReady,
}: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rafRef = useRef(0);

  // Mouse/touch state
  const isDragging = useRef(false);
  const lastX = useRef(0);
  const lastY = useRef(0);
  const lastTouchDist = useRef(0);
  const lastTapTime = useRef(0);

  // Cache batch bounds — only recompute when drawData changes
  const batchBounds = useMemo(() => computeBatchBounds(drawData?.batches ?? []), [drawData]);
  const activeView = useMemo(
    () => drawData?.views?.find((v) => v.id === drawData.activeViewId),
    [drawData],
  );

  // Canvas resize
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

  // Notify parent canvas is ready
  useEffect(() => {
    if (canvasRef.current) onCanvasReady?.(canvasRef.current);
  }, [onCanvasReady]);

  // Event listeners
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const onMouseDown = (e: MouseEvent) => {
      if (e.button !== 0) return;
      isDragging.current = true;
      lastX.current = e.clientX;
      lastY.current = e.clientY;
      e.preventDefault();
    };
    const onMouseMove = (e: MouseEvent) => {
      // Always update world coordinates for status bar
      const rect = canvas.getBoundingClientRect();
      const sx = e.clientX - rect.left;
      const sy = e.clientY - rect.top;
      if (!isDragging.current) return;
      const dx = e.clientX - lastX.current;
      const dy = e.clientY - lastY.current;
      lastX.current = e.clientX;
      lastY.current = e.clientY;
      onPan(dx, dy);
      e.preventDefault();
    };
    const onMouseUp = () => { isDragging.current = false; };
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const rect = canvas.getBoundingClientRect();
      onZoom(e.deltaY > 0 ? 0.9 : 1.1, e.clientX - rect.left, e.clientY - rect.top);
    };
    const onDblClick = () => onFit();
    const onTouchStart = (e: TouchEvent) => {
      if (e.touches.length === 1) {
        const now = Date.now();
        if (now - lastTapTime.current < 300) { onFit(); lastTapTime.current = 0; e.preventDefault(); return; }
        lastTapTime.current = now;
        lastX.current = e.touches[0].clientX;
        lastY.current = e.touches[0].clientY;
      } else if (e.touches.length === 2) {
        const dx = e.touches[1].clientX - e.touches[0].clientX;
        const dy = e.touches[1].clientY - e.touches[0].clientY;
        lastTouchDist.current = Math.sqrt(dx * dx + dy * dy);
      }
      e.preventDefault();
    };
    const onTouchMove = (e: TouchEvent) => {
      if (e.touches.length === 1) {
        const dx = e.touches[0].clientX - lastX.current;
        const dy = e.touches[0].clientY - lastY.current;
        lastX.current = e.touches[0].clientX;
        lastY.current = e.touches[0].clientY;
        onPan(dx, dy);
      } else if (e.touches.length === 2) {
        const dx = e.touches[1].clientX - e.touches[0].clientX;
        const dy = e.touches[1].clientY - e.touches[0].clientY;
        const dist = Math.sqrt(dx * dx + dy * dy);
        if (lastTouchDist.current > 0) {
          const rect = canvas.getBoundingClientRect();
          const cx = (e.touches[0].clientX + e.touches[1].clientX) / 2 - rect.left;
          const cy = (e.touches[0].clientY + e.touches[1].clientY) / 2 - rect.top;
          onZoom(dist / lastTouchDist.current, cx, cy);
        }
        lastTouchDist.current = dist;
      }
      e.preventDefault();
    };
    const onTouchEnd = () => { lastTouchDist.current = 0; };

    canvas.addEventListener('mousedown', onMouseDown);
    window.addEventListener('mousemove', onMouseMove);
    window.addEventListener('mouseup', onMouseUp);
    canvas.addEventListener('wheel', onWheel, { passive: false });
    canvas.addEventListener('dblclick', onDblClick);
    canvas.addEventListener('touchstart', onTouchStart, { passive: false });
    canvas.addEventListener('touchmove', onTouchMove, { passive: false });
    canvas.addEventListener('touchend', onTouchEnd);
    return () => {
      canvas.removeEventListener('mousedown', onMouseDown);
      window.removeEventListener('mousemove', onMouseMove);
      window.removeEventListener('mouseup', onMouseUp);
      canvas.removeEventListener('wheel', onWheel);
      canvas.removeEventListener('dblclick', onDblClick);
      canvas.removeEventListener('touchstart', onTouchStart);
      canvas.removeEventListener('touchmove', onTouchMove);
      canvas.removeEventListener('touchend', onTouchEnd);
    };
  }, [onPan, onZoom, onFit]);

  // Render loop — re-render whenever props change
  useEffect(() => {
    const render = () => {
      const canvas = canvasRef.current;
      if (!canvas || canvas.width === 0 || canvas.height === 0) return;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;

      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = '#1e1e2e';
      ctx.fillRect(0, 0, canvas.width, canvas.height);

      const paperMode = activeView?.paperMode === true;
      if (!paperMode) renderGrid(ctx, viewport);

      if (!drawData?.batches?.length) return;

      renderPaper(ctx, activeView, viewport);
      const presentationBounds = activeView?.presentationBounds ?? drawData.presentationBounds ?? drawData.bounds;
      const reliableSource = !activeView?.source || activeView.source === 'layout' ||
        activeView.source === 'vport' || activeView.source === 'paperSpaceFallback';
      const outlierBounds = computeOutlierResistantBounds(drawData.batches, batchBounds);
      const tightBounds = reliableSource ? presentationBounds : (outlierBounds ?? presentationBounds);
      renderBorder(ctx, tightBounds, viewport);
      const artifactBounds = activeView?.source === 'vport' ? undefined
        : reliableSource ? presentationBounds
        : outlierBounds ?? presentationBounds;
      withWorldClip(ctx, activeView?.clipBounds, viewport, () => {
        renderBatches(ctx, drawData.batches, batchBounds, viewport, layerVisible, activeView?.clipBounds, artifactBounds, paperMode);

        if (drawData.texts?.length) {
          renderTexts(ctx, drawData.texts, viewport, layerVisible, activeView?.clipBounds, paperMode);
        }
      });

      renderMeasurements(ctx, measurements, measurePoints, measurePreview, viewport, measureMode);
    };
    render();
  }, [drawData, activeView, viewport, layerVisible, measurements, measurePoints, measurePreview, measureMode]);

  return (
    <canvas
      ref={canvasRef}
      style={{ display: 'block', width: '100%', height: '100%', cursor: 'grab', touchAction: 'none' }}
    />
  );
}
