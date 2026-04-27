import { useState, useCallback, useEffect, useRef, useMemo } from 'react';
import type { DrawData, Viewport, RecentFile } from '../../app/types';
import CadCanvas from './CadCanvas';
import Toolbar from './Toolbar';
import LayerPanel from './LayerPanel';
import StatusBar from './StatusBar';
import LayoutTabBar from './LayoutTabBar';
import { useMeasurement } from '../../hooks/useMeasurement';
import { useLayoutViews } from '../../hooks/useLayoutViews';
import { useSelection } from '../../hooks/useSelection';
import { computeBatchBounds, fitViewToBounds } from '../../utils/geometry';

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
  const sel = useSelection();
  const layout = useLayoutViews(drawData);

  const filteredDrawData = useMemo(() => ({
    ...drawData,
    batches: layout.filteredBatches,
    texts: layout.filteredTexts,
    activeViewId: layout.currentViewId,
  }), [drawData, layout.filteredBatches, layout.filteredTexts, layout.currentViewId]);

  // Init layers
  useEffect(() => {
    if (!drawData.layers) return;
    const m = new Map<string, boolean>();
    for (const l of drawData.layers) m.set(l.name, !l.frozen && !l.off);
    setLayerVisible(m);
  }, [drawData]);

  // Auto-fit viewport when data or view changes
  useEffect(() => {
    const c = canvasRef.current;
    if (!c || !layout.filteredBatches.length) return;
    const w = c.width, h = c.height;
    if (w === 0 || h === 0) return;
    const bb = computeBatchBounds(layout.filteredBatches);
    const fit = fitViewToBounds(layout.filteredBatches, bb, w, h, layout.fitBounds);
    setViewport((prev) => ({ ...prev, ...fit }));
  }, [layout.filteredBatches, layout.fitBounds]);

  const handleResize = useCallback((w: number, h: number) => {
    const dpr = window.devicePixelRatio;
    setViewport((prev) => {
      const next = { ...prev, canvasWidth: w, canvasHeight: h, dpr };
      if (prev.canvasWidth === 0 && w > 0 && h > 0 && layout.filteredBatches.length > 0) {
        const bb = computeBatchBounds(layout.filteredBatches);
        const fit = fitViewToBounds(layout.filteredBatches, bb, w, h, layout.fitBounds);
        return { ...next, ...fit };
      }
      return next;
    });
  }, [layout.filteredBatches, layout.fitBounds]);

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
    if (!c || !layout.filteredBatches.length) return;
    const bb = computeBatchBounds(layout.filteredBatches);
    const fit = fitViewToBounds(layout.filteredBatches, bb, c.width, c.height, layout.fitBounds);
    setViewport((prev) => ({ ...prev, ...fit }));
  }, [layout.filteredBatches, layout.fitBounds]);

  const handleCanvasReady = useCallback((canvas: HTMLCanvasElement) => {
    canvasRef.current = canvas;
  }, []);

  const handleSelect = useCallback((screenX: number, screenY: number) => {
    const c = canvasRef.current;
    if (!c) return;
    const r = c.getBoundingClientRect();
    sel.pick(screenX - r.left, screenY - r.top, viewport, layout.filteredBatches, layout.filteredTexts);
  }, [viewport, layout.filteredBatches, layout.filteredTexts]);

  const handleExportPdf = useCallback(() => {
    const c = canvasRef.current;
    if (!c) return;
    const dataUrl = c.toDataURL('image/png');
    const w = window.open('', '_blank');
    if (!w) return;
    w.document.write(`<!DOCTYPE html><html><head><title>${fileName || 'CAD Export'}</title>
<style>@media print { @page { margin: 0; } body { margin: 0; } img { width: 100vw; height: 100vh; object-fit: contain; } }</style></head>
<body style="margin:0;display:flex;justify-content:center;align-items:center;min-height:100vh;background:#fff">
<img src="${dataUrl}" style="max-width:100%;max-height:100vh"/>
<script>setTimeout(()=>window.print(),300)</script>
</body></html>`);
    w.document.close();
  }, [fileName]);

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
        drawData={filteredDrawData}
        viewport={viewport}
        layerVisible={layerVisible}
        measurements={measure.measurements}
        measurePoints={measure.points}
        measurePreview={measure.preview}
        measureMode={measure.mode}
        selection={sel.selection}
        onSelect={handleSelect}
        onPan={handlePan}
        onZoom={handleZoom}
        onFit={handleFit}
        onResize={handleResize}
        onCanvasReady={handleCanvasReady}
        onMeasureClick={measure.addPoint}
        onMeasureMove={measure.setPreview}
        onMeasureFinish={measure.finishArea}
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
        onExportPdf={handleExportPdf}
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
      <LayoutTabBar
        tabs={layout.layoutTabs}
        activeViewId={layout.currentViewId}
        onSwitch={layout.switchView}
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
