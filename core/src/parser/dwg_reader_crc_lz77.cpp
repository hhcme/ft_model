// DWG CRC utilities, LZ77 decompression, and header decryption.
// Split from dwg_reader.cpp to keep file sizes under 1000 lines.

#include "cad/parser/dwg_reader.h"

#include "cad/parser/dwg_parser.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cad {

// ============================================================
// CRC utilities
// ============================================================

static uint8_t crc8_table[256];
static bool crc8_table_initialized = false;

static void init_crc8_table()
{
    if (crc8_table_initialized) return;
    for (int i = 0; i < 256; ++i) {
        uint8_t crc = static_cast<uint8_t>(i);
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x80) {
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
            } else {
                crc = static_cast<uint8_t>(crc << 1);
            }
        }
        crc8_table[i] = crc;
    }
    crc8_table_initialized = true;
}

uint8_t crc8(const uint8_t* data, size_t len)
{
    init_crc8_table();
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc = crc8_table[crc ^ data[i]];
    }
    return crc;
}

static uint16_t crc16_table[256];
static bool crc16_table_initialized = false;

static void init_crc16_table()
{
    if (crc16_table_initialized) return;
    for (int i = 0; i < 256; ++i) {
        uint16_t crc = static_cast<uint16_t>(i);
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
        crc16_table[i] = crc;
    }
    crc16_table_initialized = true;
}

uint16_t crc16(const uint8_t* data, size_t len)
{
    init_crc16_table();
    uint16_t crc = 0xC0C1;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ crc16_table[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}

// ============================================================
// LZ77 decompression for R2004+ DWG sections
// ============================================================

size_t dwg_decompress_into(const uint8_t* data,
                           size_t compressed_size,
                           uint8_t* output,
                           size_t output_size,
                           size_t write_offset)
{
    if (!data || compressed_size == 0 || !output || write_offset >= output_size) {
        return 0;
    }

    size_t src = 0;
    size_t dst = write_offset;
    size_t dst_max = output_size;

    auto read_byte = [&]() -> uint8_t {
        if (src >= compressed_size) return 0;
        return data[src++];
    };

    auto read_literal_length = [&](uint8_t opcode) -> unsigned int {
        unsigned int length = opcode & 0x0F;
        if (length == 0) {
            uint8_t b = 0;
            while (src < compressed_size && (b = read_byte()) == 0)
                length += 0xFF;
            length += 0x0F + b;
        }
        return length + 3;
    };

    auto read_comp_bytes = [&](uint8_t opcode, unsigned int mask) -> int {
        unsigned int nbytes = opcode & mask;
        if (nbytes == 0) {
            uint8_t b = 0;
            while (src < compressed_size && (b = read_byte()) == 0)
                nbytes += 0xFF;
            nbytes += mask + b;
        }
        return (int)nbytes + 2;
    };

    auto read_two_byte_offset = [&](int plus, int& offset) -> uint8_t {
        uint8_t first = read_byte();
        uint8_t second = read_byte();
        offset |= (first >> 2);
        offset |= (second << 6);
        offset += plus;
        return first;
    };

    auto copy_literal = [&](unsigned int length) -> uint8_t {
        for (unsigned int i = 0; i < length; ++i) {
            if (src >= compressed_size || dst >= dst_max) break;
            output[dst++] = data[src++];
        }
        return read_byte();
    };

    uint8_t opcode1 = read_byte();

    if ((opcode1 & 0xF0) == 0) {
        unsigned int lit_len = read_literal_length(opcode1);
        opcode1 = copy_literal(lit_len);
    }

    while (src < compressed_size && dst < dst_max) {
        int comp_bytes = 0;
        int comp_offset = 0;

        if (opcode1 < 0x10 || opcode1 >= 0x40) {
            comp_bytes = (opcode1 >> 4) - 1;
            uint8_t opcode2 = read_byte();
            comp_offset = (((opcode1 >> 2) & 3) | (opcode2 << 2)) + 1;
        } else if (opcode1 >= 0x20) {
            comp_bytes = read_comp_bytes(opcode1, 0x1F);
            opcode1 = read_two_byte_offset(1, comp_offset);
        } else {
            comp_bytes = read_comp_bytes(opcode1, 7);
            comp_offset = (opcode1 & 8) << 11;
            opcode1 = read_two_byte_offset(0x4000, comp_offset);
        }

        if (comp_offset > 0 && comp_bytes > 0) {
            size_t pos = dst;
            size_t end = pos + comp_bytes;
            if (end > dst_max) {
                comp_bytes = static_cast<int>(dst_max - pos);
                end = pos + comp_bytes;
            }
            if (static_cast<size_t>(comp_offset) > pos) {
                for (size_t p = pos; p < end; ++p) {
                    output[p] = 0;
                }
            } else {
                for (size_t p = pos; p < end; ++p) {
                    output[p] = output[p - comp_offset];
                }
            }
            dst = end;
        }

        unsigned int lit_length = opcode1 & 3;
        if (lit_length == 0) {
            opcode1 = read_byte();
            if ((opcode1 & 0xF0) == 0) {
                unsigned int llen = read_literal_length(opcode1);
                opcode1 = copy_literal(llen);
            }
        } else {
            opcode1 = copy_literal(lit_length);
        }
    }

    dwg_debug_log("[decomp] returned actual=%zu\n", (size_t)(dst - write_offset));
    return dst - write_offset;
}

std::pair<std::vector<uint8_t>, size_t> dwg_decompress(const uint8_t* data,
                                     size_t compressed_size,
                                     size_t decompressed_size)
{
    if (!data || compressed_size == 0 || decompressed_size == 0) {
        return {{}, 0};
    }

    std::vector<uint8_t> output(decompressed_size, 0);
    size_t actual = dwg_decompress_into(data, compressed_size, output.data(), output.size(), 0);
    return {std::move(output), actual};
}

// ============================================================
// R2004+ header decryption
// ============================================================

void dwg_decrypt_header(const uint8_t* encrypted,
                         uint8_t* decrypted,
                         size_t len,
                         const uint8_t* /* key */)
{
    if (!encrypted || !decrypted || len == 0) return;

    uint32_t seed = 1;
    for (size_t i = 0; i < len; ++i) {
        seed = seed * 0x343FD + 0x269EC3;
        decrypted[i] = encrypted[i] ^ static_cast<uint8_t>((seed >> 16) & 0xFF);
    }
}

} // namespace cad
