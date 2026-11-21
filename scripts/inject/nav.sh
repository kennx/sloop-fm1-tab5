#!/usr/bin/env bash
# nav.sh — 导航回归注入：四条导航行为（阶段 4 路由重构最易静默破坏）
#   ① 页签条内、页签矩形外的 TAP → miss，状态/引擎不变
#   ② STEP 鼓轨行 TAP 只读（回显 read-only、焦点不变、无引擎写、鼓格像素不变）
#   ③ 同一 30ms 拍内 KEY tab + KEY PANIC → PANIC 撤销待切页，无回跳
#   ④ 同一 30ms 拍内 3 次 KEY tab → 只前进一页（ADR-006「一拍一页」）
# 用法: bash scripts/inject/nav.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 nav 场景里；本脚本只是入口。
# 同拍语义的注入方法（批写 + 拍相位锚点 + 批内 UI 自证）见 inject.py 的 run_nav 注释。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-nav-<时间戳>.log + PNG。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario nav --port "$PORT"
