import { useCallback, useRef, useState } from 'react';
import type { DrawData, RecentFile } from '../app/types';
import { makeCacheKey, saveCached, getCached, addRecentFile, saveFileBlob, getFileBlob } from '../utils/cache';

export interface FileLoader {
  load: (file: File, forceReparse?: boolean) => Promise<DrawData>;
  loadFromCache: (cacheKey: string) => Promise<DrawData | null>;
  reparse: (cacheKey: string) => Promise<DrawData>;
  loading: boolean;
  fileName: string;
  error: string | null;
  elapsed: number;
  cancel: () => void;
}

export function useFileLoader(): FileLoader {
  const [loading, setLoading] = useState(false);
  const [fileName, setFileName] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [elapsed, setElapsed] = useState(0);
  const abortRef = useRef<AbortController | null>(null);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const cancel = useCallback(() => {
    abortRef.current?.abort();
    abortRef.current = null;
    if (timerRef.current) { clearInterval(timerRef.current); timerRef.current = null; }
    setLoading(false);
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
      name: file.name,
      size: file.size,
      timestamp: Date.now(),
      cacheKey: key,
      entityCount: data.entityCount ?? 0,
      totalVertices: data.totalVertices ?? 0,
    });
  }, []);

  const load = useCallback(async (file: File, forceReparse = false): Promise<DrawData> => {
    cancel();
    setLoading(true);
    setError(null);
    setFileName(file.name);

    const key = makeCacheKey(file);

    // Try cache first
    if (!forceReparse) {
      const cached = await getCached(key);
      if (cached) {
        saveFileBlob(key, file);
        addRecentFile({
          name: file.name, size: file.size, timestamp: Date.now(),
          cacheKey: key,
          entityCount: cached.entityCount ?? 0,
          totalVertices: cached.totalVertices ?? 0,
        });
        setLoading(false);
        return cached;
      }
    }

    // Parse from server
    startTimer();
    const ac = new AbortController();
    abortRef.current = ac;

    try {
      const name = file.name.toLowerCase();
      let data: DrawData;

      if (name.endsWith('.dwg') || name.endsWith('.dxf')) {
        data = await loadViaServer(file, ac.signal);
      } else if (name.endsWith('.gz')) {
        data = await loadGzLocal(file);
      } else if (name.endsWith('.json')) {
        data = await loadJsonLocal(file);
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

  return { load, loadFromCache, reparse, loading, fileName, error, elapsed, cancel };
}

async function loadViaServer(file: File | Blob, signal: AbortSignal): Promise<DrawData> {
  const name = file instanceof File ? file.name : '';
  const res = await fetch('/parse', {
    method: 'POST',
    body: file,
    headers: {
      'Content-Type': 'application/octet-stream',
      'X-Filename': name,
    },
    signal,
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(err || 'Server error ' + res.status);
  }
  return await res.json();
}

async function loadGzLocal(file: File): Promise<DrawData> {
  const text = await new Response(
    file.slice().stream().pipeThrough(new DecompressionStream('gzip')),
  ).text();
  return JSON.parse(text);
}

async function loadJsonLocal(file: File): Promise<DrawData> {
  const text = await file.text();
  return JSON.parse(text);
}
