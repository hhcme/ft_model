#include "cad/parser/dwg_entity_hatch.h"
#include "cad/parser/dwg_entity_common.h"
#include "cad/parser/dwg_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"
#include "cad/cad_types.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace cad {

// ============================================================
// HATCH (DWG type 78 / 0x4E) -> EntityVariant index 9
//
// Parses boundary loops (polyline and edge-defined), tessellates
// arc edges into line-segment vertices, and emits a HatchEntity.
// Ellipse and spline edges are captured as endpoints for now;
// full tessellation can be added incrementally.
//
// Version families: R2000/AC1015, R2004/AC1018, R2007/AC1021,
// R2010/AC1024, R2013/AC1027, R2018+/AC1032.
// Pipeline stage: entity geometry (after CED common header).
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
                    (void)degree;
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

} // namespace cad
