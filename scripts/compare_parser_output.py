#!/usr/bin/env python3
"""Compare our parser's render_export output against ezdxf reference data.

Runs render_export on each fixture, loads reference data from test_reference/,
and produces a structured comparison report.
"""
import json, os, subprocess, sys, math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_DATA = ROOT / "test_data"
TEST_DWG = ROOT / "test_dwg"
REF_DIR = ROOT / "test_reference" / "ezdxf"
BUILD_DIR = ROOT / "build"
RENDER_EXPORT = BUILD_DIR / "core" / "test" / "render_export"
TMP_DIR = Path("/tmp/cad_compare")

# Thresholds
GEOM_COUNT_THRESHOLD = 0.05      # 5% difference for geometry entities
ANNOT_COUNT_THRESHOLD = 0.20     # 20% for annotation entities
POSITION_THRESHOLD = 0.01        # 1% relative position error
LAYER_NAME_MATCH_REQUIRED = True

ANNOT_TYPES = {"TEXT", "MTEXT", "DIMENSION", "LEADER", "MULTILEADER", "ATTRIB"}
GEOM_TYPES = {"LINE", "ARC", "CIRCLE", "ELLIPSE", "SPLINE", "POLYLINE", "LWPOLYLINE",
              "SOLID", "POINT", "RAY", "XLINE", "HATCH", "INSERT"}


class Result:
    def __init__(self, status: str, message: str = ""):
        self.status = status  # PASS, WARN, FAIL, SKIP
        self.message = message

    def __str__(self):
        return f"{self.status:4s} {self.message}"


def run_render_export(input_path: Path) -> dict | None:
    """Run render_export and return parsed JSON."""
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    out_path = TMP_DIR / (input_path.stem + "_ours.json")
    try:
        result = subprocess.run(
            [str(RENDER_EXPORT), str(input_path), str(out_path)],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode != 0:
            print(f"  render_export failed: {result.stderr[:200]}", file=sys.stderr)
            return None
        return json.loads(out_path.read_text())
    except (subprocess.TimeoutExpired, json.JSONDecodeError) as ex:
        print(f"  render_export error: {ex}", file=sys.stderr)
        return None


def load_reference(name: str) -> dict | None:
    """Load ezdxf reference summary."""
    stem = Path(name).stem
    ref_path = REF_DIR / (stem + "_summary.json")
    if not ref_path.exists():
        return None
    return json.loads(ref_path.read_text())


def count_our_entities(our_data: dict) -> dict[str, int]:
    """Count entities by type from our render_export JSON.

    Uses entityTypeCounts field (added v0.10.x) for precise per-type counts.
    Falls back to texts array for annotation entities if not available.
    """
    # Use entityTypeCounts if available (v0.10.x+)
    if "entityTypeCounts" in our_data:
        counts = {}
        for name, count in our_data["entityTypeCounts"].items():
            counts[name.upper()] = count
        counts["_total"] = our_data.get("entityCount", sum(counts.values()))
        return counts

    # Fallback: count from texts array
    counts = {}
    for t in our_data.get("texts", []):
        kind = t.get("kind", "text").upper()
        if kind == "TEXT":
            counts["TEXT"] = counts.get("TEXT", 0) + 1
        elif kind == "MTEXT":
            counts["MTEXT"] = counts.get("MTEXT", 0) + 1
        elif kind == "DIMENSION":
            counts["DIMENSION"] = counts.get("DIMENSION", 0) + 1

    total = our_data.get("entityCount", 0)
    text_total = counts.get("TEXT", 0) + counts.get("MTEXT", 0) + counts.get("DIMENSION", 0)
    counts["_total"] = total
    counts["_geometry_approx"] = total - text_total

    return counts


def compare_entity_counts(our_counts: dict, ref_counts: dict, scene_range: float) -> list[tuple[str, Result]]:
    """Compare entity type counts."""
    results = []
    ref_total = ref_counts.get("total_entities", sum(ref_counts.get("entity_counts", {}).values()))

    for etype in sorted(set(ref_counts.get("entity_counts", {}).keys()) | set(our_counts.keys())):
        if etype.startswith("_"):
            continue
        ref_n = ref_counts.get("entity_counts", {}).get(etype, 0)
        our_n = our_counts.get(etype, 0)

        if ref_n == 0 and our_n == 0:
            continue
        if ref_n == 0:
            results.append((etype, Result("WARN", f"ours={our_n} ref=0 (extra)")))
            continue
        if our_n == 0:
            results.append((etype, Result("WARN", f"ours=0 ref={ref_n} (missing)")))
            continue

        diff_pct = abs(our_n - ref_n) / ref_n
        threshold = ANNOT_COUNT_THRESHOLD if etype in ANNOT_TYPES else GEOM_COUNT_THRESHOLD

        if diff_pct <= threshold:
            status = "PASS"
        elif diff_pct <= threshold * 2:
            status = "WARN"
        else:
            status = "FAIL"
        results.append((etype, Result(status,
            f"ours={our_n} ref={ref_n} diff={diff_pct:.1%} (threshold={threshold:.0%})")))

    # Total comparison
    our_total = our_counts.get("_total", 0)
    if ref_total > 0:
        diff_pct = abs(our_total - ref_total) / ref_total
        status = "PASS" if diff_pct <= GEOM_COUNT_THRESHOLD else "WARN" if diff_pct <= 0.10 else "FAIL"
        results.append(("TOTAL", Result(status,
            f"ours={our_total} ref={ref_total} diff={diff_pct:.1%}")))

    return results


def compare_layers(our_data: dict, ref_data: dict) -> Result:
    """Compare layer lists."""
    our_layers = {l["name"]: l for l in our_data.get("layers", [])}
    ref_layers = {l["name"]: l for l in ref_data.get("layers", [])}

    if not ref_layers:
        return Result("SKIP", "no reference layers")

    missing = sorted(set(ref_layers) - set(our_layers))
    extra = sorted(set(our_layers) - set(ref_layers))

    # Check state mismatches
    mismatches = []
    for name in set(our_layers) & set(ref_layers):
        ours = our_layers[name]
        ref = ref_layers[name]
        if ours.get("frozen") != ref.get("frozen") or ours.get("off") != ref.get("off"):
            mismatches.append(name)

    parts = []
    if missing:
        parts.append(f"missing={missing}")
    if extra:
        parts.append(f"extra={extra}")
    if mismatches:
        parts.append(f"state_mismatch={mismatches}")

    matched = len(set(our_layers) & set(ref_layers))
    total = len(ref_layers)

    if not parts:
        return Result("PASS", f"{matched}/{total} matched")
    status = "PASS" if not missing else "WARN" if len(missing) <= 2 else "FAIL"
    return Result(status, f"{matched}/{total} matched, {', '.join(parts)}")


def compare_blocks(our_data: dict, ref_data: dict) -> Result:
    """Compare block definitions."""
    our_blocks = set()
    for b in our_data.get("drawingInfo", {}).get("blocks", []):
        if isinstance(b, dict):
            our_blocks.add(b.get("name", ""))
        elif isinstance(b, str):
            our_blocks.add(b)

    # Also check batches for block info
    for batch in our_data.get("batches", []):
        ln = batch.get("layerName", "")
        if ln.startswith("BLOCK_"):
            our_blocks.add(ln)

    ref_blocks = set(ref_data.get("blocks", []))

    if not ref_blocks:
        return Result("SKIP", "no reference blocks")

    missing = sorted(ref_blocks - our_blocks)
    extra = sorted(our_blocks - ref_blocks)
    matched = len(ref_blocks & our_blocks)
    total = len(ref_blocks)

    parts = []
    if missing:
        parts.append(f"missing={missing[:5]}{'...' if len(missing) > 5 else ''}")
    if extra:
        parts.append(f"extra={extra[:5]}{'...' if len(extra) > 5 else ''}")

    if not parts:
        return Result("PASS", f"{matched}/{total} matched")
    status = "WARN" if len(missing) <= 3 else "FAIL"
    return Result(status, f"{matched}/{total} matched, {', '.join(parts)}")


def compare_geometry_samples(our_data: dict, ref_data: dict) -> Result:
    """Compare geometry position samples (LINE endpoints, CIRCLE centers, etc.)."""
    our_bounds = our_data.get("bounds", {})
    if our_bounds.get("isEmpty", True):
        return Result("SKIP", "our bounds empty")

    scene_w = our_bounds.get("maxX", 1) - our_bounds.get("minX", 0)
    scene_h = our_bounds.get("maxY", 1) - our_bounds.get("minY", 0)
    scene_range = max(scene_w, scene_h, 1.0)
    threshold = scene_range * POSITION_THRESHOLD

    # Collect our geometry points from batches
    our_points = []
    for batch in our_data.get("batches", []):
        verts = batch.get("vertices", [])
        if isinstance(verts, list) and len(verts) >= 2:
            for i in range(0, len(verts) - 1, 2):
                x, y = verts[i], verts[i + 1]
                if math.isfinite(x) and math.isfinite(y):
                    our_points.append((x, y))

    if not our_points:
        return Result("SKIP", "no geometry points in our output")

    # Match reference samples against our points
    matches = 0
    total_dist = 0
    max_dist = 0
    samples_checked = 0

    for sample in ref_data.get("line_samples", []):
        for key in ("start", "end"):
            if key not in sample:
                continue
            rx, ry = sample[key]
            best = min((abs(p[0] - rx) + abs(p[1] - ry)) for p in our_points[:5000])
            total_dist += best
            max_dist = max(max_dist, best)
            if best <= threshold:
                matches += 1
            samples_checked += 1

    for sample in ref_data.get("circle_samples", []):
        if "center" not in sample:
            continue
        rx, ry = sample["center"]
        best = min((abs(p[0] - rx) + abs(p[1] - ry)) for p in our_points[:5000])
        total_dist += best
        max_dist = max(max_dist, best)
        if best <= threshold:
            matches += 1
        samples_checked += 1

    for sample in ref_data.get("arc_samples", []):
        if "center" not in sample:
            continue
        rx, ry = sample["center"]
        best = min((abs(p[0] - rx) + abs(p[1] - ry)) for p in our_points[:5000])
        total_dist += best
        max_dist = max(max_dist, best)
        if best <= threshold:
            matches += 1
        samples_checked += 1

    if samples_checked == 0:
        return Result("SKIP", "no reference samples")

    match_rate = matches / samples_checked
    avg_dist = total_dist / samples_checked
    rel_max = max_dist / scene_range

    status = "PASS" if match_rate >= 0.95 and rel_max <= 0.05 else \
             "WARN" if match_rate >= 0.80 else "FAIL"
    return Result(status,
        f"{match_rate:.1%} match, avg={avg_dist:.4g}, max_rel={rel_max:.4g}")


def compare_text_samples(our_data: dict, ref_data: dict) -> Result:
    """Compare text content and positions."""
    our_texts = our_data.get("texts", [])
    ref_texts = ref_data.get("text_samples", [])

    if not ref_texts:
        return Result("SKIP", "no reference text")
    if not our_texts:
        return Result("FAIL", f"0/{len(ref_texts)} texts (missing all)")

    our_bounds = our_data.get("bounds", {})
    scene_range = max(
        our_bounds.get("maxX", 1) - our_bounds.get("minX", 0),
        our_bounds.get("maxY", 1) - our_bounds.get("minY", 0),
        1.0,
    )
    threshold = scene_range * 0.02

    content_matches = 0
    position_matches = 0

    for ref_t in ref_texts:
        ref_text = ref_t.get("text", "").strip()
        rx, ry = ref_t.get("x", 0), ref_t.get("y", 0)

        best_dist = float("inf")
        content_found = False
        for our_t in our_texts:
            ot = our_t.get("text", "").strip()
            # Check content match (fuzzy - partial match for MTEXT formatting)
            if ref_text and (ref_text in ot or ot in ref_text):
                content_found = True
            ox, oy = our_t.get("x", 0), our_t.get("y", 0)
            dist = abs(ox - rx) + abs(oy - ry)
            best_dist = min(best_dist, dist)

        if content_found:
            content_matches += 1
        if best_dist <= threshold:
            position_matches += 1

    n = len(ref_texts)
    content_rate = content_matches / n if n > 0 else 0
    position_rate = position_matches / n if n > 0 else 0

    status = "PASS" if content_rate >= 0.80 and position_rate >= 0.80 else \
             "WARN" if content_rate >= 0.50 or position_rate >= 0.50 else "FAIL"
    return Result(status,
        f"{content_rate:.1%} content, {position_rate:.1%} position ({n} samples)")


def compare_file(filepath: Path) -> dict:
    """Run full comparison for one file."""
    name = filepath.name
    print(f"\n=== {name} ===")

    # Load reference
    ref = load_reference(name)
    if ref is None:
        print(f"  SKIP: no reference data")
        return {"file": name, "status": "SKIP", "reason": "no reference"}

    # Run our parser
    ours = run_render_export(filepath)
    if ours is None:
        print(f"  FAIL: render_export failed")
        return {"file": name, "status": "FAIL", "reason": "parse failed"}

    our_counts = count_our_entities(ours)
    ref_counts = ref

    # Compare
    entity_results = compare_entity_counts(our_counts, ref_counts, 1.0)
    layer_result = compare_layers(ours, ref)
    block_result = compare_blocks(ours, ref)
    geom_result = compare_geometry_samples(ours, ref)
    text_result = compare_text_samples(ours, ref)

    # Print report
    print(f"\n  Entity Counts:")
    for etype, r in entity_results:
        print(f"    {etype:<15} {r}")
    print(f"  Layers:     {layer_result}")
    print(f"  Blocks:     {block_result}")
    print(f"  Geometry:   {geom_result}")
    print(f"  Text:       {text_result}")

    # Aggregate status
    all_statuses = [r.status for _, r in entity_results] + [
        layer_result.status, block_result.status, geom_result.status, text_result.status
    ]
    if "FAIL" in all_statuses:
        overall = "FAIL"
    elif "WARN" in all_statuses:
        overall = "WARN"
    elif "SKIP" in all_statuses and all(s in ("SKIP", "PASS") for s in all_statuses):
        overall = "PASS"
    else:
        overall = "PASS"

    warnings = sum(1 for s in all_statuses if s == "WARN")
    fails = sum(1 for s in all_statuses if s == "FAIL")
    suffix = ""
    if warnings:
        suffix += f" ({warnings} warnings)"
    if fails:
        suffix += f" ({fails} failures)"
    print(f"  Overall:    {overall}{suffix}")

    return {"file": name, "status": overall}


def main():
    if not RENDER_EXPORT.exists():
        print(f"Error: render_export not found at {RENDER_EXPORT}", file=sys.stderr)
        print("Run: cmake --build build --target render_export", file=sys.stderr)
        sys.exit(1)

    results = []

    # Process DXF files
    for path in sorted(TEST_DATA.glob("*.dxf")):
        results.append(compare_file(path))

    # Process DWG files (only if reference exists)
    for path in sorted(TEST_DWG.glob("*.dwg")):
        ref_path = REF_DIR / (path.stem + "_summary.json")
        if ref_path.exists():
            results.append(compare_file(path))
        else:
            print(f"\n=== {path.name} === SKIP (no reference)")

    # Summary
    print("\n" + "=" * 60)
    print("COMPARISON SUMMARY")
    print("=" * 60)
    passed = sum(1 for r in results if r["status"] == "PASS")
    warned = sum(1 for r in results if r["status"] == "WARN")
    failed = sum(1 for r in results if r["status"] == "FAIL")
    skipped = sum(1 for r in results if r["status"] == "SKIP")
    total = len(results)

    for r in results:
        print(f"  {r['status']:4s}  {r['file']}")

    print(f"\nTotal: {total} | PASS: {passed} | WARN: {warned} | FAIL: {failed} | SKIP: {skipped}")

    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
