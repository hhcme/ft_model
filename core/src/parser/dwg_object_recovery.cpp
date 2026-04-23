// Object preparation and recovery helpers.
// Framing, candidate recovery, and page-scan recovery for DWG objects.
// Extracted from dwg_parser.cpp.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_parse_helpers.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace cad {

bool DwgParser::is_known_object_type(uint32_t obj_type) const
{
    if (is_graphic_entity(obj_type)) {
        return true;
    }
    switch (obj_type) {
        case 42: case 43: case 48: case 49: case 50: case 51:
        case 52: case 53: case 54: case 55: case 56: case 57:
        case 58: case 59: case 60: case 61: case 62: case 64:
        case 65: case 69: case 70: case 74: case 79: case 82:
            return true;
        default:
            return m_sections.class_map.find(obj_type) != m_sections.class_map.end();
    }
}

bool DwgParser::prepare_object_at_offset(const uint8_t* obj_data, size_t obj_data_size,
                                          bool is_r2010_plus,
                                          size_t record_offset, PreparedObject& record,
                                          bool require_valid_handle_stream,
                                          bool require_known_type,
                                          const char** failure_reason) const
{
    auto fail = [&](const char* reason) {
        if (failure_reason) {
            *failure_reason = reason;
        }
        return false;
    };
    if (record_offset >= obj_data_size) {
        return fail("offset_out_of_range");
    }

    DwgBitReader ms_reader(obj_data + record_offset, obj_data_size - record_offset);
    uint32_t entity_size = ms_reader.read_modular_short();
    if (ms_reader.has_error() || entity_size == 0 ||
        entity_size > obj_data_size - record_offset) {
        return fail("invalid_entity_size");
    }

    ms_reader.align_to_byte();
    const size_t ms_bytes = ms_reader.bit_offset() / 8;
    if (record_offset + ms_bytes > obj_data_size) {
        return fail("invalid_ms_size");
    }

    size_t umc_bytes = 0;
    uint32_t handle_stream_size = 0;
    if (is_r2010_plus) {
        uint32_t result = 0;
        int shift = 0;
        const size_t uavail = obj_data_size - record_offset - ms_bytes;
        const uint8_t* umc_ptr = obj_data + record_offset + ms_bytes;
        bool finished = false;
        for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail; ++i) {
            const uint8_t byte_val = umc_ptr[i];
            result |= static_cast<uint32_t>(byte_val & 0x7F) << shift;
            shift += 7;
            umc_bytes = static_cast<size_t>(i) + 1;
            if ((byte_val & 0x80) == 0) {
                finished = true;
                break;
            }
        }
        if (!finished) {
            return fail("invalid_umc");
        }
        handle_stream_size = result;
    }

    const size_t entity_data_bytes = static_cast<size_t>(entity_size);
    if (record_offset + ms_bytes + umc_bytes + entity_data_bytes > obj_data_size) {
        return fail("entity_data_out_of_range");
    }

    const size_t entity_bits = entity_data_bytes * 8;
    size_t main_data_bits = entity_bits;
    bool handle_stream_valid = true;
    if (is_r2010_plus) {
        if (handle_stream_size > entity_bits) {
            handle_stream_valid = false;
            if (require_valid_handle_stream) {
                return fail("invalid_handle_stream_size");
            }
        } else {
            main_data_bits = entity_bits - handle_stream_size;
        }
    }
    if (main_data_bits < 8) {
        return fail("empty_main_data");
    }

    DwgBitReader reader(obj_data + record_offset + ms_bytes + umc_bytes,
                        entity_data_bytes);
    reader.set_bit_limit(entity_bits);
    const uint32_t obj_type = is_r2010_plus ? reader.read_bot() : reader.read_bs();
    size_t framed_main_data_bits = main_data_bits;
    // R2004-R2007: bitsize (RL) indicates end of main data / start of handle stream.
    // R2010+: handle_stream_size (UMC) is read separately before object data.
    if (!is_r2010_plus && m_version >= DwgVersion::R2004) {
        framed_main_data_bits = reader.read_rl();
        if (reader.has_error() || framed_main_data_bits == 0 ||
            framed_main_data_bits > entity_bits ||
            framed_main_data_bits <= reader.bit_offset()) {
            return fail("invalid_object_end_bit");
        }
        // R2004 handle stream heuristic: for most objects the handle stream
        // contains at least owner handle (typically 16-40 bits). Streams
        // smaller than 12 bits or larger than 256 bits are suspicious and
        // likely indicate an interior object-map offset.
        if (m_version == DwgVersion::R2004) {
            size_t hs_bits = (entity_bits > framed_main_data_bits)
                                 ? (entity_bits - framed_main_data_bits)
                                 : 0;
            if (hs_bits < 12 || hs_bits > 256) {
                return fail("r2004_handle_stream_heuristic");
            }
        }
        reader.set_bit_limit(framed_main_data_bits);
    }
    if (reader.has_error() || (require_known_type && !is_known_object_type(obj_type))) {
        return fail(reader.has_error() ? "object_type_read_failed" : "unknown_object_type");
    }
    auto self_handle = reader.read_h();
    if (reader.has_error()) {
        return fail("self_handle_read_failed");
    }

    record.offset = record_offset;
    record.ms_bytes = ms_bytes;
    record.umc_bytes = umc_bytes;
    record.entity_data_bytes = entity_data_bytes;
    record.entity_bits = entity_bits;
    record.main_data_bits = framed_main_data_bits;
    record.handle_stream_bits = entity_bits > framed_main_data_bits ? entity_bits - framed_main_data_bits : 0;
    record.handle_stream_valid = handle_stream_valid;
    record.obj_type = obj_type;
    record.self_handle = self_handle;
    if (m_version >= DwgVersion::R2007 && framed_main_data_bits > 0) {
        DwgBitReader string_probe(obj_data + record.entity_data_offset(), entity_data_bytes);
        string_probe.set_bit_limit(framed_main_data_bits);
        string_probe.setup_string_stream(static_cast<uint32_t>(framed_main_data_bits));
        record.has_string_stream = string_probe.has_string_stream();
        record.string_stream_bit_pos = string_probe.string_stream_bit_pos();
    }
    return true;
}

bool DwgParser::recover_from_candidates(uint64_t target_handle,
                                         size_t primary_offset,
                                         const uint8_t* obj_data, size_t obj_data_size,
                                         bool is_r2010_plus,
                                         PreparedObject& recovered) const
{
    auto it = m_sections.handle_offset_candidates.find(target_handle);
    if (it == m_sections.handle_offset_candidates.end()) {
        return false;
    }

    bool found = false;
    PreparedObject candidate;
    for (size_t candidate_offset : it->second) {
        if (candidate_offset == primary_offset) {
            continue;
        }
        PreparedObject current;
        if (!prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                      candidate_offset, current, false, false)) {
            continue;
        }
        const uint64_t self_abs = resolve_handle_ref(target_handle, current.self_handle);
        if (self_abs != target_handle) {
            continue;
        }
        if (found) {
            return false;
        }
        candidate = current;
        found = true;
    }
    if (!found) {
        return false;
    }
    recovered = candidate;
    return true;
}

bool DwgParser::try_recover_object(uint64_t target_handle, size_t primary_offset,
                                    const uint8_t* obj_data, size_t obj_data_size,
                                    bool is_r2007_plus, bool is_r2010_plus,
                                    PreparedObject& recovered)
{
    // Strategy 1: try candidate offsets from handle_offset_candidates map.
    if (recover_from_candidates(target_handle, primary_offset,
                                obj_data, obj_data_size, is_r2010_plus, recovered)) {
        return true;
    }

    // Strategy 2: full page scan to find matching self-handle.
    // Recovery is intentionally strict and bounded: it is a generic DWG
    // object-map repair path for bad offsets, not a file-specific fallback.
    if (++m_object_recovery_scans > 64) {
        return false;
    }

    bool found = false;
    PreparedObject candidate;
    auto scan_range = [&](size_t begin, size_t end) {
        end = std::min(end, obj_data_size);
        for (size_t pos = begin; pos < end; ++pos) {
            PreparedObject current;
            if (!prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                          pos, current, true, true)) {
                continue;
            }
            const uint64_t self_abs = resolve_handle_ref(target_handle, current.self_handle);
            if (self_abs != target_handle) {
                continue;
            }
            if (found) {
                // Ambiguous self-handle matches are worse than dropping a
                // corrupt object-map entry, because choosing one can move
                // large layouts or blocks into the wrong space.
                found = false;
                return false;
            }
            candidate = current;
            found = true;
        }
        return true;
    };

    if (!m_sections.object_pages.empty()) {
        for (const auto& [page_offset, page_size] : m_sections.object_pages) {
            if (!scan_range(page_offset, page_offset + page_size)) {
                return false;
            }
        }
    } else if (!scan_range(0, obj_data_size)) {
        return false;
    }

    if (!found) {
        return false;
    }
    recovered = candidate;
    return true;
}

} // namespace cad
