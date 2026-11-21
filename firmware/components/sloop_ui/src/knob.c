/*
 * 旋钮控件：单个自绘对象 = 弧线 + 名称/数值两行文本（见 knob.h 的契约与格式化表）。
 *
 * 为什么一个对象、自绘弧线与文本（ADR-008 §8 的预算决策）：LVGL 的 lv_arc 每只约占
 * 400B 池（bg/indicator 绘图描述符 + 3 个样式部件），另加 label 后实测 662B/旋钮/页，
 * 8 旋钮 × 3 页会在四基础页之后撑爆懒建预算（池仅剩 ~17KB）。这里用一个普通 lv_obj
 * + LV_EVENT_DRAW_MAIN：lv_draw_arc 画底弧与指示弧、lv_draw_label 画两行文本，
 * 实测约 300B/旋钮，参数页 ≤ 3.5KB（见 ADR-008 预算表）。
 *
 * 触摸语义与 lv_arc 一致：按下/拖动按**角度**设值（粗调，扫过 135°..405° 共 270°，
 * 底部 90° 为死区，就近取端）；按下同时设焦点。TAP 注入不走拖拽，见 page_params.
 *
 * 线程：全部入口在 LVGL 锁内调用（页面 build/update 与控件事件都在 LVGL 任务上下文）。
 */
#include "knob.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"

/* 弧线扫角（LVGL 角度：0=3 点钟、顺时针；>360 由 lv_draw_arc 归一） */
#define KNOB_ANGLE_START 135
#define KNOB_ANGLE_SPAN  270
#define KNOB_ARC_WIDTH   14
#define KNOB_ARC_RADIUS  (KNOB_OBJ_SIZE / 2 - KNOB_ARC_WIDTH / 2) /* 中心线半径 = 73 */
/* 文本行（对象内坐标，相对 160×160 的旋钮盒）：名称 16pt、数值 20pt，居中 */
#define KNOB_NAME_Y1 40
#define KNOB_NAME_Y2 64
#define KNOB_VAL_Y1  64
#define KNOB_VAL_Y2  94

static const char *const k_note_names[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                            "F#", "G",  "G#", "A",  "A#", "B"};

/* ------------------------------------------------------------------ 格式化 --- */
/* 上游 param_format 的口径（params.c）；指数表用生成曲线近似（见 knob.h）。 */
static void format_time(int32_t v, char *buf, size_t n)
{
    uint32_t ms10 = (uint32_t)lroundf(10.0f * powf(1.0752f, (float)(v & 127)));
    if (ms10 < 100u) {
        snprintf(buf, n, "%.1fms", (double)ms10 / 10.0);
    } else if (ms10 < 10000u) {
        snprintf(buf, n, "%ums", (unsigned)((ms10 + 5u) / 10u));
    } else if (ms10 < 100000u) {
        snprintf(buf, n, "%.2fs", (double)ms10 / 1000.0);
    } else {
        snprintf(buf, n, "%.1fs", (double)ms10 / 1000.0);
    }
}

static void format_cutoff(int32_t v, char *buf, size_t n)
{
    uint32_t hz = (uint32_t)lroundf(30.0f * powf(1.0507f, (float)(v & 127)));
    if (hz < 1000u) {
        snprintf(buf, n, "%uHz", (unsigned)hz);
    } else if (hz < 10000u) {
        snprintf(buf, n, "%.1fkHz", (double)hz / 1000.0);
    } else {
        snprintf(buf, n, "%ukHz", (unsigned)((hz + 500u) / 1000u));
    }
}

/* F_INT 的 0 终止名字表（ENV SYNC 音值等）：值域均分映射（上游 names_turn/format）。 */
static const char *name_for_int(const sloop_desc_t *d, int16_t v)
{
    uint32_t k = d->enum_count;
    int32_t range = (int32_t)d->max - d->min + 1;
    if (k == 0 || range <= 0 || d->enums == NULL) return NULL;
    int32_t x = (int32_t)v - d->min;
    if (x < 0) x = 0;
    if (x >= range) x = range - 1;
    uint32_t idx = (uint32_t)(x * (int32_t)k / range);
    if (idx >= k) idx = k - 1;
    return d->enums[idx];
}

void knob_format_value(const sloop_desc_t *d, int16_t v, char *buf, size_t n)
{
    if (buf == NULL || n == 0) return;
    buf[0] = '\0';
    const char *unit = d->unit;
    switch (d->fmt) {
    case SLOOP_DESC_FMT_ENUM: {
        int32_t idx = (int32_t)v - d->min;
        if (idx < 0) idx = 0;
        if (idx >= (int32_t)d->enum_count) idx = (int32_t)d->enum_count - 1;
        snprintf(buf, n, "%s%s", d->enums != NULL && idx >= 0 ? d->enums[idx] : "?",
                 unit != NULL ? unit : "");
        return;
    }
    case SLOOP_DESC_FMT_PCT:
        snprintf(buf, n, "%d%%", d->max > 0 ? (int)(((int32_t)v * 100 + d->max / 2) / d->max) : (int)v);
        return;
    case SLOOP_DESC_FMT_BIPCT:
        snprintf(buf, n, "%+d%%", (int)((int32_t)v * 100 / 64));
        return;
    case SLOOP_DESC_FMT_TIME:
        format_time(v, buf, n);
        return;
    case SLOOP_DESC_FMT_LFOHZ: {
        uint32_t h100 = (uint32_t)lroundf(5.0f * powf(1.0483f, (float)(v & 127)));
        if (h100 < 1000u) {
            snprintf(buf, n, "%.2fHz", (double)h100 / 100.0);
        } else {
            snprintf(buf, n, "%.1fHz", (double)h100 / 100.0);
        }
        return;
    }
    case SLOOP_DESC_FMT_CUTOFF:
        format_cutoff(v, buf, n);
        return;
    case SLOOP_DESC_FMT_DB: {
        if (v <= 0) {
            snprintf(buf, n, "OFF");
            return;
        }
        int32_t db10 = -560 + 5 * (int32_t)(v > 127 ? 127 : v); /* 上游 LEVEL_DB_X10 */
        if (db10 > -100) {
            snprintf(buf, n, "%.1fdB", (double)db10 / 10.0);
        } else {
            snprintf(buf, n, "%ddB", (int)((db10 - 5) / 10));
        }
        return;
    }
    case SLOOP_DESC_FMT_SEMI:
        snprintf(buf, n, "%+dst", (int)v);
        return;
    case SLOOP_DESC_FMT_BPM:
        snprintf(buf, n, "%dBPM", (int)v);
        return;
    case SLOOP_DESC_FMT_NOTE: {
        int32_t idx = (int32_t)v % 12;
        snprintf(buf, n, "%s", k_note_names[idx < 0 ? 0 : idx]);
        return;
    }
    case SLOOP_DESC_FMT_ONOFF:
        snprintf(buf, n, "%s", v ? "ON" : "OFF");
        return;
    case SLOOP_DESC_FMT_STEPS:
        snprintf(buf, n, "%dSTEP", (int)v);
        return;
    case SLOOP_DESC_FMT_SWING:
        snprintf(buf, n, "%d", (int)(v < 0 ? 0 : v > 100 ? 100 : v));
        return;
    case SLOOP_DESC_FMT_FILT:
        if (v == 0) {
            snprintf(buf, n, "OFF");
        } else if (v < 0) {
            snprintf(buf, n, "LP%d%%", (int)((-(int32_t)v * 100 + 32) / 64));
        } else {
            snprintf(buf, n, "HP%d%%", (int)(((int32_t)v * 100 + 31) / 63));
        }
        return;
    default:
        break;
    }
    /* F_INT（含 0 终止名字表）/ 未知 fmt：原始整数 + desc.unit（上游 F_OCT 等） */
    if (d->fmt == SLOOP_DESC_FMT_INT) {
        const char *nm = name_for_int(d, v);
        if (nm != NULL) {
            snprintf(buf, n, "%s%s", nm, unit != NULL ? unit : "");
            return;
        }
    }
    snprintf(buf, n, "%d%s", (int)v, unit != NULL ? unit : "");
}

/* ------------------------------------------------------------------ 映射 --- */
int32_t knob_value_to_arc(const sloop_desc_t *d, int16_t v)
{
    int32_t range = (int32_t)d->max - d->min;
    if (range <= 0) return 0;
    int32_t x = (int32_t)v - d->min;
    if (x < 0) x = 0;
    if (x > range) x = range;
    return (x * KNOB_ARC_RANGE + range / 2) / range;
}

int16_t knob_arc_to_value(const sloop_desc_t *d, int32_t arc)
{
    int32_t range = (int32_t)d->max - d->min;
    if (range <= 0) return d->min;
    if (arc < 0) arc = 0;
    if (arc > KNOB_ARC_RANGE) arc = KNOB_ARC_RANGE;
    return (int16_t)((int32_t)d->min + (arc * range + KNOB_ARC_RANGE / 2) / KNOB_ARC_RANGE);
}

/* 触摸角度 → 值：扫角 135°..405° 对应 0..KNOB_ARC_RANGE；底部 45°..135° 死区就近取端 */
static int16_t angle_to_value(const sloop_desc_t *d, int32_t angle)
{
    if (angle < 0) angle += 360;
    angle %= 360;
    int32_t rel;
    if (angle >= KNOB_ANGLE_START) {
        rel = angle - KNOB_ANGLE_START; /* 135..359 → 0..224 */
    } else if (angle <= KNOB_ANGLE_START + KNOB_ANGLE_SPAN - 360) {
        rel = angle + 360 - KNOB_ANGLE_START; /* 0..45 → 225..270 */
    } else {
        rel = angle < 90 ? KNOB_ANGLE_SPAN : 0; /* 死区：离哪端近取哪端 */
    }
    if (rel < 0) rel = 0;
    if (rel > KNOB_ANGLE_SPAN) rel = KNOB_ANGLE_SPAN;
    return knob_arc_to_value(d, rel * KNOB_ARC_RANGE / KNOB_ANGLE_SPAN);
}

/* ------------------------------------------------------------------ 自绘 --- */
static void draw_text_line(lv_layer_t *layer, const lv_area_t *obj, int16_t y1, int16_t y2,
                           const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.font = font;
    dsc.color = color;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.opa = LV_OPA_COVER;
    dsc.text = text;
    /* 文本在栈缓冲里：绘制任务可能在本函数返回后才被 draw unit 消费，必须让
     * LVGL 复制一份（text_local=1），否则任务持有悬空指针。 */
    dsc.text_local = 1;
    lv_area_t a = {.x1 = obj->x1, .y1 = (int32_t)(obj->y1 + y1),
                   .x2 = obj->x2, .y2 = (int32_t)(obj->y1 + y2)};
    lv_draw_label(layer, &dsc, &a);
}

static void knob_draw(knob_t *k, lv_layer_t *layer)
{
    lv_area_t c;
    lv_obj_get_coords(k->obj, &c);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.center.x = (c.x1 + c.x2) / 2;
    dsc.center.y = (c.y1 + c.y2) / 2;
    dsc.radius = KNOB_ARC_RADIUS;
    dsc.width = KNOB_ARC_WIDTH;
    dsc.rounded = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.start_angle = KNOB_ANGLE_START;
    dsc.end_angle = KNOB_ANGLE_START + KNOB_ANGLE_SPAN; /* 405 → 归一为 45 */
    dsc.color = lv_color_hex(UI_PANEL_HI);
    lv_draw_arc(layer, &dsc);

    if (k->shown && !k->locked) {
        int32_t arc = knob_value_to_arc(&k->desc, k->value);
        if (arc > 0) {
            dsc.color = lv_color_hex(k->active ? UI_ACCENT : UI_TEXT_DIM);
            dsc.end_angle = KNOB_ANGLE_START + (arc * KNOB_ANGLE_SPAN) / KNOB_ARC_RANGE;
            lv_draw_arc(layer, &dsc);
        }
    }

    if (!k->shown) return;
    lv_color_t text_color = lv_color_hex(k->active ? UI_TEXT : UI_TEXT_DIM);
    draw_text_line(layer, &c, KNOB_NAME_Y1, KNOB_NAME_Y2, &lv_font_montserrat_16,
                   lv_color_hex(k->active ? UI_ACCENT : UI_TEXT_DIM),
                   k->desc.label != NULL ? k->desc.label : "?");
    char val[24] = "--";
    if (!k->locked) knob_format_value(&k->desc, k->value, val, sizeof(val));
    draw_text_line(layer, &c, KNOB_VAL_Y1, KNOB_VAL_Y2, &lv_font_montserrat_20, text_color, val);
}

/* ------------------------------------------------------------------ 控件 --- */
void knob_show(knob_t *k, int16_t value)
{
    if (k == NULL || k->obj == NULL) return;
    k->value = value;
    k->shown = true;
    k->locked = false;
    lv_obj_invalidate(k->obj);
}

void knob_show_locked(knob_t *k)
{
    if (k == NULL || k->obj == NULL) return;
    k->shown = true;
    k->locked = true;
    lv_obj_invalidate(k->obj);
}

void knob_set_active(knob_t *k, bool active)
{
    if (k == NULL || k->obj == NULL || k->active == active) return;
    k->active = active;
    lv_obj_invalidate(k->obj);
}

/* 按下/拖动：按角度设值（粗调）。与 lv_arc 的交互语义一致（绝对角度，不是相对
 * 位移）；内圈 50px 内不响应，避免按下中心时跳到某端。 */
static void apply_pointer(knob_t *k)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || !k->shown || k->locked) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t c;
    lv_obj_get_coords(k->obj, &c);
    int32_t dx = p.x - (c.x1 + c.x2) / 2;
    int32_t dy = p.y - (c.y1 + c.y2) / 2;
    if (dx * dx + dy * dy < 50 * 50) return;
    int16_t v = angle_to_value(&k->desc, (int32_t)lv_atan2(dy, dx));
    if (v == k->value) return;
    if (k->on_change != NULL) k->on_change(k, v);
}

static void knob_event_cb(lv_event_t *e)
{
    knob_t *k = lv_event_get_user_data(e);
    if (k == NULL) return;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        knob_draw(k, lv_event_get_layer(e));
        break;
    case LV_EVENT_PRESSED:
        if (k->on_focus != NULL) k->on_focus(k);
        apply_pointer(k);
        break;
    case LV_EVENT_PRESSING:
        apply_pointer(k);
        break;
    case LV_EVENT_CLICKED:
        if (k->on_focus != NULL) k->on_focus(k);
        break;
    default:
        break;
    }
}

bool knob_create(knob_t *k, lv_obj_t *parent, int16_t cell_x, int16_t cell_y,
                 const sloop_desc_t *desc)
{
    if (k == NULL || parent == NULL || desc == NULL) return false;
    memset(k, 0, sizeof(*k));
    k->desc = *desc;

    k->obj = lv_obj_create(parent);
    if (k->obj == NULL) return false;
    lv_obj_remove_style_all(k->obj);
    lv_obj_set_scrollable(k->obj, false);
    lv_obj_set_pos(k->obj, cell_x + KNOB_OBJ_DX, cell_y + KNOB_OBJ_DY);
    lv_obj_set_size(k->obj, KNOB_OBJ_SIZE, KNOB_OBJ_SIZE);
    lv_obj_set_clickable(k->obj, true);
    lv_obj_add_event_cb(k->obj, knob_event_cb, LV_EVENT_ALL, k);
    return true;
}
