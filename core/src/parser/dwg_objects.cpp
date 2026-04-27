#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_entity_common.h"
#include "cad/parser/dwg_entity_annotation.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"
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
void parse_vertex_2d(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version);
void parse_polyline_2d(DwgBitReader& r, const EntityHeader& hdr,
                       EntitySink& scene, DwgVersion version);
void parse_polyline_pface(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                          DwgVersion version);
void parse_polyline_mesh(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene);
void parse_seqend(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene);

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
    r.read_bd(); r.read_bd(); r.read_bd();  // extrusion (3 BD)
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
    ins.block_index     = hdr.block_index;  // Set by handle stream parsing in parse_objects
    ins.insertion_point = {safe_float(ix), safe_float(iy), safe_float(iz)};
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
    vp_ent.target = Vec3{static_cast<float>(target_x),
                         static_cast<float>(target_y),
                         static_cast<float>(target_z)};
    vp_ent.view_height = static_cast<float>(view_height);
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
//         + [color CMC] + [linetype handle H] + [more handles]
// ============================================================
static int32_t parse_layer_object(DwgBitReader& r, EntitySink& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits,
                                const std::unordered_map<uint64_t, int32_t>* linetype_handle_to_index) {
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

    // For R2010+, handles are in a separate handle stream (after main_data_bits).
    // Skip remaining main-data fields only for pre-R2010 formats.
    if (version < DwgVersion::R2010) {
        while (!r.has_error() && r.bit_offset() < main_data_bits) {
            auto h = r.read_h();
            if (h.value == 0 && h.code == 0) break;
        }
    }

    // Read linetype handle from handle stream (at end of entity data)
    int32_t linetype_index = -1;
    if (main_data_bits < entity_bits) {
        DwgBitReader hr = r;  // copy reader at current position
        hr.set_bit_offset(main_data_bits);
        hr.set_bit_limit(entity_bits);
        auto lt_handle = hr.read_h();
        if (lt_handle.value != 0 && linetype_handle_to_index) {
            auto it = linetype_handle_to_index->find(lt_handle.value);
            if (it != linetype_handle_to_index->end()) {
                linetype_index = it->second;
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

    return scene.add_linetype(std::move(lt));
}

// ============================================================
// STYLE (type 53) table object
// Fields: [class_version RC] + [flags BL] + [name TU]
//         + [font_name TU] + [bigfont_name TU]
//         + [height BD] + [width BD] + [oblique BD] + [flags2 BL]
// ============================================================
static void parse_style_object(DwgBitReader& r, EntitySink& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits) {
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
    (void)oblique;
    (void)r.read_bl();  // flags2

    // Skip remaining handles
    while (!r.has_error() && r.bit_offset() < main_data_bits) {
        auto h = r.read_h();
        if (h.value == 0 && h.code == 0) break;
    }
    (void)main_data_bits; (void)entity_bits;

    if (!reader_ok(r)) return;

    TextStyle style;
    style.name = name;
    style.font_file = font_name;
    style.fixed_height = static_cast<float>(height);
    style.width_factor = static_cast<float>(width);
    style.is_shx = !bigfont_name.empty();

    scene.add_text_style(std::move(style));
}

// ============================================================
// DIMSTYLE (type 69) table object
// Complex structure: skip most fields, create placeholder.
// ============================================================
static void parse_dimstyle_object(DwgBitReader& r, EntitySink& scene,
                                   DwgVersion version,
                                   const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                   size_t main_data_bits, size_t entity_bits) {
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    // DIMSTYLE is very complex — skip all fields
    while (!r.has_error() && r.bit_offset() < main_data_bits) {
        (void)r.read_bl();
    }
    (void)main_data_bits; (void)entity_bits;

    if (!reader_ok(r)) return;

    TextStyle dimstyle;
    dimstyle.name = "[DIMSTYLE]";
    scene.add_text_style(std::move(dimstyle));
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
                              std::unordered_map<uint64_t, int32_t>* linetype_handle_to_index) {
    const uint8_t* obj_data = reader.data();
    size_t obj_bytes = reader.data_size();

    switch (obj_type) {
        case 51:  // LAYER
            {
                int32_t layer_idx = parse_layer_object(reader, scene, version, obj_data, obj_bytes,
                                   main_data_bits, entity_bits, linetype_handle_to_index);
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
            parse_style_object(reader, scene, version, obj_data, obj_bytes,
                               main_data_bits, entity_bits);
            break;
        case 69:  // DIMSTYLE
            parse_dimstyle_object(reader, scene, version, obj_data, obj_bytes,
                                  main_data_bits, entity_bits);
            break;
        case 65:  // VPORT
            return parse_vport_object(reader, scene, version, obj_data, obj_bytes,
                                      main_data_bits, entity_bits);
        default:
            break;
    }
    return -1;
}

// ============================================================
// Public entry point — dispatches to type-specific parsers
// ============================================================
void parse_dwg_entity(DwgBitReader& reader, uint32_t obj_type,
                       const EntityHeader& header, EntitySink& scene,
                       DwgVersion version) {
    g_dispatch_counts[obj_type]++;
    size_t before = scene.entities().size();
    switch (obj_type) {
        case 1:   parse_text(reader, header, scene, version);         break;  // TEXT
        case 2:   parse_block(reader, header, scene, version);        break;  // ATTRIB (skip)
        case 3:   parse_block(reader, header, scene, version);        break;  // ATTDEF (skip)
        case 4:   parse_block(reader, header, scene, version);        break;  // BLOCK
        case 5:   parse_endblk(reader, header, scene, version);       break;  // ENDBLK
        case 6:   parse_seqend(reader, header, scene);                 break;  // SEQEND
        case 7:   parse_insert(reader, header, scene, version, false); break;  // INSERT
        case 8:   parse_insert(reader, header, scene, version, true);  break;  // MINSERT
        case 17:  parse_arc(reader, header, scene, version);          break;  // ARC
        case 18:  parse_circle(reader, header, scene, version);       break;  // CIRCLE
        case 19:  parse_line(reader, header, scene, version);         break;  // LINE
        case 10:  parse_vertex_2d(reader, header, scene, version);   break;  // VERTEX_2D
        case 15:  parse_polyline_2d(reader, header, scene, version); break;  // POLYLINE_2D
        case 20:  // DIMENSION_ORDINATE
        case 21:  // DIMENSION_LINEAR
        case 22:  // DIMENSION_ALIGNED
        case 23:  // DIMENSION_ANG3PT
        case 24:  // DIMENSION_ANG2LN
        case 25:  // DIMENSION_RADIUS
        case 26:  // DIMENSION_DIAMETER
            parse_dimension(reader, header, scene, version);
            break;
        case 27:  parse_point(reader, header, scene, version);        break;  // POINT
        case 28:  parse_3dface(reader, header, scene, version);      break;  // 3DFACE
        case 29:  parse_polyline_pface(reader, header, scene, version); break;  // POLYLINE_PFACE
        case 30:  parse_polyline_mesh(reader, header, scene);         break;  // POLYLINE_MESH
        case 31:  parse_solid(reader, header, scene, version);        break;  // SOLID
        case 32:  parse_solid(reader, header, scene, version);        break;  // TRACE
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
        g_success_counts[obj_type]++;
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

std::unordered_map<uint32_t, size_t> get_dwg_entity_dispatch_counts() {
    return g_dispatch_counts;
}

} // namespace cad
