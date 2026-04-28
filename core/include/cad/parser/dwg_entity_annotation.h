#pragma once

#include "cad/parser/dwg_reader.h"
#include "cad/cad_types.h"

namespace cad {

class EntitySink;
struct EntityHeader;

// ============================================================
// DWG annotation entity parsers
//
// TEXT (type 1), MTEXT (type 44), DIMENSION (types 20-26),
// LEADER (type 45), TOLERANCE (type 46), MULTILEADER (class-based).
// HATCH (type 78) is in dwg_entity_hatch.h.
// ============================================================

void parse_text(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                DwgVersion version);

void parse_mtext(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);

void parse_dimension(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version);

void parse_leader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                  DwgVersion version);

void parse_tolerance(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                     DwgVersion version);

void parse_multileader(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                       DwgVersion version);

} // namespace cad
