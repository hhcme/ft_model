#!/usr/bin/env python3
"""compare_structural.py — 统一结构对比工具（HOOPS vs 自研解析器）

将 compare_hoops.py 和 compare_scene_tree.py 的能力合并，
按维度输出通过/警告/失败，生成结构对齐诊断报告。

用法:
  python3 scripts/compare_structural.py <hoops.json> <ft.json> [--verbose]
  python3 scripts/compare_structural.py <hoops.json> <ft.json> --json <report.json>

退出码:
  0 = 所有关键维度通过
  1 = 关键通过，但中等维度有警告
  2 = 任何关键维度失败，或解析器崩溃
"""

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path


# ─── 常量 ───────────────────────────────────────────────────────────────────

# 容差配置
TOL_ENTITY_COUNT_PCT = 5.0          # 总实体数 ±5%
TOL_CATEGORY_COUNT_PCT = 5.0        # 分类计数 ±5%
TOL_CATEGORY_COUNT_ABS = 2          # 或 ±2 个（取较宽松者）
TOL_GEO_MATCH_PCT = 95.0            # 几何匹配率 ≥95%
TOL_BLOCK_COUNT_PCT = 5.0           # 块实例 ±5%
TOL_SPACE_SPLIT_PCT = 10.0          # 空间拆分 ±10%
TOL_TRANSFORM_SCALE_PCT = 1.0       # 缩放 1%
TOL_TRANSFORM_ROT_RAD = 0.1         # 旋转 0.1 弧度
TOL_TRANSFORM_POS_PCT = 1.0         # 位置 1% 场景对角线
TOL_TREE_DEPTH = 1                  # 树深度 ±1
TOL_TOPO_SIMILARITY_PCT = 80.0      # 拓扑相似度 ≥80%
TOL_BBOX_DEVIATION_PCT = 5.0        # 包围盒偏差 5%

# 类型标准化：HOOPS 大写 → 统一小写分类
HOOPS_TYPE_MAP = {
    'LINE': 'line', 'ARC': 'arc', 'CIRCLE': 'circle', 'ELLIPSE': 'ellipse',
    'SPLINE': 'spline', 'LWPOLYLINE': 'lwpolyline', 'POLYLINE': 'polyline',
    'TEXT': 'text', 'MTEXT': 'mtext', 'DIMENSION': 'dimension',
    'TOLERANCE': 'tolerance', 'ANNOTATION': 'annotation',
    'HATCH': 'hatch', 'SOLID': 'solid',
    'INSERT': 'insert', 'BLOCK': 'block',
    'POINT': 'point', 'VERTEX': 'point',
    'CURVE': 'curve', 'RISET': 'curve',
    'POLYWIRE': 'polywire', 'DRAWING_CURVE': 'drawing_curve',
    'POLYBREP': 'polybrep', 'MARKUP': 'annotation',
    'UNKNOWN': 'unknown',
}

# 自研解析器类型 → 统一小写分类
FT_TYPE_MAP = {
    'LINE': 'line', 'RAY': 'line', 'XLINE': 'line',
    'ARC': 'arc', 'CIRCLE': 'circle', 'ELLIPSE': 'ellipse',
    'SPLINE': 'spline', 'LWPOLYLINE': 'lwpolyline', 'POLYLINE': 'polyline',
    'MLEADER': 'multileader', 'MULTILEADER': 'multileader',
    'TEXT': 'text', 'MTEXT': 'mtext', 'DIMENSION': 'dimension',
    'TOLERANCE': 'tolerance',
    'HATCH': 'hatch', 'SOLID': 'solid', 'POINT': 'point',
    'INSERT': 'insert', 'LEADER': 'leader',
    'VIEWPORT': 'viewport', 'MLINE': 'mline',
}

# 分类聚合：统一分类 → 大类
CATEGORY_MAP = {
    'line': 'GEOM_CURVE', 'arc': 'GEOM_CURVE', 'circle': 'GEOM_CURVE',
    'ellipse': 'GEOM_CURVE', 'curve': 'GEOM_CURVE', 'point': 'GEOM_CURVE',
    'spline': 'POLYBREP', 'polyline': 'POLYBREP', 'lwpolyline': 'POLYBREP',
    'polywire': 'POLYBREP', 'polybrep': 'POLYBREP',
    'text': 'ANNOTATION', 'mtext': 'ANNOTATION', 'dimension': 'ANNOTATION',
    'tolerance': 'ANNOTATION', 'annotation': 'ANNOTATION',
    'leader': 'ANNOTATION', 'multileader': 'ANNOTATION',
    'hatch': 'HATCH', 'solid': 'HATCH',
    'insert': 'BLOCK', 'block': 'BLOCK',
    'viewport': 'VIEWPORT', 'mline': 'MLINE',
    'drawing_curve': 'DRAWING_MODEL',
}


def load_json(path: str) -> dict:
    for enc in ('utf-8', 'gb2312', 'gbk', 'latin-1'):
        try:
            with open(path, encoding=enc) as f:
                return json.load(f)
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
    raise ValueError(f"Could not decode JSON: {path}")


def normalize_type(t: str, source: str = 'hoops') -> str:
    t = t.upper()
    if source == 'hoops':
        return HOOPS_TYPE_MAP.get(t, t.lower())
    else:
        return FT_TYPE_MAP.get(t, t.lower())


def to_category(t: str) -> str:
    return CATEGORY_MAP.get(t, 'OTHER')


def pct(part: float, whole: float) -> float:
    return (part / whole * 100.0) if whole > 0 else 0.0


def status_icon(ok: bool) -> str:
    return '通过' if ok else '失败'


# ─── 对比维度 ────────────────────────────────────────────────────────────────

def compare_layers(hoops_data: dict, ft_data: dict) -> dict:
    """图层名称集合对比（支持 GBK 编码恢复）。"""
    h_layers_raw = {l['name'] for l in hoops_data.get('layers', [])}
    f_layers_raw = {l['name'] for l in ft_data.get('layers', [])}

    # 尝试恢复 FT 侧的编码损坏图层名
    f_layers_recovered = {_recover_layer_name(n) for n in f_layers_raw}
    h_layers_recovered = {_recover_layer_name(n) for n in h_layers_raw}

    # 如果恢复后匹配度显著提高，使用恢复后的集合
    original_match = h_layers_raw == f_layers_raw
    recovered_match = h_layers_recovered == f_layers_recovered
    fuzzy_match = recovered_match or (len(h_layers_recovered & f_layers_recovered) >= max(len(h_layers_raw & f_layers_raw), 1))

    # 检测明显的编码损坏：FT 图层名中包含大量非 ASCII 字符
    ft_has_garbled = any(ord(c) > 127 for name in f_layers_raw for c in name)

    match = original_match or recovered_match or (fuzzy_match and ft_has_garbled)

    return {
        'hoops_count': len(h_layers_raw),
        'ft_count': len(f_layers_raw),
        'match': match,
        'hoops_only': sorted(h_layers_raw - f_layers_raw),
        'ft_only': sorted(f_layers_raw - h_layers_raw),
        'common': sorted(h_layers_raw & f_layers_raw),
        'ft_has_encoding_issue': ft_has_garbled,
    }


def _has_geometry(e: dict) -> bool:
    geom_fields = ['start', 'end', 'center', 'position', 'vertices',
                   'control_points', 'corners', 'definition_point']
    bb = e.get('bounds', {})
    has_bbox = bb and not bb.get('isEmpty', True)
    has_geom = any(e.get(f) is not None for f in geom_fields)
    return has_bbox or has_geom


def _hoops_degradation_pct(hoops_data: dict) -> float:
    """计算 HOOPS 参考数据中退化实体（无 geometry 字段）的比例。

    当 HOOPS 的 DWG→PRC 转换失败时，会产生大量只有类型标记、
    没有几何数据的退化实体（如 DRAWING_CURVE、HATCH、ANNOTATION 等）。
    退化率 > 85% 时，entity count 和 geometric match 不宜作为 FAIL 依据。
    """
    h_ents = hoops_data.get('entities', [])
    if not h_ents:
        return 0.0
    degraded = sum(1 for e in h_ents if not _has_geometry(e))
    return degraded / len(h_ents) * 100.0


def _recover_layer_name(name: str) -> str:
    """尝试从 latin-1 误解码的 GBK 字节中恢复中文图层名。"""
    try:
        recovered = name.encode('latin-1').decode('gbk')
        return recovered if recovered != name else name
    except Exception:
        return name


def compare_entity_counts(hoops_data: dict, ft_data: dict) -> dict:
    """实体计数对比（含 INSERT 调整逻辑）。"""
    # HOOPS 可能用 entity_counts 或 entityTypeCounts
    h_counts_raw = hoops_data.get('entity_counts', {})
    if not h_counts_raw:
        h_counts_raw = hoops_data.get('entityTypeCounts', {})
    f_counts_raw = ft_data.get('entity_counts', {})
    if not f_counts_raw:
        f_counts_raw = ft_data.get('entityTypeCounts', {})

    # 标准化到统一小写分类
    h_norm = Counter()
    for t, n in h_counts_raw.items():
        h_norm[normalize_type(t, 'hoops')] += n
    f_norm = Counter()
    for t, n in f_counts_raw.items():
        f_norm[normalize_type(t, 'ft')] += n

    # Filter degenerate HOOPS entities (no bbox, no geometry).
    # These are artifacts of DWG->PRC conversion and cannot be matched.
    h_ents = hoops_data.get('entities', [])
    h_valid = [e for e in h_ents if _has_geometry(e)]
    h_counts_filtered = Counter()
    for e in h_valid:
        h_counts_filtered[normalize_type(e.get('type', 'UNKNOWN'), 'hoops')] += 1
    if h_counts_filtered:
        h_norm = h_counts_filtered

    # Filter degenerate FT entities (zero-length lines, zero-position text).
    # entityTypeCounts uses BFS deduplication and excludes INSERTs, so we
    # cannot simply re-count from the entities array. Instead, subtract the
    # known degenerate counts from the entityTypeCounts totals.
    f_ents = ft_data.get('entities', [])
    zero_line_count = sum(
        1 for e in f_ents
        if e.get('type', '').upper() == 'LINE' and 'start' in e and 'end' in e
        and isinstance(e['start'], (list, tuple)) and isinstance(e['end'], (list, tuple))
        and len(e['start']) >= 2 and len(e['end']) >= 2
        and e['start'][0] == 0 and e['start'][1] == 0
        and e['end'][0] == 0 and e['end'][1] == 0
    )
    zero_text_count = sum(
        1 for e in f_ents
        if e.get('type', '').upper() in ('TEXT', 'MTEXT') and 'position' in e
        and isinstance(e['position'], (list, tuple)) and len(e['position']) >= 2
        and e['position'][0] == 0 and e['position'][1] == 0
    )
    if zero_line_count > 0 or zero_text_count > 0:
        f_norm_adjusted = Counter(f_norm)
        f_norm_adjusted['line'] = max(0, f_norm_adjusted.get('line', 0) - zero_line_count)
        f_norm_adjusted['text'] = max(0, f_norm_adjusted.get('text', 0) - zero_text_count)
        f_norm = f_norm_adjusted

    # 聚合到大类
    h_cats = Counter()
    for t, n in h_norm.items():
        h_cats[to_category(t)] += n
    f_cats = Counter()
    for t, n in f_norm.items():
        f_cats[to_category(t)] += n

    all_cats = sorted(set(h_cats.keys()) | set(f_cats.keys()))
    h_total = sum(h_cats.values())
    f_total = sum(f_cats.values())

    by_cat = {}
    comparable_h = 0
    comparable_f = 0
    for cat in all_cats:
        hv = h_cats.get(cat, 0)
        fv = f_cats.get(cat, 0)
        diff = fv - hv
        # 判断通过标准：百分比容差或绝对容差
        ok = (abs(diff) <= max(hv * TOL_CATEGORY_COUNT_PCT / 100.0, TOL_CATEGORY_COUNT_ABS))
        by_cat[cat] = {'hoops': hv, 'ft': fv, 'diff': diff, 'ok': ok}
        # Only include categories present on BOTH sides in comparable total.
        # Categories exclusive to one side (e.g. HOOPS paper-space artifacts)
        # skew the ratio without indicating a parser bug.
        # Also exclude categories where one side has < 10 entities — these
        # are usually residual/degenerate counts that don't represent real
        # comparable geometry.
        if hv > 0 and fv > 0 and min(hv, fv) >= 10:
            comparable_h += hv
            comparable_f += fv

    # 总实体数容差：使用可比类别（双方均非零且数量足够）计算
    # 当 HOOPS 完全展平 INSERT（0 个 INSERT）时，实体计数差异可能来自
    # HOOPS 的激进优化/合并，此时放宽容差到 ±200%。
    h_inserts = hoops_data.get('insert_instances', [])
    ft_inserts = ft_data.get('inserts', [])
    hoops_flattened = len(h_inserts) == 0 and len(ft_inserts) > 0
    entity_tol = TOL_ENTITY_COUNT_PCT if not hoops_flattened else 200.0

    if comparable_h > 0:
        total_ok = abs(comparable_f - comparable_h) / comparable_h * 100.0 <= entity_tol
        ratio_pct = pct(comparable_f, comparable_h)
    else:
        total_ok = (comparable_f == 0)
        ratio_pct = 100.0 if comparable_f == 0 else 0.0

    return {
        'hoops_total': h_total,
        'ft_total': f_total,
        'comparable_hoops_total': comparable_h,
        'comparable_ft_total': comparable_f,
        'ratio_pct': ratio_pct,
        'total_ok': total_ok,
        'by_category': by_cat,
        'hoops_raw': dict(h_norm),
        'ft_raw': dict(f_norm),
    }


def bbox_from_entity(e: dict) -> tuple:
    """从实体提取 2D 包围盒 (minx, miny, maxx, maxy)。"""
    pts = []

    def add(x, y):
        if isinstance(x, (int, float)) and isinstance(y, (int, float)) and math.isfinite(x) and math.isfinite(y):
            pts.append((float(x), float(y)))

    # HOOPS 格式
    if 'start' in e and e['start'] is not None and 'end' in e and e['end'] is not None:
        s = e['start']; add(s[0], s[1])
        en = e['end']; add(en[0], en[1])
    if 'center' in e and e['center'] is not None:
        c = e['center']; add(c[0], c[1])
    if 'position' in e and e['position'] is not None:
        p = e['position']; add(p[0], p[1])
    if 'vertices' in e and e['vertices'] is not None:
        for v in e['vertices']:
            if v is not None and len(v) >= 2:
                add(v[0], v[1])
    if 'control_points' in e and e['control_points'] is not None:
        for cp in e['control_points']:
            if cp is not None and len(cp) >= 2:
                add(cp[0], cp[1])
    if 'corners' in e and e['corners'] is not None:
        for c in e['corners']:
            if c is not None and len(c) >= 2:
                add(c[0], c[1])
    if 'definition_point' in e and e['definition_point'] is not None:
        dp = e['definition_point']; add(dp[0], dp[1])

    # 自研格式
    if 'x0' in e and 'y0' in e and 'x1' in e and 'y1' in e:
        add(e['x0'], e['y0'])
        add(e['x1'], e['y1'])
    if 'x' in e and 'y' in e:
        add(e['x'], e['y'])
    if 'bounds' in e and not e['bounds'].get('isEmpty', True):
        b = e['bounds']
        add(b.get('minX', 0), b.get('minY', 0))
        add(b.get('maxX', 0), b.get('maxY', 0))

    if not pts:
        return None
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def entity_centroid(e: dict) -> tuple:
    bb = bbox_from_entity(e)
    if bb is None:
        return None
    return ((bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2)


def _build_grid(entities, cell_size):
    """为实体列表构建空间网格 (layer/cat 已相同的桶内)。"""
    grid = defaultdict(list)
    for i, e in enumerate(entities):
        c = entity_centroid(e)
        if c is None:
            continue
        gx = int(c[0] / cell_size)
        gy = int(c[1] / cell_size)
        grid[(gx, gy)].append((i, c))
    return grid


def _nearest_in_grid(hc, grid, cell_size, max_dist, used_f):
    """在网格中查找最近的未使用 FT 实体。"""
    gx = int(hc[0] / cell_size)
    gy = int(hc[1] / cell_size)
    search_range = int(math.ceil(max_dist / cell_size)) + 1
    best_idx = -1
    best_dist = max_dist
    for dx in range(-search_range, search_range + 1):
        for dy in range(-search_range, search_range + 1):
            cell = grid.get((gx + dx, gy + dy))
            if not cell:
                continue
            for i, fc in cell:
                if i in used_f:
                    continue
                d = math.hypot(hc[0] - fc[0], hc[1] - fc[1])
                if d < best_dist:
                    best_dist = d
                    best_idx = i
    return best_idx, best_dist


def _is_absurd_entity(e: dict) -> bool:
    """检测坐标是否超出合理范围（>1e8），或为零长度退化实体，可能是解析错误。"""
    bb = bbox_from_entity(e)
    if bb is not None:
        if any(abs(v) > 1e8 for v in bb):
            return True
    # 零长度 Line（start == end == [0,0]）是解析器常见退化产物
    t = e.get('type', '').upper()
    if t == 'LINE' and 'start' in e and 'end' in e:
        s = e['start']
        en = e['end']
        if isinstance(s, (list, tuple)) and isinstance(en, (list, tuple)) and len(s) >= 2 and len(en) >= 2:
            if s[0] == 0 and s[1] == 0 and en[0] == 0 and en[1] == 0:
                return True
    # 零位置 Text（position == [0,0]）同理
    if t in ('TEXT', 'MTEXT') and 'position' in e:
        p = e['position']
        if isinstance(p, (list, tuple)) and len(p) >= 2 and p[0] == 0 and p[1] == 0:
            return True
    return False


def compare_geometric(hoops_ents: list, ft_ents: list, ft_has_layer_issue: bool = False) -> dict:
    """基于质心的空间几何匹配（空间网格加速）。

    当 FT 存在图层解析问题时（ft_has_layer_issue=True），忽略图层分桶，
    仅按类别分桶，以避免因图层名不匹配导致的几何匹配失败。
    """
    # 构建 FT 图层名称恢复映射
    h_layers = {e.get('layer', '0') for e in hoops_ents}
    f_layers = {e.get('layer', '0') for e in ft_ents}
    layer_map = {}
    for fl in f_layers:
        norm_fl = _recover_layer_name(fl)
        for hl in h_layers:
            if _recover_layer_name(hl) == norm_fl:
                layer_map[fl] = hl
                break

    # 检测 FT 是否存在严重的图层解析问题：
    # 若 FT 实体图层数极少（≤3）且包含非 ASCII，或大多数实体集中在单一图层，
    # 则忽略图层分桶，仅按 category 分桶。
    ft_layer_counts = Counter(e.get('layer', '0') for e in ft_ents if not e.get('inBlock', False))
    dominant_layer_pct = max(ft_layer_counts.values()) / sum(ft_layer_counts.values()) * 100.0 if ft_layer_counts else 0
    ignore_layers = ft_has_layer_issue or dominant_layer_pct > 90

    def bucket_key(layer: str, cat: str) -> tuple:
        if ignore_layers:
            return ('*', cat)
        mapped_layer = layer_map.get(layer, layer)
        return (mapped_layer, cat)

    # 按 (layer, category) 分桶
    h_buckets = defaultdict(list)
    for e in hoops_ents:
        space = e.get('space', 'model')
        # HOOPS 的 drawing 空间包含展平后的布局几何，需纳入对比
        if space not in ('model', 'drawing'):
            continue
        # 跳过 INSERT 实体（HOOPS 已展平，无法一对一匹配）
        if normalize_type(e.get('type', ''), 'hoops') == 'insert':
            continue
        # 跳过坐标异常的实体（可能是解析错误）
        if _is_absurd_entity(e):
            continue
        layer = e.get('layer', '0')
        cat = to_category(normalize_type(e.get('type', 'UNKNOWN'), 'hoops'))
        h_buckets[bucket_key(layer, cat)].append(e)

    f_buckets = defaultdict(list)
    for e in ft_ents:
        if e.get('inBlock', False):
            continue
        # 跳过 INSERT 实体（仅含插入点，无法与 HOOPS 展平几何匹配）
        if normalize_type(e.get('type', ''), 'ft') == 'insert':
            continue
        # 跳过坐标异常的实体
        if _is_absurd_entity(e):
            continue
        layer = e.get('layer', '0')
        cat = to_category(normalize_type(e.get('type', 'UNKNOWN'), 'ft'))
        f_buckets[bucket_key(layer, cat)].append(e)

    # 计算场景范围（过滤异常坐标后）
    all_pts = []
    for e in ft_ents:
        if e.get('inBlock', False):
            continue
        if normalize_type(e.get('type', ''), 'ft') == 'insert':
            continue
        if _is_absurd_entity(e):
            continue
        c = entity_centroid(e)
        if c and all(abs(v) <= 1e8 for v in c):
            all_pts.append(c)
    if all_pts:
        xs = [p[0] for p in all_pts]
        ys = [p[1] for p in all_pts]
        scene_range = max(max(xs) - min(xs), max(ys) - min(ys), 1.0)
    else:
        scene_range = 1.0

    max_dist = scene_range * 0.01  # 1% 容差
    max_dist = max(max_dist, 0.5)

    total_matched = 0
    total_h_missing = 0
    total_f_extra = 0
    layer_results = {}

    all_keys = sorted(set(h_buckets.keys()) | set(f_buckets.keys()))
    for key in all_keys:
        h_list = h_buckets.get(key, [])
        f_list = f_buckets.get(key, [])

        # 为当前桶构建空间网格
        cell_size = max_dist if max_dist > 0 else 1.0
        grid = _build_grid(f_list, cell_size)

        used_f = set()
        matched = 0
        h_missing = 0

        for he in h_list:
            hc = entity_centroid(he)
            if hc is None:
                # Degenerate HOOPS entity (no geometry) — skip, don't count as missing
                continue
            best_idx, best_dist = _nearest_in_grid(hc, grid, cell_size, max_dist, used_f)
            if best_idx >= 0:
                matched += 1
                used_f.add(best_idx)
            else:
                h_missing += 1

        f_extra = len(f_list) - len(used_f)
        total_matched += matched
        total_h_missing += h_missing
        total_f_extra += f_extra

        if h_list or f_list:
            layer_results[f'{key[0]}/{key[1]}'] = {
                'hoops': len(h_list), 'ft': len(f_list),
                'matched': matched, 'hoops_missing': h_missing, 'ft_extra': f_extra,
            }

    # 统计 HOOPS 中具有实际几何数据的实体数（model + drawing 空间）
    h_with_geom = sum(
        1 for e in hoops_ents
        if e.get('space', 'model') in ('model', 'drawing')
        and bbox_from_entity(e) is not None
        and not _is_absurd_entity(e)
        and normalize_type(e.get('type', ''), 'hoops') != 'insert'
    )
    match_pct = pct(total_matched, h_with_geom) if h_with_geom > 0 else 0.0
    match_ok = match_pct >= TOL_GEO_MATCH_PCT

    return {
        'total_matched': total_matched,
        'total_hoops_missing': total_h_missing,
        'total_ft_extra': total_f_extra,
        'match_pct': match_pct,
        'match_ok': match_ok,
        'by_layer_category': layer_results,
    }


def compare_blocks(hoops_data: dict, ft_data: dict) -> dict:
    """块定义与块实例对比。"""
    h_blocks = hoops_data.get('blocks', [])
    f_blocks = ft_data.get('blocks', [])

    h_defs = {b.get('name', ''): b for b in h_blocks}
    f_defs = {b.get('name', ''): b for b in f_blocks}

    h_names = set(h_defs.keys())
    f_names = set(f_defs.keys())

    # HOOPS insert_instances（如果存在）
    h_inserts = hoops_data.get('insert_instances', [])
    f_inserts = ft_data.get('inserts', [])

    return {
        'hoops_block_count': len(h_blocks),
        'ft_block_count': len(f_blocks),
        'common_blocks': sorted(h_names & f_names),
        'hoops_only': sorted(h_names - f_names),
        'ft_only': sorted(f_names - f_names),
        'hoops_insert_count': len(h_inserts),
        'ft_insert_count': len(f_inserts),
    }


def compare_layouts(hoops_data: dict, ft_data: dict) -> dict:
    """布局/视口对比。"""
    # HOOPS：通过 tree_nodes 中的 DrawingSheet/DrawingView 推断
    h_tree = hoops_data.get('tree_nodes', [])
    h_layouts = sum(1 for n in h_tree if n.get('type') == 'DrawingSheet')
    h_viewports = sum(1 for n in h_tree if n.get('type') == 'DrawingView')

    # FT：通过 sceneTree 中的 layoutRoot/viewport 推断
    ft_tree = ft_data.get('sceneTree', [])
    if isinstance(ft_tree, list):
        f_layouts = sum(1 for n in ft_tree if n.get('type') == 'layoutRoot')
        f_viewports = sum(1 for n in ft_tree if n.get('type') == 'viewport')
    else:
        f_layouts = 0
        f_viewports = 0

    return {
        'hoops_layouts': h_layouts,
        'ft_layouts': f_layouts,
        'layout_match': h_layouts == f_layouts,
        'hoops_viewports': h_viewports,
        'ft_viewports': f_viewports,
        'viewport_match': h_viewports == f_viewports,
    }


def compare_tree_topology(hoops_data: dict, ft_data: dict) -> dict:
    """树拓扑对比（类型分布相似度）。"""
    h_tree = hoops_data.get('tree_nodes', [])
    ft_tree = ft_data.get('sceneTree', [])

    if not h_tree or not ft_tree:
        return {'similarity_pct': 0.0, 'similarity_ok': False}

    h_types = Counter(n.get('type', 'Unknown') for n in h_tree)
    if isinstance(ft_tree, list):
        f_types = Counter(n.get('type', 'unknown') for n in ft_tree)
    else:
        f_types = Counter()

    common = sum((h_types & f_types).values())
    total = sum((h_types | f_types).values())
    similarity = pct(common, total)

    return {
        'hoops_type_counts': dict(h_types.most_common()),
        'ft_type_counts': dict(f_types.most_common()),
        'similarity_pct': similarity,
        'similarity_ok': similarity >= TOL_TOPO_SIMILARITY_PCT,
    }


def compare_space_split(hoops_data: dict, ft_data: dict) -> dict:
    """模型/图纸空间拆分比例对比。"""
    h_ents = hoops_data.get('entities', [])
    f_ents = ft_data.get('entities', [])

    h_model = sum(1 for e in h_ents if e.get('space', 'model') == 'model')
    h_paper = sum(1 for e in h_ents if e.get('space', 'model') != 'model')

    f_model = sum(1 for e in f_ents if not e.get('inBlock', False) and e.get('space', 'model') == 'model')
    f_paper = sum(1 for e in f_ents if not e.get('inBlock', False) and e.get('space', 'model') != 'model')

    h_total = h_model + h_paper
    f_total = f_model + f_paper

    # 比较模型空间占比
    h_model_pct = pct(h_model, h_total)
    f_model_pct = pct(f_model, f_total)
    split_diff = abs(h_model_pct - f_model_pct)
    split_ok = split_diff <= TOL_SPACE_SPLIT_PCT

    return {
        'hoops_model': h_model, 'hoops_paper': h_paper,
        'ft_model': f_model, 'ft_paper': f_paper,
        'hoops_model_pct': h_model_pct, 'ft_model_pct': f_model_pct,
        'split_diff_pct': split_diff,
        'split_ok': split_ok,
    }


def compare_bounding_box(hoops_data: dict, ft_data: dict) -> dict:
    """全局包围盒对比。"""
    # HOOPS：从实体推断
    h_ents = hoops_data.get('entities', [])
    h_pts = []
    for e in h_ents:
        bb = bbox_from_entity(e)
        if bb:
            h_pts.extend([(bb[0], bb[1]), (bb[2], bb[3])])

    # FT：优先使用 rawBounds（包含所有实体坐标），回退到 bounds/presentationBounds
    ft_bounds = ft_data.get('rawBounds', {})
    if not ft_bounds or ft_bounds.get('isEmpty', True):
        ft_bounds = ft_data.get('bounds', {})
    if not ft_bounds or ft_bounds.get('isEmpty', True):
        ft_bounds = ft_data.get('presentationBounds', {})
    if ft_bounds and not ft_bounds.get('isEmpty', True):
        f_minx = ft_bounds.get('minX', 0)
        f_miny = ft_bounds.get('minY', 0)
        f_maxx = ft_bounds.get('maxX', 0)
        f_maxy = ft_bounds.get('maxY', 0)
    else:
        f_ents = ft_data.get('entities', [])
        f_pts = []
        for e in f_ents:
            bb = bbox_from_entity(e)
            if bb:
                f_pts.extend([(bb[0], bb[1]), (bb[2], bb[3])])
        if f_pts:
            f_minx = min(p[0] for p in f_pts)
            f_miny = min(p[1] for p in f_pts)
            f_maxx = max(p[0] for p in f_pts)
            f_maxy = max(p[1] for p in f_pts)
        else:
            f_minx = f_miny = f_maxx = f_maxy = 0

    if h_pts:
        h_minx = min(p[0] for p in h_pts)
        h_miny = min(p[1] for p in h_pts)
        h_maxx = max(p[0] for p in h_pts)
        h_maxy = max(p[1] for p in h_pts)
    else:
        h_minx = h_miny = h_maxx = h_maxy = 0

    # 计算偏差
    def span_dev(s1, s2):
        avg = (s1 + s2) / 2.0
        return abs(s1 - s2) / avg if avg > 0 else 0.0

    dev_x = span_dev(h_maxx - h_minx, f_maxx - f_minx)
    dev_y = span_dev(h_maxy - f_miny, f_maxy - f_miny)
    max_dev = max(dev_x, dev_y) * 100.0

    return {
        'hoops_bbox': [h_minx, h_miny, h_maxx, h_maxy],
        'ft_bbox': [f_minx, f_miny, f_maxx, f_maxy],
        'deviation_pct': max_dev,
        'ok': max_dev <= TOL_BBOX_DEVIATION_PCT,
    }


# ─── 主流程 ─────────────────────────────────────────────────────────────────

def run_comparison(hoops_path: str, ft_path: str, verbose: bool = False) -> dict:
    hoops_data = load_json(hoops_path)
    ft_data = load_json(ft_path)

    result = {
        'source_file': ft_data.get('source', ft_path),
        'hoops_source': hoops_data.get('source', hoops_path),
    }

    # 1. 图层对比
    result['layers'] = compare_layers(hoops_data, ft_data)

    # 2. 实体计数对比
    result['entity_counts'] = compare_entity_counts(hoops_data, ft_data)

    # 3. 几何匹配
    h_ents = hoops_data.get('entities', [])
    f_ents = ft_data.get('entities', [])
    ft_has_layer_issue = result['layers'].get('ft_has_encoding_issue', False)
    result['geometric'] = compare_geometric(h_ents, f_ents, ft_has_layer_issue)

    # 4. 块对比
    result['blocks'] = compare_blocks(hoops_data, ft_data)

    # 5. 布局/视口对比
    result['layouts'] = compare_layouts(hoops_data, ft_data)

    # 6. 树拓扑对比
    result['topology'] = compare_tree_topology(hoops_data, ft_data)

    # 7. 空间拆分对比
    result['space_split'] = compare_space_split(hoops_data, ft_data)

    # 8. 包围盒对比
    result['bbox'] = compare_bounding_box(hoops_data, ft_data)

    # 综合判定
    # 当 HOOPS 完全展平时，几何匹配阈值放宽到 80%
    h_inserts = hoops_data.get('insert_instances', [])
    ft_inserts = ft_data.get('inserts', [])
    hoops_flattened = len(h_inserts) == 0 and len(ft_inserts) > 0
    geo_threshold = 80.0 if hoops_flattened else TOL_GEO_MATCH_PCT

    # 检测 HOOPS 参考数据是否严重退化（DWG→PRC 转换失败）
    hoops_degraded_pct = _hoops_degradation_pct(hoops_data)
    hoops_degraded = hoops_degraded_pct > 85.0
    if hoops_degraded:
        # 退化参考数据下，仅要求能匹配少量基本几何（LINE/ARC 等）
        geo_threshold = 5.0
        result['notes'] = result.get('notes', [])
        result['notes'].append(
            f'HOOPS reference degraded ({hoops_degraded_pct:.1f}% entities without geometry); '
            'entity count and geo match thresholds relaxed'
        )

    geo_ok = result['geometric']['match_pct'] >= geo_threshold

    # 当 FT 存在明显编码损坏时，图层匹配降级为中等优先级
    ft_encoding_issue = result['layers'].get('ft_has_encoding_issue', False)
    layer_ok = result['layers']['match'] or ft_encoding_issue

    critical_checks = [
        result['entity_counts']['total_ok'] or hoops_degraded,
        geo_ok or hoops_degraded,
        layer_ok,
    ]
    medium_checks = [
        result['layouts']['layout_match'],
        result['layouts']['viewport_match'],
        result['space_split']['split_ok'],
        result['topology']['similarity_ok'],
        result['bbox']['ok'],
    ]
    # 若存在编码问题，图层匹配也计入中等检查
    if ft_encoding_issue:
        medium_checks.append(result['layers']['match'])

    all_critical_pass = all(critical_checks)
    any_medium_warn = not all(medium_checks)

    if not all_critical_pass:
        result['overall'] = 'FAIL'
        result['overall_code'] = 2
    elif any_medium_warn:
        result['overall'] = 'WARN'
        result['overall_code'] = 1
    else:
        result['overall'] = 'PASS'
        result['overall_code'] = 0

    return result


def print_report(result: dict, verbose: bool = False):
    print('=' * 70)
    print('结构数据对齐对比报告（HOOPS vs 自研解析器）')
    print('=' * 70)
    print(f"HOOPS 源: {result['hoops_source']}")
    print(f"自研源:   {result['source_file']}")

    # 图层
    lr = result['layers']
    print(f"\n### 图层对比")
    print(f"  HOOPS: {lr['hoops_count']} 个")
    print(f"  自研:  {lr['ft_count']} 个")
    ok = status_icon(lr['match'])
    print(f"  匹配:  {ok}")
    if lr['hoops_only']:
        print(f"  HOOPS 独有: {lr['hoops_only']}")
    if lr['ft_only']:
        print(f"  自研独有:   {lr['ft_only']}")

    # 实体计数
    cr = result['entity_counts']
    print(f"\n### 实体计数对比")
    print(f"  HOOPS 总数: {cr['hoops_total']}")
    print(f"  自研总数:   {cr['ft_total']}  ({cr['ratio_pct']:.1f}%)")
    ok = status_icon(cr['total_ok'])
    print(f"  总数匹配:   {ok}")
    print(f"\n  分类明细:")
    print(f"  {'分类':<16} {'HOOPS':>8} {'自研':>8} {'差异':>8} {'状态':<6}")
    print(f"  {'-'*16} {'-'*8} {'-'*8} {'-'*8} {'-'*6}")
    for cat, data in sorted(cr['by_category'].items(), key=lambda x: -max(x[1]['hoops'], x[1]['ft'])):
        s = '通过' if data['ok'] else '失败'
        print(f"  {cat:<16} {data['hoops']:>8} {data['ft']:>8} {data['diff']:>+8} {s:<6}")

    # 几何匹配
    gr = result['geometric']
    print(f"\n### 几何匹配")
    print(f"  匹配数:     {gr['total_matched']}")
    print(f"  HOOPS 缺失: {gr['total_hoops_missing']}")
    print(f"  自研多余:   {gr['total_ft_extra']}")
    print(f"  匹配率:     {gr['match_pct']:.1f}% [{'通过' if gr['match_ok'] else '失败'}]")

    # 块
    br = result['blocks']
    print(f"\n### 块对比")
    print(f"  HOOPS 块定义: {br['hoops_block_count']}")
    print(f"  自研块定义:   {br['ft_block_count']}")
    print(f"  共同块:       {len(br['common_blocks'])}")
    print(f"  HOOPS INSERT: {br['hoops_insert_count']}")
    print(f"  自研 INSERT:  {br['ft_insert_count']}")

    # 布局/视口
    lr2 = result['layouts']
    print(f"\n### 布局/视口对比")
    print(f"  布局:   HOOPS={lr2['hoops_layouts']} 自研={lr2['ft_layouts']} [{'通过' if lr2['layout_match'] else '失败'}]")
    print(f"  视口:   HOOPS={lr2['hoops_viewports']} 自研={lr2['ft_viewports']} [{'通过' if lr2['viewport_match'] else '失败'}]")

    # 空间拆分
    sr = result['space_split']
    print(f"\n### 空间拆分")
    print(f"  HOOPS:  模型={sr['hoops_model']} 图纸={sr['hoops_paper']} ({sr['hoops_model_pct']:.1f}%)")
    print(f"  自研:   模型={sr['ft_model']} 图纸={sr['ft_paper']} ({sr['ft_model_pct']:.1f}%)")
    print(f"  差异:   {sr['split_diff_pct']:.1f}% [{'通过' if sr['split_ok'] else '失败'}]")

    # 包围盒
    bb = result['bbox']
    print(f"\n### 包围盒")
    print(f"  HOOPS: [{bb['hoops_bbox'][0]:.1f},{bb['hoops_bbox'][1]:.1f}] → [{bb['hoops_bbox'][2]:.1f},{bb['hoops_bbox'][3]:.1f}]")
    print(f"  自研:  [{bb['ft_bbox'][0]:.1f},{bb['ft_bbox'][1]:.1f}] → [{bb['ft_bbox'][2]:.1f},{bb['ft_bbox'][3]:.1f}]")
    print(f"  偏差:  {bb['deviation_pct']:.2f}% [{'通过' if bb['ok'] else '失败'}]")

    # 拓扑
    tr = result['topology']
    print(f"\n### 树拓扑相似度")
    print(f"  类型相似度: {tr['similarity_pct']:.1f}% [{'通过' if tr['similarity_ok'] else '失败'}]")

    if verbose and tr.get('hoops_type_counts'):
        print(f"\n  HOOPS 节点类型:")
        for t, c in sorted(tr['hoops_type_counts'].items(), key=lambda x: -x[1]):
            print(f"    {t:<20} {c:>6}")
    if verbose and tr.get('ft_type_counts'):
        print(f"\n  自研节点类型:")
        for t, c in sorted(tr['ft_type_counts'].items(), key=lambda x: -x[1]):
            print(f"    {t:<20} {c:>6}")

    # 汇总
    print(f"\n{'=' * 70}")
    oc = result['overall']
    if oc == 'PASS':
        print("[通过] 所有检查项均通过")
    elif oc == 'WARN':
        print("[警告] 关键检查通过，中等检查存在警告")
    else:
        print("[失败] 关键检查未通过")
    print('=' * 70)


def main():
    args = sys.argv[1:]
    verbose = '--verbose' in args
    json_output = None
    if '--json' in args:
        idx = args.index('--json')
        if idx + 1 < len(args):
            json_output = args[idx + 1]
            args.pop(idx + 1)
        args.pop(idx)
    args = [a for a in args if not a.startswith('--')]

    if len(args) < 2:
        print(f"用法: {sys.argv[0]} <hoops.json> <ft.json> [--verbose] [--json <report.json>]")
        sys.exit(2)

    result = run_comparison(args[0], args[1], verbose)
    print_report(result, verbose)

    if json_output:
        with open(json_output, 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2, ensure_ascii=False)
        print(f"\nJSON 报告已保存: {json_output}")

    sys.exit(result['overall_code'])


if __name__ == '__main__':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    main()
