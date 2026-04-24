#pragma once

#include "cad/cad_errors.h"
#include "cad/cad_types.h"
#include "cad/parser/dwg_reader.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace cad {

class SceneGraph;
class EntitySink;
struct ParseObjectsContext;


// ============================================================
// R2004+ file header (decrypted 108-byte header at offset 0x80)
// ============================================================
struct R2004FileHeader {
    char file_id_string[13] = {};     // 12 bytes sentinel "AcFssFcAJMB\0"
    uint32_t header_address = 0;      // @0x0C
    uint32_t header_size = 0;         // @0x10 (should be 0x6C = 108)
    uint32_t x04 = 0;                // @0x14 (always 4)
    int32_t root_tree_node_gap = 0;   // @0x18
    int32_t lowermost_left_gap = 0;   // @0x1C
    int32_t lowermost_right_gap = 0;  // @0x20
    uint32_t unknown_long = 0;        // @0x24 (always 1)
    uint32_t last_section_id = 0;     // @0x28
    uint64_t last_section_address = 0;// @0x2C (8 bytes)
    uint64_t secondheader_address = 0;// @0x34 (8 bytes)
    uint32_t numgaps = 0;             // @0x3C
    uint32_t numsections = 0;         // @0x40
    uint32_t x20 = 0;                // @0x44 (always 0x20)
    uint32_t x80 = 0;                // @0x48 (always 0x80)
    uint32_t x40 = 0;                // @0x4C (always 0x40)
    uint32_t section_map_id = 0;      // @0x50
    uint64_t section_map_address = 0; // @0x54 (8 bytes, add 0x100 for file offset)
    int32_t section_info_id = 0;      // @0x5C
    uint32_t section_array_size = 0;  // @0x60
    uint32_t gap_array_size = 0;      // @0x64
    uint32_t crc32 = 0;              // @0x68
};

// ============================================================
// R2004+ section page map entry
// ============================================================
struct SectionPageMapEntry {
    int32_t number = 0;   // Section number (negative = gap)
    uint32_t size = 0;    // Section size
    uint64_t address = 0; // Computed running address in file
};

// ============================================================
// R2004+ section info descriptor
// ============================================================
struct SectionInfoDesc {
    uint64_t size = 0;           // Total decompressed size
    uint32_t num_sections = 0;   // Number of pages
    uint32_t max_decomp_size = 0;// Max page decompressed size
    uint32_t compressed = 0;     // Compression flag (2 = compressed)
    uint32_t type = 0;          // Section type
    uint32_t encrypted = 0;     // Encryption type (0=none)
    char name[64] = {};         // Section name
    // Page records for this section
    struct PageInfo {
        int32_t number = 0;         // Page number (key for page map lookup)
        uint32_t size = 0;          // Compressed size
        uint64_t address = 0;       // Decompressed data offset (NOT file offset)
    };
    std::vector<PageInfo> pages;
};

struct DwgAuxiliarySection {
    std::string name;
    std::vector<uint8_t> data;
};

// ============================================================
// DWG R2004+ section data storage
// ============================================================
struct DwgFileSections {
    std::vector<uint8_t> header_vars;      // Section 0: Header Variables
    std::vector<uint8_t> classes;          // Section 1: Classes
    std::vector<uint8_t> object_map;       // Section 2: Object Map
    std::vector<uint8_t> object_data;      // Section 3+: Object Data (merged)

    // Absolute file offset of the start of the object_data section.
    // Used to convert absolute file offsets (from handle_map) to relative
    // offsets within the object_data buffer.
    size_t object_data_file_offset = 0;

    // handle -> offset within object_data
    std::unordered_map<uint64_t, size_t> handle_map;

    // Additional handle -> offset candidates from alternate Object Map
    // accumulator interpretations. The primary handle_map remains authoritative;
    // these are only used when the primary offset does not frame a valid object
    // and the candidate object's own handle matches the requested handle.
    std::unordered_map<uint64_t, std::vector<size_t>> handle_offset_candidates;

    // class type number -> (dxf_name, is_entity)
    std::unordered_map<uint32_t, std::pair<std::string, bool>> class_map;

    // block_header handle -> block name (from parsing type=49 objects)
    std::unordered_map<uint64_t, std::string> block_names;

    // Per-page info for Object Data pages: (offset_in_object_data, valid_byte_count)
    // Used for page-by-page entity scanning when handle_map offsets are unreliable.
    std::vector<std::pair<size_t, size_t>> object_pages;

    // Object Map page size (max_decomp_size from section_info).
    // Used to detect page boundaries during stream-based Object Map parsing.
    uint32_t objmap_page_size = 0;

    // Non-primary DWG sections that carry product-specific presentation
    // semantics, for example AutoCAD Mechanical/AcDs associative data.
    std::vector<DwgAuxiliarySection> auxiliary_sections;
};

// ============================================================
// DWG version constants
// ============================================================
enum class DwgVersion : uint8_t {
    Unknown = 0,
    R2000   = 1,    // AC1015
    R2004   = 2,    // AC1018
    R2007   = 3,    // AC1021
    R2010   = 4,    // AC1024
    R2013   = 5,    // AC1027
    R2018   = 6,    // AC1032
};

// Shared debug logging utilities (used across parser modules)
inline bool dwg_debug_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("FT_DWG_DEBUG");
        return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void dwg_debug_log(const char* fmt, ...);

const char* version_family_name(DwgVersion version);

// Forward-declared from dwg_objects.h — type-specific entity/object parsing.
struct EntityHeader;
void parse_dwg_entity(DwgBitReader& reader, uint32_t obj_type,
                       const EntityHeader& header, EntitySink& scene,
                       DwgVersion version);

// ============================================================
// DWG file parser — reads DWG files and populates a SceneGraph
//
// Supports R2004+ format (AC1018, AC1021, AC1024, AC1027, AC1032).
// Uses DwgBitReader for bitstream decoding, LZ77 decompression,
// and XOR header decryption.
// ============================================================
class DwgParser {
public:
    DwgParser();
    ~DwgParser();

    // Non-copyable
    DwgParser(const DwgParser&) = delete;
    DwgParser& operator=(const DwgParser&) = delete;

    // Parse from file path
    Result parse_file(const std::string& filepath, SceneGraph& scene);

    // Parse from memory buffer
    Result parse_buffer(const uint8_t* data, size_t size, SceneGraph& scene);

private:
    // ---- Internal parsing stages ----

    // Read and validate the version string at offset 0
    Result read_version(const uint8_t* data, size_t size);

    // Decrypt the R2004+ file header at offset 0x80, parse R2004FileHeader fields
    Result decrypt_r2004_header(const uint8_t* data, size_t size);

    // Read the R2007/AC1021 R21 container. This is separate from the
    // R2004/R2010 section-page reader: AC1021 uses an interleaved
    // Reed-Solomon-protected file header plus R21 system pages.
    Result read_r2007_container(const uint8_t* data, size_t size,
                                EntitySink& scene);

    // Read R2000/AC1015 flat sections. R2000 uses sentinel-delimited
    // sections with no encryption or compression.
    Result read_r2000_sections(const uint8_t* data, size_t size,
                               EntitySink& scene);

    // Read the section page map at section_map_address+0x100, decompress, parse entries
    Result read_section_page_map(const uint8_t* data, size_t size);

    // Read section info page to get section descriptors and page records
    Result read_section_info(const uint8_t* data, size_t size);

    // Fallback: discover sections by scanning page headers when section info fails
    Result build_sections_from_page_headers(const uint8_t* data, size_t size);

    // Decompress and assemble all file sections from page records
    Result read_sections(const uint8_t* data, size_t size);

    // Parse Section 0: Header Variables (extents, metadata)
    Result parse_header_variables(EntitySink& scene);

    // Parse Section 1: Classes (type number -> name + is_entity)
    Result parse_classes(EntitySink& scene);

    // Parse Section 2: Object Map (handle -> offset in object data)
    Result parse_object_map(const uint8_t* data, size_t size);

    // Record diagnostics for DWG sections that are decoded but not yet
    // semantically interpreted by the self-developed pipeline.
    void record_auxiliary_section_diagnostics(EntitySink& scene) const;

    // Iterate the object map and parse each entity via parse_dwg_entity()
    Result parse_objects(EntitySink& scene);

    // Pre-scan LTYPE/LAYER/BLOCK_HEADER table objects to populate handle→index
    // maps and block name lookups before the main object processing loop.
    // Populates m_layer_handle_to_index, m_linetype_handle_to_index,
    // m_sections.block_names, and block_names_from_entities.
    void prescan_table_objects(const uint8_t* obj_data, size_t obj_data_size,
                               bool is_r2007_plus, bool is_r2010_plus,
                               EntitySink& scene,
                               std::unordered_map<uint64_t, std::string>& block_names_from_entities);

    // ---- Extracted parse_objects() sub-functions ----

    // Process BLOCK (type 4) / ENDBLK (type 5) stream markers.
    // Returns true if the object was a BLOCK or ENDBLK (caller should continue).
    bool process_block_endblk(ParseObjectsContext& ctx,
                               uint32_t obj_type, uint64_t handle,
                               EntitySink& scene,
                               const uint8_t* obj_data, size_t entity_data_bytes,
                               size_t ms_bytes, size_t umc_bytes,
                               size_t main_data_bits, size_t entity_bits);

    // Decode role-based handles (owner, reactors, xdic, layer, entity-specific)
    // for graphic entities that produced geometry.
    void decode_role_handles(ParseObjectsContext& ctx,
                              uint32_t obj_type, uint64_t handle,
                              EntitySink& scene,
                              const uint8_t* obj_data, size_t obj_data_size,
                              size_t offset, size_t entity_data_bytes,
                              size_t ms_bytes, size_t umc_bytes,
                              size_t main_data_bits, size_t entity_bits,
                              size_t entities_before,
                              uint32_t saved_num_reactors,
                              bool saved_is_xdic_missing,
                              size_t entity_data_end_offset = 0);

    // Detect custom annotation/detail objects and emit proxy geometry.
    void process_custom_annotation_proxy(ParseObjectsContext& ctx,
                                          uint32_t obj_type, uint64_t handle,
                                          EntitySink& scene,
                                          const uint8_t* obj_data, size_t obj_data_size,
                                          size_t offset, size_t entity_data_bytes,
                                          size_t ms_bytes, size_t umc_bytes,
                                          size_t main_data_bits, size_t entity_bits,
                                          bool is_graphic, bool is_r2007_plus,
                                          const EntityHeader& entity_hdr,
                                          size_t reader_bit_offset);

    // Emit all diagnostic records after the main loop finishes.
    void emit_parse_diagnostics(ParseObjectsContext& ctx,
                                 EntitySink& scene,
                                 const uint8_t* obj_data, size_t obj_data_size,
                                 bool is_r2010_plus);

    // Run post-processing: INSERT block_index resolution, layout parsing,
    // and owner resolution.
    void run_post_processing(ParseObjectsContext& ctx,
                              EntitySink& scene,
                              const uint8_t* obj_data, size_t obj_data_size,
                              bool is_r2010_plus);

    // Look up object type for a handle (from cache or by framing the object).
    uint32_t lookup_object_type(ParseObjectsContext& ctx,
                                 uint64_t ref_handle,
                                 const uint8_t* obj_data, size_t obj_data_size,
                                 bool is_r2007_plus, bool is_r2010_plus);

    // Look up class name for an object type.
    std::string class_name_for_object_type(uint32_t type) const;

    // Parse section info descriptors from decompressed data
    Result parse_section_info_data(std::vector<uint8_t> info_data);

    // ---- Object preparation / recovery helpers ----

    // Result of framing a single DWG object at a given offset within
    // object_data.  Populated by prepare_object_at_offset() and used by
    // try_recover_object() / recover_from_candidates().
    struct PreparedObject {
        size_t offset = 0;
        size_t ms_bytes = 0;
        size_t umc_bytes = 0;
        size_t entity_data_bytes = 0;
        size_t entity_bits = 0;
        size_t main_data_bits = 0;
        size_t handle_stream_bits = 0;
        bool handle_stream_valid = true;
        bool has_string_stream = false;
        size_t string_stream_bit_pos = 0;
        uint32_t obj_type = 0;
        DwgBitReader::HandleRef self_handle;

        size_t entity_data_offset() const { return offset + ms_bytes + umc_bytes; }
        size_t handle_stream_bit_start() const { return main_data_bits; }
        size_t handle_stream_bit_end() const { return entity_bits; }
        bool has_handle_stream() const { return handle_stream_bits >= 8 && handle_stream_valid; }
    };

    // Check whether obj_type is a known graphic entity or table object.
    bool is_known_object_type(uint32_t obj_type) const;

    // Frame (validate) a DWG object at the given offset within obj_data.
    // Returns true if the offset points to a well-formed object header.
    // When require_known_type is true, unknown object types are rejected.
    // When require_valid_handle_stream is true, a corrupted handle stream
    // causes failure.  On failure, *failure_reason (if non-null) is set.
    bool prepare_object_at_offset(const uint8_t* obj_data, size_t obj_data_size,
                                  bool is_r2010_plus,
                                  size_t record_offset, PreparedObject& record,
                                  bool require_valid_handle_stream,
                                  bool require_known_type,
                                  const char** failure_reason = nullptr) const;

    // Try to recover an object for target_handle by scanning candidate
    // offsets from the handle_offset_candidates map.  Only candidates whose
    // self-handle matches target_handle are accepted.
    bool recover_from_candidates(uint64_t target_handle,
                                 size_t primary_offset,
                                 const uint8_t* obj_data, size_t obj_data_size,
                                 bool is_r2010_plus,
                                 PreparedObject& recovered) const;

    // Try all recovery strategies for an object with target_handle whose
    // primary offset failed prepare_object_at_offset().
    // Strategy 1: candidate offsets from handle_offset_candidates.
    // Strategy 2: full page scan to find a matching self-handle.
    // On success, recovered is populated and the method returns true.
    bool try_recover_object(uint64_t target_handle, size_t primary_offset,
                            const uint8_t* obj_data, size_t obj_data_size,
                            bool is_r2007_plus, bool is_r2010_plus,
                            PreparedObject& recovered);

    // ---- Helpers ----

    // Look up a section page map entry by page number.
    // Returns pointer to the entry, or nullptr if not found.
    const SectionPageMapEntry* find_page_map_entry(int32_t page_number) const;

    // Scan file page headers to find the file offset for a given page number.
    // Returns file offset, or (uint64_t)-1 if not found.
    uint64_t find_page_file_offset_(const uint8_t* data, size_t file_size,
                                     int32_t target_page_number) const;

public:
    // Decode modular char (variable-length encoding, high bit = continuation)
    static uint32_t read_modular_char(const uint8_t* data, size_t size,
                                       size_t& offset);

    // Decode signed modular char (MC) — sign-extended from highest data bit
    static int32_t read_modular_char_signed(const uint8_t* data, size_t size,
                                             size_t& offset);

private:
    // Read a little-endian 32-bit value from data at offset
    static uint32_t read_le32(const uint8_t* data, size_t offset);

    // Read a little-endian 16-bit value from data at offset
    static uint16_t read_le16(const uint8_t* data, size_t offset);

    // Read a 6-byte little-endian address (48 bits) from data at offset
    static uint64_t read_le48(const uint8_t* data, size_t offset);

    // Decrypt a 32-byte R2004+ section page header at given file address
    static void decrypt_section_page_header(const uint8_t* encrypted,
                                             uint8_t* decrypted,
                                             size_t address);

    // ---- Internal state ----

    DwgVersion m_version = DwgVersion::Unknown;
    DwgFileSections m_sections;
    R2004FileHeader m_file_header;

    // Section page map entries (from section page map)
    std::vector<SectionPageMapEntry> m_page_map_entries;

    // Section info descriptors (from section info page)
    std::vector<SectionInfoDesc> m_section_infos;

    // ---- DWG block definition tracking ----
    // In DWG, block definitions are marked by BLOCK (type 4) / ENDBLK (type 5)
    // entities in the object stream. Entities between them belong to the block.
    std::string m_current_block_name;
    Vec3 m_current_block_base_point = Vec3::zero();
    size_t m_block_entity_start = 0;
    uint64_t m_current_block_handle = 0;
    uint64_t m_current_block_header_handle = 0;

    // ---- DWG table handle tracking ----
    // Maps table object handles → SceneGraph indices. Populated during table
    // pre-scan because entities/layers may appear before their table records.
    std::unordered_map<uint64_t, int32_t> m_layer_handle_to_index;
    std::unordered_map<uint64_t, int32_t> m_linetype_handle_to_index;

    // ---- Object recovery scan budget ----
    // Limits total recovery scans across all objects within a single
    // parse_objects() call.  Reset at the start of each call.
    size_t m_object_recovery_scans = 0;
    size_t m_object_recovery_bytes_scanned = 0;
};

} // namespace cad
