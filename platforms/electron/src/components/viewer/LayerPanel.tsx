import { useState, useMemo, memo } from 'react';
import { Drawer, Input, List, Checkbox, Button, Space } from 'antd';

interface LayerItem {
  name: string;
  color: [number, number, number];
  frozen: boolean;
  off?: boolean;
  locked: boolean;
  plotEnabled?: boolean;
  lineweight?: number;
}

interface Props {
  open: boolean;
  onClose: () => void;
  layers: LayerItem[];
  visibleMap: Map<string, boolean>;
  onToggle: (name: string, visible: boolean) => void;
  onShowAll: () => void;
  onHideAll: () => void;
  onInvert: () => void;
}

function mapsEqual(a: Map<string, boolean>, b: Map<string, boolean>): boolean {
  if (a === b) return true;
  if (a.size !== b.size) return false;
  for (const [k, v] of a) { if (b.get(k) !== v) return false; }
  return true;
}

export default memo(function LayerPanel({
  open, onClose, layers, visibleMap, onToggle,
  onShowAll, onHideAll, onInvert,
}: Props) {
  const [search, setSearch] = useState('');

  const filtered = useMemo(() => {
    if (!search) return layers;
    const q = search.toLowerCase();
    return layers.filter((l) => l.name.toLowerCase().includes(q));
  }, [layers, search]);

  return (
    <Drawer
      title={`图层 (${layers.length})`}
      placement="left"
      open={open}
      onClose={onClose}
      width={300}
      styles={{
        header: { background: '#1e1e38', borderBottom: '1px solid rgba(255,255,255,0.1)' },
        body: { background: '#1a1a2e', padding: 0, display: 'flex', flexDirection: 'column' },
      }}
    >
      <div style={{ padding: '8px 12px', borderBottom: '1px solid rgba(255,255,255,0.08)' }}>
        <Space style={{ width: '100%', justifyContent: 'space-between' }}>
          <Button size="small" onClick={onShowAll}>全部显示</Button>
          <Button size="small" onClick={onHideAll}>全部隐藏</Button>
          <Button size="small" onClick={onInvert}>反转</Button>
        </Space>
      </div>
      <div style={{ padding: '4px 12px' }}>
        <Input.Search placeholder="Filter layers..." size="small" value={search} onChange={(e) => setSearch(e.target.value)} />
      </div>
      <div style={{ flex: 1, overflow: 'auto' }}>
        <List
          size="small"
          dataSource={filtered}
          style={{ background: 'transparent' }}
          renderItem={(layer) => {
            const visible = visibleMap.get(layer.name) ?? true;
            const [r, g, b] = layer.color;
            return (
              <List.Item style={{ padding: '4px 12px', border: 'none' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 8, width: '100%' }}>
                  <Checkbox checked={visible} onChange={(e) => onToggle(layer.name, e.target.checked)} />
                  <span style={{
                    width: 12, height: 12, borderRadius: 2,
                    background: `rgb(${r},${g},${b})`, flexShrink: 0,
                  }} />
                  <span style={{
                    color: visible ? '#e0e0f0' : 'rgba(255,255,255,0.3)',
                    overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                  }}>
                    {layer.name}
                  </span>
                  {(layer.frozen || layer.off || layer.locked || layer.plotEnabled === false) && (
                    <span style={{ marginLeft: 'auto', color: 'rgba(255,255,255,0.45)', fontSize: 11 }}>
                      {layer.off ? 'OFF' : layer.frozen ? 'FRZ' : layer.locked ? 'LOCK' : 'NOPLOT'}
                    </span>
                  )}
                </div>
              </List.Item>
            );
          }}
        />
      </div>
    </Drawer>
  );
}, (prev, next) =>
  prev.open === next.open && prev.layers === next.layers &&
  mapsEqual(prev.visibleMap, next.visibleMap) &&
  prev.onToggle === next.onToggle && prev.onShowAll === next.onShowAll &&
  prev.onHideAll === next.onHideAll && prev.onInvert === next.onInvert && prev.onClose === next.onClose,
)
