#include "cad/parser/dxf_entities_reader.h"
#include "cad/parser/dxf_tokenizer.h"
#include "cad/scene/entity.h"
#include "cad/cad_types.h"
#include "cad/cad_errors.h"

namespace cad {

bool DxfEntitiesReader::read_entity_header_field(EntityHeader& header,
                                                   const DxfGroupCodeValue& gv) {
    switch (gv.code) {
        case 62:  // Color (ACI index)
            header.color_override = gv.as_int();
            break;
        case 370: // Lineweight (in hundredths of mm)
            header.lineweight = static_cast<float>(gv.as_int()) / 100.0f;
            break;
        case 60:  // Visibility (0 = visible, 1 = invisible)
            header.is_visible = (gv.as_int() == 0);
            break;
        default:
            return false;
    }
    return true;
}

// Helper: construct EntityVariant with header and in_place_index data.
static EntityVariant make_entity(EntityHeader hdr, EntityData data) {
    return EntityVariant{hdr, std::move(data)};
}

Result DxfEntitiesReader::read(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Prime the tokenizer — read the first token
    auto next = tokenizer.next();
    if (!next.ok() || !next.value) {
        return Result::error(ErrorCode::ParseError, "Unexpected end of ENTITIES section");
    }

    while (true) {
        const auto& current = tokenizer.current();

        // After a parse_xxx() returns, current() holds the next entity's
        // code==0 token (or ENDSEC). Check for section end first.
        if (current.is_section_end()) {
            return Result::success();
        }

        // We expect current() to have code==0 (entity type marker).
        // If code != 0, something went wrong — skip forward.
        if (current.code != 0) {
            next = tokenizer.next();
            if (!next.ok() || !next.value) {
                return Result::error(ErrorCode::ParseError, "Unexpected end of ENTITIES section");
            }
            continue;
        }

        const std::string& entity_type = current.value;

        // Each parse_xxx() reads tokens until it hits code==0, then returns.
        // On return, current() holds the next entity's code==0 marker.
        if (entity_type == "LINE")              parse_line(tokenizer, scene);
        else if (entity_type == "CIRCLE")       parse_circle(tokenizer, scene);
        else if (entity_type == "ARC")          parse_arc(tokenizer, scene);
        else if (entity_type == "LWPOLYLINE")   parse_lwpolyline(tokenizer, scene);
        else if (entity_type == "INSERT")       parse_insert(tokenizer, scene);
        else if (entity_type == "TEXT")         parse_text(tokenizer, scene);
        else if (entity_type == "MTEXT")        parse_mtext(tokenizer, scene);
        else if (entity_type == "DIMENSION" ||
                 entity_type == "DIMENSION_LINEAR" ||
                 entity_type == "DIMENSION_ALIGNED")
                                                    parse_dimension(tokenizer, scene);
        else if (entity_type == "HATCH")        parse_hatch(tokenizer, scene);
        else if (entity_type == "SPLINE")       parse_spline(tokenizer, scene);
        else if (entity_type == "ELLIPSE")      parse_ellipse(tokenizer, scene);
        else if (entity_type == "POINT")        parse_point(tokenizer, scene);
        else if (entity_type == "SOLID")        parse_solid(tokenizer, scene);
        else if (entity_type == "POLYLINE")     skip_unknown_entity(tokenizer);
        else                                    skip_unknown_entity(tokenizer);
    }
}

void DxfEntitiesReader::parse_line(DxfTokenizer& tokenizer, SceneGraph& scene) {
    LineEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Line;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.start.x = gv.as_float(); break;
            case 20: data.start.y = gv.as_float(); break;
            case 30: data.start.z = gv.as_float(); break;
            case 11: data.end.x   = gv.as_float(); break;
            case 21: data.end.y   = gv.as_float(); break;
            case 31: data.end.z   = gv.as_float(); break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    hdr.bounds = Bounds3d::empty();
    hdr.bounds.expand(data.start);
    hdr.bounds.expand(data.end);
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_line(make_entity(hdr, EntityData{std::in_place_index<0>, std::move(data)}));
}

void DxfEntitiesReader::parse_circle(DxfTokenizer& tokenizer, SceneGraph& scene) {
    CircleEntity data{};
    data.normal = Vec3::unit_z();
    EntityHeader hdr{};
    hdr.type = EntityType::Circle;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.center.x = gv.as_float(); break;
            case 20: data.center.y = gv.as_float(); break;
            case 30: data.center.z = gv.as_float(); break;
            case 40: data.radius   = gv.as_float(); break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    float r = data.radius;
    hdr.bounds = Bounds3d{
        {data.center.x - r, data.center.y - r, data.center.z},
        {data.center.x + r, data.center.y + r, data.center.z}};
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_circle(make_entity(hdr, EntityData{std::in_place_index<1>, std::move(data)}));
}

void DxfEntitiesReader::parse_arc(DxfTokenizer& tokenizer, SceneGraph& scene) {
    ArcEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Arc;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.center.x     = gv.as_float(); break;
            case 20: data.center.y     = gv.as_float(); break;
            case 30: data.center.z     = gv.as_float(); break;
            case 40: data.radius       = gv.as_float(); break;
            case 50: data.start_angle  = math::radians(gv.as_float()); break;
            case 51: data.end_angle    = math::radians(gv.as_float()); break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    float r = data.radius;
    float sa = data.start_angle;
    float ea = data.end_angle;
    hdr.bounds = Bounds3d::empty();
    hdr.bounds.expand({data.center.x + r * std::cos(sa), data.center.y + r * std::sin(sa), data.center.z});
    hdr.bounds.expand({data.center.x + r * std::cos(ea), data.center.y + r * std::sin(ea), data.center.z});

    // Check cardinal axis crossings for tighter bounds
    auto check_axis = [&](float angle) {
        float a = std::fmod(angle, math::TWO_PI);
        if (a < 0) a += math::TWO_PI;
        float nsa = std::fmod(sa, math::TWO_PI); if (nsa < 0) nsa += math::TWO_PI;
        float nea = std::fmod(ea, math::TWO_PI);  if (nea < 0) nea += math::TWO_PI;
        bool crosses = (nsa <= nea) ? (a >= nsa && a <= nea) : (a >= nsa || a <= nea);
        if (crosses) hdr.bounds.expand({data.center.x + r * std::cos(angle),
                                         data.center.y + r * std::sin(angle), data.center.z});
    };
    check_axis(0.0f);
    check_axis(math::HALF_PI);
    check_axis(math::PI);
    check_axis(math::PI + math::HALF_PI);

    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_arc(make_entity(hdr, EntityData{std::in_place_index<2>, std::move(data)}));
}

void DxfEntitiesReader::parse_lwpolyline(DxfTokenizer& tokenizer, SceneGraph& scene) {
    PolylineEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::LwPolyline;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    std::vector<Vec3> vertices;
    std::vector<float> bulges;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 90: break; // vertex count hint
            case 10: {
                Vec3 v;
                v.x = gv.as_float();
                if (tokenizer.peek() && tokenizer.peeked().code == 20) {
                    tokenizer.next();
                    v.y = tokenizer.current().as_float();
                }
                vertices.push_back(v);
                break;
            }
            case 42: bulges.push_back(gv.as_float()); break;
            case 70: data.is_closed = (gv.as_int() & 1) != 0; break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    data.vertex_count = static_cast<int32_t>(vertices.size());
    data.bulges = std::move(bulges);
    hdr.bounds = Bounds3d::empty();
    for (const auto& v : vertices) hdr.bounds.expand(v);
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    int32_t offset = scene.add_polyline_vertices(vertices.data(), vertices.size());
    data.vertex_offset = offset;
    scene.add_polyline(make_entity(hdr, EntityData{std::in_place_index<4>, std::move(data)}));
}

void DxfEntitiesReader::parse_insert(DxfTokenizer& tokenizer, SceneGraph& scene) {
    InsertEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Insert;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    data.x_scale = 1.0f;
    data.y_scale = 1.0f;
    std::string block_name, layer_name;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 2:  block_name = gv.value; break;
            case 10: data.insertion_point.x = gv.as_float(); break;
            case 20: data.insertion_point.y = gv.as_float(); break;
            case 30: data.insertion_point.z = gv.as_float(); break;
            case 41: data.x_scale = gv.as_float(); break;
            case 42: data.y_scale = gv.as_float(); break;
            case 50: data.rotation = math::radians(gv.as_float()); break;
            case 70: data.column_count = gv.as_int(); break;
            case 71: data.row_count = gv.as_int(); break;
            case 44: data.column_spacing = gv.as_float(); break;
            case 45: data.row_spacing = gv.as_float(); break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    if (!block_name.empty()) data.block_index = scene.find_block(block_name);
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    hdr.bounds = Bounds3d::from_point(data.insertion_point);
    scene.add_insert(make_entity(hdr, EntityData{std::in_place_index<10>, std::move(data)}));
}

// ============================================================
// Phase 2 entity parsers
// ============================================================

void DxfEntitiesReader::parse_text(DxfTokenizer& tokenizer, SceneGraph& scene) {
    TextEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Text;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.insertion_point.x = gv.as_float(); break;
            case 20: data.insertion_point.y = gv.as_float(); break;
            case 30: data.insertion_point.z = gv.as_float(); break;
            case 40: data.height            = gv.as_float(); break;
            case 1:  data.text               = gv.value; break;
            case 50: data.rotation           = math::radians(gv.as_float()); break;
            case 41: data.width_factor       = gv.as_float(); break;
            case 72: data.alignment          = gv.as_int(); break;
            case 8:  layer_name              = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Rough bounding box based on insertion point and estimated text width
    float w = static_cast<float>(data.text.size()) * data.height * 0.6f * data.width_factor;
    float h = data.height;
    hdr.bounds = Bounds3d{
        {data.insertion_point.x, data.insertion_point.y, data.insertion_point.z},
        {data.insertion_point.x + w, data.insertion_point.y + h, data.insertion_point.z}};

    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<6>, std::move(data)}));
}

void DxfEntitiesReader::parse_mtext(DxfTokenizer& tokenizer, SceneGraph& scene) {
    TextEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::MText;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    std::string extra_text;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.insertion_point.x = gv.as_float(); break;
            case 20: data.insertion_point.y = gv.as_float(); break;
            case 30: data.insertion_point.z = gv.as_float(); break;
            case 40: data.height            = gv.as_float(); break;
            case 3:  extra_text.append(gv.value); break;  // Additional text lines
            case 1:  data.text               = gv.value; break;  // Last portion
            case 50: data.rotation           = math::radians(gv.as_float()); break;
            case 41: data.width_factor       = gv.as_float(); break;
            case 71: data.alignment          = gv.as_int(); break;
            case 8:  layer_name              = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Concatenate: code-3 text comes before code-1 text
    if (!extra_text.empty()) {
        extra_text.append(data.text);
        data.text = std::move(extra_text);
    }

    // Rough bounding box
    float w = static_cast<float>(data.text.size()) * data.height * 0.6f * data.width_factor;
    float h = data.height;
    hdr.bounds = Bounds3d{
        {data.insertion_point.x, data.insertion_point.y, data.insertion_point.z},
        {data.insertion_point.x + w, data.insertion_point.y + h, data.insertion_point.z}};

    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<7>, std::move(data)}));
}

void DxfEntitiesReader::parse_ellipse(DxfTokenizer& tokenizer, SceneGraph& scene) {
    CircleEntity data{};
    data.normal = Vec3::unit_z();
    EntityHeader hdr{};
    hdr.type = EntityType::Ellipse;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    float major_x = 0.0f, major_y = 0.0f;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.center.x = gv.as_float(); break;
            case 20: data.center.y = gv.as_float(); break;
            case 30: data.center.z = gv.as_float(); break;
            case 11: major_x       = gv.as_float(); break;
            case 21: major_y       = gv.as_float(); break;
            case 40: /* minor/major ratio — stored for future use */ break;
            case 41: /* start angle (radians) — ignored for now */ break;
            case 42: /* end angle (radians) — ignored for now */ break;
            case 8:  layer_name    = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Major axis length = distance from center to endpoint
    float major_len = std::sqrt(major_x * major_x + major_y * major_y);
    data.radius = major_len;
    // Store major axis direction in normal field for future ellipse rendering
    data.normal = Vec3{major_x, major_y, 0.0f};

    float r = data.radius;
    hdr.bounds = Bounds3d{
        {data.center.x - r, data.center.y - r, data.center.z},
        {data.center.x + r, data.center.y + r, data.center.z}};
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<12>, std::move(data)}));
}

void DxfEntitiesReader::parse_point(DxfTokenizer& tokenizer, SceneGraph& scene) {
    Vec3 data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Point;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.x = gv.as_float(); break;
            case 20: data.y = gv.as_float(); break;
            case 30: data.z = gv.as_float(); break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Expand by a small tolerance around the point
    hdr.bounds = Bounds3d{
        {data.x - 0.1f, data.y - 0.1f, data.z},
        {data.x + 0.1f, data.y + 0.1f, data.z}};
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<11>, std::move(data)}));
}

void DxfEntitiesReader::parse_solid(DxfTokenizer& tokenizer, SceneGraph& scene) {
    SolidEntity data{};
    // Default all corners to origin; third and fourth default to third corner if not provided.
    for (int i = 0; i < 4; ++i) data.corners[i] = Vec3::zero();
    data.corner_count = 3;
    EntityHeader hdr{};
    hdr.type = EntityType::Solid;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    bool has_corner[4] = {false, false, false, false};

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            // First corner (10,20,30)
            case 10: data.corners[0].x = gv.as_float(); has_corner[0] = true; break;
            case 20: data.corners[0].y = gv.as_float(); break;
            case 30: data.corners[0].z = gv.as_float(); break;
            // Second corner (11,21,31)
            case 11: data.corners[1].x = gv.as_float(); has_corner[1] = true; break;
            case 21: data.corners[1].y = gv.as_float(); break;
            case 31: data.corners[1].z = gv.as_float(); break;
            // Third corner (12,22,32)
            case 12: data.corners[2].x = gv.as_float(); has_corner[2] = true; break;
            case 22: data.corners[2].y = gv.as_float(); break;
            case 32: data.corners[2].z = gv.as_float(); break;
            // Fourth corner (13,23,33)
            case 13: data.corners[3].x = gv.as_float(); has_corner[3] = true; break;
            case 23: data.corners[3].y = gv.as_float(); break;
            case 33: data.corners[3].z = gv.as_float(); break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // If fourth corner was not specified, it defaults to the third corner
    if (!has_corner[3]) {
        data.corners[3] = data.corners[2];
        data.corner_count = 3;
    } else {
        data.corner_count = 4;
    }

    hdr.bounds = Bounds3d::empty();
    for (int i = 0; i < data.corner_count; ++i) {
        hdr.bounds.expand(data.corners[i]);
    }
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<16>, std::move(data)}));
}

void DxfEntitiesReader::parse_spline(DxfTokenizer& tokenizer, SceneGraph& scene) {
    SplineEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Spline;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    int32_t knot_count = 0;
    Vec3 current_control{};
    Vec3 current_fit{};
    bool have_control = false;
    bool have_fit = false;

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 70: data.degree      = gv.as_int(); break;
            case 71: data.degree      = gv.as_int(); break;  // degree (alternate code)
            case 72: knot_count       = gv.as_int(); break;
            // Knot values (code 40, multiple occurrences)
            case 40: data.knots.push_back(gv.as_float()); break;
            // Control points (code 10/20 pairs)
            case 10:
                if (have_control) {
                    data.control_points.push_back(current_control);
                }
                current_control.x = gv.as_float();
                have_control = true;
                break;
            case 20:
                current_control.y = gv.as_float();
                break;
            case 30:
                current_control.z = gv.as_float();
                break;
            // Fit points (code 11/21 pairs)
            case 11:
                if (have_fit) {
                    data.fit_points.push_back(current_fit);
                }
                current_fit.x = gv.as_float();
                have_fit = true;
                break;
            case 21:
                current_fit.y = gv.as_float();
                break;
            case 31:
                current_fit.z = gv.as_float();
                break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Push the last accumulated points
    if (have_control) data.control_points.push_back(current_control);
    if (have_fit) data.fit_points.push_back(current_fit);

    // Compute bounds from control points
    hdr.bounds = Bounds3d::empty();
    for (const auto& cp : data.control_points) hdr.bounds.expand(cp);
    for (const auto& fp : data.fit_points) hdr.bounds.expand(fp);

    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<5>, std::move(data)}));
}

// Phase 2 stubs — complex entities deferred to Phase 3
void DxfEntitiesReader::parse_dimension(DxfTokenizer& t, SceneGraph&)  { skip_unknown_entity(t); }

void DxfEntitiesReader::parse_hatch(DxfTokenizer& tokenizer, SceneGraph& scene) {
    HatchEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Hatch;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    int32_t path_count = 0;
    int32_t pattern_type = 0;  // 1 = solid
    // State machine for nested boundary data
    enum class ParseState { Header, Paths, InPath, InEdge };
    ParseState state = ParseState::Header;

    HatchEntity::BoundaryLoop current_loop;
    int32_t current_path_type = 0;
    int32_t edges_remaining = 0;
    bool collecting_vertices = false;
    Vec3 current_vertex{};

    auto finish_loop = [&]() {
        if (!current_loop.vertices.empty()) {
            data.loops.push_back(std::move(current_loop));
        }
        current_loop = HatchEntity::BoundaryLoop{};
        current_path_type = 0;
        edges_remaining = 0;
        collecting_vertices = false;
    };

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;

        switch (gv.code) {
            // Layer
            case 8:  layer_name = gv.value; break;

            // Elevation point (HATCH origin)
            case 10:
                if (collecting_vertices) {
                    // Flush previous vertex if pending
                    current_vertex.y = 0.0f; // reset y in case it wasn't set
                    current_vertex.x = gv.as_float();
                }
                break;
            case 20:
                if (collecting_vertices) {
                    current_vertex.y = gv.as_float();
                    current_loop.vertices.push_back(current_vertex);
                }
                break;
            case 30:
                if (collecting_vertices) {
                    current_vertex.z = gv.as_float();
                    if (!current_loop.vertices.empty()) {
                        current_loop.vertices.back().z = current_vertex.z;
                    }
                }
                break;

            // Bulge (for polyline vertices with arcs)
            case 42:
                // Stored on BoundaryLoop if needed later; ignored for solid fill
                break;

            // Hatch style (0=normal, 1=outermost, 2=ignore)
            case 75: /* hatch style — stored for future use */ break;

            // Hatch pattern type (0=user-defined, 1=predefined, 2=custom)
            case 76:
                pattern_type = gv.as_int();
                break;

            // Number of boundary paths (loops)
            case 91:
                path_count = gv.as_int();
                state = ParseState::Paths;
                break;

            // Path type flag
            case 92:
                finish_loop();  // finalize any previous loop
                current_path_type = gv.as_int();
                // bit 2 (0x02) = polyline boundary
                collecting_vertices = (current_path_type & 2) != 0;
                state = ParseState::InPath;
                break;

            // Number of edges in this boundary path
            case 93:
                edges_remaining = gv.as_int();
                break;

            // Source boundary object references (group 97, 330, etc.) — skip
            case 97:  /* number of source boundary objects */ break;

            default:
                read_entity_header_field(hdr, gv);
                break;
        }
    }

    // Flush the last loop
    finish_loop();

    // Only create entity for solid fill hatches (pattern type 1 = predefined solid)
    if (pattern_type != 1 || data.loops.empty()) {
        // Non-solid or empty hatch — skip. The tokens have already been consumed.
        return;
    }

    data.is_solid = true;

    // Compute bounds from all loop vertices
    hdr.bounds = Bounds3d::empty();
    for (const auto& loop : data.loops) {
        for (const auto& v : loop.vertices) {
            hdr.bounds.expand(v);
        }
    }

    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<9>, std::move(data)}));
}

} // namespace cad
