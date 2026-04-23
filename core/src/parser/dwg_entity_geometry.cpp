#include "cad/parser/dwg_entity_common.h"
#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cad {

// ============================================================
// Debug logging — use dwg_parser.h declarations directly
// ============================================================

// Half-float decoder (IEEE 754 binary16 -> float)
static inline float half_to_float(uint16_t h) {
    uint16_t s = (h >> 15) & 1;
    int exp = static_cast<int>((h >> 10) & 0x1F);
    uint16_t frac = h & 0x3FF;
    if (exp == 0) {
        float f = frac * 0.0009765625f;
        return s ? -f : f;
    } else if (exp == 31) {
        return s ? -1e9f : 1e9f;
    } else {
        float f = (1.0f + frac * 0.0009765625f) * powf(2.0f, static_cast<float>(exp - 15));
        return s ? -f : f;
    }
}

// ============================================================
// LINE (DWG type 19) -> EntityVariant index 0
// R2000+: B(z_is_zero), RD(start.x), DD(end.x,start.x), RD(start.y),
//   DD(end.y,start.y), conditional [RD(start.z), DD(end.z,start.z)],
//   BT(thickness), BE(extrusion).
// ============================================================
void parse_line(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                DwgVersion version) {
    bool z_is_zero = r.read_b();
    double sx = r.read_rd();
    double ex = r.read_dd(sx);
    double sy = r.read_rd();
    double ey = r.read_dd(sy);
    double sz = 0.0, ez = 0.0;
    if (!z_is_zero) {
        sz = r.read_rd();
        ez = r.read_dd(sz);
    }
    (void)r.read_bt();  // thickness
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);  // extrusion

    if (!reader_ok(r)) return;

    if (!is_safe_coord(sx) || !is_safe_coord(sy) ||
        !is_safe_coord(ex) || !is_safe_coord(ey)) return;

    LineEntity line;
    line.start = {safe_float(sx), safe_float(sy), safe_float(sz)};
    line.end   = {safe_float(ex), safe_float(ey), safe_float(ez)};
    EntityHeader line_hdr = hdr;
    line_hdr.bounds = entity_bounds_line(line);
    scene.add_entity(make_entity<0>(line_hdr, std::move(line)));
}

// ============================================================
// CIRCLE (DWG type 18) -> EntityVariant index 1
// R13+: 3BD(center), BD(radius), BT(thickness), BE(extrusion).
// ============================================================
void parse_circle(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion /*version*/) {
    double cx = r.read_bd();
    double cy = r.read_bd();
    double cz = r.read_bd();
    double radius = r.read_bd();
    r.read_bt();  // thickness
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);

    if (!reader_ok(r)) return;

    // R2004 object-map offsets can land on interior positions. Reject circles
    // with non-finite or sentinel coordinates.
    bool circle_invalid = !is_safe_coord(cx) || !is_safe_coord(cy) || !is_safe_coord(cz) ||
        !std::isfinite(radius) || radius <= 0.0 || radius > 1e9;
    if (circle_invalid) {
        static int g_circ_reject = 0;
        if (g_circ_reject < 5) {
            dwg_debug_log("[DWG CIRCLE_REJECT] handle=%lld cx=%f cy=%f cz=%f r=%f\n",
                          static_cast<long long>(hdr.dwg_handle), cx, cy, cz, radius);
            g_circ_reject++;
        }
        return;
    }

    CircleEntity circle;
    const bool has_custom_ocs = !is_default_extrusion(nx, ny, nz);
    const OcsBasis basis = has_custom_ocs ? make_ocs_basis(nx, ny, nz) : OcsBasis{};
    circle.center = has_custom_ocs
        ? ocs_point_to_wcs(cx, cy, cz, basis)
        : Vec3{safe_float(cx), safe_float(cy), safe_float(cz)};
    circle.radius = safe_float(radius);
    circle.normal = {safe_float(nx), safe_float(ny), safe_float(nz)};
    circle.start_angle = 0.0f;
    circle.end_angle   = math::TWO_PI;
    EntityHeader circ_hdr = hdr;
    circ_hdr.type = EntityType::Circle;
    circ_hdr.bounds = entity_bounds_circle(circle);
    scene.add_entity(make_entity<1>(circ_hdr, std::move(circle)));
}

// ============================================================
// ARC (DWG type 17) -> EntityVariant index 2
// NOTE: DWG stores arc angles in RADIANS — keep as-is.
// ============================================================
void parse_arc(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
               DwgVersion /*version*/) {
    double cx = r.read_bd();
    double cy = r.read_bd();
    double cz = r.read_bd();
    double radius = r.read_bd();
    r.read_bt();  // thickness
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);
    double start_angle = r.read_bd();
    double end_angle   = r.read_bd();

    if (!reader_ok(r)) return;

    // Validate coordinates: must be finite, representable as float, and within
    // CAD coordinate range.  Angles must be finite (no double->float overflow).
    bool arc_invalid = !is_safe_coord(cx) || !is_safe_coord(cy) || !is_safe_coord(cz) ||
        !std::isfinite(radius) || radius <= 0.0 || radius > 1e9 ||
        !std::isfinite(start_angle) || std::abs(start_angle) > 1e6 ||
        !std::isfinite(end_angle) || std::abs(end_angle) > 1e6;
    if (arc_invalid) {
        static int g_arc_reject = 0;
        if (g_arc_reject < 5) {
            dwg_debug_log("[DWG ARC_REJECT] handle=%lld cx=%f cy=%f cz=%f r=%f sa=%f ea=%f\n",
                          static_cast<long long>(hdr.dwg_handle), cx, cy, cz, radius, start_angle, end_angle);
            g_arc_reject++;
        }
        return;
    }

    ArcEntity arc;
    const bool has_custom_ocs = !is_default_extrusion(nx, ny, nz);
    const OcsBasis basis = has_custom_ocs ? make_ocs_basis(nx, ny, nz) : OcsBasis{};
    arc.center      = has_custom_ocs
        ? ocs_point_to_wcs(cx, cy, cz, basis)
        : Vec3{safe_float(cx), safe_float(cy), safe_float(cz)};
    arc.radius      = safe_float(radius);
    arc.start_angle = safe_float(start_angle);
    arc.end_angle   = safe_float(end_angle);
    EntityHeader arc_hdr = hdr;
    arc_hdr.type = EntityType::Arc;
    arc_hdr.bounds = entity_bounds_arc(arc);
    scene.add_entity(make_entity<2>(arc_hdr, std::move(arc)));
}

// ============================================================
// LWPOLYLINE (DWG type 48) -> EntityVariant index 4
// DWG flag bits (different from DXF!):
//   1=extrusion, 2=thickness, 4=constwidth, 8=elevation,
//   16=num_bulges, 32=num_widths, 512=closed, 256=plinegen, 1024=vertexidcount
// ============================================================
void parse_lwpolyline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                      DwgVersion version) {
    size_t lw_start = r.bit_offset();
    size_t lw_limit = r.bit_limit();

    auto dbg_lw_fail = [&](const char* reason) {
        static int g_lw_fail = 0;
        if (g_lw_fail < 5) {
            dwg_debug_log("[DWG LWPOLYLINE] fail %s start=%zu limit=%zu now=%zu\n",
                    reason, lw_start, lw_limit, r.bit_offset());
            g_lw_fail++;
        }
    };

    uint16_t flag = r.read_bs();

    if (flag & 0x04) r.read_bd();  // const_width
    double elevation = 0.0;
    if (flag & 0x08) elevation = r.read_bd();
    if (flag & 0x02) r.read_bt();  // thickness
    double nx = 0.0, ny = 0.0, nz = 1.0;
    if (flag & 0x01) {
        // LWPOLYLINE extrusion is 3BD (not BE per libredwg)
        nx = r.read_bd();
        ny = r.read_bd();
        nz = r.read_bd();
    }

    uint32_t num_points = r.read_bl();
    if (!reader_ok(r)) { dbg_lw_fail("num_points"); return; }

    // Guard against degenerate/overflowed reads: only enforce upper bound
    // if num_points is implausibly large (>10M vertices), treating exact 0
    // as valid (handled by empty_vertices check later).
    if (num_points > 10000000) { dbg_lw_fail("num_points_bounds"); return; }

    uint32_t num_bulges = 0;
    uint32_t num_widths = 0;
    if (flag & 0x10) num_bulges = r.read_bl();
    if (version >= DwgVersion::R2010 && (flag & 0x400)) {
        r.read_bl();  // vertexidcount, skip
    }
    if (flag & 0x20) num_widths = r.read_bl();

    auto dbg_lw_fail2 = [&](const char* reason) {
        static int g_lw_fail2 = 0;
        if (g_lw_fail2 < 10) {
            dwg_debug_log("[DWG LWPOLYLINE] fail %s flag=0x%x np=%u nb=%u nw=%u start=%zu limit=%zu now=%zu\n",
                    reason, flag, num_points, num_bulges, num_widths, lw_start, lw_limit, r.bit_offset());
            g_lw_fail2++;
        }
    };

    if (num_bulges > 10000000 || num_widths > 10000000) { dbg_lw_fail2("bulge_width_bounds"); return; }

    std::vector<Vec3> vertices;
    vertices.reserve(num_points);

    // Read points as 2DD_VECTOR: first point is 2 RDs, rest are 2 DDs
    double prev_x = 0.0, prev_y = 0.0;
    for (uint32_t i = 0; i < num_points; ++i) {
        double x, y;
        if (i == 0) {
            x = r.read_rd();
            y = r.read_rd();
        } else {
            x = r.read_dd(prev_x);
            y = r.read_dd(prev_y);
        }
        prev_x = x;
        prev_y = y;
        vertices.push_back({static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(elevation)});
    }

    // Read bulges (contiguous array, not interleaved)
    std::vector<float> bulges;
    if (num_bulges > 0) {
        bulges.reserve(num_bulges);
        for (uint32_t i = 0; i < num_bulges; ++i) {
            bulges.push_back(static_cast<float>(r.read_bd()));
        }
    }

    // Skip vertexids for R2010+ (already skipped count read above)

    // Skip widths (pairs of BD)
    for (uint32_t i = 0; i < num_widths; ++i) {
        r.read_bd();  // start_width
        r.read_bd();  // end_width
    }

    if (r.has_error()) { dbg_lw_fail2("reader_error"); return; }
    if (vertices.empty()) { dbg_lw_fail2("empty_vertices"); return; }

    // Validate vertex coordinates: reject if any vertex has NaN/Inf or sentinel values
    bool vertices_invalid = false;
    for (const Vec3& v : vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            std::abs(v.x) > 1e9f || std::abs(v.y) > 1e9f || std::abs(v.z) > 1e9f) {
            vertices_invalid = true;
            break;
        }
    }
    if (vertices_invalid) {
        static int g_lw_reject = 0;
        if (g_lw_reject < 5) {
            dwg_debug_log("[DWG LWPOLYLINE_REJECT] handle=%lld num_points=%zu\n",
                          static_cast<long long>(hdr.dwg_handle), vertices.size());
            g_lw_reject++;
        }
        return;
    }

    if (!is_default_extrusion(nx, ny, nz)) {
        const OcsBasis basis = make_ocs_basis(nx, ny, nz);
        for (Vec3& vertex : vertices) {
            vertex = ocs_point_to_wcs(vertex.x, vertex.y, vertex.z, basis);
        }
    }

    int32_t offset = scene.add_polyline_vertices(vertices.data(), vertices.size());

    PolylineEntity poly;
    poly.vertex_offset = offset;
    poly.vertex_count  = static_cast<int32_t>(vertices.size());
    poly.is_closed     = (flag & 512) != 0;
    poly.bulges        = std::move(bulges);

    EntityHeader poly_hdr = hdr;
    poly_hdr.type = EntityType::LwPolyline;
    // Polyline bounds from vertex data
    poly_hdr.bounds = Bounds3d::empty();
    for (const auto& v : vertices) {
        poly_hdr.bounds.expand(v);
    }
    scene.add_entity(make_entity<4>(poly_hdr, std::move(poly)));
}

// ============================================================
// POINT (DWG type 27) -> EntityVariant index 11 (Vec3)
// R13+: BD(x), BD(y), BD(z), BT(thickness), BE(extrusion), BD(x_angle).
// ============================================================
void parse_point(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion /*version*/) {
    double dx = r.read_bd();
    double dy = r.read_bd();
    double dz = r.read_bd();
    r.read_bt();  // thickness
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);
    r.read_bd();  // x_axis angle (display orientation)

    if (!reader_ok(r)) return;

    if (!is_safe_coord(dx) || !is_safe_coord(dy)) return;

    Vec3 point{safe_float(dx), safe_float(dy), safe_float(dz)};

    EntityHeader pt_hdr = hdr;
    pt_hdr.type = EntityType::Point;
    pt_hdr.bounds = Bounds3d::from_point(point);
    scene.add_entity(make_entity<11>(pt_hdr, std::move(point)));
}

// ============================================================
// ELLIPSE (DWG type 35) -> EntityVariant index 12
// NOTE: DWG ELLIPSE stores the major-axis endpoint vector, followed by
// minor_to_major_ratio. Angles are parametric and already in RADIANS.
// Angles are parametric and already in RADIANS — do NOT convert.
// ============================================================
void parse_ellipse(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                   DwgVersion /*version*/) {
    double cx = r.read_bd();
    double cy = r.read_bd();
    double cz = r.read_bd();
    double smx = r.read_bd();  // major axis endpoint x (relative to center)
    double smy = r.read_bd();  // major axis endpoint y
    double smz = r.read_bd();  // major axis endpoint z
    double enx = r.read_bd();  // extrusion x
    double eny = r.read_bd();  // extrusion y
    double enz = r.read_bd();  // extrusion z
    double ratio        = r.read_bd();
    double start_angle  = r.read_bd();
    double end_angle    = r.read_bd();

    if (!reader_ok(r)) return;

    if (!is_safe_coord(cx) || !is_safe_coord(cy) || !is_safe_coord(cz)) return;

    const bool has_custom_ocs = !is_default_extrusion(enx, eny, enz);
    const OcsBasis basis = has_custom_ocs ? make_ocs_basis(enx, eny, enz) : OcsBasis{};
    Vec3 major_axis = has_custom_ocs
        ? ocs_vector_to_wcs(smx, smy, smz, basis)
        : Vec3{safe_float(smx), safe_float(smy), safe_float(smz)};
    Vec3 center = has_custom_ocs
        ? ocs_point_to_wcs(cx, cy, cz, basis)
        : Vec3{safe_float(cx), safe_float(cy), safe_float(cz)};

    double major_radius = major_axis.length();
    if (!std::isfinite(major_radius) || major_radius <= 0.0 ||
        !std::isfinite(ratio) || ratio <= 0.0 ||
        !std::isfinite(start_angle) || !std::isfinite(end_angle)) {
        return;
    }

    // Compute the rotation angle of the major axis in the XY plane
    float rotation = std::atan2(major_axis.y, major_axis.x);

    CircleEntity ellipse;
    ellipse.center       = center;
    ellipse.radius       = static_cast<float>(major_radius);
    ellipse.normal       = {static_cast<float>(enx), static_cast<float>(eny), static_cast<float>(enz)};
    ellipse.minor_radius = static_cast<float>(major_radius * ratio);
    ellipse.rotation     = rotation;
    ellipse.start_angle  = static_cast<float>(start_angle);
    ellipse.end_angle    = static_cast<float>(end_angle);

    EntityHeader ell_hdr = hdr;
    ell_hdr.type = EntityType::Ellipse;
    ell_hdr.bounds = entity_bounds_circle(ellipse);
    scene.add_entity(make_entity<12>(ell_hdr, std::move(ellipse)));
}

// ============================================================
// SPLINE (DWG type 36) -> EntityVariant index 5
// ============================================================
void parse_spline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version) {
    // SPLINE always stores scenario first. R2013+ adds splineflags/knotparam
    // after it and may derive the effective scenario from those fields.
    uint32_t scenario = r.read_bl();
    uint32_t knotparam = 0;
    if (version >= DwgVersion::R2013) {
        uint32_t splineflags = r.read_bl();
        knotparam = r.read_bl();
        if (splineflags & 1U) {
            scenario = 2;
        }
        if (knotparam == 15U) {
            scenario = 1;
        }
    }

    uint32_t degree = r.read_bl();
    if (!reader_ok(r) || (scenario != 1U && scenario != 2U) || degree > 32U) {
        return;
    }

    SplineEntity spline;
    spline.degree = static_cast<int32_t>(degree);

    auto add_point = [](std::vector<Vec3>& points, double x, double y, double z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
        if (std::abs(x) > 1.0e8 || std::abs(y) > 1.0e8 || std::abs(z) > 1.0e8) return;
        points.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
    };

    if (scenario == 1U) {
        // Scenario 1: control point spline
        spline.is_rational = r.read_b();
        spline.is_closed = r.read_b();
        spline.is_periodic = r.read_b();
        (void)r.read_bd();  // knot_tol
        (void)r.read_bd();  // ctrl_tol
        uint32_t num_knots = r.read_bl();
        uint32_t num_control_pts = r.read_bl();
        bool weighted = r.read_b();

        if (!reader_ok(r) || num_knots > 100000 || num_control_pts > 100000) return;

        spline.knots.reserve(num_knots);
        for (uint32_t i = 0; i < num_knots; ++i) {
            spline.knots.push_back(static_cast<float>(r.read_bd()));
        }

        spline.control_points.reserve(num_control_pts);
        if (weighted || spline.is_rational) {
            spline.weights.reserve(num_control_pts);
        }
        for (uint32_t i = 0; i < num_control_pts; ++i) {
            double x = r.read_bd();
            double y = r.read_bd();
            double z = r.read_bd();
            add_point(spline.control_points, x, y, z);
            if (weighted) {
                const double weight = r.read_bd();
                if (std::isfinite(weight) && weight > 0.0 && weight < 1.0e6) {
                    spline.weights.push_back(static_cast<float>(weight));
                } else {
                    spline.weights.push_back(1.0f);
                }
            }
        }
    } else {
        // Scenario 2: fit point / bezier spline
        (void)r.read_bd();  // fit_tol
        (void)r.read_bd(); (void)r.read_bd(); (void)r.read_bd();  // beg_tan_vec 3BD
        (void)r.read_bd(); (void)r.read_bd(); (void)r.read_bd();  // end_tan_vec 3BD
        uint32_t num_fit_pts = r.read_bl();

        if (!reader_ok(r) || num_fit_pts > 100000) return;

        spline.fit_points.reserve(num_fit_pts);
        for (uint32_t i = 0; i < num_fit_pts; ++i) {
            double x = r.read_bd();
            double y = r.read_bd();
            double z = r.read_bd();
            add_point(spline.fit_points, x, y, z);
        }
    }

    if (!reader_ok(r)) return;

    if (spline.control_points.size() < 2 && spline.fit_points.size() < 2) {
        return;
    }

    EntityHeader spl_hdr = hdr;
    spl_hdr.type = EntityType::Spline;
    spl_hdr.bounds = entity_bounds_spline(spline);
    scene.add_entity(make_entity<5>(spl_hdr, std::move(spline)));
}

// ============================================================
// SOLID (DWG type 31) -> EntityVariant index 16
// DWG corner order is 1,2,3,4. DXF convention (which our struct
// SOLID (DWG type 31) / TRACE (DWG type 32) -> EntityVariant index 16
// R13+: BT0(thickness), BD(elevation), 2RD(corner1..4), BE(extrusion).
// DXF corner order is 1,2,4,3 — DWG native order (what the binary format
// follows) is 1,2,4,3 — so we swap corners 3 and 4 during read.
// ============================================================
void parse_solid(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion /*version*/) {
    (void)r.read_bt();  // thickness
    double elevation = r.read_bd();

    double c1x = r.read_rd(), c1y = r.read_rd();
    double c2x = r.read_rd(), c2y = r.read_rd();
    double c3x = r.read_rd(), c3y = r.read_rd();
    double c4x = r.read_rd(), c4y = r.read_rd();

    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);

    if (!reader_ok(r)) return;

    SolidEntity solid;
    solid.corner_count = 4;
    float ez = static_cast<float>(elevation);

    // Reorder from DWG 1,2,3,4 to DXF 1,2,4,3 convention
    solid.corners[0] = {static_cast<float>(c1x), static_cast<float>(c1y), ez};
    solid.corners[1] = {static_cast<float>(c2x), static_cast<float>(c2y), ez};
    solid.corners[2] = {static_cast<float>(c4x), static_cast<float>(c4y), ez};
    solid.corners[3] = {static_cast<float>(c3x), static_cast<float>(c3y), ez};

    EntityHeader sol_hdr = hdr;
    sol_hdr.type = EntityType::Solid;
    sol_hdr.bounds = entity_bounds_solid(solid);
    scene.add_entity(make_entity<16>(sol_hdr, std::move(solid)));
}

// ============================================================
// 3DFACE (DWG type 28) -> EntityVariant index 16 (reuses SolidEntity)
// 3DFACE is always a 4-vertex surface with no thickness.
// ============================================================
void parse_3dface(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion /*version*/) {
    double c1x = r.read_bd(), c1y = r.read_bd();
    double c2x = r.read_bd(), c2y = r.read_bd();
    double c3x = r.read_bd(), c3y = r.read_bd();
    double c4x = r.read_bd(), c4y = r.read_bd();

    if (!reader_ok(r)) return;

    SolidEntity solid;
    solid.corners[0] = {static_cast<float>(c1x), static_cast<float>(c1y), 0.0f};
    solid.corners[1] = {static_cast<float>(c2x), static_cast<float>(c2y), 0.0f};
    solid.corners[2] = {static_cast<float>(c4x), static_cast<float>(c4y), 0.0f};
    solid.corners[3] = {static_cast<float>(c3x), static_cast<float>(c3y), 0.0f};
    solid.corner_count = 4;

    EntityHeader face_hdr = hdr;
    face_hdr.type = EntityType::Solid;
    face_hdr.bounds = entity_bounds_solid(solid);
    scene.add_entity(make_entity<16>(face_hdr, std::move(solid)));
}

// ============================================================
// POLYLINE_PFACE (DWG type 29) -> EntityVariant index 4
// Contains vertex list followed by face index list.
// Output: LINE entities forming wireframe edges of each face.
// ============================================================

void parse_polyline_pface(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                          DwgVersion version) {
    uint32_t num_verts = r.read_bl();
    uint32_t num_faces = r.read_bl();

    if (!reader_ok(r) || num_verts == 0 || num_verts > 100000 ||
        num_faces == 0 || num_faces > 100000) {
        return;
    }

    // For R2010 with limited bits (~243 remaining): cap num_verts to what fits.
    // Each vertex needs 3 half-floats = 48 bits. Face indices need ~20 bits.
    // Available: ~243 - 20 (num_verts/num_faces BL) = ~223 bits for vertices.
    if (version >= DwgVersion::R2010) {
        size_t rem = r.remaining_bits();
        size_t verts_bits = num_verts * 48;  // 3 coords x 16 bits
        size_t max_verts = (rem > 20) ? ((rem - 20) / 48) : 0;
        if (num_verts > max_verts && max_verts > 0) {
            // Clamp to available bits; skip excess vertices silently.
            num_verts = static_cast<uint32_t>(max_verts);
        }
    }

    // Read all vertex coordinates.
    std::vector<Vec3> vertices;
    vertices.reserve(num_verts);
    if (version >= DwgVersion::R2010) {
        // R2010: 3 half-floats per vertex (16 bits each).
        for (uint32_t i = 0; i < num_verts && reader_ok(r); ++i) {
            uint16_t hx = r.read_rs();
            uint16_t hy = r.read_rs();
            uint16_t hz = r.read_rs();
            vertices.push_back({half_to_float(hx), half_to_float(hy), half_to_float(hz)});
        }
    } else {
        // Pre-R2010: 3 BD values per vertex.
        for (uint32_t i = 0; i < num_verts; ++i) {
            double x = r.read_bd(), y = r.read_bd(), z = r.read_bd();
            vertices.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
        }
    }

    if (!reader_ok(r)) return;

    // Guard against bit exhaustion: if there are not enough bits remaining to read
    // even one minimal face (face_size BL + 3 vertex index BLs), bail out early.
    // Each BL needs up to 34 bits (2-bit code + 32-bit value). A minimal face
    // needs 1 BL for face_size + 3 BLs for 3 vertex indices = 4 x 34 = 136 bits.
    // Without this guard, the face_size read can consume garbage from adjacent
    // entity data, producing out-of-range indices that produce no output.
    size_t rem = r.remaining_bits();
    constexpr size_t kMinFaceBits = 4 * 34;  // 1 face_size BL + 3 index BLs
    if (num_verts > 0 && (rem < kMinFaceBits || !reader_ok(r))) {
        return;
    }

    // Read face index lists. Each face: BL(size) + size x BL(1-based indices).
    std::vector<std::vector<uint32_t>> face_verts;
    face_verts.reserve(num_faces);

    for (uint32_t f = 0; f < num_faces && reader_ok(r); ++f) {
        uint32_t face_size = r.read_bl();
        if (!reader_ok(r) || face_size < 3 || face_size > 10000) break;

        std::vector<uint32_t> fv;
        fv.reserve(face_size);
        for (uint32_t j = 0; j < face_size && reader_ok(r); ++j) {
            uint32_t idx = r.read_bl();
            if (idx >= 1 && idx <= num_verts) {
                fv.push_back(idx - 1);
            }
        }
        face_verts.push_back(std::move(fv));
    }

    if (!reader_ok(r)) return;

    // Emit LINE entities: connect consecutive vertices in each face (closed loop)
    for (const auto& fv : face_verts) {
        if (fv.size() < 3) continue;
        for (size_t j = 1; j < fv.size(); ++j) {
            LineEntity line;
            line.start = vertices[fv[j - 1]];
            line.end   = vertices[fv[j]];
            EntityHeader lhdr = hdr;
            lhdr.bounds = entity_bounds_line(line);
            scene.add_entity(make_entity<0>(lhdr, std::move(line)));
        }
        // Close the loop
        LineEntity close;
        close.start = vertices[fv.back()];
        close.end   = vertices[fv.front()];
        EntityHeader clhdr = hdr;
        clhdr.bounds = entity_bounds_line(close);
        scene.add_entity(make_entity<0>(clhdr, std::move(close)));
    }
}

// ============================================================
// POLYLINE_MESH (DWG type 30) -> EntityVariant index 4
// M x N grid of vertices forming a mesh surface.
// Output: LINE entities forming the wireframe grid (Mx(N-1) + (M-1)xN lines).
// ============================================================
void parse_polyline_mesh(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene) {
    (void)r.read_bs();  // flags
    uint32_t m_size = r.read_bl();
    uint32_t n_size = r.read_bl();

    if (!reader_ok(r) || m_size == 0 || m_size > 10000 ||
        n_size == 0 || n_size > 10000) {
        return;
    }

    // Read all MxN vertices
    uint32_t total_verts = m_size * n_size;
    std::vector<Vec3> vertices;
    vertices.reserve(total_verts);
    for (uint32_t i = 0; i < total_verts; ++i) {
        double x = r.read_bd(), y = r.read_bd(), z = r.read_bd();
        vertices.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
    }

    if (!reader_ok(r)) return;

    // Emit grid lines: M rows x N columns
    // Row i, col j -> vertex index i * n_size + j
    for (uint32_t i = 0; i < m_size; ++i) {
        for (uint32_t j = 0; j + 1 < n_size; ++j) {
            uint32_t idx0 = i * n_size + j;
            uint32_t idx1 = i * n_size + (j + 1);
            LineEntity line;
            line.start = vertices[idx0];
            line.end   = vertices[idx1];
            EntityHeader lhdr = hdr;
            lhdr.bounds = entity_bounds_line(line);
            scene.add_entity(make_entity<0>(lhdr, std::move(line)));
        }
    }
    for (uint32_t j = 0; j < n_size; ++j) {
        for (uint32_t i = 0; i + 1 < m_size; ++i) {
            uint32_t idx0 = i * n_size + j;
            uint32_t idx1 = (i + 1) * n_size + j;
            LineEntity line;
            line.start = vertices[idx0];
            line.end   = vertices[idx1];
            EntityHeader lhdr = hdr;
            lhdr.bounds = entity_bounds_line(line);
            scene.add_entity(make_entity<0>(lhdr, std::move(line)));
        }
    }
}

// ============================================================
// SEQEND (DWG type 6) -> no entity output
// Terminates POLYLINE entity sequences. No geometry to store.
// ============================================================
void parse_seqend(DwgBitReader& /*r*/, const EntityHeader& /*hdr*/, EntitySink& scene) {
    // SEQEND terminates POLYLINE + VERTEX sequences.
    if (!g_pending_polyline2d.active) return;

    if (g_pending_polyline2d.vertices.size() >= 2) {
        int32_t offset = scene.add_polyline_vertices(
            g_pending_polyline2d.vertices.data(),
            g_pending_polyline2d.vertices.size());

        PolylineEntity poly;
        poly.vertex_offset = offset;
        poly.vertex_count = static_cast<int32_t>(g_pending_polyline2d.vertices.size());
        poly.is_closed = g_pending_polyline2d.closed;
        poly.bulges = std::move(g_pending_polyline2d.bulges);

        EntityHeader poly_hdr = g_pending_polyline2d.header;
        poly_hdr.type = EntityType::Polyline;
        poly_hdr.bounds = Bounds3d::empty();
        for (const auto& v : g_pending_polyline2d.vertices) {
            poly_hdr.bounds.expand(v);
        }
        scene.add_entity(make_entity<3>(poly_hdr, std::move(poly)));
    }

    g_pending_polyline2d = PendingPolyline2d{};
}

// ============================================================
// VERTEX_2D (DWG type 10) -> point entity
// R2000+: RC(flag), 3BD(point), BD(start_width), BD(end_width), BD(bulge),
//         [R2010+: BL0(id)], BD(tangent_dir)
// ============================================================
void parse_vertex_2d(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version) {
    uint8_t flag = r.read_raw_char();  // RC
    double px = r.read_bd(), py = r.read_bd(), pz = r.read_bd();  // 3BD

    // start_width / end_width: if start_width < 0, end = -start_width
    double start_width = r.read_bd();
    if (start_width < 0) {
        start_width = -start_width;
        // end_width = start_width (already read as negative of start)
    } else {
        (void)r.read_bd();  // end_width
    }

    double bulge = r.read_bd();
    (void)start_width; (void)bulge;

    if (version >= DwgVersion::R2010) {
        (void)r.read_bl();  // id (BL0)
    }
    (void)r.read_bd();  // tangent_dir

    if (!reader_ok(r)) return;

    Vec3 pt{static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz)};
    if (g_pending_polyline2d.active) {
        g_pending_polyline2d.vertices.push_back(pt);
        g_pending_polyline2d.bulges.push_back(static_cast<float>(bulge));
        return;
    }

    EntityHeader phdr = hdr;
    phdr.type = EntityType::Point;
    phdr.bounds = Bounds3d::from_point(pt);
    scene.add_entity(make_entity<11>(phdr, std::move(pt)));
}

// ============================================================
// POLYLINE_2D (DWG type 15) -> no entity output (header only)
// R2000+: BS0(flag), BS0(curve_type), BD(start_width), BD(end_width),
//         BT(thickness), BD(elevation), BE(extrusion), [R2004+: BL(num_owned)]
// ============================================================
void parse_polyline_2d(DwgBitReader& r, const EntityHeader& hdr,
                       EntitySink& /*scene*/, DwgVersion version) {
    uint16_t flag = r.read_bs();
    (void)r.read_bs();  // curve_type
    (void)r.read_bd();  // start_width
    (void)r.read_bd();  // end_width
    (void)r.read_bt();  // thickness
    (void)r.read_bd();  // elevation
    double ex, ey, ez;
    r.read_be(ex, ey, ez);
    if (version >= DwgVersion::R2004) {
        (void)r.read_bl();  // num_owned
    }
    if (!reader_ok(r)) return;

    g_pending_polyline2d.active = true;
    g_pending_polyline2d.closed = (flag & 0x01) != 0;
    g_pending_polyline2d.header = hdr;
    g_pending_polyline2d.vertices.clear();
    g_pending_polyline2d.bulges.clear();
}

// ============================================================
// RAY (DWG type 41) -> EntityVariant index 13
// ============================================================
void parse_ray(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
               DwgVersion /*version*/) {
    double sx = r.read_bd(), sy = r.read_bd(), sz = r.read_bd();
    double ux = r.read_bd(), uy = r.read_bd(), uz = r.read_bd();

    if (!reader_ok(r)) return;

    LineEntity ray;
    ray.start = {static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz)};
    ray.end   = {static_cast<float>(sx + ux),
                 static_cast<float>(sy + uy),
                 static_cast<float>(sz + uz)};

    EntityHeader ray_hdr = hdr;
    ray_hdr.type = EntityType::Ray;
    ray_hdr.bounds = Bounds3d::from_point(ray.start);
    ray_hdr.validation_flags |= 0x01u;
    scene.add_entity(make_entity<13>(ray_hdr, std::move(ray)));
}

// ============================================================
// XLINE (DWG type 42) -> EntityVariant index 14
// ============================================================
void parse_xline(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion /*version*/) {
    double bx = r.read_bd(), by = r.read_bd(), bz = r.read_bd();
    double ux = r.read_bd(), uy = r.read_bd(), uz = r.read_bd();

    if (!reader_ok(r)) return;

    LineEntity xline;
    xline.start = {static_cast<float>(bx),
                   static_cast<float>(by),
                   static_cast<float>(bz)};
    xline.end   = {static_cast<float>(bx + ux),
                   static_cast<float>(by + uy),
                   static_cast<float>(bz + uz)};

    EntityHeader xl_hdr = hdr;
    xl_hdr.type = EntityType::XLine;
    xl_hdr.bounds = Bounds3d::from_point(xline.start);
    xl_hdr.validation_flags |= 0x01u;
    scene.add_entity(make_entity<14>(xl_hdr, std::move(xline)));
}

} // namespace cad
