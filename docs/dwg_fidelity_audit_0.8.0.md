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
| Default view prioritizes Layout/Plot Window, drawing border, viewport content, model bounds, then raw bounds | Partial | `presentation_bounds()` and JSON `presentationBounds/views` exist. Missing full layout semantics now exports `layout_entity_ownership_unresolved` before falling back to finite geometry. |
| Layout Viewport clipping, scale, model content, per-viewport frozen layers | Partial | DWG VIEWPORT entities now expose diagnostic paper/model viewport windows when fields validate, and layout viewport owner handles are linked from LAYOUT refs. Projection remains deferred when `modelCoverage` is low; hidden in-view text/geometry is surfaced through `layer_state_hides_presentation_text` and `layer_state_hides_presentation_geometry`. |
| Drawing border/title block should drive sheet fitting when available | Partial | Presentation view support exists. Native paper-space border/title-block semantics and robust detection remain View gaps. |
| Mechanical drawing visual acceptance includes border, views, dimensions, leaders, balloons, text readability | Partial | `Drawing2.dwg` is the visual sentinel. TEXT/MTEXT rich rendering, proxy Mechanical leaders/bubbles, and 6 inferred detail/source crop frame proxies exist. Native Mechanical/FIELD/AcDs semantics remain Custom object semantic gaps. |
| AutoCAD Mechanical custom object handling | Partial | `ACMDATUMTARGET`, `AMDTNOTE`, `ACDBLINERES`, FIELD/FIELDLIST/MTEXT context graph diagnostics, detail-frame proxy, and `AcDsPrototype` diagnostics exist. Native bubble labels, full FIELD/ContextData graph resolution, and detail/section object semantics remain incomplete. |
| Yellow bubble ordinal proxy policy | Implemented | Ordinal labels are allowed only as visual fallback and are diagnosed as proxy semantics, not golden native labels. |
| Layer/color semantics, TrueColor, ACI, ByLayer, ByBlock | Partial | TrueColor/ACI/ByLayer and some ByBlock propagation exist. Nested inheritance and plot-style effects need more fixtures. |
| Linetype and lineweight | Partial | SceneGraph/JSON can carry linetype/lineweight; Canvas applies simple dash/width when available. DWG LTYPE mapping and complex linetypes are incomplete. |
| Draw order | Partial | Entity/header and batches can carry `draw_order`; renderer uses stable sorting. Native SORTENTS decoding remains missing. |
| Wipeout/mask | Missing | Reported in diagnostics. No first-class Wipeout entity or mask pipeline exists yet. |
| Hatch/fill appearance | Partial | Solid/polygonal hatch rendering exists. Pattern/gradient fidelity remains incomplete. |
| Plot style / CTB / STB / screening | Deferred | Reported as Plot appearance gap. Full CTB/STB evaluation is outside current implementation. |
| Text style, font, SHX/bigfont/TrueType | Partial | Text style indices, MTEXT rect width/height, rich wrapping, inline color/height/underline/paragraph/font family/bold/italic, CAD symbols, and `\U+XXXX` display exist. SHX/bigfont/font fallback diagnostics remain incomplete. |
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
| R2004 / AC1018 | Cataloged fixtures | `新块.dwg` and `好世凤凰城74号302（陈先生）.dwg` parse through the main section/object path; class table and LAYOUT payload remain explicit gaps. |
| R2007 / AC1021 | Cataloged fixtures | `zj-02-00-1.dwg` is routed to the R2007 container branch with a Version gap and UTF-16/AppInfo marker probe; full page/section/object map reader is still pending. |
| R2010 / AC1024 | Implemented sentinel | `big.dwg` covers current large-file site/masterplan behavior. |
| R2013 / AC1027 | Implemented sentinel | `Drawing2.dwg` is the adjacent-version Mechanical/Layout sentinel for R2010 parser changes. |
| R2018+ / AC1032 | Cataloged fixtures | `2026040913_69d73f952f59f.dwg` parses with layout/viewports and remains a smoke sentinel; `泰国网格屏施工图.dwg` is a large performance/catalog candidate. |

## Fixture Catalog

| Fixture | Version | Domain | Acceptance type | Current role |
|---------|---------|--------|-----------------|--------------|
| `test_data/minimal.dxf` | DXF | Synthetic core geometry | Exact | Shared SceneGraph/Renderer baseline. |
| `test_data/insert_blocks.dxf` | DXF | Synthetic INSERT/block | Exact | Transform and block rendering baseline. |
| `test_data/text_entities.dxf` | DXF | Synthetic text | Exact | TEXT/MTEXT export/render baseline. |
| `test_dwg/big.dwg` | R2010/AC1024 | Large site/masterplan | Data + visual sentinel | Large-file parsing, abnormal entity filtering, finite bounds, lower-bound counts. |
| `test_dwg/Drawing2.dwg` | R2013/AC1027 | Mechanical/Layout | Visual + diagnostics sentinel | Paper/layout, drawing border, Mechanical annotations, leaders, balloons, detail frames. Current 0.8.x data baseline: about 26,163 entities, 19 batches, 392,570 vertices, and inferred detail-frame proxies. |
| `test_dwg/zj-02-00-1.dwg` | R2007/AC1021 | Unclassified small drawing | Version catalog gate | Must not throw `Section page map out of bounds`; currently reports `dwg_r2007_container_reader_incomplete` with metadata marker probes. |
| `test_dwg/新块.dwg` | R2004/AC1018 | Block/unclassified | Optional smoke gate | Current baseline parses about 2,302 entities, 45 batches, 253 exported vertices; Classes/LAYOUT gaps remain diagnostic. |
| `test_dwg/好世凤凰城74号302（陈先生）.dwg` | R2004/AC1018 | Residential/unclassified | Catalog candidate | Current baseline parses about 13,618 entities and 38 VPORT records; repeated LAYOUT payload parse gaps show R2004 layout decoding work. |
| `test_dwg/2026040913_69d73f952f59f.dwg` | R2018+/AC1032 | Unclassified/current DWG | Optional smoke gate | Current baseline parses about 12,148 entities with 2 layouts and 2 viewports; Classes partial fallback and layout ownership gaps remain. |
| `test_dwg/张江之尚地下一层图纸.dwg` | R2007/AC1021 | Large architecture/site | Catalog candidate | Requires the same R2007 container reader as `zj-02-00-1.dwg`; keep as a later large-file AC1021 sentinel. |
| `test_dwg/泰国网格屏施工图.dwg` | R2018+/AC1032 | Large construction/grid screen | Catalog candidate | Large current-version fixture for performance and object-family coverage after the small AC1032 gate is stable. |

## Required Acceptance Gates

- Synthetic DXF exact regression must remain stable.
- `big.dwg` remains a lower-bound sentinel for large DWG parsing, abnormal entity filtering, finite JSON bounds, and performance trend checks.
- `Drawing2.dwg` remains a Layout/Paper Space Mechanical sentinel; if full layout or custom-object semantics are absent, JSON diagnostics must say so explicitly.
- Exported JSON must keep legacy fields while supporting `presentationBounds`, `rawBounds`, `views`, `activeViewId`, and `diagnostics`.
- Layout-capable JSON views may include `paperBounds`, `plotWindow`, and `clipBounds`; Electron must consume those fields without breaking model-space fallback drawings.
- `bounds`, `presentationBounds`, and `rawBounds` must contain only finite coordinates.
- DWG issue reports and commits must include version family, object family, affected pipeline stage, gap label, fixtures checked, and diagnostics changed.
