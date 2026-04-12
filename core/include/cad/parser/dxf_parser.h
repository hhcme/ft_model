#pragma once

#include "cad/cad_errors.h"
#include <string>
#include <functional>
#include <memory>

namespace cad {

class SceneGraph;
class DxfHeaderReader;
class DxfTablesReader;
class DxfBlocksReader;
class DxfEntitiesReader;
class DxfObjectsReader;

// Progress callback for parsing operations
struct ParseProgress {
    int entities_parsed = 0;
    int total_entities_estimate = 0;
    std::string current_section;
    std::string current_entity;
};

using ProgressCallback = std::function<void(const ParseProgress&)>;

// DXF file parser — reads DXF files and populates a SceneGraph
class DxfParser {
public:
    DxfParser();
    ~DxfParser();

    // Non-copyable
    DxfParser(const DxfParser&) = delete;
    DxfParser& operator=(const DxfParser&) = delete;

    // Parse from stream
    Result parse(std::istream& stream, SceneGraph& scene,
                 ProgressCallback progress = nullptr);

    // Parse from file path
    Result parse_file(const std::string& filepath, SceneGraph& scene,
                      ProgressCallback progress = nullptr);

    // Parse from memory buffer
    Result parse_buffer(const uint8_t* data, size_t size, SceneGraph& scene,
                        ProgressCallback progress = nullptr);

    // Last parse statistics
    const ParseProgress& last_progress() const { return m_progress; }

private:
    std::unique_ptr<DxfHeaderReader> m_header_reader;
    std::unique_ptr<DxfTablesReader> m_tables_reader;
    std::unique_ptr<DxfBlocksReader> m_blocks_reader;
    std::unique_ptr<DxfEntitiesReader> m_entities_reader;
    std::unique_ptr<DxfObjectsReader> m_objects_reader;
    ParseProgress m_progress;
};

} // namespace cad
