#!/usr/bin/env bash
# tracks.sh — TRACKS 注入：点卡 2（选轨）→ 断言 selected/focus_track → 截屏
# 用法: bash scripts/inject/tracks.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 tracks 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-tracks-<时间戳>.log + PNG。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario tracks --port "$PORT"
