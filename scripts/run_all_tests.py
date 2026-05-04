#!/usr/bin/env python3
"""Unified test runner for v0.9.0+ — runs structural comparison against HOOPS
and entity regression against libredwg DXF, producing a single report.

Usage:
    python scripts/run_all_tests.py [--structural-only] [--report report.json]

Requires:
    - build/core/test/render_export.exe (or Debug/render_export.exe)
    - build/tools/dwg_to_json/dwg_to_json.exe (or Debug/dwg_to_json.exe)
    - FT_DWG2DXF environment variable pointing to dwg2dxf.exe
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
TEST_DWG = ROOT / "test_dwg"
TMP = Path(os.environ.get("TMP", "/tmp"))


def _find_exe(name: str) -> Path:
    for sub in ("", "Debug/", "Release/"):
        for dir in ("core/test", "tools/dwg_to_json"):
            for ext in ("", ".exe"):
                p = BUILD_DIR / dir / sub / (name + ext)
                if p.exists():
                    return p
    return BUILD_DIR / "core/test" / name


RENDER_EXPORT = _find_exe("render_export")
DWG_TO_JSON = _find_exe("dwg_to_json")
DWG2DXF = Path(os.environ.get("FT_DWG2DXF", "/tmp/libredwg-0.13.4/programs/dwg2dxf"))

sys.path.insert(0, str(ROOT / "scripts"))


def run(cmd: list, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def generate_ft_export(dwg_path: Path, out_json: Path, timeout: int = 600) -> dict:
    """Generate FT compare-mode JSON export."""
    started = time.time()
    cmd = [str(RENDER_EXPORT), str(dwg_path), str(out_json), "--compare-mode"]
    result = run(cmd, timeout=timeout)
    ok = result.returncode == 0 and out_json.exists() and out_json.stat().st_size > 0
    return {
        "status": "PASS" if ok else "ERROR",
        "elapsed_ms": int((time.time() - started) * 1000),
        "stderr": result.stderr[-500:] if result.stderr else "",
    }


def generate_hoops_ref(dwg_path: Path, out_json: Path, timeout: int = 300) -> dict:
    """Generate HOOPS JSON reference export."""
    started = time.time()
    if out_json.exists() and out_json.stat().st_size > 1000:
        return {"status": "PASS", "cached": True, "elapsed_ms": 0}
    cmd = [str(DWG_TO_JSON), str(dwg_path), str(out_json)]
    result = run(cmd, timeout=timeout)
    ok = result.returncode == 0 and out_json.exists() and out_json.stat().st_size > 0
    return {
        "status": "PASS" if ok else "ERROR",
        "elapsed_ms": int((time.time() - started) * 1000),
        "stderr": result.stderr[-500:] if result.stderr else "",
    }


def run_structural(hoops_json: Path, ft_json: Path) -> dict:
    """Run compare_structural.py and return parsed result."""
    from compare_structural import run_comparison
    try:
        result = run_comparison(str(hoops_json), str(ft_json))
        return {
            "status": result.get("overall", "UNKNOWN"),
            "entity_ratio_pct": result.get("entity_counts", {}).get("ratio_pct"),
            "geo_match_pct": result.get("geometric", {}).get("match_pct"),
            "layers_ok": result.get("layers", {}).get("match"),
            "notes": result.get("notes", []),
        }
    except Exception as e:
        return {"status": "ERROR", "error": str(e)}


def run_entity_regression(dwg_path: Path, timeout: int = 300) -> dict:
    """Run entity-level regression via libredwg DXF comparison."""
    started = time.time()
    # Convert DWG to DXF
    dxf_path = TMP / f"ref_{dwg_path.stem}.dxf"
    if not dxf_path.exists() or dxf_path.stat().st_size == 0:
        r = run([str(DWG2DXF), "-m", str(dwg_path), "-o", str(dxf_path)], timeout=timeout)
        if not dxf_path.exists() or dxf_path.stat().st_size == 0:
            return {"status": "ERROR", "error": "dwg2dxf failed", "elapsed_ms": int((time.time()-started)*1000)}

    # Export our entities
    our_json = TMP / f"ours_{dwg_path.stem}_entities.json"
    r = run([str(RENDER_EXPORT), str(dwg_path), str(our_json)], timeout=timeout)
    if r.returncode != 0 or not our_json.exists():
        return {"status": "ERROR", "error": "entity_export failed", "elapsed_ms": int((time.time()-started)*1000)}

    # Compare
    from compare_entities import run_comparison
    try:
        result = run_comparison(str(dxf_path), str(our_json), tolerance=0.1)
        return {
            "status": result.get("status", "UNKNOWN"),
            "ref_total": result.get("ref_total"),
            "our_total": result.get("our_total"),
            "match_pct": result.get("match_pct"),
            "elapsed_ms": int((time.time()-started)*1000),
        }
    except Exception as e:
        return {"status": "ERROR", "error": str(e), "elapsed_ms": int((time.time()-started)*1000)}


def main():
    parser = argparse.ArgumentParser(description="Unified DWG test runner")
    parser.add_argument("--structural-only", action="store_true", help="Only run structural comparison")
    parser.add_argument("--report", default="build/test_report.json", help="Output report path")
    parser.add_argument("--dwg", default="", help="Filter DWG files by substring")
    args = parser.parse_args()

    dwg_files = sorted(TEST_DWG.glob("*.dwg"))
    if args.dwg:
        dwg_files = [p for p in dwg_files if args.dwg in p.name]

    print(f"Found {len(dwg_files)} DWG test file(s)")
    print("=" * 80)

    results = []
    for dwg_path in dwg_files:
        name = dwg_path.name
        print(f"\n>>> {name}")

        # Structural comparison
        ft_json = TMP / f"ft_{name}_compare.json"
        hoops_json = TMP / f"hoops_{name}.json"

        # Skip generation if both files already exist and are recent (< 24h)
        ft_cached = ft_json.exists() and ft_json.stat().st_size > 0 and (time.time() - ft_json.stat().st_mtime) < 86400
        hoops_cached = hoops_json.exists() and hoops_json.stat().st_size > 0 and (time.time() - hoops_json.stat().st_mtime) < 86400
        if ft_cached and hoops_cached:
            print(f"    Using cached exports")
            ft_res = {"status": "PASS"}
            hoops_res = {"status": "PASS"}
        else:
            ft_res = generate_ft_export(dwg_path, ft_json)
            if ft_res["status"] != "PASS":
                print(f"    FT export FAILED: {ft_res.get('stderr', '')}")
                results.append({"file": name, "structural": "SKIP", "entity": "SKIP"})
                continue

            hoops_res = generate_hoops_ref(dwg_path, hoops_json)
            if hoops_res["status"] != "PASS":
                print(f"    HOOPS export FAILED: {hoops_res.get('stderr', '')}")
                results.append({"file": name, "structural": "SKIP", "entity": "SKIP"})
                continue

        struct = run_structural(hoops_json, ft_json)
        print(f"    Structural: {struct['status']}  entity_ratio={struct.get('entity_ratio_pct'):.1f}%  geo={struct.get('geo_match_pct'):.1f}%")

        # Entity regression
        entity = {"status": "SKIP"}
        if not args.structural_only:
            entity = run_entity_regression(dwg_path)
            print(f"    Entity:     {entity['status']}  ref={entity.get('ref_total')} ours={entity.get('our_total')} match={entity.get('match_pct')}")

        results.append({
            "file": name,
            "structural": struct["status"],
            "entity": entity["status"],
            "details": {"structural": struct, "entity": entity},
        })

    # Summary
    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    ok = sum(1 for r in results if r["structural"] in ("PASS", "WARN"))
    fail = sum(1 for r in results if r["structural"] == "FAIL")
    print(f"Structural: {ok}/{len(results)} PASS/WARN, {fail} FAIL")
    if not args.structural_only:
        e_ok = sum(1 for r in results if r["entity"] == "PASS")
        e_fail = sum(1 for r in results if r["entity"] == "FAIL")
        print(f"Entity:     {e_ok}/{len(results)} PASS, {e_fail} FAIL")

    report = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "total_files": len(results),
        "structural_pass": ok,
        "structural_fail": fail,
        "results": results,
    }
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f"\nReport written to: {args.report}")


if __name__ == "__main__":
    main()
