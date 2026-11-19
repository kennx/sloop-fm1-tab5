#!/usr/bin/env bash
# flash.sh — 烧录固件到 Tab5（标准 ESP-IDF 流程，AGENTS.md §5）
# 用法: scripts/flash.sh [PORT] [--monitor] [额外 idf.py 参数...]
#   PORT 缺省自动探测（/dev/ttyACM* 优先，其次 /dev/ttyUSB*）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
. "$SCRIPT_DIR/env.sh"

ensure_idf
require_project

PORT=""
MONITOR=0
EXTRA=()
for arg in "$@"; do
    case "$arg" in
        --monitor) MONITOR=1 ;;
        --clean) ;; # flash.sh 忽略 --clean（构建交给 build.sh）
        *) if [ -z "$PORT" ]; then PORT="$arg"; else EXTRA+=("$arg"); fi ;;
    esac
done

if [ -z "$PORT" ]; then
    PORT="$(detect_port || true)"
    [ -n "$PORT" ] || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入端口参数"
fi
echo "==> 使用串口: $PORT"

BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}"

if (( MONITOR )); then
    idf.py -C "$PROJECT_DIR" -B "$BUILD_DIR" -p "$PORT" "${EXTRA[@]+"${EXTRA[@]}"}" flash monitor
else
    idf.py -C "$PROJECT_DIR" -B "$BUILD_DIR" -p "$PORT" "${EXTRA[@]+"${EXTRA[@]}"}" flash
    echo "==> 烧录完成（如需看日志: scripts/flash.sh $PORT --monitor）"
fi
