#!/usr/bin/env python3
"""test_geometric_accuracy.py — Validate geometric properties of parsed entities.

For each test DXF file, extracts entities via ezdxf (ground truth) and our
entity_export, matches them spatially, and verifies geometric properties
(position, radius, angles, etc.) within tolerance.

Usage:
  python3 scripts/test_geometric_accuracy.py [--tol 1e-3] [--all] [file.dxf ...]
"""

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
ENTITY_EXPORT = BUILD_DIR / "core/test/entity_export"
TEST_DATA = ROOT / "test_data"
TMP = Path("/tmp/cad_accuracy")

sys.path.insert(0, str(ROOT / "scripts"))
from compare_entities import extract_ezdxf_entities, match_entities, scene_range


def load_our_entities(json_path: str):
    with open(json_path) as f:
        data = json.load(f)
    return [e for e in data.get("entities", []) if not e.get("in_block", False)]


def check_num(name, ref_val, our_val, tol):
    if ref_val is None or our_val is None:
        return None
    if isinstance(ref_val, (list, tuple)):
        for i, (a, b) in enumerate(zip(ref_val, our_val)):
            if abs(a - b) > tol:
                return {"prop": f"{name}[{i}]", "ref": a, "ours": b, "delta": abs(a - b)}
    elif isinstance(ref_val, (int, float)):
        if abs(ref_val - our_val) > tol:
            return {"prop": name, "ref": ref_val, "ours": our_val, "delta": abs(ref_val - our_val)}
    return None


def check_angle(name, ref_deg, our_deg, tol_deg=0.1):
    if ref_deg is None or our_deg is None:
        return None
    diff = abs(ref_deg - our_deg) % 360
    if diff > 180:
        diff = 360 - diff
    if diff > tol_deg:
        return {"prop": name, "ref": ref_deg, "ours": our_deg, "delta": diff}
    return None


def validate_entity(ref, ours, tol=1e-3):
    """Validate geometric properties of a matched entity pair."""
    mismatches = []
    t = ref["type"]

    if t == "LINE":
        for key in ("start", "end"):
            r = check_num(key, ref.get(key), ours.get(key), tol)
            if r:
                mismatches.append(r)

    elif t == "CIRCLE":
        r = check_num("center", ref.get("center"), ours.get("center"), tol)
        if r: mismatches.append(r)
        r = check_num("radius", ref.get("radius"), ours.get("radius"), tol)
        if r: mismatches.append(r)

    elif t == "ARC":
        for key in ("center",):
            r = check_num(key, ref.get(key), ours.get(key), tol)
            if r: mismatches.append(r)
        r = check_num("radius", ref.get("radius"), ours.get("radius"), tol)
        if r: mismatches.append(r)
        r = check_num("start_angle", ref.get("start_angle"), ours.get("start_angle"), 0.1)
        if r: mismatches.append(r)
        r = check_num("end_angle", ref.get("end_angle"), ours.get("end_angle"), 0.1)
        if r: mismatches.append(r)

    elif t == "ELLIPSE":
        r = check_num("center", ref.get("center"), ours.get("center"), tol)
        if r: mismatches.append(r)
        r = check_num("major_radius", ref.get("major_radius"), ours.get("major_radius"), tol)
        if r: mismatches.append(r)
        r = check_num("minor_radius", ref.get("minor_radius"), ours.get("minor_radius"), tol)
        if r: mismatches.append(r)
        r = check_num("rotation", ref.get("rotation"), ours.get("rotation"), 0.5)
        if r: mismatches.append(r)

    elif t == "SPLINE":
        r = check_num("degree", ref.get("degree"), ours.get("degree"), 0.5)
        if r: mismatches.append(r)
        ref_cps = ref.get("control_points", [])
        our_cps = ours.get("control_points", [])
        if len(ref_cps) != len(our_cps):
            mismatches.append({"prop": "cp_count", "ref": len(ref_cps), "ours": len(our_cps)})

    elif t == "SOLID":
        ref_c = ref.get("corners", [])
        our_c = ours.get("corners", [])
        if len(ref_c) != len(our_c):
            mismatches.append({"prop": "corner_count", "ref": len(ref_c), "ours": len(our_c)})
        else:
            for i, (a, b) in enumerate(zip(ref_c, our_c)):
                r = check_num(f"corner[{i}]", a, b, tol)
                if r: mismatches.append(r)

    elif t in ("TEXT", "MTEXT"):
        ref_text = ref.get("text", "").strip()
        our_text = ours.get("text", "").strip()
        if ref_text != our_text:
            mismatches.append({"prop": "text", "ref": ref_text[:40], "ours": our_text[:40]})
        r = check_num("x", ref.get("x"), ours.get("x"), tol)
        if r: mismatches.append(r)
        r = check_num("y", ref.get("y"), ours.get("y"), tol)
        if r: mismatches.append(r)
        r = check_num("height", ref.get("height"), ours.get("height"), tol)
        if r: mismatches.append(r)

    elif t == "INSERT":
        r = check_num("x", ref.get("x"), ours.get("x"), tol)
        if r: mismatches.append(r)
        r = check_num("y", ref.get("y"), ours.get("y"), tol)
        if r: mismatches.append(r)

    elif t == "HATCH":
        r = check_num("loop_count", ref.get("loop_count"), ours.get("loop_count"), 0.5)
        if r: mismatches.append(r)

    elif t == "DIMENSION":
        r = check_num("definition_point", ref.get("definition_point"),
                       ours.get("definition_point"), tol)
        if r: mismatches.append(r)

    return mismatches


def test_file(dxf_path: str, entity_export: Path, tol: float) -> dict:
    """Run geometric accuracy test for a single DXF file."""
    TMP.mkdir(parents=True, exist_ok=True)
    our_json = TMP / f"{Path(dxf_path).stem}_accuracy.json"

    result = subprocess.run(
        [str(entity_export), dxf_path, str(our_json)],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0 or not our_json.exists():
        return {"status": "ERROR", "error": result.stderr[:500]}

    ref_entities = extract_ezdxf_entities(dxf_path)
    our_entities = load_our_entities(str(our_json))

    sr = scene_range(ref_entities)
    matched, missing, extra = match_entities(ref_entities, our_entities, sr, tol)

    all_mismatches = []
    for ref, ours in matched:
        mismatches = validate_entity(ref, ours, tol)
        if mismatches:
            all_mismatches.append({
                "type": ref["type"],
                "point": (ref.get("x") or ref.get("center", [0, 0])[0],
                          ref.get("y") or ref.get("center", [0, 0])[1]),
                "diffs": mismatches
            })

    total = len(ref_entities)
    passed = len(matched) - len(all_mismatches)
    status = "PASS" if not missing and not extra and not all_mismatches else \
             "WARN" if len(all_mismatches) < max(3, total * 0.05) else "FAIL"

    return {
        "status": status,
        "ref_total": total,
        "our_total": len(our_entities),
        "matched": len(matched),
        "missing": len(missing),
        "extra": len(extra),
        "property_mismatches": len(all_mismatches),
        "accuracy": f"{passed}/{total}" if total > 0 else "0/0",
        "mismatches": all_mismatches[:10],
    }


def main():
    parser = argparse.ArgumentParser(description="Geometric accuracy validation")
    parser.add_argument("files", nargs="*", help="DXF files to test")
    parser.add_argument("--all", action="store_true", help="Test all DXF files in test_data/")
    parser.add_argument("--tol", type=float, default=1e-3, help="Position tolerance")
    args = parser.parse_args()

    if args.all:
        files = sorted(TEST_DATA.glob("*.dxf"))
    elif args.files:
        files = [Path(f) for f in args.files]
    else:
        files = sorted(TEST_DATA.glob("*.dxf"))

    if not ENTITY_EXPORT.exists():
        print(f"ERROR: entity_export not found at {ENTITY_EXPORT}", file=sys.stderr)
        return 1

    print(f"Testing geometric accuracy (tol={args.tol})...")
    total_pass = 0
    total_fail = 0

    for f in files:
        result = test_file(str(f), ENTITY_EXPORT, args.tol)
        status_mark = "OK" if result["status"] == "PASS" else \
                      "??" if result["status"] == "WARN" else "FAIL"
        print(f"  [{status_mark}] {f.name}: accuracy={result.get('accuracy', '?')} "
              f"matched={result.get('matched', '?')} "
              f"mismatches={result.get('property_mismatches', '?')} "
              f"missing={result.get('missing', '?')} extra={result.get('extra', '?')}")

        if result["status"] == "PASS":
            total_pass += 1
        else:
            total_fail += 1
            if result.get("mismatches"):
                for m in result["mismatches"][:3]:
                    for d in m["diffs"][:2]:
                        print(f"       {m['type']}: {d['prop']} ref={d.get('ref','?')} ours={d.get('ours','?')}")

    print(f"\nResults: {total_pass} passed, {total_fail} issues out of {len(files)} files")
    return 0 if total_fail == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
