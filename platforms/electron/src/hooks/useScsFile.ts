import { useState, useEffect, useRef, useCallback } from 'react';

export type ScsConvertStatus = 'checking' | 'converting' | 'done' | 'error';

interface ScsStatus {
  scsUrl: string;
  scsExists: boolean | null;
  checking: boolean;
  /** Non-null when conversion is in progress or has a result */
  convertStatus: ScsConvertStatus | null;
  /** Error message if conversion failed */
  convertError: string | null;
}

export function useScsFile(fileName: string, _fileBlob?: Blob | null): ScsStatus {
  const scsFileName = fileName.replace(/\.(dwg|dxf)$/i, '.scs');
  const scsUrl = `/scs/${scsFileName}`;

  const [scsExists, setScsExists] = useState<boolean | null>(null);
  const [checking, setChecking] = useState(true);
  const [convertStatus, setConvertStatus] = useState<ScsConvertStatus | null>(null);
  const [convertError, setConvertError] = useState<string | null>(null);
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const clearPoll = useCallback(() => {
    if (pollRef.current) { clearInterval(pollRef.current); pollRef.current = null; }
  }, []);

  useEffect(() => {
    clearPoll();
    if (!fileName) {
      setScsExists(false);
      setChecking(false);
      setConvertStatus(null);
      setConvertError(null);
      return;
    }

    let cancelled = false;
    setChecking(true);
    setScsExists(null);
    setConvertStatus(null);
    setConvertError(null);

    const isDwg = /\.dwg$/i.test(fileName);

    // Step 1: HEAD check for existing SCS
    fetch(scsUrl, { method: 'HEAD' })
      .then(async (res) => {
        if (cancelled) return;
        if (res.ok) {
          setScsExists(true);
          setChecking(false);
          return;
        }

        // SCS not found. If DWG, attempt auto-conversion.
        if (!isDwg) {
          setScsExists(false);
          setChecking(false);
          return;
        }

        // Step 2: Trigger conversion
        setConvertStatus('converting');
        setChecking(false);

        try {
          const convRes = await fetch('/scs/convert', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ filename: fileName }),
          });
          if (!convRes.ok) {
            if (!cancelled) {
              setConvertStatus('error');
              setConvertError(`转换请求失败: HTTP ${convRes.status}`);
              setScsExists(false);
            }
            return;
          }
          const data = await convRes.json();
          if (cancelled) return;

          if (data.status === 'done') {
            // Already converted (race condition or was just completed)
            setConvertStatus('done');
            setScsExists(true);
            return;
          }

          if (data.status === 'error') {
            setConvertStatus('error');
            setConvertError(data.error || '转换失败');
            setScsExists(false);
            return;
          }

          if (data.status === 'converting' && data.scsFile) {
            // Start polling
            const scsFile = data.scsFile as string;
            pollRef.current = setInterval(async () => {
              if (cancelled) { clearPoll(); return; }
              try {
                const pollRes = await fetch(
                  `/scs/convert-status?scsFile=${encodeURIComponent(scsFile)}`
                );
                const pollData = await pollRes.json();
                if (cancelled) return;

                if (pollData.status === 'done') {
                  clearPoll();
                  setConvertStatus('done');
                  setScsExists(true);
                } else if (pollData.status === 'error') {
                  clearPoll();
                  setConvertStatus('error');
                  setConvertError(pollData.error || '转换失败');
                  setScsExists(false);
                }
                // else still "converting" — keep polling
              } catch {
                // Network error — keep trying
              }
            }, 2000);
          }
        } catch (err: any) {
          if (!cancelled) {
            setConvertStatus('error');
            setConvertError(err.message || '转换请求异常');
            setScsExists(false);
          }
        }
      })
      .catch(() => {
        if (!cancelled) {
          setScsExists(false);
          setChecking(false);
        }
      });

    return () => { cancelled = true; clearPoll(); };
  }, [scsUrl, fileName, clearPoll]);

  return { scsUrl, scsExists, checking, convertStatus, convertError };
}
