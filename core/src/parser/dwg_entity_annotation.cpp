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

void parse_tolerance(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version) {
    (void)version;

    // TOLERANCE (DWG type 46): geometric dimensioning and tolerancing frame.
    // Fields per spec:
    //   RC(class_version)
    //   3BD(insertion_point)
    //   3BD(direction) or BE(extrusion) depending on version
    //   T(text_content) — the GD&T symbols and values

    int class_ver = r.read_raw_char();
    (void)class_ver;

    double ins_x = r.read_bd();
    double ins_y = r.read_bd();
    double ins_z = r.read_bd();
    if (!reader_ok(r)) return;

    // direction / extrusion — skip
    (void)r.read_bd();
    (void)r.read_bd();
    (void)r.read_bd();

    // Text content — the actual GD&T value string
    std::string text = r.read_t();
    if (!reader_ok(r)) return;

    if (text.empty()) {
        text = "TOL";
    }

    float fx = safe_float(ins_x);
    float fy = safe_float(ins_y);
    if (!std::isfinite(fx) || !std::isfinite(fy)) return;

    TextEntity tol;
    tol.insertion_point = {fx, fy, 0.0f};
    tol.height = 2.5f;
    tol.text = std::move(text);

    EntityHeader tol_hdr = hdr;
    tol_hdr.type = EntityType::Tolerance;
    tol_hdr.bounds = Bounds3d{{fx - 5, fy - 2, 0}, {fx + 20, fy + 5, 0}};

    scene.add_entity(make_entity<18>(tol_hdr, std::move(tol)));
}

// ============================================================
// MULTILEADER (class "MULTILEADER") -> EntityVariant index 21
// Complex class-based entity with multiple leader lines and
// optional text/block content.  Minimum fidelity: extract
// leader line geometry and text.
// ============================================================
void parse_multileader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                       DwgVersion version) {
    uint32_t class_version = r.read_bl();
    (void)class_version;

    // MULTILEADER main fields (order varies by version):
    //   BL(class_version), BL(unknown), 3BD(insertion_point),
    //   3BD(plane_normal) or BE(extrusion), BL(num_leader_lines),
    //   per line: BL(num_points) + num_points × 3BD, ...
    //   BL(leader_type), BD(landing_distance), B(has_landing), B(has_dogleg),
    //   3BD(arrowhead_direction), BL(content_type),
    //   if text: T(text), BD(text_height), ...
    //
    // The actual field order is complex and version-dependent.
    // We use a robust approach: read insertion_point, then scan for
    // point arrays and text.

    double ins_x = r.read_bd();
    double ins_y = r.read_bd();
    double ins_z = r.read_bd();

    if (!reader_ok(r)) return;

    // Skip remaining fields until we can extract leader line data.
    // The MULTILEADER binary layout is deeply nested; for minimum fidelity
    // we create text + leader line geometry from what we can parse.

    MultileaderEntity ml;
    ml.insertion_point = {safe_float(ins_x), safe_float(ins_y), safe_float(ins_z)};

    // Try to read plausible fields after insertion point.
    // The exact layout is: extrusion(3BD) or BE, then BL(num_leader_lines)
    // But the version differences make exact parsing fragile.
    // Instead, scan for text content using the string stream.

    std::string text_content;
    if (version >= DwgVersion::R2007) {
        // R2007+: text may be in the string stream
        text_content = r.read_t();
    }

    // Emit the MULTILEADER as a text entity + leader line geometry
    // For now, just store the insertion point and any text we found.
    if (!text_content.empty()) {
        ml.text = std::move(text_content);
        ml.text_height = 3.5f;  // default
    }

    EntityHeader ml_hdr = hdr;
    ml_hdr.type = EntityType::Multileader;
    ml_hdr.bounds = Bounds3d::from_point(ml.insertion_point);

    scene.add_entity(make_entity<20>(ml_hdr, std::move(ml)));
}

} // namespace cad
