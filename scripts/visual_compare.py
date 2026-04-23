#!/usr/bin/env python3
from __future__ import annotations

"""visual_compare.py — Pixel-level visual comparison for CAD rendering.

Usage:
  # Render reference PNG from DXF via ezdxf
  python3 visual_compare.py --render-ref-dxf <input.dxf> -o <ref.png>

  # Convert PDF reference to PNG
  python3 visual_compare.py --pdf-to-png <input.pdf> -o <out.png>

  # Capture our Canvas rendering via Playwright
  python3 visual_compare.py --capture-canvas <render.json.gz> -o <out.png>

  # Compare two PNGs
  python3 visual_compare.py --compare <ref.png> <ours.png> [-o diff.png]
"""

import argparse
import asyncio
import functools
import http.server
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TMP_WWW = Path("/tmp/cad_compare_www")
DEFAULT_APP_URL = os.environ.get("FT_PREVIEW_APP_URL", "http://localhost:5173")


def render_dxf_to_png(dxf_path: str, output_png: str, width: int = 1920, height: int = 1080):
    """Render DXF to PNG using ezdxf + matplotlib on the same dark background as preview."""
    import ezdxf
    from ezdxf.addons.drawing import Frontend, RenderContext
    from ezdxf.addons.drawing.matplotlib import MatplotlibBackend
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    doc = ezdxf.readfile(dxf_path)
    msp = doc.modelspace()

    for insert in msp.query("INSERT"):
        try:
            block_name = insert.dxf.name
            if block_name not in doc.blocks:
                doc.blocks.new(block_name)
        except Exception:
            pass

    fig, ax = plt.subplots(figsize=(width / 100, height / 100), dpi=100)
    fig.patch.set_facecolor("#1e1e2e")
    ax.set_facecolor("#1e1e2e")
    ctx = RenderContext(doc)
    backend = MatplotlibBackend(ax)
    try:
        Frontend(ctx, backend).draw_layout(msp)
        fig.savefig(output_png, dpi=100, facecolor="#1e1e2e", bbox_inches="tight")
        plt.close(fig)
        print(f"Rendered {dxf_path} -> {output_png}")
    except Exception as ex:
        plt.close(fig)
        print(f"ezdxf frontend failed, using safe fallback: {ex}")
        render_dxf_to_png_fallback(dxf_path, output_png, width, height)


def render_dxf_to_png_fallback(dxf_path: str, output_png: str, width: int = 1920, height: int = 1080):
    """Best-effort reference PNG renderer that skips problematic DXF styles/entities."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle as MplCircle, Arc as MplArc
    from compare_entities import extract_ezdxf_entities

    entities = extract_ezdxf_entities(dxf_path)
    fig, ax = plt.subplots(figsize=(width / 100, height / 100), dpi=100)
    fig.patch.set_facecolor("#1e1e2e")
    ax.set_facecolor("#1e1e2e")
    ax.set_aspect("equal", adjustable="box")
    ax.axis("off")
    xs: list[float] = []
    ys: list[float] = []

    def add_pt(x, y):
        if isinstance(x, (int, float)) and isinstance(y, (int, float)) and abs(x) < 1e9 and abs(y) < 1e9:
            xs.append(float(x)); ys.append(float(y))

    for ent in entities:
        try:
            t = ent.get("type")
            if t == "LINE":
                x0, y0 = ent["start"][0], ent["start"][1]
                x1, y1 = ent["end"][0], ent["end"][1]
                ax.plot([x0, x1], [y0, y1], color="#d8d8d8", linewidth=0.4)
                add_pt(x0, y0); add_pt(x1, y1)
            elif t in ("LWPOLYLINE", "POLYLINE"):
                pts = ent.get("vertices") or []
                if len(pts) >= 2:
                    px = [p[0] for p in pts]
                    py = [p[1] for p in pts]
                    if ent.get("closed"):
                        px.append(px[0]); py.append(py[0])
                    ax.plot(px, py, color="#d8d8d8", linewidth=0.4)
                    for x, y in zip(px, py): add_pt(x, y)
            elif t == "CIRCLE":
                cx, cy = ent["center"][0], ent["center"][1]
                r = ent.get("radius", 0)
                ax.add_patch(MplCircle((cx, cy), r, fill=False, edgecolor="#d8d8d8", linewidth=0.4))
                add_pt(cx - r, cy - r); add_pt(cx + r, cy + r)
            elif t == "ARC":
                cx, cy = ent["center"][0], ent["center"][1]
                r = ent.get("radius", 0)
                ax.add_patch(MplArc((cx, cy), 2 * r, 2 * r,
                                    theta1=ent.get("start_angle", 0),
                                    theta2=ent.get("end_angle", 360),
                                    edgecolor="#d8d8d8", linewidth=0.4))
                add_pt(cx - r, cy - r); add_pt(cx + r, cy + r)
            elif t in ("TEXT", "MTEXT"):
                txt = str(ent.get("text", ""))[:80]
                if txt:
                    x, y = ent.get("x", 0), ent.get("y", 0)
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
    print(f"Fallback rendered {dxf_path} -> {output_png}")


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _url_ok(url: str) -> bool:
    try:
        with urllib.request.urlopen(url, timeout=1) as res:
            return 200 <= res.status < 500
    except Exception:
        return False


def _wait_for_url(url: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _url_ok(url):
            return True
        time.sleep(0.25)
    return False


class _CorsFileHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


def _start_cors_file_server(directory: Path) -> tuple[http.server.ThreadingHTTPServer, str]:
    port = _find_free_port()
    handler = functools.partial(_CorsFileHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, f"http://127.0.0.1:{port}"


def _ensure_vite_server(app_url: str) -> subprocess.Popen | None:
    if _wait_for_url(app_url, 1.0):
        return None

    print(f"Starting Vite preview app at {app_url}...")
    proc = subprocess.Popen(
        ["npm", "run", "dev:frontend", "--", "--host", "127.0.0.1"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if not _wait_for_url(app_url, 30.0):
        output = ""
        try:
            output = proc.stdout.read(2000) if proc.stdout else ""
        except Exception:
            pass
        proc.terminate()
        raise RuntimeError(f"Vite dev server did not start at {app_url}. {output}")
    return proc


def pdf_to_png(pdf_path: str, output_png: str, dpi: int = 150):
    """Convert first page of PDF to PNG via pdftoppm."""
    out_prefix = str(Path(output_png).with_suffix(""))
    subprocess.run(
        ["pdftoppm", "-png", "-r", str(dpi), "-singlefile", "-f", "1", "-l", "1", pdf_path, out_prefix],
        check=True,
    )
    # pdftoppm adds .png suffix automatically
    actual = Path(out_prefix + ".png")
    if actual.exists() and str(actual) != output_png:
        actual.rename(output_png)
    print(f"Converted {pdf_path} -> {output_png}")


async def capture_canvas(
    json_path: str,
    output_png: str,
    width: int = 1920,
    height: int = 1080,
    app_url: str = DEFAULT_APP_URL,
):
    """Capture our Canvas rendering via Playwright headless browser."""
    from playwright.async_api import async_playwright

    app_proc = None
    file_server = None
    try:
        app_proc = _ensure_vite_server(app_url)

        TMP_WWW.mkdir(parents=True, exist_ok=True)
        json_name = Path(json_path).name
        served_json = TMP_WWW / json_name
        shutil.copy2(json_path, served_json)
        file_server, file_base_url = _start_cors_file_server(TMP_WWW)
        data_url = f"{file_base_url}/{urllib.parse.quote(json_name)}"
        page_url = f"{app_url.rstrip('/')}/?data={urllib.parse.quote(data_url, safe='')}"

        async with async_playwright() as p:
            browser = await p.chromium.launch(headless=True)
            page = await browser.new_page(viewport={"width": width, "height": height})
            await page.goto(page_url, timeout=60000)

            try:
                await page.wait_for_function(
                    "() => { const c = document.querySelector('canvas'); return c && c.width > 0 && window.drawData && window.drawData.batches; }",
                    timeout=30000,
                )
                await page.wait_for_timeout(800)
            except Exception:
                await page.wait_for_timeout(3000)

            canvas = await page.query_selector("canvas")
            if canvas:
                await canvas.screenshot(path=output_png)
                print(f"Captured canvas -> {output_png}")
            else:
                raise RuntimeError("No canvas element found")

            await browser.close()
    finally:
        if file_server is not None:
            file_server.shutdown()
            file_server.server_close()
        if app_proc is not None:
            app_proc.terminate()
            try:
                app_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app_proc.kill()


def compare_images(ref_png: str, our_png: str, output_diff: str = None) -> dict:
    """Compare two images using SSIM."""
    from PIL import Image
    import numpy as np

    ref = Image.open(ref_png).convert("L")
    ours = Image.open(our_png).convert("L")

    # Resize ours to match ref
    if ref.size != ours.size:
        ours = ours.resize(ref.size, Image.LANCZOS)

    ref_arr = np.array(ref, dtype=np.float64)
    our_arr = np.array(ours, dtype=np.float64)

    # Simple SSIM computation
    C1 = (0.01 * 255) ** 2
    C2 = (0.03 * 255) ** 2
    mu1 = ref_arr.mean()
    mu2 = our_arr.mean()
    sigma1_sq = ref_arr.var()
    sigma2_sq = our_arr.var()
    sigma12 = ((ref_arr - mu1) * (our_arr - mu2)).mean()
    ssim = ((2 * mu1 * mu2 + C1) * (2 * sigma12 + C2)) / ((mu1**2 + mu2**2 + C1) * (sigma1_sq + sigma2_sq + C2))

    # Pixel difference
    diff = np.abs(ref_arr - our_arr)
    diff_pct = (diff > 10).sum() / diff.size * 100

    # Generate diff heatmap
    if output_diff:
        diff_color = np.zeros((*diff.shape, 3), dtype=np.uint8)
        diff_color[:, :, 0] = np.clip(diff * 2, 0, 255).astype(np.uint8)  # Red for differences
        diff_color[:, :, 1] = np.clip(255 - diff * 2, 0, 255).astype(np.uint8)  # Green for matches
        Image.fromarray(diff_color).save(output_diff)
        print(f"Diff heatmap -> {output_diff}")

    status = "PASS" if ssim >= 0.85 else "WARN" if ssim >= 0.70 else "FAIL"
    result = {"ssim": float(ssim), "diff_pct": float(diff_pct), "status": status, "ref_size": ref.size, "our_size": ours.size}

    print(f"SSIM: {ssim:.4f}  Diff>10: {diff_pct:.1f}%  Status: {status}")
    return result


def main():
    parser = argparse.ArgumentParser(description="CAD visual comparison tool")
    parser.add_argument("--render-ref-dxf", metavar="DXF", help="Render DXF reference PNG via ezdxf")
    parser.add_argument("--pdf-to-png", metavar="PDF", help="Convert PDF to PNG")
    parser.add_argument("--capture-canvas", metavar="JSON", help="Capture our Canvas rendering via Playwright")
    parser.add_argument("--compare", nargs=2, metavar=("REF", "OURS"), help="Compare two PNG images")
    parser.add_argument("--app-url", default=DEFAULT_APP_URL, help="React preview app URL for --capture-canvas")
    parser.add_argument("--width", type=int, default=1920, help="Render/capture width")
    parser.add_argument("--height", type=int, default=1080, help="Render/capture height")
    parser.add_argument("-o", "--output", help="Output PNG path")
    args = parser.parse_args()

    if args.render_ref_dxf:
        render_dxf_to_png(args.render_ref_dxf, args.output or "ref.png", args.width, args.height)
    elif args.pdf_to_png:
        pdf_to_png(args.pdf_to_png, args.output or "ref.png")
    elif args.capture_canvas:
        asyncio.run(capture_canvas(args.capture_canvas, args.output or "ours.png", args.width, args.height, args.app_url))
    elif args.compare:
        compare_images(args.compare[0], args.compare[1], args.output)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
