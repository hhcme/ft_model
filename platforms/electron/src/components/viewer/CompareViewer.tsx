import { useState, useCallback, useEffect, useRef } from 'react';
import type { MouseEvent as ReactMouseEvent, WheelEvent as ReactWheelEvent } from 'react';
import type { CompareResult, DrawData, Viewport } from '../../app/types';
import CadCanvas from './CadCanvas';
import HoopsViewer, { type HoopsViewerHandle } from './HoopsViewer';
import { useScsFile } from '../../hooks/useScsFile';
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

function fmtNum(v?: number): string {
  return Number.isFinite(v) ? Math.round(v ?? 0).toLocaleString() : '—';
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
  const hoopsRef = useRef<HoopsViewerHandle>(null);
  const { scsUrl, scsExists, checking: scsChecking } = useScsFile(fileName);
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

  // Drag splitter
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

  const scsFileName = fileName.replace(/\.(dwg|dxf)$/i, '.scs');

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
        <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 12 }}>
          SSIM {visualCompare?.ssim !== undefined ? visualCompare.ssim.toFixed(3) : '—'}
        </span>
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
          <div style={{ position: 'absolute', top: 12, right: 12, zIndex: 10 }}>
            <Space>
              <Badge count="自研" style={{ backgroundColor: '#00ff88', color: '#000' }} />
              {ours && <Button size="small" icon={<EyeOutlined />} onClick={handleFit}>适应</Button>}
            </Space>
          </div>
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

      {/* Right: HOOPS Viewer */}
      <div style={{ flex: 1, position: 'relative', display: 'flex', flexDirection: 'column', overflow: 'hidden', background: '#1e1e2e' }}>
        <div style={{ flex: 1, position: 'relative', overflow: 'hidden' }}>
          {scsExists === true ? (
            <HoopsViewer ref={hoopsRef} scsUrl={scsUrl} fileName={scsFileName} />
          ) : scsChecking || scsExists === null ? (
            <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
              <span style={{ color: 'rgba(255,255,255,0.72)', fontSize: 14 }}>检查 SCS 文件...</span>
            </div>
          ) : (
            <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
              <WarningOutlined style={{ fontSize: 32, color: '#ff6b6b', marginBottom: 12 }} />
              <span style={{ color: '#ff6b6b', fontSize: 14 }}>SCS 文件不存在</span>
              <span style={{ color: 'rgba(255,255,255,0.4)', fontSize: 11, marginTop: 8 }}>
                请将 {scsFileName} 放入 scs_dwg/ 目录
              </span>
            </div>
          )}
          {/* HOOPS badge */}
          <div style={{ position: 'absolute', top: 12, left: 12, zIndex: 10 }}>
            <Space>
              <Badge count="HOOPS" style={{ backgroundColor: '#ff9800', color: '#fff' }} />
              {scsExists === true && (
                <Button
                  size="small"
                  icon={<EyeOutlined />}
                  onClick={() => hoopsRef.current?.fitWorld()}
                >
                  适应
                </Button>
              )}
            </Space>
          </div>
          {/* Ref info */}
          <div style={{ position: 'absolute', bottom: 8, left: 12, zIndex: 10 }}>
            <span style={{ fontSize: 11, color: 'rgba(255,255,255,0.5)' }}>
              {scsExists ? 'HOOPS Communicator' : `参考渲染 ${referenceRenderer}`}
            </span>
          </div>
        </div>
      </div>
    </div>
  );
}
