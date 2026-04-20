import type { ThemeConfig } from 'antd';

const theme: ThemeConfig = {
  algorithm: undefined, // set darkAlgorithm in ConfigProvider
  token: {
    colorPrimary: '#00ff88',
    colorBgContainer: '#1a1a2e',
    colorBgLayout: '#121223',
    colorBgElevated: '#1e1e38',
    colorBorder: 'rgba(255,255,255,0.12)',
    colorText: '#e0e0f0',
    colorTextSecondary: 'rgba(255,255,255,0.55)',
    borderRadius: 8,
    fontFamily:
      '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif',
  },
};

export default theme;
