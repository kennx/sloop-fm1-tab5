#!/usr/bin/env bash
# params.sh — 参数页注入：GLOBAL/FX/ANALOG/FM6/SAMPLE 五页的旋钮页构建、TAP ± /
# 键盘编辑、UI KNOB 触摸路径、组作用域池水位（ADR-005 §5）、截屏
# 用法: bash scripts/inject/params.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 params 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-params-<时间戳>.log + 八张 PNG
# （global/fx/analog/fm6/fm6-alg/fm6-locked/sample/sample-set）。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario params --port "$PORT"
