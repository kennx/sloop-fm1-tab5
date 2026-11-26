/*
 * Sound 仪表盘页面（5B-1；5B 规格 §5.2；ADR-008 末节；契约见 page_sound.h）。
 *
 * 结构：滚动容器（视口 = page_layout.h 的 SOUND_VIEW_*）+ 13 张卡片对象。每张卡片是一个
 * 普通对象，标题与全部槽画在它的 DRAW_MAIN 里（不逐旋钮建对象——109 个槽若各建一个对象
 * 会撑爆池，先例 = STEP 页的自绘层）。卡片位置/尺寸 = sound_layout_place 的内容坐标，
 * 容器内即内容坐标。
 *
 * 槽：成员表（sound_layout.h）的每个成员按当前上下文（选中轨、引擎、引擎槽值、ENV SYNC、
 * INSERT TYPE）解析成存在位图 s_present + PSRAM 里的描述符/值/锁定态 s_slot。存在的槽画
 * 环 + 名称 + 值；鼓轨上锁定的轨道参数（drum_ok == 0）无环、值 `--`、拒绝拖动；不存在的槽
 * 是空白格——不画、不命中、也不拦滚动。写值只有一条路：sound_slot_write → 状态层命令 →
 * 窄读回填。
 *
 * 手势（LVGL 9.6）：容器与卡片都关掉 SCROLLABLE/SCROLL_CHAIN/GESTURE_BUBBLE，不让 LVGL
 * 自己滚，滚动由本文件的事件回调驱动（容器与卡片共用同一个回调）——
 *  - PRESSED：按下点落在某个**存在**槽的环带里（sound_ring_hit，圆心 = 槽矩形顶 + 12 + 48）
 *    ⇒ 这一次手势属于该槽并记下拖动锚点，否则属于滚动（容器自己可点击：列间空隙、卡片
 *    之间、短列下方的空白同样滚）；
 *  - PRESSING：相对按下点位移 > 4px ⇒ 算移动；滚动手势过了这个死区才起步（点击抖动不挪
 *    列表），之后每拍把手指位移交给 lv_obj_scroll_by_bounded（自己钳位到内容范围）；槽手势
 *    按相对锚点的角度差改值（knob.c 的同一套换算），环带外的移动忽略但不结束拖动；
 *  - RELEASED → SHORT_CLICKED → CLICKED：LVGL 不因为「我们自己滚了」而取消点击，长按也在
 *    手指移动后照发（它只在自己的 scroll_obj 存在时才拦，本页恒空）⇒ 点击类动作一律以
 *    s_gesture_moved 为入口条件，并在最先到达的那个点击事件里消费一次。
 * PRESS_LOCK 是默认开：按下后手指滑到别的对象上，事件仍发给按下时的那个对象。
 * LVGL 的命中测试（lv_indev_search_obj）只在点落在父对象坐标内时才下探子对象，滚出视口的
 * 卡片部分收不到按下。
 *
 * TAP 不能拖拽：命中存在的槽 = 设焦点（与点击同一动作函数），命中卡片标题行只回显。
 */
#include "page_sound.h"

#include <esp_attr.h>
#include <string.h>

#include "knob_math.h"
#include "page_layout.h"
#include "param_ids.h"
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
/* 事件回调的 user data：卡片 = 下标 0..12，容器 = 这个哨兵（手势恒为滚动） */
#define CONTAINER_UD     0xFFu

typedef struct { sloop_desc_t desc; int16_t value; bool shown, locked; } sound_slot_state_t;
/* 上下文键：槽的描述符/值/锁定态随它变的全部输入（= page_params.c 的参数页上下文键，另含
 * 引擎槽值——上游引擎钩子读该轨自己的 p[P_E0..P_E7] 决定其余槽的标签/格式）。 */
typedef struct { uint8_t track, engine, itype, esync, edit[8]; } sound_ctx_t;
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

/* 页面状态全部在 PSRAM：LVGL 池之外、也不占内部 .bss。
 * 不持 LVGL 锁的 console 读路径（slot_total / slot_info）也读 s_present / s_slot：UI 任务
 * 重解析时逐字段写，32 位对齐读是原子的，最坏读到相邻拍的描述符/值（同 page_params.c 的
 * slot_info）。 */
static EXT_RAM_BSS_ATTR sound_card_pos_t s_pos[SOUND_CARDS_MAX];
static EXT_RAM_BSS_ATTR uint16_t s_collapsed;
static EXT_RAM_BSS_ATTR int16_t s_scroll; /* 当前滚动量（内容坐标被向上卷掉的像素数） */
static EXT_RAM_BSS_ATTR lv_obj_t *s_cards[SOUND_CARDS_MAX];
static EXT_RAM_BSS_ATTR lv_obj_t *s_scroll_obj;
static EXT_RAM_BSS_ATTR int16_t s_view_top; /* 视口顶（页面区相对坐标）；-1 = 尚未落地 */
static EXT_RAM_BSS_ATTR bool s_gesture_active; /* 按下成立（有指针、容器在） */
static EXT_RAM_BSS_ATTR int16_t s_gesture_flat; /* 本次手势的槽（扁平下标）；-1 = 滚动手势 */
static EXT_RAM_BSS_ATTR int32_t s_gesture_press_x, s_gesture_press_y;
static EXT_RAM_BSS_ATTR int32_t s_gesture_last_y;
static EXT_RAM_BSS_ATTR bool s_gesture_moved;
static EXT_RAM_BSS_ATTR bool s_gesture_click_used; /* 本次松手的点击已被消费 */
static EXT_RAM_BSS_ATTR int16_t s_drag_flat; /* 有拖动锚点的槽；-1 = 无 */
static EXT_RAM_BSS_ATTR int32_t s_anchor_deg;
static EXT_RAM_BSS_ATTR int16_t s_anchor_value;
static EXT_RAM_BSS_ATTR uint32_t s_present[4];
static EXT_RAM_BSS_ATTR sound_slot_state_t s_slot[SOUND_SLOT_TABLE_MAX];
static EXT_RAM_BSS_ATTR sound_ctx_t s_ctx;
static EXT_RAM_BSS_ATTR bool s_ctx_valid;
static EXT_RAM_BSS_ATTR uint16_t s_focus_flat; /* 焦点槽（扁平下标）；键盘路由与焦点描边在 Task 4 接 */

/* 页面区相对坐标 → 内容坐标（全文件唯一一处：手势、TAP 与注入都走这里） */
static void content_point(int32_t x, int32_t y, int32_t *cx, int32_t *cy)
{
    *cx = x - SOUND_VIEW_X;
    *cy = y - s_view_top + s_scroll;
}

/* 内容坐标 → 屏幕绝对坐标（容器位置 = 视口左上角；页面根在屏幕 (0, UI_TAB_BAR_H)） */
static void screen_point(int32_t cx, int32_t cy, int32_t *sx, int32_t *sy)
{
    lv_area_t v;
    lv_obj_get_coords(s_scroll_obj, &v);
    *sx = v.x1 + cx - (int32_t)lv_obj_get_scroll_x(s_scroll_obj);
    *sy = v.y1 + cy - (int32_t)lv_obj_get_scroll_y(s_scroll_obj);
}

static bool rect_hit(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static bool present_bit(uint16_t flat)
{
    return (s_present[flat >> 5] & (1u << (flat & 31))) != 0u;
}

static int32_t card_origin_x(uint8_t card)
{
    return s_pos[card].col * (SOUND_COL_W + SOUND_COL_GAP);
}

/* 扁平下标的槽矩形（内容坐标）；下标越界/卡片折叠返回 false */
static bool flat_rect(uint16_t flat, int16_t *x, int16_t *y)
{
    uint8_t card, slot;
    int16_t w, h;
    if (!sound_layout_slot_locate(flat, &card, &slot)) return false;
    return sound_layout_slot_rect(&s_pos[card], slot, x, y, &w, &h);
}

/* 槽圆心（内容坐标）：x = 槽矩形中心，y = 矩形顶 + 12 + 48 */
static bool flat_center(uint16_t flat, int32_t *cx, int32_t *cy)
{
    int16_t x, y;
    if (!flat_rect(flat, &x, &y)) return false;
    *cx = x + SOUND_CELL_W / 2;
    *cy = y + SLOT_RING_TOP + SOUND_KNOB_D / 2;
    return true;
}

/* 只失效一个槽：lv_obj_invalidate_area 收屏幕绝对坐标并按对象与各级父对象的裁剪求交，
 * 所以给卡片对象传「卡片坐标 + 槽内容矩形 − 卡片内容原点」；滚动量已在卡片坐标里。 */
static void invalidate_slot(uint16_t flat)
{
    uint8_t card, slot;
    int16_t x, y, w, h;
    if (!sound_layout_slot_locate(flat, &card, &slot) || s_cards[card] == NULL) return;
    if (!sound_layout_slot_rect(&s_pos[card], slot, &x, &y, &w, &h)) return;
    lv_area_t a;
    lv_obj_get_coords(s_cards[card], &a);
    int32_t ox = card_origin_x(card);
    lv_area_t r = {.x1 = a.x1 + x - ox, .y1 = a.y1 + y - s_pos[card].y,
                   .x2 = a.x1 + x - ox + w - 1, .y2 = a.y1 + y - s_pos[card].y + h - 1};
    lv_obj_invalidate_area(s_cards[card], &r);
}

/* ------------------------------------------------------------------ 自绘 --- */
static void draw_text(lv_layer_t *layer, const lv_font_t *font, uint32_t color, lv_text_align_t align,
                      const lv_area_t *area, const char *text, bool text_local)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.layer = layer;
    d.font = font;
    d.color = lv_color_hex(color);
    d.align = align;
    d.text = text;
    /* 名称/标题是静态串（上游描述符/字面量）：text_local = 0，不让 LVGL 为每帧复制一份；
     * 值是栈缓冲：绘制任务可能在本函数返回后才被 draw unit 消费，让 LVGL 复制一份（同 knob.c） */
    d.text_local = text_local;
    lv_draw_label(layer, &d, area);
}

static void draw_ring(lv_layer_t *layer, int32_t cx, int32_t cy, uint32_t color, int32_t end_angle)
{
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
    arc.end_angle = end_angle;
    arc.color = lv_color_hex(color);
    lv_draw_arc(layer, &arc);
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
    draw_text(layer, &lv_font_montserrat_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, &ta, k_sound_cards[card].title,
              false);
    /* 折叠标记（字体里没有 U+25BE，用 LVGL 自带的下箭头字形；折叠/展开动作在后续任务） */
    ta.x1 = a.x2 - TITLE_TEXT_X - TITLE_MARK_W;
    ta.x2 = a.x2 - TITLE_TEXT_X;
    draw_text(layer, &lv_font_montserrat_16, UI_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, &ta, LV_SYMBOL_DOWN, false);

    /* 卡片内容原点（内容坐标）：槽矩形减去它就是对象内坐标 */
    int32_t ox = card_origin_x(card);
    int32_t oy = pos->y;
    for (uint8_t s = 0; s < k_sound_cards[card].slot_count; s++) {
        uint16_t flat = sound_layout_slot_index(card, s);
        const sound_slot_state_t *sl = &s_slot[flat];
        if (!sl->shown) continue; /* 不存在的槽：空白格 */
        int16_t x, y, w, h;
        if (!sound_layout_slot_rect(pos, s, &x, &y, &w, &h)) break; /* 折叠卡片只有标题行 */
        int32_t cell_x = a.x1 + x - ox;
        int32_t cell_y = a.y1 + y - oy;
        int32_t cx = cell_x + w / 2;
        int32_t cy = cell_y + SLOT_RING_TOP + SOUND_KNOB_D / 2;

        /* 锁定槽：无环、值 --（鼓轨逐槽规则，同参数页旋钮） */
        if (!sl->locked) {
            draw_ring(layer, cx, cy, UI_PANEL_HI, SLOT_ANGLE_START + SLOT_ANGLE_SPAN);
            int32_t arc = knob_value_to_arc(&sl->desc, sl->value);
            if (arc > 0) {
                draw_ring(layer, cx, cy, UI_TEXT_DIM, SLOT_ANGLE_START + (arc * SLOT_ANGLE_SPAN) / KNOB_ARC_RANGE);
            }
        }

        lv_area_t cell = {.x1 = cell_x, .y1 = cell_y, .x2 = cell_x + w - 1, .y2 = cell_y + SLOT_NAME_H};
        draw_text(layer, &lv_font_montserrat_14, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER, &cell, sl->desc.label,
                  false);

        char val[24] = "--";
        if (!sl->locked) knob_format_value(&sl->desc, sl->value, val, sizeof(val));
        lv_area_t va = {.x1 = cx - SOUND_KNOB_D / 2, .y1 = cy - SLOT_VALUE_H / 2,
                        .x2 = cx + SOUND_KNOB_D / 2, .y2 = cy + SLOT_VALUE_H / 2};
        draw_text(layer, &lv_font_montserrat_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, &va, val, true);
    }
}

/* ------------------------------------------------------------ 值与焦点 --- */
/* 写槽值：一条状态层命令 + 窄读回填（applied 可 NULL = 写后投影里的真值，钳位/冻结都反映
 * 在这里）。槽不存在/锁定返回 false 且不发命令。调用方已持 LVGL 锁。 */
static bool sound_slot_write(uint16_t flat, int16_t v, int16_t *applied)
{
    sound_slot_state_t *sl = &s_slot[flat];
    if (!sl->shown || sl->locked) return false;
    uint8_t card, slot;
    (void)sound_layout_slot_locate(flat, &card, &slot);
    const sound_slot_t *m = &k_sound_cards[card].slots[slot];

    edit_cmd_t cmd = {0};
    if (m->scope == SLOOP_DESC_GLOBAL) {
        cmd.type = CMD_SET_GLOBAL;
        cmd.a = m->id;
        cmd.v = (uint16_t)v;
    } else {
        cmd.type = CMD_SET_PARAM;
        cmd.a = state_selected_track();
        cmd.b = m->id;
        cmd.v = (uint16_t)v;
    }
    state_apply(&cmd);
    sl->value = m->scope == SLOOP_DESC_GLOBAL ? state_read_g(m->id) : state_read_p(m->id);
    if (applied != NULL) *applied = sl->value;
    invalidate_slot(flat);
    return true;
}

/* 槽在状态副本里的值：GLOBAL → g[id]、TRACK → p[id]（存在性已由描述符保证 id 在范围内） */
static bool slot_value(uint16_t flat, const app_state_t *st, int16_t *out)
{
    uint8_t card, slot;
    if (!sound_layout_slot_locate(flat, &card, &slot)) return false;
    const sound_slot_t *m = &k_sound_cards[card].slots[slot];
    *out = m->scope == SLOOP_DESC_GLOBAL ? st->g[m->id] : st->p[m->id];
    return true;
}

/* 设焦点（TAP 与点击共用）。本阶段只记在 s_focus_flat。 */
static void focus_set(uint16_t flat)
{
    s_focus_flat = flat;
}

/* 槽手势的两步（物理触摸与 `UI SOUND KNOB` 注入共用；点是页面区相对坐标）。按下：槽存在
 * 且未锁、点在环带 ⇒ 记锚点角度与锚点值，**不改值**；环带外/锁定只清锚点。 */
static bool sound_handle_pressed(uint16_t flat, int32_t px, int32_t py)
{
    s_drag_flat = -1;
    if (!s_slot[flat].shown || s_slot[flat].locked) return false;
    int32_t cx, cy, x, y;
    content_point(px, py, &x, &y);
    if (!flat_center(flat, &cx, &cy) || !sound_ring_hit(x - cx, y - cy)) return false;
    s_anchor_deg = (int32_t)lv_atan2(y - cy, x - cx);
    s_anchor_value = s_slot[flat].value;
    s_drag_flat = (int16_t)flat;
    return true;
}

/* 移动：相对锚点的角度差 → 值差（钳位在引擎）；环带外忽略但不结束拖动。 */
static bool sound_handle_pressing(uint16_t flat, int32_t px, int32_t py)
{
    if (s_drag_flat != (int16_t)flat) return false;
    int32_t cx, cy, x, y;
    content_point(px, py, &x, &y);
    if (!flat_center(flat, &cx, &cy) || !sound_ring_hit(x - cx, y - cy)) return false;
    int32_t delta = knob_angle_delta(s_anchor_deg, (int32_t)lv_atan2(y - cy, x - cx));
    int16_t v = knob_drag_value(&s_slot[flat].desc, s_anchor_value, delta);
    if (v == s_slot[flat].value) return true; /* 已在该值（含钳位端），不重复下发 */
    (void)sound_slot_write(flat, v, NULL);
    return true;
}

/* ---------------------------------------------------------------- 手势 --- */
static void gesture_start(lv_event_t *e)
{
    s_gesture_active = false;
    s_gesture_flat = -1;
    s_gesture_moved = false;
    s_gesture_click_used = false;
    s_drag_flat = -1;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_scroll_obj == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_gesture_active = true;
    s_gesture_press_x = p.x;
    s_gesture_press_y = p.y;
    s_gesture_last_y = p.y;
    uint32_t card = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (card >= SOUND_CARDS_MAX) return; /* 容器：列间空隙/卡片之间/短列下方的空白 ⇒ 滚动 */

    int32_t cx, cy;
    /* indev 给的是屏幕绝对坐标；content_point 收页面区相对坐标（差 UI_TAB_BAR_H） */
    content_point(p.x, p.y - UI_TAB_BAR_H, &cx, &cy);
    for (uint8_t s = 0; s < k_sound_cards[card].slot_count; s++) {
        int32_t sx, sy;
        uint16_t flat = sound_layout_slot_index(card, s);
        if (!flat_center(flat, &sx, &sy)) break; /* 折叠卡片没有槽 */
        if (!sound_ring_hit(cx - sx, cy - sy)) continue;
        /* 环带只属于画出来的槽：不存在的槽是空白格，照常滚动 */
        if (s_slot[flat].shown) {
            s_gesture_flat = (int16_t)flat;
            (void)sound_handle_pressed(flat, p.x, p.y - UI_TAB_BAR_H);
        }
        break;
    }
}

static void gesture_move(void)
{
    if (!s_gesture_active || s_scroll_obj == NULL) return;
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
    if (s_gesture_flat >= 0) {
        (void)sound_handle_pressing((uint16_t)s_gesture_flat, p.x, p.y - UI_TAB_BAR_H);
        return; /* 槽手势归旋钮，不滚 */
    }
    /* 滚动过了死区才起步，且第一拍从按下点起算（死区里的位移不丢）。点击抖动（≤4px）因此
     * 不会挪动列表。 */
    if (s_gesture_moved) {
        int32_t from = was_moved ? s_gesture_last_y : s_gesture_press_y;
        lv_obj_scroll_by_bounded(s_scroll_obj, 0, from - p.y, LV_ANIM_OFF);
        s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
    }
    s_gesture_last_y = p.y;
}

static void gesture_end(void)
{
    s_gesture_active = false;
    s_drag_flat = -1;
    /* s_gesture_moved / s_gesture_flat 留给紧随的点击事件（RELEASED 先于它们派发） */
}

/* 点击类动作的唯一入口。SHORT_CLICKED 先于 CLICKED 派发（长按之后只有 CLICKED）：先按
 * s_gesture_moved 判、再清标志，否则被清掉的标志会让随后的 CLICKED 把滚动手势当成点击；
 * 同一次松手只消费一次。 */
static void gesture_click(void)
{
    bool click = !s_gesture_moved && !s_gesture_click_used && s_gesture_flat >= 0;
    s_gesture_click_used = true;
    s_gesture_moved = false;
    if (!click) return;
    focus_set((uint16_t)s_gesture_flat);
    s_gesture_flat = -1;
}

static void card_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        /* 容器与卡片共用本回调：只有卡片自绘（容器去掉了全部样式，什么都不画） */
        if ((uint32_t)(uintptr_t)lv_event_get_user_data(e) < SOUND_CARDS_MAX) card_draw(e);
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
        s_gesture_flat = -1;
        gesture_end();
        break;
    case LV_EVENT_SHORT_CLICKED:
    case LV_EVENT_CLICKED:
        gesture_click();
        break;
    default:
        /* LONG_PRESSED / LONG_PRESSED_REPEAT 在手指移动后照常触发：本页没有长按动作，接入时
         * 必须先判 s_gesture_moved（它在按压期间有效，点击事件/PRESS_LOST 才清）。 */
        break;
    }
}

/* ------------------------------------------------------------------ 页面 --- */
/* 上下文键（鼓轨没有引擎：engine 取 0，与 page_params.c 的 sel_context 同口径） */
static void context_of(const app_state_t *st, sound_ctx_t *c)
{
    uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0u;
    bool drum = track >= APP_STATE_TRACKS - 1u;
    c->track = track;
    c->engine = drum ? 0u : (uint8_t)st->core.trk[track].engine;
    c->itype = (uint8_t)st->p[L_P_ITYPE];
    c->esync = st->p[L_P_ESYNC] != 0;
    for (uint8_t i = 0; i < 8u; i++) c->edit[i] = st->p[L_P_E0 + i];
}

/* 上下文变了才重解析：门槛不满足（TA 槽只在新 ANALOG 引擎出现）、描述符取不到、名称为空
 * 或值域退化（min == max 且无枚举）⇒ 槽不存在；TRACK 取选中轨的动态描述符，GLOBAL 取静态
 * 表；鼓轨上未标 drum_ok 的轨道参数锁定。 */
static void resolve_slots(const sound_ctx_t *c)
{
    memset(s_present, 0, sizeof(s_present));
    for (uint8_t card = 0; card < SOUND_CARDS_MAX; card++) {
        for (uint8_t slot = 0; slot < k_sound_cards[card].slot_count; slot++) {
            uint16_t flat = sound_layout_slot_index(card, slot);
            const sound_slot_t *m = &k_sound_cards[card].slots[slot];
            sloop_desc_t d;
            bool ok = (m->gate == 0u || c->engine == 0u) &&
                      (m->scope == SLOOP_DESC_GLOBAL
                           ? sloop_core_desc(SLOOP_DESC_GLOBAL, 0, m->id, &d)
                           : sloop_core_desc_track_live(c->track, m->id, &d)) &&
                      d.label != NULL && d.label[0] != '\0' && (d.min != d.max || d.enum_count != 0u);
            sound_slot_state_t *sl = &s_slot[flat];
            if (!ok) {
                sl->shown = false;
                continue;
            }
            sl->desc = d;
            sl->locked = c->track >= APP_STATE_TRACKS - 1u && m->scope != SLOOP_DESC_GLOBAL &&
                         m->drum_ok == 0u;
            sl->shown = true;
            s_present[flat >> 5] |= 1u << (flat & 31);
        }
        if (s_cards[card] != NULL) lv_obj_invalidate(s_cards[card]);
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
    /* 容器也可点击：列间空隙、卡片之间、短列下方的空白按下同样滚（手势恒为滚动） */
    lv_obj_set_clickable(s_scroll_obj, true);
    lv_obj_set_pos(s_scroll_obj, SOUND_VIEW_X, SOUND_VIEW_Y);
    lv_obj_set_size(s_scroll_obj, SOUND_VIEW_W, page_layout_sound_view_h(false));
    lv_obj_add_event_cb(s_scroll_obj, card_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)CONTAINER_UD);

    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        lv_obj_t *card = make_plain(s_scroll_obj);
        s_cards[c] = card;
        if (card == NULL) return NULL;
        lv_obj_set_scrollable(card, false);
        lv_obj_set_scroll_chain(card, false);
        lv_obj_set_gesture_bubble(card, false);
        lv_obj_set_clickable(card, true);
        lv_obj_set_pos(card, card_origin_x(c), s_pos[c].y);
        lv_obj_set_size(card, SOUND_COL_W, s_pos[c].h);
        lv_obj_add_event_cb(card, card_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)c);
    }
    return s_scroll_obj;
}

void page_sound_update(const app_state_t *st, bool bank_visible)
{
    if (s_scroll_obj == NULL) return;
    /* 银行行出现/消失 = 视口顶 80 ⇄ 128：挪容器并改高度；滚动量按新范围钳位 */
    int16_t top = page_layout_sound_view_top(bank_visible);
    if (top != s_view_top) {
        s_view_top = top;
        lv_obj_set_pos(s_scroll_obj, SOUND_VIEW_X, top);
        lv_obj_set_size(s_scroll_obj, SOUND_VIEW_W,
                        page_layout_sound_view_h(top == SOUND_VIEW_Y_BANK));
        lv_obj_scroll_to_y(s_scroll_obj, s_scroll, LV_ANIM_OFF);
        s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
    }

    sound_ctx_t ctx;
    context_of(st, &ctx);
    if (!s_ctx_valid || memcmp(&ctx, &s_ctx, sizeof(ctx)) != 0) {
        s_ctx = ctx;
        s_ctx_valid = true;
        resolve_slots(&ctx);
    }
    /* 每拍只取值：变化的槽失效自己的矩形 */
    for (uint16_t flat = 0; flat < SOUND_SLOT_TABLE_MAX; flat++) {
        if (!present_bit(flat)) continue;
        int16_t v;
        if (!slot_value(flat, st, &v)) continue;
        sound_slot_state_t *sl = &s_slot[flat];
        if (sl->value != v) {
            sl->value = v;
            invalidate_slot(flat);
        }
    }
}

void page_sound_forget(void)
{
    memset(s_cards, 0, sizeof(s_cards));
    s_scroll_obj = NULL;
    s_scroll = 0;
    s_view_top = -1;
    s_gesture_active = false;
    s_gesture_flat = -1;
    s_gesture_moved = false;
    s_gesture_click_used = false;
    s_drag_flat = -1;
    memset(s_present, 0, sizeof(s_present));
    for (uint16_t flat = 0; flat < SOUND_SLOT_TABLE_MAX; flat++) s_slot[flat].shown = false;
    s_ctx_valid = false;
    s_focus_flat = SOUND_SLOT_NONE;
}

bool page_sound_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (s_scroll_obj == NULL || s_view_top < 0) return false;
    /* 这里是手工矩形表（不走 LVGL 命中测试）：滚出视口的卡片仍占着内容坐标，视口外一律不算 */
    int16_t top = s_view_top;
    int16_t bottom = (int16_t)(top + page_layout_sound_view_h(top == SOUND_VIEW_Y_BANK));
    if (x < SOUND_VIEW_X || x >= SOUND_VIEW_X + SOUND_VIEW_W || y < top || y >= bottom) {
        return false;
    }
    int32_t cx, cy;
    content_point(x, y, &cx, &cy);
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        const sound_card_pos_t *pos = &s_pos[c];
        int32_t ox = card_origin_x(c);
        if (!rect_hit(cx, cy, ox, pos->y, SOUND_COL_W, pos->h)) continue;
        if (rect_hit(cx, cy, ox, pos->y, SOUND_COL_W, SOUND_TITLE_H)) {
            if (out != NULL) {
                out->kind = SLOOP_TAP_SOUND_CARD;
                out->a = c;
                out->v = (int16_t)((s_collapsed >> c) & 1u);
            }
            return true;
        }
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            int16_t sx, sy, sw, sh;
            if (!sound_layout_slot_rect(pos, s, &sx, &sy, &sw, &sh)) break;
            if (!rect_hit(cx, cy, sx, sy, sw, sh)) continue;
            uint16_t flat = sound_layout_slot_index(c, s);
            if (!s_slot[flat].shown) return false; /* 不存在的槽是空白格 */
            focus_set(flat);
            if (out != NULL) {
                out->kind = SLOOP_TAP_SOUND_FOCUS;
                out->a = c;
                out->b = s;
                out->v = (int16_t)flat;
            }
            return true;
        }
        return false; /* 卡片内边距：不落任何槽 */
    }
    return false;
}

/* ------------------------------------------------------- console / 注入 --- */
/* 以下前两个不取 LVGL 锁（console 任务直接调）；其余由 ui.c 的公共入口在 LVGL 锁内调。 */
uint16_t page_sound_slot_total(void)
{
    return s_scroll_obj == NULL ? 0u : sound_layout_present_count(s_present);
}

uint16_t page_sound_collapsed(void)
{
    return s_collapsed;
}

esp_err_t page_sound_slot_info(uint16_t flat, uint8_t *card, uint8_t *slot, uint8_t *scope,
                               uint8_t *id, const char **label, bool *locked, int16_t *value)
{
    if (s_scroll_obj == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t c, s;
    if (!sound_layout_slot_locate(flat, &c, &s)) return ESP_ERR_INVALID_ARG;
    if (card != NULL) *card = c;
    if (slot != NULL) *slot = s;
    const sound_slot_state_t *sl = &s_slot[flat];
    const sound_slot_t *m = &k_sound_cards[c].slots[s];
    if (scope != NULL) *scope = m->scope;
    if (id != NULL) *id = m->id;
    if (!sl->shown) return ESP_ERR_NOT_FOUND;
    if (label != NULL) *label = sl->desc.label;
    if (locked != NULL) *locked = sl->locked;
    if (value != NULL) *value = sl->value;
    return ESP_OK;
}

/* 槽矩形（屏幕绝对坐标）。槽不存在/卡片折叠：ESP_ERR_NOT_FOUND 且矩形清零。 */
esp_err_t page_sound_slot_rect(uint16_t flat, int16_t *x, int16_t *y, int16_t *w, int16_t *h)
{
    if (s_scroll_obj == NULL) return ESP_ERR_INVALID_STATE;
    if (x != NULL) *x = 0;
    if (y != NULL) *y = 0;
    if (w != NULL) *w = 0;
    if (h != NULL) *h = 0;
    int16_t cx, cy;
    if (flat >= SOUND_SLOT_TABLE_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_slot[flat].shown || !flat_rect(flat, &cx, &cy)) return ESP_ERR_NOT_FOUND;
    int32_t sx, sy;
    screen_point(cx, cy, &sx, &sy);
    if (x != NULL) *x = (int16_t)sx;
    if (y != NULL) *y = (int16_t)sy;
    if (w != NULL) *w = SOUND_CELL_W;
    if (h != NULL) *h = SOUND_CELL_H;
    return ESP_OK;
}

esp_err_t page_sound_scroll_get(int16_t *y, int16_t *max_y)
{
    if (s_scroll_obj == NULL) return ESP_ERR_INVALID_STATE;
    int32_t cur = lv_obj_get_scroll_y(s_scroll_obj);
    /* 与 lv_obj_scroll_by_bounded 的钳位同一公式 */
    int32_t max = cur + lv_obj_get_scroll_bottom(s_scroll_obj);
    if (y != NULL) *y = (int16_t)cur;
    if (max_y != NULL) *max_y = (int16_t)(max > 0 ? max : 0);
    return ESP_OK;
}

esp_err_t page_sound_scroll_set(int16_t y)
{
    int16_t max = 0;
    esp_err_t ret = page_sound_scroll_get(NULL, &max);
    if (ret != ESP_OK) return ret;
    if (y < 0) y = 0;
    if (y > max) y = max;
    lv_obj_scroll_to_y(s_scroll_obj, y, LV_ANIM_OFF);
    s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
    return ESP_OK;
}

/* 调试注入：合成点 = 槽圆心（页面区相对坐标）+ dx/dy，走与物理触摸相同的 sound_handle_*。 */
esp_err_t page_sound_test_knob(uint16_t flat, int32_t dx, int32_t dy, bool press, int16_t *value)
{
    if (s_scroll_obj == NULL) return ESP_ERR_INVALID_STATE;
    if (flat >= SOUND_SLOT_TABLE_MAX) return ESP_ERR_INVALID_ARG;
    if (value != NULL) *value = s_slot[flat].value;
    if (!s_slot[flat].shown) return ESP_ERR_NOT_FOUND;
    if (s_slot[flat].locked) return ESP_ERR_NOT_ALLOWED;
    int32_t cx, cy, sx, sy;
    if (!flat_center(flat, &cx, &cy)) return ESP_ERR_NOT_FOUND;
    screen_point(cx, cy, &sx, &sy);
    int32_t px = sx + dx;
    int32_t py = sy - UI_TAB_BAR_H + dy;
    bool ok = press ? sound_handle_pressed(flat, px, py) : sound_handle_pressing(flat, px, py);
    if (value != NULL) *value = s_slot[flat].value;
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}
