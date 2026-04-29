#!/usr/bin/env python3
"""Batch convert DWG files to SCS using the dwg_to_scs tool.

Automatically retries failed files with --safe mode (2D, tess-only).
"""

import os
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TOOL_PATH = PROJECT_ROOT / "build" / "tools" / "dwg_to_scs" / "Release" / "dwg_to_scs.exe"
INPUT_DIR = PROJECT_ROOT / "test_dwg"
OUTPUT_DIR = PROJECT_ROOT / "scs_dwg"
SDK_ROOT = os.environ.get(
    "HOOPS_EXCHANGE_ROOT",
    r"D:\findtop\code\hoops_high_performance\sdk\extracted"
    r"\HOOPS_Exchange_2026.2.0_Windows_x86-64_v142"
    r"\HOOPS_Exchange_2026.2.0",
)
TIMEOUT = 1800  # 30 minutes per file


def main():
    tool_path = TOOL_PATH
    if not tool_path.exists():
        alt = PROJECT_ROOT / "build" / "tools" / "dwg_to_scs" / "dwg_to_scs.exe"
        if alt.exists():
            tool_path = alt
        else:
            print(f"Error: converter not found at {TOOL_PATH} or {alt}")
            print("Build with: cmake --build build --target dwg_to_scs")
            sys.exit(1)

    if not INPUT_DIR.exists():
        print(f"Error: input directory not found: {INPUT_DIR}")
        sys.exit(1)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["HOOPS_EXCHANGE_ROOT"] = SDK_ROOT

    dwg_files = sorted(INPUT_DIR.glob("*.dwg"))
    if not dwg_files:
        print("No DWG files found in", INPUT_DIR)
        sys.exit(0)

    ok, fail, retried = 0, 0, 0
    total = len(dwg_files)

    for i, dwg in enumerate(dwg_files, 1):
        scs = OUTPUT_DIR / (dwg.stem + ".scs")
        print(f"\n[{i}/{total}] {dwg.name} -> {scs.name}")

        # Try normal mode first
        result = run_convert(tool_path, dwg, scs, env, safe=False)
        if result == 0:
            ok += 1
            continue

        # If normal mode failed or crashed, retry with --safe
        print(f"  Normal mode failed (rc={result}), retrying with --safe...")
        result = run_convert(tool_path, dwg, scs, env, safe=True)
        if result == 0:
            ok += 1
            retried += 1
        else:
            fail += 1
            print(f"  FAILED even with --safe (rc={result})")

    print(f"\n{'='*50}")
    print(f"Batch done: {ok} succeeded ({retried} via --safe), {fail} failed (total: {total})")
    sys.exit(1 if fail > 0 else 0)


def run_convert(tool: Path, dwg: Path, scs: Path, env: dict, safe: bool) -> int:
    """Run converter, return process exit code."""
    cmd = [str(tool)]
    if safe:
        cmd.append("--safe")
    cmd.extend([str(dwg), str(scs)])
    try:
        result = subprocess.run(cmd, env=env, capture_output=False, timeout=TIMEOUT)
        return result.returncode
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT (>{TIMEOUT}s)")
        return -1
    except Exception as e:
        print(f"  ERROR: {e}")
        return -2


if __name__ == "__main__":
    main()
