#!/usr/bin/env bash
# build.sh — 构建 sloop-fm1-tab5 固件（标准 ESP-IDF 流程，AGENTS.md §5）
# 用法: scripts/build.sh [--clean] [额外 idf.py 参数...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
. "$SCRIPT_DIR/env.sh"

ensure_idf
require_project

CLEAN=0
EXTRA=()
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        *) EXTRA+=("$arg") ;;
    esac
done

BUILD_DIR="${SLOOP_TAB5_BUILD:-$PROJECT_DIR/build}"
if (( CLEAN )) && [ -d "$BUILD_DIR" ]; then
    case "$BUILD_DIR" in
        ""|"/"|"//"|"/tmp"|"$HOME") die "拒绝清理可疑的构建目录: '$BUILD_DIR'" ;;
    esac
    echo "==> 清理 $BUILD_DIR"
    rm -rf --one-file-system "$BUILD_DIR"
fi

echo "==> 构建 $PROJECT_DIR (build: $BUILD_DIR)"
idf.py -C "$PROJECT_DIR" -B "$BUILD_DIR" "${EXTRA[@]+"${EXTRA[@]}"}" build

echo "==> 构建完成: $BUILD_DIR"
