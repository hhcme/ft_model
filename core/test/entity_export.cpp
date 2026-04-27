// entity_export.cpp — Export raw entity properties from SceneGraph (no tessellation)
// Usage: entity_export <input.dxf|input.dwg> <output.json>
// Output JSON mirrors ezdxf entity extraction format for direct structural comparison.

#include "cad/cad_types.h"
#include "cad/parser/dxf_parser.h"
#include "cad/parser/dwg_parser.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>

using namespace cad;

static std::string fmt(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

struct JsonWriter {
    std::string buf;
    bool first = true;

    void obj_open() { buf += '{'; first = true; }
    void obj_close() { buf += '}'; first = false; }
    void arr_open() { buf += '['; first = true; }
    void arr_close() { buf += ']'; first = false; }
    void comma() { if (!first) buf += ','; first = false; }

    void key(const char* k) { comma(); buf += '"'; buf += k; buf += "\":"; }
    void key_val(const char* k, const char* v) { key(k); buf += '"'; buf += json_escape(v); buf += '"'; }
    void key_val(const char* k, const std::string& v) { key_val(k, v.c_str()); }
    void key_val(const char* k, double v) { key(k); buf += fmt(v); }
    void key_val(const char* k, int v) { key(k); buf += std::to_string(v); }
    void key_val(const char* k, int64_t v) { key(k); buf += std::to_string(v); }
    void key_val(const char* k, bool v) { key(k); buf += v ? "true" : "false"; }
    void key_val(const char* k, uint64_t v) {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "\"0x%llX\"", (unsigned long long)v);
        key(k); buf += hex;
    }

    void key_vec3(const char* k, const Vec3& v) {
        key(k);
        char tmp[96];
        std::snprintf(tmp, sizeof(tmp), "[%s,%s,%s]", fmt(v.x).c_str(), fmt(v.y).c_str(), fmt(v.z).c_str());
        buf += tmp;
    }

    void key_color(const char* k, const Color& c) {
        key(k);
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "[%d,%d,%d]", c.r, c.g, c.b);
        buf += tmp;
    }
};

static Color resolve_color(const EntityHeader& hdr, const SceneGraph& scene) {
    if (hdr.has_true_color) return hdr.true_color;
    if (hdr.color_override != 256 && hdr.color_override != 0) {
        return Color::from_aci(hdr.color_override);
    }
    if (hdr.layer_index >= 0 && hdr.layer_index < (int32_t)scene.layers().size()) {
        return scene.layers()[hdr.layer_index].color;
    }
    return Color::from_aci(7);
}

static const char* entity_type_name(EntityType t) {
    switch (t) {
        case EntityType::Line:      return "LINE";
        case EntityType::Circle:    return "CIRCLE";
        case EntityType::Arc:       return "ARC";
        case EntityType::Polyline:  return "POLYLINE";
        case EntityType::LwPolyline:return "LWPOLYLINE";
        case EntityType::Spline:    return "SPLINE";
        case EntityType::Text:      return "TEXT";
        case EntityType::MText:     return "MTEXT";
        case EntityType::Dimension: return "DIMENSION";
        case EntityType::Hatch:     return "HATCH";
        case EntityType::Insert:    return "INSERT";
        case EntityType::Point:     return "POINT";
        case EntityType::Ellipse:   return "ELLIPSE";
        case EntityType::Ray:       return "RAY";
        case EntityType::XLine:     return "XLINE";
        case EntityType::Viewport:  return "VIEWPORT";
        case EntityType::Solid:     return "SOLID";
        case EntityType::Leader:    return "LEADER";
        case EntityType::Tolerance: return "TOLERANCE";
        case EntityType::MLine:     return "MLINE";
    }
    return "UNKNOWN";
}

static void write_header(JsonWriter& j, const EntityHeader& hdr, const SceneGraph& scene) {
    j.key_val("type", entity_type_name(hdr.type));
    if (hdr.layer_index >= 0 && hdr.layer_index < (int32_t)scene.layers().size()) {
        j.key_val("layer", scene.layers()[hdr.layer_index].name);
    }
    j.key_color("color", resolve_color(hdr, scene));
    j.key_val("in_block", hdr.in_block);
    const char* space = "model";
    if (hdr.space == DrawingSpace::PaperSpace) space = "paper";
    j.key_val("space", space);
    if (hdr.dwg_handle) j.key_val("handle", hdr.dwg_handle);
}

static void write_entity(JsonWriter& j, const EntityVariant& ev, const SceneGraph& scene) {
    const auto& hdr = ev.header;
    j.obj_open();
    write_header(j, hdr, scene);

    switch (hdr.type) {
    case EntityType::Line: {
        auto* d = ev.as_line();
        if (d) { j.key_vec3("start", d->start); j.key_vec3("end", d->end); }
        break;
    }
    case EntityType::Circle: {
        auto* d = ev.as_circle();
        if (d) {
            j.key_vec3("center", d->center);
            j.key_val("radius", (double)d->radius);
            if (d->minor_radius > 0) {
                j.key_val("minor_radius", (double)d->minor_radius);
                j.key_val("rotation", (double)math::degrees(d->rotation));
                if (d->start_angle != 0 || d->end_angle != 0) {
                    j.key_val("start_angle", (double)math::degrees(d->start_angle));
                    j.key_val("end_angle", (double)math::degrees(d->end_angle));
                }
            }
        }
        break;
    }
    case EntityType::Arc: {
        auto* d = ev.as_arc();
        if (d) {
            j.key_vec3("center", d->center);
            j.key_val("radius", (double)d->radius);
            j.key_val("start_angle", (double)math::degrees(d->start_angle));
            j.key_val("end_angle", (double)math::degrees(d->end_angle));
        }
        break;
    }
    case EntityType::Ellipse: {
        auto* d = ev.get_if_at<12, CircleEntity>();
        if (d) {
            j.key_vec3("center", d->center);
            j.key_val("major_radius", (double)d->radius);
            j.key_val("minor_radius", (double)d->minor_radius);
            j.key_val("rotation", (double)math::degrees(d->rotation));
            j.key_val("start_angle", (double)math::degrees(d->start_angle));
            j.key_val("end_angle", (double)math::degrees(d->end_angle));
        }
        break;
    }
    case EntityType::Polyline:
    case EntityType::LwPolyline: {
        auto* d = ev.as_polyline();
        if (d) {
            j.key_val("closed", d->is_closed);
            j.key("vertices"); j.arr_open();
            auto& vb = scene.vertex_buffer();
            for (int32_t i = 0; i < d->vertex_count && (d->vertex_offset + i) < (int32_t)vb.size(); ++i) {
                if (i > 0) j.buf += ',';
                const auto& v = vb[d->vertex_offset + i];
                char tmp[96];
                std::snprintf(tmp, sizeof(tmp), "[%s,%s,%s]", fmt(v.x).c_str(), fmt(v.y).c_str(), fmt(v.z).c_str());
                j.buf += tmp;
            }
            j.arr_close();
            if (!d->bulges.empty()) {
                j.key("bulges"); j.arr_open();
                for (size_t i = 0; i < d->bulges.size(); ++i) {
                    if (i > 0) j.buf += ',';
                    j.buf += fmt(d->bulges[i]);
                }
                j.arr_close();
            }
        }
        break;
    }
    case EntityType::MLine: {
        auto* d = std::get_if<19>(&ev.data);
        if (d && d->vertex_count >= 2) {
            j.key("vertices"); j.arr_open();
            auto& vb = scene.vertex_buffer();
            for (int32_t i = 0; i < d->vertex_count && (d->vertex_offset + i) < (int32_t)vb.size(); ++i) {
                if (i > 0) j.buf += ',';
                const auto& v = vb[d->vertex_offset + i];
                char tmp[96];
                std::snprintf(tmp, sizeof(tmp), "[%s,%s,%s]", fmt(v.x).c_str(), fmt(v.y).c_str(), fmt(v.z).c_str());
                j.buf += tmp;
            }
            j.arr_close();
        }
        break;
    }
    case EntityType::Spline: {
        auto* d = ev.as_spline();
        if (d) {
            j.key_val("degree", d->degree);
            j.key_val("closed", d->is_closed);
            if (!d->control_points.empty()) {
                j.key("control_points"); j.arr_open();
                for (size_t i = 0; i < d->control_points.size(); ++i) {
                    if (i > 0) j.buf += ',';
                    const auto& p = d->control_points[i];
                    char tmp[96];
                    std::snprintf(tmp, sizeof(tmp), "[%s,%s,%s]", fmt(p.x).c_str(), fmt(p.y).c_str(), fmt(p.z).c_str());
                    j.buf += tmp;
                }
                j.arr_close();
            }
            if (!d->knots.empty()) {
                j.key("knots"); j.arr_open();
                for (size_t i = 0; i < d->knots.size(); ++i) {
                    if (i > 0) j.buf += ',';
                    j.buf += fmt(d->knots[i]);
                }
                j.arr_close();
            }
            if (!d->fit_points.empty()) {
                j.key("fit_points"); j.arr_open();
                for (size_t i = 0; i < d->fit_points.size(); ++i) {
                    if (i > 0) j.buf += ',';
                    const auto& p = d->fit_points[i];
                    char tmp[96];
                    std::snprintf(tmp, sizeof(tmp), "[%s,%s,%s]", fmt(p.x).c_str(), fmt(p.y).c_str(), fmt(p.z).c_str());
                    j.buf += tmp;
                }
                j.arr_close();
            }
        }
        break;
    }
    case EntityType::Text:
    case EntityType::MText:
    case EntityType::Tolerance: {
        auto* d = ev.as_text();
        if (!d) d = ev.get_if_at<18, TextEntity>();
        if (d) {
            j.key_val("text", d->text);
            j.key_val("x", (double)d->insertion_point.x);
            j.key_val("y", (double)d->insertion_point.y);
            j.key_val("height", (double)d->height);
            j.key_val("rotation", (double)math::degrees(d->rotation));
            if (d->width_factor != 1.0f) j.key_val("width_factor", (double)d->width_factor);
            if (d->rect_width > 0) j.key_val("rect_width", (double)d->rect_width);
        }
        break;
    }
    case EntityType::Dimension: {
        auto* d = ev.as_dimension();
        if (d) {
            j.key_vec3("definition_point", d->definition_point);
            j.key_vec3("text_midpoint", d->text_midpoint);
            j.key_val("text", d->text);
            j.key_val("dim_type", d->dimension_type);
            j.key_val("rotation", (double)math::degrees(d->rotation));
        }
        break;
    }
    case EntityType::Hatch: {
        auto* d = ev.as_hatch();
        if (d) {
            j.key_val("pattern", d->pattern_name);
            j.key_val("scale", (double)d->pattern_scale);
            j.key_val("angle", (double)d->pattern_angle);
            j.key_val("solid", d->is_solid);
            j.key_val("loop_count", (int)d->loops.size());
        }
        break;
    }
    case EntityType::Insert: {
        auto* d = ev.as_insert();
        if (d) {
            std::string bname;
            if (d->block_index >= 0 && d->block_index < (int32_t)scene.blocks().size()) {
                bname = scene.blocks()[d->block_index].name;
            }
            j.key_val("block", bname);
            j.key_val("x", (double)d->insertion_point.x);
            j.key_val("y", (double)d->insertion_point.y);
            j.key_val("scale_x", (double)d->x_scale);
            j.key_val("scale_y", (double)d->y_scale);
            j.key_val("rotation", (double)math::degrees(d->rotation));
            if (d->column_count > 1 || d->row_count > 1) {
                j.key_val("columns", d->column_count);
                j.key_val("rows", d->row_count);
                j.key_val("col_spacing", (double)d->column_spacing);
                j.key_val("row_spacing", (double)d->row_spacing);
            }
        }
        break;
    }
    case EntityType::Solid: {
        auto* d = ev.as_solid();
        if (d) {
            j.key("corners"); j.arr_open();
            for (int32_t i = 0; i < d->corner_count && i < 4; ++i) {
                if (i > 0) j.buf += ',';
                const auto& c = d->corners[i];
                char tmp[96];
                std::snprintf(tmp, sizeof(tmp), "[%s,%s,%s]", fmt(c.x).c_str(), fmt(c.y).c_str(), fmt(c.z).c_str());
                j.buf += tmp;
            }
            j.arr_close();
        }
        break;
    }
    case EntityType::Point: {
        auto* d = ev.get_if_at<11, PointEntity>();
        if (d) j.key_vec3("position", d->position);
        break;
    }
    default:
        break;
    }

    j.obj_close();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.dxf|input.dwg> <output.json>\n", argv[0]);
        return 1;
    }

    const char* input_path = argv[1];
    const char* output_path = argv[2];

    SceneGraph scene;

    bool is_dwg = false;
    const char* ext = input_path + std::strlen(input_path);
    while (ext > input_path && *ext != '.' && *ext != '/' && *ext != '\\') ext--;
    if (*ext == '.') is_dwg = (strcasecmp(ext, ".dwg") == 0);

    Result parse_result;
    if (is_dwg) {
        DwgParser dwg_parser;
        parse_result = dwg_parser.parse_file(input_path, scene);
    } else {
        DxfParser dxf_parser;
        parse_result = dxf_parser.parse_file(input_path, scene);
    }
    if (!parse_result.ok()) {
        fprintf(stderr, "Parse error: %s\n", parse_result.message.c_str());
        return 1;
    }
    scene.shrink_to_fit();

    auto& entities = scene.entities();
    auto& layers = scene.layers();
    auto& blocks = scene.blocks();

    JsonWriter j;
    j.obj_open();
    j.key_val("source", input_path);
    j.key_val("generator", "ft_entity_export");
    j.key_val("total_entities", (int64_t)entities.size());

    // Entity type counts
    std::unordered_map<std::string, int64_t> type_counts;
    for (const auto& e : entities) {
        type_counts[entity_type_name(e.type())]++;
    }
    j.key("entity_counts"); j.obj_open();
    for (const auto& [name, count] : type_counts) {
        j.key_val(name.c_str(), count);
    }
    j.obj_close();

    // Layers
    j.key("layers"); j.arr_open();
    for (const auto& l : layers) {
        j.comma();
        j.obj_open();
        j.key_val("name", l.name);
        j.key_color("color", l.color);
        j.key_val("frozen", l.is_frozen);
        j.key_val("off", l.is_off);
        j.obj_close();
    }
    j.arr_close();

    // Blocks
    j.key("blocks"); j.arr_open();
    for (const auto& b : blocks) {
        j.comma();
        j.obj_open();
        j.key_val("name", b.name);
        // Count entities in this block
        int32_t count = 0;
        for (const auto& e : entities) {
            if (e.header.in_block && e.header.owner_block_index >= 0) {
                // Approximate: count by block_index
            }
        }
        j.obj_close();
    }
    j.arr_close();

    // Entities — skip block-definition entities (in_block), only export model-space
    j.key("entities"); j.arr_open();
    size_t exported = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].header.in_block) continue;
        if (exported > 0) j.buf += ',';
        write_entity(j, entities[i], scene);
        exported++;
    }
    j.arr_close();

    j.obj_close();

    // Write output
    bool is_gz = (std::strlen(output_path) > 3 && strcasecmp(output_path + std::strlen(output_path) - 3, ".gz") == 0);
    if (is_gz) {
        // For .gz output, write via gzip pipe
        std::string cmd = "gzip > '" + std::string(output_path) + "'";
        FILE* pipe = popen(cmd.c_str(), "w");
        if (pipe) {
            fwrite(j.buf.data(), 1, j.buf.size(), pipe);
            pclose(pipe);
        }
    } else {
        std::ofstream out(output_path, std::ios::binary);
        out.write(j.buf.data(), j.buf.size());
    }

    printf("Exported %zu entities to %s\n", exported, output_path);
    return 0;
}
