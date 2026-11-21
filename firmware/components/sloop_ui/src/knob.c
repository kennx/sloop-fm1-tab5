/*
 * 旋钮控件：单个自绘对象 = 弧线 + 名称/数值两行文本（见 knob.h 的契约）。
 *
 * 为什么一个对象、自绘弧线与文本（ADR-008 §8.2 的预算决策）：LVGL 的 lv_arc 每只
 * 约占 400B 池（bg/indicator 绘图描述符 + 3 个样式部件），另加 label 后实测
 * 662B/旋钮/页；8 旋钮 × 3 页会在四基础页之后撑爆懒建预算。这里用一个普通 lv_obj
 * + LV_EVENT_DRAW_MAIN：lv_draw_arc 画底弧与指示弧、lv_draw_label 画两行文本，
 * 实测约 300B/旋钮，参数页 ≤ 3.5KB（见 ADR-008 预算表）。
 *
 * 交互（ADR-008 §8.2）：按下只设焦点 + 记锚点；拖动按**相对角度差**改值（不是
 * lv_arc 的绝对角度语义——按下不会把值跳到触点角度），只响应半径 55..95px 环带。
 * 纯数学/格式化在 knob_math.c（host 单测）。调试注入 knob_test_* 走同一处理路径。
 *
 * 线程：全部入口在 LVGL 锁内调用（页面 build/update 与控件事件都在 LVGL 任务上下文）。
 */
#include "knob.h"

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

/* 触点 → 相对旋钮中心的 (dx,dy)。 */
static void point_delta(knob_t *k, lv_point_t p, int32_t *dx, int32_t *dy)
{
    lv_area_t c;
    lv_obj_get_coords(k->obj, &c);
    *dx = p.x - (c.x1 + c.x2) / 2;
    *dy = p.y - (c.y1 + c.y2) / 2;
}

/* 按下：设焦点 + 在环带内记录锚点（**不改值**）。返回锚点是否建立。 */
static bool handle_pressed(knob_t *k, lv_point_t p)
{
    if (k->on_focus != NULL) k->on_focus(k);
    k->dragging = false;
    if (!k->shown || k->locked) return false;
    int32_t dx, dy;
    point_delta(k, p, &dx, &dy);
    if (!knob_ring_hit(dx, dy)) return false; /* 中心/盒角：只设焦点 */
    k->anchor_deg = (int32_t)lv_atan2(dy, dx);
    k->anchor_value = k->value;
    k->dragging = true;
    return true;
}

/* 拖动：相对锚点的角度差 → 值差（钳位）；环带外忽略。返回事件是否被接受。 */
static bool handle_pressing(knob_t *k, lv_point_t p)
{
    if (!k->dragging) return false;
    int32_t dx, dy;
    point_delta(k, p, &dx, &dy);
    if (!knob_ring_hit(dx, dy)) return false;
    int32_t delta = knob_angle_delta(k->anchor_deg, (int32_t)lv_atan2(dy, dx));
    int16_t v = knob_drag_value(&k->desc, k->anchor_value, delta);
    if (v == k->value) return true; /* 已在该值（含钳位端），不重复下发 */
    if (k->on_change != NULL) k->on_change(k, v);
    return true;
}

static bool indev_point(lv_point_t *p)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return false;
    lv_indev_get_point(indev, p);
    return true;
}

static void knob_event_cb(lv_event_t *e)
{
    knob_t *k = lv_event_get_user_data(e);
    if (k == NULL) return;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        knob_draw(k, lv_event_get_layer(e));
        break;
    case LV_EVENT_PRESSED: {
        lv_point_t p;
        if (indev_point(&p)) {
            (void)handle_pressed(k, p);
        } else if (k->on_focus != NULL) {
            k->on_focus(k); /* 无指针（编码器）：只设焦点 */
        }
        break;
    }
    case LV_EVENT_PRESSING: {
        lv_point_t p;
        if (indev_point(&p)) (void)handle_pressing(k, p);
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        k->dragging = false;
        break;
    case LV_EVENT_CLICKED:
        if (k->on_focus != NULL) k->on_focus(k);
        break;
    default:
        break;
    }
}

/* 调试注入：合成坐标走与物理触摸相同的 handle_* 路径（dx/dy 相对旋钮中心）。 */
bool knob_test_press(knob_t *k, int32_t dx, int32_t dy)
{
    if (k == NULL || k->obj == NULL) return false;
    lv_area_t c;
    lv_obj_get_coords(k->obj, &c);
    lv_point_t p = {.x = (c.x1 + c.x2) / 2 + dx, .y = (c.y1 + c.y2) / 2 + dy};
    return handle_pressed(k, p);
}

bool knob_test_drag(knob_t *k, int32_t dx, int32_t dy)
{
    if (k == NULL || k->obj == NULL) return false;
    lv_area_t c;
    lv_obj_get_coords(k->obj, &c);
    lv_point_t p = {.x = (c.x1 + c.x2) / 2 + dx, .y = (c.y1 + c.y2) / 2 + dy};
    return handle_pressing(k, p);
}

bool knob_create(knob_t *k, lv_obj_t *parent, int16_t cell_x, int16_t cell_y,
                 const sloop_desc_t *desc, void *user, knob_change_cb_t on_change,
                 knob_focus_cb_t on_focus)
{
    if (k == NULL || parent == NULL || desc == NULL) return false;
    /* 只初始化本控件自己的字段：user/回调由调用方传入（先存参数再 memset 也不会
     * 丢，但这里显式保存，契约一目了然——首版 memset 把调用方先设的字段清掉了，
     * 物理触摸因此完全失效）。 */
    memset(k, 0, sizeof(*k));
    k->desc = *desc;
    k->user = user;
    k->on_change = on_change;
    k->on_focus = on_focus;

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
