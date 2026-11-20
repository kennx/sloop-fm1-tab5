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

idf_component_register(
    SRCS "src/sloop_core.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    PRIV_REQUIRES sloop_hal_tab5
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${SLOOP_UPSTREAM_SRC_DIR}"
    "${SLOOP_UPSTREAM_HAL_DIR}"
    "${SLOOP_GENERATED_INCLUDE_DIR}"
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
