/*
 * entity_visitor.cpp — HOOPS Exchange SDK entity tree visitor implementation.
 *
 * Simplified traversal: only count and list entities by type.
 */

#include "entity_visitor.h"
#include "json_writer.h"
#include <A3DSDKGeometry.h>
#include <functional>
#include <A3DSDKGeometryCrv.h>
#include <A3DSDKMarkup.h>
#include <A3DSDKMarkupDimension.h>
#include <A3DSDKDrawing.h>
#include <A3DSDKGraphics.h>
#include <A3DSDKStructure.h>
#include <A3DSDKTopology.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>

EntityVisitor::EntityVisitor() : m_default_layer_idx(-1), m_current_layer("0") {
    LayerInfo default_layer;
    default_layer.name = "0";
    default_layer.color_r = 7;
    default_layer.color_g = 7;
    default_layer.color_b = 7;
    default_layer.frozen = false;
    default_layer.off = false;
    m_layers.push_back(default_layer);
    m_layer_index["0"] = 0;
    m_default_layer_idx = 0;
    m_layer_id_to_name[0] = "0";  // Layer ID 0 = "0"

    // Create tree root
    m_tree_root = push_tree_node("ModelFile", "ModelFile");
}

int EntityVisitor::push_tree_node(const std::string& name, const std::string& type) {
    int idx = static_cast<int>(m_tree_nodes.size());
    m_tree_nodes.push_back({});
    auto& node = m_tree_nodes.back();
    node.name = name;
    node.type = type;
    node.depth = (m_current_tree_node >= 0) ? m_tree_nodes[m_current_tree_node].depth + 1 : 0;

    // Register as child of current node
    if (m_current_tree_node >= 0) {
        m_tree_nodes[m_current_tree_node].child_indices.push_back(idx);
    }

    m_current_tree_node = idx;
    return idx;
}

void EntityVisitor::pop_tree_node() {
    if (m_current_tree_node >= 0) {
        // Go to parent — find by scanning for the node that has us as child
        int current = m_current_tree_node;
        for (size_t i = 0; i < m_tree_nodes.size(); i++) {
            for (int ci : m_tree_nodes[i].child_indices) {
                if (ci == current) {
                    m_current_tree_node = static_cast<int>(i);
                    return;
                }
            }
        }
        m_current_tree_node = m_tree_root;  // fallback to root
    }
}

void EntityVisitor::add_entity_to_current_tree(int entity_index) {
    if (m_current_tree_node >= 0) {
        m_tree_nodes[m_current_tree_node].entity_indices.push_back(entity_index);
    }
}

void EntityVisitor::traverse(A3DAsmModelFile* model_file) {
    if (!model_file) return;

    A3DAsmModelFileData model_data;
    A3D_INITIALIZE_A3DAsmModelFileData(model_data);
    A3DAsmModelFileGet(model_file, &model_data);

    printf("Model file: %d product occurrences\n", model_data.m_uiPOccurrencesSize);

    if (model_data.m_ppPOccurrences) {
        for (A3DUns32 i = 0; i < model_data.m_uiPOccurrencesSize; i++) {
            A3DAsmProductOccurrence* po = model_data.m_ppPOccurrences[i];
            if (po) {
                traverse_product_occurrence(po, 0);
            }
        }
    }

    A3DAsmModelFileGet(nullptr, &model_data);
}

std::string EntityVisitor::get_layer_from_graphics(A3DRiRepresentationItem* ri) {
    if (!ri) return "";

    // Representation items support A3DRootBaseWithGraphics interface
    // Cast to A3DRootBaseWithGraphics* to get the associated graphics
    const A3DRootBaseWithGraphics* base = (const A3DRootBaseWithGraphics*)ri;
    A3DRootBaseWithGraphicsData rbwgData;
    A3D_INITIALIZE_A3DRootBaseWithGraphicsData(rbwgData);
    if (A3DRootBaseWithGraphicsGet(base, &rbwgData) != A3D_SUCCESS) {
        return "";
    }

    std::string result;
    if (rbwgData.m_pGraphics) {
        A3DGraphicsData gdata;
        A3D_INITIALIZE_A3DGraphicsData(gdata);
        if (A3DGraphicsGet(rbwgData.m_pGraphics, &gdata) == A3D_SUCCESS) {
            // A3D_DEFAULT_LAYER = ((A3DUns16)-1) = 65535
            if (gdata.m_uiLayerIndex != (A3DUns32)A3D_DEFAULT_LAYER) {
                auto it = m_layer_id_to_name.find(gdata.m_uiLayerIndex);
                if (it != m_layer_id_to_name.end()) {
                    result = it->second;
                }
            }
            A3DGraphicsGet(nullptr, &gdata);
        }
    }
    A3DRootBaseWithGraphicsGet(nullptr, &rbwgData);
    return result;
}

void EntityVisitor::traverse_product_occurrence(A3DAsmProductOccurrence* po, int depth) {
    if (!po) return;

    A3DAsmProductOccurrenceData po_data;
    A3D_INITIALIZE_A3DAsmProductOccurrenceData(po_data);
    A3DAsmProductOccurrenceGet(po, &po_data);

    // Get PO name for tree node
    std::string po_name = "ProductOccurrence";
    if (po_data.m_pPart && depth == 0) {
        po_name = "Model";
    }

    // Push tree node for this product occurrence
    int saved_tree_node = m_current_tree_node;
    int tree_idx = push_tree_node(po_name, "ProductOccurrence");

    // Save current layer context
    std::string saved_layer = m_current_layer;

    // Collect layers from this product occurrence and build ID->name map
    if (po_data.m_ppLayers) {
        for (A3DUns32 i = 0; i < po_data.m_uiLayersSize; i++) {
            A3DAsmLayer* layer = po_data.m_ppLayers[i];
            if (layer) {
                A3DAsmLayerData layer_data;
                A3D_INITIALIZE_A3DAsmLayerData(layer_data);
                A3DAsmLayerGet(layer, &layer_data);
                LayerInfo info;
                info.name = layer_data.m_pcLayerName ? layer_data.m_pcLayerName : "0";
                info.color_r = 7;
                info.color_g = 7;
                info.color_b = 7;
                info.frozen = (layer_data.m_eDisplayStatus != 0);
                info.off = (layer_data.m_eDisplayStatus == 2);

                // Build ID -> name map
                m_layer_id_to_name[layer_data.m_uiLayerID] = info.name;

                if (m_layer_index.find(info.name) == m_layer_index.end()) {
                    int idx = (int)m_layers.size();
                    m_layer_index[info.name] = idx;
                    m_layers.push_back(info);
                }

                // Use first layer as current context (best effort)
                if (i == 0) {
                    m_current_layer = info.name;
                }
                A3DAsmLayerGet(nullptr, &layer_data);
            }
        }
    }

    if (po_data.m_ppPOccurrences) {
        for (A3DUns32 i = 0; i < po_data.m_uiPOccurrencesSize; i++) {
            traverse_product_occurrence(po_data.m_ppPOccurrences[i], depth + 1);
        }
    }

    if (po_data.m_pPart) {
        traverse_part_definition(po_data.m_pPart, depth + 1);
    }

    if (po_data.m_ppAnnotations && po_data.m_uiAnnotationsSize > 0) {
        for (A3DUns32 i = 0; i < po_data.m_uiAnnotationsSize; i++) {
            visit_annotation_entity(po_data.m_ppAnnotations[i]);
        }
    }

    // Restore context
    m_current_layer = saved_layer;
    m_current_tree_node = saved_tree_node;

    A3DAsmProductOccurrenceGet(nullptr, &po_data);
}

void EntityVisitor::traverse_part_definition(A3DAsmPartDefinition* part, int depth) {
    if (!part) return;

    A3DAsmPartDefinitionData part_data;
    A3D_INITIALIZE_A3DAsmPartDefinitionData(part_data);
    A3DAsmPartDefinitionGet(part, &part_data);

    // Push tree node for part definition
    int saved_tree_node = m_current_tree_node;
    push_tree_node("PartDefinition", "PartDefinition");

    int rep_item_count = 0;
    if (part_data.m_ppRepItems) {
        for (A3DUns32 i = 0; i < part_data.m_uiRepItemsSize; i++) {
            A3DRiRepresentationItem* ri = part_data.m_ppRepItems[i];
            if (ri) {
                rep_item_count++;
                visit_representation_item(ri);
            }
        }
    }
    if (rep_item_count > 0) {
        printf("  [DEBUG] PartDefinition: %d RepItems\n", rep_item_count);
    }

    if (part_data.m_ppAnnotations) {
        for (A3DUns32 i = 0; i < part_data.m_uiAnnotationsSize; i++) {
            visit_annotation_entity(part_data.m_ppAnnotations[i]);
        }
    }

    if (part_data.m_ppDrawingModels) {
        for (A3DUns32 i = 0; i < part_data.m_uiDrawingModelsSize; i++) {
            traverse_drawing_model(part_data.m_ppDrawingModels[i], depth + 1);
        }
    }

    // Restore tree node
    m_current_tree_node = saved_tree_node;

    A3DAsmPartDefinitionGet(nullptr, &part_data);
}

void EntityVisitor::traverse_drawing_model(A3DDrawingModel* drawing_model, int depth) {
    if (!drawing_model) return;

    A3DDrawingModelData dm_data;
    A3D_INITIALIZE_A3DDrawingModelData(dm_data);
    A3DDrawingModelGet(drawing_model, &dm_data);

    // Push tree node for drawing model (Paper Space)
    int saved_tree_node = m_current_tree_node;
    push_tree_node("DrawingModel", "DrawingModel");

    if (dm_data.m_ppDrwSheets) {
        for (A3DUns32 i = 0; i < dm_data.m_uiDrwSheetsSize; i++) {
            traverse_drawing_sheet(dm_data.m_ppDrwSheets[i], depth + 1);
        }
    }

    m_current_tree_node = saved_tree_node;
    A3DDrawingModelGet(nullptr, &dm_data);
}

void EntityVisitor::traverse_drawing_sheet(A3DDrawingSheet* sheet, int depth) {
    if (!sheet) return;

    A3DDrawingSheetData sheet_data;
    A3D_INITIALIZE_A3DDrawingSheetData(sheet_data);
    A3DDrawingSheetGet(sheet, &sheet_data);

    int saved_tree_node = m_current_tree_node;
    push_tree_node("Sheet", "DrawingSheet");

    if (sheet_data.m_ppDrwViews) {
        for (A3DUns32 i = 0; i < sheet_data.m_uiDrwViewsSize; i++) {
            traverse_drawing_view(sheet_data.m_ppDrwViews[i], depth + 1);
        }
    }

    if (sheet_data.m_ppDrwBlocks) {
        for (A3DUns32 i = 0; i < sheet_data.m_uiDrwBlocksSize; i++) {
            traverse_drawing_block(sheet_data.m_ppDrwBlocks[i], depth + 1);
        }
    }

    m_current_tree_node = saved_tree_node;
    A3DDrawingSheetGet(nullptr, &sheet_data);
}

void EntityVisitor::traverse_drawing_view(A3DDrawingView* view, int depth) {
    if (!view) return;

    A3DDrawingViewData view_data;
    A3D_INITIALIZE_A3DDrawingViewData(view_data);
    A3DDrawingViewGet(view, &view_data);

    int saved_tree_node = m_current_tree_node;
    push_tree_node("View", "DrawingView");

    if (view_data.m_ppDrwBlocks) {
        for (A3DUns32 i = 0; i < view_data.m_uiDrwBlocksSize; i++) {
            traverse_drawing_block(view_data.m_ppDrwBlocks[i], depth + 1);
        }
    }

    m_current_tree_node = saved_tree_node;
    A3DDrawingViewGet(nullptr, &view_data);
}

void EntityVisitor::traverse_drawing_block(A3DDrawingBlock* block, int depth) {
    if (!block) return;

    A3DEEntityType block_type;
    A3DEntityGetType(block, &block_type);

    int saved_tree_node = m_current_tree_node;
    const char* type_name = (block_type == kA3DTypeDrawingBlockBasic) ? "DrawingBlockBasic" : "DrawingBlockOperator";
    push_tree_node("Block", type_name);

    if (block_type == kA3DTypeDrawingBlockBasic) {
        A3DDrawingBlockBasicData block_data;
        A3D_INITIALIZE_A3DDrawingBlockBasicData(block_data);
        A3DDrawingBlockBasicGet(block, &block_data);

        int entity_count_before = static_cast<int>(m_entities.size());

        if (block_data.m_ppDrwEntities) {
            for (A3DUns32 i = 0; i < block_data.m_uiDrwEntitiesSize; i++) {
                visit_drawing_entity(block_data.m_ppDrwEntities[i]);
            }
        }

        if (block_data.m_ppMarkups) {
            for (A3DUns32 i = 0; i < block_data.m_uiMarkupsSize; i++) {
                visit_markup(block_data.m_ppMarkups[i]);
            }
        }

        if (block_data.m_ppDrwBlocks) {
            for (A3DUns32 i = 0; i < block_data.m_uiDrwBlocksSize; i++) {
                traverse_drawing_block(block_data.m_ppDrwBlocks[i], depth + 1);
            }
        }

        // Track block entity count for structural comparison
        int block_entity_count = static_cast<int>(m_entities.size()) - entity_count_before;
        if (block_entity_count > 0) {
            // Use the current tree node name as block identifier
            if (m_current_tree_node >= 0 && m_current_tree_node < static_cast<int>(m_tree_nodes.size())) {
                const std::string& block_name = m_tree_nodes[m_current_tree_node].name;
                m_block_entity_counts[block_name] += block_entity_count;
            }
        }
    }

    m_current_tree_node = saved_tree_node;
}

void EntityVisitor::visit_representation_item(A3DRiRepresentationItem* ri) {
    if (!ri) return;

    A3DEEntityType type;
    A3DEntityGetType(ri, &type);

    std::string entity_type = get_type_name(type);

    // For RiSet, traverse into child representation items
    if (type == kA3DTypeRiSet) {
        A3DRiSetData set_data;
        A3D_INITIALIZE_A3DRiSetData(set_data);
        if (A3DRiSetGet(ri, &set_data) == A3D_SUCCESS && set_data.m_ppRepItems) {
            for (A3DUns32 i = 0; i < set_data.m_uiRepItemsSize; i++) {
                if (set_data.m_ppRepItems[i]) {
                    visit_representation_item(set_data.m_ppRepItems[i]);
                }
            }
        }
        A3DRiSetGet(nullptr, &set_data);
        return;
    }

    // Get layer from graphics data (A3DGraphicsData::m_uiLayerIndex)
    std::string layer_name = get_layer_from_graphics(ri);
    if (layer_name.empty()) {
        layer_name = m_current_layer;
    }
    if (layer_name.empty()) {
        layer_name = "0";
    }

    EntityData data;
    data.type = entity_type;
    data.layer = layer_name;
    data.color_r = data.color_g = data.color_b = 7;
    data.space = "model";  // Representation items are Model Space

    // Extract geometry based on representation item type
    if (type == kA3DTypeRiCurve) {
        extract_curve_geometry(ri, data);
        entity_type = data.type;
    } else if (type == kA3DTypeRiPolyWire) {
        extract_polywire_geometry(ri, data);
        entity_type = data.type;
    } else if (type == kA3DTypeRiPointSet) {
        extract_pointset_geometry(ri, data);
        entity_type = data.type;
    }

    m_entity_counts.increment(entity_type);
    data.type = entity_type;
    data.tree_depth = (m_current_tree_node >= 0) ? m_tree_nodes[m_current_tree_node].depth : 0;
    int entity_idx = static_cast<int>(m_entities.size());
    m_entities.push_back(data);
    add_entity_to_current_tree(entity_idx);
}

void EntityVisitor::visit_drawing_entity(A3DDrawingEntity* entity) {
    if (!entity) return;

    A3DEEntityType type;
    A3DEntityGetType(entity, &type);

    // Debug: print drawing entity type
    static int drawing_count = 0;
    drawing_count++;
    if (drawing_count <= 20) {
        const char* type_name = nullptr;
        switch(type) {
            case kA3DTypeDrawingCurve: type_name = "DrawingCurve"; break;
            case kA3DTypeDrawingVertices: type_name = "DrawingVertices"; break;
            case kA3DTypeDrawingFilledArea: type_name = "DrawingFilledArea"; break;
            default: type_name = "Other"; break;
        }
        printf("    [DEBUG] DrawingEntity #%d: type=%d (%s)\n", drawing_count, type, type_name);
    }

    EntityData data;
    data.type = get_type_name(type);
    data.layer = "0";
    data.color_r = data.color_g = data.color_b = 7;
    data.space = "drawing";  // Drawing entities are Layout/Paper Space

    m_entity_counts.increment(data.type);
    data.tree_depth = (m_current_tree_node >= 0) ? m_tree_nodes[m_current_tree_node].depth : 0;
    int entity_idx = static_cast<int>(m_entities.size());
    m_entities.push_back(data);
    add_entity_to_current_tree(entity_idx);
}

void EntityVisitor::visit_markup(A3DMkpMarkup* markup) {
    if (!markup) return;

    A3DEEntityType type;
    A3DEntityGetType(markup, &type);

    EntityData data;
    data.type = get_type_name(type);
    data.layer = "0";
    data.color_r = data.color_g = data.color_b = 7;
    data.space = "drawing";  // Markups are usually Drawing/Layout space

    m_entity_counts.increment(data.type);
    data.tree_depth = (m_current_tree_node >= 0) ? m_tree_nodes[m_current_tree_node].depth : 0;
    int entity_idx = static_cast<int>(m_entities.size());
    m_entities.push_back(data);
    add_entity_to_current_tree(entity_idx);
}

void EntityVisitor::visit_annotation_entity(A3DMkpAnnotationEntity* entity) {
    if (!entity) return;

    A3DEEntityType type;
    A3DEntityGetType(entity, &type);

    EntityData data;
    data.type = get_type_name(type);
    data.layer = "0";
    data.color_r = data.color_g = data.color_b = 7;
    data.space = "annotation";

    m_entity_counts.increment(data.type);
    data.tree_depth = (m_current_tree_node >= 0) ? m_tree_nodes[m_current_tree_node].depth : 0;
    int entity_idx = static_cast<int>(m_entities.size());
    m_entities.push_back(data);
    add_entity_to_current_tree(entity_idx);
}

// --- Geometry extraction helpers ---
static std::string format_point(double x, double y, double z = 0.0) {
    char buf[256];
    if (z == 0.0) {
        snprintf(buf, sizeof(buf), "[%.6g, %.6g]", x, y);
    } else {
        snprintf(buf, sizeof(buf), "[%.6g, %.6g, %.6g]", x, y, z);
    }
    return buf;
}

static std::string format_vertices(const A3DVector3dData* pts, A3DUns32 size) {
    std::string result = "[";
    for (A3DUns32 i = 0; i < size; i++) {
        if (i > 0) result += ", ";
        result += format_point(pts[i].m_dX, pts[i].m_dY, pts[i].m_dZ);
    }
    result += "]";
    return result;
}

void EntityVisitor::extract_curve_geometry(A3DRiRepresentationItem* ri, EntityData& data) {
    A3DRiCurveData curve_data;
    A3D_INITIALIZE_A3DRiCurveData(curve_data);
    if (A3DRiCurveGet(ri, &curve_data) != A3D_SUCCESS) {
        printf("    [GEOM] A3DRiCurveGet failed\n");
        A3DRiCurveGet(nullptr, &curve_data);
        return;
    }
    if (!curve_data.m_pBody) {
        printf("    [GEOM] curve_data.m_pBody is NULL\n");
        A3DRiCurveGet(nullptr, &curve_data);
        return;
    }

    A3DTopoSingleWireBodyData wire_data;
    A3D_INITIALIZE_A3DTopoSingleWireBodyData(wire_data);
    if (A3DTopoSingleWireBodyGet(curve_data.m_pBody, &wire_data) != A3D_SUCCESS) {
        printf("    [GEOM] A3DTopoSingleWireBodyGet failed\n");
        A3DRiCurveGet(nullptr, &curve_data);
        A3DTopoSingleWireBodyGet(nullptr, &wire_data);
        return;
    }
    if (!wire_data.m_pWireEdge) {
        printf("    [GEOM] wire_data.m_pWireEdge is NULL\n");
        A3DRiCurveGet(nullptr, &curve_data);
        A3DTopoSingleWireBodyGet(nullptr, &wire_data);
        return;
    }

    A3DTopoWireEdgeData edge_data;
    A3D_INITIALIZE_A3DTopoWireEdgeData(edge_data);
    if (A3DTopoWireEdgeGet(wire_data.m_pWireEdge, &edge_data) != A3D_SUCCESS) {
        printf("    [GEOM] A3DTopoWireEdgeGet failed\n");
        A3DTopoWireEdgeGet(nullptr, &edge_data);
        A3DTopoSingleWireBodyGet(nullptr, &wire_data);
        A3DRiCurveGet(nullptr, &curve_data);
        return;
    }
    if (!edge_data.m_p3dCurve) {
        printf("    [GEOM] edge_data.m_p3dCurve is NULL\n");
        A3DTopoWireEdgeGet(nullptr, &edge_data);
        A3DTopoSingleWireBodyGet(nullptr, &wire_data);
        A3DRiCurveGet(nullptr, &curve_data);
        return;
    }

    A3DEEntityType curve_type;
    A3DEntityGetType(edge_data.m_p3dCurve, &curve_type);

    if (curve_type == kA3DTypeCrvLine) {
        A3DCrvLineData line_data;
        A3D_INITIALIZE_A3DCrvLineData(line_data);
        if (A3DCrvLineGet(edge_data.m_p3dCurve, &line_data) == A3D_SUCCESS) {
            data.type = "LINE";
            double ox = line_data.m_sTrsf.m_sOrigin.m_dX;
            double oy = line_data.m_sTrsf.m_sOrigin.m_dY;
            double oz = line_data.m_sTrsf.m_sOrigin.m_dZ;
            double dx = line_data.m_sTrsf.m_sXVector.m_dX;
            double dy = line_data.m_sTrsf.m_sXVector.m_dY;
            double dz = line_data.m_sTrsf.m_sXVector.m_dZ;
            double t0 = line_data.m_sParam.m_sInterval.m_dMin * line_data.m_sParam.m_dCoeffA + line_data.m_sParam.m_dCoeffB;
            double t1 = line_data.m_sParam.m_sInterval.m_dMax * line_data.m_sParam.m_dCoeffA + line_data.m_sParam.m_dCoeffB;
            data.props["start"] = format_point(ox + t0 * dx, oy + t0 * dy, oz + t0 * dz);
            data.props["end"] = format_point(ox + t1 * dx, oy + t1 * dy, oz + t1 * dz);
            printf("    [GEOM] LINE: start=%s, end=%s\n", data.props["start"].c_str(), data.props["end"].c_str());
        }
        A3DCrvLineGet(nullptr, &line_data);
    } else if (curve_type == kA3DTypeCrvCircle) {
        A3DCrvCircleData circle_data;
        A3D_INITIALIZE_A3DCrvCircleData(circle_data);
        if (A3DCrvCircleGet(edge_data.m_p3dCurve, &circle_data) == A3D_SUCCESS) {
            data.type = "CIRCLE";
            data.props["center"] = format_point(
                circle_data.m_sTrsf.m_sOrigin.m_dX, circle_data.m_sTrsf.m_sOrigin.m_dY, circle_data.m_sTrsf.m_sOrigin.m_dZ);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.6g", circle_data.m_dRadius);
            data.props["radius"] = buf;
            // Use wire edge trim interval for arc detection
            double t0 = edge_data.m_sInterval.m_dMin;
            double t1 = edge_data.m_sInterval.m_dMax;
            double arc_len = fabs(t1 - t0);
            if (arc_len < 6.28 && arc_len > 0.01) {
                data.type = "ARC";
                snprintf(buf, sizeof(buf), "%.6g", t0 * 180.0 / 3.141592653589793);
                data.props["start_angle"] = buf;
                snprintf(buf, sizeof(buf), "%.6g", t1 * 180.0 / 3.141592653589793);
                data.props["end_angle"] = buf;
            }
        }
        A3DCrvCircleGet(nullptr, &circle_data);
    } else if (curve_type == kA3DTypeCrvEllipse) {
        A3DCrvEllipseData ellipse_data;
        A3D_INITIALIZE_A3DCrvEllipseData(ellipse_data);
        if (A3DCrvEllipseGet(edge_data.m_p3dCurve, &ellipse_data) == A3D_SUCCESS) {
            data.type = "ELLIPSE";
            double cx = ellipse_data.m_sTrsf.m_sOrigin.m_dX;
            double cy = ellipse_data.m_sTrsf.m_sOrigin.m_dY;
            double cz = ellipse_data.m_sTrsf.m_sOrigin.m_dZ;
            data.props["center"] = format_point(cx, cy, cz);
            // Major axis endpoint = center + XVector * major_radius
            double mx = ellipse_data.m_sTrsf.m_sXVector.m_dX * ellipse_data.m_dXRadius;
            double my = ellipse_data.m_sTrsf.m_sXVector.m_dY * ellipse_data.m_dXRadius;
            double mz = ellipse_data.m_sTrsf.m_sXVector.m_dZ * ellipse_data.m_dXRadius;
            data.props["major_axis_endpoint"] = format_point(cx + mx, cy + my, cz + mz);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.6g", ellipse_data.m_dXRadius);
            data.props["major_radius"] = buf;
            snprintf(buf, sizeof(buf), "%.6g", ellipse_data.m_dYRadius);
            data.props["minor_radius"] = buf;
            double t0 = edge_data.m_sInterval.m_dMin;
            double t1 = edge_data.m_sInterval.m_dMax;
            snprintf(buf, sizeof(buf), "%.6g", t0 * 180.0 / 3.141592653589793);
            data.props["start_angle"] = buf;
            snprintf(buf, sizeof(buf), "%.6g", t1 * 180.0 / 3.141592653589793);
            data.props["end_angle"] = buf;
        }
        A3DCrvEllipseGet(nullptr, &ellipse_data);
    } else if (curve_type == kA3DTypeCrvNurbs) {
        A3DCrvNurbsData nurbs_data;
        A3D_INITIALIZE_A3DCrvNurbsData(nurbs_data);
        if (A3DCrvNurbsGet(edge_data.m_p3dCurve, &nurbs_data) == A3D_SUCCESS) {
            data.type = "SPLINE";
            char buf[64];
            snprintf(buf, sizeof(buf), "%u", nurbs_data.m_uiDegree);
            data.props["degree"] = buf;
            snprintf(buf, sizeof(buf), "%u", nurbs_data.m_uiCtrlSize);
            data.props["control_point_count"] = buf;
        }
        A3DCrvNurbsGet(nullptr, &nurbs_data);
    } else if (curve_type == kA3DTypeCrvPolyLine) {
        A3DCrvPolyLineData pl_data;
        A3D_INITIALIZE_A3DCrvPolyLineData(pl_data);
        if (A3DCrvPolyLineGet(edge_data.m_p3dCurve, &pl_data) == A3D_SUCCESS) {
            data.type = "LWPOLYLINE";
            char buf[64];
            snprintf(buf, sizeof(buf), "%u", pl_data.m_uiSize);
            data.props["vertex_count"] = buf;
            // Extract actual vertex coordinates
            if (pl_data.m_uiSize > 0 && pl_data.m_pPts) {
                data.props["vertices"] = format_vertices(pl_data.m_pPts, pl_data.m_uiSize);
            }
        }
        A3DCrvPolyLineGet(nullptr, &pl_data);
    }

    A3DTopoWireEdgeGet(nullptr, &edge_data);
    A3DTopoSingleWireBodyGet(nullptr, &wire_data);
    A3DRiCurveGet(nullptr, &curve_data);
}

// PolyWire is accessed via topology API — skip for now
void EntityVisitor::extract_polywire_geometry(A3DRiRepresentationItem* ri, EntityData& data) {
    (void)ri;
    (void)data;
    // A3DRiPolyWireData is a stub; actual geometry is in topology.
    // Skip for simplicity.
}

void EntityVisitor::extract_pointset_geometry(A3DRiRepresentationItem* ri, EntityData& data) {
    A3DRiPointSetData ps_data;
    A3D_INITIALIZE_A3DRiPointSetData(ps_data);
    if (A3DRiPointSetGet(ri, &ps_data) != A3D_SUCCESS) {
        A3DRiPointSetGet(nullptr, &ps_data);
        return;
    }

    if (ps_data.m_uiSize > 0 && ps_data.m_pPts) {
        data.type = "POINT";
        data.props["position"] = format_point(
            ps_data.m_pPts[0].m_dX, ps_data.m_pPts[0].m_dY, ps_data.m_pPts[0].m_dZ);
    }

    A3DRiPointSetGet(nullptr, &ps_data);
}

std::string EntityVisitor::get_type_name(int type) {
    // Map HOOPS entity types to our output types
    if (type == kA3DTypeRiCurve) return "CURVE";
    if (type == kA3DTypeRiPointSet) return "POINT";
    if (type == kA3DTypeRiBrepModel) return "SOLID";
    if (type == kA3DTypeRiPolyBrepModel) return "POLYBREP";
    if (type == kA3DTypeRiPolyWire) return "POLYWIRE";
    if (type == kA3DTypeRiSet) return "RISET";
    if (type == kA3DTypeMkpLeader) return "LEADER";
    if (type == kA3DTypeMkpMarkup) return "MARKUP";
    if (type == kA3DTypeDrawingFilledArea) return "HATCH";
    if (type == kA3DTypeDrawingCurve) return "DRAWING_CURVE";
    if (type == kA3DTypeDrawingVertices) return "VERTEX";
    if (type == kA3DTypeDrawingBlockBasic) return "BLOCK";
    if (type == kA3DTypeDrawingBlockOperator) return "BLOCK_OP";
    if (type == kA3DTypeDrawingEntity) return "DRAWING_ENTITY";
    if (type == kA3DTypeDrawingModel) return "DRAWING_MODEL";
    if (type == kA3DTypeDrawingSheet) return "DRAWING_SHEET";
    if (type == kA3DTypeDrawingView) return "DRAWING_VIEW";
    if (type == kA3DTypeMkpAnnotationItem) return "ANNOTATION";
    if (type == kA3DTypeMkpAnnotationSet) return "ANNOTATION_SET";
    if (type == kA3DTypeMkpAnnotationReference) return "ANNOTATION_REF";
    if (type == kA3DTypeAsmProductOccurrence) return "PRODUCT_OCCURRENCE";
    if (type == kA3DTypeAsmPartDefinition) return "PART_DEFINITION";
    if (type == kA3DTypeMkpView) return "MARKUP_VIEW";
    return "UNKNOWN";
}

static bool parse_point_prop(const std::string& s, double& x, double& y, double& z) {
    // Parse "[x, y, z]" or "[x, y]"
    if (s.empty() || s[0] != '[' || s.back() != ']') return false;
    const char* p = s.c_str() + 1;
    char* end = nullptr;
    x = strtod(p, &end);
    if (end == p) return false;
    p = end;
    while (*p && (*p == ' ' || *p == ',')) p++;
    y = strtod(p, &end);
    if (end == p) return false;
    p = end;
    while (*p && (*p == ' ' || *p == ',')) p++;
    z = strtod(p, &end);
    if (end == p) z = 0.0;
    return true;
}

static void update_bbox(TreeNode& node, double x, double y, double z) {
    if (!node.has_bbox) {
        node.bbox_min[0] = node.bbox_max[0] = x;
        node.bbox_min[1] = node.bbox_max[1] = y;
        node.bbox_min[2] = node.bbox_max[2] = z;
        node.has_bbox = true;
    } else {
        if (x < node.bbox_min[0]) node.bbox_min[0] = x;
        if (y < node.bbox_min[1]) node.bbox_min[1] = y;
        if (z < node.bbox_min[2]) node.bbox_min[2] = z;
        if (x > node.bbox_max[0]) node.bbox_max[0] = x;
        if (y > node.bbox_max[1]) node.bbox_max[1] = y;
        if (z > node.bbox_max[2]) node.bbox_max[2] = z;
    }
}

static void expand_entity_bbox(const EntityData& entity, TreeNode& node) {
    double x, y, z;
    auto it = entity.props.find("start");
    if (it != entity.props.end() && parse_point_prop(it->second, x, y, z)) {
        update_bbox(node, x, y, z);
    }
    it = entity.props.find("end");
    if (it != entity.props.end() && parse_point_prop(it->second, x, y, z)) {
        update_bbox(node, x, y, z);
    }
    it = entity.props.find("center");
    if (it != entity.props.end() && parse_point_prop(it->second, x, y, z)) {
        update_bbox(node, x, y, z);
    }
    it = entity.props.find("position");
    if (it != entity.props.end() && parse_point_prop(it->second, x, y, z)) {
        update_bbox(node, x, y, z);
    }
}

void EntityVisitor::write_json(JsonWriter& writer, const char* source_file) {
    // Populate tree node bounding boxes from entity geometry
    for (auto& node : m_tree_nodes) {
        node.has_bbox = false;
        for (int ei : node.entity_indices) {
            if (ei >= 0 && ei < static_cast<int>(m_entities.size())) {
                expand_entity_bbox(m_entities[ei], node);
            }
        }
        // Merge child bboxes (bottom-up via reverse depth order)
    }
    // Second pass: merge children into parents
    std::vector<std::pair<int, int>> nodes_by_depth;
    for (size_t i = 0; i < m_tree_nodes.size(); ++i) {
        nodes_by_depth.emplace_back(m_tree_nodes[i].depth, static_cast<int>(i));
    }
    std::sort(nodes_by_depth.begin(), nodes_by_depth.end(), std::greater<std::pair<int, int>>());
    for (const auto& dp : nodes_by_depth) {
        int idx = dp.second;
        for (int ci : m_tree_nodes[idx].child_indices) {
            if (m_tree_nodes[ci].has_bbox) {
                if (!m_tree_nodes[idx].has_bbox) {
                    m_tree_nodes[idx].bbox_min[0] = m_tree_nodes[ci].bbox_min[0];
                    m_tree_nodes[idx].bbox_min[1] = m_tree_nodes[ci].bbox_min[1];
                    m_tree_nodes[idx].bbox_min[2] = m_tree_nodes[ci].bbox_min[2];
                    m_tree_nodes[idx].bbox_max[0] = m_tree_nodes[ci].bbox_max[0];
                    m_tree_nodes[idx].bbox_max[1] = m_tree_nodes[ci].bbox_max[1];
                    m_tree_nodes[idx].bbox_max[2] = m_tree_nodes[ci].bbox_max[2];
                    m_tree_nodes[idx].has_bbox = true;
                } else {
                    for (int d = 0; d < 3; ++d) {
                        if (m_tree_nodes[ci].bbox_min[d] < m_tree_nodes[idx].bbox_min[d])
                            m_tree_nodes[idx].bbox_min[d] = m_tree_nodes[ci].bbox_min[d];
                        if (m_tree_nodes[ci].bbox_max[d] > m_tree_nodes[idx].bbox_max[d])
                            m_tree_nodes[idx].bbox_max[d] = m_tree_nodes[ci].bbox_max[d];
                    }
                }
            }
        }
    }

    writer.start_object();
    writer.write_string("source", source_file);
    writer.write_string("generator", "hoops_exchange");

    // Entity counts
    writer.start_object("entity_counts");
    for (const auto& p : m_entity_counts.counts) {
        writer.write_int(p.first.c_str(), p.second);
    }
    writer.end_object();

    writer.write_int("total_entities", total_entities());

    // Layers
    writer.start_array("layers");
    for (const auto& layer : m_layers) {
        writer.start_object();
        writer.write_string("name", layer.name.c_str());
        writer.write_color_array("color", layer.color_r, layer.color_g, layer.color_b);
        writer.write_bool("frozen", layer.frozen);
        writer.write_bool("off", layer.off);
        writer.end_object();
    }
    writer.end_array();

    // Blocks
    writer.start_array("blocks");
    for (const auto& block : m_blocks) {
        writer.start_object();
        writer.write_string("name", block.name.c_str());
        writer.write_int("entity_count", block.entity_count);
        writer.end_object();
    }
    writer.end_array();

    // Block entity counts (for structural comparison)
    writer.start_object("block_entity_counts");
    for (const auto& p : m_block_entity_counts) {
        writer.write_int(p.first.c_str(), p.second);
    }
    writer.end_object();

    // Insert instances (recovered from HOOPS flattened blocks, if any)
    writer.start_array("insert_instances");
    for (const auto& ins : m_insert_instances) {
        writer.start_object();
        writer.write_string("block_name", ins.block_name.c_str());
        writer.write_key("insertion_point");
        fprintf(writer.file(), "[%.6g, %.6g, %.6g]", ins.insertion_point[0], ins.insertion_point[1], ins.insertion_point[2]);
        writer.set_first(false);
        writer.write_key("scale");
        fprintf(writer.file(), "[%.6g, %.6g, %.6g]", ins.scale[0], ins.scale[1], ins.scale[2]);
        writer.set_first(false);
        writer.write_double("rotation", ins.rotation);
        writer.write_string("layer", ins.layer.c_str());
        writer.end_object();
    }
    writer.end_array();

    // Entities
    writer.start_array("entities");
    for (const auto& entity : m_entities) {
        writer.start_object();
        writer.write_string("type", entity.type.c_str());
        writer.write_string("layer", entity.layer.c_str());
        if (!entity.space.empty()) {
            writer.write_string("space", entity.space.c_str());
        }
        writer.write_color_array("color", entity.color_r, entity.color_g, entity.color_b);

        for (const auto& prop : entity.props) {
            writer.write_key(prop.first.c_str());
            const std::string& v = prop.second;
            if (!v.empty() && v[0] == '[') {
                fprintf(writer.file(), "%s", v.c_str());
            } else {
                char* end = nullptr;
                double d = strtod(v.c_str(), &end);
                if (end != v.c_str() && *end == '\0') {
                    fprintf(writer.file(), "%.6g", d);
                } else {
                    writer.write_string_value(v.c_str());
                }
            }
            writer.set_first(false);
        }

        writer.end_object();
    }
    writer.end_array();

    // Scene tree (hierarchical structure)
    if (!m_tree_nodes.empty() && m_tree_root >= 0) {
        writer.start_object("scene_tree");

        std::function<void(int)> write_node = [&](int idx) {
            const auto& node = m_tree_nodes[idx];
            writer.write_string("name", node.name.c_str());
            writer.write_string("type", node.type.c_str());
            writer.write_int("depth", node.depth);
            writer.write_int("entity_count", static_cast<int>(node.entity_indices.size()));

            if (node.has_bbox) {
                writer.write_key("bbox_min");
                fprintf(writer.file(), "[%.6g, %.6g, %.6g]", node.bbox_min[0], node.bbox_min[1], node.bbox_min[2]);
                writer.set_first(false);
                writer.write_key("bbox_max");
                fprintf(writer.file(), "[%.6g, %.6g, %.6g]", node.bbox_max[0], node.bbox_max[1], node.bbox_max[2]);
                writer.set_first(false);
            }

            if (!node.entity_indices.empty()) {
                writer.start_array("entity_indices");
                for (int ei : node.entity_indices) {
                    writer.write_int_value(ei);
                }
                writer.end_array();
            }

            if (!node.child_indices.empty()) {
                writer.start_array("children");
                for (int ci : node.child_indices) {
                    writer.start_object();
                    write_node(ci);
                    writer.end_object();
                }
                writer.end_array();
            }
        };

        write_node(m_tree_root);
        writer.end_object();
    }

    writer.end_object();
}
