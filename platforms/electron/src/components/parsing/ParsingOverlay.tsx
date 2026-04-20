import { Spin, Button, Typography, Space } from 'antd';

interface Props {
  fileName: string;
  elapsed: number;
  onCancel: () => void;
}

export default function ParsingOverlay({ fileName, elapsed, onCancel }: Props) {
  return (
    <div style={{
      position: 'absolute', inset: 0, zIndex: 50,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      background: 'rgba(18,18,35,0.95)',
    }}>
      <Space direction="vertical" size={20} align="center">
        <Spin size="large" />
        <Typography.Text style={{ color: '#e0e0f0', fontSize: 16 }}>
          正在解析 {fileName}
        </Typography.Text>
        <Typography.Text style={{ color: 'rgba(255,255,255,0.45)', fontVariantNumeric: 'tabular-nums' }}>
          已用时 {elapsed}s
        </Typography.Text>
        <Button danger type="text" onClick={onCancel}>取消</Button>
      </Space>
    </div>
  );
}
