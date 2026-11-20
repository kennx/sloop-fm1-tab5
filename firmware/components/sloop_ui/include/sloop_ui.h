/*
 * sloop-fm1-tab5 — sloop_ui 公共接口（LVGL 四屏框架）
 *
 * 单容器屏 + 顶部页签 PLAY|TRACKS|STEP|TRANSPORT：页对象只创建一次，
 * 切页只改显隐（不销毁重建）。本任务实现 PLAY/TRANSPORT，TRACKS/STEP 为
 * 占位页（Task 5 扩展）。
 *
 * 线程：所有 LVGL 调用在 bsp_display_lock 内（esp_lvgl_port 递归锁，与
 * display.c 同一纪律）；UI 定时器 30ms 运行在 LVGL 任务里（已持锁）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 创建页签 + 四屏对象并注册 30ms UI 定时器；须在 state_init() 与显示初始化之后调用。
 * 成功时接管控件树（清掉 bring-up 测试图）；失败时调用方回退 app_display_draw_test_pattern()。 */
esp_err_t sloop_ui_start(void);

/* 切页：0=PLAY 1=TRACKS 2=STEP 3=TRANSPORT（同 app_page_t）。越界忽略。
 * 可从任意任务调用（内部加锁）；UI 未启动时静默返回。 */
void sloop_ui_switch_page(uint8_t page);

/* 当前页；UI 未启动返回 0xFF。 */
uint8_t sloop_ui_current_page(void);

/* 音符触发指示（PLAY 页大号指示）：把音符交给 30ms UI 定时器点亮，120ms 后熄灭。
 * 契约：不取 LVGL/state/引擎锁——可从任意任务调用（输入任务、console 注入）；
 * 只保留最新一个音符（无队列、无阻塞），点亮最多延迟一拍 UI（30ms）。 */
void sloop_ui_flash_note(uint8_t note);

/* InputManager 入口（input.c）：code = tab5_keyboard 事件码（row<<4|col），
 * down = 按下/释放。物理键盘（input 任务 5ms 轮询）与 console `KEY` 注入共用；
 * 可从任意任务调用（内部互斥 + 状态层互斥），不得在 ISR 中调用。 */
void sloop_ui_input_key(uint8_t code, bool down);

