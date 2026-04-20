#!/usr/bin/env python3
"""
DWG/DXF 预览服务器

启动后：
  http://localhost:2415/  → preview.html（加载 ?data=... 参数指定的文件）
  POST /parse             → 接收 DWG/DXF 文件，调用 C++ render_export，返回 JSON

前端预览页选择本地 DWG/DXF 文件 → 自动上传到 /parse → 返回 JSON 并渲染。
"""

import http.server
import json
import os
import shutil
import subprocess
import tempfile
import urllib.parse
from pathlib import Path

PORT = 2415
PROJECT_ROOT = Path(__file__).parent.resolve()
RENDER_EXPORT = PROJECT_ROOT / "build/core/test/render_export"
STATIC_DIR = PROJECT_ROOT / "platforms/electron"

# 默认预览文件
DEFAULT_DATA = PROJECT_ROOT / "test_dwg/big.json.gz"


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
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Filename")
        self.end_headers()

    def do_POST(self):
        if self.path != "/parse":
            self.send_error(404, "Unknown endpoint")
            return

        # 读取 Content-Length
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            self.send_error(400, "Empty body")
            return

        # 将请求体写入临时文件（使用 X-Filename header 确定后缀）
        body = self.rfile.read(length)
        orig_name = self.headers.get("X-Filename", "upload.dwg")
        suffix = os.path.splitext(orig_name)[1] or ".dwg"
        with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
            tmp.write(body)
            tmp_path = tmp.name

        try:
            out_path = tmp_path + ".json.gz"

            # 调用 C++ parser
            cmd = [str(RENDER_EXPORT), tmp_path, out_path]
            print(f"[parse] Running: {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=300,
            )
            if result.returncode != 0:
                print(f"[parse] stderr: {result.stderr}")
                self.send_error(500, f"Parse failed: {result.stderr[:500]}")
                return

            # 读取输出
            if not os.path.exists(out_path):
                self.send_error(500, "No output generated")
                return

            with open(out_path, "rb") as f:
                out_data = f.read()

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
            if os.path.exists(out_path):
                os.unlink(out_path)


def main():
    if not RENDER_EXPORT.exists():
        print(f"错误: {RENDER_EXPORT} 不存在")
        print(f"  请先编译: cd {PROJECT_ROOT}/build && cmake --build . --target render_export")
        return

    if not STATIC_DIR.exists():
        print(f"错误: {STATIC_DIR} 不存在")
        return

    # 生成默认预览文件（如果不存在）
    if not DEFAULT_DATA.exists():
        print(f"[init] 生成默认预览: {DEFAULT_DATA}")
        subprocess.run(
            [str(RENDER_EXPORT),
             str(PROJECT_ROOT / "test_dwg/big.dwg"),
             str(DEFAULT_DATA)],
            check=True,
            capture_output=True,
        )

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
