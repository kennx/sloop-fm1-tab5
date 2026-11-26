#!/usr/bin/env bash
# sound-test.sh — sloop_ui Sound 仪表盘纯函数 host 单测（gcc；AGENTS.md §5.5）
#
# 编译 firmware/components/sloop_ui/src/sound_layout.c（纯数据卡片成员表 + 布局/下标纯函数）与
# test/sound_layout_test.c（无 LVGL/FreeRTOS，不依赖上游生成表），运行并打印 PASS/FAIL；
# 任一路径失败退出码非 0。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPONENT_DIR="$REPO_ROOT/firmware/components/sloop_ui"
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"
BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}/sound-test"
BIN="$BUILD_DIR/sound_layout_test"

mkdir -p "$BUILD_DIR"

read -r -a CC_CMD <<< "${CC:-gcc}"
"${CC_CMD[@]}" -std=c11 -O2 -Wall -Wextra \
    -I "$COMPONENT_DIR/src" \
    -I "$COMPONENT_DIR/include" \
    -I "$COMPONENT_DIR/../sloop_core/include" \
    -I "$COMPONENT_DIR/test/stub" \
    "$COMPONENT_DIR/test/sound_layout_test.c" \
    "$COMPONENT_DIR/src/sound_layout.c" \
    -o "$BIN"

echo "== sloop_ui sound layout host test"
if "$BIN"; then
    echo "SOUND LAYOUT TEST PASSED"
else
    echo "SOUND LAYOUT TEST FAILED"
    exit 1
fi
