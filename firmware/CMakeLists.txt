# sloop-fm1-tab5 firmware — top-level CMake
# ESP-IDF >= 5.4 required by espressif/m5stack_tab5 BSP; 本机使用 v5.5.5（决策记录见 docs/design/001-idf-version.md）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sloop_fm1_tab5)
