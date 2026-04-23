// R2004+ section decoder — DwgParser member functions for decrypting,
// decompressing, and assembling R2004+ DWG file sections.
//
// Extracted from dwg_parser.cpp for modularity.  All functions here are
// DwgParser member functions declared in cad/parser/dwg_parser.h.

#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_diagnostics.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace cad {

namespace diag = detail::diagnostics;

namespace {

// ---- Local copies of small helpers also present in dwg_parser.cpp ----
// These live in an anonymous namespace in each translation unit so that
// the two .cpp files remain independently compilable.

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

} // namespace

// ============================================================
// decrypt_r2004_header -- decrypt 108-byte header at offset 0x80
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
// read_section_page_map -- decompress and parse page map entries
//
// The page map is at section_map_address + 0x100 in the file.
// The page map header is NOT encrypted (unlike regular section pages).
// It uses a 20-byte header:
//   [0x00] section_type (RL) -- should be 0x41630e3b
//   [0x04] decomp_data_size (RL) -- decompressed size
//   [0x08] comp_data_size (RL) -- compressed data size
//   [0x0C] compression_type (RL) -- 2 = LZ77 compressed
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

    // The section page map header is NOT encrypted -- read raw bytes.
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
// read_section_info -- find section info page, decompress, parse descriptors
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

            // data_size: RL -- compressed data size for this page
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
// build_sections_from_page_headers -- fallback when section info fails
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
// read_sections -- decompress and assemble all file sections
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
                // Uncompressed -- copy directly to page.address
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
        {
            FILE* fobj = fopen("/tmp/dwg_object_data.bin", "wb");
            if (fobj) {
                fwrite(m_sections.object_data.data(), 1, m_sections.object_data.size(), fobj);
                fclose(fobj);
            }
        }
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

// ============================================================
// record_auxiliary_section_diagnostics
// ============================================================

void DwgParser::record_auxiliary_section_diagnostics(EntitySink& scene) const
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
        const auto strings = diag::extract_printable_strings(aux.data.data(), aux.data.size(), 64);
        const auto markers = diag::select_auxiliary_section_markers(strings, 8);
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

            const auto points = diag::extract_plausible_raw_points(aux.data.data(), aux.data.size(), 8);
            for (const auto& p : points) {
                dwg_debug_log("[DWG] auxiliary '%s' point: %.6f, %.6f\n",
                              aux.name.c_str(), p.first, p.second);
            }
            const auto ints = diag::extract_small_int_candidates(aux.data.data(), aux.data.size(), 16);
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

} // namespace cad
