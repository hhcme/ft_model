# DWG Version Binary Deltas

DWG 文件格式在每个主要 AutoCAD 版本之间都有二进制结构变化。本文档记录 R2000 到 R2018+ 之间的增量差异。

主要参考：[Open Design Specification for .dwg files](https://www.opendesign.com/files/guestdownloads/OpenDesign_Specification_for_.dwg_files.pdf)

---

## File-Level Version Matrix

| 结构 | R2000 (AC1015) | R2004 (AC1018) | R2007 (AC1021) | R2010 (AC1024) | R2013 (AC1027) | R2018 (AC1032) |
|------|----------------|----------------|----------------|----------------|----------------|----------------|
| **Header encryption** | None | XOR @ 0x80 | XOR @ 0x80 | XOR @ 0x80 | XOR @ 0x80 | XOR @ 0x80 |
| **Section compression** | None | LZ77 | LZ77 | LZ77 | LZ77 | LZ77 |
| **Section map** | Flat sections | Page+Section system | Same as R2004 | Same | Same | Same (optimized) |
| **Object map** | Per-section accumulate | Same | Same | Same | Same | Same |
| **String encoding** | Inline TV (codepage) | Inline TV (codepage) | **Separate string stream (UTF-16)** | Same | Same | Same |
| **Handle stream** | Inline | Inline | Inline | **UMC-encoded size** | Same | Same |
| **BOT encoding** | N/A (fixed types only) | Fixed types | Fixed types | **2-bit code** | Same | Same |
| **CED header** | Full entity header | Full | Full | **Compact (CED)** | Same | Same |
| **CMC color** | BS (ACI index) | **Encoded CMC** | Encoded CMC | Encoded CMC | Encoded CMC | Encoded CMC |
| **EED alignment** | Byte-aligned | Byte-aligned | **Bit-aligned** | Bit-aligned | Bit-aligned | Bit-aligned |
| **RS error correction** | No | Yes (pages) | Yes | Yes | Yes | Yes |
| **Classes Section** | Basic | Extended | Extended | Extended | **Extended (+assoc)** | Extended (+new) |

---

## R2000 (AC1015) — Baseline

R2000 是 DWG 现代格式的基线版本。

### 文件结构
```
[R2000 Header: 6 bytes version + padding]
[Section 0: Header Variables — plain]
[Section 1: Classes — plain]
[Section 2: Object Map — plain handle→offset pairs]
[Section 3+: Object Data — plain entity bytes]
```

### 关键特征
- 无加密、无压缩
- 对象类型只有固定类型（0–499），无非固定类型
- String 全部 inline（TV 编码，单字节 codepage）
- Handle stream 直接嵌入对象数据之后
- CED 使用完整 entity header 格式（非 compact）

### 项目代码映射
- `DwgVersion::R2000 = 1`
- R2000 文件头直接读取，无需 `decrypt_r2004_header()`

---

## R2000 → R2004 Delta

R2004 引入了文件加密、压缩和页面映射系统。

### 新增机制

1. **XOR Header Encryption**（偏移 0x80）
   - 108 字节加密头（`R2004FileHeader`）
   - XOR 密钥从文件前 16 字节派生
   - 包含：section_map_id/address, section_info_id, numsections 等
   - 代码：`dwg_parser.cpp` `decrypt_r2004_header()` + `dwg_reader.cpp` `dwg_decrypt_header()`

2. **Page/Section Map System**
   - `SectionPageMapEntry`：页号 → 大小 + 文件偏移
   - `SectionInfoDesc`：section 描述符 + 页面列表
   - Section 类型：0=Header Vars, 1=Classes, 2=Object Map, 3+=Object Data
   - 代码：`dwg_parser.cpp` `read_section_page_map()` + `read_section_info()`

3. **LZ77 Section Compression**
   - 每个 section page 可压缩（SectionInfoDesc.compressed = 2）
   - 压缩格式：DWG 自定义 LZ77 变体
   - 代码：`dwg_reader.cpp` `dwg_decompress()` + `dwg_decompress_into()`

4. **CMC Encoded Color**
   - R2000 用简单 BS（ACI 索引），R2004+ 用 `read_cmc_r2004()`
   - CMC 格式：BS(index) + conditional BL(rgb) + H(handle) + T(name)
   - 代码：`dwg_reader.cpp` `read_cmc_r2004()`

### 实体字段变化
- 对象数据格式与 R2000 基本兼容
- 新增 EED bit-aligned 读取（非 byte-aligned）

### 项目代码映射
- `DwgVersion::R2004 = 2`
- 解密路径在 `dwg_parser.cpp` 中 `version >= R2004` 分支

---

## R2004 → R2007 Delta

R2007 引入了独立的 string stream 和 UTF-16 编码。

### 新增机制

1. **Separate String Stream**
   - 文本字段（T/TV/TU）不再内联在 data stream 中
   - 而是在 entity data 之后有独立的 string stream 区域
   - `setup_string_stream(bitsize)` 激活分离读取
   - `read_tv()`/`read_tu()` 自动从 string stream 读取
   - 代码：`dwg_reader.cpp` `setup_string_stream()`, `str_read_bs()`, `str_read_bl()`

2. **UTF-16 String Encoding**
   - R2007+ 文本使用 UTF-16LE 编码
   - `read_tu()` 读取 UTF-16LE 并转换为 UTF-8
   - R2007 之前的 TV 编码使用 codepage（$DWGCODEPAGE header variable）

3. **Class Section 变化**
   - Class records 中的文本字段也使用 string stream
   - `is_entity` 判断使用 class_id == 0x1F2（R2007+）vs 直接读取 B（R2004）
   - 代码：`dwg_parser.cpp` `parse_classes()` 中 R2007+ 分支

### 实体字段变化
- Common entity data (CED) 格式可能因 string stream 而不同
- EED/XData 中文本也通过 string stream

### 项目代码映射
- `DwgVersion::R2007 = 3`
- String stream 激活在 `prepare_object()` 中 `version >= R2007`
- `m_is_r2007_plus` flag 控制 TV/TU 行为

---

## R2007 → R2010 Delta

R2010 引入了 BOT 编码和 CED compact header。

### 新增机制

1. **BOT (Bit Object Type) Encoding**
   - R2010 之前：对象类型从固定范围读取
   - R2010+：使用 2-bit code 编码对象类型
     - `00` → type = read RC (raw char, 0–255)
     - `01` → type = read RC + 0x1F0 (0x1F0–0x2EF)
     - `10` → type = read RS (raw short LE, 0–0x7FFF)
     - `11` → (reserved/unused)
   - 代码：`dwg_reader.cpp` `read_bot()` (line 194)

2. **CED (Compact Entity Data) Header**
   - R2010 之前的完整 entity header 字段较多
   - R2010+ 使用 compact 格式：
     ```
     entity_mode (2 bits)
     num_reactors (BL)
     is_xdic_missing (B)
     color (BS + extended flags)
     linetype_scale (BD)
     ltype_flags (2BB)
     plotstyle_flags (2BB)
     material_flags (2BB)        — R2007+
     shadow_flags (RC)           — R2007+
     invisible (BS)
     lineweight (RC)
     ```
   - 代码：`dwg_parser.cpp` `prepare_object()` CED 读取

3. **Handle Stream Size via UMC**
   - R2010+ 对象头包含 handle stream 的 bit size（通过 UMC 编码）
   - 允许精确定位 data stream 和 handle stream 的边界
   - 代码：`dwg_parser.cpp` prepare_object 中的 bit_size 读取

### 实体字段变化
- 大部分实体字段布局与 R2007 相同
- BOT 和 CED 变化在基础设施层处理，不影响实体解析器

### 项目代码映射
- `DwgVersion::R2010 = 4`
- BOT 读取在 `read_bot()` 中
- CED 解析在 `prepare_object()` 中
- 这两个修复是项目 P0 优先级（`dwg-infra.md` Key Tasks）

---

## R2010 → R2013 Delta

R2013 主要新增对象类（associative/parametric），文件格式变化较小。

### 新增对象类

1. **Associative Framework**
   - `ACDBASSOCACTION` — 关联动作基类
   - `ACDBASSOCNETWORK` — 关联网络
   - `ACDBASSOC2DCONSTRAINTGROUP` — 二维约束组
   - `ACDBASSOCGEOMDEPENDENCY` — 几何依赖
   - `ACDBASSOCVALUEDEPENDENCY` — 值依赖
   - `ACDBASSOCALIGNEDDIMACTIONBODY` — 对齐标注关联动作
   - `ACDBASSOCOSNAPPOINTREFACTIONPARAM` — 对象捕捉参数

2. **New Entity Classes**
   - `POINTCLOUD` / `POINTCLOUDDEF` — 点云实体和定义
   - `SECTION` objects — 截面对象

3. **SPLINE 字段变化**
   - 新增 `splineflags` (BL) 和 `knotparam` (BL) 字段
   - 项目已在 `dwg_objects.cpp` 中处理（line ~649）

4. **Header Variables**
   - 新增 `ASSOCSTATUS` 等系统变量

### 实体字段变化
- 固定类型字段布局与 R2010 基本相同
- SPLINE 有少量新增字段
- CED/BOT/handle stream 格式不变

### 项目代码映射
- `DwgVersion::R2013 = 5`
- SPLINE 新增字段已在代码中处理
- 关联对象作为 custom object 通过 class_map 诊断

---

## R2013 → R2018 Delta

R2018 的文件格式变化主要是效率优化，结构基本不变。

### 变化

1. **File Efficiency Improvements**
   - Autodesk 声称优化了打开/保存效率
   - 特别是含大量对象的文件
   - 内部数据布局更紧凑

2. **New System Variables**
   - 少量新增 DWG 系统变量
   - Header Variables section 可能有新字段

3. **Minor Entity Changes**
   - 极少量实体字段调整
   - 大部分固定类型字段布局与 R2013 相同

4. **New Classes**
   - 少量新的 non-fixed 对象类
   - 预览级解析通常不受影响

### 兼容性说明
- R2010 解析器预期可处理 R2018 文件，仅略有数据丢失
- 主要差异在 Classes Section 中新增的类
- 如遇到未知类，通过 proxy diagnostics 报告

### 项目代码映射
- `DwgVersion::R2018 = 6`
- R2018 文件走与 R2010 相同的解密/解压/BOT/CED 路径
- 需要实际 R2018 fixture 验证兼容性（当前 Missing fixture）

---

## CED Header Field Sequence per Version

### R2000 (Full Entity Header)

```
BD(elevation)
2RD(insertion_pt) / 3BD — depends on entity
BD(thickness)
BE(extrusion)
BL(num_reactors)
--- Handle stream ---
H(owner)
N × H(reactors)
H(xdic) if !is_xdic_missing
H(layer)
H(ltype) if ltype_flags require
H(plotstyle) if plotstyle_flags require
```

### R2004 (similar to R2000 with CMC)

```
BD(elevation)
2RD(insertion_pt) / 3BD
BD(thickness)
BE(extrusion)
BL(num_reactors)
B(is_xdic_missing)
CMC(color)               — R2004+: read_cmc_r2004()
BD(linetype_scale)
BB(ltype_flags)
BB(plotstyle_flags)
B(invisible)
RC(lineweight)
--- Handle stream ---
H(owner)
N × H(reactors)
H(xdic) if !is_xdic_missing
H(layer)
H(ltype) if ltype_flags
H(plotstyle) if plotstyle_flags
```

### R2007 (adds material_flags)

```
BD(elevation)
2RD(insertion_pt) / 3BD
BD(thickness)
BE(extrusion)
BL(num_reactors)
B(is_xdic_missing)
CMC(color)
BD(linetype_scale)
BB(ltype_flags)
BB(plotstyle_flags)
BB(material_flags)       — R2007+ 新增
--- Handle stream ---
(same as R2004 + material handle)
```

### R2010+ (Compact CED)

```
2BB(entity_mode)         — R2010+ compact header
BL(num_reactors)
B(is_xdic_missing)
CMC(color)
BD(linetype_scale)
BB(ltype_flags)
BB(plotstyle_flags)
BB(material_flags)
RC(shadow_flags)         — R2007+ (在 R2010 compact 中保留)
BS(invisible)
RC(lineweight)
--- Handle stream ---
H(owner)
N × H(reactors)
H(xdic) if !is_xdic_missing
H(layer)
H(ltype) if ltype_flags
H(plotstyle) if plotstyle_flags
H(material) if material_flags
H(shadow) if shadow_flags
```

---

## Version Fixture Status

| Version | AC Code | Fixture | Parser Path |
|---------|---------|---------|-------------|
| R2000 | AC1015 | **Missing** | 需 fixture 验证 flat section 路径 |
| R2004 | AC1018 | **Missing** | 需 fixture 验证加密/压缩路径 |
| R2007 | AC1021 | **Missing** | 需 fixture 验证 string stream 路径 |
| R2010 | AC1024 | `big.dwg`, `Drawing2.dwg` | 主要开发路径 |
| R2013 | AC1027 | **Missing** | 需 fixture 验证 R2010 parser 兼容性 |
| R2018+ | AC1032 | **Missing** | 需 fixture 验证效率优化兼容性 |
