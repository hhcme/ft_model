#pragma once

#include "cad/parser/dxf_section_reader.h"

namespace cad {

// Reads the OBJECTS section of a DXF file.
// Phase 1: minimal implementation — skips all content.
class DxfObjectsReader : public DxfSectionReader {
public:
    Result read(DxfTokenizer& tokenizer, SceneGraph& scene) override;
};

} // namespace cad
