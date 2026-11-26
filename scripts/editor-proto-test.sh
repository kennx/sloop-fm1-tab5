#!/usr/bin/env bash
# editor-proto-test.sh — editor_proto 编辑器协议帧编解码 host 单测（gcc；AGENTS.md §5.5）
#
# 编译 firmware/components/editor_proto 的 src/editor_proto.c 与 test/editor_proto_test.c
# （纯 C，无 IDF 依赖；黄金向量取自上游 firmware/src/editor.c），运行并打印 PASS/FAIL；
# 任一路径失败退出码非 0。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPONENT_DIR="$REPO_ROOT/firmware/components/editor_proto"
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"
BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}/editor-proto-test"
BIN="$BUILD_DIR/editor_proto_test"

mkdir -p "$BUILD_DIR"

read -r -a CC_CMD <<< "${CC:-gcc}"
"${CC_CMD[@]}" -std=c11 -O2 -Wall -Wextra \
    -I "$COMPONENT_DIR/include" \
    "$COMPONENT_DIR/test/editor_proto_test.c" \
    "$COMPONENT_DIR/src/editor_proto.c" \
    -o "$BIN"

if "$BIN"; then
    echo "EDITOR PROTO TEST PASSED"
else
    echo "EDITOR PROTO TEST FAILED"
    exit 1
fi
