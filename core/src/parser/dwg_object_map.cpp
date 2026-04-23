#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_object_map.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cad {

Result DwgParser::parse_object_map(const uint8_t* /*data*/, size_t /*size*/)
{
    if (m_sections.object_map.empty()) {
        return Result::error(ErrorCode::ParseError, "No object map data");
    }

    const uint8_t* data = m_sections.object_map.data();
    size_t data_size = m_sections.object_map.size();
    size_t off = 0;

    m_sections.handle_map.clear();
    m_sections.handle_offset_candidates.clear();
    uint64_t total_objects = 0;
    uint64_t global_handle_acc = 0;
    int64_t global_offset_acc = 0;
    uint64_t offsets_in_object_data_range = 0;
    uint64_t offsets_negative = 0;
    uint64_t offsets_out_of_object_data_range = 0;

    size_t r2007_object_data_prefix = 0;

    auto add_offset_candidate = [&](uint64_t handle, int64_t offset) {
        if (handle == 0 || offset < 0) {
            return;
        }
        auto& candidates = m_sections.handle_offset_candidates[handle];
        const size_t uoffset = static_cast<size_t>(offset);
        auto push_candidate = [&](size_t candidate) {
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
                candidates.push_back(candidate);
            }
        };
        push_candidate(uoffset);
        if (m_version == DwgVersion::R2007 &&
            m_sections.object_data.size() >= 12 &&
            m_sections.object_data[8] == 0xCA &&
            m_sections.object_data[9] == 0x0D &&
            m_sections.object_data[10] == 0x00 &&
            m_sections.object_data[11] == 0x00) {
            push_candidate(uoffset + 8);
            push_candidate(uoffset + 12);
        }
        if (m_sections.object_data_file_offset > 0 &&
            uoffset >= m_sections.object_data_file_offset) {
            const size_t relative_offset = uoffset - m_sections.object_data_file_offset;
            push_candidate(relative_offset);
        }
    };

    while (off + 2 <= data_size) {
        size_t section_start = off;
        uint16_t section_size = (static_cast<uint16_t>(data[off]) << 8) |
                                 static_cast<uint16_t>(data[off + 1]);
        off += 2;

        const uint16_t max_object_map_section_size =
            (m_version == DwgVersion::R2007) ? UINT16_MAX : 2040;
        if (section_size == 0 || section_size > max_object_map_section_size) {
            if (section_size > max_object_map_section_size) {
                dwg_debug_log("[DWG] WARNING: object_map section_size=%u too large at off=%zu\n",
                        section_size, section_start);
            }
            break;
        }
        if (section_size <= 2) {
            off = section_start + section_size + 2;
            continue;
        }

        size_t section_data_end = section_start + section_size;
        if (section_data_end > data_size) break;

        uint64_t handle_acc = 0;
        int64_t offset_acc = 0;

        while (off < section_data_end && off + 2 <= data_size) {
            uint32_t handle_delta = read_modular_char(data, data_size, off);
            if (off >= section_data_end) break;
            int32_t offset_delta = read_modular_char_signed(data, data_size, off);

            handle_acc += static_cast<uint64_t>(handle_delta);
            offset_acc += offset_delta;
            global_handle_acc += static_cast<uint64_t>(handle_delta);
            global_offset_acc += offset_delta;

            if (offset_acc < 0) {
                offsets_negative++;
            } else if (m_sections.object_data.empty() ||
                       static_cast<size_t>(offset_acc) < m_sections.object_data.size()) {
                offsets_in_object_data_range++;
            } else {
                offsets_out_of_object_data_range++;
            }
            int64_t effective_offset = offset_acc;
            m_sections.handle_map[handle_acc] = static_cast<size_t>(effective_offset) + r2007_object_data_prefix;
            add_offset_candidate(handle_acc, offset_acc);
            add_offset_candidate(handle_acc, global_offset_acc);
            add_offset_candidate(global_handle_acc, offset_acc);
            add_offset_candidate(global_handle_acc, global_offset_acc);
            total_objects++;
        }

        off = section_data_end + 2;
    }

    // TEMP: dump object_map to file for analysis
    {
        FILE* fmap = fopen("/tmp/dwg_object_map.bin", "wb");
        if (fmap) {
            fwrite(data, 1, data_size, fmap);
            fclose(fmap);
        }
    }
    dwg_debug_log("[DWG] object_map: %llu entries unique=%zu (data_size=%zu, final_off=%zu)\n",
            (unsigned long long)total_objects,
            m_sections.handle_map.size(),
            data_size, off);
    dwg_debug_log("[DWG] object_map offsets: in_object_data=%llu negative=%llu out_of_range=%llu object_data_size=%zu\n",
                  static_cast<unsigned long long>(offsets_in_object_data_range),
                  static_cast<unsigned long long>(offsets_negative),
                  static_cast<unsigned long long>(offsets_out_of_object_data_range),
                  m_sections.object_data.size());
    if (dwg_debug_enabled()) {
        size_t debug_off = 0;
        size_t debug_count = 0;
        while (debug_off + 2 <= data_size && debug_count < 20) {
            uint16_t ss = (static_cast<uint16_t>(data[debug_off]) << 8) |
                          static_cast<uint16_t>(data[debug_off + 1]);
            debug_off += 2;
            if (ss == 0 || ss > 2040) break;
            size_t ss_end = debug_off - 2 + ss;
            if (ss_end > data_size) break;
            uint64_t ha = 0;
            int64_t oa = 0;
            while (debug_off < ss_end && debug_count < 20) {
                uint32_t hd = read_modular_char(data, data_size, debug_off);
                if (debug_off >= ss_end) break;
                int32_t od = read_modular_char_signed(data, data_size, debug_off);
                ha += hd;
                oa += od;
                dwg_debug_log("[DWG] object_map pair %zu: handle=%llu offset=%lld\n",
                              debug_count, (unsigned long long)ha, (long long)oa);
                debug_count++;
            }
            debug_off = ss_end + 2;
        }
    }
    return Result::success();
}

} // namespace cad
