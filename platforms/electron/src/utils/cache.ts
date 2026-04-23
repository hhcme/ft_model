import type { CompareResult, DrawData, RecentFile } from '../app/types';

const DB_NAME = 'cad-preview-cache';
const STORE = 'drawings';
const RECENT_KEY = 'cad-recent-files';
const LAST_KEY = 'cad-last-file';
const CACHE_SCHEMA_VERSION = 'dwg-fidelity-0.8.10';

function openDB(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

export function makeCacheKey(file: File): string {
  return `${CACHE_SCHEMA_VERSION}:${file.name}:${file.size}:${file.lastModified}`;
}

export async function saveCached(key: string, data: DrawData): Promise<void> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(data, key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

export async function getCached(key: string): Promise<DrawData | null> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readonly');
    const req = tx.objectStore(STORE).get(key);
    req.onsuccess = () => resolve(req.result ?? null);
    req.onerror = () => reject(req.error);
  });
}

export async function deleteCached(key: string): Promise<void> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).delete(key);
    tx.objectStore(STORE).delete(key + ':file');
    tx.objectStore(STORE).delete(key + ':compare');
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

export async function saveCompareCached(key: string, data: CompareResult): Promise<void> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(data, key + ':compare');
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

export async function getCompareCached(key: string): Promise<CompareResult | null> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readonly');
    const req = tx.objectStore(STORE).get(key + ':compare');
    req.onsuccess = () => resolve(req.result ?? null);
    req.onerror = () => reject(req.error);
  });
}

export async function saveFileBlob(key: string, blob: Blob): Promise<void> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(blob, key + ':file');
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

export async function getFileBlob(key: string): Promise<Blob | null> {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readonly');
    const req = tx.objectStore(STORE).get(key + ':file');
    req.onsuccess = () => resolve(req.result ?? null);
    req.onerror = () => reject(req.error);
  });
}

// --- Recent files (localStorage) ---

export function getRecentFiles(): RecentFile[] {
  try {
    const list = JSON.parse(localStorage.getItem(RECENT_KEY) || '[]') as RecentFile[];
    return list.filter((r) => r.cacheKey.startsWith(`${CACHE_SCHEMA_VERSION}:`));
  } catch { return []; }
}

export function addRecentFile(meta: RecentFile): void {
  const list = getRecentFiles().filter((r) => r.cacheKey !== meta.cacheKey);
  list.unshift(meta);
  if (list.length > 10) list.length = 10;
  localStorage.setItem(RECENT_KEY, JSON.stringify(list));
  localStorage.setItem(LAST_KEY, meta.cacheKey);
}

export function removeRecentFile(cacheKey: string): void {
  const list = getRecentFiles().filter((r) => r.cacheKey !== cacheKey);
  localStorage.setItem(RECENT_KEY, JSON.stringify(list));
  deleteCached(cacheKey);
  if (localStorage.getItem(LAST_KEY) === cacheKey) localStorage.removeItem(LAST_KEY);
}

export function getLastCacheKey(): string | null {
  const key = localStorage.getItem(LAST_KEY);
  return key && key.startsWith(`${CACHE_SCHEMA_VERSION}:`) ? key : null;
}
