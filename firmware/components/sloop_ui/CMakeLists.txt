# 上游生成表（TIME_MS_X10/CUTOFF_HZ/LFO_HZ_X100/LEVEL_DB_X10）的目录：knob_math.c
# 用它做与上游 param_format 同表同算法的显示格式化（host 单测 scripts/knob-test.sh
# 也引用同一目录）。表头由 scripts/host-test.sh（tools/gen_tables.py）生成；
# sloop_core 的构建本来就硬依赖它，这里给出同样明确的失败信息。
set(SLOOP_TABLES_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/sloop-fm1/build/gen")
if(NOT EXISTS "${SLOOP_TABLES_DIR}/felucca_tables.h")
    message(FATAL_ERROR "缺少 ${SLOOP_TABLES_DIR}/felucca_tables.h；先运行 scripts/host-test.sh（生成上游表头）")
endif()

idf_component_register(
    SRCS "src/ui.c" "src/input.c" "src/page_play.c" "src/page_tracks.c" "src/page_step.c"
         "src/page_transport.c" "src/knob.c" "src/knob_math.c" "src/page_layout.c"
         "src/sound_layout.c" "src/page_sound.c"
         "src/page_params.c" "src/page_projects.c" "src/page_presets.c" "src/page_song.c"
         "src/page_dsyn.c" "src/page_fm6p.c" "src/page_system.c" "src/page_tools.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__m5stack_tab5 esp_lvgl_port esp_app_format sloop_state sloop_core
                  sloop_hal_tab5 tab5_keyboard freertos
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${SLOOP_TABLES_DIR}")
