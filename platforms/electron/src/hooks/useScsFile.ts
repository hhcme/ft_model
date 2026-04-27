import { useState, useEffect } from 'react';

interface ScsStatus {
  scsUrl: string;
  scsExists: boolean | null;
  checking: boolean;
}

export function useScsFile(fileName: string): ScsStatus {
  const scsFileName = fileName.replace(/\.(dwg|dxf)$/i, '.scs');
  const scsUrl = `/scs/${scsFileName}`;

  const [scsExists, setScsExists] = useState<boolean | null>(null);
  const [checking, setChecking] = useState(true);

  useEffect(() => {
    if (!fileName) {
      setScsExists(false);
      setChecking(false);
      return;
    }

    let cancelled = false;
    setChecking(true);
    setScsExists(null);

    fetch(scsUrl, { method: 'HEAD' })
      .then((res) => {
        if (!cancelled) {
          setScsExists(res.ok);
          setChecking(false);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setScsExists(false);
          setChecking(false);
        }
      });

    return () => { cancelled = true; };
  }, [scsUrl, fileName]);

  return { scsUrl, scsExists, checking };
}
