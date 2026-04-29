# ft_model — Self-Developed 2D CAD Rendering Engine

A high-performance 2D CAD rendering engine for parsing and rendering DWG/DXF files, built with C++20 core and multi-platform rendering targets.

## Features

- **DWG/DXF Parsing** — Direct binary DWG parsing (not "convert to DXF first") + full DXF support
- **Multi-format Rendering** — Canvas 2D, WebGL, Flutter Canvas/Impeller
- **Entity Support** — Lines, arcs, circles, polylines, splines, text/MTEXT, dimensions, hatch, INSERT blocks, ellipses, solids, leaders, multileaders, and more
- **Layer System** — Full layer visibility, color, freeze/lock state
- **Viewport/Layout** — Paper space, model space, layout viewports with scale and clipping
- **Performance** — Data-oriented design, flat entity arrays, quadtree frustum culling, LOD selection

## Architecture

```
C++20 Core Engine
├── Parser    — DWG binary + DXF text parsing
├── Scene     — SceneGraph, spatial index, entity model
├── Renderer  — Tessellation, LOD, render batching
└── Export     — JSON/gzip output for frontends

Platform Renderers
├── Electron (React + Canvas 2D / WebGL)
├── Flutter (Canvas / Impeller via FFI)
└── WASM bridge for web targets
```

## Quick Start

### Prerequisites

- CMake 3.20+, C++20 compiler (MSVC/GCC/Clang)
- Node.js 18+ (for Electron frontend)
- Python 3.10+ (for preview server)

### Build C++ Engine

```bash
cd build && cmake --build . --target render_export
```

### Run Preview

```bash
# One-command launch (backend + frontend)
npm run dev

# Or separately:
python start_preview.py          # Backend on port 2415
cd platforms/electron && npm run dev  # Frontend on port 5173
```

Open http://localhost:5173

### Generate Preview Data

```bash
./build/core/test/render_export test_data/minimal.dxf /tmp/minimal.json
./build/core/test/render_export test_dwg/big.dwg /tmp/big.json
```

## Project Structure

```
ft_model/
├── core/                  # C++20 engine
│   ├── include/cad/       # Public headers
│   └── src/               # Implementation
├── platforms/electron/    # React + Canvas 2D viewer
├── tools/dwg_to_scs/      # HOOPS SCS converter (optional)
├── scripts/               # Build/test utilities
├── test_data/             # Synthetic DXF test fixtures
└── start_preview.py       # Development preview server
```

## HOOPS Integration (Optional)

The project includes optional [HOOPS Exchange](https://developer.techsoft3d.com) integration for DWG→SCS conversion and side-by-side comparison. This requires a commercial license from Tech Soft 3D.

To enable:

1. Download HOOPS Exchange SDK from [Tech Soft 3D](https://developer.techsoft3d.com)
2. Set environment variables:
   ```bash
   export HOOPS_EXCHANGE_ROOT=/path/to/hoops_exchange_sdk
   export HOOPS_LICENSE="your-license-key"
   export FT_HOOPS_SCS_CONVERTER=/path/to/hoops_scs_converter  # optional
   ```
3. The HOOPS viewer will activate automatically when these are configured

## License

[MIT](LICENSE)
