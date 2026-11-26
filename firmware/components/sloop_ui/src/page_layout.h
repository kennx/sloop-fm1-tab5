/*
 * 参数页策展布局表（sloop_ui 内部头文件；ADR-008 §8）。
 *
 * 「页 = 参数组」是 **UI 关注点**：上游 params.c 的 page_t PAGES[] 只作为语义/观感
 * 参照（ENV/LFO/FX/GLOBAL/MASTER…），本表按 8 槽重新策展（映射而非逐像素复刻）。
 * 槽位 {scope, engine, id, drum_ok} 直接喂给 sloop_core_desc / sloop_core_desc_track_live
 * （描述符单一来源）与状态层参数命令；页 id/title/est 是页面注册表条目（ui_page_t）的
 * 元数据。槽表的权威来源 = docs/design/012-feature-parity-checklist.md §3（5B-1 起全局/
 * 效果/每轨族参数的槽位表在 sound_layout.h，由 SOUND 仪表盘的卡片承载；本表只剩 SOUND
 * 与 SAMPLE 两页）。
 *
 * 本头文件与 page_layout.c **不含 LVGL**：纯数据 + 纯规则，host 单测（scripts/knob-test.sh）
 * 与固件同源编译。注册表胶水（build/update 包装、ui_page_t 条目）在 page_params.c。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sloop_core.h"
#include "sloop_state.h"

#define PARAM_PAGE_SLOTS 8
/* 每页 chip 数上限。表内两页都是无名整页（只用 chips[0]）；5B-1 退役 MOD/SCALE/VOICE/
 * GLOBAL/FX 后没有 chip 页，多 chip 槽位与下面两条规则（条件 chip、鼓轨逐槽 drum_ok）
 * 留作框架——chip 行的建控件/TAP/console `UI <page> CHIP` 路径仍在 page_params.c。 */
#define PARAM_PAGE_CHIPS 5

/* 条件 chip：chip 只在谓词成立时出现（隐藏时不占 chip 行、不参与生效 chip 的下标解析）。 */
typedef enum {
    PARAM_CHIP_ALWAYS = 0,
    PARAM_CHIP_WHEN_ANALOG, /* 仅选中轨是 ANALOG 引擎（0）的合成轨（上游 page_shown） */
} param_chip_when_t;

/* 槽位：scope = sloop_desc_scope_t。GLOBAL 用全局 id（GP[]）；TRACK 用轨道参数 id
 * （id < P_E0，描述符/值都取**选中轨**）；ENGINE 用引擎槽 id 0..7（= P_E0..P_E7 的
 * 相对号，engine 为页声明的引擎号）。
 * drum_ok：选中鼓轨时该槽是否可编辑（012 §3「鼓」列）——0 = 显示 -- 且拒绝编辑。 */
typedef struct {
    uint8_t scope;
    uint8_t engine;
    uint8_t id;
    uint8_t drum_ok;
} param_slot_t;

typedef struct {
    const char *name; /* NULL = 旧式整页（无 chip 行，8 槽即整页） */
    uint8_t when;     /* param_chip_when_t */
    uint8_t slot_count;
    param_slot_t slots[PARAM_PAGE_SLOTS]; /* slot_count 之外零填充、不读 */
} param_chip_t;

typedef struct {
    uint8_t page_id; /* app_page_t */
    const char *title;
    uint32_t est_bytes;
    uint8_t chip_count; /* 有名 chip 数；0 = 旧式整页（生效 chip = chips[0]） */
    uint8_t features;   /* PARAM_FEAT_* 位掩码：页眉/底部的附加行 */
    param_chip_t chips[PARAM_PAGE_CHIPS];
} param_page_t;

/* 页的附加行（P4-F Task 5；位掩码。建控件、TAP 命中表与可见性规则都读这几位，
 * page_params.c 不再按页 id 例外）。 */
#define PARAM_FEAT_SOUND    0x01u /* SOUND 页：页眉 ENGINE/PRESET 行（鼓轨 = 单行 KIT），
                                   * 主体 = 选中轨引擎的 8 槽（TRACK scope 的 P_E0..P_E7） */
#define PARAM_FEAT_BANK_FM6 0x02u /* 底部 bank 存取行（B1..B27 / SAVE / ERASE）：**仅**选中轨
                                   * 为 FM6 引擎的合成轨时显示（A10）；其余引擎整行隐藏、
                                   * TAP 落在该带 ⇒ miss */
#define PARAM_FEAT_SAMPLE   0x04u /* 底部采样槽管理行（SAMPLE 页，P4-D Task 8） */
#define PARAM_FEAT_DASHBOARD 0x08u /* 卡片仪表盘页（SOUND，5B-1）：主体 = page_sound.c 的滚动卡片
                                   * 区——**没有** 8 槽旋钮与 chip 行；页眉行、银行行、± 按钮仍由
                                   * page_params.c 建（按钮下移到 SOUND_BTN_Y 让出页眉行） */

/* 参数页数（布局表条目数；页 id 不要求连续）。APP_PAGE_FM6 与 5B-1 退役的 GLOBAL/FX/
 * MOD/SCALE/VOICE 是不注册的 id：page_layout_index 对它们返回 -1。 */
#define PARAM_PAGE_COUNT 2

/* 把布局表里的所有页注册进页面注册表（sloop_ui_start 内置注册之后调用；幂等由
 * ui.c 的内置注册标志保证）。返回第一个非 ESP_OK 的错误，全成功返回 ESP_OK。
 * 实现在 page_params.c（需要 ui_page_t/LVGL；本表与访问器保持无 LVGL、host 可编译）。 */
esp_err_t page_layout_register(void);

/* 按布局表下标取页定义；越界返回 NULL。 */
const param_page_t *page_layout_get(uint8_t index);
/* 页 id（app_page_t）→ 布局表下标；非参数页返回 -1。 */
int page_layout_index(uint8_t page_id);
static inline uint32_t page_layout_est(uint8_t index)
{
    const param_page_t *pg = page_layout_get(index);
    return pg != NULL ? pg->est_bytes : 0u;
}

/* 生效 chip 数：chip 页 = chip_count；旧式整页 = 1（chips[0]）。 */
static inline uint8_t page_layout_chip_count(const param_page_t *pg)
{
    return pg != NULL && pg->chip_count != 0 ? pg->chip_count : 1u;
}
/* chip 下标 → 定义；越界（含旧式页的下标 > 0）返回 NULL。 */
static inline const param_chip_t *page_layout_chip(const param_page_t *pg, uint8_t chip)
{
    if (pg == NULL || chip >= page_layout_chip_count(pg)) return NULL;
    return &pg->chips[chip];
}
/* chip 内第 slot 槽；slot ≥ slot_count 或空槽（id 0xFF，旧式页的占位）返回 NULL。 */
static inline const param_slot_t *page_layout_slot(const param_chip_t *chip, uint8_t slot)
{
    if (chip == NULL || slot >= chip->slot_count || slot >= PARAM_PAGE_SLOTS) return NULL;
    const param_slot_t *s = &chip->slots[slot];
    return s->id == 0xFF ? NULL : s;
}

/* chip 行几何：16pt Montserrat 大写 ≈ 11px/字符 + 左右留白（页签同一 10px 圆角）。
 * 建控件与 TAP 命中同源（page_params.c）。 */
static inline int16_t page_layout_chip_width(const char *name)
{
    uint32_t n = 0;
    if (name != NULL) {
        while (name[n] != '\0') n++;
    }
    return (int16_t)(n * 11u + 16u);
}

/* 条件 chip 是否出现：drum = 选中鼓轨；engine = 选中轨引擎（鼓轨传任意值）。 */
static inline bool page_layout_chip_visible(const param_chip_t *chip, bool drum, uint8_t engine)
{
    if (chip == NULL) return false;
    if (chip->when == PARAM_CHIP_WHEN_ANALOG) return !drum && engine == 0u;
    return true;
}
/* 给定上下文下的可见 chip 数 / 第 n 个可见 chip 的下标（n 从 0 起；不存在返回 -1）。
 * 隐藏 chip 不塌陷下标：可见序号 ≠ chip 下标。 */
static inline uint8_t page_layout_visible_chips(const param_page_t *pg, bool drum, uint8_t engine)
{
    uint8_t n = 0;
    for (uint8_t c = 0; c < page_layout_chip_count(pg); c++) {
        if (page_layout_chip_visible(&pg->chips[c], drum, engine)) n++;
    }
    return n;
}
static inline int page_layout_visible_chip_at(const param_page_t *pg, uint8_t n, bool drum,
                                              uint8_t engine)
{
    for (uint8_t c = 0; c < page_layout_chip_count(pg); c++) {
        if (!page_layout_chip_visible(&pg->chips[c], drum, engine)) continue;
        if (n == 0) return (int)c;
        n--;
    }
    return -1;
}

/* ---- SOUND 页（P4-F Task 5，spec §4 / A10）-------------------------------------------
 * 页眉行是**三区对象**（同 DSYN 的 LANE / FM6P 的 BANK：左右各 SOUND_ROW_ZONE 像素是
 * ∓1 区、中段是标签）。合成轨两行：row 0 = ENGINE、row 1 = PRESET；鼓轨一行：row 0 = KIT
 * （写鼓轨的 P_E0，上游 DRUMS 屏 KNOB 1）。几何是纯数据：建控件、自绘与 TAP 命中同源。
 * 中段的列：`−` 居中于左区；行名（ENGINE/PRESET/KIT）左对齐起于 SOUND_ROW_TEXT_X；名字
 * 右对齐止于行右缘 − SOUND_ROW_ZONE − 12（page_layout_sound_name_x 起的列宽须放得下上游
 * 最长的名字：引擎 7 / 预置 10 / kit 8 字符，按 11px/字符的保守上界，host 单测钉住）。 */
#define SOUND_ROW_Y     4
#define SOUND_ROW_H     32
#define SOUND_ROW_ZONE  42
#define SOUND_ROW_TEXT_X 54   /* = 区宽 42 + 12 */
#define SOUND_ROW_NAME_R 54   /* 名字列右缘 = 行右缘 − 54（区宽 42 + 12） */
static inline int16_t page_layout_sound_row_x(uint8_t row)
{
    return row == 0u ? 16 : 320;
}
static inline int16_t page_layout_sound_row_w(uint8_t row)
{
    return row == 0u ? 288 : 384;
}
/* 行名长度（`ENGINE` / `PRESET` / `KIT`）与名字列的左缘（对象内相对坐标） */
static inline uint8_t page_layout_sound_row_label_len(uint8_t row, bool drum)
{
    return row == 0u ? (drum ? 3u : 6u) : 6u;
}
static inline int16_t page_layout_sound_name_x(uint8_t row, bool drum)
{
    return (int16_t)(SOUND_ROW_TEXT_X + 11 * page_layout_sound_row_label_len(row, drum) + 8);
}
/* 页眉行数：合成轨 2（ENGINE + PRESET）/ 鼓轨 1（KIT 取代两行）。 */
static inline uint8_t page_layout_sound_rows(bool drum)
{
    return drum ? 1u : 2u;
}
/* 页眉上下文行（`SOUND · TRACK n`）的左缘 = 最后一行右侧 + 16。 */
static inline int16_t page_layout_sound_header_x(bool drum)
{
    uint8_t last = (uint8_t)(page_layout_sound_rows(drum) - 1u);
    return (int16_t)(page_layout_sound_row_x(last) + page_layout_sound_row_w(last) + 16);
}
static inline bool page_layout_is_sound(const param_page_t *pg)
{
    return pg != NULL && (pg->features & PARAM_FEAT_SOUND) != 0u;
}
/* 底部 bank 行是否显示：带该行的页 + 合成轨 + 引擎为 FM6（上游 engines.c ENGINES[9]）。
 * 鼓轨与其余引擎 ⇒ 整行隐藏（对象常驻，零分配显隐）。 */
#define PARAM_ENGINE_FM6 9u
static inline bool page_layout_bank_row_visible(const param_page_t *pg, bool drum, uint8_t engine)
{
    return pg != NULL && (pg->features & PARAM_FEAT_BANK_FM6) != 0u && !drum &&
           engine == PARAM_ENGINE_FM6;
}

static inline bool page_layout_is_dashboard(const param_page_t *pg)
{
    return pg != NULL && (pg->features & PARAM_FEAT_DASHBOARD) != 0u;
}
/* SOUND 仪表盘（5B-1）的固定行与卡片视口（页面区相对坐标）：页眉行 y=4..36，下方一行提示/
 * ± 按钮 y=40..80（按钮 x 同各参数页的 BTN_DEC_X/BTN_INC_X）；银行行在仪表盘页挪到页眉
 * 下方 SOUND_BANK_ROW_Y（= page_params.c 的 BANK_ROW_Y_DASH，仅 FM6 合成轨显示），卡片区视口
 * 顶随它下移。建控件、卡片自绘与 TAP 命中同源。 */
#define SOUND_BTN_Y        40
#define SOUND_BANK_ROW_Y   76
#define SOUND_BANK_ROW_H   44
#define SOUND_VIEW_X       16
#define SOUND_VIEW_W       1248
#define SOUND_VIEW_BOTTOM  644   /* 页面区高 648 − 底留白 4 */
#define SOUND_VIEW_Y       80    /* 银行行隐藏时的视口顶 */
#define SOUND_VIEW_Y_BANK  (SOUND_BANK_ROW_Y + SOUND_BANK_ROW_H + 8) /* 128 */
static inline int16_t page_layout_sound_view_top(bool bank_visible)
{
    return bank_visible ? SOUND_VIEW_Y_BANK : SOUND_VIEW_Y;
}
static inline int16_t page_layout_sound_view_h(bool bank_visible)
{
    return (int16_t)(SOUND_VIEW_BOTTOM - page_layout_sound_view_top(bank_visible));
}

/* 鼓轨逐槽规则（012 §3）：全局参数与标了 drum_ok 的槽在鼓轨上仍可编辑，其余槽锁定。 */
static inline bool page_layout_slot_locked(const param_slot_t *slot, bool drum)
{
    if (slot == NULL) return true;
    return drum && slot->scope != SLOOP_DESC_GLOBAL && slot->drum_ok == 0u;
}
