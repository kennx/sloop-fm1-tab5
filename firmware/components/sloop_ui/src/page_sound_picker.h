/*
 * Sound 页的引擎 / 预置 / 鼓 kit 选择器（sloop_ui 内部头文件；5B-1 Task 6，逻辑在 page_sound_picker.c）。
 *
 * 面板是页面对象根的子对象（页面区 (160,120) 960×440，盖在卡片区之上），打开时建、关闭时删——三个
 * 临时 LVGL 对象（面板 / 引擎 chip 行 / 列表），不跨页存活。合成轨：row 0 = 引擎 chip 行 + 引擎列表，
 * row 1 = 引擎 chip 行 + **当前引擎**的预置列表；鼓轨：row 0 = kit 列表（没有 chip 行：46 个放不下）。
 *
 * 选择规则（TAP、触摸与键盘同一组动作函数）：chip 只移动高亮/预览，不下发；再点一次已高亮的 chip 或
 * 在 chip 行上按 ENTER ⇒ CMD_SET_ENGINE 并关闭。列表行 = 直接选择：引擎行 / 鼓 kit 行 / 当前引擎的
 * 预置行各发一条命令并关闭；预览别的引擎时点预置行不下发（换引擎与换预置是两条命令）；确认的就是
 * 当前项时不下发（免得误触 ENTER 把当前声音复位）。←/→ 移动 chip 高亮，↑/↓ 移动列表高亮（row 0
 * 两者是同一个引擎下标），ENTER 确认最后移动的那一行，ESC 关闭。选中轨或它的声音（引擎/预置/kit）
 * 一变，page_sound_update 就关掉选择器；发命令前再比对一次打开时的上下文。
 *
 * 线程：active / move / confirm / cancel 不取 LVGL 锁，键盘路由在输入任务里调——状态是 portMUX 保护的
 * PSRAM 静态，confirm 只发一条状态层命令，对象由下一拍 page_sound_picker_tick 删除/重绘；其余入口都在
 * LVGL 上下文（持锁的 console 外壳、事件回调、每拍 update）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

/* 页面对象根（建页时挂上：面板建在它里面、晚于全部页内控件 = 最上层）；页面销毁时 forget。
 * forget 不删对象——那时页面根已经删了，只清指针与状态。 */
void page_sound_picker_attach(lv_obj_t *parent);
void page_sound_picker_forget(void);

/* 打开（持锁；页眉行名字区 / TAP）。row 0 = 引擎（鼓轨 = kit），1 = 预置；鼓轨没有 row 1。
 * 已打开时关掉重建（高亮回到当前项）。close = 立即删对象（LVGL 上下文，且不在选择器自己的事件
 * 回调里——那里的关闭只清 active，对象由 tick 删）。cancel 是给输入任务的 close：只清 active。 */
void page_sound_picker_open(uint8_t row);
void page_sound_picker_close(void);
void page_sound_picker_cancel(void);
bool page_sound_picker_active(void); /* 无锁 */

/* 键盘（无锁）：dx = chip 高亮 ±1（环绕；row 1 换预览的引擎，row 0 与列表高亮同一个下标；鼓轨忽略），
 * dy = 列表高亮 ±1（环绕）。confirm = ENTER：最后移动的那一行（chip 行 ⇒ 换引擎；列表 ⇒ 选中项），
 * 发了命令回 true。 */
void page_sound_picker_move(int8_t dx, int8_t dy);
bool page_sound_picker_confirm(void);

/* 每拍（LVGL 上下文，page_sound_update 调）：选中轨或它的声音快照变了 ⇒ 关闭；已关闭而对象还在 ⇒
 * 删；高亮变过 ⇒ 失效重绘。 */
void page_sound_picker_tick(const app_state_t *st);

/* TAP（页面区相对坐标；持锁）。tap：落在面板内 ⇒ 执行与点击回调相同的动作并回
 * SLOOP_TAP_SOUND_PICK{a=0 引擎或 kit / 1 预置, b=下标, v=命令值（下发时），d=1 鼓轨, err=结果码}，
 * 面板内的空白处吞掉不下传；面板外回 false（照常走页面的命中表）。plus_tap：页级 −/+ 在选择器打开
 * 时 = 列表高亮 ∓1（+ = 上一行，同 ↑），回 SLOOP_TAP_SOUND_PICK{… err=SLOOP_PICK_ERR_HIGHLIGHT}。 */
bool page_sound_picker_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
bool page_sound_picker_plus_tap(int8_t delta, sloop_tap_result_t *out);

/* console `UI SOUND PICK`（持锁）：第 n 行的文字/是否当前项/屏幕矩形（含 72）。未打开
 * ESP_ERR_INVALID_STATE；n 越界 ESP_ERR_NOT_FOUND（buf 置空串、矩形清零）。status：打开的行、当前
 * 引擎（鼓轨 0）、chip 高亮、列表行数。 */
esp_err_t page_sound_picker_rect(uint8_t n, int16_t *x, int16_t *y, int16_t *w, int16_t *h);
esp_err_t page_sound_picker_info(uint8_t n, char *buf, uint32_t buf_n, bool *current);
void page_sound_picker_status(uint8_t *row, uint8_t *engine, uint8_t *highlight, uint8_t *rows);
