#!/usr/bin/env bash
# step_edit.sh — STEP 注入：窗口列编辑（默认窗口两格）+ 分窗 bank1（绝对步号/隐藏列/
# 播放头映射）→ 断言 STATE/ENG 真值 → 截屏；②e 深度步态、②f 鼓 lane 编辑、
#   ②g EDIT 层 a–g 擦除（P4-F Task 7：Aa + 音名键 ⇒ 聚焦步的同名音消失，
#   `ENG STEP` 真值；两音步只摘一根、跨八度、无匹配不变、鼓轨 no-op、冻结不写）
# 用法: bash scripts/inject/step_edit.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 脚本可重复运行：先归一窗口到 {0,0,16}；目标格已亮先经同一 TAP 路径点灭再点亮；
# 结束复位默认窗口与停止态。断言与截屏都在 scripts/inject/inject.py 的 step_edit
# 场景里；本脚本只是入口。
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
