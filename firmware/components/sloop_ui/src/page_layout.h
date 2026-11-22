/*
 * 参数页策展布局表（sloop_ui 内部头文件；ADR-008 §8）。
 *
 * 「页 = 参数组」是 **UI 关注点**：上游 params.c 的 page_t PAGES[] 只作为语义/观感
 * 参照（ENV/LFO/FX/GLOBAL/MASTER…），本表按 8 槽/页重新策展（映射而非逐像素复刻）。
 * 每个槽位 {scope, engine, id} 直接喂给 sloop_core_desc（描述符单一来源）与状态层
 * 参数命令；页 id/title/est 是页面注册表条目（ui_page_t）的元数据。
 *
 * 本文件只声明接口；表本身在 page_layout.c（X-macro 一处定义，注册表条目由它生成，
 * 不存在第二份页元数据）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sloop_core.h"
#include "sloop_ui.h"

#define PARAM_PAGE_SLOTS 8

/* 槽位：scope = sloop_desc_scope_t。GLOBAL 用全局 id（GP[]）；TRACK 用轨道参数 id
 * （TP[]，id < P_E0）；ENGINE 用引擎槽 id 0..7（= P_E0..P_E7，engine 为页声明的
 * 引擎号）。id == 0xFF 表示空槽。 */
typedef struct {
    uint8_t scope;
    uint8_t engine;
    uint8_t id;
} param_slot_t;

typedef struct {
    const char *title; /* 页签文本（注册表条目同源） */
    param_slot_t slots[PARAM_PAGE_SLOTS];
} param_page_t;

/* 参数页数（= app_page_t 里 GLOBAL..SAMPLE 的连续段长度，见 page_layout.c 的静态断言）。 */
#define PARAM_PAGE_COUNT 5

/* 按布局表下标取页定义；越界返回 NULL。 */
const param_page_t *page_layout_get(uint8_t index);

/* 页 id（app_page_t）→ 布局表下标；非参数页/未注册返回 -1。 */
int page_layout_index(uint8_t page_id);

/* 把布局表里的所有页注册进页面注册表（sloop_ui_start 内置注册之后调用；幂等由
 * ui.c 的内置注册标志保证）。返回第一个非 ESP_OK 的错误，全成功返回 ESP_OK。 */
esp_err_t page_layout_register(void);
