#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_objects.h"
#include "cad/parser/dwg_r2007_codec.h"
#include "cad/parser/dwg_diagnostics.h"
#include "cad/parser/dwg_header_vars.h"
#include "cad/parser/dwg_parse_helpers.h"
#include "cad/parser/dwg_parse_objects_context.h"
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

// Import diagnostic functions into local scope so existing call sites
// continue to work without qualification.
namespace diag = detail::diagnostics;

// Import R2007 codec types and functions from the extracted module.
namespace r2007 = detail::r2007;

// ============================================================
// Shared debug logging (declared in dwg_parser.h)
// ============================================================

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

const char* version_family_name(DwgVersion version)
{
    switch (version) {
        case DwgVersion::R2000: return "R2000/AC1015";
        case DwgVersion::R2004: return "R2004/AC1018";
        case DwgVersion::R2007: return "R2007/AC1021";
        case DwgVersion::R2010: return "R2010/AC1024";
        case DwgVersion::R2013: return "R2013/AC1027";
        case DwgVersion::R2018: return "R2018+/AC1032";
        default: return "Unknown";
    }
}

namespace {

// dwg_debug_enabled() is inline in dwg_parser.h
// dwg_debug_log() and version_family_name() are declared in dwg_parser.h, defined below

// uppercase_ascii() and contains_ascii_ci() are in dwg_parse_helpers.h/cpp.

uint64_t read_u64_le(const uint8_t* data)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

int64_t read_i64_le(const uint8_t* data)
{
    return static_cast<int64_t>(read_u64_le(data));
}

// R2007 codec structs and functions moved to cad::detail::r2007 namespace
// (see dwg_r2007_codec.h / dwg_r2007_codec.cpp)

// Diagnostic helper types and functions are now in
// cad::detail::diagnostics (dwg_diagnostics.h/cpp), accessed via
// the `diag` namespace alias at the top of the cad namespace.

// SectionStringReader and parse_header_variables/parse_classes are in
// dwg_header_vars.h/cpp.

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
    scene.drawing_info().acad_version = version_family_name(m_version);

    if (m_version == DwgVersion::R2000) {
        r = read_r2000_sections(data, size, scene);
        if (!r) return r;

        r = parse_header_variables(scene);
        if (!r) return r;

        r = parse_classes(scene);
        if (!r) return r;

        r = parse_objects(scene);
        if (!r) return r;

        if (scene.total_entity_count() > 0 && scene.layers().empty()) {
            Layer layer;
            layer.name = "0";
            scene.add_layer(std::move(layer));
            scene.add_diagnostic({
                "dwg_default_layer_synthesized",
                "Semantic gap",
                "DWG entities were decoded but no LAYER table entries were recovered; synthesized default layer 0 for stable rendering and layer metadata.",
                1,
                version_family_name(m_version),
                "LAYER",
            });
        }

        return Result::success();
    }

    if (m_version == DwgVersion::R2007) {
        r = read_r2007_container(data, size, scene);
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

        if (scene.total_entity_count() > 0 && scene.layers().empty()) {
            Layer layer;
            layer.name = "0";
            scene.add_layer(std::move(layer));
            scene.add_diagnostic({
                "dwg_default_layer_synthesized",
                "Semantic gap",
                "DWG entities were decoded but no LAYER table entries were recovered; synthesized default layer 0 for stable rendering and layer metadata.",
                1,
                version_family_name(m_version),
                "LAYER",
            });
        }

        return Result::success();
    }

    r = decrypt_r2004_header(data, size);
    if (!r) return r;

    const bool plausible_r2004_style_header =
        m_file_header.header_size == 108 &&
        m_file_header.section_map_address > 0 &&
        m_file_header.section_map_address + 0x100u + 20u <= size &&
        m_file_header.numsections < 100000u;
    if (!plausible_r2004_style_header) {
        scene.add_diagnostic({
            "dwg_container_header_invalid",
            "Object framing gap",
            "DWG section-page header metadata failed boundary validation before section decoding.",
            1,
            version_family_name(m_version),
            "File container",
        });
        return Result::success();
    }

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

    if (scene.total_entity_count() > 0 && scene.layers().empty()) {
        Layer layer;
        layer.name = "0";
        scene.add_layer(std::move(layer));
        scene.add_diagnostic({
            "dwg_default_layer_synthesized",
            "Semantic gap",
            "DWG entities were decoded but no LAYER table entries were recovered; synthesized default layer 0 for stable rendering and layer metadata.",
            1,
            version_family_name(m_version),
            "LAYER",
        });
    }

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

// read_r2007_container is now in dwg_r2007_codec.cpp

// parse_object_map is now in dwg_object_map.cpp
// parse_objects helpers (is_graphic_entity, parse_layout_object, etc.) are now in dwg_parse_helpers.cpp

// Object preparation/recovery helpers (is_known_object_type,
// prepare_object_at_offset, recover_from_candidates, try_recover_object)
// are now in dwg_object_recovery.cpp.

Result DwgParser::parse_objects(EntitySink& scene)
{
    if (m_sections.object_data.empty()) {
        return Result::error(ErrorCode::ParseError, "No object data");
    }
    reset_dwg_entity_parser_state();

    const uint8_t* obj_data = m_sections.object_data.data();
    size_t obj_data_size = m_sections.object_data.size();
    bool is_r2007_plus = (m_version >= DwgVersion::R2007);
    bool is_r2010_plus = (m_version >= DwgVersion::R2010);

    ParseObjectsContext ctx;
    ctx.t_start = std::chrono::steady_clock::now();
    m_object_recovery_scans = 0;

    // Pre-scan LTYPE/LAYER/BLOCK_HEADER table objects before the main loop.
    prescan_table_objects(obj_data, obj_data_size, is_r2007_plus, is_r2010_plus,
                          scene, ctx.block_names_from_entities);
    // Iterate in handle-sorted order. BLOCK entities have lower handles
    // than their corresponding ENDBLK, and entities within a block
    // definition have handles between the BLOCK and ENDBLK handles.
    // This ensures BLOCK/ENDBLK markers correctly bracket their entities.
    std::vector<std::pair<uint64_t, size_t>> sorted_handles(
        m_sections.handle_map.begin(), m_sections.handle_map.end());
    std::sort(sorted_handles.begin(), sorted_handles.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });
    for (const auto& sorted_entry : sorted_handles) {
        uint64_t handle = sorted_entry.first;
        size_t offset = sorted_entry.second;
        ctx.processed++;
        if ((ctx.processed % 10000) == 0) {
            dwg_debug_log("[DWG] parse_objects progress: %zu / %zu\n",
                    ctx.processed, m_sections.handle_map.size());
        }
        PreparedObject record;
        const char* prepare_failure = "";
        if (!prepare_object_at_offset(obj_data, obj_data_size, is_r2010_plus,
                                     offset, record, false, false, &prepare_failure)) {
            PreparedObject recovered;
            if (!try_recover_object(handle, offset, obj_data, obj_data_size,
                                    is_r2007_plus, is_r2010_plus, recovered)) {
                if (dwg_debug_enabled() && ctx.unrecovered_object_offsets < 20) {
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
                            PreparedObject cand_record;
                            const bool cand_ok = prepare_object_at_offset(
                                obj_data, obj_data_size, is_r2010_plus,
                                cand_offset, cand_record, false, false, &cand_reason);
                            const uint64_t self_abs = cand_ok
                                ? resolve_handle_ref(handle, cand_record.self_handle)
                                : 0;
                        }
                    }
                }
                ctx.error_count++;
                ctx.unrecovered_object_offsets++;
                continue;
            }
            record = recovered;
            offset = record.offset;
            ctx.recovered_object_offsets++;
        }

        const size_t ms_bytes = record.ms_bytes;
        const size_t umc_bytes = record.umc_bytes;
        const size_t entity_data_bytes = record.entity_data_bytes;
        const size_t entity_bits = record.entity_bits;
        const size_t main_data_bits = record.main_data_bits;
        const uint32_t obj_type = record.obj_type;

        if (!record.handle_stream_valid) {
            ctx.invalid_handle_stream_framing++;
        }
        if (record.has_string_stream) {
            ctx.prepared_string_streams++;
        }
        const uint64_t primary_self_handle = resolve_handle_ref(handle, record.self_handle);
        if (primary_self_handle != handle) {
            PreparedObject recovered_by_self;
            if (m_version == DwgVersion::R2007 &&
                recover_from_candidates(handle, offset, obj_data, obj_data_size,
                                        is_r2010_plus, recovered_by_self)) {
                record = recovered_by_self;
                offset = record.offset;
                ctx.r2007_primary_self_recovered++;
                ctx.recovered_object_offsets++;
            } else {
                ctx.primary_self_handle_mismatches++;
                continue;
            }
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
            if (m_version >= DwgVersion::R2004) {
                (void)reader.read_rl();  // bitsize: end of main data / start of handle stream
            }
        }
        if (reader.has_error()) {
            ctx.error_count++;
            continue;
        }
        ctx.object_type_counts[obj_type]++;
        ctx.handle_object_types[handle] = obj_type;

        // ---- BLOCK / ENDBLK tracking (before CED to avoid bit exhaustion) ----
        if (process_block_endblk(ctx, obj_type, handle, scene,
                                  obj_data + offset, entity_data_bytes,
                                  ms_bytes, umc_bytes,
                                  main_data_bits, entity_bits)) {
            ctx.non_graphic_count++;
            continue;
        }

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
            ctx.error_count++;
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
                        ctx.error_count++;
                        continue;
                    }
                }
            }
        }

        if (reader.has_error()) {
            ctx.error_count++;
            continue;
        }


        // ---- Entity CED or Object Common Header ----

        EntityHeader entity_hdr;
        entity_hdr.entity_id = static_cast<int64_t>(handle);
        entity_hdr.dwg_handle = handle;

        uint16_t color_raw = 0;
        uint8_t color_flag = 0;
        uint32_t saved_num_reactors = 0;
        bool saved_is_xdic_missing = false;
        if (is_graphic) {
            // Graphic entity CED (Common Entity Data)
            // entity_mode (BB) is present for R2007+ per libredwg common_entity_data.spec.
            // R2004 does NOT have this field.
            if (m_version >= DwgVersion::R2007) {
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
            ctx.error_count++;
            continue;
        }

        // ---- Custom annotation detection and proxy creation ----
        {
            auto class_it_for_debug = m_sections.class_map.find(obj_type);
            const std::string class_name_for_debug =
                class_it_for_debug != m_sections.class_map.end()
                    ? class_it_for_debug->second.first
                    : std::string{};
            process_custom_annotation_proxy(
                ctx, obj_type, handle, scene,
                obj_data, obj_data_size, offset, entity_data_bytes,
                ms_bytes, umc_bytes, main_data_bits, entity_bits,
                is_graphic, is_r2007_plus, entity_hdr,
                reader.bit_offset());
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
            // R2004 BLOCK_HEADER field layout after T(name):
            //   BS(is_xref_resolved), BB anonymous/hasattrs, BB blkisxref/xrefoverlaid,
            //   [B xref_loaded (R2000+)], [BL num_owned if !blkisxref && !xrefoverlaid]
            // For R2004 the field boundaries can shift, so we skip the remaining
            // main-data fields and read directly from the handle stream instead.
            (void)anonymous;
            (void)hasattrs;

            if (!block_name.empty()) {
                m_sections.block_names[handle] = block_name;
            }

            // Always read handles from the handle stream regardless of num_owned
            // (num_owned may be misaligned for R2004).
            {
                size_t hs_bit_start = main_data_bits;
                size_t hs_bit_end   = entity_bits;
                size_t hs_bits      = (hs_bit_end > hs_bit_start) ? (hs_bit_end - hs_bit_start) : 0;
                if (hs_bits >= 8 && (hs_bit_end + 7) / 8 <= entity_data_bytes) {
                    DwgBitReader hreader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
                    hreader.set_bit_offset(hs_bit_start);
                    hreader.set_bit_limit(hs_bit_end);

                    // First handle in stream = BLOCK entity handle
                    auto block_entity_ref = hreader.read_h();
                    if (!hreader.has_error()) {
                        uint64_t block_entity_handle = resolve_handle_ref(handle, block_entity_ref);
                        if (block_entity_handle != 0 && !block_name.empty()) {
                            ctx.block_names_from_entities[block_entity_handle] = block_name;
                        }
                    }

                    // Remaining handles = entities owned by this block.
                    // Read until stream exhausted (don't trust num_owned for R2004).
                    uint32_t collected = 0;
                    for (int h_idx = 0; h_idx < 4096 && !hreader.has_error(); ++h_idx) {
                        auto entity_ref = hreader.read_h();
                        if (hreader.has_error()) break;
                        uint64_t entity_handle = resolve_handle_ref(handle, entity_ref);
                        if (entity_handle != 0) {
                            ctx.entity_handle_to_block_header[entity_handle] = handle;
                            collected++;
                        }
                    }
                    (void)block_name;
                    (void)hs_bits;
                }
            }

            ctx.non_graphic_count++;
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
                        auto& refs = ctx.layout_handle_refs[layout_index];
                        for (int h_idx = 0; h_idx < 32 && !hreader.has_error(); ++h_idx) {
                            auto href = hreader.read_h();
                            if (hreader.has_error()) break;
                            if (href.value == 0 && href.code == 0) continue;
                            uint64_t abs_handle = resolve_handle_ref(handle, href);
                            if (abs_handle != 0) {
                                refs.push_back(abs_handle);
                                if (layout_index >= 0 &&
                                    static_cast<size_t>(layout_index) < scene.layouts().size()) {
                                    auto& layouts_mut =
                                        const_cast<std::vector<Layout>&>(scene.layouts());
                                    if (layouts_mut[static_cast<size_t>(layout_index)].layout_handle == 0) {
                                        layouts_mut[static_cast<size_t>(layout_index)].layout_handle = handle;
                                    }
                                }
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
            ctx.non_graphic_count++;
            continue;
        }

        // ---- Create reader for entity-specific parsing ----
        auto make_reader = [&]() -> DwgBitReader {
            DwgBitReader r2(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);
            r2.set_bit_limit(entity_bits);
            r2.set_bit_offset(reader.bit_offset());
            r2.set_r2007_plus(is_r2007_plus);

            if (record.has_string_stream) {
                r2.restore_string_stream(
                    obj_data + offset + ms_bytes + umc_bytes,
                    entity_data_bytes,
                    record.string_stream_bit_pos);
            }
            return r2;
        };

        // ---- Dispatch to entity-specific parser ----
        size_t entities_before = scene.entities().size();
        {
            DwgBitReader entity_reader = make_reader();
            parse_dwg_entity(entity_reader, obj_type, entity_hdr, scene, m_version);
        }

        // ---- Role-based handle stream decoding ----
        decode_role_handles(ctx, obj_type, handle, scene,
                             obj_data, obj_data_size, offset, entity_data_bytes,
                             ms_bytes, umc_bytes, main_data_bits, entity_bits,
                             entities_before, saved_num_reactors, saved_is_xdic_missing);

        ctx.graphic_count++;
    }

    // ---- Diagnostic record emission ----
    emit_parse_diagnostics(ctx, scene, obj_data, obj_data_size, is_r2010_plus);
    // ---- Post-processing: resolve INSERT block_index, layout parsing, owner resolution ----
    run_post_processing(ctx, scene, obj_data, obj_data_size, is_r2010_plus);

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

// ============================================================
// class_name_for_object_type — look up class name for an object type
// ============================================================

std::string DwgParser::class_name_for_object_type(uint32_t type) const
{
    auto it = m_sections.class_map.find(type);
    if (it != m_sections.class_map.end()) return it->second.first;
    return {};
}

} // namespace cad
