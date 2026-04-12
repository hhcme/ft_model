#include "cad/parser/dxf_parser.h"
#include "cad/parser/dxf_tokenizer.h"
#include "cad/parser/dxf_header_reader.h"
#include "cad/parser/dxf_tables_reader.h"
#include "cad/parser/dxf_blocks_reader.h"
#include "cad/parser/dxf_entities_reader.h"
#include "cad/parser/dxf_objects_reader.h"
#include "cad/scene/scene_graph.h"

#include <fstream>
#include <sstream>

namespace cad {

DxfParser::DxfParser()
    : m_header_reader(std::make_unique<DxfHeaderReader>())
    , m_tables_reader(std::make_unique<DxfTablesReader>())
    , m_blocks_reader(std::make_unique<DxfBlocksReader>())
    , m_entities_reader(std::make_unique<DxfEntitiesReader>())
    , m_objects_reader(std::make_unique<DxfObjectsReader>())
{
}

DxfParser::~DxfParser() = default;

Result DxfParser::parse(std::istream& stream, SceneGraph& scene, ProgressCallback progress) {
    m_progress = ParseProgress{};
    DxfTokenizer tokenizer(stream);

    // Main parsing loop — read through DXF sections
    while (true) {
        auto next_result = tokenizer.next();
        if (!next_result.ok()) {
            return Result::error(ErrorCode::ParseError, next_result.result.message);
        }
        if (!next_result.value) {
            // EOF — done
            break;
        }

        const auto& current = tokenizer.current();

        if (current.is_eof()) {
            break;
        }

        if (current.is_section_start()) {
            // Read section name (next pair should be code 2 with section name)
            auto name_result = tokenizer.next();
            if (!name_result.ok() || !name_result.value) {
                return Result::error(ErrorCode::UnexpectedToken,
                    "Expected section name after SECTION marker");
            }

            const std::string& section_name = tokenizer.current().value;
            m_progress.current_section = section_name;

            if (progress) {
                progress(m_progress);
            }

            Result section_result = Result::success();

            if (section_name == "HEADER") {
                section_result = m_header_reader->read(tokenizer, scene);
            } else if (section_name == "TABLES") {
                section_result = m_tables_reader->read(tokenizer, scene);
            } else if (section_name == "BLOCKS") {
                section_result = m_blocks_reader->read(tokenizer, scene);
            } else if (section_name == "ENTITIES") {
                section_result = m_entities_reader->read(tokenizer, scene);
            } else if (section_name == "OBJECTS") {
                section_result = m_objects_reader->read(tokenizer, scene);
            } else {
                // Unknown section — skip to ENDSEC
                while (true) {
                    auto skip = tokenizer.next();
                    if (!skip.ok() || !skip.value) break;
                    if (tokenizer.current().is_section_end()) break;
                }
                continue;
            }

            if (!section_result.ok()) {
                return section_result;
            }

        } else if (current.code == 999) {
            // Comment — skip
            continue;
        } else if (current.code == 0 && current.value == "EOF") {
            break;
        }
        // Other top-level tokens are unexpected but we tolerate them
    }

    // Rebuild spatial index after all entities are loaded
    scene.rebuild_spatial_index();

    return Result::success();
}

Result DxfParser::parse_file(const std::string& filepath, SceneGraph& scene,
                              ProgressCallback progress) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return Result::error(ErrorCode::FileNotFound, "Cannot open file: " + filepath);
    }
    return parse(file, scene, progress);
}

Result DxfParser::parse_buffer(const uint8_t* data, size_t size, SceneGraph& scene,
                                ProgressCallback progress) {
    if (!data || size == 0) {
        return Result::error(ErrorCode::InvalidArgument, "Empty buffer");
    }
    std::string content(reinterpret_cast<const char*>(data), size);
    std::istringstream stream(content);
    return parse(stream, scene, progress);
}

} // namespace cad
