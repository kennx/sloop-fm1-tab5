#!/usr/bin/env bash
# params.sh — 参数页注入（**破坏性**，见下）：GLOBAL/FX（MIX 组 chip 页）、SOUND（通用
#   引擎页：ANALOG 编辑、鼓轨 KIT 行、ENGINE/PRESET 页眉、12 引擎轮换、FM6 宏与 bank 行）、
#   SAMPLE 槽管理行、DSYN、FM6P 锁定态、EDIT 组 chip 页 MOD/SCALE/VOICE（逐槽 label ==
#   描述符、chip 行 TAP、20 次切 chip 零分配、FILTER 条件 chip、鼓轨逐槽锁定）、TOOLS 页
#   （两段式确认、走带守卫、NEW 的副作用）、TRANSPORT 的 REC 配置行、STEP 页键盘参数锁
#   （Aa+k/l/9）；组作用域池水位（ADR-005 §5）、截屏
# 用法: bash scripts/inject/params.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 破坏面（inject.py 的 param_tools，⑪）：NEW 会清空工作工程——先把工作工程存进诊断槽 3，
#   结束 LOAD 并以 STORE LIVEHASH 逐位还原。section D 与工程槽 3 是同一个存储槽：D 捕获
#   覆盖该槽后收口清空 ⇒ 入口时槽 3 / section D 非空的内容**丢失**（只可在空的开发机上
#   跑）。歌曲顺序与 SONG/LOOP 模式按入口快照还原（入口歌曲引用 D 的项还原后缺 scene）。
#   工作工程快照的基线跨截屏复位重跑保持：重跑不再 SAVE，槽里不是快照即终止（非零退出）。
#   外层入口快照覆盖参数锁等前段的步编辑，并在进入 param_tools 前恢复；param_tools
#   先选轨 0 再拍自己的快照，保证后续 section D 与项目快照使用同一选中轨。
#
# 断言与截屏都在 scripts/inject/inject.py 的 params 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-params-<时间戳>.log + PNG
# （params-{global,fx,sound,sound-drum,sound-fm6,fm6-alg,sample,sample-set,fm6p-locked}
#   九张 + task6-{transport-rec-config,step-locked,tools,tools-armed,tools-cleared} 五张）。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario params --port "$PORT"
