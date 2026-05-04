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
                    // Primary: handle-based lookup (handles anonymous blocks
                    // like *D where name alone is ambiguous).
                    auto hbi = ctx.block_handle_to_index.find(h);
                    if (hbi != ctx.block_handle_to_index.end() && hbi->second >= 0) {
                        ins->block_index = hbi->second;
                        all_entities[eidx].header.block_index = hbi->second;
                        resolved++;
                        break;
                    }
                    // Fallback: name-based lookup
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
                const bool is_paper_space_name = (upper == "*PAPER_SPACE");
                const bool is_model_space_name = (upper == "*MODEL_SPACE");
                const bool is_layout_block_header =
                    !is_model_space_name &&
                    ctx.handle_object_types.find(h) != ctx.handle_object_types.end() &&
                    ctx.handle_object_types[h] == 49;
                if (is_paper_space_name || is_layout_block_header) {
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
    // Build block_parent map: block_handle → parent_block_handle
    // Source: entity_handle_to_block_header (entity → owning BLOCK_HEADER)
    // If a BLOCK entity handle appears as an entity in another BLOCK_HEADER's handle stream,
    // the other BLOCK_HEADER is the parent.
    std::unordered_map<uint64_t, uint64_t> block_parent_map;
    {
        // Map BLOCK entity handles → their BLOCK_HEADER handles
        std::unordered_map<uint64_t, uint64_t> block_entity_to_header;
        for (const auto& [block_entity_h, block_name] : ctx.block_names_from_entities) {
            for (const auto& [bh_handle, bh_name] : m_sections.block_names) {
                if (bh_name == block_name) {
                    block_entity_to_header[block_entity_h] = bh_handle;
                    break;
                }
            }
        }
        // Check entity_handle_to_block_header for BLOCK entities
        for (const auto& [entity_handle, owner_bh_handle] : ctx.entity_handle_to_block_header) {
            auto it = block_entity_to_header.find(entity_handle);
            if (it != block_entity_to_header.end() && it->second != owner_bh_handle) {
                block_parent_map[it->second] = owner_bh_handle;
            }
        }
    }
    // T4.4: Deterministic Paper Space inference for layout blocks.
    // First check if the block's BLOCK_HEADER handle is referenced by a LAYOUT
    // object — this is deterministic. Only fall back to entity-count heuristic
    // when the deterministic rule cannot decide.
    {
        auto& all_blocks = scene.blocks();
        for (size_t bi = 0; bi < all_blocks.size(); ++bi) {
            auto& block = all_blocks[bi];
            if (block.is_model_space || block.is_paper_space) continue;
            if (block.name.empty()) continue;

            uint64_t bh_handle = block.dwg_block_header_handle;
            if (bh_handle == 0) {
                // Fallback: look up by name
                for (const auto& [h, name] : m_sections.block_names) {
                    if (name == block.name) { bh_handle = h; break; }
                }
            }
            if (bh_handle == 0) continue;
            if (block_parent_map.count(bh_handle) > 0) continue; // already has parent

            // Deterministic rule: is this block's BLOCK_HEADER handle referenced
            // by any LAYOUT object as its owner block?
            bool is_layout_block = (layout_block_owner_handles.find(bh_handle) !=
                                    layout_block_owner_handles.end());
            if (is_layout_block) {
                block.is_paper_space = true;
                block.is_inferred_paper_space = false; // deterministic, not inferred
                // Re-assign entities to Paper Space without removing from block
                for (int32_t eidx : block.entity_indices) {
                    if (eidx >= 0 && static_cast<size_t>(eidx) < scene.entities().size()) {
                        auto& hdr = scene.entities()[eidx].header;
                        hdr.space = DrawingSpace::PaperSpace;
                        hdr.in_block = false;
                        hdr.owner_block_index = -1;
                    }
                }
                for (int32_t eidx : block.header_owned_entity_indices) {
                    if (eidx >= 0 && static_cast<size_t>(eidx) < scene.entities().size()) {
                        auto& hdr = scene.entities()[eidx].header;
                        hdr.space = DrawingSpace::PaperSpace;
                        hdr.in_block = false;
                        hdr.owner_block_index = -1;
                    }
                }
                dwg_debug_log("[DWG] deterministic Paper Space for block '%s' (layout owner)\n",
                    block.name.c_str());
                continue;
            }

            // Heuristic disabled: entity-count threshold mis-classifies many
            // normal blocks (e.g. *D dimension blocks with >2 entities) as
            // Paper Space.  Rely on deterministic LAYOUT ownership and owner
            // handle resolution (entity_common_handles / block-header membership)
            // for correct space assignment instead.
            (void)block_parent_map;
        }
    }

    // Helper: resolve a block handle to its root space (Model/Paper) by walking up the chain
    auto resolve_block_root_space = [&](uint64_t block_handle) -> int {
        // 0 = unknown, 1 = model, 2 = paper
        std::unordered_set<uint64_t> visited;
        uint64_t current = block_handle;
        for (int depth = 0; depth < 16; ++depth) {
            if (current == 0 || !visited.insert(current).second) break;
            // Check if current is a known space block
            auto name_it = m_sections.block_names.find(current);
            if (name_it != m_sections.block_names.end()) {
                std::string upper = name_it->second;
                for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (upper == "*MODEL_SPACE") return 1;
                if (upper == "*PAPER_SPACE") return 2;
            }
            auto name_it2 = ctx.block_names_from_entities.find(current);
            if (name_it2 != ctx.block_names_from_entities.end()) {
                std::string upper = name_it2->second;
                for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (upper == "*MODEL_SPACE") return 1;
                if (upper == "*PAPER_SPACE") return 2;
            }
            // Walk up to parent
            auto parent_it = block_parent_map.find(current);
            if (parent_it == block_parent_map.end()) break;
            current = parent_it->second;
        }
        return 0;
    };

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
                // Prefer handle-based lookup for anonymous blocks
                int32_t block_idx = -1;
                auto hbi = ctx.block_handle_to_index.find(block_header_handle);
                if (hbi != ctx.block_handle_to_index.end()) {
                    block_idx = hbi->second;
                }
                if (block_idx < 0) {
                    block_idx = scene.find_block(name);
                }
                if (block_idx >= 0) {
                    // Multi-level chain resolution: check if this block's root space is Paper/Model
                    int root_space = resolve_block_root_space(block_header_handle);
                    if (root_space == 2) {
                        // Block chain leads to *Paper_Space → render as PaperSpace
                        header.space = DrawingSpace::PaperSpace;
                        header.in_block = false;
                        header.owner_block_index = -1;
                        header.layout_index = -1;
                        // Try to find the layout index
                        for (const auto& [bh, li] : layout_block_owner_handles) {
                            auto name2 = m_sections.block_names.find(bh);
                            if (name2 != m_sections.block_names.end()) {
                                std::string u = name2->second;
                                for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                                if (u == "*PAPER_SPACE") {
                                    header.layout_index = li;
                                    break;
                                }
                            }
                        }
                        block_header_paper++;
                        block_header_space_resolved++;
                        continue;
                    } else if (root_space == 1) {
                        // Block chain leads to *Model_Space → render as ModelSpace
                        header.space = DrawingSpace::ModelSpace;
                        header.in_block = false;
                        header.owner_block_index = -1;
                        block_header_model++;
                        block_header_space_resolved++;
                        continue;
                    }
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
            // Final fallback: try each handle regardless of type tag.
            // R2018+ may not follow standard handle role ordering.
            for (uint64_t h : handles) {
                const std::string name = block_name_for_handle(h);
                if (!name.empty()) return h;
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

            int32_t block_idx = -1;
            {
                auto hbi = ctx.block_handle_to_index.find(owner_handle);
                if (hbi != ctx.block_handle_to_index.end()) {
                    block_idx = hbi->second;
                }
            }
            if (block_idx < 0) {
                block_idx = scene.find_block(name);
            }
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
                // Multi-level chain resolution: check if this block's root space
                // is Paper/Model by walking up the block parent chain.
                int root_space = resolve_block_root_space(owner_handle);
                if (root_space == 2) {
                    header.space = DrawingSpace::PaperSpace;
                    header.in_block = false;
                    header.owner_block_index = -1;
                    for (const auto& [bh, li] : layout_block_owner_handles) {
                        auto name2 = m_sections.block_names.find(bh);
                        if (name2 != m_sections.block_names.end()) {
                            std::string u = name2->second;
                            for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            if (u == "*PAPER_SPACE") {
                                header.layout_index = li;
                                break;
                            }
                        }
                    }
                    paper++;
                } else if (root_space == 1) {
                    header.space = DrawingSpace::ModelSpace;
                    header.in_block = false;
                    header.owner_block_index = -1;
                    model++;
                } else {
                    // Keep existing in_block state.
                    normal_block++;
                }
            }
            resolved++;
        }
        dwg_debug_log("[DWG] owner resolution: resolved=%zu model=%zu paper=%zu block=%zu owners=%zu\n",
                      resolved, model, paper, normal_block, ctx.entity_common_handles.size());
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
    // Final pass: mark any top-level entity whose block_header_handle or
    // owner_handle maps to a layout-owned block as PaperSpace.  Catches
    // entities that were missed by the block-header membership and owner
    // resolution loops (e.g. *Paper_Space entities not in entity_object_handles).
    {
        size_t final_paper = 0;
        auto& all_entities = scene.entities();
        for (auto& ent : all_entities) {
            if (ent.header.space == DrawingSpace::PaperSpace) continue;
            if (ent.header.in_block) continue;

            uint64_t check_handle = ent.header.block_header_handle;
            if (check_handle == 0) check_handle = ent.header.owner_handle;
            if (check_handle == 0) continue;

            if (layout_block_owner_handles.find(check_handle) !=
                layout_block_owner_handles.end()) {
                ent.header.space = DrawingSpace::PaperSpace;
                ent.header.in_block = false;
                ent.header.owner_block_index = -1;
                final_paper++;
            }
        }
        if (final_paper > 0) {
            dwg_debug_log("[DWG] final paper-space pass (layout owners): %zu entities\n",
                          final_paper);
        }
    }

    dwg_debug_log("[DWG] Layer resolution: resolved=%zu map_size=%zu  Linetype resolution: resolved=%zu map_size=%zu\n",
            ctx.g_layer_resolved, m_layer_handle_to_index.size(),
            ctx.g_linetype_resolved, m_linetype_handle_to_index.size());

    // Xref diagnostics: emit one diagnostic per external reference block
    {
        int32_t xref_count = 0;
        std::string xref_names;
        for (const auto& blk : scene.blocks()) {
            if (!blk.is_xref) continue;
            xref_count++;
            if (xref_names.size() < 200) {
                if (!xref_names.empty()) xref_names += ", ";
                xref_names += blk.name;
            }
        }
        if (xref_count > 0) {
            std::string msg = "Drawing contains ";
            msg += std::to_string(xref_count);
            msg += " external reference(s): ";
            msg += xref_names;
            msg += ". Referenced geometry is shown as bounding boxes; "
                   "load the referenced files separately for full content.";
            scene.add_diagnostic({
                "dwg_xref_detected",
                "External dependency",
                msg,
                xref_count,
                version_family_name(m_version),
                "Blocks",
            });
        }
    }

    // SHX font diagnostics: report fonts that require vector glyph support
    {
        int32_t shx_count = 0;
        std::string shx_names;
        for (size_t si = 0; si < scene.text_styles().size(); ++si) {
            const auto& ts = scene.text_styles()[si];
            if (!ts.is_shx) continue;
            shx_count++;
            if (shx_names.size() < 200) {
                if (!shx_names.empty()) shx_names += ", ";
                shx_names += ts.font_file.empty() ? ts.name : ts.font_file;
            }
        }
        if (shx_count > 0) {
            scene.add_diagnostic({
                "dwg_shx_fonts",
                "External dependency",
                "Drawing uses " + std::to_string(shx_count) +
                " SHX vector font(s): " + shx_names +
                ". SHX glyphs are rendered using system font fallback; "
                "stroke-accurate rendering requires SHX parsing.",
                shx_count,
                version_family_name(m_version),
                "Text styles",
            });
        }
    }

    // T4.3: Force DEFPOINTS POINT entities into block if owner is a *D dimension block.
    // These auxiliary points are owned by anonymous dimension blocks but may leak
    // to top-level when BLOCK/ENDBLK boundaries are missed.
    {
        size_t forced_in_block = 0;
        auto& all_entities = scene.entities();
        auto& all_blocks = scene.blocks();
        const auto& all_layers = scene.layers();
        auto block_name_for_handle_t43 = [&](uint64_t h) -> std::string {
            auto it1 = ctx.block_names_from_entities.find(h);
            if (it1 != ctx.block_names_from_entities.end()) return it1->second;
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) return it2->second;
            return {};
        };
        for (size_t eidx = 0; eidx < all_entities.size(); ++eidx) {
            auto& ent = all_entities[eidx];
            if (ent.type() != EntityType::Point) continue;
            // Check layer is DEFPOINTS
            if (ent.header.layer_index < 0 ||
                static_cast<size_t>(ent.header.layer_index) >= all_layers.size()) {
                continue;
            }
            std::string layer_name = all_layers[ent.header.layer_index].name;
            {
                std::string upper = layer_name;
                for (char& c : upper) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                if (upper != "DEFPOINTS") continue;
            }
            // Check if owner handle maps to an anonymous dimension block (*D...)
            uint64_t owner_bh = ent.header.block_header_handle;
            if (owner_bh == 0) continue;
            std::string owner_name = block_name_for_handle_t43(owner_bh);
            if (owner_name.empty()) continue;
            bool is_dim_block = false;
            if (owner_name.size() >= 2 && owner_name[0] == '*') {
                char prefix = static_cast<char>(std::toupper(static_cast<unsigned char>(owner_name[1])));
                is_dim_block = (prefix == 'D');
            }
            if (!is_dim_block) continue;
            // Find the block by handle and force in_block
            auto hbi = ctx.block_handle_to_index.find(owner_bh);
            if (hbi != ctx.block_handle_to_index.end() && hbi->second >= 0) {
                int32_t block_idx = hbi->second;
                ent.header.in_block = true;
                ent.header.owner_block_index = block_idx;
                // Add to block's header_owned_entity_indices if not already there
                if (block_idx < static_cast<int32_t>(all_blocks.size())) {
                    auto& block = all_blocks[block_idx];
                    bool already_present = false;
                    for (int32_t bei : block.header_owned_entity_indices) {
                        if (bei == static_cast<int32_t>(eidx)) {
                            already_present = true;
                            break;
                        }
                    }
                    if (!already_present) {
                        block.header_owned_entity_indices.push_back(static_cast<int32_t>(eidx));
                    }
                }
                forced_in_block++;
            }
        }
        if (forced_in_block > 0) {
            dwg_debug_log("[DWG] DEFPOINTS POINT forced in_block: %zu\n", forced_in_block);
        }
    }

    // Plot style (CTB/STB) diagnostics
    {
        const auto& meta = scene.drawing_info();
        if (!meta.plot_style_table.empty()) {
            const char* mode = meta.uses_named_plot_styles ? "named (STB)" : "color-dependent (CTB)";
            scene.add_diagnostic({
                "dwg_plot_style_table",
                "External dependency",
                "Drawing references plot style table '" + meta.plot_style_table +
                "' (" + std::string(mode) +
                "). Plot styles are not applied in viewer mode; "
                "use Print/PDF export for plot-style output.",
                1,
                version_family_name(m_version),
                "Plot styles",
            });
        }
    }
}

} // namespace cad
