// DwgBitReader — text encoding and R2007+ string stream.
// Split from dwg_reader.cpp to keep file sizes under 1000 lines.

#include "cad/parser/dwg_reader.h"

#include "cad/parser/dwg_parser.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifdef __has_include
#if __has_include(<iconv.h>)
#include <iconv.h>
#define HAS_ICONV 1
#endif
#endif

namespace cad {

namespace {

#ifdef HAS_ICONV
std::string convert_encoding(const std::string& raw, const char* from_encoding) {
    if (raw.empty()) return raw;
    bool has_high = false;
    for (unsigned char c : raw) {
        if (c >= 0x80) { has_high = true; break; }
    }
    if (!has_high) return raw;

    iconv_t cd = iconv_open("UTF-8", from_encoding);
    if (cd == (iconv_t)-1) return raw;

    char* in_buf = const_cast<char*>(raw.data());
    size_t in_left = raw.size();
    std::string out;
    out.reserve(raw.size() * 2);
    char out_buf[256];
    while (in_left > 0) {
        char* out_ptr = out_buf;
        size_t out_left = sizeof(out_buf);
        size_t rc = iconv(cd, &in_buf, &in_left, &out_ptr, &out_left);
        out.append(out_buf, out_ptr - out_buf);
        if (rc == (size_t)-1 && errno != E2BIG) break;
    }
    iconv_close(cd);
    return out;
}
#endif

std::string tv_to_utf8(const std::string& raw) {
    if (raw.empty()) return raw;
#ifdef HAS_ICONV
    bool looks_like_gbk = false;
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        uint8_t b = static_cast<uint8_t>(raw[i]);
        uint8_t b2 = static_cast<uint8_t>(raw[i + 1]);
        if (b >= 0x81 && b <= 0xFE && b2 >= 0x40 && b2 <= 0xFE) {
            looks_like_gbk = true;
            break;
        }
    }
    if (looks_like_gbk) {
        return convert_encoding(raw, "GBK");
    }
    bool has_high = false;
    for (unsigned char c : raw) {
        if (c >= 0x80) { has_high = true; break; }
    }
    if (has_high) {
        return convert_encoding(raw, "ISO-8859-1");
    }
#endif
    return raw;
}

} // namespace

// ============================================================
// Text reads (TV / TU / T)
// ============================================================

std::string DwgBitReader::read_tv()
{
    uint32_t length = m_use_string_stream ? str_read_bl() : read_bl();
    if (m_error) {
        return {};
    }

    if (length > 32768) {
        m_error = true;
        return {};
    }

    std::string result;
    result.reserve(length);

    if (m_use_string_stream) {
        for (uint32_t i = 0; i < length && !m_error; ++i) {
            result.push_back(static_cast<char>(str_read_raw_char()));
        }
    } else {
        if (remaining_bits() < static_cast<size_t>(length) * 8) {
            m_error = true;
            return {};
        }
        for (uint32_t i = 0; i < length; ++i) {
            result.push_back(static_cast<char>(read_raw_char()));
        }
    }

    auto decoded = tv_to_utf8(result);
    // DWG TV length includes the null terminator — strip it.
    while (!decoded.empty() && decoded.back() == '\0') {
        decoded.pop_back();
    }
    return decoded;
}

std::string DwgBitReader::read_tu()
{
    uint16_t length = m_use_string_stream ? str_read_bs() : read_bs();
    if (m_error || length == 0) {
        return {};
    }

    if (length > 32768) {
        m_error = true;
        return {};
    }

    std::string result;
    result.reserve(length);

    auto read_char_pair = [&]() -> uint16_t {
        uint8_t lo, hi;
        if (m_use_string_stream) {
            lo = str_read_raw_char();
            hi = str_read_raw_char();
        } else {
            if (remaining_bits() < 16) { m_error = true; return 0; }
            lo = read_raw_char();
            hi = read_raw_char();
        }
        return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
    };

    for (uint32_t i = 0; i < length && !m_error; ++i) {
        uint16_t ch = read_char_pair();

        if (ch < 0x80) {
            result.push_back(static_cast<char>(ch));
        } else if (ch >= 0x80 && ch < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }

    // DWG TU length may include a trailing null code unit — strip it.
    while (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::string DwgBitReader::read_t()
{
    if (m_use_string_stream) {
        return read_tu();
    }
    if (m_is_r2007_plus) {
        return {};
    }
    return read_tv();
}

// ============================================================
// R2007+ String Stream
// ============================================================

void DwgBitReader::setup_string_stream(uint32_t bitsize)
{
    m_is_r2007_plus = true;
    m_use_string_stream = false;
    if (bitsize == 0 || bitsize > m_size * 8) return;

    size_t saved_pos = m_bit_pos;
    bool saved_error = m_error;

    uint32_t start = bitsize - 1;
    m_bit_pos = start;
    if (remaining_bits() == 0) {
        m_bit_pos = saved_pos;
        m_error = saved_error;
        return;
    }
    uint8_t has_strings = read_bit();

    if (!has_strings) {
        m_bit_pos = saved_pos;
        m_error = saved_error;
        return;
    }

    if (bitsize < 17) {
        m_bit_pos = saved_pos;
        m_error = saved_error;
        return;
    }
    m_bit_pos = bitsize - 17;
    uint32_t data_size = read_rs();

    if (!m_error && (data_size & 0x8000)) {
        if (m_bit_pos < 32) {
            m_bit_pos = saved_pos;
            m_error = saved_error;
            return;
        }
        m_bit_pos -= 32;
        uint16_t hi = read_rs();
        data_size = ((data_size & 0x7FFFu) << 15) | (hi & 0x7FFFu);
    }

    if (m_error || data_size == 0 || data_size >= bitsize) {
        m_bit_pos = saved_pos;
        m_error = saved_error;
        return;
    }

    m_bit_pos = bitsize - 17;
    if (m_bit_pos < data_size) {
        m_bit_pos = saved_pos;
        m_error = saved_error;
        return;
    }
    uint32_t str_start = bitsize - 17 - data_size;

    m_str_data = m_data;
    m_str_size = m_size;
    m_str_bit_pos = str_start;
    m_use_string_stream = true;

    m_bit_pos = saved_pos;
    m_error = saved_error;
}

void DwgBitReader::clear_string_stream()
{
    m_use_string_stream = false;
    m_is_r2007_plus = false;
    m_str_data = nullptr;
    m_str_size = 0;
    m_str_bit_pos = 0;
}

uint8_t DwgBitReader::str_read_raw_char()
{
    if (!m_str_data || m_str_bit_pos + 8 > m_str_size * 8) {
        m_error = true;
        return 0;
    }
    size_t byte_idx = m_str_bit_pos >> 3;
    size_t bit_idx = m_str_bit_pos & 7;
    uint8_t result;
    if (bit_idx == 0) {
        result = m_str_data[byte_idx];
    } else {
        result = static_cast<uint8_t>(m_str_data[byte_idx] << bit_idx);
        if (byte_idx + 1 < m_str_size) {
            result |= m_str_data[byte_idx + 1] >> (8 - bit_idx);
        }
    }
    m_str_bit_pos += 8;
    return result;
}

uint16_t DwgBitReader::str_read_rs()
{
    uint8_t b0 = str_read_raw_char();
    uint8_t b1 = str_read_raw_char();
    return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint16_t DwgBitReader::str_read_bs()
{
    if (!m_str_data || m_str_bit_pos + 2 > m_str_size * 8) {
        m_error = true;
        return 0;
    }
    uint8_t code = 0;
    for (int i = 0; i < 2; ++i) {
        size_t byte_idx = m_str_bit_pos >> 3;
        size_t bit_idx = 7 - (m_str_bit_pos & 7);
        code = static_cast<uint8_t>((code << 1) | ((m_str_data[byte_idx] >> bit_idx) & 1));
        ++m_str_bit_pos;
    }
    switch (code) {
        case 0: return str_read_rs();
        case 1: return str_read_raw_char();
        case 2: return 0;
        case 3: return 256;
        default: return 0;
    }
}

uint32_t DwgBitReader::str_read_bl()
{
    if (!m_str_data || m_str_bit_pos + 2 > m_str_size * 8) {
        m_error = true;
        return 0;
    }
    uint8_t code = 0;
    for (int i = 0; i < 2; ++i) {
        size_t byte_idx = m_str_bit_pos >> 3;
        size_t bit_idx = 7 - (m_str_bit_pos & 7);
        code = static_cast<uint8_t>((code << 1) | ((m_str_data[byte_idx] >> bit_idx) & 1));
        ++m_str_bit_pos;
    }
    switch (code) {
        case 0: {
            uint32_t r = 0;
            for (int i = 0; i < 4; ++i)
                r |= static_cast<uint32_t>(str_read_raw_char()) << (i * 8);
            return r;
        }
        case 1: return str_read_raw_char();
        case 2: return 0;
        case 3: m_error = true; return 0;
        default: return 0;
    }
}

} // namespace cad
