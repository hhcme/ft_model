# DWG 通用解析渲染能力对齐计划

## Context

项目是一个自研 2D CAD 渲染引擎，用于解析和渲染 DWG/DXF 文件。对标产品为**浩辰 CAD 看图王**（2D 阶段）和 HOOPS（未来 3D 阶段）。

当前 big.dwg（R2010/AC1024，9.2MB）的解析结果与参考图 big.png 差距显著：
- **图层全部丢失**：只产出 1 个空名图层（实际应有几十个）
- **INSERT 块展开过度膨胀**：25,290 个 INSERT → 164M 顶点 / 427MB gzip
- **实体类型解析错误率**：HATCH 84/15497 失败，LWPOLYLINE 3/1960 失败
- **前端缺失核心查看功能**：无测量、无坐标显示、无实体拾取、无属性面板

内存审计结论：**无 raw new/delete 泄漏**（全 RAII），但存在 vector 无 reserve 导致的重分配开销，以及 INSERT 展开的峰值内存过高问题。

---

## Phase 1: DWG 解析基础修复（Critical）

### 1.1 修复 LAYER 表对象图层名读取

**根因**（已确认）: `parse_layer_object`（[dwg_objects.cpp:1410-1457](core/src/parser/dwg_objects.cpp#L1410)）在第 1420-1422 行**显式跳过了图层名读取**：

```cpp
// Layer name: R2010 bitstream offset is complex (string stream vs inline TU).
// Skip for now — name will be empty until this is fixed.
std::string name;
```

而同文件的 `parse_ltype_object`（L1476）和 `parse_style_object`（L1527）都通过 `r.read_t()` 正确读取了名称。代码使用的是命名枚举 `DwgVersion::R2010`（值=4），版本阈值比较正确，**不存在 off-by-one 问题**。

**修改**:
- `core/src/parser/dwg_objects.cpp` 第 1420-1422 行 → 改为 `std::string name = r.read_t();`
- 验证 `read_t()` 在 R2010 下是否正确工作（R2010 用 TU 编码，read_t 内部应已有分支处理）

**注意**: 枚举定义在 [dwg_parser.h:110-115](core/include/cad/parser/dwg_parser.h#L110)：
```
R2000=1, R2004=2, R2007=3, R2010=4
```
与 CLAUDE.md 文档记录一致，代码中所有版本比较均使用命名枚举值，无阈值错误。

### 1.2 验证 read_t() 对 R2010 的兼容性

`read_t()` 在 [dwg_reader.h:105](core/include/cad/parser/dwg_reader.h#L105) 声明。需确认：
- R2010 文件 `read_t()` 返回空字符串？还是能正确读取 TU？
- LTYPE 的 `read_t()` 调用（L1476）是否也返回空名？如果是，说明 `read_t()` 本身在 R2010 下不工作

如果 `read_t()` 不工作，需要在 [dwg_reader.cpp](core/src/parser/dwg_reader.cpp) 中实现 R2010+ 的 TU 字符串读取。

### 1.3 修复实体→图层关联

确认实体头解析（`parse_entity_header`）中 layer handle 引用能正确解析到图层名。图层名修复后，实体应自动获得正确的 layerName。

### 1.4 验证

```bash
cmake --build build --target render_export
./build/core/test/render_export test_dwg/big.dwg test_dwg/big_test.json.gz
# 预期: layers 数组包含多个命名图层（如 "0", "标注", "建筑" 等）
# 预期: 实体有正确的 layerName
# 预期: LTYPE 和 STYLE 的名称也不再为空（如 read_t 有问题）
```

---

## Phase 2: INSERT 展开膨胀治理

**问题**: 9.2MB DWG → 164M 顶点 / 427MB gzip。25,290 个 INSERT 实例展开了 62,395 个块定义实体。

### 2.1 审查现有上限是否生效

**文件**: `core/src/renderer/render_batcher.cpp`

- 现有每块 50 万顶点上限（`MAX_BLOCK_VERTICES = 500000`）
- 现有每 INSERT 100 实例上限（`MAX_INSTANCES = 100`）
- 需检查：这些常量是否真的在代码路径中被引用和执行？

### 2.2 新增全局块缓存预算

- 新增全局顶点预算（如 10M 总顶点），超出后跳过新块缓存
- 对超大块（>10 万顶点）直接跳过不渲染

### 2.3 流式批处理输出

**文件**: `core/test/render_export.cpp`

- 当前所有 batch 先积累到内存再写文件，峰值内存 ≈ 输出大小
- 改为每个 batch 完成后立即写入 gzip 流

### 2.4 验证

```bash
./build/core/test/render_export test_dwg/big.dwg test_dwg/big_test.json.gz
# 预期: 顶点数降至 <30M，gzip <100MB
```

---

## Phase 3: 内存管理加固

### 3.1 SceneGraph reserve() 预分配

**文件**: `core/src/scene/scene_graph.cpp`

- 从 DWG object map 大小估算实体数量，解析前 `m_entities.reserve(estimated_count)`
- `m_polyline_verts` 同理

### 3.2 新增 SceneGraph::reset()

- 清空所有 vector 并 `shrink_to_fit()`，用于长期运行查看器

### 3.3 RenderBatcher shrink_to_fit

- `end_frame()` 后对 `m_batches` 调用 `shrink_to_fit()`

---

## Phase 4: 2D 查看核心功能（对标 CAD 看图王）

### 现有功能

preview.html 已实现：
- 平移（鼠标拖拽/触摸）、缩放（滚轮/双指）、Fit View
- 图层面板（列表、搜索、显示/隐藏切换）
- 批次渲染（lines/linestrip/triangles）
- 视锥裁剪（AABB 级别）
- 深色背景 + 暗色增亮
- 文本渲染（TEXT/MTEXT、旋转、宽度缩放）
- 网格渲染（自适应缩放级别）
- FPS 计数器、状态栏
- gzip 解压支持

### 4.1 测量工具（核心缺失）

**文件**: `platforms/electron/preview.html`

| 工具 | 交互 | 实现 |
|------|------|------|
| 距离测量 | 点击两点 | 世界坐标距离，显示标注线和数值 |
| 面积测量 | 多点点击闭合 | Shoelace 公式 |
| 角度测量 | 三点 | 向量夹角 |
| 半径/直径 | 点击圆弧 | 需实体拾取（Phase 4.3） |

### 4.2 坐标显示

- 鼠标移动时状态栏实时显示世界坐标 (X, Y)
- 当前 zoom level、view center 信息已有

### 4.3 实体拾取（点击选择）

- 基于空间索引的点击检测
- 高亮选中实体
- 属性面板显示：类型、图层、颜色、几何参数

### 4.4 布局/模型空间切换

- 解析 LAYOUT 表对象
- Layout 标签页切换 UI

### 4.5 网格捕捉

- 网格捕捉、端点捕捉、中点捕捉
- 捕捉反馈视觉提示

---

## Phase 5: DWG 解析完整性

### 5.1 缺失实体类型

| 实体 | DWG 类型号 | 优先级 |
|------|-----------|--------|
| LEADER（引线） | 45 | 高 |
| MLEADER（多重引线） | 待确认 | 中 |
| TOLERANCE（公差） | 46 | 中 |
| TABLE（表格） | 待确认 | 中 |

### 5.2 HATCH 填充图案

当前只支持 solid fill，需要：
- 预定义图案线（ANSI31 等）
- 渐变填充

### 5.3 线型渲染

- 解析已读取线型名称但未应用
- 需实现虚线、点划线等图案

---

## 执行优先级

1. **Phase 1** — DWG 解析基础修复（阻塞性）
2. **Phase 3** — 内存管理（用户明确关注）
3. **Phase 2** — INSERT 膨胀治理（性能）
4. **Phase 4.1-4.3** — 测量、坐标、拾取（CAD 看图王核心）
5. **Phase 5.1** — LEADER 等实体补全
6. **Phase 4.4-4.5** — 布局切换、捕捉
7. **Phase 5.2-5.3** — 图案填充、线型

## 关键文件

| 文件 | 职责 |
|------|------|
| [dwg_objects.cpp](core/src/parser/dwg_objects.cpp) | 实体/表对象解析（图层名修复） |
| [dwg_reader.h](core/include/cad/parser/dwg_reader.h) / [dwg_reader.cpp](core/src/parser/dwg_reader.cpp) | 位流读取器（read_t / read_tu） |
| [dwg_parser.h](core/include/cad/parser/dwg_parser.h) | DwgVersion 枚举定义 |
| [render_batcher.cpp](core/src/renderer/render_batcher.cpp) | 渲染批处理（INSERT 膨胀治理） |
| [scene_graph.cpp](core/src/scene/scene_graph.cpp) | 场景图（内存管理） |
| [render_export.cpp](core/test/render_export.cpp) | 导出工具（流式输出） |
| [preview.html](platforms/electron/preview.html) | 前端查看器（测量、属性面板） |

## 验证方案

每个 Phase 完成后：
1. `cmake --build build --target render_export` 编译通过
2. 对 big.dwg 运行 render_export，检查 JSON（图层数量、顶点数、文件大小）
3. preview.html 打开与 big.png 参考图对比
4. 检查内存占用
5. 对 test_data/big.dxf 回归测试
