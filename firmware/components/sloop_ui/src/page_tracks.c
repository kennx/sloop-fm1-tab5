/*
 * TRACKS 页：四轨卡片横排（主题色描边），点选切轨
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 卡片 k   x = 24 + k*312, y = 40, 296×470；未选中 描边 3px + UI_PANEL 底，
 *   选中 描边 6px + UI_PANEL_SEL 底（这就是选中态，不另设文字标记）；
 *   描边用 outline（画在盒外）而不是 border：border 增宽会挪动卡内子对象的位置，
 *   选中/取消时文字会跳 3px。
 * - 卡内：TRACK n (24,26) 28pt 轨色 / 信息两行 (24,84) 20pt
 *   （"ENG k   PRE p" + "LEVEL v"，鼓轨第一行改 "DRUM KIT"）/ 电平条 (24,150)
 *   248×16 轨色 indicator / "MUTE x   SOLO y" (24,250)。
 *
 * 点击卡片发 CMD_SELECT_TRACK（cmd.c 的 cmd_select_track 同时写 selected_track 与
 * focus_track，即「设焦点轨 + 选轨」一次完成，本页不再重复 state_set_focus）。
 * 控件回调与 console `TAP` 注入都只调用 tracks_select()（唯一共同路径），TAP 的
 * 命中矩形由建卡同一组布局宏推导（无第二份坐标表）。
 *
 * 内存：文本/底色用静态共享 lv_style_t（与 page_step.c 同一纪律）；仍保留局部样式的只有
 * 四张卡的 pad_all 与四条电平条（main/indicator 的底色/圆角），量级固定且很小。
 * LVGL 池 64KB 在内置 RAM；每卡 4 个子对象（信息/编组文本合并）是把池占用压回
 * 安全区的必要取舍。脏检查：每卡缓存 (sel,engine,preset,level,mute,solo) 六字节，
 * memcmp 不等才重设文本/样式/电平条（每 30ms 无条件重设会让 LVGL 持续失效重绘）。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "sloop_state.h"

#define CARD_X0 24
#define CARD_Y  40
#define CARD_W  296
#define CARD_H  470
#define CARD_PITCH (CARD_W + 16)

typedef struct {
    uint8_t sel;
    uint8_t engine, preset;
    uint8_t level, mute, solo;
} card_view_t;

static lv_obj_t *s_cards[APP_STATE_TRACKS];
static lv_obj_t *s_info_labels[APP_STATE_TRACKS];
static lv_obj_t *s_mix_labels[APP_STATE_TRACKS];
static lv_obj_t *s_level_bars[APP_STATE_TRACKS];

static lv_style_t s_st_card[APP_STATE_TRACKS];
static lv_style_t s_st_card_sel;
static lv_style_t s_st_title[APP_STATE_TRACKS];
static lv_style_t s_st_value;

static card_view_t s_cache[APP_STATE_TRACKS];
static bool s_valid;

static void styles_init(void)
{
    /* 只初始化一次（R34/ADR-008 §3）：组作用域生命周期会反复销毁/重建本页，
     * `lv_style_init` 是 memzero——重跑会丢掉静态样式已分配的 props 数组
     * （`lv_style_set_prop` 走 lv_realloc），每次跨组往返漏约 300B LVGL 池。 */
    static bool done;
    if (done) return;
    done = true;

    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        lv_style_init(&s_st_card[k]);
        lv_style_set_radius(&s_st_card[k], 20);
        lv_style_set_bg_opa(&s_st_card[k], LV_OPA_COVER);
        lv_style_set_bg_color(&s_st_card[k], lv_color_hex(UI_PANEL));
        lv_style_set_border_width(&s_st_card[k], 0);
        lv_style_set_outline_color(&s_st_card[k], lv_color_hex(ui_track_color(k)));
        lv_style_set_outline_opa(&s_st_card[k], LV_OPA_COVER);
        lv_style_set_outline_pad(&s_st_card[k], 0);
        lv_style_set_outline_width(&s_st_card[k], 3);

        lv_style_init(&s_st_title[k]);
        lv_style_set_text_font(&s_st_title[k], &lv_font_montserrat_28);
        lv_style_set_text_color(&s_st_title[k], lv_color_hex(ui_track_color(k)));
    }
    /* 选中态叠在卡基样式上：只盖 outline 宽度与底色，描边色沿用各轨 */
    lv_style_init(&s_st_card_sel);
    lv_style_set_bg_color(&s_st_card_sel, lv_color_hex(UI_PANEL_SEL));
    lv_style_set_outline_width(&s_st_card_sel, 6);

    lv_style_init(&s_st_value);
    lv_style_set_text_font(&s_st_value, &lv_font_montserrat_20);
    lv_style_set_text_color(&s_st_value, lv_color_hex(UI_TEXT));
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_tracks_update(&st);
}

/* 卡选中（LVGL 上下文、调用方已持锁）：控件回调与 TAP 注入的唯一共同路径。 */
static void tracks_select(uint8_t k)
{
    const edit_cmd_t cmd = {.type = CMD_SELECT_TRACK, .a = k};
    state_apply(&cmd); /* 选轨 + focus_track（见 cmd.c cmd_select_track） */
    refresh();
}

static void card_clicked_cb(lv_event_t *e)
{
    tracks_select((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

/* TAP hit-test：矩形由建卡所用的同一组宏（CARD_X0/Y/W/H/PITCH）推导。 */
bool page_tracks_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (y < CARD_Y || y >= CARD_Y + CARD_H) return false;
    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        int32_t x0 = CARD_X0 + k * CARD_PITCH;
        if (x < x0 || x >= x0 + CARD_W) continue;
        tracks_select(k);
        if (out != NULL) {
            out->kind = SLOOP_TAP_TRACK_CARD;
            out->a = k;
        }
        return true;
    }
    return false;
}

static void card_apply(uint8_t k, const card_view_t *cv, uint8_t prev_sel)
{
    bool sel = cv->sel != 0;
    if (sel != (prev_sel != 0)) {
        if (sel) lv_obj_add_style(s_cards[k], &s_st_card_sel, 0);
        else lv_obj_remove_style(s_cards[k], &s_st_card_sel, 0);
    }

    char buf[48];
    if (k == APP_STATE_TRACKS - 1) {
        snprintf(buf, sizeof(buf), "DRUM KIT\nLEVEL %u", (unsigned)cv->level);
    } else {
        snprintf(buf, sizeof(buf), "ENG %u   PRE %u\nLEVEL %u", (unsigned)cv->engine,
                 (unsigned)cv->preset, (unsigned)cv->level);
    }
    lv_label_set_text(s_info_labels[k], buf);
    lv_bar_set_value(s_level_bars[k], cv->level, LV_ANIM_OFF);

    snprintf(buf, sizeof(buf), "MUTE %s   SOLO %s", cv->mute ? "ON" : "OFF",
             cv->solo ? "ON" : "OFF");
    lv_label_set_text(s_mix_labels[k], buf);
}

lv_obj_t *page_tracks_build(lv_obj_t *parent)
{
    styles_init();
    /* 懒建重试时控件是全新的：复位脏检查缓存 */
    memset(s_cache, 0, sizeof(s_cache));
    s_valid = false;

    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        lv_obj_t *card = lv_obj_create(parent);
        UI_BUILD_CHECK(card);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_pos(card, CARD_X0 + k * CARD_PITCH, CARD_Y);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_add_style(card, &s_st_card[k], 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)k);
        s_cards[k] = card;

        char buf[16];
        snprintf(buf, sizeof(buf), "TRACK %u", (unsigned)(k + 1));
        lv_obj_t *title = lv_label_create(card);
        UI_BUILD_CHECK(title);
        lv_label_set_text(title, buf);
        lv_obj_add_style(title, &s_st_title[k], 0);
        lv_obj_set_pos(title, 24, 26);

        s_info_labels[k] = lv_label_create(card);
        UI_BUILD_CHECK(s_info_labels[k]);
        lv_label_set_text(s_info_labels[k], "");
        lv_obj_add_style(s_info_labels[k], &s_st_value, 0);
        lv_obj_set_pos(s_info_labels[k], 24, 84);

        s_level_bars[k] = lv_bar_create(card);
        UI_BUILD_CHECK(s_level_bars[k]);
        lv_obj_set_pos(s_level_bars[k], 24, 150);
        lv_obj_set_size(s_level_bars[k], 248, 16);
        lv_bar_set_range(s_level_bars[k], 0, 127);
        lv_bar_set_value(s_level_bars[k], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_level_bars[k], lv_color_hex(UI_PANEL_HI), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_level_bars[k], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_level_bars[k], 6, LV_PART_MAIN);
        lv_obj_set_style_radius(s_level_bars[k], 6, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_level_bars[k], lv_color_hex(ui_track_color(k)), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_level_bars[k], LV_OPA_COVER, LV_PART_INDICATOR);

        s_mix_labels[k] = lv_label_create(card);
        UI_BUILD_CHECK(s_mix_labels[k]);
        lv_label_set_text(s_mix_labels[k], "");
        lv_obj_add_style(s_mix_labels[k], &s_st_value, 0);
        lv_obj_set_pos(s_mix_labels[k], 24, 250);
    }
    return parent;
}

void page_tracks_update(const app_state_t *st)
{
    uint8_t sel = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0;
    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        card_view_t cv = {
            .sel = (uint8_t)(k == sel),
            .engine = st->core.trk[k].engine,
            .preset = st->core.trk[k].preset,
            .level = st->core.trk[k].level,
            .mute = st->core.trk[k].mute,
            .solo = st->core.trk[k].solo,
        };
        if (s_valid && memcmp(&cv, &s_cache[k], sizeof(cv)) == 0) continue;
        card_apply(k, &cv, s_cache[k].sel);
        s_cache[k] = cv;
    }
    s_valid = true;
}
