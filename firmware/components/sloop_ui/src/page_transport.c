/*
 * TRANSPORT 页：PLAY/STOP 大按钮 + BPM 步进 + 步位置指示
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - PLAY 按钮 (48, 48) 320×180 轨绿；STOP 按钮 (400, 48) 320×180 轨橙；
 * - 状态文本  (48, 264) 28pt（PLAYING / STOPPED）；
 * - 步位置    (48, 320) 20pt，取快照 step_pos[selected] / len（停止时为 0）；
 * - BPM       (900, 40) 标题 + (880, 72) 数值 + (860/1000, 140) −/+ 按钮。
 *
 * 按钮回调发 EditCmd（CMD_PLAY/STOP、CMD_SET_BPM）走状态层 state_apply，
 * 不直接碰引擎；BPM 单步 ±1（长按连发按 brief 为本任务可选项，未做）。
 * 控件回调与 console `TAP` 注入都只调用 transport_{play,stop,bpm_step}()（唯一共同
 * 路径）；TAP 的命中矩形取自建按钮的同一组 TRANSPORT_* 几何宏。
 */
#include "theme.h"

#include <stdio.h>

#include "lvgl.h"
#include "sloop_state.h"

#define TRANSPORT_PLAY_X  48
#define TRANSPORT_PLAY_Y  48
#define TRANSPORT_PLAY_W  320
#define TRANSPORT_PLAY_H  180
#define TRANSPORT_STOP_X  400
#define TRANSPORT_STOP_Y  48
#define TRANSPORT_STOP_W  320
#define TRANSPORT_STOP_H  180
#define TRANSPORT_BPM_MINUS_X 860
#define TRANSPORT_BPM_PLUS_X  1000
#define TRANSPORT_BPM_Y       140
#define TRANSPORT_BPM_W       100
#define TRANSPORT_BPM_H       100

static lv_obj_t *s_status_label;
static lv_obj_t *s_step_label;
static lv_obj_t *s_bpm_value;

/* 脏检查缓存（-1 = 尚未刷新） */
static int8_t s_playing = -1;
static int16_t s_bpm = -1;
static int8_t s_step_track = -1;
static int16_t s_step_pos = -1;
static int16_t s_step_len = -1;

static lv_obj_t *make_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(label);
    return btn;
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_transport_update(&st);
}

/* 动作函数（LVGL 上下文、调用方已持锁）：控件点击回调与 TAP 注入的唯一共同路径。 */
static void transport_play(void)
{
    const edit_cmd_t cmd = {.type = CMD_PLAY};
    state_apply(&cmd);
    refresh();
}

static void transport_stop(void)
{
    const edit_cmd_t cmd = {.type = CMD_STOP};
    state_apply(&cmd);
    refresh();
}

static void transport_bpm_step(int delta)
{
    app_state_t st;
    state_get_copy(&st);
    int32_t bpm = (int32_t)st.core.bpm + delta;
    if (bpm < 0) bpm = 0; /* 范围钳位在 state_apply（40..240）内 */
    const edit_cmd_t cmd = {.type = CMD_SET_BPM, .v = (uint16_t)bpm};
    state_apply(&cmd);
    refresh();
}

static void play_clicked_cb(lv_event_t *e)
{
    (void)e;
    transport_play();
}

static void stop_clicked_cb(lv_event_t *e)
{
    (void)e;
    transport_stop();
}

static void bpm_clicked_cb(lv_event_t *e)
{
    transport_bpm_step((int)(intptr_t)lv_event_get_user_data(e));
}

/* TAP hit-test 表：坐标/尺寸取自上面建按钮的同一组宏（无第二份数字）。
 * 矩形互不重叠；命中后调用与控件回调相同的动作函数。 */
typedef struct {
    int16_t x, y, w, h;
    uint8_t kind; /* sloop_tap_kind_t */
    int8_t v;     /* BPM 步进方向 */
} transport_hit_t;

static const transport_hit_t k_hits[] = {
    {TRANSPORT_PLAY_X, TRANSPORT_PLAY_Y, TRANSPORT_PLAY_W, TRANSPORT_PLAY_H,
     SLOOP_TAP_TRANSPORT_PLAY, 0},
    {TRANSPORT_STOP_X, TRANSPORT_STOP_Y, TRANSPORT_STOP_W, TRANSPORT_STOP_H,
     SLOOP_TAP_TRANSPORT_STOP, 0},
    {TRANSPORT_BPM_MINUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W, TRANSPORT_BPM_H,
     SLOOP_TAP_TRANSPORT_BPM, -1},
    {TRANSPORT_BPM_PLUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W, TRANSPORT_BPM_H,
     SLOOP_TAP_TRANSPORT_BPM, 1},
};

bool page_transport_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (size_t i = 0; i < sizeof(k_hits) / sizeof(k_hits[0]); i++) {
        const transport_hit_t *hit = &k_hits[i];
        if (x < hit->x || x >= hit->x + hit->w || y < hit->y || y >= hit->y + hit->h) continue;
        switch (hit->kind) {
        case SLOOP_TAP_TRANSPORT_PLAY: transport_play(); break;
        case SLOOP_TAP_TRANSPORT_STOP: transport_stop(); break;
        case SLOOP_TAP_TRANSPORT_BPM: transport_bpm_step(hit->v); break;
        default: return false;
        }
        if (out != NULL) {
            out->kind = (sloop_tap_kind_t)hit->kind;
            out->v = hit->v;
        }
        return true;
    }
    return false;
}

void page_transport_create(lv_obj_t *parent)
{
    make_button(parent, TRANSPORT_PLAY_X, TRANSPORT_PLAY_Y, TRANSPORT_PLAY_W, TRANSPORT_PLAY_H,
                ui_track_color(1), "PLAY", play_clicked_cb, NULL);
    make_button(parent, TRANSPORT_STOP_X, TRANSPORT_STOP_Y, TRANSPORT_STOP_W, TRANSPORT_STOP_H,
                ui_track_color(3), "STOP", stop_clicked_cb, NULL);

    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, "STOPPED");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(s_status_label, 48, 264);

    s_step_label = lv_label_create(parent);
    lv_label_set_text(s_step_label, "TRACK 1   STEP 0 / 16");
    lv_obj_set_style_text_font(s_step_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_step_label, lv_color_hex(ui_track_color(0)), 0);
    lv_obj_set_pos(s_step_label, 48, 320);

    lv_obj_t *bpm_caption = lv_label_create(parent);
    lv_label_set_text(bpm_caption, "BPM");
    lv_obj_set_style_text_font(bpm_caption, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bpm_caption, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(bpm_caption, 900, 40);

    s_bpm_value = lv_label_create(parent);
    lv_label_set_text(s_bpm_value, "0");
    lv_obj_set_pos(s_bpm_value, 880, 72);
    lv_obj_set_size(s_bpm_value, 140, 40);
    lv_obj_set_style_text_align(s_bpm_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_bpm_value, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_bpm_value, lv_color_hex(UI_TEXT), 0);

    make_button(parent, TRANSPORT_BPM_MINUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W, TRANSPORT_BPM_H,
                UI_PANEL_HI, "-", bpm_clicked_cb, (void *)(intptr_t)-1);
    make_button(parent, TRANSPORT_BPM_PLUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W, TRANSPORT_BPM_H,
                UI_PANEL_HI, "+", bpm_clicked_cb, (void *)(intptr_t)1);
}

void page_transport_update(const app_state_t *st)
{
    if ((int8_t)st->core.playing != s_playing) {
        s_playing = (int8_t)st->core.playing;
        lv_label_set_text(s_status_label, s_playing ? "PLAYING" : "STOPPED");
        lv_obj_set_style_text_color(s_status_label,
                                    lv_color_hex(s_playing ? UI_TEXT : UI_TEXT_DIM), 0);
    }

    if ((int16_t)st->core.bpm != s_bpm) {
        s_bpm = (int16_t)st->core.bpm;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)s_bpm);
        lv_label_set_text(s_bpm_value, buf);
    }

    uint8_t k = st->core.selected_track;
    if (k >= APP_STATE_TRACKS) k = 0;
    uint8_t pos = st->core.step_pos[k];
    uint8_t len = st->core.trk[k].len;
    if ((int8_t)k != s_step_track || (int16_t)pos != s_step_pos || (int16_t)len != s_step_len) {
        s_step_track = (int8_t)k;
        s_step_pos = (int16_t)pos;
        s_step_len = (int16_t)len;
        char buf[32];
        snprintf(buf, sizeof(buf), "TRACK %u   STEP %u / %u", (unsigned)(k + 1), (unsigned)pos,
                 (unsigned)len);
        lv_label_set_text(s_step_label, buf);
        lv_obj_set_style_text_color(s_step_label, lv_color_hex(ui_track_color(k)), 0);
    }
}
