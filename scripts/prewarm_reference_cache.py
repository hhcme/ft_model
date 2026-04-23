#!/usr/bin/env python3
"""Pre-generate third-party reference cache for DWG fixtures.

This writes cache entries under /tmp/ft_model_reference_cache by default, using
the same fingerprint/layout consumed by start_preview.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))

import start_preview  # noqa: E402


def run_entity_comparison_limited(path: Path, ref_dxf: Path, timeout: int) -> tuple[dict | None, str | None, int]:
    started = time.time()
    entity_json = start_preview.reference_cache_paths(start_preview.file_sha256(str(path)))["root"] / "ours.entities.json"
    export = subprocess.run(
        [str(start_preview.ENTITY_EXPORT), str(path), str(entity_json)],
        capture_output=True, text=True, timeout=timeout,
    )
    if export.returncode != 0 or not entity_json.exists():
        return None, (export.stderr or export.stdout)[-1000:] or "entity_export failed", int((time.time() - started) * 1000)

    try:
        cmp = subprocess.run(
            [sys.executable, str(ROOT / "scripts/compare_entities.py"), str(ref_dxf), str(entity_json), "--tol", "0.1"],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT"}, f"entity comparison timed out after {timeout}s", int((time.time() - started) * 1000)

    out = cmp.stdout or ""
    ref_match = re.search(r"Reference \(ezdxf\):\s+(\d+)", out)
    our_match = re.search(r"Ours \(entity_export\):\s+(\d+)", out)
    matched = re.search(r"Matched:\s+(\d+),\s+Missing:\s+(\d+),\s+Extra:\s+(\d+)", out)
    prop = re.search(r"Property mismatches:\s+(\d+)", out)
    result = {
        "status": "PASS" if cmp.returncode == 0 else "FAIL",
        "refEntityCount": int(ref_match.group(1)) if ref_match else 0,
        "ourEntityCount": int(our_match.group(1)) if our_match else 0,
        "matched": int(matched.group(1)) if matched else 0,
        "missing": int(matched.group(2)) if matched else 0,
        "extra": int(matched.group(3)) if matched else 0,
        "propertyMismatches": int(prop.group(1)) if prop else 0,
        "output": out[-4000:],
    }
    if result["refEntityCount"]:
        result["entityCountDiffPct"] = abs(result["ourEntityCount"] - result["refEntityCount"]) / result["refEntityCount"]
    return result, None, int((time.time() - started) * 1000)


def prewarm_one(path: Path, entity_timeout: int, timeout_note: str = "") -> dict:
    started = time.time()
    fingerprint = start_preview.file_sha256(str(path))
    provider = start_preview.active_reference_provider()
    cached = start_preview.load_reference_cache(fingerprint, provider["id"])
    if cached:
        return {
            "fixture": path.name,
            "status": "CACHED",
            "fingerprint": fingerprint,
            "elapsedMs": 0,
            "cacheDir": str(start_preview.reference_cache_paths(fingerprint)["root"]),
        }

    paths = start_preview.reference_cache_paths(fingerprint, provider["id"])
    paths["root"].mkdir(parents=True, exist_ok=True)
    ref_dxf = paths["root"] / "reference.tmp.dxf"
    ref_png = paths["root"] / "reference.tmp.png"

    ref_png_b64, ref_err, ref_time_ms, ref_render_info = start_preview.run_reference_render(
        str(path),
        path.suffix.lower() == ".dwg",
        str(ref_png),
        str(ref_dxf),
    )

    metadata = {
        "cacheHit": False,
        "provider": provider["id"],
        "providerStrength": provider["strength"],
        "sourceFingerprint": fingerprint,
        "sourceFilename": path.name,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "parserFramework": {
            "provider": provider["id"],
            "dwgConverter": "libredwg dwg2dxf",
            "dwgConverterPath": str(start_preview.DWG2DXF),
            "dwgConverterVersion": start_preview.tool_version([str(start_preview.DWG2DXF), "--version"]) if start_preview.DWG2DXF.exists() else "missing",
            "entityExtractor": "ezdxf",
            "renderer": provider["renderer"],
            "rendererPath": provider.get("rendererPath", ""),
            "rendererVersion": start_preview.tool_version([provider["rendererPath"], "-h"]) if provider.get("rendererPath") else "n/a",
            "outputWidth": ref_render_info.get("outputWidth"),
            "outputHeight": ref_render_info.get("outputHeight"),
            "command": ref_render_info.get("command"),
        },
        "renderTimeMs": ref_time_ms,
        "entityCompareTimeMs": 0,
        "entityCompare": None,
        "entityCompareError": None,
        "referenceError": ref_err or None,
        "note": timeout_note,
    }

    if ref_png_b64 and ref_png.exists():
        start_preview.save_reference_cache(
            fingerprint,
            str(ref_png),
            str(ref_dxf) if ref_dxf.exists() else None,
            metadata,
        )
        status = "PASS" if not ref_err else "WARN"
    else:
        status = "ERROR"

    entity_compare = None
    entity_err = None
    entity_time_ms = 0
    if status != "ERROR" and ref_dxf.exists() and entity_timeout > 0:
        try:
            entity_compare, entity_err, entity_time_ms = run_entity_comparison_limited(path, ref_dxf, entity_timeout)
        except subprocess.TimeoutExpired:
            entity_compare, entity_err = {"status": "TIMEOUT"}, f"entity comparison timed out after {entity_timeout}s"
        except Exception as ex:
            entity_err = str(ex)[:1000]
        metadata["entityCompare"] = entity_compare
        metadata["entityCompareError"] = entity_err
        metadata["entityCompareTimeMs"] = entity_time_ms
        if ref_png_b64 and ref_png.exists():
            start_preview.save_reference_cache(
                fingerprint,
                str(ref_png),
                str(ref_dxf) if ref_dxf.exists() else None,
                metadata,
            )

    return {
        "fixture": path.name,
        "status": status,
        "fingerprint": fingerprint,
        "elapsedMs": int((time.time() - started) * 1000),
        "cacheDir": str(paths["root"]),
        "referenceError": ref_err or None,
        "entityCompareError": entity_err or None,
        "entityStatus": (entity_compare or {}).get("status"),
        "refEntityCount": (entity_compare or {}).get("refEntityCount"),
        "ourEntityCount": (entity_compare or {}).get("ourEntityCount"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Prewarm third-party reference cache")
    parser.add_argument("--dwg", help="Only fixtures whose filename contains this text")
    parser.add_argument("--force", action="store_true", help="Delete existing cache entries before regenerating")
    parser.add_argument("--entity-timeout", type=int, default=180, help="Entity comparison timeout per fixture; 0 skips entity compare")
    parser.add_argument("--report", default="/tmp/ft_model_reference_cache/prewarm_report.json")
    args = parser.parse_args()

    files = sorted((ROOT / "test_dwg").glob("*.dwg"))
    if args.dwg:
        files = [p for p in files if args.dwg.lower() in p.name.lower()]
    if not files:
        print("No DWG fixtures matched", file=sys.stderr)
        return 2

    start_preview.REF_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    results = []
    for path in files:
        fingerprint = start_preview.file_sha256(str(path))
        if args.force:
            provider = start_preview.active_reference_provider()
            cache_root = start_preview.reference_cache_paths(fingerprint, provider["id"])["root"]
            if cache_root.exists():
                import shutil
                shutil.rmtree(cache_root)

        print(f"\n=== Prewarm: {path.name} ===", flush=True)
        result = prewarm_one(path, args.entity_timeout)
        results.append(result)
        print(
            f"  {result['status']} entity={result.get('entityStatus', '—')} "
            f"ours={result.get('ourEntityCount', '—')} ref={result.get('refEntityCount', '—')} "
            f"cache={result['cacheDir']}",
            flush=True,
        )

    summary = {}
    for r in results:
        summary[r["status"]] = summary.get(r["status"], 0) + 1

    report = {
        "cacheDir": str(start_preview.REF_CACHE_DIR),
        "provider": start_preview.active_reference_provider(),
        "dwg2dxf": str(start_preview.DWG2DXF),
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "summary": summary,
        "results": results,
    }
    report_path = Path(args.report)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nReport: {report_path}")
    print(" ".join(f"{k}={v}" for k, v in sorted(summary.items())))
    return 0 if summary.get("ERROR", 0) == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
