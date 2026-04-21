---
name: DWG Infra Agent
description: DWG bitstream decoding, CED header parsing, classes section, and format infrastructure
type: agent
---

# DWG Infra Agent

`AGENTS.md` is the canonical rule source. This agent file narrows those rules to DWG infra-owned work and must not conflict with it.

## Role
负责 DWG 解析的底层基础设施。这是整个 DWG 攻坚的 **P0 阻塞路径**，不修好这些，其他实体解析无法工作。

## Global DWG Rules

- DWG 主链路必须纯自研，不得接入 GPL/copyleft CAD parser、外部 DWG→DXF converter、闭源/商业 SDK。
- DWG 必须直接解析二进制对象、section、object map、handle stream、string stream、version-specific bit layout。
- DXF 可作为实体语义参照和回归基线，但不能作为 DWG 解析实现路径。
- 不得为 `big.dwg`、`Drawing2.dwg` 或任何单一 fixture 写文件名特判、handle 白名单或专用坐标例外。
- 基础设施修复要服务 AutoCAD 预览级语义，包括 Layout、Paper Space、Model Space、Layout Viewport、Plot Window、图层状态和对象引用。

## DWG Version / Binary Infrastructure Rules

- 必须按版本族记录和测试：R12/AC1009、R13/AC1012、R14/AC1014、R2000/AC1015、R2004/AC1018、R2007/AC1021、R2010/AC1024、R2013/AC1027、R2018+/AC1032。
- 版本分支只能基于 `DwgVersion`、AC code、section metadata、object metadata；不得基于文件名、对象数量、坐标范围或截图效果。
- Infra 层拥有 header vars、file codepage、section/page map、object map、object size/type framing、compression/encryption/CRC、main/string/handle stream 边界、CED/common entity header、EED/XData、reactors、extension dictionary。
- Header vars 中的 INSUNITS、LIMMIN/LIMMAX、EXTMIN/EXTMAX、current model/layout hints 应保存到 SceneGraph metadata 或 diagnostics。
- Object handle/offset 必须来自 object map 或明确的 recovered candidate；recovered object 必须 diagnostic，不能静默当作正常 object。
- bit drift、未知 CED/EED/XData skip、损坏 string stream 必须记录为 Encoding gap 或 Object framing gap。
- Handle reference 必须按 code + counter relative/absolute 规则解析；owner、reactor、extension dictionary、block record、layout handle 必须保留或 diagnostic。
- R2007+ string stream 是一等解析目标。TEXT/MTEXT/FIELD/STYLE/LTYPE/LAYER/name 字段必须使用统一 string reader，覆盖 UTF-16、codepage、空字符串和损坏字符串。
- CMC/ENC 必须区分 ACI、TrueColor、ColorBook、ByLayer、ByBlock、foreground/background、transparency、lineweight、linetype scale、plot style、material、shadow flags。

## Scope

### 拥有的模块
- `core/src/parser/dwg_reader.cpp` / `dwg_reader.h` — BitReader、编码函数（`read_bot`、`read_dd`、`read_cmc` 等）
- `core/src/parser/dwg_parser.cpp` / `dwg_parser.h` — `prepare_object`、CED 头、Object Map、Classes Section

### 可修改的文件
- `core/src/parser/dwg_reader.cpp`, `core/include/cad/parser/dwg_reader.h`
- `core/src/parser/dwg_parser.cpp`, `core/include/cad/parser/dwg_parser.h`

### 不可触碰的边界
- **不得修改** `dwg_objects.cpp` — 那是 DWG Geometry/Annotation/Insert Agents 的地盘
- **不得修改** SceneGraph、RenderBatcher、前端代码

## Key Tasks (P0)

1. **修复 `read_bot()`** (`dwg_reader.cpp:194`)
   - R2010+ 2-bit code 语义：
     - `00` → read `BS`
     - `01` → return `0`
     - `10` → return `1`
     - `11` → read `RC` (raw char, add `0x1F0` if non-zero? verify against libredwg/ODA)
   - 这是当前对象类型和后续 bit 偏移全部错位的根因。

2. **实现 R2010+ CED (Compact Entity Data) 头解析** (`dwg_parser.cpp:985` 附近)
   - `entity_mode` (2 bits)
   - `num_reactors` (`BL`)
   - `is_xdic_missing` (`B`)
   - `color` (`BS`) + `color_flags` (high bits of color BS)
   - `linetype_scale` (`BD`)
   - `ltype_flags` (2BB)
   - `plotstyle_flags` (2BB)
   - `material_flags` (2BB, R2007+)
   - `shadow_flags` (`RC`, R2007+)
   - `invisible` (`BS`)
   - `lineweight` (`RC`)
   - 修好后，`DwgBitReader` 传给 `parse_dwg_entity` 时必须正好位于实体数据的第一个 bit。

3. **修复 Classes Section 解析** (`dwg_parser.cpp:658`)
   - 先读 `BL comment_size`
   - 跳过 `comment_size` 个 UTF-16LE 字符
   - 再读 `RC class_version`、`BL num_classes`
   - 然后进入 class records
   - 目标：从当前的 ~8 条记录提升到几十上百条。

## Collaboration Rules

- **Notify Parser Agent** when `DwgBitReader` API changes (new methods or changed signatures).
- **Notify all DWG sub-agents** when CED header bit layout is fixed — they will then verify their entity parsers start at the correct offset.
- **Coordinate with QA Agent** to update the DWG smoke baseline after P0 lands (entity count should jump dramatically).

## Success Criteria

- `render_export test_dwg/big.dwg` 的 `skip_bits` 从 4086 降到 <500。
- 类型直方图出现 LWPOLYLINE(48)、CIRCLE(18/24)、ARC(17)、TEXT(1)、MTEXT(44)、HATCH(49)、INSERT(7) 等常见类型。
- Classes 表解析出 >50 条记录。
- 每个新增版本分支都有适用版本、fallback 和 diagnostics code。
- DWG version fixture audit 至少记录 R2000/R2004/R2007/R2010/R2013/R2018 的 Implemented/Partial/Missing 状态。

## Handle Resolution Protocol

DWG 使用 handle 引用系统连接对象之间的关系。Handle 在文件的整个生命周期内持久不变。

### Handle 编码格式

Handle 由 `read_h()` 读取，格式为：
```
RC(code_byte):
  code    = (code_byte >> 4) & 0x0F   // 高 4 位：引用类型
  counter = code_byte & 0x0F          // 低 4 位：后续字节数
value = counter 个字节（大端序）
```

### Handle Code 语义表

基于 ODA specification 和 libredwg 逆向：

| Code | 类型 | 计算公式 | 典型用途 |
|------|------|----------|----------|
| 0x00 | — | 无效/未使用 | 结束标记 |
| 0x01 | — | (reserved) | — |
| 0x02 | Soft Pointer (减法) | abs = current_handle - value - 1 | Entity→Entity 反向引用 |
| 0x03 | Soft Pointer (加法) | abs = current_handle + value | Entity→Entity 正向引用 |
| 0x04 | Hard Pointer (减法) | abs = current_handle - value - 1 | Owner handle, Entity→Block_Header |
| 0x05 | Hard Pointer (减法) | abs = current_handle - value - 1 | Hard ownership 反向 |
| 0x06 | Absolute | abs = value | 绝对引用（最常见） |
| 0x07 | — | (reserved) | — |
| 0x08 | Hard Owner (减法) | abs = current_handle - value - 1 | Entity→所属对象 |
| 0x09 | — | (reserved) | — |
| 0x0A | Soft Pointer (减法) | abs = current_handle - value - 1 | Soft ownership 反向 |
| 0x0B | Absolute (替代) | abs = value | 绝对引用（替代形式） |
| 0x0C | Absolute (替代) | abs = value | 绝对引用（替代形式） |

核心规则：
- Code 2, 4, 5, 8, 0xA = 减法引用：`abs = current_handle - value - 1`
- Code 3 = 加法引用：`abs = current_handle + value`
- Code 6, 0xB, 0xC = 绝对引用：`abs = value`

### Owner Handle Chain（归属链）

DWG 通过 owner handle 确定每个实体的空间归属：

```
Entity → owner_handle (code 4) → BLOCK_HEADER
  → BLOCK_HEADER.layout_handle → LAYOUT 对象
    → LAYOUT.tab_order + layout_name → 确定 Paper Space 布局

Model Space block record:
  - 所有 Model Space 实体的 owner chain 最终指向 Model Space 的 BLOCK_HEADER
  - 通常 layout_name = "Model" 或 tab_order = 0

Paper Space block record:
  - 每个 Layout 的 BLOCK_HEADER 指向对应的 LAYOUT 对象
  - layout_name 如 "Layout1", "Layout2"
```

### Handle Stream 结构

每个对象的 handle stream 紧跟 data stream 之后（位于 CED/entity 数据末尾）。

典型 handle stream 序列（R2010+ 实体）：
```
1. owner_handle (H, code 4)        — 所属 BLOCK_HEADER
2. reactor_handles (N × H)         — N = num_reactors (CED 中读取)
3. xdicobjhandle (H, optional)     — 仅当 !is_xdic_missing
4. layer_handle (H)                — 图层引用（实体的 layer 属性）
5. 后续 handles（按实体类型不同）：
   - LINE: 无额外 handle
   - INSERT: block_header_handle, first_attrib, end_attrib_seqend
   - TEXT: style_handle
   - DIMENSION: block_header(anonymous), style_handle
   - LWPOLYLINE: ...
```

### Reactor Handle List

Reactor 是对象间的通知关系：
```
num_reactors (在 CED header 中读取为 BL)
handle stream 中紧接着 owner_handle 之后读取 num_reactors 个 H
每个 reactor handle 指向关注此对象的其他对象
```

### Extension Dictionary Handle

```
is_xdic_missing (在 CED header 中读取为 B)
  → false: handle stream 中有 xdicobjhandle (H)，指向扩展字典 DICTIONARY 对象
  → true: 跳过，无扩展字典
```

扩展字典通常包含 XRecord 或自定义应用数据。

### 匿名块 Handle 约定

AutoCAD 使用匿名块存储自动生成的图形：

| 前缀 | 含义 | 生成者 |
|------|------|--------|
| `*D` | Dimension block | DIMENSION 实体的匿名块 |
| `*A` | Hatch block | 关联填充边界 |
| `*E` | Region/Surface | REGION 命令 |
| `*T` | Table block | ACAD_TABLE 实体 |
| `*X` | MINSERT block | MINSERT 内部块 |
| `*U` | User anonymous | 用户/应用创建的匿名块 |

匿名块的 BLOCK_HEADER handle 在 INSERT/DIMENSION 的 handle stream 中引用。

### INSERT block_header Handle

INSERT 的 handle stream 中包含 block_header handle：
```
parse_insert() 的 handle stream:
  owner_handle, [reactor_handles], [xdic_handle],
  block_header_handle (H),  ← 这个就是 INSERT 引用的块
  first_attrib_handle, end_attrib_seqend_handle,
  [insert_handle (for MINSERT nested)]
```

解析流程：
1. 读取 block_header_handle
2. 在 `m_sections.handle_map` 中查找 → 获得 object_data 偏移
3. 该偏移处的 BLOCK_HEADER 对象已解析过，其 name 对应 `SceneGraph::blocks()` 中的 block
4. 通过 `m_sections.block_names` 或 `entity_handle_to_block_header` 映射到 `block_index`

代码参考：
- handle 读取：`dwg_reader.cpp` `read_h()` (line 498)
- handle map 构建：`dwg_parser.cpp` `parse_object_map()` (line 1740)
- block_header 收集：`dwg_parser.cpp` `block_names_from_entities` 逻辑 (line 3201)
- INSERT block 引用解析：`dwg_parser.cpp` `parse_objects()` INSERT 分支

## Table Object Binary Specifications

表对象（LAYER, LTYPE, STYLE, DIMSTYLE, VPORT 等）在 DWG 中是非图形对象，不直接渲染但为渲染提供语义（颜色、线型、标注样式等）。

### 公共表对象前缀

所有表对象共享以下前缀字段（R2004+）：

```
RC(class_version)         — R13+，通常为 0
T(name)                   — 对象名（R2007+ 从 string stream 读取）
--- R2004+ 附加 ---
B(is_xref_dep)            — 是否外部参照依赖
BS(xref_status)           — 外部参照状态（0=resolved, 1=unresolved）
--- Handle stream ---
H(owner_handle)           — 所属 _CONTROL 对象
H(xref_handle)            — 外部参照句柄（如果 is_xref_dep）
```

### LAYER (type 51) 字段序列

```
--- Common table prefix ---
RC(class_version)
T(name)

--- LAYER specific ---
BS(flag0)                 — 位标志：
  bit 0: frozen
  bit 1: frozen_in_new_viewports
  bit 2: locked
  bit 3: off (hidden)
  bit 4: plotflag (0=plottable)
  bits 5-8: linewt index (0-28 → 0,5,9,13,15,18,20,25,30,35,40,50,53,60,70,80,90,100,106,120,140,158,200,211...)

CMC(color)                — 图层颜色（R2004+: 用 read_cmc_r2004()）

--- R2000+ conditional ---
B(plotflag)               — R2000: 是否可打印
BD(linewt)                — R2000: 线宽（BD 形式，而非 flag0 bits）

--- Handle stream ---
H(linetype_handle)        — 默认线型引用
H(plotstyle_handle)       — R2004+: 打印样式引用
H(material_handle)        — R2007+: 材质引用
```

关键渲染字段：`flag0`（frozen/off/locked）、`color`、`linetype_handle`、`linewt`

代码映射：`dwg_objects.cpp` parse_layer() (line ~1590-1678)

### LTYPE (type 57) 字段序列

```
--- Common table prefix ---
RC(class_version)
T(name)

--- LTYPE specific ---
BS(flags)                 — 0=bylayer/byblock, 1=standard
T(description)            — 线型描述文字
BD(pattern_length)        — 图案总长度
BL(num_dashes)            — 短划线数量

--- Per-dash (num_dashes ×) ---
BD(length)                — 短划线长度（正=划线, 负=间隔, 0=点）
BS(complex_shapecode)     — 复杂形状代码（0=简单线段）
BD(x_offset)              — 形状 X 偏移
BD(y_offset)              — 形状 Y 偏移
BD(scale)                 — 形状缩放
BD(rotation)              — 形状旋转
BS(shape_flag)            — bit 2=shape, bit 4=text

--- Handle stream ---
每个 dash 一个 H(shapefile_handle)（仅当 shape_flag 非 0）
H(owner_handle)
```

关键渲染字段：`pattern_length`、`num_dashes`、dash lengths（决定虚线图案）

代码映射：`dwg_objects.cpp` parse_linetype() (line ~1687-1730)

### STYLE (type 53) 字段序列

```
--- Common table prefix ---
RC(class_version)
T(name)

--- STYLE specific ---
BL(flags)                 — bit 0: vertical, bit 1: fixed_height
T(font_name)              — TrueType 字体名或 SHX 文件名
T(bigfont_name)           — 大字体文件名（中文/日文/韩文 SHX）
BD(height)                — 固定文字高度（flags bit 1 时有效）
BD(width_factor)          — 宽度因子
BD(oblique)               — 倾斜角度（弧度）
BL(gen_flags)             — bit 0: backward, bit 1: upside_down
BL(pitch)                 — 字体 pitch
BL(family)                — 字体 family

--- Handle stream ---
H(shapefile_handle)       — SHX shapefile handle
H(owner_handle)
```

关键渲染字段：`font_name`、`bigfont_name`、`height`、`width_factor`、`oblique`

代码映射：`dwg_objects.cpp` parse_style() (line ~1738-1776)

### DIMSTYLE (type 69) 字段序列

DIMSTYLE 是 DWG 中字段最多的表对象（约 80 个字段），对尺寸标注渲染至关重要。

```
--- Common table prefix ---
RC(class_version)
T(name)

--- DIMSTYLE fields ---
T(dimpost)                — 主单位后缀（如 "cm"）
T(dimapost)               — 换算单位后缀
B(dimtol)                 — 是否生成公差
B(dimlim)                 — 是否生成为极限尺寸
BD(dimtm)                 — 负公差值
BD(dimtp)                 — 正公差值
BD(dimrnd)                — 舍入值
BD(dimalt)                — 换算单位因子
BS(dimazin)               — 消零标志（主单位）
BS(dimazin_alt)           — 消零标志（换算单位）

--- 控制类字段 ---
BD(dimasz)                — 箭头大小 ★CRITICAL
BD(dimcen)                — 圆心标记大小
BD(dimexe)                — 延伸线超出量 ★CRITICAL
BD(dimexo)                — 延伸线偏移量 ★CRITICAL
BD(dimgap)                — 文字与标注线间距 ★CRITICAL
BD(dimlfac)               — 线性比例因子
BD(dimscale)              — 全局标注比例 ★CRITICAL
BD(dimtxt)                — 文字高度 ★CRITICAL

--- 箭头/引线 ---
H(dimblk)                 — 箭头块 handle（默认箭头）
H(dimblk1)                — 第一个箭头块 handle
H(dimblk2)                — 第二个箭头块 handle

--- 文字控制 ---
BS(dimadec)               — 角度精度
BS(dimclrd)               — 标注线颜色 ★CRITICAL
BS(dimclre)               — 延伸线颜色 ★CRITICAL
BS(dimclrt)               — 标注文字颜色 ★CRITICAL
BS(dimdec)                — 小数精度
BS(dimfit)                — 拟合选项（R2000 之前用）
BS(dimjust)               — 文字水平对齐方式
BS(dimfrac)               — 分数格式
BS(dimunit)               — 单位格式 ★CRITICAL
BS(dimupt)                — 文字位置更新方式
BS(dimtzin)               — 公差消零
BS(dimazin)               — 主单位消零

--- 线条控制 ---
BS(dimse1)                — 抑制第一条延伸线
BS(dimse2)                — 抑制第二条延伸线
BS(dimsoxd)               — 抑制外部延伸线
BS(dimtad)                — 文字垂直位置（0=centered, 1=above, 4=below）
BS(dimtih)                — 文字在延伸线内时水平放置
BS(dimtoh)                — 文字在延伸线外时水平放置
BS(dimtofl)               — 无文字时绘制标注线
BS(dimtix)                — 强制文字在延伸线内
BS(dimtolj)               — 公差垂直对齐
BS(dimtsz)                — 斜线标记大小（替代箭头）

--- R2007+ 附加 ---
BS(dimfxlon)              — 固定长度延伸线开关
BS(dimfxlon_type)         — 固定长度类型

--- R2010+ handle stream ---
H(dimtxsty_handle)        — 标注文字样式 handle
H(dimldrblk_handle)       — 引线箭头块 handle
H(dimblk_handle)          — 箭头块 handle (R2010+ 通过 handle)
H(owner_handle)
```

★CRITICAL 标记的字段对标注渲染可见性影响最大。

代码映射：`dwg_objects.cpp` parse_dimstyle()（当前为 placeholder skip）

### VPORT (type 65) 字段序列

```
--- Common table prefix ---
RC(class_version)
T(name)

--- VPORT specific ---
BD(view_height)           — 视图高度 ★CRITICAL（决定 viewport 缩放）
2RD(view_center)          — 视图中心 X/Y
3RD(target)               — 目标点 X/Y/Z
BD(view_twist)            — 视图扭转角度（弧度）
BD(view_width)            — 视图宽度

--- 窗口/网格 ---
2RD(lower_left)           — 窗口左下角
2RD(upper_right)          — 窗口右上角
BD(grid_spacing_x)        — 网格间距 X
BD(grid_spacing_y)        — 网格间距 Y
BS(grid_on)               — 网格显示开关
BS(snap_on)               — 捕捉开关
BS(snap_style)            — 捕捉样式
BS(snap_isopair)           — 等轴测捕捉对
BD(snap_angle)            — 捕捉旋转角度
2RD(snap_base)            — 捕捉基点
2RD(snap_spacing)         — 捕捉间距

--- 显示控制 ---
BS(circle_zoom)           — 圆缩放百分比
BD(lens_length)           — 镜头长度
BD(front_clip_z)          — 前裁剪 Z 值
BD(back_clip_z)           — 后裁剪 Z 值
BS(view_mode)             — 视图模式（透视/前裁/后裁/UCS follow）
BS(fast_zoom)             — 快速缩放标志
BS(ucs_icon)              — UCS 图标标志
BS(ucs_icon_at_origin)    — UCS 图标在原点

--- R2000+ ---
BD(elevation)             — 标高
BD(ucs_elevation)         — UCS 标高
BS(shade_plot_mode)        — 着色打印模式
BS(shade_plot_type)        — 着色打印类型

--- Handle stream ---
H(owner_handle)
H(ucs_handle)             — 关联的 UCS
H(base_ucs_handle)        — 基础 UCS
```

★CRITICAL 字段 `view_height` 和 `view_center` 是计算 viewport 缩放比例的核心。

代码映射：`dwg_objects.cpp` parse_vport() (line ~1803-1903)

## Reed-Solomon Error Correction (R2004+)

R2004+ DWG 文件页面使用 Reed-Solomon (RS) 编码进行错误纠正。

### RS 参数

- 域：GF(2^8)，不可约多项式 0x11D
- 块大小：255 字节
- 奇偶校验字节：最多 127 字节
- 实际数据字节 = 255 - parity_bytes
- 每页可以分成多个 RS 块

### 当前项目状态

- 未实现 RS 解码
- R2010 sentinel 文件（big.dwg, Drawing2.dwg）无需 RS 纠错即可正确解析
- LZ77 解压和 CRC 校验已实现

### 实现策略

- **推迟**：当前文件 CRC 正常，无需 RS
- **触发条件**：当遇到 CRC 校验失败且页面数据看起来可恢复时
- **参考实现**：LibreDWG `reedsolomon.c`（GPL，仅参考算法，不可直接引入产品）
- **诊断**：CRC 失败时记录 Encoding gap，注明 RS 可纠正但未实现

### 相关代码

- CRC 校验：`dwg_reader.h` `crc8()`, `crc16()`
- LZ77 解压：`dwg_reader.h` `dwg_decompress()`
- 页面读取：`dwg_parser.cpp` `read_sections()`
