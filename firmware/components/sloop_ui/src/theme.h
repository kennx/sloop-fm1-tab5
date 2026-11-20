/*
 * sloop_ui 主题与共享布局常量（组件内部头文件）
 *
 * 视觉规范取自阶段 3 spec §3.3：四轨主题色 蓝/绿/黄/橙、背景 0x101418、
 * 顶部页签 PLAY|TRACKS|STEP|TRANSPORT 常驻。横屏 1280×720：页签条占
 * y∈[0,72)，页面区 y∈[72,720)，各页对象坐标相对页面区左上角。
 *
 * 截屏像素抽检（scripts/screenshot.py --check）的锚点由这里的常量推导；
 * 改布局须同步更新验证锚点与本文件注释。
 */
#pragma once

#include "lvgl.h"
#include "sloop_state.h"

#define UI_W 1280
#define UI_H 720
#define UI_TAB_BAR_H 72

/* 配色 */
#define UI_BG       0x101418 /* 全局背景（页面区） */
#define UI_PANEL    0x1B2026 /* 页签条/面板底 */
#define UI_PANEL_HI 0x242B33 /* 未选中页签/次级按钮/空步格 */
#define UI_PANEL_SEL 0x2C3540 /* 选中态的卡片底（比 UI_PANEL 亮一档） */
#define UI_TEXT     0xF2F5F8 /* 主文本 */
#define UI_TEXT_DIM 0x8A949E /* 次级文本 */
#define UI_ACCENT   0xE0B32A /* 选中页签/播放头（强调色） */

/* 页签几何：4 × 176px，间距 8px，起点 x=16、y=10（条高 72） */
#define UI_TAB_X0  16
#define UI_TAB_W   176
#define UI_TAB_H   52
#define UI_TAB_GAP 8
#define UI_TAB_Y   10

/* 四轨主题色（track 0..3 = 蓝/绿/黄/橙，同时是鼓轨第四行色） */
static inline uint32_t ui_track_color(uint8_t k)
{
    static const uint32_t colors[APP_STATE_TRACKS] = {0x3B7BD8, 0x3FA34D, 0xE0B32A, 0xE06A1B};
    return colors[k < APP_STATE_TRACKS ? k : 0];
}

/* 各页由 ui.c 创建一次；update 在 30ms 定时器（LVGL 任务持锁）里做脏检查刷新。 */
void page_play_create(lv_obj_t *parent);
void page_play_update(const app_state_t *st);
void page_play_flash(uint8_t note); /* 调用方已持 bsp_display_lock */

void page_tracks_create(lv_obj_t *parent);
void page_tracks_update(const app_state_t *st);

void page_step_create(lv_obj_t *parent);
void page_step_update(const app_state_t *st);

void page_transport_create(lv_obj_t *parent);
void page_transport_update(const app_state_t *st);

/* InputManager 任务（input.c）：建互斥并启动 5ms 轮询任务；由 sloop_ui_start()
 * 在 LVGL 锁外调用（键盘驱动须已初始化）。 */
esp_err_t input_start(void);

