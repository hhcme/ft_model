import { useState, useCallback, useEffect, useRef } from 'react';
import type { MouseEvent as ReactMouseEvent, SyntheticEvent, WheelEvent as ReactWheelEvent } from 'react';
import type { CompareResult, DrawData, Viewport } from '../../app/types';
import CadCanvas from './CadCanvas';
import { computeBatchBounds, fitViewToBounds, getPreferredViewBounds } from '../../utils/geometry';
import { Alert, Button, Space, Badge, Tag, Tooltip } from 'antd';
import { EyeOutlined, UploadOutlined, WarningOutlined, CloseOutlined, SyncOutlined, FolderOpenOutlined } from '@ant-design/icons';

interface Props {
  ours: DrawData | null;
  ourError: string | null;
  refPng: string | null;
  refError: string | null;
  refInfo: CompareResult['refInfo'];
  entityCompare?: CompareResult['entityCompare'];
  visualCompare?: CompareResult['visualCompare'];
  errors?: CompareResult['errors'];
  loading?: CompareResult['loading'];
  referenceMeta?: CompareResult['referenceMeta'];
  fileName: string;
  onOpenFile: (file: File) => void;
  onReparse?: () => void;
}

function statusColor(status?: string): string {
  if (status === 'PASS') return 'success';
  if (status === 'WARN') return 'warning';
  if (status === 'FAIL' || status === 'ERROR') return 'error';
  return 'default';
}

function fmtPct(v?: number): string {
  return Number.isFinite(v) ? `${((v ?? 0) * 100).toFixed(1)}%` : '—';
}

function fmtNum(v?: number): string {
  return Number.isFinite(v) ? Math.round(v ?? 0).toLocaleString() : '—';
}

interface ReferenceView {
  scale: number;
  fitScale: number;
  x: number;
  y: number;
  imageWidth: number;
  imageHeight: number;
  paneWidth: number;
  paneHeight: number;
}

function clamp(v: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, v));
}

function fitReferenceImage(imageWidth: number, imageHeight: number, paneWidth: number, paneHeight: number): ReferenceView {
  if (imageWidth <= 0 || imageHeight <= 0 || paneWidth <= 0 || paneHeight <= 0) {
    return { scale: 1, fitScale: 1, x: 0, y: 0, imageWidth, imageHeight, paneWidth, paneHeight };
  }
  const fitScale = Math.min(paneWidth / imageWidth, paneHeight / imageHeight) * 0.96;
  const scale = Number.isFinite(fitScale) && fitScale > 0 ? fitScale : 1;
  return {
    scale,
    fitScale: scale,
    x: (paneWidth - imageWidth * scale) / 2,
    y: (paneHeight - imageHeight * scale) / 2,
    imageWidth,
    imageHeight,
    paneWidth,
    paneHeight,
  };
}

export default function CompareViewer({
  ours, ourError, refPng, refError, refInfo, entityCompare, visualCompare, errors, loading, referenceMeta, fileName, onOpenFile, onReparse,
}: Props) {
  const [layerVisible, setLayerVisible] = useState<Map<string, boolean>>(new Map());
  const [hideBubble, setHideBubble] = useState(false);
  const [viewport, setViewport] = useState<Viewport>({
    centerX: 0, centerY: 0, zoom: 1,
    canvasWidth: 0, canvasHeight: 0, dpr: 1,
  });
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const containerRef = useRef<HTMLDivElement | null>(null);

  const [leftPct, setLeftPct] = useState(50);
  const [dragging, setDragging] = useState(false);
  const [refDragging, setRefDragging] = useState(false);
  const refPaneRef = useRef<HTMLDivElement | null>(null);
  const refDragRef = useRef<{ pointerX: number; pointerY: number; startX: number; startY: number } | null>(null);
  const [refView, setRefView] = useState<ReferenceView>({
    scale: 1,
    fitScale: 1,
    x: 0,
    y: 0,
    imageWidth: 0,
    imageHeight: 0,
    paneWidth: 0,
    paneHeight: 0,
  });
  const referenceProvider = referenceMeta?.provider || referenceMeta?.parserFramework?.provider || 'reference';
  const referenceRenderer = referenceMeta?.parserFramework?.renderer || referenceProvider;

  // Init layers
  useEffect(() => {
    if (!ours?.layers) return;
    const m = new Map<string, boolean>();
    for (const l of ours.layers) m.set(l.name, !l.frozen && !l.off);
    setLayerVisible(m);
  }, [ours]);

  // Auto-fit viewport
  useEffect(() => {
    const c = canvasRef.current;
    if (!c || !ours?.batches.length) return;
    const w = c.width, h = c.height;
    if (w === 0 || h === 0) return;
    const bb = computeBatchBounds(ours.batches);
    const fit = fitViewToBounds(ours.batches, bb, w, h, getPreferredViewBounds(ours));
    setViewport((prev) => ({ ...prev, ...fit }));
  }, [ours]);

  const handleResize = useCallback((w: number, h: number) => {
    const dpr = window.devicePixelRatio;
    setViewport((prev) => {
      const next = { ...prev, canvasWidth: w, canvasHeight: h, dpr };
      if (prev.canvasWidth === 0 && w > 0 && h > 0 && ours?.batches.length) {
        const bb = computeBatchBounds(ours.batches);
        const fit = fitViewToBounds(ours.batches, bb, w, h, getPreferredViewBounds(ours));
        return { ...next, ...fit };
      }
      return next;
    });
  }, [ours]);

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
    if (!c || !ours?.batches.length) return;
    const bb = computeBatchBounds(ours.batches);
    const fit = fitViewToBounds(ours.batches, bb, c.width, c.height, getPreferredViewBounds(ours));
    setViewport((prev) => ({ ...prev, ...fit }));
  }, [ours]);

  const handleCanvasReady = useCallback((canvas: HTMLCanvasElement) => {
    canvasRef.current = canvas;
  }, []);

  // 拖拽分隔条
  useEffect(() => {
    if (!dragging) return;
    const onMove = (e: MouseEvent) => {
      const rect = containerRef.current?.getBoundingClientRect();
      if (!rect) return;
      const x = e.clientX - rect.left;
      const pct = Math.max(10, Math.min(90, (x / rect.width) * 100));
      setLeftPct(pct);
    };
    const onUp = () => setDragging(false);
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    return () => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };
  }, [dragging]);

  const resetReferenceFit = useCallback(() => {
    const pane = refPaneRef.current;
    if (!pane || refView.imageWidth <= 0 || refView.imageHeight <= 0) return;
    const rect = pane.getBoundingClientRect();
    setRefView(fitReferenceImage(refView.imageWidth, refView.imageHeight, rect.width, rect.height));
  }, [refView.imageHeight, refView.imageWidth]);

  useEffect(() => {
    setRefDragging(false);
    refDragRef.current = null;
    setRefView((prev) => ({ ...prev, scale: 1, fitScale: 1, x: 0, y: 0, imageWidth: 0, imageHeight: 0 }));
  }, [refPng]);

  useEffect(() => {
    const pane = refPaneRef.current;
    if (!pane) return undefined;
    const observer = new ResizeObserver((entries) => {
      const rect = entries[0]?.contentRect;
      if (!rect) return;
      setRefView((prev) => {
        if (prev.imageWidth <= 0 || prev.imageHeight <= 0) {
          return { ...prev, paneWidth: rect.width, paneHeight: rect.height };
        }
        const wasAtFit = Math.abs(prev.scale - prev.fitScale) < 0.0001;
        if (wasAtFit) {
          return fitReferenceImage(prev.imageWidth, prev.imageHeight, rect.width, rect.height);
        }
        return { ...prev, paneWidth: rect.width, paneHeight: rect.height };
      });
    });
    observer.observe(pane);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!refDragging) return;
    const onMove = (e: MouseEvent) => {
      const drag = refDragRef.current;
      if (!drag) return;
      setRefView((prev) => ({
        ...prev,
        x: drag.startX + e.clientX - drag.pointerX,
        y: drag.startY + e.clientY - drag.pointerY,
      }));
    };
    const onUp = () => {
      setRefDragging(false);
      refDragRef.current = null;
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    return () => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };
  }, [refDragging]);

  const handleReferenceImageLoad = useCallback((e: SyntheticEvent<HTMLImageElement>) => {
    const pane = refPaneRef.current;
    const img = e.currentTarget;
    const rect = pane?.getBoundingClientRect();
    if (!rect) return;
    setRefView(fitReferenceImage(img.naturalWidth, img.naturalHeight, rect.width, rect.height));
  }, []);

  const handleReferenceWheel = useCallback((e: ReactWheelEvent<HTMLDivElement>) => {
    if (!refPng || refView.imageWidth <= 0 || refView.imageHeight <= 0) return;
    e.preventDefault();
    const rect = e.currentTarget.getBoundingClientRect();
    const pivotX = e.clientX - rect.left;
    const pivotY = e.clientY - rect.top;
    const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
    setRefView((prev) => {
      const minScale = Math.max(prev.fitScale * 0.12, 0.01);
      const maxScale = Math.max(prev.fitScale * 24, minScale * 2);
      const nextScale = clamp(prev.scale * factor, minScale, maxScale);
      const imageX = (pivotX - prev.x) / prev.scale;
      const imageY = (pivotY - prev.y) / prev.scale;
      return {
        ...prev,
        scale: nextScale,
        x: pivotX - imageX * nextScale,
        y: pivotY - imageY * nextScale,
      };
    });
  }, [refPng, refView.imageHeight, refView.imageWidth]);

  const handleReferenceMouseDown = useCallback((e: ReactMouseEvent<HTMLDivElement>) => {
    if (!refPng || e.button !== 0) return;
    e.preventDefault();
    refDragRef.current = {
      pointerX: e.clientX,
      pointerY: e.clientY,
      startX: refView.x,
      startY: refView.y,
    };
    setRefDragging(true);
  }, [refPng, refView.x, refView.y]);

  return (
    <div ref={containerRef} style={{ width: '100%', height: '100%', display: 'flex', flexDirection: 'row', background: '#121223' }}>
      {onReparse && (
        <div style={{
          position: 'absolute', top: 12, left: 12, zIndex: 20,
          display: 'flex', gap: 6, alignItems: 'center',
        }}>
          <Button size="small" icon={<FolderOpenOutlined />} onClick={() => {
            const input = document.createElement('input');
            input.type = 'file';
            input.accept = '.dwg,.dxf';
            input.onchange = (e) => {
              const f = (e.target as HTMLInputElement).files?.[0];
              if (f) onOpenFile(f);
            };
            input.click();
          }}>打开文件</Button>
          <Button size="small" icon={<SyncOutlined />} onClick={onReparse}>重新解析</Button>
        </div>
      )}
      {!hideBubble && <div style={{
        position: 'absolute',
        top: 12,
        left: '50%',
        transform: 'translateX(-50%)',
        zIndex: 30,
        display: 'flex',
        gap: 8,
        alignItems: 'center',
        background: 'rgba(12,12,25,0.82)',
        border: '1px solid rgba(255,255,255,0.12)',
        borderRadius: 6,
        padding: '6px 10px',
        boxShadow: '0 8px 24px rgba(0,0,0,0.25)',
      }}>
        <Tag color={statusColor(entityCompare?.status)}>实体 {entityCompare?.status || '—'}</Tag>
        <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 12 }}>
          自研 {fmtNum(entityCompare?.ourEntityCount ?? refInfo.ourEntityCount ?? ours?.entityCount)}
          {' / 参考 '}
          {fmtNum(entityCompare?.refEntityCount ?? refInfo.refEntityCount ?? refInfo.entityCount)}
        </span>
        <span style={{ color: 'rgba(255,255,255,0.36)' }}>|</span>
        <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 12 }}>
          缺失 {fmtNum(entityCompare?.missing ?? refInfo.missing)} · 多余 {fmtNum(entityCompare?.extra ?? refInfo.extra)}
        </span>
        <Tag color={statusColor(visualCompare?.status)}>视觉 {visualCompare?.status || '—'}</Tag>
        <Tooltip title={`差异像素: ${fmtPct(visualCompare?.diffPct !== undefined ? visualCompare.diffPct / 100 : undefined)}`}>
          <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 12 }}>
            SSIM {visualCompare?.ssim !== undefined ? visualCompare.ssim.toFixed(3) : '—'}
          </span>
        </Tooltip>
        <CloseOutlined onClick={() => setHideBubble(true)} style={{ color: 'rgba(255,255,255,0.45)', cursor: 'pointer', marginLeft: 4 }} />
      </div>}
      {(errors?.entityCompare || errors?.visualCompare) && (
        <div style={{ position: 'absolute', top: 58, left: '50%', transform: 'translateX(-50%)', zIndex: 30, width: 'min(720px, 80vw)' }}>
          <Alert
            type="warning"
            showIcon
            message={[errors?.entityCompare, errors?.visualCompare].filter(Boolean).join(' · ')}
            banner
          />
        </div>
      )}
      {/* Left: Our rendering */}
      <div style={{ flex: `0 0 ${leftPct}%`, position: 'relative', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
        <div style={{ flex: 1, position: 'relative', overflow: 'hidden' }}>
          {ours ? (
            <CadCanvas
              drawData={ours}
              viewport={viewport}
              layerVisible={layerVisible}
              measurements={[]}
              measurePoints={[]}
              measurePreview={null}
              measureMode={null}
              theme="dark"
              onPan={handlePan}
              onZoom={handleZoom}
              onFit={handleFit}
              onResize={handleResize}
              onCanvasReady={handleCanvasReady}
            />
          ) : loading?.ours && !ourError ? (
            <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
              <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 14 }}>自研解析中</span>
              <span style={{ color: 'rgba(255,255,255,0.38)', fontSize: 11, marginTop: 8 }}>
                {fileName}
              </span>
            </div>
          ) : (
            <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
              <WarningOutlined style={{ fontSize: 32, color: '#ff6b6b', marginBottom: 12 }} />
              <span style={{ color: '#ff6b6b', fontSize: 14 }}>我们的解析器渲染失败</span>
              {ourError && (
                <span style={{ color: 'rgba(255,255,255,0.4)', fontSize: 11, marginTop: 8, maxWidth: '80%', textAlign: 'center' }}>
                  {ourError}
                </span>
              )}
            </div>
          )}
          {/* Mini toolbar — only fit button (open/reparse are in the global toolbar) */}
          <div style={{ position: 'absolute', top: 12, right: 12, zIndex: 10 }}>
            <Space>
              <Badge count="自研" style={{ backgroundColor: '#00ff88', color: '#000' }} />
              {ours && <Button size="small" icon={<EyeOutlined />} onClick={handleFit}>适应</Button>}
            </Space>
          </div>
          {/* File info */}
          <div style={{ position: 'absolute', bottom: 8, left: 12, zIndex: 10 }}>
            <span style={{ fontSize: 11, color: 'rgba(255,255,255,0.5)' }}>
              {fileName}
              {ours ? ` — ${ours.entityCount} 实体 — ${refInfo.ourRenderTimeMs}ms` : ` — ${refInfo.ourRenderTimeMs}ms`}
            </span>
          </div>
        </div>
      </div>

      {/* Draggable splitter */}
      <div
        style={{
          width: 6,
          flexShrink: 0,
          background: 'rgba(255,255,255,0.08)',
          cursor: 'col-resize',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          zIndex: 20,
        }}
        onMouseDown={(e) => { e.preventDefault(); setDragging(true); }}
      >
        <div style={{
          width: 2,
          height: 24,
          borderRadius: 1,
          background: 'rgba(255,255,255,0.3)',
        }} />
      </div>

      {/* Right: Reference rendering */}
      <div style={{ flex: 1, position: 'relative', display: 'flex', flexDirection: 'column', overflow: 'hidden', background: '#1e1e2e' }}>
        <div
          ref={refPaneRef}
          style={{
            flex: 1,
            position: 'relative',
            overflow: 'hidden',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            cursor: refPng ? (refDragging ? 'grabbing' : 'grab') : 'default',
            touchAction: 'none',
            userSelect: 'none',
          }}
          onWheel={handleReferenceWheel}
          onMouseDown={handleReferenceMouseDown}
          onDoubleClick={resetReferenceFit}
        >
          {refPng ? (
            <img
              src={`data:image/png;base64,${refPng}`}
              alt="Reference rendering"
              onLoad={handleReferenceImageLoad}
              draggable={false}
              style={{
                position: 'absolute',
                left: 0,
                top: 0,
                width: refView.imageWidth || 'auto',
                height: refView.imageHeight || 'auto',
                maxWidth: 'none',
                maxHeight: 'none',
                transform: `translate(${refView.x}px, ${refView.y}px) scale(${refView.scale})`,
                transformOrigin: 'top left',
                pointerEvents: 'none',
              }}
            />
          ) : (loading?.reference || !refError) ? (
            <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
              <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 14 }}>第三方参考渲染中</span>
              <span style={{ color: 'rgba(255,255,255,0.38)', fontSize: 11, marginTop: 8 }}>
                自研预览已可继续查看
              </span>
            </div>
          ) : (
            <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
              <WarningOutlined style={{ fontSize: 32, color: '#ff6b6b', marginBottom: 12 }} />
              <span style={{ color: '#ff6b6b', fontSize: 14 }}>参考渲染失败</span>
              {refError && (
                <span style={{ color: 'rgba(255,255,255,0.4)', fontSize: 11, marginTop: 8, maxWidth: '80%', textAlign: 'center' }}>
                  {refError}
                </span>
              )}
            </div>
          )}
          {/* Reference badge */}
          <div style={{ position: 'absolute', top: 12, left: 12, zIndex: 10 }}>
            <Space>
              <Badge count="参考" style={{ backgroundColor: '#ff6b6b', color: '#fff' }} />
              {refPng && (
                <Button
                  size="small"
                  icon={<EyeOutlined />}
                  onMouseDown={(e) => e.stopPropagation()}
                  onClick={(e) => { e.stopPropagation(); resetReferenceFit(); }}
                >
                  适应
                </Button>
              )}
              <span style={{ fontSize: 12, color: 'rgba(255,255,255,0.6)' }}>
                参考渲染{referenceMeta?.cacheHit ? ' · 缓存' : ''} · {referenceRenderer}
              </span>
            </Space>
          </div>
          {/* Ref info */}
          <div style={{ position: 'absolute', bottom: 8, left: 12, zIndex: 10 }}>
            <span style={{ fontSize: 11, color: 'rgba(255,255,255,0.5)' }}>
              {refPng ? `${fmtNum(refInfo.refEntityCount ?? refInfo.entityCount)} 参考实体 — ${refInfo.renderTimeMs}ms` : `${refInfo.renderTimeMs}ms`}
            </span>
          </div>
        </div>
      </div>
    </div>
  );
}
