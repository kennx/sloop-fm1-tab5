#!/usr/bin/env bash
# song.sh — SONG 编排页注入（P4-C Task 11）：SONG 独立组 / A–D section 卡 /
# 16 项顺序 / bars·loop 配置（全部经 CMD_SONG_SET）/ SONG·CHAIN·REC·STORE·ERASE·
# LOAD·JUMP 控件、空 scene 的可见拒绝、project erase 后歌曲项消失、SONG REC 的
# NO SONG、SONG↔PLAY 与 SEQ↔SONG 的池往返、FILE→STEP 过渡峰值。
# 用法: bash scripts/inject/song.sh [PORT]   （PORT 缺省自动探测 ttyACM*）
#
# 断言与截屏都在 scripts/inject/inject.py 的 run_song 里；本脚本只是入口。
# 本场景**破坏性**：占用 section A/B 与歌曲顺序（收口擦 A/B、恢复默认歌曲），
# 自带归一、可重复运行。产物：$SLOOP_TAB5_PROJECT/build/verify/inject-song-<时间戳>.log + PNG。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
. "$SCRIPT_DIR/../env.sh"

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT="$(detect_port)" || die "未检测到串口设备。请插入 Tab5（USB-C），或显式传入 PORT"
fi

exec python3 "$SCRIPT_DIR/inject.py" --scenario song --port "$PORT"
