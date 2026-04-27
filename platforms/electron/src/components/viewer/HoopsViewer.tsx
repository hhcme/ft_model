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
      firstModelLoaded: () => viewer.fitWorld(),
      modelSwitched: () => viewer.fitWorld(),
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
