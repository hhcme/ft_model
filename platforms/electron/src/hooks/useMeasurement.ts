import { useCallback, useState } from 'react';
import type { Measurement, MeasurePoint } from '../app/types';

export interface MeasurementTool {
  mode: 'dist' | 'area' | null;
  points: MeasurePoint[];
  preview: MeasurePoint | null;
  measurements: Measurement[];
  setMode: (mode: 'dist' | 'area' | null) => void;
  addPoint: (wx: number, wy: number) => void;
  finishArea: () => void;
  setPreview: (wx: number, wy: number) => void;
  clearPreview: () => void;
  clearAll: () => void;
}

export function useMeasurement(): MeasurementTool {
  const [mode, setMode] = useState<'dist' | 'area' | null>(null);
  const [points, setPoints] = useState<MeasurePoint[]>([]);
  const [preview, setPreviewState] = useState<MeasurePoint | null>(null);
  const [measurements, setMeasurements] = useState<Measurement[]>([]);

  const setModeWrap = useCallback((m: 'dist' | 'area' | null) => {
    setMode(m);
    setPoints([]);
    setPreviewState(null);
  }, []);

  const addPoint = useCallback((wx: number, wy: number) => {
    const pt: MeasurePoint = [wx, wy];
    setPoints((prev) => {
      const next = [...prev, pt];
      // Distance: auto-complete on 2 points
      if (mode === 'dist' && next.length === 2) {
        const dx = next[1][0] - next[0][0];
        const dy = next[1][1] - next[0][1];
        const dist = Math.sqrt(dx * dx + dy * dy);
        const pts: MeasurePoint[] = [next[0], next[1]];
        setMeasurements((ms) => [...ms, { type: 'dist', points: pts, value: dist }]);
        setMode(null);
        return [];
      }
      return next;
    });
  }, [mode]);

  const finishArea = useCallback(() => {
    if (mode !== 'area' || points.length < 3) return;
    let area = 0;
    for (let i = 0; i < points.length; i++) {
      const j = (i + 1) % points.length;
      area += points[i][0] * points[j][1];
      area -= points[j][0] * points[i][1];
    }
    area = Math.abs(area) / 2;
    setMeasurements((ms) => [...ms, { type: 'area', points: [...points], value: area }]);
    setPoints([]);
    setMode(null);
  }, [mode, points]);

  const setPreview = useCallback((wx: number, wy: number) => {
    setPreviewState([wx, wy]);
  }, []);

  const clearPreview = useCallback(() => {
    setPreviewState(null);
  }, []);

  const clearAll = useCallback(() => {
    setMeasurements([]);
    setPoints([]);
    setMode(null);
    setPreviewState(null);
  }, []);

  return {
    mode, points, preview, measurements,
    setMode: setModeWrap, addPoint, finishArea,
    setPreview, clearPreview, clearAll,
  };
}
