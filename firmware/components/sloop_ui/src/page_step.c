/*
 * STEP 页：4 行 × 步窗网格（行 = 轨），点格切换有/无音，播放头高亮
 *
 * 步窗（sloop_state 的 step_window_t，UI 局部态）：引擎共 64 步 = 4 bank，页面
 * 只显示 window_len 列（1..16），列 c 对应引擎绝对步 window_start + c。列号标签是
 * **绝对步号**（window_start+1…）；窗口外的列/标签隐藏。引擎状态投影
 * （app_state_t.core.step[k][c]/dstep[c]/dstep_raw[c]）本身就是窗口列，渲染不另做
 * 偏移。
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 顶栏 "TRACK n   STEP abs   LEN l   DIV d   WIN a..b"  (24, 14) 20pt 焦点轨色；
 * - 右上角状态行（聚焦步/选中 drum lane 的派生文本）右对齐 (656, 14) 宽 600；
 * - 行标签 T1/T2/T3/DRM (24, GRID_Y0 + r*ROW_PITCH + 42) 20pt 轨色；
 * - 格子 (r,c)：x = 88 + c*73, y = 84 + r*124, 67×104，圆角 10；
 * - 列号绝对步号单行标在网格下方 (x+24, 568) 16pt：16 列全标（window_start 16
 *   对齐，故 bank1 显示 17..32），窗口外的列随格子一起隐藏；
 * - 底部两行提示 (24, 604) 16pt：字形图例 + 鼓 lane 操作。
 *
 * 渲染口径：
 * - 格底色 = 状态层「有音」判定（n>0 且 time==NOTE，与切换同一判定，TIE/REST 不算
 *   亮）；绝对步 ≥ 本轨 LEN 的格用暗色（dim）；列 ≥ window_len 的格隐藏（TAP 也命
 *   不中）。TIE 步另画「空心内框 + TIE 文本」（不是新对象：见下）。
 * - 播放头（step_pos[r] 绝对步号）落在窗口内才描边 UI_ACCENT 5px；焦点格描边
 *   UI_TEXT 3px；两者重叠时播放头优先。
 * - **格内容是自绘的**（P4-C Task 9）：64 个格对象不新增任何每格 label。一格一个
 *   静态 cell_view_t 缓存 + 一块覆盖整个网格的透明 overlay 对象（1 个对象、1 个
 *   DRAW_MAIN 回调），在 overlay 的绘制事件里按缓存的视图画文字/小方块/内框。
 *   文字用**静态**缓冲（s_note_txt/s_mic_txt/s_rat_txt，text_local=0），不产生
 *   每帧池分配；视图/文本变化时才 `lv_obj_invalidate_area()` 失效该格一小块。
 *   这样 STEP 的池增量只 +1 个对象量级（est 23,040 不抬）。
 * - 格内字形（图例见底部提示）：
 *     NOTE：音符名（最多 4 个，2×2）——白 = NORM、黄 = HARD、灰 = SOFT、
 *           深灰 = GHOST（每音符 2-bit lvl）；CONT 音符名字下方有白色短
 *           横线（长音从前一步延续）；顶条 A = accent、S = slide、F = fill-only、N = no-fill；
 *           底条 m±n = micro、xN = ratchet（N = 该步最大击打数）。
 *     TIE：空心内框（轨色）+ TIE 文本（长按音的中段；不是 NOTE）。
 *     DRM：4×4 lane 网格（lane = row*4+col，与上游 drums.c 序号一致）：亮 dot 按
 *           lane lvl 着色（黄橙 NORM / 白 HARD / 灰 SOFT / 深处 GHOST），空格是暗
 *           色小方块，选中 lane 有白色 ring；顶/底条同上（fill/micro）。鼓格底色
 *           **保持空格样式**（不用聚合 on 填满鼓轨色），否则同色的 NORM dot 不可见。
 *
 * 交互（控件回调与 TAP 注入共用同一动作函数）：
 * - 合成轨格 → state_set_focus(r,c) + CMD_TOGGLE_STEP（绝对步号）；打开时保留
 *   micro/fill（状态层语义）。
 * - 鼓轨格 → 点 4×4 网格里的 dot = 选中该 lane 并切换该步的该位；点格内其它位置 =
 *   切换「当前选中 lane」在该步的位。两条路径都 set_focus(DRM, c) 并下发
 *   CMD_DRUM_STEP_WRITE（整份原始 16 lane POD，lvl/rat 原样带回）。lane 选择是 UI
 *   局部态（s_lane），键盘 ↑/↓/ENTER 与 console `UI LANE` 共享。
 * - 键盘焦点环 = T1,T2,T3,DRM L1..L16（input.c 调 sloop_ui_step_focus_vertical；
 *   鼓轨上 ENTER = sloop_ui_step_drum_toggle）。
 *
 * 内存：64 格 + 16 列号 + 顶栏/状态行/提示 + 1 overlay 共用静态 lv_style_t；
 * 状态变化用 lv_obj_replace_style 换共享样式指针，不分配新样式。脏检查：每格
 * 1 字节样式状态（亮/高亮/暗/隐藏）+ 1 个 32-bit 内容签名（视图或选中 lane 变
 * 才失效该格自绘区）。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "sloop_state.h"

#define GRID_X0     88
#define GRID_Y0     84
#define CELL_W      67
#define CELL_H      104
#define CELL_STEP_X 73 /* 列距 = CELL_W + 6 */
#define ROW_PITCH   124 /* 行距 = CELL_H + 20 */
#define CELL_RADIUS 10
#define NUM_Y       568
/* overlay 覆盖整个 16 列网格（15 个列距 + 1 格宽）；放在格宏之后，避免读者以为
 * 宏体在校验/依赖定义顺序（C 宏只在展开处求值，但顺序写错会误导）。 */
#define GRID_W      (15 * CELL_STEP_X + CELL_W)

/* 上游 core.h 的步编码（不引入 vendored 头）：time = ST_NOTE/ST_TIE/ST_REST；
 * flags = SF_ACCENT | SF_SLIDE | SF_CONT0<<k（SLOOP 2.7 的「长音延续」标记）。 */
#define STEP_TIME_TIE   1u
#define STEP_FLAG_ACCENT 0x01u
#define STEP_FLAG_SLIDE  0x02u
#define STEP_FLAG_CONT0  0x08u

/* 鼓 lane 网格（格内坐标）：4×4、lane = row*4+col。dot 8×8、pitch 14 → 网格
 * 50×50 落在 67×104 格里（上条 flags、下条 micro/rat 各一行文字）。 */
#define DRUM_GRID_X0    8
#define DRUM_GRID_Y0    30
#define DRUM_LANE_PITCH 14
#define DRUM_LANE_DOT   8

/* 格内文本条（格内 y）：顶条 flags 14pt、音符名两行 14pt、底条 micro/rat 14pt */
#define TOP_Y   4
#define NOTE_Y  22
#define NOTE_DY 20
#define BOT_Y   84
#define CELL_FONT (&lv_font_montserrat_14)

#define HL_NONE     0u
#define HL_PLAYHEAD 1u
#define HL_FOCUS    2u

/* 格内容视图（update 时按快照填，draw 时只读；POD、全字节参与签名） */
enum { CELL_EMPTY = 0, CELL_NOTE, CELL_TIE, CELL_DRUM };
typedef struct {
    uint8_t kind;
    uint8_t n;
    uint8_t note[4];
    uint8_t flags;
    uint8_t lvl;   /* synth：每音符 2-bit */
    uint8_t rat;   /* synth：每音符 2-bit */
    uint8_t micro; /* int8 位模式 */
    uint8_t fill;
    uint8_t on[2]; /* drum：16 lane 位图 */
    uint8_t dlvl[4];
    uint8_t rat_max; /* drum：活动 lane 的最大 ratchet */
    uint8_t dim;
    uint8_t hidden;
} cell_view_t;

static lv_obj_t *s_cells[APP_STATE_TRACKS][APP_STATE_STEPS];
static uint8_t s_cell_state[APP_STATE_TRACKS][APP_STATE_STEPS];
static uint32_t s_cell_sig[APP_STATE_TRACKS][APP_STATE_STEPS];
static cell_view_t s_cell_view[APP_STATE_TRACKS][APP_STATE_STEPS];
/* 自绘文字缓冲（静态生命周期供 text_local=0 的绘制任务引用） */
static char s_note_txt[APP_STATE_TRACKS][APP_STATE_STEPS][4][4];
static char s_mic_txt[APP_STATE_TRACKS][APP_STATE_STEPS][6];
static char s_rat_txt[APP_STATE_TRACKS][APP_STATE_STEPS][6];
static lv_obj_t *s_nums[APP_STATE_STEPS]; /* 单行列号标签（绝对步号） */
static lv_obj_t *s_header;
static lv_obj_t *s_status;
static lv_obj_t *s_overlay; /* 网格自绘层（唯一新增对象） */
static uint8_t s_lane;      /* 选中的鼓 lane（0..15，UI 局部态，跨页面重建保留） */

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
/* 状态行缓存（聚焦步/选中 lane 派生文本；文本与颜色都脏检查） */
static char s_status_cache[96];
static uint32_t s_status_color;
static bool s_status_dirty = true;

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
    /* 只初始化一次（R34/ADR-008 §3）：组作用域生命周期会反复销毁/重建本页，
     * `lv_style_init` 是 memzero——重跑会丢掉静态样式已分配的 props 数组，每次跨组
     * 往返都漏进 LVGL 池（与 page_tracks.c 同一纪律）。 */
    static bool done;
    if (done) {
        lv_style_set_bg_color(&s_st_cell_off, lv_color_hex(UI_PANEL_HI));
        lv_style_set_bg_color(&s_st_cell_off_dim, lv_color_hex(dim_color(UI_PANEL_HI)));
        for (uint8_t k = 0; k < APP_STATE_TRACKS; k++) {
            lv_style_set_bg_color(&s_st_cell_on[k], lv_color_hex(ui_track_color(k)));
            lv_style_set_bg_color(&s_st_cell_on_dim[k], lv_color_hex(dim_color(ui_track_color(k))));
            lv_style_set_text_color(&s_st_row[k], lv_color_hex(ui_track_color(k)));
        }
        lv_style_set_border_color(&s_st_hl_none, lv_color_hex(UI_BG));
        lv_style_set_border_color(&s_st_hl_playhead, lv_color_hex(UI_ACCENT));
        lv_style_set_border_color(&s_st_hl_focus, lv_color_hex(UI_TEXT));
        lv_style_set_text_color(&s_st_num, lv_color_hex(UI_TEXT_DIM));
        lv_style_set_text_color(&s_st_hint, lv_color_hex(UI_TEXT_DIM));
        return;
    }
    done = true;

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

/* 当前窗口的列数（防御性钳位：状态层已保证 1..APP_STATE_STEPS） */
static uint8_t window_len_of(const app_state_t *st)
{
    uint8_t wlen = st->window.window_len;
    if (wlen < 1u) wlen = 1u;
    if (wlen > APP_STATE_STEPS) wlen = APP_STATE_STEPS;
    return wlen;
}

/* ------------------------------------------------------------ 自绘（P4-C Task 9） --- */

/* 颜色：音符名按 lvl（0 NORM 白 / 1 GHOST 深灰 / 2 SOFT 灰 / 3 HARD 黄） */
static uint32_t lvl_color(uint8_t lvl)
{
    switch (lvl & 3u) {
    case 1: return 0x5A6068;  /* GHOST：最暗 */
    case 2: return 0xA8AEB6;  /* SOFT */
    case 3: return UI_ACCENT; /* HARD：黄（最响） */
    default: return UI_TEXT;  /* NORM */
    }
}

/* 鼓 lane dot：NORM 轨色（橙）/ HARD 白 / SOFT 灰 / GHOST 暗橙 */
static uint32_t drum_lane_color(uint8_t lvl)
{
    static const uint8_t pct[4] = {100u, 50u, 75u, 0u};
    if ((lvl & 3u) == 3u) return UI_TEXT;
    uint32_t c = ui_track_color(APP_STATE_TRACKS - 1u);
    uint32_t p = pct[lvl & 3u];
    return (((c >> 16) & 0xFFu) * p / 100u) << 16 |
           (((c >> 8) & 0xFFu) * p / 100u) << 8 |
           ((c & 0xFFu) * p / 100u);
}

static void draw_rect(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t color, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_area_t a = {.x1 = x, .y1 = y, .x2 = x + w - 1, .y2 = y + h - 1};
    lv_draw_rect(layer, &dsc, &a);
}

static void draw_frame(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h,
                       uint32_t color, int32_t width, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.radius = radius;
    dsc.border_width = width;
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_color = lv_color_hex(color);
    lv_area_t a = {.x1 = x, .y1 = y, .x2 = x + w - 1, .y2 = y + h - 1};
    lv_draw_rect(layer, &dsc, &a);
}

/* text_local=0：text 指向本文件的静态缓冲/字符串字面量（生命周期 = 程序） */
static void draw_text(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t color, const char *text)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.font = CELL_FONT;
    dsc.color = lv_color_hex(color);
    dsc.opa = LV_OPA_COVER;
    dsc.align = LV_TEXT_ALIGN_LEFT;
    dsc.text = text;
    dsc.text_local = 0;
    lv_area_t a = {.x1 = x, .y1 = y, .x2 = x + w - 1, .y2 = y + h - 1};
    lv_draw_label(layer, &dsc, &a);
}

static void note_name(char *b, size_t n, uint8_t note)
{
    static const char *const pc[12] = {"C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B"};
    snprintf(b, n, "%s%d", pc[note % 12u], (int)(note / 12u) - 1);
}

/* 顶条 flags（A/S/F/N）与底条（micro/rat）——合成轨与鼓轨共用 */
static void draw_tags(lv_layer_t *layer, int32_t x, int32_t y, const cell_view_t *v,
                      uint8_t r, uint8_t c)
{
    if ((v->flags & STEP_FLAG_ACCENT) != 0) draw_text(layer, x + 6, y + TOP_Y, 12, 18, UI_TEXT, "A");
    if ((v->flags & STEP_FLAG_SLIDE) != 0) draw_text(layer, x + 22, y + TOP_Y, 12, 18, UI_TEXT, "S");
    if (v->fill == 1u) draw_text(layer, x + 38, y + TOP_Y, 12, 18, UI_ACCENT, "F");
    else if (v->fill == 2u) draw_text(layer, x + 38, y + TOP_Y, 12, 18, UI_TEXT_DIM, "N");
    if (v->micro != 0u) draw_text(layer, x + 4, y + BOT_Y, 44, 18, UI_TEXT_DIM, s_mic_txt[r][c]);
    if (v->rat_max != 0u) draw_text(layer, x + 46, y + BOT_Y, 19, 18, UI_TEXT, s_rat_txt[r][c]);
}

static void draw_cell(lv_layer_t *layer, int32_t ox, int32_t oy, uint8_t r, uint8_t c)
{
    const cell_view_t *v = &s_cell_view[r][c];
    if (v->hidden || v->kind == CELL_EMPTY) return;
    const int32_t x = ox + c * CELL_STEP_X;
    const int32_t y = oy + r * ROW_PITCH;

    if (v->kind == CELL_TIE) {
        const uint32_t col = v->dim ? dim_color(ui_track_color(r)) : ui_track_color(r);
        draw_frame(layer, x + 3, y + 3, CELL_W - 6, CELL_H - 6, col, 2, 8);
        draw_text(layer, x + 18, y + 42, 32, 20, col, "TIE");
        draw_tags(layer, x, y, v, r, c);
        return;
    }
    if (v->kind == CELL_DRUM) {
        for (uint8_t lane = 0; lane < 16u; lane++) {
            const int32_t dx = x + DRUM_GRID_X0 + (lane % 4u) * DRUM_LANE_PITCH;
            const int32_t dy = y + DRUM_GRID_Y0 + (lane / 4u) * DRUM_LANE_PITCH;
            const bool on = ((v->on[lane >> 3] >> (lane & 7u)) & 1u) != 0;
            if (on) {
                const uint8_t lvl = (uint8_t)((v->dlvl[lane / 4u] >> (2u * (lane % 4u))) & 3u);
                draw_rect(layer, dx, dy, DRUM_LANE_DOT, DRUM_LANE_DOT, drum_lane_color(lvl), 3);
            } else {
                draw_rect(layer, dx + 1, dy + 1, DRUM_LANE_DOT - 2, DRUM_LANE_DOT - 2,
                          UI_PANEL_SEL, 2);
            }
            if (lane == s_lane) {
                /* 选中 ring 画在**每个**鼓格的同一 lane 上（不只焦点格）：扫一行就知道
                 * 「下一位会落在哪个 lane」；代价是 16 个白框。只画焦点格会让其它步
                 * 的目标 lane 不可见（点格内空白正是按选中 lane 写位）。 */
                draw_frame(layer, dx - 2, dy - 2, DRUM_LANE_DOT + 4, DRUM_LANE_DOT + 4,
                           UI_TEXT, 2, 4);
            }
        }
        draw_tags(layer, x, y, v, r, c);
        return;
    }

    /* CELL_NOTE：每音符一个名字（2×2 槽），按 lvl 着色；CONT 音符名下短横线 */
    for (uint8_t k = 0; k < v->n && k < 4u; k++) {
        const int32_t nx = x + ((k & 1u) ? 38 : 12);
        const int32_t ny = y + NOTE_Y + ((k >> 1u) ? NOTE_DY : 0);
        const uint8_t lvl = (uint8_t)((v->lvl >> (2u * k)) & 3u);
        draw_text(layer, nx, ny, 28, 18, lvl_color(lvl), s_note_txt[r][c][k]);
        /* CONT 标记画在该音符名字的**正下方**（12×2 白条）：每音符独立，不会像固定
         * x+4 那样把 k=1/3 的标记画到 k=0/2 的名字旁。没有用「名字左侧 nx-8」是因为
         * 67px 格里两列名字各占 ~26px：k=0 的 "C#4"/"D4" 右缘实测到 x+30..32，nx-8 的
         * 竖条会压在相邻名字的最后一个字形上，反而看不出标记属于哪个音符。 */
        if ((v->flags & (STEP_FLAG_CONT0 << k)) != 0) {
            draw_rect(layer, nx, ny + 15, 12, 2, UI_TEXT, 0);
        }
    }
    draw_tags(layer, x, y, v, r, c);
}

static void overlay_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(s_overlay, &oc);
    const lv_area_t *clip = &layer->_clip_area;
    for (uint8_t r = 0; r < APP_STATE_TRACKS; r++) {
        for (uint8_t c = 0; c < APP_STATE_STEPS; c++) {
            const lv_area_t a = {.x1 = oc.x1 + c * CELL_STEP_X, .y1 = oc.y1 + r * ROW_PITCH,
                                 .x2 = oc.x1 + c * CELL_STEP_X + CELL_W - 1,
                                 .y2 = oc.y1 + r * ROW_PITCH + CELL_H - 1};
            /* lv_area_intersect 在 v9.6 是 private 头；这里只需「是否相交」 */
            if (a.x2 < clip->x1 || a.x1 > clip->x2 || a.y2 < clip->y1 || a.y1 > clip->y2)
                continue;
            draw_cell(layer, oc.x1, oc.y1, r, c);
        }
    }
}

/* 让 overlay 只重画这一格（LVGL 会按失效区重入本对象的 DRAW_MAIN） */
static void cell_invalidate(uint8_t r, uint8_t c)
{
    if (s_overlay == NULL) return;
    lv_area_t a;
    lv_obj_get_coords(s_cells[r][c], &a);
    lv_obj_invalidate_area(s_overlay, &a);
}

static uint32_t view_sig(const cell_view_t *v, uint8_t extra)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)v;
    for (size_t i = 0; i < sizeof(*v); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return (h ^ extra) * 16777619u;
}

/* 视图变化时才重写自绘文本缓冲（静态缓冲，绘制任务 text_local=0 引用它） */
static void view_texts(uint8_t r, uint8_t c, const cell_view_t *v)
{
    if (v->kind != CELL_NOTE) {
        s_note_txt[r][c][0][0] = '\0';
    } else {
        for (uint8_t k = 0; k < 4u; k++) {
            if (k < v->n) note_name(s_note_txt[r][c][k], sizeof(s_note_txt[r][c][k]), v->note[k]);
            else s_note_txt[r][c][k][0] = '\0';
        }
    }
    if (v->micro != 0u) {
        snprintf(s_mic_txt[r][c], sizeof(s_mic_txt[r][c]), "m%+d", (int)(int8_t)v->micro);
    } else {
        s_mic_txt[r][c][0] = '\0';
    }
    if (v->rat_max != 0u) {
        snprintf(s_rat_txt[r][c], sizeof(s_rat_txt[r][c]), "x%u", (unsigned)v->rat_max + 1u);
    } else {
        s_rat_txt[r][c][0] = '\0';
    }
}

/* 焦点步/选中 lane 的派生状态行（文本 + 颜色）；调用方做脏比较 */
static void status_build(const app_state_t *st, char *buf, size_t n, uint32_t *color)
{
    const step_window_t win = st->window;
    const uint8_t wlen = window_len_of(st);
    const uint8_t ft = st->focus_track < APP_STATE_TRACKS ? st->focus_track : 0u;
    const uint8_t fs = st->focus_step < wlen ? st->focus_step : 0u;
    const uint8_t abs = step_window_abs(&win, fs);
    static const char *const fill_tok[4] = {"-", "F", "N", "-"};
    static const char *const lvl_tok[4] = {"N", "G", "S", "H"};
    /* 文案与上游 drums.c 的 LANE_SHORT 一致（上游是 static 表、没有公共头；这里只做
     * 显示，不把 vendored 头引进 UI。改上游名字时同步这张 16 项表）。 */
    static const char *const lane_name[16] = {
        "KICK", "KICK2", "SNARE", "CLAP", "HAT", "OPEN", "PEDAL", "RIM",
        "SNR2", "TOM L", "TOM H", "CRASH", "RIDE", "SHAKE", "CONGA", "BELL"};

    if (ft == APP_STATE_TRACKS - 1u) {
        const sloop_dstep_t *d = &st->core.dstep_raw[fs];
        const uint16_t mask = (uint16_t)(d->on[0] | (uint16_t)d->on[1] << 8);
        const bool on = ((mask >> s_lane) & 1u) != 0;
        const uint8_t lvl = (uint8_t)((d->lvl[s_lane / 4u] >> (2u * (s_lane % 4u))) & 3u);
        const uint8_t rat = (uint8_t)((d->rat[s_lane / 4u] >> (2u * (s_lane % 4u))) & 3u);
        const uint8_t fill = st->core.dstep[fs].fill & 3u;
        const int8_t micro = (int8_t)st->core.dstep[fs].micro;
        snprintf(buf, n, "DRM S%u LANE %u %s %s LVL %s RAT x%u MIC %+d FILL %s",
                 (unsigned)(abs + 1), (unsigned)(s_lane + 1), lane_name[s_lane],
                 on ? "ON" : "OFF", lvl_tok[lvl], (unsigned)rat + 1u, (int)micro,
                 fill_tok[fill]);
        *color = ui_track_color(APP_STATE_TRACKS - 1u);
        return;
    }

    const sloop_core_snapshot_t *core = &st->core;
    const uint8_t time = core->step[ft][fs].time;
    const uint8_t cnt = core->step[ft][fs].n;
    uint8_t lvl_max = 0, rat_max = 0;
    for (uint8_t k = 0; k < cnt && k < 4u; k++) {
        uint8_t lvl = (uint8_t)((core->step[ft][fs].lvl >> (2u * k)) & 3u);
        uint8_t rat = (uint8_t)((core->step[ft][fs].rat >> (2u * k)) & 3u);
        if (lvl > lvl_max) lvl_max = lvl;
        if (rat > rat_max) rat_max = rat;
    }
    *color = ui_track_color(ft);
    if (time == STEP_TIME_TIE && !state_step_is_on(cnt, time)) {
        snprintf(buf, n, "T%u S%u TIE", (unsigned)(ft + 1), (unsigned)(abs + 1));
        return;
    }
    if (!state_step_is_on(cnt, time)) {
        snprintf(buf, n, "T%u S%u EMPTY", (unsigned)(ft + 1), (unsigned)(abs + 1));
        return;
    }
    int off = snprintf(buf, n, "T%u S%u NOTE", (unsigned)(ft + 1), (unsigned)(abs + 1));
    for (uint8_t k = 0; k < cnt && k < 4u && off > 0 && (size_t)off + 6 < n; k++) {
        char nm[6];
        note_name(nm, sizeof(nm), core->step[ft][fs].note[k]);
        off += snprintf(buf + off, n - (size_t)off, " %s%s", nm,
                        (core->step[ft][fs].flags & (STEP_FLAG_CONT0 << k)) ? "*" : "");
    }
    if (off > 0) {
        snprintf(buf + off, n - (size_t)off, " LVL %s RAT x%u MIC %+d FILL %s%s%s",
                 lvl_tok[lvl_max], (unsigned)rat_max + 1u, (int)(int8_t)core->step[ft][fs].micro,
                 fill_tok[core->step[ft][fs].fill & 3u],
                 (core->step[ft][fs].flags & STEP_FLAG_SLIDE) ? " SLD" : "",
                 (core->step[ft][fs].flags & STEP_FLAG_ACCENT) ? " ACC" : "");
    }
    return;
}

/* ------------------------------------------------------------ 动作（触摸/TAP 共用） --- */

/* 鼓 lane 位切换（控件回调与 TAP 的唯一共同路径）：lane < 0 = 当前选中 lane。
 * 读快照的原始 16 lane 字节 → 翻一位 → 整份 POD 下发 CMD_DRUM_STEP_WRITE。
 *
 * `st` 是**调用方的一份** AppState 缓冲：入口时是写前快照，返回时被同一缓冲刷新成
 * 写后快照（state_get_copy 复用）。这样输入任务栈上不再叠两份 ~600B 的 app_state_t
 * （输入任务栈 4KB，KEY STATS 的 input_stack_free 是证据口）。
 * bit/mask 取自**写后回读**：诊断冻结/非法时状态层不落地，echo 必须报写前真值 + err，
 * 不得按本地翻转结果伪造成功。err = 0 成功，否则是 app_seq_status_t。 */
static bool drum_toggle(app_state_t *st, uint8_t c, int lane, uint8_t *bit,
                        uint16_t *mask, uint8_t *err)
{
    if (c >= window_len_of(st)) return false; /* 隐藏列：不可点 */
    if (lane < 0) lane = s_lane;
    if (lane > 15) lane = 15;
    const uint8_t abs = step_window_abs(&st->window, c);
    sloop_dstep_t d = st->core.dstep_raw[c];
    uint16_t m = (uint16_t)(d.on[0] | (uint16_t)d.on[1] << 8);
    m ^= (uint16_t)(1u << lane);
    d.on[0] = (uint8_t)(m & 0xFFu);
    d.on[1] = (uint8_t)(m >> 8);
    state_set_focus(APP_STATE_TRACKS - 1u, c);
    const edit_cmd_t cmd = {.type = CMD_DRUM_STEP_WRITE, .b = abs, .p = &d};
    state_apply(&cmd);
    s_lane = (uint8_t)lane;
    state_get_copy(st); /* 同一缓冲刷新：写后真值（失败时投影未变，仍是原值） */
    const sloop_dstep_t *now = &st->core.dstep_raw[c];
    const uint16_t actual = (uint16_t)(now->on[0] | (uint16_t)now->on[1] << 8);
    if (bit != NULL) *bit = (uint8_t)((actual >> lane) & 1u);
    if (mask != NULL) *mask = actual;
    if (err != NULL) *err = st->seq_status == SEQ_STATUS_OK ? 0u : st->seq_status;
    return true;
}

/* 格激活（LVGL 上下文、调用方已持锁）：控件点击回调与 TAP 注入的**唯一共同路径**。
 * 两条路径的 EditCmd 序列因此由本函数唯一决定（ADR-005 的 EditCmd 层等价）。
 * 鼓轨分支调 drum_toggle()（TAP 的鼓轨路径直接调它，不经过本函数）。 */
static bool step_activate(uint8_t r, uint8_t c)
{
    app_state_t st;
    state_get_copy(&st);
    if (c >= window_len_of(&st)) return false; /* 隐藏列：不可点（TAP 命中表也不含） */
    if (r >= APP_STATE_TRACKS - 1u) {
        const bool ok = drum_toggle(&st, c, -1, NULL, NULL, NULL);
        refresh();
        return ok;
    }
    const step_window_t win = st.window;
    const uint8_t abs = step_window_abs(&win, c);
    state_set_focus(r, c); /* UI 局部态：焦点轨/窗口列 */
    const edit_cmd_t cmd = {.type = CMD_TOGGLE_STEP, .a = r, .b = abs};
    state_apply(&cmd);
    refresh();
    return true;
}

static void cell_clicked_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    (void)step_activate((uint8_t)(id / APP_STATE_STEPS), (uint8_t)(id % APP_STATE_STEPS));
}

/* TAP hit-test：矩形由建格所用的同一组宏（GRID_X0/Y0、CELL_W/H、CELL_STEP_X、
 * ROW_PITCH）现场推导——不另存一份坐标表，布局改动不会与注入漂移。列数取当前
 * 窗口（隐藏列不命中），out->b 回绝对步号（与 EditCmd 同口径）。
 * 鼓行：先按 4×4 lane 网格（DRUM_GRID_X0/Y0 与 DRUM_LANE_PITCH）判定是否点中某个 dot，
 * 点中 = 选中该 lane；否则用当前选中 lane。两条路径都切换该步的该位。 */
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
            const uint8_t abs = step_window_abs(&win, c);
            if (r == APP_STATE_TRACKS - 1u) {
                const int32_t lx = x - x0;
                const int32_t ly = y - y0;
                int lane = -1;
                if (lx >= DRUM_GRID_X0 && lx < DRUM_GRID_X0 + 4 * DRUM_LANE_PITCH &&
                    ly >= DRUM_GRID_Y0 && ly < DRUM_GRID_Y0 + 4 * DRUM_LANE_PITCH) {
                    lane = ((ly - DRUM_GRID_Y0) / DRUM_LANE_PITCH) * 4 +
                           ((lx - DRUM_GRID_X0) / DRUM_LANE_PITCH);
                }
                if (lane < 0) lane = s_lane; /* 格内其它位置：操作当前选中 lane */
                uint8_t bit = 0, err = 0;
                uint16_t mask = 0;
                if (!drum_toggle(&st, c, lane, &bit, &mask, &err)) return false;
                refresh();
                if (out != NULL) {
                    out->kind = SLOOP_TAP_STEP_DRUM;
                    out->a = (uint8_t)lane;
                    out->b = abs;
                    out->v = (int16_t)mask; /* 写后回读（失败时 = 写前真值） */
                    out->d = (int8_t)bit;
                    out->err = err;
                }
                return true;
            }
            (void)step_activate(r, c);
            if (out != NULL) {
                out->kind = SLOOP_TAP_STEP_CELL;
                out->a = r;
                out->b = abs;
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
    memset(s_cell_sig, 0, sizeof(s_cell_sig));
    memset(s_cell_view, 0, sizeof(s_cell_view));
    memset(s_nums, 0, sizeof(s_nums));
    s_overlay = NULL;
    s_header_dirty = true;
    s_win_dirty = true;
    s_status_dirty = true;

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

    /* 格内自绘层：一个透明、不可点的对象覆盖整个网格（在格之后创建 ⇒ 画在格上面）。
     * 64 格的 TIE/CONT/SLIDE/lvl/rat/micro/fill 与鼓 lane 网格都由它在 DRAW_MAIN
     * 里画；不为任何一格新增 label（ADR-008 的池预算）。 */
    s_overlay = lv_obj_create(parent);
    UI_BUILD_CHECK(s_overlay);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_set_clickable(s_overlay, false);
    lv_obj_set_pos(s_overlay, GRID_X0, GRID_Y0);
    lv_obj_set_size(s_overlay, GRID_W, 3 * ROW_PITCH + CELL_H);
    lv_obj_add_event_cb(s_overlay, overlay_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *hint = lv_label_create(parent);
    UI_BUILD_CHECK(hint);
    lv_label_set_text(hint,
                      "TAP = TOGGLE   DIM = BEYOND LEN   TIE = HOLD   A = ACCENT   S = SLIDE   "
                      "F = FILL   N = NO-FILL   m± = NUDGE   xN = RATCHET\n"
                      "NOTE: WHITE=NORM YELLOW=HARD GREY=SOFT DARK=GHOST  |  DRM: TAP DOT="
                      "SELECT+TOGGLE, TAP CELL=TOGGLE, UP/DN=LANE, ENTER=TOGGLE");
    lv_obj_add_style(hint, &s_st_hint, 0);
    lv_obj_set_pos(hint, 24, 604);
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
            if (s_nums[c] != NULL) {
                lv_label_set_text(s_nums[c], buf);
                lv_obj_set_hidden(s_nums[c], c >= wlen);
            }
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

    /* 状态行：聚焦步/选中 lane 的派生文本（P4-C Task 9；TAP 与键盘焦点变化都可见） */
    {
        char buf[96];
        uint32_t color = UI_TEXT_DIM;
        status_build(st, buf, sizeof(buf), &color);
        if (s_status_dirty || strcmp(buf, s_status_cache) != 0 ||
            color != s_status_color) {
            s_status_dirty = false;
            snprintf(s_status_cache, sizeof(s_status_cache), "%s", buf);
            s_status_color = color;
            if (s_status != NULL) {
                lv_label_set_text(s_status, buf);
                lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
            }
        }
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
            cell_view_t v;
            memset(&v, 0, sizeof(v));
            v.hidden = c >= wlen ? 1u : 0u;
            v.dim = (start + c) >= len ? 1u : 0u;
            bool on;
            if (r == APP_STATE_TRACKS - 1u) {
                /* 鼓轨：原始 16 lane 位图 + 该步 fill/micro（micro 取 dstep 聚合投影） */
                const sloop_dstep_t *d = &st->core.dstep_raw[c];
                v.kind = CELL_DRUM;
                v.on[0] = d->on[0];
                v.on[1] = d->on[1];
                memcpy(v.dlvl, d->lvl, sizeof(v.dlvl));
                for (uint8_t lane = 0; lane < 16u; lane++) {
                    if (((uint16_t)(d->on[0] | (uint16_t)d->on[1] << 8) >> lane & 1u) == 0) continue;
                    uint8_t rat = (uint8_t)((d->rat[lane / 4u] >> (2u * (lane % 4u))) & 3u);
                    if (rat > v.rat_max) v.rat_max = rat;
                }
                v.fill = st->core.dstep[c].fill & 3u;
                v.micro = st->core.dstep[c].micro;
                /* 鼓格底色保持「空格」样式：活动由 4×4 lane dot 表达。若整格按聚合 on
                 * 填满鼓轨橙色，lane dot 的 NORM 色（同一橙色）与格底同色、不可见。 */
                on = false;
            } else {
                const uint8_t n = st->core.step[r][c].n;
                const uint8_t time = st->core.step[r][c].time;
                v.n = n;
                memcpy(v.note, st->core.step[r][c].note, sizeof(v.note));
                v.flags = st->core.step[r][c].flags;
                v.lvl = st->core.step[r][c].lvl;
                v.rat = st->core.step[r][c].rat;
                v.micro = (uint8_t)st->core.step[r][c].micro;
                v.fill = st->core.step[r][c].fill & 3u;
                for (uint8_t k = 0; k < n && k < 4u; k++) {
                    uint8_t rat = (uint8_t)((v.rat >> (2u * k)) & 3u);
                    if (rat > v.rat_max) v.rat_max = rat;
                }
                if (time == STEP_TIME_TIE && !state_step_is_on(n, time)) {
                    v.kind = CELL_TIE;
                    on = false;
                } else if (state_step_is_on(n, time)) {
                    v.kind = CELL_NOTE;
                    on = true;
                } else {
                    v.kind = CELL_EMPTY;
                    on = false;
                }
            }

            uint8_t hl = HL_NONE;
            if (playhead == (int16_t)c) {
                hl = HL_PLAYHEAD;
            } else if (r == ft && c == fs) {
                hl = HL_FOCUS;
            }
            uint8_t want = (uint8_t)((on ? CELL_ON : 0u) | (hl << 1) |
                                     (v.dim ? CELL_DIM : 0u) |
                                     (v.hidden ? CELL_HIDDEN : 0u));
            if (want != s_cell_state[r][c]) {
                cell_apply(r, c, s_cell_state[r][c], want);
                s_cell_state[r][c] = want;
            }
            const uint32_t sig = view_sig(&v, r == APP_STATE_TRACKS - 1u ? s_lane : 0u);
            if (sig == s_cell_sig[r][c]) continue;
            s_cell_sig[r][c] = sig;
            s_cell_view[r][c] = v;
            view_texts(r, c, &v);
            cell_invalidate(r, c);
        }
    }
}

/* ------------------------------------------------------------ InputManager 路由 --- */

bool sloop_ui_step_page_active(void)
{
    return sloop_ui_current_page() == APP_PAGE_STEP;
}

uint8_t sloop_ui_step_lane_get(void)
{
    return s_lane;
}

void sloop_ui_step_lane_set(uint8_t lane)
{
    if (lane > 15u) return;
    s_lane = lane;
}

/* 焦点环：T1,T2,T3,L1..L16（上行 = -1、下行 = +1）。鼓轨上逐 lane，越界回合成轨；
 * lane 选择是 UI 局部态，不写引擎（页面由 30ms tick 重画 ring）。 */
void sloop_ui_step_focus_vertical(int8_t delta)
{
    if (delta == 0) return;
    app_state_t st;
    state_get_copy(&st);
    uint8_t wlen = window_len_of(&st);
    uint8_t track = st.focus_track < APP_STATE_TRACKS ? st.focus_track : 0u;
    uint8_t step = st.focus_step < wlen ? st.focus_step : 0u;
    if (track < APP_STATE_TRACKS - 1u) {
        int next = (int)track + delta;
        if (next < 0) {
            s_lane = 15u;
            state_set_focus(APP_STATE_TRACKS - 1u, step);
        } else if (next > (int)(APP_STATE_TRACKS - 2u)) {
            s_lane = 0u;
            state_set_focus(APP_STATE_TRACKS - 1u, step);
        } else {
            state_set_focus((uint8_t)next, step);
        }
        return;
    }
    int lane = (int)s_lane + delta;
    if (lane < 0) {
        state_set_focus(APP_STATE_TRACKS - 2u, step); /* L1 ↑ → T3 */
    } else if (lane > 15) {
        state_set_focus(0u, step); /* L16 ↓ → T1 */
    } else {
        s_lane = (uint8_t)lane;
        state_set_focus(APP_STATE_TRACKS - 1u, step);
    }
}

void sloop_ui_step_drum_toggle(void)
{
    app_state_t st; /* 本函数只保留一份快照：drum_toggle 复用同一缓冲（输入任务栈） */
    state_get_copy(&st);
    if (st.focus_track != APP_STATE_TRACKS - 1u) return;
    const uint8_t wlen = window_len_of(&st);
    const uint8_t col = st.focus_step < wlen ? st.focus_step : 0u;
    (void)drum_toggle(&st, col, (int)s_lane, NULL, NULL, NULL);
    /* 不在这里 refresh：本函数在输入任务上下文（不持 LVGL 锁），页面由 30ms
     * UI 定时器的 page_step_update 按新快照重画。 */
}
