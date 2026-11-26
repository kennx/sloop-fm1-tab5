/*
 * Sound 仪表盘的卡片成员表与布局纯函数（sloop_ui 内部头文件；5B-1，5B 规格 §5.2）。
 *
 * 成员表 = 选中轨的全部参数组，13 张卡（Global / Master / Envelope / LFO / Engine edit /
 * Voice / FX / Scale / Arp / Pattern / Drums / MIDI / Texture），顺序与分组照 Sloop Go
 * ParamsScreen.kt 的 layout(pe0)。每个成员 = 作用域 + 参数 id（param_ids.h 的本地复刻），
 * 直接喂给 sloop_core_desc_track_live / sloop_core_desc（描述符单一来源）与状态层参数命令；
 * 槽位语义与鼓轨逐槽规则的权威来源 = docs/design/012-feature-parity-checklist.md §3。
 *
 * 成员表是**静态**的：某轨某引擎下实际存在的槽（引擎门槛、描述符取不到、min==max 且无
 * 枚举）由页面解析成「存在位图」，下标/布局函数只认这张位图，不认描述符。
 *
 * 本头文件与 sound_layout.c **不含 LVGL**：纯数据 + 纯函数，host 单测（scripts/sound-test.sh）
 * 与固件同源编译。页面在 page_sound.c。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sloop_core.h"

#define SOUND_CARDS_MAX     13
#define SOUND_SLOT_TABLE_MAX 109    /* 表内成员总数（含仅 ANALOG 的 P_FTYPE） */
#define SOUND_SLOT_NONE     0xFFFFu
typedef struct { uint8_t scope; uint8_t id; uint8_t gate; uint8_t drum_ok; } sound_slot_t;
/* scope = sloop_desc_scope_t；gate：0 = 总是，1 = 仅 ANALOG 引擎；drum_ok 同 param_slot_t */
typedef struct { const char *title; uint8_t slot_count; const sound_slot_t *slots; } sound_card_t;
extern const sound_card_t k_sound_cards[SOUND_CARDS_MAX];

#define SOUND_COL_W 616
#define SOUND_COL_GAP 16
#define SOUND_CARD_PAD 12
#define SOUND_TITLE_H 36
#define SOUND_CELL_W 140
#define SOUND_CELL_H 132
#define SOUND_KNOB_D 96
#define SOUND_RING_MIN_R 30
#define SOUND_RING_MAX_R 52

typedef struct { uint8_t card; int16_t col, y, h, rows; } sound_card_pos_t;

uint16_t sound_layout_slot_index(uint8_t card, uint8_t slot);          /* 越界 = SOUND_SLOT_NONE */
bool    sound_layout_slot_locate(uint16_t flat, uint8_t *card, uint8_t *slot);
/* 两列 staggered 布局，**内容坐标**（原点 = 列 0 / y 0，不含页边距）：折叠卡片高 = SOUND_TITLE_H，卡片间距 8；
 * 返回内容总高。页面坐标 = 内容坐标 + (16, 视口顶) − 滚动量，由调用方换算。 */
int16_t sound_layout_place(uint16_t collapsed, sound_card_pos_t out[SOUND_CARDS_MAX]);
/* 槽在内容坐标里的矩形（x = col*(616+16) + 12 + slot%4*140；y = 卡片 y + 36 + slot/4*132）。卡片折叠/槽越界返回 false。 */
bool    sound_layout_slot_rect(const sound_card_pos_t *pos, uint8_t slot,
                               int16_t *x, int16_t *y, int16_t *w, int16_t *h);
/* 存在位图（SOUND_SLOT_TABLE_MAX 位）上移动焦点：delta 可为 ±1；跳过不存在的槽与折叠卡片，环绕；无存在槽返回 false。 */
bool    sound_layout_flat_move(const uint32_t present[4], uint16_t collapsed, int16_t from, int8_t delta,
                               uint16_t *out);
/* 存在位图里下标最小的槽（不看卡片是否折叠；无存在槽返回 false）。 */
bool    sound_layout_first_present(const uint32_t present[4], uint16_t *out);
uint16_t sound_layout_present_count(const uint32_t present[4]);
bool    sound_ring_hit(int32_t dx, int32_t dy);                         /* 30 ≤ r ≤ 52 */
