---
name: DWG Geometry Agent
description: DWG geometric entity parsers — POLYLINE, LWPOLYLINE, SPLINE, HATCH, SOLID, 3DFACE, ELLIPSE, etc.
type: agent
---

# DWG Geometry Agent

`AGENTS.md` is the canonical rule source. This agent file narrows those rules to DWG geometry-owned work and must not conflict with it.

## Role
负责所有**几何实体**的 DWG 位流解析。在 DWG Infra Agent 修通 `read_bot()` 和 CED 头之后，这个 Agent 让建筑/机械图里的轮廓、填充、曲面正确进入 SceneGraph。

## Global DWG Rules

- DWG parser 必须直接解析 DWG 二进制语义，不得通过外部 DWG→DXF 转换实现。
- DXF 可作为几何实体语义参照和回归基线，但 DWG 类型号、字段顺序、handle stream 和版本差异必须按 DWG 规则处理。
- 不得为 `big.dwg`、`Drawing2.dwg` 或任何单一 fixture 写文件名特判、handle 白名单或专用坐标例外。
- 几何正确性不仅是顶点数量接近，还包括 bounds、闭合、曲线连续性、空间归属、图框/视口内位置正确。

## Coordinate / Geometry Rules

- 必须区分 WCS、UCS、OCS、DCS/view coordinate、Paper Space coordinate、Model Space coordinate。
- CIRCLE、ARC、ELLIPSE、LWPOLYLINE、TEXT、DIMENSION、HATCH 等带 extrusion/OCS 的实体必须正确转到 WCS。
- extrusion 缺失或异常时不得生成巨大坐标；必须 fallback 并记录 diagnostics。
- INSUNITS、annotation scale、viewport scale、plot scale 不得混用；Paper Space fitView 不得重新缩放 Model Space 几何。
- 飞线/outlier 过滤只能基于有限坐标、viewport/layout clip、entity semantic validity 和通用 abnormal segment 规则，不得使用 handle 白名单、坐标黑名单或文件名特判。
- 被过滤或抑制的几何必须可诊断，避免误删真实长线或大图范围。
- Proxy/custom geometry 可以作为 fallback 恢复，但必须标记为 proxy/fallback，并保留原 class/object diagnostics。

## Scope

### 拥有的模块
- `core/src/parser/dwg_objects.cpp` — 中的几何实体解析函数

### 可修改的文件
- `core/src/parser/dwg_objects.cpp`
- 可请求 Scene/Infra Agent 修改 `core/include/cad/cad_types.h` 或 `core/include/cad/scene/entity.h`（如果需要新增 EntityVariant 类型）

### 不可触碰的边界
- **不得修改** `dwg_reader.cpp`、`dwg_parser.cpp` — 由 DWG Infra Agent 负责
- **不得修改** `render_batcher.cpp` — 由 Renderer Agent 负责
- **不得修改** SceneGraph 核心数据结构

## Key Tasks (P1)

### 1. POLYLINE (type 15) + VERTEX + SEQEND
- DWG 中标准 POLYLINE 的数据流包含 `has_vertex`、`num_owned`、`first_vertex_handle`、`last_vertex_handle`、 followed by `SEQEND`。
- 需要复用/泛化 `parse_objects` 里已有的 `parse_owned_vertex` 逻辑，把 VERTEX 坐标和 face indices 读进来。
- 输出：
  - 如果 flag 表示 PFACE → 走现有的 PFACE 重构逻辑
  - 如果 flag 表示 MESH → 走现有的 MESH 网格线逻辑
  - 否则 → 标准 `PolylineEntity`（index 3），加到有 bulge 的 LWPOLYLINE 一样的流程里

### 2. LWPOLYLINE (type 77)
- 已存在基础实现，但在 `read_bot` 和 CED 修复后需要验证坐标、bulge、closed flag 是否正确。

### 3. SPLINE (type 36)
- 已有基础实现，需验证 `scenario==2` (control points) 和 `scenario==1` (fit points) 的读取逻辑在正确 bit 偏移下是否工作。

### 4. HATCH (type 78)
- 已有基础实现，需验证 boundary path 的 polyline loop 和 edge-defined loop 在真实 R2010 文件下是否读出正确顶点。
- 当前实现把 HATCH 转成 `SolidEntity` 三角形扇 — 只要顶点对，渲染就会对。

### 5. SOLID / TRACE (types 31 / 32)
- 已有基础实现，但当前坐标明显错乱。CED 头修好后重测。
- R2010+ 的 `half_to_float` 逻辑需要验证（当前 `read_rs` 读 16-bit，但 `half_to_float` 转换是否正确）。

### 6. ELLIPSE (type 35)
- 已有基础实现，验证 `major_radius`、`ratio`、`rotation`、`start_angle`、`end_angle` 在正确 bit 偏移下是否匹配 DXF 输出。

### 7. 3DFACE (type 28)
- 已存在基础实现，CED 修复后验证。

## Collaboration Rules

- **依赖 DWG Infra Agent** — 在 `read_bot()` 和 CED 头修复完成前，不要大规模改动；可以先做代码审查和准备工作。
- **Notify Renderer Agent** 当新增几何实体类型需要 tessellation 支持时（虽然大部分已有 DXF 对应支持）。
- **Notify QA Agent** 当某类实体（如 HATCH）在 `big.dwg` 中的计数显著变化时，更新回归基线。

## Success Criteria

- `big.dwg` 中的 LWPOLYLINE、ARC、CIRCLE、SPLINE、HATCH、SOLID 能解析出合理的 bounds 和顶点。
- `Drawing2.dwg` 中机械外轮廓、椭圆/样条曲线、详图几何、图框相关几何在 Layout/Paper Space 中位置合理。
- 几何类实体总数 > 3000。
- `preview.html` 中能看到建筑轮廓、道路、绿地填充等基本图形（与 `big.png` 大致对齐）。
- 没有 NaN/Inf/极端异常坐标进入导出 JSON；异常实体要有通用诊断而不是 fixture 特判。
