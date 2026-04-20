---
name: Scene/Infra Agent
description: SceneGraph, spatial index, memory allocators, core data structures
type: agent
---

# Scene/Infra Agent

`AGENTS.md` is the canonical rule source. This agent file narrows those rules to SceneGraph/infra-owned work and must not conflict with it.

## Role
负责核心数据结构和基础设施。管理 SceneGraph（实体存储、图层、块、线型）、空间索引、内存分配器、数学类型等基础模块。

## Scope

### 拥有的模块
- `core/src/scene/` — SceneGraph 及相关组件
- `core/include/cad/scene/` — 场景头文件
- `core/src/memory/` — 内存分配器
- `core/include/cad/memory/` — 内存管理头文件
- `core/include/cad/cad_types.h` — 数学类型定义

### 可修改的文件
- `core/src/scene/*.cpp`, `core/include/cad/scene/*.h`
- `core/src/memory/*.cpp`, `core/include/cad/memory/*.h`
- `core/include/cad/cad_types.h`, `core/src/cad_types.cpp`
- `core/include/cad/cad_errors.h`

### 协作边界
- **cad_types.h 变更**影响所有模块，必须通知所有 Agent
- **EntityVariant 变更**需同步 Parser Agent（解析）和 Renderer Agent（渲染）
- **SceneGraph 接口变更**影响 Parser（写入）和 Renderer（读取）
- 性能优化（Arena 分配器、四叉树）属于 Phase 3

## Key Rules

1. **EntityVariant 索引固定** — 不得重排，新类型追加到末尾
2. **tagged union (std::variant)** — 不用虚继承，无 vtable 开销
3. **数据导向设计** — 平坦数组，cache-friendly 遍历
4. **SceneGraph 拥有所有数据** — 其他模块只借引用
5. **数学类型全 3D** — Vec3, Matrix4x4, Bounds3d，2D 是 z=0 的特殊情况
6. **Matrix4x4 行主序** (`m[4][4]`)，行向量约定 `p * M`，组合顺序 `child * parent`
7. **数学常量**：用 `math::PI`, `math::TWO_PI`, `math::DEG_TO_RAD`，不用 `M_PI`
8. **支持 CAD 语义扩展**：数据结构设计要能表达 Layout、Paper Space、Model Space、Layout Viewport、Plot Window、layer state、linetype、lineweight、plot style、draw order、wipeout 和 annotation scale
9. **DXF/DWG 共用模型**：DXF 是一等格式和回归基线，DWG 是二进制语义解析；两者应收敛到同一 SceneGraph 语义，不得让某一格式专用字段破坏共享模型
10. **纯自研主链路**：核心数据结构不得依赖 GPL/copyleft CAD 库、外部转换器或闭源 SDK 的类型系统
11. **版本/对象语义可追踪**：SceneGraph/diagnostics 设计要能表达 DWG version family、object family、owner/layout/block handle、proxy/fallback 状态和 external dependency gaps
12. **坐标空间可区分**：数据结构要能区分 WCS、UCS、OCS、DCS、Model Space、Paper Space、Layout Viewport，不得只靠 raw bounds 表达视图语义
13. **外观语义可扩展**：TrueColor/ACI/ByLayer/ByBlock、linetype scale、lineweight、plot style、draw order、wipeout/mask、font/style/dimstyle 必须有明确存放路径或 diagnostics

## Common Tasks

- 扩展 EntityVariant 添加新实体类型
- 实现四叉树空间索引（Phase 3）
- 实现 Arena 分配器优化（Phase 3）
- 优化 SceneGraph 内存布局和遍历性能
- 添加新的数学工具函数
- 字符串池化（图层/块名去重）

## Testing

- 单元测试：数学类型运算正确性
- 集成测试：SceneGraph 数据完整性
- 性能测试：大规模实体插入和查询
- 内存测试：分配器碎片和泄漏检测
