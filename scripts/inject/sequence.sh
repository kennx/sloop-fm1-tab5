#!/usr/bin/env bash
# sequence.sh — sequence 端到端设备矩阵（P4-C Task 12，第八条场景）：深度步/锁/工具 →
# REC/free take/16 lane 鼓 → live sections/dirty 落盘 → quick chain → SONG A2+B3/loop 恢复 →
# SONG REC → STORE TEST/单例 SELFTEST 后歌曲保持 → esptool 硬复位后 PER3 恢复。
# 用法: bash scripts/inject/sequence.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 run_sequence 里；本脚本只是入口。
# 判别性反例：第 25 锁 full、非法锁不变、鼓轨 TRANSPOSE 拒绝、9 项 chain 拒绝、空 scene
# 的 LOAD/REQUEST/CHAIN/JUMP 拒绝、空 SONG REC 不写配置、REC IS ON 时 MODE 被拒、
# 物理 PLAY 丢弃 free take vs API PLAY 收尾。
# 本场景**破坏性**：占用工程槽 A–C（section）与 D（诊断）、歌曲顺序与四条轨的图案；
# 自带归一（⓪/⑨），可重复运行。产物：
# $SLOOP_TAB5_PROJECT/build/verify/inject-sequence-<时间戳>.log + 三张 PNG。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario sequence --port "$PORT"
