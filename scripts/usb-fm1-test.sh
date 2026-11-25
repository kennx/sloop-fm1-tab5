#!/usr/bin/env bash
# usb-fm1-test.sh — usb_fm1 的 USB-MIDI 事件包 ⇄ SysEx 帧 host 单测（gcc；AGENTS.md §5.5）
#
# 编译 firmware/components/usb_fm1 的 src/midi_stream.c 与 test/midi_stream_test.c
# （纯 C，无 IDF 依赖；事件包约定取自上游 firmware/src/usb.c），运行并打印 PASS/FAIL；
# 任一路径失败退出码非 0。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPONENT_DIR="$REPO_ROOT/firmware/components/usb_fm1"
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"
BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}/usb-fm1-test"
BIN="$BUILD_DIR/midi_stream_test"

mkdir -p "$BUILD_DIR"

read -r -a CC_CMD <<< "${CC:-gcc}"
"${CC_CMD[@]}" -std=c11 -O2 -Wall -Wextra \
    -I "$COMPONENT_DIR/src" \
    "$COMPONENT_DIR/test/midi_stream_test.c" \
    "$COMPONENT_DIR/src/midi_stream.c" \
    -o "$BIN"

if "$BIN"; then
    echo "USB FM1 TEST PASSED"
else
    echo "USB FM1 TEST FAILED"
    exit 1
fi
