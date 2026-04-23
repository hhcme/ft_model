#include "cad/parser/dwg_r2007_codec.h"
#include "cad/parser/dwg_parser.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace cad {

namespace {

uint64_t read_u64_le(const uint8_t* data)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

int64_t read_i64_le(const uint8_t* data)
{
    return static_cast<int64_t>(read_u64_le(data));
}

} // namespace

namespace detail {
namespace r2007 {

// ============================================================
// R21 literal copy helpers — match libredwg copy_bytes_2/copy_bytes_3/copy_16
// These perform byte-reversal for 2-byte and 3-byte copies, and 8-byte-half
// swap for 16-byte copies, exactly as libredwg's decode_r2007.c does.
// ============================================================

void r21_copy_1(uint8_t*& dst, const uint8_t* src, int offset)
{
    *dst++ = *(src + offset);
}
void r21_copy_2(uint8_t*& dst, const uint8_t* src, int offset)
{
    dst[0] = *(src + offset + 1);
    dst[1] = *(src + offset);
    dst += 2;
}
void r21_copy_3(uint8_t*& dst, const uint8_t* src, int offset)
{
    dst[0] = *(src + offset + 2);
    dst[1] = *(src + offset + 1);
    dst[2] = *(src + offset);
    dst += 3;
}
void r21_copy_4(uint8_t*& dst, const uint8_t* src, int offset)
{
    std::memcpy(dst, src + offset, 4);
    dst += 4;
}
void r21_copy_8(uint8_t*& dst, const uint8_t* src, int offset)
{
    std::memcpy(dst, src + offset, 8);
    dst += 8;
}
void r21_copy_16(uint8_t*& dst, const uint8_t* src, int offset)
{
    std::memcpy(dst, src + offset + 8, 8);
    std::memcpy(dst + 8, src + offset, 8);
    dst += 16;
}

// Copy literal bytes from src to dst, matching libredwg copy_compressed_bytes.
// length must be 1..32.  dst is advanced by length.
void r21_copy_compressed_bytes(uint8_t*& dst, const uint8_t* src, int length)
{
    while (length >= 32) {
        r21_copy_16(dst, src, 16);
        r21_copy_16(dst, src, 0);
        src += 32;
        length -= 32;
    }
    switch (length) {
        case 0: break;
        case 1: r21_copy_1(dst, src, 0); break;
        case 2: r21_copy_2(dst, src, 0); break;
        case 3: r21_copy_3(dst, src, 0); break;
        case 4: r21_copy_4(dst, src, 0); break;
        case 5: r21_copy_1(dst, src, 4); r21_copy_4(dst, src, 0); break;
        case 6: r21_copy_1(dst, src, 5); r21_copy_4(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 7: r21_copy_2(dst, src, 5); r21_copy_4(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 8: r21_copy_8(dst, src, 0); break;
        case 9: r21_copy_1(dst, src, 8); r21_copy_8(dst, src, 0); break;
        case 10: r21_copy_1(dst, src, 9); r21_copy_8(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 11: r21_copy_2(dst, src, 9); r21_copy_8(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 12: r21_copy_4(dst, src, 8); r21_copy_8(dst, src, 0); break;
        case 13: r21_copy_1(dst, src, 12); r21_copy_4(dst, src, 8); r21_copy_8(dst, src, 0); break;
        case 14: r21_copy_1(dst, src, 13); r21_copy_4(dst, src, 9); r21_copy_8(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 15: r21_copy_2(dst, src, 13); r21_copy_4(dst, src, 9); r21_copy_8(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 16: r21_copy_16(dst, src, 0); break;
        case 17: r21_copy_8(dst, src, 9); r21_copy_1(dst, src, 8); r21_copy_8(dst, src, 0); break;
        case 18: r21_copy_1(dst, src, 17); r21_copy_16(dst, src, 1); r21_copy_1(dst, src, 0); break;
        case 19: r21_copy_3(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 20: r21_copy_4(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 21: r21_copy_1(dst, src, 20); r21_copy_4(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 22: r21_copy_2(dst, src, 20); r21_copy_4(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 23: r21_copy_3(dst, src, 20); r21_copy_4(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 24: r21_copy_8(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 25: r21_copy_8(dst, src, 17); r21_copy_1(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 26: r21_copy_1(dst, src, 25); r21_copy_8(dst, src, 17); r21_copy_1(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 27: r21_copy_2(dst, src, 25); r21_copy_8(dst, src, 17); r21_copy_1(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 28: r21_copy_4(dst, src, 24); r21_copy_8(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 29: r21_copy_1(dst, src, 28); r21_copy_4(dst, src, 24); r21_copy_8(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 30: r21_copy_2(dst, src, 28); r21_copy_4(dst, src, 24); r21_copy_8(dst, src, 16); r21_copy_16(dst, src, 0); break;
        case 31: r21_copy_1(dst, src, 30); r21_copy_4(dst, src, 26); r21_copy_8(dst, src, 18); r21_copy_16(dst, src, 2); r21_copy_2(dst, src, 0); break;
        default: break; // length >= 32 handled by while loop
    }
}

std::vector<uint8_t> r21_decompress(const uint8_t* src, size_t src_size, size_t out_size)
{
    std::vector<uint8_t> out;
    if (!src || src_size == 0 || out_size == 0) return out;
    out.reserve(out_size);

    size_t i = 0;
    auto read = [&]() -> uint8_t {
        return i < src_size ? src[i++] : 0;
    };
    auto read_literal_length = [&](uint8_t op) {
        size_t length = static_cast<size_t>(op) + 8;
        if (length == 0x17) {
            uint32_t n = read();
            length += n;
            if (n == 0xff) {
                do {
                    if (i + 1 >= src_size) break;
                    n = static_cast<uint32_t>(src[i]) |
                        (static_cast<uint32_t>(src[i + 1]) << 8);
                    i += 2;
                    length += n;
                } while (n == 0xffff);
            }
        }
        return length;
    };
    auto copy_literal = [&](size_t length) {
        while (length >= 32 && i + 32 <= src_size && out.size() + 32 <= out_size) {
            const size_t old_size = out.size();
            out.resize(old_size + 32);
            uint8_t* dst_ptr = out.data() + old_size;
            r21_copy_compressed_bytes(dst_ptr, src + i, 32);
            i += 32;
            length -= 32;
        }
        if (length > 0 && length < 32 && i + length <= src_size && out.size() + length <= out_size) {
            const size_t old_size = out.size();
            out.resize(old_size + length);
            uint8_t* dst_ptr = out.data() + old_size;
            r21_copy_compressed_bytes(dst_ptr, src + i, static_cast<int>(length));
            i += length;
        } else if (length > 0) {
            i = std::min(src_size, i + length);
        }
    };
    auto read_instructions = [&](uint8_t& op, uint32_t& source_offset, uint32_t& length) {
        switch (op >> 4) {
            case 0:
                length = (op & 0x0f) + 0x13;
                source_offset = read();
                op = read();
                length = ((op >> 3) & 0x10) + length;
                source_offset = ((op & 0x78) << 5) + 1 + source_offset;
                break;
            case 1:
                length = (op & 0x0f) + 3;
                source_offset = read();
                op = read();
                source_offset = ((op & 0xf8) << 5) + 1 + source_offset;
                break;
            case 2:
                source_offset = read();
                source_offset |= static_cast<uint32_t>(read()) << 8;
                length = op & 7;
                if ((op & 8) == 0) {
                    op = read();
                    length = (op & 0xf8) + length;
                } else {
                    source_offset++;
                    length = (static_cast<uint32_t>(read()) << 3) + length;
                    op = read();
                    length = (((op & 0xf8) << 8) + length) + 0x100;
                }
                break;
            default:
                length = op >> 4;
                source_offset = op & 15;
                op = read();
                source_offset = (((op & 0xf8) << 1) + source_offset) + 1;
                break;
        }
    };
    auto copy_backref = [&](uint32_t source_offset, uint32_t length) {
        for (uint32_t n = 0; n < length && out.size() < out_size; ++n) {
            if (source_offset == 0 || source_offset > out.size()) {
                out.push_back(0);
            } else {
                out.push_back(out[out.size() - source_offset]);
            }
        }
    };

    uint8_t op = read();
    size_t literal_length = 0;
    if ((op >> 4) == 2 && i + 2 < src_size) {
        i += 2;
        literal_length = read() & 7;
    }

    while (i < src_size && out.size() < out_size) {
        if (literal_length == 0) {
            literal_length = read_literal_length(op);
        }
        copy_literal(literal_length);
        literal_length = 0;
        if (i >= src_size || out.size() >= out_size) break;

        op = read();
        uint32_t source_offset = 0;
        uint32_t length = 0;
        read_instructions(op, source_offset, length);
        while (true) {
            copy_backref(source_offset, length);
            if (out.size() >= out_size) break;
            literal_length = op & 7;
            if (literal_length != 0 || i >= src_size) break;
            op = read();
            if ((op >> 4) == 0) break;
            if ((op >> 4) == 15) op &= 15;
            read_instructions(op, source_offset, length);
        }
    }

    if (out.size() < out_size) out.resize(out_size, 0);
    return out;
}

} // namespace r2007
} // namespace detail

// Page decode/validation functions are in dwg_r2007_page_decode.cpp

namespace detail {
namespace r2007 {

// (functions extracted to dwg_r2007_page_decode.cpp)

} // namespace r2007
} // namespace detail

// Page decode/validation functions extracted to dwg_r2007_page_decode.cpp.

// Import R2007 codec functions for the container reader.
namespace r2007 = detail::r2007;

// ============================================================
// read_r2007_container — AC1021/R21 container reader
// ============================================================

Result DwgParser::read_r2007_container(const uint8_t* data,
                                       size_t size,
                                       EntitySink& scene)
{
    if (!data || size < 0x80 + 0x400) {
        return Result::error(ErrorCode::InvalidFormat,
                             "DWG data too small for R2007 container");
    }

    auto add_gap = [&](const std::string& code,
                       const std::string& category,
                       const std::string& message,
                       int32_t count = 1,
                       const std::string& object_family = "File container") {
        scene.add_diagnostic({
            code,
            category,
            message,
            count,
            version_family_name(m_version),
            object_family,
            0,
            "R2007 container",
        });
    };

    const auto header_system_bytes =
        r2007::r2007_take_system_data_no_correction(data + 0x80, 0x3d8, 3, 239);
    if (header_system_bytes.size() < 0x20) {
        add_gap("dwg_r2007_header_decode_failed",
                "Version gap",
                "R2007 file header systematic bytes could not be recovered from the interleaved header page.");
        return Result::success();
    }

    const int32_t compressed_header_size =
        static_cast<int32_t>(read_le32(header_system_bytes.data(), 0x18));
    const size_t header_payload_size = (compressed_header_size < 0)
        ? static_cast<size_t>(-compressed_header_size)
        : static_cast<size_t>(compressed_header_size);
    if (header_payload_size == 0 || 0x20 + header_payload_size > header_system_bytes.size()) {
        add_gap("dwg_r2007_header_decode_failed",
                "Version gap",
                "R2007 file header payload length is outside the decoded header page.");
        return Result::success();
    }

    std::vector<uint8_t> file_header = (compressed_header_size > 0)
        ? r2007::r21_decompress(header_system_bytes.data() + 0x20, header_payload_size, 0x110)
        : std::vector<uint8_t>(header_system_bytes.begin() + 0x20,
                               header_system_bytes.begin() + 0x20 +
                                   static_cast<std::ptrdiff_t>(header_payload_size));
    if (file_header.size() < 0x110) {
        add_gap("dwg_r2007_header_decode_failed",
                "Version gap",
                "R2007 file header R21 decompression did not produce the expected 0x110 bytes.");
        return Result::success();
    }

    r2007::HeaderData header;
    header.header_size = read_u64_le(file_header.data() + 0x00);
    header.file_size = read_u64_le(file_header.data() + 0x08);
    header.pages_map_correction_factor = read_u64_le(file_header.data() + 0x18);
    header.pages_map_offset = read_u64_le(file_header.data() + 0x38);
    header.pages_map_id = read_u64_le(file_header.data() + 0x40);
    header.pages_map_size_compressed = read_u64_le(file_header.data() + 0x50);
    header.pages_map_size_uncompressed = read_u64_le(file_header.data() + 0x58);
    header.pages_amount = read_u64_le(file_header.data() + 0x60);
    header.pages_max_id = read_u64_le(file_header.data() + 0x68);
    header.sections_amount = read_u64_le(file_header.data() + 0xA0);
    header.sections_map_size_compressed = read_u64_le(file_header.data() + 0xB0);
    header.sections_map_id = read_u64_le(file_header.data() + 0xC0);
    header.sections_map_size_uncompressed = read_u64_le(file_header.data() + 0xC8);
    header.sections_map_correction_factor = read_u64_le(file_header.data() + 0xD8);
    header.stream_version = read_u64_le(file_header.data() + 0xE8);

    dwg_debug_log("[DWG] R2007 header: file=%llu pages_map_off=0x%llx "
                  "pages_map_id=%llu pages=%llu max_id=%llu sections=%llu "
                  "section_map_id=%llu pm_comp=%llu pm_un=%llu pm_corr=%llu "
                  "sm_comp=%llu sm_un=%llu sm_corr=%llu\n",
                  static_cast<unsigned long long>(header.file_size),
                  static_cast<unsigned long long>(header.pages_map_offset + 0x480),
                  static_cast<unsigned long long>(header.pages_map_id),
                  static_cast<unsigned long long>(header.pages_amount),
                  static_cast<unsigned long long>(header.pages_max_id),
                  static_cast<unsigned long long>(header.sections_amount),
                  static_cast<unsigned long long>(header.sections_map_id),
                  static_cast<unsigned long long>(header.pages_map_size_compressed),
                  static_cast<unsigned long long>(header.pages_map_size_uncompressed),
                  static_cast<unsigned long long>(header.pages_map_correction_factor),
                  static_cast<unsigned long long>(header.sections_map_size_compressed),
                  static_cast<unsigned long long>(header.sections_map_size_uncompressed),
                  static_cast<unsigned long long>(header.sections_map_correction_factor));

    if (header.header_size != 0x70 || header.pages_map_size_compressed == 0 ||
        header.pages_map_size_uncompressed == 0 || header.sections_map_size_compressed == 0 ||
        header.sections_map_size_uncompressed == 0) {
        add_gap("dwg_r2007_header_decode_failed",
                "Version gap",
                "R2007 file header decoded, but required page/section map fields were missing or implausible.");
        return Result::success();
    }

    const uint64_t pages_map_file_offset = header.pages_map_offset + 0x480;
    const size_t pages_map_page_size =
        r2007::r2007_system_page_size(static_cast<size_t>(header.pages_map_size_uncompressed));
    if (pages_map_file_offset + pages_map_page_size > size) {
        add_gap("dwg_r2007_page_map_out_of_bounds",
                "Version gap",
                "R2007 page map points outside the file stream.");
        return Result::success();
    }

    const auto pages_map_data = r2007::r2007_decode_system_page_no_correction(
        data + static_cast<size_t>(pages_map_file_offset),
        size - static_cast<size_t>(pages_map_file_offset),
        static_cast<size_t>(header.pages_map_size_compressed),
        static_cast<size_t>(header.pages_map_size_uncompressed),
        static_cast<size_t>(header.pages_map_correction_factor));
    dwg_debug_log("[DWG] R2007 page map decoded bytes=%zu\n", pages_map_data.size());

    if (pages_map_data.empty()) {
        add_gap("dwg_r2007_page_map_decode_failed",
                "Version gap",
                "R2007 system page decode for the page map did not produce usable data.");
        return Result::success();
    }

    std::unordered_map<uint64_t, r2007::PageRecord> pages;
    uint64_t running_offset = 0;
    for (size_t off = 0; off + 16 <= pages_map_data.size(); off += 16) {
        const int64_t page_size = read_i64_le(pages_map_data.data() + off);
        const int64_t page_id = read_i64_le(pages_map_data.data() + off + 8);
        if (page_size <= 0 || page_id == 0) {
            running_offset += page_size > 0 ? static_cast<uint64_t>(page_size) : 0;
            continue;
        }
        pages[static_cast<uint64_t>(std::llabs(page_id))] = {
            page_id,
            static_cast<uint64_t>(page_size),
            running_offset,
        };
        running_offset += static_cast<uint64_t>(page_size);
        if (pages.size() <= 32) {
            dwg_debug_log("[DWG] R2007 page id=%lld size=%lld offset=0x%llx\n",
                          static_cast<long long>(page_id),
                          static_cast<long long>(page_size),
                          static_cast<unsigned long long>(running_offset - static_cast<uint64_t>(page_size)));
        }
    }

    auto section_map_page = pages.find(header.sections_map_id);
    if (section_map_page == pages.end()) {
        add_gap("dwg_r2007_section_map_missing",
                "Version gap",
                "R2007 page map decoded, but the section map page id was not present.");
        return Result::success();
    }
    const size_t sections_map_page_size =
        r2007::r2007_system_page_size(static_cast<size_t>(header.sections_map_size_uncompressed));
    dwg_debug_log("[DWG] R2007 section map page record id=%lld size=%llu rel_off=0x%llx expected_sys_page=%zu\n",
                  static_cast<long long>(section_map_page->second.id),
                  static_cast<unsigned long long>(section_map_page->second.size),
                  static_cast<unsigned long long>(section_map_page->second.offset),
                  sections_map_page_size);
    const uint64_t section_map_file_offset = section_map_page->second.offset + 0x480;
    if (section_map_file_offset + sections_map_page_size > size) {
        add_gap("dwg_r2007_section_map_out_of_bounds",
                "Version gap",
                "R2007 section map page points outside the file stream.");
        return Result::success();
    }

    const auto section_map_data = r2007::r2007_decode_system_page_no_correction(
        data + static_cast<size_t>(section_map_file_offset),
        size - static_cast<size_t>(section_map_file_offset),
        static_cast<size_t>(header.sections_map_size_compressed),
        static_cast<size_t>(header.sections_map_size_uncompressed),
        static_cast<size_t>(header.sections_map_correction_factor));
    dwg_debug_log("[DWG] R2007 section map page offset=0x%llx decoded bytes=%zu first=%02x %02x %02x %02x\n",
                  static_cast<unsigned long long>(section_map_file_offset),
                  section_map_data.size(),
                  section_map_data.empty() ? 0 : section_map_data[0],
                  section_map_data.size() > 1 ? section_map_data[1] : 0,
                  section_map_data.size() > 2 ? section_map_data[2] : 0,
                  section_map_data.size() > 3 ? section_map_data[3] : 0);

    std::vector<r2007::SectionRecord> sections;
    for (size_t off = 0; off + 64 <= section_map_data.size();) {
        r2007::SectionRecord section;
        section.data_size = read_u64_le(section_map_data.data() + off + 0x00);
        section.max_size = read_u64_le(section_map_data.data() + off + 0x08);
        section.encryption = read_u64_le(section_map_data.data() + off + 0x10);
        section.hash_code = read_u64_le(section_map_data.data() + off + 0x18);
        const uint64_t name_length = read_u64_le(section_map_data.data() + off + 0x20);
        section.encoding = read_u64_le(section_map_data.data() + off + 0x30);
        section.num_pages = read_u64_le(section_map_data.data() + off + 0x38);
        dwg_debug_log("[DWG] R2007 section candidate off=%zu data=%llu max=%llu name_len=%llu enc=%llu pages=%llu\n",
                      off,
                      static_cast<unsigned long long>(section.data_size),
                      static_cast<unsigned long long>(section.max_size),
                      static_cast<unsigned long long>(name_length),
                      static_cast<unsigned long long>(section.encoding),
                      static_cast<unsigned long long>(section.num_pages));
        off += 0x40;
        if (name_length > 2048 || off + name_length > section_map_data.size()) {
            dwg_debug_log("[DWG] R2007 section map stop: invalid name length=%llu off=%zu size=%zu\n",
                          static_cast<unsigned long long>(name_length),
                          off,
                          section_map_data.size());
            break;
        }
        if (name_length > 0) {
            const size_t name_end = off + static_cast<size_t>(name_length);
            for (; off + 1 < name_end && off + 1 < section_map_data.size(); off += 2) {
                const uint16_t ch = static_cast<uint16_t>(section_map_data[off]) |
                                    (static_cast<uint16_t>(section_map_data[off + 1]) << 8);
                if (ch < 0x80) {
                    section.name.push_back(static_cast<char>(ch));
                }
            }
            off = name_end;
        }
        if (section.num_pages > 100000 ||
            off + section.num_pages * 56 > section_map_data.size()) {
            dwg_debug_log("[DWG] R2007 section map stop: invalid page count=%llu off=%zu size=%zu\n",
                          static_cast<unsigned long long>(section.num_pages),
                          off,
                          section_map_data.size());
            break;
        }
        for (uint64_t p = 0; p < section.num_pages; ++p) {
            r2007::SectionRecord::Page page;
            page.data_offset = read_u64_le(section_map_data.data() + off + 0x00);
            page.page_size = read_u64_le(section_map_data.data() + off + 0x08);
            page.page_id = read_i64_le(section_map_data.data() + off + 0x10);
            page.uncompressed_size = read_u64_le(section_map_data.data() + off + 0x18);
            page.compressed_size = read_u64_le(section_map_data.data() + off + 0x20);
            page.checksum = read_u64_le(section_map_data.data() + off + 0x28);
            page.crc = read_u64_le(section_map_data.data() + off + 0x30);
            section.pages.push_back(page);
            off += 56;
            if ((section.name.find("AcDb:") != std::string::npos ||
                 section.name.empty()) && p < 4) {
                dwg_debug_log("[DWG] R2007 section page name='%s' page=%llu id=%lld data_off=%llu page_size=%llu uncomp=%llu comp=%llu\n",
                              section.name.c_str(),
                              static_cast<unsigned long long>(p),
                              static_cast<long long>(page.page_id),
                              static_cast<unsigned long long>(page.data_offset),
                              static_cast<unsigned long long>(page.page_size),
                              static_cast<unsigned long long>(page.uncompressed_size),
                              static_cast<unsigned long long>(page.compressed_size));
            }
        }
        if (!section.name.empty()) {
            dwg_debug_log("[DWG] R2007 section '%s' size=%llu pages=%llu enc=%llu max=%llu\n",
                          section.name.c_str(),
                          static_cast<unsigned long long>(section.data_size),
                          static_cast<unsigned long long>(section.num_pages),
                          static_cast<unsigned long long>(section.encoding),
                          static_cast<unsigned long long>(section.max_size));
            sections.push_back(std::move(section));
        }
    }

    dwg_debug_log("[DWG] R2007 sections decoded: %zu\n", sections.size());
    if (sections.empty()) {
        add_gap("dwg_r2007_section_map_decode_failed",
                "Version gap",
                "R2007 section map decompressed, but no section descriptors were decoded.");
        return Result::success();
    }

    m_sections = DwgFileSections{};
    auto decode_section = [&](const r2007::SectionRecord& section) {
        std::vector<uint8_t> assembled;
        if (section.data_size > 0 && section.data_size < (1ull << 34)) {
            assembled.resize(static_cast<size_t>(section.data_size), 0);
        }
        for (const auto& page : section.pages) {
            const auto page_it = pages.find(static_cast<uint64_t>(std::llabs(page.page_id)));
            if (page_it == pages.end()) continue;
            const uint64_t file_offset = page_it->second.offset + 0x480;
            const size_t encoded_size = static_cast<size_t>(page_it->second.size);
            const size_t compressed_size = static_cast<size_t>(
                page.compressed_size ? page.compressed_size : encoded_size);
            if (file_offset + encoded_size > size || encoded_size == 0 || compressed_size == 0) continue;
            if (section.name.find("AcDb:Handles") != std::string::npos ||
                section.name.find("AcDb:Classes") != std::string::npos ||
                section.name.find("AcDb:Header") != std::string::npos) {
                dwg_debug_log("[DWG] R2007 decode section '%s' page_id=%lld file_off=0x%llx encoded=%zu comp=%zu uncomp=%llu encoding=%llu\n",
                              section.name.c_str(),
                              static_cast<long long>(page.page_id),
                              static_cast<unsigned long long>(file_offset),
                              encoded_size,
                              compressed_size,
                              static_cast<unsigned long long>(page.uncompressed_size),
                              static_cast<unsigned long long>(section.encoding));
            }
            std::vector<uint8_t> page_data;
            if (page.uncompressed_size > 0 &&
                (section.encoding == 1 || section.encoding == 4)) {
                // DEBUG: print deinterleaved data for Handles
                if (section.name.find("AcDb:Handles") != std::string::npos && dwg_debug_enabled()) {
                    const size_t dbg_block_count = encoded_size / 255;
                    auto dbg_deinterleaved = r2007::r2007_take_system_data_no_correction(
                        data + static_cast<size_t>(file_offset), encoded_size, dbg_block_count, 251);
                    dwg_debug_log("[DWG] R2007 Handles deinterleaved first 20: ");
                    for (size_t dbg_i = 0; dbg_i < 20 && dbg_i < dbg_deinterleaved.size(); ++dbg_i) {
                        dwg_debug_log("%02x ", dbg_deinterleaved[dbg_i]);
                    }
                    dwg_debug_log("\n");
                }
                page_data = r2007::r2007_decode_data_page_no_correction(
                    data + static_cast<size_t>(file_offset),
                    encoded_size,
                    compressed_size,
                    static_cast<size_t>(page.uncompressed_size),
                    section.encoding,
                    false);
                std::vector<uint8_t> skip_crc_page_data;
                std::vector<uint8_t> skip_crc9_page_data;
                if (section.encoding == 4 && compressed_size > 8 &&
                    compressed_size < static_cast<size_t>(page.uncompressed_size) &&
                    (section.name.find("AcDb:AcDbObjects") != std::string::npos ||
                     section.name.find("AcDb:Handles") != std::string::npos ||
                     section.name.find("AcDb:Classes") != std::string::npos ||
                     section.name.find("AcDb:Header") != std::string::npos)) {
                    skip_crc_page_data = r2007::r2007_decode_data_page_no_correction(
                        data + static_cast<size_t>(file_offset),
                        encoded_size,
                        compressed_size,
                        static_cast<size_t>(page.uncompressed_size),
                        section.encoding,
                        false,
                        8);
                    skip_crc9_page_data = r2007::r2007_decode_data_page_no_correction(
                        data + static_cast<size_t>(file_offset),
                        encoded_size,
                        compressed_size,
                        static_cast<size_t>(page.uncompressed_size),
                        section.encoding,
                        false,
                        9);
                    const bool normal_has_objects_marker =
                        page_data.size() >= 12 &&
                        page_data[8] == 0xCA && page_data[9] == 0x0D &&
                        page_data[10] == 0x00 && page_data[11] == 0x00;
                    const bool skip_has_objects_marker =
                        skip_crc_page_data.size() >= 4 &&
                        skip_crc_page_data[0] == 0xCA && skip_crc_page_data[1] == 0x0D &&
                        skip_crc_page_data[2] == 0x00 && skip_crc_page_data[3] == 0x00;
                    const bool skip9_has_objects_marker =
                        skip_crc9_page_data.size() >= 4 &&
                        skip_crc9_page_data[0] == 0xCA && skip_crc9_page_data[1] == 0x0D &&
                        skip_crc9_page_data[2] == 0x00 && skip_crc9_page_data[3] == 0x00;
                    const bool skip_has_classes_marker =
                        r2007::r2007_classes_page_plausible(skip_crc_page_data) ||
                        r2007::r2007_classes_page_has_split_initial_literal(skip_crc_page_data);
                    const bool normal_has_classes_marker =
                        r2007::r2007_classes_page_plausible(page_data) ||
                        r2007::r2007_classes_page_has_split_initial_literal(page_data);
                    if ((section.name.find("AcDb:AcDbObjects") != std::string::npos &&
                         normal_has_objects_marker && skip_has_objects_marker) ||
                        (section.name.find("AcDb:Classes") != std::string::npos &&
                         !normal_has_classes_marker && skip_has_classes_marker)) {
                        dwg_debug_log("[DWG] R2007 section '%s' page_id=%lld selected +8 compressed payload candidate\n",
                                      section.name.c_str(),
                                      static_cast<long long>(page.page_id));
                        page_data = std::move(skip_crc_page_data);
                    } else if (section.name.find("AcDb:AcDbObjects") != std::string::npos &&
                               normal_has_objects_marker && skip9_has_objects_marker) {
                        dwg_debug_log("[DWG] R2007 section '%s' page_id=%lld selected +9 compressed payload candidate\n",
                                      section.name.c_str(),
                                      static_cast<long long>(page.page_id));
                        page_data = std::move(skip_crc9_page_data);
                    } else if (dwg_debug_enabled() &&
                               section.name.find("AcDb:AcDbObjects") != std::string::npos &&
                               page.data_offset == 0) {
                        dwg_debug_log("[DWG] R2007 AcDbObjects candidates normal=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x skip8=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                      page_data.size() > 0 ? page_data[0] : 0,
                                      page_data.size() > 1 ? page_data[1] : 0,
                                      page_data.size() > 2 ? page_data[2] : 0,
                                      page_data.size() > 3 ? page_data[3] : 0,
                                      page_data.size() > 4 ? page_data[4] : 0,
                                      page_data.size() > 5 ? page_data[5] : 0,
                                      page_data.size() > 6 ? page_data[6] : 0,
                                      page_data.size() > 7 ? page_data[7] : 0,
                                      page_data.size() > 8 ? page_data[8] : 0,
                                      page_data.size() > 9 ? page_data[9] : 0,
                                      page_data.size() > 10 ? page_data[10] : 0,
                                      page_data.size() > 11 ? page_data[11] : 0,
                                      skip_crc_page_data.size() > 0 ? skip_crc_page_data[0] : 0,
                                      skip_crc_page_data.size() > 1 ? skip_crc_page_data[1] : 0,
                                      skip_crc_page_data.size() > 2 ? skip_crc_page_data[2] : 0,
                                      skip_crc_page_data.size() > 3 ? skip_crc_page_data[3] : 0,
                                      skip_crc_page_data.size() > 4 ? skip_crc_page_data[4] : 0,
                                      skip_crc_page_data.size() > 5 ? skip_crc_page_data[5] : 0,
                                      skip_crc_page_data.size() > 6 ? skip_crc_page_data[6] : 0,
                                      skip_crc_page_data.size() > 7 ? skip_crc_page_data[7] : 0,
                                      skip_crc_page_data.size() > 8 ? skip_crc_page_data[8] : 0,
                                      skip_crc_page_data.size() > 9 ? skip_crc_page_data[9] : 0,
                                      skip_crc_page_data.size() > 10 ? skip_crc_page_data[10] : 0,
                                      skip_crc_page_data.size() > 11 ? skip_crc_page_data[11] : 0);
                    }
                }
                if (section.encoding == 4 &&
                    (section.name.find("AcDb:Handles") != std::string::npos ||
                     section.name.find("AcDb:Classes") != std::string::npos)) {
                    auto alt_page_data = r2007::r2007_decode_data_page_no_correction(
                        data + static_cast<size_t>(file_offset),
                        encoded_size,
                        compressed_size,
                        static_cast<size_t>(page.uncompressed_size),
                        section.encoding,
                        true);
                    std::vector<uint8_t> best_handles_page_data;
                    if (section.name.find("AcDb:Handles") != std::string::npos) {
                        struct HandlesCandidate {
                            std::vector<uint8_t>* data = nullptr;
                            const char* label = "";
                            int64_t score = 0;
                        };
                        std::vector<HandlesCandidate> candidates;
                        candidates.push_back({&page_data, "primary", r2007::r2007_handles_page_score(page_data)});
                        candidates.push_back({&alt_page_data, "non-interleaved", r2007::r2007_handles_page_score(alt_page_data)});
                        if (!skip_crc_page_data.empty()) {
                            candidates.push_back({&skip_crc_page_data, "+8", r2007::r2007_handles_page_score(skip_crc_page_data)});
                        }
                        if (!skip_crc9_page_data.empty()) {
                            candidates.push_back({&skip_crc9_page_data, "+9", r2007::r2007_handles_page_score(skip_crc9_page_data)});
                        }
                        if (dwg_debug_enabled() && page.data_offset == 0) {
                            std::string score_message = "[DWG] R2007 Handles candidate scores page_id=";
                            score_message += std::to_string(page.page_id);
                            for (const auto& candidate : candidates) {
                                score_message += " ";
                                score_message += candidate.label;
                                score_message += "=";
                                score_message += std::to_string(candidate.score);
                            }
                            score_message += "\n";
                            dwg_debug_log("%s", score_message.c_str());
                            for (const auto& candidate : candidates) {
                                if (!candidate.data || candidate.data->empty()) continue;
                                const auto& cd = *candidate.data;
                                dwg_debug_log(
                                    "[DWG] R2007 Handles candidate %s first=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x size=%zu\n",
                                    candidate.label,
                                    cd.size() > 0 ? cd[0] : 0,
                                    cd.size() > 1 ? cd[1] : 0,
                                    cd.size() > 2 ? cd[2] : 0,
                                    cd.size() > 3 ? cd[3] : 0,
                                    cd.size() > 4 ? cd[4] : 0,
                                    cd.size() > 5 ? cd[5] : 0,
                                    cd.size() > 6 ? cd[6] : 0,
                                    cd.size() > 7 ? cd[7] : 0,
                                    cd.size() > 8 ? cd[8] : 0,
                                    cd.size() > 9 ? cd[9] : 0,
                                    cd.size() > 10 ? cd[10] : 0,
                                    cd.size() > 11 ? cd[11] : 0,
                                    cd.size());
                            }
                        }
                        auto best_it = std::max_element(
                            candidates.begin(), candidates.end(),
                            [](const HandlesCandidate& a, const HandlesCandidate& b) {
                                return a.score < b.score;
                            });
                        if (best_it != candidates.end() && best_it->data &&
                            best_it->data != &page_data && best_it->score > candidates.front().score) {
                            dwg_debug_log("[DWG] R2007 Handles page_id=%lld selected %s candidate score=%lld primary_score=%lld\n",
                                          static_cast<long long>(page.page_id),
                                          best_it->label,
                                          static_cast<long long>(best_it->score),
                                          static_cast<long long>(candidates.front().score));
                            best_handles_page_data = *best_it->data;
                        }
                    }
                    const bool primary_ok =
                        (section.name.find("AcDb:Handles") != std::string::npos &&
                         r2007::r2007_handles_page_plausible(page_data)) ||
                        (section.name.find("AcDb:Classes") != std::string::npos &&
                         r2007::r2007_classes_page_plausible(page_data));
                    const bool alt_ok =
                        (section.name.find("AcDb:Handles") != std::string::npos &&
                         r2007::r2007_handles_page_plausible(alt_page_data)) ||
                        (section.name.find("AcDb:Classes") != std::string::npos &&
                         r2007::r2007_classes_page_plausible(alt_page_data));
                    if (!primary_ok && alt_ok) {
                        dwg_debug_log("[DWG] R2007 section '%s' page_id=%lld selected non-interleaved data-page candidate\n",
                                      section.name.c_str(),
                                      static_cast<long long>(page.page_id));
                        page_data = std::move(alt_page_data);
                    } else if (!best_handles_page_data.empty()) {
                        page_data = std::move(best_handles_page_data);
                    } else if (section.name.find("AcDb:Classes") != std::string::npos &&
                               r2007::r2007_classes_page_has_split_initial_literal(page_data)) {
                        dwg_debug_log("[DWG] R2007 Classes page_id=%lld repaired split initial literal sentinel\n",
                                      static_cast<long long>(page.page_id));
                        r2007::r2007_repair_split_classes_literal(page_data);
                    } else if (section.name.find("AcDb:Classes") != std::string::npos) {
                        dwg_debug_log("[DWG] R2007 Classes candidate primary=%02x %02x %02x %02x alt=%02x %02x %02x %02x\n",
                                      page_data.size() > 0 ? page_data[0] : 0,
                                      page_data.size() > 1 ? page_data[1] : 0,
                                      page_data.size() > 2 ? page_data[2] : 0,
                                      page_data.size() > 3 ? page_data[3] : 0,
                                      alt_page_data.size() > 0 ? alt_page_data[0] : 0,
                                      alt_page_data.size() > 1 ? alt_page_data[1] : 0,
                                      alt_page_data.size() > 2 ? alt_page_data[2] : 0,
                                      alt_page_data.size() > 3 ? alt_page_data[3] : 0);
                    } else if (section.name.find("AcDb:Handles") != std::string::npos && page.data_offset == 0) {
                        dwg_debug_log("[DWG] R2007 Handles candidate primary=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x alt=%02x %02x %02x %02x\n",
                                      page_data.size() > 0 ? page_data[0] : 0,
                                      page_data.size() > 1 ? page_data[1] : 0,
                                      page_data.size() > 2 ? page_data[2] : 0,
                                      page_data.size() > 3 ? page_data[3] : 0,
                                      page_data.size() > 4 ? page_data[4] : 0,
                                      page_data.size() > 5 ? page_data[5] : 0,
                                      page_data.size() > 6 ? page_data[6] : 0,
                                      page_data.size() > 7 ? page_data[7] : 0,
                                      page_data.size() > 8 ? page_data[8] : 0,
                                      page_data.size() > 9 ? page_data[9] : 0,
                                      page_data.size() > 10 ? page_data[10] : 0,
                                      page_data.size() > 11 ? page_data[11] : 0,
                                      page_data.size() > 12 ? page_data[12] : 0,
                                      page_data.size() > 13 ? page_data[13] : 0,
                                      page_data.size() > 14 ? page_data[14] : 0,
                                      page_data.size() > 15 ? page_data[15] : 0,
                                      alt_page_data.size() > 0 ? alt_page_data[0] : 0,
                                      alt_page_data.size() > 1 ? alt_page_data[1] : 0,
                                      alt_page_data.size() > 2 ? alt_page_data[2] : 0,
                                      alt_page_data.size() > 3 ? alt_page_data[3] : 0);
                    }
                }
            } else {
                const size_t take = std::min<size_t>(encoded_size, page.page_size);
                page_data.assign(data + static_cast<size_t>(file_offset),
                                 data + static_cast<size_t>(file_offset) + take);
            }
            const size_t target = static_cast<size_t>(std::min<uint64_t>(page.data_offset, assembled.size()));
            if (target + page_data.size() > assembled.size()) {
                assembled.resize(target + page_data.size(), 0);
            }
            std::copy(page_data.begin(), page_data.end(), assembled.begin() + static_cast<std::ptrdiff_t>(target));
            if (section.name.find("AcDbObjects") != std::string::npos ||
                section.name.find("AcDb:AcDbObjects") != std::string::npos) {
                m_sections.object_pages.emplace_back(target, page_data.size());
            }
        }
        if (section.data_size > 0 && assembled.size() > section.data_size) {
            assembled.resize(static_cast<size_t>(section.data_size));
        }
        return assembled;
    };

    for (const auto& section : sections) {
        std::vector<uint8_t> decoded = decode_section(section);
        if (section.name.find("AcDb:Header") != std::string::npos) {
            m_sections.header_vars = std::move(decoded);
        } else if (section.name.find("AcDb:Classes") != std::string::npos) {
            m_sections.classes = std::move(decoded);
        } else if (section.name.find("AcDb:Handles") != std::string::npos) {
            m_sections.object_map = std::move(decoded);
            m_sections.objmap_page_size = static_cast<uint32_t>(
                std::min<uint64_t>(section.max_size, UINT32_MAX));
        } else if (section.name.find("AcDb:AcDbObjects") != std::string::npos ||
                   section.name.find("AcDbObjects") != std::string::npos) {
            m_sections.object_data = std::move(decoded);
            if (!section.pages.empty()) {
                const auto page_it = pages.find(static_cast<uint64_t>(std::llabs(section.pages.front().page_id)));
                if (page_it != pages.end()) {
                    m_sections.object_data_file_offset = static_cast<size_t>(page_it->second.offset + 0x480);
                }
            }
        } else if (!decoded.empty()) {
            m_sections.auxiliary_sections.push_back({section.name, std::move(decoded)});
        }
    }

    scene.add_diagnostic({
        "dwg_r2007_container_decoded",
        "Version gap",
        "R2007/AC1021 R21 file header, page map, and section map were decoded without external conversion. Reed-Solomon error correction and encrypted section payloads remain provisional.",
        static_cast<int32_t>(std::min<size_t>(sections.size(), 2147483647u)),
        version_family_name(m_version),
        "File container",
        0,
        "R2007 container",
    });

    if (m_sections.object_data.empty() || m_sections.object_map.empty()) {
        add_gap("dwg_r2007_object_sections_missing",
                "Version gap",
                "R2007 container decoded, but AcDbObjects or AcDb:Handles section did not produce usable bytes.",
                1,
                "AcDbObjects/Handles");
    }

    return Result::success();
}

} // namespace cad
