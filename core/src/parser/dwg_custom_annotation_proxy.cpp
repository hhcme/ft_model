// Custom annotation detection and proxy creation — extracted from dwg_parser.cpp.
// Detects LEADER/DATUM/NOTE/FIELD/MTEXT custom objects and creates proxy entities.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_parse_objects_context.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_diagnostics.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/scene/scene_graph.h"
#include "cad/scene/entity.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace cad {

namespace diag = detail::diagnostics;

void DwgParser::process_custom_annotation_proxy(
    ParseObjectsContext& ctx,
    uint32_t obj_type, uint64_t handle,
    EntitySink& scene,
    const uint8_t* obj_data, size_t obj_data_size,
    size_t offset, size_t entity_data_bytes,
    size_t ms_bytes, size_t umc_bytes,
    size_t main_data_bits, size_t entity_bits,
    bool is_graphic, bool is_r2007_plus,
    const EntityHeader& entity_hdr,
    size_t reader_bit_offset)
{
        const bool is_r2010_plus = m_version >= DwgVersion::R2010;
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
            auto type_it = ctx.handle_object_types.find(ref_handle);
            if (type_it != ctx.handle_object_types.end()) {
                return type_it->second;
            }
            auto offset_it = m_sections.handle_map.find(ref_handle);
            PreparedObject ref_record;
            if (offset_it != m_sections.handle_map.end() &&
                prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                         offset_it->second, ref_record, false, false, nullptr)) {
                return ref_record.obj_type;
            }
            {
                const size_t primary_offset =
                    offset_it != m_sections.handle_map.end() ? offset_it->second : static_cast<size_t>(-1);
                if (try_recover_object(ref_handle, primary_offset, obj_data, obj_data_size,
                                       is_r2007_plus, is_r2010_plus, ref_record)) {
                    return ref_record.obj_type;
                }
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
            ctx.custom_fieldlist_objects++;
        } else if (field_class) {
            ctx.custom_field_objects++;
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            const std::vector<std::string> field_strings = diag::read_custom_t_strings(
                entity_data, entity_data_bytes, main_data_bits, reader_bit_offset, 16);
            ctx.custom_field_strings += field_strings.size();
            for (const std::string& value : field_strings) {
                if (ctx.custom_field_string_samples.size() >= 8) break;
                bool duplicate = false;
                for (const std::string& existing : ctx.custom_field_string_samples) {
                    if (existing == value) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    ctx.custom_field_string_samples.push_back(value);
                }
            }
        }
        if (mtext_context_class) {
            ctx.custom_mtext_context_objects++;
        }
        if (detail_style_class) {
            ctx.custom_detail_style_objects++;
        }
        if (detail_custom_class) {
            ctx.custom_detail_custom_objects++;
        }

        if (obj_type == 42 || obj_type == 43) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            const std::vector<std::string> dict_strings = diag::read_custom_t_strings(
                entity_data, entity_data_bytes, main_data_bits, reader_bit_offset, 12);
            if (!dict_strings.empty()) {
                ctx.dictionary_string_samples[handle] = dict_strings;
            }
            ctx.dictionary_handle_refs[handle] = scan_handle_stream_refs(48);
        }
        if (obj_type == 70 || obj_type == 79) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            const std::vector<std::string> record_strings = diag::read_custom_t_strings(
                entity_data, entity_data_bytes, main_data_bits, reader_bit_offset, 12);
            if (!record_strings.empty()) {
                ctx.xrecord_string_samples[handle] = record_strings;
            }
            ctx.xrecord_handle_refs[handle] = scan_handle_stream_refs(48);
        }

        if (annotation_like_custom || fieldlist_class || field_class ||
            mtext_context_class || detail_style_class || detail_custom_class) {
            for (uint64_t ref_handle : scan_handle_stream_refs(48)) {
                const uint32_t ref_type = object_type_for_handle(ref_handle);
                if (ref_type == 42 || ref_type == 43) {
                    ctx.custom_annotation_dictionary_refs.insert(ref_handle);
                } else if (ref_type == 70 || ref_type == 79) {
                    ctx.custom_annotation_xrecord_refs.insert(ref_handle);
                } else if (ref_type == 0) {
                    ctx.custom_annotation_unresolved_refs.insert(ref_handle);
                }
            }
        }

        const size_t custom_debug_limit =
            contains_ascii_ci(class_name_for_debug, "DATUMTARGET") ? 40u : 3u;
        if (dwg_debug_enabled() && annotation_like_custom &&
            ctx.custom_annotation_debug_samples[obj_type] < custom_debug_limit) {
            ctx.custom_annotation_debug_samples[obj_type]++;
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            std::vector<std::string> snippets =
                diag::extract_printable_strings(entity_data, entity_data_bytes, 4);
            std::vector<std::pair<double, double>> raw_points =
                diag::extract_plausible_raw_points(entity_data, entity_data_bytes, 20);
            dwg_debug_log("[DWG] custom_annotation_sample type=%u class='%s' handle=%llu "
                          "graphic=%u bytes=%zu main_bits=%zu handle_bits=%zu reader_bit=%zu strings=",
                          obj_type,
                          class_name_for_debug.c_str(),
                          static_cast<unsigned long long>(handle),
                          is_graphic ? 1u : 0u,
                          entity_data_bytes,
                          main_data_bits,
                          entity_bits > main_data_bits ? entity_bits - main_data_bits : 0,
                          reader_bit_offset);
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
                    diag::extract_printable_strings(entity_data, entity_data_bytes, 24, 1);
                dwg_debug_log("[DWG] datumtarget_strings handle=%llu values=",
                              static_cast<unsigned long long>(handle));
                for (size_t si = 0; si < short_strings.size(); ++si) {
                    dwg_debug_log("%s'%s'",
                                  si == 0 ? "" : "|",
                                  short_strings[si].c_str());
                }
                dwg_debug_log("\n");
                std::vector<diag::RawPointCandidate> point_offsets =
                    diag::extract_plausible_raw_points_with_offsets(entity_data, entity_data_bytes, 24);
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
                const auto ints = diag::extract_small_int_candidates(entity_data, entity_data_bytes, 80);
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
                    diag::extract_printable_strings(entity_data, entity_data_bytes, 24, 1);
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
                    const std::vector<std::string> t_values = diag::read_custom_t_strings(
                        entity_data, entity_data_bytes, main_data_bits, reader_bit_offset, 12);
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
                        if (ctx.line_resource_line_handles.insert(abs_handle).second) {
                            ctx.custom_line_resource_refs++;
                        }
                    } else if (target_type == 0) {
                        ctx.custom_line_resource_unresolved_refs++;
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
                diag::extract_printable_strings(entity_data, entity_data_bytes, 12);
            std::string note_text = diag::select_annotation_text_snippet(snippets);
            std::vector<std::pair<double, double>> raw_points =
                diag::extract_plausible_raw_points(entity_data, entity_data_bytes, 80);
            Vec3 anchor = Vec3::zero();
            std::vector<Vec3> leader_path = diag::select_annotation_leader_path(raw_points);
            if (!note_text.empty() && diag::select_annotation_anchor(raw_points, anchor)) {
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
                ctx.custom_annotation_text_fallbacks++;
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
                ctx.custom_note_leader_proxies++;
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
                diag::extract_plausible_raw_points(entity_data, entity_data_bytes, 96);
            std::vector<Vec3> leader_path = diag::select_annotation_leader_path(raw_points);
            if (leader_path.size() >= 2) {
                Vec3 callout_center = leader_path.front();
                Vec3 callout_target = leader_path[1];
                (void)diag::select_annotation_callout_proxy(
                    raw_points, leader_path, callout_center, callout_target);

                const float callout_radius = diag::datum_callout_radius(callout_center, callout_target);
                Vec3 leader_start = callout_center;
                const float dir_x = callout_target.x - callout_center.x;
                const float dir_y = callout_target.y - callout_center.y;
                const float dir_len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
                if (std::isfinite(dir_len) && dir_len > 1.0f) {
                    leader_start.x += dir_x / dir_len * callout_radius;
                    leader_start.y += dir_y / dir_len * callout_radius;
                }

                std::vector<Vec3> visual_leader_path;
                visual_leader_path.push_back(leader_start);
                visual_leader_path.push_back(callout_target);

                // If the leader is very short, the raw binary extraction likely
                // missed the geometry-end point.  Extrapolate the leader to a
                // minimum useful length so it reaches toward drawing geometry.
                {
                    const float vl_dx = callout_target.x - leader_start.x;
                    const float vl_dy = callout_target.y - leader_start.y;
                    const float vl_len = std::sqrt(vl_dx * vl_dx + vl_dy * vl_dy);
                    constexpr float kMinVisualLeaderLen = 400.0f;
                    if (std::isfinite(vl_len) && vl_len > 1.0f && vl_len < kMinVisualLeaderLen) {
                        const float extension = kMinVisualLeaderLen - vl_len;
                        const float nd_x = vl_dx / vl_len;
                        const float nd_y = vl_dy / vl_len;
                        Vec3 extended_target{
                            callout_target.x + nd_x * extension,
                            callout_target.y + nd_y * extension,
                            0.0f};
                        visual_leader_path.back() = extended_target;
                    }
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
                callout.radius = callout_radius;

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
                callout_label.text = std::to_string(ctx.custom_datum_target_proxies + 1);
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

                ctx.custom_datum_target_proxies++;
                ctx.custom_datum_target_label_proxies++;
            }
        }

        // WIPEOUT — render as background-filled rectangle (minimum fidelity)
        // WIPEOUT inherits from IMAGE; binary layout has insertion point,
        // u_vector, v_vector, and image size in the main entity data.
        const bool wipeout_class =
            contains_ascii_ci(class_name_for_debug, "WIPEOUT");
        if (wipeout_class) {
            const uint8_t* entity_data = obj_data + offset + ms_bytes + umc_bytes;
            std::vector<std::pair<double, double>> raw_points =
                diag::extract_plausible_raw_points(entity_data, entity_data_bytes, 96);

            // WIPEOUT geometry: first 3 plausible points are typically
            // insertion_point, u_vector_end, v_vector_end (or close to it).
            // We use the first few points to form a quadrilateral.
            if (raw_points.size() >= 3) {
                const auto& p0 = raw_points[0];  // insertion point
                const auto& p1 = raw_points[1];  // u-vector direction
                const auto& p2 = raw_points[2];  // v-vector direction

                // Compute the four corners: ins, ins+u, ins+u+v, ins+v
                float ix = static_cast<float>(p0.first);
                float iy = static_cast<float>(p0.second);
                float ux = static_cast<float>(p1.first - p0.first);
                float uy = static_cast<float>(p1.second - p0.second);
                float vx = static_cast<float>(p2.first - p0.first);
                float vy = static_cast<float>(p2.second - p0.second);

                float ulen = std::sqrt(ux * ux + uy * uy);
                float vlen = std::sqrt(vx * vx + vy * vy);
                if (std::isfinite(ulen) && std::isfinite(vlen) &&
                    ulen > 1e-3f && vlen > 1e-3f &&
                    ulen < 1e6f && vlen < 1e6f) {
                    SolidEntity wipeout;
                    wipeout.corners[0] = {ix, iy, 0.0f};
                    wipeout.corners[1] = {ix + ux, iy + uy, 0.0f};
                    wipeout.corners[2] = {ix + vx, iy + vy, 0.0f};
                    wipeout.corners[3] = {ix + ux + vx, iy + uy + vy, 0.0f};
                    wipeout.corner_count = 4;

                    EntityHeader wo_hdr = entity_hdr;
                    wo_hdr.type = EntityType::Solid;
                    wo_hdr.space = DrawingSpace::ModelSpace;
                    wo_hdr.color_override = 7;  // ACI white: wipeout mask

                    Bounds3d wo_bounds = Bounds3d::empty();
                    for (int ci = 0; ci < 4; ++ci) wo_bounds.expand(wipeout.corners[ci]);
                    wo_hdr.bounds = wo_bounds;

                    EntityVariant wo_entity;
                    wo_entity.header = wo_hdr;
                    wo_entity.data.emplace<16>(std::move(wipeout));
                    scene.add_entity(std::move(wo_entity));
                    ctx.custom_wipeout_proxies++;
                }
            }
        }

}

} // namespace cad
