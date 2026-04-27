# CAD Rendering Engine — Project Rules

## Project Overview

Self-developed 2D CAD rendering engine for parsing and rendering DWG/DXF files.
C++20 core engine with Canvas 2D / WebGL / Flutter rendering targets.
Commercial product — no GPL or copyleft dependencies.

`AGENTS.md` is the canonical rule source. Claude-side instructions must follow the same
self-developed CAD/DWG policy, DXF role, and AutoCAD-level viewer fidelity standard.

## Self-Developed Core Policy

- DWG/DXF parser, SceneGraph, RenderBatcher, export, and platform renderers must remain self-developed.
- Do not use GPL/copyleft CAD parsers, external DWG→DXF converters, or closed/commercial SDKs in the product rendering path.
- MIT/BSD tools may be used only for tests, diagnostics, and fixture generation.
- DWG support must parse DWG binary semantics directly; it must not be implemented as "convert DWG to DXF first".
- DXF is a first-class product input format, a DWG semantic reference, and a regression baseline.

**Reference products:** HOOPS, CAD看图王

**Target platforms:**
- Desktop: Electron (Vue + React) — C++ compiled to WASM, WebGL rendering in renderer process
- Mobile: Flutter — C++ via FFI, Flutter Canvas/Impeller native rendering

## Build & Run

```bash
# Build C++ engine (from project root)
cd build && cmake --build . --target render_export
cd build && cmake --build . --target cad_core

# Generate preview data from DXF/DWG (generated JSON stays out of git)
./build/core/test/render_export test_data/minimal.dxf /tmp/minimal.json
./build/core/test/render_export test_dwg/big.dwg /tmp/big.json

# === React Preview (v0.6+, recommended) ===
# One-command launch (from project root)
npm run dev                       # starts backend + frontend concurrently

# Or launch separately:
# Terminal 1: Python backend (/parse endpoint)
python3 start_preview.py          # port 2415
# Terminal 2: Vite dev server
cd platforms/electron && npm run dev
# Open: http://localhost:5173

# === Legacy Canvas Preview (backup) ===
python3 -m http.server 8080
# Open: http://localhost:8080/platforms/electron/preview.html?data=/tmp/minimal.json
```

## Architecture Rules

### Entity Model
- **Tagged union (std::variant)**, NOT virtual inheritance. No vtable overhead.
- Flat arrays, data-oriented design for cache-friendly traversal.
- EntityVariant indices are FIXED — do not reorder. New types append at end.
- `PolylineEntity` reused for both POLYLINE and LWPOLYLINE (indices 3 and 4).
- `CircleEntity` extended for ELLIPSE (index 12) with minor_radius, rotation, start/end_angle fields.
- `TextEntity` reused for both TEXT and MTEXT (indices 6 and 7).
- `Vec3` used for Point (index 11).
- `LineEntity` reused for Ray (index 13) and XLine (index 14).
- `Vec3` reused for Viewport (index 15) as placeholder center point.
- `SolidEntity` at index 16 — filled 3/4 vertex polygon.
- `LeaderEntity` at index 17 — leader line path + arrowhead.
- `TextEntity` reused for Tolerance (index 18) — GD&T feature control frame.
- `PolylineEntity` reused for MLine (index 19) — multiline entity.

**EntityVariant index map (DO NOT REORDER):**
| Index | Type | Data Struct |
|-------|------|-------------|
| 0 | Line | LineEntity |
| 1 | Circle | CircleEntity |
| 2 | Arc | ArcEntity |
| 3 | Polyline | PolylineEntity |
| 4 | LwPolyline | PolylineEntity |
| 5 | Spline | SplineEntity |
| 6 | Text | TextEntity |
| 7 | MText | TextEntity |
| 8 | Dimension | DimensionEntity |
| 9 | Hatch | HatchEntity |
| 10 | Insert | InsertEntity |
| 11 | Point | PointEntity |
| 12 | Ellipse | CircleEntity |
| 13 | Ray | LineEntity |
| 14 | XLine | LineEntity |
| 15 | Viewport | ViewportEntity |
| 16 | Solid | SolidEntity |
| 17 | Leader | LeaderEntity |
| 18 | Tolerance | TextEntity |
| 19 | MLine | PolylineEntity |

### Math Types
- All 3D types (Vec3, Matrix4x4, Bounds3d) — 2D is z=0 special case for future 3D extension.
- `math::PI`, `math::TWO_PI`, `math::DEG_TO_RAD` — use these, NOT `M_PI`.
- Matrix4x4 uses row-major storage (`m[4][4]`). `transform_point` is correct for CPU-side transforms.
- Row-vector convention: `p * M`, so composition requires `child * parent` order.

### SceneGraph
- Owns ALL data. Everything else borrows references.
- Vertex buffer for polyline vertices — offset+count indexing.
- Layers, blocks, linetypes stored as vectors with name→index lookup.

### RenderBatcher
- Outputs `RenderBatch` with topology (LineList/LineStrip/TriangleList), color, vertex_data.
- **entity_starts**: For LineStrip batches, records vertex index where each entity starts. Essential for correct Canvas rendering (moveTo breaks between entities).
- **Transform pipeline**: `submit_entity` → `submit_entity_impl` with Matrix4x4 xform. INSERT block entities get composed transform applied during tessellation.
- **Block cache**: `m_block_cache` (cleared each `begin_frame`). Tessellate once, reuse for all INSERT instances of that block.
- Color resolution: entity ACI override (color_override != 256 && != 0) → layer color fallback.
- TEXT/MTEXT entities: render thin underline in vertex data (actual text rendered by viewer via `fillText()`).
- Output format: raw JSON or gzip (auto-detected by `.gz` suffix). Coordinates written with `%.3g` precision. Vertices are flat arrays `[x0,y0,x1,y1,...]` (not nested `[[x,y],...]`).

### Block / INSERT Handling
- **Block definition entities must be filtered** when iterating — they are rendered ONLY through INSERT references with transforms applied. Use `EntityHeader.in_block` flag.
- **Block tessellation cache**: each unique block definition is tessellated once per frame (identity transform), stored in `m_block_cache` as `block_index → (batches, vertex_count)`. Subsequent INSERTs reuse the cached tessellation, applying transforms on-the-fly.
- **Per-block vertex budget**: blocks whose cached tessellation exceeds 500,000 vertices are skipped — prevents OOM when a complex block is referenced by thousands of INSERTs.
- **Per-INSERT instance cap**: INSERT arrays (M×N) are sampled uniformly when total instances exceed 100 — preserves visual density while bounding memory. Apply `sqrt(MAX_INSTANCES / total)` scaling per dimension.
- INSERT transform: `Matrix4x4::affine_2d(scale, rotation, translation)` composed with parent transform.
- **Row-vector convention:** `insert_xform * translation_2d(offset)` then `block_xform * parent_xform` (child first, then parent).
- Depth limit of 16 for recursive INSERT nesting.
- INSERT coordinate validation: skip entities with coordinates >1e8, scale >1e4, or coord*scale >1e9.

## DXF Parsing Rules

- Each entity type has a dedicated `parse_*` method in `DxfEntitiesReader`.
- POLYLINE entities use VERTEX sub-entities followed by SEQEND — parser must consume the VERTEX loop.
- HATCH boundaries: polyline loops (path_type bit 2) AND edge-defined loops (bit 0) with line/arc edges.
- HATCH edge tessellation: line edges add endpoints directly; arc edges tessellate into segments.
- ELLIPSE: major axis endpoint (group 11/21) relative to center, minor/major ratio (group 40), parametric angles (41/42, already in radians).
- ARC angles in DXF are DEGREES — must convert to radians via `math::radians()` in parser.
- ELLIPSE parametric angles (41/42) are already RADIANS — do NOT convert.
- TEXT/MTEXT: group code 1 = text content; MTEXT group code 3 = additional text lines (prepend before code 1).
- DIMENSION: group codes 10/20/30 = definition point, 11/21/31 = text midpoint, 13/23/33 = ext1 start, 14/24/34 = ext2 start, 70 = type, 50 = rotation, 1 = text.
- SOLID: DXF order is corners 1,2,4,3 → triangles v0-v1-v3 and v1-v2-v3.
- When adding new entity types: add to EntityType enum, EntityVariant, DxfEntitiesReader dispatcher, RenderBatcher.

## Rendering Rules

- Linestrip batches MUST track `entity_starts` for path breaks between entities.
- Preview.html uses `batch.breaks` array to split linestrip rendering into per-entity sub-paths.
- Per-entity frustum culling: check first/last vertex against viewport with 50% margin.
- Dark color boost: brightness < 30 → boost to minimum 60 for dark background visibility.
- LOD: arc/circle segment count varies with zoom level via LodSelector.
- Robust fitView must use finite exported/viewable geometry with outlier-resistant sampling. Layout/Paper Space priority overrides raw model-space bounds.
- Text rendering: Canvas `fillText()` with proper world-to-screen transform, Y-flip, rotation, width scaling.
- MTEXT formatting/rendering: preserve `rectWidth`/`rectHeight`, wrap rich text, and handle `\P`, inline color/height/underline/font family/bold/italic, stacked text fallback, `\U+XXXX`, and `%%c`/`%%d`/`%%p` CAD symbols.

## DWG Viewer Fidelity Rules

- Target quality is AutoCAD preview level, not just visible geometry.
- Default open view must prioritize Layout/Plot Window → drawing border/title block → layout viewport content → model-space main entity bounds → raw scene bounds fallback.
- Model Space, Paper Space, and Layout Viewport content must not be collapsed into one raw bounds for primary preview fitting.
- Layout Viewports need viewport clipping, viewport scale, model-space content display, and per-viewport frozen/hidden layer state.
- Mechanical drawing acceptance checks border completeness, main/detail views inside the sheet, readable dimensions/leaders/balloons/text, and correct placement.
- Rendering fidelity includes layer/color inheritance, ByLayer/ByBlock, linetype, lineweight, draw order, wipeout/masking, hatch, text style, dimension style, annotation scale, and plot-style behavior.
- Watermarks from reference screenshots are not a DWG fidelity target.

## DWG Industry Compatibility Rules

`AGENTS.md` is canonical. Claude-side DWG work must follow these summarized rules:

- Track DWG version families explicitly: R12/AC1009, R13/AC1012, R14/AC1014, R2000/AC1015, R2004/AC1018, R2007/AC1021, R2010/AC1024, R2013/AC1027, and R2018+/AC1032.
- Do not infer all version behavior from `big.dwg` or any single R2010 fixture. Version-specific branches must use `DwgVersion`, AC code, section metadata, or object metadata.
- Infrastructure owns header variables, section/page maps, object maps, object framing, compression/encryption/CRC, string streams, handle streams, CED/common entity header, EED/XData, reactors, extension dictionaries, and diagnostics.
- DWG objects must be classified as Standard Entity, Table Object, Dictionary Object, Block/Layout Container, Proxy Object, Custom Object, or External Dependency before deciding whether they render.
- Proxy/custom objects must never be silently ignored. Recovered proxy geometry/text must be labeled as fallback and exported with diagnostics.
- AutoCAD Mechanical objects are priority custom objects: `ACMDATUMTARGET`, `AMDTNOTE`, `ACDBLINERES`, `ACMDETAIL*`, `ACMSECTION*`, `ACDBDETAILVIEWSTYLE`, `ACDBSECTIONVIEWSTYLE`, `FIELD`, `FIELDLIST`, `ACDB_MTEXTOBJECTCONTEXTDATA_CLASS`, and `AcDb:AcDsPrototype_*`.
- Yellow bubble ordinal labels are visual fallback only. Native FIELD/Mechanical labels must replace them once decoded, and proxy labels must not become exact golden regression data.
- Current 0.8.x Mechanical support includes proxy datum/callout leaders, yellow bubble circles/ordinal fallback, FIELD/FIELDLIST/ContextData diagnostics, and inferred detail/source crop frames for Drawing2-style files. Native two-line bubble labels remain a custom object semantic gap.
- Keep WCS/UCS/OCS/DCS, Model Space, Paper Space, viewport scale, plot scale, INSUNITS, and annotation scale distinct.
- Track fonts and annotation semantics: SHX, TrueType, bigfont, MTEXT formatting, DIMSTYLE, anonymous dimension blocks, Leader/MLeader, Balloon/Callout, FIELD/ContextData.
- Track plot and external dependency gaps: CTB/STB, screening, draw order, wipeout/mask, xref, image/PDF/DGN/DWF underlay, OLE, missing fonts.

## DWG Parsing Notes

- External references such as libredwg or ODA documentation may be used for spec verification only; do not copy GPL code or introduce those libraries into the product path.
- Entity type numbers are HEX in spec but DECIMAL in our dispatch: LINE=19(0x13), ARC=17(0x11), CIRCLE=18(0x12), TEXT=1, INSERT=7, HATCH=78(0x4E), etc.
- **Object map**: per libredwg R2004 decoder, BOTH handle and offset accumulators reset per section.
- **R2004+ CMC (Encoded Color)**: reads `BS(index)` + `BL(rgb)` + `RC(flag)` + conditional text. NOT just BS. Use `read_cmc_r2004()`.
- **R2007+ string stream**: text fields (TV/TU/T) are in a separate string stream, NOT the main entity data stream. Use the shared string-stream reader and keep codepage/UTF-16/empty/corrupt string behavior diagnosable.
- **HATCH edges**: 2RD coordinates use `read_rd()` (raw double), NOT `read_bd()`. Curve_type uses `read_raw_char()` (RC).
- **DIMENSION common fields (R2010)**: `RC(class_version)` → `3BD(extrusion)` → `2RD(text_midpt)` → `BD(elevation)` → `RC(flag1)` → text → BD0×2 → 3BD_1 → BD0 → BS×3 → BD → B×3 → 2RD0. Note `2RD` = raw doubles.
- Table objects (BLOCK_HEADER=49, LAYER=51, LTYPE=57, etc.) are non-graphical — dispatch but produce no geometry.
- BLOCK(4)/ENDBLK(5) are stream markers — dispatch but produce no geometry.
- Entity type dispatch in `dwg_objects.cpp` switch statement; add new types by case number + parser function.
- `DwgVersion` enum: R2004=2, R2007=3, R2010=4.

## Canvas Preview (preview.html) Notes

- Y-axis flip: `sy = -(wy - viewCenterY) * viewZoom + canvas.height / 2`
- Zoom toward mouse position: recalculate center after zoom so cursor stays on same world point.
- Touch input: pinch zoom with two-finger gesture.
- Layer panel: visibility toggles by layer name (frozen layers hidden by default).
- Batch-level frustum culling using precomputed batch bounds (world coords).
- **Gzip support**: preview.html fetches `.json.gz` files using `fetch()` with `Automatic gzip decompression` (`DecompressionStream`) — native browser API, no extra JS library needed.
- Generated preview JSON belongs in `/tmp`, IndexedDB, or ignored paths and is not committed.
- **此文件为旧版备用**，新版预览器使用 React + Ant Design（见下方）。

## React Preview (v0.6.1+) Notes

**技术栈**：Vite + React 18 + TypeScript + Ant Design 5（暗色主题，`colorPrimary: '#00ff88'`）

**一键启动**（WebStorm）：
- `.idea/runConfigurations/` 包含 3 个配置：Preview_All（前后端同时启动）、Preview_Backend、Preview_Frontend
- 或命令行：`npm run dev`（项目根目录，通过 concurrently 同时启动后端+前端）

**单独启动**：
```bash
# 终端 1：Python 后端（/parse 端点）
python3 start_preview.py          # 端口 2415

# 终端 2：Vite 开发服务器（代理 /parse → 2415）
cd platforms/electron && npm run dev
# 打开 http://localhost:5173
```

**目录结构**（`platforms/electron/src/`）：
- `app/` — 根组件（App.tsx）、主题配置（theme.ts）、全局类型定义（types.ts）
- `components/landing/` — 着陆页：文件上传 + 最近文件列表
- `components/viewer/` — 预览器：Canvas 渲染 + 工具栏 + 图层面板 + 状态栏
- `components/parsing/` — 解析进度遮罩
- `hooks/` — `useFileLoader`（文件加载+缓存）、`useMeasurement`（距离/面积测量）
- `utils/` — `renderer`（Canvas 渲染+图框）、`geometry`（包围盒+fitView 离群过滤）、`transforms`（坐标变换）、`cache`（IndexedDB+localStorage）、`textUtils`（MTEXT 格式处理）

**缓存机制**：
- IndexedDB（`cad-preview-cache`）存储解析后的 DrawData + 原始文件 Blob
- localStorage 存储最近文件元数据（名称、大小、实体数、cacheKey），最多 10 条
- CacheKey includes a schema prefix plus `${file.name}:${file.size}:${file.lastModified}`; bump `CACHE_SCHEMA_VERSION` whenever DrawData/rendering semantics change.
- 页面刷新自动恢复上次打开的文件
- 重新解析：从 IndexedDB 取出原始 Blob 重新发给 /parse，无需重新选文件
- 从最近文件切换也会更新"上次打开"记录

**渲染管线要点**：
- `getViewportWorldBounds`：屏幕四角转世界坐标，注意 Y 轴翻转（screenToWorld(0, canvasH) → minY, screenToWorld(canvasW, 0) → maxY）
- `fitViewToBounds`：优先使用 Layout/Plot Window/presentation bounds；没有有效 view 时才使用有限几何的离群过滤 fallback
- `renderBatches`：batch 级视锥裁剪 + linestrip 逐实体 AABB 裁剪（非首尾顶点）
- `renderBorder`：根据 DrawData.bounds 绘制虚线图框 + 淡背景填充
- batch bounds 用 `useMemo` 缓存，避免每次平移/缩放重算

**核心设计原则**：
1. 单一职责：组件只做一件事，逻辑提取到 hooks，计算提取到 utils
2. Props 驱动：不直接访问全局状态
3. Ant Design 优先：Button/Tooltip/Upload/Spin/List/Drawer 等
4. 纯函数渲染：`renderBatches()`, `worldToScreen()` 等在 `utils/` 中
5. 文件大小限制：组件 ≤150 行，hooks/utils ≤200 行，app/ ≤100 行

**/parse 端点**（start_preview.py）：
- 接受 raw binary body + `X-Filename` header
- 返回 `Content-Encoding: gzip` 的 JSON
- 支持 CORS（开发环境 Vite 代理需要）

## Coding Standards

- C++20 with std::variant. No virtual dispatch for entities.
- MIT/BSD third-party dependencies only.
- All math types in `cad_types.h` — extend there first.
- Header files use `#pragma once`.
- Namespace: `cad::`.
- Member variables: `m_` prefix.
- Include order: corresponding header → project headers → system headers.
- **No special-casing for specific test files.** All fixes must be general algorithmic/math improvements.

## Git Workflow

- Remote: `origin` → `https://gitee.com/smarthhc/ft_model.git`
- Branch: `main` (开发阶段直推，后续可引入 feature 分支)
- Commit style: conventional commits (`feat:`, `fix:`, `refactor:`, `docs:`, `perf:`, `test:`)
- Co-authored commits with Claude: add `Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>`
- Push with: `git push origin main`

### PR / Change Review Standards

- **核心结构变更**（EntityVariant, RenderBatch, SceneGraph 接口）需说明影响范围并通知相关 Agent
- **C++ 性能敏感代码**审查要点：避免不必要的拷贝、关注内存布局和 cache 友好性
- **不得针对特定测试文件特殊处理**，所有修复必须是通用改进
- 变更后须通过 synthetic DXF exact regression and real DWG sentinel visual/data regression

## Testing

- Use ezdxf (MIT) to generate test DXF files.
- Validate shared parser/renderer behavior with synthetic DXF exact regression fixtures.
- Use `test_dwg/big.dwg` for abnormal-entity filtering, large-file behavior, and main drawing sentinel checks.
- Use `test_dwg/Drawing2.dwg` for Layout/Paper Space, drawing border, viewport, mechanical annotation, leader, balloon, and title-block visual checks.
- Track DWG version fixtures for R2000, R2004, R2007, R2010, R2013, and R2018+. Missing real fixtures must be recorded in the audit as Missing fixture.
- Use `test_dwg/zj-02-00-1.dwg` and `test_dwg/新块.dwg` as fixture catalog candidates after classification.
- Visual comparison against `test_dwg/big.png` reference image.
- Regression: run `render_export` on DXF and DWG, verify JSON schema, finite bounds, diagnostics, and sentinel lower bounds.
- Never commit generated JSON files to git (they are in .gitignore).

## Required DWG Change Description

Every DWG change summary should include version family, object family, affected pipeline stage, gap label, fixtures checked, and diagnostics changed.

## Agent Team

项目按模块划分 5 个 Agent 角色，每个 Agent 有独立的指令文件（`.claude/agents/`）。

| Agent | 指令文件 | 负责模块 |
|-------|---------|---------|
| **Parser** | `.claude/agents/parser.md` | DXF/DWG 解析 (`core/src/parser/`) |
| **Renderer** | `.claude/agents/renderer.md` | 渲染管线、tessellation (`core/src/renderer/`) |
| **Scene/Infra** | `.claude/agents/scene-infra.md` | SceneGraph、空间索引、内存、数学类型 (`core/src/scene/`, `core/src/memory/`, `cad_types.h`) |
| **Platform** | `.claude/agents/platform.md` | Canvas/WebGL 前端、WASM 桥接 (`platforms/electron/`, `gfx/`) |
| **QA** | `.claude/agents/qa.md` | 测试、回归、性能基准 (`core/test/`, `scripts/`) |

### 协作规则

1. **模块边界清晰** — 每个 Agent 只修改自己负责的文件，不越界修改其他模块
2. **跨模块变更需协调** — 新增实体类型需 Parser → Scene/Infra → Renderer → Platform 串行配合
3. **变更通知链**：核心结构变更（EntityVariant、RenderBatch 格式、SceneGraph 接口）必须通知所有受影响的 Agent
4. **QA Agent 有只读全局权限** — 可审读任何模块代码，但不得修改生产代码，发现问题报给对应 Agent
5. **主 Agent 统一审查** — 跨模块变更由主 Agent 审查，子 Agent 并行执行不越界
6. **DWG 攻坚小组并行约束** — Infra 子 Agent 完成后，Geometry/Annotation/Insert 才可并行

## v0.10.0 Architecture & Performance Rules

### 通用性原则（Generality Principle）
- DWG 解析器是**通用工具**，禁止针对特定测试文件的 hack
- 所有修复必须是通用算法/数学改进
- Bug 根因分析从"为什么解析器无法兼容此模式"角度思考，而非"为什么这个文件解析不了"
- 新增实体类型或版本支持时，必须考虑该模式在其他版本/文件中的普遍性

### 模块化原则（Modularity — 硬约束）
- **每个源文件硬上限 1,000 行**。超过必须拆分
- 解析/渲染/场景/平台各模块只修改自己负责的文件
- 解析器内部按**版本族**（R2000/R2004/R2007/R2010+）和**功能域**（容器/头部/对象映射/实体/诊断）拆分
- 实体解析按**几何/标注/填充/块参照**四域拆分
- 新增 `EntitySink` 接口解耦 Parser↔SceneGraph，解析器不直接操作 SceneGraph
- codec/decoder 模块不依赖 SceneGraph，只接收 raw buffer，返回结构化数据

### 数学优先原则（Math-First Principle）
复杂计算优先使用数学方法，而非暴力/启发式方法：

| 问题 | 数学方法 | 替代 |
|------|---------|------|
| 弧线细分段数 | 弦高误差公式 `N >= π / arccos(1 - T/(R·ppu))` | 固定段数/启发式 |
| 大弧线 tessellation | 递归中点自适应细分 | 均匀角度细分 |
| 离群几何检测 | RANSAC（随机抽样一致性） | 百分位/启发式 |
| 空间查询 | Quadtree O(log N) | 暴力遍历 |
| LOD 选择 | screen-space error metric | circumference_pixels / 8 |
| fitView 边界 | RANSAC + 最小包围矩形 | 简单 min/max |

### 性能基线（Performance Baselines）
- Canvas 预览器：**60fps @ fitView**、首帧 < 3s、图层切换 < 16ms
- 输出体积：每个实体平均 < 120 bytes JSON
- 渲染管线使用 **rAF + dirty flag** 模式，禁止 useEffect 直接触发全量重绘
- 文本测量必须缓存（`TextMeasureCache`），禁止每帧重复 `measureText()`
- 空间索引必须在解析完成后构建，裁剪管线必须使用索引查询

### 文件体积管控（File Size Control）
- 代码文件每个 < 1,000 行
- 输出 JSON gzip 压缩后 < 原始 DWG 文件大小
- `test_dwg/` 目录禁止存放生成文件（JSON/gzip 属于 `/tmp` 或 `.gitignore`）
- 顶点精度使用 `%.3g`（视觉无损），避免不必要的精度浪费
- 块定义几何在 INSERT 展开后省略，避免重复输出

### DWG Parser 模块结构（v0.10.0 拆分后）
```
core/src/parser/
├── dwg_parser.cpp          (~800 行) 版本检测 + 模块编排 + parse_buffer 入口
├── dwg_r2007_codec.cpp     (~800 行) R2007/R21 容器解码
├── dwg_r2004_decoder.cpp   (~600 行) R2004 头部解密 + LZ77
├── dwg_object_map.cpp      (~400 行) 对象偏移映射 + handle 流
├── dwg_header_vars.cpp     (~500 行) 头部变量 + 类段
├── dwg_diagnostics.cpp     (~400 行) 辅助段诊断
├── dwg_entity_sink.h/cpp   (~140 行) EntitySink 接口 + 适配器
├── dwg_objects.cpp          (~300 行) 实体分发 + CED 公共头
├── dwg_entity_geometry.cpp  (~600 行) LINE/ARC/CIRCLE/POLYLINE/LWPOLYLINE/ELLIPSE/SPLINE/SOLID
├── dwg_entity_annotation.cpp (~500 行) TEXT/MTEXT/DIMENSION/ATTRIB/LEADER/MULTILEADER
├── dwg_entity_hatch.cpp     (~400 行) HATCH 边界环 + 边细分
├── dwg_entity_insert.cpp    (~400 行) INSERT/MINSERT + 块解析
├── dxg_entities_reader.cpp  (保持) DXF 实体读取
└── dwg_reader.cpp           (保持) DWG bitstream 读取
```
