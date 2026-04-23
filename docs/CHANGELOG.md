# Changelog

## [0.10.0] - 2026-04-23

Architecture decomposition, algorithm optimizations, and performance improvements.

### Architecture (Phase 1)
- **dwg_parser.cpp**: 6,459 → 3,160 lines. Extracted R2007 codec, R2004 decoder, diagnostics, header vars, object map, parse helpers, entity geometry, and entity annotation into 8 focused modules.
- **dwg_objects.cpp**: 2,177 → 783 lines. Extracted 16 geometry parsers and 4 annotation parsers into dedicated modules.
- New modules: `dwg_r2007_codec.cpp` (1,068), `dwg_r2004_decoder.cpp` (885), `dwg_entity_geometry.cpp` (865), `dwg_diagnostics.cpp` (567), `dwg_entity_annotation.cpp` (510), `dwg_header_vars.cpp` (388), `dwg_parse_helpers.cpp` (284), `dwg_object_map.cpp` (161).
- Shared utilities extracted to `dwg_entity_common.h` (OCS basis, safe coord checks, entity factory template).

### Algorithms (Phase 2)
- **Quadtree spatial index**: `entities_in_bounds()` now uses O(log N) Quadtree queries when available, with brute-force fallback.
- **FrustumCuller**: Replaced 22-line stub with working implementation delegating to SceneGraph's spatial index.
- **Chord-height error LOD**: Circle/arc segment count uses `N >= π / arccos(1 - T/(R·ppu))` formula (T=0.5px tolerance) instead of `circumference/8` heuristic.
- **Adaptive arc subdivision**: Large screen-space arcs (R*ppu > 1000px) use recursive midpoint subdivision with sagitta error metric.
- **Ellipse adaptive subdivision**: Large ellipses use perpendicular distance from midpoint to chord as error metric.

### Performance (Phase 3)
- **rAF render loop**: Replaced useEffect-triggered full redraws with requestAnimationFrame + dirty flag pattern (60fps).
- **React.memo**: Toolbar, StatusBar, LayerPanel wrapped with memo + custom areEqual comparators.
- **TextMeasureCache**: Frame-level text width cache keyed by font+text, invalidated on zoom change.
- **Web Worker parsing**: JSON decompression + parse moved off main thread via `parseWorker.ts`.

### Output Optimization (Phase 4.3)
- **Vertex format**: Flat arrays `[x0,y0,x1,y1,...]` instead of nested `[[x,y],...]` — ~30% size reduction.
- **Precision**: `%.3g` (3 significant figures) instead of `%.4g` — ~16% additional reduction.
- **Results**: big.dwg 11.0MB→8.7MB raw, 2.0MB→0.2MB gzip. 泰国网格屏施工图 195MB→136MB raw, 42MB→0.9MB gzip.

### DWG Version Coverage
- AC1018 (R2004): 4 fixtures (好世凤凰城, 平立剖, 新块, 设计图纸)
- AC1021 (R2007): 2 fixtures (zj-02-00-1, 张江之尚)
- AC1024 (R2010): 1 fixture (big)
- AC1027 (R2013): 1 fixture (Drawing2)
- AC1032 (R2018+): 2 fixtures (2026040913, 泰国网格屏施工图)
- **Missing**: R2000 (AC1015), R14 (AC1014), R13 (AC1012), R12 (AC1009)

## [0.1.0] - 2026-04-19

Initial DWG/DXF parsing and rendering baseline.

### DWG Parser
- R2004+ file format decoding (header decryption, section page map, LZ77 decompression)
- Object map parsing with accumulator-per-section reset
- Entity type dispatch: LINE, CIRCLE, ARC, TEXT, MTEXT, INSERT/MINSERT, POLYLINE_2D, LWPOLYLINE, SPLINE, HATCH, ELLIPSE, SOLID, POINT, RAY, XLINE, DIMENSION (all 7 subtypes), 3DFACE, VIEWPORT
- Table object dispatch: LAYER, LTYPE, STYLE, DIMSTYLE
- R2004+ CMC color decoding
- INSERT block reference resolution with handle stream parsing
- Block definition tracking and insert_expansion post-processing
- HATCH boundary parsing with line/arc edge tessellation

### DXF Parser
- Full section parsing (HEADER, TABLES, BLOCKS, ENTITIES, OBJECTS)
- All entity types supported via DxfEntitiesReader
- Layer, linetype, style, and block definition tables

### Rendering Engine
- Data-oriented EntityVariant (std::variant, no vtable)
- RenderBatcher with LineList/LineStrip/TriangleList topologies
- INSERT block tessellation cache (per-frame, identity transform)
- Per-block vertex budget (500K) and per-INSERT instance cap (100)
- Gzip output support

### Canvas 2D Preview (preview.html)
- Pan (mouse drag, touch), zoom (wheel, pinch), robust fit view
- Layer panel with visibility toggles and search
- Frustum culling (batch-level AABB)
- Dark background with auto color boost
- TEXT/MTEXT rendering with rotation and width factor
- Adaptive grid rendering
- Gzip decompression via DecompressionStream
- FPS counter and status bar
- DWG/DXF file upload with server-side parsing

### Known Issues (0.1.0)
- **LAYER names empty**: `parse_layer_object` skips name reading (TODO)
- **INSERT over-expansion**: 9MB DWG generates 164M vertices
- **HATCH failures**: ~0.5% of HATCH entities fail parsing
- **No measurement tools**: distance, area, angle not implemented
- **No entity picking**: no click-to-select or property panel
- **No layout switching**: model/paper space not supported
- **Solid fill only**: HATCH pattern lines not implemented
- **No linetype rendering**: linetype names parsed but not applied

## [0.2.0] - Planned

See `.claude/plans/crystalline-snacking-minsky.md` for roadmap.
