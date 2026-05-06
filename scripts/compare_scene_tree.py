#!/usr/bin/env python3
"""compare_scene_tree.py — Compare SCS reference tree vs self-developed parser tree.

DEPRECATED: Use compare_structural.py instead. This script is kept for backward
compatibility and will be removed in a future release.

Compares the scene tree structure from HOOPS SCS metadata against the
self-developed DWG parser's SceneGraph output.

Usage:
  python3 scripts/compare_scene_tree.py <scs_tree.json> <ft_tree.json> [--verbose]
  python3 scripts/compare_scene_tree.py /tmp/Drawing2_scs_tree.json /tmp/Drawing2_ft.json --verbose

Inputs:
  - SCS tree: output of extract_scs_tree.py (from SCS metadata)
  - FT tree:  output of render_export with scene_tree field

Output: Text comparison report to stdout, exit code 0 if all checks pass.
"""

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path


# ─── Helpers ────────────────────────────────────────────────────────────────

def load_json(path: str) -> dict:
    for enc in ('utf-8', 'gb2312', 'gbk', 'latin-1'):
        try:
            with open(path, encoding=enc) as f:
                return json.load(f)
        except UnicodeDecodeError:
            continue
    raise ValueError(f"Could not decode {path}")


def pct(part: int, whole: int) -> str:
    if whole == 0:
        return 'N/A'
    return f"{part / whole * 100:.1f}%"


def status_icon(ok: bool) -> str:
    return 'OK' if ok else 'MISMATCH'


# ─── Tree utilities ─────────────────────────────────────────────────────────

def walk_tree(node: dict, fn, depth=0):
    """Walk a tree node, calling fn(node, depth) at each node."""
    fn(node, depth)
    for child in node.get('children', []):
        walk_tree(child, fn, depth + 1)


def collect_nodes_flat(tree: dict) -> list:
    """Flatten a tree into a list of all nodes."""
    result = []
    walk_tree(tree, lambda n, d: result.append(n))
    return result


def tree_depth(tree: dict) -> int:
    """Compute max depth of a tree."""
    if 'children' not in tree or not tree['children']:
        return tree.get('depth', 0)
    return max(tree_depth(c) for c in tree['children'])


def count_by_type(tree: dict) -> Counter:
    """Count nodes by type."""
    counts = Counter()
    walk_tree(tree, lambda n, d: counts.update([n.get('type', 'Unknown')]))
    return counts


def tree_topology_signature(node: dict) -> str:
    """Generate a topology signature for structural comparison.
    Ignores IDs and names, only considers type and child structure."""
    children = node.get('children', [])
    child_sigs = sorted(tree_topology_signature(c) for c in children)
    return f"{node.get('type', '?')}({','.join(child_sigs)})"


# ─── Layer comparison ───────────────────────────────────────────────────────

def extract_ft_layers(ft_data: dict) -> dict:
    """Extract layers from FT export data."""
    layers = {}
    for layer in ft_data.get('layers', []):
        name = layer.get('name', '')
        # Support both dwg_to_json and render_export formats
        color = layer.get('color', [7, 7, 7])
        frozen = layer.get('frozen', False)
        off = layer.get('off', False)
        layers[name] = {
            'color': color,
            'frozen': frozen,
            'off': off,
        }
    return layers


def extract_ft_blocks(ft_data: dict) -> dict:
    """Extract blocks from FT export data."""
    blocks = {}

    # Try explicit blocks array (dwg_to_json format)
    for block in ft_data.get('blocks', []):
        name = block.get('name', '')
        blocks[name] = {
            'entity_count': block.get('entity_count', 0),
            'entity_indices': block.get('entity_indices', []),
        }

    if blocks:
        return blocks

    # Fall back to sceneTree (render_export format)
    scene_tree = ft_data.get('sceneTree', [])
    if isinstance(scene_tree, list):
        for node in scene_tree:
            if node.get('type') == 'blockDefinition':
                name = f"block_{node.get('id', '?')}"
                blocks[name] = {
                    'entity_count': len(node.get('entityIndices', [])),
                    'entity_indices': node.get('entityIndices', []),
                }

    return blocks


def extract_ft_entity_distribution(ft_data: dict) -> dict:
    """Extract entity distribution by (layer, type) from FT data."""
    dist = defaultdict(int)

    # Try entities list first (dwg_to_json format)
    entities = ft_data.get('entities', [])
    if entities:
        for entity in entities:
            if entity.get('in_block', False):
                continue
            layer = entity.get('layer', '0')
            etype = entity.get('type', 'UNKNOWN')
            space = entity.get('space', 'model')
            dist[(layer, etype, space)] += 1
        return dict(dist)

    # Fall back to entityTypeCounts (render_export format)
    type_counts = ft_data.get('entityTypeCounts', {})
    for etype, count in type_counts.items():
        dist[('all', etype, 'all')] += count
    return dict(dist)


def extract_ft_scene_tree_stats(ft_data: dict) -> dict:
    """Extract stats from FT export data (supports both nested and flat formats)."""
    # FT exports sceneTree as a flat array of nodes with id/parentId/children
    scene_tree = ft_data.get('sceneTree', ft_data.get('scene_tree'))
    if not scene_tree:
        return {}

    if isinstance(scene_tree, list):
        # Flat array format (FT render_export output)
        total = len(scene_tree)
        type_counts = Counter()
        max_depth = 0
        for node in scene_tree:
            ntype = node.get('type', 'unknown')
            type_counts[ntype] += 1
        return {
            'total_nodes': total,
            'max_depth': 0,  # Can't easily compute from flat array
            'type_counts': dict(type_counts),
        }
    elif isinstance(scene_tree, dict):
        # Nested tree format
        return {
            'total_nodes': len(collect_nodes_flat(scene_tree)),
            'max_depth': tree_depth(scene_tree),
            'type_counts': dict(count_by_type(scene_tree)),
        }
    return {}


# ─── SCS tree stats ─────────────────────────────────────────────────────────

def extract_scs_stats(scs_data: dict) -> dict:
    """Extract stats from HOOPS/SCS data."""
    stats = scs_data.get('stats', {})
    tree = scs_data.get('scene_tree')
    if tree and not stats:
        flat = collect_nodes_flat(tree) if isinstance(tree, dict) else tree if isinstance(tree, list) else []
        stats = {
            'total_nodes': len(flat),
            'max_depth': tree_depth(tree) if isinstance(tree, dict) else 0,
            'type_counts': dict(count_by_type(tree)) if isinstance(tree, dict) else {},
        }
    # Also extract entity_counts (HOOPS format)
    entity_counts = scs_data.get('entity_counts', {})
    if entity_counts:
        stats['entity_counts'] = entity_counts
        stats['total_entities'] = scs_data.get('total_entities', sum(entity_counts.values()))
    return stats


# ─── Bounding box comparison ────────────────────────────────────────────────

def bbox_deviation(bbox1_min, bbox1_max, bbox2_min, bbox2_max) -> float:
    """Compute relative deviation between two bounding boxes.
    Returns max relative deviation in any dimension (0.0 = identical)."""
    if not all([bbox1_min, bbox1_max, bbox2_min, bbox2_max]):
        return float('nan')

    deviations = []
    for i in range(3):
        span1 = bbox1_max[i] - bbox1_min[i]
        span2 = bbox2_max[i] - bbox2_min[i]
        avg_span = (span1 + span2) / 2
        if avg_span > 0:
            deviations.append(abs(span1 - span2) / avg_span)
        else:
            deviations.append(0.0)

    # Also compare center offset
    center1 = [(bbox1_min[i] + bbox1_max[i]) / 2 for i in range(3)]
    center2 = [(bbox2_min[i] + bbox2_max[i]) / 2 for i in range(3)]
    diag = math.sqrt(sum((bbox1_max[i] - bbox1_min[i])**2 for i in range(3)))
    if diag > 0:
        offset = math.sqrt(sum((center1[i] - center2[i])**2 for i in range(3)))
        deviations.append(offset / diag)

    return max(deviations)


# ─── SCS entity name patterns ───────────────────────────────────────────────

# Map SCS node name patterns to entity categories for comparison
SCS_NAME_TO_CATEGORY = {
    'CATERPILLAR': 'LayoutGroup',
    'MODEL': 'ModelSpace',
    'PAPER': 'PaperSpace',
}


# ─── Main comparison ────────────────────────────────────────────────────────

def run_comparison(scs_path: str, ft_path: str, verbose: bool = False) -> dict:
    scs_data = load_json(scs_path)
    ft_data = load_json(ft_path)

    result = {
        'scs_source': scs_data.get('source', scs_path),
        'ft_source': ft_data.get('source', ft_path),
        'checks': {},
    }

    # ── 1. Layer comparison ──────────────────────────────────────────
    ft_layers = extract_ft_layers(ft_data)
    scs_tree = scs_data.get('scene_tree', {})

    # SCS doesn't have explicit layer list in metadata, but we can infer
    # from node names or use flat_nodes
    scs_node_names = set()
    for node in scs_data.get('flat_nodes', []):
        name = node.get('name', '')
        if name and not name.startswith('*') and 'CATERPILLAR' not in name.upper():
            scs_node_names.add(name)

    ft_layer_names = set(ft_layers.keys())

    layer_result = {
        'scs_node_names_count': len(scs_node_names),
        'ft_layer_count': len(ft_layer_names),
        'ft_layers': sorted(ft_layer_names),
        'scs_names': sorted(scs_node_names),
        'common': sorted(ft_layer_names & scs_node_names),
        'ft_only': sorted(ft_layer_names - scs_node_names),
        'scs_only': sorted(scs_node_names - ft_layer_names),
    }
    result['checks']['layers'] = layer_result

    # ── 2. Tree structure comparison ─────────────────────────────────
    scs_stats = extract_scs_stats(scs_data)
    ft_stats = extract_ft_scene_tree_stats(ft_data)

    # Also count from flat data if scene_tree not available
    scs_node_count = scs_stats.get('total_nodes', scs_data.get('node_count', len(scs_data.get('flat_nodes', []))))
    ft_node_count = ft_stats.get('total_nodes', 0)

    # Count FT entities (non-block)
    ft_entities_list = ft_data.get('entities', [])
    ft_entity_count = len([e for e in ft_entities_list if not e.get('in_block', False)])
    # If no entities list, use entityTypeCounts
    if ft_entity_count == 0:
        ft_entity_count = sum(ft_data.get('entityTypeCounts', {}).values())
    ft_total_entities = ft_data.get('entityCount', ft_data.get('total_entities', ft_entity_count))

    tree_result = {
        'scs_node_count': scs_node_count,
        'ft_node_count': ft_node_count,
        'ft_entity_count': ft_entity_count,
        'ft_total_entities': ft_total_entities,
        'scs_max_depth': scs_stats.get('max_depth', 0),
        'ft_max_depth': ft_stats.get('max_depth', 0),
        'coverage_pct': pct(ft_node_count, scs_node_count) if scs_node_count > 0 else 'N/A',
    }
    result['checks']['tree'] = tree_result

    # ── 3. Block comparison ──────────────────────────────────────────
    ft_blocks = extract_ft_blocks(ft_data)

    # Extract block-like nodes from SCS
    scs_block_nodes = []
    scs_anon_blocks = []
    for node in scs_data.get('flat_nodes', []):
        name = node.get('name', '')
        if name.startswith('*'):
            scs_anon_blocks.append(name)
        elif name and 'CATERPILLAR' not in name.upper():
            # Could be a block or layer — hard to distinguish without more context
            pass

    block_result = {
        'ft_block_count': len(ft_blocks),
        'ft_blocks': sorted(ft_blocks.keys()),
        'scs_anonymous_blocks': sorted(scs_anon_blocks),
        'scs_anon_count': len(scs_anon_blocks),
    }
    result['checks']['blocks'] = block_result

    # ── 4. Entity distribution ───────────────────────────────────────
    # Normalize type names: HOOPS uses UPPERCASE, FT uses CamelCase
    TYPE_NORMALIZE = {
        'LINE': 'Line', 'ARC': 'Arc', 'CIRCLE': 'Circle',
        'ELLIPSE': 'Ellipse', 'SPLINE': 'Spline',
        'LWPOLYLINE': 'LwPolyline', 'POLYLINE': 'Polyline',
        'TEXT': 'Text', 'MTEXT': 'MText',
        'DIMENSION': 'Dimension', 'HATCH': 'Hatch',
        'INSERT': 'Insert', 'POINT': 'Point',
        'SOLID': 'Solid', 'LEADER': 'Leader',
        'TOLERANCE': 'Tolerance', 'MLEADER': 'Multileader',
        'RAY': 'Ray', 'XLINE': 'XLine',
        'VIEWPORT': 'Viewport', 'MLINE': 'MLine',
        # HOOPS Drawing Model types → mapped to closest FT types
        # DRAWING_CURVE represents Drawing Model curves (arcs, lines, circles, etc.)
        # These are Paper Space geometry that FT parses as standard entity types
        'DRAWING_CURVE': 'DrawingCurve',
        'POLYBREP': 'PolygonBrep',
        'ANNOTATION': 'DrawingAnnotation',
        'HATCH': 'Hatch',
    }

    ft_dist = extract_ft_entity_distribution(ft_data)

    # Aggregate FT by type
    ft_type_counts = Counter()
    ft_space_counts = Counter()
    for (layer, etype, space), count in ft_dist.items():
        normalized = TYPE_NORMALIZE.get(etype, etype)
        ft_type_counts[normalized] += count
        ft_space_counts[space] += count

    # Get HOOPS entity counts
    hoops_entity_counts = scs_stats.get('entity_counts', {})
    hoops_total_entities = scs_stats.get('total_entities', 0)

    hoops_normalized = Counter()
    for t, n in hoops_entity_counts.items():
        normalized = TYPE_NORMALIZE.get(t, t)
        hoops_normalized[normalized] += n

    # Compute differences
    all_types = sorted(set(ft_type_counts.keys()) | set(hoops_normalized.keys()))
    type_diff = {}
    for t in all_types:
        h = hoops_normalized.get(t, 0)
        f = ft_type_counts.get(t, 0)
        type_diff[t] = {
            'hoops': h,
            'ft': f,
            'diff': f - h,
        }

    # Reconciled comparison: account for HOOPS Drawing Model types
    # HOOPS types that have no direct DWG equivalent (Drawing Model specific)
    HOOPS_DRAWING_MODEL_TYPES = {'DrawingCurve', 'Hatch', 'DrawingAnnotation', 'PolygonBrep'}
    # FT types that are Paper Space entities (counted as "extra" vs HOOPS model)
    FT_PAPER_TYPES = {'Circle', 'Text', 'MText', 'Insert'}

    hoops_model_total = sum(v for k, v in hoops_normalized.items() if k not in HOOPS_DRAWING_MODEL_TYPES)
    hoops_drawing_total = sum(v for k, v in hoops_normalized.items() if k in HOOPS_DRAWING_MODEL_TYPES)
    ft_total_val = sum(ft_type_counts.values())

    # Fair comparison: compare model-space types only
    model_type_diff = {}
    for t in all_types:
        if t in HOOPS_DRAWING_MODEL_TYPES:
            continue
        h = hoops_normalized.get(t, 0)
        f = ft_type_counts.get(t, 0)
        if h > 0 or f > 0:
            model_type_diff[t] = {
                'hoops': h,
                'ft': f,
                'diff': f - h,
                'match_pct': f / h * 100 if h > 0 else (100.0 if f == 0 else float('inf')),
            }

    entity_result = {
        'hoops_entity_types': dict(hoops_normalized.most_common()),
        'hoops_total': hoops_total_entities,
        'hoops_model_total': hoops_model_total,
        'hoops_drawing_model_total': hoops_drawing_total,
        'ft_entity_types': dict(ft_type_counts.most_common()),
        'ft_total': ft_total_val,
        'ft_space_distribution': dict(ft_space_counts),
        'scs_node_types': scs_stats.get('type_counts', {}),
        'type_diff': type_diff,
        'model_type_diff': model_type_diff,
        'reconciled': {
            'hoops_model_only': hoops_model_total,
            'hoops_drawing_model_specific': hoops_drawing_total,
            'ft_total': ft_total_val,
            'model_coverage_pct': ft_total_val / hoops_model_total * 100 if hoops_model_total > 0 else 0,
            'overall_coverage_pct': ft_total_val / hoops_total_entities * 100 if hoops_total_entities > 0 else 0,
        },
    }
    result['checks']['entities'] = entity_result

    # ── 5. Bounding box comparison ───────────────────────────────────
    scs_bbox_min = scs_stats.get('total_bbox_min')
    scs_bbox_max = scs_stats.get('total_bbox_max')

    # FT bounding box from export data
    ft_bbox = None

    # Try bounds from render_export format
    ft_bounds = ft_data.get('bounds', {})
    if ft_bounds and not ft_bounds.get('isEmpty', True):
        ft_bbox = {
            'min': [ft_bounds.get('minX', 0), ft_bounds.get('minY', 0), 0],
            'max': [ft_bounds.get('maxX', 0), ft_bounds.get('maxY', 0), 0],
        }

    # Fall back to scene tree bounds
    if not ft_bbox:
        ft_tree = ft_data.get('sceneTree', ft_data.get('scene_tree'))
        if isinstance(ft_tree, list):
            mins = [float('inf')] * 3
            maxs = [float('-inf')] * 3
            for n in ft_tree:
                wb = n.get('worldBounds', {})
                if wb and not wb.get('isEmpty', False):
                    mins[0] = min(mins[0], wb.get('minX', float('inf')))
                    mins[1] = min(mins[1], wb.get('minY', float('inf')))
                    maxs[0] = max(maxs[0], wb.get('maxX', float('-inf')))
                    maxs[1] = max(maxs[1], wb.get('maxY', float('-inf')))
            if mins[0] != float('inf'):
                ft_bbox = {'min': mins, 'max': maxs}

    bbox_result = {
        'scs_bbox': {'min': scs_bbox_min, 'max': scs_bbox_max} if scs_bbox_min else None,
        'ft_bbox': ft_bbox,
    }

    if scs_bbox_min and ft_bbox:
        deviation = bbox_deviation(scs_bbox_min, scs_bbox_max,
                                   ft_bbox['min'], ft_bbox['max'])
        bbox_result['deviation'] = deviation
        bbox_result['deviation_pct'] = f"{deviation * 100:.2f}%"
        bbox_result['ok'] = deviation < 0.05  # 5% tolerance

    result['checks']['bbox'] = bbox_result

    # ── 6. Topology comparison (if both have scene trees) ────────────
    ft_tree = ft_data.get('sceneTree', ft_data.get('scene_tree'))
    scs_tree = scs_data.get('scene_tree')
    if ft_tree and scs_tree:
        # Get type sequences from both trees
        if isinstance(scs_tree, dict):
            scs_flat = collect_nodes_flat(scs_tree)
        elif isinstance(scs_tree, list):
            scs_flat = scs_tree
        else:
            scs_flat = []

        if isinstance(ft_tree, list):
            ft_flat = ft_tree
        elif isinstance(ft_tree, dict):
            ft_flat = collect_nodes_flat(ft_tree)
        else:
            ft_flat = []

        scs_type_seq = [n.get('type', '') for n in scs_flat]
        ft_type_seq = [n.get('type', '') for n in ft_flat]

        scs_type_counter = Counter(scs_type_seq)
        ft_type_counter = Counter(ft_type_seq)
        common_types = sum((scs_type_counter & ft_type_counter).values())
        total_types = sum((scs_type_counter | ft_type_counter).values())
        type_similarity = common_types / total_types if total_types > 0 else 0

        topology_result = {
            'scs_type_distribution': dict(Counter(scs_type_seq).most_common()),
            'ft_type_distribution': dict(Counter(ft_type_seq).most_common()),
            'type_similarity': f"{type_similarity * 100:.1f}%",
        }
        result['checks']['topology'] = topology_result

    return result


# ─── Report printing ────────────────────────────────────────────────────────

def print_report(result: dict, verbose: bool = False):
    print('=' * 70)
    print('SCS vs Self-Developed Parser — Structure Tree Comparison')
    print('=' * 70)
    print(f"SCS source: {result['scs_source']}")
    print(f"FT  source: {result['ft_source']}")

    # Layers
    lr = result['checks'].get('layers', {})
    print(f"\n### Layer Comparison")
    print(f"  SCS node names: {lr.get('scs_node_names_count', 0)}")
    print(f"  FT layers:      {lr.get('ft_layer_count', 0)}")
    if lr.get('ft_only'):
        print(f"  FT-only layers: {lr['ft_only']}")
    if lr.get('scs_only'):
        print(f"  SCS-only names: {lr['scs_only'][:10]}{'...' if len(lr.get('scs_only', [])) > 10 else ''}")
    if lr.get('common'):
        print(f"  Common:         {len(lr['common'])}")

    # Tree
    tr = result['checks'].get('tree', {})
    print(f"\n### Tree Structure")
    print(f"  SCS nodes:      {tr.get('scs_node_count', 0)}")
    print(f"  FT nodes:       {tr.get('ft_node_count', 0)}")
    print(f"  FT entities:    {tr.get('ft_entity_count', 0)} (total incl. block defs: {tr.get('ft_total_entities', 0)})")
    print(f"  Coverage:       {tr.get('coverage_pct', 'N/A')}")
    print(f"  SCS depth:      {tr.get('scs_max_depth', 0)}")
    print(f"  FT depth:       {tr.get('ft_max_depth', 0)}")

    # Blocks
    br = result['checks'].get('blocks', {})
    print(f"\n### Block Definitions")
    print(f"  FT blocks:      {br.get('ft_block_count', 0)}")
    print(f"  SCS anon blocks: {br.get('scs_anon_count', 0)}")
    if verbose and br.get('ft_blocks'):
        print(f"  FT block names: {br['ft_blocks'][:20]}{'...' if len(br.get('ft_blocks', [])) > 20 else ''}")
    if verbose and br.get('scs_anonymous_blocks'):
        print(f"  SCS anon blocks: {br['scs_anonymous_blocks'][:20]}")

    # Entity distribution comparison
    er = result['checks'].get('entities', {})
    reconciled = er.get('reconciled', {})
    type_diff = er.get('type_diff', {})
    model_type_diff = er.get('model_type_diff', {})

    if reconciled:
        print(f"\n### Entity Coverage (Reconciled)")
        print(f"  HOOPS total:         {reconciled.get('hoops_model_only', 0) + reconciled.get('hoops_drawing_model_specific', 0)}")
        print(f"  HOOPS model space:   {reconciled.get('hoops_model_only', 0)}")
        print(f"  HOOPS Drawing Model: {reconciled.get('hoops_drawing_model_specific', 0)} (no DWG equivalent)")
        print(f"  FT total:            {reconciled.get('ft_total', 0)}")
        print(f"  Model coverage:      {reconciled.get('model_coverage_pct', 0):.1f}%")
        print(f"  Overall coverage:    {reconciled.get('overall_coverage_pct', 0):.1f}%")

    if model_type_diff:
        print(f"\n### Model-Space Type Comparison (excl. Drawing Model specifics)")
        print(f"  {'Type':<20} {'HOOPS':>8} {'FT':>8} {'Diff':>8} {'Match':>7} Status")
        print(f"  {'-'*20} {'-'*8} {'-'*8} {'-'*8} {'-'*7} {'-'*10}")
        for t in sorted(model_type_diff.keys(), key=lambda x: -max(model_type_diff[x]['hoops'], model_type_diff[x]['ft'])):
            d = model_type_diff[t]
            match_pct = d.get('match_pct', 0)
            status = "OK" if abs(d['diff']) <= max(1, d['hoops'] * 0.05) else "WARN"
            print(f"  {t:<20} {d['hoops']:>8} {d['ft']:>8} {d['diff']:>+8} {match_pct:>6.1f}% [{status}]")

    if verbose:
        scs_types = er.get('scs_node_types', {})
        if scs_types:
            print(f"\n  HOOPS scene tree node types:")
            for t, c in sorted(scs_types.items(), key=lambda x: -x[1]):
                print(f"    {t:<20} {c:>6}")

        ft_spaces = er.get('ft_space_distribution', {})
        if ft_spaces:
            print(f"\n  FT space distribution:")
            for s, c in sorted(ft_spaces.items(), key=lambda x: -x[1]):
                print(f"    {s:<12} {c:>8}")

    # Bounding box
    br_bbox = result['checks'].get('bbox', {})
    print(f"\n### Bounding Box")
    if br_bbox.get('scs_bbox') and br_bbox['scs_bbox'].get('min'):
        bb = br_bbox['scs_bbox']
        print(f"  SCS: [{bb['min'][0]:.1f},{bb['min'][1]:.1f}] → [{bb['max'][0]:.1f},{bb['max'][1]:.1f}]")
    if br_bbox.get('ft_bbox'):
        bb = br_bbox['ft_bbox']
        print(f"  FT:  [{bb['min'][0]:.1f},{bb['min'][1]:.1f}] → [{bb['max'][0]:.1f},{bb['max'][1]:.1f}]")
    if 'deviation_pct' in br_bbox:
        icon = 'OK' if br_bbox.get('ok') else 'MISMATCH'
        print(f"  Deviation: {br_bbox['deviation_pct']} [{icon}]")

    # Topology
    topo = result['checks'].get('topology', {})
    if topo:
        print(f"\n### Topology Similarity")
        print(f"  Type similarity: {topo.get('type_similarity', 'N/A')}")
        if verbose:
            print(f"\n  SCS type distribution:")
            for t, c in sorted(topo.get('scs_type_distribution', {}).items(), key=lambda x: -x[1]):
                print(f"    {t:<20} {c:>6}")
            print(f"\n  FT type distribution:")
            for t, c in sorted(topo.get('ft_type_distribution', {}).items(), key=lambda x: -x[1]):
                print(f"    {t:<20} {c:>6}")

    # Summary
    print(f"\n{'=' * 70}")
    print(f"### Summary")

    layers_ok = lr.get('ft_layer_count', 0) > 0
    nodes_ok = tr.get('ft_node_count', 0) > 0
    bbox_ok = br_bbox.get('ok', False)

    print(f"  Layers:    {'HAS DATA' if layers_ok else 'NO DATA'} ({lr.get('ft_layer_count', 0)} layers)")
    print(f"  Nodes:     {tr.get('coverage_pct', 'N/A')} coverage ({tr.get('ft_node_count', 0)}/{tr.get('scs_node_count', 0)})")
    print(f"  Entities:  {tr.get('ft_entity_count', 0)} non-block entities")
    print(f"  Blocks:    {br.get('ft_block_count', 0)} block definitions")
    if 'deviation_pct' in br_bbox:
        print(f"  BBox:      {br_bbox['deviation_pct']} deviation [{('OK' if bbox_ok else 'WARN')}]")

    # Overall assessment
    issues = []
    if not layers_ok:
        issues.append("No FT layer data")
    if tr.get('ft_node_count', 0) == 0:
        issues.append("No FT scene tree (need build_scene_tree)")
    if not bbox_ok and 'deviation_pct' in br_bbox:
        issues.append(f"BBox deviation {br_bbox['deviation_pct']}")

    if not issues:
        print(f"\n  [OK] All basic checks passed")
    else:
        print(f"\n  [WARN] Issues: {'; '.join(issues)}")

    return len(issues) == 0


def main():
    print("[DEPRECATED] compare_scene_tree.py is deprecated. Use compare_structural.py instead.")
    print()

    verbose = '--verbose' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]

    if len(args) < 2:
        print(f"Usage: {sys.argv[0]} <scs_tree.json> <ft_tree.json> [--verbose]")
        print(f"\nCompares SCS reference tree against self-developed parser tree.")
        sys.exit(2)

    scs_path = args[0]
    ft_path = args[1]

    result = run_comparison(scs_path, ft_path, verbose)
    ok = print_report(result, verbose)

    # Also save JSON result
    json_output = ft_path.replace('.json', '_comparison.json')
    try:
        with open(json_output, 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2, ensure_ascii=False)
        print(f"\nDetailed JSON saved to: {json_output}")
    except Exception:
        pass

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    main()
