#!/usr/bin/env bash
# sound.sh — SOUND 卡片仪表盘注入（5B-1）：`UI MEM` 池水位（est 8,704）、13 卡 109 槽逐槽 label ==
#   `ENG DESC`、每卡首槽 `UI SOUND KNOB` 拖动改值 / 拖回还原、环带外与无锚点忽略、键盘焦点 ±1 与环绕 /
#   ↑↓ / ENTER / 页级 ±、折叠跳过与全折叠拒绝（R12）、滚动钳位 / 折叠重排 / 焦点自动滚入、选轨条、
#   引擎 / 预置 / kit 选择器（R16）、鼓轨逐槽锁定、INSERT 标签随 ITYPE、编辑跨页保值、12 引擎轮换、
#   SEQ→SOUND 过渡峰值与 SOUND↔STEP ×10 往返、6 张截屏像素抽检（场景逻辑 = inject.py 的 run_sound）
# 用法: bash scripts/inject/sound.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 改动面：轨 0 的引擎·预置（12 引擎轮换 / 选择器）、轨 0 的 71 个参数与 50 个全局参数（拖动改值后拖回）、
#   鼓轨 kit、选中轨、ZOOM（仅入口为开时归一为关）；轨 1 若非 ANALOG 先为⑧ 归一并还原。
#   入口快照在归一之前拍，try/finally 还原并逐项核对
#   （轨 0 = `ENG TRACK` 回入口引擎·预置 + 71 参数整份回写 ⇒ 入口声音须来自工厂预置）。不做破坏性工程/银行
#   STORE 操作（ZOOM 归一只用 `STORE SETTINGS SET`）、不碰步 / lane / bank / 用户预置。
# 断言与截屏都在 scripts/inject/inject.py 的 sound 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-sound-<时间戳>.log + PNG
#   （sound-{default,scrolled,collapsed,picker-engine,picker-preset,drum}.png 六张）。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario sound --port "$PORT"
