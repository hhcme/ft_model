#!/usr/bin/env python3
"""svg_vector_compare.py — Vector-level SVG comparison for CAD rendering.

Compares two SVG files by extracting geometric primitives (circles, lines,
paths, rects, ellipses) and matching them spatially. Reports matched, missing,
extra, and property-level mismatches at 1e-3 tolerance — no rasterization.

Usage:
  python3 scripts/svg_vector_compare.py <reference.svg> <ours.svg> [--tol 1e-3]

Can also produce reference SVGs from DXF via ezdxf SVG backend:
  python3 scripts/svg_vector_compare.py --render-ref <input.dxf> -o ref.svg
"""

import argparse
import math
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path
from typing import Optional


def parse_svg_transform(transform_str: str):
    """Parse a simple SVG transform string (translate, scale, matrix)."""
    if not transform_str:
        return None
    m = re.match(r'matrix\(\s*([^)]+)\)', transform_str)
    if m:
        vals = [float(x) for x in m.group(1).replace(',', ' ').split()]
        if len(vals) == 6:
            return vals  # [a, b, c, d, e, f]
    return None


def apply_transform(point, matrix):
    """Apply a 2D affine matrix [a,b,c,d,e,f] to a point (x,y)."""
    if matrix is None:
        return point
    x, y = point
    a, b, c, d, e, f = matrix
    return (a * x + c * y + e, b * x + d * y + f)


class GeomElement:
    """A geometric element extracted from SVG."""
    def __init__(self, etype: str, props: dict, transform=None):
        self.etype = etype
        self.props = props
        self.transform = transform

    def characteristic_point(self):
        if self.etype == 'circle':
            return (self.props['cx'], self.props['cy'])
        elif self.etype == 'ellipse':
            return (self.props['cx'], self.props['cy'])
        elif self.etype == 'line':
            return ((self.props['x1'] + self.props['x2']) / 2,
                    (self.props['y1'] + self.props['y2']) / 2)
        elif self.etype == 'rect':
            return (self.props['x'] + self.props['width'] / 2,
                    self.props['y'] + self.props['height'] / 2)
        elif self.etype == 'path':
            pts = self.props.get('points', [])
            if pts:
                return pts[0]
        return (0, 0)

    def __repr__(self):
        pt = self.characteristic_point()
        return f"{self.etype}({pt[0]:.1f},{pt[1]:.1f})"


SVG_NS = '{http://www.w3.org/2000/svg}'


def extract_svg_elements(svg_path: str) -> list[GeomElement]:
    """Extract geometric elements from an SVG file."""
    tree = ET.parse(svg_path)
    root = tree.getroot()

    elements = []

    def _extract(node, parent_transform=None):
        tag = node.tag.replace(SVG_NS, '')
        transform_str = node.get('transform', '')
        local_transform = parse_svg_transform(transform_str)
        # Compose transforms if needed (for simplicity, just use local)

        if tag == 'circle':
            try:
                cx = float(node.get('cx', 0))
                cy = float(node.get('cy', 0))
                r = float(node.get('r', 0))
                if r > 0:
                    pt = apply_transform((cx, cy), local_transform or parent_transform)
                    elements.append(GeomElement('circle', {
                        'cx': pt[0], 'cy': pt[1], 'r': r
                    }))
            except (ValueError, TypeError):
                pass

        elif tag == 'ellipse':
            try:
                cx = float(node.get('cx', 0))
                cy = float(node.get('cy', 0))
                rx = float(node.get('rx', 0))
                ry = float(node.get('ry', 0))
                if rx > 0 and ry > 0:
                    pt = apply_transform((cx, cy), local_transform or parent_transform)
                    elements.append(GeomElement('ellipse', {
                        'cx': pt[0], 'cy': pt[1], 'rx': rx, 'ry': ry
                    }))
            except (ValueError, TypeError):
                pass

        elif tag == 'line':
            try:
                x1 = float(node.get('x1', 0))
                y1 = float(node.get('y1', 0))
                x2 = float(node.get('x2', 0))
                y2 = float(node.get('y2', 0))
                p1 = apply_transform((x1, y1), local_transform or parent_transform)
                p2 = apply_transform((x2, y2), local_transform or parent_transform)
                elements.append(GeomElement('line', {
                    'x1': p1[0], 'y1': p1[1], 'x2': p2[0], 'y2': p2[1]
                }))
            except (ValueError, TypeError):
                pass

        elif tag == 'rect':
            try:
                x = float(node.get('x', 0))
                y = float(node.get('y', 0))
                w = float(node.get('width', 0))
                h = float(node.get('height', 0))
                if w > 0 and h > 0:
                    pt = apply_transform((x, y), local_transform or parent_transform)
                    elements.append(GeomElement('rect', {
                        'x': pt[0], 'y': pt[1], 'width': w, 'height': h
                    }))
            except (ValueError, TypeError):
                pass

        elif tag == 'path':
            d = node.get('d', '')
            if d:
                points = parse_svg_path_points(d)
                if points:
                    elements.append(GeomElement('path', {
                        'points': points, 'command_count': len(d.split())
                    }))

        elif tag == 'polyline' or tag == 'polygon':
            pts_str = node.get('points', '')
            if pts_str:
                points = parse_points_attr(pts_str)
                if points:
                    elements.append(GeomElement('path' if tag == 'polyline' else 'polygon', {
                        'points': points
                    }))

        # Recurse into children
        for child in node:
            _extract(child, local_transform or parent_transform)

    _extract(root)
    return elements


def parse_svg_path_points(d: str) -> list[tuple]:
    """Extract coordinate points from SVG path data (d attribute)."""
    points = []
    # Match number patterns (including negatives and decimals)
    nums = re.findall(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?', d)
    for i in range(0, len(nums) - 1, 2):
        try:
            x, y = float(nums[i]), float(nums[i + 1])
            points.append((x, y))
        except (ValueError, IndexError):
            pass
    return points


def parse_points_attr(pts_str: str) -> list[tuple]:
    """Parse SVG points attribute (x1,y1 x2,y2 ...)."""
    points = []
    nums = re.findall(r'[-+]?(?:\d+\.?\d*|\.\d+)', pts_str)
    for i in range(0, len(nums) - 1, 2):
        try:
            points.append((float(nums[i]), float(nums[i + 1])))
        except (ValueError, IndexError):
            pass
    return points


def match_elements(ref: list[GeomElement], ours: list[GeomElement],
                   tol: float = 1e-3):
    """Greedy nearest-neighbor matching by type + spatial proximity."""
    by_type_ref = defaultdict(list)
    by_type_ours = defaultdict(list)
    for e in ref:
        by_type_ref[e.etype].append(e)
    for e in ours:
        by_type_ours[e.etype].append(e)

    max_dist = max(tol, 5.0)  # SVG units tolerance
    matched = []
    missing = []
    extra = []

    all_types = set(by_type_ref.keys()) | set(by_type_ours.keys())
    for t in all_types:
        ref_list = list(by_type_ref.get(t, []))
        our_list = list(by_type_ours.get(t, []))
        used = set()

        for re in ref_list:
            rp = re.characteristic_point()
            best_idx = -1
            best_dist = float('inf')
            for i, oe in enumerate(our_list):
                if i in used:
                    continue
                op = oe.characteristic_point()
                d = math.sqrt((rp[0] - op[0]) ** 2 + (rp[1] - op[1]) ** 2)
                if d < best_dist:
                    best_dist = d
                    best_idx = i
            if best_idx >= 0 and best_dist <= max_dist:
                matched.append((re, our_list[best_idx]))
                used.add(best_idx)
            else:
                missing.append(re)

        for i, oe in enumerate(our_list):
            if i not in used:
                extra.append(oe)

    return matched, missing, extra


def compare_element_props(ref: GeomElement, ours: GeomElement,
                          tol: float = 1e-3) -> list[dict]:
    """Compare properties of matched element pair."""
    diffs = []

    def check_num(name, rv, ov):
        if rv is None or ov is None:
            return
        if isinstance(rv, (list, tuple)):
            for i, (a, b) in enumerate(zip(rv, ov)):
                if abs(a - b) > tol:
                    diffs.append({"prop": f"{name}[{i}]", "ref": a, "ours": b})
        elif isinstance(rv, (int, float)):
            if abs(rv - ov) > tol:
                diffs.append({"prop": name, "ref": rv, "ours": ov})

    if ref.etype == 'circle':
        check_num('cx', ref.props.get('cx'), ours.props.get('cx'))
        check_num('cy', ref.props.get('cy'), ours.props.get('cy'))
        check_num('r', ref.props.get('r'), ours.props.get('r'))
    elif ref.etype == 'ellipse':
        check_num('cx', ref.props.get('cx'), ours.props.get('cx'))
        check_num('cy', ref.props.get('cy'), ours.props.get('cy'))
        check_num('rx', ref.props.get('rx'), ours.props.get('rx'))
        check_num('ry', ref.props.get('ry'), ours.props.get('ry'))
    elif ref.etype == 'line':
        check_num('x1', ref.props.get('x1'), ours.props.get('x1'))
        check_num('y1', ref.props.get('y1'), ours.props.get('y1'))
        check_num('x2', ref.props.get('x2'), ours.props.get('x2'))
        check_num('y2', ref.props.get('y2'), ours.props.get('y2'))
    elif ref.etype == 'rect':
        check_num('x', ref.props.get('x'), ours.props.get('x'))
        check_num('y', ref.props.get('y'), ours.props.get('y'))
        check_num('width', ref.props.get('width'), ours.props.get('width'))
        check_num('height', ref.props.get('height'), ours.props.get('height'))
    elif ref.etype in ('path', 'polygon'):
        rp = ref.props.get('points', [])
        op = ours.props.get('points', [])
        if abs(len(rp) - len(op)) > 0:
            diffs.append({"prop": "point_count", "ref": len(rp), "ours": len(op)})

    return diffs


def render_dxf_to_svg(dxf_path: str, output_svg: str):
    """Render DXF to SVG using ezdxf SVG backend."""
    import ezdxf
    from ezdxf.addons.drawing import Frontend, RenderContext
    from ezdxf.addons.drawing.svg import SVGBackend

    doc = ezdxf.readfile(dxf_path)
    msp = doc.modelspace()
    ctx = RenderContext(doc)
    backend = SVGBackend()
    Frontend(ctx, backend).draw_layout(msp)

    svg_string = backend.get_string()
    Path(output_svg).parent.mkdir(parents=True, exist_ok=True)
    Path(output_svg).write_text(svg_string, encoding='utf-8')
    print(f"Rendered {dxf_path} -> {output_svg}")


def render_dwg_to_svg_libredwg(dwg_path: str, output_svg: str,
                                dwg2svg: str = "dwg2SVG"):
    """Render DWG to SVG using LibreDWG dwg2SVG."""
    import subprocess
    result = subprocess.run(
        [dwg2svg, dwg_path],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        raise RuntimeError(f"dwg2SVG failed: {result.stderr[:500]}")
    svg_string = result.stdout
    if not svg_string.strip():
        raise RuntimeError("dwg2SVG produced empty output")
    Path(output_svg).parent.mkdir(parents=True, exist_ok=True)
    Path(output_svg).write_text(svg_string, encoding='utf-8')
    print(f"Rendered {dwg_path} -> {output_svg}")


def compare_svg_files(ref_svg: str, our_svg: str, tol: float = 1e-3) -> dict:
    """Main SVG comparison entry point."""
    ref_elements = extract_svg_elements(ref_svg)
    our_elements = extract_svg_elements(our_svg)

    matched, missing, extra = match_elements(ref_elements, our_elements, tol)

    all_diffs = []
    for re, oe in matched:
        diffs = compare_element_props(re, oe, tol)
        if diffs:
            all_diffs.append({
                "type": re.etype,
                "ref_point": re.characteristic_point(),
                "diffs": diffs
            })

    ref_types = defaultdict(int)
    our_types = defaultdict(int)
    for e in ref_elements:
        ref_types[e.etype] += 1
    for e in our_elements:
        our_types[e.etype] += 1

    status = "PASS"
    if missing or extra or all_diffs:
        status = "WARN" if (not missing and not extra and len(all_diffs) < 5) else "FAIL"

    return {
        "status": status,
        "ref_total": len(ref_elements),
        "our_total": len(our_elements),
        "matched": len(matched),
        "missing": len(missing),
        "extra": len(extra),
        "property_mismatches": len(all_diffs),
        "ref_types": dict(ref_types),
        "our_types": dict(our_types),
        "mismatches": all_diffs[:20],
        "missing_elements": [{"type": e.etype, "point": e.characteristic_point()} for e in missing[:10]],
        "extra_elements": [{"type": e.etype, "point": e.characteristic_point()} for e in extra[:10]],
    }


def main():
    parser = argparse.ArgumentParser(description="SVG vector-level comparison")
    parser.add_argument("ref_svg", nargs='?', help="Reference SVG file")
    parser.add_argument("our_svg", nargs='?', help="Our SVG file to compare")
    parser.add_argument("--tol", type=float, default=1e-3, help="Position tolerance")
    parser.add_argument("--render-ref", metavar="DXF", help="Render DXF to SVG via ezdxf")
    parser.add_argument("--render-dwg", metavar="DWG", help="Render DWG to SVG via LibreDWG")
    parser.add_argument("-o", "--output", help="Output SVG path for --render-*")
    args = parser.parse_args()

    if args.render_ref:
        render_dxf_to_svg(args.render_ref, args.output or "ref.svg")
    elif args.render_dwg:
        render_dwg_to_svg_libredwg(args.render_dwg, args.output or "ref.svg")
    elif args.ref_svg and args.our_svg:
        result = compare_svg_files(args.ref_svg, args.our_svg, args.tol)

        print(f"Status: {result['status']}")
        print(f"Reference: {result['ref_total']} elements")
        print(f"Ours: {result['our_total']} elements")
        print(f"Matched: {result['matched']}, Missing: {result['missing']}, Extra: {result['extra']}")
        print(f"Property mismatches: {result['property_mismatches']}")

        if result['mismatches']:
            print(f"\nProperty mismatches (first {len(result['mismatches'])}):")
            for m in result['mismatches']:
                for d in m['diffs']:
                    print(f"  {m['type']} at ({m['ref_point'][0]:.1f},{m['ref_point'][1]:.1f}): "
                          f"{d['prop']} ref={d.get('ref', '?')} ours={d.get('ours', '?')}")

        sys.exit(0 if result['status'] == 'PASS' else 1)
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
