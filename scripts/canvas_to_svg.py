#!/usr/bin/env python3
"""canvas_to_svg.py — Convert render_export JSON to SVG for vector comparison.

Reads the render batch JSON produced by `render_export` and converts each batch
to SVG elements (line, polyline, polygon). The output SVG can be compared
against reference SVGs from ezdxf or LibreDWG via svg_vector_compare.py.

Usage:
  python3 scripts/canvas_to_svg.py <render.json> -o output.svg
  python3 scripts/canvas_to_svg.py <render.json.gz> -o output.svg
"""

import argparse
import gzip
import json
import math
import sys
from pathlib import Path


def vertex_pairs(vertices: list[float]):
    """Convert flat [x0,y0,x1,y1,...] to [(x0,y0), (x1,y1), ...]."""
    pts = []
    for i in range(0, len(vertices) - 1, 2):
        pts.append((vertices[i], vertices[i + 1]))
    return pts


def format_color(color):
    """Convert color to SVG-compatible format."""
    if isinstance(color, (list, tuple)) and len(color) >= 3:
        r, g, b = [int(c) if isinstance(c, (int, float)) else 0 for c in color[:3]]
        r = max(0, min(255, r))
        g = max(0, min(255, g))
        b = max(0, min(255, b))
        return f"rgb({r},{g},{b})"
    if isinstance(color, int):
        r = (color >> 16) & 0xFF
        g = (color >> 8) & 0xFF
        b = color & 0xFF
        return f"rgb({r},{g},{b})"
    return "rgb(200,200,200)"


def batch_to_svg_elements(batch: dict, y_flip: float = None) -> list[str]:
    """Convert a render batch to SVG element strings."""
    topology = batch.get("topology", "")
    vertices = batch.get("vertices", [])
    color = batch.get("color", [200, 200, 200])
    stroke = format_color(color)

    if not vertices:
        return []

    pts = vertex_pairs(vertices)

    if y_flip is not None:
        pts = [(x, y_flip - y) for x, y in pts]

    elements = []

    if topology == "lines":
        # LineList: every pair of vertices is a separate line
        for i in range(0, len(pts) - 1, 2):
            x1, y1 = pts[i]
            x2, y2 = pts[i + 1]
            elements.append(
                f'<line x1="{x1:.3g}" y1="{y1:.3g}" x2="{x2:.3g}" y2="{y2:.3g}" '
                f'stroke="{stroke}" stroke-width="0.5" fill="none"/>'
            )

    elif topology == "linestrip":
        # LineStrip: connected vertices, with breaks
        breaks = batch.get("breaks", [])
        if breaks:
            start = 0
            for brk in breaks:
                segment = pts[start:brk + 1]
                if len(segment) >= 2:
                    path_d = "M" + " L".join(f"{x:.3g} {y:.3g}" for x, y in segment)
                    elements.append(
                        f'<path d="{path_d}" stroke="{stroke}" stroke-width="0.5" fill="none"/>'
                    )
                start = brk
            # Last segment
            if start < len(pts):
                segment = pts[start:]
                if len(segment) >= 2:
                    path_d = "M" + " L".join(f"{x:.3g} {y:.3g}" for x, y in segment)
                    elements.append(
                        f'<path d="{path_d}" stroke="{stroke}" stroke-width="0.5" fill="none"/>'
                    )
        else:
            if len(pts) >= 2:
                path_d = "M" + " L".join(f"{x:.3g} {y:.3g}" for x, y in pts)
                elements.append(
                    f'<path d="{path_d}" stroke="{stroke}" stroke-width="0.5" fill="none"/>'
                )

    elif topology == "triangles":
        # TriangleList: every 3 vertices is a triangle
        for i in range(0, len(pts) - 2, 3):
            tri = pts[i:i + 3]
            if len(tri) == 3:
                pts_str = " ".join(f"{x:.3g},{y:.3g}" for x, y in tri)
                elements.append(
                    f'<polygon points="{pts_str}" fill="{stroke}" '
                    f'stroke="{stroke}" stroke-width="0.2"/>'
                )

    return elements


def render_json_to_svg(render_json_path: str, output_svg: str,
                       y_flip: float = None, width: int = 1920, height: int = 1080):
    """Convert render_export JSON to SVG."""
    path = Path(render_json_path)
    if path.suffix == '.gz':
        with gzip.open(path, 'rt', encoding='utf-8') as f:
            data = json.load(f)
    else:
        with open(path, 'r', encoding='utf-8') as f:
            data = json.load(f)

    batches = data.get("batches", [])

    all_elements = []
    for batch in batches:
        elements = batch_to_svg_elements(batch, y_flip)
        all_elements.extend(elements)

    # Auto-compute viewBox from data bounds
    bounds = data.get("bounds", {})
    if bounds and not bounds.get("isEmpty", True):
        min_x = bounds.get("minX", -100)
        min_y = bounds.get("minY", -100)
        max_x = bounds.get("maxX", 100)
        max_y = bounds.get("maxY", 100)
        if y_flip is not None:
            min_y, max_y = y_flip - max_y, y_flip - min_y
        pad = max(max_x - min_x, max_y - min_y) * 0.02
        viewbox = f"{min_x - pad:.3g} {min_y - pad:.3g} {max_x - min_x + 2 * pad:.3g} {max_y - min_y + 2 * pad:.3g}"
    else:
        viewbox = f"0 0 {width} {height}"

    svg = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="{viewbox}" width="{width}" height="{height}">\n'
        f'<rect width="100%" height="100%" fill="#1e1e2e"/>\n'
        + '\n'.join(all_elements) + '\n'
        + f'</svg>'
    )

    Path(output_svg).parent.mkdir(parents=True, exist_ok=True)
    Path(output_svg).write_text(svg, encoding='utf-8')
    print(f"Converted {render_json_path} -> {output_svg} ({len(all_elements)} elements)")


def main():
    parser = argparse.ArgumentParser(description="Convert render_export JSON to SVG")
    parser.add_argument("render_json", help="render_export JSON (or .json.gz) file")
    parser.add_argument("-o", "--output", required=True, help="Output SVG path")
    parser.add_argument("--y-flip", type=float, default=None,
                        help="Y-flip value (canvas height for Y-axis inversion)")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    args = parser.parse_args()

    render_json_to_svg(args.render_json, args.output, args.y_flip,
                       args.width, args.height)


if __name__ == '__main__':
    main()
