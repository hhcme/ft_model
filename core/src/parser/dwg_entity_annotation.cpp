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

    double align_x = 0.0, align_y = 0.0;
    bool has_align_pt = !(dataflags & 0x02);
    if (has_align_pt) {
        align_x = r.read_dd(ix);
        align_y = r.read_dd(iy);
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

    if (!reader_ok(r)) return;

    if (!is_safe_coord(ix) || !is_safe_coord(iy) ||
        !std::isfinite(height) || height <= 0.0) return;

    TextEntity txt;
    const bool has_ocs = !is_default_extrusion(nx, ny, nz);
    OcsBasis basis;
    if (has_ocs) {
        basis = make_ocs_basis(nx, ny, nz);
        txt.insertion_point = ocs_point_to_wcs(ix, iy, iz, basis);
    } else {
        txt.insertion_point = {safe_float(ix), safe_float(iy), safe_float(iz)};
    }
    if (has_align_pt) {
        if (has_ocs) {
            txt.alignment_point = ocs_point_to_wcs(align_x, align_y, 0.0, basis);
        } else {
            txt.alignment_point = {safe_float(align_x), safe_float(align_y), 0.0f};
        }
    }
    txt.height          = safe_float(height);
    txt.rotation        = safe_float(rotation);
    txt.width_factor    = safe_float(width_factor);
    txt.oblique_angle   = safe_float(oblique_angle);
    txt.generation      = static_cast<int32_t>(generation);
    txt.v_align         = static_cast<int32_t>(v_align);
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
    double ix = r.read_bd(), iy = r.read_bd(), iz = r.read_bd();
    double nx = r.read_bd(), ny = r.read_bd(), nz = r.read_bd();
    double xax = r.read_bd(), yay = r.read_bd(), zaz = r.read_bd();
    (void)zaz;
    double rect_width = r.read_bd();
    double rect_height = 0.0;
    if (version >= DwgVersion::R2007) rect_height = r.read_bd();
    double height = r.read_bd();
    uint16_t attachment = r.read_bs(), flow_dir = r.read_bs();
    r.read_bd(); r.read_bd();  // extents
    std::string text = r.read_t();
    uint16_t ls_style = 0; double ls_factor = 1.0;
    if (version >= DwgVersion::R2000) { ls_style = r.read_bs(); ls_factor = r.read_bd(); r.read_b(); }
    if (!reader_ok(r)) return;
    if (!is_safe_coord(ix) || !is_safe_coord(iy) || !std::isfinite(height) || height <= 0.0) return;
    float rotation = std::atan2(static_cast<float>(yay), static_cast<float>(xax));
    TextEntity txt;
    const bool has_ocs = !is_default_extrusion(nx, ny, nz);
    if (has_ocs) {
        OcsBasis basis = make_ocs_basis(nx, ny, nz);
        txt.insertion_point = ocs_point_to_wcs(ix, iy, iz, basis);
        Vec3 xd = ocs_vector_to_wcs(xax, yay, zaz, basis);
        rotation = std::atan2(xd.y, xd.x);
    } else { txt.insertion_point = {safe_float(ix), safe_float(iy), safe_float(iz)}; }
    txt.height = safe_float(height); txt.rotation = rotation; txt.width_factor = 1.0f;
    txt.flow_dir = static_cast<int32_t>(flow_dir);
    txt.linespace_style = static_cast<int32_t>(ls_style);
    txt.linespace_factor = safe_float(ls_factor);
    txt.rect_width = (std::isfinite(rect_width) && rect_width > 0.0) ? safe_float(rect_width) : 0.0f;
    txt.rect_height = (std::isfinite(rect_height) && rect_height > 0.0) ? safe_float(rect_height) : 0.0f;
    txt.text = std::move(text); txt.alignment = static_cast<int32_t>(attachment);
    EntityHeader mtxt_hdr = hdr; mtxt_hdr.type = EntityType::MText;
    mtxt_hdr.bounds = entity_bounds_text(txt);
    scene.add_entity(make_entity<7>(mtxt_hdr, std::move(txt)));
}

// ============================================================
// DIMENSION types (DWG types 20-25) -> EntityVariant index 8
// ============================================================

struct DimTypeSpecific {
    Vec3 def_pt = {0, 0, 0};
    Vec3 xline1_pt = {0, 0, 0};
    Vec3 xline2_pt = {0, 0, 0};
    Vec3 extra_pt = {0, 0, 0};
    Vec3 extra_pt2 = {0, 0, 0};
};

static bool read_dim_type_specific(DwgBitReader& r, uint32_t obj_type, DimTypeSpecific& out) {
    switch (obj_type) {
    case 20: // DIMENSION_ORDINATE
        out.def_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline1_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline2_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        (void)r.read_raw_char();
        break;
    case 21: // DIMENSION_LINEAR
        out.xline1_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline2_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.def_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        (void)r.read_bd(); (void)r.read_bd();
        break;
    case 22: // DIMENSION_ALIGNED
        out.xline1_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline2_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.def_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        (void)r.read_bd();
        break;
    case 23: // DIMENSION_ANG3PT
        out.def_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline1_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline2_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.extra_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        break;
    case 24: // DIMENSION_ANG2LN
        { double dpx = r.read_rd(), dpy = r.read_rd(); out.def_pt = {safe_float(dpx), safe_float(dpy), 0.0f}; }
        out.xline1_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.xline2_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.extra_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.extra_pt2 = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        break;
    case 25: // DIMENSION_RADIUS
        out.def_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.extra_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        (void)r.read_bd();
        break;
    case 26: // DIMENSION_DIAMETER
        out.extra_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        out.def_pt = {safe_float(r.read_bd()), safe_float(r.read_bd()), safe_float(r.read_bd())};
        (void)r.read_bd();
        break;
    default: return false;
    }
    return reader_ok(r);
}

void parse_dimension(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version, uint32_t obj_type) {
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
    uint16_t attachment = 0;
    uint16_t lspace_style = 0;
    double lspace_factor = 1.0;
    double act_measurement = 0.0;
    if (version >= DwgVersion::R2000) {
        attachment = r.read_bs();
        lspace_style = r.read_bs();
        lspace_factor = r.read_bd();
        act_measurement = r.read_bd();
    }

    // R2007+: unknown, flip_arrow1, flip_arrow2
    bool flip_arrow1 = false, flip_arrow2 = false;
    if (version >= DwgVersion::R2007) {
        (void)r.read_b();  // unknown
        flip_arrow1 = r.read_b();
        flip_arrow2 = r.read_b();
    }

    // clone_ins_pt (2RD0)
    (void)r.read_rd(); (void)r.read_rd();

    if (!reader_ok(r)) return;

    DimTypeSpecific spec;
    if (!read_dim_type_specific(r, obj_type, spec)) return;
    if (!reader_ok(r)) return;

    const bool has_custom_ocs = !is_default_extrusion(ex, ey, ez);
    OcsBasis basis;
    if (has_custom_ocs) basis = make_ocs_basis(ex, ey, ez);
    auto ocs_pt = [&](double x, double y, double z) -> Vec3 {
        if (has_custom_ocs) return ocs_point_to_wcs(x, y, z, basis);
        return {safe_float(x), safe_float(y), safe_float(z)};
    };

    DimensionEntity dim;
    dim.definition_point = ocs_pt(spec.def_pt.x, spec.def_pt.y, spec.def_pt.z);
    dim.text_midpoint    = ocs_pt(mx, my, 0.0);
    dim.ext1_start       = ocs_pt(spec.xline1_pt.x, spec.xline1_pt.y, spec.xline1_pt.z);
    dim.ext2_start       = ocs_pt(spec.xline2_pt.x, spec.xline2_pt.y, spec.xline2_pt.z);
    dim.extra_pt         = ocs_pt(spec.extra_pt.x, spec.extra_pt.y, spec.extra_pt.z);
    dim.extra_pt2        = ocs_pt(spec.extra_pt2.x, spec.extra_pt2.y, spec.extra_pt2.z);
    dim.text             = std::move(text);
    dim.dimension_type   = static_cast<int32_t>(flag1);
    dim.rotation         = safe_float(text_rot);
    dim.measured_value   = static_cast<float>(act_measurement);
    dim.horiz_dir        = safe_float(horiz_dir);
    dim.attachment       = static_cast<int32_t>(attachment);
    dim.lspace_style     = static_cast<int32_t>(lspace_style);
    dim.lspace_factor    = safe_float(lspace_factor);
    dim.flip_arrow1      = flip_arrow1;
    dim.flip_arrow2      = flip_arrow2;
    dim.ins_scale        = {safe_float(isx), safe_float(isy), safe_float(isz)};
    dim.ins_rotation     = safe_float(ins_rot);

    if (dim.text.empty() && std::isfinite(act_measurement) && act_measurement != 0.0) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2f", act_measurement); dim.text = buf;
    }

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

    // Read remaining binary fields after points
    r.read_bd(); r.read_bd(); r.read_bd();  // arrowhead_block_insertion
    uint32_t num_arrow_verts = r.read_bl();
    for (uint32_t i = 0; i < num_arrow_verts && i < 100 && !r.has_error(); ++i)
        r.read_bd(), r.read_bd(), r.read_bd();
    uint32_t num_clip_pts = r.read_bl();
    for (uint32_t i = 0; i < num_clip_pts && i < 100 && !r.has_error(); ++i)
        r.read_bd(), r.read_bd(), r.read_bd();
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);  // plane_normal / extrusion
    double hdx = r.read_bd(), hdy = r.read_bd(), hdz = r.read_bd();  // horizontal_direction
    (void)r.read_b();   // has_hookline
    (void)r.read_bl();  // leader_creation_type

    // LEADER points are stored in WCS per DWG spec. The extrusion/normal
    // defines the plane for OCS entities (TEXT, MTEXT, etc.) but for LEADER
    // the path is already in world coordinates. Do NOT apply OCS transform.

    LeaderEntity leader;
    leader.is_spline = (path_type == 1);
    leader.has_arrowhead = has_arrowhead;
    leader.horizontal_direction = {safe_float(hdx), safe_float(hdy), safe_float(hdz)};

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
// Binary layout (AcDbMLeader): BL(class_ver), BL(attach_type),
// 3BD(ins_pt), BE(extrusion), BL(num_leaders),
// per line: BL(num_pts), num_pts×3BD, BL(break_idx),
//   3BD(break_start/end), BL(line_idx), B(has_last)[3BD(last)],
// BL(content_type) 0=None,1=Block,2=MText,
// if MText: T(text), 3BD(text_loc/dir), T(style), BD(height), ...
// ============================================================
void parse_multileader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                       DwgVersion version) {
    uint32_t class_version = r.read_bl();
    (void)r.read_bl();  // text_attachment_type

    double ins_x = r.read_bd(), ins_y = r.read_bd(), ins_z = r.read_bd();
    double nx = 0, ny = 0, nz = 0;
    r.read_be(nx, ny, nz);

    if (!reader_ok(r)) return;

    MultileaderEntity ml;
    const bool has_custom_ocs = !is_default_extrusion(nx, ny, nz);
    OcsBasis basis;
    if (has_custom_ocs) {
        basis = make_ocs_basis(nx, ny, nz);
        ml.insertion_point = ocs_point_to_wcs(ins_x, ins_y, ins_z, basis);
    } else {
        ml.insertion_point = {safe_float(ins_x), safe_float(ins_y), safe_float(ins_z)};
    }

    uint32_t num_leaders = r.read_bl();
    if (num_leaders > 100) num_leaders = 0;

    std::vector<Vec3> all_vertices;
    std::vector<int32_t> line_vertex_counts;

    for (uint32_t li = 0; li < num_leaders && !r.has_error(); ++li) {
        uint32_t num_pts = r.read_bl();
        if (num_pts == 0 || num_pts > 1000) {
            for (uint32_t pi = 0; pi < num_pts && pi < 1000 && !r.has_error(); ++pi)
                r.read_bd(), r.read_bd(), r.read_bd();
            if (!r.has_error()) { r.read_bl(); r.read_bd(); r.read_bd(); r.read_bd();
                r.read_bd(); r.read_bd(); r.read_bd(); r.read_bl(); }
            if (!r.has_error()) { bool hv = r.read_b(); if(hv) r.read_bd(), r.read_bd(), r.read_bd(); }
            continue;
        }
        std::vector<Vec3> line_pts;
        for (uint32_t pi = 0; pi < num_pts && !r.has_error(); ++pi) {
            double px = r.read_bd(), py = r.read_bd(), pz = r.read_bd();
            Vec3 pt = has_custom_ocs
                ? ocs_point_to_wcs(px, py, pz, basis)
                : Vec3{safe_float(px), safe_float(py), safe_float(pz)};
            if (std::isfinite(pt.x) && std::isfinite(pt.y) && is_safe_coord(pt.x) && is_safe_coord(pt.y))
                line_pts.push_back(pt);
        }
        if (!r.has_error()) { r.read_bl(); r.read_bd(); r.read_bd(); r.read_bd();
            r.read_bd(); r.read_bd(); r.read_bd(); r.read_bl(); }
        if (!r.has_error()) {
            bool has_last = r.read_b();
            if (has_last) {
                double lvx = r.read_bd(), lvy = r.read_bd(), lvz = r.read_bd();
                Vec3 lv = has_custom_ocs
                    ? ocs_point_to_wcs(lvx, lvy, lvz, basis)
                    : Vec3{safe_float(lvx), safe_float(lvy), safe_float(lvz)};
                if (std::isfinite(lv.x) && std::isfinite(lv.y)) line_pts.push_back(lv);
            }
        }
        if (!line_pts.empty()) {
            line_vertex_counts.push_back(static_cast<int32_t>(line_pts.size()));
            all_vertices.insert(all_vertices.end(), line_pts.begin(), line_pts.end());
        }
    }

    ml.leader_line_count = static_cast<int32_t>(line_vertex_counts.size());
    ml.leader_line_vertex_counts = std::move(line_vertex_counts);
    if (!all_vertices.empty()) {
        ml.vertex_offset = scene.add_polyline_vertices(all_vertices.data(), all_vertices.size());
        ml.vertex_count = static_cast<int32_t>(all_vertices.size());
    }

    if (!reader_ok(r)) {
        EntityHeader ml_hdr = hdr;
        ml_hdr.type = EntityType::Multileader;
        Bounds3d b = Bounds3d::from_point(ml.insertion_point);
        for (const auto& v : all_vertices) b.expand(v);
        ml_hdr.bounds = b;
        scene.add_entity(make_entity<20>(ml_hdr, std::move(ml)));
        return;
    }

    uint32_t content_type = r.read_bl();
    if (content_type == 2 && !r.has_error()) {
        ml.text = r.read_t();
        double tlx = r.read_bd(), tly = r.read_bd(), tlz = r.read_bd();
        double tdx = r.read_bd(), tdy = r.read_bd(), tdz = r.read_bd();
        (void)r.read_t();  // text_style name (handle resolved in post-processing)
        double th = r.read_bd();
        if (has_custom_ocs) {
            ml.text_location = ocs_point_to_wcs(tlx, tly, tlz, basis);
            ml.text_direction = ocs_vector_to_wcs(tdx, tdy, tdz, basis);
        } else {
            ml.text_location = {safe_float(tlx), safe_float(tly), safe_float(tlz)};
            ml.text_direction = {safe_float(tdx), safe_float(tdy), safe_float(tdz)};
        }
        if (std::isfinite(th) && th > 0.0 && th < 1.0e6) ml.text_height = safe_float(th);
        if (!ml.text.empty() && ml.text_height <= 0.0f) ml.text_height = 3.5f;
    } else if (content_type == 1 && !r.has_error()) {
        r.read_h();
        r.read_bd(); r.read_bd(); r.read_bd();
        r.read_bd(); r.read_bd(); r.read_bd();
    }

    EntityHeader ml_hdr = hdr;
    ml_hdr.type = EntityType::Multileader;
    Bounds3d b = Bounds3d::from_point(ml.insertion_point);
    for (const auto& v : all_vertices) b.expand(v);
    ml_hdr.bounds = b;
    scene.add_entity(make_entity<20>(ml_hdr, std::move(ml)));
}

} // namespace cad
