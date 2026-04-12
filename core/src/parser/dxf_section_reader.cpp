#include "cad/parser/dxf_section_reader.h"

namespace cad {

void DxfSectionReader::skip_unknown_entity(DxfTokenizer& tokenizer) {
    // Read until we find group code 0 (next entity/ENDSEC)
    while (true) {
        auto result = tokenizer.next();
        if (!result.ok() || !result.value) break;
        if (tokenizer.current().code == 0) break;
    }
}

void DxfSectionReader::skip_to_code_zero(DxfTokenizer& tokenizer) {
    skip_unknown_entity(tokenizer);
}

} // namespace cad
