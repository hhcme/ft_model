import { Upload, Typography, Tag, Space, List } from 'antd';
import { FileImageOutlined } from '@ant-design/icons';
import type { RecentFile } from '../../app/types';

interface Props {
  onFile: (file: File) => void;
  recentFiles: RecentFile[];
  onOpenRecent: (recent: RecentFile) => void;
}

const ACCEPT = '.dwg,.dxf,.json,.json.gz';

function formatTime(ts: number): string {
  const d = new Date(ts);
  const now = new Date();
  if (d.toDateString() === now.toDateString()) {
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }
  return d.toLocaleDateString([], { month: 'short', day: 'numeric' });
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / 1048576).toFixed(1) + ' MB';
}

export default function LandingPage({ onFile, recentFiles, onOpenRecent }: Props) {
  const handleUpload = (file: File) => {
    onFile(file);
    return false;
  };

  return (
    <div style={{
      width: '100%', height: '100%',
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      background: '#121223',
    }}>
      <Space direction="vertical" size={20} align="center">
        <FileImageOutlined style={{ fontSize: 64, color: 'rgba(255,255,255,0.25)' }} />
        <Typography.Title level={3} style={{ color: '#e0e0f0', margin: 0 }}>
          CAD Engine Preview
        </Typography.Title>
        <Typography.Text style={{ color: 'rgba(255,255,255,0.45)' }}>
          将 DWG、DXF 或 JSON 文件拖拽到此处，或点击上传
        </Typography.Text>
        <Upload.Dragger
          accept={ACCEPT}
          showUploadList={false}
          beforeUpload={(f) => handleUpload(f as File)}
          multiple={false}
          style={{ background: 'rgba(255,255,255,0.03)', border: '1px dashed rgba(255,255,255,0.15)', width: 360 }}
        >
          <p style={{ color: 'rgba(255,255,255,0.5)', padding: '20px 0' }}>
            点击或拖拽文件到此区域
          </p>
        </Upload.Dragger>
        <Space>
          {['.DWG', '.DXF', '.JSON', '.GZ'].map((f) => (
            <Tag key={f} style={{ borderColor: 'rgba(255,255,255,0.1)', color: 'rgba(255,255,255,0.4)' }}>{f}</Tag>
          ))}
        </Space>
        {recentFiles.length > 0 && (
          <div style={{ width: 400, maxHeight: 260, overflowY: 'auto' }}>
            <Typography.Text style={{ color: 'rgba(255,255,255,0.35)', fontSize: 12 }}>
              最近文件
            </Typography.Text>
            <List
              size="small"
              dataSource={recentFiles.slice(0, 8)}
              renderItem={(item) => (
                <List.Item
                  style={{ cursor: 'pointer', padding: '6px 8px', borderRadius: 4 }}
                  onClick={() => onOpenRecent(item)}
                >
                  <List.Item.Meta
                    title={
                      <Typography.Text style={{ color: '#e0e0f0', fontSize: 13 }}>
                        {item.name}
                      </Typography.Text>
                    }
                    description={
                      <Typography.Text style={{ color: 'rgba(255,255,255,0.3)', fontSize: 11 }}>
                        {formatSize(item.size)} · {item.entityCount} entities · {formatTime(item.timestamp)}
                      </Typography.Text>
                    }
                  />
                </List.Item>
              )}
            />
          </div>
        )}
      </Space>
    </div>
  );
}
