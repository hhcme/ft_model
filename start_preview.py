#!/usr/bin/env python3
from __future__ import annotations

"""
DWG/DXF 预览服务器

启动后：
  http://localhost:2415/  → preview.html（加载 ?data=... 参数指定的文件）
  POST /parse             → 接收 DWG/DXF 文件，调用 C++ render_export，返回 JSON

前端预览页选择本地 DWG/DXF 文件 → 自动上传到 /parse → 返回 JSON 并渲染。
"""

import base64
import hashlib
import gzip
import http.server
import json
import os
import sys
import subprocess
import tempfile
import time
from pathlib import Path
import shutil
from urllib.parse import unquote

PORT = 2415
PROJECT_ROOT = Path(__file__).parent.resolve()
RENDER_EXPORT = PROJECT_ROOT / "build/core/test/render_export"
ENTITY_EXPORT = PROJECT_ROOT / "build/core/test/entity_export"
DWG2DXF = Path(os.environ.get("FT_DWG2DXF", "/tmp/libredwg-0.13.4/programs/dwg2dxf"))
QCAD_RENDERER = os.environ.get("FT_QCAD_RENDERER")
STATIC_DIR = PROJECT_ROOT / "platforms/electron"
REF_CACHE_DIR = Path(os.environ.get("FT_REFERENCE_CACHE_DIR", "/tmp/ft_model_reference_cache"))
REFERENCE_CACHE_VERSION = os.environ.get("FT_REFERENCE_CACHE_VERSION", "qcad-hires-0.3")
QCAD_DEFAULT_WIDTH = int(os.environ.get("FT_QCAD_WIDTH", "3840"))
QCAD_DEFAULT_HEIGHT = int(os.environ.get("FT_QCAD_HEIGHT", "2880"))
SCRIPTS_DIR = PROJECT_ROOT / "scripts"
SCS_ROOT = PROJECT_ROOT / "scs_dwg"
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

def run_render_export(input_path: str) -> tuple[bytes, str]:
    """Run our C++ render_export. Returns (gzip_json_bytes, error_msg)."""
    out_path = input_path + ".json.gz"
    cmd = [str(RENDER_EXPORT), input_path, out_path]
    print(f"[render] Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        return b"", result.stderr[:500]
    if not os.path.exists(out_path):
        return b"", "No output generated"
    with open(out_path, "rb") as f:
        data = f.read()
    os.unlink(out_path)
    return data, ""


def run_entity_export(input_path: str, output_path: str) -> str:
    """Run raw entity export. Returns empty string on success."""
    if not ENTITY_EXPORT.exists():
        return f"entity_export not found: {ENTITY_EXPORT}"
    result = subprocess.run(
        [str(ENTITY_EXPORT), input_path, output_path],
        capture_output=True, text=True, timeout=300,
    )
    if result.returncode != 0:
        return result.stderr[-500:] or result.stdout[-500:] or "entity_export failed"
    if not os.path.exists(output_path):
        return "entity_export produced no output"
    return ""


def convert_dwg_to_dxf(input_path: str, output_path: str) -> str:
    """Convert DWG to DXF through the test-only reference parser."""
    if not DWG2DXF.exists():
        return f"dwg2dxf not found: {DWG2DXF} (set FT_DWG2DXF)"

    result = subprocess.run(
        [str(DWG2DXF), "-m", input_path, "-o", output_path],
        capture_output=True, text=True, timeout=300,
    )
    if result.returncode != 0 or not os.path.exists(output_path) or os.path.getsize(output_path) == 0:
        result = subprocess.run(
            [str(DWG2DXF), input_path, "-o", output_path],
            capture_output=True, text=True, timeout=300,
        )
    if not os.path.exists(output_path) or os.path.getsize(output_path) == 0:
        return f"dwg2dxf failed: {(result.stderr or result.stdout)[-500:]}"
    return ""


def encode_file_b64(path: str) -> str:
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode("ascii")


def file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def tool_version(cmd: list[str]) -> str:
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        return (result.stdout or result.stderr).strip().splitlines()[0][:160]
    except Exception:
        return "unknown"


def find_qcad_renderer() -> Path | None:
    candidates: list[Path] = []
    if QCAD_RENDERER:
        candidates.append(Path(QCAD_RENDERER))
    for name in ("dwg2png", "dwg2bmp"):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for app_glob in (
        "/Applications/QCAD*.app/Contents/Resources/dwg2png",
        "/Applications/QCAD*.app/Contents/Resources/dwg2bmp",
        f"{Path.home()}/Applications/QCAD*.app/Contents/Resources/dwg2png",
        f"{Path.home()}/Applications/QCAD*.app/Contents/Resources/dwg2bmp",
    ):
        candidates.extend(Path("/").glob(app_glob.lstrip("/")))
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate
    return None


def active_reference_provider() -> dict:
    qcad = find_qcad_renderer()
    if qcad:
        return {
            "id": "qcad-cli",
            "strength": "medium",
            "rendererPath": str(qcad),
            "renderer": qcad.name,
        }
    return {
        "id": "libredwg-ezdxf",
        "strength": "weak",
        "rendererPath": "",
        "renderer": "ezdxf.addons.drawing.matplotlib",
    }


def reference_cache_paths(fingerprint: str, provider_id: str | None = None) -> dict[str, Path]:
    provider = provider_id or active_reference_provider()["id"]
    root = REF_CACHE_DIR / provider / fingerprint[:2] / fingerprint
    return {
        "root": root,
        "meta": root / "metadata.json",
        "png": root / "reference.png",
        "dxf": root / "reference.dxf",
    }


def load_reference_cache(fingerprint: str, provider_id: str | None = None) -> dict | None:
    provider = active_reference_provider() if provider_id is None else {"id": provider_id}
    paths = reference_cache_paths(fingerprint, provider["id"])
    if not paths["meta"].exists() or not paths["png"].exists():
        return None
    try:
        metadata = json.loads(paths["meta"].read_text(encoding="utf-8"))
        if metadata.get("cacheVersion") != REFERENCE_CACHE_VERSION:
            return None
        return {
            "refPng": encode_file_b64(str(paths["png"])),
            "refError": None,
            "entityCompare": metadata.get("entityCompare"),
            "visualCompare": None,
            "errors": {
                "ourParser": None,
                "reference": None,
                "entityCompare": metadata.get("entityCompareError"),
                "visualCompare": None,
            },
            "refInfo": {
                "entityCount": (metadata.get("entityCompare") or {}).get("refEntityCount", 0),
                "ourEntityCount": (metadata.get("entityCompare") or {}).get("ourEntityCount", 0),
                "refEntityCount": (metadata.get("entityCompare") or {}).get("refEntityCount", 0),
                "entityTypeCounts": (metadata.get("entityCompare") or {}).get("entityTypeCounts", {}),
                "missing": (metadata.get("entityCompare") or {}).get("missing", 0),
                "extra": (metadata.get("entityCompare") or {}).get("extra", 0),
                "renderTimeMs": 0,
                "ourRenderTimeMs": 0,
                "entityCompareTimeMs": metadata.get("entityCompareTimeMs", 0),
            },
            "referenceMeta": {**metadata, "cacheHit": True},
        }
    except Exception as ex:
        print(f"[reference-cache] read failed: {ex}")
        return None


def save_reference_cache(fingerprint: str, png_path: str, dxf_path: str | None, metadata: dict) -> None:
    provider_id = metadata.get("provider") or metadata.get("parserFramework", {}).get("provider") or active_reference_provider()["id"]
    paths = reference_cache_paths(fingerprint, provider_id)
    paths["root"].mkdir(parents=True, exist_ok=True)
    if os.path.exists(png_path):
        import shutil
        shutil.copy2(png_path, paths["png"])
    if dxf_path and os.path.exists(dxf_path):
        import shutil
        shutil.copy2(dxf_path, paths["dxf"])
        metadata["referenceDxfPath"] = str(paths["dxf"])
    metadata["cacheVersion"] = REFERENCE_CACHE_VERSION
    paths["meta"].write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")


def run_entity_comparison(input_path: str, ref_dxf_path: str | None) -> tuple[dict | None, str, int]:
    """Compare raw entity output against the third-party DXF reference."""
    t0 = time.time()
    if ref_dxf_path is None:
        return None, "no reference DXF", 0
    our_entities_path = input_path + ".entities.json"
    try:
        err = run_entity_export(input_path, our_entities_path)
        if err:
            return None, err, int((time.time() - t0) * 1000)

        from compare_entities import run_comparison

        result = run_comparison(ref_dxf_path, our_entities_path, 0.1)
        ref_total = int(result.get("ref_total", 0))
        our_total = int(result.get("our_total", 0))
        compare = {
            "status": result.get("status", "FAIL"),
            "ourEntityCount": our_total,
            "refEntityCount": ref_total,
            "matched": int(result.get("matched", 0)),
            "missing": int(result.get("missing", 0)),
            "extra": int(result.get("extra", 0)),
            "propertyMismatches": int(result.get("property_mismatches", 0)),
            "entityTypeCounts": {
                "ours": result.get("our_types", {}),
                "reference": result.get("ref_types", {}),
            },
            "missingSamples": result.get("missing_entities", []),
            "extraSamples": result.get("extra_entities", []),
        }
        if ref_total > 0:
            compare["entityCountDiffPct"] = abs(our_total - ref_total) / ref_total
        return compare, "", int((time.time() - t0) * 1000)
    except Exception as ex:
        return None, str(ex)[:500], int((time.time() - t0) * 1000)
    finally:
        if os.path.exists(our_entities_path):
            os.unlink(our_entities_path)


def render_dxf_to_png(dxf_path: str, output_png: str, width: int = 1920, height: int = 1080) -> str:
    """Render DXF to PNG using ezdxf + matplotlib with dark background.
    Returns empty string on success, error message on failure."""
    import ezdxf
    from ezdxf.addons.drawing import Frontend, RenderContext
    from ezdxf.addons.drawing.matplotlib import MatplotlibBackend
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    try:
        doc = ezdxf.readfile(dxf_path)
    except Exception as ex:
        return f"ezdxf readfile failed: {str(ex)[:200]}"

    msp = doc.modelspace()

    # Fix missing block definitions that libredwg may have dropped
    for insert in msp.query('INSERT'):
        try:
            block_name = insert.dxf.name
            if block_name not in doc.blocks:
                doc.blocks.new(block_name)
                print(f"[render] Created missing block definition: {block_name}")
        except Exception:
            pass

    try:
        fig, ax = plt.subplots(figsize=(width / 100, height / 100), dpi=100)
        fig.patch.set_facecolor("#1e1e2e")
        ax.set_facecolor("#1e1e2e")
        ctx = RenderContext(doc)
        backend = MatplotlibBackend(ax)
        Frontend(ctx, backend).draw_layout(msp)
        fig.savefig(output_png, dpi=100, facecolor="#1e1e2e", bbox_inches="tight")
        plt.close(fig)
        print(f"[render] ezdxf ref -> {output_png}")
        return ""
    except Exception as ex:
        try:
            plt.close('all')
        except Exception:
            pass
        print(f"[render] ezdxf frontend failed, using safe fallback: {ex}")
        return render_dxf_to_png_fallback(dxf_path, output_png, width, height, f"ezdxf frontend failed: {ex}")


def render_dxf_to_png_fallback(dxf_path: str, output_png: str, width: int, height: int, reason: str) -> str:
    """Best-effort reference renderer that skips problematic DXF entities/styles."""
    try:
        from compare_entities import extract_ezdxf_entities
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.patches import Circle as MplCircle, Arc as MplArc

        entities = extract_ezdxf_entities(dxf_path)
        fig, ax = plt.subplots(figsize=(width / 100, height / 100), dpi=100)
        fig.patch.set_facecolor("#1e1e2e")
        ax.set_facecolor("#1e1e2e")
        ax.set_aspect("equal", adjustable="box")
        ax.axis("off")

        xs: list[float] = []
        ys: list[float] = []

        def add_pt(x, y):
            if isinstance(x, (int, float)) and isinstance(y, (int, float)):
                if abs(x) < 1e9 and abs(y) < 1e9:
                    xs.append(float(x)); ys.append(float(y))

        def color():
            return "#d8d8d8"

        for ent in entities:
            try:
                t = ent.get("type")
                if t == "LINE":
                    x0, y0 = ent["start"][0], ent["start"][1]
                    x1, y1 = ent["end"][0], ent["end"][1]
                    ax.plot([x0, x1], [y0, y1], color=color(), linewidth=0.4)
                    add_pt(x0, y0); add_pt(x1, y1)
                elif t in ("LWPOLYLINE", "POLYLINE"):
                    pts = ent.get("vertices") or []
                    if len(pts) >= 2:
                        px = [p[0] for p in pts]
                        py = [p[1] for p in pts]
                        if ent.get("closed"):
                            px.append(px[0]); py.append(py[0])
                        ax.plot(px, py, color=color(), linewidth=0.4)
                        for x, y in zip(px, py): add_pt(x, y)
                elif t == "CIRCLE":
                    cx, cy = ent["center"][0], ent["center"][1]
                    r = ent.get("radius", 0)
                    ax.add_patch(MplCircle((cx, cy), r, fill=False, edgecolor=color(), linewidth=0.4))
                    add_pt(cx - r, cy - r); add_pt(cx + r, cy + r)
                elif t == "ARC":
                    cx, cy = ent["center"][0], ent["center"][1]
                    r = ent.get("radius", 0)
                    ax.add_patch(MplArc((cx, cy), 2 * r, 2 * r,
                                        theta1=ent.get("start_angle", 0),
                                        theta2=ent.get("end_angle", 360),
                                        edgecolor=color(), linewidth=0.4))
                    add_pt(cx - r, cy - r); add_pt(cx + r, cy + r)
                elif t in ("TEXT", "MTEXT"):
                    x, y = ent.get("x", 0), ent.get("y", 0)
                    txt = str(ent.get("text", ""))[:80]
                    if txt:
                        ax.text(x, y, txt, color="#a9b7d0", fontsize=3, rotation=ent.get("rotation", 0))
                        add_pt(x, y)
            except Exception:
                continue

        if xs and ys:
            xs2 = sorted(xs); ys2 = sorted(ys)
            lo = max(0, int(len(xs2) * 0.01))
            hi = min(len(xs2) - 1, int(len(xs2) * 0.99))
            min_x, max_x = xs2[lo], xs2[hi]
            min_y, max_y = ys2[lo], ys2[hi]
            pad = max(max_x - min_x, max_y - min_y, 1.0) * 0.05
            ax.set_xlim(min_x - pad, max_x + pad)
            ax.set_ylim(min_y - pad, max_y + pad)

        fig.savefig(output_png, dpi=100, facecolor="#1e1e2e", bbox_inches="tight")
        plt.close(fig)
        print(f"[render] fallback ref -> {output_png} ({reason})")
        return ""
    except Exception as ex:
        try:
            plt.close('all')
        except Exception:
            pass
        return f"{reason}; fallback failed: {str(ex)[:300]}"


def render_with_qcad(input_path: str, output_png: str) -> tuple[bool, str, dict]:
    renderer = find_qcad_renderer()
    if not renderer:
        return False, "QCAD renderer not found", {}

    started = time.time()
    renderer_name = renderer.name.lower()
    output_path = Path(output_png)
    tmp_output = output_path
    if renderer_name == "dwg2bmp":
        tmp_output = output_path.with_suffix(".bmp")

    commands: list[list[str]] = []
    # QCAD command-line tools vary slightly by version. Try the documented
    # option form first, then positional forms used by older builds.
    render_options = [
        "-a",
        "-c",
        "-f",
        "-x",
        str(QCAD_DEFAULT_WIDTH),
        "-y",
        str(QCAD_DEFAULT_HEIGHT),
    ]
    commands.append([str(renderer), *render_options, "-o", str(tmp_output), input_path])
    commands.append([str(renderer), "-o", str(tmp_output), input_path])
    commands.append([str(renderer), input_path, str(tmp_output)])

    last = None
    for cmd in commands:
        last = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if last.returncode == 0 and tmp_output.exists() and tmp_output.stat().st_size > 0:
            if tmp_output != output_path:
                try:
                    from PIL import Image
                    Image.open(tmp_output).save(output_path)
                    tmp_output.unlink(missing_ok=True)
                except Exception as ex:
                    return False, f"QCAD rendered BMP but PNG conversion failed: {ex}", {}
            pdf_fallback = maybe_replace_sparse_qcad_png(input_path, str(output_path))
            return True, "", {
                "renderTimeMs": int((time.time() - started) * 1000),
                "command": cmd,
                "rendererVersion": tool_version([str(renderer), "-h"]),
                "outputWidth": QCAD_DEFAULT_WIDTH,
                "outputHeight": QCAD_DEFAULT_HEIGHT,
                **pdf_fallback,
            }

    stderr = (last.stderr or last.stdout if last else "QCAD failed")[-1000:]
    return False, f"QCAD render failed: {stderr}", {}


def image_ink_ratio(png_path: str) -> tuple[float, tuple[int, int, int, int] | None]:
    try:
        from PIL import Image
        image = Image.open(png_path).convert("RGB")
        bg = image.getpixel((0, 0))
        pixels = image.load()
        width, height = image.size
        ink = 0
        min_x, min_y, max_x, max_y = width, height, 0, 0
        for y in range(height):
            for x in range(width):
                r, g, b = pixels[x, y]
                if max(abs(r - bg[0]), abs(g - bg[1]), abs(b - bg[2])) > 8:
                    ink += 1
                    min_x = min(min_x, x)
                    min_y = min(min_y, y)
                    max_x = max(max_x, x)
                    max_y = max(max_y, y)
        if ink == 0:
            return 0.0, None
        return ink / float(width * height), (min_x, min_y, max_x + 1, max_y + 1)
    except Exception:
        return 1.0, None


def render_pdf_to_png(pdf_path: Path, output_png: str) -> str:
    try:
        import pypdfium2 as pdfium
        pdf = pdfium.PdfDocument(str(pdf_path))
        page = pdf[0]
        bitmap = page.render(scale=2.0)
        image = bitmap.to_pil().convert("RGB")
        image.save(output_png)
        return ""
    except Exception as ex:
        pdftoppm = shutil.which("pdftoppm")
        if not pdftoppm:
            return f"PDF fallback failed: {ex}"
        prefix = str(Path(output_png).with_suffix(""))
        result = subprocess.run(
            [pdftoppm, "-png", "-singlefile", "-r", "160", str(pdf_path), prefix],
            capture_output=True,
            text=True,
            timeout=120,
        )
        generated = prefix + ".png"
        if result.returncode != 0 or not os.path.exists(generated):
            return (result.stderr or result.stdout or str(ex))[-500:]
        if generated != output_png:
            shutil.move(generated, output_png)
        return ""


def maybe_replace_sparse_qcad_png(input_path: str, output_png: str) -> dict:
    ratio, bbox = image_ink_ratio(output_png)
    if ratio >= float(os.environ.get("FT_QCAD_SPARSE_INK_THRESHOLD", "0.002")):
        return {"inkRatio": ratio, "inkBounds": bbox}

    pdf_path = Path(input_path).with_suffix(".pdf")
    if not pdf_path.exists():
        return {"inkRatio": ratio, "inkBounds": bbox, "sparseQcad": True}

    err = render_pdf_to_png(pdf_path, output_png)
    if err:
        return {"inkRatio": ratio, "inkBounds": bbox, "sparseQcad": True, "pdfFallbackError": err}
    pdf_ratio, pdf_bbox = image_ink_ratio(output_png)
    print(f"[render] QCAD sparse output ratio={ratio:.6f}; used sibling PDF fallback {pdf_path.name}")
    return {
        "inkRatio": pdf_ratio,
        "inkBounds": pdf_bbox,
        "sparseQcad": True,
        "pdfFallback": str(pdf_path),
        "actualRenderer": "sibling-pdf",
    }


def run_our_parser(tmp_path: str) -> tuple[bytes | None, str, int]:
    """Run our C++ parser in a background thread. Returns (gzip_data, error_msg, elapsed_ms)."""
    import time
    t0 = time.time()
    ours_data, err = run_render_export(tmp_path)
    elapsed = int((time.time() - t0) * 1000)
    if err:
        return None, err, elapsed
    return ours_data, "", elapsed


def run_reference_render(tmp_path: str, is_dwg: bool, ref_png_path: str, ref_dxf_path: str) -> tuple[str | None, str, int, dict]:
    """Run reference renderer (libredwg + ezdxf) in a background thread.
    Returns (base64_png, error_msg, elapsed_ms, render_info)."""
    import time
    t0 = time.time()
    try:
        provider = active_reference_provider()
        if provider["id"] == "qcad-cli":
            ok, err, info = render_with_qcad(tmp_path, ref_png_path)
            if ok:
                elapsed = int((time.time() - t0) * 1000)
                return encode_file_b64(ref_png_path), "", elapsed, info
            print(f"[render] QCAD failed, falling back to libredwg+ezdxf: {err}")

        if is_dwg:
            err = convert_dwg_to_dxf(tmp_path, ref_dxf_path)
            if err:
                elapsed = int((time.time() - t0) * 1000)
                return None, err, elapsed, {}
        else:
            ref_dxf_path = tmp_path

        err = render_dxf_to_png(ref_dxf_path, ref_png_path)
        if err:
            elapsed = int((time.time() - t0) * 1000)
            return None, err, elapsed, {}

        elapsed = int((time.time() - t0) * 1000)
        return encode_file_b64(ref_png_path), "", elapsed, {}
    except Exception as ex:
        elapsed = int((time.time() - t0) * 1000)
        return None, str(ex)[:500], elapsed, {}


def run_visual_comparison_from_pngs(ref_png_path: str, our_png_path: str, diff_png_path: str) -> tuple[dict | None, str]:
    try:
        if not os.path.exists(ref_png_path) or not os.path.exists(our_png_path):
            return None, "reference or ours PNG missing"
        from visual_compare import compare_images

        result = compare_images(ref_png_path, our_png_path, diff_png_path)
        visual = {
            "status": result.get("status", "FAIL"),
            "ssim": result.get("ssim"),
            "diffPct": result.get("diff_pct"),
            "refSize": result.get("ref_size"),
            "ourSize": result.get("our_size"),
        }
        if os.path.exists(diff_png_path):
            visual["diffPng"] = encode_file_b64(diff_png_path)
        return visual, ""
    except Exception as ex:
        return None, str(ex)[:500]


def json_response(handler: http.server.BaseHTTPRequestHandler, code: int, obj: dict) -> None:
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(body)


class PreviewHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[HTTP] {fmt % args}")

    def do_GET(self):
        # Strip query string for routing
        raw_path = self.path.split("?")[0]

        if raw_path == "/" or raw_path == "/index.html":
            self.send_response(302)
            self.send_header("Location", "/preview.html")
            self.end_headers()
            return

        if raw_path == "/preview.html":
            html_path = STATIC_DIR / "preview.html"
            if not html_path.exists():
                self.send_error(404, "preview.html not found")
                return
            with open(html_path, "rb") as f:
                content = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(content)
            return

        # SCS file serving for HOOPS viewer
        if raw_path.startswith("/api/scs/"):
            filename = unquote(raw_path[len("/api/scs/"):])
            safe_filename = Path(filename).name
            scs_path = SCS_ROOT / safe_filename
            if not scs_path.exists():
                json_response(self, 404, {"ok": False, "error": "scs_not_found", "file": safe_filename})
                return
            data = scs_path.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
            return

        # 静态文件（.json.gz 等）
        # /test_dwg/big.json.gz → PROJECT_ROOT/test_dwg/big.json.gz
        safe_path = raw_path.lstrip("/")
        # If path is absolute (starts with Users/ on macOS), use it directly.
        # Otherwise join with PROJECT_ROOT.
        if safe_path.startswith("Users/") or safe_path.startswith("home/"):
            real_path = Path("/") / safe_path
        else:
            real_path = PROJECT_ROOT / safe_path
        try:
            real_path = real_path.resolve()
            real_path.relative_to(PROJECT_ROOT)
        except ValueError:
            self.send_error(403, "Forbidden")
            return
        if real_path.is_file():
            with open(real_path, "rb") as f:
                content = f.read()
            self.send_response(200)
            if str(real_path).endswith(".gz"):
                self.send_header("Content-Type", "application/octet-stream")
            else:
                self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", len(content))
            self.end_headers()
            self.wfile.write(content)
        else:
            self.send_error(404, f"File not found: {self.path}")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Filename, X-Filename-Encoded")
        self.end_headers()

    def do_POST(self):
        if self.path == "/parse":
            self._handle_parse()
        elif self.path == "/compare-reference":
            self._handle_compare_reference()
        elif self.path == "/compare-render":
            # Backward-compatible route. Interactive preview intentionally does
            # not run Playwright/SSIM here; full visual regression lives in
            # scripts/run_regression.py.
            self._handle_compare_reference()
        else:
            self.send_error(404, "Unknown endpoint")

    def _read_upload(self) -> str:
        """Read POST body into a temp file. Returns temp path."""
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            self.send_error(400, "Empty body")
            return ""
        body = self.rfile.read(length)
        orig_name = self._upload_filename()
        suffix = os.path.splitext(orig_name)[1] or ".dwg"
        with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
            tmp.write(body)
            return tmp.name

    def _upload_filename(self) -> str:
        encoded = self.headers.get("X-Filename-Encoded")
        if encoded:
            return unquote(encoded)
        return self.headers.get("X-Filename", "upload.dwg")

    def _handle_parse(self):
        tmp_path = self._read_upload()
        if not tmp_path:
            return
        try:
            out_data, err = run_render_export(tmp_path)
            if err:
                print(f"[parse] stderr: {err}")
                self.send_error(500, f"Parse failed: {err}")
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", len(out_data))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(out_data)
            print(f"[parse] Done: {len(out_data)/1024:.1f} KB gzipped")
        finally:
            os.unlink(tmp_path)

    def _handle_compare_reference(self):
        tmp_path = self._read_upload()
        if not tmp_path:
            return
        orig_name = self._upload_filename()
        is_dwg = orig_name.lower().endswith(".dwg")
        ref_png_path = tmp_path + ".ref.png"
        ref_dxf_path = tmp_path + ".ref.dxf"

        try:
            fingerprint = file_sha256(tmp_path)
            provider = active_reference_provider()
            cached = load_reference_cache(fingerprint, provider["id"])
            if cached:
                resp_body = json.dumps(cached, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", len(resp_body))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(resp_body)
                print(f"[compare-reference] cache hit {fingerprint[:12]}")
                return

            ref_png_b64, ref_err, ref_time_ms, ref_render_info = run_reference_render(
                tmp_path,
                is_dwg,
                ref_png_path,
                ref_dxf_path,
            )

            ref_for_compare = ref_dxf_path if is_dwg else tmp_path
            entity_compare, entity_err, entity_time_ms = run_entity_comparison(
                tmp_path,
                ref_for_compare if os.path.exists(ref_for_compare) else None,
            )

            reference_meta = {
                "cacheHit": False,
                "provider": provider["id"],
                "providerStrength": provider["strength"],
                "sourceFingerprint": fingerprint,
                "sourceFilename": orig_name,
                "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "parserFramework": {
                    "provider": provider["id"],
                    "dwgConverter": "libredwg dwg2dxf",
                    "dwgConverterPath": str(DWG2DXF),
                    "dwgConverterVersion": tool_version([str(DWG2DXF), "--version"]) if DWG2DXF.exists() else "missing",
                    "entityExtractor": "ezdxf",
                    "renderer": provider["renderer"],
                    "rendererPath": provider.get("rendererPath", ""),
                    "rendererVersion": tool_version([provider["rendererPath"], "-h"]) if provider.get("rendererPath") else "n/a",
                    "outputWidth": ref_render_info.get("outputWidth"),
                    "outputHeight": ref_render_info.get("outputHeight"),
                    "command": ref_render_info.get("command"),
                },
                "renderTimeMs": ref_time_ms,
                "entityCompareTimeMs": entity_time_ms,
                "entityCompare": entity_compare,
                "entityCompareError": entity_err or None,
                "referenceError": ref_err or None,
            }
            if ref_png_b64 and os.path.exists(ref_png_path):
                save_reference_cache(
                    fingerprint,
                    ref_png_path,
                    ref_dxf_path if os.path.exists(ref_dxf_path) else None,
                    reference_meta,
                )

            response = {
                "ours": None,
                "ourError": None,
                "refPng": ref_png_b64,
                "refError": ref_err or None,
                "entityCompare": entity_compare,
                "visualCompare": None,
                "errors": {
                    "ourParser": None,
                    "reference": ref_err or None,
                    "entityCompare": entity_err or None,
                    "visualCompare": None,
                },
                "referenceMeta": reference_meta,
                "refInfo": {
                    "entityCount": (entity_compare or {}).get("refEntityCount", 0),
                    "ourEntityCount": (entity_compare or {}).get("ourEntityCount", 0),
                    "refEntityCount": (entity_compare or {}).get("refEntityCount", 0),
                    "entityTypeCounts": (entity_compare or {}).get("entityTypeCounts", {}),
                    "missing": (entity_compare or {}).get("missing", 0),
                    "extra": (entity_compare or {}).get("extra", 0),
                    "renderTimeMs": ref_time_ms,
                    "ourRenderTimeMs": 0,
                    "entityCompareTimeMs": entity_time_ms,
                },
            }

            if not ref_png_b64 and not entity_compare:
                self.send_error(500, f"Reference failed. Ref: {ref_err}. Entity: {entity_err}")
                return

            resp_body = json.dumps(response, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", len(resp_body))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(resp_body)
            ref_kb = len(ref_png_b64) / 1024 if ref_png_b64 else 0
            print(f"[compare-reference] ref={ref_kb:.1f}KB err={ref_err or 'OK'} entity={entity_err or (entity_compare or {}).get('status', 'OK')}")

        finally:
            os.unlink(tmp_path)
            for path in (ref_png_path, ref_dxf_path):
                if os.path.exists(path):
                    os.unlink(path)


def main():
    if not RENDER_EXPORT.exists():
        print(f"错误: {RENDER_EXPORT} 不存在")
        print(f"  请先编译: cd {PROJECT_ROOT}/build && cmake --build . --target render_export")
        return

    if not STATIC_DIR.exists():
        print(f"错误: {STATIC_DIR} 不存在")
        return

    if not ENTITY_EXPORT.exists():
        print(f"提示: {ENTITY_EXPORT} 不存在，/compare-render 的实体对比会不可用")
        print(f"  可编译: cd {PROJECT_ROOT}/build && cmake --build . --target entity_export")
    if not DWG2DXF.exists():
        print(f"提示: dwg2dxf 不存在: {DWG2DXF}")
        print("  第三方参考预览/对比会不可用；可设置 FT_DWG2DXF 指向测试用 dwg2dxf")

    print(f"=" * 60)
    print(f"  后端服务器启动: http://localhost:{PORT}")
    print(f"  /parse 端点就绪，供 Vite dev server 代理")
    print(f"=" * 60)
    print(f"  启动前端: cd platforms/electron && npm run dev")
    print()
    print()

    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), PreviewHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[server] 退出")


if __name__ == "__main__":
    main()
