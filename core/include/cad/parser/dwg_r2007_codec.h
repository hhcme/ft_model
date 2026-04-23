#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cad {
namespace detail {
namespace r2007 {

// ============================================================
// R2007/AC1021 R21 container data structures
// ============================================================

struct HeaderData {
    uint64_t header_size = 0;
    uint64_t file_size = 0;
    uint64_t pages_map_correction_factor = 0;
    uint64_t pages_map_offset = 0;
    uint64_t pages_map_id = 0;
    uint64_t pages_map_size_compressed = 0;
    uint64_t pages_map_size_uncompressed = 0;
    uint64_t pages_amount = 0;
    uint64_t pages_max_id = 0;
    uint64_t sections_amount = 0;
    uint64_t sections_map_size_compressed = 0;
    uint64_t sections_map_id = 0;
    uint64_t sections_map_size_uncompressed = 0;
    uint64_t sections_map_correction_factor = 0;
    uint64_t stream_version = 0;
};

struct PageRecord {
    int64_t id = 0;
    uint64_t size = 0;
    uint64_t offset = 0;
};

struct SectionRecord {
    uint64_t data_size = 0;
    uint64_t max_size = 0;
    uint64_t encryption = 0;
    uint64_t hash_code = 0;
    uint64_t encoding = 0;
    uint64_t num_pages = 0;
    std::string name;

    struct Page {
        uint64_t data_offset = 0;
        uint64_t page_size = 0;
        int64_t page_id = 0;
        uint64_t uncompressed_size = 0;
        uint64_t compressed_size = 0;
        uint64_t checksum = 0;
        uint64_t crc = 0;
    };
    std::vector<Page> pages;
};

// ============================================================
// R21 literal copy helpers — byte-reversal and half-swap
// matching libredwg copy_bytes_2/copy_bytes_3/copy_16
// ============================================================

void r21_copy_1(uint8_t*& dst, const uint8_t* src, int offset);
void r21_copy_2(uint8_t*& dst, const uint8_t* src, int offset);
void r21_copy_3(uint8_t*& dst, const uint8_t* src, int offset);
void r21_copy_4(uint8_t*& dst, const uint8_t* src, int offset);
void r21_copy_8(uint8_t*& dst, const uint8_t* src, int offset);
void r21_copy_16(uint8_t*& dst, const uint8_t* src, int offset);

// Copy literal bytes from src to dst, matching libredwg copy_compressed_bytes.
// length must be 1..32.  dst is advanced by length.
void r21_copy_compressed_bytes(uint8_t*& dst, const uint8_t* src, int length);

// R21 decompression (LZ77 variant used in R2007/AC1021 DWG files)
std::vector<uint8_t> r21_decompress(const uint8_t* src, size_t src_size, size_t out_size);

// ============================================================
// R2007 system/data page codec
// ============================================================

// De-interleave RS-encoded data without Reed-Solomon error correction
std::vector<uint8_t> r2007_take_system_data_no_correction(const uint8_t* encoded,
                                                          size_t encoded_size,
                                                          size_t factor,
                                                          size_t data_bytes_per_block);

// Align value up to the given alignment
size_t align_up(size_t value, size_t alignment);

// Compute the expected system page size for a given uncompressed payload
size_t r2007_system_page_size(size_t uncompressed_size);

// Decode an R2007 system page (page map, section map) without RS correction
std::vector<uint8_t> r2007_decode_system_page_no_correction(const uint8_t* encoded,
                                                            size_t available_size,
                                                            size_t compressed_size,
                                                            size_t uncompressed_size,
                                                            size_t correction_factor);

// Decode an R2007 data page (object data, handles, classes, header)
std::vector<uint8_t> r2007_decode_data_page_no_correction(const uint8_t* encoded,
                                                          size_t encoded_size,
                                                          size_t compressed_size,
                                                          size_t uncompressed_size,
                                                          uint64_t section_encoding,
                                                          bool force_non_interleaved,
                                                          size_t compressed_prefix_skip = 0);

// ============================================================
// R2007 page validation / scoring heuristics
// ============================================================

// Check if decompressed data starts with the Classes section sentinel
bool r2007_classes_page_plausible(const std::vector<uint8_t>& data);

// Check if the initial 16-byte literal has its halves swapped (split sentinel)
bool r2007_classes_page_has_split_initial_literal(const std::vector<uint8_t>& data);

// Rotate the first 16 bytes to restore the correct sentinel order
void r2007_repair_split_classes_literal(std::vector<uint8_t>& data);

// Check if decompressed data looks like a valid Object Map (Handles) page
bool r2007_handles_page_plausible(const std::vector<uint8_t>& data);

// Score a Handles page candidate by parsing handle/offset pairs.
// Higher score = more plausible. Returns a negative value for unusable data.
int64_t r2007_handles_page_score(const std::vector<uint8_t>& data);

} // namespace r2007
} // namespace detail
} // namespace cad
