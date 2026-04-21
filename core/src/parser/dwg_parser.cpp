#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_objects.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cad {

namespace {

bool dwg_debug_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("FT_DWG_DEBUG");
        return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void dwg_debug_log(const char* fmt, ...)
{
    if (!dwg_debug_enabled()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

std::string uppercase_ascii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains_ascii_ci(const std::string& text, const char* needle)
{
    return uppercase_ascii(text).find(uppercase_ascii(needle ? needle : "")) != std::string::npos;
}

std::vector<std::string> extract_printable_strings(const uint8_t* data,
                                                   size_t size,
                                                   size_t limit,
                                                   size_t min_length = 4)
{
    std::vector<std::string> out;
    auto push = [&](std::string s) {
        if (s.size() >= min_length && out.size() < limit) {
            out.push_back(std::move(s));
        }
    };

    std::string ascii;
    for (size_t i = 0; i < size && out.size() < limit; ++i) {
        const uint8_t c = data[i];
        if (c >= 32 && c <= 126) {
            ascii.push_back(static_cast<char>(c));
        } else {
            push(ascii);
            ascii.clear();
        }
    }
    push(ascii);

    std::string utf16;
    for (size_t i = 0; i + 1 < size && out.size() < limit; i += 2) {
        const uint8_t lo = data[i];
        const uint8_t hi = data[i + 1];
        if (hi == 0 && lo >= 32 && lo <= 126) {
            utf16.push_back(static_cast<char>(lo));
        } else {
            push(utf16);
            utf16.clear();
        }
    }
    push(utf16);

    return out;
}

std::vector<std::pair<double, double>> extract_plausible_raw_points(const uint8_t* data,
                                                                    size_t size,
                                                                    size_t limit)
{
    std::vector<std::pair<double, double>> out;
    auto read_le_double = [&](size_t offset) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<uint64_t>(data[offset + static_cast<size_t>(i)]) << (i * 8);
        }
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    for (size_t i = 0; i + 15 < size && out.size() < limit; ++i) {
        const double x = read_le_double(i);
        const double y = read_le_double(i + 8);
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        if (std::abs(x) > 1.0e7 || std::abs(y) > 1.0e7) continue;
        if (std::abs(x) < 1.0e-6 && std::abs(y) < 1.0e-6) continue;
        if (std::abs(x) < 1.0 && std::abs(y) < 1.0) continue;
        out.push_back({x, y});
    }
    return out;
}

struct RawPointCandidate {
    size_t offset = 0;
    double x = 0.0;
    double y = 0.0;
};

struct RawSmallIntCandidate {
    size_t offset = 0;
    uint32_t value = 0;
    uint8_t bytes = 0;
};

std::vector<RawSmallIntCandidate> extract_small_int_candidates(const uint8_t* data,
                                                               size_t size,
                                                               size_t limit)
{
    std::vector<RawSmallIntCandidate> out;
    auto push_unique = [&](size_t offset, uint32_t value, uint8_t bytes) {
        if (value == 0 || value > 99 || out.size() >= limit) return;
        for (const auto& existing : out) {
            if (existing.offset == offset && existing.value == value && existing.bytes == bytes) {
                return;
            }
        }
        out.push_back({offset, value, bytes});
    };

    for (size_t i = 0; i + 1 < size && out.size() < limit; ++i) {
        const uint32_t v16 = static_cast<uint32_t>(data[i]) |
                             (static_cast<uint32_t>(data[i + 1]) << 8);
        push_unique(i, v16, 2);
    }
    for (size_t i = 0; i + 3 < size && out.size() < limit; ++i) {
        const uint32_t v32 = static_cast<uint32_t>(data[i]) |
                             (static_cast<uint32_t>(data[i + 1]) << 8) |
                             (static_cast<uint32_t>(data[i + 2]) << 16) |
                             (static_cast<uint32_t>(data[i + 3]) << 24);
        push_unique(i, v32, 4);
    }
    return out;
}

std::vector<std::string> select_auxiliary_section_markers(
    const std::vector<std::string>& strings,
    size_t limit)
{
    std::vector<std::string> markers;
    auto priority = [](const std::string& value) {
        const std::string upper = uppercase_ascii(value);
        if (upper.find("DATIDX") != std::string::npos) return 0;
        if (upper.find("SEGIDX") != std::string::npos) return 1;
        if (upper.find("PRVSAV") != std::string::npos) return 2;
        if (upper.find("JARD") != std::string::npos) return 3;
        if (upper.find("IDX") != std::string::npos) return 4;
        return 5;
    };

    std::vector<std::string> candidates;
    for (const std::string& value : strings) {
        if (value.size() < 3 || value.size() > 48) continue;
        if (priority(value) >= 5) continue;
        std::string marker = value;
        const std::string upper = uppercase_ascii(value);
        if (upper.find("DATIDX") != std::string::npos) marker = "datidx";
        else if (upper.find("SEGIDX") != std::string::npos) marker = "segidx";
        else if (upper.find("PRVSAV") != std::string::npos) marker = "prvsav";
        else if (upper.find("SCHIDX") != std::string::npos) marker = "schidx";
        else if (upper.find("JARD") != std::string::npos) marker = "jard";
        bool duplicate = false;
        for (const std::string& existing : candidates) {
            if (existing == marker) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            candidates.push_back(marker);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](const std::string& a, const std::string& b) {
                  const int pa = priority(a);
                  const int pb = priority(b);
                  if (pa != pb) return pa < pb;
                  return a < b;
              });
    for (const std::string& value : candidates) {
        if (markers.size() >= limit) break;
        markers.push_back(value);
    }
    return markers;
}

std::vector<RawPointCandidate> extract_plausible_raw_points_with_offsets(
    const uint8_t* data,
    size_t size,
    size_t limit)
{
    std::vector<RawPointCandidate> out;
    auto read_le_double = [&](size_t offset) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<uint64_t>(data[offset + static_cast<size_t>(i)]) << (i * 8);
        }
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    for (size_t i = 0; i + 15 < size && out.size() < limit; ++i) {
        const double x = read_le_double(i);
        const double y = read_le_double(i + 8);
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        if (std::abs(x) > 1.0e7 || std::abs(y) > 1.0e7) continue;
        if (std::abs(x) < 1.0e-6 && std::abs(y) < 1.0e-6) continue;
        if (std::abs(x) < 1.0 && std::abs(y) < 1.0) continue;
        out.push_back({i, x, y});
    }
    return out;
}

std::string select_annotation_text_snippet(const std::vector<std::string>& snippets)
{
    std::string best;
    size_t best_alpha = 0;
    for (const std::string& snippet : snippets) {
        size_t alpha = 0;
        size_t printable = 0;
        for (unsigned char c : snippet) {
            if (std::isalpha(c)) alpha++;
            if (c >= 32 && c <= 126) printable++;
        }
        if (alpha < 8) continue;
        if (printable * 2 < snippet.size()) continue;
        const bool prose_like = snippet.find(' ') != std::string::npos ||
                                snippet.find("\\P") != std::string::npos;
        if (!prose_like) continue;
        if (alpha > best_alpha || (alpha == best_alpha && snippet.size() > best.size())) {
            best = snippet;
            best_alpha = alpha;
        }
    }
    return best;
}

bool select_annotation_anchor(const std::vector<std::pair<double, double>>& points,
                              Vec3& anchor)
{
    for (const auto& [x, y] : points) {
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        if (std::abs(x) < 100.0 || std::abs(y) < 100.0) continue;
        if (std::abs(x) > 1000000.0 || std::abs(y) > 1000000.0) continue;
        anchor = Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f};
        return true;
    }
    return false;
}

bool is_annotation_world_point(double x, double y)
{
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x) >= 100.0 && std::abs(y) >= 100.0 &&
           std::abs(x) <= 1000000.0 && std::abs(y) <= 1000000.0;
}

std::vector<Vec3> unique_annotation_world_points(
    const std::vector<std::pair<double, double>>& points,
    size_t limit)
{
    std::vector<Vec3> out;
    for (const auto& [x, y] : points) {
        if (!is_annotation_world_point(x, y)) continue;

        Vec3 candidate{static_cast<float>(x), static_cast<float>(y), 0.0f};
        bool duplicate = false;
        for (const Vec3& existing : out) {
            const float dx = existing.x - candidate.x;
            const float dy = existing.y - candidate.y;
            if ((dx * dx + dy * dy) < 1.0f) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        out.push_back(candidate);
        if (out.size() >= limit) break;
    }
    return out;
}

std::vector<Vec3> select_annotation_leader_path(
    const std::vector<std::pair<double, double>>& points)
{
    std::vector<Vec3> candidates = unique_annotation_world_points(points, 24);
    std::vector<Vec3> path;
    if (candidates.size() < 2) return path;

    path.push_back(candidates[0]);
    float reference_len = 0.0f;
    for (size_t i = 1; i < candidates.size(); ++i) {
        const Vec3& prev = path.back();
        const Vec3& next = candidates[i];
        const float dx = next.x - prev.x;
        const float dy = next.y - prev.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(len) || len < 1.0f) continue;
        if (len > 10000.0f) break;

        if (reference_len <= 0.0f && len >= 20.0f) {
            reference_len = len;
        }
        const float max_step = std::max(reference_len * 2.5f, 250.0f);
        if (path.size() >= 2 && len > max_step) {
            break;
        }

        path.push_back(next);
        if (path.size() >= 6) break;
    }

    if (path.size() < 2) path.clear();
    return path;
}

bool select_annotation_callout_proxy(
    const std::vector<std::pair<double, double>>& points,
    const std::vector<Vec3>& leader_path,
    Vec3& callout_center,
    Vec3& callout_target)
{
    if (leader_path.size() < 2) return false;

    const std::vector<Vec3> candidates = unique_annotation_world_points(points, 64);
    float best_score = -1.0f;
    Vec3 best_center = leader_path.front();

    for (const Vec3& candidate : candidates) {
        std::vector<Vec3> near_points;
        near_points.reserve(8);
        float max_near_dist = 0.0f;
        for (const Vec3& other : candidates) {
            const float dx = other.x - candidate.x;
            const float dy = other.y - candidate.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (!std::isfinite(dist) || dist > 58.0f) continue;
            near_points.push_back(other);
            max_near_dist = std::max(max_near_dist, dist);
        }

        // Mechanical callout bubbles tend to appear as a compact cluster of
        // center/perimeter/landing points inside the custom object payload.
        if (near_points.size() < 3) continue;

        Vec3 center = candidate;
        float best_center_balance = 1.0e9f;
        for (const Vec3& possible_center : near_points) {
            float balance = 0.0f;
            for (const Vec3& p : near_points) {
                const float dx = p.x - possible_center.x;
                const float dy = p.y - possible_center.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (std::isfinite(dist)) {
                    balance += dist;
                }
            }
            if (balance < best_center_balance) {
                best_center_balance = balance;
                center = possible_center;
            }
        }

        float nearest_far = 1.0e9f;
        for (const Vec3& p : leader_path) {
            const float dx = p.x - center.x;
            const float dy = p.y - center.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (std::isfinite(dist) && dist > 70.0f) {
                nearest_far = std::min(nearest_far, dist);
            }
        }
        if (nearest_far == 1.0e9f) continue;

        const float score =
            static_cast<float>(near_points.size()) * 1000.0f -
            max_near_dist * 8.0f -
            nearest_far * 0.02f;
        if (score > best_score) {
            best_score = score;
            best_center = center;
        }
    }

    callout_center = best_score > 0.0f ? best_center : leader_path.front();

    float best_target_dist = -1.0f;
    callout_target = leader_path.size() > 1 ? leader_path[1] : leader_path.front();
    for (const Vec3& p : leader_path) {
        const float dx = p.x - callout_center.x;
        const float dy = p.y - callout_center.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (std::isfinite(dist) && dist > best_target_dist) {
            best_target_dist = dist;
            callout_target = p;
        }
    }

    return true;
}

float datum_callout_radius(const Vec3& callout_point, const Vec3& target_point)
{
    const float dx = target_point.x - callout_point.x;
    const float dy = target_point.y - callout_point.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(len) || len <= 0.0f) {
        return 28.0f;
    }
    return std::clamp(len * 0.06f, 18.0f, 42.0f);
}

std::vector<std::string> read_custom_t_strings(
    const uint8_t* entity_data,
    size_t entity_data_bytes,
    size_t main_data_bits,
    size_t start_bit,
    size_t limit)
{
    std::vector<std::string> out;
    if (!entity_data || entity_data_bytes == 0 || main_data_bits == 0 ||
        start_bit >= main_data_bits) {
        return out;
    }

    DwgBitReader probe(entity_data, entity_data_bytes);
    probe.set_bit_limit(main_data_bits);
    probe.setup_string_stream(static_cast<uint32_t>(main_data_bits));
    probe.set_bit_offset(start_bit);

    for (size_t i = 0; i < limit && !probe.has_error(); ++i) {
        std::string value = probe.read_t();
        if (probe.has_error()) break;
        if (value.empty()) continue;
        bool duplicate = false;
        for (const std::string& existing : out) {
            if (existing == value) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) out.push_back(std::move(value));
    }
    return out;
}

class SectionStringReader {
public:
    SectionStringReader(const uint8_t* data, size_t size, size_t bit_pos)
        : m_data(data), m_size(size), m_bit_pos(bit_pos) {}

    bool has_error() const { return m_error; }
    size_t bit_offset() const { return m_bit_pos; }

    std::string read_tu()
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

private:
    uint8_t read_raw_char()
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

    uint16_t read_rs()
    {
        const uint8_t lo = read_raw_char();
        const uint8_t hi = read_raw_char();
        return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
    }

    uint16_t read_bs()
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

    static void append_utf8(std::string& out, uint16_t ch)
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

    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_bit_pos = 0;
    bool m_error = false;
};

} // namespace

// ============================================================
// DwgParser — constructor / destructor
// ============================================================

DwgParser::DwgParser()  = default;
DwgParser::~DwgParser() = default;

// ============================================================
// parse_file — read entire file into memory, delegate to parse_buffer
// ============================================================

Result DwgParser::parse_file(const std::string& filepath, SceneGraph& scene)
{
    std::ifstream ifs(filepath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Result::error(ErrorCode::FileNotFound, "Cannot open DWG: " + filepath);
    }
    auto sz = ifs.tellg();
    if (sz <= 0) {
        return Result::error(ErrorCode::FileReadError, "DWG file is empty");
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), buf.size())) {
        return Result::error(ErrorCode::FileReadError, "Failed to read DWG file");
    }
    return parse_buffer(buf.data(), buf.size(), scene);
}

// ============================================================
// parse_buffer — main parsing pipeline
// ============================================================

Result DwgParser::parse_buffer(const uint8_t* data, size_t size, SceneGraph& scene)
{
    if (!data || size < 0x100) {
        return Result::error(ErrorCode::InvalidFormat, "DWG data too small");
    }

    Result r;

    r = read_version(data, size);
    if (!r) return r;

    r = decrypt_r2004_header(data, size);
    if (!r) return r;

    r = read_section_page_map(data, size);
    if (!r) return r;

    r = read_section_info(data, size);
    if (!r) {
        // Fallback: try to discover sections by scanning page headers
        r = build_sections_from_page_headers(data, size);
        if (!r) return r;
    }

    r = read_sections(data, size);
    if (!r) return r;

    r = parse_header_variables(scene);
    if (!r) return r;

    r = parse_classes(scene);
    if (!r) return r;

    record_auxiliary_section_diagnostics(scene);

    r = parse_object_map(data, size);
    if (!r) return r;

    r = parse_objects(scene);
    if (!r) return r;

    return Result::success();
}

// ============================================================
// read_version — parse the version string at offset 0
// ============================================================

Result DwgParser::read_version(const uint8_t* data, size_t size)
{
    if (size < 6) {
        return Result::error(ErrorCode::InvalidFormat, "DWG data too small for version");
    }

    char ver[7] = {};
    std::memcpy(ver, data, 6);

    if (std::strcmp(ver, "AC1018") == 0)      m_version = DwgVersion::R2004;
    else if (std::strcmp(ver, "AC1021") == 0)  m_version = DwgVersion::R2007;
    else if (std::strcmp(ver, "AC1024") == 0)  m_version = DwgVersion::R2010;
    else if (std::strcmp(ver, "AC1027") == 0)  m_version = DwgVersion::R2013;
    else if (std::strcmp(ver, "AC1032") == 0)  m_version = DwgVersion::R2018;
    else if (std::strcmp(ver, "AC1015") == 0)  m_version = DwgVersion::R2000;
    else {
        return Result::error(ErrorCode::InvalidFormat,
                             std::string("Unsupported DWG version: ") + ver);
    }

    dwg_debug_log("[DWG] version=%s (enum=%d)\n", ver, static_cast<int>(m_version));
    return Result::success();
}

// ============================================================
// decrypt_r2004_header — decrypt 108-byte header at offset 0x80
// ============================================================

Result DwgParser::decrypt_r2004_header(const uint8_t* data, size_t size)
{
    // R2000 does not have the encrypted 108-byte header
    if (m_version < DwgVersion::R2004) {
        return Result::success();
    }

    if (size < 0x80 + 108) {
        return Result::error(ErrorCode::InvalidFormat, "DWG data too small for R2004 header");
    }

    // Derive XOR key from first 16 file bytes (bytes 0..15)
    uint8_t key[16];
    std::memcpy(key, data, 16);

    // Decrypt 108 bytes at offset 0x80
    uint8_t decrypted[108];
    dwg_decrypt_header(data + 0x80, decrypted, 108, key);

    // Parse into m_file_header
    auto& h = m_file_header;
    std::memcpy(h.file_id_string, decrypted, 12);
    h.file_id_string[12] = '\0';

    h.header_address     = read_le32(decrypted, 0x0C);
    h.header_size        = read_le32(decrypted, 0x10);
    h.x04                = read_le32(decrypted, 0x14);
    h.root_tree_node_gap  = static_cast<int32_t>(read_le32(decrypted, 0x18));
    h.lowermost_left_gap  = static_cast<int32_t>(read_le32(decrypted, 0x1C));
    h.lowermost_right_gap = static_cast<int32_t>(read_le32(decrypted, 0x20));
    h.unknown_long       = read_le32(decrypted, 0x24);
    h.last_section_id    = read_le32(decrypted, 0x28);
    h.last_section_address    = read_le48(decrypted, 0x2C);
    h.secondheader_address    = read_le48(decrypted, 0x34);
    h.numgaps            = read_le32(decrypted, 0x3C);
    h.numsections        = read_le32(decrypted, 0x40);
    h.x20                = read_le32(decrypted, 0x44);
    h.x80                = read_le32(decrypted, 0x48);
    h.x40                = read_le32(decrypted, 0x4C);
    h.section_map_id     = read_le32(decrypted, 0x50);
    h.section_map_address = read_le48(decrypted, 0x54);
    h.section_info_id    = static_cast<int32_t>(read_le32(decrypted, 0x5C));
    h.section_array_size = read_le32(decrypted, 0x60);
    h.gap_array_size     = read_le32(decrypted, 0x64);
    h.crc32              = read_le32(decrypted, 0x68);

    dwg_debug_log("[DWG] header_id='%.12s' header_size=%u section_map_id=%u "
            "section_map_addr=0x%llx section_info_id=%d numsections=%u\n",
            h.file_id_string, h.header_size, h.section_map_id,
            (unsigned long long)h.section_map_address,
            h.section_info_id, h.numsections);

    return Result::success();
}

// ============================================================
// read_section_page_map — decompress and parse page map entries
//
// The page map is at section_map_address + 0x100 in the file.
// The page map header is NOT encrypted (unlike regular section pages).
// It uses a 20-byte header:
//   [0x00] section_type (RL) — should be 0x41630e3b
//   [0x04] decomp_data_size (RL) — decompressed size
//   [0x08] comp_data_size (RL) — compressed data size
//   [0x0C] compression_type (RL) — 2 = LZ77 compressed
//   [0x10] checksum (RL)
// Compressed data starts at offset 20 within the header.
// Decompress using dwg_decompress.
// Parse entries: each entry is 2 modular shorts (section_number, section_size).
// Running address accumulates: each entry's address = previous_address + previous_size.
// ============================================================

Result DwgParser::read_section_page_map(const uint8_t* data, size_t size)
{
    if (m_version < DwgVersion::R2004) {
        return Result::error(ErrorCode::InvalidFormat, "R2000 DWG not yet supported");
    }

    // Section page map is at section_map_address + 0x100 in the file.
    uint64_t map_file_offset = m_file_header.section_map_address + 0x100;
    if (map_file_offset + 20 > size) {
        return Result::error(ErrorCode::InvalidFormat, "Section page map out of bounds");
    }

    // The section page map header is NOT encrypted — read raw bytes.
    // Per libredwg: section_type should be 0x41630e3b
    uint32_t section_type   = read_le32(data + map_file_offset, 0x00);
    uint32_t decomp_size    = read_le32(data + map_file_offset, 0x04);
    uint32_t comp_data_size = read_le32(data + map_file_offset, 0x08);
    uint32_t compression_type = read_le32(data + map_file_offset, 0x0C);
    uint32_t checksum       = read_le32(data + map_file_offset, 0x10);

    dwg_debug_log("[DWG] page_map_header: section_type=0x%08X decomp=%u comp=%u type=%u\n",
            section_type, decomp_size, comp_data_size, compression_type);

    if (section_type != 0x41630e3b) {
        return Result::error(ErrorCode::InvalidFormat, "Invalid section page map tag");
    }
    if (comp_data_size == 0 || decomp_size == 0) {
        return Result::error(ErrorCode::InvalidFormat, "Invalid page map sizes");
    }
    // Compressed data starts at offset 20 within the header
    if (map_file_offset + 20 + comp_data_size > size) {
        return Result::error(ErrorCode::InvalidFormat, "Page map data extends beyond file");
    }

    // Decompress
    auto [decompressed, actual] = dwg_decompress(
        data + map_file_offset + 20, comp_data_size, decomp_size);

    if (actual == 0 && decomp_size > 0) {
        return Result::error(ErrorCode::ParseError, "Failed to decompress section page map");
    }

    // Parse section page map entries from decompressed data.
    // Per libredwg: each entry is 8 bytes: RL(number) + RL(size).
    // number is int32_t (can be negative for gaps), size is uint32_t.
    // Running address accumulates.
    const uint8_t* d = decompressed.data();
    size_t dsize = decompressed.size();
    size_t off = 0;
    int64_t running_address = 0x100;  // Starting address per libredwg

    m_page_map_entries.clear();

    while (off + 8 <= dsize) {
        // Read RL (int32_t) for section_number
        int32_t sec_num = static_cast<int32_t>(
            d[off] | (d[off+1] << 8) | (d[off+2] << 16) | (d[off+3] << 24));
        off += 4;

        // Read RL (uint32_t) for section_size
        uint32_t sec_size = d[off] | (d[off+1] << 8) | (d[off+2] << 16) | (d[off+3] << 24);
        off += 4;

        SectionPageMapEntry entry;
        entry.number  = sec_num;
        entry.size    = sec_size;
        entry.address = static_cast<uint64_t>(running_address);

        m_page_map_entries.push_back(entry);

        // Only add to running address if the number is within section_array_size
        if (sec_num >= 0 && static_cast<uint32_t>(sec_num) <= m_file_header.section_array_size) {
            running_address += sec_size;
        }
    }

    dwg_debug_log("[DWG] page_map: %zu entries\n", m_page_map_entries.size());
    return Result::success();
}

// ============================================================
// Helper: find page file offset from page map entry number
//
// Scans file page headers starting from section_map_address+0x100
// to find the page with the matching section_number.
// ============================================================

uint64_t DwgParser::find_page_file_offset_(const uint8_t* data, size_t file_size,
                                            int32_t target_page_number) const
{
    uint64_t scan = m_file_header.section_map_address + 0x100;

    // Skip the page map page itself (20-byte header + compressed data)
    if (scan + 20 <= file_size) {
        // Page map header is NOT encrypted
        uint32_t comp_size = read_le32(data + scan, 0x08);
        if (comp_size > 0 && comp_size < file_size) {
            scan += 20 + comp_size;
        } else {
            scan += 0x100;  // fallback skip
        }
    }

    for (size_t attempts = 0; attempts < 5000 && scan + 32 <= file_size; ++attempts) {
        uint8_t phdr[32];
        decrypt_section_page_header(data + scan, phdr, static_cast<size_t>(scan));

        int32_t pnum    = static_cast<int32_t>(read_le32(phdr, 0x04));
        uint32_t ds     = read_le32(phdr, 0x08);
        // Also check the tag field at 0x00 for validity
        uint32_t ptag   = read_le32(phdr, 0x00);

        if (pnum == target_page_number) {
            return scan;
        }

        // Sanity check: if tag is not a valid page header tag and data_size is
        // garbage, we've gone past the valid pages. Valid tags: 0x416C3044, 0x416C3045
        if (ds == 0 || ds > file_size) {
            // Try a fixed-size skip as a recovery measure
            scan += 0x100;
            continue;
        }

        scan += 32 + ds;
    }

    return static_cast<uint64_t>(-1);  // not found
}

// ============================================================
// read_section_info — find section info page, decompress, parse descriptors
//
// Find the section info page by looking for section_number matching
// section_info_id in the page map.
// The section info is at the file address from the page map entry.
// Read 32-byte encrypted page header, decompress.
// Parse section info: RL(num_descriptors), then for each:
//   RL(data_size) + RL(remaining) + RC(encrypted) + RC(compressed)
//   + RL(max_decomp_size) + 64-byte name + RL(num_sections)
//   + for each section: RL(page_number) + RL(data_size) + RL64(start_offset)
// ============================================================

Result DwgParser::read_section_info(const uint8_t* data, size_t size)
{
    // Find the page map entry with number == section_info_id
    // The section info page lives at the FILE OFFSET given by the page map entry's address.
    const SectionPageMapEntry* info_entry = nullptr;
    for (const auto& e : m_page_map_entries) {
        if (e.number == m_file_header.section_info_id) {
            info_entry = &e;
            break;
        }
    }
    if (!info_entry) {
        return Result::error(ErrorCode::InvalidFormat, "Section info page not found in page map");
    }

    // The address from the page map is the FILE OFFSET of the section info page.
    // Per libredwg: the section info page uses a 20-byte header (NOT encrypted),
    // same as the section page map, with tag 0x4163003b.
    uint64_t info_file_offset = info_entry->address;
    if (info_file_offset + 20 > size) {
        return Result::error(ErrorCode::InvalidFormat, "Section info page out of bounds");
    }

    // Read 20-byte header (NOT encrypted, like the page map header)
    uint32_t section_type     = read_le32(data + info_file_offset, 0x00);
    uint32_t decomp_data_size = read_le32(data + info_file_offset, 0x04);
    uint32_t comp_data_size   = read_le32(data + info_file_offset, 0x08);
    uint32_t compression_type = read_le32(data + info_file_offset, 0x0C);
    uint32_t checksum         = read_le32(data + info_file_offset, 0x10);

    dwg_debug_log("[DWG] section_info page: offset=0x%llx tag=0x%08X decomp=%u comp=%u type=%u\n",
            (unsigned long long)info_file_offset, section_type,
            decomp_data_size, comp_data_size, compression_type);

    // If the tag doesn't match the section info tag, try decrypting
    // as a regular 32-byte encrypted page header.
    if (section_type != 0x4163003b) {
        // Maybe this page has an encrypted 32-byte header instead.
        if (info_file_offset + 32 > size) {
            return Result::error(ErrorCode::InvalidFormat, "Section info page out of bounds for encrypted header");
        }
        uint8_t phdr[32];
        decrypt_section_page_header(data + info_file_offset, phdr,
                                    static_cast<size_t>(info_file_offset));

        // Encrypted page header layout:
        // [0x00] page_type (should be 0x4163043b)
        // [0x04] section_type (should be related to section_info_id)
        // [0x08] data_size (compressed)
        // [0x0C] page_size (decompressed)
        uint32_t page_type = read_le32(phdr, 0x00);
        uint32_t sec_type  = read_le32(phdr, 0x04);
        comp_data_size     = read_le32(phdr, 0x08);
        decomp_data_size   = read_le32(phdr, 0x0C);

        dwg_debug_log("[DWG] section_info encrypted page: page_type=0x%08X sec_type=%u data_size=%u decomp=%u\n",
                page_type, sec_type, comp_data_size, decomp_data_size);

        if (page_type != 0x4163043b) {
            return Result::error(ErrorCode::InvalidFormat, "Section info page has invalid encrypted header");
        }
        if (comp_data_size == 0 || decomp_data_size == 0) {
            return Result::error(ErrorCode::InvalidFormat, "Invalid section info page sizes");
        }
        if (info_file_offset + 32 + comp_data_size > size) {
            return Result::error(ErrorCode::InvalidFormat, "Section info data extends beyond file");
        }

        auto [info_data, actual] = dwg_decompress(
            data + info_file_offset + 32, comp_data_size, decomp_data_size);

        if (actual == 0 && decomp_data_size > 0) {
            return Result::error(ErrorCode::ParseError, "Failed to decompress section info");
        }

        return parse_section_info_data(std::move(info_data));
    }

    if (comp_data_size == 0 || decomp_data_size == 0) {
        return Result::error(ErrorCode::InvalidFormat, "Invalid section info sizes");
    }
    if (info_file_offset + 20 + comp_data_size > size) {
        return Result::error(ErrorCode::InvalidFormat, "Section info data extends beyond file");
    }

    // Decompress
    auto [info_data, actual] = dwg_decompress(
        data + info_file_offset + 20, comp_data_size, decomp_data_size);

    if (actual == 0 && decomp_data_size > 0) {
        return Result::error(ErrorCode::ParseError, "Failed to decompress section info");
    }

    return parse_section_info_data(std::move(info_data));
}

Result DwgParser::parse_section_info_data(std::vector<uint8_t> info_data)
{
    // Parse section info from decompressed data.
    // Global header (20 bytes):
    //   RL(num_desc), RL(compressed), RL(max_size), RL(encrypted), RL(num_desc2)
    DwgBitReader reader(info_data.data(), info_data.size());

    uint32_t num_desc    = reader.read_rl();  // number of descriptors
    uint32_t compressed  = reader.read_rl();
    uint32_t max_size    = reader.read_rl();
    uint32_t encrypted   = reader.read_rl();
    uint32_t num_desc2   = reader.read_rl();

    dwg_debug_log("[DWG] section_info: num_desc=%u compressed=%u max_size=%u encrypted=%u\n",
            num_desc, compressed, max_size, encrypted);

    m_section_infos.clear();
    if (num_desc > 200) num_desc = 200;  // sanity limit

    for (uint32_t d = 0; d < num_desc && !reader.has_error(); ++d) {
        // Each descriptor:
        //   RLL(size - 8 bytes), RL(num_sections), RL(max_decomp_size),
        //   RL(unknown), RL(compressed), RL(type), RL(encrypted),
        //   64 bytes(name)
        //   then for each section: RL(number), RL(size), RLL(address - 8 bytes)

        // RLL = 64-bit little-endian: low RL + high RL
        uint32_t size_lo  = reader.read_rl();
        uint32_t size_hi  = reader.read_rl();
        uint64_t desc_size = (static_cast<uint64_t>(size_hi) << 32) | size_lo;

        SectionInfoDesc desc;
        desc.size = desc_size;

        desc.num_sections    = reader.read_rl();
        desc.max_decomp_size = reader.read_rl();
        (void)reader.read_rl();  // unknown
        desc.compressed      = reader.read_rl();
        desc.type            = reader.read_rl();
        desc.encrypted       = reader.read_rl();

        // Read 64-byte name
        for (int c = 0; c < 64 && !reader.has_error(); ++c) {
            desc.name[c] = static_cast<char>(reader.read_raw_char());
        }
        desc.name[63] = '\0';

        // Sanity
        if (desc.num_sections > 100000) {
            dwg_debug_log("[DWG] WARNING: section '%s' has too many pages (%u), skipping\n",
                    desc.name, desc.num_sections);
            continue;
        }

        dwg_debug_log("[DWG] section_info[%u]: name='%.64s' pages=%u size=%llu "
                "compressed=%u encrypted=%u max_decomp=%u type=%u\n",
                d, desc.name, desc.num_sections,
                (unsigned long long)desc.size,
                desc.compressed, desc.encrypted, desc.max_decomp_size, desc.type);

        // Read page records
        for (uint32_t p = 0; p < desc.num_sections && !reader.has_error(); ++p) {
            SectionInfoDesc::PageInfo pi;
            pi.number  = static_cast<int32_t>(reader.read_rl());  // page number

            // data_size: RL — compressed data size for this page
            uint32_t page_data_size = reader.read_rl();
            pi.size = page_data_size;

            // start_offset: RLL (64-bit) = low RL + high RL
            uint32_t addr_lo = reader.read_rl();
            uint32_t addr_hi = reader.read_rl();
            pi.address = (static_cast<uint64_t>(addr_hi) << 32) | addr_lo;

            desc.pages.push_back(pi);
        }

        m_section_infos.push_back(std::move(desc));
    }

    if (m_section_infos.empty()) {
        return Result::error(ErrorCode::ParseError, "No section descriptors found");
    }

    return Result::success();
}

// ============================================================
// build_sections_from_page_headers — fallback when section info fails
// ============================================================

Result DwgParser::build_sections_from_page_headers(const uint8_t* data, size_t size)
{
    dwg_debug_log("[DWG] WARNING: section info failed, trying page header scan fallback\n");

    if (m_page_map_entries.empty()) {
        return Result::error(ErrorCode::InvalidFormat, "No page map entries for fallback");
    }

    // Group pages by section number found in page headers
    std::unordered_map<int32_t, SectionInfoDesc> section_map;

    uint64_t scan_offset = m_file_header.section_map_address + 0x100;
    // Skip page map page (20-byte header + compressed data, NOT encrypted)
    if (scan_offset + 20 <= size) {
        uint32_t comp_size = read_le32(data + scan_offset, 0x08);
        if (comp_size > 0 && comp_size < size) {
            scan_offset += 20 + comp_size;
        } else {
            scan_offset += 0x100;
        }
    }

    for (size_t i = 0; i < m_page_map_entries.size() + 100 && scan_offset + 32 <= size; ++i) {
        uint8_t phdr[32];
        decrypt_section_page_header(data + scan_offset, phdr, static_cast<size_t>(scan_offset));

        uint32_t ptag      = read_le32(phdr, 0x00);
        int32_t sec_num    = static_cast<int32_t>(read_le32(phdr, 0x04));
        uint32_t data_size = read_le32(phdr, 0x08);
        uint32_t decomp    = read_le32(phdr, 0x0C);

        // Skip invalid pages
        if (data_size == 0 || data_size > size || sec_num == 0) {
            scan_offset += 32 + data_size;
            continue;
        }

        auto& desc = section_map[sec_num];
        if (desc.pages.empty()) {
            desc.type = static_cast<uint32_t>(sec_num);
            desc.compressed = 2;
        }
        desc.num_sections++;

        SectionInfoDesc::PageInfo pi;
        pi.number  = sec_num;
        pi.size    = data_size;
        pi.address = desc.size;
        desc.pages.push_back(pi);
        desc.size += decomp;
        if (decomp > desc.max_decomp_size) desc.max_decomp_size = decomp;

        scan_offset += 32 + data_size;
    }

    m_section_infos.clear();
    for (auto& [num, desc] : section_map) {
        snprintf(desc.name, sizeof(desc.name), "Section_%d", num);
        m_section_infos.push_back(std::move(desc));
    }

    if (m_section_infos.empty()) {
        return Result::error(ErrorCode::ParseError, "Fallback: no sections found");
    }

    dwg_debug_log("[DWG] fallback: found %zu sections\n", m_section_infos.size());
    return Result::success();
}

// ============================================================
// read_sections — decompress and assemble all file sections
//
// For each section descriptor, find its pages in the page map.
// Decompress each page and assemble into the section buffer.
// Map sections: name "AcDb:Header" -> header_vars,
//   "AcDb:Classes" -> classes, "AcDb:ObjectMap" -> object_map,
//   rest -> object_data.
// Track object_data_file_offset.
// ============================================================

Result DwgParser::read_sections(const uint8_t* data, size_t size)
{
    m_sections = DwgFileSections{};

    // Identify sections by name
    SectionInfoDesc* sec_header_vars = nullptr;
    SectionInfoDesc* sec_classes     = nullptr;
    SectionInfoDesc* sec_object_map  = nullptr;
    std::vector<SectionInfoDesc*> sec_object_data_list;
    std::vector<SectionInfoDesc*> sec_auxiliary_list;

    for (auto& desc : m_section_infos) {
        std::string name(desc.name);
        if (name.find("AcDb:Header") != std::string::npos) {
            sec_header_vars = &desc;
        } else if (name.find("AcDb:Classes") != std::string::npos) {
            sec_classes = &desc;
        } else if (name.find("AcDb:ObjectMap") != std::string::npos ||
                   name.find("AcDb:Handles") != std::string::npos) {
            sec_object_map = &desc;
        } else if (name.find("AcDb:AcDbObjects") != std::string::npos ||
                   name.find("AcDb:Objects") != std::string::npos) {
            sec_object_data_list.push_back(&desc);
        } else if (contains_ascii_ci(name, "AcDb:AcDs") ||
                   contains_ascii_ci(name, "AcDsPrototype")) {
            sec_auxiliary_list.push_back(&desc);
        }
    }

    // If no sections matched by name, try by type/order
    if (!sec_header_vars || !sec_classes || !sec_object_map) {
        dwg_debug_log("[DWG] WARNING: section name matching incomplete, trying by order\n");
        if (m_section_infos.size() >= 3) {
            if (!sec_header_vars) sec_header_vars = &m_section_infos[0];
            if (!sec_classes)     sec_classes     = &m_section_infos[1];
            if (!sec_object_map) sec_object_map  = &m_section_infos[2];
            // Sections 3+ are object data
            for (size_t i = 3; i < m_section_infos.size(); ++i) {
                if (m_section_infos[i].pages.empty()) continue;
                // Check if it's not already assigned
                std::string name(m_section_infos[i].name);
                if (name.find("AcDb:Header") == std::string::npos &&
                    name.find("AcDb:Classes") == std::string::npos &&
                    name.find("AcDb:ObjectMap") == std::string::npos &&
                    !contains_ascii_ci(name, "AcDb:AcDs") &&
                    !contains_ascii_ci(name, "AcDsPrototype")) {
                    sec_object_data_list.push_back(&m_section_infos[i]);
                }
            }
        }
    }

    // Decompress a section into a buffer.
    // For each page in the section, look up its file offset from the page map,
    // then decrypt the page header, decompress the data, and append.
    auto decompress_section = [&](SectionInfoDesc& desc, bool track_object_pages = false) -> std::vector<uint8_t> {
        dwg_debug_log("[DWG] decompressing section '%s' (%zu pages)\n",
                desc.name, desc.pages.size());
        // Calculate total decompressed size from desc.size
        uint64_t total_size = desc.size;
        if (total_size == 0) {
            // Estimate from max_decomp_size * num_sections
            total_size = static_cast<uint64_t>(desc.max_decomp_size) * desc.num_sections;
        }

        if (total_size == 0 || total_size > 500 * 1024 * 1024) {
            dwg_debug_log("[DWG] WARNING: section '%s' has invalid total size %llu\n",
                    desc.name, (unsigned long long)total_size);
            return {};
        }

        std::vector<uint8_t> result(static_cast<size_t>(total_size), 0);
        size_t write_offset = 0;

        for (auto& page : desc.pages) {
            dwg_debug_log("[DWG]   page %d: addr=%llu size=%u\n",
                    page.number, (unsigned long long)page.address, page.size);
            // Look up the page's FILE OFFSET from the page map entries.
            // The section info gives us page NUMBER; the page map gives us ADDRESS (file offset).
            uint64_t page_file_offset = 0;
            bool found = false;
            for (const auto& pme : m_page_map_entries) {
                if (pme.number == page.number) {
                    page_file_offset = pme.address;
                    found = true;
                    break;
                }
            }

            if (!found || page_file_offset + 32 > size) {
                dwg_debug_log("[DWG] WARNING: page %d not found in page map\n", page.number);
                continue;
            }

            // Decrypt the 32-byte page header
            uint8_t phdr[32];
            decrypt_section_page_header(data + page_file_offset, phdr,
                                        static_cast<size_t>(page_file_offset));

            // Page header fields (decrypted):
            // [0x00] page_type (should be 0x4163043b)
            // [0x04] section_type
            // [0x08] data_size (compressed)
            // [0x0C] page_size (decompressed)
            // [0x10] address (start offset in decompressed section)
            uint32_t ptag = read_le32(phdr, 0x00);
            uint32_t ds = read_le32(phdr, 0x08);
            uint32_t page_decomp = read_le32(phdr, 0x0C);

            if (ptag != 0x4163043b) {
                dwg_debug_log("[DWG] WARNING: page %d at 0x%llx has bad tag 0x%08X\n",
                        page.number, (unsigned long long)page_file_offset, ptag);
                continue;
            }

            if (ds == 0 || page_file_offset + 32 + ds > size) {
                dwg_debug_log("[DWG] WARNING: page %d invalid data_size=%u\n", page.number, ds);
                continue;
            }

            size_t before_offset = static_cast<size_t>(page.address);

            if (desc.compressed == 2 && ds > 0) {
                // Ensure output buffer is large enough
                size_t target_offset = static_cast<size_t>(page.address);
                // Cap the decompressed output to max_decomp_size per page
                size_t max_output = target_offset + desc.max_decomp_size;
                if (max_output > result.size()) {
                    result.resize(max_output, 0);
                }
                size_t actual = dwg_decompress_into(
                    data + page_file_offset + 32, ds,
                    result.data(), max_output, target_offset);
                write_offset = target_offset + actual;
            } else {
                // Uncompressed — copy directly to page.address
                size_t target_offset = static_cast<size_t>(page.address);
                if (target_offset + ds > result.size()) {
                    result.resize(target_offset + ds, 0);
                }
                if (ds > 0) {
                    std::memcpy(result.data() + target_offset,
                               data + page_file_offset + 32, ds);
                    write_offset = target_offset + ds;
                }
            }

            // Track per-page info only for AcDbObjects. Other sections use
            // the same decompressor but their page-relative addresses are not
            // valid offsets into m_sections.object_data.
            size_t page_bytes = write_offset - before_offset;
            if (track_object_pages && page_bytes > 0) {
                m_sections.object_pages.emplace_back(page.address, page_bytes);
            }
        }

        // Resize to the actual section data size (from section info descriptor).
        // This trims zero-padded areas at the end of the last page.
        result.resize(static_cast<size_t>(total_size));
        return result;
    };

    if (sec_header_vars) {
        m_sections.header_vars = decompress_section(*sec_header_vars);
        dwg_debug_log("[DWG] header_vars: %zu bytes\n", m_sections.header_vars.size());
    }

    if (sec_classes) {
        m_sections.classes = decompress_section(*sec_classes);
        dwg_debug_log("[DWG] classes: %zu bytes\n", m_sections.classes.size());
    }

    if (sec_object_map) {
        // Object map pages must be concatenated sequentially (no address gaps)
        // because parse_object_map expects a contiguous stream of RS_BE sections.
        m_sections.object_map.clear();
        m_sections.objmap_page_size = sec_object_map->max_decomp_size;
        dwg_debug_log("[DWG] decompressing object_map (%zu pages)\n", sec_object_map->pages.size());
        for (auto& page : sec_object_map->pages) {
            // Find file offset from page map
            uint64_t page_file_offset = 0;
            bool found = false;
            for (const auto& pme : m_page_map_entries) {
                if (pme.number == page.number) {
                    page_file_offset = pme.address;
                    found = true;
                    break;
                }
            }
            if (!found || page_file_offset + 32 > size) {
                dwg_debug_log("[DWG] WARNING: object_map page %d not found\n", page.number);
                continue;
            }

            // Decrypt the 32-byte page header
            uint8_t phdr[32];
            decrypt_section_page_header(data + page_file_offset, phdr,
                                        static_cast<size_t>(page_file_offset));

            uint32_t ptag = read_le32(phdr, 0x00);
            uint32_t ds   = read_le32(phdr, 0x08);

            if (ptag != 0x4163043b || ds == 0 || page_file_offset + 32 + ds > size) {
                dwg_debug_log("[DWG] WARNING: object_map page %d bad tag or size\n", page.number);
                continue;
            }

            if (sec_object_map->compressed == 2 && ds > 0) {
                auto [page_buf, actual] = dwg_decompress(
                    data + page_file_offset + 32, ds,
                    static_cast<size_t>(sec_object_map->max_decomp_size));
                if (!page_buf.empty()) {
                    m_sections.object_map.insert(m_sections.object_map.end(),
                                                 page_buf.begin(),
                                                 page_buf.begin() + actual);
                }
            } else {
                m_sections.object_map.insert(m_sections.object_map.end(),
                                             data + page_file_offset + 32,
                                             data + page_file_offset + 32 + ds);
            }
        }
        // Trim to the actual section data size; trailing bytes are page padding.
        if (m_sections.object_map.size() > static_cast<size_t>(sec_object_map->size)) {
            m_sections.object_map.resize(static_cast<size_t>(sec_object_map->size));
        }
        dwg_debug_log("[DWG] object_map: %zu bytes (page_size=%u)\n",
                m_sections.object_map.size(), m_sections.objmap_page_size);
    }

    // Merge all object data sections
    if (!sec_object_data_list.empty()) {
        size_t total_obj = 0;
        for (auto* s : sec_object_data_list) total_obj += s->size;
        m_sections.object_data.reserve(total_obj);

        size_t first_offset = static_cast<size_t>(-1);
        m_sections.object_pages.clear();
        for (auto* s : sec_object_data_list) {
            auto buf = decompress_section(*s, true);
            if (buf.empty()) continue;

            // Track the starting file offset for the first object data section
            if (first_offset == static_cast<size_t>(-1) && !s->pages.empty()) {
                // Look up the file offset of the first page from the page map
                const SectionPageMapEntry* pme = find_page_map_entry(s->pages[0].number);
                if (pme) {
                    first_offset = static_cast<size_t>(pme->address);
                }
            }

            size_t old_size = m_sections.object_data.size();
            m_sections.object_data.insert(m_sections.object_data.end(),
                                          buf.begin(), buf.end());
            // Adjust object_pages offsets
            for (auto& pp : m_sections.object_pages) {
                // Pages from this section already have their correct offsets
                // from decompress_section; no adjustment needed since we
                // track by absolute address.
            }
        }
        m_sections.object_data_file_offset = first_offset;

        dwg_debug_log("[DWG] object_data: %zu bytes (file_offset=%zu)\n",
                m_sections.object_data.size(), m_sections.object_data_file_offset);
    }

    for (auto* s : sec_auxiliary_list) {
        auto buf = decompress_section(*s, false);
        if (buf.empty()) continue;

        DwgAuxiliarySection aux;
        aux.name = s->name;
        aux.data = std::move(buf);
        dwg_debug_log("[DWG] auxiliary section '%s': %zu bytes\n",
                      aux.name.c_str(), aux.data.size());
        m_sections.auxiliary_sections.push_back(std::move(aux));
    }

    return Result::success();
}

void DwgParser::record_auxiliary_section_diagnostics(SceneGraph& scene) const
{
    for (const auto& aux : m_sections.auxiliary_sections) {
        const std::string upper_name = uppercase_ascii(aux.name);
        if (upper_name.find("ACDS") == std::string::npos &&
            upper_name.find("PROTOTYPE") == std::string::npos) {
            continue;
        }

        const bool is_prototype = upper_name.find("PROTOTYPE") != std::string::npos;
        std::string message = "DWG auxiliary section '";
        message += aux.name;
        message += "' was decoded (";
        message += std::to_string(aux.data.size());
        message += " bytes) but is not yet semantically interpreted. This often carries AutoCAD Mechanical/associative drawing-view presentation data.";
        const auto strings = extract_printable_strings(aux.data.data(), aux.data.size(), 64);
        const auto markers = select_auxiliary_section_markers(strings, 8);
        if (!markers.empty()) {
            message += " Markers: ";
            for (size_t i = 0; i < markers.size(); ++i) {
                if (i > 0) message += ", ";
                std::string marker = markers[i];
                if (marker.size() > 32) {
                    marker = marker.substr(0, 29) + "...";
                }
                message += "'";
                message += marker;
                message += "'";
            }
        }

        scene.add_diagnostic({
            is_prototype ? "dwg_acds_prototype_deferred" : "dwg_acds_section_deferred",
            "Semantic gap",
            message,
            static_cast<int32_t>(std::min<size_t>(aux.data.size(), 2147483647u)),
        });

        if (dwg_debug_enabled() && !aux.data.empty()) {
            const size_t string_limit = std::min<size_t>(strings.size(), 16);
            for (size_t si = 0; si < string_limit; ++si) {
                dwg_debug_log("[DWG] auxiliary '%s' string: %s\n",
                              aux.name.c_str(), strings[si].c_str());
            }

            const auto points = extract_plausible_raw_points(aux.data.data(), aux.data.size(), 8);
            for (const auto& p : points) {
                dwg_debug_log("[DWG] auxiliary '%s' point: %.6f, %.6f\n",
                              aux.name.c_str(), p.first, p.second);
            }
            const auto ints = extract_small_int_candidates(aux.data.data(), aux.data.size(), 16);
            for (const auto& c : ints) {
                dwg_debug_log("[DWG] auxiliary '%s' int: offset=%zu value=%u bytes=%u\n",
                              aux.name.c_str(),
                              c.offset,
                              c.value,
                              static_cast<unsigned>(c.bytes));
            }
        }
    }
}

// ============================================================
// parse_header_variables — read Section 0 (extents, metadata)
// ============================================================

Result DwgParser::parse_header_variables(SceneGraph& scene)
{
    if (m_sections.header_vars.empty()) {
        return Result::success();
    }

    // Header variables section contains drawing metadata.
    // For now, we skip detailed parsing — extents come from entity bounds.
    (void)scene;
    return Result::success();
}

// ============================================================
// parse_classes — read Section 1 (class definitions)
//
// Read classes section.
// Parse class definitions: BL(max_num), then for each:
//   BS(class_num), TV(dxf_name), TV(cpp_name), TX(app_name),
//   BS(proxy_flags), B(is_entity)
// Store in class_map: type -> (dxf_name, is_entity)
// ============================================================

Result DwgParser::parse_classes(SceneGraph& scene)
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
        (void)reader.read_rl();  // hsize
        uint32_t bitsize = reader.read_rl();
        max_num = reader.read_bs();
        (void)reader.read_raw_char();
        (void)reader.read_raw_char();
        (void)reader.read_b();
        dwg_debug_log("[DWG] classes_header: data_size=%zu area=%u bits=%u max=%u pos=%zu\n",
                      data_size, data_area_size, bitsize, max_num, reader.bit_offset());
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
            if (class_string_start != 0) {
                class_data_limit = class_string_start;
            }
            dwg_debug_log("[DWG] classes_string_stream: active=%u start=%zu limit=%zu err=%u\n",
                          class_string_start != 0 ? 1u : 0u,
                          class_string_start, class_data_limit,
                          reader.has_error() ? 1u : 0u);
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

        // Classes Section strings are inline in the class records. Do not
        // reuse the R2007+ entity string-stream layout here; doing so leaves
        // the main reader at the wrong field and destroys the class map.
        std::string dxf_name;
        std::string cpp_name;
        std::string app_name;
        uint16_t proxy_flags = 0;
        bool is_entity = false;

        if (m_version < DwgVersion::R2007) {
            proxy_flags = reader.read_bs();
            dxf_name = reader.read_tv();
            cpp_name = reader.read_tv();
            app_name = reader.read_tv();
            uint16_t class_id = reader.read_bs();
            is_entity = (class_id == 0x1F2);
        } else {
            proxy_flags = reader.read_bs();
            if (class_string_start == 0) break;
            app_name = class_strings.read_tu();
            cpp_name = class_strings.read_tu();
            dxf_name = class_strings.read_tu();
            uint16_t class_id = reader.read_bs();
            is_entity = (class_id == 0x1F2);
            (void)reader.read_bl();  // num_instances
            (void)reader.read_raw_char();  // dwg_version
            (void)reader.read_raw_char();  // maintenance_version
            (void)reader.read_raw_char();
            (void)reader.read_raw_char();
        }

        if (reader.has_error() || class_strings.has_error()) break;

        m_sections.class_map[class_type] = {dxf_name, is_entity};
        dwg_debug_log("[DWG] class type=%u dxf='%s' entity=%u\n",
                      class_type, dxf_name.c_str(), is_entity ? 1u : 0u);
        (void)entry_start;
        (void)proxy_flags;
    }

    dwg_debug_log("[DWG] classes: %zu entries parsed\n", m_sections.class_map.size());
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
    }
    return Result::success();
}

// ============================================================
// parse_object_map — read Section 2 (handle -> offset entries)
//
// Parse handle -> offset pairs.
// Per-section RESET of BOTH handle and offset accumulators.
// Each section: RS(section_size), then MC pairs (handle_delta, offset_delta)
//   until end.
// handle += handle_delta, offset += offset_delta
//   (but RESET both to 0 at section start).
// ============================================================

Result DwgParser::parse_object_map(const uint8_t* /*data*/, size_t /*size*/)
{
    if (m_sections.object_map.empty()) {
        return Result::error(ErrorCode::ParseError, "No object map data");
    }

    const uint8_t* data = m_sections.object_map.data();
    size_t data_size = m_sections.object_map.size();
    size_t off = 0;

    m_sections.handle_map.clear();
    m_sections.handle_offset_candidates.clear();
    uint64_t total_objects = 0;
    uint64_t global_handle_acc = 0;
    int64_t global_offset_acc = 0;

    auto add_offset_candidate = [&](uint64_t handle, int64_t offset) {
        if (handle == 0 || offset < 0) {
            return;
        }
        auto& candidates = m_sections.handle_offset_candidates[handle];
        const size_t uoffset = static_cast<size_t>(offset);
        if (std::find(candidates.begin(), candidates.end(), uoffset) == candidates.end()) {
            candidates.push_back(uoffset);
        }
        if (m_sections.object_data_file_offset > 0 &&
            uoffset >= m_sections.object_data_file_offset) {
            const size_t relative_offset = uoffset - m_sections.object_data_file_offset;
            if (std::find(candidates.begin(), candidates.end(), relative_offset) == candidates.end()) {
                candidates.push_back(relative_offset);
            }
        }
    };

    while (off + 2 <= data_size) {
        size_t section_start = off;
        // RS_BE(section_size) — Big-Endian 16-bit section size
        // Per libredwg: section_size INCLUDES the 2-byte RS_BE header itself.
        uint16_t section_size = (static_cast<uint16_t>(data[off]) << 8) |
                                 static_cast<uint16_t>(data[off + 1]);
        off += 2;

        if (section_size == 0 || section_size > 2040) {
            // End marker or invalid size
            if (section_size > 2040) {
                dwg_debug_log("[DWG] WARNING: object_map section_size=%u too large at off=%zu\n",
                        section_size, section_start);
            }
            break;
        }
        if (section_size <= 2) {
            // Empty section (just the header) — skip CRC and continue
            off = section_start + section_size + 2;
            continue;
        }

        // Data ends at section_start + section_size (RS_BE header is included)
        size_t section_data_end = section_start + section_size;
        if (section_data_end > data_size) break;

        // Per DWG R2004+ spec: handle and offset accumulators reset per section.
        // The first delta in each section jumps to the correct global handle.
        uint64_t handle_acc = 0;
        int64_t offset_acc = 0;

        while (off < section_data_end && off + 2 <= data_size) {
            // UMC (unsigned modular char) for handle offset
            uint32_t handle_delta = read_modular_char(data, data_size, off);
            if (off >= section_data_end) break;
            // MC (signed modular char) for address offset
            int32_t offset_delta = read_modular_char_signed(data, data_size, off);

            handle_acc += static_cast<uint64_t>(handle_delta);
            offset_acc += offset_delta;
            global_handle_acc += static_cast<uint64_t>(handle_delta);
            global_offset_acc += offset_delta;

            m_sections.handle_map[handle_acc] = static_cast<size_t>(offset_acc);
            // Keep non-authoritative alternatives for recovery. Real R2004+
            // files in the wild differ on whether handle and address
            // accumulators reset at page/section boundaries. We retain the
            // conservative reset/reset interpretation as primary, and only use
            // these candidates after validating the object's own handle.
            add_offset_candidate(handle_acc, offset_acc);
            add_offset_candidate(handle_acc, global_offset_acc);
            add_offset_candidate(global_handle_acc, offset_acc);
            add_offset_candidate(global_handle_acc, global_offset_acc);
            total_objects++;
        }

        // Skip CRC (2 bytes, RS_BE) after the section data
        off = section_data_end + 2;
    }

    dwg_debug_log("[DWG] object_map: %llu entries unique=%zu (data_size=%zu, final_off=%zu)\n",
            (unsigned long long)total_objects,
            m_sections.handle_map.size(),
            data_size, off);
    return Result::success();
}

// ============================================================
// parse_objects — iterate handle map, parse each entity
//
// THE MOST COMPLEX STAGE.
//
// PreparedObject struct with: data, size, bit_offset, bit_limit,
//   obj_type, header, str_bit_pos, has_string_stream,
//   is_r2007_plus, is_graphic
//
// prepare_object lambda:
//   a. Read MS (Modular Short): entity data size
//   b. Read UMC (for R2010+): handlestream_size
//   c. Create DwgBitReader, set bit offset past UMC
//   d. Setup string stream if R2007+ and bit_limit > 0
//   e. Read BOT (R2010+) or BS (older) to get obj_type
//   f. Read H (owner handle)
//   g. Read EED loop: BS(size), if 0 break, skip H + size bytes
//   h. For graphic entities ONLY: read preview
//   i. For graphic entities: read entity CED
//   j. For non-graphic objects: read object common header
//
// is_graphic_entity function: explicit switch for known entity types.
// ============================================================

namespace {

// Per libredwg: returns true if the DWG type number is a graphic entity.
bool is_graphic_entity(uint32_t obj_type)
{
    switch (obj_type) {
        case 1:   // TEXT
        case 2:   // ATTRIB
        case 3:   // ATTDEF
        case 4:   // BLOCK
        case 5:   // ENDBLK
        case 6:   // SEQEND
        case 7:   // INSERT
        case 8:   // MINSERT
        case 10:  // VERTEX_2D
        case 11:  // VERTEX_3D
        case 12:  // VERTEX_MESH
        case 13:  // VERTEX_PFACE
        case 14:  // VERTEX_PFACE_FACE
        case 15:  // POLYLINE_2D
        case 16:  // POLYLINE_3D
        case 17:  // ARC
        case 18:  // CIRCLE
        case 19:  // LINE
        case 20:  // DIMENSION_ORDINATE
        case 21:  // DIMENSION_LINEAR
        case 22:  // DIMENSION_ALIGNED
        case 23:  // DIMENSION_ANG3PT
        case 24:  // DIMENSION_ANG2LN
        case 25:  // DIMENSION_RADIUS
        case 26:  // DIMENSION_DIAMETER
        case 27:  // POINT
        case 28:  // 3DFACE
        case 29:  // POLYLINE_PFACE
        case 30:  // POLYLINE_MESH
        case 31:  // SOLID
        case 32:  // TRACE
        case 34:  // VIEWPORT
        case 35:  // ELLIPSE
        case 36:  // SPLINE
        case 40:  // RAY
        case 41:  // XLINE
        case 44:  // MTEXT
        case 77:  // LWPOLYLINE
        case 78:  // HATCH
            return true;

        // Explicitly non-graphic
        case 42:  // DICTIONARY
        case 43:  // DICTIONARYWDFLT
        case 48:  // BLOCK_CONTROL
        case 49:  // BLOCK_HEADER
        case 50:  // LAYER_CONTROL
        case 51:  // LAYER
        case 52:  // STYLE_CONTROL
        case 53:  // STYLE (TEXTSTYLE)
        case 54:  // LTYPE_CONTROL
        case 55:  // LTYPE (some DWGs)
        case 56:  // VIEW
        case 57:  // UCS
        case 58:  // VPORT
        case 59:  // APPID
        case 60:  // DIMSTYLE
        case 61:  // VP_ENT_HDR
        case 62:  // GROUP
        case 64:  // MLINESTYLE
        case 70:  // XRECORD
        case 74:  // PROXY_OBJECT
        case 79:  // XRECORD/roundtrip dictionary record in modern DWGs
        case 82:  // LAYOUT
            return false;

        default:
            return false;
    }
}

bool valid_layout_size(double w, double h)
{
    return std::isfinite(w) && std::isfinite(h) && w > 1.0 && h > 1.0 && w < 1.0e7 && h < 1.0e7;
}

void read_2rd(DwgBitReader& reader, double& x, double& y)
{
    x = reader.read_double();
    y = reader.read_double();
}

int32_t parse_layout_object(DwgBitReader& reader, SceneGraph& scene, DwgVersion version)
{
    DwgBitReader r = reader;

    (void)r.read_t();       // plotsettings.printer_cfg_file
    (void)r.read_t();       // plotsettings.paper_size
    (void)r.read_bs();      // plotsettings.plot_flags
    const double left_margin = r.read_bd();
    const double bottom_margin = r.read_bd();
    const double right_margin = r.read_bd();
    const double top_margin = r.read_bd();
    const double paper_width = r.read_bd();
    const double paper_height = r.read_bd();
    (void)r.read_t();       // plotsettings.canonical_media_name
    double plot_origin_x = 0.0;
    double plot_origin_y = 0.0;
    r.read_2d_point(plot_origin_x, plot_origin_y);
    (void)r.read_bs();      // plotsettings.plot_paper_unit
    (void)r.read_bs();      // plotsettings.plot_rotation_mode
    (void)r.read_bs();      // plotsettings.plot_type
    double win_ll_x = 0.0;
    double win_ll_y = 0.0;
    double win_ur_x = 0.0;
    double win_ur_y = 0.0;
    r.read_2d_point(win_ll_x, win_ll_y);
    r.read_2d_point(win_ur_x, win_ur_y);
    (void)r.read_bd();      // plotsettings.paper_units
    (void)r.read_bd();      // plotsettings.drawing_units
    (void)r.read_t();       // plotsettings.stylesheet
    (void)r.read_bs();      // plotsettings.std_scale_type
    (void)r.read_bd();      // plotsettings.std_scale_factor
    double image_origin_x = 0.0;
    double image_origin_y = 0.0;
    r.read_2d_point(image_origin_x, image_origin_y);
    if (version >= DwgVersion::R2004) {
        (void)r.read_bs();  // plotsettings.shadeplot_type
        (void)r.read_bs();  // plotsettings.shadeplot_reslevel
        (void)r.read_bs();  // plotsettings.shadeplot_customdpi
    }

    Layout layout;
    layout.name = r.read_t();
    (void)r.read_bs();      // tab_order
    (void)r.read_bs();      // layout_flags
    double ignored_z = 0.0;
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // INSBASE
    double lim_min_x = 0.0;
    double lim_min_y = 0.0;
    double lim_max_x = 0.0;
    double lim_max_y = 0.0;
    read_2rd(r, lim_min_x, lim_min_y);
    read_2rd(r, lim_max_x, lim_max_y);
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // UCSORG
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // UCSXDIR
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // UCSYDIR
    (void)r.read_bd();      // ucs_elevation
    (void)r.read_bs();      // UCSORTHOVIEW
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // EXTMIN
    r.read_3d_point(ignored_z, ignored_z, ignored_z);  // EXTMAX
    if (version >= DwgVersion::R2004) {
        (void)r.read_bl();  // num_viewports
    }

    if (r.has_error()) {
        return -1;
    }

    if (layout.name.empty()) {
        layout.name = "Layout";
    }
    std::string upper = layout.name;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    layout.is_model_layout = (upper == "MODEL");

    if (valid_layout_size(paper_width, paper_height)) {
        layout.paper_bounds.expand(Vec3{0.0f, 0.0f, 0.0f});
        layout.paper_bounds.expand(Vec3{static_cast<float>(paper_width),
                                        static_cast<float>(paper_height), 0.0f});

        const float printable_min_x = static_cast<float>(std::max(0.0, left_margin));
        const float printable_min_y = static_cast<float>(std::max(0.0, bottom_margin));
        const float printable_max_x = static_cast<float>(std::max(left_margin, paper_width - right_margin));
        const float printable_max_y = static_cast<float>(std::max(bottom_margin, paper_height - top_margin));
        if (printable_max_x > printable_min_x && printable_max_y > printable_min_y) {
            layout.border_bounds.expand(Vec3{printable_min_x, printable_min_y, 0.0f});
            layout.border_bounds.expand(Vec3{printable_max_x, printable_max_y, 0.0f});
        }
    }

    if (std::isfinite(win_ll_x) && std::isfinite(win_ll_y) &&
        std::isfinite(win_ur_x) && std::isfinite(win_ur_y) &&
        win_ur_x > win_ll_x && win_ur_y > win_ll_y) {
        layout.plot_window.expand(Vec3{static_cast<float>(win_ll_x), static_cast<float>(win_ll_y), 0.0f});
        layout.plot_window.expand(Vec3{static_cast<float>(win_ur_x), static_cast<float>(win_ur_y), 0.0f});
    }

    dwg_debug_log("[DWG] layout parsed name='%s' model=%u paper=(%.3f,%.3f) margins=(%.3f,%.3f,%.3f,%.3f) window=(%.3f,%.3f)-(%.3f,%.3f) limits=(%.3f,%.3f)-(%.3f,%.3f)\n",
                  layout.name.c_str(),
                  layout.is_model_layout ? 1u : 0u,
                  paper_width,
                  paper_height,
                  left_margin,
                  bottom_margin,
                  right_margin,
                  top_margin,
                  win_ll_x,
                  win_ll_y,
                  win_ur_x,
                  win_ur_y,
                  lim_min_x,
                  lim_min_y,
                  lim_max_x,
                  lim_max_y);

    return scene.add_layout(std::move(layout));
}

uint64_t resolve_handle_ref(uint64_t source_handle, const DwgBitReader::HandleRef& ref)
{
    switch (ref.code) {
        case 2: case 3: case 4: case 5:
            return ref.value;  // TYPEDOBJHANDLE: absolute
        case 6:
            return source_handle + 1;
        case 8:
            return (source_handle > 1) ? source_handle - 1 : 0;
        case 0xA:
            return source_handle + ref.value;
        case 0xC:
            return (source_handle > ref.value) ? source_handle - ref.value : 0;
        default:
            return ref.value;  // code 0/1: soft pointer, treat as absolute
    }
}

} // anonymous namespace

Result DwgParser::parse_objects(SceneGraph& scene)
{
    if (m_sections.object_data.empty()) {
        return Result::error(ErrorCode::ParseError, "No object data");
    }
    reset_dwg_entity_parser_state();

    const uint8_t* obj_data = m_sections.object_data.data();
    size_t obj_data_size = m_sections.object_data.size();
    bool is_r2007_plus = (m_version >= DwgVersion::R2007);
    bool is_r2010_plus = (m_version >= DwgVersion::R2010);

    size_t graphic_count = 0;
    size_t non_graphic_count = 0;
    size_t error_count = 0;
    size_t g_layer_resolved = 0;
    std::unordered_map<uint32_t, size_t> object_type_counts;

    size_t processed = 0;
    auto t_start = std::chrono::steady_clock::now();

    // ============================================================
    // Pre-scan: parse LTYPE then LAYER objects first to populate handle maps
    // before any entities need them.
    // This is necessary because the object map is NOT sorted by
    // handle value — entities may appear before their layer objects.
    // ============================================================
    auto prescan_table_type = [&](uint32_t target_type) {
        for (const auto& [handle, offset] : m_sections.handle_map) {
            if (offset >= obj_data_size) continue;

            DwgBitReader ms_r(obj_data + offset, obj_data_size - offset);
            uint32_t esz = ms_r.read_modular_short();
            if (ms_r.has_error() || esz == 0 || esz > obj_data_size - offset) continue;
            ms_r.align_to_byte();
            size_t msb = ms_r.bit_offset() / 8;

            size_t umcb = 0;
            uint32_t hss = 0;
            if (is_r2010_plus) {
                uint32_t res = 0;
                const uint8_t* up = obj_data + offset + msb;
                size_t uavail = obj_data_size - offset - msb;
                for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail; ++i) {
                    uint8_t bv = up[i];
                    res = (res << 7) | (bv & 0x7F);
                    umcb = static_cast<size_t>(i) + 1;
                    if ((bv & 0x80) == 0) break;
                }
                hss = res;
            }

            size_t edb = static_cast<size_t>(esz);
            if (offset + msb + umcb + edb > obj_data_size) continue;

            DwgBitReader r(obj_data + offset + msb + umcb, edb);
            size_t mdb = edb * 8;
            if (is_r2010_plus && hss <= edb * 8) mdb = edb * 8 - hss;
            r.set_bit_limit(mdb);
            if (is_r2007_plus && mdb > 0) r.setup_string_stream(static_cast<uint32_t>(mdb));

            uint32_t ot = is_r2010_plus ? r.read_bot() : r.read_bs();
            if (r.has_error() || ot != target_type) continue;

            // Skip common object header: handle (H) + EED loop + reactors + flags
            (void)r.read_h();  // object handle
            uint16_t eed_sz = 0;
            while (!r.has_error()) {
                eed_sz = r.read_bs();
                if (eed_sz == 0) break;
                (void)r.read_h();  // EED application handle
                size_t skip = static_cast<size_t>(eed_sz) * 8;
                if (r.bit_offset() + skip <= r.bit_limit()) {
                    r.set_bit_offset(r.bit_offset() + skip);
                } else {
                    break;
                }
            }
            (void)r.read_bl();  // num_reactors (BL)
            (void)r.read_b();   // is_xdic_missing (B, R2004+)
            if (!r.has_error()) {
                parse_dwg_table_object(r, ot, scene, m_version,
                                       edb * 8, mdb, handle,
                                       &m_layer_handle_to_index,
                                       &m_linetype_handle_to_index);
            }
        }
    };
    prescan_table_type(57); // LTYPE
    prescan_table_type(51); // LAYER
    dwg_debug_log("[DWG] Pre-scan: %zu layer handles, %zu linetype handles loaded\n",
            m_layer_handle_to_index.size(), m_linetype_handle_to_index.size());

    // Collect INSERT handle stream data for post-processing resolution.
    // Maps entity_index → vector of handles from INSERT handle stream.
    std::unordered_map<size_t, std::vector<uint64_t>> insert_handles;
    std::unordered_map<size_t, std::vector<uint64_t>> entity_common_handles;
    std::unordered_map<size_t, uint64_t> entity_owner_handles;
    std::unordered_map<size_t, uint64_t> entity_object_handles;
    std::unordered_map<uint64_t, uint64_t> entity_handle_to_block_header;
    std::unordered_map<uint64_t, uint32_t> handle_object_types;
    std::unordered_map<int32_t, std::vector<uint64_t>> layout_handle_refs;
    std::unordered_map<uint32_t, size_t> custom_annotation_debug_samples;
    std::unordered_set<uint64_t> line_resource_line_handles;
    size_t custom_annotation_text_fallbacks = 0;
    size_t custom_note_leader_proxies = 0;
    size_t custom_datum_target_proxies = 0;
    size_t custom_datum_target_label_proxies = 0;
    size_t custom_line_resource_refs = 0;
    size_t custom_line_resource_unresolved_refs = 0;
    size_t custom_field_objects = 0;
    size_t custom_field_strings = 0;
    size_t custom_fieldlist_objects = 0;
    size_t custom_mtext_context_objects = 0;
    size_t custom_detail_style_objects = 0;
    size_t custom_detail_custom_objects = 0;
    std::unordered_set<uint64_t> custom_annotation_dictionary_refs;
    std::unordered_set<uint64_t> custom_annotation_xrecord_refs;
    std::unordered_set<uint64_t> custom_annotation_unresolved_refs;
    std::unordered_map<uint64_t, std::vector<std::string>> dictionary_string_samples;
    std::unordered_map<uint64_t, std::vector<uint64_t>> dictionary_handle_refs;
    std::unordered_map<uint64_t, std::vector<std::string>> xrecord_string_samples;
    std::unordered_map<uint64_t, std::vector<uint64_t>> xrecord_handle_refs;
    std::vector<std::string> custom_field_string_samples;
    size_t recovered_object_offsets = 0;
    size_t unrecovered_object_offsets = 0;
    size_t object_recovery_scans = 0;
    size_t primary_self_handle_mismatches = 0;

    struct ObjectRecord {
        size_t offset = 0;
        size_t ms_bytes = 0;
        size_t umc_bytes = 0;
        size_t entity_data_bytes = 0;
        size_t entity_bits = 0;
        size_t main_data_bits = 0;
        uint32_t obj_type = 0;
        DwgBitReader::HandleRef self_handle;
    };

    auto is_known_object_type = [&](uint32_t obj_type) {
        if (is_graphic_entity(obj_type)) {
            return true;
        }
        switch (obj_type) {
            case 42: case 43: case 48: case 49: case 50: case 51:
            case 52: case 53: case 54: case 55: case 56: case 57:
            case 58: case 59: case 60: case 61: case 62: case 64:
            case 65: case 69: case 70: case 74: case 79: case 82:
                return true;
            default:
                return m_sections.class_map.find(obj_type) != m_sections.class_map.end();
        }
    };

    auto prepare_object_record = [&](size_t record_offset, ObjectRecord& record,
                                     bool require_valid_handle_stream,
                                     bool require_known_type,
                                     const char** failure_reason = nullptr) {
        auto fail = [&](const char* reason) {
            if (failure_reason) {
                *failure_reason = reason;
            }
            return false;
        };
        if (record_offset >= obj_data_size) {
            return fail("offset_out_of_range");
        }

        DwgBitReader ms_reader(obj_data + record_offset, obj_data_size - record_offset);
        uint32_t entity_size = ms_reader.read_modular_short();
        if (ms_reader.has_error() || entity_size == 0 ||
            entity_size > obj_data_size - record_offset) {
            return fail("invalid_entity_size");
        }

        ms_reader.align_to_byte();
        const size_t ms_bytes = ms_reader.bit_offset() / 8;
        if (record_offset + ms_bytes > obj_data_size) {
            return fail("invalid_ms_size");
        }

        size_t umc_bytes = 0;
        uint32_t handle_stream_size = 0;
        if (is_r2010_plus) {
            uint32_t result = 0;
            int shift = 0;
            const size_t uavail = obj_data_size - record_offset - ms_bytes;
            const uint8_t* umc_ptr = obj_data + record_offset + ms_bytes;
            bool finished = false;
            for (int i = 0; i < 4 && static_cast<size_t>(i) < uavail; ++i) {
                const uint8_t byte_val = umc_ptr[i];
                result |= static_cast<uint32_t>(byte_val & 0x7F) << shift;
                shift += 7;
                umc_bytes = static_cast<size_t>(i) + 1;
                if ((byte_val & 0x80) == 0) {
                    finished = true;
                    break;
                }
            }
            if (!finished) {
                return fail("invalid_umc");
            }
            handle_stream_size = result;
        }

        const size_t entity_data_bytes = static_cast<size_t>(entity_size);
        if (record_offset + ms_bytes + umc_bytes + entity_data_bytes > obj_data_size) {
            return fail("entity_data_out_of_range");
        }

        const size_t entity_bits = entity_data_bytes * 8;
        size_t main_data_bits = entity_bits;
        if (is_r2010_plus) {
            if (handle_stream_size > entity_bits) {
                if (require_valid_handle_stream) {
                    return fail("invalid_handle_stream_size");
                }
            } else {
                main_data_bits = entity_bits - handle_stream_size;
            }
        }
        if (main_data_bits < 8) {
            return fail("empty_main_data");
        }

        DwgBitReader reader(obj_data + record_offset + ms_bytes + umc_bytes,
                            entity_data_bytes);
        reader.set_bit_limit(main_data_bits);
        const uint32_t obj_type = is_r2010_plus ? reader.read_bot() : reader.read_bs();
        if (reader.has_error() || (require_known_type && !is_known_object_type(obj_type))) {
            return fail(reader.has_error() ? "object_type_read_failed" : "unknown_object_type");
        }

        auto self_handle = reader.read_h();
        if (reader.has_error()) {
            return fail("self_handle_read_failed");
        }

        record.offset = record_offset;
        record.ms_bytes = ms_bytes;
        record.umc_bytes = umc_bytes;
        record.entity_data_bytes = entity_data_bytes;
        record.entity_bits = entity_bits;
        record.main_data_bits = main_data_bits;
        record.obj_type = obj_type;
        record.self_handle = self_handle;
        return true;
    };

    auto recover_object_record_for_handle = [&](uint64_t target_handle,
                                                ObjectRecord& recovered) {
        // Recovery is intentionally strict and bounded: it is a generic DWG
        // object-map repair path for bad offsets, not a file-specific fallback.
        if (++object_recovery_scans > 64) {
            return false;
        }

        bool found = false;
        ObjectRecord candidate;
        auto scan_range = [&](size_t begin, size_t end) {
            end = std::min(end, obj_data_size);
            for (size_t pos = begin; pos < end; ++pos) {
                ObjectRecord current;
                if (!prepare_object_record(pos, current, true, true)) {
                    continue;
                }
                const uint64_t self_abs = resolve_handle_ref(target_handle, current.self_handle);
                if (self_abs != target_handle) {
                    continue;
                }
                if (found) {
                    // Ambiguous self-handle matches are worse than dropping a
                    // corrupt object-map entry, because choosing one can move
                    // large layouts or blocks into the wrong space.
                    found = false;
                    return false;
                }
                candidate = current;
                found = true;
            }
            return true;
        };

        if (!m_sections.object_pages.empty()) {
            for (const auto& [page_offset, page_size] : m_sections.object_pages) {
                if (!scan_range(page_offset, page_offset + page_size)) {
                    return false;
                }
            }
        } else if (!scan_range(0, obj_data_size)) {
            return false;
        }

        if (!found) {
            return false;
        }
        recovered = candidate;
        return true;
    };

    auto recover_object_record_from_candidates = [&](uint64_t target_handle,
                                                     size_t primary_offset,
                                                     ObjectRecord& recovered) {
        auto it = m_sections.handle_offset_candidates.find(target_handle);
        if (it == m_sections.handle_offset_candidates.end()) {
            return false;
        }

        bool found = false;
        ObjectRecord candidate;
        for (size_t candidate_offset : it->second) {
            if (candidate_offset == primary_offset) {
                continue;
            }
            ObjectRecord current;
            if (!prepare_object_record(candidate_offset, current, false, false)) {
                continue;
            }
            const uint64_t self_abs = resolve_handle_ref(target_handle, current.self_handle);
            if (self_abs != target_handle) {
                continue;
            }
            if (found) {
                return false;
            }
            candidate = current;
            found = true;
        }
        if (!found) {
            return false;
        }
        recovered = candidate;
        return true;
    };

    // Collect BLOCK entity handle→name mapping from handle streams.
    // BLOCK entities have correct unique names (e.g., "*D1077") while
    // BLOCK_HEADER (type 49) stores truncated names (e.g., "*D").
    std::unordered_map<uint64_t, std::string> block_names_from_entities;
    for (const auto& handle_entry : m_sections.handle_map) {
        uint64_t handle = handle_entry.first;
        size_t offset = handle_entry.second;
        processed++;
        if ((processed % 10000) == 0) {
            dwg_debug_log("[DWG] parse_objects progress: %zu / %zu\n",
                    processed, m_sections.handle_map.size());
        }
        ObjectRecord record;
        const char* prepare_failure = "";
        if (!prepare_object_record(offset, record, false, false, &prepare_failure)) {
            ObjectRecord recovered;
            if (!recover_object_record_from_candidates(handle, offset, recovered) &&
                !recover_object_record_for_handle(handle, recovered)) {
                if (dwg_debug_enabled() && unrecovered_object_offsets < 20) {
                    dwg_debug_log("[DWG] object-map invalid: handle=%llu offset=%zu reason=%s\n",
                                  static_cast<unsigned long long>(handle),
                                  offset,
                                  prepare_failure ? prepare_failure : "");
                    auto cand_it = m_sections.handle_offset_candidates.find(handle);
                    if (cand_it != m_sections.handle_offset_candidates.end()) {
                        const size_t cand_limit = std::min<size_t>(cand_it->second.size(), 8);
                        for (size_t ci = 0; ci < cand_limit; ++ci) {
                            const size_t cand_offset = cand_it->second[ci];
                            const char* cand_reason = "";
                            ObjectRecord cand_record;
                            const bool cand_ok = prepare_object_record(
                                cand_offset, cand_record, false, false, &cand_reason);
                            const uint64_t self_abs = cand_ok
                                ? resolve_handle_ref(handle, cand_record.self_handle)
                                : 0;
                            dwg_debug_log("[DWG]   candidate[%zu] offset=%zu ok=%u type=%u self=%llu reason=%s\n",
                                          ci,
                                          cand_offset,
                                          cand_ok ? 1u : 0u,
                                          cand_ok ? cand_record.obj_type : 0u,
                                          static_cast<unsigned long long>(self_abs),
                                          cand_reason ? cand_reason : "");
                        }
                    }
                }
                error_count++;
                unrecovered_object_offsets++;
                continue;
            }
            record = recovered;
            offset = record.offset;
            recovered_object_offsets++;
        }

        const size_t ms_bytes = record.ms_bytes;
        const size_t umc_bytes = record.umc_bytes;
        const size_t entity_data_bytes = record.entity_data_bytes;
        const size_t entity_bits = record.entity_bits;
        const size_t main_data_bits = record.main_data_bits;
        const uint32_t obj_type = record.obj_type;
        const uint64_t primary_self_handle = resolve_handle_ref(handle, record.self_handle);
        if (primary_self_handle != 0 && primary_self_handle != handle) {
            if (dwg_debug_enabled() && primary_self_handle_mismatches < 20) {
                dwg_debug_log("[DWG] object-map self mismatch: map_handle=%llu self=%llu offset=%zu type=%u\n",
                              static_cast<unsigned long long>(handle),
                              static_cast<unsigned long long>(primary_self_handle),
                              offset,
                              obj_type);
            }
            primary_self_handle_mismatches++;
        }

        // Entity reader starts AFTER MS + UMC, with exactly entity_size bytes.
        DwgBitReader reader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
        reader.set_bit_limit(main_data_bits);
        if (is_r2007_plus && main_data_bits > 0) {
            reader.setup_string_stream(static_cast<uint32_t>(main_data_bits));
        }

        if (is_r2010_plus) {
            (void)reader.read_bot();
        } else {
            (void)reader.read_bs();
        }
        if (reader.has_error()) {
            error_count++;
            continue;
        }
        object_type_counts[obj_type]++;
        handle_object_types[handle] = obj_type;

        bool is_graphic = is_graphic_entity(obj_type);

        // Check class_map for custom entity types
        if (!is_graphic) {
            auto it = m_sections.class_map.find(obj_type);
            if (it != m_sections.class_map.end() && it->second.second) {
                is_graphic = true;
            }
        }

        (void)reader.read_h();  // object handle

        // EED (Extended Entity Data) loop
        uint16_t eed_size = 0;
        while (!reader.has_error()) {
            eed_size = reader.read_bs();
            if (eed_size == 0) break;

            (void)reader.read_h();  // EED application handle

            // Skip EED data bytes (NOT byte-aligned per libredwg)
            size_t skip_bits = static_cast<size_t>(eed_size) * 8;
            if (reader.bit_offset() + skip_bits <= reader.bit_limit()) {
                reader.set_bit_offset(reader.bit_offset() + skip_bits);
            } else {
                break;  // Cannot skip past entity boundary
            }
        }

        if (reader.has_error()) {
            error_count++;
            continue;
        }

        // Preview (graphic entities only)
        bool has_preview = false;
        uint64_t preview_size = 0;
        if (is_graphic) {
            has_preview = reader.read_b();
            if (has_preview) {
                preview_size = reader.read_bll();
                reader.align_to_byte();
                if (preview_size > 0 && preview_size < obj_data_size) {
                    size_t skip_bits = static_cast<size_t>(preview_size) * 8;
                    if (reader.bit_offset() + skip_bits <= reader.bit_limit()) {
                        reader.set_bit_offset(reader.bit_offset() + skip_bits);
                    } else {
                        error_count++;
                        continue;
                    }
                }
            }
        }

        if (reader.has_error()) {
            error_count++;
            continue;
        }

        // ---- Entity CED or Object Common Header ----

        EntityHeader entity_hdr;
        entity_hdr.entity_id = static_cast<int64_t>(handle);

        uint16_t color_raw = 0;
        uint8_t color_flag = 0;
        uint32_t saved_num_reactors = 0;
        bool saved_is_xdic_missing = false;
        if (is_graphic) {
            // Graphic entity CED (Common Entity Data)
            if (is_r2010_plus) {
                (void)reader.read_bits(2);  // entity_mode (BB)
            }

            saved_num_reactors = reader.read_bl();
            saved_is_xdic_missing = reader.read_b();

            if (m_version >= DwgVersion::R2013) {
                (void)reader.read_b();  // is_has_ds_data (R2013+)
            }

            // Color
            uint16_t color_index;
            if (m_version >= DwgVersion::R2004) {
                // R2004+ ENC (Entity Color Encoding) per libredwg common_entity_data.spec
                color_raw = reader.read_bs();
                color_flag = static_cast<uint8_t>(color_raw >> 8);
                color_index = color_raw & 0x1ff;
                if (color_flag & 0x20) {
                    (void)reader.read_bl();  // alpha_raw
                }
                if (color_flag & 0x40) {
                    (void)reader.read_h();   // handle
                } else if (color_flag & 0x80) {
                    uint32_t rgb = reader.read_bl();  // True Color: BL (32-bit), 0x00BBGGRR
                    if (rgb != 0) {
                        uint32_t rgb24 = rgb & 0xFFFFFF;
                        if (rgb24 != 0) {
                            entity_hdr.true_color = Color(
                                static_cast<uint8_t>(rgb24 & 0xFF),
                                static_cast<uint8_t>((rgb24 >> 8) & 0xFF),
                                static_cast<uint8_t>((rgb24 >> 16) & 0xFF));
                            entity_hdr.has_true_color = true;
                        }
                    }
                }
                if (m_version < DwgVersion::R2007) {
                    if ((color_flag & 0x41) == 0x41) {
                        (void)reader.read_tv();  // name
                    }
                    if ((color_flag & 0x42) == 0x42) {
                        (void)reader.read_tv();  // book_name
                    }
                }
            } else {
                color_index = reader.read_bs();
            }
            entity_hdr.color_override = (color_index != 256 && color_index != 0)
                                        ? static_cast<int32_t>(color_index) : 256;

            // linetype_scale (BD)
            double lts = reader.read_bd();
            (void)lts;

            // linetype flags (BB)
            uint8_t ltype_flags = reader.read_bits(2);
            if (ltype_flags == 3) {
                entity_hdr.linetype_index = -2;  // BYBLOCK
            }

            // plotstyle flags (BB)
            (void)reader.read_bits(2);

            // R2004+: material flags (BB)
            if (m_version >= DwgVersion::R2004) {
                (void)reader.read_bits(2);
            }

            // R2007+: shadow flags (RC = 8 bits per libredwg FIELD_RC0)
            if (m_version >= DwgVersion::R2007) {
                (void)reader.read_raw_char();
            }

            // R2010+: visualstyle flags
            if (is_r2010_plus) {
                (void)reader.read_b();  // has_full_visualstyle
                (void)reader.read_b();  // has_face_visualstyle
                (void)reader.read_b();  // has_edge_visualstyle
            }

            // invisible (BS)
            uint16_t invisible = reader.read_bs();
            entity_hdr.is_visible = (invisible == 0);

            // lineweight (RC)
            uint8_t lineweight_raw = reader.read_raw_char();
            if (lineweight_raw > 0 && lineweight_raw < 200) {
                entity_hdr.lineweight = static_cast<float>(lineweight_raw) / 100.0f;
            }

            (void)saved_num_reactors;
        } else {
            // Non-graphic object common header
            uint32_t num_reactors = reader.read_bl();
            (void)reader.read_b();  // is_xdic_missing
            if (m_version >= DwgVersion::R2013) {
                (void)reader.read_b();  // is_has_ds_data (R2013+)
            }
            (void)num_reactors;
        }

        if (reader.has_error()) {
            error_count++;
            continue;
        }

        auto class_it_for_debug = m_sections.class_map.find(obj_type);
        const std::string class_name_for_debug =
            class_it_for_debug != m_sections.class_map.end()
                ? class_it_for_debug->second.first
                : std::string{};
        auto class_name_for_object_type = [&](uint32_t type) -> std::string {
            auto it = m_sections.class_map.find(type);
            if (it != m_sections.class_map.end()) {
                return it->second.first;
            }
            return {};
        };
        auto object_type_for_handle = [&](uint64_t ref_handle) -> uint32_t {
            auto type_it = handle_object_types.find(ref_handle);
            if (type_it != handle_object_types.end()) {
                return type_it->second;
            }
            auto offset_it = m_sections.handle_map.find(ref_handle);
            ObjectRecord ref_record;
            if (offset_it != m_sections.handle_map.end() &&
                prepare_object_record(offset_it->second, ref_record, false, false, nullptr)) {
                return ref_record.obj_type;
            }
            const size_t primary_offset =
                offset_it != m_sections.handle_map.end() ? offset_it->second : static_cast<size_t>(-1);
            if (recover_object_record_from_candidates(ref_handle, primary_offset, ref_record)) {
                return ref_record.obj_type;
            }
            if (recover_object_record_for_handle(ref_handle, ref_record)) {
                return ref_record.obj_type;
            }
            if (offset_it == m_sections.handle_map.end()) {
                return 0;
            }
            return 0;
        };
        auto class_name_for_handle = [&](uint64_t ref_handle) -> std::string {
            return class_name_for_object_type(object_type_for_handle(ref_handle));
        };
        auto scan_handle_stream_refs = [&](int limit) {
            std::vector<uint64_t> refs;
            const size_t hs_bit_start = main_data_bits;
            const size_t hs_bit_end = entity_bits;
            const size_t hs_bits = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
            if (hs_bits < 8 || (hs_bit_end + 7) / 8 > entity_data_bytes) {
                return refs;
            }

            DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
            hreader.set_bit_offset(hs_bit_start);
            hreader.set_bit_limit(hs_bit_end);
            for (int h_idx = 0; h_idx < limit && !hreader.has_error(); ++h_idx) {
                auto href = hreader.read_h();
                if (hreader.has_error()) break;
                if (href.value == 0 && href.code == 0) continue;
                const uint64_t abs_handle = resolve_handle_ref(handle, href);
                if (abs_handle != 0) {
                    refs.push_back(abs_handle);
                }
            }
            return refs;
        };
        const bool annotation_like_custom =
            !class_name_for_debug.empty() &&
            (contains_ascii_ci(class_name_for_debug, "LEADER") ||
             contains_ascii_ci(class_name_for_debug, "DATUM") ||
             contains_ascii_ci(class_name_for_debug, "NOTE") ||
             contains_ascii_ci(class_name_for_debug, "FIELD") ||
             contains_ascii_ci(class_name_for_debug, "MTEXT") ||
             contains_ascii_ci(class_name_for_debug, "DIM") ||
             contains_ascii_ci(class_name_for_debug, "LINERES") ||
             contains_ascii_ci(class_name_for_debug, "DETAIL") ||
             contains_ascii_ci(class_name_for_debug, "SECTION") ||
             contains_ascii_ci(class_name_for_debug, "VIEW") ||
             contains_ascii_ci(class_name_for_debug, "BALLOON") ||
             contains_ascii_ci(class_name_for_debug, "CALLOUT"));

        const bool fieldlist_class =
            contains_ascii_ci(class_name_for_debug, "FIELDLIST");
        const bool field_class =
            contains_ascii_ci(class_name_for_debug, "FIELD") && !fieldlist_class;
        const bool mtext_context_class =
            contains_ascii_ci(class_name_for_debug, "MTEXTOBJECTCONTEXTDATA") ||
            contains_ascii_ci(class_name_for_debug, "CONTEXTDATA");
        const bool detail_style_class =
            contains_ascii_ci(class_name_for_debug, "DETAILVIEWSTYLE");
        const bool detail_custom_class =
            contains_ascii_ci(class_name_for_debug, "DETAIL") &&
            !detail_style_class &&
            !contains_ascii_ci(class_name_for_debug, "STANDARD");
        if (fieldlist_class) {
            custom_fieldlist_objects++;
        } else if (field_class) {
            custom_field_objects++;
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            const std::vector<std::string> field_strings = read_custom_t_strings(
                entity_data, entity_data_bytes, main_data_bits, reader.bit_offset(), 16);
            custom_field_strings += field_strings.size();
            for (const std::string& value : field_strings) {
                if (custom_field_string_samples.size() >= 8) break;
                bool duplicate = false;
                for (const std::string& existing : custom_field_string_samples) {
                    if (existing == value) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    custom_field_string_samples.push_back(value);
                }
            }
        }
        if (mtext_context_class) {
            custom_mtext_context_objects++;
        }
        if (detail_style_class) {
            custom_detail_style_objects++;
        }
        if (detail_custom_class) {
            custom_detail_custom_objects++;
        }

        if (obj_type == 42 || obj_type == 43) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            const std::vector<std::string> dict_strings = read_custom_t_strings(
                entity_data, entity_data_bytes, main_data_bits, reader.bit_offset(), 12);
            if (!dict_strings.empty()) {
                dictionary_string_samples[handle] = dict_strings;
            }
            dictionary_handle_refs[handle] = scan_handle_stream_refs(48);
        }
        if (obj_type == 70 || obj_type == 79) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            const std::vector<std::string> record_strings = read_custom_t_strings(
                entity_data, entity_data_bytes, main_data_bits, reader.bit_offset(), 12);
            if (!record_strings.empty()) {
                xrecord_string_samples[handle] = record_strings;
            }
            xrecord_handle_refs[handle] = scan_handle_stream_refs(48);
        }

        if (annotation_like_custom || fieldlist_class || field_class ||
            mtext_context_class || detail_style_class || detail_custom_class) {
            for (uint64_t ref_handle : scan_handle_stream_refs(48)) {
                const uint32_t ref_type = object_type_for_handle(ref_handle);
                if (ref_type == 42 || ref_type == 43) {
                    custom_annotation_dictionary_refs.insert(ref_handle);
                } else if (ref_type == 70 || ref_type == 79) {
                    custom_annotation_xrecord_refs.insert(ref_handle);
                } else if (ref_type == 0) {
                    custom_annotation_unresolved_refs.insert(ref_handle);
                }
            }
        }

        const size_t custom_debug_limit =
            contains_ascii_ci(class_name_for_debug, "DATUMTARGET") ? 40u : 3u;
        if (dwg_debug_enabled() && annotation_like_custom &&
            custom_annotation_debug_samples[obj_type] < custom_debug_limit) {
            custom_annotation_debug_samples[obj_type]++;
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            std::vector<std::string> snippets =
                extract_printable_strings(entity_data, entity_data_bytes, 4);
            std::vector<std::pair<double, double>> raw_points =
                extract_plausible_raw_points(entity_data, entity_data_bytes, 20);
            dwg_debug_log("[DWG] custom_annotation_sample type=%u class='%s' handle=%llu "
                          "graphic=%u bytes=%zu main_bits=%zu handle_bits=%zu reader_bit=%zu strings=",
                          obj_type,
                          class_name_for_debug.c_str(),
                          static_cast<unsigned long long>(handle),
                          is_graphic ? 1u : 0u,
                          entity_data_bytes,
                          main_data_bits,
                          entity_bits > main_data_bits ? entity_bits - main_data_bits : 0,
                          reader.bit_offset());
            for (size_t si = 0; si < snippets.size(); ++si) {
                dwg_debug_log("%s'%s'", si == 0 ? "" : "|", snippets[si].c_str());
            }
            dwg_debug_log(" points=");
            for (size_t pi = 0; pi < raw_points.size(); ++pi) {
                dwg_debug_log("%s(%.3f,%.3f)",
                              pi == 0 ? "" : "|",
                              raw_points[pi].first,
                              raw_points[pi].second);
            }
            dwg_debug_log("\n");
            if (contains_ascii_ci(class_name_for_debug, "DATUMTARGET")) {
                const auto short_strings =
                    extract_printable_strings(entity_data, entity_data_bytes, 24, 1);
                dwg_debug_log("[DWG] datumtarget_strings handle=%llu values=",
                              static_cast<unsigned long long>(handle));
                for (size_t si = 0; si < short_strings.size(); ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  short_strings[si].c_str());
                }
                dwg_debug_log("\n");
                std::vector<RawPointCandidate> point_offsets =
                    extract_plausible_raw_points_with_offsets(entity_data, entity_data_bytes, 24);
                dwg_debug_log("[DWG] datumtarget_probe handle=%llu point_offsets=",
                              static_cast<unsigned long long>(handle));
                for (size_t pi = 0; pi < point_offsets.size(); ++pi) {
                    const auto& p = point_offsets[pi];
                    dwg_debug_log("%s%zu:(%.3f,%.3f)",
                                  pi == 0 ? "" : "|",
                                  p.offset,
                                  p.x,
                                  p.y);
                }
                dwg_debug_log("\n");
                const auto ints = extract_small_int_candidates(entity_data, entity_data_bytes, 80);
                dwg_debug_log("[DWG] datumtarget_ints handle=%llu values=",
                              static_cast<unsigned long long>(handle));
                for (size_t ii = 0; ii < ints.size(); ++ii) {
                    const auto& c = ints[ii];
                    dwg_debug_log("%s%zu:%u/%u",
                                  ii == 0 ? "" : "|",
                                  c.offset,
                                  c.value,
                                  static_cast<unsigned>(c.bytes));
                }
                dwg_debug_log("\n");
            }
            if (contains_ascii_ci(class_name_for_debug, "FIELD") ||
                contains_ascii_ci(class_name_for_debug, "CONTEXTDATA")) {
                const auto short_strings =
                    extract_printable_strings(entity_data, entity_data_bytes, 24, 1);
                dwg_debug_log("[DWG] custom_short_strings class='%s' handle=%llu values=",
                              class_name_for_debug.c_str(),
                              static_cast<unsigned long long>(handle));
                for (size_t si = 0; si < short_strings.size(); ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  short_strings[si].c_str());
                }
                dwg_debug_log("\n");

                if (is_r2007_plus && main_data_bits > 0) {
                    const std::vector<std::string> t_values = read_custom_t_strings(
                        entity_data, entity_data_bytes, main_data_bits, reader.bit_offset(), 12);
                    dwg_debug_log("[DWG] custom_t_strings class='%s' handle=%llu values=",
                                  class_name_for_debug.c_str(),
                                  static_cast<unsigned long long>(handle));
                    for (size_t si = 0; si < t_values.size(); ++si) {
                        dwg_debug_log("%s'%s'",
                                      si == 0 ? "" : "|",
                                      t_values[si].c_str());
                    }
                    dwg_debug_log("\n");
                }
            }
        }
        if (dwg_debug_enabled() && annotation_like_custom) {
            const size_t hs_bit_start = main_data_bits;
            const size_t hs_bit_end = entity_bits;
            const size_t hs_bits = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
            if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                hreader.set_bit_offset(hs_bit_start);
                hreader.set_bit_limit(hs_bit_end);
                dwg_debug_log("[DWG] annotation_handles class='%s' handle=%llu refs=",
                              class_name_for_debug.c_str(),
                              static_cast<unsigned long long>(handle));
                for (int h_idx = 0; h_idx < 24 && !hreader.has_error(); ++h_idx) {
                    auto href = hreader.read_h();
                    if (hreader.has_error()) break;
                    if (href.value == 0 && href.code == 0) {
                        dwg_debug_log("%s%d:0(code=0)",
                                      h_idx == 0 ? "" : "|",
                                      h_idx);
                        continue;
                    }
                    const uint64_t abs_handle = resolve_handle_ref(handle, href);
                    const uint32_t ref_type = object_type_for_handle(abs_handle);
                    const std::string ref_class = class_name_for_handle(abs_handle);
                    dwg_debug_log("%s%d:%llu(code=%u,obj=%u,type='%s')",
                                  h_idx == 0 ? "" : "|",
                                  h_idx,
                                  static_cast<unsigned long long>(abs_handle),
                                  static_cast<unsigned>(href.code),
                                  ref_type,
                                  ref_class.c_str());
                }
                dwg_debug_log("\n");
            }
        }
        if (contains_ascii_ci(class_name_for_debug, "LINERES")) {
            const size_t hs_bit_start = main_data_bits;
            const size_t hs_bit_end = entity_bits;
            const size_t hs_bits = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
            if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                hreader.set_bit_offset(hs_bit_start);
                hreader.set_bit_limit(hs_bit_end);
                for (int h_idx = 0; h_idx < 24 && !hreader.has_error(); ++h_idx) {
                    auto href = hreader.read_h();
                    if (hreader.has_error()) break;
                    if (href.value == 0 && href.code == 0) continue;
                    const uint64_t abs_handle = resolve_handle_ref(handle, href);
                    if (abs_handle == 0) continue;
                    const uint32_t target_type = object_type_for_handle(abs_handle);
                    if (target_type == 19) {
                        if (line_resource_line_handles.insert(abs_handle).second) {
                            custom_line_resource_refs++;
                        }
                    } else if (target_type == 0) {
                        custom_line_resource_unresolved_refs++;
                    }
                }
            }
        }

        const bool note_text_fallback_class =
            !is_graphic &&
            contains_ascii_ci(class_name_for_debug, "NOTE") &&
            !contains_ascii_ci(class_name_for_debug, "TEMPLATE") &&
            !contains_ascii_ci(class_name_for_debug, "STANDARD") &&
            !contains_ascii_ci(class_name_for_debug, "STYLE") &&
            !contains_ascii_ci(class_name_for_debug, "CONTEXT");
        if (note_text_fallback_class) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            std::vector<std::string> snippets =
                extract_printable_strings(entity_data, entity_data_bytes, 12);
            std::string note_text = select_annotation_text_snippet(snippets);
            std::vector<std::pair<double, double>> raw_points =
                extract_plausible_raw_points(entity_data, entity_data_bytes, 80);
            Vec3 anchor = Vec3::zero();
            std::vector<Vec3> leader_path = select_annotation_leader_path(raw_points);
            if (!note_text.empty() && select_annotation_anchor(raw_points, anchor)) {
                TextEntity text;
                text.insertion_point = anchor;
                text.height = 39.375f;
                text.width_factor = 1.0f;
                text.text = note_text;

                EntityHeader note_hdr = entity_hdr;
                note_hdr.type = EntityType::MText;
                note_hdr.space = DrawingSpace::ModelSpace;
                note_hdr.bounds = entity_bounds_text(text);

                EntityVariant note_entity;
                note_entity.header = note_hdr;
                note_entity.data.emplace<7>(std::move(text));
                scene.add_entity(std::move(note_entity));
                custom_annotation_text_fallbacks++;
            }

            if (leader_path.size() >= 2) {
                for (size_t pi = 1; pi < leader_path.size(); ++pi) {
                    LineEntity leader;
                    leader.start = leader_path[pi - 1];
                    leader.end = leader_path[pi];

                    EntityHeader leader_hdr = entity_hdr;
                    leader_hdr.type = EntityType::Line;
                    leader_hdr.space = DrawingSpace::ModelSpace;
                    leader_hdr.color_override = 3;  // ACI green: mechanical note leader proxy.
                    leader_hdr.bounds = entity_bounds_line(leader);

                    EntityVariant leader_entity;
                    leader_entity.header = leader_hdr;
                    leader_entity.data.emplace<0>(std::move(leader));
                    scene.add_entity(std::move(leader_entity));
                }
                custom_note_leader_proxies++;
            }
        }

        const bool datum_target_fallback_class =
            !is_graphic &&
            contains_ascii_ci(class_name_for_debug, "DATUMTARGET") &&
            !contains_ascii_ci(class_name_for_debug, "TEMPLATE") &&
            !contains_ascii_ci(class_name_for_debug, "STANDARD") &&
            !contains_ascii_ci(class_name_for_debug, "STYLE") &&
            !contains_ascii_ci(class_name_for_debug, "CONTEXT");
        if (datum_target_fallback_class) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            std::vector<std::pair<double, double>> raw_points =
                extract_plausible_raw_points(entity_data, entity_data_bytes, 96);
            std::vector<Vec3> leader_path = select_annotation_leader_path(raw_points);
            if (leader_path.size() >= 2) {
                Vec3 callout_center = leader_path.front();
                Vec3 callout_target = leader_path[1];
                (void)select_annotation_callout_proxy(
                    raw_points, leader_path, callout_center, callout_target);

                std::vector<Vec3> visual_leader_path = leader_path;
                size_t closest_to_callout = 0;
                float closest_dist = 1.0e9f;
                for (size_t i = 0; i < visual_leader_path.size(); ++i) {
                    const float dx = visual_leader_path[i].x - callout_center.x;
                    const float dy = visual_leader_path[i].y - callout_center.y;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    if (std::isfinite(dist) && dist < closest_dist) {
                        closest_dist = dist;
                        closest_to_callout = i;
                    }
                }
                if (!visual_leader_path.empty()) {
                    visual_leader_path[closest_to_callout] = callout_center;
                }

                for (size_t pi = 1; pi < visual_leader_path.size(); ++pi) {
                    LineEntity leader;
                    leader.start = visual_leader_path[pi - 1];
                    leader.end = visual_leader_path[pi];

                    EntityHeader leader_hdr = entity_hdr;
                    leader_hdr.type = EntityType::Line;
                    leader_hdr.space = DrawingSpace::ModelSpace;
                    leader_hdr.color_override = 3;  // ACI green: mechanical leader proxy.
                    leader_hdr.bounds = entity_bounds_line(leader);

                    EntityVariant leader_entity;
                    leader_entity.header = leader_hdr;
                    leader_entity.data.emplace<0>(std::move(leader));
                    scene.add_entity(std::move(leader_entity));
                }

                CircleEntity callout;
                callout.center = callout_center;
                callout.radius = datum_callout_radius(callout_center, callout_target);

                EntityHeader callout_hdr = entity_hdr;
                callout_hdr.type = EntityType::Circle;
                callout_hdr.space = DrawingSpace::ModelSpace;
                callout_hdr.color_override = 2;  // ACI yellow: datum/callout bubble proxy.
                callout_hdr.bounds = entity_bounds_circle(callout);

                EntityVariant callout_entity;
                callout_entity.header = callout_hdr;
                callout_entity.data.emplace<1>(std::move(callout));
                scene.add_entity(std::move(callout_entity));

                TextEntity callout_label;
                callout_label.insertion_point = callout_center;
                callout_label.width_factor = 1.0f;
                callout_label.alignment = 1;
                callout_label.text = std::to_string(custom_datum_target_proxies + 1);
                const float label_scale =
                    callout_label.text.size() > 1 ? 0.58f : 0.72f;
                callout_label.height = std::clamp(
                    callout.radius * label_scale,
                    8.0f,
                    22.0f);

                EntityHeader label_hdr = entity_hdr;
                label_hdr.type = EntityType::Text;
                label_hdr.space = DrawingSpace::ModelSpace;
                label_hdr.color_override = 2;  // ACI yellow: datum/callout bubble label proxy.
                label_hdr.bounds = entity_bounds_text(callout_label);

                EntityVariant label_entity;
                label_entity.header = label_hdr;
                label_entity.data.emplace<6>(std::move(callout_label));
                scene.add_entity(std::move(label_entity));

                custom_datum_target_proxies++;
                custom_datum_target_label_proxies++;
            }
        }

        // Capture string stream state for later restore
        size_t str_bit_pos = reader.string_stream_bit_pos();
        bool has_string_stream = reader.has_string_stream();

        // ---- Handle BLOCK_HEADER (type 49) inline ----
        if (obj_type == 49) {
            // R2004+ BLOCK_HEADER uses COMMON_TABLE_FLAGS first:
            //   T(name), BS(is_xref_resolved), H(xref in handle stream),
            // followed by block flags and num_owned. The previous reader
            // treated the first fields as generic BLs, which shifted the
            // stream and lost *MODEL_SPACE in R2013 drawings.
            std::string block_name = reader.read_t();
            (void)reader.read_bs();  // is_xref_resolved
            const bool anonymous = reader.read_b();
            const bool hasattrs = reader.read_b();
            const bool blkisxref = reader.read_b();
            const bool xrefoverlaid = reader.read_b();
            if (m_version >= DwgVersion::R2000) {
                (void)reader.read_b();  // xref_loaded
            }
            uint32_t num_owned = 0;
            if (!blkisxref && !xrefoverlaid) {
                num_owned = reader.read_bl();
            }
            (void)anonymous;
            (void)hasattrs;

            if (!block_name.empty()) {
                m_sections.block_names[handle] = block_name;
            }

            if (num_owned < 0x0f00000u) {
                size_t hs_bit_start = main_data_bits;
                size_t hs_bit_end   = entity_bits;
                size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
                if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                    DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                    hreader.set_bit_offset(hs_bit_start);
                    hreader.set_bit_limit(hs_bit_end);

                    auto block_entity_ref = hreader.read_h();
                    if (!hreader.has_error()) {
                        uint64_t block_entity_handle = resolve_handle_ref(handle, block_entity_ref);
                        if (block_entity_handle != 0 && !block_name.empty()) {
                            block_names_from_entities[block_entity_handle] = block_name;
                        }
                    }

                    uint32_t collected = 0;
                    for (uint32_t i = 0; i < num_owned && !hreader.has_error(); ++i) {
                        auto entity_ref = hreader.read_h();
                        if (hreader.has_error()) break;
                        uint64_t entity_handle = resolve_handle_ref(handle, entity_ref);
                        if (entity_handle != 0) {
                            entity_handle_to_block_header[entity_handle] = handle;
                            collected++;
                        }
                    }
                    if (dwg_debug_enabled() && (!block_name.empty() || collected > 0 || num_owned > 1000)) {
                        dwg_debug_log("[DWG] block_header handle=%llu name='%s' owned=%u collected=%u\n",
                                      static_cast<unsigned long long>(handle),
                                      block_name.c_str(),
                                      num_owned,
                                      collected);
                    }
                }
            }

            non_graphic_count++;
            continue;
        }

        // Skip all other non-graphic objects
        if (!is_graphic) {
            if (obj_type == 82) {
                const int32_t layout_index = parse_layout_object(reader, scene, m_version);
                if (layout_index < 0) {
                    scene.add_diagnostic({
                        "dwg_layout_parse_failed",
                        "Parse gap",
                        "A DWG LAYOUT object was detected but its PlotSettings/Layout payload could not be decoded.",
                        1,
                    });
                } else {
                    size_t hs_bit_start = main_data_bits;
                    size_t hs_bit_end   = entity_bits;
                    size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
                    if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                        DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                        hreader.set_bit_offset(hs_bit_start);
                        hreader.set_bit_limit(hs_bit_end);
                        auto& refs = layout_handle_refs[layout_index];
                        for (int h_idx = 0; h_idx < 32 && !hreader.has_error(); ++h_idx) {
                            auto href = hreader.read_h();
                            if (hreader.has_error()) break;
                            if (href.value == 0 && href.code == 0) continue;
                            uint64_t abs_handle = resolve_handle_ref(handle, href);
                            if (abs_handle != 0) {
                                refs.push_back(abs_handle);
                            }
                        }
                    }
                }
            }
            // Handle table objects (LAYER, LTYPE, STYLE, DIMSTYLE)
            if (obj_type == 51 || obj_type == 53 ||
                obj_type == 65 ||
                obj_type == 57 || obj_type == 69) {
                parse_dwg_table_object(reader, obj_type, scene,
                                       m_version, entity_bits, main_data_bits,
                                       handle,
                                       &m_layer_handle_to_index,
                                       &m_linetype_handle_to_index);
            }
            non_graphic_count++;
            continue;
        }

        // ---- Handle BLOCK / ENDBLK for block definition tracking ----
        if (obj_type == 4) { // BLOCK
            if (is_r2010_plus) {
                (void)reader.read_raw_char(); // class_version (RC)
            }
            std::string block_name;
            if (has_string_stream) {
                block_name = reader.read_t();
            } else if (is_r2007_plus) {
                block_name = reader.read_tu();
            } else {
                block_name = reader.read_tv();
            }
            // Read 3BD(base_pt) + BS(flag) + B(xref)
            double bpx = reader.read_bd();
            double bpy = reader.read_bd();
            double bpz = reader.read_bd();
            (void)reader.read_bs();
            (void)reader.read_b();

            m_current_block_name = block_name;
            m_current_block_base_point = Vec3{
                static_cast<float>(bpx), static_cast<float>(bpy), static_cast<float>(bpz)};
            m_block_entity_start = scene.entities().size();

            // Store mapping from BLOCK entity handle → block name.
            // In DWG, a BLOCK entity and its BLOCK_HEADER table object often share
            // the same handle value. By also mapping the BLOCK entity's own handle,
            // INSERT entities can resolve their block_header handles directly.
            if (!block_name.empty()) {
                block_names_from_entities[handle] = block_name;
            }

            // Read handle stream to find BLOCK_HEADER handle.
            // Store mapping: BLOCK_HEADER handle → correct name from BLOCK entity.
            // Handle references use relative encoding: target = entity_handle - value - 1
            // for most codes, or absolute for code 6.
            if (!block_name.empty()) {
                size_t hs_bit_start = main_data_bits;
                size_t hs_bit_end   = entity_bits;
                size_t hs_bits = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
                if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                    DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                    hreader.set_bit_offset(hs_bit_start);
                    hreader.set_bit_limit(hs_bit_end);
                    for (int h_idx = 0; h_idx < 20 && !hreader.has_error(); ++h_idx) {
                        auto href = hreader.read_h();
                        if (hreader.has_error()) break;
                        if (href.value == 0 && href.code == 0) break;

                        uint64_t abs_handle = 0;
                        abs_handle = resolve_handle_ref(handle, href);
                        if (abs_handle != 0 && abs_handle != handle) {
                            block_names_from_entities[abs_handle] = block_name;
                        }
                    }
                }
            }
            non_graphic_count++;
            continue;
        }

        if (obj_type == 5) { // ENDBLK
            if (!m_current_block_name.empty()) {
                Block block;
                block.name = m_current_block_name;
                block.base_point = m_current_block_base_point;
                for (size_t i = m_block_entity_start; i < scene.entities().size(); ++i) {
                    block.entity_indices.push_back(static_cast<int32_t>(i));
                }
                int32_t block_idx = scene.add_block(block);
                const auto& added_block = scene.blocks()[static_cast<size_t>(block_idx)];
                for (size_t i = m_block_entity_start; i < scene.entities().size(); ++i) {
                    auto& child = scene.entities()[i].header;
                    child.in_block = true;
                    child.owner_block_index = block_idx;
                    if (added_block.is_paper_space) {
                        child.space = DrawingSpace::PaperSpace;
                    } else if (added_block.is_model_space) {
                        child.space = DrawingSpace::ModelSpace;
                    }
                }
                m_current_block_name.clear();
                m_current_block_base_point = Vec3::zero();
            }
            non_graphic_count++;
            continue;
        }

        // ---- Create reader for entity-specific parsing ----
        auto make_reader = [&]() -> DwgBitReader {
            DwgBitReader r2(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
            r2.set_bit_limit(entity_bits);
            r2.set_bit_offset(reader.bit_offset());
            r2.set_r2007_plus(is_r2007_plus);

            if (has_string_stream) {
                r2.restore_string_stream(
                    obj_data + offset + ms_bytes + umc_bytes,
                    entity_data_bytes,
                    str_bit_pos);
            }
            return r2;
        };

        // ---- Dispatch to entity-specific parser ----
        size_t entities_before = scene.entities().size();
        {
            DwgBitReader entity_reader = make_reader();
            parse_dwg_entity(entity_reader, obj_type, entity_hdr, scene, m_version);
        }

        // ---- Handle stream: capture common handles for space/block semantics ----
        // The owner handle is not consistently the first handle in every DWG
        // object flavor; some files put layer/style handles first. Preserve the
        // bounded stream and choose the semantic owner during post-processing.
        if (scene.entities().size() > entities_before) {
            for (size_t eidx = entities_before; eidx < scene.entities().size(); ++eidx) {
                entity_object_handles[eidx] = handle;
            }

            size_t hs_bit_start = main_data_bits;
            size_t hs_bit_end   = entity_bits;
            size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
            if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                hreader.set_bit_offset(hs_bit_start);
                hreader.set_bit_limit(hs_bit_end);
                std::vector<uint64_t> handles;
                for (int h_idx = 0; h_idx < 30 && !hreader.has_error(); ++h_idx) {
                    auto href = hreader.read_h();
                    if (hreader.has_error()) break;
                    if (href.value == 0 && href.code == 0) continue;
                    uint64_t abs_handle = resolve_handle_ref(handle, href);
                    if (abs_handle != 0) {
                        handles.push_back(abs_handle);
                    }
                }
                if (!handles.empty()) {
                    for (size_t eidx = entities_before; eidx < scene.entities().size(); ++eidx) {
                        entity_common_handles[eidx] = handles;
                        // Kept for diagnostics/fallback compatibility; the
                        // post-process resolver below scans all handles.
                        entity_owner_handles[eidx] = handles.front();
                    }
                }
            }
        }

        // ---- Handle stream for INSERT block_header resolution ----
        // Collect handles for post-processing (block_names may not be populated yet)
        if (obj_type == 7 || obj_type == 8) {
            size_t hs_bit_start = main_data_bits;
            size_t hs_bit_end   = entity_bits;
            size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;

            if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                hreader.set_bit_offset(hs_bit_start);
                hreader.set_bit_limit(hs_bit_end);

                std::vector<uint64_t> handles;
                for (int h_idx = 0; h_idx < 20 && !hreader.has_error(); ++h_idx) {
                    auto href = hreader.read_h();
                    if (hreader.has_error()) break;
                    if (href.value == 0 && href.code == 0) break;

                    uint64_t abs_handle = resolve_handle_ref(handle, href);
                    if (abs_handle != 0) {
                        handles.push_back(abs_handle);
                    }
                }
                if (!handles.empty() && !scene.entities().empty()) {
                    size_t eidx = scene.entities().size() - 1;
                    insert_handles[eidx] = std::move(handles);
                }
            }
        }

        // ---- Handle stream: resolve layer handle for all graphic entities ----
        // Handle stream layout (ODA spec 2.13):
        //   owner_handle(1) + reactor_handles(N) + xdicobjhandle(optional, 1) + layer_handle(1) + ...
        // Handle reference codes determine absolute value:
        //   code 2-5: TYPEDOBJHANDLE — href.value is absolute handle
        //   code 6:   OFFSETOBJHANDLE — abs = entity_handle + 1
        //   code 8:   OFFSETOBJHANDLE — abs = entity_handle - 1
        //   code 0xA: OFFSETOBJHANDLE — abs = entity_handle + href.value
        //   code 0xC: OFFSETOBJHANDLE — abs = entity_handle - href.value
        if (!m_layer_handle_to_index.empty() && !scene.entities().empty()) {
            size_t hs_bit_start = main_data_bits;
            size_t hs_bit_end   = entity_bits;
            size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;

            if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                // Scan all handles in the handle stream for a layer match.
                // This approach is more robust than positional skip because
                // different entity types have varying handle layouts (some have
                // no owner handle, some have extra type-specific handles).
                DwgBitReader scan(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                scan.set_bit_offset(hs_bit_start);
                scan.set_bit_limit(hs_bit_end);
                for (int h_idx = 0; h_idx < 30 && !scan.has_error(); ++h_idx) {
                    auto href = scan.read_h();
                    if (scan.has_error()) break;
                    if (href.value == 0 && href.code == 0) break;

                    uint64_t abs_h = resolve_handle_ref(handle, href);
                    auto it = m_layer_handle_to_index.find(abs_h);
                    if (it != m_layer_handle_to_index.end()) {
                        scene.entities().back().header.layer_index = it->second;
                        g_layer_resolved++;
                        break;
                    }
                }
            }
        }

        graphic_count++;
    }

    dwg_debug_log("[DWG] parse_objects: %zu graphic, %zu non-graphic, %zu errors\n",
            graphic_count, non_graphic_count, error_count);
    size_t line_resource_entities_found = 0;
    size_t line_resource_entities_in_block = 0;
    if (!line_resource_line_handles.empty()) {
        std::unordered_map<uint64_t, size_t> entity_index_by_handle;
        for (const auto& [eidx, entity_handle] : entity_object_handles) {
            entity_index_by_handle[entity_handle] = eidx;
        }
        const auto& all_entities = scene.entities();
        for (uint64_t line_handle : line_resource_line_handles) {
            auto it = entity_index_by_handle.find(line_handle);
            if (it == entity_index_by_handle.end() || it->second >= all_entities.size()) {
                continue;
            }
            line_resource_entities_found++;
            if (all_entities[it->second].header.in_block) {
                line_resource_entities_in_block++;
            }
            if (dwg_debug_enabled()) {
                const auto& hdr = all_entities[it->second].header;
                dwg_debug_log("[DWG] lineres target line handle=%llu entity=%zu in_block=%u space=%u layer=%d\n",
                              static_cast<unsigned long long>(line_handle),
                              it->second,
                              hdr.in_block ? 1u : 0u,
                              static_cast<unsigned>(hdr.space),
                              hdr.layer_index);
            }
        }
    }
    if (!line_resource_line_handles.empty()) {
        dwg_debug_log("[DWG] line resources: refs=%zu entities=%zu in_block=%zu\n",
                      line_resource_line_handles.size(),
                      line_resource_entities_found,
                      line_resource_entities_in_block);
    }
    if (recovered_object_offsets > 0 || unrecovered_object_offsets > 0) {
        dwg_debug_log("[DWG] object-map recovery: recovered=%zu unrecovered=%zu scans=%zu\n",
                      recovered_object_offsets,
                      unrecovered_object_offsets,
                      object_recovery_scans);
    }
    if (primary_self_handle_mismatches > 0) {
        dwg_debug_log("[DWG] object-map self mismatches: %zu\n",
                      primary_self_handle_mismatches);
    }
    auto object_type_for_summary = [&](uint64_t h) -> uint32_t {
        auto it = handle_object_types.find(h);
        if (it != handle_object_types.end()) {
            return it->second;
        }
        auto offset_it = m_sections.handle_map.find(h);
        if (offset_it == m_sections.handle_map.end()) {
            return 0;
        }
        ObjectRecord record;
        if (!prepare_object_record(offset_it->second, record, false, false, nullptr)) {
            return 0;
        }
        return record.obj_type;
    };
    auto class_name_for_summary = [&](uint32_t type) -> std::string {
        auto it = m_sections.class_map.find(type);
        if (it != m_sections.class_map.end()) {
            return it->second.first;
        }
        return {};
    };
    {
        std::vector<uint64_t> pending_dictionaries(
            custom_annotation_dictionary_refs.begin(),
            custom_annotation_dictionary_refs.end());
        for (size_t qi = 0; qi < pending_dictionaries.size() && qi < 128; ++qi) {
            const uint64_t dict_handle = pending_dictionaries[qi];
            auto refs_it = dictionary_handle_refs.find(dict_handle);
            if (refs_it == dictionary_handle_refs.end()) {
                continue;
            }
            for (uint64_t ref : refs_it->second) {
                const uint32_t ref_type = object_type_for_summary(ref);
                if (ref_type == 42 || ref_type == 43) {
                    if (custom_annotation_dictionary_refs.insert(ref).second) {
                        pending_dictionaries.push_back(ref);
                    }
                } else if (ref_type == 70 || ref_type == 79) {
                    custom_annotation_xrecord_refs.insert(ref);
                }
            }
        }
    }
    if (dwg_debug_enabled() && !custom_annotation_dictionary_refs.empty()) {
        std::vector<uint64_t> dictionary_refs(
            custom_annotation_dictionary_refs.begin(),
            custom_annotation_dictionary_refs.end());
        std::sort(dictionary_refs.begin(), dictionary_refs.end());
        const size_t dictionary_limit = std::min<size_t>(dictionary_refs.size(), 24);
        for (size_t di = 0; di < dictionary_limit; ++di) {
            const uint64_t dict_handle = dictionary_refs[di];
            dwg_debug_log("[DWG] custom_dictionary handle=%llu strings=",
                          static_cast<unsigned long long>(dict_handle));
            auto strings_it = dictionary_string_samples.find(dict_handle);
            if (strings_it != dictionary_string_samples.end()) {
                const size_t string_limit = std::min<size_t>(strings_it->second.size(), 8);
                for (size_t si = 0; si < string_limit; ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  strings_it->second[si].c_str());
                }
            }
            dwg_debug_log(" refs=");
            auto refs_it = dictionary_handle_refs.find(dict_handle);
            if (refs_it != dictionary_handle_refs.end()) {
                const size_t ref_limit = std::min<size_t>(refs_it->second.size(), 16);
                for (size_t ri = 0; ri < ref_limit; ++ri) {
                    const uint64_t ref = refs_it->second[ri];
                    const uint32_t ref_type = object_type_for_summary(ref);
                    const std::string ref_class = class_name_for_summary(ref_type);
                    dwg_debug_log("%s%llu(type=%u,class='%s')",
                                  ri == 0 ? "" : "|",
                                  static_cast<unsigned long long>(ref),
                                  ref_type,
                                  ref_class.c_str());
                }
            }
            dwg_debug_log("\n");
        }
    }
    if (dwg_debug_enabled() && !custom_annotation_xrecord_refs.empty()) {
        std::vector<uint64_t> xrecord_refs(
            custom_annotation_xrecord_refs.begin(),
            custom_annotation_xrecord_refs.end());
        std::sort(xrecord_refs.begin(), xrecord_refs.end());
        const size_t xrecord_limit = std::min<size_t>(xrecord_refs.size(), 24);
        for (size_t xi = 0; xi < xrecord_limit; ++xi) {
            const uint64_t record_handle = xrecord_refs[xi];
            dwg_debug_log("[DWG] custom_xrecord handle=%llu strings=",
                          static_cast<unsigned long long>(record_handle));
            auto strings_it = xrecord_string_samples.find(record_handle);
            if (strings_it != xrecord_string_samples.end()) {
                const size_t string_limit = std::min<size_t>(strings_it->second.size(), 8);
                for (size_t si = 0; si < string_limit; ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  strings_it->second[si].c_str());
                }
            }
            dwg_debug_log(" refs=");
            auto refs_it = xrecord_handle_refs.find(record_handle);
            if (refs_it != xrecord_handle_refs.end()) {
                const size_t ref_limit = std::min<size_t>(refs_it->second.size(), 16);
                for (size_t ri = 0; ri < ref_limit; ++ri) {
                    const uint64_t ref = refs_it->second[ri];
                    const uint32_t ref_type = object_type_for_summary(ref);
                    const std::string ref_class = class_name_for_summary(ref_type);
                    dwg_debug_log("%s%llu(type=%u,class='%s')",
                                  ri == 0 ? "" : "|",
                                  static_cast<unsigned long long>(ref),
                                  ref_type,
                                  ref_class.c_str());
                }
            }
            dwg_debug_log("\n");
        }
    }
    if (recovered_object_offsets > 0) {
        scene.add_diagnostic({
            "dwg_object_map_offset_recovered",
            "Parse gap",
            "One or more DWG Object Map offsets were invalid and recovered by matching the object's own handle.",
            static_cast<int32_t>(std::min<size_t>(recovered_object_offsets, 2147483647u)),
        });
    }
    if (unrecovered_object_offsets > 0) {
        scene.add_diagnostic({
            "dwg_object_map_offset_invalid",
            "Parse gap",
            "One or more DWG Object Map offsets were invalid and could not be recovered without ambiguity.",
            static_cast<int32_t>(std::min<size_t>(unrecovered_object_offsets, 2147483647u)),
        });
    }
    if (custom_annotation_text_fallbacks > 0) {
        scene.add_diagnostic({
            "custom_annotation_text_fallback",
            "Render gap",
            "Text was recovered from custom DWG annotation objects using a conservative proxy-text fallback; native object geometry is still pending.",
            static_cast<int32_t>(std::min<size_t>(custom_annotation_text_fallbacks, 2147483647u)),
        });
    }
    if (custom_note_leader_proxies > 0) {
        scene.add_diagnostic({
            "custom_note_leader_proxy",
            "Render gap",
            "Leader geometry was recovered from custom DWG note objects using conservative proxy linework.",
            static_cast<int32_t>(std::min<size_t>(custom_note_leader_proxies, 2147483647u)),
        });
    }
    if (custom_datum_target_proxies > 0) {
        scene.add_diagnostic({
            "custom_datum_target_proxy",
            "Render gap",
            "Datum/callout leader geometry was recovered from custom DWG annotation objects using conservative proxy lines and bubbles.",
            static_cast<int32_t>(std::min<size_t>(custom_datum_target_proxies, 2147483647u)),
        });
        scene.add_diagnostic({
            "custom_datum_target_label_proxy",
            "Semantic gap",
            "Datum/callout bubble labels were emitted as deterministic ordinal proxies because no stable native label field has been identified in the DWG custom object payload yet.",
            static_cast<int32_t>(std::min<size_t>(custom_datum_target_label_proxies, 2147483647u)),
        });
    }
    if (custom_line_resource_refs > 0) {
        scene.add_diagnostic({
            "custom_line_resource_refs",
            "Semantic gap",
            "AutoCAD Mechanical line resource objects referenced ordinary LINE entities; referenced geometry was tracked to avoid treating the resource object as standalone geometry.",
            static_cast<int32_t>(std::min<size_t>(custom_line_resource_refs, 2147483647u)),
        });
    }
    if (custom_line_resource_unresolved_refs > 0) {
        scene.add_diagnostic({
            "custom_line_resource_unresolved_refs",
            "Parse gap",
            "AutoCAD Mechanical line resource objects referenced handles that are not resolved to known DWG objects yet.",
            static_cast<int32_t>(std::min<size_t>(custom_line_resource_unresolved_refs, 2147483647u)),
        });
    }
    if (custom_field_objects > 0) {
        std::string message = "DWG FIELD objects were decoded through the R2007+ string stream";
        if (!custom_field_string_samples.empty()) {
            message += "; samples: ";
            for (size_t i = 0; i < custom_field_string_samples.size(); ++i) {
                if (i > 0) message += ", ";
                std::string sample = custom_field_string_samples[i];
                if (sample.size() > 48) {
                    sample = sample.substr(0, 45) + "...";
                }
                message += "'";
                message += sample;
                message += "'";
            }
        }
        message += ". FIELD evaluation and Mechanical label binding are still pending.";
        scene.add_diagnostic({
            "custom_field_string_stream_decoded",
            "Semantic gap",
            message,
            static_cast<int32_t>(std::min<size_t>(custom_field_objects, 2147483647u)),
        });
    }
    if (custom_fieldlist_objects > 0 || custom_mtext_context_objects > 0) {
        std::string message = "DWG FIELDLIST/MTEXT context objects were identified in the custom annotation handle graph.";
        scene.add_diagnostic({
            "custom_field_context_graph_identified",
            "Handle resolution gap",
            message,
            static_cast<int32_t>(std::min<size_t>(
                custom_fieldlist_objects + custom_mtext_context_objects,
                2147483647u)),
        });
    }
    if (!custom_annotation_dictionary_refs.empty() ||
        !custom_annotation_xrecord_refs.empty()) {
        size_t dictionaries_with_strings = 0;
        std::vector<std::string> dictionary_samples;
        for (uint64_t dict_handle : custom_annotation_dictionary_refs) {
            auto sample_it = dictionary_string_samples.find(dict_handle);
            if (sample_it != dictionary_string_samples.end() &&
                !sample_it->second.empty()) {
                dictionaries_with_strings++;
                for (const std::string& value : sample_it->second) {
                    if (dictionary_samples.size() >= 64) break;
                    bool duplicate = false;
                    for (const std::string& existing : dictionary_samples) {
                        if (existing == value) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        dictionary_samples.push_back(value);
                    }
                }
            }
        }
        auto sample_priority = [](const std::string& value) {
            const std::string upper = uppercase_ascii(value);
            if (upper.find("DETAILVIEW") != std::string::npos) return 0;
            if (upper.find("FIELD") != std::string::npos) return 1;
            if (upper.find("MTEXT") != std::string::npos ||
                upper.find("CONTEXT") != std::string::npos) return 2;
            if (upper.find("ANNOTATION") != std::string::npos ||
                upper.find("SCALE") != std::string::npos) return 3;
            if (upper.find("LAYOUT") != std::string::npos ||
                upper.find("MLEADER") != std::string::npos) return 4;
            return 5;
        };
        std::sort(dictionary_samples.begin(), dictionary_samples.end(),
                  [&](const std::string& a, const std::string& b) {
                      const int pa = sample_priority(a);
                      const int pb = sample_priority(b);
                      if (pa != pb) return pa < pb;
                      return a < b;
                  });

        std::string message =
            "Custom DWG annotation/detail objects reference DICTIONARY/XRECORD graph nodes";
        message += " (dictionaries=";
        message += std::to_string(custom_annotation_dictionary_refs.size());
        message += ", xrecords=";
        message += std::to_string(custom_annotation_xrecord_refs.size());
        message += ")";
        if (dictionaries_with_strings > 0) {
            message += "; decoded dictionary string samples: ";
            const size_t sample_limit = std::min<size_t>(dictionary_samples.size(), 6);
            for (size_t i = 0; i < sample_limit; ++i) {
                if (i > 0) message += ", ";
                std::string sample = dictionary_samples[i];
                if (sample.size() > 40) {
                    sample = sample.substr(0, 37) + "...";
                }
                message += "'";
                message += sample;
                message += "'";
            }
        }
        if (!custom_annotation_xrecord_refs.empty()) {
            size_t xrecords_with_strings = 0;
            for (uint64_t xrecord_handle : custom_annotation_xrecord_refs) {
                auto sample_it = xrecord_string_samples.find(xrecord_handle);
                if (sample_it != xrecord_string_samples.end() &&
                    !sample_it->second.empty()) {
                    xrecords_with_strings++;
                }
            }
            if (xrecords_with_strings > 0) {
                message += "; xrecords with decoded strings=";
                message += std::to_string(xrecords_with_strings);
            }
        }
        message += ". Native dictionary entry semantics are still pending.";
        scene.add_diagnostic({
            "custom_annotation_dictionary_graph_identified",
            "Handle resolution gap",
            message,
            static_cast<int32_t>(std::min<size_t>(
                custom_annotation_dictionary_refs.size() + custom_annotation_xrecord_refs.size(),
                2147483647u)),
        });
    }
    if (!custom_annotation_unresolved_refs.empty()) {
        scene.add_diagnostic({
            "custom_annotation_graph_unresolved_refs",
            "Handle resolution gap",
            "Custom DWG annotation/detail objects reference handles that do not resolve to known DWG objects yet.",
            static_cast<int32_t>(std::min<size_t>(
                custom_annotation_unresolved_refs.size(),
                2147483647u)),
        });
    }
    if (custom_detail_style_objects > 0 || custom_detail_custom_objects > 0) {
        scene.add_diagnostic({
            "mechanical_detail_view_graph_identified",
            "Custom object semantic gap",
            "AutoCAD Mechanical detail-view style/custom objects were identified; native crop/frame parameters still need semantic decoding, so visual detail frames may use fallback proxies.",
            static_cast<int32_t>(std::min<size_t>(
                custom_detail_style_objects + custom_detail_custom_objects,
                2147483647u)),
        });
    }
    if (dwg_debug_enabled()) {
        std::vector<std::pair<uint32_t, size_t>> type_counts(
            object_type_counts.begin(), object_type_counts.end());
        std::sort(type_counts.begin(), type_counts.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        const size_t limit = std::min<size_t>(type_counts.size(), 40);
        for (size_t i = 0; i < limit; ++i) {
            auto [type, count] = type_counts[i];
            auto cls = m_sections.class_map.find(type);
            const char* name = (cls != m_sections.class_map.end()) ? cls->second.first.c_str() : "";
            dwg_debug_log("[DWG] object_type type=%u count=%zu class='%s'\n",
                          type, count, name);
        }
    }
    {
        struct UnsupportedAnnotationClass {
            std::string name;
            size_t count = 0;
        };
        std::vector<UnsupportedAnnotationClass> unsupported_annotations;
        size_t unsupported_annotation_count = 0;
        size_t support_annotation_count = 0;
        size_t unresolved_note_count = 0;
        for (const auto& [type, count] : object_type_counts) {
            auto cls = m_sections.class_map.find(type);
            if (cls == m_sections.class_map.end() || cls->second.first.empty()) continue;

            const std::string& name = cls->second.first;
            const bool annotation_like =
                contains_ascii_ci(name, "LEADER") ||
                contains_ascii_ci(name, "DATUM") ||
                contains_ascii_ci(name, "NOTE") ||
                contains_ascii_ci(name, "FIELD") ||
                contains_ascii_ci(name, "MTEXT") ||
                contains_ascii_ci(name, "DIM") ||
                contains_ascii_ci(name, "DETAIL") ||
                contains_ascii_ci(name, "SECTION") ||
                contains_ascii_ci(name, "VIEW") ||
                contains_ascii_ci(name, "BALLOON") ||
                contains_ascii_ci(name, "CALLOUT");
            if (!annotation_like) continue;

            const bool is_proxied_datum_target =
                custom_datum_target_proxies > 0 &&
                contains_ascii_ci(name, "DATUMTARGET") &&
                !contains_ascii_ci(name, "STANDARD") &&
                !contains_ascii_ci(name, "TEMPLATE") &&
                !contains_ascii_ci(name, "STYLE") &&
                !contains_ascii_ci(name, "CONTEXT");
            if (is_proxied_datum_target) continue;

            const bool is_note_object =
                contains_ascii_ci(name, "NOTE") &&
                !contains_ascii_ci(name, "TEMPLATE") &&
                !contains_ascii_ci(name, "STANDARD") &&
                !contains_ascii_ci(name, "STYLE") &&
                !contains_ascii_ci(name, "CONTEXT");
            if (is_note_object) {
                const size_t recovered = std::min(count, custom_annotation_text_fallbacks);
                if (count > recovered) {
                    unresolved_note_count += count - recovered;
                }
                continue;
            }

            const bool is_annotation_support_object =
                contains_ascii_ci(name, "TEMPLATE") ||
                contains_ascii_ci(name, "STANDARD") ||
                contains_ascii_ci(name, "STYLE") ||
                contains_ascii_ci(name, "CONTEXT") ||
                contains_ascii_ci(name, "FIELD") ||
                contains_ascii_ci(name, "STD");
            if (is_annotation_support_object) {
                support_annotation_count += count;
                continue;
            }

            unsupported_annotations.push_back({name, count});
            unsupported_annotation_count += count;
        }
        if (!unsupported_annotations.empty()) {
            std::sort(unsupported_annotations.begin(), unsupported_annotations.end(),
                      [](const auto& a, const auto& b) {
                          if (a.count != b.count) return a.count > b.count;
                          return a.name < b.name;
                      });
            std::string message = "DWG custom annotation/mechanical objects are present but not fully rendered: ";
            const size_t limit = std::min<size_t>(unsupported_annotations.size(), 6);
            for (size_t i = 0; i < limit; ++i) {
                if (i > 0) message += ", ";
                message += unsupported_annotations[i].name;
                message += "(" + std::to_string(unsupported_annotations[i].count) + ")";
            }
            if (unsupported_annotations.size() > limit) {
                message += ", ...";
            }
            message += ".";
            scene.add_diagnostic({
                "unsupported_custom_annotation_objects",
                "Parse gap",
                message,
                static_cast<int32_t>(std::min<size_t>(unsupported_annotation_count, 2147483647u)),
            });
        }
        if (support_annotation_count > 0) {
            scene.add_diagnostic({
                "custom_annotation_support_objects_deferred",
                "Semantic gap",
                "Custom annotation support/style/context/field objects were identified but are not standalone render entities.",
                static_cast<int32_t>(std::min<size_t>(support_annotation_count, 2147483647u)),
            });
        }
        if (unresolved_note_count > 0) {
            scene.add_diagnostic({
                "custom_note_text_unresolved",
                "Parse gap",
                "One or more custom note objects did not expose a stable text payload for proxy text recovery.",
                static_cast<int32_t>(std::min<size_t>(unresolved_note_count, 2147483647u)),
            });
        }
    }
    // ---- Post-processing: resolve INSERT block_index ----
    // After all objects are parsed, BLOCK_HEADER names (type 49) and
    // BLOCK/ENDBLK definitions are fully populated. Now resolve any INSERTs
    // that couldn't be resolved during the main loop due to ordering.
    //
    // block_names_from_entities contains two key types of entries:
    //   - BLOCK entity handle → full block name (primary, always correct)
    //   - BLOCK_HEADER handle → full block name (from handle stream, also correct)
    //
    // m_sections.block_names contains BLOCK_HEADER (type 49) entries, but those
    // names are truncated for anonymous dimension blocks (e.g., "*D" instead of
    // "*D1077"). Use as a fallback only when the entity map doesn't have the name.
    {
        size_t resolved = 0;
        bool apply = true;
        if (apply) {
            auto& all_entities = scene.entities();
            for (auto& [eidx, handles] : insert_handles) {
                if (eidx >= all_entities.size()) continue;
                auto* ins = std::get_if<InsertEntity>(&all_entities[eidx].data);
                if (!ins || ins->block_index >= 0) continue;

                for (uint64_t h : handles) {
                    std::string name;
                    auto it1 = block_names_from_entities.find(h);
                    if (it1 != block_names_from_entities.end()) {
                        name = it1->second;
                    }
                    if (name.empty()) {
                        auto it2 = m_sections.block_names.find(h);
                        if (it2 != m_sections.block_names.end()) {
                            name = it2->second;
                        }
                    }
                    if (!name.empty()) {
                        int32_t block_idx = scene.find_block(name);
                        if (block_idx >= 0) {
                            ins->block_index = block_idx;
                            all_entities[eidx].header.block_index = block_idx;
                            resolved++;
                        }
                        break;
                    }
                }
            }
        }
        dwg_debug_log("[DWG] INSERT post-processing: resolved %zu / %zu INSERTs "
                "(entity_names=%zu, bh_names=%zu, blocks=%zu, apply=%s)\n",
                resolved, insert_handles.size(),
                block_names_from_entities.size(), m_sections.block_names.size(),
                scene.blocks().size(), apply ? "yes" : "no");
    }
    std::unordered_map<uint64_t, int32_t> layout_block_owner_handles;
    if (!layout_handle_refs.empty()) {
        auto block_name_for_handle = [&](uint64_t h) -> std::string {
            auto it1 = block_names_from_entities.find(h);
            if (it1 != block_names_from_entities.end()) {
                return it1->second;
            }
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) {
                return it2->second;
            }
            return {};
        };
        for (const auto& [layout_index, refs] : layout_handle_refs) {
            for (uint64_t h : refs) {
                std::string name = block_name_for_handle(h);
                std::string upper = name;
                for (char& c : upper) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                const bool is_space_name = (upper == "*MODEL_SPACE" || upper == "*PAPER_SPACE");
                const bool is_layout_block_header =
                    handle_object_types.find(h) != handle_object_types.end() &&
                    handle_object_types[h] == 49;
                if (is_space_name || is_layout_block_header) {
                    layout_block_owner_handles[h] = layout_index;
                }
            }
        }
        dwg_debug_log("[DWG] layout handles: layouts=%zu owner_block_refs=%zu\n",
                      layout_handle_refs.size(), layout_block_owner_handles.size());
    }
    size_t block_header_space_resolved = 0;
    size_t block_header_model = 0;
    size_t block_header_paper = 0;
    size_t block_header_entities_attached = 0;
    if (!entity_handle_to_block_header.empty() && !entity_object_handles.empty()) {
        auto block_name_for_handle = [&](uint64_t h) -> std::string {
            auto it1 = block_names_from_entities.find(h);
            if (it1 != block_names_from_entities.end()) {
                return it1->second;
            }
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) {
                return it2->second;
            }
            return {};
        };

        auto& all_entities = scene.entities();
        auto& all_blocks = scene.blocks();
        std::unordered_set<uint64_t> block_header_entity_membership;
        for (size_t bi = 0; bi < all_blocks.size(); ++bi) {
            for (int32_t existing_eidx : all_blocks[bi].header_owned_entity_indices) {
                if (existing_eidx < 0) continue;
                block_header_entity_membership.insert(
                    (static_cast<uint64_t>(bi) << 32) |
                    static_cast<uint32_t>(existing_eidx));
            }
        }
        for (const auto& [eidx, entity_handle] : entity_object_handles) {
            if (eidx >= all_entities.size()) continue;
            auto owner_it = entity_handle_to_block_header.find(entity_handle);
            if (owner_it == entity_handle_to_block_header.end()) continue;

            const uint64_t block_header_handle = owner_it->second;
            std::string name = block_name_for_handle(block_header_handle);
            std::string upper_name = name;
            for (char& c : upper_name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }

            auto& header = all_entities[eidx].header;
            auto layout_owner = layout_block_owner_handles.find(block_header_handle);
            if (layout_owner != layout_block_owner_handles.end()) {
                header.layout_index = layout_owner->second;
                const auto& layouts = scene.layouts();
                const bool is_model_layout =
                    layout_owner->second >= 0 &&
                    layout_owner->second < static_cast<int32_t>(layouts.size()) &&
                    layouts[static_cast<size_t>(layout_owner->second)].is_model_layout;
                header.space = is_model_layout ? DrawingSpace::ModelSpace : DrawingSpace::PaperSpace;
                header.in_block = false;
                header.owner_block_index = -1;
                if (is_model_layout) {
                    block_header_model++;
                } else {
                    block_header_paper++;
                }
                block_header_space_resolved++;
                continue;
            }

            if (upper_name == "*MODEL_SPACE") {
                header.space = DrawingSpace::ModelSpace;
                header.in_block = false;
                header.owner_block_index = -1;
                block_header_model++;
                block_header_space_resolved++;
            } else if (upper_name == "*PAPER_SPACE") {
                header.space = DrawingSpace::PaperSpace;
                header.in_block = false;
                header.owner_block_index = -1;
                block_header_paper++;
                block_header_space_resolved++;
            } else if (!name.empty()) {
                int32_t block_idx = scene.find_block(name);
                if (block_idx >= 0) {
                    header.owner_block_index = block_idx;
                    header.in_block = true;
                    const uint64_t membership_key =
                        (static_cast<uint64_t>(block_idx) << 32) |
                        static_cast<uint32_t>(eidx);
                    if (block_header_entity_membership.insert(membership_key).second) {
                        auto& block = all_blocks[static_cast<size_t>(block_idx)];
                        block.header_owned_entity_indices.push_back(static_cast<int32_t>(eidx));
                        block_header_entities_attached++;
                    }
                }
            }
        }
        dwg_debug_log("[DWG] block-header membership: resolved=%zu model=%zu paper=%zu attached=%zu members=%zu entity_handles=%zu\n",
                      block_header_space_resolved,
                      block_header_model,
                      block_header_paper,
                      block_header_entities_attached,
                      entity_handle_to_block_header.size(),
                      entity_object_handles.size());
    }
    size_t default_model_space = 0;
    if (block_header_model == 0 && block_header_paper == 0 && !scene.layouts().empty()) {
        auto& all_entities = scene.entities();
        for (const auto& [eidx, entity_handle] : entity_object_handles) {
            (void)entity_handle;
            if (eidx >= all_entities.size()) continue;
            auto& header = all_entities[eidx].header;
            if (header.space == DrawingSpace::Unknown && !header.in_block) {
                header.space = DrawingSpace::ModelSpace;
                default_model_space++;
            }
        }
        dwg_debug_log("[DWG] default model-space assignment: %zu entities\n",
                      default_model_space);
    }
    {
        size_t resolved = 0;
        size_t paper = 0;
        size_t model = 0;
        size_t normal_block = 0;
        auto& all_entities = scene.entities();
        auto block_name_for_handle = [&](uint64_t h) -> std::string {
            auto it1 = block_names_from_entities.find(h);
            if (it1 != block_names_from_entities.end()) {
                return it1->second;
            }
            auto it2 = m_sections.block_names.find(h);
            if (it2 != m_sections.block_names.end()) {
                return it2->second;
            }
            return {};
        };
        auto upper_block_name_for_handle = [&](uint64_t h) {
            std::string upper = block_name_for_handle(h);
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return upper;
        };
        auto choose_semantic_owner = [&](const std::vector<uint64_t>& handles) -> uint64_t {
            for (uint64_t h : handles) {
                if (layout_block_owner_handles.find(h) != layout_block_owner_handles.end()) {
                    return h;
                }
            }
            for (uint64_t h : handles) {
                const std::string upper_name = upper_block_name_for_handle(h);
                if (upper_name == "*MODEL_SPACE" || upper_name == "*PAPER_SPACE") {
                    return h;
                }
            }
            for (uint64_t h : handles) {
                const std::string name = block_name_for_handle(h);
                if (!name.empty()) {
                    return h;
                }
            }
            return 0;
        };

        for (const auto& [eidx, handles] : entity_common_handles) {
            if (eidx >= all_entities.size()) continue;
            const uint64_t owner_handle = choose_semantic_owner(handles);
            if (owner_handle == 0) continue;

            auto layout_owner = layout_block_owner_handles.find(owner_handle);
            if (layout_owner != layout_block_owner_handles.end()) {
                auto& header = all_entities[eidx].header;
                header.owner_block_index = -1;
                header.layout_index = layout_owner->second;
                header.in_block = false;
                const auto& layouts = scene.layouts();
                const bool is_model_layout =
                    layout_owner->second >= 0 &&
                    layout_owner->second < static_cast<int32_t>(layouts.size()) &&
                    layouts[static_cast<size_t>(layout_owner->second)].is_model_layout;
                header.space = is_model_layout ? DrawingSpace::ModelSpace : DrawingSpace::PaperSpace;
                if (is_model_layout) {
                    model++;
                } else {
                    paper++;
                }
                resolved++;
                continue;
            }

            std::string name = block_name_for_handle(owner_handle);
            if (name.empty()) continue;

            std::string upper_name = upper_block_name_for_handle(owner_handle);
            const bool is_model_space_owner = (upper_name == "*MODEL_SPACE");
            const bool is_paper_space_owner = (upper_name == "*PAPER_SPACE");

            int32_t block_idx = scene.find_block(name);
            if (block_idx < 0 && !is_model_space_owner && !is_paper_space_owner) continue;

            auto& header = all_entities[eidx].header;
            header.owner_block_index = block_idx;
            const Block* block = nullptr;
            if (block_idx >= 0) {
                block = &scene.blocks()[static_cast<size_t>(block_idx)];
            }
            if (is_paper_space_owner || (block && block->is_paper_space)) {
                header.space = DrawingSpace::PaperSpace;
                header.in_block = false;
                paper++;
            } else if (is_model_space_owner || (block && block->is_model_space)) {
                header.space = DrawingSpace::ModelSpace;
                header.in_block = false;
                model++;
            } else {
                // Keep existing in_block state. BLOCK/ENDBLK parsing already
                // marks real block-definition children. Owner handles can also
                // point to dictionaries or ordinary containers; hiding those
                // here would drop valid model-space geometry in real DWGs.
                normal_block++;
            }
            resolved++;
        }
        dwg_debug_log("[DWG] owner resolution: resolved=%zu model=%zu paper=%zu block=%zu owners=%zu\n",
                      resolved, model, paper, normal_block, entity_common_handles.size());
        if (dwg_debug_enabled() && !entity_common_handles.empty()) {
            std::unordered_map<uint64_t, size_t> owner_counts;
            for (const auto& [eidx, handles] : entity_common_handles) {
                (void)eidx;
                const uint64_t owner_handle = choose_semantic_owner(handles);
                if (owner_handle != 0) {
                    owner_counts[owner_handle]++;
                }
            }
            std::vector<std::pair<uint64_t, size_t>> ranked_owners(owner_counts.begin(), owner_counts.end());
            std::sort(ranked_owners.begin(), ranked_owners.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second != b.second) return a.second > b.second;
                          return a.first < b.first;
                      });

            auto block_name_for_debug = [&](uint64_t h) -> std::string {
                auto it1 = block_names_from_entities.find(h);
                if (it1 != block_names_from_entities.end()) {
                    return it1->second;
                }
                auto it2 = m_sections.block_names.find(h);
                if (it2 != m_sections.block_names.end()) {
                    return it2->second;
                }
                return {};
            };
            auto object_type_for_debug = [&](uint64_t h) -> uint32_t {
                auto it = handle_object_types.find(h);
                if (it != handle_object_types.end()) {
                    return it->second;
                }
                auto offset_it = m_sections.handle_map.find(h);
                if (offset_it == m_sections.handle_map.end()) {
                    return 0;
                }
                ObjectRecord record;
                if (!prepare_object_record(offset_it->second, record, false, false, nullptr)) {
                    return 0;
                }
                return record.obj_type;
            };
            auto class_name_for_debug_type = [&](uint32_t type) -> std::string {
                auto it = m_sections.class_map.find(type);
                if (it != m_sections.class_map.end()) {
                    return it->second.first;
                }
                return {};
            };

            const size_t owner_limit = std::min<size_t>(ranked_owners.size(), 12);
            for (size_t i = 0; i < owner_limit; ++i) {
                const uint64_t owner = ranked_owners[i].first;
                const std::string name = block_name_for_debug(owner);
                const uint32_t owner_type = object_type_for_debug(owner);
                const std::string owner_class = class_name_for_debug_type(owner_type);
                dwg_debug_log("[DWG] owner top[%zu]: handle=%llu count=%zu type=%u class='%s' name='%s'\n",
                              i,
                              static_cast<unsigned long long>(owner),
                              ranked_owners[i].second,
                              owner_type,
                              owner_class.c_str(),
                              name.c_str());
            }
            for (const auto& [layout_index, refs] : layout_handle_refs) {
                const size_t ref_limit = std::min<size_t>(refs.size(), 12);
                for (size_t i = 0; i < ref_limit; ++i) {
                    const uint64_t ref = refs[i];
                    const std::string name = block_name_for_debug(ref);
                    const uint32_t ref_type = object_type_for_debug(ref);
                    const std::string ref_class = class_name_for_debug_type(ref_type);
                    dwg_debug_log("[DWG] layout_ref layout=%d idx=%zu handle=%llu type=%u class='%s' name='%s'\n",
                                  layout_index,
                                  i,
                                  static_cast<unsigned long long>(ref),
                                  ref_type,
                                  ref_class.c_str(),
                                  name.c_str());
                }
            }
        }
        const bool has_resolved_space = std::any_of(
            all_entities.begin(), all_entities.end(),
            [](const EntityVariant& ent) {
                return ent.header.space == DrawingSpace::ModelSpace ||
                       ent.header.space == DrawingSpace::PaperSpace;
            });
        if (!entity_common_handles.empty() && !has_resolved_space) {
            scene.add_diagnostic({
                "dwg_space_owner_unresolved",
                "Semantic gap",
                "DWG entity owner handles were captured but could not be resolved to *MODEL_SPACE or *PAPER_SPACE block records.",
                static_cast<int32_t>(entity_common_handles.size()),
            });
        }
    }
    dwg_debug_log("[DWG] Layer resolution: resolved=%zu map_size=%zu\n",
            g_layer_resolved, m_layer_handle_to_index.size());

    return Result::success();
}

// ============================================================
// Helper: find_page_map_entry
// ============================================================

const SectionPageMapEntry* DwgParser::find_page_map_entry(int32_t page_number) const
{
    for (const auto& entry : m_page_map_entries) {
        if (entry.number == page_number) {
            return &entry;
        }
    }
    return nullptr;
}

// ============================================================
// Helper: read_modular_char — variable-length unsigned encoding (UMC)
// High bit of each byte = more bytes follow. Each byte contributes 7 bits.
// First byte is in lowest position (little-endian bit packing).
// ============================================================

uint32_t DwgParser::read_modular_char(const uint8_t* data, size_t size, size_t& offset)
{
    uint32_t result = 0;
    int shift = 0;
    for (int i = 0; i < 8 && offset < size; ++i) {
        uint8_t byte_val = data[offset++];
        result |= static_cast<uint32_t>(byte_val & 0x7F) << shift;
        shift += 7;
        if ((byte_val & 0x80) == 0) break;
    }
    return result;
}

// ============================================================
// Helper: read_modular_char_signed — signed modular char (MC)
// High bit of each byte = more bytes follow.
// Last byte uses bit 6 as sign flag, bits 0-5 as data.
// Previous bytes contribute 7 bits each.
// First byte is in lowest position (little-endian bit packing).
// ============================================================

int32_t DwgParser::read_modular_char_signed(const uint8_t* data, size_t size, size_t& offset)
{
    int32_t result = 0;
    bool negative = false;
    int shift = 0;

    for (int i = 0; i < 5 && offset < size; ++i) {
        uint8_t byte_val = data[offset++];

        if (byte_val & 0x80) {
            // More bytes follow: 7 data bits
            result |= static_cast<int32_t>(byte_val & 0x7F) << shift;
            shift += 7;
        } else {
            // Last byte: bit 6 = sign, bits 0-5 = data
            negative = (byte_val & 0x40) != 0;
            result |= static_cast<int32_t>(byte_val & 0x3F) << shift;
            break;
        }
    }

    return negative ? -result : result;
}

// ============================================================
// Helper: little-endian readers
// ============================================================

uint32_t DwgParser::read_le32(const uint8_t* data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint16_t DwgParser::read_le16(const uint8_t* data, size_t offset)
{
    return static_cast<uint16_t>(data[offset]) |
           (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint64_t DwgParser::read_le48(const uint8_t* data, size_t offset)
{
    return static_cast<uint64_t>(data[offset]) |
           (static_cast<uint64_t>(data[offset + 1]) << 8) |
           (static_cast<uint64_t>(data[offset + 2]) << 16) |
           (static_cast<uint64_t>(data[offset + 3]) << 24) |
           (static_cast<uint64_t>(data[offset + 4]) << 32) |
           (static_cast<uint64_t>(data[offset + 5]) << 40);
}

// ============================================================
// Helper: decrypt_section_page_header
//
// R2004+ regular section page headers are XOR-encrypted.
// Per libredwg decrypt_R2004_section_page_header:
//   sec_mask = htole32(0x4164536b ^ address)
//   Each uint32 word: decrypted[k] = encrypted[k] ^ sec_mask
// where address is the file offset of the page header.
//
// The section PAGE MAP header (0x41630e3b) is NOT encrypted.
// This function is only for regular section data pages.
// ============================================================

void DwgParser::decrypt_section_page_header(const uint8_t* encrypted,
                                             uint8_t* decrypted,
                                             size_t address)
{
    if (!encrypted || !decrypted) return;

    // Compute XOR mask: 0x4164536b ^ address (little-endian)
    uint32_t mask = 0x4164536bu ^ static_cast<uint32_t>(address);

    // Decrypt 8 uint32 words (32 bytes total)
    for (int k = 0; k < 8; ++k) {
        uint32_t enc_word = static_cast<uint32_t>(encrypted[k*4]) |
                            (static_cast<uint32_t>(encrypted[k*4+1]) << 8) |
                            (static_cast<uint32_t>(encrypted[k*4+2]) << 16) |
                            (static_cast<uint32_t>(encrypted[k*4+3]) << 24);
        uint32_t dec_word = enc_word ^ mask;
        decrypted[k*4]     = static_cast<uint8_t>(dec_word & 0xFF);
        decrypted[k*4 + 1] = static_cast<uint8_t>((dec_word >> 8) & 0xFF);
        decrypted[k*4 + 2] = static_cast<uint8_t>((dec_word >> 16) & 0xFF);
        decrypted[k*4 + 3] = static_cast<uint8_t>((dec_word >> 24) & 0xFF);
    }
}

} // namespace cad
