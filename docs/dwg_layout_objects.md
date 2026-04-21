# DWG Layout / Dictionary / Draw Order Objects

本文档记录 DWG 中布局、字典、打印设置和绘制顺序相关对象的二进制布局。

这些对象对 CAD 预览器的 Layout/Paper Space 视图、图框识别和初始 fitView 至关重要。

---

## DICTIONARY 对象

DWG 中的 DICTIONARY 是通用的 name→handle 映射容器，用于组织非图形对象。

### 字段序列

```
--- Common object header ---
RC(class_version)         — 通常为 0

--- DICTIONARY specific ---
BL(num_entries)           — 字典条目数量

--- Per-entry (num_entries ×) ---
T(name)                   — 条目名称（R2007+: string stream）
H(handle)                 — 条目对象 handle

--- Handle stream ---
H(owner_handle)
N × H(reactor_handles)
H(xdicobjhandle)         — 如果 !is_xdic_missing
```

### 关键字典实例

| 字典名 | 内容 | 预览相关性 |
|--------|------|------------|
| `ACAD_LAYOUT` | 所有 LAYOUT 对象的 name→handle | **Critical** |
| `ACAD_PLOTSETTINGS` | PLOTSETTINGS 对象集合 | High |
| `ACAD_GROUP` | GROUP 对象集合 | Low |
| `ACAD_MLINESTYLE` | MLINE 样式 | Low |
| `ACAD_MATERIAL` | 材质定义 | Low |
| `ACAD_COLOR` | 命名颜色 | Low |
| `ACAD_SORTENTS` | SORTENTSTABLE 对象 | High |
| `ACAD_PLOTSTYLENAME` | 打印样式表 | Medium |
| `ACAD_TABLESTYLE` | 表格样式 | Low |
| `ACAD_MLEADERSTYLE` | 多重引线样式 | Medium |

### 布局解析策略

```
1. 定位 ACAD_LAYOUT 字典
2. 遍历字典条目 → 每个 handle 指向一个 LAYOUT 对象
3. 解析每个 LAYOUT → 获取 layout_name, tab_order, paper_size, plot_window, block_record_handle
4. block_record_handle → BLOCK_HEADER → 确定该 layout 的 Model/Paper space
5. tab_order 或当前活动标志 → 确定默认显示哪个 Layout
```

---

## PLOTSETTINGS 对象

PLOTSETTINGS 定义打印配置，LAYOUT 对象继承 PLOTSETTINGS 的所有字段。

### 字段序列

```
--- Common object header ---
RC(class_version)         — 通常为 0

--- PLOTSETTINGS fields ---
T(plotter_name)           — 打印机/绘图仪名称
T(stylesheet)             — CTB/STB 打印样式表名

BD(paper_width)           — 纸张宽度（mm/inches） ★CRITICAL
BD(paper_height)          — 纸张高度 ★CRITICAL

BD(plot_origin_x)         — 打印原点 X ★CRITICAL
BD(plot_origin_y)         — 打印原点 Y ★CRITICAL

BD(plot_window_xmin)      — 打印窗口 X 最小值 ★CRITICAL
BD(plot_window_ymin)      — 打印窗口 Y 最小值 ★CRITICAL
BD(plot_window_xmax)      — 打印窗口 X 最大值 ★CRITICAL
BD(plot_window_ymax)      — 打印窗口 Y 最大值 ★CRITICAL

BS(plot_type)             — 打印范围类型：
  0 = display
  1 = extents
  2 = limits
  3 = view
  4 = window               — 使用 plot_window 坐标
  5 = layout               — 使用 layout margins

BS(plot_rotation)         — 打印旋转：
  0 = 0°
  1 = 90°
  2 = 180°
  3 = 270°

BS(plot_paper_units)      — 纸张单位：
  0 = inches
  1 = mm
  2 = pixels

B(scale_type)             — 比例类型
BD(custom_scale_numerator)   — 自定义比例分子
BD(custom_scale_denominator) — 自定义比例分母 ★CRITICAL

BD(paper_image_origin_x)  — 纸张图像原点 X
BD(paper_image_origin_y)  — 纸张图像原点 Y

BS(standard_scale_type)   — 标准比例类型
BS(scale_factor)          — 比例因子
BS(sheet_units)           — 图纸单位

BD(printable_margin_left)    — 可打印边距左 ★CRITICAL
BD(printable_margin_right)   — 可打印边距右
BD(printable_margin_top)     — 可打印边距上
BD(printable_margin_bottom)  — 可打印边距下

--- Handle stream ---
H(owner_handle)
N × H(reactor_handles)
H(xdicobjhandle)
H(plotview_handle)       — 命名视图 handle
H(visual_style_handle)   — 视觉样式 handle
H(shade_plot_handle)     — 着色打印 handle
```

★CRITICAL 字段用于计算 fitView 的初始视口范围：
- `plot_type=4` 时使用 `plot_window_xmin/ymin/xmax/ymax` 作为绘图范围
- `plot_type=5` 时使用纸张尺寸减去 printable margins
- `custom_scale_numerator/denominator` 计算 viewport 缩放比

---

## LAYOUT 对象

LAYOUT 继承 PLOTSETTINGS 的所有字段，并添加布局特定字段。

### 完整字段序列

```
--- PLOTSETTINGS 继承字段 ---
(所有 PLOTSETTINGS 字段，见上方)

--- LAYOUT 特定字段 ---
T(layout_name)           — 布局名称（如 "Layout1", "Layout2"） ★CRITICAL
BS(tab_order)            — 标签页顺序 ★CRITICAL（确定显示优先级）
BS(flags)                — 布局标志：
  bit 0: model_space (Model Space 标志)
  bit 2: layout_active (当前活动布局)

H(block_record_handle)   — 关联的 BLOCK_HEADER handle ★CRITICAL

--- 布局图形范围 ---
BD(limmin_x)             — 图纸空间界限最小 X
BD(limmin_y)             — 图纸空间界限最小 Y
BD(limmax_x)             — 图纸空间界限最大 X
BD(limmax_y)             — 图纸空间界限最大 Y

BD(ins_base_x)           — 插入基点 X
BD(ins_base_y)           — 插入基点 Y

BD(extmin_x)             — 图纸空间范围最小 X ★CRITICAL
BD(extmin_y)             — 图纸空间范围最小 Y
BD(extmax_x)             — 图纸空间范围最大 X
BD(extmax_y)             — 图纸空间范围最大 Y

BD(elevation)            — 标高

--- UCS ---
3BD(ucs_origin)          — UCS 原点
3BD(ucs_xaxis)           — UCS X 轴方向
3BD(ucs_yaxis)           — UCS Y 轴方向
BS(ucs_type)             — UCS 类型
B(orthographic_type)     — 正交 UCS 类型

--- Handle stream ---
H(owner_handle)          — 所属 ACAD_LAYOUT 字典
N × H(reactor_handles)
H(xdicobjhandle)
H(block_record_handle)   — BLOCK_HEADER handle（与 data stream 中的相同）
H(paper_space_vport)     — 图纸空间视口 handle
H(active_viewport)       — 活动视口 handle
H(ucs_handle)            — UCS handle
H(ucs_base_handle)       — 基础 UCS handle
H(visual_style_handle)   — 视觉样式
H(shade_plot_handle)     — 着色打印
```

### 布局类型识别

```
Model Space:
  flags bit 0 = 1
  layout_name = "Model"
  tab_order = 0
  → block_record_handle 指向 Model Space BLOCK_HEADER

Paper Space Layouts:
  flags bit 0 = 0
  layout_name = "Layout1", "Layout2" 等
  tab_order = 1, 2, ... (确定标签页顺序)
  → block_record_handle 指向该 Layout 的 Paper Space BLOCK_HEADER
```

### fitView 使用

```
优先级（AGENTS.md 规定的标准）：
1. Active Layout 的 plot_window (plot_type=4)
2. Active Layout 的 paper_size - margins
3. Active Layout 的 extmin/extmax
4. Layout viewport 内容的 model space bounds
5. 纯 Model Space 的 finite entity bounds
6. raw scene bounds fallback
```

---

## SORTENTSTABLE 对象（绘制顺序）

SORTENTSTABLE 定义 Paper Space 中对象的绘制顺序（前后关系）。

### 字段序列

```
--- Common object header ---
RC(class_version)

--- SORTENTSTABLE specific ---
BL(num_entries)           — 实体数量

--- Per-entry (num_entries ×) ---
H(entity_handle)          — 实体 handle
H(sort_handle)            — 排序 handle（决定绘制顺序）

--- Handle stream ---
H(owner_handle)           — 通常为 Paper Space 的 BLOCK_HEADER
N × H(reactor_handles)
H(xdicobjhandle)
```

### 绘制顺序语义

- `sort_handle` 值越大的实体绘制在越前面（覆盖后面的）
- 如果实体没有出现在 SORTENTSTABLE 中，使用默认的 object map 顺序
- SORTENTSTABLE 通常只存在于 Paper Space layouts

### 定位方式

```
1. 找到 ACAD_SORTENTS 字典条目 → 获得 SORTENTSTABLE handle
2. 或者通过 BLOCK_HEADER 的 extension dictionary 查找
3. 如果没有 SORTENTSTABLE，使用默认顺序（stable sort by entity order）
```

---

## BLOCK_HEADER (type 49) 对象

BLOCK_HEADER 是 DWG 中最关键的表对象之一，定义了每个 block 的元数据和 handle 引用。

### 字段序列

```
--- Common table prefix ---
RC(class_version)         — 通常为 0
T(name)                   — block 名称

--- BLOCK_HEADER specific ---
B(is_xref)                — 是否外部参照
B(is_xref_resolved)       — 外部参照是否已解析
B(is_xref_dep)            — 是否外部参照依赖
H(xref_handle)            — 外部参照 handle（如果 is_xref）

BS(insert_count)           — 该 block 被 INSERT 引用的次数

--- R2000+ conditional ---
T(xref_pname)             — 外部参照路径（如果 is_xref）
B(anonymous)              — 是否匿名块（*D, *A, *E, *T, *X, *U）
--- R2004+ ---
B(has_attrs)              — 是否包含属性定义
BD(insertion_base_x)      — 插入基点 X（通常 = BLOCK 命令的基点）
BD(insertion_base_y)      — 插入基点 Y
BD(insertion_base_z)      — 插入基点 Z

--- R2007+ ---
T(name)                   — 从 string stream 重新读取

--- Handle stream ---
H(block_handle)           — BLOCK (type 4) 开始标记的 handle
H(endblk_handle)          — ENDBLK (type 5) 结束标记的 handle
--- 如果非匿名且非 xref ---
H(first_entity)           — 第一个实体 handle（0 表示空块）
H(last_entity)            — 最后一个实体 handle
--- R2010+ ---
BL(num_owned)             — 拥有的实体数量
H(first_entity)           — 第一个实体
H(last_entity)            — 最后一个实体
--- 布局相关 ---
H(layout_handle)          — 关联的 LAYOUT handle（仅 Model/Paper space block）
--- R2004+ ---
H(preview_handle)         — 块预览图像 handle
```

### 关键用途

1. **Block 解析**：`block_handle` → `endblk_handle` 之间的实体属于该 block
2. **INSERT 解析**：INSERT 的 handle stream 中 `block_header_handle` → 查找此对象 → 获取 block name → `block_index`
3. **Layout 识别**：`layout_handle` 非空 → 此 BLOCK_HEADER 是 Model/Paper space block record
4. **匿名块识别**：`anonymous=true` + name 前缀 → 判断 `*D`(dimension), `*A`(hatch) 等

### 匿名块命名约定

| 前缀 | 含义 | 生成方式 |
|------|------|----------|
| `*D` | Dimension block | DIMENSION 实体创建，包含标注线/箭头/文字 |
| `*A` | Associative hatch | 关联填充边界 |
| `*E` | Region entity | REGION/SURFACE 命令 |
| `*T` | Table block | ACAD_TABLE 的内部块 |
| `*X` | MINSERT block | MINSERT 的内部重复块 |
| `*U` | User anonymous | 应用/用户创建 |
| `*X` | Xref dependent | 外部参照依赖块 |

### 项目代码映射

- Block name 收集：`dwg_parser.cpp` 中 `entity_handle_to_block_header` map
- INSERT block 引用：`dwg_parser.cpp` parse_objects() INSERT 分支
- Block entity 过滤：`EntityHeader.in_block` flag（block 定义中的实体不直接渲染）
