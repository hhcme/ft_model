#include "cad/parser/dwg_parse_helpers.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/cad_types.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace cad {

bool is_graphic_entity(uint32_t obj_type)
{
    switch (obj_type) {
        case 1:   // TEXT
        case 2:   // ATTRIB
        case 3:   // ATTDEF
        case 4:   // BLOCK
        case 5:   // ENDBLK
        case 6:   // SEQEND
        case 7:   // INSERT
        case 8:   // MINSERT
        case 10:  // VERTEX_2D
        case 11:  // VERTEX_3D
        case 12:  // VERTEX_MESH
        case 13:  // VERTEX_PFACE
        case 14:  // VERTEX_PFACE_FACE
        case 15:  // POLYLINE_2D
        case 16:  // POLYLINE_3D
        case 17:  // ARC
        case 18:  // CIRCLE
        case 19:  // LINE
        case 20:  // DIMENSION_ORDINATE
        case 21:  // DIMENSION_LINEAR
        case 22:  // DIMENSION_ALIGNED
        case 23:  // DIMENSION_ANG3PT
        case 24:  // DIMENSION_ANG2LN
        case 25:  // DIMENSION_RADIUS
        case 26:  // DIMENSION_DIAMETER
        case 27:  // POINT
        case 28:  // 3DFACE
        case 29:  // POLYLINE_PFACE
        case 30:  // POLYLINE_MESH
        case 31:  // SOLID
        case 32:  // TRACE
        case 34:  // VIEWPORT
        case 35:  // ELLIPSE
        case 36:  // SPLINE
        case 40:  // RAY
        case 41:  // XLINE
        case 44:  // MTEXT
        case 77:  // LWPOLYLINE
        case 78:  // HATCH
            return true;

        // Explicitly non-graphic
        case 42:  // DICTIONARY
        case 43:  // DICTIONARYWDFLT
        case 48:  // BLOCK_CONTROL
        case 49:  // BLOCK_HEADER
        case 50:  // LAYER_CONTROL
        case 51:  // LAYER
        case 52:  // STYLE_CONTROL
        case 53:  // STYLE (TEXTSTYLE)
        case 54:  // LTYPE_CONTROL
        case 55:  // LTYPE (some DWGs)
        case 56:  // VIEW
        case 57:  // UCS
        case 58:  // VPORT
        case 59:  // APPID
        case 60:  // DIMSTYLE
        case 61:  // VP_ENT_HDR
        case 62:  // GROUP
        case 64:  // MLINESTYLE
        case 70:  // XRECORD
        case 74:  // PROXY_OBJECT
        case 79:  // XRECORD/roundtrip dictionary record
        case 82:  // LAYOUT
            return false;

        default:
            return false;
    }
}

bool valid_layout_size(double w, double h)
{
    return std::isfinite(w) && std::isfinite(h) && w > 1.0 && h > 1.0 && w < 1.0e7 && h < 1.0e7;
}

void read_2rd(DwgBitReader& reader, double& x, double& y)
{
    x = reader.read_double();
    y = reader.read_double();
}

int32_t parse_layout_object(DwgBitReader& reader, EntitySink& scene, DwgVersion version)
{
    DwgBitReader r = reader;

    (void)r.read_t();       // plotsettings.printer_cfg_file
    (void)r.read_t();       // plotsettings.paper_size
    (void)r.read_bs();      // plotsettings.plot_flags
    const double left_margin = r.read_bd();
    const double bottom_margin = r.read_bd();
    const double right_margin = r.read_bd();
    const double top_margin = r.read_bd();
    const double paper_width = r.read_bd();
    const double paper_height = r.read_bd();
    (void)r.read_t();       // plotsettings.canonical_media_name
    double plot_origin_x = 0.0;
    double plot_origin_y = 0.0;
    r.read_2d_point(plot_origin_x, plot_origin_y);
    const uint16_t plot_paper_unit = r.read_bs();
    const uint16_t plot_rotation_mode = r.read_bs();
    (void)r.read_bs();      // plotsettings.plot_type
    double win_ll_x = 0.0;
    double win_ll_y = 0.0;
    double win_ur_x = 0.0;
    double win_ur_y = 0.0;
    r.read_2d_point(win_ll_x, win_ll_y);
    r.read_2d_point(win_ur_x, win_ur_y);
    const double paper_units = r.read_bd();
    const double drawing_units = r.read_bd();
    (void)r.read_t();       // plotsettings.stylesheet
    (void)r.read_bs();      // plotsettings.std_scale_type
    const double std_scale_factor = r.read_bd();
    double image_origin_x = 0.0;
    double image_origin_y = 0.0;
    r.read_2d_point(image_origin_x, image_origin_y);
    if (version >= DwgVersion::R2004) {
        (void)r.read_bs();  // plotsettings.shadeplot_type
        (void)r.read_bs();  // plotsettings.shadeplot_reslevel
        (void)r.read_bs();  // plotsettings.shadeplot_customdpi
    }

    Layout layout;
    layout.name = r.read_t();
    (void)r.read_bs();      // tab_order
    const uint16_t layout_flags = r.read_bs();
    double insbase_x = 0.0;
    double insbase_y = 0.0;
    double insbase_z = 0.0;
    r.read_3d_point(insbase_x, insbase_y, insbase_z);  // INSBASE
    double lim_min_x = 0.0;
    double lim_min_y = 0.0;
    double lim_max_x = 0.0;
    double lim_max_y = 0.0;
    read_2rd(r, lim_min_x, lim_min_y);
    read_2rd(r, lim_max_x, lim_max_y);
    double ignored_z = 0.0;
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // UCSORG
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // UCSXDIR
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // UCSYDIR
    (void)r.read_bd();      // ucs_elevation
    (void)r.read_bs();      // UCSORTHOVIEW
    double ext_min_x = 0.0;
    double ext_min_y = 0.0;
    double ext_min_z = 0.0;
    double ext_max_x = 0.0;
    double ext_max_y = 0.0;
    double ext_max_z = 0.0;
    r.read_3d_point(ext_min_x, ext_min_y, ext_min_z);  // EXTMIN
    r.read_3d_point(ext_max_x, ext_max_y, ext_max_z);  // EXTMAX
    if (version >= DwgVersion::R2004) {
        (void)r.read_bl();  // num_viewports
    }

    if (r.has_error()) {
        return -1;
    }

    if (layout.name.empty()) {
        layout.name = "Layout";
    }
    layout.plot_origin = Vec3{static_cast<float>(plot_origin_x),
                              static_cast<float>(plot_origin_y), 0.0f};
    layout.plot_rotation = static_cast<int32_t>(plot_rotation_mode);
    layout.paper_units = static_cast<int32_t>(plot_paper_unit);
    if (std::isfinite(std_scale_factor) && std_scale_factor > 0.0) {
        layout.plot_scale = static_cast<float>(std_scale_factor);
    } else if (std::isfinite(paper_units) && std::isfinite(drawing_units) &&
               paper_units > 0.0 && drawing_units > 0.0) {
        layout.plot_scale = static_cast<float>(drawing_units / paper_units);
    }
    layout.is_active = (layout_flags & 0x01u) != 0;
    layout.is_current = (layout_flags & 0x02u) != 0;
    if (std::isfinite(insbase_x) && std::isfinite(insbase_y) && std::isfinite(insbase_z)) {
        layout.insertion_base = Vec3{static_cast<float>(insbase_x),
                                     static_cast<float>(insbase_y),
                                     static_cast<float>(insbase_z)};
    }
    std::string upper = layout.name;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    layout.is_model_layout = (upper == "MODEL");

    if (valid_layout_size(paper_width, paper_height)) {
        layout.paper_bounds.expand(Vec3{0.0f, 0.0f, 0.0f});
        layout.paper_bounds.expand(Vec3{static_cast<float>(paper_width),
                                        static_cast<float>(paper_height), 0.0f});

        const float printable_min_x = static_cast<float>(std::max(0.0, left_margin));
        const float printable_min_y = static_cast<float>(std::max(0.0, bottom_margin));
        const float printable_max_x = static_cast<float>(std::max(left_margin, paper_width - right_margin));
        const float printable_max_y = static_cast<float>(std::max(bottom_margin, paper_height - top_margin));
        if (printable_max_x > printable_min_x && printable_max_y > printable_min_y) {
            layout.border_bounds.expand(Vec3{printable_min_x, printable_min_y, 0.0f});
            layout.border_bounds.expand(Vec3{printable_max_x, printable_max_y, 0.0f});
        }
    }

    if (std::isfinite(win_ll_x) && std::isfinite(win_ll_y) &&
        std::isfinite(win_ur_x) && std::isfinite(win_ur_y) &&
        win_ur_x > win_ll_x && win_ur_y > win_ll_y) {
        layout.plot_window.expand(Vec3{static_cast<float>(win_ll_x), static_cast<float>(win_ll_y), 0.0f});
        layout.plot_window.expand(Vec3{static_cast<float>(win_ur_x), static_cast<float>(win_ur_y), 0.0f});
    }
    if (std::isfinite(lim_min_x) && std::isfinite(lim_min_y) &&
        std::isfinite(lim_max_x) && std::isfinite(lim_max_y) &&
        lim_max_x > lim_min_x && lim_max_y > lim_min_y) {
        layout.limits.expand(Vec3{static_cast<float>(lim_min_x), static_cast<float>(lim_min_y), 0.0f});
        layout.limits.expand(Vec3{static_cast<float>(lim_max_x), static_cast<float>(lim_max_y), 0.0f});
    }
    if (std::isfinite(ext_min_x) && std::isfinite(ext_min_y) && std::isfinite(ext_min_z) &&
        std::isfinite(ext_max_x) && std::isfinite(ext_max_y) && std::isfinite(ext_max_z) &&
        ext_max_x > ext_min_x && ext_max_y > ext_min_y) {
        layout.extents.expand(Vec3{static_cast<float>(ext_min_x),
                                   static_cast<float>(ext_min_y),
                                   static_cast<float>(ext_min_z)});
        layout.extents.expand(Vec3{static_cast<float>(ext_max_x),
                                   static_cast<float>(ext_max_y),
                                   static_cast<float>(ext_max_z)});
    }

    dwg_debug_log("[DWG] layout parsed name='%s' model=%u paper=(%.3f,%.3f) margins=(%.3f,%.3f,%.3f,%.3f) window=(%.3f,%.3f)-(%.3f,%.3f) limits=(%.3f,%.3f)-(%.3f,%.3f) ext=(%.3f,%.3f)-(%.3f,%.3f) insbase=(%.3f,%.3f,%.3f)\n",
                  layout.name.c_str(),
                  layout.is_model_layout ? 1u : 0u,
                  paper_width,
                  paper_height,
                  left_margin,
                  bottom_margin,
                  right_margin,
                  top_margin,
                  win_ll_x,
                  win_ll_y,
                  win_ur_x,
                  win_ur_y,
                  lim_min_x,
                  lim_min_y,
                  lim_max_x,
                  lim_max_y,
                  ext_min_x,
                  ext_min_y,
                  ext_max_x,
                  ext_max_y,
                  insbase_x,
                  insbase_y,
                  insbase_z);

    return scene.add_layout(std::move(layout));
}

uint64_t resolve_handle_ref(uint64_t source_handle, const DwgBitReader::HandleRef& ref)
{
    switch (ref.code) {
        case 2: case 3: case 4: case 5:
            return ref.value;  // TYPEDOBJHANDLE: absolute
        case 6:
            return source_handle + 1;
        case 8:
            return (source_handle > 1) ? source_handle - 1 : 0;
        case 0xA:
            return source_handle + ref.value;
        case 0xC:
            return (source_handle > ref.value) ? source_handle - ref.value : 0;
        default:
            return ref.value;  // code 0/1: soft pointer, treat as absolute
    }
}

std::string uppercase_ascii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains_ascii_ci(const std::string& text, const char* needle)
{
    return uppercase_ascii(text).find(uppercase_ascii(needle ? needle : "")) != std::string::npos;
}

} // namespace cad
