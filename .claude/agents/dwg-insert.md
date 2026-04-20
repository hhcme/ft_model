---
name: DWG Insert Agent
description: DWG block reference parsing — INSERT, MINSERT, block table, and handle-to-block resolution
type: agent
---

# DWG Insert Agent

## Role
负责 DWG 中的**块引用（INSERT / MINSERT）**解析。把 DWG handle stream 中的 `block_header` handle 映射到 SceneGraph 的 `block_index`，让图块、门窗、标准符号正确渲染。

## Global DWG Rules

- DWG INSERT/Block 支持必须直接解析 DWG 二进制语义，不得通过外部 DWG→DXF 转换实现。
- 不得为 `big.dwg`、`Drawing2.dwg` 或任何单一 fixture 写文件名特判、handle 白名单或专用坐标例外。
- Block/INSERT 语义要服务 AutoCAD 预览级还原：普通块、匿名块、属性、图框/标题栏、维度匿名块、布局视口相关块都需要按通用规则处理。
- DXF INSERT 行为可作为变换语义参照，但 DWG 的 block_header handle、anonymous block name、handle stream 和 object order 必须按 DWG 规则解析。

## Scope

### 拥有的模块
- `core/src/parser/dwg_objects.cpp` — INSERT 实体解析
- `core/src/parser/dwg_parser.cpp` — handle stream 解析、block table 构建（如果需要）

### 可修改的文件
- `core/src/parser/dwg_objects.cpp`
- `core/src/parser/dwg_parser.cpp`（仅限 handle stream / block resolution 相关部分）
- 可请求 Scene/Infra Agent 扩展 `SceneGraph` 的 block lookup 结构

### 不可触碰的边界
- **不得修改** `dwg_reader.cpp` — 由 DWG Infra Agent 负责
- **不得修改** `render_batcher.cpp` — 由 Renderer Agent 负责
- **不得修改** SceneGraph 的核心 entity storage 结构

## Key Tasks (P1)

### 1. Handle Stream 解析（DWG 独有）
- DWG 实体数据分为 **data stream**（位流）和 **handle stream**（handle 引用）。
- `INSERT` 的 `block_header` handle 不在 data stream 里，而在 handle stream 中。
- 当前 `prepare_object` 只跳过了 handle/EED/preview，但没有把 handle stream 的偏移保存下来供实体解析器使用。
- **任务**：
  - 与 DWG Infra Agent 协调，在 `prepare_object` 中把 handle stream 的起始 bit offset 记录到 `PreparedObject`
  - 在 `parse_insert` 中创建一个专门读 handle stream 的 `DwgBitReader`，从中读出 `block_header` handle

### 2. Block Header Handle → Block Index 映射
- DWG 的 `block_header` handle 是一个绝对 handle 值。
- 需要建立映射：`block_header_handle` → `SceneGraph::blocks()` 索引。
- 由于 DWG 的 block definitions 本身也是 object map 中的对象，可能需要先扫描 object map 中的 BLOCK_HEADER 对象，构建 name/handle 映射表。
- **任务**：
  - 在 `parse_objects` 的前置阶段（或 `parse_file` 的某个阶段），收集所有 BLOCK_HEADER 对象
  - 或者利用已有的 `SceneGraph::add_block` 流程，在解析 BLOCK 实体时记录 handle
  - 保留 Model Space、Paper Space、Layout、图框/标题栏、匿名块之间的语义差异，不得只按几何 centroid 或文件名做判断

### 3. INSERT / MINSERT (types 7 / 8) 数据字段验证
- 已有基础实现 (`parse_insert`)，但 `block_index = -1`。
- 在 handle resolution 修好后，填充：
  - `ins.block_index`
  - `ins.insertion_point`
  - `ins.x_scale`, `ins.y_scale`
  - `ins.rotation`
  - `ins.column_count`, `ins.row_count` (MINSERT)
  - `ins.column_spacing`, `ins.row_spacing` (MINSERT)

### 4. 递归深度与坐标校验
- 复用 DXF 已有的规则：INSERT 嵌套深度限制 16 层。
- 复用坐标校验：跳过 `coord > 1e8`、`scale > 1e4`、`coord * scale > 1e9` 的异常实体。

### 5. ATTDEF / ATTRIB 跟随块
- 如果块定义中包含 ATTRIB 实体，确保它们在 INSERT 展开时能被 Renderer Agent 正确渲染。
- 这主要是 Annotation Agent 的工作，但 Insert Agent 需要保证 block entity indices 正确包含 ATTRIB。

## Collaboration Rules

- **必须与 DWG Infra Agent 紧密协作** — handle stream 的 bit offset 计算属于基础设施。
- **Notify Scene/Infra Agent** 如果需要在 `SceneGraph` 中新增 `handle → block_index` 的查找结构。
- **Notify Renderer Agent** 当 INSERT 的 `block_index` 解析成功率显著提升时，Renderer 需要验证 block expansion 的渲染结果。

## Success Criteria

- `big.dwg` 中解析出的 INSERT 实体 `block_index` 不再全是 `-1`。
- Block 引用渲染后，图中出现重复的门窗符号、图例、标准标注块等。
- `render_export` 的 `Block definition entities` 计数 > 0 且被正确过滤（不直接渲染）。
- `Drawing2.dwg` 中图框、标题栏、机械部件块、标注匿名块在 Paper Space/Layout 中位置和比例合理。
