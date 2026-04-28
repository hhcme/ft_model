import { useCallback, useEffect, useRef, useState } from 'react';
import type { DrawData, RecentFile, CompareResult } from '../app/types';
import { makeCacheKey, saveCached, getCached, addRecentFile, saveFileBlob, getFileBlob, saveCompareCached, getCompareCached } from '../utils/cache';

export interface FileLoader {
  load: (file: File, forceReparse?: boolean) => Promise<DrawData>;
  loadCompare: (file: File) => Promise<CompareResult>;
  loadCompareReference: (file: File, signal?: AbortSignal) => Promise<CompareResult>;
  loadCompareFromCache: (cacheKey: string) => Promise<CompareResult | null>;
  saveCompareToCache: (cacheKey: string, data: CompareResult) => Promise<void>;
  loadFromCache: (cacheKey: string) => Promise<DrawData | null>;
  reparse: (cacheKey: string) => Promise<DrawData>;
  /** Manifest data loaded first for large files (bounds, layers, views — no vertices) */
  manifest: DrawData | null;
  loading: boolean;
  fileName: string;
  error: string | null;
  elapsed: number;
  cancel: () => void;
}

/** Lazy singleton Web Worker for off-main-thread JSON parsing / gzip decompression. */
let workerInstance: Worker | null = null;
let workerIdCounter = 0;
const pendingParses = new Map<number, { resolve: (d: DrawData) => void; reject: (e: Error) => void }>();

function getWorker(): Worker {
  if (!workerInstance) {
    workerInstance = new Worker(new URL('../workers/parseWorker.ts', import.meta.url), { type: 'module' });
    workerInstance.onmessage = (e) => {
      const { id, data, error } = e.data;
      const p = pendingParses.get(id);
      if (!p) return;
      pendingParses.delete(id);
      if (error) p.reject(new Error(error));
      else p.resolve(data as DrawData);
    };
    workerInstance.onerror = (e) => {
      for (const [, p] of pendingParses) p.reject(new Error(e.message));
      pendingParses.clear();
    };
  }
  return workerInstance;
}

function parseWithWorker(buffer: ArrayBuffer, fileName: string): Promise<DrawData> {
  const id = ++workerIdCounter;
  return new Promise<DrawData>((resolve, reject) => {
    pendingParses.set(id, { resolve, reject });
    getWorker().postMessage({ id, buffer, fileName }, [buffer]);
  });
}

export function useFileLoader(): FileLoader {
  const [loading, setLoading] = useState(false);
  const [fileName, setFileName] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [elapsed, setElapsed] = useState(0);
  const [manifest, setManifest] = useState<DrawData | null>(null);
  const abortRef = useRef<AbortController | null>(null);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const cancel = useCallback(() => {
    abortRef.current?.abort();
    abortRef.current = null;
    if (timerRef.current) { clearInterval(timerRef.current); timerRef.current = null; }
    setLoading(false);
  }, []);

  useEffect(() => () => {
    if (timerRef.current) clearInterval(timerRef.current);
    workerInstance?.terminate();
    workerInstance = null;
  }, []);

  const startTimer = useCallback(() => {
    const start = Date.now();
    setElapsed(0);
    timerRef.current = setInterval(() => {
      setElapsed(Math.round((Date.now() - start) / 1000));
    }, 1000);
  }, []);

  const stopTimer = useCallback(() => {
    if (timerRef.current) { clearInterval(timerRef.current); timerRef.current = null; }
  }, []);

  const writeToCache = useCallback((file: File, data: DrawData) => {
    const key = makeCacheKey(file);
    saveCached(key, data);
    saveFileBlob(key, file);
    addRecentFile({
      name: file.name, size: file.size, timestamp: Date.now(), cacheKey: key,
      entityCount: data.entityCount ?? 0, totalVertices: data.totalVertices ?? 0,
    });
  }, []);

  const load = useCallback(async (file: File, forceReparse = false): Promise<DrawData> => {
    cancel();
    setLoading(true);
    setError(null);
    setFileName(file.name);

    const key = makeCacheKey(file);
    if (!forceReparse) {
      const cached = await getCached(key);
      if (cached) {
        saveFileBlob(key, file);
        addRecentFile({
          name: file.name, size: file.size, timestamp: Date.now(), cacheKey: key,
          entityCount: cached.entityCount ?? 0, totalVertices: cached.totalVertices ?? 0,
        });
        setLoading(false);
        return cached;
      }
    }

    startTimer();
    const ac = new AbortController();
    abortRef.current = ac;

    try {
      const name = file.name.toLowerCase();
      let data: DrawData;

      if (name.endsWith('.dwg') || name.endsWith('.dxf')) {
        // For large files (>2MB), load manifest first for instant bounds/views
        const LARGE_FILE_THRESHOLD = 2 * 1024 * 1024;
        if (file.size > LARGE_FILE_THRESHOLD) {
          try {
            const manifestData = await loadManifestViaServer(file, ac.signal);
            if (manifestData) {
              setManifest(manifestData);
            }
          } catch {
            // Manifest load failure is non-fatal — full load will follow
          }
        }
        data = await loadViaServer(file, ac.signal);
      } else if (name.endsWith('.gz') || name.endsWith('.json')) {
        // Offload gzip decompression + JSON.parse to Web Worker
        const buffer = await file.arrayBuffer();
        data = await parseWithWorker(buffer, file.name);
      } else {
        throw new Error('Unsupported file type: .' + name.split('.').pop());
      }

      stopTimer();
      setLoading(false);
      writeToCache(file, data);
      return data;
    } catch (err: any) {
      stopTimer();
      setLoading(false);
      if (err.name === 'AbortError') throw err;
      setError(err.message || String(err));
      throw err;
    }
  }, [cancel, startTimer, stopTimer, writeToCache]);

  const loadFromCache = useCallback(async (cacheKey: string): Promise<DrawData | null> => {
    return getCached(cacheKey);
  }, []);

  const reparse = useCallback(async (cacheKey: string): Promise<DrawData> => {
    const blob = await getFileBlob(cacheKey);
    if (!blob) throw new Error('NO_FILE');

    startTimer();
    const ac = new AbortController();
    abortRef.current = ac;

    try {
      const data = await loadViaServer(blob, ac.signal);
      stopTimer();
      setLoading(false);
      saveCached(cacheKey, data);
      return data;
    } catch (err: any) {
      stopTimer();
      setLoading(false);
      if (err.name === 'AbortError') throw err;
      throw err;
    }
  }, [startTimer, stopTimer]);

  const loadCompare = useCallback(async (file: File): Promise<CompareResult> => {
    cancel();
    setLoading(true);
    setError(null);
    setFileName(file.name);
    startTimer();
    const ac = new AbortController();
    abortRef.current = ac;

    try {
      const result = await loadCompareViaServer(file, ac.signal);
      stopTimer();
      setLoading(false);
      return result;
    } catch (err: any) {
      stopTimer();
      setLoading(false);
      if (err.name === 'AbortError') throw err;
      setError(err.message || String(err));
      throw err;
    }
  }, [cancel, startTimer, stopTimer]);

  const loadCompareReference = useCallback(async (file: File, signal?: AbortSignal): Promise<CompareResult> => {
    return loadCompareReferenceViaServer(file, signal);
  }, []);

  const loadCompareFromCache = useCallback(async (cacheKey: string): Promise<CompareResult | null> => {
    return getCompareCached(cacheKey);
  }, []);

  const saveCompareToCache = useCallback(async (cacheKey: string, data: CompareResult): Promise<void> => {
    return saveCompareCached(cacheKey, data);
  }, []);

  return {
    load, loadCompare, loadCompareReference, loadCompareFromCache, saveCompareToCache,
    loadFromCache, reparse, manifest, loading, fileName, error, elapsed, cancel,
  };
}

function uploadHeaders(fileName: string): HeadersInit {
  const asciiName = fileName
    ? fileName.replace(/[^\x20-\x7E]/g, '_')
    : 'upload.dwg';
  return {
    'Content-Type': 'application/octet-stream',
    'X-Filename': asciiName,
    'X-Filename-Encoded': encodeURIComponent(fileName || asciiName),
  };
}

/** Load manifest-only data (bounds, layers, views — no vertex data) for fast initial display. */
async function loadManifestViaServer(file: File | Blob, signal: AbortSignal): Promise<DrawData | null> {
  const name = file instanceof File ? file.name : '';
  try {
    const res = await fetch('/parse?manifest=1', {
      method: 'POST',
      body: file,
      headers: uploadHeaders(name),
      signal,
    });
    if (!res.ok) return null;
    const buffer = await res.arrayBuffer();
    return parseWithWorker(buffer, name || 'manifest.json');
  } catch {
    return null;
  }
}

async function loadViaServer(file: File | Blob, signal: AbortSignal): Promise<DrawData> {
  const name = file instanceof File ? file.name : '';
  const res = await fetch('/parse', {
    method: 'POST',
    body: file,
    headers: uploadHeaders(name),
    signal,
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(err || 'Server error ' + res.status);
  }
  // The browser's fetch() auto-decompresses Content-Encoding: gzip responses,
  // so res.arrayBuffer() already returns plain JSON bytes. Pass a .json name
  // to the Worker so it skips decompression and goes straight to JSON.parse.
  const buffer = await res.arrayBuffer();
  return parseWithWorker(buffer, name || 'server.json');
}

async function loadCompareViaServer(file: File, signal: AbortSignal): Promise<CompareResult> {
  const res = await fetch('/compare-render', {
    method: 'POST',
    body: file,
    headers: uploadHeaders(file.name),
    signal,
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(err || 'Server error ' + res.status);
  }
  return res.json();
}

async function loadCompareReferenceViaServer(file: File, signal?: AbortSignal): Promise<CompareResult> {
  const res = await fetch('/compare-reference', {
    method: 'POST',
    body: file,
    headers: uploadHeaders(file.name),
    signal,
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(err || 'Server error ' + res.status);
  }
  return res.json();
}
