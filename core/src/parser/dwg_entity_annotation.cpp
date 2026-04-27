#include "cad/parser/dwg_entity_annotation.h"
#include "cad/parser/dwg_entity_common.h"
#include "cad/parser/dwg_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"
#include "cad/cad_types.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cad {

// ============================================================
// TEXT (DWG type 1) -> EntityVariant index 6
// R2000+ compressed format uses dataflags (RC) to control conditional fields.
// ============================================================
void parse_text(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                DwgVersion version) {
    uint8_t dataflags = r.read_raw_char();

    double elevation = 0.0;
    if (!(dataflags & 0x01)) {
        elevation = r.read_rd();
    }

    double ix = r.read_rd();
    double iy = r.read_rd();
    double iz = elevation;

    if (!(dataflags & 0x02)) {
        r.read_dd(ix);  // alignment_pt.x
        r.read_dd(iy);  // alignment_pt.y
    }

    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);
    r.read_bt();

    double oblique_angle = 0.0;
    if (!(dataflags & 0x04)) {
        oblique_angle = r.read_rd();
    }

    double rotation = 0.0;
    if (!(dataflags & 0x08)) {
        rotation = r.read_rd();
    }

    double height = r.read_rd();

    double width_factor = 1.0;
    if (!(dataflags & 0x10)) {
        width_factor = r.read_rd();
    }

    // R2007+: text is in the string stream, read_t() handles dispatch.
    std::string text = r.read_t();

    uint16_t generation = 0;
    if (!(dataflags & 0x20)) {
        generation = r.read_bs();
    }

    uint16_t h_align = 0;
    if (!(dataflags & 0x40)) {
        h_align = r.read_bs();
    }

    uint16_t v_align = 0;
    if (!(dataflags & 0x80)) {
        v_align = r.read_bs();
    }

    (void)oblique_angle;
    (void)generation;
    (void)v_align;

    if (!reader_ok(r)) return;

    if (!is_safe_coord(ix) || !is_safe_coord(iy) ||
        !std::isfinite(height) || height <= 0.0) return;

    TextEntity txt;
    txt.insertion_point = {safe_float(ix), safe_float(iy), safe_float(iz)};
    txt.height          = safe_float(height);
    txt.rotation        = safe_float(rotation);
    txt.width_factor    = safe_float(width_factor);
    txt.text            = std::move(text);
    txt.alignment       = static_cast<int32_t>(h_align);

    EntityHeader txt_hdr = hdr;
    txt_hdr.type = EntityType::Text;
    txt_hdr.bounds = entity_bounds_text(txt);
    scene.add_entity(make_entity<6>(txt_hdr, std::move(txt)));
}

// ============================================================
// MTEXT (DWG type 44) -> EntityVariant index 7
// ============================================================
void parse_mtext(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version) {
    double ix = r.read_bd();
    double iy = r.read_bd();
    double iz = r.read_bd();
    r.read_bd(); r.read_bd(); r.read_bd();  // extrusion
    double xax = r.read_bd();               // x_axis_dir.x
    double yay = r.read_bd();               // x_axis_dir.y
    double zaz = r.read_bd();               // x_axis_dir.z
    double rect_width = r.read_bd();
    double rect_height = 0.0;
    if (version >= DwgVersion::R2007) {
        rect_height = r.read_bd();
    }
    double height = r.read_bd();
    uint16_t attachment = r.read_bs();
    uint16_t flow_dir = r.read_bs();
    r.read_bd();  // extents_height
    r.read_bd();  // extents_width
    std::string text = r.read_t();

    if (version >= DwgVersion::R2000) {
        r.read_bs();  // linespace_style
        r.read_bd();  // linespace_factor
        r.read_b();   // unknown_b0
    }

    (void)flow_dir;
    (void)zaz;

    if (!reader_ok(r)) return;

    if (!is_safe_coord(ix) || !is_safe_coord(iy) ||
        !std::isfinite(height) || height <= 0.0) return;

    float rotation = std::atan2(static_cast<float>(yay), static_cast<float>(xax));

    TextEntity txt;
    txt.insertion_point = {safe_float(ix), safe_float(iy), safe_float(iz)};
    txt.height          = safe_float(height);
    txt.rotation        = rotation;
    txt.width_factor    = 1.0f;
    txt.rect_width      = std::isfinite(rect_width) && rect_width > 0.0
                              ? safe_float(rect_width)
                              : 0.0f;
    txt.rect_height     = std::isfinite(rect_height) && rect_height > 0.0
                              ? safe_float(rect_height)
                              : 0.0f;
    txt.text            = std::move(text);
    txt.alignment       = static_cast<int32_t>(attachment);

    EntityHeader mtxt_hdr = hdr;
    mtxt_hdr.type = EntityType::MText;
    mtxt_hdr.bounds = entity_bounds_text(txt);
    scene.add_entity(make_entity<7>(mtxt_hdr, std::move(txt)));
}

// ============================================================
// DIMENSION types (DWG types 20-25) -> EntityVariant index 8
// Common layout: extrusion, elevation, anonymous block handle,
// dimension type, user text, rotation, direction, then points.
// ============================================================
void parse_dimension(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version) {
    // COMMON_ENTITY_DIMENSION (R2000+ binary, non-DXF):
    // R2010+: RC(class_version)
    // 3BD(extrusion)
    // 2RD(text_midpt) -- RAW doubles, not BD!
    // BD(elevation)
    // RC(flag1) -- raw char, not BS
    // T(user_text) -- R2007+ in string stream
    // BD0(text_rotation), BD0(horiz_dir)
    // 3BD_1(ins_scale)
    // BD0(ins_rotation)
    // R2000+: BS(attachment), BS1(lspace_style), BD1(lspace_factor), BD(act_measurement)
    // R2007+: B(unknown), B(flip_arrow1), B(flip_arrow2)
    // 2RD0(clone_ins_pt)
    // Then type-specific points, then handles.

    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    // extrusion (3BD)
    double ex = r.read_bd(), ey = r.read_bd(), ez = r.read_bd();

    // text_midpt (2RD -- raw doubles!)
    double mx = r.read_rd(), my = r.read_rd();

    // elevation
    double elev = r.read_bd();

    // flag1 (RC -- raw char)
    uint8_t flag1 = r.read_raw_char();

    // user_text: R2007+ in string stream, read_t() handles dispatch.
    std::string text = r.read_t();

    // text_rotation, horiz_dir (BD0)
    double text_rot = r.read_bd();
    double horiz_dir = r.read_bd();

    // ins_scale (3BD_1) -- 3 BD with default 1.0
    double isx = r.read_bd(), isy = r.read_bd(), isz = r.read_bd();

    // ins_rotation (BD0)
    double ins_rot = r.read_bd();

    // R2000+: attachment, lspace_style, lspace_factor, act_measurement
    if (version >= DwgVersion::R2000) {
        (void)r.read_bs();  // attachment
        (void)r.read_bs();  // lspace_style (BS1 = default 1)
        (void)r.read_bd();  // lspace_factor (BD1 = default 1)
        (void)r.read_bd();  // act_measurement
    }

    // R2007+: unknown, flip_arrow1, flip_arrow2
    if (version >= DwgVersion::R2007) {
        (void)r.read_b();  // unknown
        (void)r.read_b();  // flip_arrow1
        (void)r.read_b();  // flip_arrow2
    }

    // clone_ins_pt (2RD0)
    double cix = r.read_rd(), ciy = r.read_rd();

    (void)ex; (void)ey; (void)ez; (void)elev; (void)flag1;
    (void)text_rot; (void)horiz_dir; (void)isx; (void)isy; (void)isz;
    (void)ins_rot; (void)cix; (void)ciy;

    // Type-specific fields -- read definition point and extension points
    // Most dimension types have def_pt (3BD) plus 2-4 more 3BD points
    double dx = 0, dy = 0, dz = 0;
    if (!r.has_error()) {
        // definition point (3BD) -- common to most dimension types
        dx = r.read_bd(); dy = r.read_bd(); dz = r.read_bd();
    }

    // Read remaining type-specific 3BD points (up to 3 more)
    // DIMENSION_LINEAR: xline1_pt(3BD), xline2_pt(3BD), dim_rotation(BD), oblique(BD)
    // DIMENSION_ALIGNED: xline1_pt(3BD), xline2_pt(3BD), dim_rotation(BD)
    // DIMENSION_ANG3PT: def_pt2(3BD), def_pt3(3BD), def_pt4(3BD)
    // etc.
    for (int i = 0; i < 9 && !r.has_error(); ++i) {
        r.read_bd();
    }

    if (!reader_ok(r)) return;

    DimensionEntity dim;
    dim.definition_point = {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    dim.text_midpoint    = {static_cast<float>(mx), static_cast<float>(my), 0.0f};
    dim.text             = std::move(text);
    dim.dimension_type   = static_cast<int32_t>(flag1);
    dim.rotation         = static_cast<float>(text_rot);

    EntityHeader dim_hdr = hdr;
    dim_hdr.type = EntityType::Dimension;
    dim_hdr.bounds = entity_bounds_dimension(dim);
    scene.add_entity(make_entity<8>(dim_hdr, std::move(dim)));
}

// ============================================================
// HATCH (DWG type 49) -> converted to SolidEntity (index 16)
// Tessellates boundary paths into filled triangles.
// ============================================================

// Tessellate a circular arc into line segments and append to verts.
static void tessellate_arc(double cx, double cy, double radius,
                double start_angle, double end_angle,
                std::vector<Vec3>& verts) {
    if (radius <= 0.0) return;
    // Normalize angles to [0, 2pi)
    while (start_angle < 0.0) start_angle += math::TWO_PI;
    while (end_angle < 0.0) end_angle += math::TWO_PI;
    start_angle = fmod(start_angle, math::TWO_PI);
    end_angle   = fmod(end_angle,   math::TWO_PI);

    double sweep = end_angle - start_angle;
    if (sweep < 0.0) sweep += math::TWO_PI;
    int segments = std::max(4, static_cast<int>(std::ceil(sweep * 32.0 / math::TWO_PI)));
    segments = std::min(segments, 360);

    // Start point
    verts.push_back({static_cast<float>(cx + radius * std::cos(start_angle)),
                     static_cast<float>(cy + radius * std::sin(start_angle)), 0.0f});
    for (int i = 1; i <= segments; ++i) {
        double a = start_angle + sweep * i / segments;
        verts.push_back({static_cast<float>(cx + radius * std::cos(a)),
                         static_cast<float>(cy + radius * std::sin(a)), 0.0f});
    }
}

void parse_hatch(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version) {
    size_t hatch_start = r.bit_offset();
    size_t hatch_limit = r.bit_limit();

    auto dbg_hatch_fail = [&](int stage) {
        static int g_hatch_fail = 0;
        if (g_hatch_fail < 10) {
            dwg_debug_log("[DWG HATCH] fail stage=%d start=%zu limit=%zu now=%zu\n",
                    stage, hatch_start, hatch_limit, r.bit_offset());
            g_hatch_fail++;
        }
    };

    // R2004+: gradient fill data comes first (all fields in main stream)
    if (version >= DwgVersion::R2004) {
        (void)r.read_bl();   // is_gradient_fill
        (void)r.read_bl();   // reserved
        (void)r.read_bd();   // gradient_angle
        (void)r.read_bd();   // gradient_shift
        (void)r.read_bl();   // single_color_gradient
        (void)r.read_bd();   // gradient_tint
        uint32_t num_colors = r.read_bl();     // num_colors
        if (!reader_ok(r) || num_colors > 1000) { dbg_hatch_fail(1); return; }
        for (uint32_t c = 0; c < num_colors; ++c) {
            (void)r.read_bd();                 // shift_value
            (void)r.read_cmc_r2004(version);   // color (R2004+ CMC)
        }
        // gradient_name: R2007+ uses string stream via read_t()
        if (version < DwgVersion::R2007) {
            (void)r.read_tv();
        } else {
            (void)r.read_t();  // gradient_name from string stream
        }
    }

    (void)r.read_bd();  // elevation
    r.read_bd(); r.read_bd(); r.read_bd();  // extrusion x,y,z (3BD)

    // pattern_name: R2007+ from string stream (via read_t()), pre-R2007 from main stream
    std::string pattern_name;
    if (version < DwgVersion::R2007) {
        pattern_name = r.read_tv();
    } else {
        pattern_name = r.read_t();  // R2007+ string stream
    }

    bool is_solid_fill = r.read_b();    // is_solid_fill
    (void)r.read_b();                   // is_associative
    uint32_t num_boundary_paths = r.read_bl();

    if (!reader_ok(r)) { dbg_hatch_fail(2); return; }
    if (num_boundary_paths > 10000) { dbg_hatch_fail(3); return; }

    // Collect all boundary paths into loops before emitting any entity.
    std::vector<HatchEntity::BoundaryLoop> loops;

    for (uint32_t p = 0; p < num_boundary_paths; ++p) {
        std::vector<Vec3> verts;

        uint32_t path_flag = r.read_bl();   // BL flag (not BS)
        bool is_polyline = (path_flag & 0x02) != 0;

        if (!is_polyline) {
            // Edge-defined boundary path
            uint32_t num_edges = r.read_bl();  // num_segs_or_paths
            if (!reader_ok(r) || num_edges > 10000) { dbg_hatch_fail(4); r.set_bit_offset(r.bit_limit()); return; }
            for (uint32_t e = 0; e < num_edges; ++e) {
                if (r.has_error()) {
                    dbg_hatch_fail(5);
                    return;
                }
                uint8_t curve_type = r.read_raw_char();  // RC curve_type (not BS)
                if (r.has_error()) {
                    dbg_hatch_fail(5);
                    return;
                }
                if (curve_type == 1) {  // Line edge (2RD + 2RD)
                    double x1 = r.read_rd(), y1 = r.read_rd();
                    double x2 = r.read_rd(), y2 = r.read_rd();
                    verts.push_back({static_cast<float>(x1), static_cast<float>(y1), 0.0f});
                    verts.push_back({static_cast<float>(x2), static_cast<float>(y2), 0.0f});
                } else if (curve_type == 2) {  // Circular arc edge (2RD + BD*3 + B)
                    double cx = r.read_rd(), cy = r.read_rd();
                    double radius = r.read_bd();
                    double sa = r.read_bd(), ea = r.read_bd();
                    (void)r.read_b();  // is_ccw
                    if (radius > 1e-12) {
                        tessellate_arc(cx, cy, radius, sa, ea, verts);
                    }
                } else if (curve_type == 3) {  // Elliptical arc edge (2RD + 2RD + BD*3 + B)
                    double ecx = r.read_rd(), ecy = r.read_rd();  // center
                    double emx = r.read_rd(), emy = r.read_rd();  // endpoint (major axis)
                    (void)r.read_bd();  // minor/major ratio
                    (void)r.read_bd();  // start_angle
                    (void)r.read_bd();  // end_angle
                    (void)r.read_b();   // is_ccw
                    verts.push_back({static_cast<float>(ecx), static_cast<float>(ecy), 0.0f});
                    verts.push_back({static_cast<float>(emx), static_cast<float>(emy), 0.0f});
                } else if (curve_type == 4) {  // Spline edge
                    uint32_t degree = r.read_bl();
                    bool is_rational = r.read_b();
                    (void)r.read_b();   // is_periodic
                    uint32_t num_knots = r.read_bl();
                    if (num_knots > 10000) { r.set_bit_offset(r.bit_limit()); return; }
                    for (uint32_t k = 0; k < num_knots; ++k) (void)r.read_bd();
                    uint32_t num_ctrl = r.read_bl();
                    if (num_ctrl > 10000) { r.set_bit_offset(r.bit_limit()); return; }
                    for (uint32_t c2 = 0; c2 < num_ctrl; ++c2) {
                        (void)r.read_rd(); (void)r.read_rd();  // 2RD control point
                        if (is_rational) (void)r.read_bd();    // weight
                    }
                    if (version >= DwgVersion::R2010) {
                        uint32_t num_fit = r.read_bl();
                        if (num_fit > 10000) { r.set_bit_offset(r.bit_limit()); return; }
                        for (uint32_t f = 0; f < num_fit; ++f) {
                            (void)r.read_rd(); (void)r.read_rd();  // 2RD fit point
                        }
                        (void)r.read_rd(); (void)r.read_rd();  // start tangent 2RD
                        (void)r.read_rd(); (void)r.read_rd();  // end tangent 2RD
                    }
                } else {
                    // Unknown edge type -- stop reading this path's edges
                    break;
                }
            }
        } else {
            // Polyline boundary path
            bool bulges_present = r.read_b();  // B bulges_present
            (void)r.read_b();                  // B closed
            uint32_t num_verts = r.read_bl();  // BL num_segs_or_paths
            if (!reader_ok(r) || num_verts > 100000) { dbg_hatch_fail(6); r.set_bit_offset(r.bit_limit()); return; }
            verts.reserve(num_verts);
            for (uint32_t i = 0; i < num_verts; ++i) {
                double vx = r.read_rd();  // 2RD point
                double vy = r.read_rd();
                verts.push_back({static_cast<float>(vx), static_cast<float>(vy), 0.0f});
                if (bulges_present) (void)r.read_bd();  // bulge
            }
        }

        // Read num_boundary_handles (skip; handle data read later in handle stream)
        (void)r.read_bl();

        // Clean up vertices: remove duplicate consecutive points
        if (verts.size() < 3) continue;
        std::vector<Vec3> clean;
        clean.push_back(verts[0]);
        for (size_t i = 1; i < verts.size(); ++i) {
            const Vec3& a = clean.back();
            const Vec3& b = verts[i];
            if (std::abs(a.x - b.x) > 1e-9f || std::abs(a.y - b.y) > 1e-9f) {
                clean.push_back(b);
            }
        }
        if (clean.size() < 3) continue;

        HatchEntity::BoundaryLoop loop;
        loop.vertices = std::move(clean);
        loop.is_closed = true;
        loops.push_back(std::move(loop));
    }

    // Guard: if reader is exhausted before we reach post-path data, bail out.
    // Some large hatches exhaust the bit stream during boundary path reading.
    if (!reader_ok(r)) { dbg_hatch_fail(7); return; }

    // Read post-path data (pattern definition for non-solid fills)
    (void)r.read_bs();  // style
    (void)r.read_bs();  // pattern_type
    float pattern_angle = 0.0f;
    float pattern_scale = 1.0f;
    if (!is_solid_fill) {
        pattern_angle = static_cast<float>(r.read_bd());  // angle
        pattern_scale = static_cast<float>(r.read_bd());  // scale_spacing
        (void)r.read_b();   // double_flag
        uint32_t num_deflines = r.read_bs();
        if (num_deflines <= 10000) {
            for (uint32_t d = 0; d < num_deflines && reader_ok(r); ++d) {
                (void)r.read_bd();  // angle
                (void)r.read_bd(); (void)r.read_bd();  // pt0
                (void)r.read_bd(); (void)r.read_bd();  // offset
                uint32_t nd = r.read_bs();
                for (uint32_t j = 0; j < nd && reader_ok(r); ++j) (void)r.read_bd();
            }
        }
    }
    // seeds
    if (reader_ok(r)) {
        uint32_t num_seeds = r.read_bl();
        if (num_seeds <= 10000) {
            for (uint32_t s = 0; s < num_seeds && reader_ok(r); ++s) {
                (void)r.read_rd(); (void)r.read_rd();  // 2RD seed point
            }
        }
    }
    if (!reader_ok(r)) { dbg_hatch_fail(7); return; }

    // Emit a single HatchEntity covering all boundary paths.
    // RenderBatcher handles tessellation (solid: TriangleList; pattern: deferred).
    if (loops.empty()) return;

    HatchEntity hatch;
    hatch.pattern_name = std::move(pattern_name);
    hatch.is_solid = true;  // Always render as solid fill (pattern rendering deferred to renderer agent)
    hatch.pattern_angle = pattern_angle;
    hatch.pattern_scale = pattern_scale;
    hatch.loops = std::move(loops);

    EntityHeader hatch_hdr = hdr;
    hatch_hdr.type = EntityType::Hatch;
    hatch_hdr.bounds = entity_bounds_hatch(hatch);
    scene.add_entity(make_entity<9>(hatch_hdr, std::move(hatch)));
}

// ============================================================
// LEADER (DWG type 45) -> EntityVariant index 17
// Binary layout (R2000+):
//   B(unknown)
//   BS(dimension_style) or T(dim_style_name for older versions)
//   B(is_arrowhead_enabled) — R2010+: always B
//   B(path_type)  // 0=straight, 1=spline
//   BL(num_points)
//   num_points × 3BD(points)
//   3BD(arrowhead_block_insertion) or Vec3(0,0,0) if no custom arrow
//   BL(num_arrowhead_vertices)
//   BL(num_clipping_points)
//   3BD(plane_normal) or BE(extrusion)
//   3BD(horizontal_direction)
//   B(has_hookline)
//   BL(leader_creation_type)  // 0=from text, 1=from annotation
// ============================================================
void parse_leader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version) {
    (void)r.read_b();  // unknown_b

    // dimension style: BS for R2000-R2004, T for R2007+
    if (version >= DwgVersion::R2007) {
        (void)r.read_t();   // dim_style_name (string stream)
    } else {
        (void)r.read_bs();  // dim_style_handle_index
    }

    bool has_arrowhead = r.read_b();
    uint8_t path_type = static_cast<uint8_t>(r.read_b());  // 0=straight, 1=spline

    uint32_t num_points = r.read_bl();
    if (!reader_ok(r) || num_points == 0 || num_points > 1000) return;

    std::vector<Vec3> points;
    points.reserve(num_points);
    for (uint32_t i = 0; i < num_points; ++i) {
        double px = r.read_bd();
        double py = r.read_bd();
        double pz = r.read_bd();
        if (!reader_ok(r)) return;
        points.push_back({safe_float(px), safe_float(py), safe_float(pz)});
    }

    // Validate points
    bool valid = true;
    for (const auto& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !is_safe_coord(p.x) || !is_safe_coord(p.y)) {
            valid = false;
            break;
        }
    }
    if (!valid) return;

    // Skip remaining fields (arrowhead, clipping, normal, etc.)
    // We have the point data we need for geometry.

    LeaderEntity leader;
    leader.is_spline = (path_type == 1);
    leader.has_arrowhead = has_arrowhead;

    // Store points in vertex buffer
    int32_t offset = scene.add_polyline_vertices(points.data(), points.size());
    leader.vertex_count = static_cast<int32_t>(points.size());
    leader.vertex_offset = offset;

    // Compute bounds from points
    Bounds3d bounds = Bounds3d::empty();
    for (const auto& p : points) bounds.expand(p);

    EntityHeader leader_hdr = hdr;
    leader_hdr.type = EntityType::Leader;
    leader_hdr.bounds = bounds;
    scene.add_entity(make_entity<17>(leader_hdr, std::move(leader)));
}

} // namespace cad
