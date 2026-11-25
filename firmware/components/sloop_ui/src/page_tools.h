/* TOOLS 页（sloop_ui 内部接口；P4-F Task 6，MIX 组）。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

lv_obj_t *page_tools_build(lv_obj_t *parent);
void page_tools_update(const app_state_t *st);
bool page_tools_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
const char *page_tools_status_text(void);
