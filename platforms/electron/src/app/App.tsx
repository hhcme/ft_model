import { useState, useCallback, useEffect, useRef } from 'react';
import { ConfigProvider, theme, App as AntApp } from 'antd';
import themeConfig from './theme';
import type { DrawData, AppPhase, RecentFile, CompareResult } from './types';
import LandingPage from '../components/landing/LandingPage';
import ParsingOverlay from '../components/parsing/ParsingOverlay';
import CadViewer from '../components/viewer/CadViewer';
import CompareViewer from '../components/viewer/CompareViewer';
import { useFileLoader } from '../hooks/useFileLoader';
import { getRecentFiles, getLastCacheKey, addRecentFile, makeCacheKey, getFileBlob } from '../utils/cache';

declare global {
  interface Window {
    drawData?: DrawData;
  }
}

async function loadDrawDataFromUrl(url: string): Promise<DrawData> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`HTTP ${res.status}: ${url}`);
  const lower = url.toLowerCase().split('?')[0];
  if (lower.endsWith('.gz')) {
    const stream = res.body?.pipeThrough(new DecompressionStream('gzip'));
    if (!stream) throw new Error('Browser does not support gzip streaming');
    return JSON.parse(await new Response(stream).text()) as DrawData;
  }
  return res.json() as Promise<DrawData>;
}

function isCurrentReferenceCache(result: CompareResult | null): result is CompareResult {
  if (!result?.refPng) return false;
  const provider = result.referenceMeta?.provider || result.referenceMeta?.parserFramework?.provider;
  return provider === 'qcad-cli';
}

function AppInner() {
  const [phase, setPhase] = useState<AppPhase>('landing');
  const [drawData, setDrawData] = useState<DrawData | null>(null);
  const [compareResult, setCompareResult] = useState<CompareResult | null>(null);
  const [activeFileName, setActiveFileName] = useState('');
  const [recentFiles, setRecentFiles] = useState<RecentFile[]>([]);
  const [lastFileKey, setLastFileKey] = useState<string | null>(null);
  const compareRunIdRef = useRef(0);
  const referenceAbortRef = useRef<AbortController | null>(null);
  const loader = useFileLoader();
  const { message } = AntApp.useApp();

  // Load recent files list
  useEffect(() => {
    setRecentFiles(getRecentFiles());
    setLastFileKey(getLastCacheKey());
  }, []);

  // Auto-restore last file from cache
  useEffect(() => {
    if (new URLSearchParams(window.location.search).has('data')) return;
    if (!lastFileKey) return;
    let cancelled = false;
    (async () => {
      const data = await loader.loadFromCache(lastFileKey);
      if (cancelled || !data) return;
      const recent = getRecentFiles().find((r) => r.cacheKey === lastFileKey);
      const cachedCompare = await loader.loadCompareFromCache(lastFileKey);
      const usableCachedCompare = isCurrentReferenceCache(cachedCompare) ? cachedCompare : null;
      if (cancelled) return;
      const restoredCompare: CompareResult = usableCachedCompare
        ? {
            ...usableCachedCompare,
            ours: data,
            ourError: null,
            loading: { ours: false, reference: false },
            refInfo: {
              ...usableCachedCompare.refInfo,
              ourEntityCount: data.entityCount ?? usableCachedCompare.refInfo.ourEntityCount,
            },
          }
        : {
            ours: data,
            ourError: null,
            refPng: null,
            refError: null,
            entityCompare: null,
            visualCompare: null,
            loading: { ours: false, reference: false },
            errors: {},
            refInfo: {
              entityCount: 0,
              ourEntityCount: data.entityCount ?? 0,
              refEntityCount: 0,
              missing: 0,
              extra: 0,
              renderTimeMs: 0,
              ourRenderTimeMs: 0,
            },
          };
      setDrawData(data);
      setCompareResult(restoredCompare);
      setActiveFileName(recent?.name || '');
      setPhase('compare');
      message.info(`已恢复上次文件: ${recent?.name || 'unknown'}`);
      if (!usableCachedCompare && recent) {
        const blob = await getFileBlob(lastFileKey);
        if (!cancelled && blob) {
          const file = new File([blob], recent.name, { lastModified: recent.timestamp });
          setCompareResult((prev) => prev ? {
            ...prev,
            loading: { ...(prev.loading ?? {}), reference: true },
          } : prev);
          loader.loadCompareReference(file)
            .then(async (refResult) => {
              if (cancelled) return;
              await loader.saveCompareToCache(lastFileKey, refResult);
              setCompareResult((prev) => ({
                ...(prev ?? restoredCompare),
                ...refResult,
                ours: data,
                loading: { ...(prev?.loading ?? {}), reference: false },
                refInfo: {
                  ...restoredCompare.refInfo,
                  ...(prev?.refInfo ?? {}),
                  ...refResult.refInfo,
                },
              }));
            })
            .catch((err: any) => {
              if (cancelled) return;
              setCompareResult((prev) => prev ? {
                ...prev,
                refError: err.message || String(err),
                loading: { ...(prev.loading ?? {}), reference: false },
                errors: { ...(prev.errors ?? {}), reference: err.message || String(err) },
              } : prev);
            });
        }
      }
    })();
    return () => { cancelled = true; };
  }, [lastFileKey]); // eslint-disable-line react-hooks/exhaustive-deps

  // Test/automation entrypoint: /?data=<json-or-json.gz-url>
  useEffect(() => {
    const dataUrl = new URLSearchParams(window.location.search).get('data');
    if (!dataUrl) return;
    let cancelled = false;
    setPhase('parsing');
    loader.cancel();
    (async () => {
      try {
        const data = await loadDrawDataFromUrl(dataUrl);
        if (cancelled) return;
        setDrawData(data);
        setPhase('viewer');
        message.success(`已加载预览数据 — ${data.entityCount ?? 0} entities`);
      } catch (err: any) {
        if (cancelled) return;
        setPhase('error');
        message.error(err.message || '加载预览数据失败');
      }
    })();
    return () => { cancelled = true; };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (drawData) window.drawData = drawData;
  }, [drawData]);

  const refreshRecent = useCallback(() => {
    setRecentFiles(getRecentFiles());
  }, []);

  const handleFile = useCallback(async (file: File, mode: 'view' | 'compare' = 'compare', forceReparse = false) => {
    referenceAbortRef.current?.abort();
    const runId = ++compareRunIdRef.current;
    setActiveFileName(file.name);
    try {
      if (mode === 'compare') {
        const started = Date.now();
        const cacheKey = makeCacheKey(file);
        const baseResult: CompareResult = {
          ours: null,
          ourError: null,
          refPng: null,
          refError: null,
          entityCompare: null,
          visualCompare: null,
          loading: { ours: true, reference: true },
          errors: {},
          refInfo: {
            entityCount: 0,
            ourEntityCount: 0,
            refEntityCount: 0,
            missing: 0,
            extra: 0,
            renderTimeMs: 0,
            ourRenderTimeMs: 0,
          },
        };
        setCompareResult(baseResult);
        setPhase('compare');

        loader.load(file, forceReparse)
          .then((data) => {
            if (compareRunIdRef.current !== runId) return;
            setDrawData(data);
            const elapsed = Date.now() - started;
            setCompareResult((prev) => ({
              ...(prev ?? baseResult),
              ours: data,
              ourError: null,
              loading: { ...(prev?.loading ?? {}), ours: false },
              refInfo: {
                ...(prev?.refInfo ?? baseResult.refInfo),
                ourEntityCount: data.entityCount ?? 0,
                ourRenderTimeMs: elapsed,
              },
            }));
            message.success(`自研解析完成 — ${data.entityCount} entities`);
          })
          .catch((err: any) => {
            if (err.name === 'AbortError' || compareRunIdRef.current !== runId) return;
            setCompareResult((prev) => ({
              ...(prev ?? baseResult),
              ourError: err.message || String(err),
              loading: { ...(prev?.loading ?? {}), ours: false },
              errors: {
                ...(prev?.errors ?? {}),
                ourParser: err.message || String(err),
              },
            }));
            message.error(err.message || 'Parse failed');
          });

        const refAbort = new AbortController();
        referenceAbortRef.current = refAbort;
        loader.loadCompareFromCache(cacheKey)
          .then((cached) => {
            if (isCurrentReferenceCache(cached) && compareRunIdRef.current === runId) {
              setCompareResult((prev) => ({
                ...(prev ?? baseResult),
                ...cached,
                ours: prev?.ours ?? null,
                ourError: prev?.ourError ?? null,
                loading: { ...(prev?.loading ?? {}), reference: false },
                refInfo: {
                  ...baseResult.refInfo,
                  ...(prev?.refInfo ?? {}),
                  ...cached.refInfo,
                },
              }));
              return cached;
            }
            return loader.loadCompareReference(file, refAbort.signal)
              .then(async (refResult) => {
                await loader.saveCompareToCache(cacheKey, refResult);
                return refResult;
              });
          })
          .then((refResult) => {
            if (compareRunIdRef.current !== runId) return;
            setCompareResult((prev) => ({
              ...(prev ?? baseResult),
              ...refResult,
              ours: prev?.ours ?? null,
              ourError: prev?.ourError ?? null,
              loading: { ...(prev?.loading ?? {}), reference: false },
              refInfo: {
                ...baseResult.refInfo,
                ...(prev?.refInfo ?? {}),
                ...refResult.refInfo,
              },
            }));
            const entityStatus = refResult.entityCompare?.status ? `实体${refResult.entityCompare.status}` : '实体—';
            const refOk = refResult.refPng ? '参考渲染成功' : '参考渲染失败';
            message.info(`第三方对比完成 — ${refOk}, ${entityStatus}`);
          })
          .catch((err: any) => {
            if (err.name === 'AbortError' || compareRunIdRef.current !== runId) return;
            setCompareResult((prev) => ({
              ...(prev ?? baseResult),
              ours: prev?.ours ?? null,
              refError: err.message || String(err),
              loading: { ...(prev?.loading ?? {}), reference: false },
              errors: {
                ...(prev?.errors ?? {}),
                reference: err.message || String(err),
              },
            }));
            message.warning(`第三方参考失败：${err.message || String(err)}`);
          });
      } else {
        setPhase('parsing');
        const data = await loader.load(file, forceReparse);
        setDrawData(data);
        setPhase('viewer');
        refreshRecent();
        if (!forceReparse) {
          message.success(`${file.name} — ${data.entityCount} entities, ${data.totalVertices} vertices`);
        }
      }
    } catch (err: any) {
      if (err.name === 'AbortError') { setPhase('landing'); return; }
      if (mode !== 'compare') setPhase('error');
      message.error(err.message || 'Parse failed');
    }
  }, [loader, message, refreshRecent]);

  const handleOpenRecent = useCallback(async (recent: RecentFile) => {
    setPhase('parsing');
    const data = await loader.loadFromCache(recent.cacheKey);
    if (data) {
      addRecentFile({ ...recent, timestamp: Date.now() });
      setDrawData(data);
      setPhase('viewer');
      refreshRecent();
    } else {
      setPhase('landing');
      message.warning('缓存已失效，请重新选择文件');
    }
  }, [loader, message, refreshRecent]);

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
    referenceAbortRef.current?.abort();
    ++compareRunIdRef.current;
    loader.cancel();
    setPhase('landing');
  }, [loader]);

  const currentFileName = phase === 'viewer' && drawData
    ? getRecentFiles().find((r) => r.cacheKey === getLastCacheKey())?.name || ''
    : activeFileName || loader.fileName;

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
          onOpenFile={(f, forceReparse) => handleFile(f, 'compare', forceReparse)}
          fileName={currentFileName}
          recentFiles={recentFiles}
          onOpenRecent={handleOpenRecent}
          onReparse={handleReparse}
        />
      )}
      {phase === 'compare' && compareResult && (
        <CompareViewer
          ours={compareResult.ours}
          ourError={compareResult.ourError}
          refPng={compareResult.refPng}
          refError={compareResult.refError}
          refInfo={compareResult.refInfo}
          entityCompare={compareResult.entityCompare}
          visualCompare={compareResult.visualCompare}
          errors={compareResult.errors}
          loading={compareResult.loading}
          referenceMeta={compareResult.referenceMeta}
          fileName={activeFileName || loader.fileName}
          onOpenFile={(f) => handleFile(f, 'compare')}
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
