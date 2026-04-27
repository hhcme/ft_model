#pragma once

#include "cad/parser/dwg_reader.h"
#include "cad/cad_types.h"

namespace cad {

class EntitySink;
struct EntityHeader;

// ============================================================
// DWG annotation and hatch entity parsers
//
// TEXT (type 1), MTEXT (type 44), DIMENSION (types 20-26),
// and HATCH (type 78) parsing extracted from dwg_objects.cpp.
// ============================================================

void parse_text(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                DwgVersion version);

void parse_mtext(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);

void parse_dimension(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version);

void parse_hatch(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);

void parse_leader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version);

void parse_tolerance(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version);

} // namespace cad
