/*
 * 描述符驱动的参数页（sloop_ui 内部头文件；ADR-008 §8）。
 *
 * 一页 = 布局表的 8 槽旋钮（knob.c）+ 页眉上下文行 + 一对 ± 细调按钮。
 * 值显示/编辑经 sloop_core_desc（label/fmt/min/max/enums）与状态层命令：
 *   GLOBAL  → CMD_SET_GLOBAL
 *   TRACK   → CMD_SET_PARAM（a = 选中轨，b = 轨道参数 id）
 *   ENGINE  → CMD_SET_PARAM（a = 选中轨，b = P_E0 + 槽位 id；描述符取页声明的引擎）
 *   SOUND   → CMD_SET_PARAM（a = 选中轨，b = P_E0..P_E7；描述符经 sloop_core_desc_track_live
 *             取**选中轨引擎**的槽——TRACK scope，标签随引擎钩子换）
 * 引擎页（SAMPLE）只在「选中轨的引擎 = 页声明的引擎」时可编辑（否则显示 --，避免用
 * SAMPLE 的 label 写另一个引擎的槽位）；鼓轨不适用引擎页。
 *
 * chip 页（P4-F Task 4/5）：布局表 chip_count ≥ 2 的页（MOD/SCALE/VOICE/GLOBAL/FX）有
 * chip 行，8 个旋钮对象常驻，切 chip 零分配重配；选中 chip 是页内 UI 局部态
 * （page_params_chip_get/set 与 chip 行、console `UI <page> CHIP` 共用）。鼓轨规则逐槽：
 * 未标 drum_ok 的槽显示 --、拒绝编辑。sloop_ui.h 的 sloop_ui_param_chip_* 是加锁外壳。
 *
 * SOUND 页（P4-F Task 5）：页眉 ENGINE/PRESET 两行（鼓轨 = 单行 KIT）与仅 FM6 引擎出现的
 * bank 行，见 page_params.c 文件头；TAP 回显走 sloop_tap_kind_t 的 SLOOP_TAP_SOUND_*。
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

/* SOUND 页 bank 存取行的状态行文本（console `TAP` 回显与屏幕同源，P4-B Task 13 /
 * P4-F Task 5）：该页没有 bank 行（不是 SOUND）或 bank 行尚未构建（页未建/已随源组
 * 销毁）时返回 NULL；映射复用 theme.h 的 ui_file_status_text（act 0 SAVE / 2 ERASE）。
 * 不取 LVGL 锁（读 UI 局部态 + 状态层快照）。 */
const char *page_params_bank_status_text(uint8_t index);

/* SAMPLE 页槽管理行的状态行文本（console `TAP` 回显与屏幕同源，P4-D Task 8）：
 * 该页没有槽管理行（不是 SAMPLE）或行尚未构建（页未建/已随源组销毁）时返回 NULL；
 * 页面本地拒绝（SET 不在 USR1..USR4，或选中轨引擎 ≠ SAMPLE）返回 "NOT USR"；其余映射
 * 复用 theme.h 的 ui_sample_status_text（`SET U<n>` / `ERASE OK` / `STOP BEFORE ERASE` /
 * `NO STORAGE` / `ERASE ERROR`）。不取 LVGL 锁（读 UI 局部态 + 状态层快照）。 */
const char *page_params_sample_status_text(uint8_t index);

/* chip 页的生效 chip（读：条件 chip 隐藏时 = 夹后的首个可见 chip）。不取 LVGL 锁。 */
uint8_t page_params_chip_get(uint8_t index);
/* 写 chip 选择：**调用方已持 LVGL 锁**（已建页立即重配刷新）。非 chip 页/越界 =
 * ESP_ERR_INVALID_ARG；条件 chip 在当前上下文隐藏 = ESP_ERR_NOT_ALLOWED（不改选择）。 */
esp_err_t page_params_chip_set(uint8_t index, uint8_t chip);
/* 槽位诊断（console 逐槽行）：绝对 id / 旋钮正在显示的名称（"-" = 无）/ 是否锁定。
 * 槽不存在回 false。不取 LVGL 锁。 */
bool page_params_slot_info(uint8_t index, uint8_t chip, uint8_t slot, uint8_t *id,
                           const char **label, bool *locked);
/* 槽位是否锁定（引擎页不适用/鼓轨逐槽规则/槽不存在）；chip 越界 = 锁定。不取 LVGL 锁。 */
bool page_params_slot_locked(uint8_t index, uint8_t chip, uint8_t slot);

/* 调试注入（console `UI KNOB`）：向**当前**参数页第 slot 槽旋钮派发合成
 * PRESSED/PRESSING（dx/dy 相对旋钮中心），走与物理触摸相同的处理路径。
 * 调用方已持 LVGL 锁。返回 ESP_OK（事件被接受）/ ESP_ERR_NOT_FOUND（环带外，
 * 事件未接受）/ ESP_ERR_INVALID_ARG（非参数页、槽位越界或无控件）/
 * ESP_ERR_INVALID_STATE（页未建）。*value 回填旋钮当前值（可 NULL）。 */
esp_err_t page_params_test_knob(uint8_t slot, int32_t dx, int32_t dy, bool press, int16_t *value);
