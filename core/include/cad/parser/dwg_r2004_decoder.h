#pragma once

// R2004+ section decoder — DwgParser member function implementations
// separated from dwg_parser.cpp for modularity.
//
// Functions defined in dwg_r2004_decoder.cpp:
//   decrypt_r2004_header
//   read_section_page_map
//   find_page_file_offset_
//   read_section_info
//   parse_section_info_data
//   build_sections_from_page_headers
//   read_sections
//   record_auxiliary_section_diagnostics
//
// No additional declarations needed — these are all DwgParser member functions
// declared in dwg_parser.h.  This header exists only to document the split.

namespace cad {
// (empty — all declarations are in dwg_parser.h)
} // namespace cad
