#pragma once

#include "cad/parser/dxf_section_reader.h"

namespace cad {

// Reads the TABLES section of a DXF file.
// Dispatches to sub-parsers for each table type (LTYPE, LAYER, STYLE, VPORT, etc.).
class DxfTablesReader : public DxfSectionReader {
public:
    Result read(DxfTokenizer& tokenizer, SceneGraph& scene) override;

private:
    // Parse a single LTYPE table entry
    void parse_ltype_table(DxfTokenizer& tokenizer, SceneGraph& scene);
    // Parse a single LAYER table entry
    void parse_layer_table(DxfTokenizer& tokenizer, SceneGraph& scene);
    // Parse a single STYLE table entry
    void parse_style_table(DxfTokenizer& tokenizer, SceneGraph& scene);
    // Parse a single VPORT table entry
    void parse_vport_table(DxfTokenizer& tokenizer, SceneGraph& scene);
    // Skip an unknown table
    void skip_table(DxfTokenizer& tokenizer);
};

} // namespace cad
