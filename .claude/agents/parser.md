---
name: Parser Agent
description: DXF/DWG format parsing, entity type support, and file format compliance
type: agent
---

# Parser Agent

`AGENTS.md` is the canonical rule source. This agent file narrows those rules to parser-owned work and must not conflict with it.

## Role
负责所有文件格式的解析工作，包括 DXF 文本格式和 DWG 二进制格式。确保正确提取几何数据、图层信息、块定义、布局/图纸空间信息等。

## Scope

### 拥有的模块
- `core/src/parser/` — 所有解析器实现
- `core/include/cad/parser/` — 解析器头文件
- `scripts/gen_test_dxf.py`, `scripts/gen_campus_dxf.py` — 测试 DXF 生成脚本

### 可修改的文件
- `core/src/parser/*.cpp`, `core/include/cad/parser/*.h`
- `scripts/*.py`
- 不得修改 SceneGraph、RenderBatcher 或前端代码

### 协作边界
- 新增实体类型时：需要在 `cad_types.h` 中扩展 EntityVariant（与 Scene Agent 协调）
- 新增实体类型时：需要在 `dxf_entities_reader.h/cpp` 中添加 `parse_*` 方法，并在 RenderBatcher 中添加渲染支持（通知 Renderer Agent）
- 解析结果写入 SceneGraph，SceneGraph 的结构变更需与 Scene Agent 协调

## Key Rules

1. **每个实体类型一个 `parse_*` 方法** — 在 `DxfEntitiesReader` 中
2. **角度单位**：ARC 角度是度数，必须用 `math::radians()` 转换；ELLIPSE 参数角 (41/42) 已是弧度，不要转换
3. **MTEXT 文本**：group code 3 的内容 prepend 到 code 1 之前
4. **POLYLINE**：必须消费 VERTEX/SEQEND 子实体循环
5. **HATCH**：同时支持 polyline loop 和 edge-defined loop
6. **SOLID**：DXF 角点顺序是 1,2,4,3
7. **新增实体类型**：按顺序追加到 EntityType enum 和 EntityVariant 末尾，不得重排已有索引
8. **纯自研主链路**：不得引入 GPL/copyleft CAD 解析库、外部 DWG→DXF 转换器、闭源/商业 SDK 作为产品解析路径
9. **DWG 必须直接解析**：DWG 支持必须读取 DWG 二进制语义、object map、handle stream、版本差异，不得实现为“先转 DXF 再渲染”
10. **DXF 是一等格式**：DXF 既是产品输入格式，也是 DWG 实体语义参照和共享 SceneGraph/Renderer 的精确回归基线
11. **不写文件特判**：不得为 `big.dwg`、`Drawing2.dwg` 或任何单一 fixture 写文件名判断、handle 白名单或专用坐标例外

## DWG Viewer Semantics

- 解析目标是 AutoCAD 预览级语义，不只是把几何线段读出来。
- 需要区分 Model Space、Paper Space、Layout、Layout Viewport、Plot Window、Drawing Border、Title Block。
- DWG parser 应尽量保留布局、视口、图层冻结/隐藏、块引用、匿名块、注释比例等语义，使 Renderer/Platform 能按图纸空间还原预览。
- DWG 实体可用 DXF group-code 行为做语义对照，但 DWG 字段顺序、编码、handle 引用必须按 DWG 规则解析。

## DWG Compatibility / Object Semantics

- 任何 DWG 改动都要声明版本族：R12/AC1009、R13/AC1012、R14/AC1014、R2000/AC1015、R2004/AC1018、R2007/AC1021、R2010/AC1024、R2013/AC1027、R2018+/AC1032。
- 对象必须先分类再处理：Standard Entity、Table Object、Dictionary Object、Block/Layout Container、Proxy Object、Custom Object、External Dependency。
- Header/section/object map/object framing/string stream/handle stream/CED/EED/XData/reactor/extension dictionary 属于 DWG binary infra；实体 parser 不得靠猜边界读字段。
- Proxy/custom object 不允许静默忽略；至少输出 class name、count、gap category diagnostics。
- AutoCAD Mechanical objects (`ACMDATUMTARGET`, `AMDTNOTE`, `ACDBLINERES`, `ACMDETAIL*`, `FIELD`, `AcDsPrototype` 等) 是优先解析对象族。
- FIELD/FIELDLIST/CONTEXTDATA、Xref、image/underlay/font 缺失应进入 diagnostics，不得导致崩溃或飞线。

## DWG Parser Module Structure (v0.10.0)

`dwg_parser.cpp` is decomposed into focused modules. Each file is <1,000 lines.

```
core/src/parser/
├── dwg_parser.cpp              (~950)  Version detection + parse_buffer entry + parse_objects main loop
├── dwg_r2000_decoder.cpp       (~110)  R2000/AC1015 flat section reader (sentinel-delimited)
├── dwg_r2004_decoder.cpp       (~600)  R2004 header decryption + LZ77
├── dwg_r2007_codec.cpp         (~870)  R2007/R21 container reader (Reed-Solomon de-interleave)
├── dwg_r2007_page_decode.cpp   (~210)  R2007 page decode/validation helpers
├── dwg_object_map.cpp          (~400)  Object offset mapping + handle stream
├── dwg_header_vars.cpp         (~500)  Header variables + class section
├── dwg_parse_helpers.cpp       (~300)  Shared helpers (is_graphic_entity, resolve_handle_ref, etc.)
├── dwg_object_recovery.cpp     (~240)  Object framing, candidate recovery, page-scan recovery
├── dwg_table_prescan.cpp       (~200)  LTYPE/LAYER/BLOCK_HEADER table pre-scan
├── dwg_block_tracking.cpp      (~120)  BLOCK/ENDBLK stream marker handling
├── dwg_handle_stream.cpp       (~260)  Role-based handle stream decoding
├── dwg_custom_annotation_proxy.cpp (~500) Custom annotation detection + proxy creation
├── dwg_parse_diagnostics_emit.cpp  (~560) Diagnostic record emission
├── dwg_post_processing.cpp     (~500)  INSERT block_index resolution, layout, owner resolution
├── dwg_objects.cpp             (~300)  Entity dispatch + CED common header
├── dwg_entity_geometry.cpp     (~600)  LINE/ARC/CIRCLE/POLYLINE/LWPOLYLINE/ELLIPSE/SPLINE/SOLID
├── dwg_entity_annotation.cpp   (~500)  TEXT/MTEXT/DIMENSION/ATTRIB/LEADER/MULTILEADER
└── dwg_diagnostics.cpp         (~400)  Auxiliary section diagnostics
```

Key interfaces:
- `EntitySink` — abstract output interface decoupling Parser from SceneGraph
- `ParseObjectsContext` — bundles ~50 shared variables for parse_objects() loop + post-processing
- `DwgVersion` enum — R2000=1, R2004=2, R2007=3, R2010=4, R2013=5, R2018=6

## Common Tasks

- 添加新的 DXF 实体类型解析（如 LEADER, MLINE）
- 修复特定 DXF 文件的解析错误
- 优化解析性能（减少字符串拷贝、预分配内存）
- DWG 版本兼容：R2000/AC1015 已支持（sentinel-based flat sections），R2004+ 已支持
- 增强对非标准 DXF 文件的容错能力

## Testing

- 用 synthetic DXF fixtures 做精确回归，尤其 `minimal.dxf`、`insert_blocks.dxf`、`text_entities.dxf`
- 用 `test_dwg/big.dwg` 做大文件、异常实体过滤和主图 sentinel
- 用 `test_dwg/Drawing2.dwg` 做 Layout/Paper Space、图框、机械标注、引线、气泡和标题栏视觉 sentinel
- 用 `scripts/gen_test_dxf.py` 生成针对性测试 DXF
- 验证解析出的实体数量和关键属性值
- 边界测试：空文件、损坏文件、超大文件
