#pragma once

#include "cad/parser/dxf_tokenizer.h"
#include "cad/cad_errors.h"

namespace cad {

class SceneGraph;

// Base class for all DXF section readers
class DxfSectionReader {
public:
    virtual ~DxfSectionReader() = default;
    virtual Result read(DxfTokenizer& tokenizer, SceneGraph& scene) = 0;

protected:
    // Skip to next entity type marker (group code 0) or ENDSEC
    void skip_unknown_entity(DxfTokenizer& tokenizer);

    // Read all pairs until we hit group code 0
    void skip_to_code_zero(DxfTokenizer& tokenizer);
};

} // namespace cad
