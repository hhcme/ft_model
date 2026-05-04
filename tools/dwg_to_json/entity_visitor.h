/*
 * entity_visitor.h — HOOPS Exchange SDK entity tree visitor.
 *
 * Recursively traverses the HOOPS entity tree and collects entity data
 * for JSON export.
 */

#ifndef ENTITY_VISITOR_H
#define ENTITY_VISITOR_H

#include <A3DSDKIncludes.h>
#include <A3DSDKStructure.h>
#include <A3DSDKRootEntities.h>
#include <A3DSDKGeometry.h>
#include <A3DSDKGeometryCrv.h>
#include <A3DSDKMarkup.h>
#include <A3DSDKMarkupDimension.h>
#include <A3DSDKDrawing.h>
#include <A3DSDKEnums.h>
#include <A3DSDKTypes.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

// Forward declarations
class JsonWriter;

// Entity counts by type
struct EntityCounts {
    std::map<std::string, int> counts;

    void increment(const std::string& type) {
        counts[type]++;
    }

    int total() const {
        int sum = 0;
        for (const auto& p : counts) sum += p.second;
        return sum;
    }
};

// Layer info
struct LayerInfo {
    std::string name;
    int color_r, color_g, color_b;
    bool frozen;
    bool off;
};

// Block info
struct BlockInfo {
    std::string name;
    int entity_count;
};

// Insert instance info (recovered from HOOPS flattened blocks)
struct InsertInstance {
    std::string block_name;
    double insertion_point[3];
    double scale[3];
    double rotation;
    std::string layer;
};

// Individual entity data
struct EntityData {
    std::string type;
    std::string layer;
    int color_r, color_g, color_b;
    std::map<std::string, std::string> props;
    std::string space;  // "model" or "drawing"
    int tree_depth = 0; // depth in the HOOPS entity tree
};

// Scene tree node for hierarchical output
struct TreeNode {
    std::string name;
    std::string type;  // "ProductOccurrence", "PartDefinition", "DrawingModel", etc.
    int depth = 0;
    std::vector<int> child_indices;   // indices into the tree_nodes vector
    std::vector<int> entity_indices;  // indices into the entities vector
    double bbox_min[3] = {0, 0, 0};
    double bbox_max[3] = {0, 0, 0};
    bool has_bbox = false;
};

// Main visitor class
class EntityVisitor {
public:
    EntityVisitor();

    void traverse(A3DAsmModelFile* model_file);

    const EntityCounts& entity_counts() const { return m_entity_counts; }
    int total_entities() const { return m_entity_counts.total(); }
    const std::vector<LayerInfo>& layers() const { return m_layers; }
    const std::vector<BlockInfo>& blocks() const { return m_blocks; }
    const std::vector<EntityData>& entities() const { return m_entities; }
    const std::vector<TreeNode>& tree_nodes() const { return m_tree_nodes; }
    const std::vector<InsertInstance>& insert_instances() const { return m_insert_instances; }
    int tree_root_index() const { return m_tree_root; }

    void write_json(JsonWriter& writer, const char* source_file);

private:
    void traverse_product_occurrence(A3DAsmProductOccurrence* po, int depth);
    void traverse_part_definition(A3DAsmPartDefinition* part, int depth);
    void traverse_drawing_model(A3DDrawingModel* drawing_model, int depth);
    void traverse_drawing_sheet(A3DDrawingSheet* sheet, int depth);
    void traverse_drawing_view(A3DDrawingView* view, int depth);
    void traverse_drawing_block(A3DDrawingBlock* block, int depth);

    void visit_representation_item(A3DRiRepresentationItem* ri);
    void visit_drawing_entity(A3DDrawingEntity* entity);
    void visit_markup(A3DMkpMarkup* markup);
    void visit_annotation_entity(A3DMkpAnnotationEntity* entity);

    void extract_curve_geometry(A3DRiRepresentationItem* ri, EntityData& data);
    void extract_polywire_geometry(A3DRiRepresentationItem* ri, EntityData& data);
    void extract_pointset_geometry(A3DRiRepresentationItem* ri, EntityData& data);

    std::string get_type_name(int type);

    // Layer ID -> name map (built from A3DAsmLayerData::m_uiLayerID)
    std::map<A3DUns32, std::string> m_layer_id_to_name;
    // Current layer context during traversal
    std::string m_current_layer;

    EntityCounts m_entity_counts;
    std::vector<LayerInfo> m_layers;
    std::vector<BlockInfo> m_blocks;
    std::vector<EntityData> m_entities;
    std::map<std::string, int> m_layer_index;
    int m_default_layer_idx;

    // Scene tree tracking
    std::vector<TreeNode> m_tree_nodes;
    int m_tree_root = -1;
    int m_current_tree_node = -1;  // current parent node during traversal

    // Block instance tracking (recovered from HOOPS flattened blocks)
    std::vector<InsertInstance> m_insert_instances;

    // Per-block entity counts for hierarchy validation
    std::map<std::string, int> m_block_entity_counts;

    // Tree building helpers
    int push_tree_node(const std::string& name, const std::string& type);
    void pop_tree_node();
    void add_entity_to_current_tree(int entity_index);

    // Helper: resolve layer name for a representation item from its graphics data
    std::string get_layer_from_graphics(A3DRiRepresentationItem* ri);
};

#endif // ENTITY_VISITOR_H
