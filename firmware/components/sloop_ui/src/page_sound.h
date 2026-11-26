/*
 * Sound 仪表盘页面（sloop_ui 内部头文件；5B-1，5B 规格 §5.2；ADR-008 末节）。
 *
 * 主体 = 一个滚动容器 + 13 张自绘卡片（成员表与布局 = sound_layout.h 的纯函数）：选中轨的
 * 全部参数组各占一张卡，卡内每行 4 个槽（140×132，96px 的环）。它作为 PARAM_FEAT_DASHBOARD
 * 挂在参数页框架上——页眉行、FM6 银行行、页级 ± 按钮仍由 page_params.c 建，本模块只管卡片
 * 区。卡片成员、槽的值/锁定/拖动改值在后续任务接入。
 *
 * 坐标：入口收到的 (x, y) 是页面区相对坐标；sound_layout_* 给的是**内容坐标**（原点 = 列 0 /
 * y 0），两者只差「视口左上角 − 滚动量」，换算在 page_sound.c 的一处（content_point）。
 *
 * 手势：容器与卡片都不让 LVGL 自己滚（SCROLLABLE/SCROLL_CHAIN/GESTURE_BUBBLE 清掉），滚动
 * 由卡片的事件回调驱动 lv_obj_scroll_by_bounded——这样「环带内按下 = 旋钮、环带外 = 滚动」
 * 的归属在按下那一刻就定了，不经过 LVGL 的滚动判定。
 *
 * 线程：全部入口由 page_params.c 在 LVGL 锁内调用（build/update/tap）或 LVGL 事件回调里跑。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

/* 在 parent（页面根）内建滚动容器 + 13 张卡片；任一对象分配失败返回 NULL（半成品由 ui.c
 * 随页面根删除回收，控件指针由紧随的 page_sound_forget 清掉）。视口先按「银行行隐藏」放，
 * 紧随的首拍 update 按选中轨引擎挪位（构建路径不取状态副本）。 */
lv_obj_t *page_sound_build(lv_obj_t *parent);
/* 每拍：银行行可见性翻转 ⇒ 挪容器；选中轨/引擎变 ⇒ 重取槽名称并失效重绘。 */
void page_sound_update(const app_state_t *st);
/* 页面销毁（组作用域 / 构建失败）：清控件指针与手势态。 */
void page_sound_forget(void);
/* TAP 注入（页面区相对坐标）：命中的卡片标题行 ⇒ SLOOP_TAP_SOUND_CARD{a=卡片}；命中槽
 * 矩形 ⇒ SLOOP_TAP_SOUND_FOCUS{a=卡片 b=槽 v=扁平下标}；视口外/空白处未命中返回 false。
 * 本阶段只判命中，不改任何状态。 */
bool page_sound_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
