#include "cad/parser/dxf_tokenizer.h"
#include <cstdlib>
#include <algorithm>

namespace cad {

// ============================================================
// DxfGroupCodeValue typed accessors
// ============================================================

int DxfGroupCodeValue::as_int() const {
    // DXF group code ranges determine type:
    // 60-79, 90-99, 370-379, 400-409, 420-429 → integer
    return std::atoi(value.c_str());
}

float DxfGroupCodeValue::as_float() const {
    return static_cast<float>(std::atof(value.c_str()));
}

double DxfGroupCodeValue::as_double() const {
    return std::atof(value.c_str());
}

// ============================================================
// DxfTokenizer
// ============================================================

DxfTokenizer::DxfTokenizer(std::istream& stream)
    : m_stream(stream)
{
}

std::string DxfTokenizer::read_line() {
    std::string line;
    if (!std::getline(m_stream, line)) {
        return "";
    }
    m_line_number++;

    // Trim trailing whitespace (CR, spaces)
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    // Trim leading whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    return line.substr(start);
}

bool DxfTokenizer::read_next_pair(DxfGroupCodeValue& out) {
    // Read group code line
    std::string code_line = read_line();
    if (code_line.empty() && !m_stream.good()) {
        return false; // EOF
    }

    // Parse group code
    int code = 0;
    try {
        size_t pos = 0;
        code = std::stoi(code_line, &pos);
        // Handle potential trailing text after the number
    } catch (...) {
        // Invalid group code — skip and try next
        return read_next_pair(out);
    }

    // Read value line
    std::string val_line = read_line();

    out.code = code;
    out.value = val_line;
    out.line_number = m_line_number - 1; // Point to code line
    return true;
}

ResultOf<bool> DxfTokenizer::next() {
    if (m_has_peek) {
        m_current = m_peek;
        m_has_peek = false;
        return ResultOf<bool>::success(true);
    }

    if (!read_next_pair(m_current)) {
        return ResultOf<bool>::success(false); // EOF
    }
    return ResultOf<bool>::success(true);
}

ResultOf<bool> DxfTokenizer::peek() {
    if (m_has_peek) {
        return ResultOf<bool>::success(true);
    }

    if (!read_next_pair(m_peek)) {
        return ResultOf<bool>::success(false); // EOF
    }
    m_has_peek = true;
    return ResultOf<bool>::success(true);
}

} // namespace cad
