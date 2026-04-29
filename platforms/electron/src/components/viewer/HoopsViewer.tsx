import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react';
import { WebViewer } from '@ts3d-hoops/web-viewer-monolith';

export interface HoopsViewerHandle {
  fitWorld: () => void;
  reset: () => void;
  setView: (preset: 'front' | 'back' | 'left' | 'right' | 'top' | 'bottom' | 'iso') => void;
}

interface Props {
  scsUrl: string;
  fileName: string;
}

const VIEW_MAP: Record<string, number> = {
  front: 4,
  back: 5,
  left: 2,
  right: 3,
  top: 0,
  bottom: 1,
  iso: 6,
};

// OperatorId values (from @ts3d-hoops/web-viewer types)
const OP_ORBIT = 2;
const OP_TURNTABLE = 9;

/** Configure viewer for 2D drawing mode after model loads. */
function setup2DDrawingMode(viewer: WebViewer) {
  try {
    // Orthographic projection (no perspective distortion)
    viewer.view.setProjectionMode(1); // Projection.Orthographic = 1
  } catch { /* ignore if unsupported */ }

  try {
    // Remove 3D orbit/turntable operators — keep only pan & zoom for 2D
    const ops = viewer.operatorManager;
    ops.remove(OP_ORBIT);
    ops.remove(OP_TURNTABLE);
  } catch { /* ignore */ }

  try {
    // Enable white background sheet (paper)
    const sm = viewer.sheetManager;
    sm.setBackgroundSheetEnabled(true).catch(() => {});
  } catch { /* ignore if sheetManager unavailable */ }
}

const HoopsViewer = forwardRef<HoopsViewerHandle, Props>(({ scsUrl, fileName }, ref) => {
  const containerRef = useRef<HTMLDivElement | null>(null);
  const viewerRef = useRef<WebViewer | null>(null);
  const [status, setStatus] = useState<'loading' | 'ready' | 'error'>('loading');
  const [errorMsg, setErrorMsg] = useState('');

  useImperativeHandle(ref, () => ({
    fitWorld: () => viewerRef.current?.fitWorld(),
    reset: () => viewerRef.current?.reset(),
    setView: (preset) => {
      const viewer = viewerRef.current;
      if (!viewer) return;
      const orientation = VIEW_MAP[preset];
      if (orientation !== undefined) viewer.setViewOrientation(orientation);
    },
  }));

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    setStatus('loading');
    setErrorMsg('');

    const viewer = new WebViewer({
      container,
      endpointUri: scsUrl,
      rendererType: 0,
      streamingMode: 1,
    });
    viewerRef.current = viewer;

    viewer.setCallbacks({
      sceneReady: () => setStatus('ready'),
      firstModelLoaded: () => {
        setup2DDrawingMode(viewer);
        viewer.fitWorld();
      },
      modelSwitched: () => {
        setup2DDrawingMode(viewer);
        viewer.fitWorld();
      },
    });

    try {
      viewer.start();
    } catch (err) {
      setStatus('error');
      setErrorMsg((err as Error).message || 'HOOPS 启动失败');
    }

    const onResize = () => viewer.resizeCanvas();
    window.addEventListener('resize', onResize);

    return () => {
      window.removeEventListener('resize', onResize);
      viewer.shutdown();
      viewerRef.current = null;
    };
  }, [scsUrl]);

  return (
    <div style={{ position: 'relative', width: '100%', height: '100%', overflow: 'hidden' }}>
      <div
        ref={containerRef}
        style={{
          width: '100%',
          height: '100%',
          outline: 'none',
          opacity: status === 'ready' ? 1 : 0.3,
        }}
        tabIndex={0}
      />
      {status === 'loading' && (
        <div
          style={{
            position: 'absolute',
            inset: 0,
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
            background: 'rgba(18,18,35,0.85)',
            color: 'rgba(255,255,255,0.72)',
            zIndex: 10,
          }}
        >
          <div style={{ fontSize: 14, marginBottom: 8 }}>HOOPS 加载中...</div>
          <div style={{ fontSize: 12, opacity: 0.7 }}>{fileName}</div>
        </div>
      )}
      {status === 'error' && (
        <div
          style={{
            position: 'absolute',
            inset: 0,
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
            background: 'rgba(18,18,35,0.9)',
            color: '#ff6b6b',
            zIndex: 10,
            padding: 24,
            textAlign: 'center',
          }}
        >
          <div style={{ fontSize: 14, marginBottom: 8 }}>HOOPS 加载失败</div>
          <div style={{ fontSize: 12, opacity: 0.8, maxWidth: 300 }}>{errorMsg}</div>
        </div>
      )}
    </div>
  );
});

HoopsViewer.displayName = 'HoopsViewer';
export default HoopsViewer;
