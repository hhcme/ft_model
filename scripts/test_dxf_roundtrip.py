#!/usr/bin/env python3
"""test_dxf_roundtrip.py — Validate DWG parser matches DXF parser for same content.

Converts DWG to DXF via LibreDWG dwg2dxf, then parses both through our engine
and compares the results. This verifies DWG↔DXF semantic consistency.

Usage:
  python3 scripts/test_dxf_roundtrip.py [--dwg file.dwg] [--all]
"""

import argparse
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
ENTITY_EXPORT = BUILD_DIR / "core/test/entity_export"
TEST_DWG = ROOT / "test_dwg"
DWG2DXF = Path(os.environ.get("FT_DWG2DXF", "/tmp/libredwg-0.13.4/programs/dwg2dxf"))
TMP = Path("/tmp/cad_roundtrip")

sys.path.insert(0, str(ROOT / "scripts"))
from compare_entities import match_entities, scene_range


def load_our_entities(json_path: str):
    with open(json_path) as f:
        data = json.load(f)
    return [e for e in data.get("entities", []) if not e.get("in_block", False)]


def run_roundtrip(dwg_path: str, tol: float = 1e-3) -> dict:
    """Run DWG→DXF→parse vs DWG→parse comparison."""
    TMP.mkdir(parents=True, exist_ok=True)
    stem = Path(dwg_path).stem

    # Step 1: Parse DWG directly
    dwg_json = TMP / f"{stem}_dwg.json"
    result = subprocess.run(
        [str(ENTITY_EXPORT), str(dwg_path), str(dwg_json)],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        return {"status": "ERROR", "error": f"DWG parse: {result.stderr[:300]}"}

    dwg_entities = load_our_entities(str(dwg_json))

    # Step 2: Convert DWG to DXF via LibreDWG
    ref_dxf = TMP / f"{stem}_roundtrip.dxf"
    if not ref_dxf.exists() or ref_dxf.stat().st_size == 0:
        r = subprocess.run(
            [str(DWG2DXF), "-m", str(dwg_path), "-o", str(ref_dxf)],
            capture_output=True, text=True, timeout=120
        )
        if r.returncode != 0:
            r = subprocess.run(
                [str(DWG2DXF), str(dwg_path), "-o", str(ref_dxf)],
                capture_output=True, text=True, timeout=120
            )

    if not ref_dxf.exists() or ref_dxf.stat().st_size == 0:
        return {"status": "SKIP", "error": "dwg2dxf conversion failed"}

    # Step 3: Parse DXF
    dxf_json = TMP / f"{stem}_dxf.json"
    result = subprocess.run(
        [str(ENTITY_EXPORT), str(ref_dxf), str(dxf_json)],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        return {"status": "ERROR", "error": f"DXF parse: {result.stderr[:300]}"}

    dxf_entities = load_our_entities(str(dxf_json))

    # Step 4: Compare
    sr = scene_range(dxf_entities)
    matched, missing, extra = match_entities(dxf_entities, dwg_entities, sr, tol)

    dwg_types = Counter(e["type"] for e in dwg_entities)
    dxf_types = Counter(e["type"] for e in dxf_entities)

    status = "PASS" if not missing and not extra else \
             "WARN" if len(missing) + len(extra) <= 5 else "FAIL"

    return {
        "status": status,
        "dwg_entities": len(dwg_entities),
        "dxf_entities": len(dxf_entities),
        "matched": len(matched),
        "missing": len(missing),
        "extra": len(extra),
        "dwg_types": dict(dwg_types),
        "dxf_types": dict(dxf_types),
    }


def main():
    parser = argparse.ArgumentParser(description="DWG↔DXF roundtrip validation")
    parser.add_argument("--dwg", help="Single DWG file")
    parser.add_argument("--all", action="store_true", help="Test all DWG files")
    parser.add_argument("--tol", type=float, default=1e-3)
    args = parser.parse_args()

    if args.dwg:
        files = [Path(args.dwg)]
    elif args.all:
        files = sorted(TEST_DWG.glob("*.dwg"))
    else:
        files = sorted(TEST_DWG.glob("*.dwg"))

    if not DWG2DXF.exists():
        print(f"ERROR: dwg2dxf not found at {DWG2DXF}", file=sys.stderr)
        print("Set FT_DWG2DXF environment variable to the dwg2dxf path.", file=sys.stderr)
        return 1

    if not ENTITY_EXPORT.exists():
        print(f"ERROR: entity_export not found at {ENTITY_EXPORT}", file=sys.stderr)
        return 1

    print("DWG↔DXF roundtrip validation...")
    total_pass = 0
    total_fail = 0

    for f in files:
        result = run_roundtrip(str(f), args.tol)
        mark = "OK" if result["status"] == "PASS" else \
               "??" if result["status"] == "WARN" else \
               "--" if result["status"] == "SKIP" else "FAIL"
        print(f"  [{mark}] {f.name}: "
              f"dwg={result.get('dwg_entities', '?')} "
              f"dxf={result.get('dxf_entities', '?')} "
              f"matched={result.get('matched', '?')} "
              f"missing={result.get('missing', '?')} "
              f"extra={result.get('extra', '?')}")

        if result["status"] == "PASS":
            total_pass += 1
        elif result["status"] == "SKIP":
            pass
        else:
            total_fail += 1

            # Show type differences
            dwg_t = result.get("dwg_types", {})
            dxf_t = result.get("dxf_types", {})
            all_types = sorted(set(dwg_t) | set(dxf_t))
            for t in all_types:
                dc = dxf_t.get(t, 0)
                wc = dwg_t.get(t, 0)
                if dc != wc:
                    print(f"       {t}: dxf={dc} dwg={wc}")

    print(f"\nResults: {total_pass} passed, {total_fail} issues")
    return 0 if total_fail == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
