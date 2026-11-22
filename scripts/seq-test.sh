#!/usr/bin/env bash
# seq-test.sh — P4-C 原始步/鼓 lane/micro/fill/参数锁适配 host 单测。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
UPSTREAM_DIR="$REPO_ROOT/third_party/sloop-fm1"
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"
BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}/seq-test"
BIN="$BUILD_DIR/seq_host_test"
TABLES_DIR="$UPSTREAM_DIR/build/gen"

if [ ! -f "$TABLES_DIR/felucca_tables.h" ]; then
    echo "缺少 $TABLES_DIR/felucca_tables.h；先运行 scripts/host-test.sh（生成上游表头）" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
read -r -a CC_CMD <<< "${CC:-gcc}"
"${CC_CMD[@]}" -std=gnu11 -O2 -Wall -Wextra -fwrapv -fno-strict-aliasing -ffp-contract=off \
    -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers -Wno-int-to-pointer-cast \
    -Wno-maybe-uninitialized \
    -I "$REPO_ROOT/firmware/components/sloop_core/include" \
    -I "$REPO_ROOT/firmware/components/sloop_state/test/stub" \
    -I "$UPSTREAM_DIR/firmware/src" \
    -I "$TABLES_DIR" \
    "$SCRIPT_DIR/seq-host-test.c" -lm -o "$BIN"

echo "== sloop_core sequencer adapter host test"
"$BIN"
