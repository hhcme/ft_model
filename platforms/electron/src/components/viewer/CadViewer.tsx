import { useState, useCallback, useEffect, useRef } from 'react';
import type { DrawData, Viewport, RecentFile } from '../../app/types';
import CadCanvas from './CadCanvas';
import Toolbar from './Toolbar';
import LayerPanel from './LayerPanel';
import StatusBar from './StatusBar';
import { useMeasurement } from '../../hooks/useMeasurement';
import { computeBatchBounds, fitViewToBounds, getPreferredViewBounds } from '../../utils/geometry';

interface Props {
  drawData: DrawData;
  onOpenFile: (file: File, forceReparse?: boolean) => void;
  fileName: string;
  recentFiles: RecentFile[];
  onOpenRecent: (recent: RecentFile) => void;
  onReparse: () => void;
}

export default function CadViewer({ drawData, onOpenFile, fileName, recentFiles, onOpenRecent, onReparse }: Props) {
  const [layerVisible, setLayerVisible] = useState<Map<string, boolean>>(new Map());
  const [layersOpen, setLayersOpen] = useState(false);
  const [viewport, setViewport] = useState<Viewport>({
    centerX: 0, centerY: 0, zoom: 1,
    canvasWidth: 0, canvasHeight: 0, dpr: 1,
  });
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const measure = useMeasurement();

  // Init layers
  useEffect(() => {
    if (!drawData.layers) return;
    const m = new Map<string, boolean>();
    for (const l of drawData.layers) m.set(l.name, !l.frozen && !l.off && l.plotEnabled !== false);
    setLayerVisible(m);
  }, [drawData]);

  // Auto-fit viewport when a new file is loaded
  useEffect(() => {
    const c = canvasRef.current;
    if (!c || !drawData.batches.length) return;
    const w = c.width, h = c.height;
    if (w === 0 || h === 0) return;
    const bb = computeBatchBounds(drawData.batches);
    const fit = fitViewToBounds(drawData.batches, bb, w, h, getPreferredViewBounds(drawData));
    setViewport((prev) => ({ ...prev, ...fit }));
  }, [drawData]);

  const handleResize = useCallback((w: number, h: number) => {
    const dpr = window.devicePixelRatio;
    setViewport((prev) => {
      const next = { ...prev, canvasWidth: w, canvasHeight: h, dpr };
      // First time or data changed: auto fit
      if (prev.canvasWidth === 0 && w > 0 && h > 0 && drawData.batches.length > 0) {
        const bb = computeBatchBounds(drawData.batches);
        const fit = fitViewToBounds(drawData.batches, bb, w, h, getPreferredViewBounds(drawData));
        return { ...next, ...fit };
      }
      return next;
    });
  }, [drawData]);

  const handlePan = useCallback((dx: number, dy: number) => {
    setViewport((prev) => ({
      ...prev,
      centerX: prev.centerX - dx / prev.zoom,
      centerY: prev.centerY + dy / prev.zoom,
    }));
  }, []);

  const handleZoom = useCallback((factor: number, pivotSx: number, pivotSy: number) => {
    setViewport((prev) => {
      const newZoom = prev.zoom * factor;
      const wx = (pivotSx - prev.canvasWidth / 2) / prev.zoom + prev.centerX;
      const wy = -(pivotSy - prev.canvasHeight / 2) / prev.zoom + prev.centerY;
      return {
        ...prev, zoom: newZoom,
        centerX: wx - (pivotSx - prev.canvasWidth / 2) / newZoom,
        centerY: wy + (pivotSy - prev.canvasHeight / 2) / newZoom,
      };
    });
  }, []);

  const handleFit = useCallback(() => {
    const c = canvasRef.current;
    if (!c || !drawData.batches.length) return;
    const bb = computeBatchBounds(drawData.batches);
    const fit = fitViewToBounds(drawData.batches, bb, c.width, c.height, getPreferredViewBounds(drawData));
    setViewport((prev) => ({ ...prev, ...fit }));
  }, [drawData]);

  const handleCanvasReady = useCallback((canvas: HTMLCanvasElement) => {
    canvasRef.current = canvas;
  }, []);

  const toggleLayer = useCallback((name: string, visible: boolean) => {
    setLayerVisible((prev) => { const n = new Map(prev); n.set(name, visible); return n; });
  }, []);
  const showAll = useCallback(() => {
    setLayerVisible((prev) => { const n = new Map(prev); for (const k of n.keys()) n.set(k, true); return n; });
  }, []);
  const hideAll = useCallback(() => {
    setLayerVisible((prev) => { const n = new Map(prev); for (const k of n.keys()) n.set(k, false); return n; });
  }, []);
  const invertAll = useCallback(() => {
    setLayerVisible((prev) => { const n = new Map(prev); for (const [k, v] of n) n.set(k, !v); return n; });
  }, []);

  return (
    <div style={{ width: '100%', height: '100%', position: 'relative', background: '#1e1e2e' }}>
      <CadCanvas
        drawData={drawData}
        viewport={viewport}
        layerVisible={layerVisible}
        measurements={measure.measurements}
        measurePoints={measure.points}
        measurePreview={measure.preview}
        measureMode={measure.mode}
        onPan={handlePan}
        onZoom={handleZoom}
        onFit={handleFit}
        onResize={handleResize}
        onCanvasReady={handleCanvasReady}
      />
      <Toolbar
        onFit={handleFit}
        onReset={handleFit}
        onToggleLayers={() => setLayersOpen((v) => !v)}
        measureMode={measure.mode}
        onMeasureDist={() => measure.setMode(measure.mode === 'dist' ? null : 'dist')}
        onMeasureArea={() => measure.setMode(measure.mode === 'area' ? null : 'area')}
        onMeasureClear={measure.clearAll}
        onOpenFile={onOpenFile}
        onReparse={onReparse}
        recentFiles={recentFiles}
        onOpenRecent={onOpenRecent}
      />
      <LayerPanel
        open={layersOpen}
        onClose={() => setLayersOpen(false)}
        layers={drawData.layers || []}
        visibleMap={layerVisible}
        onToggle={toggleLayer}
        onShowAll={showAll}
        onHideAll={hideAll}
        onInvert={invertAll}
      />
      <StatusBar
        fileName={fileName || (drawData.entityCount + ' entities')}
        entityCount={drawData.entityCount || 0}
        vertexCount={drawData.totalVertices || 0}
        mouseWorld={null}
      />
    </div>
  );
}
