#pragma once

#include "cad/parser/dwg_reader.h"
#include "cad/cad_types.h"

namespace cad {

class EntitySink;
struct EntityHeader;

// ============================================================
// DWG HATCH entity parser (type 78 / 0x4E)
//
// Parses HATCH boundary loops (polyline and edge-defined),
// tessellates arc/ellipse/spline edges into line-segment
// vertices, and emits a single HatchEntity per HATCH object.
// ============================================================

void parse_hatch(DwgBitReader& r, const EntityHeader& hdr, EntitySink& scene,
                 DwgVersion version);

} // namespace cad
