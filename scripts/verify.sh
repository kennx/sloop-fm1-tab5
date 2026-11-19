#!/usr/bin/env bash
# verify.sh — 一键验证：构建 → 烧录 → 等待启动 → 截屏（AGENTS.md §5.4/§5.5 门禁入口）
# 用法: scripts/verify.sh [PORT]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
. "$SCRIPT_DIR/env.sh"

ensure_idf
require_project

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port || true)"
    [ -n "$PORT" ] || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入端口参数"
fi
echo "==> 使用串口: $PORT"

"$SCRIPT_DIR/build.sh"
"$SCRIPT_DIR/flash.sh" "$PORT"

echo "==> 等待设备启动 (4s)..."
sleep 4

OUT_DIR="$PROJECT_DIR/build/verify"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/screenshot-$(date +%Y%m%d-%H%M%S).png"

python3 "$SCRIPT_DIR/screenshot.py" "$PORT" "$OUT"

echo
echo "=========================================="
echo "截屏已保存: $OUT"
echo "下一步（UI 验证门禁, AGENTS.md §5.4）: 用视觉能力读取该 PNG，"
echo "检查花屏/撕裂/错位/缺色，并核对布局与预期状态一致。"
echo "=========================================="
