/*
 * 描述符驱动的参数页（ADR-008 §8）：布局表 8 槽旋钮 + 页眉 + ± 细调。
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H = 72）：
 * - 页眉：上下文行 (16, 6) 20pt（GLOBAL / TRACK n / ANALOG · TRACK n；引擎不符时红字
 *   "… USES ENG k — N/A"）；
 * - ± 细调：右上 (−) (1096,2) / (+) (1184,2)，80×40，作用于**焦点槽**（点旋钮设焦点）；
 * - 旋钮网格：4 列 × 2 行，cell 300×290，x = 16 + c·316，y = 52 + r·298
 *   （旋钮盒 160×160 在单元内 (70,26)：弧线 + 名称/数值两行文本，自绘见 knob.c）。
 *
 * 值路径（唯一）：槽位值 → 状态层命令 → 引擎（钳位在引擎）→ 立即回读投影刷新本页。
 * 触摸按下/拖动按角度设值（粗调，knob.c）；± 与键盘 ↑/↓ 是 1 个原始单位（细调）。
 * TAP 注入与控件回调共用同一动作函数（param_step/slot_write），只是 TAP 不能拖拽。
 *
 * 引擎页的适用性：槽位描述符取**页声明的引擎**（布局表），值是 trk[选中轨].p[P_E0+i]；
 * 选中轨的引擎与页不符（或选中鼓轨）时页面不可编辑、显示 --，避免拿 ANALOG 的 label
 * 写另一个引擎的槽位。
 */
#include "page_params.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "knob.h"
#include "page_layout.h"
#include "theme.h"

/* 网格/控件几何（TAP 命中矩形与建控件同源） */
#define HEADER_X 16
#define HEADER_Y 6
#define GRID_X0  16
#define GRID_Y0  52
#define CELL_W   300
#define CELL_H   290
#define CELL_GAP 16
#define ROW_GAP  8
#define CELL_PITCH (CELL_W + CELL_GAP) /* 316 */
#define ROW_PITCH  (CELL_H + ROW_GAP)  /* 298 */
#define BTN_W     80
#define BTN_H     40
#define BTN_Y     2
#define BTN_DEC_X 1096
#define BTN_INC_X 1184

/* 引擎槽在 trk[].p[] 里的绝对 id 起点（上游 P_E0 = P_COUNT - 8） */
#define PARAM_ENGINE_BASE (SLOOP_CORE_P_COUNT - 8u)

typedef struct {
    knob_t knobs[PARAM_PAGE_SLOTS];
    bool present[PARAM_PAGE_SLOTS]; /* 该槽有描述符且已建控件 */
    lv_obj_t *header;
    lv_obj_t *btn_dec;
    lv_obj_t *btn_inc;
    uint8_t shown_track;
    uint8_t shown_engine;
    int8_t shown_locked;  /* -1 = 尚未渲染上下文 */
    uint8_t focus_shown;  /* 最近渲染的焦点槽 */
    bool ctx_valid;
} page_ui_t;

static page_ui_t s_ui[PARAM_PAGE_COUNT];

/* 焦点槽（UI 局部态）：TAP 在 LVGL 上下文写、键盘在输入任务读/写 → 临界区保护。 */
static portMUX_TYPE s_focus_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_focus[PARAM_PAGE_COUNT];

/* 旋钮的页面上下文编码进 knob_t.user（页下标 << 4 | 槽位） */
#define KNOB_CTX(page, slot) ((void *)(uintptr_t)(((unsigned)(page) << 4) | (unsigned)(slot)))
static uint8_t ctx_page(const void *p)
{
    return (uint8_t)((uintptr_t)p >> 4);
}
static uint8_t ctx_slot(const void *p)
{
    return (uint8_t)((uintptr_t)p & 0x0Fu);
}

static uint8_t focus_get(uint8_t index)
{
    portENTER_CRITICAL(&s_focus_mux);
    uint8_t f = s_focus[index];
    portEXIT_CRITICAL(&s_focus_mux);
    return f < PARAM_PAGE_SLOTS ? f : 0u;
}

static void focus_set(uint8_t index, uint8_t slot)
{
    if (index >= PARAM_PAGE_COUNT || slot >= PARAM_PAGE_SLOTS) return;
    portENTER_CRITICAL(&s_focus_mux);
    s_focus[index] = slot;
    portEXIT_CRITICAL(&s_focus_mux);
}

/* ---------------------------------------------------------------- 槽位解析 --- */
static bool slot_desc(const param_page_t *pg, uint8_t slot, sloop_desc_t *out)
{
    const param_slot_t *s = &pg->slots[slot];
    if (s->id == 0xFF) return false;
    return sloop_core_desc((sloop_desc_scope_t)s->scope, s->engine, s->id, out);
}

/* 页声明的引擎（ENGINE 槽位的 engine 字段）；非引擎页返回 0xFF */
static uint8_t page_engine(const param_page_t *pg)
{
    for (uint8_t i = 0; i < PARAM_PAGE_SLOTS; i++) {
        if (pg->slots[i].id != 0xFF && pg->slots[i].scope == SLOOP_DESC_ENGINE) {
            return pg->slots[i].engine;
        }
    }
    return 0xFF;
}

/* 引擎页不可编辑：选中鼓轨（没有引擎槽）或所选轨引擎与页声明不符 */
static bool page_locked(const param_page_t *pg, const app_state_t *st)
{
    uint8_t eng = page_engine(pg);
    if (eng == 0xFF) return false;
    uint8_t track = st->core.selected_track;
    if (track >= 3u) return true;
    return st->core.trk[track].engine != eng;
}

/* 槽位当前值（GLOBAL→g[]、TRACK→p[]、ENGINE→p[P_E0+id]）；不可读返回 false */
static bool slot_value(const param_page_t *pg, uint8_t slot, const app_state_t *st, int16_t *out)
{
    const param_slot_t *s = &pg->slots[slot];
    if (s->id == 0xFF || out == NULL) return false;
    switch (s->scope) {
    case SLOOP_DESC_GLOBAL:
        if (s->id >= SLOOP_CORE_G_COUNT) return false;
        *out = st->g[s->id];
        return true;
    case SLOOP_DESC_TRACK:
        if (s->id >= PARAM_ENGINE_BASE) return false;
        *out = st->p[s->id];
        return true;
    case SLOOP_DESC_ENGINE:
        if (s->id >= 8u) return false;
        *out = st->p[PARAM_ENGINE_BASE + s->id];
        return true;
    default:
        return false;
    }
}

/* 槽位参数在状态层命令里的 id：GLOBAL 是全局 id，TRACK 是 TP id，ENGINE 是 P_E0+i */
static uint8_t slot_abs_id(const param_page_t *pg, uint8_t slot)
{
    const param_slot_t *s = &pg->slots[slot];
    return s->scope == SLOOP_DESC_ENGINE ? (uint8_t)(PARAM_ENGINE_BASE + s->id) : s->id;
}

/* 写槽位值（状态层命令 + 引擎钳位）；返回 false = 页面不可编辑/槽位不可读。
 * applied（可 NULL）回填写后投影里的真值（钳位结果）。 */
static bool slot_write(uint8_t index, uint8_t slot, int16_t value, int16_t *applied)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || slot >= PARAM_PAGE_SLOTS) return false;
    app_state_t st;
    state_get_copy(&st);
    if (page_locked(pg, &st)) return false;
    const param_slot_t *s = &pg->slots[slot];
    if (s->id == 0xFF) return false;

    edit_cmd_t cmd = {0};
    if (s->scope == SLOOP_DESC_GLOBAL) {
        cmd.type = CMD_SET_GLOBAL;
        cmd.a = s->id;
        cmd.v = (uint16_t)value;
    } else if (s->scope == SLOOP_DESC_TRACK || s->scope == SLOOP_DESC_ENGINE) {
        cmd.type = CMD_SET_PARAM;
        cmd.a = st.core.selected_track;
        cmd.b = slot_abs_id(pg, slot);
        cmd.v = (uint16_t)value;
    } else {
        return false;
    }
    state_apply(&cmd);
    if (applied != NULL) {
        state_get_copy(&st);
        int16_t v;
        *applied = slot_value(pg, slot, &st, &v) ? v : value;
    }
    return true;
}

static void refresh(uint8_t index)
{
    app_state_t st;
    state_get_copy(&st);
    page_params_update(index, &st);
}

/* ---------------------------------------------------------------- 上下文/焦点 --- */
static void update_header(uint8_t index, const param_page_t *pg, const app_state_t *st, uint8_t track,
                          bool locked)
{
    page_ui_t *ui = &s_ui[index];
    if (ui->header == NULL) return;
    char text[64];
    uint8_t scope = pg->slots[0].scope;
    if (scope == SLOOP_DESC_GLOBAL) {
        snprintf(text, sizeof(text), "%s", pg->title);
    } else if (scope == SLOOP_DESC_TRACK) {
        snprintf(text, sizeof(text), "%s · TRACK %u", pg->title, (unsigned)(track + 1));
    } else if (locked && track >= 3u) {
        snprintf(text, sizeof(text), "%s · TRACK %u (DRUM) — N/A", pg->title, (unsigned)(track + 1));
    } else if (locked) {
        snprintf(text, sizeof(text), "%s · TRACK %u USES ENG %u — N/A", pg->title,
                 (unsigned)(track + 1), (unsigned)st->core.trk[track].engine);
    } else {
        snprintf(text, sizeof(text), "%s · TRACK %u", pg->title, (unsigned)(track + 1));
    }
    lv_label_set_text(ui->header, text);
    lv_obj_set_style_text_color(ui->header, lv_color_hex(locked ? UI_ERROR : UI_TEXT_DIM), 0);
}

static void apply_focus(uint8_t index, uint8_t focus)
{
    page_ui_t *ui = &s_ui[index];
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        knob_set_active(&ui->knobs[slot], slot == focus);
    }
    ui->focus_shown = focus;
}

/* ---------------------------------------------------------------- 动作函数 --- */
/* 焦点槽 ±1（控件回调与 TAP 的共同路径）。返回 false = 不可编辑/无值。 */
static bool param_step(uint8_t index, int8_t delta, int16_t *applied)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL) return false;
    app_state_t st;
    state_get_copy(&st);
    uint8_t slot = focus_get(index);
    int16_t v;
    if (!slot_value(pg, slot, &st, &v) || page_locked(pg, &st)) return false;
    return slot_write(index, slot, (int16_t)(v + delta), applied);
}

/* 旋钮拖动（LVGL 上下文）：值变化 → 状态层 → 立即刷新本页 */
static void knob_change_cb(knob_t *k, int16_t value)
{
    uint8_t index = ctx_page(k->user);
    slot_write(index, ctx_slot(k->user), value, NULL);
    refresh(index);
}

/* 旋钮按下/文本点击：设焦点（输入任务下一拍按 s_focus 刷新样式） */
static void knob_focus_cb(knob_t *k)
{
    uint8_t index = ctx_page(k->user);
    uint8_t slot = ctx_slot(k->user);
    focus_set(index, slot);
    apply_focus(index, slot);
}

static void btn_cb(lv_event_t *e)
{
    /* user_data = (页下标 << 4 | 方向位)，方向位 1 = +1、0 = -1（与 KNOB_CTX 同编码） */
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t index = (uint8_t)(ctx >> 4);
    int8_t delta = (ctx & 1u) != 0 ? 1 : -1;
    param_step(index, delta, NULL);
    refresh(index);
}

/* ---------------------------------------------------------------- 键盘路由 --- */
/* 当前页在布局表里的下标；非参数页/UI 未启动返回 -1（不取任何锁） */
static int current_index(void)
{
    return page_layout_index(sloop_ui_current_page());
}

bool sloop_ui_param_page_active(void)
{
    return current_index() >= 0;
}

void sloop_ui_param_focus_move(int8_t delta)
{
    int idx = current_index();
    if (idx < 0 || delta == 0) return;
    const param_page_t *pg = page_layout_get((uint8_t)idx);
    uint8_t f = focus_get((uint8_t)idx);
    for (uint8_t n = 0; n < PARAM_PAGE_SLOTS; n++) {
        f = (uint8_t)((f + PARAM_PAGE_SLOTS + delta) % PARAM_PAGE_SLOTS);
        sloop_desc_t d;
        if (slot_desc(pg, f, &d)) break;
    }
    focus_set((uint8_t)idx, f);
}

void sloop_ui_param_adjust(int8_t delta)
{
    int idx = current_index();
    if (idx < 0 || delta == 0) return;
    param_step((uint8_t)idx, delta, NULL); /* 状态层写；控件由 30ms UI 定时器刷新 */
}

/* ---------------------------------------------------------------- 构建 --- */
/* ± 按钮样式（共享 static style：不给每个按钮建局部样式，池预算纪律同 knob.c） */
static lv_style_t s_st_btn;
static bool s_btn_style_ready;

static void btn_style_init(void)
{
    if (s_btn_style_ready) return;
    s_btn_style_ready = true;
    lv_style_init(&s_st_btn);
    lv_style_set_text_align(&s_st_btn, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(&s_st_btn, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_btn, lv_color_hex(UI_TEXT));
    lv_style_set_bg_color(&s_st_btn, lv_color_hex(UI_PANEL_HI));
    lv_style_set_bg_opa(&s_st_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_st_btn, 10);
    lv_style_set_pad_top(&s_st_btn, 4);
}

static lv_obj_t *make_button(lv_obj_t *parent, int16_t x, int16_t y, const char *text,
                             lv_event_cb_t cb, void *user)
{
    btn_style_init();
    lv_obj_t *btn = lv_label_create(parent);
    if (btn == NULL) return NULL;
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_add_style(btn, &s_st_btn, 0);
    lv_obj_set_clickable(btn, true);
    lv_label_set_text(btn, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

lv_obj_t *page_params_build(uint8_t index, lv_obj_t *parent)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || parent == NULL) return NULL;
    page_ui_t *ui = &s_ui[index];
    /* 懒建重试时控件是全新的：复位上下文/焦点缓存（焦点槽本身保留） */
    memset(ui, 0, sizeof(*ui));
    ui->shown_locked = -1;
    ui->focus_shown = 0xFF;

    ui->header = lv_label_create(parent);
    UI_BUILD_CHECK(ui->header);
    lv_obj_set_pos(ui->header, HEADER_X, HEADER_Y);
    lv_obj_set_size(ui->header, 1000, 30);
    lv_obj_set_style_text_font(ui->header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->header, lv_color_hex(UI_TEXT_DIM), 0);
    lv_label_set_long_mode(ui->header, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ui->header, "");

    /* ± 按钮：共用同一回调，user_data 编码 (页下标 << 4 | 方向位)；方向位 0=减 1=加 */
    ui->btn_dec = make_button(parent, BTN_DEC_X, BTN_Y, "-", btn_cb,
                              (void *)(uintptr_t)(((unsigned)index << 4) | 0u));
    UI_BUILD_CHECK(ui->btn_dec);
    ui->btn_inc = make_button(parent, BTN_INC_X, BTN_Y, "+", btn_cb,
                              (void *)(uintptr_t)(((unsigned)index << 4) | 1u));
    UI_BUILD_CHECK(ui->btn_inc);

    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        sloop_desc_t desc;
        if (!slot_desc(pg, slot, &desc)) continue;
        knob_t *k = &ui->knobs[slot];
        k->user = KNOB_CTX(index, slot);
        k->on_change = knob_change_cb;
        k->on_focus = knob_focus_cb;
        int16_t cx = (int16_t)(GRID_X0 + (slot % 4u) * CELL_PITCH);
        int16_t cy = (int16_t)(GRID_Y0 + (slot / 4u) * ROW_PITCH);
        if (!knob_create(k, parent, cx, cy, &desc)) return NULL;
        ui->present[slot] = true;
    }

    /* 焦点槽落在第一个有控件的槽上（重试后槽集合可能变化） */
    uint8_t f = focus_get(index);
    if (!ui->present[f]) {
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (ui->present[slot]) {
                focus_set(index, slot);
                break;
            }
        }
    }
    return parent;
}

void page_params_update(uint8_t index, const app_state_t *st)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || st == NULL) return;
    page_ui_t *ui = &s_ui[index];
    if (ui->header == NULL) return; /* 防御：ui.c 只刷新已建页 */

    uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0;
    bool locked = page_locked(pg, st);
    uint8_t engine = page_engine(pg);
    if (!ui->ctx_valid || ui->shown_track != track || ui->shown_engine != engine ||
        ui->shown_locked != (int8_t)locked) {
        ui->ctx_valid = true;
        ui->shown_track = track;
        ui->shown_engine = engine;
        ui->shown_locked = (int8_t)locked;
        update_header(index, pg, st, track, locked);
    }

    uint8_t focus = focus_get(index);
    if (ui->focus_shown != focus) apply_focus(index, focus);

    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        knob_t *k = &ui->knobs[slot];
        if (locked && pg->slots[slot].scope == SLOOP_DESC_ENGINE) {
            if (!k->shown || !k->locked) knob_show_locked(k);
            continue;
        }
        int16_t v;
        if (!slot_value(pg, slot, st, &v)) continue;
        if (!k->shown || k->locked || k->value != v) knob_show(k, v);
    }
}

/* ---------------------------------------------------------------- TAP --- */
static bool rect_hit(int16_t x, int16_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void param_tap_button(uint8_t index, int8_t delta, sloop_tap_result_t *out)
{
    const param_page_t *pg = page_layout_get(index);
    app_state_t st;
    state_get_copy(&st);
    uint8_t slot = focus_get(index);
    int16_t applied = 0;
    bool ok = !page_locked(pg, &st) && param_step(index, delta, &applied);
    if (out != NULL) {
        out->a = slot;
        out->b = ok ? slot_abs_id(pg, slot) : 0u;
        out->v = ok ? applied : 0;
        out->d = delta;
        out->kind = ok ? SLOOP_TAP_PARAM_STEP : SLOOP_TAP_PARAM_LOCKED;
    }
    refresh(index);
}

bool page_params_tap(uint8_t index, int16_t x, int16_t y, sloop_tap_result_t *out)
{
    const param_page_t *pg = page_layout_get(index);
    page_ui_t *ui = &s_ui[index];
    if (pg == NULL || ui->header == NULL) return false;

    if (rect_hit(x, y, BTN_DEC_X, BTN_Y, BTN_W, BTN_H)) {
        param_tap_button(index, -1, out);
        return true;
    }
    if (rect_hit(x, y, BTN_INC_X, BTN_Y, BTN_W, BTN_H)) {
        param_tap_button(index, +1, out);
        return true;
    }
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        int16_t cx = (int16_t)(GRID_X0 + (slot % 4u) * CELL_PITCH);
        int16_t cy = (int16_t)(GRID_Y0 + (slot / 4u) * ROW_PITCH);
        if (!rect_hit(x, y, cx + KNOB_OBJ_DX, cy + KNOB_OBJ_DY, KNOB_OBJ_SIZE, KNOB_OBJ_SIZE)) {
            continue;
        }
        focus_set(index, slot);
        apply_focus(index, slot);
        if (out != NULL) {
            out->kind = SLOOP_TAP_PARAM_FOCUS;
            out->a = slot;
        }
        return true;
    }
    return false;
}
