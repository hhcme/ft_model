#pragma once

#include "cad/parser/dwg_reader.h"
#include "cad/scene/entity.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace cad {

class SceneGraph;

// ============================================================
// DWG entity/object type readers
//
// Parses individual DWG entities from bitstream data.
// The DwgBitReader is positioned AFTER the common entity header.
// obj_type is the DWG type number (e.g. 19=LINE, 18=CIRCLE, etc.).
// The header contains layer_index, color, visibility, etc. that
// were already decoded from the common entity header by the caller.
// ============================================================

// Parse a DWG entity from bitstream data.
// On error or unknown type, silently skips the entity (no throw).
enum class DwgVersion : uint8_t;

void parse_dwg_entity(DwgBitReader& reader, uint32_t obj_type,
                       const EntityHeader& header, SceneGraph& scene,
                       DwgVersion version);

// Parse a DWG table/object entry (LAYER, LTYPE, STYLE, DIMSTYLE).
// The reader is positioned after the common object header.
// For R2010+, a handle stream at the end of the object data
// contains linetype/plotstyle/material handles.
// This function reads those handles after the main fields.
void parse_dwg_table_object(DwgBitReader& reader, uint32_t obj_type,
                              SceneGraph& scene, DwgVersion version,
                              size_t entity_bits, size_t main_data_bits);

// Diagnostic helper: returns a map of successfully parsed DWG type -> count.
std::unordered_map<uint32_t, size_t> get_dwg_entity_success_counts();

// Diagnostic helper: returns a map of dispatched DWG type -> count.
std::unordered_map<uint32_t, size_t> get_dwg_entity_dispatch_counts();

} // namespace cad
