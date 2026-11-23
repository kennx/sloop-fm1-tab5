#!/usr/bin/env bash
# nav.sh — 导航回归注入：五条导航行为（阶段 4 路由重构最易静默破坏）
#   ① 页签条内、组签矩形外的 TAP → miss，状态/引擎不变（组签矩形内同样 miss：
#      TAP 不路由页签，切页走 LVGL 回调/UI/KEY tab）
#   ② STEP 鼓轨行 TAP 编辑 lane（P4-C Task 9：回显 CMD_DRUM_STEP_WRITE、焦点落鼓轨、
#      ENG SNAP 只有该 dstep 行变化、鼓格像素变化；再 TAP 复位位图）
#   ③ 同一 30ms 拍内 KEY tab + KEY PANIC → PANIC 撤销待切页，无回跳
#   ④ 同一 30ms 拍内 3 次 KEY tab → 只前进一页（ADR-006「一拍一页」；TAB 顺序 =
#      组顺序扁平轮转）
#   ⑤ 组作用域生命周期：EDIT→SEQ 后 EDIT 组 built=0、同组切换只改显隐、
#      跨组 build_ms 0 < x ≤100ms（ADR-005 §3/§5）
#   ⑥ 手指导航（R32）：UI TAB/UI SUB 与物理组签/子签同一动作函数——点组进上次
#      子页且记忆跨往返、跨组点组销毁源组并建目标首个子页；同拍 KEY tab 排队时
#      UI TAB 胜出、待切页被丢弃（R30）；FILE 组（Task 12）、SONG 单页组
#      （P4-C Task 11）与 **SYNTH 组三页**（P4-D Task 8：SAMPLE/DSYN/FM6P）点组/
#      子签切换、离开组后 built=0、`UI SUB 3` 越界
#   ⑦ TAB 全 14 页轮转：PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→DSYN→FM6P→STEP→
#      TRANSPORT→SONG→PROJECTS→PRESETS→PLAY（异步切页用 STATE SNAP 轮询等落地）
#   ⑧ 跨组往返不泄漏（R34）：PLAY↔TRACKS ×10 的 UI MEM pool_free 首末差 ≤ 128B 且
#      无逐次下降（每页 styles_init 只跑一次；修复前每往返 ~300B）
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
