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
  /** Current converter type ("official" or "simple") */
  converter: string | null;
  /** Force re-convert: delete existing SCS and trigger a new conversion */
  reconvert: () => Promise<void>;
}

export function useScsFile(fileName: string, _fileBlob?: Blob | null): ScsStatus {
  const scsFileName = fileName.replace(/\.(dwg|dxf)$/i, '.scs');
  const scsUrl = `/scs/${scsFileName}`;

  const [scsExists, setScsExists] = useState<boolean | null>(null);
  const [checking, setChecking] = useState(true);
  const [convertStatus, setConvertStatus] = useState<ScsConvertStatus | null>(null);
  const [convertError, setConvertError] = useState<string | null>(null);
  const [converter, setConverter] = useState<string | null>(null);
  const [reconvertKey, setReconvertKey] = useState(0);
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const clearPoll = useCallback(() => {
    if (pollRef.current) { clearInterval(pollRef.current); pollRef.current = null; }
  }, []);

  // Fetch converter config once
  useEffect(() => {
    fetch('/scs/converter-config')
      .then((r) => r.ok ? r.json() : null)
      .then((d) => { if (d?.converter) setConverter(d.converter); })
      .catch(() => {});
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

          if (data.converter) setConverter(data.converter);

          if (data.status === 'done') {
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
            const scsFile = data.scsFile as string;
            pollRef.current = setInterval(async () => {
              if (cancelled) { clearPoll(); return; }
              try {
                const pollRes = await fetch(
                  `/scs/convert-status?scsFile=${encodeURIComponent(scsFile)}`
                );
                const pollData = await pollRes.json();
                if (cancelled) return;

                if (pollData.converter) setConverter(pollData.converter);

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
  }, [scsUrl, fileName, clearPoll, reconvertKey]);

  const reconvert = useCallback(async () => {
    if (!fileName) return;
    setConvertStatus('converting');
    setConvertError(null);
    setScsExists(false);

    try {
      const convRes = await fetch('/scs/convert', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ filename: fileName, force: true }),
      });
      if (!convRes.ok) {
        setConvertStatus('error');
        setConvertError(`重新转换失败: HTTP ${convRes.status}`);
        return;
      }
      const data = await convRes.json();
      if (data.converter) setConverter(data.converter);

      if (data.status === 'done') {
        setConvertStatus('done');
        setScsExists(true);
        setReconvertKey((k) => k + 1);
        return;
      }

      if (data.status === 'error') {
        setConvertStatus('error');
        setConvertError(data.error || '转换失败');
        return;
      }

      if (data.status === 'converting' && data.scsFile) {
        const scsFile = data.scsFile as string;
        clearPoll();
        pollRef.current = setInterval(async () => {
          try {
            const pollRes = await fetch(
              `/scs/convert-status?scsFile=${encodeURIComponent(scsFile)}`
            );
            const pollData = await pollRes.json();
            if (pollData.converter) setConverter(pollData.converter);

            if (pollData.status === 'done') {
              clearPoll();
              setConvertStatus('done');
              setScsExists(true);
              setReconvertKey((k) => k + 1);
            } else if (pollData.status === 'error') {
              clearPoll();
              setConvertStatus('error');
              setConvertError(pollData.error || '转换失败');
            }
          } catch {
            // keep trying
          }
        }, 2000);
      }
    } catch (err: any) {
      setConvertStatus('error');
      setConvertError(err.message || '重新转换异常');
    }
  }, [fileName, clearPoll]);

  return { scsUrl, scsExists, checking, convertStatus, convertError, converter, reconvert };
}
