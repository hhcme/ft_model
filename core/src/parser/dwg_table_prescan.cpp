// Pre-scan table objects — LTYPE, LAYER, BLOCK_HEADER handle maps.
// Extracted from dwg_parser.cpp to reduce file size.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/scene/scene_graph.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace cad {

void DwgParser::prescan_table_objects(
    const uint8_t* obj_data, size_t obj_data_size,
    bool is_r2007_plus, bool is_r2010_plus,
    EntitySink& scene,
    std::unordered_map<uint64_t, std::string>& block_names_from_entities)
{
    // Lambda: pre-scan a single table type (LTYPE=57 or LAYER=51)
    // by iterating the handle map and calling parse_dwg_table_object.
    auto prescan_table_type = [&](uint32_t target_type) {
        for (const auto& [handle, offset] : m_sections.handle_map) {
            if (offset >= obj_data_size) continue;

            DwgBitReader ms_r(obj_data + offset, obj_data_size - offset);
            uint32_t esz = ms_r.read_modular_short();
            if (ms_r.has_error() || esz == 0 || esz > obj_data_size - offset) continue;
            ms_r.align_to_byte();
            size_t msb = ms_r.bit_offset() / 8;

            size_t umcb = 0;
            uint32_t hss = 0;
            if (is_r2010_plus) {
                uint32_t res = 0;
                int shift = 0;
                const uint8_t* up = obj_data + offset + msb;
                size_t uavail = obj_data_size - offset - msb;
                for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail; ++i) {
                    uint8_t bv = up[i];
                    res |= static_cast<uint32_t>(bv & 0x7F) << shift;
                    shift += 7;
                    umcb = static_cast<size_t>(i) + 1;
                    if ((bv & 0x80) == 0) break;
                }
                hss = res;
            }

            size_t edb = static_cast<size_t>(esz);
            if (offset + msb + umcb + edb > obj_data_size) continue;

            DwgBitReader r(obj_data + offset + msb + umcb, edb);
            size_t mdb = edb * 8;
            if (is_r2010_plus && hss <= edb * 8) mdb = edb * 8 - hss;
            r.set_bit_limit(mdb);
            if (is_r2007_plus && mdb > 0) r.setup_string_stream(static_cast<uint32_t>(mdb));

            uint32_t ot = is_r2010_plus ? r.read_bot() : r.read_bs();
            if (r.has_error() || ot != target_type) continue;
            if (!is_r2010_plus && m_version >= DwgVersion::R2004) {
                const uint32_t object_end_bit = r.read_rl();
                if (r.has_error() || object_end_bit == 0 || object_end_bit > edb * 8) {
                    continue;
                }
                mdb = object_end_bit;
                r.set_bit_limit(mdb);
                if (is_r2007_plus) {
                    r.setup_string_stream(static_cast<uint32_t>(mdb));
                }
            }

            // Skip common object header: handle (H) + EED loop + reactors + flags
            (void)r.read_h();  // object handle
            uint16_t eed_sz = 0;
            while (!r.has_error()) {
                eed_sz = r.read_bs();
                if (eed_sz == 0) break;
                (void)r.read_h();  // EED application handle
                size_t skip = static_cast<size_t>(eed_sz) * 8;
                if (r.bit_offset() + skip <= r.bit_limit()) {
                    r.set_bit_offset(r.bit_offset() + skip);
                } else {
                    break;
                }
            }
            (void)r.read_bl();  // num_reactors (BL)
            (void)r.read_b();   // is_xdic_missing (B, R2004+)
            if (!r.has_error()) {
                parse_dwg_table_object(r, ot, scene, m_version,
                                       edb * 8, mdb, handle,
                                       &m_layer_handle_to_index,
                                       &m_linetype_handle_to_index);
            }
        }
    };

    prescan_table_type(57); // LTYPE
    prescan_table_type(51); // LAYER

    // Pre-scan BLOCK_HEADER (type 49) to populate block names before
    // the main loop encounters BLOCK entities that need name lookups.
    // parse_dwg_table_object doesn't handle type 49, so we do it here.
    {
        size_t prescan_block_names = 0;
        size_t prescan_reverse_maps = 0;
        for (const auto& [bh_handle, bh_offset] : m_sections.handle_map) {
            if (bh_offset >= obj_data_size) continue;
            DwgBitReader ms_r(obj_data + bh_offset, obj_data_size - bh_offset);
            uint32_t esz = ms_r.read_modular_short();
            if (ms_r.has_error() || esz == 0 || esz > obj_data_size - bh_offset) continue;
            ms_r.align_to_byte();
            size_t msb = ms_r.bit_offset() / 8;

            size_t umcb = 0;
            if (is_r2010_plus) {
                const uint8_t* up = obj_data + bh_offset + msb;
                size_t uavail = obj_data_size - bh_offset - msb;
                for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail; ++i) {
                    umcb = static_cast<size_t>(i) + 1;
                    if ((up[i] & 0x80) == 0) break;
                }
            }

            size_t edb = static_cast<size_t>(esz);
            if (bh_offset + msb + umcb + edb > obj_data_size) continue;

            DwgBitReader r(obj_data + bh_offset + msb + umcb, edb);
            size_t mdb = edb * 8;
            if (is_r2010_plus) {
                // Read UMC for handle stream size
                uint32_t hss = 0;
                int hss_shift = 0;
                const uint8_t* up2 = obj_data + bh_offset + msb;
                size_t uavail2 = obj_data_size - bh_offset - msb;
                for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail2; ++i) {
                    hss |= static_cast<uint32_t>(up2[i] & 0x7F) << hss_shift;
                    hss_shift += 7;
                    if ((up2[i] & 0x80) == 0) break;
                }
                if (hss <= edb * 8) mdb = edb * 8 - hss;
            }
            r.set_bit_limit(mdb);
            if (is_r2007_plus && mdb > 0) r.setup_string_stream(static_cast<uint32_t>(mdb));

            uint32_t ot = is_r2010_plus ? r.read_bot() : r.read_bs();
            if (r.has_error() || ot != 49) continue;
            if (!is_r2010_plus && m_version >= DwgVersion::R2004) {
                uint32_t object_end_bit = r.read_rl();
                if (r.has_error() || object_end_bit == 0 || object_end_bit > edb * 8) continue;
                mdb = object_end_bit;
                r.set_bit_limit(mdb);
                if (is_r2007_plus) r.setup_string_stream(static_cast<uint32_t>(mdb));
            }
            // Skip common object header: handle + EED
            (void)r.read_h();
            uint16_t eed_sz = 0;
            while (!r.has_error()) {
                eed_sz = r.read_bs();
                if (eed_sz == 0) break;
                (void)r.read_h();
                size_t skip = static_cast<size_t>(eed_sz) * 8;
                if (r.bit_offset() + skip <= r.bit_limit()) {
                    r.set_bit_offset(r.bit_offset() + skip);
                } else break;
            }
            (void)r.read_bl();  // num_reactors
            (void)r.read_b();   // is_xdic_missing

            // BLOCK_HEADER specific: T(name) + BS(is_xref_resolved) + flags + BL(num_owned)
            std::string block_name = r.read_t();
            if (!block_name.empty() && !r.has_error()) {
                m_sections.block_names[bh_handle] = block_name;
                prescan_block_names++;
                // Also read handle stream to find the BLOCK entity handle.
                // This lets us map BLOCK_handle → name without relying on
                // nearby handle search during the main loop.
                size_t hs_start = static_cast<size_t>(is_r2010_plus ? 0 : 0);
                if (is_r2010_plus) {
                    // For R2010+, use UMC handle_stream_size
                    const uint8_t* up3 = obj_data + bh_offset + msb;
                    size_t uavail3 = obj_data_size - bh_offset - msb;
                    uint32_t hss = 0;
                    int hss_shift3 = 0;
                    for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail3; ++i) {
                        hss |= static_cast<uint32_t>(up3[i] & 0x7F) << hss_shift3;
                        hss_shift3 += 7;
                        if ((up3[i] & 0x80) == 0) break;
                    }
                    hs_start = (hss < edb * 8) ? (edb * 8 - hss) : 0;
                } else if (m_version >= DwgVersion::R2004) {
                    // For R2004, mdb (from RL bitsize) marks start of handle stream
                    hs_start = mdb;
                }
                if (hs_start > 0 && hs_start < edb * 8) {
                    DwgBitReader hreader2(obj_data + bh_offset + msb + umcb, edb);
                    hreader2.set_bit_offset(hs_start);
                    hreader2.set_bit_limit(edb * 8);
                    auto bref = hreader2.read_h();
                    if (!hreader2.has_error()) {
                        uint64_t block_entity_h = resolve_handle_ref(bh_handle, bref);
                        if (block_entity_h != 0 && block_entity_h != bh_handle) {
                            block_names_from_entities[block_entity_h] = block_name;
                            prescan_reverse_maps++;
                        }
                    }
                }
            }
        }
        dwg_debug_log("[DWG] BLOCK_HEADER pre-scan: %zu names loaded, %zu reverse maps\n",
                      prescan_block_names, prescan_reverse_maps);
    }
    dwg_debug_log("[DWG] Pre-scan: %zu layer handles, %zu linetype handles, %zu block names loaded\n",
            m_layer_handle_to_index.size(), m_linetype_handle_to_index.size(),
            m_sections.block_names.size());
}

} // namespace cad
