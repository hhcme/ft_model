import { useState, useCallback, useEffect } from 'react';
import { ConfigProvider, theme, App as AntApp } from 'antd';
import themeConfig from './theme';
import type { DrawData, AppPhase, RecentFile } from './types';
import LandingPage from '../components/landing/LandingPage';
import ParsingOverlay from '../components/parsing/ParsingOverlay';
import CadViewer from '../components/viewer/CadViewer';
import { useFileLoader } from '../hooks/useFileLoader';
import { getRecentFiles, getLastCacheKey } from '../utils/cache';

function AppInner() {
  const [phase, setPhase] = useState<AppPhase>('landing');
  const [drawData, setDrawData] = useState<DrawData | null>(null);
  const [recentFiles, setRecentFiles] = useState<RecentFile[]>([]);
  const [lastFileKey, setLastFileKey] = useState<string | null>(null);
  const loader = useFileLoader();
  const { message } = AntApp.useApp();

  // Load recent files list
  useEffect(() => {
    setRecentFiles(getRecentFiles());
    setLastFileKey(getLastCacheKey());
  }, []);

  // Auto-restore last file from cache
  useEffect(() => {
    if (!lastFileKey) return;
    let cancelled = false;
    (async () => {
      const data = await loader.loadFromCache(lastFileKey);
      if (cancelled || !data) return;
      const recent = getRecentFiles().find((r) => r.cacheKey === lastFileKey);
      setDrawData(data);
      setPhase('viewer');
      message.info(`已恢复上次文件: ${recent?.name || 'unknown'}`);
    })();
    return () => { cancelled = true; };
  }, [lastFileKey]); // eslint-disable-line react-hooks/exhaustive-deps

  const refreshRecent = useCallback(() => {
    setRecentFiles(getRecentFiles());
  }, []);

  const handleFile = useCallback(async (file: File, forceReparse = false) => {
    setPhase('parsing');
    try {
      const data = await loader.load(file, forceReparse);
      setDrawData(data);
      setPhase('viewer');
      refreshRecent();
      if (!forceReparse) {
        message.success(`${file.name} — ${data.entityCount} entities, ${data.totalVertices} vertices`);
      }
    } catch (err: any) {
      if (err.name === 'AbortError') { setPhase('landing'); return; }
      setPhase('error');
      message.error(err.message || 'Parse failed');
    }
  }, [loader, message, refreshRecent]);

  const handleOpenRecent = useCallback(async (recent: RecentFile) => {
    setPhase('parsing');
    const data = await loader.loadFromCache(recent.cacheKey);
    if (data) {
      setDrawData(data);
      setPhase('viewer');
    } else {
      setPhase('landing');
      message.warning('缓存已失效，请重新选择文件');
    }
  }, [loader, message]);

  const handleReparse = useCallback(async () => {
    const key = getLastCacheKey();
    if (!key) return;
    setPhase('parsing');
    try {
      const data = await loader.reparse(key);
      setDrawData(data);
      setPhase('viewer');
      refreshRecent();
      message.success(`重新解析完成 — ${data.entityCount} entities`);
    } catch (err: any) {
      if (err.message === 'NO_FILE') {
        setPhase('viewer');
        message.warning('原始文件缓存不存在，请重新选择文件');
      } else if (err.name === 'AbortError') {
        setPhase('viewer');
      } else {
        setPhase('viewer');
        message.error(err.message || '重新解析失败');
      }
    }
  }, [loader, message, refreshRecent]);

  const handleCancel = useCallback(() => {
    loader.cancel();
    setPhase('landing');
  }, [loader]);

  const currentFileName = phase === 'viewer' && drawData
    ? getRecentFiles().find((r) => r.cacheKey === getLastCacheKey())?.name || ''
    : loader.fileName;

  return (
    <div style={{ width: '100%', height: '100%', position: 'relative' }}>
      {phase === 'landing' && (
        <LandingPage onFile={handleFile} recentFiles={recentFiles} onOpenRecent={handleOpenRecent} />
      )}
      {phase === 'parsing' && (
        <ParsingOverlay fileName={loader.fileName} elapsed={loader.elapsed} onCancel={handleCancel} />
      )}
      {phase === 'error' && (
        <ParsingOverlay fileName={loader.fileName} elapsed={loader.elapsed} onCancel={() => setPhase('landing')} />
      )}
      {phase === 'viewer' && drawData && (
        <CadViewer
          drawData={drawData}
          onOpenFile={handleFile}
          fileName={currentFileName}
          recentFiles={recentFiles}
          onOpenRecent={handleOpenRecent}
          onReparse={handleReparse}
        />
      )}
    </div>
  );
}

export default function App() {
  return (
    <ConfigProvider theme={{ ...themeConfig, algorithm: theme.darkAlgorithm }}>
      <AntApp style={{ width: '100%', height: '100%' }}>
        <AppInner />
      </AntApp>
    </ConfigProvider>
  );
}
