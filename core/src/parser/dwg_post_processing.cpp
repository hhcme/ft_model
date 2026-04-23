// dwg_post_processing.cpp — INSERT block_index resolution, layout parsing,
// and owner resolution. Runs after the main parse_objects() loop.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_parse_objects_context.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cad {

// ============================================================
// run_post_processing — resolve INSERT block_index, layout ownership,
// and entity→space assignment after the main object loop.
// ============================================================

void DwgParser::run_post_processing(ParseObjectsContext& ctx,
                                     EntitySink& scene,
                                     const uint8_t* obj_data, size_t obj_data_size,
                                     bool is_r2010_plus)
{
    // ---- Post-processing: resolve INSERT block_index ----
    // After all objects are parsed, BLOCK_HEADER names (type 49) and
    // BLOCK/ENDBLK definitions are fully populated. Now resolve any INSERTs
    // that couldn't be resolved during the main loop due to ordering.
    //
    // block_names_from_entities (in ctx) contains two key types of entries:
    //   - BLOCK entity handle → full block name (primary, always correct)
    //   - BLOCK_HEADER handle → full block name (from handle stream, also correct)
    //
    // m_sections.block_names contains BLOCK_HEADER (type 49) entries, but those
    // names are truncated for anonymous dimension blocks (e.g., "*D" instead of
    // "*D1077"). Use as a fallback only when the entity map doesn't have the name.
    {
        dwg_debug_log("[DWG] INSERT capture: ctx.insert_handles=%zu fallback_candidates=%zu\n",
            ctx.insert_handles.size(), ctx.insert_handle_fallback_candidates.size());
        // Merge fallback_candidates into ctx.insert_handles for entities not already tracked.
        // The positional parsing (ctx.insert_handles) may catch some; the fallback scan
        // catches others. Both need to be resolved in post-processing.
        for (auto& [eidx, handles] : ctx.insert_handle_fallback_candidates) {
            if (ctx.insert_handles.find(eidx) == ctx.insert_handles.end()) {
                ctx.insert_handles[eidx] = std::move(handles);
                ctx.insert_handle_role_fallbacks++;
            }
        }
        size_t resolved = 0;
        bool apply = true;
        if (apply) {
            auto& all_entities = scene.entities();
            for (auto& [eidx, handles] : ctx.insert_handles) {
                if (eidx >= all_entities.size()) continue;
                auto* ins = std::get_if<InsertEntity>(&all_entities[eidx].data);
                if (!ins || ins->block_index >= 0) continue;

                for (uint64_t h : handles) {
                    std::string name;
                    auto it1 = ctx.block_names_from_entities.find(h);
                    if (it1 != ctx.block_names_from_entities.end()) {
                        name = it1->second;
                    }
                    if (name.empty()) {
                        auto it2 = m_sections.block_names.find(h);
                        if (it2 != m_sections.block_names.end()) {
                            name = it2->second;
                        }
                    }
                    if (!name.empty()) {
                        int32_t block_idx = scene.find_block(name);
                        if (block_idx >= 0) {
                            ins->block_index = block_idx;
                            all_entities[eidx].header.block_index = block_idx;
                            resolved++;
                        }
                        break;
                    }
                }
            }
        }
        dwg_debug_log("[DWG] INSERT post-processing: resolved %zu / %zu INSERTs "
                "(entity_names=%zu, bh_names=%zu, blocks=%zu, apply=%s)\n",
                resolved, ctx.insert_handles.size(),
                ctx.block_names_from_entities.size(), m_sections.block_names.size(),
                scene.blocks().size(), apply ? "yes" : "no");
        if (ctx.insert_handle_role_fallbacks > 0) {
            dwg_debug_log("[DWG] INSERT handle role fallback: %zu\n",
                          ctx.insert_handle_role_fallbacks);
            scene.add_diagnostic({
                "dwg_insert_handle_role_fallback",
                "Handle resolution gap",
                "One or more INSERT/MINSERT objects did not expose a block-header reference at the expected role position; parser used a bounded BLOCK_HEADER-only handle fallback.",
                static_cast<int32_t>(std::min<size_t>(ctx.insert_handle_role_fallbacks, 2147483647u)),
                version_family_name(m_version),
                "INSERT",
            });
        }
    }
    std::unordered_map<uint64_t, int32_t> layout_block_owner_handles;
    std::unordered_map<uint64_t, int32_t> layout_viewport_handles;
    if (!ctx.layout_handle_refs.empty()) {
        auto block_name_for_handle = [&](uint64_t h) -> std::string {
            auto it1 = ctx.block_names_from_entities.find(h);
            if (it1 != ctx.block_names_from_entities.end()) {
                return it1->second;
            }
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) {
                return it2->second;
            }
            return {};
        };
        for (const auto& [layout_index, refs] : ctx.layout_handle_refs) {
            for (uint64_t h : refs) {
                std::string name = block_name_for_handle(h);
                std::string upper = name;
                for (char& c : upper) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                const bool is_space_name = (upper == "*MODEL_SPACE" || upper == "*PAPER_SPACE");
                const bool is_layout_block_header =
                    ctx.handle_object_types.find(h) != ctx.handle_object_types.end() &&
                    ctx.handle_object_types[h] == 49;
                if (is_space_name || is_layout_block_header) {
                    layout_block_owner_handles[h] = layout_index;
                }
                auto type_it = ctx.handle_object_types.find(h);
                if (type_it != ctx.handle_object_types.end() && type_it->second == 34) {
                    layout_viewport_handles[h] = layout_index;
                }
            }
        }
        dwg_debug_log("[DWG] layout handles: layouts=%zu owner_block_refs=%zu viewport_refs=%zu\n",
                      ctx.layout_handle_refs.size(),
                      layout_block_owner_handles.size(),
                      layout_viewport_handles.size());
    }
    if (!layout_viewport_handles.empty()) {
        size_t viewport_layouts_resolved = 0;
        auto& all_entities = scene.entities();
        auto& viewports_mut = const_cast<std::vector<Viewport>&>(scene.viewports());
        for (const auto& [eidx, entity_handle] : ctx.entity_object_handles) {
            if (eidx >= all_entities.size()) continue;
            auto layout_it = layout_viewport_handles.find(entity_handle);
            if (layout_it == layout_viewport_handles.end()) continue;
            auto& header = all_entities[eidx].header;
            if (header.type != EntityType::Viewport) continue;
            header.layout_index = layout_it->second;
            header.space = DrawingSpace::PaperSpace;
            if (header.viewport_index >= 0 &&
                static_cast<size_t>(header.viewport_index) < viewports_mut.size()) {
                viewports_mut[static_cast<size_t>(header.viewport_index)].layout_index =
                    layout_it->second;
            }
            viewport_layouts_resolved++;
        }
        dwg_debug_log("[DWG] layout viewport owners resolved: %zu / %zu\n",
                      viewport_layouts_resolved,
                      layout_viewport_handles.size());
    }
    size_t block_header_space_resolved = 0;
    size_t block_header_model = 0;
    size_t block_header_paper = 0;
    size_t block_header_entities_attached = 0;
    if (!ctx.entity_handle_to_block_header.empty() && !ctx.entity_object_handles.empty()) {
        auto block_name_for_handle = [&](uint64_t h) -> std::string {
            auto it1 = ctx.block_names_from_entities.find(h);
            if (it1 != ctx.block_names_from_entities.end()) {
                return it1->second;
            }
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) {
                return it2->second;
            }
            return {};
        };

        auto& all_entities = scene.entities();
        auto& all_blocks = scene.blocks();
        std::unordered_set<uint64_t> block_header_entity_membership;
        for (size_t bi = 0; bi < all_blocks.size(); ++bi) {
            for (int32_t existing_eidx : all_blocks[bi].header_owned_entity_indices) {
                if (existing_eidx < 0) continue;
                block_header_entity_membership.insert(
                    (static_cast<uint64_t>(bi) << 32) |
                    static_cast<uint32_t>(existing_eidx));
            }
        }
        for (const auto& [eidx, entity_handle] : ctx.entity_object_handles) {
            if (eidx >= all_entities.size()) continue;
            auto owner_it = ctx.entity_handle_to_block_header.find(entity_handle);
            if (owner_it == ctx.entity_handle_to_block_header.end()) continue;

            const uint64_t block_header_handle = owner_it->second;
            std::string name = block_name_for_handle(block_header_handle);
            std::string upper_name = name;
            for (char& c : upper_name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }

            auto& header = all_entities[eidx].header;
            header.block_header_handle = block_header_handle;
            header.owner_handle = block_header_handle;
            auto layout_owner = layout_block_owner_handles.find(block_header_handle);
            if (layout_owner != layout_block_owner_handles.end()) {
                header.layout_index = layout_owner->second;
                const auto& layouts = scene.layouts();
                const bool is_model_layout =
                    layout_owner->second >= 0 &&
                    layout_owner->second < static_cast<int32_t>(layouts.size()) &&
                    layouts[static_cast<size_t>(layout_owner->second)].is_model_layout;
                header.space = is_model_layout ? DrawingSpace::ModelSpace : DrawingSpace::PaperSpace;
                header.in_block = false;
                header.owner_block_index = -1;
                if (is_model_layout) {
                    block_header_model++;
                } else {
                    block_header_paper++;
                }
                block_header_space_resolved++;
                continue;
            }

            if (upper_name == "*MODEL_SPACE") {
                header.space = DrawingSpace::ModelSpace;
                header.in_block = false;
                header.owner_block_index = -1;
                block_header_model++;
                block_header_space_resolved++;
            } else if (upper_name == "*PAPER_SPACE") {
                header.space = DrawingSpace::PaperSpace;
                header.in_block = false;
                header.owner_block_index = -1;
                block_header_paper++;
                block_header_space_resolved++;
            } else if (!name.empty()) {
                int32_t block_idx = scene.find_block(name);
                if (block_idx >= 0) {
                    header.owner_block_index = block_idx;
                    header.in_block = true;
                    const uint64_t membership_key =
                        (static_cast<uint64_t>(block_idx) << 32) |
                        static_cast<uint32_t>(eidx);
                    if (block_header_entity_membership.insert(membership_key).second) {
                        auto& block = all_blocks[static_cast<size_t>(block_idx)];
                        block.header_owned_entity_indices.push_back(static_cast<int32_t>(eidx));
                        block_header_entities_attached++;
                    }
                }
            }
        }
        dwg_debug_log("[DWG] block-header membership: resolved=%zu model=%zu paper=%zu attached=%zu members=%zu entity_handles=%zu\n",
                      block_header_space_resolved,
                      block_header_model,
                      block_header_paper,
                      block_header_entities_attached,
                      ctx.entity_handle_to_block_header.size(),
                      ctx.entity_object_handles.size());
    }
    size_t default_model_space = 0;
    if (block_header_model == 0 && block_header_paper == 0 && !scene.layouts().empty()) {
        auto& all_entities = scene.entities();
        for (const auto& [eidx, entity_handle] : ctx.entity_object_handles) {
            (void)entity_handle;
            if (eidx >= all_entities.size()) continue;
            auto& header = all_entities[eidx].header;
            if (header.space == DrawingSpace::Unknown && !header.in_block) {
                header.space = DrawingSpace::ModelSpace;
                default_model_space++;
            }
        }
        dwg_debug_log("[DWG] default model-space assignment: %zu entities\n",
                      default_model_space);
    }
    {
        size_t resolved = 0;
        size_t paper = 0;
        size_t model = 0;
        size_t normal_block = 0;
        auto& all_entities = scene.entities();
        auto block_name_for_handle = [&](uint64_t h) -> std::string {
            auto it1 = ctx.block_names_from_entities.find(h);
            if (it1 != ctx.block_names_from_entities.end()) {
                return it1->second;
            }
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) {
                return it2->second;
            }
            return {};
        };
        auto upper_block_name_for_handle = [&](uint64_t h) {
            std::string upper = block_name_for_handle(h);
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return upper;
        };
        auto choose_semantic_owner = [&](const std::vector<uint64_t>& handles) -> uint64_t {
            for (uint64_t h : handles) {
                if (layout_block_owner_handles.find(h) != layout_block_owner_handles.end()) {
                    return h;
                }
            }
            for (uint64_t h : handles) {
                auto type_it = ctx.handle_object_types.find(h);
                if (type_it != ctx.handle_object_types.end() && type_it->second != 49) {
                    continue;
                }
                const std::string upper_name = upper_block_name_for_handle(h);
                if (upper_name == "*MODEL_SPACE" || upper_name == "*PAPER_SPACE") {
                    return h;
                }
            }
            for (uint64_t h : handles) {
                auto type_it = ctx.handle_object_types.find(h);
                if (type_it == ctx.handle_object_types.end() || type_it->second != 49) {
                    continue;
                }
                const std::string name = block_name_for_handle(h);
                if (!name.empty()) {
                    return h;
                }
            }
            return 0;
        };

        for (const auto& [eidx, handles] : ctx.entity_common_handles) {
            if (eidx >= all_entities.size()) continue;
            const uint64_t owner_handle = choose_semantic_owner(handles);
            if (owner_handle == 0) continue;

            auto layout_owner = layout_block_owner_handles.find(owner_handle);
            if (layout_owner != layout_block_owner_handles.end()) {
                auto& header = all_entities[eidx].header;
                header.owner_handle = owner_handle;
                header.block_header_handle = owner_handle;
                header.owner_block_index = -1;
                header.layout_index = layout_owner->second;
                header.in_block = false;
                const auto& layouts = scene.layouts();
                const bool is_model_layout =
                    layout_owner->second >= 0 &&
                    layout_owner->second < static_cast<int32_t>(layouts.size()) &&
                    layouts[static_cast<size_t>(layout_owner->second)].is_model_layout;
                header.space = is_model_layout ? DrawingSpace::ModelSpace : DrawingSpace::PaperSpace;
                if (is_model_layout) {
                    model++;
                } else {
                    paper++;
                }
                resolved++;
                continue;
            }

            std::string name = block_name_for_handle(owner_handle);
            if (name.empty()) continue;

            std::string upper_name = upper_block_name_for_handle(owner_handle);
            const bool is_model_space_owner = (upper_name == "*MODEL_SPACE");
            const bool is_paper_space_owner = (upper_name == "*PAPER_SPACE");

            int32_t block_idx = scene.find_block(name);
            if (block_idx < 0 && !is_model_space_owner && !is_paper_space_owner) continue;

            auto& header = all_entities[eidx].header;
            header.owner_handle = owner_handle;
            if (ctx.handle_object_types.find(owner_handle) != ctx.handle_object_types.end() &&
                ctx.handle_object_types[owner_handle] == 49) {
                header.block_header_handle = owner_handle;
            }
            header.owner_block_index = block_idx;
            const Block* block = nullptr;
            if (block_idx >= 0) {
                block = &scene.blocks()[static_cast<size_t>(block_idx)];
            }
            if (is_paper_space_owner || (block && block->is_paper_space)) {
                header.space = DrawingSpace::PaperSpace;
                header.in_block = false;
                paper++;
            } else if (is_model_space_owner || (block && block->is_model_space)) {
                header.space = DrawingSpace::ModelSpace;
                header.in_block = false;
                model++;
            } else {
                // Keep existing in_block state. BLOCK/ENDBLK parsing already
                // marks real block-definition children. Owner handles can also
                // point to dictionaries or ordinary containers; hiding those
                // here would drop valid model-space geometry in real DWGs.
                normal_block++;
            }
            resolved++;
        }
        dwg_debug_log("[DWG] owner resolution: resolved=%zu model=%zu paper=%zu block=%zu owners=%zu\n",
                      resolved, model, paper, normal_block, ctx.entity_common_handles.size());
        if (dwg_debug_enabled() && !ctx.entity_common_handles.empty()) {
            std::unordered_map<uint64_t, size_t> owner_counts;
            for (const auto& [eidx, handles] : ctx.entity_common_handles) {
                (void)eidx;
                const uint64_t owner_handle = choose_semantic_owner(handles);
                if (owner_handle != 0) {
                    owner_counts[owner_handle]++;
                }
            }
            std::vector<std::pair<uint64_t, size_t>> ranked_owners(owner_counts.begin(), owner_counts.end());
            std::sort(ranked_owners.begin(), ranked_owners.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second != b.second) return a.second > b.second;
                          return a.first < b.first;
                      });

            auto block_name_for_debug = [&](uint64_t h) -> std::string {
                auto it1 = ctx.block_names_from_entities.find(h);
                if (it1 != ctx.block_names_from_entities.end()) {
                    return it1->second;
                }
                auto it2 = m_sections.block_names.find(h);
                if (it2 != m_sections.block_names.end()) {
                    return it2->second;
                }
                return {};
            };
            auto object_type_for_debug = [&](uint64_t h) -> uint32_t {
                auto it = ctx.handle_object_types.find(h);
                if (it != ctx.handle_object_types.end()) {
                    return it->second;
                }
                auto offset_it = m_sections.handle_map.find(h);
                if (offset_it == m_sections.handle_map.end()) {
                    return 0;
                }
                PreparedObject record;
                if (!prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                             offset_it->second, record, false, false, nullptr)) {
                    return 0;
                }
                return record.obj_type;
            };
            auto class_name_for_debug_type = [&](uint32_t type) -> std::string {
                auto it = m_sections.class_map.find(type);
                if (it != m_sections.class_map.end()) {
                    return it->second.first;
                }
                return {};
            };

            const size_t owner_limit = std::min<size_t>(ranked_owners.size(), 12);
            for (size_t i = 0; i < owner_limit; ++i) {
                const uint64_t owner = ranked_owners[i].first;
                const std::string name = block_name_for_debug(owner);
                const uint32_t owner_type = object_type_for_debug(owner);
                const std::string owner_class = class_name_for_debug_type(owner_type);
                dwg_debug_log("[DWG] owner top[%zu]: handle=%llu count=%zu type=%u class='%s' name='%s'\n",
                              i,
                              static_cast<unsigned long long>(owner),
                              ranked_owners[i].second,
                              owner_type,
                              owner_class.c_str(),
                              name.c_str());
            }
            for (const auto& [layout_index, refs] : ctx.layout_handle_refs) {
                const size_t ref_limit = std::min<size_t>(refs.size(), 12);
                for (size_t i = 0; i < ref_limit; ++i) {
                    const uint64_t ref = refs[i];
                    const std::string name = block_name_for_debug(ref);
                    const uint32_t ref_type = object_type_for_debug(ref);
                    const std::string ref_class = class_name_for_debug_type(ref_type);
                    dwg_debug_log("[DWG] layout_ref layout=%d idx=%zu handle=%llu type=%u class='%s' name='%s'\n",
                                  layout_index,
                                  i,
                                  static_cast<unsigned long long>(ref),
                                  ref_type,
                                  ref_class.c_str(),
                                  name.c_str());
                }
            }
        }
        const bool has_resolved_space = std::any_of(
            all_entities.begin(), all_entities.end(),
            [](const EntityVariant& ent) {
                return ent.header.space == DrawingSpace::ModelSpace ||
                       ent.header.space == DrawingSpace::PaperSpace;
            });
        if (!ctx.entity_common_handles.empty() && !has_resolved_space) {
            scene.add_diagnostic({
                "dwg_space_owner_unresolved",
                "Semantic gap",
                "DWG entity owner handles were captured but could not be resolved to *MODEL_SPACE or *PAPER_SPACE block records.",
                static_cast<int32_t>(ctx.entity_common_handles.size()),
            });
        }
    }
    dwg_debug_log("[DWG] Layer resolution: resolved=%zu map_size=%zu\n",
            ctx.g_layer_resolved, m_layer_handle_to_index.size());
}

} // namespace cad
