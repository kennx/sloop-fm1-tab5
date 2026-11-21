/*
 * STEP 页：4 行 × 步窗网格（行 = 轨），点格切换有/无音，播放头高亮
 *
 * 步窗（sloop_state 的 step_window_t，UI 局部态）：引擎共 64 步 = 4 bank，页面
 * 只显示 window_len 列（1..16），列 c 对应引擎绝对步 window_start + c。列号标签是
 * **绝对步号**（window_start+1…）；窗口外的列/标签隐藏。引擎状态投影
 * （app_state_t.core.step[k][c]/dstep[c]）本身就是窗口列，渲染不另做偏移。
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 顶栏 "TRACK n   STEP abs   LEN l   DIV d   WIN a..b"  (24, 14) 20pt 焦点轨色；
 * - 右上角状态行（点格后的反馈：T1 S17 / DRUM EDIT = PHASE 4）右对齐 (656, 14) 宽 600；
 * - 行标签 T1/T2/T3/DRM (24, 84 + r*124 + 42) 20pt 轨色；
 * - 格子 (r,c)：x = 88 + c*73, y = 84 + r*124, 67×104，圆角 10；
 * - 列号绝对步号单行标在网格下方 (x+24, 568) 16pt：16 列全标（window_start 16
 *   对齐，故 bank1 显示 17..32），窗口外的列随格子一起隐藏；
 * - 底部提示 (24, 606) 16pt。
 *
 * 渲染口径：格「亮」= state_step_is_on()（n>0 且 time==NOTE，与状态层切换同一判定，
 * TIE/REST 的步不算亮）；绝对步 ≥ 本轨 LEN 的格用暗色（dim）表示「播放不经过」，
 * 仍可点（编辑不改语义）；列 ≥ window_len 的格隐藏（TAP 也命不中）。
 * 鼓轨（第 4 行）只展示引擎快照 dstep[c].on 的窗口聚合，点击不发 CMD_TOGGLE_STEP、
 * 也不改焦点（焦点轨由合成轨行决定），只写状态行 "DRUM EDIT = PHASE 4"。
 * 高亮：播放中当前步（step_pos[r] 绝对步号）落在窗口内才描边 UI_ACCENT 5px——
 * 窗口外（在别的 bank 播放）不显示残留高亮；焦点格（focus_track/focus_step 列）
 * 描边 UI_TEXT 3px；两者重叠时播放头优先。
 *
 * 点击：合成轨先 state_set_focus()（UI 局部态）再 state_apply(CMD_TOGGLE_STEP)，
 * 步号 = step_window_abs()（绝对索引，引擎不感知窗口）；随后立即刷新本页一次
 * （不等 30ms 定时器，切页/截屏不受 tick 时序影响）。控件回调与 console `TAP`
 * 注入都只调用 step_activate()（唯一共同路径），TAP 的命中矩形由建格同一组布局
 * 宏推导（无第二份坐标表），列数取当前 window_len。
 *
 * 内存：64 格 + 16 个列号标签共用静态 lv_style_t（格底 10 种 = 亮/暗 × 空/四轨色 +
 * 高亮 3 种 + 文本 6 种），每对象一份局部样式会把 LVGL 池（约 58KB）吃穿；
 * 状态变化用 lv_obj_replace_style 换共享样式指针，不分配新样式。列号标签是**单行**
 * （旧实现按行重复建了 4 份重叠标签），16 列全标，对象数与旧版相同（16）。
 * 仍用局部样式的只有顶栏/状态行两个单例标签（改色/对齐/宽度），量级固定。
 * 脏检查：每格缓存 1 字节（bit0 亮 / bit1..2 高亮种类 / bit3 绝对步 ≥ LEN 暗 /
 * bit4 列 ≥ window_len 隐藏），变化才换样式/隐藏标志。
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
#define NUM_Y       568

#define HL_NONE     0u
#define HL_PLAYHEAD 1u
#define HL_FOCUS    2u

static lv_obj_t *s_cells[APP_STATE_TRACKS][APP_STATE_STEPS];
static uint8_t s_cell_state[APP_STATE_TRACKS][APP_STATE_STEPS];
static lv_obj_t *s_nums[APP_STATE_STEPS]; /* 单行列号标签（绝对步号） */
static lv_obj_t *s_header;
static lv_obj_t *s_status;

static lv_style_t s_st_cell_off;
static lv_style_t s_st_cell_off_dim;                       /* 绝对步 ≥ LEN：更暗一档 */
static lv_style_t s_st_cell_on[APP_STATE_TRACKS];
static lv_style_t s_st_cell_on_dim[APP_STATE_TRACKS];      /* 绝对步 ≥ LEN 的亮格 */
static lv_style_t s_st_hl_none;
static lv_style_t s_st_hl_playhead;
static lv_style_t s_st_hl_focus;
static lv_style_t s_st_row[APP_STATE_TRACKS];
static lv_style_t s_st_num;
static lv_style_t s_st_hint;

static struct {
    uint8_t track, step, len, div, start, wlen;
} s_header_cache;
/* 首帧强制刷新：缓存零值可能与 create 时的初始文案一致（如 len/div 恰好为 0），
 * 不强制的话首个 tick 可能跳过刷新、把硬编码文案留在屏上。 */
static bool s_header_dirty = true;
/* 列号标签缓存：窗口起点/列数变化后需要重写绝对步号并隐藏窗口外的列 */
static uint8_t s_win_start;
static uint8_t s_win_len;
static bool s_win_dirty = true;

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

/* 绝对步 ≥ LEN 的暗色：亮格取轨色 45%，空格的底色由 UI_PANEL_HI 落到更暗一档 */
#define DIM_PCT 45u
static uint32_t dim_color(uint32_t c)
{
    uint32_t r = ((c >> 16) & 0xFFu) * DIM_PCT / 100u;
    uint32_t g = ((c >> 8) & 0xFFu) * DIM_PCT / 100u;
    uint32_t b = (c & 0xFFu) * DIM_PCT / 100u;
    return (r << 16) | (g << 8) | b;
}

static void styles_init(void)
{
    cell_shape_style(&s_st_cell_off, UI_PANEL_HI);
    cell_shape_style(&s_st_cell_off_dim, dim_color(UI_PANEL_HI));
    for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
        cell_shape_style(&s_st_cell_on[k], ui_track_color(k));
        cell_shape_style(&s_st_cell_on_dim[k], dim_color(ui_track_color(k)));
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

/* 格状态字节：bit0 亮、bit1..2 高亮种类、bit3 暗（绝对步 ≥ LEN）、bit4 隐藏（列 ≥ 窗口列数）。 */
#define CELL_ON     0x01u
#define CELL_HL     0x06u
#define CELL_DIM    0x08u
#define CELL_HIDDEN 0x10u

static lv_style_t *base_style(uint8_t r, uint8_t state)
{
    bool on = (state & CELL_ON) != 0;
    bool dim = (state & CELL_DIM) != 0;
    if (on) return dim ? &s_st_cell_on_dim[r] : &s_st_cell_on[r];
    return dim ? &s_st_cell_off_dim : &s_st_cell_off;
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

/* 当前窗口的列数（防御性钳位：状态层已保证 1..APP_STATE_STEPS） */
static uint8_t window_len_of(const app_state_t *st)
{
    uint8_t wlen = st->window.window_len;
    if (wlen < 1u) wlen = 1u;
    if (wlen > APP_STATE_STEPS) wlen = APP_STATE_STEPS;
    return wlen;
}

/* 格激活（LVGL 上下文、调用方已持锁）：控件点击回调与 TAP 注入的**唯一共同路径**。
 * 两条路径的 EditCmd 序列因此由本函数唯一决定（ADR-005 的 EditCmd 层等价）。 */
static void step_activate(uint8_t r, uint8_t c)
{
    app_state_t st;
    state_get_copy(&st);
    if (c >= window_len_of(&st)) return; /* 隐藏列：不可点（TAP 命中表也不含） */
    const step_window_t win = st.window;
    const uint8_t abs = step_window_abs(&win, c);
    if (r >= APP_STATE_TRACKS - 1) {
        /* 鼓轨只读：MVP 不发 CMD_TOGGLE_STEP（阶段 4 接 DRUM_STEP），也不改焦点——
         * 焦点轨跟着合成轨行走，点鼓轨不应把焦点留在不可编辑的行上 */
        ESP_LOGI(TAG, "鼓轨步 %u 点击忽略（MVP 只读，阶段 4 接 DRUM_STEP）", (unsigned)abs);
        set_status("DRUM EDIT = PHASE 4", ui_track_color(APP_STATE_TRACKS - 1));
    } else {
        state_set_focus(r, c); /* UI 局部态：焦点轨/窗口列 */
        const edit_cmd_t cmd = {.type = CMD_TOGGLE_STEP, .a = r, .b = abs};
        state_apply(&cmd);
        char buf[16];
        snprintf(buf, sizeof(buf), "T%u S%u", (unsigned)(r + 1), (unsigned)(abs + 1));
        set_status(buf, ui_track_color(r));
    }
    refresh();
}

static void cell_clicked_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    step_activate((uint8_t)(id / APP_STATE_STEPS), (uint8_t)(id % APP_STATE_STEPS));
}

/* TAP hit-test：矩形由建格所用的同一组宏（GRID_X0/Y0、CELL_W/H、CELL_STEP_X、
 * ROW_PITCH）现场推导——不另存一份坐标表，布局改动不会与注入漂移。列数取当前
 * 窗口（隐藏列不命中），out->b 回绝对步号（与 EditCmd 同口径）。
 * 行/列各一次区间判定；落在格间距（列间 6px、行间 20px）或网格外即未命中。 */
bool page_step_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    app_state_t st;
    state_get_copy(&st);
    const uint8_t wlen = window_len_of(&st);
    const step_window_t win = st.window;

    for (uint8_t r = 0; r < APP_STATE_TRACKS; r++) {
        int32_t y0 = GRID_Y0 + r * ROW_PITCH;
        if (y < y0 || y >= y0 + CELL_H) continue;
        for (uint8_t c = 0; c < wlen; c++) {
            int32_t x0 = GRID_X0 + c * CELL_STEP_X;
            if (x < x0 || x >= x0 + CELL_W) continue;
            step_activate(r, c);
            if (out != NULL) {
                out->kind = (r == APP_STATE_TRACKS - 1) ? SLOOP_TAP_STEP_DRUM : SLOOP_TAP_STEP_CELL;
                out->a = r;
                out->b = step_window_abs(&win, c);
            }
            return true;
        }
        return false; /* 行命中但 x 落列缝/隐藏列/网格右缘 */
    }
    return false;
}

static void cell_apply(uint8_t r, uint8_t c, uint8_t old_state, uint8_t state)
{
    lv_obj_t *cell = s_cells[r][c];
    if (((old_state ^ state) & CELL_HIDDEN) != 0) {
        lv_obj_set_hidden(cell, (state & CELL_HIDDEN) != 0);
    }
    lv_style_t *old_base = base_style(r, old_state);
    lv_style_t *new_base = base_style(r, state);
    if (old_base != new_base) lv_obj_replace_style(cell, old_base, new_base, 0);

    lv_style_t *old_hl = hl_style((uint8_t)((old_state & CELL_HL) >> 1));
    lv_style_t *new_hl = hl_style((uint8_t)((state & CELL_HL) >> 1));
    if (old_hl != new_hl) lv_obj_replace_style(cell, old_hl, new_hl, 0);
}

lv_obj_t *page_step_build(lv_obj_t *parent)
{
    styles_init();
    /* 懒建重试时控件是全新的：复位格子脏检查缓存与首帧强制刷新 */
    memset(s_cell_state, 0, sizeof(s_cell_state));
    memset(s_nums, 0, sizeof(s_nums));
    s_header_dirty = true;
    s_win_dirty = true;

    s_header = lv_label_create(parent);
    UI_BUILD_CHECK(s_header);
    lv_label_set_text(s_header, "TRACK 1   STEP 1   LEN 16   DIV 1   WIN 1..16");
    lv_obj_set_style_text_font(s_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(ui_track_color(0)), 0);
    lv_obj_set_pos(s_header, 24, 14);

    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_label_set_text(s_status, "");
    lv_obj_set_width(s_status, 600);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_style(s_status, &s_st_hint, 0);
    lv_obj_set_pos(s_status, 656, 14);

    static const char *const row_names[APP_STATE_TRACKS] = {"T1", "T2", "T3", "DRM"};
    for (uint8_t r = 0; r < APP_STATE_TRACKS; r++) {
        lv_obj_t *row_label = lv_label_create(parent);
        UI_BUILD_CHECK(row_label);
        lv_label_set_text(row_label, row_names[r]);
        lv_obj_add_style(row_label, &s_st_row[r], 0);
        lv_obj_set_pos(row_label, 24, GRID_Y0 + r * ROW_PITCH + 42);

        for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
            lv_obj_t *cell = lv_obj_create(parent);
            UI_BUILD_CHECK(cell);
            lv_obj_remove_style_all(cell);
            lv_obj_set_scrollable(cell, false);
            lv_obj_set_pos(cell, GRID_X0 + c * CELL_STEP_X, GRID_Y0 + r * ROW_PITCH);
            lv_obj_set_size(cell, CELL_W, CELL_H);
            lv_obj_add_style(cell, &s_st_cell_off, 0);
            lv_obj_add_style(cell, &s_st_hl_none, 0);
            lv_obj_add_event_cb(cell, cell_clicked_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(r * APP_STATE_STEPS + c));
            s_cells[r][c] = cell;
        }
    }

    /* 列号单行（绝对步号；窗口切换时由 update 重写并隐藏窗口外的列）。16 列全标，
     * 对象数与旧版相同（旧版每行重复建同一位置的标签）。 */
    for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%u", (unsigned)(c + 1));
        lv_obj_t *num = lv_label_create(parent);
        UI_BUILD_CHECK(num);
        lv_label_set_text(num, buf);
        lv_obj_add_style(num, &s_st_num, 0);
        lv_obj_set_pos(num, GRID_X0 + c * CELL_STEP_X + 24, NUM_Y);
        s_nums[c] = num;
    }

    lv_obj_t *hint = lv_label_create(parent);
    UI_BUILD_CHECK(hint);
    lv_label_set_text(hint, "TAP = TOGGLE ON/OFF    DIM = BEYOND LEN    DRUM ROW = READ ONLY (16-STEP WINDOW)");
    lv_obj_add_style(hint, &s_st_hint, 0);
    lv_obj_set_pos(hint, 24, 606);
    return parent;
}

void page_step_update(const app_state_t *st)
{
    const step_window_t win = st->window;
    uint8_t wlen = window_len_of(st);
    uint8_t start = win.window_start;

    /* 窗口列数/起点变化：重写列号（绝对步号）并隐藏窗口外的列（格子的隐藏状态
     * 在下面的每格脏检查里换）。 */
    if (s_win_dirty || start != s_win_start || wlen != s_win_len) {
        s_win_dirty = false;
        s_win_start = start;
        s_win_len = wlen;
        for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%u", (unsigned)(start + c + 1));
            lv_label_set_text(s_nums[c], buf);
            lv_obj_set_hidden(s_nums[c], c >= wlen);
        }
        s_header_dirty = true;
    }

    uint8_t ft = st->focus_track < APP_STATE_TRACKS ? st->focus_track : 0;
    uint8_t fs = st->focus_step < wlen ? st->focus_step : 0;
    uint8_t len = st->core.trk[ft].len;
    uint8_t div = st->core.trk[ft].div;

    if (s_header_dirty || ft != s_header_cache.track || fs != s_header_cache.step ||
        len != s_header_cache.len || div != s_header_cache.div ||
        start != s_header_cache.start || wlen != s_header_cache.wlen) {
        s_header_dirty = false;
        s_header_cache.track = ft;
        s_header_cache.step = fs;
        s_header_cache.len = len;
        s_header_cache.div = div;
        s_header_cache.start = start;
        s_header_cache.wlen = wlen;
        char buf[64];
        /* 焦点/窗口都按**绝对步号**显示（列号与引擎步号同口径，不随窗口变） */
        snprintf(buf, sizeof(buf), "TRACK %u   STEP %u   LEN %u   DIV %u   WIN %u..%u",
                 (unsigned)(ft + 1), (unsigned)(start + fs + 1), (unsigned)len, (unsigned)div,
                 (unsigned)(start + 1), (unsigned)(start + wlen));
        lv_label_set_text(s_header, buf);
        lv_obj_set_style_text_color(s_header, lv_color_hex(ui_track_color(ft)), 0);
    }

    for (uint8_t r = 0; r < APP_STATE_TRACKS; r++) {
        /* 停止时不显示播放头（快照 step_pos 停止时为 0，画在 0 列会误导）。
         * 播放头是绝对步号：映射到窗口列，落在窗口外（别的 bank 在播）不显示——
         * 否则会留下与时序不符的残留高亮。 */
        int16_t playhead = -1;
        if (st->core.playing) {
            int16_t col = (int16_t)st->core.step_pos[r] - (int16_t)start;
            if (col >= 0 && col < (int16_t)wlen) playhead = col;
        }
        /* 绝对步 ≥ 本轨 LEN 的格暗显（dim）：LEN 之外播放不经过，编辑仍可点 */
        uint8_t len = st->core.trk[r].len;
        if (len < 1u) len = 1u;
        for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
            bool on;
            if (r == APP_STATE_TRACKS - 1) {
                on = st->core.dstep[c].on != 0; /* 鼓轨：引擎快照的窗口聚合 on */
            } else {
                on = state_step_is_on(st->core.step[r][c].n, st->core.step[r][c].time);
            }
            uint8_t hl = HL_NONE;
            if (playhead == (int16_t)c) {
                hl = HL_PLAYHEAD;
            } else if (r == ft && c == fs) {
                hl = HL_FOCUS;
            }
            uint8_t want = (uint8_t)((on ? CELL_ON : 0u) | (hl << 1) |
                                     ((start + c) >= len ? CELL_DIM : 0u) |
                                     (c >= wlen ? CELL_HIDDEN : 0u));
            if (want == s_cell_state[r][c]) continue;
            cell_apply(r, c, s_cell_state[r][c], want);
            s_cell_state[r][c] = want;
        }
    }
}
