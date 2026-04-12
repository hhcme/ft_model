#pragma once

#include "cad/cad_errors.h"
#include <string>
#include <istream>

namespace cad {

// A single DXF group code + value pair
struct DxfGroupCodeValue {
    int code = -1;
    std::string value;
    int line_number = 0;

    // Typed value accessors
    int as_int() const;
    float as_float() const;
    double as_double() const;
    std::string as_string() const { return value; }

    // Check if this is an entity type marker (group code 0)
    bool is_entity_marker() const { return code == 0; }
    // Check if this is a section start (code 0, value "SECTION")
    bool is_section_start() const { return code == 0 && value == "SECTION"; }
    // Check if this is a section end (code 0, value "ENDSEC")
    bool is_section_end() const { return code == 0 && value == "ENDSEC"; }
    // Check if this is EOF (code 0, value "EOF")
    bool is_eof() const { return code == 0 && value == "EOF"; }
};

// Reads DXF group code / value pairs from a stream
class DxfTokenizer {
public:
    explicit DxfTokenizer(std::istream& stream);

    // Read next group code / value pair
    ResultOf<bool> next();

    // Current pair
    const DxfGroupCodeValue& current() const { return m_current; }

    // Peek ahead without consuming
    ResultOf<bool> peek();
    const DxfGroupCodeValue& peeked() const { return m_peek; }
    bool has_peek() const { return m_has_peek; }

    // Invalidate peek cache (after modifying stream position)
    void invalidate_peek() { m_has_peek = false; }

    // Position
    int current_line() const { return m_line_number; }

private:
    std::istream& m_stream;
    DxfGroupCodeValue m_current;
    DxfGroupCodeValue m_peek;
    bool m_has_peek = false;
    int m_line_number = 0;

    bool read_next_pair(DxfGroupCodeValue& out);
    std::string read_line();
};

} // namespace cad
