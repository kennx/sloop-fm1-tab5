set(SLOOP_UPSTREAM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/sloop-fm1")
set(SLOOP_UPSTREAM_SRC_DIR "${SLOOP_UPSTREAM_DIR}/firmware/src")
set(SLOOP_UPSTREAM_HAL_DIR "${SLOOP_UPSTREAM_DIR}/firmware/hal")
set(SLOOP_GENERATED_INCLUDE_DIR "${SLOOP_UPSTREAM_DIR}/build/gen")

# 生成头由 scripts/host-test.sh / tools 生成；缺失时先跑 Task 2 Step 2 命令。
foreach(header felucca_tables.h felucca_samples.h felucca_drumkits.h felucca_fm6.h)
    if(NOT EXISTS "${SLOOP_GENERATED_INCLUDE_DIR}/${header}")
        message(FATAL_ERROR "缺少生成头 ${header}；先运行 scripts/host-test.sh（Task 2 Step 2）")
    endif()
endforeach()

set(SLOOP_GOLDEN_TXT "${SLOOP_UPSTREAM_DIR}/tests/golden.txt")
# golden.txt 变化后必须重新配置，golden.inc 才会重生成（否则设备侧仍是旧哈希表）
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SLOOP_GOLDEN_TXT}")
file(READ "${SLOOP_GOLDEN_TXT}" SLOOP_GOLDEN_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," SLOOP_GOLDEN_BYTES "${SLOOP_GOLDEN_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/golden.inc"
    "static const uint8_t sloop_golden_txt[] = {${SLOOP_GOLDEN_BYTES}};\n")

idf_component_register(
    SRCS "src/selftest.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    PRIV_REQUIRES sloop_hal_tab5 heap
)

set_source_files_properties(src/selftest.c PROPERTIES COMPILE_OPTIONS "-Wno-format;-msmall-data-limit=0")

# regress.c 的 512 项 job/golden/CPU 表不进内部 RAM；DSP 状态保持内部 RAM。
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../../../tools/move_regress_tables.py"
        ${CMAKE_OBJDUMP} ${CMAKE_OBJCOPY} $<TARGET_FILE:${COMPONENT_LIB}>
    COMMENT "Move regress bookkeeping tables to PSRAM"
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${SLOOP_UPSTREAM_SRC_DIR}"
    "${SLOOP_UPSTREAM_HAL_DIR}"
    "${SLOOP_GENERATED_INCLUDE_DIR}"
    "${SLOOP_UPSTREAM_DIR}/tests"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fwrapv
    -fno-strict-aliasing
    -ffp-contract=off
    -Wno-unused-function
    -Wno-missing-field-initializers
    -Wno-unused-variable
    -Wno-maybe-uninitialized
)
