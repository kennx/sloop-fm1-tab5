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

/* TAP 注入（坐标为页面区相对坐标，调用方已扣页签条）：与控件回调同一动作函数。 */
bool page_params_tap(uint8_t index, int16_t x, int16_t y, sloop_tap_result_t *out);
