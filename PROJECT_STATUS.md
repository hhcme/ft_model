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
- [x] Robust finite-geometry fitView for outlier coordinate handling

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

### Phase 4: DWG Support — IN PROGRESS

> 0.8.0 reframes DWG progress around AutoCAD-level preview semantics, not raw geometry totals.
> `test_dwg/big.dwg` remains a large-file sentinel, while `test_dwg/Drawing2.dwg` is the Layout/Paper Space mechanical drawing sentinel.
> Current export schema includes `presentationBounds`, `views`, and `diagnostics` so missing Layout/Viewport/Plot semantics are explicit.

#### What Works (0.8.x baseline)

The table below is a historical `big.dwg` parser health snapshot, not a
release contract. DWG correctness is now evaluated by version family, object
family, diagnostics, Layout/Paper Space semantics, and visual acceptance.

| DWG Type | Entity | Dispatched | Success | Rate |
|----------|--------|------------|---------|------|
| 19 | LINE | 39,084 | 39,084 | 100% |
| 7 | INSERT | 25,290 | 25,290 | 100% |
| 77 | LWPOLYLINE | 15,497 | 15,413 | 99.5% |
| 1 | TEXT | 4,986 | 4,986 | 100% |
| 10 | VERTEX_2D | 5,947 | 5,947 | 100% |
| 27 | POINT | 2,715 | 2,715 | 100% |
| 78 | HATCH | 1,368 | 1,359 | 99.3% |
| 17 | ARC | 3,508 | 3,508 | 100% |
| 35 | ELLIPSE | 1,960 | 1,957 | 99.8% |
| 18 | CIRCLE | 1,900 | 1,900 | 100% |
| 44 | MTEXT | 954 | 954 | 100% |
| 22 | DIM_ALIGNED | 358 | 358 | 100% |
| 21 | DIM_LINEAR | 189 | 189 | 100% |
| 36 | SPLINE | 291 | 291 | 100% |
| 31 | SOLID | 176 | 176 | 100% |
| 34 | VIEWPORT | 2 | 2 | 100% |
| 15 | POLYLINE_2D | 241 | 241 | 100%* |
| 4 | BLOCK | 1,079 | 1,079 | 100%* |
| 5 | ENDBLK | 1,079 | 1,079 | 100%* |
| 6 | SEQEND | 329 | 329 | 100%* |
| 2 | ATTRIB | 300 | 300 | 100%* |
| 3 | ATTDEF | 173 | 173 | 100%* |

\* BLOCK, ENDBLK, SEQEND, ATTRIB, ATTDEF, and POLYLINE_2D do not produce SceneGraph entities by design (they are stream markers, sub-entities, or headers whose geometry lives in child VERTEX_2D objects). Success rate measures clean parse completion, not entity creation.

#### Infrastructure Completed

- [x] R2004+ encrypted header decryption
- [x] R2004+ LZ77 section decompression
- [x] Section page map + section info parsing
- [x] Object map: 139 sections → 109,834 handles (per-section offset reset fix)
- [x] R2010 BOT (Bit Object Type) encoding
- [x] R2010+ CED (Compact Entity Data) header consumption
- [x] R2004+ CMC (Encoded Color) reading (BS + BL + RC + conditional text)
- [x] R2007+ string stream extraction (`setup_string_stream`, `read_t`/`read_tu` dispatch)

#### Remaining Work

##### P0 — Versioned Binary Infrastructure

- [x] String stream extraction implemented in `DwgBitReader::setup_string_stream()`
  - Locates `has_strings` flag at bit (bitsize-1)
  - Extracts string data: RS data_size (with extended size support), then string bytes
  - `read_t()` / `read_tu()` / `read_tv()` automatically dispatch to string stream when active
- [x] Text content restored for current R2010 sentinel fixtures.
- [ ] HATCH pattern_name still skipped for R2007+ (needs wire-up in `parse_hatch`)
- [ ] Version-family fixture matrix still incomplete: R2000, R2004, R2007, R2013, and R2018+ need representative DWG fixtures.
- [ ] Header variables, INSUNITS, LIMITS/EXTENTS, current layout hints, EED/XData/reactors, and extension dictionary links need fuller SceneGraph/diagnostics preservation.

##### P1 — Block Resolution (unlocks 25,290 INSERTs)

- [x] BLOCK/ENDBLK (type 4/5) tracking: captures 919 block definitions with entity indices
- [x] BLOCK_HEADER (type 49) inline parsing: stores 1,072 handle→name mappings
- [x] BLOCK entity handle stream reading: computes absolute handles (2,135 entries)
- [x] INSERT handle stream collection with bit-limit enforcement
- [x] Post-processing INSERT resolution: deferred to after all objects parsed (handles ordering)
- [x] Conditional block entity filtering: only active when INSERT expansion is verified
- [ ] Fix BLOCK_HEADER name truncation: stores `*D` instead of `*D1077` for anonymous dimension blocks
- [ ] Apply INSERT transform to block entities for rendering while preserving owner/layout/block/ByBlock semantics
- [ ] Expand missing geometry through correct block/reference semantics; geometry volume is a trend signal, not a goal by itself.

##### P2 — Table Objects & Polish

- [ ] Parse LAYER table objects (type=51) → proper layer names/colors
- [ ] Parse LTYPE table objects (type=57) → linetype dash patterns
- [ ] Parse STYLE table objects (type=53) → text style names
- [ ] DIMSTYLE table (type=69) → dimension style parameters
- [ ] Classes section parsing for custom entity types >= 500
- [ ] LAYOUT/PLOTSETTINGS/VPORT/SORTENTS objects → active layout, plot window, viewport clipping, draw order
- [ ] Proxy/custom object diagnostics → class name, object count, gap category, recovered fallback marker

##### P3 — Layout / Paper Space / Annotation Fidelity

- [x] `Drawing2.dwg` recognized as the Mechanical/Layout/Paper Space visual sentinel.
- [x] Paper background, border candidates, presentation bounds, and diagnostics are present in the preview/export path.
- [ ] Native FIELD/FIELDLIST/ContextData and AutoCAD Mechanical annotation semantics are partial; yellow bubble ordinal fallback is allowed only as a visual fallback and is not golden data.
- [ ] Layout viewport model view center/height/custom scale/twist/clip/frozen layers need complete parsing and export.
- [ ] Leader/MLeader/Balloon/Callout/DIMSTYLE fidelity remains a primary 0.8.x gap.

##### P4 — Edge Cases & Validation

- [x] POLYLINE_2D (type=15) — parsed cleanly; vertices rendered as POINT entities (DWG semantic difference from DXF). Full PolylineEntity assembly requires handle-stream vertex chaining (deferred).
- [x] LWPOLYLINE extrusion bug — fixed: LWPOLYLINE uses `3BD` extrusion, not `BE`. Restored 67 entities / ~368 vertices.
- [ ] LWPOLYLINE 0.5% failure (84/15497) — likely CED color-handle edge cases; low priority
- [ ] HATCH 0.7% failure (9/1368) — post-path data exhaustion on a few large hatches; low priority
- [ ] ELLIPSE 0.2% failure (3/1960) — likely CED edge cases; low priority
- [ ] RAY/XLINE entity parsing (types 40/41)
- [ ] DWG smoke regression in `test_regression_smoke.cpp`

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

## Current Metrics (as of 2026-04-17)

### DXF Performance

| File | Entities | Batches | Vertices |
|------|----------|---------|----------|
| campus_masterplan.dxf | 3,804 | 11 | 21,605 |

### DWG Sentinel Baseline (`test_dwg/big.dwg`, R2010)

| Metric | Value |
|--------|-------|
| Object map handles | 109,834 |
| Parsed entities | ~103,000 lower-bound sentinel |
| Render batches | >= 50 |
| Exported vertices | >= 350,000 lower-bound sentinel |
| Text entries | >= 5,000 lower-bound sentinel |
| Diagnostics | Required for unsupported version/object/layout/render gaps |

### DWG Compatibility / Fidelity Rule Status

| Area | Status | Notes |
|------|--------|-------|
| Pure self-developed DWG/DXF core | Implemented | Product path must not use GPL/copyleft CAD parsers, external DWG→DXF converters, or closed SDKs. |
| DXF as first-class format | Implemented | Synthetic DXF fixtures remain exact regression baselines. |
| Version family matrix | Partial | R2010 sentinel exists; R2000/R2004/R2007/R2013/R2018+ fixtures are currently missing or not cataloged. |
| Binary infra diagnostics | Partial | String stream/CMC/CED are partly implemented; EED/XData/reactors/extension dictionary need fuller diagnostics. |
| Layout/Paper Space | Partial | Presentation bounds and paper view path exist; full Layout/PLOTSETTINGS/Viewport semantics remain incomplete. |
| Mechanical/custom objects | Partial | Proxy/fallback visuals are allowed for `Drawing2.dwg`; native FIELD/Mechanical semantic recovery remains incomplete. |
| Plot appearance | Partial | Basic color/line/text display exists; CTB/STB, full lineweight/linetype, draw order, wipeout/mask need completion. |
| External dependencies | Missing | Xref, raster/image/OLE/PDF/DGN/DWF underlay, fonts, and missing dependency diagnostics need a cataloged pipeline. |

### DWG Progress Timeline

| Date | Handles | Entities | Batches | Vertices | Key Fix |
|------|---------|----------|---------|----------|---------|
| 2026-04-14 | ~850 | ~742 | 3 | 1,526 | Initial DWG scan |
| 2026-04-16 | 4,316 | 224 | 1 | 1,344 | BOT/CED/Classes fixes |
| 2026-04-16 | 4,316 | 742 | 3 | 1,526 | Entity parsers working |
| 2026-04-17 | 109,834 | 103,149 | 66 | 305,635 | Object map per-section offset reset |
| 2026-04-17 | 109,834 | 137,087 | 76 | 401,320 | DIMENSION rewrite + HATCH CMC fix |
| 2026-04-17 | 109,834 | 136,567 | 76 | 399,927 | EED alignment fix + LWPOLYLINE extrusion fix |
| 2026-04-18 | 109,834 | 136,567 | 76 | 399,927 | BLOCK tracking + INSERT handle stream + conditional block filtering |

### Build Info

| Item | Value |
|------|-------|
| Language | C++20 |
| Build system | CMake |
| DWG parser code | 4,821 lines (6 files) |
| Remote | gitee.com/smarthhc/ft_model.git |

---

## Key DWG Technical Decisions

### Object Map Offset Semantics (R2004+)

Per libredwg `read_2004_section_handles`: each section in the object map resets **both** handle and offset accumulators. Our initial code only reset handle, causing offset overflow (842 valid vs 108,992 overflow). Fixed by resetting `last_offset = 0` per section.

### R2004+ CMC (Encoded Color)

CMC in R2004+ binary is NOT just a BS color index. It reads: `BS(index)` + `BL(rgb)` + `RC(flag)` + conditional `T(name/book_name)` from string stream. Our initial `read_cmc()` only read BS, causing 40+ bit drift per CMC field. This was the root cause of HATCH 77% failure rate (gradient fill colors use CMC).

### R2007+ String Stream

For R2007+ (including R2010), text fields (TV/TU/T) reside in a **separate string stream**, not the main entity data stream. The main stream contains only non-text fields. When `has_strings=1` (flag at bit position `bitsize-1`), all `FIELD_T` reads must come from `str_dat` instead of `dat`.

Current status: the shared string stream reader is implemented for current R2007+ sentinel coverage. Remaining work is broader version-family validation, damaged/empty string diagnostics, STYLE/LTYPE/LAYER name coverage, FIELD/FIELDLIST linking, and MTEXT ContextData semantics.

### DIMENSION Common Fields (R2010)

Binary order: `RC(class_version)` → `3BD(extrusion)` → `2RD(text_midpt)` → `BD(elevation)` → `RC(flag1)` → `T(user_text)` → `BD0(text_rot)` → `BD0(horiz_dir)` → `3BD_1(ins_scale)` → `BD0(ins_rot)` → `BS(attachment)` → `BS1(lspace_style)` → `BD1(lspace_factor)` → `BD(act_measurement)` → `B/B/B(unknown/flip)` → `2RD0(clone_ins_pt)`. Note `2RD` = raw doubles (NOT BD).

### EED Alignment Fix

The EED (Extended Entity Data) loop was incorrectly calling `align_to_byte()` before skipping EED raw data bytes. Per libredwg's `bit_read_fixed`, EED data is read **bit-by-bit from the current position** without byte alignment. Aligning caused up to 7 bits of desync per EED iteration, cascading into failures for all subsequent CED fields and entity-specific parsers. Removing `align_to_byte()` restored ~50,000+ entities.

### LWPOLYLINE Extrusion Encoding

LWPOLYLINE's extrusion field is `3BD` (three independent BitDoubles), **not** `BE` (BitExtrusion with a leading B-bit default flag). Using `read_be()` on LWPOLYLINE caused 1-bit desync whenever the first BD's code differed from the B-bit expectation. This was the root cause of the remaining ~151 LWPOLYLINE failures (fixed → 15,413/15,497 success).

### Handle Reference Encoding (R2010)

DWG handle references are encoded as `[code:4][counter:4][counter bytes of data]`. The code determines how to compute the absolute handle:
- Code 0x06: absolute handle (value is the handle)
- Code 0x0C: target = entity_handle + value
- Codes 0x02/0x04/0x08/0x0A: target = entity_handle - value - 1 (subtractive)

INSERT handle streams use code 0x06 (absolute) for BLOCK_HEADER references — raw values match BLOCK_HEADER handles directly. BLOCK entity handle streams use subtractive codes, requiring computation.

### BLOCK_HEADER Name Truncation

BLOCK_HEADER (type 49) stores truncated names for anonymous dimension blocks: `*D` instead of `*D1077`. BLOCK entities (type 4) correctly read the full unique name. Workaround: read BLOCK entity handle streams to build a mapping from BLOCK_HEADER handles to correct BLOCK entity names. Currently produces 2,135 correct mappings out of ~1,072 needed (some handles overlap). INSERT expansion is disabled until this is resolved.

---

## Key Files

### DWG Parser

| File | Lines | Purpose |
|------|-------|---------|
| `core/src/parser/dwg_parser.cpp` | 2,048 | Container: decrypt, sections, object map, CED, prepare_object |
| `core/src/parser/dwg_objects.cpp` | 1,346 | Entity-specific parsers (LINE, ARC, HATCH, DIMENSION, etc.) |
| `core/src/parser/dwg_reader.cpp` | 869 | Bit-level reader (BS/BL/BD/BOT/CMC/MC/UMC) + LZ77 + CRC |
| `core/include/cad/parser/dwg_parser.h` | 219 | DwgParser class, DwgVersion enum, section structs |
| `core/include/cad/parser/dwg_reader.h` | 195 | DwgBitReader class with all DWG encoding methods |
| `core/include/cad/parser/dwg_objects.h` | 38 | Entity dispatch entry point |

### DXF Parser + Renderer

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
| `core/test/test_render_export.cpp` | DXF/DWG → JSON export tool |
| `platforms/electron/preview.html` | Canvas 2D viewer with layers, frustum culling |

---

## Regression Gate

- `core/test/test_regression_smoke.cpp` is the lightweight ongoing gate.
- Exact fixture checks:
  - `test_data/minimal.dxf` → `4 entities / 2 batches / 44 vertices / 0 texts`
  - `test_data/insert_blocks.dxf` → `33 entities / 2 batches / 783 vertices / 0 texts`
  - `test_data/text_entities.dxf` → `7 entities / 1 batch / 14 vertices / 7 texts`
- DWG sentinel lower-bound:
  - `test_dwg/big.dwg` → at least `100,000 entities / 50 batches / 350,000 vertices / 5,000 texts`
  - `test_dwg/Drawing2.dwg` → optional Layout/Paper Space visual sentinel; JSON must include diagnostics when full layout semantics are missing.

## Test Data

- `test_dwg/big.dwg` — R2010/AC1024 campus masterplan (9.6MB, 109K objects, primary DWG test)
- `test_dwg/big.png` — Reference image for visual comparison
- `test_dwg/Drawing2.dwg` — Mechanical/Layout/Paper Space visual sentinel
- `test_dwg/zj-02-00-1.dwg` — DWG fixture catalog candidate; domain and gate pending classification
- `test_dwg/新块.dwg` — DWG fixture catalog candidate for block/reference behavior; gate pending classification
- `test_data/campus_masterplan.dxf` — DXF version of campus plan
- Various synthetic DXF files in `test_data/` generated via ezdxf (MIT)
