#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/scene/scene_graph.h"
#include "cad/cad_types.h"

#include <cmath>
#include <utility>

namespace cad {

namespace {

// ============================================================
// Constants
// ============================================================
constexpr double kVeryLargeDistance = 1e10;

// ============================================================
// Diagnostic: track which entity types successfully add to scene
// ============================================================
std::unordered_map<uint32_t, size_t> g_success_counts;
std::unordered_map<uint32_t, size_t> g_dispatch_counts;

// ============================================================
// Helper: construct EntityVariant with header and in_place_index data.
// Must use std::in_place_index<N> because EntityData contains
// duplicate types (LineEntity at 0/13/14, CircleEntity at 1/12, etc.)
// ============================================================
template<size_t I, typename T>
inline EntityVariant make_entity(EntityHeader hdr, T data) {
    return EntityVariant{hdr, EntityData{std::in_place_index<I>, std::move(data)}};
}

// ============================================================
// Helper: check reader for error after reads, return true if OK
// ============================================================
inline bool reader_ok(const DwgBitReader& reader) {
    return !reader.has_error();
}

// ============================================================
// LINE (DWG type 19) -> EntityVariant index 0
// R2000+: B(z_is_zero), RD(start.x), DD(end.x,start.x), RD(start.y),
//   DD(end.y,start.y), conditional [RD(start.z), DD(end.z,start.z)],
//   BT(thickness), BE(extrusion).
// ============================================================
static int g_line_dbg = 0;
void parse_line(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                DwgVersion version) {
    size_t start_pos = r.bit_offset();
    size_t bit_lim = r.bit_limit();
    bool z_is_zero = r.read_b();
    size_t p1 = r.bit_offset();
    double sx = r.read_rd();
    size_t p2 = r.bit_offset();
    double ex = r.read_dd(sx);
    size_t p3 = r.bit_offset();
    double sy = r.read_rd();
    size_t p4 = r.bit_offset();
    double ey = r.read_dd(sy);
    size_t p5 = r.bit_offset();
    double sz = 0.0, ez = 0.0;
    if (!z_is_zero) {
        sz = r.read_rd();
        ez = r.read_dd(sz);
    }
    size_t p6 = r.bit_offset();
    (void)r.read_bt();  // thickness
    size_t p7 = r.bit_offset();
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);  // extrusion
    size_t p8 = r.bit_offset();

    bool ok = reader_ok(r);
    if (!ok) {
        static int g_line_fail = 0;
        if (g_line_fail < 20) {
            fprintf(stderr, "[DWG DBG] LINE FAIL start=%zu limit=%zu end=%zu z=%d\n"
                            "            p1=%zu p2=%zu p3=%zu p4=%zu p5=%zu p6=%zu p7=%zu p8=%zu\n",
                    start_pos, bit_lim, r.bit_offset(), (int)z_is_zero,
                    p1, p2, p3, p4, p5, p6, p7, p8);
            g_line_fail++;
        }
    } else {
        static int g_line_ok = 0;
        if (g_line_ok < 5) {
            fprintf(stderr, "[DWG DBG] LINE OK start=%zu limit=%zu end=%zu z=%d\n",
                    start_pos, bit_lim, r.bit_offset(), (int)z_is_zero);
            g_line_ok++;
        }
    }
    if (!ok) {
        return;
    }

    LineEntity line;
    line.start = {static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz)};
    line.end   = {static_cast<float>(ex), static_cast<float>(ey), static_cast<float>(ez)};
    EntityHeader line_hdr = hdr;
    line_hdr.bounds = entity_bounds_line(line);
    scene.add_entity(make_entity<0>(line_hdr, std::move(line)));
}

// ============================================================
// CIRCLE (DWG type 18) -> EntityVariant index 1
// R13+: 3BD(center), BD(radius), BT(thickness), BE(extrusion).
// ============================================================
void parse_circle(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                  DwgVersion /*version*/) {
    double cx = r.read_bd();
    double cy = r.read_bd();
    double cz = r.read_bd();
    double radius = r.read_bd();
    r.read_bt();  // thickness
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);

    if (!reader_ok(r)) return;

    CircleEntity circle;
    circle.center = {static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)};
    circle.radius = static_cast<float>(radius);
    circle.normal = {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)};
    circle.start_angle = 0.0f;
    circle.end_angle   = math::TWO_PI;
    EntityHeader circ_hdr = hdr;
    circ_hdr.bounds = entity_bounds_circle(circle);
    scene.add_entity(make_entity<1>(circ_hdr, std::move(circle)));
}

// ============================================================
// ARC (DWG type 17) -> EntityVariant index 2
// NOTE: DWG stores arc angles in RADIANS — keep as-is.
// ============================================================
void parse_arc(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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

    ArcEntity arc;
    arc.center      = {static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)};
    arc.radius      = static_cast<float>(radius);
    arc.start_angle = static_cast<float>(start_angle);
    arc.end_angle   = static_cast<float>(end_angle);
    EntityHeader arc_hdr = hdr;
    arc_hdr.bounds = entity_bounds_arc(arc);
    scene.add_entity(make_entity<2>(arc_hdr, std::move(arc)));
}

// ============================================================
// LWPOLYLINE (DWG type 48) -> EntityVariant index 4
// DWG flag bits (different from DXF!):
//   1=extrusion, 2=thickness, 4=constwidth, 8=elevation,
//   16=num_bulges, 32=num_widths, 512=closed, 256=plinegen, 1024=vertexidcount
// ============================================================
void parse_lwpolyline(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                      DwgVersion version) {
    size_t lw_start = r.bit_offset();
    size_t lw_limit = r.bit_limit();

    auto dbg_lw_fail = [&](const char* reason) {
        static int g_lw_fail = 0;
        if (g_lw_fail < 5) {
            fprintf(stderr, "[DWG LWPOLYLINE] fail %s start=%zu limit=%zu now=%zu\n",
                    reason, lw_start, lw_limit, r.bit_offset());
            g_lw_fail++;
        }
    };

    uint16_t flag = r.read_bs();

    if (flag & 0x04) r.read_bd();  // const_width
    if (flag & 0x08) r.read_bd();  // elevation
    if (flag & 0x02) r.read_bt();  // thickness
    if (flag & 0x01) {
        // LWPOLYLINE extrusion is 3BD (not BE per libredwg)
        (void)r.read_bd(); (void)r.read_bd(); (void)r.read_bd();
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
            fprintf(stderr, "[DWG LWPOLYLINE] fail %s flag=0x%x np=%u nb=%u nw=%u start=%zu limit=%zu now=%zu\n",
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
        vertices.push_back({static_cast<float>(x), static_cast<float>(y), 0.0f});
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
// TEXT (DWG type 1) -> EntityVariant index 6
// R2000+ compressed format uses dataflags (RC) to control conditional fields.
// ============================================================
void parse_text(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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

    TextEntity txt;
    txt.insertion_point = {static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iz)};
    txt.height          = static_cast<float>(height);
    txt.rotation        = static_cast<float>(rotation);
    txt.width_factor    = static_cast<float>(width_factor);
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
void parse_mtext(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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

    (void)rect_width;
    (void)rect_height;
    (void)flow_dir;
    (void)zaz;

    if (!reader_ok(r)) return;

    float rotation = std::atan2(static_cast<float>(yay), static_cast<float>(xax));

    TextEntity txt;
    txt.insertion_point = {static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iz)};
    txt.height          = static_cast<float>(height);
    txt.rotation        = rotation;
    txt.width_factor    = 1.0f;
    txt.text            = std::move(text);
    txt.alignment       = static_cast<int32_t>(attachment);

    EntityHeader mtxt_hdr = hdr;
    mtxt_hdr.type = EntityType::MText;
    mtxt_hdr.bounds = entity_bounds_text(txt);
    scene.add_entity(make_entity<7>(mtxt_hdr, std::move(txt)));
}

// ============================================================
// INSERT (DWG type 7) / MINSERT (DWG type 8) -> EntityVariant index 10
// DWG does NOT store block_name in the data stream; it stores a block_header
// handle in the handle stream. For now we create the entity without resolving
// the block index.
// ============================================================
void parse_insert(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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

    InsertEntity ins;
    ins.block_index     = hdr.block_index;  // Set by handle stream parsing in parse_objects
    ins.insertion_point = {static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iz)};
    ins.x_scale         = static_cast<float>(sx);
    ins.y_scale         = static_cast<float>(sy);
    ins.rotation        = static_cast<float>(rotation);
    ins.column_count    = static_cast<int32_t>(num_cols);
    ins.row_count       = static_cast<int32_t>(num_rows);
    ins.column_spacing  = static_cast<float>(col_spacing);
    ins.row_spacing     = static_cast<float>(row_spacing);

    EntityHeader ins_hdr = hdr;
    ins_hdr.type = EntityType::Insert;
    ins_hdr.bounds = entity_bounds_insert(ins);
    scene.add_entity(make_entity<10>(ins_hdr, std::move(ins)));
}

// ============================================================
// POINT (DWG type 27) -> EntityVariant index 11 (Vec3)
// R13+: BD(x), BD(y), BD(z), BT(thickness), BE(extrusion), BD(x_angle).
// ============================================================
void parse_point(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                 DwgVersion /*version*/) {
    double dx = r.read_bd();
    double dy = r.read_bd();
    double dz = r.read_bd();
    r.read_bt();  // thickness
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);
    r.read_bd();  // x_axis angle (display orientation)

    if (!reader_ok(r)) return;

    Vec3 point{static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};

    EntityHeader pt_hdr = hdr;
    pt_hdr.type = EntityType::Point;
    pt_hdr.bounds = Bounds3d::from_point(point);
    scene.add_entity(make_entity<11>(pt_hdr, std::move(point)));
}

// ============================================================
// ELLIPSE (DWG type 36) -> EntityVariant index 12
// NOTE: DWG ELLIPSE stores minor_to_major_ratio, not minor_radius.
// Angles are parametric and already in RADIANS — do NOT convert.
// ============================================================
void parse_ellipse(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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
    double major_radius = r.read_bd();
    double ratio        = r.read_bd();
    double start_angle  = r.read_bd();
    double end_angle    = r.read_bd();

    if (!reader_ok(r)) return;

    // Compute the rotation angle of the major axis in the XY plane
    float rotation = std::atan2(static_cast<float>(smy), static_cast<float>(smx));

    CircleEntity ellipse;
    ellipse.center       = {static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)};
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
// SPLINE (DWG type 37) -> EntityVariant index 5
// ============================================================
void parse_spline(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                  DwgVersion /*version*/) {
    // Per libredwg dwg.spec: BL scenario, BL degree
    uint32_t scenario = r.read_bl();
    uint32_t degree   = r.read_bl();

    SplineEntity spline;
    spline.degree = static_cast<int32_t>(degree);

    if (scenario == 1 || (scenario & 1)) {
        // Scenario 1: control point spline
        (void)r.read_b();   // rational
        (void)r.read_b();   // closed
        (void)r.read_b();   // periodic
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
        for (uint32_t i = 0; i < num_control_pts; ++i) {
            double x = r.read_bd();
            double y = r.read_bd();
            double z = r.read_bd();
            spline.control_points.push_back(
                {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
            if (weighted) (void)r.read_bd();  // weight
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
            spline.fit_points.push_back(
                {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
        }
    }

    if (!reader_ok(r)) return;

    EntityHeader spl_hdr = hdr;
    spl_hdr.type = EntityType::Spline;
    spl_hdr.bounds = entity_bounds_spline(spline);
    scene.add_entity(make_entity<5>(spl_hdr, std::move(spline)));
}

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
// SOLID (DWG type 31) -> EntityVariant index 16
// DWG corner order is 1,2,3,4. DXF convention (which our struct
// SOLID (DWG type 31) / TRACE (DWG type 32) -> EntityVariant index 16
// R13+: BT0(thickness), BD(elevation), 2RD(corner1..4), BE(extrusion).
// DXF corner order is 1,2,4,3 — DWG native order (what the binary format
// follows) is 1,2,4,3 — so we swap corners 3 and 4 during read.
// ============================================================
void parse_solid(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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
// DIMENSION types (DWG types 20-25) -> EntityVariant index 8
// Common layout: extrusion, elevation, anonymous block handle,
// dimension type, user text, rotation, direction, then points.
// ============================================================
void parse_dimension(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                     DwgVersion version) {
    // COMMON_ENTITY_DIMENSION (R2000+ binary, non-DXF):
    // R2010+: RC(class_version)
    // 3BD(extrusion)
    // 2RD(text_midpt) — RAW doubles, not BD!
    // BD(elevation)
    // RC(flag1) — raw char, not BS
    // T(user_text) — R2007+ in string stream
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

    // text_midpt (2RD — raw doubles!)
    double mx = r.read_rd(), my = r.read_rd();

    // elevation
    double elev = r.read_bd();

    // flag1 (RC — raw char)
    uint8_t flag1 = r.read_raw_char();

    // user_text: R2007+ in string stream, read_t() handles dispatch.
    std::string text = r.read_t();

    // text_rotation, horiz_dir (BD0)
    double text_rot = r.read_bd();
    double horiz_dir = r.read_bd();

    // ins_scale (3BD_1) — 3 BD with default 1.0
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

    // Type-specific fields — read definition point and extension points
    // Most dimension types have def_pt (3BD) plus 2-4 more 3BD points
    double dx = 0, dy = 0, dz = 0;
    if (!r.has_error()) {
        // definition point (3BD) — common to most dimension types
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
namespace {
// Tessellate a circular arc into line segments and append to verts.
void tessellate_arc(double cx, double cy, double radius,
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

} // anonymous namespace

void parse_hatch(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                 DwgVersion version) {
    size_t hatch_start = r.bit_offset();
    size_t hatch_limit = r.bit_limit();

    auto dbg_hatch_fail = [&](int stage) {
        static int g_hatch_fail = 0;
        if (g_hatch_fail < 10) {
            fprintf(stderr, "[DWG HATCH] fail stage=%d start=%zu limit=%zu now=%zu\n",
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
            r.read_cmc_r2004(version);         // color (R2004+ CMC)
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
                    // Unknown edge type — stop reading this path's edges
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
// 3DFACE (DWG type 28) -> EntityVariant index 16 (reuses SolidEntity)
// 3DFACE is always a 4-vertex surface with no thickness.
// ============================================================
void parse_3dface(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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

void parse_polyline_pface(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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
        size_t verts_bits = num_verts * 48;  // 3 coords × 16 bits
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
    // needs 1 BL for face_size + 3 BLs for 3 vertex indices = 4 × 34 = 136 bits.
    // Without this guard, the face_size read can consume garbage from adjacent
    // entity data, producing out-of-range indices that produce no output.
    size_t rem = r.remaining_bits();
    constexpr size_t kMinFaceBits = 4 * 34;  // 1 face_size BL + 3 index BLs
    if (num_verts > 0 && (rem < kMinFaceBits || !reader_ok(r))) {
        return;
    }

    // Read face index lists. Each face: BL(size) + size × BL(1-based indices).
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
// Output: LINE entities forming the wireframe grid (M×(N-1) + (M-1)×N lines).
// ============================================================
void parse_polyline_mesh(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene) {
    (void)r.read_bs();  // flags
    uint32_t m_size = r.read_bl();
    uint32_t n_size = r.read_bl();

    if (!reader_ok(r) || m_size == 0 || m_size > 10000 ||
        n_size == 0 || n_size > 10000) {
        return;
    }

    // Read all M×N vertices
    uint32_t total_verts = m_size * n_size;
    std::vector<Vec3> vertices;
    vertices.reserve(total_verts);
    for (uint32_t i = 0; i < total_verts; ++i) {
        double x = r.read_bd(), y = r.read_bd(), z = r.read_bd();
        vertices.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
    }

    if (!reader_ok(r)) return;

    // Emit grid lines: M rows × N columns
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
void parse_seqend(DwgBitReader& /*r*/, const EntityHeader& /*hdr*/, SceneGraph& /*scene*/) {
    // SEQEND has no geometry — it only marks the end of a polyline's vertex list.
}

// ============================================================
// VERTEX_2D (DWG type 10) -> point entity
// R2000+: RC(flag), 3BD(point), BD(start_width), BD(end_width), BD(bulge),
//         [R2010+: BL0(id)], BD(tangent_dir)
// ============================================================
void parse_vertex_2d(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
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
void parse_polyline_2d(DwgBitReader& r, const EntityHeader& /*hdr*/,
                       SceneGraph& /*scene*/, DwgVersion version) {
    (void)r.read_bs();  // flag
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
    // No geometry output — vertices are separate VERTEX_2D entities
}

// ============================================================
// BLOCK (DWG type 4) / ENDBLK (DWG type 5) -> no entity output
// Block begin/end markers in the entity stream.
// ============================================================
void parse_block(DwgBitReader& /*r*/, const EntityHeader& /*hdr*/,
                 SceneGraph& /*scene*/, DwgVersion /*version*/) {
    // BLOCK entities are markers, no geometry.
}
void parse_endblk(DwgBitReader& /*r*/, const EntityHeader& /*hdr*/,
                  SceneGraph& /*scene*/, DwgVersion /*version*/) {
    // ENDBLK entities are markers, no geometry.
}

// ============================================================
// VIEWPORT (DWG type 70) -> EntityVariant index 15 (Vec3 placeholder)
// Stores the viewport center point as a position hint.
// ============================================================
void parse_viewport(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene) {
    double cx = r.read_bd();
    double cy = r.read_bd();
    double cz = r.read_bd();

    if (!reader_ok(r)) return;

    Vec3 center{static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)};

    EntityHeader vp_hdr = hdr;
    vp_hdr.type = EntityType::Viewport;
    vp_hdr.bounds = Bounds3d::from_point(center);
    scene.add_entity(make_entity<15>(vp_hdr, std::move(center)));
}

// ============================================================
// RAY (DWG type 41) -> EntityVariant index 13
// ============================================================
void parse_ray(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
               DwgVersion /*version*/) {
    double sx = r.read_bd(), sy = r.read_bd(), sz = r.read_bd();
    double ux = r.read_bd(), uy = r.read_bd(), uz = r.read_bd();

    if (!reader_ok(r)) return;

    float d = static_cast<float>(kVeryLargeDistance);
    LineEntity ray;
    ray.start = {static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz)};
    ray.end   = {static_cast<float>(sx + ux * d),
                 static_cast<float>(sy + uy * d),
                 static_cast<float>(sz + uz * d)};

    EntityHeader ray_hdr = hdr;
    ray_hdr.type = EntityType::Ray;
    ray_hdr.bounds = entity_bounds_line(ray);
    scene.add_entity(make_entity<13>(ray_hdr, std::move(ray)));
}

// ============================================================
// XLINE (DWG type 42) -> EntityVariant index 14
// ============================================================
void parse_xline(DwgBitReader& r, const EntityHeader& hdr, SceneGraph& scene,
                 DwgVersion /*version*/) {
    double bx = r.read_bd(), by = r.read_bd(), bz = r.read_bd();
    double ux = r.read_bd(), uy = r.read_bd(), uz = r.read_bd();

    if (!reader_ok(r)) return;

    float d = static_cast<float>(kVeryLargeDistance);
    LineEntity xline;
    xline.start = {static_cast<float>(bx - ux * d),
                   static_cast<float>(by - uy * d),
                   static_cast<float>(bz - uz * d)};
    xline.end   = {static_cast<float>(bx + ux * d),
                   static_cast<float>(by + uy * d),
                   static_cast<float>(bz + uz * d)};

    EntityHeader xl_hdr = hdr;
    xl_hdr.type = EntityType::XLine;
    xl_hdr.bounds = entity_bounds_line(xline);
    scene.add_entity(make_entity<14>(xl_hdr, std::move(xline)));
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
static void parse_layer_object(DwgBitReader& r, SceneGraph& scene,
                                DwgVersion version,
                                const uint8_t* obj_data, size_t obj_bytes,
                                size_t main_data_bits, size_t entity_bits) {
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    (void)r.read_bs();  // standard_flags

    // Read layer name (TU in R2007+, TV in older)
    std::string name;
    if (version >= DwgVersion::R2007) {
        name = read_table_text(r);
    } else {
        name = r.read_tv();
    }

    // Read color (CMC) — returns ACI color index
    uint16_t color_index = r.read_cmc_r2004(version);
    Color color = Color::from_aci(static_cast<int>(color_index));

    // Skip remaining handles before the handle stream
    while (!r.has_error() && r.bit_offset() < main_data_bits) {
        auto h = r.read_h();
        if (h.value == 0 && h.code == 0) break;
    }

    // Read linetype handle from handle stream (at end of entity data)
    int32_t linetype_index = 0;
    if (main_data_bits < entity_bits) {
        auto lt_handle = read_one_handle(obj_data, obj_bytes, main_data_bits, entity_bits);
        if (lt_handle.value != 0) {
            linetype_index = static_cast<int32_t>(lt_handle.value);
        }
    }

    if (!reader_ok(r)) return;

    // Add/update layer in SceneGraph
    int32_t layer_idx = scene.find_or_add_layer(name);
    Layer layer;
    layer.name = name;
    layer.color = color;
    if (linetype_index != 0) {
        layer.linetype_index = linetype_index;
    }
    scene.update_layer(layer_idx, layer);
}

// ============================================================
// LTYPE (type 57) table object
// Fields: [class_version RC] + [flags BS] + [name TU] + [description TU]
//         + [pattern_length BD] + [num_dashes BL]
//         + num_dashes × [BD(dash_or_gap)]
// ============================================================
static void parse_ltype_object(DwgBitReader& r, SceneGraph& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits) {
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    (void)r.read_bs();  // flags

    std::string name;
    if (version >= DwgVersion::R2007) {
        name = read_table_text(r);
    } else {
        name = r.read_tv();
    }

    std::string description;
    if (version >= DwgVersion::R2007) {
        description = read_table_text(r);
    } else {
        description = r.read_tv();
    }

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

    if (!reader_ok(r)) return;

    Linetype lt;
    lt.name = name;
    lt.description = description;
    if (!dash_lengths.empty()) {
        lt.pattern.dash_array = std::move(dash_lengths);
    }

    scene.add_linetype(std::move(lt));
}

// ============================================================
// STYLE (type 53) table object
// Fields: [class_version RC] + [flags BL] + [name TU]
//         + [font_name TU] + [bigfont_name TU]
//         + [height BD] + [width BD] + [oblique BD] + [flags2 BL]
// ============================================================
static void parse_style_object(DwgBitReader& r, SceneGraph& scene,
                                DwgVersion version,
                                const uint8_t* /*obj_data*/, size_t /*obj_bytes*/,
                                size_t main_data_bits, size_t entity_bits) {
    if (version >= DwgVersion::R2010) {
        (void)r.read_raw_char();  // class_version (RC)
    }

    (void)r.read_bl();  // flags

    std::string name;
    if (version >= DwgVersion::R2007) {
        name = read_table_text(r);
    } else {
        name = r.read_tv();
    }

    std::string font_name;
    if (version >= DwgVersion::R2007) {
        font_name = read_table_text(r);
    } else {
        font_name = r.read_tv();
    }

    std::string bigfont_name;
    if (version >= DwgVersion::R2007) {
        bigfont_name = read_table_text(r);
    } else {
        bigfont_name = r.read_tv();
    }

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
static void parse_dimstyle_object(DwgBitReader& r, SceneGraph& scene,
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

// ============================================================
// Public entry point for table objects
// ============================================================
void parse_dwg_table_object(DwgBitReader& reader, uint32_t obj_type,
                              SceneGraph& scene, DwgVersion version,
                              size_t entity_bits, size_t main_data_bits) {
    const uint8_t* obj_data = reader.data();
    size_t obj_bytes = reader.data_size();

    switch (obj_type) {
        case 51:  // LAYER
            parse_layer_object(reader, scene, version, obj_data, obj_bytes,
                               main_data_bits, entity_bits);
            break;
        case 57:  // LTYPE
            parse_ltype_object(reader, scene, version, obj_data, obj_bytes,
                               main_data_bits, entity_bits);
            break;
        case 53:  // STYLE
            parse_style_object(reader, scene, version, obj_data, obj_bytes,
                               main_data_bits, entity_bits);
            break;
        case 69:  // DIMSTYLE
            parse_dimstyle_object(reader, scene, version, obj_data, obj_bytes,
                                  main_data_bits, entity_bits);
            break;
        default:
            break;
    }
}

// ============================================================
// Public entry point — dispatches to type-specific parsers
// ============================================================
void parse_dwg_entity(DwgBitReader& reader, uint32_t obj_type,
                       const EntityHeader& header, SceneGraph& scene,
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
        case 34:  parse_viewport(reader, header, scene);               break;  // VIEWPORT
        case 35:  parse_ellipse(reader, header, scene, version);      break;  // ELLIPSE
        case 36:  parse_spline(reader, header, scene, version);       break;  // SPLINE
        case 40:  parse_ray(reader, header, scene, version);          break;  // RAY
        case 41:  parse_xline(reader, header, scene, version);        break;  // XLINE
        case 44:  parse_mtext(reader, header, scene, version);        break;  // MTEXT
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

std::unordered_map<uint32_t, size_t> get_dwg_entity_success_counts() {
    return g_success_counts;
}

std::unordered_map<uint32_t, size_t> get_dwg_entity_dispatch_counts() {
    return g_dispatch_counts;
}

} // namespace cad
