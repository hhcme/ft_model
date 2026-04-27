#include "cad/parser/dxf_blocks_reader.h"
#include "cad/scene/scene_graph.h"

namespace cad {

// Helper: construct EntityVariant with header and data at the correct variant index.
// The EntityData variant has duplicate types, so we must use in_place_index.
static EntityVariant make_entity(EntityHeader hdr, EntityData data) {
    return EntityVariant{hdr, std::move(data)};
}

void DxfBlocksReader::parse_block(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Current token is group code 0, value "BLOCK"
    Block block;

    // Read BLOCK header fields until we hit group code 0 (first entity or ENDBLK)
    while (true) {
        auto peek_result = tokenizer.peek();
        if (!peek_result.ok() || !peek_result.value) break;
        if (tokenizer.peeked().code == 0) break;

        auto next_result = tokenizer.next();
        if (!next_result.ok() || !next_result.value) break;

        const auto& gv = tokenizer.current();
        switch (gv.code) {
            case 2:  block.name = gv.value; break;
            case 10: block.base_point.x = gv.as_float(); break;
            case 20: block.base_point.y = gv.as_float(); break;
            case 30: block.base_point.z = gv.as_float(); break;
            default: break;
        }
    }

    // Add the block to the scene graph to get its index
    int32_t block_index = scene.add_block(std::move(block));

    // Record entity count before parsing block contents
    size_t entity_count_before = scene.total_entity_count();

    // Parse entities inside the block until ENDBLK.
    while (true) {
        const auto& gv = tokenizer.current();

        if (gv.code == 0 && gv.value == "ENDBLK") {
            break;
        }

        if (gv.code == 0) {
            const std::string& entity_type = gv.value;

            if (entity_type == "LINE") {
                LineEntity data{};
                EntityHeader hdr{};
                hdr.type = EntityType::Line;
                hdr.is_visible = true;
                std::string layer_name;

                while (true) {
                    auto nr = tokenizer.next();
                    if (!nr.ok() || !nr.value) break;
                    const auto& p = tokenizer.current();
                    if (p.code == 0) break;
                    switch (p.code) {
                        case 8:  layer_name = p.value; break;
                        case 62: hdr.color_override = p.as_int(); break;
                        case 370: hdr.lineweight = static_cast<float>(p.as_int()) / 100.0f; break;
                        case 10: data.start.x = p.as_float(); break;
                        case 20: data.start.y = p.as_float(); break;
                        case 30: data.start.z = p.as_float(); break;
                        case 11: data.end.x = p.as_float(); break;
                        case 21: data.end.y = p.as_float(); break;
                        case 31: data.end.z = p.as_float(); break;
                        default: break;
                    }
                }
                hdr.bounds = Bounds3d::empty();
                hdr.bounds.expand(data.start);
                hdr.bounds.expand(data.end);
                if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
                scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<0>, std::move(data)}));
            } else if (entity_type == "CIRCLE") {
                CircleEntity data{};
                data.normal = Vec3::unit_z();
                EntityHeader hdr{};
                hdr.type = EntityType::Circle;
                hdr.is_visible = true;
                std::string layer_name;

                while (true) {
                    auto nr = tokenizer.next();
                    if (!nr.ok() || !nr.value) break;
                    const auto& p = tokenizer.current();
                    if (p.code == 0) break;
                    switch (p.code) {
                        case 8:  layer_name = p.value; break;
                        case 62: hdr.color_override = p.as_int(); break;
                        case 370: hdr.lineweight = static_cast<float>(p.as_int()) / 100.0f; break;
                        case 10: data.center.x = p.as_float(); break;
                        case 20: data.center.y = p.as_float(); break;
                        case 30: data.center.z = p.as_float(); break;
                        case 40: data.radius = p.as_float(); break;
                        default: break;
                    }
                }
                float r = data.radius;
                hdr.bounds = Bounds3d{
                    {data.center.x - r, data.center.y - r, data.center.z},
                    {data.center.x + r, data.center.y + r, data.center.z}};
                if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
                scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<1>, std::move(data)}));
            } else if (entity_type == "ARC") {
                ArcEntity data{};
                EntityHeader hdr{};
                hdr.type = EntityType::Arc;
                hdr.is_visible = true;
                std::string layer_name;

                while (true) {
                    auto nr = tokenizer.next();
                    if (!nr.ok() || !nr.value) break;
                    const auto& p = tokenizer.current();
                    if (p.code == 0) break;
                    switch (p.code) {
                        case 8:  layer_name = p.value; break;
                        case 62: hdr.color_override = p.as_int(); break;
                        case 370: hdr.lineweight = static_cast<float>(p.as_int()) / 100.0f; break;
                        case 10: data.center.x = p.as_float(); break;
                        case 20: data.center.y = p.as_float(); break;
                        case 30: data.center.z = p.as_float(); break;
                        case 40: data.radius = p.as_float(); break;
                        case 50: data.start_angle = math::radians(p.as_float()); break;
                        case 51: data.end_angle = math::radians(p.as_float()); break;
                        default: break;
                    }
                }
                hdr.bounds = Bounds3d::empty();
                hdr.bounds.expand({data.center.x, data.center.y, data.center.z});
                if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
                scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<2>, std::move(data)}));
            } else if (entity_type == "LWPOLYLINE") {
                PolylineEntity data{};
                EntityHeader hdr{};
                hdr.type = EntityType::LwPolyline;
                hdr.is_visible = true;
                std::string layer_name;
                std::vector<Vec3> vertices;
                std::vector<float> bulges;

                while (true) {
                    auto nr = tokenizer.next();
                    if (!nr.ok() || !nr.value) break;
                    const auto& p = tokenizer.current();
                    if (p.code == 0) break;
                    switch (p.code) {
                        case 8:  layer_name = p.value; break;
                        case 62: hdr.color_override = p.as_int(); break;
                        case 370: hdr.lineweight = static_cast<float>(p.as_int()) / 100.0f; break;
                        case 70: data.is_closed = (p.as_int() & 1) != 0; break;
                        case 90: break; // vertex count hint
                        case 10: {
                            Vec3 v;
                            v.x = p.as_float();
                            if (tokenizer.peek() && tokenizer.peeked().code == 20) {
                                tokenizer.next();
                                v.y = tokenizer.current().as_float();
                            }
                            vertices.push_back(v);
                            break;
                        }
                        case 42: bulges.push_back(p.as_float()); break;
                        default: break;
                    }
                }
                data.vertex_count = static_cast<int32_t>(vertices.size());
                data.bulges = std::move(bulges);
                hdr.bounds = Bounds3d::empty();
                for (const auto& v : vertices) hdr.bounds.expand(v);
                if (!layer_name.empty()) hdr.layer_index = scene.find_or_add_layer(layer_name);
                int32_t offset = scene.add_polyline_vertices(vertices.data(), vertices.size());
                data.vertex_offset = offset;
                scene.add_entity(make_entity(hdr, EntityData{std::in_place_index<4>, std::move(data)}));
            } else {
                skip_unknown_entity(tokenizer);
            }
            continue;
        }

        // If we got here with a non-zero code, read next
        auto adv = tokenizer.next();
        if (!adv.ok() || !adv.value) break;
    }

    // Record entity indices for this block and mark as block children
    size_t entity_count_after = scene.total_entity_count();
    auto& blocks = const_cast<std::vector<Block>&>(scene.blocks());
    auto& entities = scene.entities();
    for (size_t i = entity_count_before; i < entity_count_after; ++i) {
        blocks[block_index].entity_indices.push_back(static_cast<int32_t>(i));
        entities[i].header.in_block = true;
    }

    // Update block bounds from its entities
    Bounds3d block_bounds = Bounds3d::empty();
    for (size_t i = entity_count_before; i < entity_count_after; ++i) {
        block_bounds.expand(entities[i].bounds());
    }
    blocks[block_index].bounds = block_bounds;
}

Result DxfBlocksReader::read(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // The caller has consumed "SECTION" / "BLOCKS".
    // Read BLOCK...ENDBLK pairs until ENDSEC.

    while (true) {
        auto result = tokenizer.next();
        if (!result.ok() || !result.value) {
            return Result::error(ErrorCode::UnexpectedToken,
                                  "Unexpected EOF in BLOCKS section");
        }

        const auto& gv = tokenizer.current();

        if (gv.is_section_end()) {
            return Result::success();
        }

        if (gv.code == 0 && gv.value == "BLOCK") {
            parse_block(tokenizer, scene);
        }
    }
}

} // namespace cad
