#pragma once

// BlockReference is represented by InsertEntity in entity.h.
// An InsertEntity stores the block_index, insertion point, scale,
// rotation, and array parameters — serving as the block reference
// (also known as an "insert" in DXF terminology).

// No separate struct is needed; use InsertEntity directly.

#include "cad/scene/entity.h"

namespace cad {

// Alias for clarity in code that refers to "block references"
using BlockReference = InsertEntity;

} // namespace cad
