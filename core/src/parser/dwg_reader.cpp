#include "cad/parser/dwg_reader.h"

#include "cad/parser/dwg_parser.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace cad {

// ============================================================
// DwgBitReader implementation
// ============================================================

DwgBitReader::DwgBitReader(const uint8_t* data, size_t size)
    : m_data(data)
    , m_size(size)
{
}

uint8_t DwgBitReader::read_bit()
{
    if (m_error || remaining_bits() == 0) {
        m_error = true;
        return 0;
    }
    size_t byte_idx = m_bit_pos >> 3;
    size_t bit_idx = 7 - (m_bit_pos & 7);  // MSB first within each byte
    uint8_t val = (m_data[byte_idx] >> bit_idx) & 1;
    ++m_bit_pos;
    return val;
}

uint8_t DwgBitReader::read_bits(int count)
{
    if (count < 0 || count > 8) {
        m_error = true;
        return 0;
    }
    if (m_error || remaining_bits() < static_cast<size_t>(count)) {
        m_error = true;
        return 0;
    }
    uint8_t result = 0;
    for (int i = 0; i < count; ++i) {
        result = static_cast<uint8_t>((result << 1) | read_bit());
    }
    return result;
}

void DwgBitReader::align_to_byte()
{
    if (m_error) return;
    size_t remainder = m_bit_pos & 7;
    if (remainder != 0) {
        m_bit_pos += (8 - remainder);
    }
}

size_t DwgBitReader::bit_offset() const
{
    return m_bit_pos;
}

void DwgBitReader::set_bit_offset(size_t offset)
{
    size_t max_bits = m_bit_limit ? m_bit_limit : m_size * 8;
    if (offset > max_bits) {
        m_error = true;
        return;
    }
    m_bit_pos = offset;
}

size_t DwgBitReader::remaining_bits() const
{
    size_t max_bits = m_bit_limit ? m_bit_limit : m_size * 8;
    if (m_bit_pos >= max_bits) return 0;
    return max_bits - m_bit_pos;
}

void DwgBitReader::set_bit_limit(size_t limit)
{
    m_bit_limit = limit;
}

size_t DwgBitReader::bit_limit() const
{
    return m_bit_limit;
}

uint8_t DwgBitReader::read_u8()
{
    if (m_error || remaining_bits() < 8) {
        m_error = true;
        return 0;
    }
    uint8_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result = static_cast<uint8_t>((result << 1) | read_bit());
    }
    return result;
}

uint16_t DwgBitReader::read_u16()
{
    if (m_error || remaining_bits() < 16) {
        m_error = true;
        return 0;
    }
    uint16_t result = 0;
    for (int i = 0; i < 16; ++i) {
        result = static_cast<uint16_t>((result << 1) | read_bit());
    }
    return result;
}

uint32_t DwgBitReader::read_u32()
{
    if (m_error || remaining_bits() < 32) {
        m_error = true;
        return 0;
    }
    uint32_t result = 0;
    for (int i = 0; i < 32; ++i) {
        result = (result << 1) | read_bit();
    }
    return result;
}

int16_t DwgBitReader::read_s16()
{
    return static_cast<int16_t>(read_u16());
}

int32_t DwgBitReader::read_s32()
{
    return static_cast<int32_t>(read_u32());
}

float DwgBitReader::read_float()
{
    if (m_error || remaining_bits() < 32) {
        m_error = true;
        return 0.0f;
    }
    // DWG stores raw floats in little-endian byte order, byte-aligned.
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = read_raw_char();
    uint32_t bits = static_cast<uint32_t>(b[0]) |
                    (static_cast<uint32_t>(b[1]) << 8) |
                    (static_cast<uint32_t>(b[2]) << 16) |
                    (static_cast<uint32_t>(b[3]) << 24);
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

double DwgBitReader::read_double()
{
    if (m_error || remaining_bits() < 64) {
        m_error = true;
        return 0.0;
    }
    // DWG stores raw doubles in little-endian byte order, byte-aligned.
    // Use read_raw_char() to preserve byte boundaries.
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = read_raw_char();
    uint64_t bits = static_cast<uint64_t>(b[0]) |
                    (static_cast<uint64_t>(b[1]) << 8) |
                    (static_cast<uint64_t>(b[2]) << 16) |
                    (static_cast<uint64_t>(b[3]) << 24) |
                    (static_cast<uint64_t>(b[4]) << 32) |
                    (static_cast<uint64_t>(b[5]) << 40) |
                    (static_cast<uint64_t>(b[6]) << 48) |
                    (static_cast<uint64_t>(b[7]) << 56);
    double result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

uint8_t DwgBitReader::read_raw_char()
{
    // RC (Raw Char): 8-bit read from current bit position, NOT byte-aligned.
    // Per libredwg bit_read_RC: reads 8 bits starting at current bit position,
    // possibly spanning two adjacent bytes.
    if (m_error || remaining_bits() < 8) {
        m_error = true;
        return 0;
    }
    size_t byte_idx = m_bit_pos >> 3;
    size_t bit_idx = m_bit_pos & 7;
    uint8_t result;
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

// ---- DWG variable-length encodings ----

uint16_t DwgBitReader::read_bot()
{
    // R2010+ BOT (Bit Object Type) per libredwg bit_read_BOT:
    //   BB=00 -> read RC (1 raw byte) and return it
    //   BB=01 -> read RC + 0x1F0
    //   BB=10 or 11 -> read RS (2 raw bytes, LE) and return it
    uint8_t two_bit_code = read_bits(2);
    switch (two_bit_code) {
        case 0: return read_raw_char();
        case 1: return static_cast<uint16_t>(read_raw_char()) + 0x1F0;
        case 2:
        case 3: return read_rs();
        default: return 0;
    }
}

uint16_t DwgBitReader::read_rs()
{
    // RS (Raw Short): 16-bit little-endian value, bit-aligned.
    // Per libredwg bit_read_RS: reads 2 RCs (current byte index), low byte first.
    uint8_t b0 = read_raw_char();
    uint8_t b1 = read_raw_char();
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t DwgBitReader::read_rl()
{
    // RL (Raw Long): 32-bit little-endian value, bit-aligned.
    // Per libredwg bit_read_RL: reads 4 RCs (current byte index), low byte first.
    uint8_t b0 = read_raw_char();
    uint8_t b1 = read_raw_char();
    uint8_t b2 = read_raw_char();
    uint8_t b3 = read_raw_char();
    return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
           (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b3) << 24);
}

uint16_t DwgBitReader::read_bs()
{
    // BS (BitShort): 2-bit BB code then value
    //   BB=00: RS (byte-aligned 16-bit LE)
    //   BB=01: RC (byte-aligned 8-bit)
    //   BB=10: implicit 0
    //   BB=11: implicit 256
    uint8_t code = read_bits(2);
    uint16_t result = 0;
    switch (code) {
        case 0: result = read_rs(); break;
        case 1: result = read_raw_char(); break;
        case 2: result = 0; break;
        case 3: result = 256; break;  // Per ODA spec & libredwg: code 3 → 256
        default: result = 0; break;
    }
    // DEBUG: only print in class parsing context
    // dwg_debug_log("[read_bs] bit_pos=%zu code=%u result=%u m_error=%d\n", m_bit_pos, code, result, (int)m_error);
    return result;
}

uint32_t DwgBitReader::read_bl()
{
    // BL (BitLong): 2-bit BB code then value
    //   BB=00: RL (byte-aligned 32-bit LE)
    //   BB=01: RC (byte-aligned 8-bit)
    //   BB=10: implicit 0
    //   BB=11: NOT USED
    uint8_t code = read_bits(2);
    uint32_t result = 0;
    switch (code) {
        case 0: result = read_rl(); break;
        case 1: result = read_raw_char(); break;
        case 2: result = 0; break;
        case 3: result = 256; break;  // Per libredwg: return 256, do not abort
        default: result = 0; break;
    }
    return result;
}

uint64_t DwgBitReader::read_bll()
{
    // BLL (BitLongLong): 3-bit length code then bytes.
    // Per libredwg bit_read_BLL:
    //   len = (BB << 1) | B
    //   len=0 → 0, len=1 → RC (1 byte), len=2 → RS (2 bytes LE),
    //   len=4 → RL (4 bytes LE), other → len bytes LE
    unsigned len = (read_bits(2) << 1) | read_bit();
    switch (len) {
        case 0: return 0;
        case 1: return read_raw_char();
        case 2: return read_rs();
        case 4: return read_rl();
        default: {
            if (len > 8) { m_error = true; return 0; }
            uint64_t result = 0;
            for (unsigned i = 0; i < len && !m_error; ++i) {
                result |= static_cast<uint64_t>(read_raw_char()) << (i * 8);
            }
            return result;
        }
    }
}

double DwgBitReader::read_bd()
{
    uint8_t code = read_bits(2);
    switch (code) {
        case 0: return read_double();    // Full 64-bit double
        case 1: return 1.0;             // Implicit 1.0
        case 2: return 0.0;             // Implicit 0.0
        case 3:
            // Per libredwg spec: code 3 is "NOT USED"; do not treat as fatal error.
            return std::numeric_limits<double>::quiet_NaN();
        default: return 0.0;
    }
}

bool DwgBitReader::read_b()
{
    return read_bit() != 0;
}

double DwgBitReader::read_bt()
{
    // R2000+: BT (Bit Thickness) is a 1-bit flag followed by optional BD.
    // Per libredwg bit_read_BT: read 1 B bit; if 1 -> thickness=0.0, else read BD.
    if (read_b()) {
        return 0.0;
    }
    return read_bd();
}

double DwgBitReader::read_rd()
{
    // RD (Raw Double): 64-bit IEEE-754 double, bit-aligned.
    // Per libredwg bit_read_RD: reads 64 bits from current bit position.
    return read_double();
}

float DwgBitReader::read_rf()
{
    // RF (Raw Float): 16-bit IEEE-754 half-float, bit-aligned.
    // Reads 16 bits and converts to float.
    // Used for compact R2010+ entity data (e.g., SOLID corners).
    uint16_t bits = 0;
    if (remaining_bits() >= 16 && !m_error) {
        bits = static_cast<uint16_t>(read_bits(16));
        // IEEE 754 half-float: sign(1) + exp(5) + mantissa(10)
        // Convert to float (sign 1 + exp 8 + mantissa 23)
        uint32_t sign = (bits >> 15) & 1u;
        uint32_t exp16 = (bits >> 10) & 0x1Fu;
        uint32_t mant16 = bits & 0x3FFu;
        uint32_t sign32 = sign << 31;
        uint32_t exp32;
        uint32_t mant32;
        if (exp16 == 0) {
            // Subnormal or zero
            exp32 = 0;
            mant32 = mant16 << 13;
        } else if (exp16 == 31) {
            // Inf or NaN
            exp32 = 255;
            mant32 = mant16 << 13;
        } else {
            exp32 = exp16 - 15 + 127;  // Bias adjustment
            mant32 = mant16 << 13;
        }
        uint32_t float_bits = sign32 | (exp32 << 23) | mant32;
        float result;
        std::memcpy(&result, &float_bits, sizeof(float));
        return result;
    }
    m_error = true;
    return 0.0f;
}

DwgBitReader::HandleRef DwgBitReader::read_h()
{
    HandleRef ref;

    // DWG handles are byte-aligned (RC reads) per libredwg bit_read_H.
    if (m_error || remaining_bits() < 8) {
        m_error = true;
        return ref;
    }
    uint8_t code_byte = read_raw_char();
    ref.code = (code_byte >> 4) & 0x0F;
    uint8_t counter = code_byte & 0x0F;

    if (remaining_bits() < static_cast<size_t>(counter) * 8) {
        m_error = true;
        return ref;
    }

    // Read counter bytes, big-endian (each is a raw byte-aligned byte)
    ref.value = 0;
    for (uint8_t i = 0; i < counter; ++i) {
        ref.value = (ref.value << 8) | read_raw_char();
    }

    return ref;
}

uint16_t DwgBitReader::read_cmc()
{
    // Simplified CMC: just the color index as a BS
    // NOTE: For R2004+ files, use read_cmc_r2004() instead which reads
    // the full Encoded Color (BS index + BL rgb + RC flag + optional text).
    return read_bs();
}

DwgBitReader::CmcColor DwgBitReader::read_cmc_r2004(DwgVersion version)
{
    // R2004+ CMC (Color Method Code) per libredwg bit_read_CMC:
    //   BS(index), BL(rgb), RC(flag)
    //   if flag & 1: read T (name) from string stream (R2007+) or TV
    //   if flag & 2: read T (book_name) from string stream or TV
    CmcColor result;
    result.index = read_bs();
    if (version >= DwgVersion::R2004) {
        uint32_t rgb_raw = read_bl();    // 0x00BBGGRR
        uint8_t flag = read_raw_char();  // flag (RC)
        result.rgb = rgb_raw;
        result.has_rgb = (rgb_raw != 0);
        if (flag < 4) {
            // name/book_name text fields are in the string stream for R2007+
            if (version < DwgVersion::R2007) {
                if (flag & 1) (void)read_tv();  // name
                if (flag & 2) (void)read_tv();  // book_name
            }
        }
    }
    return result;
}

void DwgBitReader::read_be(double& x, double& y, double& z)
{
    // If first bit is 1, extrusion is default (0, 0, 1)
    if (read_b()) {
        x = 0.0;
        y = 0.0;
        z = 1.0;
    } else {
        x = read_bd();
        y = read_bd();
        z = read_bd();
    }
}

double DwgBitReader::read_dd(double default_value)
{
    // DD (Default Double) per libredwg bit_read_DD.
    // 2-bit code determines how the value differs from the default:
    //   00: value equals default (no extra data)
    //   01: replace lower 4 bytes of default with 4 RCs (byte-aligned)
    //   10: replace bytes 0-5 of default with 6 RCs (keeps upper 2 bytes)
    //   11: read full 8-byte RD
    uint8_t code = read_bits(2);
    uint8_t* p = reinterpret_cast<uint8_t*>(&default_value);
    switch (code) {
        case 0: return default_value;
        case 1:
            // Replace lower 4 bytes (indices 0-3) with 4 raw bytes
            p[0] = read_raw_char();
            p[1] = read_raw_char();
            p[2] = read_raw_char();
            p[3] = read_raw_char();
            return default_value;
        case 2:
            // Replace bytes 4-5 then bytes 0-3 (in that order per libredwg)
            p[4] = read_raw_char();
            p[5] = read_raw_char();
            p[0] = read_raw_char();
            p[1] = read_raw_char();
            p[2] = read_raw_char();
            p[3] = read_raw_char();
            return default_value;
        case 3: return read_double();
        default:
            m_error = true;
            return 0.0;
    }
}

uint32_t DwgBitReader::read_modular_char()
{
    // Variable-length encoding: high bit of each byte means "more bytes follow".
    // Each byte contributes 7 bits of data.
    // First byte is in lowest position (little-endian bit packing), per DWG spec.
    uint32_t result = 0;
    int shift = 0;
    for (int i = 0; i < 4; ++i) {  // Max 4 bytes = 28 bits
        if (m_error || remaining_bits() < 8) {
            m_error = true;
            return result;
        }
        uint8_t byte_val = read_u8();
        result |= static_cast<uint32_t>(byte_val & 0x7F) << shift;
        shift += 7;
        if ((byte_val & 0x80) == 0) {
            break;
        }
    }
    return result;
}

uint32_t DwgBitReader::read_modular_short()
{
    // Variable-length encoding using 16-bit LE words (RS).
    // High bit of each word means more words follow; the low 15 bits are
    // accumulated least-significant chunk first.
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < 4; ++i) {
        if (m_error || remaining_bits() < 16) {
            m_error = true;
            return static_cast<uint32_t>(result);
        }
        uint16_t word_val = read_rs();  // RS is little-endian, byte-aligned
        result |= static_cast<uint64_t>(word_val & 0x7FFF) << shift;
        if ((word_val & 0x8000) == 0) {
            break;
        }
        shift += 15;
        if (shift >= 32) {
            m_error = true;
            break;
        }
    }
    return static_cast<uint32_t>(result);
}

void DwgBitReader::read_2d_point(double& x, double& y)
{
    x = read_bd();
    y = read_bd();
}

void DwgBitReader::read_3d_point(double& x, double& y, double& z)
{
    x = read_bd();
    y = read_bd();
    z = read_bd();
}

bool DwgBitReader::has_error() const
{
    return m_error;
}

} // namespace cad
