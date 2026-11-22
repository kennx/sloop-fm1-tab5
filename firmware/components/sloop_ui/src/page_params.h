/*
 * 描述符驱动的参数页（sloop_ui 内部头文件；ADR-008 §8）。
 *
 * 一页 = 布局表的 8 槽旋钮（knob.c）+ 页眉上下文行 + 一对 ± 细调按钮。
 * 值显示/编辑经 sloop_core_desc（label/fmt/min/max/enums）与状态层命令：
 *   GLOBAL  → CMD_SET_GLOBAL
 *   TRACK   → CMD_SET_PARAM（a = 选中轨，b = 轨道参数 id）
 *   ENGINE  → CMD_SET_PARAM（a = 选中轨，b = P_E0 + 槽位 id；描述符取页声明的引擎）
 * 引擎页只在「选中轨的引擎 = 页声明的引擎」时可编辑（否则显示 --，避免用 ANALOG 的
 * label 写另一个引擎的槽位）；鼓轨不适用引擎页。
 *
 * 线程：build/update/tap 由 ui.c 在 LVGL 锁内调用；键盘路由（sloop_ui_param_*）不取
 * LVGL 锁，焦点用临界区保护，调值经状态层、控件刷新交给 30ms UI 定时器。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

/* 页构建/刷新（ui_page_t 的 build/update 经 page_layout.c 的包装传入布局表下标）。 */
lv_obj_t *page_params_build(uint8_t index, lv_obj_t *parent);
void page_params_update(uint8_t index, const app_state_t *st);
/* 页面控件已销毁（组作用域 / 强制失败重建）：清本页控件指针与 bank 拒绝态。 */
void page_params_forget(uint8_t index);

/* TAP 注入（坐标为页面区相对坐标，调用方已扣页签条）：与控件回调同一动作函数。 */
bool page_params_tap(uint8_t index, int16_t x, int16_t y, sloop_tap_result_t *out);

/* FM6 页 bank 存取行的状态行文本（console `TAP` 回显与屏幕同源，P4-B Task 13）：
 * 该页没有 bank 行（不是 FM6）或 bank 行尚未构建（页未建/已随源组销毁）时返回 NULL；
 * 锁定页（选中轨引擎 ≠ FM6，含鼓轨）的本地拒绝返回 "NOT FM6"；其余映射复用 theme.h
 * 的 ui_file_status_text（act 0 SAVE / 2 ERASE）。不取 LVGL 锁（读 UI 局部态 +
 * 状态层快照）。 */
const char *page_params_bank_status_text(uint8_t index);

/* 调试注入（console `UI KNOB`）：向**当前**参数页第 slot 槽旋钮派发合成
 * PRESSED/PRESSING（dx/dy 相对旋钮中心），走与物理触摸相同的处理路径。
 * 调用方已持 LVGL 锁。返回 ESP_OK（事件被接受）/ ESP_ERR_NOT_FOUND（环带外，
 * 事件未接受）/ ESP_ERR_INVALID_ARG（非参数页、槽位越界或无控件）/
 * ESP_ERR_INVALID_STATE（页未建）。*value 回填旋钮当前值（可 NULL）。 */
esp_err_t page_params_test_knob(uint8_t slot, int32_t dx, int32_t dy, bool press, int16_t *value);
