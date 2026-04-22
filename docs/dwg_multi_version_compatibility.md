# DWG 多版本兼容通用策略

本文档定义 DWG 解析器如何同时支持多个 AutoCAD 版本族，确保解析代码的可维护性、向前/向后兼容性，以及跨版本回归的稳定性。

**适用范围**：Parser Agent、DWG Infra Agent 及所有 DWG 子 Agent 在编写版本相关代码时必须遵守。

---

## 1. 版本检测与分支架构

### 1.1 单一版本入口原则

- 所有实体解析函数必须接收 `DwgVersion version` 参数。
- 版本分支在**字段级**完成，不在**函数级**拆分（禁止为 R2000/R2004/R2007/R2010 各写一套 `parse_line_*()`）。
- 每个实体类型只保留一个 `parse_*()` 函数，内部用 `if/else` 处理版本差异。

### 1.2 版本常量表

| 枚举值 | 数值 | AC Code | 关键差异 |
|--------|------|---------|----------|
| `R2000` | 1 | AC1015 | 无加密/压缩，Flat Section，无 String Stream |
| `R2004` | 2 | AC1018 | XOR 加密，LZ77 压缩，Page/Section Map，CMC Encoded Color |
| `R2007` | 3 | AC1021 | **独立 String Stream**，UTF-16LE，Class Section 变化 |
| `R2010` | 4 | AC1024 | **BOT 编码**，**CED Compact Header**，Handle Stream Size via UMC |
| `R2013` | 5 | AC1027 | 新增 Associative/Parametric 类，SPLINE 新增字段 |
| `R2018` | 6 | AC1032 | 效率优化，紧凑布局，Classes 新增类 |

**强制规则**：所有版本比较必须使用命名枚举值（`version >= R2007`），禁止出现 `version >= 3` 或 `version >= 0x1021` 这类 magic number。

### 1.3 版本范围判断语义

| 判断方式 | 适用场景 | 示例 |
|----------|----------|------|
| `version >= R2007` | 功能从 R2007 开始新增并延续到后续版本 | String Stream, UTF-16, material_flags |
| `version >= R2004 && version < R2007` | 仅中间版本特有 | R2004/R2010 共用但 R2007 编码不同的字段 |
| `version == R2010` | 仅特定版本引入且后续版本可能变化 | BOT 编码的首次引入点（后续验证） |
| `version <= R2004` | 旧版本兼容路径 | R2000 无 CMC，只有 BS ACI |

**禁止**：基于文件名、对象数量、坐标范围、AC code 字符串前缀做版本分支。

---

## 2. 字段级条件读取模式

### 2.1 三种标准模式

#### 模式 A：版本条件字段（存在/不存在）

字段在某个版本引入，后续版本一直保留：

```
// R2007+ 新增 material_flags (2BB)
if (version >= R2007) {
    uint8_t material_flags = r.read_2bb();
}
// R2007+ 新增 shadow_flags (RC)
if (version >= R2007) {
    uint8_t shadow_flags = r.read_rc();
}
```

#### 模式 B：版本间字段数量差异

字段存在但数量或类型随版本变化：

```
// R2010+ CED 中 num_reactors 是 BL；R2000 是 B 或 BL 视情况而定
// 如果存在数量差异，用条件读取，不假设固定数量
uint32_t num_reactors = 0;
if (version >= R2010) {
    num_reactors = r.read_bl();
} else if (version >= R2000) {
    num_reactors = r.read_b();  // 或 read_bl()，视实体类型而定
}
```

#### 模式 C：版本间编码差异

同一语义字段在不同版本用不同编码：

```
// 字符串：R2007+ UTF-16LE 独立 String Stream；R2004- TV inline codepage
std::string name;
if (version >= R2007) {
    name = r.read_tu();  // UTF-16LE from string stream
} else {
    name = r.read_tv();  // TV from inline bitstream
}

// 颜色：R2004+ Encoded CMC；R2000 简单 BS ACI
Color color;
if (version >= R2004) {
    color = r.read_cmc_r2004();
} else {
    uint16_t aci = r.read_bs();
    color = Color::from_aci(aci);
}
```

### 2.2 BOT 编码的版本隔离

R2010+ 引入 BOT (Bit Object Type) 编码。该编码属于**对象头基础设施**，实体解析器不得依赖它：

```
// 正确：实体解析器只关心 type number，不关心 type 是怎么读出来的
// prepare_object() 已经处理好 BOT，parse_entity() 接收到的 type 是正确的

// 错误：在 parse_* 中判断 "if (version >= R2010) read_bot()"
// BOT 读取必须在 prepare_object() 中完成
```

### 2.3 String Stream 的版本隔离

R2007+ 的 String Stream 属于**基础设施层**：

- `prepare_object()` 负责 `setup_string_stream(bitsize)` 和 String Stream 定位
- 实体解析器中的 `read_t()` / `read_tu()` / `read_tv()` 自动从正确的 stream 读取
- 实体解析器**不得**直接操作 String Stream 的 bit offset

---

## 3. 共享基础设施 vs 分支实现

### 3.1 三层架构

```
┌─────────────────────────────────────────────┐
│  共享基础设施层 (Shared Infrastructure)       │  ← 所有版本通用，无版本分支
│  - Object Map 解析 (handle→offset 映射)       │
│  - Handle 编码/解码 (read_h)                 │
│  - 基本几何类型 (Vec3, Matrix4x4)            │
│  - BitReader 基础操作 (read_b, read_bs, ...) │
├─────────────────────────────────────────────┤
│  版本分支层 (Version-Conditional Layer)       │  ← if/else 处理差异
│  - String Stream 读取 (R2007+)               │
│  - CED Header 格式 (R2010+ compact vs full)  │
│  - CMC Color 编码 (R2004+ vs R2000)          │
│  - BOT Encoding (R2010+)                     │
├─────────────────────────────────────────────┤
│  版本特化层 (Version-Specialized Layer)       │  ← 独立路径，条件进入
│  - R2000 Flat Section 读取路径               │
│  - R2004+ Decrypt/Decompress/LZ77 路径       │
│  - R2007 Container Reader 独立路径           │
│  - R2010+ Section Page Map 路径              │
└─────────────────────────────────────────────┘
```

### 3.2 共享基础设施变更规则

共享层代码变更（如 `read_h()` 的 handle code 解析逻辑）影响**所有版本**，必须：

1. 修改前检查所有版本 fixture 是否可能受影响
2. 修改后全版本回归测试
3. 在 commit message 中标注 `"共享层变更，影响全版本"`
4. 如果变更只应影响某个版本，说明该变更**不应该放在共享层**

### 3.3 版本特化层隔离规则

- R2007 Container Reader 必须是**独立路径**，不得混入 R2004/R2010 的 `decrypt_r2004_header()` 逻辑
- 版本特化层入口必须清晰：`if (version == R2007) { read_r2007_container(); }`
- 版本特化层内部可以复用共享层的 `DwgBitReader`，但 section/page map 解析必须独立

---

## 4. 向前/向后兼容 Fallback 策略

### 4.1 兼容矩阵

| 文件版本 | 解析器支持到 | 策略 |
|----------|-------------|------|
| R2000 | R2018 | 走 R2000 flat section 路径，无需解密/解压 |
| R2004 | R2018 | 走 R2004+ decrypt/decompress 路径，无 string stream |
| R2007 | R2018 | 走 R2007 container 路径，启用 string stream |
| R2010 | R2018 | 走 R2004+ decrypt 路径，启用 BOT/CED |
| R2013 | R2018 | 与 R2010 共享路径，新增类通过 class_map 处理 |
| R2018 | R2018 | 与 R2010 共享路径，验证新增类兼容性 |

### 4.2 六种 Fallback 场景

| 场景 | 处理方式 | 诊断代码 | 级别 |
|------|----------|----------|------|
| **新版本文件遇到旧解析器** | 跳过未知字段，继续解析 | `version_gap_newer_unsupported` | Warning |
| **旧版本文件遇到新解析器** | 用默认值/空值填充缺失字段 | `version_gap_older_default` | Info |
| **版本检测不确定** | 保守走最低兼容路径 | `version_gap_uncertain_fallback` | Warning |
| **字段存在但编码未知** | 读取原始字节跳过，记录字节数 | `encoding_gap_skipped_bytes` | Warning |
| **版本 fixture 缺失** | 标记为 Missing fixture，不声称兼容 | `version_gap_missing_fixture` | Info |
| **版本族边界模糊**（如 R2010 vs R2013） | 走 R2010 路径，验证 R2013 fixture | — | — |

### 4.3 默认值规范

当旧版本缺少新版本字段时，使用以下默认值：

| 字段类型 | 默认值 | 说明 |
|----------|--------|------|
| `B` (bool) | `false` | 如 material_flags 不存在 → 无材质 |
| `BS` (short) | `0` | 如 invisible → 默认可见 |
| `BL` (long) | `0` | 如 num_reactors → 无反应器 |
| `BD` (double) | `0.0` | 如 linetype_scale → 默认比例 1.0（后续修正） |
| `T` (string) | `""` (empty) | 如 style name → 使用默认样式 |
| `H` (handle) | `0` (null) | 如 material handle → 无材质引用 |
| `CMC` (color) | `ACI 256` (ByLayer) | 如 CMC 不存在 → 使用 ByLayer |

**注意**：BD 默认值在实际渲染时可能需要修正（如 linetype_scale 默认应为 1.0 而非 0.0）。默认值仅用于数据占位，渲染层应负责语义修正。

---

## 5. 跨版本回归保护

### 5.1 Fixture 覆盖规则

每个共享层代码变更后，必须验证以下最小 fixture 集合：

| 版本族 | 最小验证 fixture | 验证重点 |
|--------|-----------------|----------|
| R2004 | `新块.dwg` | Section map, decrypt, decompress, 基础实体 |
| R2007 | `zj-02-00-1.dwg` | Container reader, UTF-16, string stream |
| R2010 | `big.dwg` | BOT, CED, handle stream, 大规模实体 |
| R2013 | `Drawing2.dwg` | Layout, Mechanical, viewport, paper space |
| R2018+ | `2026040913_69d73f952f59f.dwg` | 前向兼容性，新类处理 |

### 5.2 回归检查清单

共享层变更后必须检查：

- [ ] 所有现有 fixture 的 JSON 导出成功（不崩溃）
- [ ] `big.dwg` 的 entity count 不下降（或下降有明确原因）
- [ ] `Drawing2.dwg` 的 layout/viewport 诊断不恶化
- [ ] `skip_bits` 不增加（或增加有明确原因并记录）
- [ ] R2007 fixture 不因 R2010 修复而退化
- [ ] R2013 fixture 不因 R2010 修复而退化

### 5.3 禁止行为

- **禁止**用 `big.dwg`（R2010）的行为推断 R2004/R2007/R2013/R2018 的行为
- **禁止**用 `Drawing2.dwg`（R2013）的行为推断其他版本的行为
- **禁止**添加只让某个 fixture 通过但破坏通用性的特殊处理
- **禁止**修改一个版本的解析逻辑时不检查相邻版本 fixture

---

## 6. 版本 Fixture 矩阵管理

### 6.1 当前 Fixture 状态

| 版本族 | AC Code | 最小 Fixture | 完整 Fixtures | 解析路径 | 状态 |
|--------|---------|-------------|--------------|----------|------|
| R2000 | AC1015 | **Missing** | Missing | Flat sections | 未验证 |
| R2004 | AC1018 | `新块.dwg` | `新块.dwg`, `好世凤凰城...` | Decrypt+LZ77 | Partial |
| R2007 | AC1021 | `zj-02-00-1.dwg` | `zj-02-00-1.dwg`, `张江之尚...` | Container (incomplete) | Partial |
| R2010 | AC1024 | `big.dwg` | `big.dwg` | Decrypt+LZ77+BOT+CED | Implemented |
| R2013 | AC1027 | `Drawing2.dwg` | `Drawing2.dwg` | R2010 path + new classes | Implemented |
| R2018+ | AC1032 | `2026040913_...dwg` | `2026040913_...dwg`, `泰国网格屏...` | R2010 path + efficiency | Partial |

### 6.2 Fixture 升级规则

当新增版本 fixture 时：

1. 记录文件名、AC code、文件大小、对象数量预期范围
2. 记录预期解析出的实体类型分布（至少 Top 5）
3. 记录预期诊断（允许的 gap 列表）
4. 首次解析结果作为 baseline 存入 audit，但不作为 golden exact
5. 后续变更后对比 baseline，显著变化必须调查

### 6.3 Missing Fixture 处理

对于没有真实 fixture 的版本族：

- 在 `docs/dwg_fidelity_audit_*.md` 中标记为 **Missing fixture**
- 不得声称该版本族已兼容
- 遇到该版本文件时输出 `version_gap_missing_fixture` 诊断
- 优先使用 synthetic DXF + 手动调整来验证共享层通用性

---

## 7. 常见版本兼容陷阱

### 7.1 陷阱一：版本判断写死

```cpp
// 错误：写死版本号
if (version >= 4) {  // 4 是什么？R2010？还是其他含义？
    ...
}

// 正确：使用命名枚举
if (version >= DwgVersion::R2010) {
    ...
}
```

### 7.2 陷阱二：假设字段顺序不变

```cpp
// 错误：假设 R2004 和 R2010 的字段顺序完全相同
auto a = r.read_bd();
auto b = r.read_bd();
auto c = r.read_bd();  // R2010 这里可能多了/少了字段

// 正确：每个版本分支独立描述字段序列
if (version >= R2010) {
    auto mode = r.read_2bb();  // R2010+ 多了 entity_mode
}
auto a = r.read_bd();
auto b = r.read_bd();
```

### 7.3 陷阱三：String Stream 假设

```cpp
// 错误：R2007+ 直接使用 read_tv()
std::string name = r.read_tv();  // R2007+ 应该走 string stream

// 正确：使用统一的 read_t()，内部自动选择 stream
std::string name = r.read_t();   // read_t 根据版本自动选择 tv/tu
```

### 7.4 陷阱四：Handle 编码假设

```cpp
// 错误：假设 handle 总是 8 字节
uint64_t h = r.read_uint64();

// 正确：使用 read_h()，它正确处理 code+counter 变长编码
Handle h = r.read_h();
```

### 7.5 陷阱五：忽略 EED/XData 版本差异

```cpp
// 错误：统一用 byte-aligned 读取 EED
while (has_eed) {
    r.read_byte_aligned();  // R2007+ 是 bit-aligned！
}

// 正确：版本分支
if (version >= R2007) {
    skip_eed_bit_aligned();
} else {
    skip_eed_byte_aligned();
}
```

---

## 8. Commit / Code Review 规范

### 8.1 版本相关 Commit Message

所有涉及版本兼容的 commit 必须包含：

```
[版本族] [对象族] [影响阶段] [Gap 标签]

- 受影响的版本范围（如：R2010+）
- 变更的字段/编码
- 回归检查的 fixture 列表
- 诊断代码变更

例：
R2010/AC1024, CED Header, parser, Encoding gap
- 修复 CED compact header 的 entity_mode 2BB 读取
- 影响 R2010/R2013/R2018（共享 R2010+ 路径）
- 回归：big.dwg, Drawing2.dwg, 2026040913_69d73f952f59f.dwg
- 新增诊断：ced_entity_mode_mismatch
```

### 8.2 Code Review 检查项

审查版本相关代码时，必须检查：

- [ ] 版本比较使用命名枚举，无 magic number
- [ ] 新增字段有明确的版本条件，无遗漏的 else 分支
- [ ] 默认值是否合理且已记录
- [ ] 是否影响其他版本族的 fixture
- [ ] 诊断代码是否覆盖新增/修改的分支
- [ ] 共享层代码变更是否标注 `"共享层，影响全版本"`

---

## 9. 版本族扩展指南

当 AutoCAD 发布新版本（如 R2024/AC1034）时：

1. **评估差异**：对比 R2018+ 的规范变更，确定是文件格式变更还是仅新增类
2. **选择路径**：
   - 如果仅新增类 → 复用 R2018 路径，更新 class_map 即可
   - 如果文件格式变更 → 新增版本枚举值，创建独立解析路径
3. **获取 fixture**：至少一个真实 fixture 才能声称支持
4. **更新文档**：`dwg_version_deltas.md` + `dwg_multi_version_compatibility.md`
5. **回归验证**：确保新增版本不破坏旧版本

---

## 10. 参考文档

- `docs/dwg_version_deltas.md` — 版本间二进制差异详情
- `docs/dwg_object_type_reference.md` — 对象类型码和类映射
- `docs/dwg_fidelity_audit_*.md` — 当前实现状态审计
- `AGENTS.md` — DWG 版本兼容性标准（顶层规范）
