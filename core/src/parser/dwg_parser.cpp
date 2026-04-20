#include "cad/parser/dwg_parser.h"
#include "cad/parser/dwg_reader.h"
#include "cad/parser/dwg_objects.h"
#include "cad/scene/scene_graph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace cad {

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

    r = parse_classes();
    if (!r) return r;

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

    fprintf(stderr, "[DWG] version=%s (enum=%d)\n", ver, static_cast<int>(m_version));
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

    fprintf(stderr, "[DWG] header_id='%.12s' header_size=%u section_map_id=%u "
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

    fprintf(stderr, "[DWG] page_map_header: section_type=0x%08X decomp=%u comp=%u type=%u\n",
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

    fprintf(stderr, "[DWG] page_map: %zu entries\n", m_page_map_entries.size());
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

    fprintf(stderr, "[DWG] section_info page: offset=0x%llx tag=0x%08X decomp=%u comp=%u type=%u\n",
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

        fprintf(stderr, "[DWG] section_info encrypted page: page_type=0x%08X sec_type=%u data_size=%u decomp=%u\n",
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

    fprintf(stderr, "[DWG] section_info: num_desc=%u compressed=%u max_size=%u encrypted=%u\n",
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
            fprintf(stderr, "[DWG] WARNING: section '%s' has too many pages (%u), skipping\n",
                    desc.name, desc.num_sections);
            continue;
        }

        fprintf(stderr, "[DWG] section_info[%u]: name='%.64s' pages=%u size=%llu "
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
    fprintf(stderr, "[DWG] WARNING: section info failed, trying page header scan fallback\n");

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

    fprintf(stderr, "[DWG] fallback: found %zu sections\n", m_section_infos.size());
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
        }
    }

    // If no sections matched by name, try by type/order
    if (!sec_header_vars || !sec_classes || !sec_object_map) {
        fprintf(stderr, "[DWG] WARNING: section name matching incomplete, trying by order\n");
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
                    name.find("AcDb:ObjectMap") == std::string::npos) {
                    sec_object_data_list.push_back(&m_section_infos[i]);
                }
            }
        }
    }

    // Decompress a section into a buffer.
    // For each page in the section, look up its file offset from the page map,
    // then decrypt the page header, decompress the data, and append.
    auto decompress_section = [&](SectionInfoDesc& desc) -> std::vector<uint8_t> {
        fprintf(stderr, "[DWG] decompressing section '%s' (%zu pages)\n",
                desc.name, desc.pages.size());
        // Calculate total decompressed size from desc.size
        uint64_t total_size = desc.size;
        if (total_size == 0) {
            // Estimate from max_decomp_size * num_sections
            total_size = static_cast<uint64_t>(desc.max_decomp_size) * desc.num_sections;
        }

        if (total_size == 0 || total_size > 500 * 1024 * 1024) {
            fprintf(stderr, "[DWG] WARNING: section '%s' has invalid total size %llu\n",
                    desc.name, (unsigned long long)total_size);
            return {};
        }

        std::vector<uint8_t> result(static_cast<size_t>(total_size), 0);
        size_t write_offset = 0;

        for (auto& page : desc.pages) {
            fprintf(stderr, "[DWG]   page %d: addr=%llu size=%u\n",
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
                fprintf(stderr, "[DWG] WARNING: page %d not found in page map\n", page.number);
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
                fprintf(stderr, "[DWG] WARNING: page %d at 0x%llx has bad tag 0x%08X\n",
                        page.number, (unsigned long long)page_file_offset, ptag);
                continue;
            }

            if (ds == 0 || page_file_offset + 32 + ds > size) {
                fprintf(stderr, "[DWG] WARNING: page %d invalid data_size=%u\n", page.number, ds);
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

            // Track per-page info for object data
            size_t page_bytes = write_offset - before_offset;
            if (page_bytes > 0) {
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
        fprintf(stderr, "[DWG] header_vars: %zu bytes\n", m_sections.header_vars.size());
    }

    if (sec_classes) {
        m_sections.classes = decompress_section(*sec_classes);
        fprintf(stderr, "[DWG] classes: %zu bytes\n", m_sections.classes.size());
    }

    if (sec_object_map) {
        // Object map pages must be concatenated sequentially (no address gaps)
        // because parse_object_map expects a contiguous stream of RS_BE sections.
        m_sections.object_map.clear();
        m_sections.objmap_page_size = sec_object_map->max_decomp_size;
        fprintf(stderr, "[DWG] decompressing object_map (%zu pages)\n", sec_object_map->pages.size());
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
                fprintf(stderr, "[DWG] WARNING: object_map page %d not found\n", page.number);
                continue;
            }

            // Decrypt the 32-byte page header
            uint8_t phdr[32];
            decrypt_section_page_header(data + page_file_offset, phdr,
                                        static_cast<size_t>(page_file_offset));

            uint32_t ptag = read_le32(phdr, 0x00);
            uint32_t ds   = read_le32(phdr, 0x08);

            if (ptag != 0x4163043b || ds == 0 || page_file_offset + 32 + ds > size) {
                fprintf(stderr, "[DWG] WARNING: object_map page %d bad tag or size\n", page.number);
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
        fprintf(stderr, "[DWG] object_map: %zu bytes (page_size=%u)\n",
                m_sections.object_map.size(), m_sections.objmap_page_size);
    }

    // Merge all object data sections
    if (!sec_object_data_list.empty()) {
        size_t total_obj = 0;
        for (auto* s : sec_object_data_list) total_obj += s->size;
        m_sections.object_data.reserve(total_obj);

        size_t first_offset = static_cast<size_t>(-1);
        for (auto* s : sec_object_data_list) {
            auto buf = decompress_section(*s);
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

        fprintf(stderr, "[DWG] object_data: %zu bytes (file_offset=%zu)\n",
                m_sections.object_data.size(), m_sections.object_data_file_offset);
    }

    return Result::success();
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

Result DwgParser::parse_classes()
{
    if (m_sections.classes.empty()) {
        return Result::success();
    }

    const uint8_t* data = m_sections.classes.data();
    size_t data_size = m_sections.classes.size();

    DwgBitReader reader(data, data_size);
    reader.set_r2007_plus(m_version >= DwgVersion::R2007);

    // R2007+ string stream: setup if applicable
    if (m_version >= DwgVersion::R2007) {
        reader.setup_string_stream(static_cast<uint32_t>(data_size * 8));
    }

    uint32_t max_num = reader.read_rl();  // BL(max_num)
    (void)max_num;

    while (!reader.has_error() && reader.remaining_bits() > 8) {
        size_t entry_start = reader.bit_offset();

        uint16_t class_type = reader.read_bs();
        if (reader.has_error() || class_type == 0) break;

        // For R2007+: TV reads from string stream; for older: TV reads bytes
        std::string dxf_name;
        std::string cpp_name;
        std::string app_name;

        if (m_version < DwgVersion::R2007) {
            dxf_name = reader.read_tv();
            cpp_name = reader.read_tv();
            app_name = reader.read_tv();
        } else {
            // R2007+: T reads from string stream (TU internally)
            dxf_name = reader.read_t();
            cpp_name = reader.read_t();
            app_name = reader.read_t();
        }

        uint16_t proxy_flags = reader.read_bs();
        bool is_entity = reader.read_b();

        // Skip remaining class fields
        // R2000+: BS(proxy_flags2), B(is_zombie)
        if (!reader.has_error()) {
            (void)reader.read_bs();  // proxy_flags2 / app_data_flag
            (void)reader.read_b();   // is_zombie / is_null
        }

        if (reader.has_error()) break;

        m_sections.class_map[class_type] = {dxf_name, is_entity};

        // Align to byte boundary for next entry
        reader.align_to_byte();
    }

    fprintf(stderr, "[DWG] classes: %zu entries parsed\n", m_sections.class_map.size());
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
    uint64_t total_objects = 0;

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
                fprintf(stderr, "[DWG] WARNING: object_map section_size=%u too large at off=%zu\n",
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

            m_sections.handle_map[handle_acc] = static_cast<size_t>(offset_acc);
            total_objects++;
        }

        // Skip CRC (2 bytes, RS_BE) after the section data
        off = section_data_end + 2;
    }

    fprintf(stderr, "[DWG] object_map: %llu entries (data_size=%zu, final_off=%zu)\n",
            (unsigned long long)total_objects, data_size, off);
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
            return false;

        default:
            return false;
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

    size_t processed = 0;
    auto t_start = std::chrono::steady_clock::now();

    // ============================================================
    // Pre-scan: parse LAYER objects (type 51) first to populate
    // m_layer_handle_to_index before any entities need it.
    // This is necessary because the object map is NOT sorted by
    // handle value — entities may appear before their layer objects.
    // ============================================================
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
        if (r.has_error()) continue;

        if (ot == 51) {  // LAYER
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
                                       edb * 8, mdb, handle, &m_layer_handle_to_index);
            }
        }
    }
    fprintf(stderr, "[DWG] Pre-scan: %zu layer handles loaded\n",
            m_layer_handle_to_index.size());

    // Collect INSERT handle stream data for post-processing resolution.
    // Maps entity_index → vector of handles from INSERT handle stream.
    std::unordered_map<size_t, std::vector<uint64_t>> insert_handles;

    // Collect BLOCK entity handle→name mapping from handle streams.
    // BLOCK entities have correct unique names (e.g., "*D1077") while
    // BLOCK_HEADER (type 49) stores truncated names (e.g., "*D").
    std::unordered_map<uint64_t, std::string> block_names_from_entities;
    for (const auto& [handle, offset] : m_sections.handle_map) {
        processed++;
        if ((processed % 10000) == 0) {
            fprintf(stderr, "[DWG] parse_objects progress: %zu / %zu\n",
                    processed, m_sections.handle_map.size());
        }
        if (offset >= obj_data_size) {
            error_count++;
            continue;
        }

        // MS (Modular Short): entity data size in bytes
        DwgBitReader ms_reader(obj_data + offset, obj_data_size - offset);
        uint32_t entity_size = ms_reader.read_modular_short();
        if (ms_reader.has_error() || entity_size == 0 || entity_size > obj_data_size - offset) {
            error_count++;
            continue;
        }

        ms_reader.align_to_byte();
        size_t ms_bytes = ms_reader.bit_offset() / 8;

        size_t entity_data_bytes = static_cast<size_t>(entity_size);
        size_t entity_bits = entity_data_bytes * 8;

        if (offset + ms_bytes + entity_data_bytes > obj_data_size) {
            error_count++;
            continue;
        }

        // UMC (Unsigned Modular Char): handle stream size in BITS for R2010+
        // Per libredwg spec, UMC is read from the page data stream (OUTSIDE entity data).
        // It sits between the MS and the entity data.
        size_t umc_bytes = 0;
        uint32_t handle_stream_size = 0;
        if (is_r2010_plus) {
            uint32_t result = 0;
            int shift = 0;
            const uint8_t* umc_ptr = obj_data + offset + ms_bytes;
            for (int i = 0; i < 4; ++i) {
                uint8_t byte_val = umc_ptr[i];
                result |= static_cast<uint32_t>(byte_val & 0x7F) << shift;
                shift += 7;
                if ((byte_val & 0x80) == 0) {
                    umc_bytes = static_cast<size_t>(i) + 1;
                    break;
                }
            }
            handle_stream_size = result;
        }

        if (offset + ms_bytes + umc_bytes + entity_data_bytes > obj_data_size) {
            error_count++;
            continue;
        }

        // Entity reader starts AFTER MS + UMC, with exactly entity_size bytes.
        DwgBitReader reader(obj_data + offset + ms_bytes + umc_bytes, entity_data_bytes);

        // Main entity data excludes the trailing handle stream (R2010+).
        size_t main_data_bits = entity_bits;
        if (is_r2010_plus && handle_stream_size <= entity_bits) {
            main_data_bits = entity_bits - handle_stream_size;
        }
        reader.set_bit_limit(main_data_bits);

        if (is_r2007_plus && main_data_bits > 0) {
            reader.setup_string_stream(static_cast<uint32_t>(main_data_bits));
        }

        uint32_t obj_type = 0;
        if (is_r2010_plus) {
            obj_type = reader.read_bot();
        } else {
            obj_type = reader.read_bs();
        }

        if (reader.has_error()) {
            error_count++;
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
            (void)reader.read_raw_char();

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

        // Capture string stream state for later restore
        size_t str_bit_pos = reader.string_stream_bit_pos();
        bool has_string_stream = reader.has_string_stream();

        // ---- Handle BLOCK_HEADER (type 49) inline ----
        if (obj_type == 49) {
            if (is_r2010_plus) {
                (void)reader.read_raw_char();  // class_version (RC)
            }

            // BLOCK_HEADER fields:
            // BL(unknown_0), BL(unknown_1), T(block_name), ...
            // Simplified: skip to name field
            (void)reader.read_bl();  // unknown BL
            (void)reader.read_bl();  // unknown BL

            // Read block name
            std::string block_name;
            if (has_string_stream) {
                block_name = reader.read_t();  // reads from string stream (TU)
            } else if (is_r2007_plus) {
                // R2007+ without string stream: name is in string stream,
                // which we can't read. Skip.
                block_name = "";
            } else {
                block_name = reader.read_tv();
            }

            if (!block_name.empty()) {
                m_sections.block_names[handle] = block_name;
            }

            non_graphic_count++;
            continue;
        }

        // Skip all other non-graphic objects
        if (!is_graphic) {
            // Handle table objects (LAYER, LTYPE, STYLE, DIMSTYLE)
            if (obj_type == 51 || obj_type == 53 ||
                obj_type == 57 || obj_type == 69) {
                parse_dwg_table_object(reader, obj_type, scene,
                                       m_version, entity_bits, main_data_bits,
                                       handle, &m_layer_handle_to_index);
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
                        switch (href.code) {
                            case 2: case 3: case 4: case 5:
                                abs_handle = href.value; break;
                            case 6:
                                abs_handle = handle + 1; break;
                            case 8:
                                abs_handle = (handle > 1) ? handle - 1 : 0; break;
                            case 0xA:
                                abs_handle = handle + href.value; break;
                            case 0xC:
                                abs_handle = (handle > href.value) ? handle - href.value : 0; break;
                            default:
                                abs_handle = href.value; break;
                        }
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
                    // Mark as block child so top-level iteration skips them
                    scene.entities()[i].header.in_block = true;
                }
                scene.add_block(block);
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

                    // Resolve absolute handle using same encoding as layer resolution
                    uint64_t abs_handle = 0;
                    switch (href.code) {
                        case 2: case 3: case 4: case 5:
                            abs_handle = href.value; break;
                        case 6:
                            abs_handle = handle + 1; break;
                        case 8:
                            abs_handle = (handle > 1) ? handle - 1 : 0; break;
                        case 0xA:
                            abs_handle = handle + href.value; break;
                        case 0xC:
                            abs_handle = (handle > href.value) ? handle - href.value : 0; break;
                        default:
                            abs_handle = href.value; break;
                    }
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
                // Helper: resolve handle reference to absolute handle
                auto resolve_abs = [handle](const DwgBitReader::HandleRef& ref) -> uint64_t {
                    switch (ref.code) {
                        case 2: case 3: case 4: case 5:
                            return ref.value;  // TYPEDOBJHANDLE: absolute
                        case 6:
                            return handle + 1;
                        case 8:
                            return (handle > 1) ? handle - 1 : 0;
                        case 0xA:
                            return handle + ref.value;
                        case 0xC:
                            return (handle > ref.value) ? handle - ref.value : 0;
                        default:
                            return ref.value;  // code 0,1: soft pointer, treat as absolute
                    }
                };

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

                    uint64_t abs_h = resolve_abs(href);
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

    fprintf(stderr, "[DWG] parse_objects: %zu graphic, %zu non-graphic, %zu errors\n",
            graphic_count, non_graphic_count, error_count);
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
        fprintf(stderr, "[DWG] INSERT post-processing: resolved %zu / %zu INSERTs "
                "(entity_names=%zu, bh_names=%zu, blocks=%zu, apply=%s)\n",
                resolved, insert_handles.size(),
                block_names_from_entities.size(), m_sections.block_names.size(),
                scene.blocks().size(), apply ? "yes" : "no");
    }
    fprintf(stderr, "[DWG] Layer resolution: resolved=%zu map_size=%zu\n",
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
