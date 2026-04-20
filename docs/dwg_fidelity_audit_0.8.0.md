# DWG Fidelity Audit 0.8.0

This audit maps the DWG industry rules in `AGENTS.md` to the current implementation state.
Every DWG issue should be classified with both a primary gap label and, when useful, a compatibility sub-label.

## Gap Labels

- Parse gap
- Semantic gap
- Render gap
- View gap
- Plot appearance gap
- Version gap
- Encoding gap
- Object framing gap
- Handle resolution gap
- Custom object semantic gap
- External dependency gap

## Core Audit Matrix

| Rule | Status | Implementation / Gap |
|------|--------|-----------------------|
| Self-developed DWG/DXF parser, SceneGraph, RenderBatcher, export, and platform renderers | Implemented | Product path remains in project C++/TS code. No external DWG-to-DXF converter, GPL CAD parser, or closed SDK is used. |
| DXF remains a first-class product format and exact regression baseline | Implemented | Synthetic DXF fixtures remain in `regression_smoke`; DXF parser still feeds shared SceneGraph and renderer behavior. |
| DWG parser reads binary semantics directly | Implemented | DWG sections, object map, CED, handle streams, and entity data are parsed directly. |
| DWG version compatibility is tracked by release family | Partial | R2010/AC1024 is the best-covered real fixture. R2000/R2004/R2007/R2013/R2018 fixture coverage still needs catalog entries and gates. |
| Header/section/page/object map infrastructure | Partial | R2004+ decryption/decompression and object map parsing exist. Header vars, checksums, and recovered-object diagnostics need broader coverage. |
| Object framing and string/handle stream boundaries | Partial | R2007+ string stream and handle stream parsing exist for current fixtures. Version-family edge cases and richer diagnostics remain needed. |
| EED/XData/reactor/extension dictionary semantics | Partial | EED skip alignment is handled for current files. XData/reactor/extension dictionary semantic preservation is still incomplete. |
| DWG object classification before rendering | Partial | Standard entities and some tables/custom objects are distinguished. Full Standard/Table/Dictionary/Layout/Proxy/Custom/External classification is still incomplete. |
| Model Space, Paper Space, Layout, and Layout Viewport are distinct concepts | Partial | `DrawingSpace`, `Layout`, and expanded `Viewport` exist in SceneGraph. Full DWG Layout object decoding remains a Semantic/View gap. |
| Default view prioritizes Layout/Plot Window, drawing border, viewport content, model bounds, then raw bounds | Partial | `presentation_bounds()` and JSON `presentationBounds/views` exist. Missing full layout semantics still require finite-geometry fallback diagnostics. |
| Layout Viewport clipping, scale, model content, per-viewport frozen layers | Partial | Data fields and JSON/frontend consumption exist. Native DWG viewport clip/scale/layer-freeze decoding remains incomplete. |
| Drawing border/title block should drive sheet fitting when available | Partial | Presentation view support exists. Native paper-space border/title-block semantics and robust detection remain View gaps. |
| Mechanical drawing visual acceptance includes border, views, dimensions, leaders, balloons, text readability | Partial | `Drawing2.dwg` is the visual sentinel. TEXT/MTEXT and proxy Mechanical annotations exist; native Mechanical/FIELD/AcDs semantics remain Custom object semantic gaps. |
| AutoCAD Mechanical custom object handling | Partial | `ACMDATUMTARGET`, `AMDTNOTE`, `ACDBLINERES`, detail-frame proxy, and `AcDsPrototype` diagnostics exist. Native labels, full FIELD/ContextData graph, and detail/section object semantics remain incomplete. |
| Yellow bubble ordinal proxy policy | Implemented | Ordinal labels are allowed only as visual fallback and are diagnosed as proxy semantics, not golden native labels. |
| Layer/color semantics, TrueColor, ACI, ByLayer, ByBlock | Partial | TrueColor/ACI/ByLayer and some ByBlock propagation exist. Nested inheritance and plot-style effects need more fixtures. |
| Linetype and lineweight | Partial | SceneGraph/JSON can carry linetype/lineweight; Canvas applies simple dash/width when available. DWG LTYPE mapping and complex linetypes are incomplete. |
| Draw order | Partial | Entity/header and batches can carry `draw_order`; renderer uses stable sorting. Native SORTENTS decoding remains missing. |
| Wipeout/mask | Missing | Reported in diagnostics. No first-class Wipeout entity or mask pipeline exists yet. |
| Hatch/fill appearance | Partial | Solid/polygonal hatch rendering exists. Pattern/gradient fidelity remains incomplete. |
| Plot style / CTB / STB / screening | Deferred | Reported as Plot appearance gap. Full CTB/STB evaluation is outside current implementation. |
| Text style, font, SHX/bigfont/TrueType | Partial | Text style indices and MTEXT formatting support exist. SHX/bigfont/font fallback diagnostics remain incomplete. |
| Dimension style, Leader/MLeader, annotation scale | Partial | Simplified dimensions and proxy leaders exist. DIMSTYLE, MLeader, balloon/callout native semantics remain incomplete. |
| OCS/UCS/DCS/viewport coordinate semantics | Partial | Several entity OCS fixes exist. Full UCS/DCS/viewport twist/clip semantics remain incomplete. |
| Xref/image/OLE/underlay external dependencies | Missing | Must be diagnosed as External dependency gaps when encountered. No full rendering path yet. |
| Parser diagnostics preserved through export | Partial | SceneGraph diagnostics surface in JSON. Diagnostics need version family, object family, and external dependency detail where known. |
| No fixture-specific special casing | Implemented | Current rules require generic semantics; no filename branch should be added for `big.dwg`, `Drawing2.dwg`, or other fixtures. |

## DWG Version Fixture Matrix

| Version family | Fixture status | Current gate |
|----------------|----------------|--------------|
| R12 / AC1009 | Missing fixture | Deferred with Version gap diagnostics when encountered. |
| R13 / AC1012 | Missing fixture | Deferred with Version gap diagnostics when encountered. |
| R14 / AC1014 | Missing fixture | Deferred with Version gap diagnostics when encountered. |
| R2000 / AC1015 | Missing fixture | Needs real fixture before compatibility can be claimed. |
| R2004 / AC1018 | Missing fixture | Needs real fixture for encrypted/compressed/object-map regression. |
| R2007 / AC1021 | Missing fixture | Needs real fixture for string-stream compatibility. |
| R2010 / AC1024 | Implemented sentinel | `big.dwg` and `Drawing2.dwg` cover current large-file and Mechanical/Layout behavior. |
| R2013 / AC1027 | Missing fixture | Needs adjacent-version regression for R2010 parser changes. |
| R2018+ / AC1032 | Missing fixture | Needs current-version customer-style fixture. |

## Fixture Catalog

| Fixture | Version | Domain | Acceptance type | Current role |
|---------|---------|--------|-----------------|--------------|
| `test_data/minimal.dxf` | DXF | Synthetic core geometry | Exact | Shared SceneGraph/Renderer baseline. |
| `test_data/insert_blocks.dxf` | DXF | Synthetic INSERT/block | Exact | Transform and block rendering baseline. |
| `test_data/text_entities.dxf` | DXF | Synthetic text | Exact | TEXT/MTEXT export/render baseline. |
| `test_dwg/big.dwg` | R2010/AC1024 | Large site/masterplan | Data + visual sentinel | Large-file parsing, abnormal entity filtering, finite bounds, lower-bound counts. |
| `test_dwg/Drawing2.dwg` | Unknown until cataloged | Mechanical/Layout | Visual + diagnostics sentinel | Paper/layout, drawing border, Mechanical annotations, leaders, balloons, detail frames. |
| `test_dwg/zj-02-00-1.dwg` | Unknown | Unclassified | Catalog candidate | Record version/domain/diagnostics before making it a gate. |
| `test_dwg/新块.dwg` | Unknown | Block/unclassified | Catalog candidate | Record version/domain/diagnostics before making it a gate. |

## Required Acceptance Gates

- Synthetic DXF exact regression must remain stable.
- `big.dwg` remains a lower-bound sentinel for large DWG parsing, abnormal entity filtering, finite JSON bounds, and performance trend checks.
- `Drawing2.dwg` remains a Layout/Paper Space Mechanical sentinel; if full layout or custom-object semantics are absent, JSON diagnostics must say so explicitly.
- Exported JSON must keep legacy fields while supporting `presentationBounds`, `rawBounds`, `views`, `activeViewId`, and `diagnostics`.
- Layout-capable JSON views may include `paperBounds`, `plotWindow`, and `clipBounds`; Electron must consume those fields without breaking model-space fallback drawings.
- `bounds`, `presentationBounds`, and `rawBounds` must contain only finite coordinates.
- DWG issue reports and commits must include version family, object family, affected pipeline stage, gap label, fixtures checked, and diagnostics changed.
