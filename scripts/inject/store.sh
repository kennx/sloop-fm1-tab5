#!/usr/bin/env bash
# store.sh — 存储门禁注入：工程 A–D 的「RAM 槽（PSRAM）+ flash」双层往返 + 开机 fetch
#   ① 四槽 ERASE → LIST 全 EMPTY（归一，脚本可重复）
#   ② BPM 100 → LIVEHASH h0 → SAVE 0 → HASH 0 == h0
#   ③ BPM 120 → LIVEHASH h1 ≠ h0
#   ④ LOAD 0 → LIVEHASH == h0、STATE SNAP bpm 回 100
#   ⑤ ERASE 0 → LIST EMPTY、LIVEHASH 不变
#   ⑥ 再 SAVE 0 → h2 == h0（同一引擎状态 ⇒ 同一 hash）
#   ⑦ esptool 硬复位（不写 flash）→ LIST 仍 USED、HASH 0 == h2（RAM 槽开机 fetch）、
#      LOAD 0 → LIVEHASH == h2
#   ⑧ ENG SELFTEST（单例）之后存储仍可用：HASH 0 == h2（PSRAM 指针/RAM 槽不被自检
#      restore 回滚——.noinit 重定向的回归守卫）
#   ⑨ 用户预置 32 槽：ERASE 归一 → 写 8 参数 → SAVE 0（默认名 ANALOG 01）→ LIST USED →
#      改值 → LOAD 0 0（8 参数逐项还原）→ ERASE 0 → 鼓轨 SAVE/LOAD 回 STORE ERR drum →
#      ENG SELFTEST 之后 LIST 仍 USED → esptool 硬复位后 LIST 仍 USED（up_boot）且
#      LOAD 还原同一组值
# 用法: bash scripts/inject/store.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言在 scripts/inject/inject.py 的 store 场景里；本脚本只是入口。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-store-<时间戳>.log（串口原始回显）。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario store --port "$PORT"
