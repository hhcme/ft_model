#!/usr/bin/env python3
"""compare_libredwg_json.py — 3-way entity comparison: ezdxf vs LibreDWG vs ours.

Uses LibreDWG's dwg2JSON tool as a third reference for DWG entity validation.
When both ezdxf and LibreDWG agree but our parser disagrees, it confirms a bug.

Usage:
  python3 scripts/compare_libredwg_json.py [--dwg file.dwg] [--all]
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
DWG2JSON = Path(os.environ.get("FT_DWG2JSON", "/tmp/libredwg-0.13.4/programs/dwg2JSON"))
TMP = Path("/tmp/cad_3way")

sys.path.insert(0, str(ROOT / "scripts"))
from compare_entities import extract_ezdxf_entities, match_entities, scene_range


def load_our_entities(json_path: str):
    with open(json_path) as f:
        data = json.load(f)
    return [e for e in data.get("entities", []) if not e.get("in_block", False)], \
           data.get("entity_counts", {})


def run_libredwg_json(dwg_path: str, output_json: str) -> dict:
    """Run dwg2JSON to get third-party entity data."""
    if not DWG2JSON.exists():
        return {"status": "SKIP", "error": f"dwg2JSON not found: {DWG2JSON}"}

    result = subprocess.run(
        [str(DWG2JSON), str(dwg_path)],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        return {"status": "ERROR", "error": result.stderr[:500]}

    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError:
        # dwg2JSON may output JSON with extra text
        lines = result.stdout.strip().split('\n')
        json_str = ''
        started = False
        for line in lines:
            if line.strip().startswith('{') or line.strip().startswith('['):
                started = True
            if started:
                json_str += line
        try:
            data = json.loads(json_str)
        except json.JSONDecodeError:
            return {"status": "ERROR", "error": "Failed to parse dwg2JSON output"}

    Path(output_json).parent.mkdir(parents=True, exist_ok=True)
    Path(output_json).write_text(json.dumps(data, indent=2), encoding='utf-8')
    return {"status": "OK", "data": data}


def run_3way_comparison(dwg_path: str) -> dict:
    """Run 3-way comparison for a single DWG file."""
    TMP.mkdir(parents=True, exist_ok=True)
    stem = Path(dwg_path).stem

    # Step 1: Our parser
    our_json = TMP / f"{stem}_ours.json"
    result = subprocess.run(
        [str(ENTITY_EXPORT), str(dwg_path), str(our_json)],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        return {"status": "ERROR", "error": f"entity_export: {result.stderr[:300]}"}

    our_entities, our_counts = load_our_entities(str(our_json))

    # Step 2: ezdxf reference (via dwg2dxf)
    ref_dxf = TMP / f"{stem}_ref.dxf"
    if not ref_dxf.exists():
        r = subprocess.run(
            [str(DWG2DXF), "-m", str(dwg_path), "-o", str(ref_dxf)],
            capture_output=True, text=True, timeout=120
        )
        if r.returncode != 0:
            r = subprocess.run(
                [str(DWG2DXF), str(dwg_path), "-o", str(ref_dxf)],
                capture_output=True, text=True, timeout=120
            )

    ezdxf_entities = []
    if ref_dxf.exists() and ref_dxf.stat().st_size > 0:
        try:
            ezdxf_entities = extract_ezdxf_entities(str(ref_dxf))
        except Exception as ex:
            pass

    # Step 3: LibreDWG dwg2JSON reference
    libredwg_result = run_libredwg_json(str(dwg_path), str(TMP / f"{stem}_libredwg.json"))

    # Summary
    our_types = Counter(e["type"] for e in our_entities)
    ezdxf_types = Counter(e["type"] for e in ezdxf_entities)

    return {
        "status": "OK",
        "our_entity_count": len(our_entities),
        "ezdxf_entity_count": len(ezdxf_entities),
        "libredwg_status": libredwg_result["status"],
        "our_types": dict(our_types),
        "ezdxf_types": dict(ezdxf_types),
    }


def main():
    parser = argparse.ArgumentParser(description="3-way DWG entity comparison")
    parser.add_argument("--dwg", help="Single DWG file to analyze")
    parser.add_argument("--all", action="store_true", help="Test all DWG files")
    args = parser.parse_args()

    if args.dwg:
        files = [Path(args.dwg)]
    elif args.all:
        files = sorted(TEST_DWG.glob("*.dwg"))
    else:
        files = sorted(TEST_DWG.glob("*.dwg"))

    print(f"3-way comparison: ezdxf vs LibreDWG vs ours")
    print(f"  entity_export: {ENTITY_EXPORT}")
    print(f"  dwg2dxf: {DWG2DXF}")
    print(f"  dwg2JSON: {DWG2JSON}")
    print()

    for f in files:
        print(f"=== {f.name} ===")
        result = run_3way_comparison(str(f))
        if result["status"] == "ERROR":
            print(f"  ERROR: {result.get('error', '?')}")
            continue

        print(f"  Ours: {result['our_entity_count']} entities")
        print(f"  ezdxf: {result['ezdxf_entity_count']} entities")
        print(f"  LibreDWG: {result['libredwg_status']}")

        # Compare type counts
        all_types = sorted(set(result["our_types"]) | set(result["ezdxf_types"]))
        for t in all_types:
            oc = result["our_types"].get(t, 0)
            ec = result["ezdxf_types"].get(t, 0)
            mark = " OK" if oc == ec else " !!"
            print(f"    {t:16s} ours={oc:6d}  ezdxf={ec:6d}{mark}")
        print()


if __name__ == '__main__':
    main()
