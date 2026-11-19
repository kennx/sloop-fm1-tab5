/*
 * sloop-fm1-tab5 — 显示组装（bring-up）
 *
 * 经 BSP 组装 MIPI-DSI DPI panel + LVGL，并保留 panel 句柄供后续
 * SCR 截屏（AGENTS.md §5.3）取帧缓冲；同时输出固定测试图，
 * 布局锚定值见 display.c 文件头注释（Task 4 像素抽检依赖）。
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

esp_err_t app_display_init(void);                /* 组装 DPI panel + LVGL；成功后内部持有 panel 句柄 */
esp_lcd_panel_handle_t app_display_panel(void);  /* 供 console SCR 取帧缓冲（Task 3 消费） */
