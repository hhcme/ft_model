#!/usr/bin/env python3
"""Generate reference data from DXF/DWG files using ezdxf (MIT) as reference parser.

Outputs structured summaries to test_reference/ for comparison testing.
For DWG files, looks for pre-converted DXF in test_reference/odafc/.
"""
import json, os, sys, math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_DATA = ROOT / "test_data"
TEST_DWG = ROOT / "test_dwg"
REF_DIR = ROOT / "test_reference" / "ezdxf"
ODAF_DIR = ROOT / "test_reference" / "odafc"

ENTITY_TYPES = [
    "LINE", "ARC", "CIRCLE", "TEXT", "MTEXT", "INSERT", "DIMENSION",
    "HATCH", "ELLIPSE", "SPLINE", "POLYLINE", "LWPOLYLINE", "SOLID",
    "POINT", "RAY", "XLINE", "VIEWPORT", "LEADER", "MULTILEADER",
    "MLINE", "TOLERANCE", "3DFACE", "ATTRIB", "MINSERT",
]

MAX_SAMPLES = 20


def extract_summary(doc, source_name: str) -> dict:
    """Extract structured summary from an ezdxf document."""
    msp = doc.modelspace()

    # Entity counts
    counts = {}
    for e in msp:
        t = e.dxftype()
        counts[t] = counts.get(t, 0) + 1

    # Layers
    layers = []
    for layer in doc.layers:
        layers.append({
            "name": layer.dxf.name,
            "color": layer.dxf.color,
            "frozen": layer.is_frozen(),
            "off": layer.is_off(),
            "locked": layer.is_locked(),
        })

    # Blocks
    blocks = sorted(b.dxf.name for b in doc.blocks if not b.name.startswith("*"))

    # Geometry samples
    line_samples = []
    circle_samples = []
    arc_samples = []
    text_samples = []
    insert_samples = []
    hatch_count = 0
    dim_count = 0

    for e in msp:
        t = e.dxftype()
        if t == "LINE" and len(line_samples) < MAX_SAMPLES:
            line_samples.append({
                "start": [e.dxf.start.x, e.dxf.start.y],
                "end": [e.dxf.end.x, e.dxf.end.y],
            })
        elif t == "CIRCLE" and len(circle_samples) < MAX_SAMPLES:
            circle_samples.append({
                "center": [e.dxf.center.x, e.dxf.center.y],
                "radius": e.dxf.radius,
            })
        elif t == "ARC" and len(arc_samples) < MAX_SAMPLES:
            arc_samples.append({
                "center": [e.dxf.center.x, e.dxf.center.y],
                "radius": e.dxf.radius,
                "start_angle": e.dxf.start_angle,
                "end_angle": e.dxf.end_angle,
            })
        elif t in ("TEXT", "MTEXT") and len(text_samples) < MAX_SAMPLES:
            text_samples.append({
                "type": t,
                "text": e.dxf.text if t == "TEXT" else e.text,
                "x": e.dxf.insert.x if t == "TEXT" else e.dxf.insert.x,
                "y": e.dxf.insert.y if t == "TEXT" else e.dxf.insert.y,
                "height": e.dxf.height if hasattr(e.dxf, "height") else 0,
            })
        elif t == "INSERT" and len(insert_samples) < MAX_SAMPLES:
            insert_samples.append({
                "block": e.dxf.name,
                "x": e.dxf.insert.x,
                "y": e.dxf.insert.y,
                "scale_x": e.dxf.get("xscale", 1.0),
                "scale_y": e.dxf.get("yscale", 1.0),
                "rotation": e.dxf.get("rotation", 0.0),
            })
        elif t == "HATCH":
            hatch_count += 1
        elif t == "DIMENSION":
            dim_count += 1

    # Compute bounds
    extents = doc.header.get("$EXTMIN"), doc.header.get("$EXTMAX")
    bounds = {}
    if extents[0] and extents[1]:
        bounds = {
            "min_x": extents[0][0], "min_y": extents[0][1],
            "max_x": extents[1][0], "max_y": extents[1][1],
        }

    return {
        "source": source_name,
        "generator": "ezdxf",
        "entity_counts": counts,
        "total_entities": sum(counts.values()),
        "layers": layers,
        "blocks": blocks,
        "bounds": bounds if bounds else None,
        "line_samples": line_samples,
        "circle_samples": circle_samples,
        "arc_samples": arc_samples,
        "text_samples": text_samples,
        "insert_samples": insert_samples,
        "hatch_count": hatch_count,
        "dimension_count": dim_count,
    }


def find_dwg_dxf(name: str) -> Path | None:
    """Find a pre-converted DXF for a DWG file."""
    candidates = [
        ODAF_DIR / (name.rsplit(".", 1)[0] + ".dxf"),
        TEST_DWG / (name.rsplit(".", 1)[0] + ".dxf"),
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def main():
    import ezdxf

    REF_DIR.mkdir(parents=True, exist_ok=True)
    ODAF_DIR.mkdir(parents=True, exist_ok=True)

    files_processed = []

    # Process DXF test data
    for dxf_path in sorted(TEST_DATA.glob("*.dxf")):
        print(f"Processing DXF: {dxf_path.name}")
        doc = ezdxf.readfile(str(dxf_path))
        summary = extract_summary(doc, dxf_path.name)
        out_path = REF_DIR / (dxf_path.stem + "_summary.json")
        out_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False))
        files_processed.append((dxf_path.name, "dxf", summary["total_entities"]))

    # Process DWG test data (via pre-converted DXF or ezdxf odafc)
    for dwg_path in sorted(TEST_DWG.glob("*.dwg")):
        dxf_path = find_dwg_dxf(dwg_path.name)
        if dxf_path:
            print(f"Processing DWG (via DXF): {dwg_path.name} ← {dxf_path}")
            doc = ezdxf.readfile(str(dxf_path))
            summary = extract_summary(doc, dwg_path.name)
            out_path = REF_DIR / (dwg_path.stem + "_summary.json")
            out_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False))
            files_processed.append((dwg_path.name, "dwg→dxf", summary["total_entities"]))
        else:
            # Try ezdxf odafc addon (requires ODA File Converter installed)
            try:
                from ezdxf.addons import odafc
                print(f"Processing DWG (via ODA): {dwg_path.name}")
                doc = odafc.readfile(str(dwg_path))
                summary = extract_summary(doc, dwg_path.name)
                out_path = REF_DIR / (dwg_path.stem + "_summary.json")
                out_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False))
                files_processed.append((dwg_path.name, "dwg→oda", summary["total_entities"]))
            except Exception as ex:
                print(f"  SKIP (no DXF reference): {dwg_path.name} — {ex}")

    # Summary
    print(f"\n{'File':<40} {'Type':<12} {'Entities':>10}")
    print("-" * 65)
    for name, typ, count in files_processed:
        print(f"{name:<40} {typ:<12} {count:>10}")
    print(f"\nTotal: {len(files_processed)} files processed")
    print(f"Output: {REF_DIR}")


if __name__ == "__main__":
    main()
