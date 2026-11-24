/* SYSTEM 设置页（sloop_ui 内部接口；P4-E Task 6）。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

lv_obj_t *page_system_build(lv_obj_t *parent);
void page_system_update(const app_state_t *st);
void page_system_forget(void);
bool page_system_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
esp_err_t page_system_key(uint32_t code);
const char *page_system_status_text(void);
void page_system_clear_action(void);
esp_err_t page_system_selftest_arm(void);
