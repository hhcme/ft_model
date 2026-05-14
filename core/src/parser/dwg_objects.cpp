#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_entity_common.h"
#include "cad/parser/dwg_entity_annotation.h"
#include "cad/parser/dwg_entity_hatch.h"
#include "cad/parser/dwg_parse_objects_context.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"
#include "cad/scene/shx_font_cache.h"
#include "cad/cad_types.h"
#include "cad/scene/viewport.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <utility>

namespace cad {

// ============================================================
// Debug logging for this TU — shadows dwg_parser.h versions
// ============================================================
namespace {

bool dwg_debug_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("FT_DWG_DEBUG");
        return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void dwg_debug_log(const char* fmt, ...)
{
    if (!dwg_debug_enabled()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

} // anonymous namespace

// ============================================================
// Diagnostic: track which entity types successfully add to scene
// ============================================================
std::unordered_map<uint32_t, size_t> g_success_counts;
std::unordered_map<uint32_t, size_t> g_dispatch_counts;

PendingPolyline2d g_pending_polyline2d;

// ============================================================
// Forward declarations for geometry parsers in dwg_entity_geometry.cpp
// ============================================================
void parse_line(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                DwgVersion version);
void parse_circle(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version);
void parse_arc(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
               DwgVersion version);
void parse_lwpolyline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                      DwgVersion version);
void parse_point(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);
void parse_ellipse(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                   DwgVersion version);
void parse_spline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version);
void parse_solid(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);
void parse_3dface(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);
void parse_ray(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
               DwgVersion version);
void parse_xline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);
void parse_mline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                DwgVersion version);
void parse_wipeout(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                   DwgVersion version);
void parse_multileader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                       DwgVersion version);
void parse_vertex_2d(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version, PendingPolyline2d& pending);
void parse_polyline_2d(DwgBitReader& r, const EntityHeader& hdr,
                       EntitySink& scene, DwgVersion version, PendingPolyline2d& pending);
void parse_polyline_pface(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                          DwgVersion version);
void parse_polyline_mesh(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene);
void parse_seqend(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  PendingPolyline2d& pending);

// ============================================================
// Local entity parsers that remain in this TU
// ============================================================
namespace {

// ============================================================
// INSERT (DWG type 7) / MINSERT (DWG type 8) -> EntityVariant index 10
// DWG does NOT store block_name in the data stream; it stores a block_header
// handle in the handle stream. For now we create the entity without resolving
// the block index.
// ============================================================
void parse_insert(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version, bool is_minsert) {
    double ix = r.read_bd();
    double iy = r.read_bd();
    double iz = r.read_bd();

    uint8_t scale_flag = r.read_bits(2);  // BB
    double sx = 1.0, sy = 1.0, sz = 1.0;
    if (scale_flag == 3) {
        sx = sy = sz = 1.0;
    } else if (scale_flag == 1) {
        sx = 1.0;
        sy = r.read_dd(1.0);
        sz = r.read_dd(1.0);
    } else if (scale_flag == 2) {
        sx = r.read_rd();
        sy = sx;
        sz = sx;
    } else { // scale_flag == 0
        sx = r.read_rd();
        sy = r.read_dd(sx);
        sz = r.read_dd(sx);
    }

    double rotation = r.read_bd();
    double nx = r.read_bd(), ny = r.read_bd(), nz = r.read_bd();  // extrusion
    bool has_attribs = r.read_b();
    if (version >= DwgVersion::R2004 && has_attribs) {
        r.read_bl();  // num_owned
    }

    uint16_t num_cols = 1;
    uint16_t num_rows = 1;
    double col_spacing = 0.0;
    double row_spacing = 0.0;
    if (is_minsert) {
        num_cols = r.read_bs();
        num_rows = r.read_bs();
        col_spacing = r.read_bd();
        row_spacing = r.read_bd();
    }

    if (!reader_ok(r)) return;

    // Validate INSERT: skip if insertion point or scale are clearly corrupt.
    if (!is_safe_coord(ix) || !is_safe_coord(iy) ||
        !std::isfinite(sx) || !std::isfinite(sy) ||
        std::abs(sx) > 1e4 || std::abs(sy) > 1e4 ||
        !std::isfinite(rotation)) return;

    InsertEntity ins;
    ins.block_index     = hdr.block_index;
    if (!is_default_extrusion(nx, ny, nz)) {
        OcsBasis basis = make_ocs_basis(nx, ny, nz);
        ins.insertion_point = ocs_point_to_wcs(ix, iy, iz, basis);
    } else {
        ins.insertion_point = {safe_float(ix), safe_float(iy), safe_float(iz)};
    }
    ins.x_scale         = safe_float(sx);
    ins.y_scale         = safe_float(sy);
    ins.rotation        = safe_float(rotation);
    ins.column_count    = static_cast<int32_t>(num_cols);
    ins.row_count       = static_cast<int32_t>(num_rows);
    ins.column_spacing  = safe_float(col_spacing);
    ins.row_spacing     = safe_float(row_spacing);

    EntityHeader ins_hdr = hdr;
    ins_hdr.type = EntityType::Insert;
    ins_hdr.bounds = entity_bounds_insert(ins);
    scene.add_entity(make_entity<10>(ins_hdr, std::move(ins)));
}

// ============================================================
// BLOCK (DWG type 4) / ENDBLK (DWG type 5) -> no entity output
// Block begin/end markers in the entity stream.
// ============================================================
void parse_block(DwgBitReader& /*r*/, const EntityHeader& /*hdr*/,
                 EntitySink& /*scene*/, DwgVersion /*version*/) {
    // BLOCK entities are markers, no geometry.
}
void parse_endblk(DwgBitReader& /*r*/, const EntityHeader& /*hdr*/,
                  EntitySink& /*scene*/, DwgVersion /*version*/) {
    // ENDBLK entities are markers, no geometry.
}

// ============================================================
// VIEWPORT (DWG type 34) -> EntityVariant index 15 (Vec3 placeholder)
// Decode the paper-space rectangle and core model-view fields when the
// stream is aligned. If validation fails, keep the older center-only
// placeholder so a bad viewport read cannot corrupt bounds or layout choice.
// ============================================================
void parse_viewport(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                    DwgVersion /*version*/) {
    double cx = r.read_bd();
    double cy = r.read_bd();
    double cz = r.read_bd();

    if (!reader_ok(r)) return;

    Vec3 center{static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)};

    Viewport vp;
    vp.name = "VIEWPORT";
    vp.center = center;
    vp.paper_center = center;
    vp.model_view_center = center;
    vp.is_paper_space = true;

    DwgBitReader probe = r;
    const double paper_width = probe.read_bd();
    const double paper_height = probe.read_bd();
    double target_x = probe.read_bd();
    double target_y = probe.read_bd();
    double target_z = probe.read_bd();
    double dir_x = probe.read_bd();
    double dir_y = probe.read_bd();
    double dir_z = probe.read_bd();
    const double twist = probe.read_bd();
    const double view_height = probe.read_bd();
    (void)probe.read_bd();  // lens_length
    (void)probe.read_bd();  // front_clip_z
    (void)probe.read_bd();  // back_clip_z
    (void)probe.read_bd();  // snap_angle
    double view_center_x = 0.0;
    double view_center_y = 0.0;
    probe.read_2d_point(view_center_x, view_center_y);

    const bool complete_viewport =
        reader_ok(probe) &&
        std::isfinite(paper_width) && std::isfinite(paper_height) &&
        std::isfinite(view_height) &&
        std::isfinite(view_center_x) && std::isfinite(view_center_y) &&
        std::isfinite(target_x) && std::isfinite(target_y) && std::isfinite(target_z) &&
        std::isfinite(dir_x) && std::isfinite(dir_y) && std::isfinite(dir_z) &&
        paper_width > 0.0 && paper_height > 0.0 && view_height > 0.0 &&
        paper_width < 1.0e7 && paper_height < 1.0e7 && view_height < 1.0e9;

    Bounds3d viewport_bounds = Bounds3d::from_point(center);
    if (complete_viewport) {
        vp.paper_width = static_cast<float>(paper_width);
        vp.paper_height = static_cast<float>(paper_height);
        vp.width = static_cast<float>(paper_width);
        vp.height = static_cast<float>(paper_height);
        vp.paper_center = center;
        vp.model_view_center = Vec3{static_cast<float>(view_center_x),
                                    static_cast<float>(view_center_y), 0.0f};
        vp.model_view_target = Vec3{static_cast<float>(target_x),
                                    static_cast<float>(target_y),
                                    static_cast<float>(target_z)};
        vp.view_height = static_cast<float>(view_height);
        vp.twist_angle = static_cast<float>(twist);
        vp.custom_scale = static_cast<float>(paper_height / view_height);
        const float half_w = static_cast<float>(paper_width * 0.5);
        const float half_h = static_cast<float>(paper_height * 0.5);
        viewport_bounds = Bounds3d::empty();
        viewport_bounds.expand(Vec3{center.x - half_w, center.y - half_h, center.z});
        viewport_bounds.expand(Vec3{center.x + half_w, center.y + half_h, center.z});
        vp.clip_boundary = viewport_bounds;
    }

    int32_t viewport_index = scene.add_viewport(std::move(vp));

    ViewportEntity vp_ent;
    vp_ent.center = center;
    vp_ent.width = static_cast<float>(paper_width);
    vp_ent.height = static_cast<float>(paper_height);
    vp_ent.status = 1;
    vp_ent.target = Vec3{static_cast<float>(target_x),
                         static_cast<float>(target_y),
                         static_cast<float>(target_z)};
    vp_ent.view_height = static_cast<float>(view_height);
    vp_ent.view_width = (view_height > 0.0 && paper_height > 0.0)
        ? static_cast<float>(paper_width * view_height / paper_height) : 0.0f;
    vp_ent.twist_angle = static_cast<float>(twist);
    vp_ent.custom_scale = static_cast<float>(paper_height / view_height);
    vp_ent.has_custom_scale = true;

    EntityHeader vp_hdr = hdr;
    vp_hdr.type = EntityType::Viewport;
    vp_hdr.space = DrawingSpace::PaperSpace;
    vp_hdr.viewport_index = viewport_index;
    vp_hdr.bounds = viewport_bounds;
    scene.add_entity(make_entity<15>(vp_hdr, std::move(vp_ent)));
}

} // anonymous namespace

// ============================================================
// Table/object parsers — LAYER, LTYPE, STYLE, DIMSTYLE
// These are non-graphic objects. The reader is positioned after
// the common object header (reactors, xdic flag).
// For R2010+ objects, a handle stream at the end contains
// linetype/plotstyle/material handles.
// ============================================================

namespace {

// Helper: read one handle from the handle stream.
// Creates a temporary reader starting at handle_bits_offset,
// with up to (bit_limit - handle_bits_offset) bits available.
DwgBitReader::HandleRef read_one_handle(const uint8_t* data, size_t data_bytes,
                          size_t handle_bits_offset, size_t bit_limit) {
    DwgBitReader hr(data, data_bytes);
    hr.set_bit_offset(handle_bits_offset);
    hr.set_bit_limit(bit_limit);
    return hr.read_h();
}

// Read TU (UTF-16LE) text for table objects.
// For R2007+ table objects, text fields are inline UTF-16TU in the main
// data stream (NOT in a separate string stream, which is empty for table objects).
// We read the BS length prefix then UTF-16LE code units directly.
std::string read_table_text(DwgBitReader& r) {
    uint16_t char_len = r.read_bs();  // TU length in UTF-16 code units
    if (r.has_error() || char_len == 0 || char_len > 8192) {
        return {};
    }
    std::string result;
    result.reserve(char_len);
    for (uint16_t i = 0; i < char_len && !r.has_error(); ++i) {
        uint16_t ch = r.read_rs();  // UTF-16LE code unit
        if (ch >= 1 && ch < 0xD800) {
            // BMP character (exclude NUL and surrogate range)
            result.push_back(static_cast<char>(ch));
        } else if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < char_len) {
            // High surrogate — read low surrogate
            uint16_t lo = r.read_rs();
            uint32_t cp = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
            // UTF-8 encode
            if (cp < 0x80) {
                result.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            i++;
        }
    }
    return result;
}

} // anonymous namespace

// ============================================================
// LAYER (type 51) table object
// Fields: [class_version RC] + [standard_flags BS] + [name TU]
//         + [color CMC] + [linetype handle H] + [plotstyle handle H]
// ============================================================
static int32_t parse_layer_object(DwgBitReader& r, EntitySink& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits,
                                const std::unordered_map<uint64_t, int32_t>* linetype_handle_to_index,
                                std::unordered_map<uint64_t, int32_t>* plotstyle_handle_to_index) {
    DwgBitReader state_reader = r;
    (void)state_reader.read_raw_char();  // class_version
    std::string name = state_reader.read_t();

    // BS(flag0) — bitmask:
    //   bit 0: frozen
    //   bit 1: frozen in new viewports
    //   bit 2: locked
    //   bit 3: off
    //   bit 4: plot flag (clear = plottable in R2004+ table objects)
    //   bits 5..: lineweight code
    uint16_t flag0 = state_reader.read_bs();
    bool is_frozen = (flag0 & 1) != 0;
    bool is_off = (flag0 & 8) != 0;
    bool is_locked = (flag0 & 4) != 0;
    bool plot_enabled = (flag0 & 16) == 0;
    uint16_t lineweight_code = static_cast<uint16_t>((flag0 >> 5) & 0x1F);

    // The corrected state field order above fixes frozen/off flags for R2010+
    // sentinels, but current CMC decoding still matches existing visual
    // fixtures when read from the legacy table-color position. Keep color
    // fidelity stable while CMC/table framing remains a tracked Encoding gap.
    DwgBitReader color_reader = r;
    std::string legacy_name = color_reader.read_t();
    if (name.empty()) {
        name = legacy_name;
    }
    (void)color_reader.read_bs();
    (void)color_reader.read_bs();
    auto cmc = color_reader.read_cmc_r2004(version);

    Color color;
    if (cmc.index > 0) {
        color = Color::from_aci(static_cast<int>(cmc.index));
    } else if (cmc.has_rgb) {
        uint8_t high_byte = static_cast<uint8_t>(cmc.rgb >> 24);
        if (high_byte >= 0xC0) {
            color = Color::from_aci(static_cast<uint8_t>(cmc.rgb & 0xFF));
        } else {
            uint32_t rgb24 = cmc.rgb & 0xFFFFFF;
            if (rgb24 != 0) {
                color = Color(static_cast<uint8_t>(rgb24 & 0xFF),
                              static_cast<uint8_t>((rgb24 >> 8) & 0xFF),
                              static_cast<uint8_t>((rgb24 >> 16) & 0xFF));
            } else {
                color = Color::white();
            }
        }
    } else {
        color = Color::white();
    }

    // Extract linetype and plotstyle handles from handle stream.
    // Pre-R2010: handles are inline in main data after object-specific fields.
    // R2010+: handles are in a separate stream after main_data_bits.
    // We scan handles and match against maps rather than assuming fixed order.
    int32_t linetype_index = -1;
    int32_t plotstyle_index = -1;
    if (version < DwgVersion::R2010) {
        while (!r.has_error() && r.bit_offset() < main_data_bits) {
            auto h = r.read_h();
            if (h.value == 0 && h.code == 0) break;
            if (linetype_index < 0 && linetype_handle_to_index) {
                auto it = linetype_handle_to_index->find(h.value);
                if (it != linetype_handle_to_index->end()) {
                    linetype_index = it->second;
                    continue;
                }
            }
            if (plotstyle_index < 0 && plotstyle_handle_to_index) {
                auto it = plotstyle_handle_to_index->find(h.value);
                if (it != plotstyle_handle_to_index->end()) {
                    plotstyle_index = it->second;
                }
            }
        }
    } else if (main_data_bits < entity_bits) {
        DwgBitReader hr = r;
        hr.set_bit_offset(main_data_bits);
        hr.set_bit_limit(entity_bits);
        for (int i = 0; i < 6 && !hr.has_error(); ++i) {
            auto h = hr.read_h();
            if (h.value == 0 && h.code == 0) break;
            if (linetype_index < 0 && linetype_handle_to_index) {
                auto it = linetype_handle_to_index->find(h.value);
                if (it != linetype_handle_to_index->end()) {
                    linetype_index = it->second;
                    continue;
                }
            }
            if (plotstyle_index < 0 && plotstyle_handle_to_index) {
                auto it = plotstyle_handle_to_index->find(h.value);
                if (it != plotstyle_handle_to_index->end()) {
                    plotstyle_index = it->second;
                }
            }
        }
    }

    if (!reader_ok(state_reader) || !reader_ok(color_reader)) return -1;

    // Add/update layer in SceneGraph
    int32_t layer_idx = scene.find_or_add_layer(name);
    Layer layer;
    layer.name = name;
    layer.color = color;
    layer.is_frozen = is_frozen;
    layer.is_off = is_off;
    layer.is_locked = is_locked;
    layer.plot_enabled = plot_enabled;
    if (lineweight_code > 0) {
        layer.lineweight = static_cast<float>(lineweight_code) / 100.0f;
    }
    if (linetype_index >= 0) {
        layer.linetype_index = linetype_index;
    }
    if (plotstyle_index >= 0) {
        layer.plot_style_index = plotstyle_index;
    }
    scene.update_layer(layer_idx, layer);
    return layer_idx;
}

// ============================================================
// LTYPE (type 57) table object
// Fields: [class_version RC] + [flags BS] + [name TU] + [description TU]
//         + [pattern_length BD] + [num_dashes BL]
//         + num_dashes x [BD(dash_or_gap)]
// ============================================================
static int32_t parse_ltype_object(DwgBitReader& r, EntitySink& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits) {
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    (void)r.read_bs();  // flags

    // Read from string stream (R2007+) or main stream (older) — read_t() handles both
    std::string name = r.read_t();
    std::string description = r.read_t();

    double pattern_length = r.read_bd();
    (void)pattern_length;

    uint32_t num_dashes = r.read_bl();
    if (num_dashes > 32) num_dashes = 32;

    std::vector<float> dash_lengths;
    dash_lengths.reserve(num_dashes);
    for (uint32_t i = 0; i < num_dashes && !r.has_error(); ++i) {
        dash_lengths.push_back(static_cast<float>(r.read_bd()));
    }

    // Alignment byte: 'L'=left, 'C'=center, 'R'=right, 'F'=fit, 0=none
    int alignment = 0;
    if (!r.has_error() && r.bit_offset() < main_data_bits) {
        alignment = r.read_raw_char();
    }

    // Skip any remaining handles
    while (!r.has_error() && r.bit_offset() < main_data_bits) {
        auto h = r.read_h();
        if (h.value == 0 && h.code == 0) break;
    }
    (void)main_data_bits; (void)entity_bits;

    if (!reader_ok(r)) return -1;

    Linetype lt;
    lt.name = name;
    lt.description = description;
    // Filter out corrupt dash data: if any entry is inf/nan, the LTYPE
    // fields are misaligned (common for R2007 when string stream is off).
    // Treat as continuous (solid line).
    bool corrupt = false;
    for (auto d : dash_lengths) {
        if (!std::isfinite(d)) { corrupt = true; break; }
    }
    if (!corrupt && !dash_lengths.empty()) {
        lt.pattern.dash_array = std::move(dash_lengths);
    }
    if (alignment >= 'L' && alignment <= 'F') {
        lt.pattern.alignment = alignment;
    }

    return scene.add_linetype(std::move(lt));
}

// ============================================================
// STYLE (type 53) table object
// Fields: [class_version RC] + [flags BL] + [name TU]
//         + [font_name TU] + [bigfont_name TU]
//         + [height BD] + [width BD] + [oblique BD] + [flags2 BL]
//         + [plotstyle handle H]
// ============================================================
static int32_t parse_style_object(DwgBitReader& r, EntitySink& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits,
                                std::unordered_map<uint64_t, int32_t>* /*plotstyle_handle_to_index*/) {
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    (void)r.read_bl();  // flags

    // Read from string stream (R2007+) or main stream (older) — read_t() handles both
    std::string name = r.read_t();
    std::string font_name = r.read_t();
    std::string bigfont_name = r.read_t();

    double height = r.read_bd();
    double width = r.read_bd();
    double oblique = r.read_bd();
    uint32_t flags2 = r.read_bl();  // generation flags: bit0=vertical, bit2=upside-down, bit3=backward

    // Scan remaining handles (pre-R2010 inline; R2010+ in handle stream after main_data_bits)
    // Plotstyle handle resolution for STYLE/DIMSTYLE is handled by the caller
    // which populates plotstyle_handle_to_index after we return the style index.
    if (version < DwgVersion::R2010) {
        while (!r.has_error() && r.bit_offset() < main_data_bits) {
            auto h = r.read_h();
            if (h.value == 0 && h.code == 0) break;
        }
    } else if (main_data_bits < entity_bits) {
        DwgBitReader hr = r;
        hr.set_bit_offset(main_data_bits);
        hr.set_bit_limit(entity_bits);
        for (int i = 0; i < 4 && !hr.has_error(); ++i) {
            auto h = hr.read_h();
            if (h.value == 0 && h.code == 0) break;
        }
    }
    (void)main_data_bits; (void)entity_bits;

    if (!reader_ok(r)) return -1;

    TextStyle style;
    style.name = name;
    style.font_file = font_name;
    style.bigfont_file = bigfont_name;
    style.fixed_height = static_cast<float>(height);
    style.width_factor = static_cast<float>(width);
    style.oblique_angle = static_cast<float>(oblique);
    style.generation_flags = static_cast<int32_t>(flags2);
    // SHX detection: either bigfont is present or font_file ends with .shx
    {
        std::string fl = font_name;
        std::transform(fl.begin(), fl.end(), fl.begin(), ::tolower);
        style.is_shx = !bigfont_name.empty() ||
            (fl.size() > 4 && fl.compare(fl.size() - 4, 4, ".shx") == 0);
    }

    // Resolve SHX font from file paths — populates shx_font_index / bigfont_index
    if (style.is_shx) {
        ShxFontCache::instance().resolve_text_style(style, "");
    }

    return scene.add_text_style(std::move(style));
}

// ============================================================
// DIMSTYLE (type 69) table object
// Best-effort parser: reads name and critical dimension variables.
// Field order follows the agent spec (dwg-infra.md); misalignment
// is tolerated — we sanity-check each value and skip on error.
// ============================================================
static int32_t parse_dimstyle_object(DwgBitReader& r, EntitySink& scene,
                                   DwgVersion version,
                                   const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                   size_t main_data_bits, size_t entity_bits,
                                   std::unordered_map<uint64_t, int32_t>* /*plotstyle_handle_to_index*/) {
    (void)entity_bits;
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    TextStyle dimstyle;
    dimstyle.is_dimstyle = true;

    // Strings (R2007+ read from string stream; older read inline TV)
    dimstyle.name = r.read_t();
    std::string dimpost  = r.read_t();
    std::string dimapost = r.read_t();
    (void)dimpost;
    (void)dimapost;

    // Boolean flags
    bool dimtol = r.read_b();
    bool dimlim = r.read_b();
    (void)dimtol;
    (void)dimlim;

    // Helper: read BD with bounds checking
    auto read_safe_bd = [&](float min_val, float max_val) -> float {
        if (r.has_error() || r.bit_offset() >= main_data_bits) {
            return 0.0f;
        }
        double v = r.read_bd();
        if (r.has_error() || !std::isfinite(v) || static_cast<float>(v) < min_val || static_cast<float>(v) > max_val) {
            return 0.0f;
        }
        return static_cast<float>(v);
    };

    // Tolerance / rounding (read to consume bits, not stored yet)
    (void)read_safe_bd(-1e6f, 1e6f); // dimtm
    (void)read_safe_bd(-1e6f, 1e6f); // dimtp
    (void)read_safe_bd(0.0f, 1e6f);  // dimrnd
    (void)read_safe_bd(0.0f, 1e6f);  // dimalt (alternate scale factor)

    // Zero-suppression BS fields (consume bits)
    if (!r.has_error() && r.bit_offset() < main_data_bits) (void)r.read_bs(); // dimazin
    if (!r.has_error() && r.bit_offset() < main_data_bits) (void)r.read_bs(); // dimazin_alt

    // Critical control fields (mapped to TextStyle DIMSTYLE members)
    float dimasz = read_safe_bd(0.0f, 1e6f);
    if (dimasz > 0.0f) dimstyle.arrow_size = dimasz;

    (void)read_safe_bd(0.0f, 1e6f); // dimcen (not stored in TextStyle)

    float dimexe = read_safe_bd(0.0f, 1e6f);
    if (dimexe >= 0.0f) dimstyle.ext_line_extension = dimexe;

    float dimexo = read_safe_bd(0.0f, 1e6f);
    if (dimexo >= 0.0f) dimstyle.ext_line_offset = dimexo;

    float dimgap = read_safe_bd(-1e6f, 1e6f);
    (void)dimgap; // not stored in TextStyle yet

    float dimlfac = read_safe_bd(0.0f, 1e6f);
    if (dimlfac > 0.0f) dimstyle.linear_scale_factor = dimlfac;

    float dimscale = read_safe_bd(0.0f, 1e6f);
    if (dimscale > 0.0f) dimstyle.dim_scale = dimscale;

    float dimtxt = read_safe_bd(0.0f, 1e6f);
    if (dimtxt > 0.0f) dimstyle.dim_text_height = dimtxt;

    // Skip arrow-block handles (dimblk, dimblk1, dimblk2).
    // Pre-R2010: inline in main data; R2010+: in handle stream.
    for (int i = 0; i < 3 && !r.has_error() && r.bit_offset() < main_data_bits; ++i) {
        auto h = r.read_h();
        if (h.value == 0 && h.code == 0) break;
    }

    auto read_safe_bs = [&]() -> int32_t {
        if (r.has_error() || r.bit_offset() >= main_data_bits) return -1;
        uint16_t v = r.read_bs();
        if (r.has_error()) return -1;
        return static_cast<int32_t>(v);
    };

    (void)read_safe_bs(); // dimadec
    int32_t dimclrd_idx = read_safe_bs();
    int32_t dimclre_idx = read_safe_bs();
    int32_t dimclrt_idx = read_safe_bs();
    if (dimclrd_idx >= 0 && dimclrd_idx <= 256) {
        dimstyle.dim_clrd = Color::from_aci(dimclrd_idx);
    }
    if (dimclre_idx >= 0 && dimclre_idx <= 256) {
        dimstyle.dim_clre = Color::from_aci(dimclre_idx);
    }
    if (dimclrt_idx >= 0 && dimclrt_idx <= 256) {
        dimstyle.dim_clrt = Color::from_aci(dimclrt_idx);
    }

    int32_t dimdec = read_safe_bs();
    if (dimdec >= 0) dimstyle.dim_precision = dimdec;

    (void)read_safe_bs(); // dimfit
    (void)read_safe_bs(); // dimjust
    (void)read_safe_bs(); // dimfrac

    int32_t dimunit = read_safe_bs();
    if (dimunit >= 0) dimstyle.dim_unit = dimunit;

    (void)read_safe_bs(); // dimupt
    (void)read_safe_bs(); // dimtzin
    (void)read_safe_bs(); // dimazin

    int32_t dimse1 = read_safe_bs();
    if (dimse1 >= 0) dimstyle.dim_se1 = (dimse1 != 0);

    int32_t dimse2 = read_safe_bs();
    if (dimse2 >= 0) dimstyle.dim_se2 = (dimse2 != 0);

    (void)read_safe_bs(); // dimsoxd

    int32_t dimtad = read_safe_bs();
    if (dimtad >= 0) dimstyle.dim_tad = dimtad;

    (void)read_safe_bs(); // dimtih
    (void)read_safe_bs(); // dimtoh

    int32_t dimtofl = read_safe_bs();
    if (dimtofl >= 0) dimstyle.dim_tofl = (dimtofl != 0);

    // Skip any remaining main-data fields
    while (!r.has_error() && r.bit_offset() < main_data_bits) {
        (void)r.read_bl();
    }

    if (!reader_ok(r)) return -1;

    // Apply deterministic defaults for missing critical fields
    if (dimstyle.dim_scale <= 0.0f) dimstyle.dim_scale = 1.0f;
    if (dimstyle.dim_text_height <= 0.0f) dimstyle.dim_text_height = 2.5f;
    if (dimstyle.arrow_size <= 0.0f) dimstyle.arrow_size = 0.18f * dimstyle.dim_scale;
    if (dimstyle.ext_line_extension < 0.0f) dimstyle.ext_line_extension = 1.25f;
    if (dimstyle.ext_line_offset < 0.0f) dimstyle.ext_line_offset = 0.625f;
    if (dimstyle.linear_scale_factor <= 0.0f) dimstyle.linear_scale_factor = 1.0f;

    return scene.add_text_style(std::move(dimstyle));
}

// ============================================================
// Stub table object parsers — read name and skip remaining fields.
// These objects are not yet stored in SceneGraph, but parsing
// them avoids bit drift and provides diagnostics.
// ============================================================
static void parse_stub_table_object(DwgBitReader& r, DwgVersion version,
                                    size_t main_data_bits, const char* type_name) {
    (void)type_name;
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version
    }
    (void)r.read_t();  // name
    // Skip remaining main-data fields
    while (!r.has_error() && r.bit_offset() < main_data_bits) {
        (void)r.read_bl();
    }
}

static int32_t parse_vport_object(DwgBitReader& r, EntitySink& scene,
                                  DwgVersion version,
                                  const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                  size_t main_data_bits, size_t entity_bits) {
    std::string name = r.read_t();
    (void)r.read_bs();  // common table flags / xref status

    const double view_height = r.read_bd();
    const double view_width = r.read_bd();
    const double view_center_x = r.read_double();
    const double view_center_y = r.read_double();
    const double target_x = r.read_bd();
    const double target_y = r.read_bd();
    const double target_z = r.read_bd();
    (void)r.read_bd();  // VIEWDIR.x
    (void)r.read_bd();  // VIEWDIR.y
    (void)r.read_bd();  // VIEWDIR.z
    const double twist = r.read_bd();
    (void)r.read_bd();  // lens_length
    (void)r.read_bd();  // front_clip_z
    (void)r.read_bd();  // back_clip_z
    (void)r.read_bits(4); // VIEWMODE
    if (version >= DwgVersion::R2000) {
        (void)r.read_raw_char(); // render_mode
    }
    if (version >= DwgVersion::R2007) {
        (void)r.read_b();        // use_default_lights
        (void)r.read_raw_char(); // default_lighting_type
        (void)r.read_bd();       // brightness
        (void)r.read_bd();       // contrast
        (void)r.read_cmc_r2004(version);
    }

    const double lower_left_x = r.read_double();
    const double lower_left_y = r.read_double();
    const double upper_right_x = r.read_double();
    const double upper_right_y = r.read_double();

    (void)r.read_b();       // UCSFOLLOW
    (void)r.read_bs();      // circle_zoom
    (void)r.read_b();       // FASTZOOM
    (void)r.read_bits(2);   // UCSICON
    (void)r.read_b();       // GRIDMODE
    (void)r.read_double();  // GRIDUNIT.x
    (void)r.read_double();  // GRIDUNIT.y
    (void)r.read_b();       // SNAPMODE
    (void)r.read_b();       // SNAPSTYLE
    (void)r.read_bs();      // SNAPISOPAIR
    if (version != DwgVersion::R2007) {
        (void)r.read_bd();      // SNAPANG
        (void)r.read_double();  // SNAPBASE.x
        (void)r.read_double();  // SNAPBASE.y
    }
    (void)r.read_double();  // SNAPUNIT.x
    (void)r.read_double();  // SNAPUNIT.y

    if (version >= DwgVersion::R2000) {
        (void)r.read_b();  // ucs_at_origin
        (void)r.read_b();  // UCSVP
        (void)r.read_bd(); (void)r.read_bd(); (void)r.read_bd(); // ucsorg
        (void)r.read_bd(); (void)r.read_bd(); (void)r.read_bd(); // ucsxdir
        (void)r.read_bd(); (void)r.read_bd(); (void)r.read_bd(); // ucsydir
        (void)r.read_bd(); // ucs_elevation
        (void)r.read_bs(); // UCSORTHOVIEW
    }
    if (version >= DwgVersion::R2007) {
        (void)r.read_bs(); // grid_flags
        (void)r.read_bs(); // grid_major
    }
    (void)main_data_bits;
    (void)entity_bits;

    if (!reader_ok(r) ||
        !std::isfinite(view_height) || !std::isfinite(view_width) ||
        !std::isfinite(view_center_x) || !std::isfinite(view_center_y) ||
        view_height <= 0.0 || view_width <= 0.0 ||
        view_height > 1.0e9 || view_width > 1.0e9) {
        return -1;
    }

    Viewport vp;
    vp.name = name.empty() ? "*Active" : name;
    vp.center = Vec3{static_cast<float>(view_center_x),
                     static_cast<float>(view_center_y), 0.0f};
    vp.width = static_cast<float>(view_width);
    vp.height = static_cast<float>(view_height);
    vp.paper_center = Vec3{static_cast<float>((lower_left_x + upper_right_x) * 0.5),
                           static_cast<float>((lower_left_y + upper_right_y) * 0.5),
                           0.0f};
    vp.paper_width = static_cast<float>(upper_right_x - lower_left_x);
    vp.paper_height = static_cast<float>(upper_right_y - lower_left_y);
    vp.model_view_center = vp.center;
    vp.model_view_target = Vec3{static_cast<float>(target_x),
                                static_cast<float>(target_y),
                                static_cast<float>(target_z)};
    vp.view_height = static_cast<float>(view_height);
    vp.custom_scale = static_cast<float>(view_width / view_height);
    vp.twist_angle = static_cast<float>(twist);
    vp.is_paper_space = false;
    return scene.add_viewport(std::move(vp));
}

// ============================================================
// Public entry point for table objects
// ============================================================
int32_t parse_dwg_table_object(DwgBitReader& reader, uint32_t obj_type,
                              EntitySink& scene, DwgVersion version,
                              size_t entity_bits, size_t main_data_bits,
                              uint64_t handle,
                              std::unordered_map<uint64_t, int32_t>* layer_handle_to_index,
                              std::unordered_map<uint64_t, int32_t>* linetype_handle_to_index,
                              std::unordered_map<uint64_t, int32_t>* text_style_handle_to_index,
                              std::unordered_map<uint64_t, int32_t>* plotstyle_handle_to_index) {
    const uint8_t* obj_data = reader.data();
    size_t obj_bytes = reader.data_size();

    switch (obj_type) {
        case 51:  // LAYER
            {
                int32_t layer_idx = parse_layer_object(reader, scene, version, obj_data, obj_bytes,
                                   main_data_bits, entity_bits, linetype_handle_to_index,
                                   plotstyle_handle_to_index);
                if (layer_handle_to_index && layer_idx >= 0) {
                    (*layer_handle_to_index)[handle] = layer_idx;
                }
                return layer_idx;
            }
            break;
        case 57:  // LTYPE
            {
                int32_t linetype_idx = parse_ltype_object(reader, scene, version, obj_data, obj_bytes,
                                                          main_data_bits, entity_bits);
                if (linetype_handle_to_index && linetype_idx >= 0) {
                    (*linetype_handle_to_index)[handle] = linetype_idx;
                }
                return linetype_idx;
            }
        case 53:  // STYLE
            {
                int32_t style_idx = parse_style_object(reader, scene, version, obj_data, obj_bytes,
                               main_data_bits, entity_bits, plotstyle_handle_to_index);
                if (plotstyle_handle_to_index && style_idx >= 0) {
                    (*plotstyle_handle_to_index)[handle] = style_idx;
                }
                if (text_style_handle_to_index && style_idx >= 0) {
                    (*text_style_handle_to_index)[handle] = style_idx;
                }
                return style_idx;
            }
        case 69:  // DIMSTYLE
            {
                int32_t dimstyle_idx = parse_dimstyle_object(reader, scene, version, obj_data, obj_bytes,
                                  main_data_bits, entity_bits, plotstyle_handle_to_index);
                if (plotstyle_handle_to_index && dimstyle_idx >= 0) {
                    (*plotstyle_handle_to_index)[handle] = dimstyle_idx;
                }
                return dimstyle_idx;
            }
        case 65:  // VPORT
            return parse_vport_object(reader, scene, version, obj_data, obj_bytes,
                                      main_data_bits, entity_bits);
        case 62:  // GROUP
            parse_stub_table_object(reader, version, main_data_bits, "GROUP");
            break;
        case 64:  // MLINESTYLE
            parse_stub_table_object(reader, version, main_data_bits, "MLINESTYLE");
            break;
        case 66:  // UCS
            parse_stub_table_object(reader, version, main_data_bits, "UCS");
            break;
        case 67:  // VIEW
            parse_stub_table_object(reader, version, main_data_bits, "VIEW");
            break;
        case 68:  // APPID
            parse_stub_table_object(reader, version, main_data_bits, "APPID");
            break;
        default:
            break;
    }
    return -1;
}

// ============================================================
// Public entry point — dispatches to type-specific parsers
// ============================================================

static void apply_entity_semantics(EntityVariant& entity) {
    switch (entity.header.type) {
        case EntityType::Dimension: case EntityType::Leader:
        case EntityType::Tolerance: case EntityType::Multileader:
            entity.header.semantic = EntitySemantic::Annotation;
            entity.header.modifiers |= kModAlwaysDraw;
            break;
        case EntityType::Text: case EntityType::MText:
            entity.header.semantic = EntitySemantic::Text;
            entity.header.modifiers |= kModScreenOriented;
            break;
        case EntityType::Hatch: case EntityType::Solid:
            entity.header.semantic = EntitySemantic::Fill;
            break;
        case EntityType::Insert: case EntityType::Viewport:
            entity.header.semantic = EntitySemantic::Structure;
            break;
        case EntityType::Point: case EntityType::Ray: case EntityType::XLine:
            entity.header.semantic = EntitySemantic::Helper;
            break;
        default:
            entity.header.semantic = EntitySemantic::Geometry;
            break;
    }
}

void parse_dwg_entity(DwgBitReader& reader, uint32_t obj_type,
                       const EntityHeader& header, EntitySink& scene,
                       DwgVersion version,
                       const char* class_name,
                       ParseObjectsContext& ctx) {
    g_dispatch_counts[obj_type]++;  // DEPRECATED global - TODO remove
    size_t before = scene.entities().size();

    // Class-based entity dispatch (type >= 500, identified by class name)
    if (obj_type >= 500 && class_name) {
        if (std::strstr(class_name, "MULTILEADER") != nullptr) {
            parse_multileader(reader, header, scene, version);
            bool success = (scene.entities().size() > before);
            if (success) g_success_counts[obj_type]++;
            return;
        }
        if (std::strstr(class_name, "HATCH") != nullptr ||
            std::strstr(class_name, "AcDbHatch") != nullptr) {
            parse_hatch(reader, header, scene, version);
            bool success = (scene.entities().size() > before);
            if (success) g_success_counts[obj_type]++;
            return;
        }
        if (std::strstr(class_name, "DIMENSION") != nullptr ||
            std::strstr(class_name, "AcDbDimension") != nullptr) {
            parse_dimension(reader, header, scene, version, obj_type);
            bool success = (scene.entities().size() > before);
            if (success) g_success_counts[obj_type]++;
            return;
        }
        // Unknown class entity — skip silently.
        return;
    }

    switch (obj_type) {
        case 1:   parse_text(reader, header, scene, version);         break;  // TEXT
        case 2:   // ATTRIB — handled inline in annotation parser, skip entity output
        case 3:   // ATTDEF — template definition, skip entity output
            break;
        case 4:   parse_block(reader, header, scene, version);        break;  // BLOCK
        case 5:   parse_endblk(reader, header, scene, version);       break;  // ENDBLK
        case 6:   parse_seqend(reader, header, scene, ctx.pending_polyline2d); break;  // SEQEND
        case 7:   parse_insert(reader, header, scene, version, false); break;  // INSERT
        case 8:   parse_insert(reader, header, scene, version, true);  break;  // MINSERT
        case 17:  parse_arc(reader, header, scene, version);          break;  // ARC
        case 18:  parse_circle(reader, header, scene, version);       break;  // CIRCLE
        case 19:  parse_line(reader, header, scene, version);         break;  // LINE
        case 10:  parse_vertex_2d(reader, header, scene, version, ctx.pending_polyline2d); break;
        case 15:  parse_polyline_2d(reader, header, scene, version, ctx.pending_polyline2d); break;
        case 20: case 21: case 22: case 23: case 24: case 25: case 26:
            parse_dimension(reader, header, scene, version, obj_type); break;
        case 27:  parse_point(reader, header, scene, version);        break;  // POINT
        case 28:  parse_3dface(reader, header, scene, version);      break;  // 3DFACE
        case 29:  parse_polyline_pface(reader, header, scene, version); break;  // POLYLINE_PFACE
        case 30:  parse_polyline_mesh(reader, header, scene);         break;  // POLYLINE_MESH
        case 31:  parse_solid(reader, header, scene, version);        break;  // SOLID
        case 32:  parse_solid(reader, header, scene, version);        break;  // TRACE
        case 33:  parse_wipeout(reader, header, scene, version);       break;  // WIPEOUT
        case 34:  parse_viewport(reader, header, scene, version);       break;  // VIEWPORT
        case 35:  parse_ellipse(reader, header, scene, version);      break;  // ELLIPSE
        case 36:  parse_spline(reader, header, scene, version);       break;  // SPLINE
        case 40:  parse_ray(reader, header, scene, version);          break;  // RAY
        case 41:  parse_xline(reader, header, scene, version);        break;  // XLINE
        case 44:  parse_mtext(reader, header, scene, version);        break;  // MTEXT
        case 45:  parse_leader(reader, header, scene, version);       break;  // LEADER
        case 46:  parse_tolerance(reader, header, scene, version);    break;  // TOLERANCE
        case 47:  parse_mline(reader, header, scene, version);       break;  // MLINE
        case 77:  parse_lwpolyline(reader, header, scene, version);   break;  // LWPOLYLINE
        case 78:  parse_hatch(reader, header, scene, version);        break;  // HATCH

        default:
            // Unknown entity type — skip silently.
            break;
    }
    bool success = (scene.entities().size() > before);
    // Types that don't produce SceneGraph entities by design:
    // ATTRIB(2), ATTDEF(3), BLOCK(4), ENDBLK(5), SEQEND(6), POLYLINE_2D(15)
    if (!success && (obj_type == 2 || obj_type == 3 || obj_type == 4 ||
                     obj_type == 5 || obj_type == 6 || obj_type == 15)) {
        success = !reader.has_error();
    }
    if (success) {
        g_success_counts[obj_type]++;  // DEPRECATED global
    } else {
        g_success_counts[obj_type + 10000]++;  // DEPRECATED global
    }

    // Apply EntitySemantic and EntityModifier
    for (size_t eidx = before; eidx < scene.entities().size(); ++eidx) {
        apply_entity_semantics(scene.entities()[eidx]);
    }
}

void reset_dwg_entity_parser_state() {
    g_success_counts.clear();
    g_dispatch_counts.clear();
    g_pending_polyline2d = PendingPolyline2d{};
}

std::unordered_map<uint32_t, size_t> get_dwg_entity_success_counts() {
    return g_success_counts;
}

void dump_dwg_entity_parse_stats() {
    fprintf(stderr, "[DWG ENTITY PARSE STATS]\n");
    for (const auto& [type, dispatched] : g_dispatch_counts) {
        auto it = g_success_counts.find(type);
        size_t succeeded = (it != g_success_counts.end()) ? it->second : 0;
        auto fail_it = g_success_counts.find(type + 10000);
        size_t failed = (fail_it != g_success_counts.end()) ? fail_it->second : 0;
        if (dispatched > 0 && type < 100) {
            double rate = succeeded * 100.0 / dispatched;
            fprintf(stderr, "  type=%3u dispatched=%5zu succeeded=%5zu failed=%5zu rate=%.0f%%\n",
                type, dispatched, succeeded, failed, rate);
        }
    }
}

std::unordered_map<uint32_t, size_t> get_dwg_entity_dispatch_counts() {
    return g_dispatch_counts;
}

} // namespace cad
