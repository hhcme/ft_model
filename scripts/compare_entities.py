#!/usr/bin/env python3
"""compare_entities.py — Structural entity-level comparison between ezdxf and our parser.

Usage:
  python3 compare_entities.py <reference.dxf> <our_entities.json> [--tol 1e-3]

Compares entities extracted by ezdxf (from DXF) against our entity_export JSON output.
Reports matched/missing/extra entities and property-level mismatches.
"""

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path


def extract_ezdxf_entities(dxf_path: str) -> list[dict]:
    """Extract all model-space entities using ezdxf, normalized to our format."""
    import ezdxf
    try:
        doc = ezdxf.readfile(dxf_path)
    except (ezdxf.lldxf.const.DXFStructureError, Exception):
        # libredwg output may have structural issues in TABLES/OBJECTS sections.
        # Extract just the ENTITIES section and build a minimal DXF.
        import tempfile, os
        with open(dxf_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        in_entities = False
        entity_lines = []
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped == "ENTITIES" and i > 0 and lines[i-1].strip() == "2":
                in_entities = True
                entity_lines = ["  0\r\n", "SECTION\r\n", "  2\r\n", "ENTITIES\r\n"]
                continue
            if in_entities:
                entity_lines.append(line)
                if stripped == "ENDSEC" and i > 0 and lines[i-1].strip() == "0":
                    break
        if not entity_lines:
            raise
        with tempfile.NamedTemporaryFile(mode="w", suffix=".dxf", delete=False,
                                          encoding="utf-8") as tmp:
            tmp.write("  0\r\nSECTION\r\n  2\r\nHEADER\r\n")
            tmp.write("  9\r\n$ACADVER\r\n  1\r\nAC1024\r\n")
            tmp.write("  0\r\nENDSEC\r\n")
            for line in entity_lines:
                tmp.write(line)
            tmp.write("  0\r\nEOF\r\n")
            tmp_path = tmp.name
        try:
            doc = ezdxf.readfile(tmp_path)
        finally:
            os.unlink(tmp_path)
    msp = doc.modelspace()
    entities = []

    for e in msp:
        d = {"type": e.dxftype(), "layer": e.dxf.layer if hasattr(e.dxf, "layer") else "0"}

        if e.dxftype() == "LINE":
            d["start"] = [e.dxf.start.x, e.dxf.start.y, e.dxf.start.z]
            d["end"] = [e.dxf.end.x, e.dxf.end.y, e.dxf.end.z]

        elif e.dxftype() == "CIRCLE":
            d["center"] = [e.dxf.center.x, e.dxf.center.y, e.dxf.center.z]
            d["radius"] = e.dxf.radius

        elif e.dxftype() == "ARC":
            d["center"] = [e.dxf.center.x, e.dxf.center.y, e.dxf.center.z]
            d["radius"] = e.dxf.radius
            d["start_angle"] = e.dxf.start_angle
            d["end_angle"] = e.dxf.end_angle

        elif e.dxftype() == "ELLIPSE":
            c = e.dxf.center
            major = e.dxf.major_axis
            ratio = e.dxf.ratio
            major_len = math.sqrt(major.x**2 + major.y**2 + major.z**2)
            rot = math.degrees(math.atan2(major.y, major.x))
            d["center"] = [c.x, c.y, c.z]
            d["major_radius"] = major_len
            d["minor_radius"] = major_len * ratio
            d["rotation"] = rot
            d["start_angle"] = math.degrees(e.dxf.start_param)
            d["end_angle"] = math.degrees(e.dxf.end_param)

        elif e.dxftype() in ("TEXT", "MTEXT"):
            ins = e.dxf.insert if e.dxftype() == "TEXT" else e.dxf.insert
            d["text"] = e.dxf.text if e.dxftype() == "TEXT" else e.text
            d["x"] = float(ins.x)
            d["y"] = float(ins.y)
            # TEXT uses "height", MTEXT uses "char_height"
            try:
                d["height"] = float(e.dxf.height)
            except Exception:
                try:
                    d["height"] = float(e.dxf.char_height)
                except Exception:
                    d["height"] = 0
            try:
                d["rotation"] = float(e.dxf.rotation)
            except Exception:
                pass

        elif e.dxftype() == "LWPOLYLINE":
            pts = list(e.get_points(format="xyb"))
            d["vertices"] = [[float(p[0]), float(p[1]), 0] for p in pts]
            d["bulges"] = [float(p[2]) if len(p) > 2 else 0 for p in pts]
            d["closed"] = e.closed

        elif e.dxftype() == "POLYLINE":
            pts = list(e.vertices)
            d["vertices"] = [[v.dxf.location.x, v.dxf.location.y, v.dxf.location.z] for v in pts]
            d["closed"] = e.is_closed

        elif e.dxftype() == "INSERT":
            d["block"] = e.dxf.name
            d["x"] = e.dxf.insert.x
            d["y"] = e.dxf.insert.y
            if e.dxf.xscale != 1:
                d["scale_x"] = e.dxf.xscale
            if e.dxf.yscale != 1:
                d["scale_y"] = e.dxf.yscale
            if e.dxf.rotation != 0:
                d["rotation"] = e.dxf.rotation
            if hasattr(e, "column_count") and e.dxf.column_count > 1:
                d["columns"] = e.dxf.column_count
                d["rows"] = e.dxf.row_count
                d["col_spacing"] = e.dxf.column_spacing
                d["row_spacing"] = e.dxf.row_spacing

        elif e.dxftype() == "SPLINE":
            try:
                cps = e.control_points
                if cps:
                    d["control_points"] = [[float(p[0]), float(p[1]), float(p[2])] for p in cps]
            except Exception:
                pass
            try:
                fps = e.fit_points
                if fps:
                    d["fit_points"] = [[float(p[0]), float(p[1]), float(p[2])] for p in fps]
            except Exception:
                pass
            d["degree"] = int(e.dxf.degree) if hasattr(e.dxf, "degree") else 3
            if hasattr(e.dxf, "flags"):
                d["closed"] = bool(e.dxf.flags & 1)

        elif e.dxftype() == "ELLIPSE":
            c = e.dxf.center
            major = e.dxf.major_axis
            ratio = e.dxf.ratio
            major_len = math.sqrt(major.x**2 + major.y**2 + major.z**2)
            rot = math.degrees(math.atan2(major.y, major.x))
            d["center"] = [c.x, c.y, c.z]
            d["major_radius"] = major_len
            d["minor_radius"] = major_len * ratio
            d["ratio"] = ratio
            d["rotation"] = rot
            d["start_angle"] = math.degrees(e.dxf.start_param)
            d["end_angle"] = math.degrees(e.dxf.end_param)

        elif e.dxftype() == "DIMENSION":
            d["definition_point"] = [e.dxf.defpoint.x, e.dxf.defpoint.y, e.dxf.defpoint.z]
            if e.dxf.text_midpoint:
                d["text_midpoint"] = [e.dxf.text_midpoint.x, e.dxf.text_midpoint.y, e.dxf.text_midpoint.z]
            d["text"] = e.dxf.text if hasattr(e.dxf, "text") else ""
            if hasattr(e.dxf, "dimtype"):
                d["dim_type"] = e.dxf.dimtype

        elif e.dxftype() == "HATCH":
            d["pattern"] = e.dxf.pattern_name if hasattr(e.dxf, "pattern_name") else ""
            d["solid"] = e.dxf.solid_fill if hasattr(e.dxf, "solid_fill") else False
            d["loop_count"] = len(e.paths) if hasattr(e, "paths") else 0

        elif e.dxftype() == "SOLID":
            # SOLID has 4 corner points
            pts = []
            for attr in ("first", "second", "third", "fourth"):
                if hasattr(e.dxf, attr):
                    p = getattr(e.dxf, attr)
                    pts.append([p.x, p.y, p.z])
            d["corners"] = pts

        elif e.dxftype() == "POINT":
            p = e.dxf.location
            d["position"] = [p.x, p.y, p.z]

        entities.append(d)

    return entities


def load_our_entities(json_path: str) -> list[dict]:
    """Load entities from our entity_export JSON."""
    with open(json_path) as f:
        data = json.load(f)
    return data.get("entities", []), data.get("entity_counts", {})


def _num(v):
    """Ensure a numeric value (not numpy type)."""
    return float(v) if hasattr(v, '__float__') else v

def characteristic_point(e: dict) -> tuple:
    """Return (x, y) characteristic point for spatial matching."""
    if "start" in e:
        s, d = e["start"], e["end"]
        return (_num((s[0]+d[0])/2), _num((s[1]+d[1])/2))
    if "center" in e:
        return (_num(e["center"][0]), _num(e["center"][1]))
    if "x" in e:
        return (_num(e["x"]), _num(e["y"]))
    if "vertices" in e and e["vertices"]:
        v = e["vertices"][0]
        return (_num(v[0]), _num(v[1]))
    if "control_points" in e and e["control_points"]:
        v = e["control_points"][0]
        return (_num(v[0]), _num(v[1]))
    if "fit_points" in e and e["fit_points"]:
        v = e["fit_points"][0]
        return (_num(v[0]), _num(v[1]))
    if "position" in e:
        return (_num(e["position"][0]), _num(e["position"][1]))
    if "definition_point" in e:
        return (_num(e["definition_point"][0]), _num(e["definition_point"][1]))
    if "corners" in e and e["corners"]:
        return (_num(e["corners"][0][0]), _num(e["corners"][0][1]))
    if "position" in e:
        return (_num(e["position"][0]), _num(e["position"][1]))
    return (0.0, 0.0)


def scene_range(ref_entities: list[dict]) -> float:
    """Compute approximate scene size for tolerance scaling."""
    if not ref_entities:
        return 1.0
    min_x = min_y = float("inf")
    max_x = max_y = float("-inf")
    for e in ref_entities:
        pt = characteristic_point(e)
        if isinstance(pt, tuple) and len(pt) >= 2:
            px, py = pt[0], pt[1]
        else:
            continue
        if not (isinstance(px, (int, float)) and isinstance(py, (int, float))):
            continue
        min_x = min(min_x, px); max_x = max(max_x, px)
        min_y = min(min_y, py); max_y = max(max_y, py)
    return max(max_x - min_x, max_y - min_y, 1.0)


def match_entities(ref: list[dict], ours: list[dict], sr: float, tol: float = 1e-3):
    """Greedy nearest-neighbor matching by type + spatial proximity.
    Returns (matched_pairs, missing_from_ours, extra_in_ours)."""
    by_type_ref = defaultdict(list)
    by_type_ours = defaultdict(list)
    for e in ref:
        by_type_ref[e["type"]].append(e)
    for e in ours:
        by_type_ours[e["type"]].append(e)

    max_dist = sr * 0.01  # 1% of scene range as max matching distance
    matched = []
    missing = []
    extra = []

    all_types = set(by_type_ref.keys()) | set(by_type_ours.keys())
    for t in all_types:
        ref_list = list(by_type_ref.get(t, []))
        our_list = list(by_type_ours.get(t, []))
        used = set()

        for re in ref_list:
            rp = characteristic_point(re)
            best_idx = -1
            best_dist = float("inf")
            for i, oe in enumerate(our_list):
                if i in used:
                    continue
                op = characteristic_point(oe)
                d = math.sqrt((rp[0]-op[0])**2 + (rp[1]-op[1])**2)
                if d < best_dist:
                    best_dist = d
                    best_idx = i
            if best_idx >= 0 and best_dist <= max(max_dist, tol):
                matched.append((re, our_list[best_idx]))
                used.add(best_idx)
            else:
                missing.append(re)

        for i, oe in enumerate(our_list):
            if i not in used:
                extra.append(oe)

    return matched, missing, extra


def compare_props(ref: dict, ours: dict, tol: float = 1e-3) -> list[dict]:
    """Compare properties of matched entity pair. Returns list of mismatches."""
    diffs = []

    def check_num(name, rv, ov):
        if rv is None or ov is None:
            return
        if isinstance(rv, (list, tuple)):
            for i, (a, b) in enumerate(zip(rv, ov)):
                if abs(a - b) > tol:
                    diffs.append({"prop": f"{name}[{i}]", "ref": a, "ours": b, "delta": abs(a-b)})
        elif isinstance(rv, (int, float)):
            if abs(rv - ov) > tol:
                diffs.append({"prop": name, "ref": rv, "ours": ov, "delta": abs(rv-ov)})

    def check_str(name, rv, ov):
        if rv != ov:
            diffs.append({"prop": name, "ref": rv, "ours": ov})

    if ref["type"] != ours["type"]:
        diffs.append({"prop": "type", "ref": ref["type"], "ours": ours["type"]})
        return diffs

    t = ref["type"]
    if t == "LINE":
        check_num("start", ref.get("start"), ours.get("start"))
        check_num("end", ref.get("end"), ours.get("end"))
    elif t in ("CIRCLE",):
        check_num("center", ref.get("center"), ours.get("center"))
        check_num("radius", ref.get("radius"), ours.get("radius"))
    elif t == "ARC":
        check_num("center", ref.get("center"), ours.get("center"))
        check_num("radius", ref.get("radius"), ours.get("radius"))
        check_num("start_angle", ref.get("start_angle"), ours.get("start_angle"))
        check_num("end_angle", ref.get("end_angle"), ours.get("end_angle"))
    elif t == "ELLIPSE":
        check_num("center", ref.get("center"), ours.get("center"))
        check_num("major_radius", ref.get("major_radius"), ours.get("major_radius"))
        check_num("minor_radius", ref.get("minor_radius"), ours.get("minor_radius"))
        check_num("rotation", ref.get("rotation"), ours.get("rotation"))
    elif t in ("TEXT", "MTEXT"):
        check_str("text", ref.get("text", "").strip(), ours.get("text", "").strip())
        check_num("x", ref.get("x"), ours.get("x"))
        check_num("y", ref.get("y"), ours.get("y"))
        check_num("height", ref.get("height"), ours.get("height"))
    elif t == "INSERT":
        check_str("block", ref.get("block", ""), ours.get("block", ""))
        check_num("x", ref.get("x"), ours.get("x"))
        check_num("y", ref.get("y"), ours.get("y"))
    elif t in ("LWPOLYLINE", "POLYLINE"):
        rv = ref.get("vertices", [])
        ov = ours.get("vertices", [])
        if abs(len(rv) - len(ov)) > 0:
            diffs.append({"prop": "vertex_count", "ref": len(rv), "ours": len(ov)})
        else:
            for i, (a, b) in enumerate(zip(rv, ov)):
                check_num(f"v[{i}]", a, b)
    elif t == "SPLINE":
        check_num("degree", ref.get("degree"), ours.get("degree"))
        if ref.get("control_points") and ours.get("control_points"):
            rv = ref["control_points"]
            ov = ours["control_points"]
            if abs(len(rv) - len(ov)) > 0:
                diffs.append({"prop": "cp_count", "ref": len(rv), "ours": len(ov)})
            else:
                for i, (a, b) in enumerate(zip(rv, ov)):
                    check_num(f"cp[{i}]", a, b)
        if ref.get("fit_points") and ours.get("fit_points"):
            rv = ref["fit_points"]
            ov = ours["fit_points"]
            if abs(len(rv) - len(ov)) > 0:
                diffs.append({"prop": "fp_count", "ref": len(rv), "ours": len(ov)})
            else:
                for i, (a, b) in enumerate(zip(rv, ov)):
                    check_num(f"fp[{i}]", a, b)
    elif t == "ELLIPSE":
        check_num("center", ref.get("center"), ours.get("center"))
        check_num("major_radius", ref.get("major_radius"), ours.get("major_radius"))
        check_num("minor_radius", ref.get("minor_radius"), ours.get("minor_radius"))
        check_num("rotation", ref.get("rotation"), ours.get("rotation"))
    elif t == "DIMENSION":
        check_num("definition_point", ref.get("definition_point"), ours.get("definition_point"))
        check_str("text", ref.get("text", ""), ours.get("text", ""))
    elif t == "HATCH":
        check_str("pattern", ref.get("pattern", ""), ours.get("pattern", ""))
        check_num("loop_count", ref.get("loop_count"), ours.get("loop_count"))
    elif t == "SOLID":
        rv = ref.get("corners", [])
        ov = ours.get("corners", [])
        if abs(len(rv) - len(ov)) > 0:
            diffs.append({"prop": "corner_count", "ref": len(rv), "ours": len(ov)})
        else:
            for i, (a, b) in enumerate(zip(rv, ov)):
                check_num(f"corner[{i}]", a, b)
    elif t == "POINT":
        check_num("position", ref.get("position"), ours.get("position"))

    return diffs


def run_comparison(dxf_path: str, our_json_path: str, tol: float = 1e-3) -> dict:
    """Main comparison entry point."""
    ref_entities = extract_ezdxf_entities(dxf_path)
    our_entities, our_counts = load_our_entities(our_json_path)

    # Filter: only model-space, non-block entities for fair comparison
    ref_filtered = [e for e in ref_entities]
    our_filtered = [e for e in our_entities if not e.get("in_block", False) and e.get("space") == "model"]

    sr = scene_range(ref_filtered)
    matched, missing, extra = match_entities(ref_filtered, our_filtered, sr, tol)

    # Property comparison
    all_diffs = []
    for re, oe in matched:
        diffs = compare_props(re, oe, tol)
        if diffs:
            all_diffs.append({"type": re["type"], "ref_point": characteristic_point(re), "diffs": diffs})

    ref_types = Counter(e["type"] for e in ref_filtered)
    our_types = Counter(e["type"] for e in our_filtered)

    return {
        "status": "PASS" if not missing and not extra and not all_diffs else "FAIL",
        "ref_total": len(ref_filtered),
        "our_total": len(our_filtered),
        "matched": len(matched),
        "missing": len(missing),
        "extra": len(extra),
        "property_mismatches": len(all_diffs),
        "ref_types": dict(ref_types),
        "our_types": dict(our_types),
        "mismatches": all_diffs[:20],  # limit output
        "missing_entities": [{"type": e["type"], "point": characteristic_point(e)} for e in missing[:10]],
        "extra_entities": [{"type": e["type"], "point": characteristic_point(e)} for e in extra[:10]],
    }


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <reference.dxf> <our_entities.json> [--tol 1e-3]")
        sys.exit(2)

    dxf_path = sys.argv[1]
    json_path = sys.argv[2]
    tol = 1e-3
    if len(sys.argv) > 3 and sys.argv[3] == "--tol" and len(sys.argv) > 4:
        tol = float(sys.argv[4])

    result = run_comparison(dxf_path, json_path, tol)

    print(f"Status: {result['status']}")
    print(f"Reference (ezdxf): {result['ref_total']} entities")
    print(f"Ours (entity_export): {result['our_total']} entities")
    print(f"Matched: {result['matched']}, Missing: {result['missing']}, Extra: {result['extra']}")
    print(f"Property mismatches: {result['property_mismatches']}")
    print()

    print("Type comparison:")
    all_types = sorted(set(result["ref_types"]) | set(result["our_types"]))
    for t in all_types:
        rc = result["ref_types"].get(t, 0)
        oc = result["our_types"].get(t, 0)
        mark = " OK" if rc == oc else " !!"
        print(f"  {t:16s} ref={rc:6d}  ours={oc:6d}{mark}")

    if result["mismatches"]:
        print(f"\nProperty mismatches (first {len(result['mismatches'])}):")
        for m in result["mismatches"]:
            for d in m["diffs"]:
                print(f"  {m['type']} at ({m['ref_point'][0]:.1f},{m['ref_point'][1]:.1f}): "
                      f"{d['prop']} ref={d.get('ref','?')} ours={d.get('ours','?')}")

    if result["missing_entities"]:
        print(f"\nMissing from ours (first {len(result['missing_entities'])}):")
        for e in result["missing_entities"]:
            print(f"  {e['type']} at ({e['point'][0]:.1f}, {e['point'][1]:.1f})")

    if result["extra_entities"]:
        print(f"\nExtra in ours (first {len(result['extra_entities'])}):")
        for e in result["extra_entities"]:
            print(f"  {e['type']} at ({e['point'][0]:.1f}, {e['point'][1]:.1f})")

    sys.exit(0 if result["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
