// Role-based handle stream decoding — extracted from dwg_parser.cpp parse_objects().
// Decodes owner, reactors, extension_dictionary, layer, and entity-specific
// handles from the handle stream for graphic entities that produced geometry.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_parse_objects_context.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace cad {

void DwgParser::decode_role_handles(
    ParseObjectsContext& ctx,
    uint32_t obj_type, uint64_t handle,
    EntitySink& scene,
    const uint8_t* obj_data, size_t obj_data_size,
    size_t offset, size_t entity_data_bytes,
    size_t ms_bytes, size_t umc_bytes,
    size_t main_data_bits, size_t entity_bits,
    size_t entities_before,
    uint32_t saved_num_reactors,
    bool saved_is_xdic_missing,
    size_t entity_data_end_offset)
{
    if (scene.entities().size() <= entities_before) return;

    const uint8_t* entity_ptr = obj_data + offset + ms_bytes + umc_bytes;

    struct HandleRoles {
        uint64_t owner = 0;
        uint64_t extension_dictionary = 0;
        uint64_t layer = 0;
        std::vector<uint64_t> reactors;
        std::vector<uint64_t> entity_specific;
        bool ok = false;
    };

    auto read_abs_handle = [&](DwgBitReader& hreader) -> uint64_t {
        auto href = hreader.read_h();
        if (hreader.has_error() || (href.value == 0 && href.code == 0)) {
            return 0;
        }
        return resolve_handle_ref(handle, href);
    };

    // Local wrapper for lookup_object_type with full buffer.
    auto object_type_for_handle = [&](uint64_t ref_handle) -> uint32_t {
        return lookup_object_type(ctx, ref_handle,
                                   obj_data, obj_data_size,
                                   m_version >= DwgVersion::R2007,
                                   m_version >= DwgVersion::R2010);
    };

    HandleRoles roles;
    const size_t hs_bit_start = main_data_bits;
    const size_t hs_bit_end   = entity_bits;
    const size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;

    // R2000 inline handle path: handles are embedded after entity-specific data.
    // entity_data_end_offset is the reader position after entity-specific parsing.
    const bool use_r2000_inline = (hs_bits < 8) &&
        (entity_data_end_offset > 0) &&
        (entity_data_end_offset + 8 <= entity_bits) &&
        (m_version == DwgVersion::R2000);

    if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
        DwgBitReader hreader(entity_ptr, entity_data_bytes);
        hreader.set_bit_offset(hs_bit_start);
        hreader.set_bit_limit(hs_bit_end);


        roles.owner = read_abs_handle(hreader);
        const uint32_t reactor_limit = std::min<uint32_t>(saved_num_reactors, 1024u);
        roles.reactors.reserve(reactor_limit);
        for (uint32_t ri = 0; ri < reactor_limit && !hreader.has_error(); ++ri) {
            uint64_t reactor_handle = read_abs_handle(hreader);
            if (reactor_handle != 0) {
                roles.reactors.push_back(reactor_handle);
            }
        }
        if (!saved_is_xdic_missing && !hreader.has_error()) {
            roles.extension_dictionary = read_abs_handle(hreader);
        }
        if (!hreader.has_error()) {
            roles.layer = read_abs_handle(hreader);
        }

        // Entity-specific handles
        for (int h_idx = 0; h_idx < 30 && !hreader.has_error(); ++h_idx) {
            uint64_t abs_handle = read_abs_handle(hreader);
            if (abs_handle == 0) {
                break;
            }
            roles.entity_specific.push_back(abs_handle);
        }
        roles.ok = !hreader.has_error();

    } else if (use_r2000_inline) {
        // R2000: handles are inline in entity data, after entity-specific fields.
        DwgBitReader hreader(entity_ptr, entity_data_bytes);
        hreader.set_bit_offset(entity_data_end_offset);
        hreader.set_bit_limit(entity_bits);

        roles.owner = read_abs_handle(hreader);
        const uint32_t reactor_limit = std::min<uint32_t>(saved_num_reactors, 1024u);
        roles.reactors.reserve(reactor_limit);
        for (uint32_t ri = 0; ri < reactor_limit && !hreader.has_error(); ++ri) {
            uint64_t reactor_handle = read_abs_handle(hreader);
            if (reactor_handle != 0) {
                roles.reactors.push_back(reactor_handle);
            }
        }
        if (!saved_is_xdic_missing && !hreader.has_error()) {
            roles.extension_dictionary = read_abs_handle(hreader);
        }
        if (!hreader.has_error()) {
            roles.layer = read_abs_handle(hreader);
        }

        for (int h_idx = 0; h_idx < 30 && !hreader.has_error(); ++h_idx) {
            uint64_t abs_handle = read_abs_handle(hreader);
            if (abs_handle == 0) {
                break;
            }
            roles.entity_specific.push_back(abs_handle);
        }
        roles.ok = !hreader.has_error();
    }

    for (size_t eidx = entities_before; eidx < scene.entities().size(); ++eidx) {
        ctx.entity_object_handles[eidx] = handle;
        auto& added_header = scene.entities()[eidx].header;
        if (roles.owner != 0) {
            ctx.entity_owner_handles[eidx] = roles.owner;
            ctx.entity_common_handles[eidx] = {roles.owner};
            added_header.owner_handle = roles.owner;
        }
        int32_t resolved_layer_index = -1;
        if (roles.layer != 0) {
            auto layer_it = m_layer_handle_to_index.find(roles.layer);
            if (layer_it != m_layer_handle_to_index.end()) {
                resolved_layer_index = layer_it->second;
            }
        }
        if (resolved_layer_index < 0 && !m_layer_handle_to_index.empty() &&
            hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
            DwgBitReader scan(entity_ptr, entity_data_bytes);
            scan.set_bit_offset(hs_bit_start);
            scan.set_bit_limit(hs_bit_end);
            for (int h_idx = 0; h_idx < 30 && !scan.has_error(); ++h_idx) {
                uint64_t abs_handle = read_abs_handle(scan);
                if (abs_handle == 0) {
                    break;
                }
                auto layer_it = m_layer_handle_to_index.find(abs_handle);
                if (layer_it != m_layer_handle_to_index.end()) {
                    resolved_layer_index = layer_it->second;
                    break;
                }
            }
        }
        if (resolved_layer_index >= 0) {
            added_header.layer_index = resolved_layer_index;
            ctx.g_layer_resolved++;
        }
    }

    if (obj_type == 7 || obj_type == 8) {
        // INSERT/MINSERT block-header handle resolution.
        // R2004 (exact): RL bitsize is unreliable, handle stream may be
        // misaligned — needs extended scan window and multiple fallbacks.
        // R2000: no separate handle stream at all — handles are embedded
        // in entity data (block_index stays -1 for DWG; DXF path is fine).
        // R2007/R2010+: UMC handle_stream_size is reliable — the default
        // scan at lines 86-93 finds block-header handles correctly.
        std::vector<uint64_t> insert_specific = roles.entity_specific;
        const bool is_r2004_insert = (m_version == DwgVersion::R2004);
        if (insert_specific.empty()) {
            std::vector<uint64_t> fallback_specific;
            auto try_scan_range = [&](size_t from_bit, size_t to_bit) {
                if (from_bit >= to_bit || to_bit > entity_bits) return;
                DwgBitReader scan(entity_ptr, entity_data_bytes);
                scan.set_bit_offset(from_bit);
                scan.set_bit_limit(to_bit);
                for (int h_idx = 0; h_idx < 30 && !scan.has_error(); ++h_idx) {
                    uint64_t abs_handle = read_abs_handle(scan);
                    if (abs_handle == 0) continue;
                    bool is_block_header =
                        object_type_for_handle(abs_handle) == 49 ||
                        m_sections.block_names.find(abs_handle) !=
                            m_sections.block_names.end();
                    if (is_block_header) {
                        fallback_specific.push_back(abs_handle);
                    }
                }
            };
            if (hs_bits >= 8) {
                size_t scan_start = hs_bit_start;
                if (is_r2004_insert && hs_bit_start >= 96) {
                    scan_start = hs_bit_start - 96;
                }
                try_scan_range(scan_start, hs_bit_end);
            }
            if (fallback_specific.empty() && is_r2004_insert && entity_bits >= 64) {
                for (size_t probe = 48; probe + 8 <= entity_bits && fallback_specific.empty(); probe += 8) {
                    try_scan_range(probe, std::min(probe + 128, entity_bits));
                }
            }
            if (fallback_specific.empty() && is_r2004_insert) {
                for (uint64_t h : {roles.owner, roles.extension_dictionary, roles.layer}) {
                    if (h != 0) {
                        bool is_bh = object_type_for_handle(h) == 49 ||
                            m_sections.block_names.find(h) != m_sections.block_names.end();
                        if (is_bh) {
                            fallback_specific.push_back(h);
                        }
                    }
                }
            }
            if (!fallback_specific.empty()) {
                const size_t eidx = scene.entities().size() - 1;
                ctx.insert_handle_fallback_candidates[eidx] =
                    std::move(fallback_specific);
            }
        }
        if (!insert_specific.empty()) {
            const size_t eidx = scene.entities().size() - 1;
            ctx.insert_handles[eidx] = std::move(insert_specific);
        }
    }

    if (obj_type == 34 && roles.ok) {
        auto& all_entities = scene.entities();
        std::vector<int32_t> frozen_layer_indices;
        std::unordered_set<int32_t> seen_layer_indices;
        for (uint64_t ref_handle : roles.entity_specific) {
            auto layer_it = m_layer_handle_to_index.find(ref_handle);
            if (layer_it == m_layer_handle_to_index.end()) {
                continue;
            }
            if (seen_layer_indices.insert(layer_it->second).second) {
                frozen_layer_indices.push_back(layer_it->second);
            }
        }
        if (!frozen_layer_indices.empty()) {
            for (size_t eidx = entities_before; eidx < all_entities.size(); ++eidx) {
                auto& added_header = all_entities[eidx].header;
                if (added_header.type != EntityType::Viewport ||
                    added_header.viewport_index < 0) {
                    continue;
                }
                auto& viewports_mut = const_cast<std::vector<Viewport>&>(scene.viewports());
                if (static_cast<size_t>(added_header.viewport_index) >= viewports_mut.size()) {
                    continue;
                }
                auto& vp = viewports_mut[static_cast<size_t>(added_header.viewport_index)];
                for (int32_t layer_index : frozen_layer_indices) {
                    if (std::find(vp.frozen_layer_indices.begin(),
                                  vp.frozen_layer_indices.end(),
                                  layer_index) == vp.frozen_layer_indices.end()) {
                        vp.frozen_layer_indices.push_back(layer_index);
                        ctx.viewport_frozen_layer_refs++;
                    }
                }
                ctx.viewport_frozen_layer_viewports++;
            }
        }
    }
}

// ---- lookup_object_type implementation ----
uint32_t DwgParser::lookup_object_type(
    ParseObjectsContext& ctx,
    uint64_t ref_handle,
    const uint8_t* obj_data, size_t obj_data_size,
    bool is_r2007_plus, bool is_r2010_plus)
{
    auto type_it = ctx.handle_object_types.find(ref_handle);
    if (type_it != ctx.handle_object_types.end()) {
        return type_it->second;
    }
    auto offset_it = m_sections.handle_map.find(ref_handle);
    PreparedObject ref_record;
    if (offset_it != m_sections.handle_map.end() &&
        prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                 offset_it->second, ref_record, false, false, nullptr)) {
        return ref_record.obj_type;
    }
    {
        const size_t primary_offset =
            offset_it != m_sections.handle_map.end() ? offset_it->second : static_cast<size_t>(-1);
        if (try_recover_object(ref_handle, primary_offset, obj_data, obj_data_size,
                               is_r2007_plus, is_r2010_plus, ref_record)) {
            return ref_record.obj_type;
        }
    }
    return 0;
}

} // namespace cad
