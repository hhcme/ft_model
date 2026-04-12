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
        else if (entity_type == "POLYLINE")     parse_polyline(tokenizer, scene);
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

void DxfEntitiesReader::parse_polyline(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Classic POLYLINE: header followed by VERTEX sub-entities and SEQEND.
    // Structure:
    //   POLYLINE  (66=1 means vertices follow, 70=flags)
    //     VERTEX  (10/20/30=position, 42=bulge)
    //     VERTEX  ...
    //   SEQEND
    PolylineEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Polyline;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    std::vector<Vec3> vertices;
    std::vector<float> bulges;

    // Read POLYLINE header
    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 70: data.is_closed = (gv.as_int() & 1) != 0; break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Read VERTEX sub-entities until SEQEND or non-VERTEX
    while (true) {
        const auto& gv = tokenizer.current();
        if (gv.code != 0) break;

        if (gv.value == "VERTEX") {
            Vec3 vert{};
            float bulge = 0.0f;
            while (true) {
                auto next = tokenizer.next();
                if (!next.ok() || !next.value) break;
                const auto& vgv = tokenizer.current();
                if (vgv.code == 0) break;
                switch (vgv.code) {
                    case 10: vert.x = vgv.as_float(); break;
                    case 20: vert.y = vgv.as_float(); break;
                    case 30: vert.z = vgv.as_float(); break;
                    case 42: bulge = vgv.as_float(); break;
                    default: break;
                }
            }
            vertices.push_back(vert);
            bulges.push_back(bulge);
        } else if (gv.value == "SEQEND") {
            // Consume SEQEND and its trailing group codes
            while (true) {
                auto next = tokenizer.next();
                if (!next.ok() || !next.value) break;
                if (tokenizer.current().code == 0) break;
            }
            break;
        } else {
            // Not VERTEX and not SEQEND — next entity, stop
            break;
        }
    }

    data.vertex_count = static_cast<int32_t>(vertices.size());
    data.bulges = std::move(bulges);
    hdr.bounds = Bounds3d::empty();
    for (const auto& v : vertices) hdr.bounds.expand(v);
    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);

    if (data.vertex_count >= 2) {
        int32_t offset = scene.add_polyline_vertices(vertices.data(), vertices.size());
        data.vertex_offset = offset;
        scene.add_polyline(make_entity(hdr, EntityData{std::in_place_index<3>, std::move(data)}));
    }
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
    EntityHeader hdr{};
    hdr.type = EntityType::Ellipse;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;
    float major_x = 0.0f, major_y = 0.0f;
    float ratio = 1.0f;

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
            case 40: ratio         = gv.as_float(); break; // minor/major ratio
            case 41: data.start_angle = gv.as_float(); break; // start angle (radians)
            case 42: data.end_angle   = gv.as_float(); break; // end angle (radians)
            case 8:  layer_name    = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    float major_len = std::sqrt(major_x * major_x + major_y * major_y);
    data.radius = major_len;
    data.minor_radius = major_len * ratio;
    data.rotation = std::atan2(major_y, major_x); // major axis angle

    // Bounds: approximate using axis-aligned bounding box of rotated ellipse
    float cos_r = std::cos(data.rotation);
    float sin_r = std::sin(data.rotation);
    float a = data.radius;
    float b = data.minor_radius;
    // AABB half-extents of a rotated ellipse
    float hw = std::sqrt(a * a * cos_r * cos_r + b * b * sin_r * sin_r);
    float hh = std::sqrt(a * a * sin_r * sin_r + b * b * cos_r * cos_r);

    hdr.bounds = Bounds3d{
        {data.center.x - hw, data.center.y - hh, data.center.z},
        {data.center.x + hw, data.center.y + hh, data.center.z}};
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
void DxfEntitiesReader::parse_dimension(DxfTokenizer& tokenizer, SceneGraph& scene) {
    DimensionEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Dimension;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    // Definition point (group 10/20/30) — the point where the dimension line meets the extension line
    // Text midpoint (group 11/21/31) — where the dimension text is placed
    // First extension line start (group 13/23/33)
    // Second extension line start (group 14/24/34)
    // Angle (group 50) — rotation for linear/aligned dimensions
    Vec3 ext1_start{}, ext2_start{};

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;
        switch (gv.code) {
            case 10: data.definition_point.x = gv.as_float(); break;
            case 20: data.definition_point.y = gv.as_float(); break;
            case 30: data.definition_point.z = gv.as_float(); break;
            case 11: data.text_midpoint.x = gv.as_float(); break;
            case 21: data.text_midpoint.y = gv.as_float(); break;
            case 31: data.text_midpoint.z = gv.as_float(); break;
            case 13: ext1_start.x = gv.as_float(); break;
            case 23: ext1_start.y = gv.as_float(); break;
            case 33: ext1_start.z = gv.as_float(); break;
            case 14: ext2_start.x = gv.as_float(); break;
            case 24: ext2_start.y = gv.as_float(); break;
            case 34: ext2_start.z = gv.as_float(); break;
            case 70: data.dimension_type = gv.as_int(); break;
            case 50: data.rotation = math::radians(gv.as_float()); break;
            case 1:  data.text = gv.value; break;
            case 8:  layer_name = gv.value; break;
            default: read_entity_header_field(hdr, gv); break;
        }
    }

    // Compute bounds from definition point and extension line origins
    hdr.bounds = Bounds3d::empty();
    hdr.bounds.expand(data.definition_point);
    hdr.bounds.expand(data.text_midpoint);
    hdr.bounds.expand(ext1_start);
    hdr.bounds.expand(ext2_start);

    if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
    scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<8>, std::move(data)}));
}

void DxfEntitiesReader::parse_hatch(DxfTokenizer& tokenizer, SceneGraph& scene) {
    HatchEntity data{};
    EntityHeader hdr{};
    hdr.type = EntityType::Hatch;
    hdr.is_visible = true;
    hdr.dimensionality = 0x02;
    std::string layer_name;

    int32_t path_count = 0;
    bool is_solid_fill = false;

    // Current loop being built
    HatchEntity::BoundaryLoop current_loop;
    int32_t current_path_type = 0;
    bool collecting_polyline_vertices = false;
    Vec3 current_vertex{};

    // Edge parsing state
    int32_t edges_remaining = 0;
    int32_t current_edge_type = 0;
    // Accumulators for edge data
    float edge_x1 = 0, edge_y1 = 0, edge_x2 = 0, edge_y2 = 0; // line edge
    float edge_cx = 0, edge_cy = 0, edge_radius = 0;            // arc edge
    float edge_start_angle = 0, edge_end_angle = 0;
    int32_t edge_ccw = 1;

    auto finish_loop = [&]() {
        if (!current_loop.vertices.empty()) {
            data.loops.push_back(std::move(current_loop));
        }
        current_loop = HatchEntity::BoundaryLoop{};
        current_path_type = 0;
        collecting_polyline_vertices = false;
        edges_remaining = 0;
    };

    auto tessellate_arc_edge = [&](float cx, float cy, float radius,
                                   float start_a, float end_a, bool ccw) {
        // Tessellate arc into line segments
        float span = end_a - start_a;
        if (ccw) {
            if (span < 0) span += math::TWO_PI;
        } else {
            if (span > 0) span -= math::TWO_PI;
        }
        int segs = std::max(4, static_cast<int>(std::abs(span) * radius / 5.0f));
        segs = std::min(segs, 64);
        for (int i = 0; i <= segs; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(segs);
            float a = start_a + t * span;
            float x = cx + radius * std::cos(a);
            float y = cy + radius * std::sin(a);
            current_loop.vertices.push_back({x, y, 0.0f});
        }
    };

    auto flush_edge = [&]() {
        if (current_edge_type == 1) {
            // Line edge: add start and end points
            current_loop.vertices.push_back({edge_x1, edge_y1, 0.0f});
            current_loop.vertices.push_back({edge_x2, edge_y2, 0.0f});
        } else if (current_edge_type == 2) {
            // Arc edge: tessellate
            tessellate_arc_edge(edge_cx, edge_cy, edge_radius,
                                edge_start_angle, edge_end_angle, edge_ccw != 0);
        }
        current_edge_type = 0;
    };

    while (true) {
        auto next = tokenizer.next();
        if (!next.ok() || !next.value) break;
        const auto& gv = tokenizer.current();
        if (gv.code == 0) break;

        switch (gv.code) {
            case 8:  layer_name = gv.value; break;

            // Hatch solid fill flag (1=solid, 0=pattern)
            case 60: /* boundary annotation — skip */ break;

            // Hatch pattern type: 0=user, 1=predefined, 2=custom
            case 75: break;

            // Solid fill flag (group code 70 for HATCH): 1 = solid, 0 = patterned
            case 71:
                is_solid_fill = (gv.as_int() == 1);
                break;

            // Pattern type (0=user, 1=predefined, 2=custom)
            case 76:
                if (gv.as_int() == 1) is_solid_fill = true;
                break;

            // Pattern name
            case 2:
                data.pattern_name = gv.value;
                break;

            // Pattern scale
            case 41:
                data.pattern_scale = gv.as_float();
                break;

            // Pattern angle
            case 52:
                data.pattern_angle = gv.as_float();
                break;

            // Number of boundary paths (loops)
            case 91:
                path_count = gv.as_int();
                break;

            // Path type flag — start of a new boundary loop
            case 92:
                flush_edge();      // flush any pending edge
                finish_loop();     // finalize any previous loop
                current_path_type = gv.as_int();
                // bit 2 (0x04) = polyline, bit 0 (0x01) = edge-defined
                collecting_polyline_vertices = (current_path_type & 0x04) != 0;
                break;

            // Number of edges for edge-defined path
            case 93:
                edges_remaining = gv.as_int();
                break;

            // Edge type (1=line, 2=arc, 3=ellipse arc, 4=spline)
            case 72:
                flush_edge(); // flush previous edge if any
                current_edge_type = gv.as_int();
                break;

            // LINE edge: start point
            case 10:
                if (collecting_polyline_vertices) {
                    current_vertex = {};
                    current_vertex.x = gv.as_float();
                } else if (current_edge_type == 1) {
                    edge_x1 = gv.as_float();
                } else if (current_edge_type == 2) {
                    edge_cx = gv.as_float();
                }
                break;
            case 20:
                if (collecting_polyline_vertices) {
                    current_vertex.y = gv.as_float();
                    current_loop.vertices.push_back(current_vertex);
                } else if (current_edge_type == 1) {
                    edge_y1 = gv.as_float();
                } else if (current_edge_type == 2) {
                    edge_cy = gv.as_float();
                }
                break;

            // LINE edge: end point (group codes 11/21)
            case 11:
                if (current_edge_type == 1) edge_x2 = gv.as_float();
                break;
            case 21:
                if (current_edge_type == 1) edge_y2 = gv.as_float();
                break;

            // ARC edge: radius
            case 40:
                if (current_edge_type == 2) edge_radius = gv.as_float();
                break;

            // ARC edge: start angle (degrees)
            case 50:
                if (current_edge_type == 2) edge_start_angle = gv.as_float() * math::DEG_TO_RAD;
                break;

            // ARC edge: end angle (degrees)
            case 51:
                if (current_edge_type == 2) edge_end_angle = gv.as_float() * math::DEG_TO_RAD;
                break;

            // ARC edge: CCW flag
            case 73:
                if (current_edge_type == 2) edge_ccw = gv.as_int();
                break;

            // Source boundary object references — skip
            case 97:  break;
            case 330: break;

            // Bulge (for polyline vertices with arcs)
            case 42: break;

            default:
                read_entity_header_field(hdr, gv);
                break;
        }
    }

    // Flush any remaining edge and loop
    flush_edge();
    finish_loop();

    if (data.loops.empty()) return;

    data.is_solid = true; // Render all hatch boundaries as solid fill for visual preview

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
