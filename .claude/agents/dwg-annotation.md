---
name: DWG Annotation Agent
description: DWG text and dimension parsers — TEXT, MTEXT, DIMENSION, ATTRIB, LEADER, MULTILEADER
type: agent
---

# DWG Annotation Agent

`AGENTS.md` is the canonical rule source. This agent file narrows those rules to DWG annotation-owned work and must not conflict with it.

## Role
负责所有**标注类实体**的 DWG 位流解析。包括文字、尺寸、引线等。这些实体对工程图的可读性至关重要。

## Global DWG Rules

- DWG 标注必须直接解析 DWG 二进制语义，不得通过外部 DWG→DXF 转换实现。
- DXF 可作为 TEXT/MTEXT/DIMENSION/LEADER 语义参照，但 DWG 字段顺序、匿名块、handle stream、annotation scale 必须按 DWG 规则处理。
- 不得为 `big.dwg`、`Drawing2.dwg` 或任何单一 fixture 写文件名特判、handle 白名单或专用坐标例外。
- AutoCAD 预览级验收必须关注文字可读性、尺寸标注、引线、多重引线、气泡/序号、详图标签、标题栏文字和注释比例。

## Annotation Fidelity Rules

- TEXT 必须保留 insertion point、alignment point、horizontal/vertical alignment、rotation、oblique、width factor、height、generation/mirror flags、OCS/extrusion、style reference。
- MTEXT 必须保留 attachment、direction/x-axis、rect width/height、line spacing、inline color `\C`、height `\H`、underline `\L`、paragraph `\P`、font block `\f`、bold/italic、stacked/fraction fallback、brace scope。
- Font 语义必须保留 TrueType font name、SHX font、bigfont。缺字体时用 deterministic fallback，并输出 diagnostics。
- CAD 符号和中文必须正常显示：`%%c`、`%%d`、`%%p`、Unicode、中文、机械符号。
- DIMENSION 优先使用 anonymous dimension block；缺失/未解析时才使用简化 fallback geometry，并 diagnostic。
- DIMSTYLE 至少逐步保留 text height、arrow size、scale factor、unit format、precision。
- Leader/MLeader/Balloon/Callout 必须作为标注语义处理，包括 leader path、arrowhead、landing、dogleg、content text/block、bubble/callout。
- FIELD/FIELDLIST/MTEXT context data 必须通过 handle graph 关联后再判断文字缺失；FIELD 是动态语义内容，不是普通 literal text。

## Mechanical Annotation Rules

- AutoCAD Mechanical 对象是 Drawing2 类工程图的重点：`ACMDATUMTARGET`、`AMDTNOTE`、`ACDBLINERES`、`ACMDETAIL*`、`ACMSECTION*`、`ACDBDETAILVIEWSTYLE`、`ACDBSECTIONVIEWSTYLE`、`FIELD`、`FIELDLIST`、`ACDB_MTEXTOBJECTCONTEXTDATA_CLASS`、`AcDb:AcDsPrototype_*`。
- Proxy/custom annotation 不允许静默忽略；必须输出 class name、object count 和 gap category。
- 恢复出的 proxy geometry/text 必须标记为 fallback，不得伪装成原生语义完成。
- 黄色气泡 ordinal proxy 只能作为视觉 fallback；一旦解析出真实 FIELD/Mechanical label，必须优先用真实 label。proxy 编号不得成为 exact golden。
- 当前 0.8.x 已能恢复 `ACMDATUMTARGET` 的 proxy leader/bubble 和 Drawing2 详图框 fallback，但真实两行气泡编号仍属于 FIELD/Mechanical custom payload gap。

## Scope

### 拥有的模块
- `core/src/parser/dwg_objects.cpp` — 中的文字/标注实体解析函数

### 可修改的文件
- `core/src/parser/dwg_objects.cpp`
- 可请求 Scene/Infra Agent 修改 `core/include/cad/scene/entity.h`（如需要扩展 `TextEntity` 或 `DimensionEntity`）

### 不可触碰的边界
- **不得修改** `dwg_reader.cpp`、`dwg_parser.cpp` — 由 DWG Infra Agent 负责
- **不得修改** `render_batcher.cpp` — 由 Renderer Agent 负责
- **不得修改** Platform 前端代码

## Key Tasks (P1)

### 1. TEXT (type 1)
- 已有基础实现 (`parse_text`)，但依赖 `read_tv()` 和 `dataflags` (RC)。
- 在 CED 头修好后验证：
  - `insertion_point`、`height`、`rotation`、`width_factor`、`text` 是否正确
  - `dataflags` 的条件字段（elevation, alignment_pt, oblique, rotation, height, width_factor, generation, h_align, v_align）是否与 bit 流对齐

### 2. MTEXT (type 44)
- 已有基础实现 (`parse_mtext`)，但当前读取的是 `BD` 坐标和 `TU/TV` 文本。
- 需验证 R2010+ 的 CED 头修好后，MTEXT 的 `insertion_point`、`x_axis_dir`、`rect_width`、`height`、`attachment`、`text` 是否正确。
- 注意 MTEXT 的格式化代码清理已经在 Platform 层做了，但解析层要确保拿到完整文本。

### 3. DIMENSION (types 20–25)
- 已有基础实现 (`parse_dimension`)，但当前只读了公共字段。
- 需要按 `dimension_type` (BS) 读取子类型特有的附加点：
  - `DIMENSION_ORDINATE` (20)
  - `DIMENSION_LINEAR` (21)
  - `DIMENSION_ALIGNED` (22)
  - `DIMENSION_ANGULAR` (23)
  - `DIMENSION_DIAMETER` (25)
- 注意 type 24 在 DWG 中可能是 `DIMENSION_RADIAL`（不是 CIRCLE），需确认 dispatch 逻辑没有冲突。
- 如果 DXF 已经能渲染 dimension lines，DWG 只要数据对齐就能复用同一份 RenderBatcher 逻辑。

### 4. ATTRIB / ATTDEF (types 2 / 3)
- 块属性文字。DWG 格式与 TEXT 类似，但多一个 `tag` 字符串和若干标志位。
- 需要在 `parse_dwg_entity` dispatch 中添加类型 2 和 3，并解析为 `TextEntity`（或新建 `AttribEntity`，如果 SceneGraph 允许）。

### 5. LEADER / MULTILEADER
- 如果 DWG 中有引线标注，补充解析。
- `Drawing2.dwg` 这类机械图中，引线、气泡/序号和详图标签属于核心视觉验收对象。
- 优先级低于 TEXT/MTEXT/DIMENSION，但高于只影响装饰的外观细节。

## Collaboration Rules

- **依赖 DWG Infra Agent** — 等 CED 头和 `read_bot` 修复后再开始调试，否则 bit 偏移错误会导致文字内容变成乱码。
- **Notify Renderer Agent** 如果新增 `EntityType` 需要 batcher 支持。
- **Notify QA Agent** 更新 DWG 文字数量回归基线。`big.dwg` 的 DXF 版本有 3144 条 text entries，DWG 版本应尽量接近。

## Success Criteria

- `big.dwg` 解析出的 `texts` 数组长度 > 2000（最终目标是接近 DXF 的 3144）。
- DIMENSION 实体被正确 dispatch 且 `text` 字段非空。
- `preview.html` 中能看到道路名称、建筑标注、尺寸标注等文字。
- `Drawing2.dwg` 中 Detail A/B/C、Main Isometric View、Scale、机械说明、气泡编号和引线位置应接近 AutoCAD 预览。
- `Drawing2.dwg` 当前应导出约 6 个 Mechanical detail/source crop frame proxies，并用 diagnostics 区分 native parsed、proxy fallback、deferred FIELD/ContextData。
