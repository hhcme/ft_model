#include "cad/parser/dwg_diagnostics.h"
#include "cad/parser/dwg_reader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace cad {
namespace detail {
namespace diagnostics {

namespace {

std::string uppercase_ascii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

} // anonymous namespace

// ============================================================
// String / text extraction
// ============================================================

std::vector<std::string> extract_printable_strings(const uint8_t* data,
                                                   size_t size,
                                                   size_t limit,
                                                   size_t min_length)
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

// ============================================================
// Point / coordinate extraction
// ============================================================

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

// ============================================================
// Text marker search
// ============================================================

std::vector<TextMarkerCandidate> find_text_markers(const uint8_t* data,
                                                   size_t size,
                                                   const std::vector<std::string>& needles,
                                                   size_t limit)
{
    std::vector<TextMarkerCandidate> out;
    auto push_unique = [&](size_t offset, const std::string& text, const char* encoding) {
        if (out.size() >= limit) return;
        for (const auto& existing : out) {
            if (existing.offset == offset && existing.text == text) {
                return;
            }
        }
        out.push_back({offset, text, encoding});
    };

    for (const std::string& needle : needles) {
        if (needle.empty()) continue;

        for (size_t i = 0; i + needle.size() <= size && out.size() < limit; ++i) {
            if (std::memcmp(data + i, needle.data(), needle.size()) == 0) {
                push_unique(i, needle, "ascii");
            }
        }

        std::vector<uint8_t> utf16;
        utf16.reserve(needle.size() * 2);
        for (char c : needle) {
            utf16.push_back(static_cast<uint8_t>(c));
            utf16.push_back(0);
        }
        for (size_t i = 0; i + utf16.size() <= size && out.size() < limit; ++i) {
            if (std::memcmp(data + i, utf16.data(), utf16.size()) == 0) {
                push_unique(i, needle, "utf16le");
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.offset < b.offset;
    });
    return out;
}

std::string format_text_marker_sample(const std::vector<TextMarkerCandidate>& markers,
                                      size_t limit)
{
    std::string out;
    const size_t count = std::min(markers.size(), limit);
    for (size_t i = 0; i < count; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%s@0x%zx/%s",
                      i == 0 ? "" : ",",
                      markers[i].text.c_str(),
                      markers[i].offset,
                      markers[i].encoding);
        out += buf;
    }
    return out;
}

// ============================================================
// Small integer extraction
// ============================================================

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

std::string format_offset_sample(const std::vector<size_t>& offsets, size_t limit)
{
    std::string out;
    const size_t count = std::min(offsets.size(), limit);
    for (size_t i = 0; i < count; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s0x%zx", i == 0 ? "" : ",", offsets[i]);
        out += buf;
    }
    return out;
}

// ============================================================
// Auxiliary section marker selection
// ============================================================

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

// ============================================================
// Annotation analysis helpers
// ============================================================

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

float median_nearest_neighbor_distance(const std::vector<Vec3>& pts)
{
    if (pts.size() < 2) return 50.0f;
    std::vector<float> nn_dists;
    nn_dists.reserve(pts.size());
    for (const Vec3& p : pts) {
        float best = 1.0e9f;
        for (const Vec3& q : pts) {
            if (&p == &q) continue;
            const float dx = p.x - q.x;
            const float dy = p.y - q.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < best) best = d;
        }
        if (std::isfinite(best) && best < 1.0e8f) nn_dists.push_back(best);
    }
    if (nn_dists.empty()) return 50.0f;
    const size_t mid = nn_dists.size() / 2;
    std::nth_element(nn_dists.begin(), nn_dists.begin() + static_cast<std::ptrdiff_t>(mid), nn_dists.end());
    return nn_dists[mid];
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

    const float med_nn = median_nearest_neighbor_distance(candidates);
    const float max_segment = std::clamp(med_nn * 50.0f, 2000.0f, 50000.0f);
    const float min_ref = std::clamp(med_nn * 0.5f, 5.0f, 200.0f);
    const float max_step_mult = 2.5f;
    const float min_step_fallback = std::clamp(med_nn * 5.0f, 50.0f, 1000.0f);

    path.push_back(candidates[0]);
    float reference_len = 0.0f;
    for (size_t i = 1; i < candidates.size(); ++i) {
        const Vec3& prev = path.back();
        const Vec3& next = candidates[i];
        const float dx = next.x - prev.x;
        const float dy = next.y - prev.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(len) || len < 1.0f) continue;
        if (len > max_segment) break;

        if (reference_len <= 0.0f && len >= min_ref) {
            reference_len = len;
        }
        const float max_step = std::max(reference_len * max_step_mult, min_step_fallback);
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

    const float med_nn = median_nearest_neighbor_distance(candidates);
    const float cluster_radius = std::clamp(med_nn * 2.5f, 15.0f, 200.0f);
    const float leader_min_dist = std::clamp(med_nn * 3.0f, 30.0f, 300.0f);

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
            if (!std::isfinite(dist) || dist > cluster_radius) continue;
            near_points.push_back(other);
            max_near_dist = std::max(max_near_dist, dist);
        }

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
            if (std::isfinite(dist) && dist > leader_min_dist) {
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

    float best_target_dist = 1.0e9f;
    callout_target = leader_path.size() > 1 ? leader_path[1] : leader_path.front();
    for (const Vec3& p : leader_path) {
        const float dx = p.x - callout_center.x;
        const float dy = p.y - callout_center.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (std::isfinite(dist) && dist > leader_min_dist && dist < best_target_dist) {
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
    return std::clamp(len * 0.08f, 12.0f, 60.0f);
}

} // namespace diagnostics
} // namespace detail
} // namespace cad
