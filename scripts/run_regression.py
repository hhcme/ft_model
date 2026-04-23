#!/usr/bin/env python3
"""Full DWG comparison regression: self parser vs test-only reference parser.

Default scope is all ``test_dwg/*.dwg``. Intermediate files are written to
``/tmp/cad_compare`` and the structured report defaults to
``build/regression_report.json``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
TEST_DWG = ROOT / "test_dwg"
ENTITY_EXPORT = BUILD_DIR / "core/test/entity_export"
RENDER_EXPORT = BUILD_DIR / "core/test/render_export"
DWG2DXF = Path(os.environ.get("FT_DWG2DXF", "/tmp/libredwg-0.13.4/programs/dwg2dxf"))
TMP = Path("/tmp/cad_compare")
REF_DXF_DIR = TMP / "ref_dxf"

sys.path.insert(0, str(ROOT / "scripts"))
from compare_entities import run_comparison  # noqa: E402
from visual_compare import capture_canvas, compare_images, render_dxf_to_png  # noqa: E402
from visual_compare import _ensure_vite_server  # noqa: E402


def run(cmd: list[str], timeout: int, cwd: Path = ROOT) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def build_tools() -> dict:
    started = time.time()
    result = run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "entity_export", "render_export"],
        timeout=600,
    )
    return {
        "status": "PASS" if result.returncode == 0 else "ERROR",
        "elapsedMs": int((time.time() - started) * 1000),
        "stdout": result.stdout[-2000:],
        "stderr": result.stderr[-2000:],
    }


def check_prereqs(include_visual: bool) -> list[str]:
    errors: list[str] = []
    if not ENTITY_EXPORT.exists():
        errors.append(f"missing entity_export: {ENTITY_EXPORT}")
    if not RENDER_EXPORT.exists():
        errors.append(f"missing render_export: {RENDER_EXPORT}")
    if not DWG2DXF.exists():
        errors.append(f"missing dwg2dxf: {DWG2DXF} (set FT_DWG2DXF)")
    modules = ["ezdxf"]
    if include_visual:
        modules.extend(["matplotlib", "PIL", "numpy", "playwright"])
    for module in modules:
        try:
            __import__(module)
        except Exception as ex:
            errors.append(f"missing Python dependency {module}: {ex}")
    return errors


def convert_dwg_to_dxf(dwg_path: Path, timeout: int) -> tuple[Path | None, dict]:
    started = time.time()
    REF_DXF_DIR.mkdir(parents=True, exist_ok=True)
    dxf_path = REF_DXF_DIR / f"{dwg_path.stem}.dxf"
    if dxf_path.exists() and dxf_path.stat().st_size > 0:
        return dxf_path, {"status": "PASS", "cached": True, "path": str(dxf_path), "elapsedMs": 0}

    result = run([str(DWG2DXF), "-m", str(dwg_path), "-o", str(dxf_path)], timeout=timeout)
    if result.returncode != 0 or not dxf_path.exists() or dxf_path.stat().st_size == 0:
        result = run([str(DWG2DXF), str(dwg_path), "-o", str(dxf_path)], timeout=timeout)

    ok = dxf_path.exists() and dxf_path.stat().st_size > 0
    return (
        dxf_path if ok else None,
        {
            "status": "PASS" if ok else "ERROR",
            "path": str(dxf_path),
            "elapsedMs": int((time.time() - started) * 1000),
            "stdout": result.stdout[-1500:],
            "stderr": result.stderr[-1500:],
        },
    )


def run_entity_compare(dwg_path: Path, ref_dxf: Path, timeout: int) -> dict:
    started = time.time()
    out_json = TMP / f"{dwg_path.stem}_entities.json"
    result = run([str(ENTITY_EXPORT), str(dwg_path), str(out_json)], timeout=timeout)
    if result.returncode != 0 or not out_json.exists():
        return {
            "status": "ERROR",
            "elapsedMs": int((time.time() - started) * 1000),
            "error": result.stderr[-1500:] or result.stdout[-1500:] or "entity_export failed",
        }

    try:
        compare = run_comparison(str(ref_dxf), str(out_json), 0.1)
        ref_total = int(compare.get("ref_total", 0))
        our_total = int(compare.get("our_total", 0))
        return {
            "status": compare.get("status", "FAIL"),
            "elapsedMs": int((time.time() - started) * 1000),
            "ourEntityCount": our_total,
            "refEntityCount": ref_total,
            "matched": int(compare.get("matched", 0)),
            "missing": int(compare.get("missing", 0)),
            "extra": int(compare.get("extra", 0)),
            "propertyMismatches": int(compare.get("property_mismatches", 0)),
            "entityCountDiffPct": abs(our_total - ref_total) / ref_total if ref_total else None,
            "entityTypeCounts": {
                "ours": compare.get("our_types", {}),
                "reference": compare.get("ref_types", {}),
            },
            "missingSamples": compare.get("missing_entities", []),
            "extraSamples": compare.get("extra_entities", []),
            "output": str(out_json),
        }
    except Exception as ex:
        return {
            "status": "ERROR",
            "elapsedMs": int((time.time() - started) * 1000),
            "error": str(ex)[:1500],
            "output": str(out_json),
        }


def run_visual_compare(dwg_path: Path, ref_dxf: Path, timeout: int, app_url: str) -> dict:
    started = time.time()
    render_json = TMP / f"{dwg_path.stem}_render.json.gz"
    ref_png = TMP / f"{dwg_path.stem}_ref.png"
    ours_png = TMP / f"{dwg_path.stem}_ours.png"
    diff_png = TMP / f"{dwg_path.stem}_diff.png"

    result = run([str(RENDER_EXPORT), str(dwg_path), str(render_json)], timeout=timeout)
    if result.returncode != 0 or not render_json.exists():
        return {
            "status": "ERROR",
            "elapsedMs": int((time.time() - started) * 1000),
            "error": result.stderr[-1500:] or result.stdout[-1500:] or "render_export failed",
        }

    try:
        render_dxf_to_png(str(ref_dxf), str(ref_png))
        asyncio.run(capture_canvas(str(render_json), str(ours_png), app_url=app_url))
        compare = compare_images(str(ref_png), str(ours_png), str(diff_png))
        return {
            "status": compare.get("status", "FAIL"),
            "elapsedMs": int((time.time() - started) * 1000),
            "ssim": compare.get("ssim"),
            "diffPct": compare.get("diff_pct"),
            "refSize": compare.get("ref_size"),
            "ourSize": compare.get("our_size"),
            "renderJson": str(render_json),
            "refPng": str(ref_png),
            "oursPng": str(ours_png),
            "diffPng": str(diff_png),
        }
    except subprocess.TimeoutExpired:
        return {
            "status": "TIMEOUT",
            "elapsedMs": int((time.time() - started) * 1000),
            "error": f"visual comparison timed out after {timeout}s",
            "renderJson": str(render_json),
        }
    except Exception as ex:
        return {
            "status": "ERROR",
            "elapsedMs": int((time.time() - started) * 1000),
            "error": str(ex)[:1500],
            "renderJson": str(render_json),
        }


def run_fixture(dwg_path: Path, args: argparse.Namespace) -> dict:
    print(f"\n=== DWG: {dwg_path.name} ===")
    fixture_started = time.time()
    result: dict = {
        "fixture": dwg_path.name,
        "path": str(dwg_path),
        "sizeBytes": dwg_path.stat().st_size,
        "status": "PASS",
    }

    ref_dxf, convert = convert_dwg_to_dxf(dwg_path, args.timeout)
    result["referenceConversion"] = convert
    if ref_dxf is None:
        result["status"] = "ERROR"
        result["elapsedMs"] = int((time.time() - fixture_started) * 1000)
        print("  Reference conversion: ERROR")
        return result

    if not args.visual_only:
        entity = run_entity_compare(dwg_path, ref_dxf, args.timeout)
        result["entityCompare"] = entity
        print(f"  Entity: {entity['status']}  ours={entity.get('ourEntityCount', '—')} ref={entity.get('refEntityCount', '—')}")
        if entity["status"] not in ("PASS", "WARN"):
            result["status"] = entity["status"]

    if not args.entity_only:
        visual = run_visual_compare(dwg_path, ref_dxf, args.timeout, args.app_url)
        result["visualCompare"] = visual
        print(f"  Visual: {visual['status']}  ssim={visual.get('ssim', '—')}")
        if visual["status"] == "FAIL" or visual["status"] in ("ERROR", "TIMEOUT"):
            result["status"] = visual["status"]

    result["elapsedMs"] = int((time.time() - fixture_started) * 1000)
    return result


def summarize(results: list[dict]) -> dict:
    counts = {"PASS": 0, "WARN": 0, "FAIL": 0, "ERROR": 0, "TIMEOUT": 0, "SKIP": 0}
    for item in results:
        status = item.get("status", "ERROR")
        counts[status] = counts.get(status, 0) + 1
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description="Run DWG parser/render comparison regression")
    parser.add_argument("--entity-only", action="store_true")
    parser.add_argument("--visual-only", action="store_true")
    parser.add_argument("--dwg", help="Only run DWG fixtures whose filename contains this text")
    parser.add_argument("--no-build", action="store_true", help="Skip cmake tool build")
    parser.add_argument("--timeout", type=int, default=600, help="Per-stage timeout in seconds")
    parser.add_argument("--report", default=str(BUILD_DIR / "regression_report.json"))
    parser.add_argument("--app-url", default=os.environ.get("FT_PREVIEW_APP_URL", "http://localhost:5173"))
    parser.add_argument("--fail-on-warn", action="store_true")
    args = parser.parse_args()

    os.chdir(ROOT)
    TMP.mkdir(parents=True, exist_ok=True)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    report: dict = {
        "root": str(ROOT),
        "tmpDir": str(TMP),
        "dwg2dxf": str(DWG2DXF),
        "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "results": [],
    }

    if not args.no_build:
        print("=== Building comparison tools ===")
        build = build_tools()
        report["build"] = build
        print(f"Build: {build['status']}")
        if build["status"] != "PASS":
            Path(args.report).parent.mkdir(parents=True, exist_ok=True)
            Path(args.report).write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
            return 1

    prereq_errors = check_prereqs(include_visual=not args.entity_only)
    if prereq_errors:
        report["prereqErrors"] = prereq_errors
        for err in prereq_errors:
            print(f"Prereq ERROR: {err}", file=sys.stderr)
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        return 1

    app_proc = None
    if not args.entity_only:
        app_proc = _ensure_vite_server(args.app_url)

    try:
        dwg_files = sorted(TEST_DWG.glob("*.dwg"))
        if args.dwg:
            dwg_files = [p for p in dwg_files if args.dwg.lower() in p.name.lower()]
        if not dwg_files:
            print("No DWG fixtures matched", file=sys.stderr)
            return 2

        for dwg_path in dwg_files:
            report["results"].append(run_fixture(dwg_path, args))
    finally:
        if app_proc is not None:
            app_proc.terminate()
            try:
                app_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app_proc.kill()

    counts = summarize(report["results"])
    report["summary"] = counts
    report["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")

    print("\n" + "=" * 60)
    print("REGRESSION SUMMARY")
    print("=" * 60)
    for item in report["results"]:
        entity = item.get("entityCompare") or {}
        visual = item.get("visualCompare") or {}
        print(
            f"  {item['fixture'][:38]:38s} {item.get('status', 'ERROR'):7s} "
            f"entity={entity.get('status', '—'):5s} visual={visual.get('status', '—'):5s} "
            f"ssim={visual.get('ssim', '—')}"
        )
    print(" ".join(f"{k}={v}" for k, v in sorted(counts.items())))

    report_path = Path(args.report)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Report: {report_path}")

    blocking = counts.get("FAIL", 0) + counts.get("ERROR", 0) + counts.get("TIMEOUT", 0)
    if args.fail_on_warn:
        blocking += counts.get("WARN", 0)
    return 0 if blocking == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
