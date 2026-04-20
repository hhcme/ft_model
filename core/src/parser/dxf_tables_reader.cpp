#include "cad/parser/dxf_tables_reader.h"
#include "cad/scene/scene_graph.h"

namespace cad {

void DxfTablesReader::parse_ltype_table(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Current token is "LTYPE" (from group code 0)
    // Read until next group code 0 (next table entry or ENDTAB)
    Linetype lt;
    std::vector<float> dash_elements; // Temporary storage for dash/gap values from code 49

    while (true) {
        auto peek_result = tokenizer.peek();
        if (!peek_result.ok() || !peek_result.value) break;

        if (tokenizer.peeked().code == 0) break;

        auto next_result = tokenizer.next();
        if (!next_result.ok() || !next_result.value) break;

        const auto& gv = tokenizer.current();
        switch (gv.code) {
            case 2:  // Linetype name
                lt.name = gv.value;
                break;
            case 3:  // Description text
                lt.description = gv.value;
                break;
            case 49: // Dash/gap element value
                dash_elements.push_back(gv.as_float());
                break;
            default:
                break;
        }
    }

    // Store dash array into the LinePattern's dash_array (std::vector<float>)
    if (!dash_elements.empty()) {
        lt.pattern.dash_array = dash_elements;
    }

    scene.add_linetype(std::move(lt));
}

void DxfTablesReader::parse_layer_table(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Current token is "LAYER" (from group code 0)
    Layer layer;

    while (true) {
        auto peek_result = tokenizer.peek();
        if (!peek_result.ok() || !peek_result.value) break;

        if (tokenizer.peeked().code == 0) break;

        auto next_result = tokenizer.next();
        if (!next_result.ok() || !next_result.value) break;

        const auto& gv = tokenizer.current();
        switch (gv.code) {
            case 2:  // Layer name
                layer.name = gv.value;
                break;
            case 62: // Color number (negative if layer is off)
                {
                    int color_val = gv.as_int();
                    if (color_val < 0) {
                        layer.is_off = true;
                        color_val = -color_val;
                    }
                    layer.color = Color::from_aci(color_val);
                }
                break;
            case 70: // Standard flags
                {
                    int flags = gv.as_int();
                    layer.is_frozen = (flags & 0x01) != 0; // Bit 1 = frozen
                    if (flags & 0x04) layer.is_off = true; // Bit 4 = off
                    layer.is_locked = (flags & 0x08) != 0; // Bit 8 = locked
                }
                break;
            case 6:  // Linetype name reference (resolved later)
                {
                    int32_t lt = scene.find_linetype(gv.value);
                    if (lt >= 0) layer.linetype_index = lt;
                }
                break;
            case 290: // Plot flag
                layer.plot_enabled = gv.as_int() != 0;
                break;
            case 370: // Lineweight in hundredths of mm
                layer.lineweight = static_cast<float>(gv.as_int()) / 100.0f;
                break;
            default:
                break;
        }
    }

    scene.add_layer(std::move(layer));
}

void DxfTablesReader::parse_style_table(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Current token is "STYLE" (from group code 0)
    TextStyle style;

    while (true) {
        auto peek_result = tokenizer.peek();
        if (!peek_result.ok() || !peek_result.value) break;

        if (tokenizer.peeked().code == 0) break;

        auto next_result = tokenizer.next();
        if (!next_result.ok() || !next_result.value) break;

        const auto& gv = tokenizer.current();
        switch (gv.code) {
            case 2:  // Style name
                style.name = gv.value;
                break;
            case 3:  // Font file name (primary)
                style.font_file = gv.value;
                break;
            case 4:  // Bigfont file (ignored)
                break;
            case 40: // Fixed text height (0 = not fixed)
                style.fixed_height = gv.as_float();
                break;
            case 41: // Width factor
                style.width_factor = gv.as_float();
                break;
            case 50: // Oblique angle (ignored for now)
                break;
            case 71: // Text generation flags
                break;
            case 42: // Last height used
                break;
            default:
                break;
        }
    }

    scene.add_text_style(std::move(style));
}

void DxfTablesReader::parse_vport_table(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // Current token is "VPORT" (from group code 0)
    Viewport vp;

    float lower_left_x = 0.0f, lower_left_y = 0.0f;
    float upper_right_x = 0.0f, upper_right_y = 0.0f;

    while (true) {
        auto peek_result = tokenizer.peek();
        if (!peek_result.ok() || !peek_result.value) break;

        if (tokenizer.peeked().code == 0) break;

        auto next_result = tokenizer.next();
        if (!next_result.ok() || !next_result.value) break;

        const auto& gv = tokenizer.current();
        switch (gv.code) {
            case 2:  // Viewport name
                vp.name = gv.value;
                break;
            case 10: // Lower-left corner X
                lower_left_x = gv.as_float();
                break;
            case 20: // Lower-left corner Y
                lower_left_y = gv.as_float();
                break;
            case 11: // Upper-right corner X
                upper_right_x = gv.as_float();
                break;
            case 21: // Upper-right corner Y
                upper_right_y = gv.as_float();
                break;
            case 12: // View center X
                break;
            case 22: // View center Y
                break;
            case 40: // View height
                vp.height = gv.as_float();
                break;
            case 41: // View aspect ratio
                if (vp.height > 0) {
                    vp.width = vp.height * gv.as_float();
                }
                break;
            default:
                break;
        }
    }

    // Compute center from lower-left and upper-right if available
    vp.center = Vec3(
        (lower_left_x + upper_right_x) * 0.5f,
        (lower_left_y + upper_right_y) * 0.5f,
        0.0f
    );
    vp.paper_center = vp.center;
    if (vp.width == 0.0f) vp.width = upper_right_x - lower_left_x;
    if (vp.height == 0.0f) vp.height = upper_right_y - lower_left_y;
    vp.paper_width = vp.width;
    vp.paper_height = vp.height;

    scene.add_viewport(std::move(vp));
}

void DxfTablesReader::skip_table(DxfTokenizer& tokenizer) {
    // Skip all entries until we find group code 0 with value "ENDTAB"
    while (true) {
        auto result = tokenizer.next();
        if (!result.ok() || !result.value) break;

        const auto& gv = tokenizer.current();
        if (gv.code == 0 && gv.value == "ENDTAB") {
            return;
        }
    }
}

Result DxfTablesReader::read(DxfTokenizer& tokenizer, SceneGraph& scene) {
    // The caller has consumed "SECTION" / "TABLES".
    // We now read TABLE...ENDTAB blocks until ENDSEC.

    while (true) {
        auto result = tokenizer.next();
        if (!result.ok() || !result.value) {
            return Result::error(ErrorCode::UnexpectedToken,
                                  "Unexpected EOF in TABLES section");
        }

        const auto& gv = tokenizer.current();

        if (gv.is_section_end()) {
            return Result::success();
        }

        if (gv.code == 0 && gv.value == "TABLE") {
            // Read TABLE header: skip all fields until we see code==0
            // (which will be the first entry type or ENDTAB)
            std::string table_type;
            while (true) {
                auto inner = tokenizer.next();
                if (!inner.ok() || !inner.value) {
                    return Result::error(ErrorCode::UnexpectedToken,
                                          "Unexpected EOF reading table header");
                }
                const auto& inner_gv = tokenizer.current();
                if (inner_gv.code == 2 && table_type.empty()) {
                    table_type = inner_gv.value;
                }
                if (inner_gv.code == 0) {
                    // This is the first entry or ENDTAB — don't consume it,
                    // fall through to the entry loop below.
                    break;
                }
                // Skip other header fields (code 5 handle, code 70 count, etc.)
            }

            // Now read entries until ENDTAB.
            // The current token is already at code==0 (first entry or ENDTAB).
            while (true) {
                const auto& entry_gv = tokenizer.current();

                if (entry_gv.code == 0 && entry_gv.value == "ENDTAB") {
                    // Consume ENDTAB and break
                    break;
                }

                if (entry_gv.code == 0) {
                    const std::string& entry_type = entry_gv.value;
                    if (entry_type == "LTYPE") {
                        parse_ltype_table(tokenizer, scene);
                    } else if (entry_type == "LAYER") {
                        parse_layer_table(tokenizer, scene);
                    } else if (entry_type == "STYLE") {
                        parse_style_table(tokenizer, scene);
                    } else if (entry_type == "VPORT") {
                        parse_vport_table(tokenizer, scene);
                    } else {
                        // VIEW, UCS, APPID, DIMSTYLE, etc. — skip entry
                        skip_unknown_entity(tokenizer);
                    }
                }

                // After parse_xxx returns, current() is at code==0 (next entry or ENDTAB).
                // But skip_unknown_entity might leave us at ENDTAB or next entry.
                // Verify we have a valid current token:
                if (tokenizer.current().code == 0 &&
                    tokenizer.current().value == "ENDTAB") {
                    break;
                }

                // Safety: if somehow we're not at code==0, advance
                if (tokenizer.current().code != 0) {
                    auto next = tokenizer.next();
                    if (!next.ok() || !next.value) {
                        return Result::error(ErrorCode::UnexpectedToken,
                                              "Unexpected EOF in table entries");
                    }
                }
            }
        }
    }
}

} // namespace cad
