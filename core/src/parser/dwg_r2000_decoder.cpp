// R2000/AC1015 flat section reader.
// R2000 uses sentinel-delimited sections with no encryption or compression.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_entity_sink.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cad {

namespace {

// R2000 section sentinel markers (16 bytes each)
constexpr uint8_t kSectionSentinelBegin[] = {
    0xCF, 0x7B, 0x1F, 0x23, 0xFD, 0xDE, 0x38, 0xA9,
    0x5F, 0x7C, 0x68, 0xB8, 0x4E, 0x6D, 0x33, 0x57,
};

constexpr uint8_t kSectionSentinelEnd[] = {
    0x30, 0x84, 0xE0, 0xDC, 0x02, 0x21, 0xC7, 0x56,
    0xA0, 0x83, 0x97, 0x47, 0xB1, 0x92, 0xCC, 0xA8,
};

// Scan for a 16-byte pattern in data, starting from offset.
// Returns offset of first byte of match, or (size_t)-1 if not found.
size_t scan_sentinel(const uint8_t* data, size_t size,
                     const uint8_t* sentinel, size_t start = 0)
{
    if (size < 16 || start + 16 > size) return static_cast<size_t>(-1);
    for (size_t i = start; i <= size - 16; ++i) {
        if (std::memcmp(data + i, sentinel, 16) == 0) return i;
    }
    return static_cast<size_t>(-1);
}

} // anonymous namespace

Result DwgParser::read_r2000_sections(const uint8_t* data, size_t size,
                                      EntitySink& scene)
{
    if (!data || size < 0x100) {
        return Result::error(ErrorCode::InvalidFormat,
                             "DWG data too small for R2000 sections");
    }

    // R2000 section locator table: scan for section-begin sentinel.
    // The sentinel marks the start of each section record.
    // Format: sentinel(16) + section_number(4 LE) + size(4 LE) + page_count(4 LE)
    // After the record header, the section data follows directly.

    size_t pos = 0;
    int sections_found = 0;

    while (pos < size) {
        size_t sent_pos = scan_sentinel(data, size, kSectionSentinelBegin, pos);
        if (sent_pos == static_cast<size_t>(-1)) break;

        size_t hdr_start = sent_pos + 16;
        if (hdr_start + 12 > size) break;

        uint32_t section_num = DwgParser::read_le32(data, hdr_start);
        uint32_t section_size = DwgParser::read_le32(data, hdr_start + 4);
        uint32_t page_count = DwgParser::read_le32(data, hdr_start + 8);

        // Skip the record header (12 bytes) + page records (8 bytes each)
        size_t data_start = hdr_start + 12 + static_cast<size_t>(page_count) * 8;

        // Find the end sentinel
        size_t end_pos = scan_sentinel(data, size, kSectionSentinelEnd, data_start);
        size_t data_end;
        if (end_pos != static_cast<size_t>(-1)) {
            data_end = end_pos;
        } else {
            // No end sentinel — use section_size as fallback
            data_end = std::min(data_start + section_size, size);
        }

        if (data_start > size || data_end > size || data_start >= data_end) {
            pos = data_start;
            continue;
        }

        size_t actual_size = data_end - data_start;
        const uint8_t* section_data = data + data_start;

        // R2000 section numbers:
        //   0 = Header variables
        //   1 = Class section
        //   2 = Object map (handle -> offset)
        //   3+ = Object data pages
        switch (section_num) {
        case 0: // Header variables
            m_sections.header_vars.assign(section_data, section_data + actual_size);
            dwg_debug_log("[R2000] Section 0 (header vars): %zu bytes\n", actual_size);
            break;

        case 1: // Classes
            m_sections.classes.assign(section_data, section_data + actual_size);
            dwg_debug_log("[R2000] Section 1 (classes): %zu bytes\n", actual_size);
            break;

        case 2: // Object map
            m_sections.object_map.assign(section_data, section_data + actual_size);
            dwg_debug_log("[R2000] Section 2 (object map): %zu bytes\n", actual_size);
            break;

        default: // Object data pages (section_num >= 3)
            if (section_num >= 3) {
                if (m_sections.object_data.empty()) {
                    m_sections.object_data_file_offset = data_start;
                }
                size_t page_offset = m_sections.object_data.size();
                m_sections.object_data.insert(m_sections.object_data.end(),
                                              section_data, section_data + actual_size);
                m_sections.object_pages.emplace_back(page_offset, actual_size);
                dwg_debug_log("[R2000] Section %u (object data): %zu bytes at page_offset=%zu\n",
                              section_num, actual_size, page_offset);
            }
            break;
        }

        sections_found++;
        pos = (end_pos != static_cast<size_t>(-1)) ? end_pos + 16 : data_end;
    }

    if (sections_found == 0) {
        return Result::error(ErrorCode::InvalidFormat,
                             "R2000: no sections found (sentinel scan failed)");
    }

    // Build handle_map from object_map section.
    // R2000 object map format: same as R2004 — modular char pairs
    // (handle_delta, offset_delta) with per-section accumulators.
    if (!m_sections.object_map.empty()) {
        // Delegate to the shared object map parser
        Result r = parse_object_map(data, size);
        if (!r) return r;
    }

    dwg_debug_log("[R2000] Found %d sections, object_data=%zu bytes, handle_map=%zu entries\n",
                  sections_found, m_sections.object_data.size(), m_sections.handle_map.size());

    scene.add_diagnostic({
        "dwg_r2000_sections_decoded",
        "Version support",
        "R2000/AC1015 flat sections decoded: header vars, classes, object map, and object data.",
        sections_found,
        version_family_name(m_version),
        "File container",
    });

    return Result::success();
}

} // namespace cad
