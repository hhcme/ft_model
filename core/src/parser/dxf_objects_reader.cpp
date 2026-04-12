#include "cad/parser/dxf_objects_reader.h"
#include "cad/scene/scene_graph.h"

namespace cad {

Result DxfObjectsReader::read(DxfTokenizer& tokenizer, SceneGraph& /*scene*/) {
    // Phase 1: skip all content in the OBJECTS section.
    // Read until we find ENDSEC.

    while (true) {
        auto result = tokenizer.next();
        if (!result.ok() || !result.value) {
            return Result::error(ErrorCode::UnexpectedToken,
                                  "Unexpected EOF in OBJECTS section");
        }

        const auto& gv = tokenizer.current();

        if (gv.is_section_end()) {
            return Result::success();
        }

        // Skip everything else
    }
}

} // namespace cad
