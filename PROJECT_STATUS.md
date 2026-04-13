# CAD Rendering Engine — Project Plan & Status

## Overall Architecture

```
SceneGraph → FrustumCuller → LODSelector → RenderBatcher → GfxCommandList
                                ↓
                    Canvas 2D (preview.html)
                    WebGL (WASM, future)
                    Flutter (FFI, future)
```

C++ core outputs RenderBatch with vertex data + topology. Platform renderers consume the batches.

---

## Phase Progress

### Phase 1: DXF Parser + Basic Rendering — DONE

- [x] Project skeleton: CMakeLists, cad_types.h, cad_errors.h
- [x] DXF tokenizer (group code / value reader)
- [x] Section reader base class + all section readers (HEADER, TABLES, BLOCKS, ENTITIES, OBJECTS)
- [x] Header reader: drawing info, extents, ACAD version
- [x] Tables reader: layers, linetypes, text styles, VPORT
- [x] Blocks reader: block definitions with entity index lists
- [x] Entities reader: LINE, CIRCLE, ARC, LWPOLYLINE
- [x] SceneGraph with entity storage, layer management, block lookups
- [x] Entity model: tagged union (std::variant) with 17 entity types
- [x] Math types: Vec3, Matrix4x4, Bounds3d, Color (3D-extendable)
- [x] Camera: pan, zoom, fit_to_bounds
- [x] RenderBatcher: tessellation of lines, circles, arcs, polylines
- [x] LOD selector: adaptive curve segment count
- [x] Canvas 2D preview: preview.html with mouse/touch input
- [x] Frustum culling: per-batch and per-entity
- [x] MAD-based fitView for outlier coordinate handling

### Phase 2: Complete Entity Parsing + Interaction — MOSTLY DONE

- [x] POLYLINE parsing (VERTEX/SEQEND sub-entities)
- [x] INSERT parsing and recursive block expansion with transforms
- [x] HATCH parsing (solid fill + edge-defined boundaries with line/arc edges)
- [x] ELLIPSE parsing (major axis, ratio, parametric angles, rotation)
- [x] SPLINE parsing (fit points, control points, knots)
- [x] TEXT parsing (insertion point, height, rotation, width factor, alignment)
- [x] MTEXT parsing (group code 3 for additional text, code 1 for final portion)
- [x] DIMENSION parsing (definition point, text midpoint, extension line origins)
- [x] POINT parsing and rendering (small cross)
- [x] SOLID parsing and rendering (filled 3/4 vertex polygon)
- [x] Entity boundary tracking (entity_starts / breaks for LineStrip)
- [x] Block definition entity filtering (prevent duplicate rendering)
- [x] INSERT transform pipeline (affine_2d with scale, rotation, translation)
- [x] Matrix composition order fix (row-vector: child * parent)
- [x] INSERT coordinate validation (skip corrupt extreme entities)
- [x] Text rendering in Canvas (fillText with rotation, scaling, MTEXT format stripping)
- [x] Dimension line rendering (dimension line + tick marks)
- [ ] Selection engine (point select, box select) — implemented but not wired to UI
- [ ] Snap engine (endpoint, midpoint, center) — implemented but not wired to UI
- [ ] Gesture handler — implemented but not wired to UI
- [ ] Layer color/brightness management for text visibility
- [ ] Measurement tools (distance, angle, area)

### Phase 3: Performance Optimization — PENDING

- [ ] Arena allocator for parse-time entity allocation
- [ ] String pool for layer/block name deduplication
- [ ] LOD system for adaptive curve tessellation at different zoom levels
- [ ] Quadtree spatial index for O(log N) viewport culling
- [ ] Vertex buffer reuse across frames (incremental update)
- [ ] Batch sorting and merging optimization
- [ ] Target: 500K entities at 60fps

### Phase 4: DWG Support — PENDING

- [ ] DWG binary parser (based on ODA OpenDWG public spec, NOT libredwg — GPL)
- [ ] DWG header parsing
- [ ] DWG section decoder (Header, Class, Object Map, Object Data)
- [ ] DWG entity reader (reuses same SceneGraph)
- [ ] Handle reference system (hex IDs)
- [ ] R2004+ LZ77 decompression
- [ ] CRC checksum validation
- [ ] Target: R2000~R2018 versions

### Phase 5: Product Polish — PENDING

- [ ] SHX font parsing → vector glyph atlas
- [ ] TTF SDF glyph atlas (WebGL) / Flutter native text (Impeller)
- [ ] Measurement tools (distance, angle, area)
- [ ] Bitmap export (high resolution)
- [ ] PDF export
- [ ] Error handling and fault tolerance
- [ ] Electron: WASM build + Emscripten + WebGL backend
- [ ] Flutter: FFI bridge + CadCanvas widget + command buffer renderer
- [ ] Plugin API: unified interface across platforms

---

## Current State (as of latest commit)

| Metric | Value |
|--------|-------|
| Git commits | 6 (on main) |
| Entity types parsed | 17 (all DXF entity types) |
| Entity types rendering | 16 (Dimension = basic lines; no Ray/XLine/Viewport) |
| Big.dxf entities | 83,742 |
| Big.dxf blocks filtered | 41,044 |
| Rendered batches | 55 |
| Total vertices | 394,477 |
| Text entries exported | 3,144 |
| Builds with | CMake, C++20 |
| Remote | gitee.com/smarthhc/ft_model.git |

## Key Files

| File | Purpose |
|------|---------|
| `core/include/cad/cad_types.h` | All math types (Vec3, Matrix4x4, Bounds3d, Color) |
| `core/include/cad/scene/entity.h` | Entity model (tagged union, EntityVariant, bounds funcs) |
| `core/include/cad/scene/scene_graph.h` | Scene container (entities, layers, blocks, vertex buffer) |
| `core/include/cad/parser/dxf_parser.h` | DXF parse entry point |
| `core/include/cad/parser/dxf_entities_reader.h` | Entity type dispatch + all parse_* declarations |
| `core/include/cad/renderer/render_batcher.h` | RenderBatch struct + RenderBatcher class |
| `core/include/cad/renderer/camera.h` | 2D camera (pan, zoom, fit_to_bounds) |
| `core/src/renderer/render_batcher.cpp` | All tessellation + transform logic |
| `core/src/parser/dxf_entities_reader.cpp` | All entity parsing implementations |
| `core/test/test_render_export.cpp` | DXF → JSON export tool |
| `platforms/electron/preview.html` | Canvas 2D viewer with layers, frustum culling |

## Known Issues

1. Some entity text may contain MTEXT formatting codes that need further cleaning
2. Dimension rendering is minimal (tick marks + line) — full dimension geometry not yet implemented
3. Ray/XLine/Viewport rendering is not implemented (entity types parsed but skipped by batcher)
4. No font rendering yet — text rendered as Canvas fillText() with system sans-serif

## Test Data

- `test_data/big.dxf` — 50MB campus masterplan with 83K+ entities (primary test case)
- Reference image: `test_dwg/big.png` (colorful campus plan with roads, buildings, vegetation, water)
- Generated via `scripts/gen_test_dxf.py` using ezdxf (MIT)
