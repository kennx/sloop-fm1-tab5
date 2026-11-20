/*
 * STEP 页：4 行 × 16 步网格（行 = 轨），点格切换有/无音，播放头高亮
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 顶栏 "TRACK n   STEP i   LEN l   DIV d"  (24, 14) 20pt 焦点轨色；
 * - 右上角状态行（点格后的反馈：T1 S2 / DRUM EDIT = PHASE 4）右对齐 (656, 14) 宽 600；
 * - 行标签 T1/T2/T3/DRM (24, 84 + r*124 + 42) 20pt 轨色；
 * - 格子 (r,c)：x = 88 + c*73, y = 84 + r*124, 67×104，圆角 10；
 * - 列号只标 4 的倍数（4/8/12/16），各标在其所在列格的左侧 (x+24, 568) 16pt；
 * - 底部提示 (24, 606) 16pt。
 *
 * 渲染口径：格「亮」= state_step_is_on()（n>0 且 time==NOTE，与状态层切换同一判定，
 * TIE/REST 的步不算亮）；鼓轨（第 4 行）只展示引擎快照 dstep[i].on 的聚合，点击
 * 不发 CMD_TOGGLE_STEP、也不改焦点（焦点轨由合成轨行决定），只写状态行
 * "DRUM EDIT = PHASE 4"。
 * 高亮：播放中当前步（step_pos[r]，仅当 <16 列）描边 UI_ACCENT 5px；焦点格
 * （focus_track/focus_step）描边 UI_TEXT 3px；两者重叠时播放头优先。
 *
 * 点击：合成轨先 state_set_focus()（UI 局部态）再 state_apply(CMD_TOGGLE_STEP)，
 * 随后立即刷新本页一次（不等 30ms 定时器，切页/截屏不受 tick 时序影响）。
 *
 * 内存：75 个格/标签对象共用静态 lv_style_t（格底 5 种 + 高亮 3 种 + 文本 6 种），
 * 每对象一份局部样式会把 LVGL 池（约 58KB）吃穿；状态变化用 lv_obj_replace_style
 * 换共享样式指针，不分配新样式。仍用局部样式的只有顶栏/状态行两个单例标签
 * （改色/对齐/宽度），量级固定。脏检查：每格缓存 1 字节（bit0 亮 / bit1..2 高亮
 * 种类），变化才换样式。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"
#include "sloop_state.h"

static const char *TAG = "sloop_ui";

#define GRID_X0     88
#define GRID_Y0     84
#define CELL_W      67
#define CELL_H      104
#define CELL_STEP_X 73 /* 列距 = CELL_W + 6 */
#define ROW_PITCH   124 /* 行距 = CELL_H + 20 */
#define CELL_RADIUS 10

#define HL_NONE     0u
#define HL_PLAYHEAD 1u
#define HL_FOCUS    2u

static lv_obj_t *s_cells[APP_STATE_TRACKS][APP_STATE_STEPS];
static uint8_t s_cell_state[APP_STATE_TRACKS][APP_STATE_STEPS];
static lv_obj_t *s_header;
static lv_obj_t *s_status;

static lv_style_t s_st_cell_off;
static lv_style_t s_st_cell_on[APP_STATE_TRACKS];
static lv_style_t s_st_hl_none;
static lv_style_t s_st_hl_playhead;
static lv_style_t s_st_hl_focus;
static lv_style_t s_st_row[APP_STATE_TRACKS];
static lv_style_t s_st_num;
static lv_style_t s_st_hint;

static struct {
    uint8_t track, step, len, div;
} s_header_cache;
/* 首帧强制刷新：缓存零值可能与 create 时的初始文案一致（如 len/div 恰好为 0），
 * 不强制的话首个 tick 可能跳过刷新、把硬编码文案留在屏上。 */
static bool s_header_dirty = true;

static void cell_shape_style(lv_style_t *st, uint32_t bg)
{
    lv_style_init(st);
    lv_style_set_radius(st, CELL_RADIUS);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, lv_color_hex(bg));
    lv_style_set_border_opa(st, LV_OPA_COVER);
    lv_style_set_border_width(st, 0);
}

static void border_style(lv_style_t *st, uint32_t color, int32_t width)
{
    lv_style_init(st);
    lv_style_set_border_opa(st, LV_OPA_COVER);
    lv_style_set_border_color(st, lv_color_hex(color));
    lv_style_set_border_width(st, width);
}

static void styles_init(void)
{
    cell_shape_style(&s_st_cell_off, UI_PANEL_HI);
    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        cell_shape_style(&s_st_cell_on[k], ui_track_color(k));
    }
    border_style(&s_st_hl_none, UI_BG, 0); /* 不画边框；基样式已是 0，保留换样式空位 */
    border_style(&s_st_hl_playhead, UI_ACCENT, 5);
    border_style(&s_st_hl_focus, UI_TEXT, 3);

    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        lv_style_init(&s_st_row[k]);
        lv_style_set_text_font(&s_st_row[k], &lv_font_montserrat_20);
        lv_style_set_text_color(&s_st_row[k], lv_color_hex(ui_track_color(k)));
    }
    lv_style_init(&s_st_num);
    lv_style_set_text_font(&s_st_num, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_num, lv_color_hex(UI_TEXT_DIM));
    lv_style_init(&s_st_hint);
    lv_style_set_text_font(&s_st_hint, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_hint, lv_color_hex(UI_TEXT_DIM));
}

static lv_style_t *hl_style(uint8_t hl)
{
    if (hl == HL_PLAYHEAD) return &s_st_hl_playhead;
    if (hl == HL_FOCUS) return &s_st_hl_focus;
    return &s_st_hl_none;
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_step_update(&st);
}

static void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

static void cell_clicked_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t r = (uint8_t)(id / APP_STATE_STEPS);
    uint8_t c = (uint8_t)(id % APP_STATE_STEPS);

    if (r >= APP_STATE_TRACKS - 1) {
        /* 鼓轨只读：MVP 不发 CMD_TOGGLE_STEP（阶段 4 接 DRUM_STEP），也不改焦点——
         * 焦点轨跟着合成轨行走，点鼓轨不应把焦点留在不可编辑的行上 */
        ESP_LOGI(TAG, "鼓轨步 %u 点击忽略（MVP 只读，阶段 4 接 DRUM_STEP）", (unsigned)c);
        set_status("DRUM EDIT = PHASE 4", ui_track_color(APP_STATE_TRACKS - 1));
    } else {
        state_set_focus(r, c); /* UI 局部态：焦点轨/步 */
        const edit_cmd_t cmd = {.type = CMD_TOGGLE_STEP, .a = r, .b = c};
        state_apply(&cmd);
        char buf[16];
        snprintf(buf, sizeof(buf), "T%u S%u", (unsigned)(r + 1), (unsigned)(c + 1));
        set_status(buf, ui_track_color(r));
    }
    refresh();
}

static void cell_apply(uint8_t r, uint8_t c, uint8_t old_state, uint8_t state)
{
    lv_obj_t *cell = s_cells[r][c];
    lv_style_t *old_base = (old_state & 1u) ? &s_st_cell_on[r] : &s_st_cell_off;
    lv_style_t *new_base = (state & 1u) ? &s_st_cell_on[r] : &s_st_cell_off;
    if (old_base != new_base) lv_obj_replace_style(cell, old_base, new_base, 0);

    lv_style_t *old_hl = hl_style((uint8_t)((old_state >> 1) & 3u));
    lv_style_t *new_hl = hl_style((uint8_t)((state >> 1) & 3u));
    if (old_hl != new_hl) lv_obj_replace_style(cell, old_hl, new_hl, 0);
}

void page_step_create(lv_obj_t *parent)
{
    styles_init();

    s_header = lv_label_create(parent);
    lv_label_set_text(s_header, "TRACK 1   STEP 0   LEN 16   DIV 1");
    lv_obj_set_style_text_font(s_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(ui_track_color(0)), 0);
    lv_obj_set_pos(s_header, 24, 14);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "");
    lv_obj_set_width(s_status, 600);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_style(s_status, &s_st_hint, 0);
    lv_obj_set_pos(s_status, 656, 14);

    static const char *const row_names[APP_STATE_TRACKS] = {"T1", "T2", "T3", "DRM"};
    for (uint8_t r = 0; r < APP_STATE_TRACKS; r++) {
        lv_obj_t *row_label = lv_label_create(parent);
        lv_label_set_text(row_label, row_names[r]);
        lv_obj_add_style(row_label, &s_st_row[r], 0);
        lv_obj_set_pos(row_label, 24, GRID_Y0 + r * ROW_PITCH + 42);

        for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
            lv_obj_t *cell = lv_obj_create(parent);
            lv_obj_remove_style_all(cell);
            lv_obj_set_scrollable(cell, false);
            lv_obj_set_pos(cell, GRID_X0 + c * CELL_STEP_X, GRID_Y0 + r * ROW_PITCH);
            lv_obj_set_size(cell, CELL_W, CELL_H);
            lv_obj_add_style(cell, &s_st_cell_off, 0);
            lv_obj_add_style(cell, &s_st_hl_none, 0);
            lv_obj_add_event_cb(cell, cell_clicked_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(r * APP_STATE_STEPS + c));
            s_cells[r][c] = cell;

            /* 列号只标 4 的倍数：每列一个标签会多占 12 个对象（LVGL 池在内置 RAM，
             * 64 格已吃掉大半，见文件头） */
            if ((c + 1) % 4 != 0) continue;
            char buf[4];
            snprintf(buf, sizeof(buf), "%u", (unsigned)(c + 1));
            lv_obj_t *num = lv_label_create(parent);
            lv_label_set_text(num, buf);
            lv_obj_add_style(num, &s_st_num, 0);
            lv_obj_set_pos(num, GRID_X0 + c * CELL_STEP_X + 24, 568);
        }
    }

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "TAP = TOGGLE ON/OFF    DRUM ROW = READ ONLY (LEN > 16 NOT SHOWN)");
    lv_obj_add_style(hint, &s_st_hint, 0);
    lv_obj_set_pos(hint, 24, 606);
}

void page_step_update(const app_state_t *st)
{
    uint8_t ft = st->focus_track < APP_STATE_TRACKS ? st->focus_track : 0;
    uint8_t fs = st->focus_step < APP_STATE_STEPS ? st->focus_step : 0;
    uint8_t len = st->core.trk[ft].len;
    uint8_t div = st->core.trk[ft].div;

    if (s_header_dirty || ft != s_header_cache.track || fs != s_header_cache.step ||
        len != s_header_cache.len || div != s_header_cache.div) {
        s_header_dirty = false;
        s_header_cache.track = ft;
        s_header_cache.step = fs;
        s_header_cache.len = len;
        s_header_cache.div = div;
        char buf[48];
        snprintf(buf, sizeof(buf), "TRACK %u   STEP %u   LEN %u   DIV %u", (unsigned)(ft + 1),
                 (unsigned)(fs + 1), (unsigned)len, (unsigned)div);
        lv_label_set_text(s_header, buf);
        lv_obj_set_style_text_color(s_header, lv_color_hex(ui_track_color(ft)), 0);
    }

    for (uint8_t r = 0; r < APP_STATE_TRACKS; r++) {
        /* 停止时不显示播放头（快照 step_pos 停止时为 0，画在 0 列会误导） */
        int8_t playhead = st->core.playing ? (int8_t)st->core.step_pos[r] : -1;
        if (playhead >= (int8_t)APP_STATE_STEPS) playhead = -1; /* 超出 16 列视窗 */
        for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
            bool on;
            if (r == APP_STATE_TRACKS - 1) {
                on = st->core.dstep[c].on != 0; /* 鼓轨：引擎快照的聚合 on */
            } else {
                on = state_step_is_on(st->core.step[r][c].n, st->core.step[r][c].time);
            }
            uint8_t hl = HL_NONE;
            if (playhead == (int8_t)c) {
                hl = HL_PLAYHEAD;
            } else if (r == ft && c == fs) {
                hl = HL_FOCUS;
            }
            uint8_t want = (uint8_t)((on ? 1u : 0u) | (hl << 1));
            if (want == s_cell_state[r][c]) continue;
            cell_apply(r, c, s_cell_state[r][c], want);
            s_cell_state[r][c] = want;
        }
    }
}
