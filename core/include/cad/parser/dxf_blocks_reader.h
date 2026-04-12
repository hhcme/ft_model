#pragma once

#include "cad/parser/dxf_section_reader.h"

namespace cad {

// Reads the BLOCKS section of a DXF file.
// For each BLOCK...ENDBLK pair, creates a Block entry and parses contained entities.
class DxfBlocksReader : public DxfSectionReader {
public:
    Result read(DxfTokenizer& tokenizer, SceneGraph& scene) override;

private:
    // Parse a single block definition and its entities
    void parse_block(DxfTokenizer& tokenizer, SceneGraph& scene);
};

} // namespace cad
