#!/usr/bin/env bash
# state-test.sh — sloop_state 状态层 host 单测（gcc + stub 引擎；AGENTS.md §5.5）
#
# 编译 firmware/components/sloop_state 的 src/*.c（-DHOST_TEST 关掉 FreeRTOS 互斥）
# 与 test/ 下的 stub 引擎/用例，运行并打印 PASS/FAIL；任一路径失败退出码非 0。
# 真实 sloop_hal.h 经 test/stub/esp_codec_dev.h 在 host 上可包含（替身签名由编译器核对）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPONENT_DIR="$REPO_ROOT/firmware/components/sloop_state"
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"
BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}/state-test"
BIN="$BUILD_DIR/state_test"

mkdir -p "$BUILD_DIR"

read -r -a CC_CMD <<< "${CC:-gcc}"
"${CC_CMD[@]}" -std=c11 -O2 -Wall -Wextra -DHOST_TEST \
    -I "$COMPONENT_DIR/include" \
    -I "$COMPONENT_DIR/../sloop_core/include" \
    -I "$COMPONENT_DIR/../sloop_hal_tab5/include" \
    -I "$COMPONENT_DIR/test" \
    -I "$COMPONENT_DIR/test/stub" \
    "$COMPONENT_DIR/test/state_test.c" \
    "$COMPONENT_DIR/test/stub_engine.c" \
    "$COMPONENT_DIR"/src/*.c \
    -o "$BIN"

echo "== state layer host test"
if "$BIN"; then
    echo "STATE TEST PASSED"
else
    echo "STATE TEST FAILED"
    exit 1
fi
