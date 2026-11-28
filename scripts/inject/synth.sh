#!/usr/bin/env bash
# synth.sh — SYNTH 组注入矩阵（P4-D Task 11）：采样槽 / DSYN 鼓合成 / FM6 深度 patch
#   ⓪ 归一：STORE AUTO OFF、停走带、轨 0、擦 USR1、DSYN kit 0 = 工厂 808、银行槽 0..2 空、
#      步窗 0/16
#   ① 采样（SMP + scripts/sample_upload.py 的编码器）：上传 2s 正弦 → `SMP INFO` 的
#      name/zones/KB → `ENG TRACK 0 4 0` + `PARAM 63 8` + 按住键 ⇒ `ENG SNAP voices≥1`
#      → `SMP ERASE` ⇒ 静音；BEGIN 不 END（镜像失效）⇒ 静音；CRC 坏数据 ⇒ `SMP ERR crc`
#      且槽仍空；走带中 BEGIN/ERASE ⇒ `SMP ERR playing`
#   ② DSYN：`DSYN GET 0 0` 22B → SET WAVE/PITCH/DECAY/LEVEL + `DSYN CRUSH` → `DSYN INFO`
#      回读 → `DSYN AUDITION` → `DSYN STORE`（dirty=0）→ 硬复位后 GET 逐字节一致 →
#      `ENG SELFTEST` 后仍一致（DSU reload）
#   ③ FM6P：FM6 轨 `FM6P GET` 155B → SET ALG/OP1 R1/OL → 155B 偏移回读 → `FM6P ALG 0`
#      清 ALG 宏 → 非 FM6 轨 SET ⇒ `locked` 且 patch 不变 → `STORE BANK PUT` + GET==LIVE
#      → 硬复位后银行槽逐字节保持
#   ④ UI：SAMPLE/DSYN/FM6P 三页截屏 + 像素抽检、`UI MEM` 的 SYNTH Σ 20,480 与逐页
#      delta ≤ est、TAP 回显与屏幕状态行同源
#   ⑤ 池：SYNTH 三页全建 → STEP 首建 `pool_free ≥4,096`；SYNTH/PLAY/SONG/FILE 往返 ≤128B
#   ⑥ 收口：擦 USR1、DSYN 工厂 808 + STORE、银行槽擦除、轨 0/ANALOG、STORE AUTO ON；
#      随后恢复入口轨 0/1 的引擎/预置与参数向量，最后恢复入口选轨
# 用法: bash scripts/inject/synth.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 synth 场景里；本脚本只是入口。
# **破坏性**（占用 USR1 / DSYN kit 0 / 银行槽 1，可能改写 autosave 工程），自带归一、可重复。
# 产物：$SLOOP_TAB5_PROJECT/build/verify/inject-synth-<时间戳>.log + 三张 PNG
# （sample/dsyn/fm6p）。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario synth --port "$PORT"
