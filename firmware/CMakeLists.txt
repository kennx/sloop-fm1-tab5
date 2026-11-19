# sloop-fm1-tab5 firmware — top-level CMake
# ESP-IDF >= 5.4 required by espressif/m5stack_tab5 BSP; 本机使用 v5.5.5（决策记录见 docs/design/001-idf-version.md）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sloop_fm1_tab5)

# 构建期版本守卫（ADR-001，BSP espressif/m5stack_tab5 要求 >=5.4）。
# 注: IDF_VERSION CMake 变量在 5.5.5 的 project() 后仍为空（时序随版本变化），本安装为
# git checkout（无 version.txt），故从 git 描述取版本；取不到时降级为警告，由 BSP 的
# idf>=5.4 依赖约束在组件解析时兜底。
execute_process(
    COMMAND git -C "$ENV{IDF_PATH}" describe --tags --abbrev=0
    OUTPUT_VARIABLE _idf_ver_raw
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(REGEX REPLACE "^v" "" _idf_ver "${_idf_ver_raw}")
if(_idf_ver STREQUAL "")
    message(WARNING "无法确定 ESP-IDF 版本（$ENV{IDF_PATH} 非 git 仓库？），跳过版本守卫")
elseif(_idf_ver VERSION_LESS "5.4")
    message(FATAL_ERROR "ESP-IDF >= 5.4 required (BSP espressif/m5stack_tab5); found ${_idf_ver}")
endif()
