#pragma once

#include "cad/cad_types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cad {

class DwgBitReader;

namespace detail {
namespace diagnostics {

// ============================================================
// Diagnostic data structs for raw binary section probing
// ============================================================

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

struct TextMarkerCandidate {
    size_t offset = 0;
    std::string text;
    const char* encoding = "ascii";
};

// ============================================================
// String / text extraction
// ============================================================

// Extract printable ASCII and UTF-16LE strings from raw binary data.
std::vector<std::string> extract_printable_strings(const uint8_t* data,
                                                   size_t size,
                                                   size_t limit,
                                                   size_t min_length = 4);

// Read up to `limit` text values (via DwgBitReader::read_t) starting at
// `start_bit` within the entity data stream. Used to probe custom object
// string payloads (FIELD, DICTIONARY, XRECORD, etc.).
std::vector<std::string> read_custom_t_strings(
    const uint8_t* entity_data,
    size_t entity_data_bytes,
    size_t main_data_bits,
    size_t start_bit,
    size_t limit);

// ============================================================
// Point / coordinate extraction
// ============================================================

// Scan raw bytes for plausible double-pair coordinates.
std::vector<std::pair<double, double>> extract_plausible_raw_points(const uint8_t* data,
                                                                    size_t size,
                                                                    size_t limit);

// Like extract_plausible_raw_points, but also records the byte offset.
std::vector<RawPointCandidate> extract_plausible_raw_points_with_offsets(
    const uint8_t* data,
    size_t size,
    size_t limit);

// ============================================================
// Text marker search
// ============================================================

// Search raw binary data for ASCII and UTF-16LE occurrences of needle strings.
std::vector<TextMarkerCandidate> find_text_markers(const uint8_t* data,
                                                   size_t size,
                                                   const std::vector<std::string>& needles,
                                                   size_t limit);

// Format a sample of text markers for diagnostic output.
std::string format_text_marker_sample(const std::vector<TextMarkerCandidate>& markers,
                                      size_t limit);

// ============================================================
// Small integer extraction
// ============================================================

std::vector<RawSmallIntCandidate> extract_small_int_candidates(const uint8_t* data,
                                                               size_t size,
                                                               size_t limit);

// Format a sample of byte offsets for diagnostic output.
std::string format_offset_sample(const std::vector<size_t>& offsets, size_t limit);

// ============================================================
// Auxiliary section marker selection
// ============================================================

// Select priority-ordered markers from extracted strings (datidx, segidx, etc.).
std::vector<std::string> select_auxiliary_section_markers(
    const std::vector<std::string>& strings,
    size_t limit);

// ============================================================
// Annotation analysis helpers
// ============================================================

// Pick the most "prose-like" text snippet from candidates.
std::string select_annotation_text_snippet(const std::vector<std::string>& snippets);

// Find the first plausible world-space anchor point from raw double pairs.
bool select_annotation_anchor(const std::vector<std::pair<double, double>>& points,
                              Vec3& anchor);

// Check whether a coordinate pair looks like a valid annotation world point.
bool is_annotation_world_point(double x, double y);

// Compute the median nearest-neighbor distance among points.
float median_nearest_neighbor_distance(const std::vector<Vec3>& pts);

// De-duplicate raw points into unique Vec3 world points (up to `limit`).
std::vector<Vec3> unique_annotation_world_points(
    const std::vector<std::pair<double, double>>& points,
    size_t limit);

// Attempt to extract a leader path (ordered segment of nearby points).
std::vector<Vec3> select_annotation_leader_path(
    const std::vector<std::pair<double, double>>& points);

// Find a callout cluster near the leader path, returning center + target.
bool select_annotation_callout_proxy(
    const std::vector<std::pair<double, double>>& points,
    const std::vector<Vec3>& leader_path,
    Vec3& callout_center,
    Vec3& callout_target);

// Compute a reasonable callout circle radius from center-to-target distance.
float datum_callout_radius(const Vec3& callout_point, const Vec3& target_point);

} // namespace diagnostics
} // namespace detail
} // namespace cad
