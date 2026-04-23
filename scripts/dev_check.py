#!/usr/bin/env python3
"""Check local preview/comparison prerequisites without mutating the repo."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RENDER_EXPORT = ROOT / "build/core/test/render_export"
ENTITY_EXPORT = ROOT / "build/core/test/entity_export"
DWG2DXF = Path(os.environ.get("FT_DWG2DXF", "/tmp/libredwg-0.13.4/programs/dwg2dxf"))
QCAD_RENDERER = os.environ.get("FT_QCAD_RENDERER")


def check_path(label: str, path: Path, required: bool = True) -> bool:
    ok = path.exists()
    mark = "OK" if ok else ("WARN" if not required else "ERROR")
    print(f"[{mark}] {label}: {path}")
    return ok or not required


def check_module(name: str, required: bool = True) -> bool:
    try:
        __import__(name)
        print(f"[OK] Python module: {name}")
        return True
    except Exception as ex:
        mark = "WARN" if not required else "ERROR"
        print(f"[{mark}] Python module: {name} ({ex})")
        return not required


def main() -> int:
    ok = True
    ok = check_path("render_export", RENDER_EXPORT) and ok
    ok = check_path("entity_export", ENTITY_EXPORT) and ok
    ok = check_path("dwg2dxf", DWG2DXF, required=False) and ok

    qcad_candidates = []
    if QCAD_RENDERER:
        qcad_candidates.append(Path(QCAD_RENDERER))
    for name in ("dwg2png", "dwg2bmp"):
        found = shutil.which(name)
        if found:
            qcad_candidates.append(Path(found))
    for pattern in (
        "/Applications/QCAD*.app/Contents/Resources/dwg2png",
        "/Applications/QCAD*.app/Contents/Resources/dwg2bmp",
    ):
        qcad_candidates.extend(Path("/").glob(pattern.lstrip("/")))
    qcad = next((p for p in qcad_candidates if p.exists()), None)
    print(f"[{'OK' if qcad else 'WARN'}] QCAD renderer: {qcad or 'not found; falling back to libredwg+ezdxf'}")

    if not DWG2DXF.exists():
        print("      Set FT_DWG2DXF to enable third-party reference comparison.")

    for module in ("ezdxf", "matplotlib", "PIL", "numpy"):
        ok = check_module(module, required=False) and ok
    ok = check_module("playwright", required=False) and ok

    npm = shutil.which("npm")
    print(f"[{'OK' if npm else 'ERROR'}] npm: {npm or 'not found'}")
    ok = bool(npm) and ok

    if not ok:
        print("\nPreview can still start if render_export exists, but comparison features may be disabled.")
        print(f"Build tools: cd {ROOT / 'build'} && cmake --build . --target entity_export render_export")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
