#!/usr/bin/env bash
# transport.sh — TRANSPORT 注入：走带（PLAY/STOP/BPM±）+ ⑩ 工具区（REC 五态、
# fill held/next、聚焦步 LVL/RAT/MIC/SPAN/SLIDE、pattern ROT/LEN/TRANS/CLEAR/UNDO/REDO）、
# 跨组过渡峰值（SEQ 全建 → PRESETS），截屏 TRANSPORT/工具态与播放中的 STEP。
# **破坏性**：⑩g pattern 工具占用诊断槽 3——工作工程先 SAVE 3 快照，结束 LOAD 并以
#   STORE LIVEHASH 逐位还原；槽 3 入口非空则原内容被快照覆盖（之后留着快照），入口为空则
#   收口擦回空。快照基线跨截屏复位重跑保持（proj_snapshot_open：重跑不再 SAVE，槽里不是
#   快照即终止）。
# 用法: bash scripts/inject/transport.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 transport 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-transport-<时间戳>.log + 两张 PNG。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario transport --port "$PORT"
