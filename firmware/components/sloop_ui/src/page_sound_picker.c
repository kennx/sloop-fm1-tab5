/*
 * Sound 页的引擎 / 预置 / 鼓 kit 选择器（5B-1 Task 6；契约见 page_sound_picker.h）。
 *
 * 三个临时对象：面板（UI_PANEL 圆角 12 + 底部一行提示）、引擎 chip 行（合成轨；单对象自绘，chip 宽 =
 * 11px/字符 + 24、间距 8，12 个排不下 928px ⇒ 整行横向滚动，高亮 chip 始终留在行内）、列表（单对象
 * 自绘，行高 32、一屏 11 行，高亮行滚入）。几何只有下面一组宏：建对象、自绘与 TAP 命中同源。
 *
 * 状态分两半：逻辑态（active / 行 / 两个高亮 / 行数 / 打开时的上下文）是 portMUX 保护的 PSRAM 静态，
 * 输入任务（键盘路由，不取 LVGL 锁）与 LVGL 任务都会读写；对象指针与两个滚动量只在 LVGL 上下文碰。
 * 键盘改了高亮只置脏位，滚动量跟上高亮、重绘与「关闭后删对象」都发生在 page_sound_picker_tick（≤30ms）；
 * 唯一能单独动列表滚动量的是 console 的矩形查询（把不在视口里的行滚进来）。唯一例外是
 * 选择器自己的点击回调——那里不能删自己正在派发事件的对象，只清 active，对象留给 tick。
 *
 * 名字现取现画：每次绘制逐行调 sloop_core_*_name 拷进 s_text 并立刻交给 lv_draw_label
 * （text_local = 0 的前提同 page_sound.c：软件绘制单元就地 dispatch，返回前文本已消费）。
 */
#include "page_sound_picker.h"

#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "theme.h"

/* 面板与内部几何（页面区相对坐标；屏幕 y = +UI_TAB_BAR_H） */
#define PICK_X      160
#define PICK_Y      120
#define PICK_W      960
#define PICK_H      440
#define PICK_RADIUS 12
#define PICK_PAD    16
#define INNER_W     (PICK_W - 2 * PICK_PAD)
#define CHIP_Y      8
#define CHIP_H      40
#define CHIP_GAP    8
#define CHIP_CHAR_W 11 /* chip 宽 = 11px/字符 + 24（page_layout_chip_width 同一保守上界） */
#define CHIP_NAME_PAD 24
#define CHIP_EDGE   8 /* 高亮 chip 距行缘的留白 */
#define ROW_H       32
#define ROWS_VISIBLE 11
#define LIST_Y_SYNTH (CHIP_Y + CHIP_H + CHIP_GAP)
#define LIST_Y_DRUM 24
#define PICK_LIST_H      (ROW_H * ROWS_VISIBLE)
/* 鼓轨的 kit = P_E0（上游 DRUMS 屏 KNOB 1；与 page_params.c 的 sound_step 同一条命令） */
#define KIT_PARAM   (SLOOP_CORE_P_COUNT - 8u)
#define PICK_ENGINES 12u /* sloop_core_nengines()；chip 宽表的容量 */

/* 键盘焦点：最后移动的是 chip 行还是列表（ENTER 确认它） */
#define ZONE_CHIPS 0u
#define ZONE_LIST  1u

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR bool s_active;      /* 选择器打开（输入任务读、LVGL 任务写） */
static EXT_RAM_BSS_ATTR uint8_t s_row;      /* 0 = 引擎（鼓轨 = kit）/ 1 = 预置 */
static EXT_RAM_BSS_ATTR uint8_t s_zone;     /* ZONE_* */
static EXT_RAM_BSS_ATTR uint8_t s_chip_hl;  /* chip 高亮 = 预览的引擎（鼓轨 0） */
static EXT_RAM_BSS_ATTR uint8_t s_list_hl;  /* 列表高亮 */
static EXT_RAM_BSS_ATTR uint8_t s_rows;     /* 列表行数（row 1 随预览的引擎变） */
static EXT_RAM_BSS_ATTR bool s_dirty;       /* 高亮变过：下一拍失效重绘 */
/* 打开时的上下文：选中轨 + 它的声音快照（引擎/预置/kit 的当前项）；发命令前与 tick 里各比对一次 */
static EXT_RAM_BSS_ATTR uint8_t s_track;
static EXT_RAM_BSS_ATTR uint8_t s_kit;
static EXT_RAM_BSS_ATTR sloop_sound_snapshot_t s_snd;

/* 只在 LVGL 上下文（持锁的 console 外壳 / 事件回调 / 每拍）读写 */
static EXT_RAM_BSS_ATTR lv_obj_t *s_parent; /* 页面对象根 */
static EXT_RAM_BSS_ATTR lv_obj_t *s_panel;
static EXT_RAM_BSS_ATTR lv_obj_t *s_chips;
static EXT_RAM_BSS_ATTR lv_obj_t *s_list;
static EXT_RAM_BSS_ATTR uint8_t s_scroll;        /* 列表首行的下标 */
static EXT_RAM_BSS_ATTR int16_t s_chip_scroll;   /* chip 行横向滚动量 */
static EXT_RAM_BSS_ATTR int16_t s_chip_w[PICK_ENGINES];
static EXT_RAM_BSS_ATTR char s_text[32];

/* 提示行（R13 的规则写在这里；固件只带 Montserrat：必须全 ASCII）——引擎 / 预置 / kit */
static const char *const k_hints[3] = {
    "Left/Right or Up/Down: move | ENTER or tap a row: use engine | ESC: close",
    "Up/Down: move | ENTER or tap a row: load preset | Chip: preview, again: switch engine | ESC: close",
    "Up/Down: move | ENTER or tap a row: use kit | ESC: close",
};

typedef struct {
    uint8_t row, zone, chip_hl, list_hl, rows, track, kit;
    bool drum;
    uint8_t engine, preset; /* 打开时的当前引擎 / 预置 */
} pick_view_t;

/* 逻辑态的一次性读（临界区内整份拷出） */
static bool view_get(pick_view_t *v)
{
    *v = (pick_view_t){0};
    portENTER_CRITICAL(&s_mux);
    bool on = s_active;
    if (on) {
        *v = (pick_view_t){.row = s_row,        .zone = s_zone,       .chip_hl = s_chip_hl,
                           .list_hl = s_list_hl, .rows = s_rows,       .track = s_track,
                           .kit = s_kit,         .drum = s_snd.drum != 0u,
                           .engine = s_snd.engine, .preset = s_snd.preset};
    }
    portEXIT_CRITICAL(&s_mux);
    return on;
}

static void deactivate(void)
{
    portENTER_CRITICAL(&s_mux);
    s_active = false;
    portEXIT_CRITICAL(&s_mux);
}

static bool rect_hit(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static uint8_t wrap(int32_t v, uint8_t n)
{
    if (n == 0u) return 0u;
    while (v < 0) v += n;
    while (v >= n) v -= n;
    return (uint8_t)v;
}

static int16_t list_y(bool drum)
{
    return drum ? LIST_Y_DRUM : LIST_Y_SYNTH;
}

/* ------------------------------------------------------------------ 自绘 --- */
static void draw_text(lv_layer_t *layer, const lv_font_t *font, uint32_t color, lv_text_align_t align,
                      const lv_area_t *area, const char *text)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.layer = layer;
    d.font = font;
    d.color = lv_color_hex(color);
    d.align = align;
    d.text = text;
    lv_draw_label(layer, &d, area);
}

static void draw_box(lv_layer_t *layer, const lv_area_t *a, uint32_t color, int32_t radius)
{
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.base.layer = layer;
    rect.bg_color = lv_color_hex(color);
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = radius;
    lv_draw_rect(layer, &rect, a);
}

/* 第 e 个 chip 的左缘（行内坐标，未减滚动）与整行宽度 */
static int16_t chip_x(uint8_t e)
{
    int16_t x = 0;
    for (uint8_t i = 0; i < e && i < PICK_ENGINES; i++) x = (int16_t)(x + s_chip_w[i] + CHIP_GAP);
    return x;
}

static int16_t chips_total(void)
{
    int16_t w = chip_x(sloop_core_nengines());
    return w > 0 ? (int16_t)(w - CHIP_GAP) : 0;
}

/* 高亮 chip 留在行内（粘滞：只在它越出留白带时才滚；总宽放得下 = 0） */
static void chips_scroll(const pick_view_t *v)
{
    int16_t max = (int16_t)(chips_total() - INNER_W);
    if (max <= 0) {
        s_chip_scroll = 0;
        return;
    }
    int16_t x1 = chip_x(v->chip_hl), x2 = (int16_t)(x1 + s_chip_w[v->chip_hl]);
    if (x1 < s_chip_scroll + CHIP_EDGE) {
        s_chip_scroll = (int16_t)(x1 - CHIP_EDGE);
    } else if (x2 > s_chip_scroll + INNER_W - CHIP_EDGE) {
        s_chip_scroll = (int16_t)(x2 - INNER_W + CHIP_EDGE);
    }
    if (s_chip_scroll < 0) s_chip_scroll = 0;
    if (s_chip_scroll > max) s_chip_scroll = max;
}

/* 行内坐标（已含滚动）落在哪个 chip；不在任何 chip 上 = 引擎数 */
static uint8_t chip_at(int16_t x)
{
    uint8_t n = sloop_core_nengines();
    for (uint8_t e = 0; e < n && e < PICK_ENGINES; e++) {
        int16_t x1 = chip_x(e);
        if (x >= x1 && x < x1 + s_chip_w[e]) return e;
    }
    return n;
}

/* 高亮行留在列表里（粘滞；只由 scroll_sync 调） */
static void list_scroll(const pick_view_t *v)
{
    if (v->list_hl < s_scroll) s_scroll = v->list_hl;
    else if (v->list_hl >= s_scroll + ROWS_VISIBLE) s_scroll = (uint8_t)(v->list_hl - ROWS_VISIBLE + 1);
}

/* 列表第 n 行的文字：预置 `"%2u  %s"`（1 起），引擎 / kit 纯名字 */
static void item_text(const pick_view_t *v, uint8_t n, char *buf, uint32_t buf_n)
{
    if (buf == NULL || buf_n == 0u) return;
    buf[0] = '\0';
    if (v->drum) {
        (void)sloop_core_kit_name(n, buf, buf_n);
    } else if (v->row == 0u) {
        (void)sloop_core_engine_name(n, buf, buf_n);
    } else {
        char name[32];
        if (sloop_core_preset_name(v->chip_hl, n, name, sizeof(name))) {
            snprintf(buf, buf_n, "%2u  %s", (unsigned)(n + 1u), name);
        }
    }
}

static void panel_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.base.layer = layer;
    rect.bg_color = lv_color_hex(UI_PANEL);
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = PICK_RADIUS;
    /* 描边：卡片也是 UI_PANEL 底，没有这一圈面板的边就看不出来 */
    rect.border_color = lv_color_hex(UI_PANEL_HI);
    rect.border_width = 2;
    rect.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &rect, &a);

    pick_view_t v;
    if (!view_get(&v)) return;
    lv_area_t ta = {.x1 = a.x1 + PICK_PAD, .y1 = a.y1 + list_y(v.drum) + PICK_LIST_H + 6, .x2 = a.x2 - PICK_PAD, .y2 = a.y2};
    draw_text(layer, &lv_font_montserrat_14, UI_TEXT_DIM, LV_TEXT_ALIGN_LEFT, &ta,
              k_hints[v.drum ? 2 : v.row > 0u ? 1 : 0]);
}

static void chips_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    pick_view_t v;
    if (!view_get(&v)) return;

    uint8_t n = sloop_core_nengines();
    for (uint8_t eng = 0; eng < n && eng < PICK_ENGINES; eng++) {
        int32_t x1 = a.x1 + chip_x(eng) - s_chip_scroll, x2 = x1 + s_chip_w[eng] - 1;
        if (x2 < a.x1 || x1 > a.x2) continue; /* 滚出行外的 chip：LVGL 本就按对象矩形裁剪，这里只是省一次绘制 */
        bool hl = eng == v.chip_hl;
        lv_area_t ca = {.x1 = x1, .y1 = a.y1, .x2 = x2, .y2 = a.y2};
        draw_box(layer, &ca, hl ? UI_ACCENT : UI_PANEL_HI, 10);
        (void)sloop_core_engine_name(eng, s_text, sizeof(s_text));
        lv_area_t ta = {.x1 = x1, .y1 = a.y1 + 10, .x2 = x2, .y2 = a.y2};
        draw_text(layer, &lv_font_montserrat_16, hl ? UI_BG : UI_TEXT, LV_TEXT_ALIGN_CENTER, &ta, s_text);
    }
}

static void list_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    pick_view_t v;
    if (!view_get(&v)) return;
    uint8_t cur = v.drum ? v.kit : v.row == 0u ? v.engine : v.preset;

    for (uint8_t i = 0; i < ROWS_VISIBLE; i++) {
        uint8_t n = (uint8_t)(s_scroll + i);
        if (n >= v.rows) break;
        int32_t y1 = a.y1 + (int32_t)i * ROW_H;
        if (n == v.list_hl) {
            lv_area_t ra = {.x1 = a.x1, .y1 = y1, .x2 = a.x2, .y2 = y1 + ROW_H - 1};
            draw_box(layer, &ra, UI_PANEL_HI, 6);
        }
        item_text(&v, n, s_text, sizeof(s_text));
        lv_area_t ta = {.x1 = a.x1 + 12, .y1 = y1 + 6, .x2 = a.x2, .y2 = y1 + ROW_H};
        draw_text(layer, &lv_font_montserrat_16, n == cur ? UI_ACCENT : n == v.list_hl ? UI_TEXT : UI_TEXT_DIM,
                  LV_TEXT_ALIGN_LEFT, &ta, s_text);
    }
}

/* ------------------------------------------------------------- 对象生命周期 --- */
static lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj != NULL) {
        lv_obj_remove_style_all(obj);
        lv_obj_set_scrollable(obj, false);
    }
    return obj;
}

/* 删面板（子对象随之删除）。只在 LVGL 上下文、且不在选择器自己的事件回调里调。 */
static void destroy(void)
{
    if (s_panel != NULL) lv_obj_delete(s_panel);
    s_panel = NULL;
    s_chips = NULL;
    s_list = NULL;
}

static void invalidate_all(void)
{
    if (s_chips != NULL) lv_obj_invalidate(s_chips);
    if (s_list != NULL) lv_obj_invalidate(s_list);
}

/* 滚动量只有一个主人：高亮变了才在这里跟着高亮走（粘滞），绘制与命中只读它——否则「矩形查询把
 * 第 n 行滚进来」之后，命中再按高亮重算一次滚动，报出去的矩形就不是 TAP 落下的那一行。 */
static void scroll_sync(void)
{
    pick_view_t v;
    if (!view_get(&v)) return;
    chips_scroll(&v);
    list_scroll(&v);
}

/* LVGL 上下文：已关闭 ⇒ 删对象；高亮变过 ⇒ 滚动量跟上高亮并失效重绘 */
static void flush(void)
{
    portENTER_CRITICAL(&s_mux);
    bool on = s_active;
    bool dirty = s_dirty;
    s_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    if (!on) {
        destroy();
    } else if (dirty) {
        scroll_sync();
        invalidate_all();
    }
}

/* ------------------------------------------------------------------ 动作 --- */
static void fill_result(sloop_tap_result_t *out, uint8_t kind, uint8_t index, bool sent, uint8_t err, bool drum)
{
    if (out == NULL) return;
    *out = (sloop_tap_result_t){.kind = SLOOP_TAP_SOUND_PICK,
                                .a = kind,
                                .b = index,
                                .v = sent ? (int16_t)index : 0,
                                .d = drum ? 1 : 0,
                                .err = err};
}

/* 选择项的提交（列表行 / 已高亮的 chip / ENTER 共用）。kind 0 = 引擎（鼓轨 = kit），1 = 预置。
 * 不下发的三种情形：打开后上下文已变；确认的就是当前项（引擎 = 默认值 + 预置 0，预置 = 整份声音，
 * 误触 ENTER 不该把当前声音复位）；状态层拒（冻结/越界）。发了命令回 true。无论哪种都关闭。 */
static bool pick_commit(uint8_t kind, uint8_t index, sloop_tap_result_t *out)
{
    pick_view_t v;
    if (!view_get(&v)) {
        fill_result(out, kind, index, false, SLOOP_PICK_ERR_CONTEXT, false);
        return false;
    }
    uint8_t err = SLOOP_PICK_ERR_NONE;
    if (state_selected_track() != v.track || (!v.drum && state_selected_engine() != v.engine)) {
        err = SLOOP_PICK_ERR_CONTEXT;
    } else if (index == (kind == 1u ? v.preset : v.drum ? v.kit : v.engine)) {
        err = SLOOP_PICK_ERR_CURRENT;
    } else {
        edit_cmd_t cmd = {0};
        if (kind == 1u) {
            cmd.type = CMD_SET_PRESET;
            cmd.a = v.track;
            cmd.v = index;
        } else if (v.drum) {
            cmd.type = CMD_SET_PARAM;
            cmd.a = v.track;
            cmd.b = KIT_PARAM;
            cmd.v = index;
        } else {
            cmd.type = CMD_SET_ENGINE;
            cmd.a = v.track;
            cmd.v = index;
        }
        state_apply(&cmd);
        uint8_t st = v.drum ? (sloop_seq_frozen() ? APP_SOUND_STATUS_FROZEN : APP_SOUND_STATUS_OK)
                            : state_sound_status();
        if (st == APP_SOUND_STATUS_FROZEN) err = SLOOP_PICK_ERR_FROZEN;
        else if (st != APP_SOUND_STATUS_OK) err = SLOOP_PICK_ERR_INVALID;
    }
    deactivate();
    fill_result(out, kind, index, err == SLOOP_PICK_ERR_NONE, err, v.drum);
    return err == SLOOP_PICK_ERR_NONE;
}

/* 列表第 n 行的选择：预览别的引擎时点预置行不下发（换引擎与换预置是两条命令），面板保持打开 */
static bool list_select(uint8_t n, sloop_tap_result_t *out)
{
    pick_view_t v;
    if (!view_get(&v)) {
        fill_result(out, 1u, n, false, SLOOP_PICK_ERR_CONTEXT, false);
        return false;
    }
    if (v.row == 1u && v.chip_hl != v.engine) {
        fill_result(out, 1u, n, false, SLOOP_PICK_ERR_PREVIEW, false);
        return false;
    }
    return pick_commit(v.row, n, out);
}

/* chip：没高亮 ⇒ 只高亮（row 1 的列表换成该引擎的预置）；已高亮 ⇒ 确认换引擎 */
static bool chip_select(uint8_t eng, sloop_tap_result_t *out)
{
    pick_view_t v;
    if (!view_get(&v)) {
        fill_result(out, 0u, eng, false, SLOOP_PICK_ERR_CONTEXT, false);
        return false;
    }
    if (eng == v.chip_hl) return pick_commit(0u, eng, out);
    uint8_t rows = v.row == 1u ? sloop_core_npresets(eng) : v.rows;
    portENTER_CRITICAL(&s_mux);
    s_chip_hl = eng;
    s_zone = ZONE_CHIPS;
    s_rows = rows;
    /* row 0 的列表就是引擎：列表高亮跟着 chip 走；row 1 换了预览的引擎 ⇒ 预置高亮回到第 1 个 */
    s_list_hl = s_row == 0u ? eng : 0u;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
    fill_result(out, 0u, eng, false, SLOOP_PICK_ERR_HIGHLIGHT, false);
    return false;
}

/* ---------------------------------------------------------------- 入口（LVGL） --- */
static void panel_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) panel_draw(e);
    /* 点击落在面板空白处：面板可点击，事件到此为止（不传给被盖住的卡片区） */
}

static void chips_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        chips_draw(e);
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) break;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_target_obj(e), &a);
        pick_view_t v;
        if (!view_get(&v)) break;
        uint8_t eng = chip_at((int16_t)(p.x - a.x1 + s_chip_scroll));
        if (eng < sloop_core_nengines()) {
            (void)chip_select(eng, NULL);
            invalidate_all(); /* 若已确认关闭：对象留给下一拍 tick 删 */
        }
        break;
    }
    default:
        break;
    }
}

static void list_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        list_draw(e);
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) break;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_target_obj(e), &a);
        pick_view_t v;
        if (!view_get(&v)) break;
        uint8_t n = (uint8_t)((p.y - a.y1) / ROW_H + s_scroll);
        if (n < v.rows) {
            (void)list_select(n, NULL);
            invalidate_all();
        }
        break;
    }
    default:
        break;
    }
}

void page_sound_picker_attach(lv_obj_t *parent)
{
    s_parent = parent;
}

void page_sound_picker_forget(void)
{
    deactivate();
    s_parent = NULL;
    s_panel = NULL;
    s_chips = NULL;
    s_list = NULL;
}

void page_sound_picker_open(uint8_t row)
{
    if (s_parent == NULL) return;
    sloop_sound_snapshot_t snd;
    state_read_sound(&snd);
    if (snd.drum && row != 0u) return; /* 鼓轨没有 PRESET 行 */
    uint8_t track = state_selected_track();
    uint8_t neng = sloop_core_nengines();
    int16_t chip_w[PICK_ENGINES];
    for (uint8_t e = 0; e < PICK_ENGINES; e++) {
        uint8_t len = 0;
        if (e < neng && sloop_core_engine_name(e, s_text, sizeof(s_text))) len = (uint8_t)strlen(s_text);
        chip_w[e] = (int16_t)(CHIP_CHAR_W * len + CHIP_NAME_PAD);
    }
    int16_t kit = track < APP_STATE_TRACKS ? state_read_p(KIT_PARAM) : 0;

    deactivate();
    destroy(); /* 已开着：关掉重建，高亮回到当前项 */

    s_panel = make_plain(s_parent);
    if (s_panel == NULL) return;
    lv_obj_set_pos(s_panel, PICK_X, PICK_Y);
    lv_obj_set_size(s_panel, PICK_W, PICK_H);
    lv_obj_set_clickable(s_panel, true);
    lv_obj_add_event_cb(s_panel, panel_event_cb, LV_EVENT_ALL, NULL);

    if (!snd.drum) {
        s_chips = make_plain(s_panel);
        if (s_chips == NULL) {
            destroy();
            return;
        }
        lv_obj_set_pos(s_chips, PICK_PAD, CHIP_Y);
        lv_obj_set_size(s_chips, INNER_W, CHIP_H);
        lv_obj_set_clickable(s_chips, true);
        lv_obj_add_event_cb(s_chips, chips_event_cb, LV_EVENT_ALL, NULL);
    }
    s_list = make_plain(s_panel);
    if (s_list == NULL) {
        destroy();
        return;
    }
    lv_obj_set_pos(s_list, PICK_PAD, (int16_t)list_y(snd.drum != 0u));
    lv_obj_set_size(s_list, INNER_W, PICK_LIST_H);
    lv_obj_set_clickable(s_list, true);
    lv_obj_add_event_cb(s_list, list_event_cb, LV_EVENT_ALL, NULL);

    memcpy(s_chip_w, chip_w, sizeof(s_chip_w));
    s_snd = snd;
    uint8_t rows = snd.drum ? sloop_core_nkits() : row == 0u ? neng : snd.npresets;
    uint8_t hl = (uint8_t)(snd.drum ? kit : row == 0u ? snd.engine : snd.preset);
    portENTER_CRITICAL(&s_mux);
    s_row = row;
    s_track = track;
    s_kit = (uint8_t)(kit < 0 ? 0 : kit > 255 ? 255 : kit);
    s_chip_hl = snd.engine;
    s_rows = rows;
    s_list_hl = hl < rows ? hl : 0u;
    s_zone = row == 0u ? ZONE_CHIPS : ZONE_LIST;
    s_scroll = 0;
    s_chip_scroll = 0;
    s_dirty = false;
    s_active = true;
    portEXIT_CRITICAL(&s_mux);

    scroll_sync();
}

void page_sound_picker_close(void)
{
    deactivate();
    flush();
}

void page_sound_picker_cancel(void)
{
    deactivate();
}

bool page_sound_picker_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool on = s_active;
    portEXIT_CRITICAL(&s_mux);
    return on;
}

void page_sound_picker_tick(const app_state_t *st)
{
    portENTER_CRITICAL(&s_mux);
    bool on = s_active;
    portEXIT_CRITICAL(&s_mux);
    if (on && st != NULL) {
        uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0u;
        /* 选中轨或它的声音（引擎/预置/kit）变了：列表说的不再是当前轨，关掉 */
        portENTER_CRITICAL(&s_mux);
        bool same = track == s_track && memcmp(&st->sound, &s_snd, sizeof(s_snd)) == 0;
        portEXIT_CRITICAL(&s_mux);
        if (!same) deactivate();
    }
    flush();
}

/* TAP / 页级 ±（页面区相对坐标 → 页面层换算；这里只收页面区坐标） */
bool page_sound_picker_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    pick_view_t v;
    if (s_panel == NULL || !view_get(&v)) return false;
    if (!rect_hit(x, y, PICK_X, PICK_Y, PICK_W, PICK_H)) return false;
    int32_t ly = PICK_Y + list_y(v.drum);
    if (!v.drum && rect_hit(x, y, PICK_X + PICK_PAD, PICK_Y + CHIP_Y, INNER_W, CHIP_H)) {
        uint8_t eng = chip_at((int16_t)(x - (PICK_X + PICK_PAD) + s_chip_scroll));
        if (eng < sloop_core_nengines()) (void)chip_select(eng, out);
    } else if (rect_hit(x, y, PICK_X + PICK_PAD, ly, INNER_W, PICK_LIST_H)) {
        uint8_t n = (uint8_t)((y - ly) / ROW_H + s_scroll);
        if (n < v.rows) (void)list_select(n, out);
    }
    flush(); /* 面板内的空白处同样吞掉；已关闭 ⇒ 立即删对象（TAP 不在选择器的事件回调里） */
    return true;
}

bool page_sound_picker_plus_tap(int8_t delta, sloop_tap_result_t *out)
{
    pick_view_t v;
    if (!view_get(&v)) return false;
    page_sound_picker_move(0, (int8_t)-delta); /* + = 上一行，同 ↑（页级 ± 与 ↑/↓ 同是调值口径） */
    (void)view_get(&v);
    fill_result(out, v.row, v.list_hl, false, SLOOP_PICK_ERR_HIGHLIGHT, v.drum);
    flush();
    return true;
}

/* ------------------------------------------------------- console（LVGL 锁内） --- */
esp_err_t page_sound_picker_rect(uint8_t n, int16_t *x, int16_t *y, int16_t *w, int16_t *h)
{
    if (x != NULL) *x = 0;
    if (y != NULL) *y = 0;
    if (w != NULL) *w = 0;
    if (h != NULL) *h = 0;
    pick_view_t v;
    if (s_list == NULL || !view_get(&v)) return ESP_ERR_INVALID_STATE;
    if (n >= v.rows) return ESP_ERR_NOT_FOUND;
    /* 不在视口里的行先滚进来：报一个 TAP 够不着的屏幕外坐标没有意义。此后高亮再变，滚动量才
     * 回去跟着高亮走（flush 的 scroll_sync） */
    if (n < s_scroll) s_scroll = n;
    else if (n >= s_scroll + ROWS_VISIBLE) s_scroll = (uint8_t)(n - ROWS_VISIBLE + 1);
    lv_obj_invalidate(s_list);
    lv_area_t a;
    lv_obj_get_coords(s_list, &a);
    if (x != NULL) *x = (int16_t)a.x1;
    if (y != NULL) *y = (int16_t)(a.y1 + (int32_t)(n - s_scroll) * ROW_H);
    if (w != NULL) *w = INNER_W;
    if (h != NULL) *h = ROW_H;
    return ESP_OK;
}

esp_err_t page_sound_picker_info(uint8_t n, char *buf, uint32_t buf_n, bool *current)
{
    if (buf != NULL && buf_n != 0u) buf[0] = '\0';
    if (current != NULL) *current = false;
    pick_view_t v;
    if (s_list == NULL || !view_get(&v)) return ESP_ERR_INVALID_STATE;
    if (n >= v.rows) return ESP_ERR_NOT_FOUND;
    item_text(&v, n, buf, buf_n);
    if (current != NULL) *current = n == (v.drum ? v.kit : v.row == 0u ? v.engine : v.preset);
    return ESP_OK;
}

void page_sound_picker_status(uint8_t *row, uint8_t *engine, uint8_t *highlight, uint8_t *rows)
{
    pick_view_t v;
    (void)view_get(&v);
    if (row != NULL) *row = v.row;
    if (engine != NULL) *engine = v.engine;
    if (highlight != NULL) *highlight = v.chip_hl;
    if (rows != NULL) *rows = v.rows;
}

/* ---------------------------------------------------- 键盘（输入任务，无锁） --- */
void page_sound_picker_move(int8_t dx, int8_t dy)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_active) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (dx != 0 && !s_snd.drum) {
        uint8_t eng = wrap((int32_t)s_chip_hl + dx, sloop_core_nengines());
        s_chip_hl = eng;
        s_zone = ZONE_CHIPS;
        if (s_row == 1u) {
            s_list_hl = 0u;
            s_rows = sloop_core_npresets(eng);
        } else {
            s_list_hl = eng; /* row 0 的列表就是引擎：两个高亮是同一个下标 */
        }
        s_dirty = true;
    } else if (dy != 0) {
        uint8_t n = s_rows;
        s_list_hl = wrap((int32_t)s_list_hl + dy, n);
        s_zone = ZONE_LIST;
        if (s_row == 0u && !s_snd.drum) s_chip_hl = s_list_hl;
        s_dirty = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool page_sound_picker_confirm(void)
{
    pick_view_t v;
    if (!view_get(&v)) return false;
    if (v.zone == ZONE_CHIPS && !v.drum) return pick_commit(0u, v.chip_hl, NULL);
    return list_select(v.list_hl, NULL);
}
