# DWG Object Type Code Reference

DWG 对象通过类型码（type number）进行调度。固定类型（0–499）由 DWG 规范预定义，非固定类型（≥500）通过 Classes Section 映射到类名。

主要参考：[Open Design Specification for .dwg files](https://www.opendesign.com/files/guestdownloads/OpenDesign_Specification_for_.dwg_files.pdf)

---

## Fixed Object Types (0–499)

### Entity Types（图形实体）

| Type | DXF Name | Description | 项目状态 |
|------|----------|-------------|----------|
| 1 | TEXT | 单行文字 | Dispatched |
| 2 | ATTRIB | 块属性文字 | Skip (in block) |
| 3 | ATTDEF | 块属性定义 | Skip (in block) |
| 7 | INSERT | 块引用 | Dispatched |
| 8 | MINSERT | 多行多列块引用 | Dispatched |
| 10 | VERTEX_2D | 二维顶点（POLYLINE 子实体） | Dispatched |
| 11 | VERTEX_3D | 三维顶点 | Missing |
| 12 | VERTEX_MESH | 网格顶点 | Missing |
| 13 | VERTEX_PFACE | PFACE 顶点 | Missing |
| 14 | VERTEX_PFACE_FACE | PFACE 面索引 | Missing |
| 15 | POLYLINE_2D | 二维多段线 | Dispatched |
| 16 | POLYLINE_3D | 三维多段线 | Missing |
| 17 | ARC | 圆弧 | Dispatched |
| 18 | CIRCLE | 圆 | Dispatched |
| 19 | LINE | 直线 | Dispatched |
| 20 | DIMENSION_ORDINATE | 坐标标注 | Dispatched (common) |
| 21 | DIMENSION_LINEAR | 线性标注 | Dispatched (common) |
| 22 | DIMENSION_ALIGNED | 对齐标注 | Dispatched (common) |
| 23 | DIMENSION_ANG3PT | 三点角度标注 | Dispatched (common) |
| 24 | DIMENSION_ANG2LN | 两线角度标注 | Dispatched (common) |
| 25 | DIMENSION_RADIUS | 半径标注 | Dispatched (common) |
| 26 | DIMENSION_DIAMETER | 直径标注 | Dispatched (common) |
| 27 | POINT | 点 | Dispatched |
| 28 | 3DFACE | 三维面 | Dispatched |
| 29 | POLYLINE_PFACE | PFACE 多段面 | Dispatched |
| 30 | POLYLINE_MESH | 多边形网格 | Dispatched |
| 31 | SOLID | 实心填充（2D） | Dispatched |
| 32 | TRACE | 轨迹线 | Dispatched |
| 33 | SHAPE | 形（SHX 形引用） | Missing |
| 34 | VIEWPORT | 视口实体 | Dispatched |
| 35 | ELLIPSE | 椭圆 | Dispatched |
| 36 | SPLINE | 样条曲线 | Dispatched |
| 40 | RAY | 射线 | Dispatched |
| 41 | XLINE | 构造线 | Dispatched |
| 44 | MTEXT | 多行文字 | Dispatched |

### Container / Marker Types（流标记）

| Type | DXF Name | Description | 项目状态 |
|------|----------|-------------|----------|
| 0 | UNUSED | 保留/未使用 | — |
| 4 | BLOCK | 块定义开始标记 | Dispatched (marker) |
| 5 | ENDBLK | 块定义结束标记 | Dispatched (marker) |
| 6 | SEQEND | 序列结束标记 | Dispatched (marker) |

### Table Object Types（表对象，非图形）

| Type | DXF Name | Description | 项目状态 |
|------|----------|-------------|----------|
| 48 | BLOCK_CONTROL | 块表控制对象 | Missing |
| 49 | BLOCK_HEADER | 块表头（含 block record 信息） | Partial |
| 50 | LAYER_CONTROL | 图层表控制对象 | Missing |
| 51 | LAYER | 图层 | Dispatched |
| 52 | STYLE_CONTROL | 文字样式表控制 | Missing |
| 53 | STYLE | 文字样式 | Dispatched |
| 54 | — | (reserved) | — |
| 55 | — | (reserved) | — |
| 56 | LTYPE_CONTROL | 线型表控制 | Missing |
| 57 | LTYPE | 线型 | Dispatched |
| 58–64 | — | (reserved / rarely used) | — |
| 65 | VPORT | 视口表 | Dispatched |
| 66 | UCS | 用户坐标系 | Partial |
| 67 | VIEW | 命名视图 | Partial |
| 68 | APPID | 应用程序 ID | Partial |
| 69 | DIMSTYLE | 标注样式 | Partial (skip) |
| 70 | VPENTITY | 视口实体表项 | Missing |

### Other Fixed Types

| Type | DXF Name | Description | 项目状态 |
|------|----------|-------------|----------|
| 37–39 | — | (reserved) | — |
| 42–43 | — | (reserved) | — |
| 45 | LEADER | 引线标注 | Missing |
| 46 | TOLERANCE | 形位公差 | Missing |
| 47 | MLINE | 多线 | Missing |
| 71–76 | — | (reserved) | — |
| 77 | LWPOLYLINE | 轻量多段线 | Dispatched |
| 78 | HATCH | 填充 | Dispatched |

---

## Non-Fixed Type System (type ≥ 500)

DWG 中类型码 ≥ 500 的对象通过 Classes Section 映射到类名。

### 映射机制

```
object_type ≥ 500
    → index = object_type - 500
    → classes[index] → (dxf_name, cpp_name, is_entity, proxy_flags)
```

Classes Section 中每条记录的格式：
```
BL(class_version)
T(dxf_name)          // 如 "ACDBPLACEHOLDER"
T(cpp_name)          // 如 "AcDbPlaceholder"
T(app_name)          // 如 "ObjectDBX Classes"
BS(proxy_flags)      // 位标志
B(is_entity)         // true = 图形实体, false = 非图形对象
BL(class_num)        // 赋予的类型号（= 500 + index）
```

项目代码参考：`dwg_parser.cpp` 的 `parse_classes()` → `m_sections.class_map[type] = {dxf_name, is_entity}`

### 已知类名按产品族分类

#### Standard AutoCAD 类

| Class Name (dxf_name) | is_entity | 预览相关性 | 说明 |
|------------------------|-----------|------------|------|
| `ACDBPLACEHOLDER` | false | Low | 占位对象 |
| `LAYOUT` | false | **Critical** | 布局对象，含 paper size、plot window、viewport handles |
| `PLOTSETTINGS` | false | High | 打印设置，LAYOUT 继承此对象 |
| `SORTENTSTABLE` | false | High | 绘制顺序表 |
| `DICTIONARYWDFLT` | false | Medium | 带默认值的字典 |
| `GROUP` | false | Low | 对象分组 |
| `MLINESTYLE` | false | Low | 多线样式 |
| `ACAD_TABLE` | true | Medium | 表格实体（AutoCAD 表格） |
| `MULTILEADER` | true | **Critical** | 多重引线（机械/建筑标注核心） |
| `MLEADERSTYLE` | false | Medium | 多重引线样式 |
| `WIPEOUT` | true | High | 遮罩/擦除对象 |
| `IMAGE` | true | Medium | 光栅图像实体 |
| `IMAGEDEF` | false | Low | 图像定义（含文件路径） |
| `IMAGEDEF_REACTOR` | false | Low | 图像定义反应器 |
| `OLE2FRAME` | true | Low | OLE 嵌入对象 |
| `ACAD_PROXY_ENTITY` | true | **Critical** | 代理实体（无法识别的实体） |
| `ACAD_PROXY_OBJECT` | false | High | 代理对象（无法识别的非图形对象） |
| `HELIX` | true | Low | 螺旋线 |
| `SUN` | false | Low | 光照对象 |
| `LIGHT` | true | Low | 灯光实体 |
| `PDFDEFINITION` | false | Low | PDF 参考底图定义 |
| `PDFUNDERLAY` | true | Low | PDF 参考底图实体 |
| `DGNDEFINITION` | false | Low | DGN 参考底图定义 |
| `DGNUNDERLAY` | true | Low | DGN 参考底图实体 |
| `DWFDEFINITION` | false | Low | DWF 参考底图定义 |
| `DWFUNDERLAY` | true | Low | DWF 参考底图实体 |
| `RASTER` | false | Low | 光栅图像变量 |
| `SPATIAL_FILTER` | false | Low | 空间过滤器 |
| `SPATIAL_INDEX` | false | Low | 空间索引 |
| `IDBUFFER` | false | Low | 对象 ID 缓冲 |
| `XRECORD` | false | Medium | 扩展记录（存储自定义数据） |

#### AutoCAD Mechanical 类

| Class Name (dxf_name) | is_entity | 预览相关性 | 说明 |
|------------------------|-----------|------------|------|
| `ACMDATUMTARGET` | true | **Critical** | 基准目标符号 |
| `AMDTNOTE` | true | **Critical** | Mechanical 注释/标签 |
| `ACDBLINERES` | true | High | 线性分辨率对象 |
| `ACMDETAIL*` | true | High | 详图视图（类名可变） |
| `ACMSECTION*` | true | High | 剖面视图（类名可变） |
| `ACDBDETAILVIEWSTYLE` | false | Medium | 详图视图样式 |
| `ACDBSECTIONVIEWSTYLE` | false | Medium | 剖面视图样式 |
| `AcDb:AcDsPrototype_*` | false | Medium | AcDs 关联数据原型 |

#### Field / Context Data 类

| Class Name (dxf_name) | is_entity | 预览相关性 | 说明 |
|------------------------|-----------|------------|------|
| `FIELD` | false | **Critical** | 动态字段（日期、对象属性等） |
| `FIELDLIST` | false | High | 字段列表 |
| `ACDB_MTEXTOBJECTCONTEXTDATA_CLASS` | false | High | MTEXT 上下文相关数据 |
| `ACDB_TEXTOBJECTCONTEXTDATA` | false | Medium | TEXT 上下文相关数据 |

#### R2010+ Associative / Parametric 类

| Class Name (dxf_name) | is_entity | 预览相关性 | 说明 |
|------------------------|-----------|------------|------|
| `ACDBASSOCACTION` | false | Low | 关联动作 |
| `ACDBASSOCNETWORK` | false | Low | 关联网络 |
| `ACDBASSOC2DCONSTRAINTGROUP` | false | Low | 二维约束组 |
| `ACDBASSOCGEOMDEPENDENCY` | false | Low | 几何依赖 |
| `ACDBASSOCVALUEDEPENDENCY` | false | Low | 值依赖 |
| `ACDBASSOCALIGNEDDIMACTIONBODY` | false | Low | 对齐标注关联动作 |
| `ACDBASSOCOSNAPPOINTREFACTIONPARAM` | false | Low | 对象捕捉参数 |

#### R2013+ 新增类

| Class Name (dxf_name) | is_entity | 预览相关性 | 说明 |
|------------------------|-----------|------------|------|
| `POINTCLOUD` | true | Low | 点云 |
| `POINTCLOUDDEF` | false | Low | 点云定义 |
| `SECTION` | — | Low | 截面对象 |
| `GEODATA` | false | Low | 地理数据 |

---

## Entity vs Object Dispatch Rules

项目调度逻辑在 `dwg_parser.cpp` 的 `parse_objects()` 循环中：

```
1. prepare_object() → 读取 type/size/CED header，定位 data/handle/string stream
2. 固定类型 (0–499):
   - Container markers (4/5/6): 记录 block 边界，不产生实体
   - Table objects (48–70): parse_table_object() 路径，更新 layer/linetype/style
   - Entity types (1/7/8/10/15/17-19/20-26/27-32/34-36/40/41/44/77/78): parse_dwg_entity()
3. 非固定类型 (≥500):
   - 查 class_map → 获取 (dxf_name, is_entity)
   - is_entity=true: 尝试 parse_custom_entity() 或记录 proxy diagnostics
   - is_entity=false: 记录 object diagnostics
```

### is_graphic vs is_entity

- `is_entity = true`：对象是图形实体，可以渲染（LINE, ARC, MULTILEADER 等）
- `is_entity = false`：对象是非图形对象（LAYER, LAYOUT, DICTIONARY 等），通常不直接渲染但提供语义
- 代理对象的 `proxy_flags` 标志指示原始对象的 is_entity 属性

### 调度参考代码位置

- 固定实体调度：`dwg_objects.cpp` 的 `parse_dwg_entity()` switch 表
- 固定表对象调度：`dwg_parser.cpp` parse_objects() 中的 type 48–70 分支
- 非固定类型 class_map 查询：`dwg_parser.cpp` parse_objects() 中 `m_sections.class_map.find(obj_type)`
