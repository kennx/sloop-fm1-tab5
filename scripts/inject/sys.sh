#!/usr/bin/env bash
# sys.sh — 设置页（P4-E）端到端矩阵：settings 生效副本 / 延迟写 / 自检后重载 / SYSTEM 页
#   ① 归一（STORE AUTO OFF、停走带、ENG AUDIO ON）→ `STORE SETTINGS INFO` 解析 →
#      `SET palette 2` ⇒ `dirty=1` → `SAVE` ⇒ `STORE SETTINGS SAVED len=<n> side=<0|1>`、
#      `dirty=0` → esptool 硬复位 → INFO 仍 `palette=2` → 收口（palette 回起点值、AUTO ON）
#   ①b RAW settings 往返；② SYSTEM/SYS 页与 8 组签；③ TAP/键盘/走带中 NVS；
#   ④ 硬复位持久化与 REC 三项；⑤ 182 例 golden（后立即复位取干净基线）；⑥ SEQ↔SYS 池峰值；⑦ 收口；
#   ⑧ SELFTEST chip（STOP FIRST → worker/恢复 → RESET FIRST）；
#   ⑨ RAW settings + SIDE theme（COLOR/像素/硬复位/STORE TEST）
# 用法: bash scripts/inject/sys.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言都在 scripts/inject/inject.py 的 sys 场景里；本脚本只是入口。
# **破坏性**（改写 OBJ_SETTINGS 的 palette），自带归一、可重复。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-sys-<时间戳>.{log,png}
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario sys --port "$PORT"
