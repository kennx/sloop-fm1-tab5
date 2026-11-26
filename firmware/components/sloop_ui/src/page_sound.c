/*
 * Sound 仪表盘页面（5B-1；5B 规格 §5.2；ADR-008 末节；契约见 page_sound.h）。
 *
 * 结构：滚动容器（视口 = page_layout.h 的 SOUND_VIEW_*）+ 13 张卡片对象。每张卡片是一个
 * 普通对象，标题与全部槽画在它的 DRAW_MAIN 里（不逐旋钮建对象——109 个槽若各建一个对象
 * 会撑爆池，先例 = STEP 页的自绘层）。卡片位置/尺寸 = sound_layout_place 的内容坐标，
 * 容器内即内容坐标。
 *
 * 手势（LVGL 9.6）：容器与卡片都关掉 SCROLLABLE/SCROLL_CHAIN/GESTURE_BUBBLE，不让 LVGL
 * 自己滚，滚动由本文件的回调驱动——
 *  - PRESSED：记卡片与按下点；按下点落在某槽的环带里（sound_ring_hit，圆心 = 槽矩形顶
 *    + 12 + 48）⇒ 这一次手势属于该槽，否则属于滚动；
 *  - PRESSING：相对按下点位移 > 4px ⇒ 算移动；滚动手势过了这个死区才起步（点击抖动不挪
 *    列表），之后每拍把手指位移交给 lv_obj_scroll_by_bounded（自己钳位到内容范围）；
 *  - RELEASED → CLICKED/SHORT_CLICKED：LVGL 不因为「我们自己滚了」而取消点击，长按也会在
 *    手势中途触发 ⇒ 移动过的手势一律不算点击。
 * PRESS_LOCK 是默认开：按下后手指滑到别的卡片上，事件仍发给按下时的那张卡。
 * LVGL 的命中测试不看父对象的裁剪——滚出视口的卡片部分照样收得到按下，所以按下点先按
 * 视口矩形过一遍。TAP 不能拖拽，只判命中（标题行 / 槽矩形）。
 *
 * 槽的名称取描述符（TRACK = sloop_core_desc_track_live、GLOBAL = sloop_core_desc），只存
 * 上游静态串的指针（PSRAM）；值、锁定态与拖动改值不在本阶段。
 */
#include "page_sound.h"

#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

#include "page_layout.h"
#include "sound_layout.h"
#include "theme.h"

/* 卡片/槽的绘制几何（内容坐标 = sound_layout.h；这里的量只在绘制与环带命中里用） */
#define CARD_RADIUS      10
#define TITLE_TEXT_X     12
#define TITLE_TEXT_Y     8
#define TITLE_TEXT_H     19 /* 16pt 行高 */
#define TITLE_MARK_W     20
#define SLOT_RING_TOP    12 /* 环的外缘到槽矩形顶的距离 */
#define SLOT_RING_R      (SOUND_KNOB_D / 2 - 4) /* 中心线半径 44：外缘 = 矩形顶 + 12 + 48 */
#define SLOT_RING_WIDTH  8
#define SLOT_ANGLE_START 135
#define SLOT_ANGLE_SPAN  270 /* LVGL 角度：0 = 3 点钟、顺时针；135 → 405（归一为 45） */
#define SLOT_NAME_H      16 /* 14pt 行高 */
#define SLOT_VALUE_H     19 /* 16pt 行高 */
#define GESTURE_MOVE_PX  4

/* 无主题装饰的容器/卡片（同 ui.c 的 make_plain；那是 static，这里按同一写法另写一个） */
static lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj != NULL) {
        lv_obj_remove_style_all(obj);
        lv_obj_set_scrollable(obj, false);
    }
    return obj;
}

/* 页面状态全部在 PSRAM：LVGL 池之外、也不占内部 .bss。 */
static EXT_RAM_BSS_ATTR sound_card_pos_t s_pos[SOUND_CARDS_MAX];
static EXT_RAM_BSS_ATTR uint16_t s_collapsed;
static EXT_RAM_BSS_ATTR int16_t s_scroll; /* 当前滚动量（内容坐标被向上卷掉的像素数） */
static EXT_RAM_BSS_ATTR lv_obj_t *s_cards[SOUND_CARDS_MAX];
static EXT_RAM_BSS_ATTR lv_obj_t *s_scroll_obj;
static EXT_RAM_BSS_ATTR int16_t s_view_top; /* 视口顶（页面区相对坐标）；-1 = 尚未落地 */
static EXT_RAM_BSS_ATTR int32_t s_gesture_card; /* 当前手势的卡片；-1 = 无/未成立 */
static EXT_RAM_BSS_ATTR int8_t s_gesture_slot;  /* 当前手势的槽；-1 = 滚动手势 */
static EXT_RAM_BSS_ATTR int32_t s_gesture_press_x, s_gesture_press_y;
static EXT_RAM_BSS_ATTR int32_t s_gesture_last_y;
static EXT_RAM_BSS_ATTR bool s_gesture_moved;
/* 槽名称（描述符 label，指向上游静态串；取不到 = "-"）与它对应的选中轨/引擎 */
static EXT_RAM_BSS_ATTR const char *s_label[SOUND_SLOT_TABLE_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_label_track, s_label_engine;
static EXT_RAM_BSS_ATTR bool s_label_valid;

/* 页面区相对坐标 → 内容坐标（全文件唯一一处：绘制经对象坐标、TAP 与手势都走这里） */
static void content_point(int32_t x, int32_t y, int32_t *cx, int32_t *cy)
{
    *cx = x - SOUND_VIEW_X;
    *cy = y - s_view_top + s_scroll;
}

static bool rect_hit(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* 槽圆心（内容坐标）：x = 槽矩形中心，y = 矩形顶 + 12 + 48；卡片折叠/槽越界返回 false */
static bool slot_center(const sound_card_pos_t *pos, uint8_t slot, int32_t *cx, int32_t *cy)
{
    int16_t x, y, w, h;
    if (!sound_layout_slot_rect(pos, slot, &x, &y, &w, &h)) return false;
    *cx = x + w / 2;
    *cy = y + SLOT_RING_TOP + SOUND_KNOB_D / 2;
    return true;
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
    /* 文本全是静态串（上游描述符/字面量）：text_local = 0，不让 LVGL 为每帧复制一份 */
    d.text = text;
    lv_draw_label(layer, &d, area);
}

static void card_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    uint8_t card = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    const sound_card_pos_t *pos = &s_pos[card];
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.base.layer = layer;
    rect.bg_color = lv_color_hex(UI_PANEL);
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = CARD_RADIUS;
    lv_draw_rect(layer, &rect, &a);

    lv_area_t ta = {.x1 = a.x1 + TITLE_TEXT_X, .y1 = a.y1 + TITLE_TEXT_Y,
                    .x2 = a.x2 - TITLE_TEXT_X - TITLE_MARK_W - 8, .y2 = a.y1 + TITLE_TEXT_Y + TITLE_TEXT_H};
    draw_text(layer, &lv_font_montserrat_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, &ta,
              k_sound_cards[card].title);
    /* 折叠标记（字体里没有 U+25BE，用 LVGL 自带的下箭头字形；折叠/展开动作在后续任务） */
    ta.x1 = a.x2 - TITLE_TEXT_X - TITLE_MARK_W;
    ta.x2 = a.x2 - TITLE_TEXT_X;
    draw_text(layer, &lv_font_montserrat_16, UI_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, &ta, LV_SYMBOL_DOWN);

    /* 卡片内容原点（内容坐标）：槽矩形减去它就是对象内坐标 */
    int32_t ox = pos->col * (SOUND_COL_W + SOUND_COL_GAP);
    int32_t oy = pos->y;
    for (uint8_t s = 0; s < k_sound_cards[card].slot_count; s++) {
        int16_t x, y, w, h;
        if (!sound_layout_slot_rect(pos, s, &x, &y, &w, &h)) break; /* 折叠卡片只有标题行 */
        int32_t cx = a.x1 + x - ox + w / 2;
        int32_t cy = a.y1 + y - oy + SLOT_RING_TOP + SOUND_KNOB_D / 2;

        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.base.layer = layer;
        arc.center.x = cx;
        arc.center.y = cy;
        arc.radius = SLOT_RING_R;
        arc.width = SLOT_RING_WIDTH;
        arc.rounded = 1;
        arc.opa = LV_OPA_COVER;
        arc.start_angle = SLOT_ANGLE_START;
        arc.end_angle = SLOT_ANGLE_START + SLOT_ANGLE_SPAN;
        arc.color = lv_color_hex(UI_PANEL_HI);
        lv_draw_arc(layer, &arc);

        lv_area_t cell = {.x1 = a.x1 + x - ox, .y1 = a.y1 + y - oy,
                          .x2 = a.x1 + x - ox + w - 1, .y2 = a.y1 + y - oy + SLOT_NAME_H};
        const char *name = s_label[sound_layout_slot_index(card, s)];
        draw_text(layer, &lv_font_montserrat_14, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER, &cell,
                  name != NULL ? name : "-");

        /* 值占位：弧内居中（取值与格式化在后续任务） */
        lv_area_t va = {.x1 = cx - SOUND_KNOB_D / 2, .y1 = cy - SLOT_VALUE_H / 2,
                        .x2 = cx + SOUND_KNOB_D / 2, .y2 = cy + SLOT_VALUE_H / 2};
        draw_text(layer, &lv_font_montserrat_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, &va, "--");
    }
}

/* ------------------------------------------------------------------ 手势 --- */
static void gesture_start(lv_event_t *e)
{
    s_gesture_moved = false;
    s_gesture_card = -1;
    s_gesture_slot = -1;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_scroll_obj == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    /* 视口外（滚出视口的卡片部分、被页眉盖住的部分）不成立 */
    lv_area_t view;
    lv_obj_get_coords(s_scroll_obj, &view);
    if (p.x < view.x1 || p.x > view.x2 || p.y < view.y1 || p.y > view.y2) return;

    int32_t card = (int32_t)(uintptr_t)lv_event_get_user_data(e);
    s_gesture_card = card;
    s_gesture_press_x = p.x;
    s_gesture_press_y = p.y;
    s_gesture_last_y = p.y;

    int32_t cx, cy;
    /* indev 给的是屏幕绝对坐标；content_point 收页面区相对坐标（差 UI_TAB_BAR_H） */
    content_point(p.x, p.y - UI_TAB_BAR_H, &cx, &cy);
    for (uint8_t s = 0; s < k_sound_cards[card].slot_count; s++) {
        int32_t sx, sy;
        if (!slot_center(&s_pos[card], s, &sx, &sy)) break; /* 折叠卡片没有槽 */
        if (sound_ring_hit(cx - sx, cy - sy)) {
            s_gesture_slot = (int8_t)s;
            break;
        }
    }
}

static void gesture_move(void)
{
    if (s_gesture_card < 0 || s_scroll_obj == NULL) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_gesture_press_x;
    int32_t dy = p.y - s_gesture_press_y;
    bool was_moved = s_gesture_moved;
    if (dx > GESTURE_MOVE_PX || dx < -GESTURE_MOVE_PX || dy > GESTURE_MOVE_PX ||
        dy < -GESTURE_MOVE_PX) {
        s_gesture_moved = true;
    }
    /* 环带内的手势归旋钮（改值在后续任务），不滚；其余过了死区才滚，且第一拍从按下点
     * 起算（死区里的位移不丢）。点击抖动（≤4px）因此不会挪动列表。 */
    if (s_gesture_slot < 0 && s_gesture_moved) {
        int32_t from = was_moved ? s_gesture_last_y : s_gesture_press_y;
        lv_obj_scroll_by_bounded(s_scroll_obj, 0, from - p.y, LV_ANIM_OFF);
        s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
    }
    s_gesture_last_y = p.y;
}

static void gesture_end(void)
{
    s_gesture_card = -1;
    s_gesture_slot = -1;
    /* s_gesture_moved 留给紧随的 CLICKED/SHORT_CLICKED 判断（RELEASED 先于它们派发） */
}

static void card_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        card_draw(e);
        break;
    case LV_EVENT_PRESSED:
        gesture_start(e);
        break;
    case LV_EVENT_PRESSING:
        gesture_move();
        break;
    case LV_EVENT_RELEASED:
        gesture_end();
        break;
    case LV_EVENT_PRESS_LOST:
        /* 丢失按压后不会再有 CLICKED：标志一并清掉，免得吞掉下一次点击 */
        s_gesture_moved = false;
        gesture_end();
        break;
    case LV_EVENT_CLICKED:
    case LV_EVENT_SHORT_CLICKED:
        /* 滚动/拖动后的松手仍是 CLICKED：移动过的手势不算点击。点击动作（折叠、设焦点）
         * 接入时以 s_gesture_moved 为入口条件。 */
        s_gesture_moved = false;
        break;
    default:
        break; /* LONG_PRESSED* 在手势中途照常触发；长按动作接入时同样先判 s_gesture_moved */
    }
}

/* ------------------------------------------------------------------ 页面 --- */
/* 仪表盘页在布局表里的下标（SOUND；银行行可见性谓词要页定义） */
static const param_page_t *dashboard_page(void)
{
    return page_layout_get((uint8_t)page_layout_index(APP_PAGE_ANALOG));
}

/* 银行行此刻是否显示（page_params.c 的同一谓词：带该行的页 + 合成轨 + 引擎 FM6） */
static bool bank_row_visible(const app_state_t *st)
{
    uint8_t track = st->core.selected_track;
    bool drum = track >= APP_STATE_TRACKS - 1u;
    uint8_t engine = drum || track >= APP_STATE_TRACKS ? 0u : (uint8_t)st->core.trk[track].engine;
    return page_layout_bank_row_visible(dashboard_page(), drum, engine);
}

/* 选中轨/引擎变了才重取槽名称（上游静态串的指针），并整卡失效重绘 */
static void resolve_labels(const app_state_t *st)
{
    uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0u;
    uint8_t engine = (uint8_t)st->core.trk[track].engine;
    if (s_label_valid && track == s_label_track && engine == s_label_engine) return;
    s_label_valid = true;
    s_label_track = track;
    s_label_engine = engine;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            const sound_slot_t *m = &k_sound_cards[c].slots[s];
            sloop_desc_t d;
            bool ok = m->scope == SLOOP_DESC_GLOBAL
                          ? sloop_core_desc(SLOOP_DESC_GLOBAL, 0, m->id, &d)
                          : sloop_core_desc_track_live(track, m->id, &d);
            s_label[sound_layout_slot_index(c, s)] = ok && d.label != NULL ? d.label : "-";
        }
        if (s_cards[c] != NULL) lv_obj_invalidate(s_cards[c]);
    }
}

lv_obj_t *page_sound_build(lv_obj_t *parent)
{
    page_sound_forget();
    s_collapsed = 0;
    (void)sound_layout_place(s_collapsed, s_pos);

    s_scroll_obj = make_plain(parent);
    if (s_scroll_obj == NULL) return NULL;
    lv_obj_set_scroll_chain(s_scroll_obj, false);
    lv_obj_set_gesture_bubble(s_scroll_obj, false);
    lv_obj_set_pos(s_scroll_obj, SOUND_VIEW_X, SOUND_VIEW_Y);
    lv_obj_set_size(s_scroll_obj, SOUND_VIEW_W, page_layout_sound_view_h(false));

    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        lv_obj_t *card = make_plain(s_scroll_obj);
        s_cards[c] = card;
        if (card == NULL) return NULL;
        lv_obj_set_scrollable(card, false);
        lv_obj_set_scroll_chain(card, false);
        lv_obj_set_gesture_bubble(card, false);
        lv_obj_set_clickable(card, true);
        lv_obj_set_pos(card, s_pos[c].col * (SOUND_COL_W + SOUND_COL_GAP), s_pos[c].y);
        lv_obj_set_size(card, SOUND_COL_W, s_pos[c].h);
        lv_obj_add_event_cb(card, card_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)c);
    }
    return s_scroll_obj;
}

void page_sound_update(const app_state_t *st)
{
    if (s_scroll_obj == NULL) return;
    /* 银行行出现/消失 = 视口顶 80 ⇄ 128：挪容器并改高度；滚动量按新范围钳位 */
    int16_t top = page_layout_sound_view_top(bank_row_visible(st));
    if (top != s_view_top) {
        s_view_top = top;
        lv_obj_set_pos(s_scroll_obj, SOUND_VIEW_X, top);
        lv_obj_set_size(s_scroll_obj, SOUND_VIEW_W,
                        page_layout_sound_view_h(top == SOUND_VIEW_Y_BANK));
        lv_obj_scroll_to_y(s_scroll_obj, s_scroll, LV_ANIM_OFF);
        s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
    }
    resolve_labels(st);
}

void page_sound_forget(void)
{
    memset(s_cards, 0, sizeof(s_cards));
    s_scroll_obj = NULL;
    s_scroll = 0;
    s_view_top = -1;
    s_gesture_card = -1;
    s_gesture_slot = -1;
    s_gesture_moved = false;
    s_label_valid = false;
}

bool page_sound_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (s_scroll_obj == NULL || s_view_top < 0) return false;
    /* 命中测试不看父对象裁剪：视口外的点一律不算（滚出视口的卡片仍占着那些坐标） */
    int16_t top = s_view_top;
    int16_t bottom = (int16_t)(top + page_layout_sound_view_h(top == SOUND_VIEW_Y_BANK));
    if (x < SOUND_VIEW_X || x >= SOUND_VIEW_X + SOUND_VIEW_W || y < top || y >= bottom) {
        return false;
    }
    int32_t cx, cy;
    content_point(x, y, &cx, &cy);
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        const sound_card_pos_t *pos = &s_pos[c];
        int32_t ox = pos->col * (SOUND_COL_W + SOUND_COL_GAP);
        if (!rect_hit(cx, cy, ox, pos->y, SOUND_COL_W, pos->h)) continue;
        if (rect_hit(cx, cy, ox, pos->y, SOUND_COL_W, SOUND_TITLE_H)) {
            if (out != NULL) {
                out->kind = SLOOP_TAP_SOUND_CARD;
                out->a = c;
            }
            return true;
        }
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            int16_t sx, sy, sw, sh;
            if (!sound_layout_slot_rect(pos, s, &sx, &sy, &sw, &sh)) break;
            if (!rect_hit(cx, cy, sx, sy, sw, sh)) continue;
            if (out != NULL) {
                out->kind = SLOOP_TAP_SOUND_FOCUS;
                out->a = c;
                out->b = s;
                out->v = (int16_t)sound_layout_slot_index(c, s);
            }
            return true;
        }
        return false; /* 卡片内边距：不落任何槽 */
    }
    return false;
}
