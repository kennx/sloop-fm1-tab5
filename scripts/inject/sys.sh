#!/usr/bin/env bash
# sys.sh — 设置页（P4-E）端到端矩阵：settings 生效副本 / 延迟写 / 自检后重载 / SYSTEM 页
#   ① 归一（STORE AUTO OFF、停走带、ENG AUDIO ON）→ `STORE SETTINGS INFO` 解析 →
#      `SET palette 2` ⇒ `dirty=1` → `SAVE` ⇒ `STORE SETTINGS SAVED len=<n> side=<0|1>`、
#      `dirty=0` → esptool 硬复位 → INFO 仍 `palette=2` → 收口（palette 回起点值、AUTO ON）
#   （后续任务在同一条 run_sys 里追加状态层命令 / SYSTEM 页 / RAW settings 注入等段落）
# 用法: bash scripts/inject/sys.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言都在 scripts/inject/inject.py 的 sys 场景里；本脚本只是入口。
# **破坏性**（改写 OBJ_SETTINGS 的 palette），自带归一、可重复。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-sys-<时间戳>.log
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario sys --port "$PORT"
