# CAD Rendering Engine — Project Rules

## Project Overview

Self-developed 2D CAD rendering engine for parsing and rendering DWG/DXF files.
C++20 core engine with Canvas 2D / WebGL / Flutter rendering targets.
Commercial product — no GPL or copyleft dependencies.

**Reference products:** HOOPS, CAD看图王

**Target platforms:**
- Desktop: Electron (Vue + React) — C++ compiled to WASM, WebGL rendering in renderer process
- Mobile: Flutter — C++ via FFI, Flutter Canvas/Impeller native rendering

## Self-Developed Core Policy

- The DWG/DXF parser, SceneGraph, RenderBatcher, export pipeline, and platform renderers are product core code and must remain self-developed.
- Do not use GPL/copyleft CAD parsers, external DWG→DXF converters, or closed/commercial SDKs in the product rendering path.
- MIT/BSD tools may be used for tests, diagnostics, and fixture generation only, such as generating synthetic DXF fixtures.
- DWG support must parse DWG binary semantics directly. Do not implement DWG support by converting DWG to DXF first.
- DXF support is not an external dependency. It is a first-class product input format, a DWG semantic reference, and a regression baseline.

## DXF Role

- DXF remains a supported product format alongside DWG.
- Synthetic DXF fixtures are the exact regression baseline for shared SceneGraph and Renderer behavior.
- Real DWG files are sentinel fixtures for binary parser progress, layout/view fidelity, and large-file behavior.
- DWG entity semantics may be compared against DXF group-code behavior, but the DWG parser must preserve DWG-specific binary layout, handle streams, object maps, and version differences.
- Any DWG change that touches shared entities, rendering, colors, text, blocks, bounds, or export shape must verify DXF regression coverage is still intact.

## Build & Run

```bash
# Build (from project root)
cd build && cmake --build . --target render_export
cd build && cmake --build . --target cad_core
cd build && cmake --build . --target regression_smoke

# Generate preview data from DXF/DWG (generated JSON stays out of git)
./build/core/test/render_export test_data/minimal.dxf /tmp/minimal.json
./build/core/test/render_export test_dwg/big.dwg /tmp/big.json

# Run parser/render smoke regression
./build/core/test/regression_smoke
ctest --test-dir build --output-on-failure -R regression_smoke

# Serve Canvas 2D preview
python3 -m http.server 8080
# Open: http://localhost:8080/platforms/electron/preview.html?data=/tmp/minimal.json
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
- Color resolution: entity ACI override (color_override != 256 && != 0) → layer color fallback.
- TEXT/MTEXT entities: render thin underline in vertex data (actual text rendered by viewer via `fillText()`).

### Block / INSERT Handling
- **Block definition entities must be filtered** when iterating — they are rendered ONLY through INSERT references with transforms applied.
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
- Robust fitView must use finite exported/viewable geometry with outlier-resistant sampling. Layout/Paper Space priority rules below override raw model-space bounds.
- Text rendering: Canvas `fillText()` with proper world-to-screen transform, Y-flip, rotation, width scaling.
- MTEXT formatting/rendering must preserve rich text semantics where available: `\P`, inline color `\C`, height `\H`, underline `\L`, font family/bold/italic from `\f`, stacked text fallback, `\U+XXXX`, `%%c`, `%%d`, and `%%p`. Plain cleaning is only a fallback display path.

## DWG Viewer Fidelity Standard

Target quality is **AutoCAD preview level**, not merely "some geometry is visible".

- Default open view must prioritize the active Layout/Paper Space when available. If the drawing has a border, title block, plot window, or layout viewport, initial fitView should use the paper/layout view.
- Model Space, Paper Space, and Layout Viewport content must not be collapsed into one raw scene bounds for primary preview fitting.
- Layout Viewports must eventually support viewport clipping, viewport scale, model-space content displayed through the viewport, and per-viewport layer frozen/hidden state.
- fitView priority: Layout/Plot Window → drawing border/title block → layout viewport content → model-space main entity bounds → raw scene bounds fallback.
- Mechanical drawing acceptance requires checking that the drawing border is complete and level, main/detail views sit inside the sheet, and dimensions/leaders/balloons/text are readable and located correctly.
- Rendering fidelity includes CAD visual semantics: layer/color resolution, ByLayer/ByBlock inheritance, linetype, lineweight, draw order, wipeout/masking, hatch appearance, text style, dimension style, annotation scale, and plot-style behavior.
- Watermarks from reference screenshots are not a DWG fidelity target and should not drive parser or renderer behavior.

## DWG Version Compatibility Standard

DWG support is version-family work. Any DWG parser, renderer, export, or QA change must state the affected AutoCAD release family and must not infer general DWG behavior from one fixture.

**Version families to track:**

| Family | AC code | Compatibility notes |
|--------|---------|---------------------|
| R12 | AC1009 | Legacy pre-object-map behavior; support may be deferred but must be diagnosed. |
| R13 | AC1012 | Early object model; field layouts differ from later releases. |
| R14 | AC1014 | Legacy object map and table layouts; important for older customer files. |
| R2000 | AC1015 | Modern baseline for many entity layouts and handle semantics. |
| R2004 | AC1018 | Encrypted/compressed sections and R2004 object map rules. |
| R2007 | AC1021 | String stream and R2007+ common object changes. |
| R2010 | AC1024 | CED/common entity header behavior used by current large sentinels. |
| R2013 | AC1027 | Adjacent modern version; R2010 fixes must not break it. |
| R2018+ | AC1032 | Current-file compatibility target; unsupported deltas must be diagnostic. |

For each version family, document and verify: file header/AC code, section/page map, object map encoding, object size/type framing, compression/encryption/CRC, handle stream encoding, string stream encoding, CED/common entity header layout, EED/XData/reactors/extension dictionary, color encoding, table/object/entity field layout, and proxy/custom object behavior.

- Do not use `big.dwg` or any single R2010 file to infer all DWG version behavior.
- All version branches must be based on `DwgVersion`, AC code, section metadata, or object metadata; never on filename, object count, coordinate range, or screenshot appearance.
- Every version-specific branch must document applicable versions, older-version fallback, newer-version fallback, and the diagnostic code used when decoding is incomplete.
- A fix for one family must be regression-checked against adjacent families where fixtures exist; for example R2007 string-stream changes must not break R2004, and R2010 CED changes must not break R2013/R2018.

## DWG Binary Infrastructure Rules

DWG binary infrastructure belongs in the parser infrastructure layer. Entity parsers should consume correctly framed streams; they must not guess section, object, string, or handle boundaries.

- Header variables such as codepage, INSUNITS, LIMMIN/LIMMAX, EXTMIN/EXTMAX, and current model/layout hints must be preserved in SceneGraph metadata or exported diagnostics when available.
- Section/page map parsing must validate boundaries, compressed size, decompressed size, and checksums/CRC when the version provides them.
- Encrypted or compressed sections must be decoded centrally before entity parsing; entity parsers must not bypass infrastructure decoding.
- Object handles and offsets must come from the object map or an explicitly recorded recovered candidate. Recovered objects require diagnostics and must not be treated as normal without traceability.
- Object type, object size, main bit stream, string stream, and handle stream boundaries must be computed by infrastructure. Bit drift must be reported as an Encoding gap or Object framing gap; never swallow it and generate random geometry.
- Handle references must follow code + counter relative/absolute semantics. Owner handles, reactors, extension dictionaries, block records, and layout handles must be retained or diagnosed.
- R2007+ string streams are first-class. TEXT/MTEXT/FIELD/STYLE/LTYPE/LAYER/name fields must use the shared string reader with UTF-16/codepage/error handling.
- CMC/ENC must distinguish ACI, TrueColor, ColorBook, ByLayer, ByBlock, foreground/background, transparency, lineweight, linetype scale, plot style, material, and shadow flags. Unrendered fields still need storage or diagnostics.
- XData/EED/reactors/extension dictionaries must not be skipped in a way that shifts later fields. If skipped, the skip rule and applicable version family must be documented.

## DWG Object Semantics Standard

DWG objects must be classified before deciding whether they draw. Type numbers alone are not enough for AutoCAD-level preview fidelity.

- **Standard Entity**: LINE, ARC, CIRCLE, LWPOLYLINE, SPLINE, HATCH, TEXT, MTEXT, DIMENSION, INSERT, and similar graphical entities. Geometry, layer, color, linetype, lineweight, owner, space, draw order, and finite bounds must reach SceneGraph.
- **Table Object**: LAYER, LTYPE, STYLE, DIMSTYLE, BLOCK_RECORD, VPORT, UCS, VIEW, and related tables. They usually do not render directly but provide renderer/export semantics.
- **Dictionary Object**: DICTIONARY, LAYOUT, PLOTSETTINGS, SORTENTS, GROUP, MLINESTYLE, and similar non-graphical semantic objects.
- **Block/Layout Container**: BLOCK_HEADER, BLOCK begin/end markers, SEQEND, ATTRIB/ATTDEF owner chains, anonymous blocks, model/paper space block records.
- **Proxy Object**: AutoCAD or vertical-product proxy representation. Never silently ignore; export class name/count/gap category diagnostics.
- **Custom Object**: AutoCAD Mechanical/AEC/Plant/Civil or other vertical industry object. If proxy geometry/text is recovered, mark it as proxy/fallback and do not present it as native semantic completion.

AutoCAD Mechanical is a priority custom-object family for engineering drawings. Track and diagnose `ACMDATUMTARGET`, `AMDTNOTE`, `ACDBLINERES`, `ACMDETAIL*`, `ACMSECTION*`, `ACDBDETAILVIEWSTYLE`, `ACDBSECTIONVIEWSTYLE`, `FIELD`, `FIELDLIST`, `ACDB_MTEXTOBJECTCONTEXTDATA_CLASS`, and `AcDb:AcDsPrototype_*`.

Current yellow bubble ordinal labels are allowed only as visual fallback. They must remain diagnosed as a Semantic/Render gap, must be replaced by native FIELD/Mechanical labels once decoded, and must never become exact golden regression data.

## Layout / Paper Space / Viewport Standard

CAD viewer startup behavior should match professional drawing viewers, not a raw geometry debugger.

- Default view priority is fixed: active Layout/Paper Space → Plot Window/Plot Area → Drawing Border/Title Block → Layout Viewport content → Model Space main drawing bounds → finite visible geometry fallback → raw extents fallback.
- Model Space, Paper Space, and Layout Viewport content must not be mixed into one raw bounds for primary fitView.
- LAYOUT objects must be parsed or diagnosed for layout name, active/current layout hints, paper size, plot window, plot rotation, plot origin/offset, printable margins, plot scale, and paper units.
- Layout Viewports must be parsed or diagnosed for paper-space rectangle, model view center/target, view height, custom scale, twist angle, clipping boundary, viewport on/off state, and per-viewport frozen layers.
- Drawing border/title block detection should prefer Paper Space/Layout objects and block references, then obvious closed rectangle/title-block candidates. Never special-case `Drawing2.dwg`.
- Mechanical visual acceptance requires a complete level border, main/detail/section views inside the sheet, readable titles/scales/detail names/balloons/leaders/dimensions, and initial fitView that shows the sheet rather than raw model extents.

## Geometry / Coordinate / Transform Standard

- Keep WCS, UCS, OCS, DCS/view coordinates, Paper Space coordinates, and Model Space coordinates distinct.
- Entities with extrusion/OCS semantics, including CIRCLE, ARC, ELLIPSE, LWPOLYLINE, TEXT, DIMENSION, and HATCH, must be transformed to WCS correctly. Missing/invalid extrusion must fallback with diagnostics, not giant coordinates.
- Block/INSERT transforms must follow the project row-vector convention. Nested INSERT depth limits remain required.
- INSERT/MINSERT/ATTRIB/ATTDEF must preserve owner, block, layout, annotation, and ByBlock inheritance semantics.
- INSUNITS, annotation scale, viewport scale, and plot scale must not be conflated. Paper-space fitView must not rescale model-space geometry.
- Flying-line/outlier filtering must be based on finite coordinates, viewport/layout clipping, semantic validity, and generic abnormal segment rules. It must not use handle allowlists, coordinate blacklists, filenames, or sentinel-specific exceptions.
- Filtered or suppressed geometry must be diagnosable so true long lines or large drawings can be distinguished from corrupt objects.

## Rendering / Plot Appearance Standard

- Color precedence: entity TrueColor → entity ACI → ByBlock inherited color → ByLayer color → plot style override → visible fallback.
- Linetype rendering must carry LTYPE dash pattern, global linetype scale, entity linetype scale, and viewport/paper scale where available. Complex shape/text linetypes may be deferred only with diagnostics.
- Lineweight must map DWG values to display/plot stroke width with a deterministic minimum-visible-width strategy.
- Draw order from SORTENTS or equivalent object order is first-class. Hatch, wipeout, masks, solids, text, and dimensions must be stable-sorted.
- Hatch/fill support must distinguish solid, pattern, and gradient. Missing pattern/gradient fidelity requires diagnostics.
- Wipeout/mask objects must not be rendered as ordinary outline polygons. Minimum fallback is a background/paper-colored mask.
- CTB/STB, screening, plot color, and plot lineweight overrides may be deferred, but must be reported as Plot appearance gaps.
- Paper mode applies only to Layout/Paper Space views; model-only drawings should not be forced onto paper background.

## Text / Font / Annotation Standard

- TEXT parsing/rendering must preserve insertion point, alignment point, horizontal/vertical alignment, rotation, oblique angle, width factor, height, generation/mirror flags, OCS/extrusion, and style reference.
- MTEXT must preserve attachment, direction/x-axis, rect width/height, line spacing, inline color `\C`, height `\H`, underline `\L`, paragraph `\P`, font blocks `\f`, stacked/fraction text fallback, and brace scoping.
- Font handling must retain TrueType font names, SHX fonts, and bigfont references. Missing fonts use deterministic fallback plus diagnostics.
- Common CAD symbols such as `%%c`, `%%d`, `%%p`, Unicode, Chinese text, and mechanical symbols must display correctly.
- DIMENSION should prefer anonymous dimension block graphics. Simplified dimension fallback is allowed only when the anonymous block is absent/unresolved and must be diagnosed.
- DIMSTYLE fields such as text height, arrow size, scale factor, unit format, and precision should feed SceneGraph/export as they become available.
- Leader/MLeader/Balloon/Callout require leader paths, arrowheads, landing/dogleg, content text/block, and bubble/callout semantics. Mechanical datum/callout proxy is fallback only.
- FIELD/FIELDLIST/MTEXT context data must be linked by handle graph before declaring text missing; FIELD is semantic content, not ordinary literal text.

## Layers / Xref / External Dependencies Standard

- Layer state includes on/off, frozen/thawed, locked, plottable/non-plottable, per-viewport frozen, color, linetype, lineweight, and plot style.
- Off/frozen layers are hidden by default; locked layers still display. Non-plottable layers display in viewer mode but may be hidden in explicit plot-preview mode.
- Xref block/path/resolution state must be diagnosed. Missing or unloaded xrefs must not crash or generate placeholder flying geometry.
- Raster IMAGE, PDF/DGN/DWF underlay, OLE, and other external dependencies may be deferred, but must be reported as External dependency gaps.
- Layer filters/states may be deferred, but must not break base layer visibility.

## Diagnostics / Gap Taxonomy

Use these labels when describing DWG regressions, diagnostics, plans, commits, and tests:

- **Parse gap**: an object is missing from SceneGraph.
- **Semantic gap**: an object exists, but space, block reference, coordinate system, viewport, scale, or style semantics are wrong.
- **Render gap**: an object exists with correct data, but linetype, lineweight, fill, text, dimension, mask, draw order, or color output is wrong.
- **View gap**: default bounds/fitView does not follow Layout/Paper Space/Viewport rules.
- **Plot appearance gap**: CTB/STB, plot style, lineweight, paper/background, screening, or plotted color behavior is not represented.
- **Version gap**: a version-family field layout or encoding difference is unsupported.
- **Encoding gap**: string/color/handle/CMC/CED/EED or similar binary encoding is unsupported or uncertain.
- **Object framing gap**: object stream boundaries, size, type, string stream, or handle stream offsets are wrong.
- **Handle resolution gap**: handle/object/owner/reactor/extension dictionary references are unresolved.
- **Custom object semantic gap**: proxy/custom object exists but native vertical-product semantics are not recovered.
- **External dependency gap**: xref, image, underlay, font, or similar external resource is missing or unsupported.

Diagnostics must include code, category, message, count when applicable, version family when known, and class/object name when known. Core gaps must appear in exported JSON diagnostics, not only console logs. Debug logs are off by default and enabled with `FT_DWG_DEBUG=1`.

## CAD / DWG Glossary

Use these terms consistently in issues, plans, comments, tests, and agent handoffs:

- **Model Space (模型空间)**: full-scale model geometry workspace.
- **Paper Space (图纸空间)**: sheet/layout workspace used for plotting and drawing composition.
- **Layout (布局)**: named paper-space sheet containing title blocks, borders, and layout viewports.
- **Layout Viewport (布局视口)**: paper-space window showing clipped/scaled model-space content.
- **Plot Window / Plot Area (打印窗口/打印范围)**: plotted region used to decide sheet preview extents.
- **Drawing Border / Title Block (图框/标题栏)**: paper-space border and metadata block defining the sheet presentation.
- **Layer (图层)**: object grouping carrying default color, linetype, lineweight, frozen/locked/plot state.
- **ACI / True Color**: AutoCAD Color Index and explicit RGB color.
- **ByLayer / ByBlock (随层/随块)**: property inheritance from layer or block reference.
- **Linetype / Lineweight (线型/线宽)**: dash pattern and plotted/displayed stroke width.
- **Plot Style / CTB / STB (打印样式)**: color-dependent or named plot-style rules affecting plotted appearance.
- **Block / INSERT / Block Reference (块/块参照)**: reusable block definition and placed instance transform.
- **Anonymous Block (匿名块)**: generated block such as `*D...` often used by dimensions and complex annotations.
- **Attribute / ATTRIB (属性)**: text-like data attached to block references.
- **Xref (外部参照)**: externally referenced drawing content.
- **TEXT / MTEXT**: single-line and multiline text entities.
- **Dimension (标注)**: associative/non-associative dimension entity and its generated graphics.
- **Leader / MLeader (引线/多重引线)**: annotation leader lines, arrows, callouts, and attached text/block content.
- **Balloon / Callout (气泡/序号标注)**: numbered or labeled annotation marker commonly used in mechanical drawings.
- **Detail View (详图)**: enlarged or referenced drawing view.
- **Annotative Scale (注释比例)**: scale-dependent annotation display behavior.
- **Hatch (填充)**: solid, patterned, or gradient area fill.
- **Wipeout / Mask (遮罩)**: object masking underlying geometry.
- **Draw Order (绘制顺序)**: front/back display order for overlapping objects.
- **WCS / UCS / OCS**: world, user, and object coordinate systems.
- **DCS (Display Coordinate System)**: viewport/display coordinate space.
- **Version Family / AC Code**: AutoCAD DWG release family and file code such as R2010/AC1024.
- **Proxy Object / Custom Object**: AutoCAD or vertical-product object that is not a standard entity/table object.
- **FIELD / ContextData**: dynamic annotation content and context-dependent display state.
- **SHX / Bigfont / TrueType**: CAD vector font, Asian bigfont pairing, and system font references.
- **XData / EED / Reactor / Extension Dictionary**: extended data and object relationship mechanisms.
- **Extents / Limits / Clip Boundary (范围/界限/裁剪边界)**: geometric, drawing, or viewport clipping bounds.

## Canvas Preview (preview.html) Notes

- Y-axis flip: `sy = -(wy - viewCenterY) * viewZoom + canvas.height / 2`
- Zoom toward mouse position: recalculate center after zoom so cursor stays on same world point.
- Touch input: pinch zoom with two-finger gesture.
- Layer panel: visibility toggles by layer name (frozen layers hidden by default).
- Batch-level frustum culling using precomputed batch bounds (world coords).
- Generated preview JSON is not committed to git. Use `/tmp` or ignored fixture paths for exports.

## Coding Standards

- C++20 with std::variant. No virtual dispatch for entities.
- MIT/BSD third-party dependencies only.
- Product parsing/rendering must not depend on GPL/copyleft CAD libraries, external file converters, or closed SDKs.
- All math types in `cad_types.h` — extend there first.
- Header files use `#pragma once`.
- Namespace: `cad::`.
- Member variables: `m_` prefix.
- Include order: corresponding header → project headers → system headers.
- **No special-casing for specific test files.** All fixes must be general algorithmic/math improvements.

## DWG Development Rules

- **DWG support is a general parser effort, not a `big.dwg` tuning exercise.**
- `test_dwg/big.dwg` is a **sentinel regression fixture**, not a golden file whose exact object count or vertex layout should drive parser logic.
- `test_dwg/Drawing2.dwg` is a **Layout/Paper Space mechanical drawing visual sentinel** for sheet fitting, drawing border fidelity, layout viewport semantics, dimensions, leaders, balloons, and text placement.
- Prioritize fixes in this order: container/section decoding → object framing → handle/reference resolution → entity semantics → rendering/export validation.
- Do not add filename checks, handle whitelists, coordinate heuristics, or object-type exceptions that only make one DWG pass. This applies to both `big.dwg` and `Drawing2.dwg`.
- When DWG behavior changes, verify it against both synthetic DXF fixtures and real-world sentinel files before considering the change valid.

### DWG Change Description Standard

Every DWG change, issue, plan, or commit summary should include: version family, object family, affected pipeline stage, gap label, fixtures checked, and diagnostics changed. Example: `R2010/AC1024, Custom Object, parser/export, Custom object semantic gap, checked Drawing2.dwg, adds custom_datum_target_label_proxy`.

## Git Workflow

- Remote: `origin` → `https://gitee.com/smarthhc/ft_model.git`
- Branch: `main` (开发阶段直推，后续可引入 feature 分支)
- Commit style: conventional commits (`feat:`, `fix:`, `refactor:`, `docs:`, `perf:`, `test:`)
- Co-authored commits with Codex: add `Co-Authored-By: Codex Opus 4.6 <noreply@anthropic.com>`
- Push with: `git push origin main`

### PR / Change Review Standards

- **核心结构变更**（EntityVariant, RenderBatch, SceneGraph 接口）需说明影响范围并通知相关 Agent
- **C++ 性能敏感代码**审查要点：避免不必要的拷贝、关注内存布局和 cache 友好性
- **不得针对特定测试文件特殊处理**，所有修复必须是通用改进
- 变更后须通过 synthetic DXF exact regression and real DWG sentinel visual/data regression

## Testing

- Use ezdxf (MIT) to generate test DXF files.
- Validate shared parser/renderer behavior with synthetic DXF exact regression fixtures.
- Visual comparison against `test_dwg/big.png` reference image.
- Use `test_dwg/big.dwg` for abnormal-entity filtering, large-file behavior, and main drawing sentinel checks.
- Use `test_dwg/Drawing2.dwg` for Layout/Paper Space, drawing border, viewport, mechanical annotation, leader, balloon, and title-block visual checks.
- Track DWG version fixtures for R2000, R2004, R2007, R2010, R2013, and R2018+. If a real fixture is missing, mark it as Missing fixture in the audit rather than silently claiming compatibility.
- Fixture catalog entries should record filename, AC code/version, domain, expected spaces/layouts, expected object families, current diagnostics, and acceptance type.
- `test_dwg/zj-02-00-1.dwg` and `test_dwg/新块.dwg` are fixture catalog candidates; classify them before making them release gates.
- Never commit test_data JSON files to git (they are in .gitignore).
- `core/test/test_regression_smoke.cpp` is the lightweight parser/render gate for ongoing development.
- `regression_smoke` must cover synthetic DXF fixtures exactly, and treat `test_dwg/big.dwg` as a lower-bound sentinel only.
- DWG progress should be measured by broader fixture coverage and object/entity success rate improvements, not by matching one file's exact output.

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

### 临时 DWG 攻坚小组（DWG Strike Team）

在 DWG 支持完成之前（Phase 4），临时在 **Parser Agent** 内部增设 4 个专业子 Agent，以并行化庞大的实体解析工作。这些子 Agent 都工作在 `core/src/parser/` 目录下，但通过**文件级分工**和**阶段锁**避免冲突：

| Sub-Agent | 指令文件 | 负责内容 | 依赖关系 |
|-----------|---------|---------|---------|
| **DWG Infra** | `.claude/agents/dwg-infra.md` | `read_bot`、CED 头、Classes Section、Object Map 基础设施 | **无阻塞，必须先完成** |
| **DWG Geometry** | `.claude/agents/dwg-geometry.md` | 几何实体：POLYLINE/LWPOLYLINE、SPLINE、HATCH、SOLID、ELLIPSE、3DFACE | 依赖 DWG Infra |
| **DWG Annotation** | `.claude/agents/dwg-annotation.md` | 标注文字：TEXT、MTEXT、DIMENSION、ATTRIB/ATTDEF | 依赖 DWG Infra |
| **DWG Insert** | `.claude/agents/dwg-insert.md` | 块引用：INSERT/MINSERT、handle stream、block resolution | 依赖 DWG Infra |

#### 并行策略与冲突规避

- **阶段 1（串行）**：只有 **DWG Infra Agent** 修改 `dwg_reader.cpp` 和 `dwg_parser.cpp`。在它提交之前，其他 DWG 子 Agent 只能做代码审查和准备工作。
- **阶段 2（并行）**：DWG Infra 完成后，**Geometry / Annotation / Insert** 三个子 Agent 可以**同时**修改 `dwg_objects.cpp` 的不同函数区域：
  - Geometry Agent 负责 `parse_polyline*`、`parse_spline`、`parse_hatch`、`parse_solid`、`parse_ellipse`、`parse_3dface`
  - Annotation Agent 负责 `parse_text`、`parse_mtext`、`parse_dimension`、`parse_attrib`
  - Insert Agent 负责 `parse_insert` 以及 `dwg_parser.cpp` 中 handle-resolution 的辅助逻辑
- **协调机制**：
  - 所有子 Agent 在修改 `dwg_objects.cpp` 的 `switch (obj_type)` dispatch 表时，必须采用**追加-only**策略，不得重排已有 case。
  - 如果多个子 Agent 同时需要修改同一个辅助函数（如 `half_to_float`），由 **DWG Infra Agent** 或主线程统一仲裁。
- **Renderer / QA Agent 参与方式**：
  - Renderer Agent 在 P1 中期介入，验证新增实体类型的 batcher 支持（大部分已有 DXF 对应逻辑）。
  - QA Agent 全程跟踪，每次 P0 或 P1 里程碑后更新 `regression_smoke` 基线。

#### 何时解散

当 `test_dwg/big.dwg` 的解析结果在视觉上与 `test_dwg/big.png` 基本对齐，且 `skip_bits < 100` 时，DWG 攻坚小组解散，后续工作回归标准的 5-Agent 模块制。
