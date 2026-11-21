#!/usr/bin/env bash
# knob-test.sh — sloop_ui 旋钮纯函数 host 单测（gcc；AGENTS.md §5.5）
#
# 编译 firmware/components/sloop_ui/src/knob_math.c 与 test/knob_test.c（无 LVGL/
# FreeRTOS），运行并打印 PASS/FAIL；任一路径失败退出码非 0。依赖上游生成表
# third_party/sloop-fm1/build/gen/felucca_tables.h（scripts/host-test.sh 生成）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPONENT_DIR="$REPO_ROOT/firmware/components/sloop_ui"
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"
BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}/knob-test"
BIN="$BUILD_DIR/knob_test"
TABLES_DIR="$REPO_ROOT/third_party/sloop-fm1/build/gen"

if [ ! -f "$TABLES_DIR/felucca_tables.h" ]; then
    echo "缺少 $TABLES_DIR/felucca_tables.h；先运行 scripts/host-test.sh（生成上游表头）" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"

read -r -a CC_CMD <<< "${CC:-gcc}"
"${CC_CMD[@]}" -std=c11 -O2 -Wall -Wextra \
    -I "$COMPONENT_DIR/src" \
    -I "$COMPONENT_DIR/../sloop_core/include" \
    -I "$COMPONENT_DIR/test/stub" \
    -I "$TABLES_DIR" \
    "$COMPONENT_DIR/test/knob_test.c" \
    "$COMPONENT_DIR/src/knob_math.c" \
    -o "$BIN"

echo "== sloop_ui knob math host test"
if "$BIN"; then
    echo "KNOB TEST PASSED"
else
    echo "KNOB TEST FAILED"
    exit 1
fi
