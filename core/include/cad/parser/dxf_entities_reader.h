#pragma once

#include "cad/parser/dxf_section_reader.h"
#include "cad/scene/scene_graph.h"

namespace cad {

// Reads the ENTITIES section of a DXF file.
// Dispatches to type-specific parsing based on group code 0 entity markers.
class DxfEntitiesReader : public DxfSectionReader {
public:
    Result read(DxfTokenizer& tokenizer, SceneGraph& scene) override;

private:
    // Phase 1: fully implemented entity readers
    void parse_line(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_circle(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_arc(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_lwpolyline(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_polyline(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_insert(DxfTokenizer& tokenizer, SceneGraph& scene);

    // Phase 2: stub readers (skip to next entity)
    void parse_text(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_mtext(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_dimension(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_hatch(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_spline(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_ellipse(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_point(DxfTokenizer& tokenizer, SceneGraph& scene);
    void parse_solid(DxfTokenizer& tokenizer, SceneGraph& scene);

    // Read common entity header fields (layer, linetype, color, lineweight)
    // from the current group code/value pair into header.
    // Returns true if the group code was consumed (was a header field).
    static bool read_entity_header_field(EntityHeader& header,
                                          const DxfGroupCodeValue& gv);
};

} // namespace cad
