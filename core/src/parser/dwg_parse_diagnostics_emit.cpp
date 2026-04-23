// Diagnostic record emission — extracted from dwg_parser.cpp parse_objects().
// Emits diagnostic records and debug logging after the main loop finishes.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_parse_objects_context.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_diagnostics.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace cad {

namespace diag = detail::diagnostics;

void DwgParser::emit_parse_diagnostics(
    ParseObjectsContext& ctx,
    EntitySink& scene,
    const uint8_t* obj_data, size_t obj_data_size,
    bool is_r2010_plus)
{
    dwg_debug_log("[DWG] parse_objects: %zu graphic, %zu non-graphic, %zu errors\n",
            ctx.graphic_count, ctx.non_graphic_count, ctx.error_count);
    if (ctx.viewport_frozen_layer_refs > 0) {
        scene.add_diagnostic({
            "dwg_viewport_frozen_layers_decoded",
            "Semantic gap",
            "DWG VIEWPORT handle streams reference per-viewport frozen layer handles. They are stored on Viewport metadata, but the renderer does not yet apply layout-viewport layer overrides while viewport projection remains diagnostic-only.",
            static_cast<int32_t>(std::min<size_t>(ctx.viewport_frozen_layer_refs, 2147483647u)),
            version_family_name(m_version),
            "VIEWPORT/LAYER",
            34,
        });
        dwg_debug_log("[DWG] viewport frozen layers decoded: refs=%zu viewports=%zu\n",
                      ctx.viewport_frozen_layer_refs,
                      ctx.viewport_frozen_layer_viewports);
    }
    size_t line_resource_entities_found = 0;
    size_t line_resource_entities_in_block = 0;
    if (!ctx.line_resource_line_handles.empty()) {
        std::unordered_map<uint64_t, size_t> entity_index_by_handle;
        for (const auto& [eidx, entity_handle] : ctx.entity_object_handles) {
            entity_index_by_handle[entity_handle] = eidx;
        }
        const auto& all_entities = scene.entities();
        for (uint64_t line_handle : ctx.line_resource_line_handles) {
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
    if (!ctx.line_resource_line_handles.empty()) {
        dwg_debug_log("[DWG] line resources: refs=%zu entities=%zu in_block=%zu\n",
                      ctx.line_resource_line_handles.size(),
                      line_resource_entities_found,
                      line_resource_entities_in_block);
    }
    if (ctx.recovered_object_offsets > 0 || ctx.unrecovered_object_offsets > 0) {
        dwg_debug_log("[DWG] object-map recovery: recovered=%zu unrecovered=%zu scans=%zu\n",
                      ctx.recovered_object_offsets,
                      ctx.unrecovered_object_offsets,
                      m_object_recovery_scans);
    }
    if (ctx.primary_self_handle_mismatches > 0) {
        dwg_debug_log("[DWG] object-map self mismatches: %zu\n",
                      ctx.primary_self_handle_mismatches);
    }
    if (ctx.r2007_primary_self_recovered > 0) {
        dwg_debug_log("[DWG] R2007 primary self-handle recoveries: %zu\n",
                      ctx.r2007_primary_self_recovered);
    }
    if (ctx.invalid_handle_stream_framing > 0) {
        dwg_debug_log("[DWG] invalid handle stream framing: %zu\n",
                      ctx.invalid_handle_stream_framing);
        scene.add_diagnostic({
            "dwg_handle_stream_framing_invalid",
            "Object framing gap",
            "One or more DWG objects declared a handle stream outside the prepared object boundary; those objects were parsed with a conservative no-handle-stream fallback.",
            static_cast<int32_t>(std::min<size_t>(ctx.invalid_handle_stream_framing, 2147483647u)),
            version_family_name(m_version),
            "PreparedObject",
        });
    }
    if (dwg_debug_enabled() && ctx.prepared_string_streams > 0) {
        dwg_debug_log("[DWG] prepared string streams: %zu\n", ctx.prepared_string_streams);
    }
    auto object_type_for_summary = [&](uint64_t h) -> uint32_t {
        auto it = ctx.handle_object_types.find(h);
        if (it != ctx.handle_object_types.end()) {
            return it->second;
        }
        auto offset_it = m_sections.handle_map.find(h);
        if (offset_it == m_sections.handle_map.end()) {
            return 0;
        }
        PreparedObject record;
        if (!prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                     offset_it->second, record, false, false, nullptr)) {
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
            ctx.custom_annotation_dictionary_refs.begin(),
            ctx.custom_annotation_dictionary_refs.end());
        for (size_t qi = 0; qi < pending_dictionaries.size() && qi < 128; ++qi) {
            const uint64_t dict_handle = pending_dictionaries[qi];
            auto refs_it = ctx.dictionary_handle_refs.find(dict_handle);
            if (refs_it == ctx.dictionary_handle_refs.end()) {
                continue;
            }
            for (uint64_t ref : refs_it->second) {
                const uint32_t ref_type = object_type_for_summary(ref);
                if (ref_type == 42 || ref_type == 43) {
                    if (ctx.custom_annotation_dictionary_refs.insert(ref).second) {
                        pending_dictionaries.push_back(ref);
                    }
                } else if (ref_type == 70 || ref_type == 79) {
                    ctx.custom_annotation_xrecord_refs.insert(ref);
                }
            }
        }
    }
    if (dwg_debug_enabled() && !ctx.custom_annotation_dictionary_refs.empty()) {
        std::vector<uint64_t> dictionary_refs(
            ctx.custom_annotation_dictionary_refs.begin(),
            ctx.custom_annotation_dictionary_refs.end());
        std::sort(dictionary_refs.begin(), dictionary_refs.end());
        const size_t dictionary_limit = std::min<size_t>(dictionary_refs.size(), 24);
        for (size_t di = 0; di < dictionary_limit; ++di) {
            const uint64_t dict_handle = dictionary_refs[di];
            dwg_debug_log("[DWG] custom_dictionary handle=%llu strings=",
                          static_cast<unsigned long long>(dict_handle));
            auto strings_it = ctx.dictionary_string_samples.find(dict_handle);
            if (strings_it != ctx.dictionary_string_samples.end()) {
                const size_t string_limit = std::min<size_t>(strings_it->second.size(), 8);
                for (size_t si = 0; si < string_limit; ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  strings_it->second[si].c_str());
                }
            }
            dwg_debug_log(" refs=");
            auto refs_it = ctx.dictionary_handle_refs.find(dict_handle);
            if (refs_it != ctx.dictionary_handle_refs.end()) {
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
    if (dwg_debug_enabled() && !ctx.custom_annotation_xrecord_refs.empty()) {
        std::vector<uint64_t> xrecord_refs(
            ctx.custom_annotation_xrecord_refs.begin(),
            ctx.custom_annotation_xrecord_refs.end());
        std::sort(xrecord_refs.begin(), xrecord_refs.end());
        const size_t xrecord_limit = std::min<size_t>(xrecord_refs.size(), 24);
        for (size_t xi = 0; xi < xrecord_limit; ++xi) {
            const uint64_t record_handle = xrecord_refs[xi];
            dwg_debug_log("[DWG] custom_xrecord handle=%llu strings=",
                          static_cast<unsigned long long>(record_handle));
            auto strings_it = ctx.xrecord_string_samples.find(record_handle);
            if (strings_it != ctx.xrecord_string_samples.end()) {
                const size_t string_limit = std::min<size_t>(strings_it->second.size(), 8);
                for (size_t si = 0; si < string_limit; ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  strings_it->second[si].c_str());
                }
            }
            dwg_debug_log(" refs=");
            auto refs_it = ctx.xrecord_handle_refs.find(record_handle);
            if (refs_it != ctx.xrecord_handle_refs.end()) {
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
    if (ctx.recovered_object_offsets > 0) {
        scene.add_diagnostic({
            "dwg_object_map_offset_recovered",
            "Parse gap",
            "One or more DWG Object Map offsets were invalid and recovered by matching the object's own handle.",
            static_cast<int32_t>(std::min<size_t>(ctx.recovered_object_offsets, 2147483647u)),
        });
    }
    if (ctx.unrecovered_object_offsets > 0) {
        scene.add_diagnostic({
            "dwg_object_map_offset_invalid",
            "Parse gap",
            "One or more DWG Object Map offsets were invalid and could not be recovered without ambiguity.",
            static_cast<int32_t>(std::min<size_t>(ctx.unrecovered_object_offsets, 2147483647u)),
        });
    }
    if (ctx.custom_annotation_text_fallbacks > 0) {
        scene.add_diagnostic({
            "custom_annotation_text_fallback",
            "Render gap",
            "Text was recovered from custom DWG annotation objects using a conservative proxy-text fallback; native object geometry is still pending.",
            static_cast<int32_t>(std::min<size_t>(ctx.custom_annotation_text_fallbacks, 2147483647u)),
        });
    }
    if (ctx.custom_note_leader_proxies > 0) {
        scene.add_diagnostic({
            "custom_note_leader_proxy",
            "Render gap",
            "Leader geometry was recovered from custom DWG note objects using conservative proxy linework.",
            static_cast<int32_t>(std::min<size_t>(ctx.custom_note_leader_proxies, 2147483647u)),
        });
    }
    if (ctx.custom_datum_target_proxies > 0) {
        scene.add_diagnostic({
            "custom_datum_target_proxy",
            "Render gap",
            "Datum/callout leader geometry was recovered from custom DWG annotation objects using conservative proxy lines and bubbles.",
            static_cast<int32_t>(std::min<size_t>(ctx.custom_datum_target_proxies, 2147483647u)),
        });
        scene.add_diagnostic({
            "custom_datum_target_label_proxy",
            "Semantic gap",
            "Datum/callout bubble labels were emitted as deterministic ordinal proxies because no stable native label field has been identified in the DWG custom object payload yet.",
            static_cast<int32_t>(std::min<size_t>(ctx.custom_datum_target_label_proxies, 2147483647u)),
        });
    }
    if (ctx.custom_line_resource_refs > 0) {
        scene.add_diagnostic({
            "ctx.custom_line_resource_refs",
            "Semantic gap",
            "AutoCAD Mechanical line resource objects referenced ordinary LINE entities; referenced geometry was tracked to avoid treating the resource object as standalone geometry.",
            static_cast<int32_t>(std::min<size_t>(ctx.custom_line_resource_refs, 2147483647u)),
        });
    }
    if (ctx.custom_line_resource_unresolved_refs > 0) {
        scene.add_diagnostic({
            "ctx.custom_line_resource_unresolved_refs",
            "Parse gap",
            "AutoCAD Mechanical line resource objects referenced handles that are not resolved to known DWG objects yet.",
            static_cast<int32_t>(std::min<size_t>(ctx.custom_line_resource_unresolved_refs, 2147483647u)),
        });
    }
    if (ctx.custom_field_objects > 0) {
        std::string message = "DWG FIELD objects were decoded through the R2007+ string stream";
        if (!ctx.custom_field_string_samples.empty()) {
            message += "; samples: ";
            for (size_t i = 0; i < ctx.custom_field_string_samples.size(); ++i) {
                if (i > 0) message += ", ";
                std::string sample = ctx.custom_field_string_samples[i];
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
            static_cast<int32_t>(std::min<size_t>(ctx.custom_field_objects, 2147483647u)),
        });
    }
    if (ctx.custom_fieldlist_objects > 0 || ctx.custom_mtext_context_objects > 0) {
        std::string message = "DWG FIELDLIST/MTEXT context objects were identified in the custom annotation handle graph.";
        scene.add_diagnostic({
            "custom_field_context_graph_identified",
            "Handle resolution gap",
            message,
            static_cast<int32_t>(std::min<size_t>(
                ctx.custom_fieldlist_objects + ctx.custom_mtext_context_objects,
                2147483647u)),
        });
    }
    if (!ctx.custom_annotation_dictionary_refs.empty() ||
        !ctx.custom_annotation_xrecord_refs.empty()) {
        size_t dictionaries_with_strings = 0;
        std::vector<std::string> dictionary_samples;
        for (uint64_t dict_handle : ctx.custom_annotation_dictionary_refs) {
            auto sample_it = ctx.dictionary_string_samples.find(dict_handle);
            if (sample_it != ctx.dictionary_string_samples.end() &&
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
        message += std::to_string(ctx.custom_annotation_dictionary_refs.size());
        message += ", xrecords=";
        message += std::to_string(ctx.custom_annotation_xrecord_refs.size());
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
        if (!ctx.custom_annotation_xrecord_refs.empty()) {
            size_t xrecords_with_strings = 0;
            for (uint64_t xrecord_handle : ctx.custom_annotation_xrecord_refs) {
                auto sample_it = ctx.xrecord_string_samples.find(xrecord_handle);
                if (sample_it != ctx.xrecord_string_samples.end() &&
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
                ctx.custom_annotation_dictionary_refs.size() + ctx.custom_annotation_xrecord_refs.size(),
                2147483647u)),
        });
    }
    if (!ctx.custom_annotation_unresolved_refs.empty()) {
        scene.add_diagnostic({
            "custom_annotation_graph_unresolved_refs",
            "Handle resolution gap",
            "Custom DWG annotation/detail objects reference handles that do not resolve to known DWG objects yet.",
            static_cast<int32_t>(std::min<size_t>(
                ctx.custom_annotation_unresolved_refs.size(),
                2147483647u)),
        });
    }
    if (ctx.custom_detail_style_objects > 0 || ctx.custom_detail_custom_objects > 0) {
        scene.add_diagnostic({
            "mechanical_detail_view_graph_identified",
            "Custom object semantic gap",
            "AutoCAD Mechanical detail-view style/custom objects were identified; native crop/frame parameters still need semantic decoding, so visual detail frames may use fallback proxies.",
            static_cast<int32_t>(std::min<size_t>(
                ctx.custom_detail_style_objects + ctx.custom_detail_custom_objects,
                2147483647u)),
        });
    }
    if (dwg_debug_enabled()) {
        std::vector<std::pair<uint32_t, size_t>> type_counts(
            ctx.object_type_counts.begin(), ctx.object_type_counts.end());
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
        for (const auto& [type, count] : ctx.object_type_counts) {
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
                ctx.custom_datum_target_proxies > 0 &&
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
                const size_t recovered = std::min(count, ctx.custom_annotation_text_fallbacks);
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

}

} // namespace cad
