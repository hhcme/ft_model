// BLOCK / ENDBLK tracking — extracted from dwg_parser.cpp parse_objects().
// Handles type 4 (BLOCK) and type 5 (ENDBLK) stream markers that bracket
// block definition entities.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_parse_objects_context.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/scene/scene_graph.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cad {

bool DwgParser::process_block_endblk(
    ParseObjectsContext& ctx,
    uint32_t obj_type, uint64_t handle,
    EntitySink& scene,
    const uint8_t* obj_data, size_t entity_data_bytes,
    size_t ms_bytes, size_t umc_bytes,
    size_t main_data_bits, size_t entity_bits)
{
    if (obj_type == 4) { // BLOCK
        m_block_entity_start = scene.entities().size();
        m_current_block_handle = handle;
        m_current_block_name = "__pending__";
        dwg_debug_log("[DWG] BLOCK handle=%llu entities_before=%zu\n",
                      static_cast<unsigned long long>(handle),
                      m_block_entity_start);
        // First check pre-scan mapping (BLOCK_HEADER handle stream → name)
        auto bn_pre = ctx.block_names_from_entities.find(handle);
        if (bn_pre != ctx.block_names_from_entities.end() && !bn_pre->second.empty()) {
            m_current_block_name = bn_pre->second;
        }
        // Also check direct BLOCK_HEADER handle match (some DWGs share handles)
        if (m_current_block_name == "__pending__") {
            auto bn_direct = m_sections.block_names.find(handle);
            if (bn_direct != m_sections.block_names.end() && !bn_direct->second.empty()) {
                m_current_block_name = bn_direct->second;
            }
        }
        // Try handle stream bridge to find BLOCK_HEADER name.
        {
            size_t hs_bit_start = main_data_bits;
            size_t hs_bit_end   = entity_bits;
            size_t hs_bits = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
            if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                DwgBitReader hreader(obj_data + ms_bytes + umc_bytes, entity_data_bytes);
                hreader.set_bit_offset(hs_bit_start);
                hreader.set_bit_limit(hs_bit_end);
                for (int h_idx = 0; h_idx < 20 && !hreader.has_error(); ++h_idx) {
                    auto href = hreader.read_h();
                    if (hreader.has_error()) break;
                    if (href.value == 0 && href.code == 0) break;
                    uint64_t abs_handle = resolve_handle_ref(handle, href);
                    auto type_it = ctx.handle_object_types.find(abs_handle);
                    if (abs_handle != 0 && abs_handle != handle &&
                        type_it != ctx.handle_object_types.end() && type_it->second == 49) {
                        auto bn_it = m_sections.block_names.find(abs_handle);
                        if (bn_it != m_sections.block_names.end() && !bn_it->second.empty()) {
                            m_current_block_name = bn_it->second;
                            ctx.block_names_from_entities[abs_handle] = bn_it->second;
                            ctx.block_names_from_entities[handle] = bn_it->second;
                        }
                        break;
                    }
                }
            }
        }
        return true; // BLOCK handled
    }

    if (obj_type == 5) { // ENDBLK
        if (m_current_block_name == "__pending__") {
            // Search nearby handles in m_sections.block_names (pre-scanned)
            for (int64_t delta = -10; delta <= 10; ++delta) {
                uint64_t probe = static_cast<uint64_t>(
                    static_cast<int64_t>(m_current_block_handle) + delta);
                auto bn_it = m_sections.block_names.find(probe);
                if (bn_it != m_sections.block_names.end() && !bn_it->second.empty()) {
                    m_current_block_name = bn_it->second;
                    ctx.block_names_from_entities[m_current_block_handle] = bn_it->second;
                    break;
                }
            }
            if (m_current_block_name == "__pending__") {
                m_current_block_name.clear();
            }
        }
        // Always mark entities as in_block — we know they're inside a block
        // definition regardless of whether the name was resolved.
        {
            size_t entity_count = scene.entities().size() - m_block_entity_start;
            dwg_debug_log("[DWG] ENDBLK name='%s' entities=%zu (start=%zu total=%zu) handle=%llu\n",
                          m_current_block_name.c_str(),
                          entity_count,
                          m_block_entity_start,
                          scene.entities().size(),
                          static_cast<unsigned long long>(m_current_block_handle));
            Block block;
            block.name = m_current_block_name;
            block.base_point = m_current_block_base_point;
            block.dwg_block_handle = m_current_block_handle;
            for (size_t i = m_block_entity_start; i < scene.entities().size(); ++i) {
                block.entity_indices.push_back(static_cast<int32_t>(i));
            }
            int32_t block_idx = scene.add_block(block);
            const auto& added_block = scene.blocks()[static_cast<size_t>(block_idx)];
            for (size_t i = m_block_entity_start; i < scene.entities().size(); ++i) {
                auto& child = scene.entities()[i].header;
                child.in_block = true;
                child.owner_block_index = block_idx;
                if (added_block.is_paper_space) {
                    child.space = DrawingSpace::PaperSpace;
                } else if (added_block.is_model_space) {
                    child.space = DrawingSpace::ModelSpace;
                }
            }
            m_current_block_name.clear();
            m_current_block_base_point = Vec3::zero();
            m_current_block_handle = 0;
        }
        return true; // ENDBLK handled
    }

    return false; // Not a BLOCK or ENDBLK
}

} // namespace cad
