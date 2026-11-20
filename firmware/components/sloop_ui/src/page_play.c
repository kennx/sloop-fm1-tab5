/*
 * PLAY 页：当前选轨的 engine/preset 数字 + 电平条 + 音符触发指示
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - TRACK n       (48, 24)  28pt，轨主题色；
 * - ENGINE/PRESET (48, 74) 20pt，次级色；
 * - LEVEL 标题    (48, 164) 16pt + 数值 (864, 196)；
 * - 电平条        (48, 192) 800×36，indicator 用轨主题色（snapshot trk.level 0..127）；
 * - 音符指示      (940, 48) 300×300 圆角盒，默认 "--"/暗色；sloop_ui_flash_note()
 *   点亮为轨主题色并显示音名，120ms 后由 UI 定时器熄灭（s_flash_tick 计龄）。
 *
 * MVP 不做引擎/预设名称表（spec §3.3），显示快照数字；鼓轨（track 3）无引擎与
 * 预设（快照原值不可用），显示 "DRUM KIT"。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>

#include "lvgl.h"
#include "sloop_state.h"

#define FLASH_HOLD_MS 120

static lv_obj_t *s_track_label;
static lv_obj_t *s_engine_label;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_level_value;
static lv_obj_t *s_flash_box;
static lv_obj_t *s_flash_label;

/* 脏检查缓存（-1 = 尚未刷新） */
static int8_t s_track = -1;
static int16_t s_engine = -1;
static int16_t s_preset = -1;
static int16_t s_level = -1;

static uint32_t s_flash_tick;
static bool s_flash_active;

static void note_name(uint8_t note, char *buf, size_t len)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", names[note % 12], (int)(note / 12) - 1);
}

static void flash_clear(void)
{
    lv_label_set_text(s_flash_label, "--");
    lv_obj_set_style_text_color(s_flash_label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_bg_color(s_flash_box, lv_color_hex(UI_PANEL), 0);
    s_flash_active = false;
}

void page_play_create(lv_obj_t *parent)
{
    s_track_label = lv_label_create(parent);
    lv_label_set_text(s_track_label, "TRACK 1");
    lv_obj_set_style_text_font(s_track_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_track_label, lv_color_hex(ui_track_color(0)), 0);
    lv_obj_set_pos(s_track_label, 48, 24);

    s_engine_label = lv_label_create(parent);
    lv_label_set_text(s_engine_label, "ENGINE 0   PRESET 0");
    lv_obj_set_style_text_font(s_engine_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_engine_label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(s_engine_label, 48, 74);

    lv_obj_t *level_caption = lv_label_create(parent);
    lv_label_set_text(level_caption, "LEVEL");
    lv_obj_set_style_text_font(level_caption, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(level_caption, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(level_caption, 48, 164);

    s_level_bar = lv_bar_create(parent);
    lv_obj_set_pos(s_level_bar, 48, 192);
    lv_obj_set_size(s_level_bar, 800, 36);
    lv_bar_set_range(s_level_bar, 0, 127);
    lv_bar_set_value(s_level_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(UI_PANEL_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_level_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(ui_track_color(0)), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_level_bar, LV_OPA_COVER, LV_PART_INDICATOR);

    s_level_value = lv_label_create(parent);
    lv_label_set_text(s_level_value, "0");
    lv_obj_set_style_text_font(s_level_value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_level_value, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_pos(s_level_value, 864, 196);

    s_flash_box = lv_obj_create(parent);
    lv_obj_set_scrollable(s_flash_box, false);
    lv_obj_set_pos(s_flash_box, 940, 48);
    lv_obj_set_size(s_flash_box, 300, 300);
    lv_obj_set_style_radius(s_flash_box, 24, 0);
    lv_obj_set_style_bg_color(s_flash_box, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_flash_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_flash_box, 2, 0);
    lv_obj_set_style_border_color(s_flash_box, lv_color_hex(UI_PANEL_HI), 0);
    lv_obj_set_style_pad_all(s_flash_box, 0, 0);

    s_flash_label = lv_label_create(s_flash_box);
    lv_label_set_text(s_flash_label, "--");
    lv_obj_set_style_text_font(s_flash_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_flash_label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_center(s_flash_label);

    lv_obj_t *note_caption = lv_label_create(parent);
    lv_label_set_text(note_caption, "NOTE");
    lv_obj_set_width(note_caption, 300);
    lv_obj_set_style_text_align(note_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(note_caption, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(note_caption, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(note_caption, 940, 356);
}

void page_play_update(const app_state_t *st)
{
    uint8_t k = st->core.selected_track;
    if (k >= APP_STATE_TRACKS) k = 0;

    if ((int8_t)k != s_track) {
        s_track = (int8_t)k;
        char buf[16];
        snprintf(buf, sizeof(buf), "TRACK %u", (unsigned)(k + 1));
        lv_label_set_text(s_track_label, buf);
        lv_obj_set_style_text_color(s_track_label, lv_color_hex(ui_track_color(k)), 0);
        lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(ui_track_color(k)), LV_PART_INDICATOR);
        s_engine = -2; /* 轨变了，强制刷新 engine/preset 文本 */
    }

    int16_t engine = (k == APP_STATE_TRACKS - 1) ? -1 : st->core.trk[k].engine; /* 鼓轨无引擎 */
    int16_t preset = (k == APP_STATE_TRACKS - 1) ? -1 : st->core.trk[k].preset;
    if (engine != s_engine || preset != s_preset) {
        s_engine = engine;
        s_preset = preset;
        char buf[32];
        if (engine < 0) {
            lv_label_set_text(s_engine_label, "DRUM KIT / STEP TRACK");
        } else {
            snprintf(buf, sizeof(buf), "ENGINE %d   PRESET %d", (int)engine, (int)preset);
            lv_label_set_text(s_engine_label, buf);
        }
    }

    if ((int16_t)st->core.trk[k].level != s_level) {
        s_level = st->core.trk[k].level;
        lv_bar_set_value(s_level_bar, s_level, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)s_level);
        lv_label_set_text(s_level_value, buf);
    }

    if (s_flash_active && lv_tick_elaps(s_flash_tick) >= FLASH_HOLD_MS) {
        flash_clear();
    }
}

void page_play_flash(uint8_t note)
{
    if (s_flash_box == NULL) return;
    char buf[8];
    note_name(note, buf, sizeof(buf));
    lv_label_set_text(s_flash_label, buf);
    lv_obj_set_style_text_color(s_flash_label, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_color(s_flash_box,
                              lv_color_hex(ui_track_color(s_track < 0 ? 0 : (uint8_t)s_track)), 0);
    s_flash_tick = lv_tick_get();
    s_flash_active = true;
}
