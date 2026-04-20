import { Button, Tooltip, Upload, Dropdown } from 'antd';
import type { MenuProps } from 'antd';
import {
  CompressOutlined, UndoOutlined, AppstoreOutlined,
  ColumnWidthOutlined, BorderOutlined, ClearOutlined,
  FolderOpenOutlined, SyncOutlined, ClockCircleOutlined,
} from '@ant-design/icons';
import type { RecentFile } from '../../app/types';

interface Props {
  onFit: () => void;
  onReset: () => void;
  onToggleLayers: () => void;
  measureMode: 'dist' | 'area' | null;
  onMeasureDist: () => void;
  onMeasureArea: () => void;
  onMeasureClear: () => void;
  onOpenFile: (file: File, forceReparse?: boolean) => void;
  onReparse: () => void;
  recentFiles: RecentFile[];
  onOpenRecent: (recent: RecentFile) => void;
}

export default function Toolbar({
  onFit, onReset, onToggleLayers,
  measureMode, onMeasureDist, onMeasureArea, onMeasureClear,
  onOpenFile, onReparse, recentFiles, onOpenRecent,
}: Props) {
  const handleUpload = (file: File) => { onOpenFile(file); return false; };

  const recentItems: MenuProps['items'] = recentFiles.slice(0, 8).map((r) => ({
    key: r.cacheKey,
    label: (
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 12, minWidth: 200 }}>
        <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{r.name}</span>
        <span style={{ color: 'rgba(255,255,255,0.3)', fontSize: 11, flexShrink: 0 }}>
          {r.entityCount} entities
        </span>
      </div>
    ),
  }));

  const handleRecentClick: MenuProps['onClick'] = ({ key }) => {
    const r = recentFiles.find((f) => f.cacheKey === key);
    if (r) onOpenRecent(r);
  };

  return (
    <div style={{
      position: 'absolute', top: 12, left: 12, zIndex: 10,
      display: 'flex', gap: 8, alignItems: 'center',
      background: 'rgba(0,0,0,0.45)', padding: '6px 12px', borderRadius: 8,
    }}>
      <Upload accept=".dwg,.dxf,.json,.json.gz" showUploadList={false} beforeUpload={handleUpload}>
        <Tooltip title="打开文件"><Button type="text" icon={<FolderOpenOutlined />} size="small" /></Tooltip>
      </Upload>
      <Tooltip title="重新解析">
        <Button type="text" icon={<SyncOutlined />} size="small" onClick={onReparse} />
      </Tooltip>
      {recentFiles.length > 0 && (
        <Dropdown menu={{ items: recentItems, onClick: handleRecentClick }} trigger={['click']}>
          <Tooltip title="最近文件">
            <Button type="text" icon={<ClockCircleOutlined />} size="small" />
          </Tooltip>
        </Dropdown>
      )}
      <span style={{ color: 'rgba(255,255,255,0.15)' }}>|</span>
      <Tooltip title="适应窗口"><Button type="text" icon={<CompressOutlined />} size="small" onClick={onFit} /></Tooltip>
      <Tooltip title="重置"><Button type="text" icon={<UndoOutlined />} size="small" onClick={onReset} /></Tooltip>
      <Tooltip title="图层"><Button type="text" icon={<AppstoreOutlined />} size="small" onClick={onToggleLayers} /></Tooltip>
      <span style={{ color: 'rgba(255,255,255,0.15)' }}>|</span>
      <Tooltip title="测距">
        <Button type="text" icon={<ColumnWidthOutlined />} size="small"
          style={measureMode === 'dist' ? { color: '#00ff88' } : {}}
          onClick={onMeasureDist}
        />
      </Tooltip>
      <Tooltip title="测面">
        <Button type="text" icon={<BorderOutlined />} size="small"
          style={measureMode === 'area' ? { color: '#00ff88' } : {}}
          onClick={onMeasureArea}
        />
      </Tooltip>
      <Tooltip title="清除测量"><Button type="text" icon={<ClearOutlined />} size="small" onClick={onMeasureClear} /></Tooltip>
    </div>
  );
}
