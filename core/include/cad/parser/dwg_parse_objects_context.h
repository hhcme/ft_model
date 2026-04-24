#pragma once

// Shared context for the parse_objects() main loop, post-processing,
// and diagnostic emission phases.  Bundles the ~50 local variables
// that were previously declared inline in parse_objects().

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cad {

struct ParseObjectsContext {
    // ---- Counters (loop body) ----
    size_t graphic_count = 0;
    size_t non_graphic_count = 0;
    size_t error_count = 0;
    size_t g_layer_resolved = 0;
    std::unordered_map<uint32_t, size_t> object_type_counts;

    size_t processed = 0;
    std::chrono::steady_clock::time_point t_start;

    // ---- Maps (loop body) ----
    std::unordered_map<uint64_t, std::string> block_names_from_entities;
    std::unordered_map<uint64_t, int32_t> block_handle_to_index;
    std::unordered_map<size_t, std::vector<uint64_t>> insert_handles;
    std::unordered_map<size_t, std::vector<uint64_t>> insert_handle_fallback_candidates;
    std::unordered_map<size_t, std::vector<uint64_t>> entity_common_handles;
    std::unordered_map<size_t, uint64_t> entity_owner_handles;
    std::unordered_map<size_t, uint64_t> entity_object_handles;
    std::unordered_map<uint64_t, uint64_t> entity_handle_to_block_header;
    std::unordered_map<uint64_t, uint32_t> handle_object_types;
    std::unordered_map<int32_t, std::vector<uint64_t>> layout_handle_refs;
    std::unordered_map<uint32_t, size_t> custom_annotation_debug_samples;
    std::unordered_map<uint64_t, std::vector<std::string>> dictionary_string_samples;
    std::unordered_map<uint64_t, std::vector<uint64_t>> dictionary_handle_refs;
    std::unordered_map<uint64_t, std::vector<std::string>> xrecord_string_samples;
    std::unordered_map<uint64_t, std::vector<uint64_t>> xrecord_handle_refs;

    // ---- Sets (loop body) ----
    std::unordered_set<uint64_t> line_resource_line_handles;
    std::unordered_set<uint64_t> custom_annotation_dictionary_refs;
    std::unordered_set<uint64_t> custom_annotation_xrecord_refs;
    std::unordered_set<uint64_t> custom_annotation_unresolved_refs;

    // ---- Size_t counters ----
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
    size_t recovered_object_offsets = 0;
    size_t unrecovered_object_offsets = 0;
    size_t primary_self_handle_mismatches = 0;
    size_t insert_handle_role_fallbacks = 0;
    size_t invalid_handle_stream_framing = 0;
    size_t prepared_string_streams = 0;
    size_t r2007_primary_self_recovered = 0;
    size_t viewport_frozen_layer_refs = 0;
    size_t viewport_frozen_layer_viewports = 0;

    // ---- Vectors ----
    std::vector<std::string> custom_field_string_samples;
};

} // namespace cad
