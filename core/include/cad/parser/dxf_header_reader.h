#pragma once

#include "cad/parser/dxf_section_reader.h"
#include "cad/cad_types.h"

namespace cad {

// Reads the HEADER section of a DXF file.
// Extracts key drawing variables and stores them in SceneGraph::drawing_info.
class DxfHeaderReader : public DxfSectionReader {
public:
    Result read(DxfTokenizer& tokenizer, SceneGraph& scene) override;

private:
    // Reads a single 3D point from successive 10/20/30 (or similar) group codes
    static Vec3 read_point_3d(const Vec3& current, int code, double value);

    // Reads one variable (e.g. $ACADVER) from group code/value pairs
    void read_variable(DxfTokenizer& tokenizer, SceneGraph& scene);
};

} // namespace cad
