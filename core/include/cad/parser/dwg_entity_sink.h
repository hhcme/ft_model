#pragma once

#include "cad/cad_types.h"
#include "cad/scene/entity.h"
#include "cad/scene/layer.h"
#include "cad/scene/linetype.h"
#include "cad/scene/block.h"
#include "cad/scene/text_style.h"
#include "cad/scene/viewport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cad {

struct Layout;
struct SceneDiagnostic;
struct DrawingMetadata;

// ============================================================
// EntitySink — abstract output interface for DWG/DXF parsers
//
// Decouples parser code from SceneGraph.  Parsers write into an
// EntitySink; SceneGraph implements this interface.  This allows
// parser modules to be extracted without depending on SceneGraph
// directly, and enables testing with mock sinks.
// ============================================================
class EntitySink {
public:
    virtual ~EntitySink() = default;

    // ---- Entity management ----
    virtual int32_t add_entity(EntityVariant entity) = 0;
    virtual int32_t add_polyline_vertices(const Vec3* vertices, size_t count) = 0;
    virtual std::vector<Vec3>& vertex_buffer() = 0;
    virtual std::vector<EntityVariant>& entities() = 0;
    virtual const std::vector<EntityVariant>& entities() const = 0;
    virtual size_t total_entity_count() const = 0;

    // ---- Table management ----
    virtual int32_t add_layer(Layer layer) = 0;
    virtual int32_t add_linetype(Linetype lt) = 0;
    virtual int32_t add_text_style(TextStyle style) = 0;
    virtual int32_t add_block(Block block) = 0;
    virtual int32_t add_viewport(Viewport vp) = 0;
    virtual int32_t add_layout(Layout layout) = 0;
    virtual void add_diagnostic(SceneDiagnostic diagnostic) = 0;

    // ---- Lookups ----
    virtual int32_t find_or_add_layer(const std::string& name) = 0;
    virtual bool update_layer(int32_t index, const Layer& layer) = 0;
    virtual int32_t find_block(const std::string& name) const = 0;
    virtual int32_t find_linetype(const std::string& name) const = 0;
    virtual int32_t find_text_style(const std::string& name) const = 0;

    // ---- Metadata ----
    virtual DrawingMetadata& drawing_info() = 0;

    // ---- Pre-allocation ----
    virtual void reserve(size_t entity_count, size_t vertex_count = 0) = 0;

    // ---- Table access (mutable for post-processing) ----
    virtual std::vector<Block>& blocks() = 0;
    virtual const std::vector<Block>& blocks() const = 0;
    virtual std::vector<Layout>& layouts() = 0;
    virtual const std::vector<Layout>& layouts() const = 0;
    virtual std::vector<Viewport>& viewports() = 0;
    virtual const std::vector<Viewport>& viewports() const = 0;
    virtual const std::vector<Layer>& layers() const = 0;
    virtual const std::vector<SceneDiagnostic>& diagnostics() const = 0;
};

} // namespace cad
