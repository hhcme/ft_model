#!/usr/bin/env python3
from __future__ import annotations

"""
DWG/DXF 预览服务器

启动后：
  http://localhost:2415/  → preview.html（加载 ?data=... 参数指定的文件）
  POST /parse             → 接收 DWG/DXF 文件，调用 C++ render_export，返回 JSON

前端预览页选择本地 DWG/DXF 文件 → 自动上传到 /parse → 返回 JSON 并渲染。
HOOPS SCS 转换使用官方 hoops_scs_converter（自动回退读取模式）。
"""

import gzip
import http.server
import json
import os
import sys
import subprocess
import tempfile
import threading
import time
from pathlib import Path
import shutil
from urllib.parse import unquote

PORT = 2415
PROJECT_ROOT = Path(__file__).parent.resolve()
RENDER_EXPORT = PROJECT_ROOT / "build/core/test/render_export"
ENTITY_EXPORT = PROJECT_ROOT / "build/core/test/entity_export"
# Windows MSVC puts Release/ subdirectory and .exe suffix
if sys.platform == "win32":
    _release = PROJECT_ROOT / "build/core/test/Release/render_export.exe"
    if _release.exists():
        RENDER_EXPORT = _release
    _release = PROJECT_ROOT / "build/core/test/Release/entity_export.exe"
    if _release.exists():
        ENTITY_EXPORT = _release
STATIC_DIR = PROJECT_ROOT / "platforms/electron"
SCRIPTS_DIR = PROJECT_ROOT / "scripts"
SCS_ROOT = PROJECT_ROOT / "scs_dwg"
DWG_CACHE_ROOT = PROJECT_ROOT / "dwg_cache"
DWG_CACHE_ROOT.mkdir(parents=True, exist_ok=True)

# Simple (custom) DWG→SCS converter
DWG_TO_SCS_TOOL = PROJECT_ROOT / "build/tools/dwg_to_scs/Release/dwg_to_scs.exe"
if not DWG_TO_SCS_TOOL.exists():
    _alt = PROJECT_ROOT / "build/tools/dwg_to_scs/dwg_to_scs.exe"
    if _alt.exists():
        DWG_TO_SCS_TOOL = _alt

# Official HOOPS SCS converter (preferred — auto-fallback read modes for DWG)
HOOPS_SCS_CONVERTER = Path(os.environ.get(
    "FT_HOOPS_SCS_CONVERTER",
    r"D:\findtop\code\hoops_high_performance\tools\scs_converter\bin\hoops_scs_converter.exe",
))
# Active converter: "official" (default) or "simple"
SCS_CONVERTER_TOOL = os.environ.get("FT_SCS_CONVERTER_TOOL", "official")

# Default HOOPS SDK root if not set in environment
if not os.environ.get("HOOPS_EXCHANGE_ROOT"):
    _default_sdk = (
        r"D:\findtop\code\hoops_high_performance\sdk\extracted"
        r"\HOOPS_Exchange_2026.2.0_Windows_x86-64_v142"
        r"\HOOPS_Exchange_2026.2.0"
    )
    if Path(_default_sdk).exists():
        os.environ["HOOPS_EXCHANGE_ROOT"] = _default_sdk

# Thread-safe conversion status tracker
_convert_lock = threading.Lock()
_convert_status: dict[str, dict] = {}  # {safe_stem: {status, error, converter}}
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))


def run_render_export(input_path: str, manifest_only: bool = False) -> tuple[bytes, str]:
    """Run our C++ render_export. Returns (gzip_json_bytes, error_msg)."""
    out_path = input_path + ".json.gz"
    cmd = [str(RENDER_EXPORT), input_path, out_path]
    if manifest_only:
        cmd.append("--manifest")
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


def _read_hoops_license() -> str:
    """Read HOOPS license from environment or hoops_license.h."""
    env = os.environ.get("HOOPS_LICENSE", "")
    if env:
        return env
    lic_file = PROJECT_ROOT / "tools/dwg_to_scs/hoops_license.h"
    if lic_file.exists():
        for line in lic_file.read_text(encoding="utf-8").splitlines():
            if '#define HOOPS_LICENSE' in line:
                start = line.find('"')
                end = line.rfind('"')
                if start >= 0 and end > start:
                    return line[start + 1:end]
    return ""


def json_response(handler: http.server.BaseHTTPRequestHandler, code: int, obj: dict) -> None:
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(body)


# ---------------------------------------------------------------------------
# SCS Conversion: official converter (default) and simple converter (fallback)
# ---------------------------------------------------------------------------

def _run_simple_scs_converter(dwg_path: Path, scs_path: Path, safe_stem: str, env: dict):
    """Run the simple (custom) dwg_to_scs converter."""
    cmd = [str(DWG_TO_SCS_TOOL), str(dwg_path), str(scs_path)]
    print(f"[convert] Starting (simple): {safe_stem}")
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=300)
        if r.returncode == 0 and scs_path.exists():
            with _convert_lock:
                _convert_status[safe_stem] = {"status": "done", "error": None, "converter": "simple"}
            print(f"[convert] OK (simple): {safe_stem} ({scs_path.stat().st_size / 1024:.0f} KB)")
        else:
            err = (r.stderr or r.stdout or "unknown error")[-500:]
            with _convert_lock:
                _convert_status[safe_stem] = {"status": "error", "error": f"[simple] {err}"}
            print(f"[convert] FAIL (simple): {safe_stem}: {err[:200]}")
    except Exception as ex:
        with _convert_lock:
            _convert_status[safe_stem] = {"status": "error", "error": str(ex)[:500]}
        print(f"[convert] ERROR (simple): {safe_stem}: {ex}")


def _run_official_scs_converter(dwg_path: Path, scs_path: Path, safe_stem: str, env: dict):
    """Run the official hoops_scs_converter with auto-fallback load modes for DWG."""
    sdk_bin = Path(env["HOOPS_EXCHANGE_ROOT"]) / "bin" / "win64_v142"
    license_key = _read_hoops_license()
    cmd = [
        str(HOOPS_SCS_CONVERTER),
        "--input", str(dwg_path),
        "--output", str(scs_path),
        "--bin-dir", str(sdk_bin),
        "--license", license_key,
        "--source-kind", "dwg",
        "--load-mode", "auto",
        "--precision-level", "3",
    ]
    print(f"[convert] Starting (official): {safe_stem}")
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
        if r.returncode == 0 and scs_path.exists():
            with _convert_lock:
                _convert_status[safe_stem] = {"status": "done", "error": None, "converter": "official"}
            print(f"[convert] OK (official): {safe_stem} ({scs_path.stat().st_size / 1024:.0f} KB)")
        else:
            err = (r.stderr or r.stdout or "unknown error")[-500:]
            with _convert_lock:
                _convert_status[safe_stem] = {"status": "error", "error": f"[official] {err}"}
            print(f"[convert] FAIL (official): {safe_stem}: {err[:200]}")
    except Exception as ex:
        with _convert_lock:
            _convert_status[safe_stem] = {"status": "error", "error": str(ex)[:500]}
        print(f"[convert] ERROR (official): {safe_stem}: {ex}")


def _run_dwg_to_scs(dwg_path: Path, scs_path: Path, safe_stem: str, use_official: bool | None = None):
    """Run SCS conversion in background thread, dispatch to official or simple converter."""
    env = os.environ.copy()
    if not env.get("HOOPS_EXCHANGE_ROOT"):
        with _convert_lock:
            _convert_status[safe_stem] = {"status": "error", "error": "HOOPS_EXCHANGE_ROOT not set"}
        return

    official = use_official if use_official is not None else (SCS_CONVERTER_TOOL == "official")

    if official and HOOPS_SCS_CONVERTER.exists():
        _run_official_scs_converter(dwg_path, scs_path, safe_stem, env)
    elif DWG_TO_SCS_TOOL.exists():
        if official:
            print(f"[convert] Official converter not found, falling back to simple: {HOOPS_SCS_CONVERTER}")
        _run_simple_scs_converter(dwg_path, scs_path, safe_stem, env)
    else:
        with _convert_lock:
            _convert_status[safe_stem] = {
                "status": "error",
                "error": f"No converter found. Official: {HOOPS_SCS_CONVERTER} (exists={HOOPS_SCS_CONVERTER.exists()}), Simple: {DWG_TO_SCS_TOOL} (exists={DWG_TO_SCS_TOOL.exists()})",
            }


# ---------------------------------------------------------------------------
# HTTP Handler
# ---------------------------------------------------------------------------

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

        # Converter config
        if raw_path == "/api/scs/converter-config":
            self._handle_converter_config()
            return

        # SCS conversion status polling
        if raw_path.startswith("/api/scs/convert-status"):
            self._handle_convert_status()
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
        safe_path = raw_path.lstrip("/")
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

    def _handle_convert_scs(self):
        """POST /api/scs/convert — trigger DWG→SCS conversion."""
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            req = json.loads(body)
        except Exception:
            json_response(self, 400, {"ok": False, "error": "invalid JSON"})
            return

        filename = req.get("filename", "")
        force = req.get("force", False)
        converter = req.get("converter", None)  # "official" | "simple" | None
        safe_name = Path(filename).name
        stem = Path(safe_name).stem
        scs_name = stem + ".scs"
        scs_path = SCS_ROOT / scs_name

        # Force re-convert: delete existing SCS
        if force and scs_path.exists():
            scs_path.unlink()
            print(f"[convert] Deleted existing SCS for reparse: {scs_name}")

        # Already converted (and not forcing)?
        if scs_path.exists() and not force:
            with _convert_lock:
                st = _convert_status.get(stem)
                converter_used = st.get("converter") if st else None
            json_response(self, 200, {"ok": True, "status": "done", "scsFile": scs_name, "converter": converter_used})
            return

        # Already converting?
        with _convert_lock:
            if stem in _convert_status and _convert_status[stem]["status"] == "converting":
                json_response(self, 200, {"ok": True, "status": "converting", "scsFile": scs_name})
                return
            _convert_status[stem] = {"status": "converting", "error": None}

        # Find DWG source
        dwg_path = None
        for candidate in [DWG_CACHE_ROOT / safe_name, PROJECT_ROOT / "test_dwg" / safe_name]:
            if candidate.exists():
                dwg_path = candidate
                break

        if not dwg_path:
            with _convert_lock:
                _convert_status[stem] = {"status": "error", "error": f"DWG not found: {safe_name}"}
            json_response(self, 404, {"ok": False, "error": "dwg_not_found", "scsFile": scs_name})
            return

        # Determine which converter to use
        use_official = None if converter is None else (converter == "official")

        # Start conversion in background thread
        t = threading.Thread(
            target=_run_dwg_to_scs,
            args=(dwg_path, scs_path, stem),
            kwargs={"use_official": use_official},
            daemon=True,
        )
        t.start()
        json_response(self, 200, {"ok": True, "status": "converting", "scsFile": scs_name})

    def _handle_convert_status(self):
        """GET /api/scs/convert-status?scsFile=<name>"""
        from urllib.parse import parse_qs
        qs = parse_qs(self.path.split("?", 1)[1] if "?" in self.path else "")
        scs_file = (qs.get("scsFile") or [""])[0]
        if not scs_file:
            json_response(self, 400, {"ok": False, "error": "missing scsFile"})
            return

        # Check disk first (source of truth)
        scs_path = SCS_ROOT / Path(scs_file).name
        if scs_path.exists():
            json_response(self, 200, {"ok": True, "status": "done", "scsFile": scs_file})
            return

        # Check in-memory status
        stem = Path(scs_file).stem
        with _convert_lock:
            st = _convert_status.get(stem)
        if st:
            json_response(self, 200, {"ok": True, "status": st["status"], "scsFile": scs_file, "error": st.get("error"), "converter": st.get("converter")})
        else:
            json_response(self, 200, {"ok": False, "status": "unknown", "scsFile": scs_file})

    def _handle_converter_config(self):
        """GET/POST /api/scs/converter-config — read or set the active SCS converter."""
        global SCS_CONVERTER_TOOL
        if self.command == "GET":
            json_response(self, 200, {
                "ok": True,
                "converter": SCS_CONVERTER_TOOL,
                "officialAvailable": HOOPS_SCS_CONVERTER.exists(),
                "simpleAvailable": DWG_TO_SCS_TOOL.exists(),
            })
            return
        # POST — toggle
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length > 0 else b"{}"
        try:
            req = json.loads(body)
        except Exception:
            json_response(self, 400, {"ok": False, "error": "invalid JSON"})
            return
        choice = req.get("converter", "official")
        if choice not in ("official", "simple"):
            json_response(self, 400, {"ok": False, "error": "invalid converter choice"})
            return
        SCS_CONVERTER_TOOL = choice
        print(f"[config] SCS converter set to: {choice}")
        json_response(self, 200, {"ok": True, "converter": SCS_CONVERTER_TOOL})

    def do_HEAD(self):
        """Handle HEAD requests — same routing as GET, body omitted."""
        raw_path = self.path.split("?")[0]

        # SCS file check (primary consumer: HOOPS viewer frontend useScsFile)
        if raw_path.startswith("/api/scs/"):
            filename = unquote(raw_path[len("/api/scs/"):])
            safe_filename = Path(filename).name
            scs_path = SCS_ROOT / safe_filename
            self.send_response(200 if scs_path.exists() else 404)
            self.send_header("Content-Type", "application/octet-stream")
            if scs_path.exists():
                self.send_header("Content-Length", str(scs_path.stat().st_size))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            return

        # Parse result check (static .json.gz files)
        if raw_path.startswith("/test_dwg/") or raw_path.startswith("/test_data/"):
            safe_path = raw_path.lstrip("/")
            real_path = PROJECT_ROOT / safe_path
            try:
                real_path = real_path.resolve()
                real_path.relative_to(PROJECT_ROOT)
                if real_path.exists() and real_path.is_file():
                    self.send_response(200)
                    self.send_header("Content-Length", str(real_path.stat().st_size))
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    return
            except (ValueError, OSError):
                pass

        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Filename, X-Filename-Encoded")
        self.end_headers()

    def do_POST(self):
        raw = self.path.split("?")[0]
        if raw == "/parse":
            self._handle_parse()
        elif raw == "/api/scs/convert":
            self._handle_convert_scs()
        elif raw == "/api/scs/converter-config":
            self._handle_converter_config()
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
        # Cache DWG uploads for potential SCS conversion
        orig_name = self._upload_filename()
        if orig_name.lower().endswith(".dwg"):
            try:
                cache_path = DWG_CACHE_ROOT / Path(orig_name).name
                shutil.copy2(tmp_path, cache_path)
            except OSError:
                pass
        try:
            manifest_only = "manifest=1" in self.path or "manifest=true" in self.path
            out_data, err = run_render_export(tmp_path, manifest_only=manifest_only)
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


def main():
    if not RENDER_EXPORT.exists():
        print(f"错误: {RENDER_EXPORT} 不存在")
        print(f"  请先编译: cd {PROJECT_ROOT}/build && cmake --build . --target render_export")
        return

    if not STATIC_DIR.exists():
        print(f"错误: {STATIC_DIR} 不存在")
        return

    if HOOPS_SCS_CONVERTER.exists():
        print(f"  HOOPS official converter: {HOOPS_SCS_CONVERTER}")
    else:
        print(f"  提示: official converter 不存在: {HOOPS_SCS_CONVERTER}")

    if DWG_TO_SCS_TOOL.exists():
        print(f"  Simple converter (fallback): {DWG_TO_SCS_TOOL}")
    else:
        print(f"  提示: simple converter 也不存在: {DWG_TO_SCS_TOOL}")

    print(f"  活动转换器: {SCS_CONVERTER_TOOL}")
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
