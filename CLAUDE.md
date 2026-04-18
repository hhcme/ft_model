# CAD Rendering Engine — Project Rules

## Project Overview

Self-developed 2D CAD rendering engine for parsing and rendering DWG/DXF files.
C++20 core engine with Canvas 2D / WebGL / Flutter rendering targets.
Commercial product — no GPL or copyleft dependencies.

**Reference products:** HOOPS, CAD看图王

**Target platforms:**
- Desktop: Electron (Vue + React) — C++ compiled to WASM, WebGL rendering in renderer process
- Mobile: Flutter — C++ via FFI, Flutter Canvas/Impeller native rendering

## Build & Run

```bash
# Build (from project root)
cd build && cmake --build . --target render_export
cd build && cmake --build . --target cad_core

# Generate preview data from DXF/DWG (auto-gzip if output ends with .gz)
./build/core/test/render_export test_data/big.dxf test_data/big.json.gz
./build/core/test/render_export test_dwg/big.dwg test_dwg/big.json.gz

# Serve Canvas 2D preview
python3 -m http.server 8080
# Open: http://localhost:8080/platforms/electron/preview.html?data=/test_dwg/big.json.gz
```

## Architecture Rules

### Entity Model
- **Tagged union (std::variant)**, NOT virtual inheritance. No vtable overhead.
- Flat arrays, data-oriented design for cache-friendly traversal.
- EntityVariant indices are FIXED — do not reorder. New types append at end.
- `PolylineEntity` reused for both POLYLINE and LWPOLYLINE (indices 3 and 4).
- `CircleEntity` extended for ELLIPSE (index 12) with minor_radius, rotation, start/end_angle fields.
- `TextEntity` reused for both TEXT and MTEXT (indices 6 and 7).
- `Vec3` used for Point (index 11).
- `LineEntity` reused for Ray (index 13) and XLine (index 14).
- `Vec3` reused for Viewport (index 15) as placeholder center point.
- `SolidEntity` at index 16 — filled 3/4 vertex polygon.

**EntityVariant index map (DO NOT REORDER):**
| Index | Type | Data Struct |
|-------|------|-------------|
| 0 | Line | LineEntity |
| 1 | Circle | CircleEntity |
| 2 | Arc | ArcEntity |
| 3 | Polyline | PolylineEntity |
| 4 | LwPolyline | PolylineEntity |
| 5 | Spline | SplineEntity |
| 6 | Text | TextEntity |
| 7 | MText | TextEntity |
| 8 | Dimension | DimensionEntity |
| 9 | Hatch | HatchEntity |
| 10 | Insert | InsertEntity |
| 11 | Point | Vec3 |
| 12 | Ellipse | CircleEntity |
| 13 | Ray | LineEntity |
| 14 | XLine | LineEntity |
| 15 | Viewport | Vec3 |
| 16 | Solid | SolidEntity |

### Math Types
- All 3D types (Vec3, Matrix4x4, Bounds3d) — 2D is z=0 special case for future 3D extension.
- `math::PI`, `math::TWO_PI`, `math::DEG_TO_RAD` — use these, NOT `M_PI`.
- Matrix4x4 uses row-major storage (`m[4][4]`). `transform_point` is correct for CPU-side transforms.
- Row-vector convention: `p * M`, so composition requires `child * parent` order.

### SceneGraph
- Owns ALL data. Everything else borrows references.
- Vertex buffer for polyline vertices — offset+count indexing.
- Layers, blocks, linetypes stored as vectors with name→index lookup.

### RenderBatcher
- Outputs `RenderBatch` with topology (LineList/LineStrip/TriangleList), color, vertex_data.
- **entity_starts**: For LineStrip batches, records vertex index where each entity starts. Essential for correct Canvas rendering (moveTo breaks between entities).
- **Transform pipeline**: `submit_entity` → `submit_entity_impl` with Matrix4x4 xform. INSERT block entities get composed transform applied during tessellation.
- **Block cache**: `m_block_cache` (cleared each `begin_frame`). Tessellate once, reuse for all INSERT instances of that block.
- Color resolution: entity ACI override (color_override != 256 && != 0) → layer color fallback.
- TEXT/MTEXT entities: render thin underline in vertex data (actual text rendered by viewer via `fillText()`).
- Output format: raw JSON or gzip (auto-detected by `.gz` suffix). Coordinates written with `%.4g` precision.

### Block / INSERT Handling
- **Block definition entities must be filtered** when iterating — they are rendered ONLY through INSERT references with transforms applied. Use `EntityHeader.in_block` flag.
- **Block tessellation cache**: each unique block definition is tessellated once per frame (identity transform), stored in `m_block_cache` as `block_index → (batches, vertex_count)`. Subsequent INSERTs reuse the cached tessellation, applying transforms on-the-fly.
- **Per-block vertex budget**: blocks whose cached tessellation exceeds 500,000 vertices are skipped — prevents OOM when a complex block is referenced by thousands of INSERTs.
- **Per-INSERT instance cap**: INSERT arrays (M×N) are sampled uniformly when total instances exceed 100 — preserves visual density while bounding memory. Apply `sqrt(MAX_INSTANCES / total)` scaling per dimension.
- INSERT transform: `Matrix4x4::affine_2d(scale, rotation, translation)` composed with parent transform.
- **Row-vector convention:** `insert_xform * translation_2d(offset)` then `block_xform * parent_xform` (child first, then parent).
- Depth limit of 16 for recursive INSERT nesting.
- INSERT coordinate validation: skip entities with coordinates >1e8, scale >1e4, or coord*scale >1e9.

## DXF Parsing Rules

- Each entity type has a dedicated `parse_*` method in `DxfEntitiesReader`.
- POLYLINE entities use VERTEX sub-entities followed by SEQEND — parser must consume the VERTEX loop.
- HATCH boundaries: polyline loops (path_type bit 2) AND edge-defined loops (bit 0) with line/arc edges.
- HATCH edge tessellation: line edges add endpoints directly; arc edges tessellate into segments.
- ELLIPSE: major axis endpoint (group 11/21) relative to center, minor/major ratio (group 40), parametric angles (41/42, already in radians).
- ARC angles in DXF are DEGREES — must convert to radians via `math::radians()` in parser.
- ELLIPSE parametric angles (41/42) are already RADIANS — do NOT convert.
- TEXT/MTEXT: group code 1 = text content; MTEXT group code 3 = additional text lines (prepend before code 1).
- DIMENSION: group codes 10/20/30 = definition point, 11/21/31 = text midpoint, 13/23/33 = ext1 start, 14/24/34 = ext2 start, 70 = type, 50 = rotation, 1 = text.
- SOLID: DXF order is corners 1,2,4,3 → triangles v0-v1-v3 and v1-v2-v3.
- When adding new entity types: add to EntityType enum, EntityVariant, DxfEntitiesReader dispatcher, RenderBatcher.

## Rendering Rules

- Linestrip batches MUST track `entity_starts` for path breaks between entities.
- Preview.html uses `batch.breaks` array to split linestrip rendering into per-entity sub-paths.
- Per-entity frustum culling: check first/last vertex against viewport with 50% margin.
- Dark color boost: brightness < 30 → boost to minimum 60 for dark background visibility.
- LOD: arc/circle segment count varies with zoom level via LodSelector.
- MAD-based fitView: samples 5000 vertices, uses median+MAD to find densest cluster, handles outlier coordinates.
- Text rendering: Canvas `fillText()` with proper world-to-screen transform, Y-flip, rotation, width scaling.
- MTEXT formatting codes: strip `\P` → newline, remove `{\...}` style codes, remove stray braces.

## DWG Parsing Rules

- Reference implementation: libredwg (`/tmp/libredwg/`), used ONLY for spec verification — never copy code (GPL).
- Entity type numbers are HEX in spec but DECIMAL in our dispatch: LINE=19(0x13), ARC=17(0x11), CIRCLE=18(0x12), TEXT=1, INSERT=7, HATCH=78(0x4E), etc.
- **Object map**: per libredwg R2004 decoder, BOTH handle and offset accumulators reset per section.
- **R2004+ CMC (Encoded Color)**: reads `BS(index)` + `BL(rgb)` + `RC(flag)` + conditional text. NOT just BS. Use `read_cmc_r2004()`.
- **R2007+ string stream**: text fields (TV/TU/T) are in a separate string stream, NOT the main entity data stream. Skip text reads when version >= R2007 until string stream is implemented.
- **HATCH edges**: 2RD coordinates use `read_rd()` (raw double), NOT `read_bd()`. Curve_type uses `read_raw_char()` (RC).
- **DIMENSION common fields (R2010)**: `RC(class_version)` → `3BD(extrusion)` → `2RD(text_midpt)` → `BD(elevation)` → `RC(flag1)` → text → BD0×2 → 3BD_1 → BD0 → BS×3 → BD → B×3 → 2RD0. Note `2RD` = raw doubles.
- Table objects (BLOCK_HEADER=49, LAYER=51, LTYPE=57, etc.) are non-graphical — dispatch but produce no geometry.
- BLOCK(4)/ENDBLK(5) are stream markers — dispatch but produce no geometry.
- Entity type dispatch in `dwg_objects.cpp` switch statement; add new types by case number + parser function.
- `DwgVersion` enum: R2004=2, R2007=3, R2010=4.

## Canvas Preview (preview.html) Notes

- Y-axis flip: `sy = -(wy - viewCenterY) * viewZoom + canvas.height / 2`
- Zoom toward mouse position: recalculate center after zoom so cursor stays on same world point.
- Touch input: pinch zoom with two-finger gesture.
- Layer panel: visibility toggles by layer name (frozen layers hidden by default).
- Batch-level frustum culling using precomputed batch bounds (world coords).
- **Gzip support**: preview.html fetches `.json.gz` files using `fetch()` with `Automatic gzip decompression` (`DecompressionStream`) — native browser API, no extra JS library needed.
- `test_dwg/big.json.gz` is generated from `big.dwg` and NOT committed to git (in .gitignore).

## Coding Standards

- C++20 with std::variant. No virtual dispatch for entities.
- MIT/BSD third-party dependencies only.
- All math types in `cad_types.h` — extend there first.
- Header files use `#pragma once`.
- Namespace: `cad::`.
- Member variables: `m_` prefix.
- Include order: corresponding header → project headers → system headers.
- **No special-casing for specific test files.** All fixes must be general algorithmic/math improvements.

## Git Workflow

- Remote: `origin` → `https://gitee.com/smarthhc/ft_model.git`
- Branch: `main` (开发阶段直推，后续可引入 feature 分支)
- Commit style: conventional commits (`feat:`, `fix:`, `refactor:`, `docs:`, `perf:`, `test:`)
- Co-authored commits with Claude: add `Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>`
- Push with: `git push origin main`

### PR / Change Review Standards

- **核心结构变更**（EntityVariant, RenderBatch, SceneGraph 接口）需说明影响范围并通知相关 Agent
- **C++ 性能敏感代码**审查要点：避免不必要的拷贝、关注内存布局和 cache 友好性
- **不得针对特定测试文件特殊处理**，所有修复必须是通用改进
- 变更后须通过 `big.dxf` 回归验证

## Testing

- Use ezdxf (MIT) to generate test DXF files.
- Primary test files: `test_data/big.dxf` (DXF) and `test_dwg/big.dwg` (DWG R2010/AC1024).
- Visual comparison against `test_dwg/big.png` reference image.
- Regression: run `render_export` on both DXF and DWG, verify JSON output and vertex counts.
- Never commit generated JSON files to git (they are in .gitignore).

## Agent Team

项目按模块划分 5 个 Agent 角色，每个 Agent 有独立的指令文件（`.claude/agents/`）。

| Agent | 指令文件 | 负责模块 |
|-------|---------|---------|
| **Parser** | `.claude/agents/parser.md` | DXF/DWG 解析 (`core/src/parser/`) |
| **Renderer** | `.claude/agents/renderer.md` | 渲染管线、tessellation (`core/src/renderer/`) |
| **Scene/Infra** | `.claude/agents/scene-infra.md` | SceneGraph、空间索引、内存、数学类型 (`core/src/scene/`, `core/src/memory/`, `cad_types.h`) |
| **Platform** | `.claude/agents/platform.md` | Canvas/WebGL 前端、WASM 桥接 (`platforms/electron/`, `gfx/`) |
| **QA** | `.claude/agents/qa.md` | 测试、回归、性能基准 (`core/test/`, `scripts/`) |

### 协作规则

1. **模块边界清晰** — 每个 Agent 只修改自己负责的文件，不越界修改其他模块
2. **跨模块变更需协调** — 新增实体类型需 Parser → Scene/Infra → Renderer → Platform 串行配合
3. **变更通知链**：核心结构变更（EntityVariant、RenderBatch 格式、SceneGraph 接口）必须通知所有受影响的 Agent
4. **QA Agent 有只读全局权限** — 可审读任何模块代码，但不得修改生产代码，发现问题报给对应 Agent
