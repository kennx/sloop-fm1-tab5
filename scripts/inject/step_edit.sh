#!/usr/bin/env bash
# step_edit.sh — STEP 注入：点两格（轨 0 步 2 / 轨 1 步 5）→ 断言编辑器真值与焦点 → 截屏
# 用法: bash scripts/inject/step_edit.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 脚本可重复运行：若目标格已亮，先经同一 TAP 路径点灭再点亮。
# 断言与截屏都在 scripts/inject/inject.py 的 step_edit 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-step_edit-<时间戳>.log + PNG。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario step_edit --port "$PORT"
