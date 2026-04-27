#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_diagnostics.h"
#include "cad/parser/dwg_header_vars.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace cad {

namespace diag = detail::diagnostics;

// ============================================================
// SectionStringReader implementation
// ============================================================

std::string SectionStringReader::read_tu()
{
    uint16_t length = read_bs();
    if (m_error || length == 0) {
        return {};
    }
    if (length > 32768) {
        m_error = true;
        return {};
    }

    std::string result;
    result.reserve(length);
    for (uint32_t i = 0; i < length && !m_error; ++i) {
        const uint8_t lo = read_raw_char();
        const uint8_t hi = read_raw_char();
        const uint16_t ch = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        append_utf8(result, ch);
    }
    return result;
}

uint8_t SectionStringReader::read_raw_char()
{
    if (!m_data || m_bit_pos + 8 > m_size * 8) {
        m_error = true;
        return 0;
    }
    const size_t byte_idx = m_bit_pos >> 3;
    const size_t bit_idx = m_bit_pos & 7;
    uint8_t result = 0;
    if (bit_idx == 0) {
        result = m_data[byte_idx];
    } else {
        result = static_cast<uint8_t>(m_data[byte_idx] << bit_idx);
        if (byte_idx + 1 < m_size) {
            result |= m_data[byte_idx + 1] >> (8 - bit_idx);
        }
    }
    m_bit_pos += 8;
    return result;
}

uint16_t SectionStringReader::read_rs()
{
    const uint8_t lo = read_raw_char();
    const uint8_t hi = read_raw_char();
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

uint16_t SectionStringReader::read_bs()
{
    if (!m_data || m_bit_pos + 2 > m_size * 8) {
        m_error = true;
        return 0;
    }
    uint8_t code = 0;
    for (int i = 0; i < 2; ++i) {
        const size_t byte_idx = m_bit_pos >> 3;
        const size_t bit_idx = 7 - (m_bit_pos & 7);
        code = static_cast<uint8_t>((code << 1) | ((m_data[byte_idx] >> bit_idx) & 1));
        ++m_bit_pos;
    }
    switch (code) {
        case 0: return read_rs();
        case 1: return read_raw_char();
        case 2: return 0;
        case 3: return 256;
        default: return 0;
    }
}

void SectionStringReader::append_utf8(std::string& out, uint16_t ch)
{
    if (ch < 0x80) {
        out.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
}

// ============================================================
// parse_header_variables — read Section 0 (HEADER)
// ============================================================

Result DwgParser::parse_header_variables(EntitySink& scene)
{
    if (m_sections.header_vars.empty()) {
        return Result::success();
    }

    scene.drawing_info().dwg_header_vars_bytes = m_sections.header_vars.size();
    if (scene.drawing_info().acad_version.empty()) {
        scene.drawing_info().acad_version = version_family_name(m_version);
    }

    const auto strings = diag::extract_printable_strings(
        m_sections.header_vars.data(), m_sections.header_vars.size(), 12);
    const auto points = diag::extract_plausible_raw_points_with_offsets(
        m_sections.header_vars.data(), m_sections.header_vars.size(), 10);

    std::string message = "DWG Header Variables section was decoded (";
    message += std::to_string(m_sections.header_vars.size());
    message += " bytes), but the version-family ordered variable reader is incomplete.";
    if (!points.empty()) {
        message += " Raw double-pair probes: ";
        const size_t n = std::min<size_t>(points.size(), 4);
        for (size_t i = 0; i < n; ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s0x%zx=(%.3f,%.3f)",
                          i == 0 ? "" : ";",
                          points[i].offset,
                          points[i].x,
                          points[i].y);
            message += buf;
        }
        message += ".";
    }
    if (!strings.empty()) {
        message += " Text probes: ";
        const size_t n = std::min<size_t>(strings.size(), 4);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) message += "; ";
            std::string s = strings[i];
            if (s.size() > 32) {
                s = s.substr(0, 29) + "...";
            }
            message += "'";
            message += s;
            message += "'";
        }
        message += ".";
    }
    scene.add_diagnostic({
        "dwg_header_vars_reader_incomplete",
        "Encoding gap",
        message,
        static_cast<int32_t>(std::min<size_t>(m_sections.header_vars.size(), 2147483647u)),
        version_family_name(m_version),
        "HEADER",
    });

    (void)points;
    (void)strings;
    return Result::success();
}

// ============================================================
// parse_classes — read Section 1 (class definitions)
// ============================================================

Result DwgParser::parse_classes(EntitySink& scene)
{
    if (m_sections.classes.empty()) {
        return Result::success();
    }

    const uint8_t* data = m_sections.classes.data();
    size_t data_size = m_sections.classes.size();
    bool skipped_class_sentinel = false;
    if (data_size >= 16 && data[0] == 0x8D && data[1] == 0xA1 &&
        data[2] == 0xC4 && data[3] == 0xB8) {
        data += 16;
        data_size -= 16;
        skipped_class_sentinel = true;
    }

    DwgBitReader reader(data, data_size);
    reader.set_r2007_plus(m_version >= DwgVersion::R2007);

    size_t class_data_limit = data_size * 8;
    uint32_t max_num = 0;
    size_t class_string_start = 0;
    if (m_version >= DwgVersion::R2004) {
        uint32_t data_area_size = reader.read_rl();
        (void)data_area_size;
        uint32_t hsize = 0;
        uint32_t bitsize = 0;
        if (m_version >= DwgVersion::R2007) {
            hsize = reader.read_rl();
            bitsize = reader.read_rl();
        }
        max_num = reader.read_bs();
        (void)reader.read_raw_char();
        (void)reader.read_raw_char();
        (void)reader.read_b();
        if (!reader.has_error() && bitsize > 0 && bitsize <= data_size * 8) {
            reader.set_bit_limit(bitsize);
            auto looks_like_class_string_start = [&](size_t start) -> bool {
                SectionStringReader probe(data, data_size, start);
                const std::string first = probe.read_tu();
                const std::string second = probe.read_tu();
                const std::string third = probe.read_tu();
                return !probe.has_error() &&
                       first.find("ObjectDBX") != std::string::npos &&
                       second.find("AcDb") == 0 &&
                       !third.empty();
            };
            auto find_class_string_stream = [&]() -> size_t {
                const size_t total_bits = data_size * 8;
                const size_t scan_start = total_bits > 512 ? total_bits - 512 : 17;
                for (size_t end = total_bits; end > scan_start; --end) {
                    if (end < 17) break;
                    DwgBitReader sr(data, data_size);
                    sr.set_bit_offset(end - 17);
                    uint32_t len = sr.read_rs();
                    if (sr.has_error() || len == 0) continue;
                    if (len & 0x8000u) {
                        if (end < 49) continue;
                        DwgBitReader ext(data, data_size);
                        ext.set_bit_offset(end - 49);
                        uint16_t hi = ext.read_rs();
                        if (ext.has_error()) continue;
                        len = ((len & 0x7FFFu) << 15) | (hi & 0x7FFFu);
                    }
                    if (len == 0 || len >= end) continue;
                    const size_t bases[] = {17, 33};
                    for (size_t base : bases) {
                        if (end < base + len) continue;
                        const size_t raw_start = end - base - len;
                        std::array<size_t, 2> candidates = {
                            raw_start,
                            (skipped_class_sentinel && raw_start >= 128) ? raw_start - 128 : raw_start,
                        };
                        for (size_t start : candidates) {
                            if (start <= reader.bit_offset() || start >= bitsize) continue;

                            if (looks_like_class_string_start(start)) {
                                return start;
                            }
                        }
                    }
                }
                const size_t probe_limit = std::min<size_t>(bitsize, 32768);
                for (size_t start = reader.bit_offset() + 1; start < probe_limit; ++start) {
                    if (looks_like_class_string_start(start)) {
                        return start;
                    }
                }
                return size_t{0};
            };
            class_string_start = find_class_string_stream();

            // Fallback: if scanner failed, try byte-aligned positions after
            // the binary class data (bitsize boundary). The string stream
            // typically starts right after the binary records.
            if (class_string_start == 0) {
                auto try_strings_at = [&](size_t pos) -> bool {
                    if (pos >= data_size * 8) return false;
                    SectionStringReader probe(data, data_size, pos);
                    const std::string first = probe.read_tu();
                    const std::string second = probe.read_tu();
                    return !probe.has_error() && !first.empty() && !second.empty();
                };
                for (size_t base = bitsize; base < bitsize + 128 && base < data_size * 8; base += 8) {
                    if (try_strings_at(base)) {
                        class_string_start = base;
                        break;
                    }
                }
            }

            if (class_string_start != 0) {
                class_data_limit = class_string_start;
            }
        }
    } else {
        max_num = reader.read_rl();
    }

    SectionStringReader class_strings(data, data_size, class_string_start);
    while (!reader.has_error() && reader.bit_offset() + 16 < class_data_limit) {
        size_t entry_start = reader.bit_offset();

        uint16_t class_type = reader.read_bs();
        if (reader.has_error() || class_type == 0 || class_type > 4096) break;
        if (class_type > max_num + 4096) break;

        std::string dxf_name;
        std::string cpp_name;
        std::string app_name;
        uint16_t proxy_flags = 0;
        bool is_entity = false;

        if (m_version < DwgVersion::R2007) {
            proxy_flags = reader.read_bs();
            app_name = reader.read_tv();
            cpp_name = reader.read_tv();
            dxf_name = reader.read_tv();
            is_entity = reader.read_b();
            (void)reader.read_bs();   // item_class_id
            (void)reader.read_bl();   // num_instances
            (void)reader.read_bs();   // dwg_version
            (void)reader.read_bs();   // maint_version
            (void)reader.read_bl();   // unknown_1
            (void)reader.read_bl();   // unknown_2
        } else {
            proxy_flags = reader.read_bs();
            if (class_string_start != 0) {
                app_name = class_strings.read_tu();
                cpp_name = class_strings.read_tu();
                dxf_name = class_strings.read_tu();
            }
            // When string stream is unavailable, skip TU reads but
            // main reader position is unaffected (strings are separate stream).
            uint16_t class_id = reader.read_bs();
            is_entity = (class_id == 0x1F2);
            (void)reader.read_bl();  // num_instances
            (void)reader.read_raw_char();  // dwg_version
            (void)reader.read_raw_char();  // maintenance_version
            (void)reader.read_raw_char();
            (void)reader.read_raw_char();
        }

        if (reader.has_error()) break;
        if (class_string_start != 0 && class_strings.has_error()) break;

        m_sections.class_map[class_type] = {dxf_name, is_entity};
        (void)entry_start;
        (void)proxy_flags;
    }

    const size_t parsed_class_records = m_sections.class_map.size();
    dwg_debug_log("[DWG] classes: %zu entries parsed\n", parsed_class_records);
    if (m_version >= DwgVersion::R2004 && class_string_start != 0 &&
        m_sections.class_map.size() < 3) {
        SectionStringReader fallback(data, data_size, class_string_start);
        uint32_t class_number = 500;
        while (!fallback.has_error() && class_number <= max_num) {
            (void)fallback.read_tu();  // application name
            (void)fallback.read_tu();  // C++ class name
            std::string dxf_name = fallback.read_tu();
            if (fallback.has_error() || dxf_name.empty()) break;
            m_sections.class_map.emplace(class_number, std::make_pair(dxf_name, false));
            ++class_number;
        }
        dwg_debug_log("[DWG] classes fallback map: %zu entries\n", m_sections.class_map.size());
    }
    if (!m_sections.classes.empty() && m_sections.class_map.empty()) {
        scene.add_diagnostic({
            "dwg_classes_unparsed",
            "Parse gap",
            "DWG Classes Section is present but class records were not decoded; custom object names such as LAYOUT may be unavailable.",
            1,
        });
    } else if (m_version >= DwgVersion::R2004 && !m_sections.classes.empty() &&
               parsed_class_records < 10 && m_sections.class_map.size() > parsed_class_records) {
        scene.add_diagnostic({
            "dwg_classes_partial_fallback",
            "Object framing gap",
            "DWG Classes Section records were only partially decoded; parser used the class string stream as a bounded fallback map. Custom object names are available, but class ids/entity flags may be provisional.",
            static_cast<int32_t>(m_sections.class_map.size() - parsed_class_records),
            version_family_name(m_version),
            "Classes Section",
            0,
            "AcDb:Classes",
        });
    }
    return Result::success();
}

} // namespace cad
