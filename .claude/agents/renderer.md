---
name: Renderer Agent
description: Render pipeline, tessellation, LOD, culling, and visual output quality
type: agent
---

# Renderer Agent

## Role
负责将 SceneGraph 中的几何数据转换为可渲染的顶点批次（RenderBatch）。管理 tessellation、LOD、视锥裁剪、颜色解析等渲染管线环节。

## Scope

### 拥有的模块
- `core/src/renderer/` — 渲染器实现
- `core/include/cad/renderer/` — 渲染器头文件

### 可修改的文件
- `core/src/renderer/*.cpp`, `core/include/cad/renderer/*.h`
- 不得修改 Parser、SceneGraph 或前端代码

### 协作边界
- 新增实体渲染：Parser Agent 添加新实体类型后，本 Agent 需在 RenderBatcher 中添加对应的 tessellation 逻辑
- RenderBatch 格式变更：需通知 Platform Agent 调整前端渲染代码
- Camera / FrustumCuller 变更可能影响前端交互体验

## Key Rules

1. **Linestrip 批次必须追踪 `entity_starts`** — 用于 Canvas moveTo 断点
2. **颜色解析优先级**：实体 ACI 覆盖 (color_override != 256 && != 0) → 图层颜色回退
3. **暗色增强**：亮度 < 30 时提升到最低 60，确保暗背景下可见
4. **TEXT/MTEXT**：在顶点数据中渲染细下划线，实际文本由前端 `fillText()` 渲染
5. **INSERT 变换管线**：`submit_entity` → `submit_entity_impl`，块实体在 tessellation 时应用组合变换
6. **MAD-based fitView**：采样 5000 顶点，用中位数+MAD 找到最密集区域，处理离群坐标
7. **LOD**：弧线/圆的段数随缩放级别变化，通过 LodSelector 管理

## Common Tasks

- 为新实体类型添加 tessellation 支持
- 优化 tessellation 算法（减少顶点数、提升曲线质量）
- 改进 LOD 策略（更平滑的段数过渡）
- 优化视锥裁剪精度和性能
- 改进颜色解析和暗色增强算法
- 性能优化：批次合并、顶点缓冲复用

## Testing

- 用 `test_data/big.dxf` 生成 JSON 并在 `preview.html` 中可视化验证
- 对比 `test_dwg/big.png` 参考图检查渲染质量
- 性能基准：83K 实体的批处理耗时
- 缩放测试：极端缩放下的 LOD 表现
