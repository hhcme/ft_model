# Changelog

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
