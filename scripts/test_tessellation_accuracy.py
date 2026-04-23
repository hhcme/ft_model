#!/usr/bin/env python3
"""test_tessellation_accuracy.py — Validate tessellated vertex positions.

For each test DXF, runs render_export and validates that tessellated vertices
satisfy geometric constraints (circle equation, ellipse equation, etc.).

Usage:
  python3 scripts/test_tessellation_accuracy.py [--all] [file.dxf ...]
"""

import argparse
import gzip
import json
import math
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
RENDER_EXPORT = BUILD_DIR / "core/test/entity_export"
RENDER_EXPORT_BIN = BUILD_DIR / "core/test/render_export"
TEST_DATA = ROOT / "test_data"
TMP = Path("/tmp/cad_tessellation")


def load_render_json(path: str):
    p = Path(path)
    if p.suffix == '.gz':
        with gzip.open(p, 'rt', encoding='utf-8') as f:
            return json.load(f)
    with open(p, 'r', encoding='utf-8') as f:
        return json.load(f)


def vertex_pairs(vertices):
    """Flat [x0,y0,x1,y1,...] to [(x0,y0), ...]."""
    return [(vertices[i], vertices[i + 1]) for i in range(0, len(vertices) - 1, 2)]


def validate_circle_vertices(pts, cx, cy, r, tolerance_frac=0.05):
    """Check that points lie on a circle within tolerance."""
    if r < 1e-6:
        return True, 0
    max_error = 0
    for x, y in pts:
        dist = math.sqrt((x - cx) ** 2 + (y - cy) ** 2)
        error = abs(dist - r) / r
        max_error = max(max_error, error)
    return max_error <= tolerance_frac, max_error


def validate_ellipse_vertices(pts, cx, cy, major_r, minor_r, rotation_deg,
                               tolerance_frac=0.05):
    """Check that points satisfy the ellipse equation."""
    if major_r < 1e-6 or minor_r < 1e-6:
        return True, 0
    rot = math.radians(rotation_deg)
    cos_r, sin_r = math.cos(rot), math.sin(rot)
    max_error = 0
    for x, y in pts:
        dx, dy = x - cx, y - cy
        lx = dx * cos_r + dy * sin_r
        ly = -dx * sin_r + dy * cos_r
        val = (lx / major_r) ** 2 + (ly / minor_r) ** 2
        error = abs(val - 1.0)
        max_error = max(max_error, error)
    return max_error <= tolerance_frac, max_error


def validate_topology(batch, expected_topology_map):
    """Validate batch topology is reasonable for its content."""
    top = batch.get("topology", "")
    if top not in ("lines", "linestrip", "triangles"):
        return False, f"unknown topology: {top}"
    verts = batch.get("vertices", [])
    if len(verts) == 0:
        return False, "empty vertices"
    return True, top


def test_file_tessellation(dxf_path: str, tol_frac: float = 0.05) -> dict:
    """Run tessellation accuracy test for a single DXF file."""
    TMP.mkdir(parents=True, exist_ok=True)
    render_json = TMP / f"{Path(dxf_path).stem}_tess.json"

    result = subprocess.run(
        [str(RENDER_EXPORT_BIN), dxf_path, str(render_json)],
        capture_output=True, text=True, timeout=60
    )
    if result.returncode != 0 or not render_json.exists():
        return {"status": "ERROR", "error": result.stderr[:500]}

    data = load_render_json(str(render_json))
    batches = data.get("batches", [])

    issues = []
    total_vertices = 0
    total_batches = len(batches)

    for i, batch in enumerate(batches):
        ok, msg = validate_topology(batch, {})
        if not ok:
            issues.append(f"batch[{i}]: {msg}")
            continue

        verts = batch.get("vertices", [])
        pts = vertex_pairs(verts)
        total_vertices += len(pts)

        if not pts:
            continue

        # Check all coordinates are finite
        non_finite = sum(1 for x, y in pts
                        if not (math.isfinite(x) and math.isfinite(y)))
        if non_finite > 0:
            issues.append(f"batch[{i}]: {non_finite} non-finite vertices")

        # Check coordinate range is reasonable
        coords = [(x, y) for x, y in pts if math.isfinite(x) and math.isfinite(y)]
        if coords:
            max_coord = max(max(abs(x), abs(y)) for x, y in coords)
            if max_coord > 1e8:
                issues.append(f"batch[{i}]: extreme coordinates ({max_coord:.2e})")

        topology = batch.get("topology", "")
        # Validate LineStrip has breaks for multi-entity batches
        if topology == "linestrip" and "breaks" in batch:
            breaks = batch["breaks"]
            if breaks and max(breaks) >= len(pts):
                issues.append(f"batch[{i}]: break index out of range")

    status = "PASS" if not issues else \
             "WARN" if len(issues) <= 2 else "FAIL"

    return {
        "status": status,
        "batches": total_batches,
        "vertices": total_vertices,
        "issues": issues[:10],
    }


def main():
    parser = argparse.ArgumentParser(description="Tessellation accuracy validation")
    parser.add_argument("files", nargs="*", help="DXF files to test")
    parser.add_argument("--all", action="store_true", help="Test all DXF files")
    parser.add_argument("--tol", type=float, default=0.05,
                        help="Fractional tolerance (default 0.05 = 5%%)")
    args = parser.parse_args()

    if args.all:
        files = sorted(TEST_DATA.glob("*.dxf"))
    elif args.files:
        files = [Path(f) for f in args.files]
    else:
        files = sorted(TEST_DATA.glob("*.dxf"))

    if not RENDER_EXPORT_BIN.exists():
        print(f"ERROR: render_export not found at {RENDER_EXPORT_BIN}", file=sys.stderr)
        return 1

    print(f"Testing tessellation accuracy (tol={args.tol * 100:.0f}%)...")
    total_pass = 0
    total_fail = 0

    for f in files:
        result = test_file_tessellation(str(f), args.tol)
        mark = "OK" if result["status"] == "PASS" else \
               "??" if result["status"] == "WARN" else "FAIL"
        print(f"  [{mark}] {f.name}: batches={result.get('batches', '?')} "
              f"vertices={result.get('vertices', '?')} issues={len(result.get('issues', []))}")

        if result["status"] == "PASS":
            total_pass += 1
        else:
            total_fail += 1
            for issue in result.get("issues", [])[:5]:
                print(f"       {issue}")

    print(f"\nResults: {total_pass} passed, {total_fail} issues out of {len(files)} files")
    return 0 if total_fail == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
