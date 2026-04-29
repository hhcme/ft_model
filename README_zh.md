# ft_model — 自研 2D CAD 渲染引擎

**[English](README.md)**

高性能 2D CAD 渲染引擎，用于解析和渲染 DWG/DXF 文件，采用 C++20 核心引擎 + 多平台渲染目标架构。

> 🤖 本项目由 AI 辅助开发（Claude）。核心 C++ 引擎、DWG 二进制解析器、DXF 解析器、渲染器和前端查看器均为 AI 生成的代码。

## 特性

- **DWG/DXF 解析** — 直接解析 DWG 二进制（非"先转 DXF"方式）+ 完整 DXF 支持
- **多格式渲染** — Canvas 2D、WebGL、Flutter Canvas/Impeller
- **实体支持** — 直线、圆弧、圆、多段线、样条曲线、文字/MTEXT、标注、填充、INSERT 块参照、椭圆、实体填充、引线、多重引线等
- **图层系统** — 完整的图层可见性、颜色、冻结/锁定状态
- **视口/布局** — 图纸空间、模型空间、带缩放和裁剪的布局视口
- **性能** — 数据导向设计、平坦实体数组、四叉树视锥裁剪、LOD 选择

## 架构

```
C++20 核心引擎
├── Parser    — DWG 二进制 + DXF 文本解析
├── Scene     — 场景图、空间索引、实体模型
├── Renderer  — 曲面细分、LOD、渲染批处理
└── Export     — JSON/gzip 输出

平台渲染器
├── Electron (React + Canvas 2D / WebGL)
├── Flutter (Canvas / Impeller via FFI)
└── WASM bridge 用于 Web 目标
```

## 快速开始

### 前置条件

- CMake 3.20+、C++20 编译器（MSVC/GCC/Clang）
- Node.js 18+（用于 Electron 前端）
- Python 3.10+（用于预览服务器）

### 构建 C++ 引擎

```bash
cd build && cmake --build . --target render_export
```

### 运行预览

```bash
# 一键启动（后端 + 前端）
npm run dev

# 或分别启动：
python start_preview.py          # 后端，端口 2415
cd platforms/electron && npm run dev  # 前端，端口 5173
```

打开 http://localhost:5173

### 生成预览数据

```bash
./build/core/test/render_export test_data/minimal.dxf /tmp/minimal.json
```

## 项目结构

```
ft_model/
├── core/                  # C++20 引擎
│   ├── include/cad/       # 公共头文件
│   └── src/               # 实现
├── platforms/electron/    # React + Canvas 2D 查看器
├── tools/dwg_to_scs/      # HOOPS SCS 转换器（可选）
├── scripts/               # 构建/测试工具
├── test_data/             # 合成 DXF 测试文件
└── start_preview.py       # 开发预览服务器
```

## HOOPS 集成（可选）

项目包含可选的 [HOOPS Exchange](https://developer.techsoft3d.com) 集成，用于 DWG→SCS 转换和并排对比。这需要 Tech Soft 3D 的商业许可证。

启用步骤：

1. 从 [Tech Soft 3D](https://developer.techsoft3d.com) 下载 HOOPS Exchange SDK
2. 设置环境变量：
   ```bash
   export HOOPS_EXCHANGE_ROOT=/path/to/hoops_exchange_sdk
   export HOOPS_LICENSE="your-license-key"
   export FT_HOOPS_SCS_CONVERTER=/path/to/hoops_scs_converter  # 可选
   ```
3. 配置完成后 HOOPS 查看器会自动启用

## 许可证

[MIT](LICENSE)
