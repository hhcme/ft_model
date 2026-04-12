#include "cad/parser/dxf_header_reader.h"
#include "cad/scene/scene_graph.h"

namespace cad {

Vec3 DxfHeaderReader::read_point_3d(const Vec3& current, int code, double value) {
    Vec3 result = current;
    // DXF group codes for point components:
    // X: 10-19, Y: 20-29, Z: 30-39
    // We handle only the first point group (code % 10 == 0)
    int code_family = code % 10;
    if (code_family != 0) return result;

    if (code >= 10 && code < 20) {
        result.x = static_cast<float>(value);
    } else if (code >= 20 && code < 30) {
        result.y = static_cast<float>(value);
    } else if (code >= 30 && code < 40) {
        result.z = static_cast<float>(value);
    }
    return result;
}

void DxfHeaderReader::read_variable(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // The current token is group code 9 with the variable name (e.g. "$ACADVER")
    std::string var_name = tokenizer.current().value;

    // Accumulate point components per variable
    Vec3 extmin = scene.drawing_info().extents.min;
    Vec3 extmax = scene.drawing_info().extents.max;
    Vec3 insbase = scene.drawing_info().insertion_base;

    while (true) {
        auto peek_result = tokenizer.peek();
        if (!peek_result.ok() || !peek_result.value) break;

        const auto& peeked = tokenizer.peeked();
        if (peeked.code == 9 || peeked.code == 0) {
            // Next variable or ENDSEC — stop
            break;
        }

        // Consume the token
        auto next_result = tokenizer.next();
        if (!next_result.ok() || !next_result.value) break;

        const auto& gv = tokenizer.current();
        double val = gv.as_double();

        if (var_name == "$ACADVER") {
            if (gv.code == 1) {
                scene.drawing_info().acad_version = gv.value;
            }
        } else if (var_name == "$EXTMIN") {
            if (gv.code >= 10 && gv.code < 40) {
                extmin = read_point_3d(extmin, gv.code, val);
                scene.drawing_info().extents.min = extmin;
            }
        } else if (var_name == "$EXTMAX") {
            if (gv.code >= 10 && gv.code < 40) {
                extmax = read_point_3d(extmax, gv.code, val);
                scene.drawing_info().extents.max = extmax;
            }
        } else if (var_name == "$INSBASE") {
            if (gv.code >= 10 && gv.code < 40) {
                insbase = read_point_3d(insbase, gv.code, val);
                scene.drawing_info().insertion_base = insbase;
            }
        } else if (var_name == "$TEXTSIZE") {
            if (gv.code == 40) {
                scene.drawing_info().text_size = static_cast<float>(val);
            }
        }
        // All other variables are silently ignored
    }
}

Result DxfHeaderReader::read(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // The caller has consumed "SECTION" / "HEADER".
    // Read group code/value pairs until ENDSEC.

    while (true) {
        auto result = tokenizer.next();
        if (!result.ok() || !result.value) {
            return Result::error(ErrorCode::UnexpectedToken,
                                  "Unexpected EOF in HEADER section");
        }

        const auto& gv = tokenizer.current();

        // Check for ENDSEC
        if (gv.is_section_end()) {
            return Result::success();
        }

        // Group code 9 marks the start of a header variable
        if (gv.code == 9) {
            read_variable(tokenizer, scene);
        }
        // Other codes within header are ignored
    }
}

} // namespace cad
