import { Space, Typography } from 'antd';

interface Props {
  fileName: string;
  entityCount: number;
  vertexCount: number;
  mouseWorld: { x: number; y: number } | null;
}

export default function StatusBar({ fileName, entityCount, vertexCount, mouseWorld }: Props) {
  return (
    <div style={{
      position: 'absolute', bottom: 0, left: 0, right: 0, zIndex: 10,
      display: 'flex', alignItems: 'center', gap: 16,
      background: 'rgba(0,0,0,0.5)', padding: '4px 16px', fontSize: 12,
      color: 'rgba(255,255,255,0.7)',
    }}>
      <span style={{ color: '#e0e0f0' }}>{fileName || 'No file'}</span>
      <span style={{ color: 'rgba(255,255,255,0.15)' }}>|</span>
      <span>{entityCount.toLocaleString()} entities</span>
      <span>{vertexCount.toLocaleString()} vertices</span>
      <span style={{ color: 'rgba(255,255,255,0.15)' }}>|</span>
      <span style={{ fontVariantNumeric: 'tabular-nums', minWidth: 180 }}>
        {mouseWorld
          ? `X: ${mouseWorld.x.toFixed(2)}  Y: ${mouseWorld.y.toFixed(2)}`
          : 'X: —  Y: —'}
      </span>
    </div>
  );
}
