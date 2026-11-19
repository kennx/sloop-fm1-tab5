#!/usr/bin/env bash
# env.sh — sloop-fm1-tab5 构建/烧录脚本共享的环境探测库（被 source，不直接执行）
#
# 探测顺序：
#   1. 当前环境已有 idf.py（PATH 中）→ 直接使用
#   2. $ESP_IDF_EXPORT 指向的 export.sh
#   3. 常见安装位置：~/esp/esp-idf、~/esp/v*/esp-idf、/opt/esp-idf、/usr/local/esp-idf
# 命中后在该子 shell 内 source export.sh，使 idf.py 可用。

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 默认 ESP-IDF 工程目录，可用 SLOOP_TAB5_PROJECT 覆盖
PROJECT_DIR="${SLOOP_TAB5_PROJECT:-$REPO_ROOT/firmware}"

die() { echo "错误: $*" >&2; exit 1; }

_idf_export_candidates() {
    local c
    [ -n "${ESP_IDF_EXPORT:-}" ] && printf '%s\n' "$ESP_IDF_EXPORT"
    for c in "$HOME"/esp/esp-idf/export.sh \
             "$HOME"/esp/v*/esp-idf/export.sh \
             /opt/esp-idf/export.sh \
             /usr/local/esp-idf/export.sh; do
        [ -f "$c" ] && printf '%s\n' "$c"
    done
}

# 确保 idf.py 可用；失败则给出指引进而死掉
ensure_idf() {
    if command -v idf.py >/dev/null 2>&1; then
        return 0
    fi
    local c
    while IFS= read -r c; do
        # shellcheck disable=SC1090
        if . "$c" >/dev/null 2>&1 && command -v idf.py >/dev/null 2>&1; then
            echo "已加载 ESP-IDF 环境: $c" >&2
            return 0
        else
            echo "提示: 候选 export.sh 加载失败（环境损坏或缺少依赖）: $c" >&2
        fi
    done < <(_idf_export_candidates)
    die "未找到 ESP-IDF（idf.py）。请先安装 ESP-IDF ≥5.4，或 source 其 export.sh，或设置 ESP_IDF_EXPORT=/path/to/esp-idf/export.sh"
}

# 探测串口：优先 USB Serial/JTAG（ttyACM），其次 USB-UART（ttyUSB）
detect_port() {
    local p
    for p in /dev/ttyACM[0-9]* /dev/ttyUSB[0-9]*; do
        [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

# 确认工程存在
require_project() {
    [ -f "$PROJECT_DIR/CMakeLists.txt" ] || \
        die "ESP-IDF 工程未初始化: $PROJECT_DIR（无 CMakeLists.txt）。可设置 SLOOP_TAB5_PROJECT 指向工程目录，或先创建 firmware/ 工程"
}
