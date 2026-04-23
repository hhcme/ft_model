#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cad {

class SceneGraph;
class DwgParser;

// Internal: helper for reading TU (UTF-16LE) strings from a DWG bitstream
// at a specific bit offset. Used by parse_classes to read class name records.
class SectionStringReader {
public:
    SectionStringReader(const uint8_t* data, size_t size, size_t bit_pos)
        : m_data(data), m_size(size), m_bit_pos(bit_pos) {}

    bool has_error() const { return m_error; }
    size_t bit_offset() const { return m_bit_pos; }

    std::string read_tu();

private:
    uint8_t read_raw_char();
    uint16_t read_rs();
    uint16_t read_bs();

    static void append_utf8(std::string& out, uint16_t ch);

    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_bit_pos = 0;
    bool m_error = false;
};

} // namespace cad
