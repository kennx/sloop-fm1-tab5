/*
 * sloop-fm1-tab5 — 显示组装（bring-up）
 *
 * 经 BSP 组装 MIPI-DSI DPI panel + LVGL，并保留 panel 句柄供后续
 * SCR 截屏（AGENTS.md §5.3）取帧缓冲；同时输出固定测试图，
 * 布局锚定值见 display.c 文件头注释（Task 4 像素抽检依赖）。
 * 触摸 indev 亦在此挂接，并启动十字准星 + 坐标日志反馈（Task 5）。
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

esp_err_t app_display_init(void);                /* 组装 DPI panel + LVGL；成功后内部持有 panel 句柄 */
esp_lcd_panel_handle_t app_display_panel(void);  /* 供 console SCR 取帧缓冲（Task 3 消费） */
void app_display_touch_feedback_start(void);     /* 触摸十字准星 + 坐标日志（Task 5；init 成功路径内部已调用） */
void app_display_draw_test_pattern(void);        /* UI 启动失败时的回退：清屏重画 bring-up 测试图 */
