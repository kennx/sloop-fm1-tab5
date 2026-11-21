#!/usr/bin/env bash
# store.sh — 存储门禁注入：工程 A–D 的「RAM 槽（PSRAM）+ flash」双层往返 + 开机 fetch
#   ⑩ 换引擎/预置 = 换声音（最先跑：旧固件没有 `ENG PRESET` ⇒ RED 快速失败）：
#      `ENG PRESET 0 2 1` 取期望向量 → `ENG TRACK 0 2 1` ⇒ p[] 逐项 71/71 一致、
#      P_LEVEL/P_SLEN（param_kept）保留 → `ENG TRACK 0 1 0` 换引擎 ⇒ 71/71 +
#      独立上游表 oracle（R17：eng_digital.c/eng_analog.c/preset_trim.h 字面量）→
#      按住 KEY 0x42 时换预置 ⇒ `ENG SNAP voices` 归 0（panic 路径）、playing 不变 →
#      `STORE PROJ SYNTH fmt1|foreign`（R18：project_load 的两条 fallback；破坏诊断槽 3）
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
#   ⑪ 自动存档：空闲写 / 20s gap / 同态不写 / 播放与发声抑制 / STORE AUTO OFF / 复位恢复
#   ⑫ FM6 银行 27 槽：live patch → PUT/GET 往返（字节一致）→ LIST → 自检后紧跟 PUT（写路径
#      必须重扫 .bss 的 used 位，DI-18）→ 硬复位后仍 USED → PTCH=11 轮询 LIVE 到槽 3 的字节 +
#      ENG TRIG 出声 → ERASE/空槽/越界/播放与鼓轨拒绝
#   ⑬ 设备侧 torn-write 套件：STORE FAULT 1 + PROJ ERASE 3 的 HAL 探针（STORE ERR save、
#      STORE STATS faults=1 armed=0）→ STORE TEST 六场景 PASS 6/6（两次）→ 播放中拒绝 →
#      停止后仍 PASS（对象 = OBJ_SETTINGS，套件内部快照/恢复；破坏性诊断）
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
