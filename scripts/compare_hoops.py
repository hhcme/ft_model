#!/usr/bin/env python3
"""compare_hoops.py — HOOPS Exchange SDK vs self-developed parser structural comparison.

DEPRECATED: Use compare_structural.py instead. This script is kept for backward
compatibility and will be removed in a future release.

Usage:
  python3 compare_hoops.py <hoops.json> <ft.json> [--verbose]

Compares entity structure, layer data, and geometric output between
HOOPS Exchange SDK (dwg_to_json) and self-developed parser (entity_export).
"""

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path


def load_json(path: str) -> dict:
    # Try UTF-8 first, fall back to GB2312 (Chinese encoding used by FT parser)
    for enc in ('utf-8', 'gb2312', 'gbk', 'latin-1'):
        try:
            with open(path, encoding=enc) as f:
                return json.load(f)
        except UnicodeDecodeError:
            continue
    raise ValueError(f"Could not decode {path} with any known encoding")


def normalize_hoops_type(t: str) -> str:
    """Map HOOPS types to FT-like categories for comparison."""
    # HOOPS uses specific type names, map to same categories as FT
    if t in ('LINE', 'ARC', 'CIRCLE', 'ELLIPSE', 'CURVE', 'RISET'):
        return 'GEOM_CURVE'
    elif t in ('LWPOLYLINE', 'POLYLINE', 'SPLINE', 'POLYWIRE', 'DRAWING_CURVE'):
        return 'POLYBREP'
    elif t in ('TEXT', 'MTEXT', 'DIMENSION', 'TOLERANCE', 'ANNOTATION'):
        return 'ANNOTATION'
    elif t in ('HATCH', 'SOLID'):
        return 'HATCH'
    elif t in ('INSERT',):
        return 'BLOCK'
    elif t in ('POINT', 'VERTEX'):
        return 'GEOM_CURVE'
    elif t == 'MARKUP':
        return 'ANNOTATION'
    elif t == 'UNKNOWN':
        return 'UNKNOWN'
    else:
        return t


def normalize_ft_type(t: str) -> str:
    """Map FT types to common categories."""
    if t in ('LINE', 'RAY', 'XLINE'):
        return 'GEOM_CURVE'
    elif t in ('ARC', 'CIRCLE', 'ELLIPSE'):
        return 'GEOM_CURVE'
    elif t in ('TEXT', 'MTEXT', 'DIMENSION', 'TOLERANCE'):
        return 'ANNOTATION'
    elif t == 'HATCH':
        return 'HATCH'
    elif t in ('LWPOLYLINE', 'POLYLINE', 'SPLINE', 'MLEADER'):
        return 'POLYBREP'
    elif t in ('INSERT',):
        return 'BLOCK'
    elif t in ('SOLID', 'POINT'):
        return 'GEOM_CURVE'
    else:
        return t


def compare_layers(hoops_data: dict, ft_data: dict) -> dict:
    """Compare layer structure between parsers."""
    h_layers = {l['name'] for l in hoops_data.get('layers', [])}
    f_layers = {l['name'] for l in ft_data.get('layers', [])}

    return {
        'hoops_count': len(h_layers),
        'ft_count': len(f_layers),
        'match': h_layers == f_layers,
        'hoops_only': sorted(h_layers - f_layers),
        'ft_only': sorted(f_layers - h_layers),
        'common': sorted(h_layers & f_layers),
    }


def compare_entity_counts_from_lists(hoops_ents: list, ft_ents: list) -> dict:
    """Compare entity counts from filtered entity lists."""
    from collections import Counter

    h_counts = Counter()
    for e in hoops_ents:
        h_counts[normalize_hoops_type(e.get('type', 'UNKNOWN'))] += 1

    f_counts = Counter()
    for e in ft_ents:
        f_counts[normalize_ft_type(e.get('type', 'UNKNOWN'))] += 1

    all_cats = sorted(set(h_counts.keys()) | set(f_counts.keys()))

    h_total = sum(h_counts.values())
    f_total = sum(f_counts.values())

    return {
        'hoops_total': h_total,
        'ft_total': f_total,
        'ratio_pct': round(f_total / h_total * 100, 1) if h_total > 0 else 0,
        'hoops_raw': dict(h_counts),
        'ft_raw': dict(f_counts),
        'hoops_categories': dict(h_counts),
        'ft_categories': dict(f_counts),
        'by_category': {
            cat: {
                'hoops': h_counts.get(cat, 0),
                'ft': f_counts.get(cat, 0),
                'diff': f_counts.get(cat, 0) - h_counts.get(cat, 0),
            }
            for cat in all_cats
        }
    }


def compare_entity_counts(hoops_data: dict, ft_data: dict) -> dict:
    """Compare entity counts with type normalization."""
    h_counts = hoops_data.get('entity_counts', {})
    f_counts = ft_data.get('entity_counts', {})

    # Normalize both to category counts
    h_cats = Counter()
    for t, n in h_counts.items():
        h_cats[normalize_hoops_type(t)] += n

    f_cats = Counter()
    for t, n in f_counts.items():
        f_cats[normalize_ft_type(t)] += n

    all_cats = sorted(set(h_cats.keys()) | set(f_cats.keys()))

    h_total = sum(h_counts.values())
    f_total = sum(f_counts.values())

    return {
        'hoops_total': h_total,
        'ft_total': f_total,
        'ratio_pct': round(f_total / h_total * 100, 1) if h_total > 0 else 0,
        'hoops_raw': dict(h_counts),
        'ft_raw': dict(f_counts),
        'hoops_categories': dict(h_cats),
        'ft_categories': dict(f_cats),
        'by_category': {
            cat: {
                'hoops': h_cats.get(cat, 0),
                'ft': f_cats.get(cat, 0),
                'diff': f_cats.get(cat, 0) - h_cats.get(cat, 0),
            }
            for cat in all_cats
        }
    }


def geometric_buckets(entities: list, category_fn) -> defaultdict:
    """Group entities by (layer, category) for spatial comparison."""
    buckets = defaultdict(list)
    for e in entities:
        layer = e.get('layer', '0')
        cat = category_fn(e.get('type', 'UNKNOWN'))
        buckets[(layer, cat)].append(e)
    return buckets


def bbox_from_entity(e: dict) -> tuple:
    """Get bounding box for an entity (minx, miny, maxx, maxy)."""
    pts = []

    def add(x, y):
        if isinstance(x, (int, float)) and isinstance(y, (int, float)):
            pts.append((float(x), float(y)))

    t = e.get('type', '')

    if 'start' in e and 'end' in e:
        add(e['start'][0], e['start'][1])
        add(e['end'][0], e['end'][1])
    # FT uses x0/y0/x1/y1 for LINE entities
    if 'x0' in e and 'y0' in e and 'x1' in e and 'y1' in e:
        add(e['x0'], e['y0'])
        add(e['x1'], e['y1'])
    if 'center' in e:
        add(e['center'][0], e['center'][1])
    if 'x' in e and 'y' in e:
        add(e['x'], e['y'])
    if 'vertices' in e:
        for v in e['vertices']:
            if len(v) >= 2:
                add(v[0], v[1])
    if 'control_points' in e:
        for cp in e['control_points']:
            if len(cp) >= 2:
                add(cp[0], cp[1])
    if 'corners' in e:
        for c in e['corners']:
            if len(c) >= 2:
                add(c[0], c[1])
    if 'definition_point' in e:
        dp = e['definition_point']
        add(dp[0], dp[1])
    if 'position' in e:
        p = e['position']
        add(p[0], p[1])

    if not pts:
        return (0, 0, 0, 0)

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def entity_centroid(e: dict) -> tuple:
    """Get centroid of entity for spatial matching."""
    bb = bbox_from_entity(e)
    return ((bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2)


def match_geometric_entities(hoops_entities: list, ft_entities: list,
                             category_fn_h, category_fn_f, tol_pct: float = 0.01) -> dict:
    """Match entities by layer+category then spatial proximity."""
    h_buckets = geometric_buckets(hoops_entities, category_fn_h)
    f_buckets = geometric_buckets(ft_entities, category_fn_f)

    all_keys = sorted(set(h_buckets.keys()) | set(f_buckets.keys()))

    total_matched = 0
    total_h_missing = 0
    total_f_extra = 0
    layer_results = {}

    # Compute overall scene range
    all_pts = []
    for e in ft_entities:
        c = entity_centroid(e)
        all_pts.append(c)
    if all_pts:
        xs = [p[0] for p in all_pts]
        ys = [p[1] for p in all_pts]
        scene_range = max(max(xs) - min(xs), max(ys) - min(ys), 1.0)
    else:
        scene_range = 1.0

    max_dist = scene_range * tol_pct

    for key in all_keys:
        layer, cat = key
        h_list = h_buckets.get(key, [])
        f_list = f_buckets.get(key, [])

        used_f = set()
        matched = 0
        h_missing = 0
        f_extra = 0

        for he in h_list:
            hc = entity_centroid(he)
            best_idx = -1
            best_dist = float('inf')
            for i, fe in enumerate(f_list):
                if i in used_f:
                    continue
                fc = entity_centroid(fe)
                d = math.sqrt((hc[0] - fc[0])**2 + (hc[1] - fc[1])**2)
                if d < best_dist:
                    best_dist = d
                    best_idx = i
            if best_idx >= 0 and best_dist <= max(max_dist, 0.5):
                matched += 1
                used_f.add(best_idx)
            else:
                h_missing += 1

        for i, fe in enumerate(f_list):
            if i not in used_f:
                f_extra += 1

        total_matched += matched
        total_h_missing += h_missing
        total_f_extra += f_extra

        if h_list or f_list:
            layer_results[f'{layer}/{cat}'] = {
                'hoops': len(h_list),
                'ft': len(f_list),
                'matched': matched,
                'hoops_missing': h_missing,
                'ft_extra': f_extra,
            }

    return {
        'total_matched': total_matched,
        'total_hoops_missing': total_h_missing,
        'total_ft_extra': total_f_extra,
        'by_layer_category': layer_results,
    }


def run_comparison(hoops_path: str, ft_path: str, verbose: bool = False) -> dict:
    hoops_data = load_json(hoops_path)
    ft_data = load_json(ft_path)

    layer_result = compare_layers(hoops_data, ft_data)

    hoops_ents = hoops_data.get('entities', [])
    ft_ents = ft_data.get('entities', [])

    # Filter out in_block entities for fair comparison
    # Only compare model space entities (not drawing/annotation space)
    # Note: HOOPS drawing space may contain HATCH/DRAWING_CURVE that FT reads as model space
    # To ensure fair comparison, we only compare HOOPS model space entities
    ft_ents_filtered = [e for e in ft_ents if not e.get('in_block', False)]
    hoops_ents_filtered = [e for e in hoops_ents if e.get('space', 'model') == 'model']

    # Count using filtered entities for fair comparison
    count_result = compare_entity_counts_from_lists(hoops_ents_filtered, ft_ents_filtered)

    # INSERT-aware adjustment: HOOPS flattens block references, FT keeps INSERTs.
    # Compute adjusted ratio excluding INSERT from FT for a fairer comparison.
    ft_insert_count = sum(1 for e in ft_ents_filtered if e.get('type') == 'INSERT')
    ft_non_insert = [e for e in ft_ents_filtered if e.get('type') != 'INSERT']
    count_result_no_insert = compare_entity_counts_from_lists(hoops_ents_filtered, ft_non_insert)
    count_result['ft_insert_count'] = ft_insert_count
    count_result['adjusted_ratio_pct'] = count_result_no_insert['ratio_pct']
    count_result['ft_total_no_insert'] = count_result_no_insert['ft_total']

    geo_result = match_geometric_entities(
        hoops_ents_filtered, ft_ents_filtered,
        normalize_hoops_type, normalize_ft_type
    )

    return {
        'source_file': ft_data.get('source', ft_path),
        'layers': layer_result,
        'entity_counts': count_result,
        'geometric': geo_result,
    }


def print_report(result: dict, verbose: bool = False):
    print("=" * 70)
    print(f"HOOPS vs Self-Developed Parser Comparison")
    print(f"Source: {result['source_file']}")
    print("=" * 70)

    # Layer comparison
    lr = result['layers']
    print(f"\n### Layers")
    print(f"  HOOPS: {lr['hoops_count']} layers")
    print(f"  FT:    {lr['ft_count']} layers")
    match_str = "[MATCH]" if lr['match'] else "[MISMATCH]"
    print(f"  Match: {match_str}")
    if lr['hoops_only']:
        print(f"  HOOPS-only: {lr['hoops_only']}")
    if lr['ft_only']:
        print(f"  FT-only:    {lr['ft_only']}")

    # Entity count comparison
    cr = result['entity_counts']
    print(f"\n### Entity Counts")
    print(f"  HOOPS total: {cr['hoops_total']}")
    print(f"  FT total:    {cr['ft_total']}  ({cr['ratio_pct']}% of HOOPS)")
    if cr.get('ft_insert_count', 0) > 0:
        print(f"  FT INSERTs:  {cr['ft_insert_count']}  (HOOPS flattens blocks, no INSERTs)")
        print(f"  FT adjusted: {cr['ft_total_no_insert']}  ({cr['adjusted_ratio_pct']}% of HOOPS, excl. INSERT)")

    print(f"\n  By category:")
    print(f"  {'Category':<16} {'HOOPS':>8} {'FT':>8} {'Diff':>8} Status")
    print(f"  {'-'*16} {'-'*8} {'-'*8} {'-'*8} {'-'*6}")
    for cat, data in sorted(cr['by_category'].items()):
        diff = data['diff']
        status = "OK" if diff == 0 else f"FT+{diff}" if diff > 0 else f"HOOPS+{-diff}"
        print(f"  {cat:<16} {data['hoops']:>8} {data['ft']:>8} {diff:>+8} {status}")

    print(f"\n  HOOPS raw types:")
    for t, n in sorted(cr['hoops_raw'].items()):
        print(f"    {t}: {n}")
    print(f"\n  FT raw types:")
    for t, n in sorted(cr['ft_raw'].items()):
        print(f"    {t}: {n}")

    # Geometric matching
    gr = result['geometric']
    print(f"\n### Geometric Matching")
    print(f"  Matched:   {gr['total_matched']}")
    print(f"  FT extra:  {gr['total_ft_extra']}  (in FT but not matched in HOOPS)")
    print(f"  HOOPS missing: {gr['total_hoops_missing']}  (in HOOPS but not matched in FT)")

    if verbose and gr['by_layer_category']:
        print(f"\n  By layer/category:")
        for key, data in sorted(gr['by_layer_category'].items()):
            print(f"    {key:<30} H:{data['hoops']:>4} F:{data['ft']:>4} matched:{data['matched']:>4} h_miss:{data['hoops_missing']:>3} f_extra:{data['ft_extra']:>3}")


import io
import sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')


def main():
    print("[DEPRECATED] compare_hoops.py is deprecated. Use compare_structural.py instead.")
    print()

    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <hoops.json> <ft.json> [--verbose]")
        sys.exit(2)

    hoops_path = sys.argv[1]
    ft_path = sys.argv[2]
    verbose = '--verbose' in sys.argv

    result = run_comparison(hoops_path, ft_path, verbose)
    print_report(result, verbose)

    # Exit code: 0 if layers match and entity counts are reasonable
    layers_ok = result['layers']['match']
    count_ratio = result['entity_counts']['ratio_pct']
    adjusted_ratio = result['entity_counts'].get('adjusted_ratio_pct', count_ratio)
    # Use adjusted ratio (excl. INSERT) when FT has INSERTs, otherwise raw ratio
    effective_ratio = adjusted_ratio if result['entity_counts'].get('ft_insert_count', 0) > 0 else count_ratio
    # Consider reasonable if effective ratio is 50-250% of HOOPS
    counts_ok = 40 <= effective_ratio <= 250

    if layers_ok and counts_ok:
        print("\n[OK] OVERALL: LAYERS MATCH, counts in reasonable range")
        sys.exit(0)
    else:
        print(f"\n[WARN] OVERALL: ISSUES FOUND (layers={'OK' if layers_ok else 'MISMATCH'}, ratio={effective_ratio}%)")
        sys.exit(1)


if __name__ == '__main__':
    main()
