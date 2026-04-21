# DWG External Dependency Protocol

DWG 文件可以引用外部资源：外部参照（Xref）、光栅图像、PDF/DGN/DWF 参考底图、OLE 对象等。

本文档记录这些外部依赖的二进制布局和解析要求。

根据 AGENTS.md 规则，这些外部依赖可能推迟完整实现，但**必须诊断**（External dependency gap）。

---

## Xref（外部参照）

### BLOCK_HEADER Xref 字段

外部参照通过 BLOCK_HEADER 对象标识：

```
B(is_xref)                — 是否外部参照
B(is_xref_resolved)       — 外部参照是否已解析（嵌入到当前 DWG）
B(is_xref_dep)            — 是否外部参照依赖（当前 DWG 被其他 DWG 参照时的符号）
H(xref_handle)            — 外部参照 handle
T(xref_pname)             — 外部参照文件路径（相对/绝对）
```

### Xref 类型

| 类型 | 说明 | 预览行为 |
|------|------|----------|
| Overlay | 覆盖参照，不嵌套传递 | 显示参照内容 |
| Attachment | 附着参照，嵌套传递 | 显示参照内容 |
| Unresolved | 路径无效或文件丢失 | **不渲染**，记录 External dependency gap |
| Unloaded | 用户卸载（不显示） | **不渲染**，但记录 xref 元数据 |

### Xref 路径解析

```
1. BLOCK_HEADER.xref_pname → 相对路径或绝对路径
2. 如果相对路径，相对于宿主 DWG 文件目录
3. 支持的环境变量替换（如 `$(PROJECT)/blocks/door.dwg`）
4. 找不到文件 → External dependency gap diagnostic
```

### 诊断要求

```
遇到 Xref 时必须记录：
- xref_name（BLOCK_HEADER name）
- xref_status（resolved/unresolved/unloaded）
- xref_path（文件路径，如果可用）
- 诊断类别：External dependency gap
```

---

## IMAGE 实体

IMAGE 实体在 DWG 中通过 class "IMAGE"（非固定类型，≥500）引用。

### 字段序列

```
--- Common entity header (CED) ---

--- IMAGE specific ---
BL(class_version)         — 通常为 0
3BD(insertion_point)      — 图像插入点 ★CRITICAL
3BD(u_vector)             — 图像 U 方向向量 ★CRITICAL（像素到世界变换）
3BD(v_vector)             — 图像 V 方向向量 ★CRITICAL
RD(image_width)           — 图像宽度（像素）
RD(image_height)          — 图像高度（像素）
BS(image_display_props)   — 显示属性标志：
  bit 0: show image
  bit 1: show image when not aligned to screen
  bit 2: use clipping boundary
  bit 3: transparency on
B(show_clipping)          — 是否显示裁剪边界

BS(brightness)            — 亮度（0-100）
BS(contrast)              — 对比度（0-100）
BS(fade)                  — 淡出（0-100）

--- Handle stream ---
H(owner_handle)
N × H(reactor_handles)
H(xdicobjhandle)
H(imagedef_handle)        — IMAGEDEF 对象 handle ★CRITICAL
H(imagedef_reactor_handle)— IMAGEDEF_REACTOR handle
```

### IMAGEDEF 对象

IMAGEDEF（class "IMAGEDEF"）包含图像文件路径和分辨率：

```
BL(class_version)
T(file_path)              — 图像文件路径 ★CRITICAL
BD(image_width)           — 图像默认宽度（像素）
BD(image_height)          — 图像默认高度（像素）
B(is_loaded)              — 是否已加载
BD(resolution_units)      — 分辨率单位
BD(resolution_x)          — X 方向分辨率
BD(resolution_y)          — Y 方向分辨率
```

### 预览实现策略

- **最低保真度**：绘制 IMAGE 的矩形轮廓（使用 insertion_point + u/v_vector 计算）
- **中等保真度**：加载图像文件并渲染
- **推迟**：亮度/对比度/淡出/透明度效果
- **必须诊断**：文件路径缺失或图像无法加载 → External dependency gap

---

## WIPEOUT 实体

WIPEOUT（class "WIPEOUT"）是 IMAGE 的子类，用于遮罩底层对象。

### 字段序列

```
与 IMAGE 完全相同的二进制布局。
WIPEOUT 继承 IMAGE 的所有字段。
```

### 渲染语义（关键差异）

```
IMAGE:  渲染光栅图像内容
WIPEOUT: 用背景/图纸颜色填充矩形区域，遮罩底层对象

最低保真度：
- 绘制背景色矩形（使用 insertion_point + u/v_vector 计算）
- 这等效于在绘制顺序中将该区域"擦除"

渲染管线集成：
- WIPEOUT 必须在绘制顺序中正确排序（在 SORTENTSTABLE 中）
- 被遮罩的实体不应显示（或被 WIPEOUT 覆盖）
- 背景/图纸颜色来自当前主题（暗色主题用深色，亮色主题用白色）
```

AGENTS.md 规则：*"Wipeout/mask objects must not be rendered as ordinary outline polygons. Minimum fallback is a background/paper-colored mask."*

---

## PDF / DGN / DWF Underlay

### 对象结构

每种参考底图都有两个对象：定义（DEFINITION）和引用（UNDERLAY）。

| 底图类型 | 定义类 | 引用类 |
|----------|--------|--------|
| PDF | `PDFDEFINITION` | `PDFUNDERLAY` |
| DGN | `DGNDEFINITION` | `DGNUNDERLAY` |
| DWF | `DWFDEFINITION` | `DWFUNDERLAY` |

### DEFINITION 对象

```
T(file_path)              — 外部文件路径
T(name)                   — 定义名称
```

### UNDERLAY 实体

```
3BD(insertion_point)      — 插入点
BD(scale_x)               — X 缩放
BD(scale_y)               — Y 缩放
BD(rotation)              — 旋转角度
BS(display_flags)         — 显示标志
B(show_clipping)          — 裁剪显示
BD(contrast)              — 对比度
BD(fade)                  — 淡出
--- 裁剪边界 ---
BL(num_clip_verts)        — 裁剪顶点数
num_clip_verts × 2RD     — 裁剪多边形顶点
--- Handle stream ---
H(definition_handle)      — 对应 DEFINITION 的 handle
```

### 预览实现策略

- **最低保真度**：绘制底图的矩形轮廓（插入点 + 缩放 + 旋转）
- **中等保真度**：加载外部文件并渲染（PDF 需要 PDF 渲染库）
- **推迟**：对比度/淡出/裁剪效果
- **必须诊断**：外部文件不可用时 → External dependency gap

---

## OLE2FRAME (固定 type 74)

OLE 嵌入对象（如 Excel 表格截图、Word 文档等）。

### 字段序列

```
--- Common entity header ---
3BD(insertion_point)
BD(width)
BD(height)
--- OLE data ---
BL(ole_data_size)
ole_data_size bytes of OLE binary data
```

### 预览实现策略

- **推迟**：OLE 渲染需要 OLE 解析器，预览级可以跳过
- **必须诊断**：记录 OLE 存在但未渲染 → External dependency gap
- OLE 实体可能包含预览图（metafile），但解析复杂度高

---

## 诊断规范

所有外部依赖遇到时必须输出诊断信息：

```json
{
  "code": "external_dependency",
  "category": "External dependency gap",
  "message": "IMAGE entity references unavailable image file",
  "class_name": "IMAGE",
  "count": 3,
  "version_family": "R2010/AC1024"
}
```

诊断字段要求：
- `code`: 外部依赖类型（xref_missing, image_missing, underlay_missing, ole_unsupported）
- `category`: 始终为 "External dependency gap"
- `class_name`: 对象类名
- `count`: 该类未解析的实例数
- `version_family`: DWG 版本族
