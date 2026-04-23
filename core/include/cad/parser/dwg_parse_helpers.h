#pragma once

#include "cad/parser/dwg_reader.h"

#include <cstdint>

namespace cad {

class EntitySink;

// Returns true if the DWG type number is a graphic entity.
bool is_graphic_entity(uint32_t obj_type);

// Validate that layout dimensions are within reasonable bounds.
bool valid_layout_size(double w, double h);

// Read two raw doubles from the reader.
void read_2rd(DwgBitReader& reader, double& x, double& y);

// Parse a LAYOUT object from the reader, add it to scene, return layout index.
int32_t parse_layout_object(DwgBitReader& reader, EntitySink& scene, DwgVersion version);

// Resolve a handle reference relative to a source handle.
uint64_t resolve_handle_ref(uint64_t source_handle, const DwgBitReader::HandleRef& ref);

// Convert ASCII string to uppercase.
std::string uppercase_ascii(std::string s);

// ASCII case-insensitive substring search.
bool contains_ascii_ci(const std::string& text, const char* needle);

} // namespace cad
