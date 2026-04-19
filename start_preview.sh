#!/bin/bash
# start_preview.sh — 启动 DWG/DXF 预览服务
#
# 用法：
#   ./start_preview.sh                       # 默认预览 big.dwg
#   ./start_preview.sh test_data/big.dxf     # 指定其他文件

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
DWG_FILE="${1:-$PROJECT_ROOT/test_dwg/big.dwg}"
OUTPUT_JSON="$PROJECT_ROOT/test_dwg/big.json.gz"

# 生成预览文件
if [ ! -f "$DWG_FILE" ]; then
    echo "文件不存在: $DWG_FILE"
    exit 1
fi

echo "[1/2] 生成预览数据: $DWG_FILE → $OUTPUT_JSON"
"$PROJECT_ROOT/build/core/test/render_export" "$DWG_FILE" "$OUTPUT_JSON"
if [ $? -ne 0 ]; then
    echo "解析失败"
    exit 1
fi

# 启动 HTTP 服务器
echo "[2/2] 启动 HTTP 服务..."
cd "$PROJECT_ROOT" || exit 1
open "http://localhost:8088/platforms/electron/preview.html?data=$(python3 -c "import urllib.parse; print(urllib.parse.quote('$OUTPUT_JSON'))")"
python3 -m http.server 8088
