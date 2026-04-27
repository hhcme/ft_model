import { useCallback, useRef, useEffect } from 'react';
import type { LayoutTab } from '../../hooks/useLayoutViews';

interface Props {
  tabs: LayoutTab[];
  activeViewId: string;
  onSwitch: (viewId: string) => void;
}

export default function LayoutTabBar({ tabs, activeViewId, onSwitch }: Props) {
  const barRef = useRef<HTMLDivElement>(null);

  // Scroll active tab into view
  useEffect(() => {
    if (!barRef.current) return;
    const active = barRef.current.querySelector('[data-active="true"]');
    active?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }, [activeViewId]);

  const handleClick = useCallback((id: string) => {
    onSwitch(id);
  }, [onSwitch]);

  // Only show tab bar when there are layout tabs (beyond just Model)
  if (tabs.length <= 1) return null;

  return (
    <div
      ref={barRef}
      style={{
        position: 'absolute',
        bottom: 28,
        left: 0,
        right: 0,
        zIndex: 10,
        display: 'flex',
        alignItems: 'stretch',
        background: 'rgba(0,0,0,0.45)',
        borderTop: '1px solid rgba(255,255,255,0.08)',
        overflowX: 'auto',
        scrollbarWidth: 'thin',
      }}
    >
      {tabs.map((tab) => {
        const isActive = tab.id === activeViewId;
        return (
          <button
            key={tab.id}
            data-active={isActive}
            onClick={() => handleClick(tab.id)}
            style={{
              padding: '6px 14px',
              fontSize: 12,
              fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif',
              border: 'none',
              borderRight: '1px solid rgba(255,255,255,0.06)',
              borderBottom: isActive ? '2px solid #00ff88' : '2px solid transparent',
              background: isActive ? 'rgba(0,255,136,0.06)' : 'transparent',
              color: isActive ? '#00ff88' : 'rgba(255,255,255,0.55)',
              cursor: 'pointer',
              whiteSpace: 'nowrap',
              transition: 'color 0.15s, background 0.15s',
              outline: 'none',
            }}
            onMouseEnter={(e) => {
              if (!isActive) {
                e.currentTarget.style.color = 'rgba(255,255,255,0.8)';
                e.currentTarget.style.background = 'rgba(255,255,255,0.05)';
              }
            }}
            onMouseLeave={(e) => {
              if (!isActive) {
                e.currentTarget.style.color = 'rgba(255,255,255,0.55)';
                e.currentTarget.style.background = 'transparent';
              }
            }}
          >
            {tab.name}
          </button>
        );
      })}
    </div>
  );
}
