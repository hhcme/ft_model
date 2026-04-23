// R2007 page decode and validation helpers.
// Extracted from dwg_r2007_codec.cpp to reduce file size.

#include "cad/parser/dwg_r2007_codec.h"
#include "cad/parser/dwg_parser.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cad {
namespace detail {
namespace r2007 {

std::vector<uint8_t> r2007_take_system_data_no_correction(const uint8_t* encoded,
                                                          size_t encoded_size,
                                                          size_t factor,
                                                          size_t data_bytes_per_block)
{
    std::vector<uint8_t> decoded;
    if (!encoded || factor == 0 || data_bytes_per_block == 0) return decoded;
    decoded.reserve(factor * data_bytes_per_block);
    for (size_t block = 0; block < factor; ++block) {
        std::vector<uint8_t> codeword;
        codeword.reserve(255);
        for (size_t i = 0; i < 255; ++i) {
            const size_t source = factor * i + block;
            if (source >= encoded_size) break;
            codeword.push_back(encoded[source]);
        }
        const size_t take = std::min(data_bytes_per_block, codeword.size());
        decoded.insert(decoded.end(), codeword.begin(), codeword.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return decoded;
}

size_t align_up(size_t value, size_t alignment)
{
    if (alignment == 0) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

size_t r2007_system_page_size(size_t uncompressed_size)
{
    if (uncompressed_size == 0) return 0x400;
    const size_t blocks = (uncompressed_size * 2 + 238) / 239;
    return std::max<size_t>(0x400, align_up(blocks * 255, 0x20));
}

std::vector<uint8_t> r2007_decode_system_page_no_correction(const uint8_t* encoded,
                                                            size_t available_size,
                                                            size_t compressed_size,
                                                            size_t uncompressed_size,
                                                            size_t correction_factor)
{
    if (!encoded || compressed_size == 0 || uncompressed_size == 0) return {};

    const size_t page_size =
        std::min(available_size, r2007_system_page_size(uncompressed_size));
    if (page_size == 0) return {};

    size_t interleave_blocks = page_size / 255;
    if (correction_factor > 0) {
        const size_t pre_rs_bytes = align_up(compressed_size, 8) * correction_factor;
        interleave_blocks = std::max<size_t>(1, (pre_rs_bytes + 238) / 239);
        if (interleave_blocks * 255 > page_size) {
            interleave_blocks = std::max<size_t>(1, page_size / 255);
        }
    }

    auto system_data = r2007_take_system_data_no_correction(
        encoded, page_size, interleave_blocks, 239);
    if (system_data.size() < compressed_size) return {};

    return r21_decompress(system_data.data(), compressed_size, uncompressed_size);
}

std::vector<uint8_t> r2007_decode_data_page_no_correction(const uint8_t* encoded,
                                                          size_t encoded_size,
                                                          size_t compressed_size,
                                                          size_t uncompressed_size,
                                                          uint64_t section_encoding,
                                                          bool force_non_interleaved,
                                                          size_t compressed_prefix_skip)
{
    if (!encoded || encoded_size == 0 || compressed_size == 0 || uncompressed_size == 0) {
        return {};
    }

    std::vector<uint8_t> page_data;
    const size_t block_count = encoded_size / 255;
    if (section_encoding == 4 && block_count > 0 && !force_non_interleaved) {
        page_data = r2007_take_system_data_no_correction(encoded, encoded_size, block_count, 251);
    } else {
        const size_t data_bytes = block_count > 0
            ? std::min(encoded_size, block_count * 251)
            : encoded_size;
        page_data.assign(encoded, encoded + static_cast<std::ptrdiff_t>(data_bytes));
    }

    if (page_data.size() < compressed_size || compressed_prefix_skip >= compressed_size) {
        return {};
    }
    if (compressed_size >= uncompressed_size) {
        if (compressed_prefix_skip >= uncompressed_size) return {};
        return std::vector<uint8_t>(
            page_data.begin() + static_cast<std::ptrdiff_t>(compressed_prefix_skip),
            page_data.begin() + static_cast<std::ptrdiff_t>(uncompressed_size));
    }
    return r21_decompress(page_data.data() + compressed_prefix_skip,
                          compressed_size - compressed_prefix_skip,
                          uncompressed_size);
}

bool r2007_classes_page_plausible(const std::vector<uint8_t>& data)
{
    static constexpr uint8_t kClassesSentinel[] = {
        0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
        0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A,
    };
    return data.size() >= sizeof(kClassesSentinel) &&
           std::equal(std::begin(kClassesSentinel),
                      std::end(kClassesSentinel),
                      data.begin());
}

bool r2007_classes_page_has_split_initial_literal(const std::vector<uint8_t>& data)
{
    static constexpr uint8_t kClassesSentinel[] = {
        0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
        0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A,
    };
    return data.size() >= sizeof(kClassesSentinel) &&
           std::equal(std::begin(kClassesSentinel) + 8,
                      std::end(kClassesSentinel),
                      data.begin()) &&
           std::equal(std::begin(kClassesSentinel),
                      std::begin(kClassesSentinel) + 8,
                      data.begin() + 8);
}

void r2007_repair_split_classes_literal(std::vector<uint8_t>& data)
{
    if (!r2007_classes_page_has_split_initial_literal(data)) return;
    std::rotate(data.begin(), data.begin() + 8, data.begin() + 16);
}

bool r2007_handles_page_plausible(const std::vector<uint8_t>& data)
{
    if (data.size() < 2) return false;
    const uint16_t section_size = (static_cast<uint16_t>(data[0]) << 8) |
                                  static_cast<uint16_t>(data[1]);
    return section_size > 0 && section_size <= 2040;
}

int64_t r2007_handles_page_score(const std::vector<uint8_t>& data)
{
    if (data.size() < 2) return 0;
    size_t off = 0;
    int64_t score = 0;
    size_t total_entries = 0;
    const size_t data_size = data.size();
    while (off + 2 <= data_size) {
        const size_t section_start = off;
        const uint16_t section_size = (static_cast<uint16_t>(data[off]) << 8) |
                                      static_cast<uint16_t>(data[off + 1]);
        if (section_size == 0 || section_size > 2040) break;
        const size_t section_data_end = section_start + section_size;
        if (section_data_end > data.size()) break;
        off += 2;
        uint64_t handle_acc = 0;
        int64_t offset_acc = 0;
        size_t entries = 0;
        size_t plausible_entries = 0;
        size_t negative_offsets = 0;
        size_t huge_handle_jumps = 0;
        size_t huge_offset_jumps = 0;
        size_t duplicate_offsets = 0;
        int64_t previous_offset = -1;
        while (off < section_data_end && entries < 256) {
            size_t before = off;
            const uint32_t handle_delta =
                DwgParser::read_modular_char(data.data(), data.size(), off);
            if (off <= before || off >= section_data_end) break;
            before = off;
            const int32_t offset_delta =
                DwgParser::read_modular_char_signed(data.data(), data.size(), off);
            if (off <= before || off > section_data_end) break;
            handle_acc += handle_delta;
            offset_acc += offset_delta;
            if (offset_acc < 0) {
                negative_offsets++;
            } else if (offset_acc < 500000000) {
                plausible_entries++;
                if (previous_offset >= 0 && offset_acc == previous_offset) {
                    duplicate_offsets++;
                }
                previous_offset = offset_acc;
            }
            if (handle_delta > 1000000) {
                huge_handle_jumps++;
            }
            if (std::llabs(static_cast<long long>(offset_delta)) > 2000000) {
                huge_offset_jumps++;
            }
            entries++;
        }
        if (entries == 0) break;
        total_entries += entries;
        score += static_cast<int64_t>(plausible_entries) * 8;
        score += static_cast<int64_t>(entries) / 8;
        score -= static_cast<int64_t>(negative_offsets) * 10;
        score -= static_cast<int64_t>(huge_handle_jumps) * 12;
        score -= static_cast<int64_t>(huge_offset_jumps) * 8;
        score -= static_cast<int64_t>(duplicate_offsets) * 4;
        off = section_data_end + 2;
    }
    if (total_entries == 0) return -1;
    return score;
}

} // namespace r2007
} // namespace detail
} // namespace cad
