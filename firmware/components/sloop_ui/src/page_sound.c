/*
 * Sound 仪表盘页面（5B-1；5B 规格 §5.2；ADR-008 末节；契约见 page_sound.h）。
 *
 * 结构：滚动容器（视口 = page_layout.h 的 SOUND_VIEW_*）+ 13 张卡片对象。每张卡片是一个
 * 普通对象，标题与全部槽画在它的 DRAW_MAIN 里（不逐旋钮建对象——109 个槽若各建一个对象
 * 会撑爆池，先例 = STEP 页的自绘层）。卡片位置/尺寸 = sound_layout_place 的内容坐标，
 * 容器内即内容坐标。
 *
 * 槽：成员表（sound_layout.h）的每个成员按当前上下文（选中轨、引擎、引擎槽值、ENV SYNC、
 * INSERT TYPE）解析成存在位图 s_present + PSRAM 里的描述符/值/锁定态 s_slot。键变了才重解析，
 * 且只重绘「有槽的描述符/锁定态变了」的卡片（换轨/换引擎才全部重绘——拖引擎槽/ITYPE/ESYNC
 * 旋钮本身就会改键，不能每次移动都重画整页）。存在的槽画环 + 名称 + 值；鼓轨上锁定的轨道参数
 * （drum_ok == 0）无环、值 `--`、拒绝拖动；不存在的槽是空白格——不画、不命中、也不拦滚动。
 * 写值只有一条路：sound_slot_write → 状态层命令 → 窄读回填。
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
 * TAP 不能拖拽：命中存在的槽 = 设焦点、命中标题行 = 翻折叠位、命中选轨条 = 选轨（都与点击
 * 同一动作函数）。
 *
 * 选择器（5B-1 Task 6，page_sound_picker.c）：点页眉行名字区打开的引擎/预置/kit 面板盖在卡片区之上。
 * 本文件只路由：build/forget 挂/清它的页面对象根，update 每拍调 page_sound_picker_tick，TAP 先问它；
 * 它打开时 ←/→/↑/↓/页级 ± 不再动卡片的焦点槽，转给 page_sound_picker_move。
 *
 * 焦点（5B-1 Task 4）：s_focus_flat 是 portMUX 保护的 PSRAM 静态——键盘路由在输入任务里读写，
 * TAP/绘制在 LVGL 任务里。本文件里凡是**不取 LVGL 锁**的入口（page_sound_focus_move、
 * page_sound_adjust、page_sound_card_toggle 等六个）只读写这份状态（adjust 另发一条状态层
 * 命令），滚动到可见、重排、重绘全部留给下一拍 update（LVGL 上下文）。焦点槽恒是**可聚焦**的槽
 * （存在且所在卡片没折叠）：上下文变化让它消失、或它所在的卡被折叠时，update 按 focus_resolve
 * 重指到扁平序里下一个可聚焦的槽。焦点与折叠位是页内 UI 局部态，跨组页面重建保留。
 * 写值只有一条路 sound_slot_cmd，它拒「槽状态说的不是此刻的选中轨/引擎」（换轨后下一拍之前）。
 */
#include "page_sound.h"

#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "knob_math.h"
#include "page_layout.h"
#include "page_sound_picker.h"
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
/* 选轨条 / 提示·读数行（页面区相对坐标；建控件、自绘与 TAP 命中同源）：选轨条 4 格各 76px，
 * 提示行在 ± 按钮左侧（x 16..1088，按钮 1096 起） */
#define SEL_X        960
#define SEL_Y        4
#define SEL_W        304
#define SEL_H        32
#define SEL_CELL_W   76
#define HINT_X       16
#define HINT_Y       40
#define HINT_W       1072
#define HINT_H       32
/* 焦点槽滚进视口时四周留的边距 */
#define REVEAL_MARGIN 8
/* ZOOM 关时的提示文案。固件只带 Montserrat：ASCII 0x20..0x7E 之外一律没有字形（U+00B7 也没有），
 * LV_USE_FONT_PLACEHOLDER 会把它画成一个 10x16 的空框——文案的每个字符都必须留在 ASCII 内。 */
#define SOUND_HINT_TEXT                                                                \
    "Drag a knob to edit | Drag empty space to scroll | Left/Right focus | "           \
    "Up/Down value | Tap header name to choose"

typedef struct { sloop_desc_t desc; int16_t value; bool shown, locked; } sound_slot_state_t;
/* 上下文键：槽的描述符/值/锁定态随它变的全部输入（= page_params.c 的参数页上下文键，另含
 * 引擎槽值——上游引擎钩子读该轨自己的 p[P_E0..P_E7] 决定其余槽的标签/格式）。鼓轨没有引擎
 * （engine 记 0），所以单列 drum：引擎门槛只对合成轨的 ANALOG 生效。 */
typedef struct { uint8_t track, engine, itype, esync, edit[8]; bool drum; } sound_ctx_t;
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
static EXT_RAM_BSS_ATTR uint16_t s_collapsed; /* 折叠位（一位一张卡）；页面重建保留；临界区读写 */
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
static EXT_RAM_BSS_ATTR int8_t s_gesture_title_card; /* 本次手势按在谁的标题行上；-1 = 否 */
static EXT_RAM_BSS_ATTR int16_t s_drag_flat; /* 有拖动锚点的槽；-1 = 无 */
static EXT_RAM_BSS_ATTR int32_t s_anchor_deg;
static EXT_RAM_BSS_ATTR int16_t s_anchor_value;
static EXT_RAM_BSS_ATTR uint8_t s_anchor_track, s_anchor_engine; /* 锚点值/描述符所属的上下文 */
static EXT_RAM_BSS_ATTR uint32_t s_present[4];
static EXT_RAM_BSS_ATTR sound_slot_state_t s_slot[SOUND_SLOT_TABLE_MAX];
static EXT_RAM_BSS_ATTR sound_ctx_t s_ctx;
static EXT_RAM_BSS_ATTR bool s_ctx_valid;
/* 值文本：一个缓冲给全部槽共用。前提 = 软件绘制单元 + CONFIG_LV_USE_OS=0 ⇒ LVGL 9.6 在
 * lv_draw_finalize_task_creation 里就地 lv_draw_dispatch，lv_draw_label 返回前文本已被消费。
 * 换成异步/多线程绘制单元后这个前提不成立：届时要么 text_local = 1，要么每个槽各给缓冲。 */
static EXT_RAM_BSS_ATTR char s_value_text[24];
/* 焦点槽（扁平下标）；SOUND_SLOT_NONE = 尚无。临界区保护：输入任务（键盘）与 LVGL 任务（TAP/
 * 绘制/每拍重指）都会写。 */
static portMUX_TYPE s_focus_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR uint16_t s_focus_flat;
static EXT_RAM_BSS_ATTR int16_t s_focus_shown; /* 最近一拍落地的焦点；-1 = 尚无（首拍强制） */
static EXT_RAM_BSS_ATTR uint16_t s_layout_shown; /* s_pos 对应的折叠位；≠ s_collapsed ⇒ 重排 */
static EXT_RAM_BSS_ATTR lv_obj_t *s_sel_obj;    /* 选轨条 */
static EXT_RAM_BSS_ATTR lv_obj_t *s_hint_obj;   /* 提示 / 读数行 */
static EXT_RAM_BSS_ATTR int8_t s_sel_track_shown; /* 选轨条最近渲染的选中轨；-1 = 尚未 */
static EXT_RAM_BSS_ATTR bool s_hint_zoom_shown; /* 读数行最近渲染的模式：true = ZOOM 读数 */
static EXT_RAM_BSS_ATTR char s_readout_text[64]; /* 读数行文本：自用缓冲，不与 s_value_text 共用 */
/* 读数行最近渲染的（焦点 / 值 / 模式 / 存在性 / 锁定态）：任一项变了才重写文本并失效 */
typedef struct { uint16_t flat; int16_t value; bool zoom, shown, locked; } hint_key_t;
static EXT_RAM_BSS_ATTR hint_key_t s_hint_shown;
static EXT_RAM_BSS_ATTR bool s_hint_valid;

static const char *const k_sel_labels[4] = {"1", "2", "3", "4"};

/* 焦点读写（临界区；page_params.c 的 focus_get/focus_set 同一写法） */
static uint16_t focus_get(void)
{
    portENTER_CRITICAL(&s_focus_mux);
    uint16_t f = s_focus_flat;
    portEXIT_CRITICAL(&s_focus_mux);
    return f;
}

static void focus_set(uint16_t flat)
{
    portENTER_CRITICAL(&s_focus_mux);
    s_focus_flat = flat;
    portEXIT_CRITICAL(&s_focus_mux);
}

/* 折叠位读写：与焦点同一把锁（键盘路由在输入任务里读，标题行回调 / TAP / console 在 LVGL 任务里翻） */
static uint16_t collapsed_get(void)
{
    portENTER_CRITICAL(&s_focus_mux);
    uint16_t c = s_collapsed;
    portEXIT_CRITICAL(&s_focus_mux);
    return c;
}

static void collapsed_set(uint16_t c)
{
    portENTER_CRITICAL(&s_focus_mux);
    s_collapsed = c;
    portEXIT_CRITICAL(&s_focus_mux);
}

static bool collapsed_bit(uint8_t card)
{
    return card < SOUND_CARDS_MAX && (collapsed_get() >> card) & 1u;
}

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

/* 焦点描边画在槽矩形之内，所以失效槽矩形就够了；折叠卡没有槽矩形（整卡随尺寸变化重绘）。 */
static void invalidate_focus(int16_t flat)
{
    if (flat >= 0 && flat < SOUND_SLOT_TABLE_MAX) invalidate_slot((uint16_t)flat);
}

/* 焦点可聚焦 = 存在且所在卡片没折叠。 */
static bool focus_ok(uint16_t flat)
{
    uint8_t card, slot;
    return flat < SOUND_SLOT_TABLE_MAX && present_bit(flat) && sound_layout_slot_locate(flat, &card, &slot) &&
           !collapsed_bit(card);
}

/* 焦点悬空的处置（update 每拍与 focus_get 共用这一条规则）：尚无焦点 ⇒ 首个存在槽；焦点槽已
 * 不存在（换轨/换引擎后描述符消失）或被折叠（R12）⇒ 扁平序里下一个可聚焦的槽（环绕）；找不到
 * 这样的槽（全部折叠）而焦点槽仍存在 ⇒ 保持原焦点（编辑惰性：sound_slot_cmd 拒折叠卡）；焦点槽不
 * 存在才落到首个存在槽；没有存在槽 ⇒ NONE。会把结果写回焦点。
 * 页面重建后、首拍重解析之前 s_present / s_slot 还是空的：此时只回显、不动焦点——否则不持锁的
 * focus_get 会把幸存的焦点抹成首个槽。 */
static uint16_t focus_resolve(void)
{
    if (!s_ctx_valid) return focus_get();
    uint16_t f = focus_get();
    uint16_t next = f;
    if (f < SOUND_SLOT_TABLE_MAX && focus_ok(f)) return f;
    if (f >= SOUND_SLOT_TABLE_MAX) {
        (void)sound_layout_first_present(s_present, &next);
    } else if (sound_layout_flat_move(s_present, collapsed_get(), (int16_t)f, 1, &next)) {
        /* 重指到下一个可聚焦的槽 */
    } else if (!present_bit(f)) {
        (void)sound_layout_first_present(s_present, &next);
    }
    /* 否则 = 全部折叠而焦点槽仍在：next 保持 f（R12：不动焦点，编辑惰性） */
    if (next != f) focus_set(next);
    return next;
}

/* 焦点槽滚进视口（LVGL 上下文）：槽矩形（所在卡折叠 = 标题行）越过视口上下缘（留
 * REVEAL_MARGIN）才滚；lv_obj_scroll_by_bounded 自己钳到内容范围。 */
static void focus_reveal(uint16_t flat)
{
    uint8_t card, slot;
    int16_t x, y;
    int32_t h = SOUND_TITLE_H; /* 所在卡折叠：只保证标题行可见 */
    if (!sound_layout_slot_locate(flat, &card, &slot)) return;
    if (flat_rect(flat, &x, &y)) h = SOUND_CELL_H;
    else y = s_pos[card].y;
    int32_t view_h = page_layout_sound_view_h(s_view_top == SOUND_VIEW_Y_BANK);
    int32_t scroll = lv_obj_get_scroll_y(s_scroll_obj);
    int32_t target = scroll;
    if ((int32_t)y < scroll + REVEAL_MARGIN) {
        target = (int32_t)y - REVEAL_MARGIN;
    } else if ((int32_t)y + h > scroll + view_h - REVEAL_MARGIN) {
        target = (int32_t)y + h - view_h + REVEAL_MARGIN;
    }
    if (target == scroll) return;
    /* dy 是「手指位移」口径（同 gesture_move）：负值 = 内容上卷 = scroll_y 增大 */
    lv_obj_scroll_by_bounded(s_scroll_obj, 0, scroll - target, LV_ANIM_OFF);
    s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
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
    /* 文本都指向生命周期长于本次绘制的数据（上游描述符/字面量、s_value_text）：text_local = 0，
     * 不让 LVGL 每帧为每个槽 lv_strndup 一份 */
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
    draw_text(layer, &lv_font_montserrat_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, &ta, k_sound_cards[card].title);
    /* 折叠标记（字体里没有 U+25BE，用 LVGL 自带的箭头字形） */
    ta.x1 = a.x2 - TITLE_TEXT_X - TITLE_MARK_W;
    ta.x2 = a.x2 - TITLE_TEXT_X;
    draw_text(layer, &lv_font_montserrat_16, UI_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, &ta,
              collapsed_bit(card) ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);

    /* 卡片内容原点（内容坐标）：槽矩形减去它就是对象内坐标 */
    int32_t ox = card_origin_x(card);
    int32_t oy = pos->y;
    uint16_t focus = focus_get();
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

        /* 焦点描边：贴槽矩形内缘一圈，不越出矩形（失效槽矩形即可刷新） */
        if (flat == focus) {
            lv_draw_rect_dsc_t frame;
            lv_draw_rect_dsc_init(&frame);
            frame.base.layer = layer;
            frame.bg_opa = LV_OPA_TRANSP;
            frame.border_color = lv_color_hex(UI_ACCENT);
            frame.border_width = 2;
            frame.border_opa = LV_OPA_COVER;
            frame.radius = 8;
            lv_area_t fa = {.x1 = cell_x + 1, .y1 = cell_y + 1, .x2 = cell_x + w - 2, .y2 = cell_y + h - 2};
            lv_draw_rect(layer, &frame, &fa);
        }

        /* 锁定槽：无环、值 --（鼓轨逐槽规则，同参数页旋钮） */
        if (!sl->locked) {
            draw_ring(layer, cx, cy, UI_PANEL_HI, SLOT_ANGLE_START + SLOT_ANGLE_SPAN);
            int32_t arc = knob_value_to_arc(&sl->desc, sl->value);
            if (arc > 0) {
                draw_ring(layer, cx, cy, UI_TEXT_DIM, SLOT_ANGLE_START + (arc * SLOT_ANGLE_SPAN) / KNOB_ARC_RANGE);
            }
        }

        lv_area_t cell = {.x1 = cell_x, .y1 = cell_y, .x2 = cell_x + w - 1, .y2 = cell_y + SLOT_NAME_H};
        draw_text(layer, &lv_font_montserrat_14, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER, &cell, sl->desc.label);

        /* LVGL 9.6 在 lv_draw_finalize_task_creation 里就地 lv_draw_dispatch（lv_draw.c），上一个
         * 值的绘制任务在下一次格式化之前已经消费完 ⇒ 全部槽共用一个缓冲 */
        if (sl->locked) {
            strcpy(s_value_text, "--");
        } else {
            knob_format_value(&sl->desc, sl->value, s_value_text, sizeof(s_value_text));
        }
        lv_area_t va = {.x1 = cx - SOUND_KNOB_D / 2, .y1 = cy - SLOT_VALUE_H / 2,
                        .x2 = cx + SOUND_KNOB_D / 2, .y2 = cy + SLOT_VALUE_H / 2};
        draw_text(layer, &lv_font_montserrat_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, &va, s_value_text);
    }
}

/* ------------------------------------------------------------ 值与焦点 --- */
/* 槽值命令（唯一的写路径；不碰 LVGL，任何任务都可调）：一条状态层命令，applied（可 NULL）=
 * 写后投影里的真值（钳位/冻结都反映在这里）。槽不存在/锁定/所在卡片折叠返回 false 且不发命令。
 * 另拒「上下文过期」：槽的存在性/锁定态/描述符是上一拍按 s_ctx 解析的，若此刻的选中轨/引擎
 * 已经不同（键盘换轨之后、下一拍重解析之前，或页面已销毁），这个槽的状态说的不是当前轨——
 * 不写（拖动路径的锚点比对是同一条纪律）。 */
static bool sound_slot_cmd(uint16_t flat, int16_t v, int16_t *applied)
{
    if (!s_ctx_valid || s_ctx.track != state_selected_track() || s_ctx.engine != state_selected_engine()) {
        return false;
    }
    sound_slot_state_t *sl = &s_slot[flat];
    if (!sl->shown || sl->locked) return false;
    uint8_t card, slot;
    if (!sound_layout_slot_locate(flat, &card, &slot) || collapsed_bit(card)) return false;
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
    if (applied != NULL) *applied = m->scope == SLOOP_DESC_GLOBAL ? state_read_g(m->id) : state_read_p(m->id);
    return true;
}

/* 写槽值并刷新这个槽（LVGL 上下文：拖动用） */
static bool sound_slot_write(uint16_t flat, int16_t v, int16_t *applied)
{
    int16_t got = 0;
    if (!sound_slot_cmd(flat, v, &got)) return false;
    s_slot[flat].value = got;
    invalidate_slot(flat);
    if (applied != NULL) *applied = got;
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
    /* 锚点值/描述符属于上一拍解析的上下文（s_ctx）：记下，pressing 里与**此刻**的选中轨/引擎比对 */
    s_anchor_track = s_ctx.track;
    s_anchor_engine = s_ctx.engine;
    s_drag_flat = (int16_t)flat;
    return true;
}

/* 移动：相对锚点的角度差 → 值差（钳位在引擎）；环带外忽略但不结束拖动。 */
static bool sound_handle_pressing(uint16_t flat, int32_t px, int32_t py)
{
    if (s_drag_flat != (int16_t)flat) return false;
    /* 键盘换轨/换引擎之后、下一拍清锚点之前（≤30ms）的窗口：锚点还是旧轨的，这一次拖动
     * 不得写进新轨——结束拖动，不写。 */
    if (state_selected_track() != s_anchor_track || state_selected_engine() != s_anchor_engine) {
        s_drag_flat = -1;
        return false;
    }
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
    s_gesture_title_card = -1;
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
    if (rect_hit(cx, cy, card_origin_x(card), s_pos[card].y, SOUND_COL_W, SOUND_TITLE_H)) {
        s_gesture_title_card = (int8_t)card; /* 松手时没移动过 ⇒ 翻折叠位 */
    }
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
        /* dy = 指针位移（新 − 旧），内容跟着手指走：LVGL 自己的手势滚动传的也是 vect.y
         * （lv_indev_scroll.c → lv_obj_scroll_by_raw，子对象按 dy 平移） */
        lv_obj_scroll_by_bounded(s_scroll_obj, 0, p.y - from, LV_ANIM_OFF);
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
 * 同一次松手只消费一次。点击槽 = 设焦点；点击卡片标题行 = 翻折叠位（重排在下一拍）。 */
static void gesture_click(void)
{
    bool click = !s_gesture_moved && !s_gesture_click_used;
    int16_t slot = s_gesture_flat;
    int8_t title = s_gesture_title_card;
    s_gesture_click_used = true;
    s_gesture_moved = false;
    s_gesture_flat = -1;
    s_gesture_title_card = -1;
    if (!click) return;
    if (slot >= 0) focus_set((uint16_t)slot);
    else if (title >= 0) page_sound_card_toggle((uint8_t)title);
}

/* 长按的唯一入口：移动过的手势不算长按。LVGL 9.6 在手指移动后照发 LONG_PRESSED（它只在自己的
 * scroll_obj 存在时才拦，本页手动滚动 ⇒ 恒空），s_gesture_moved 在按压期间有效。本页还没有长按
 * 动作；接入时动作放在这里，入口条件就是这个判断。 */
static void gesture_long_press(void)
{
    if (s_gesture_moved) return;
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
        s_gesture_title_card = -1;
        gesture_end();
        break;
    case LV_EVENT_SHORT_CLICKED:
    case LV_EVENT_CLICKED:
        gesture_click();
        break;
    case LV_EVENT_LONG_PRESSED:
    case LV_EVENT_LONG_PRESSED_REPEAT:
        gesture_long_press();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------- 选轨条 / 提示·读数行 --- */
/* 选轨（控件回调与 TAP 的唯一共同路径，同 page_tracks.c 的 tracks_select） */
static void sound_select_track(uint8_t k)
{
    const edit_cmd_t cmd = {.type = CMD_SELECT_TRACK, .a = k};
    state_apply(&cmd);
}

/* 4 格：当前轨那格 UI_PANEL_SEL 底 + 轨色数字，其余 UI_PANEL 底 + 次级文字色。选中轨读
 * s_sel_track_shown（update 在选中轨变化时先写它再失效本对象）。 */
static void sel_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    for (uint8_t k = 0; k < 4u; k++) {
        bool cur = (int8_t)k == s_sel_track_shown;
        /* 左右各缩 2px：相邻格之间留一道缝 */
        lv_area_t cell = {.x1 = a.x1 + k * SEL_CELL_W + 2, .y1 = a.y1,
                          .x2 = a.x1 + (k + 1) * SEL_CELL_W - 3, .y2 = a.y2};
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.base.layer = layer;
        rect.bg_color = lv_color_hex(cur ? UI_PANEL_SEL : UI_PANEL);
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = 8;
        lv_draw_rect(layer, &rect, &cell);
        /* 16pt 行高 ≈20px：文字区从行顶 +6 起 */
        lv_area_t ta = {.x1 = cell.x1, .y1 = a.y1 + 6, .x2 = cell.x2, .y2 = a.y2};
        draw_text(layer, &lv_font_montserrat_16, cur ? ui_track_color(k) : UI_TEXT_DIM,
                  LV_TEXT_ALIGN_CENTER, &ta, k_sel_labels[k]);
    }
}

/* 点击：按指针落在哪一格（区间与绘制/TAP 同源）选轨 */
static void sel_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        sel_draw(e);
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_target_obj(e), &a);
        int32_t rel = p.x - a.x1;
        if (rel < 0 || rel >= SEL_W) return;
        sound_select_track((uint8_t)(rel / SEL_CELL_W));
        break;
    }
    default:
        break;
    }
}

/* ZOOM 关：14pt 提示；ZOOM 开：焦点槽的 28pt 读数（文本在 update 里格式化进 s_readout_text） */
static void hint_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (s_hint_zoom_shown) {
        draw_text(layer, &lv_font_montserrat_28, UI_TEXT, LV_TEXT_ALIGN_LEFT, &a, s_readout_text);
        return;
    }
    lv_area_t ta = {.x1 = a.x1, .y1 = a.y1 + 6, .x2 = a.x2, .y2 = a.y2};
    draw_text(layer, &lv_font_montserrat_14, UI_TEXT_DIM, LV_TEXT_ALIGN_LEFT, &ta, SOUND_HINT_TEXT);
}

/* ------------------------------------------------------------------ 页面 --- */
/* 上下文键（鼓轨没有引擎：engine 取 0，与 page_params.c 的 sel_context 同口径） */
static void context_of(const app_state_t *st, sound_ctx_t *c)
{
    uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0u;
    c->track = track;
    c->drum = track >= APP_STATE_TRACKS - 1u;
    c->engine = c->drum ? 0u : (uint8_t)st->core.trk[track].engine;
    c->itype = (uint8_t)st->p[L_P_ITYPE];
    c->esync = st->p[L_P_ESYNC] != 0;
    for (uint8_t i = 0; i < 8u; i++) c->edit[i] = st->p[L_P_E0 + i];
}

/* 两份描述符在屏幕上是否有差别（只比会画出来/会影响拖动的字段） */
static bool desc_same(const sloop_desc_t *a, const sloop_desc_t *b)
{
    return a->label == b->label && a->unit == b->unit && a->fmt == b->fmt && a->min == b->min &&
           a->max == b->max && a->enums == b->enums && a->enum_count == b->enum_count;
}

/* 上下文变了才重解析：门槛不满足（TA 槽只在 ANALOG **合成轨**出现，同 page_layout_chip_visible）、
 * 描述符取不到、名称为空或值域退化（min == max 且无枚举）⇒ 槽不存在；TRACK 取选中轨的动态描述符，
 * GLOBAL 取静态表；鼓轨上未标 drum_ok 的轨道参数锁定。
 * 重绘范围：换轨/换引擎（全部值都变）⇒ 整页；否则只有「存在性/描述符/锁定态变了的槽」所在的卡片。
 * 拖引擎槽、ITYPE、ESYNC 旋钮本身就会改上下文键，每次移动都会走到这里。 */
static void resolve_slots(const sound_ctx_t *c, const sound_ctx_t *prev)
{
    bool all = prev->track != c->track || prev->engine != c->engine;
    uint16_t dirty = all ? (uint16_t)((1u << SOUND_CARDS_MAX) - 1u) : 0u;
    uint32_t present[4] = {0};

    for (uint8_t card = 0; card < SOUND_CARDS_MAX; card++) {
        for (uint8_t slot = 0; slot < k_sound_cards[card].slot_count; slot++) {
            uint16_t flat = sound_layout_slot_index(card, slot);
            const sound_slot_t *m = &k_sound_cards[card].slots[slot];
            sloop_desc_t d;
            bool ok = (m->gate == 0u || (!c->drum && c->engine == 0u)) &&
                      (m->scope == SLOOP_DESC_GLOBAL
                           ? sloop_core_desc(SLOOP_DESC_GLOBAL, 0, m->id, &d)
                           : sloop_core_desc_track_live(c->track, m->id, &d)) &&
                      d.label != NULL && d.label[0] != '\0' && (d.min != d.max || d.enum_count != 0u);
            bool locked = ok && c->drum && m->scope != SLOOP_DESC_GLOBAL && m->drum_ok == 0u;
            sound_slot_state_t *sl = &s_slot[flat];
            bool was = sl->shown;
            bool desc_changed = ok && (!was || !desc_same(&sl->desc, &d));
            bool changed = ok != was || desc_changed || (ok && locked != sl->locked);
            /* 不持锁的 console 读路径可能插在中间：字段按「先描述符、后锁定态与 shown」的顺序写 */
            if (desc_changed) sl->desc = d;
            sl->locked = locked;
            sl->shown = ok;
            if (ok) present[flat >> 5] |= 1u << (flat & 31);
            if (changed) dirty |= (uint16_t)(1u << card);
        }
    }
    memcpy(s_present, present, sizeof(s_present));
    for (uint8_t card = 0; card < SOUND_CARDS_MAX; card++) {
        if ((dirty & (uint16_t)(1u << card)) != 0u && s_cards[card] != NULL) lv_obj_invalidate(s_cards[card]);
    }
}

lv_obj_t *page_sound_build(lv_obj_t *parent)
{
    page_sound_forget();
    page_sound_picker_attach(parent); /* 选择器面板的父对象：晚于下面全部控件创建 = 最上层 */
    /* 焦点与折叠位是页内 UI 局部态：页面重建保留（同 page_params_forget 保留 s_focus/s_chip） */
    (void)sound_layout_place(collapsed_get(), s_pos);
    s_layout_shown = collapsed_get();

    /* 选轨条与提示/读数行：先于滚动容器建（后建的对象在上面），它们与视口不重叠 */
    s_sel_obj = make_plain(parent);
    if (s_sel_obj == NULL) return NULL;
    lv_obj_set_pos(s_sel_obj, SEL_X, SEL_Y);
    lv_obj_set_size(s_sel_obj, SEL_W, SEL_H);
    lv_obj_set_clickable(s_sel_obj, true);
    lv_obj_add_event_cb(s_sel_obj, sel_event_cb, LV_EVENT_ALL, NULL);
    s_sel_track_shown = -1;

    s_hint_obj = make_plain(parent);
    if (s_hint_obj == NULL) return NULL;
    lv_obj_set_pos(s_hint_obj, HINT_X, HINT_Y);
    lv_obj_set_size(s_hint_obj, HINT_W, HINT_H);
    /* 28pt 行高略超过 32px 的行高：允许描边溢出几像素，不被对象矩形裁掉 */
    lv_obj_set_ext_draw_size(s_hint_obj, 4);
    lv_obj_add_event_cb(s_hint_obj, hint_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_hint_zoom_shown = false;
    s_hint_valid = false;

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

    /* 折叠位变了（标题行点击 / `UI SOUND CARDTOGGLE`）：按新折叠位重排，挪/缩 13 张卡片
     * （lv_obj_set_pos/size 自己失效新旧区域），滚动量按缩小后的内容高度重钳 */
    bool reveal = s_layout_shown != collapsed_get();
    if (reveal) {
        (void)sound_layout_place(collapsed_get(), s_pos);
        for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
            if (s_cards[c] == NULL) continue;
            lv_obj_set_pos(s_cards[c], card_origin_x(c), s_pos[c].y);
            lv_obj_set_size(s_cards[c], SOUND_COL_W, s_pos[c].h);
        }
        lv_obj_scroll_to_y(s_scroll_obj, s_scroll, LV_ANIM_OFF);
        s_scroll = (int16_t)lv_obj_get_scroll_y(s_scroll_obj);
        s_layout_shown = collapsed_get();
    }

    sound_ctx_t ctx;
    context_of(st, &ctx);
    if (!s_ctx_valid || memcmp(&ctx, &s_ctx, sizeof(ctx)) != 0) {
        /* 换轨/换引擎：旧锚点作废（键盘在拖动中途换了轨，不能拿旧轨的锚点写新轨）。ITYPE/ESYNC/
         * 引擎槽值的变化是拖动本身造成的，不打断。 */
        if (!s_ctx_valid || ctx.track != s_ctx.track || ctx.engine != s_ctx.engine) s_drag_flat = -1;
        sound_ctx_t prev = s_ctx;
        s_ctx = ctx;
        bool first = !s_ctx_valid;
        s_ctx_valid = true;
        if (first) memset(&prev, 0xFF, sizeof(prev)); /* 建页/重建后的首拍：整页 */
        resolve_slots(&ctx, &prev);
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

    /* 焦点：悬空的重指（换轨/换引擎后描述符消失）；变了只失效旧/新两个槽的矩形（描边画在
     * 矩形内），并滚进视口。重排之后也再滚一次——折叠可能把焦点槽挤出视口或挪近。 */
    uint16_t focus = focus_resolve();
    if ((int16_t)focus != s_focus_shown) {
        invalidate_focus(s_focus_shown);
        s_focus_shown = (int16_t)focus;
        invalidate_focus(s_focus_shown);
        reveal = true;
    }
    if (reveal && focus < SOUND_SLOT_TABLE_MAX) focus_reveal(focus);

    /* 选轨条：选中轨变了才失效重绘 */
    if ((int8_t)ctx.track != s_sel_track_shown) {
        s_sel_track_shown = (int8_t)ctx.track;
        lv_obj_invalidate(s_sel_obj);
    }

    /* 提示 / 读数行：键（模式 ZOOM / 焦点 / 焦点槽的值/存在性/锁定态/折叠）变了才重写文本并
     * 失效；ZOOM 关时提示文案是静态串，画过一次就不再失效。
     * 读数 = 标签 + 与槽里同一格式化的值（knob_format_value）；焦点槽锁定/折叠/不存在 = `--`。 */
    uint8_t fcard = 0, fslot = 0;
    const sound_slot_state_t *fs = focus < SOUND_SLOT_TABLE_MAX ? &s_slot[focus] : NULL;
    bool zoom = st->settings.zoom != 0;
    int16_t fval = fs != NULL ? fs->value : 0;
    bool fshown = fs != NULL && fs->shown;
    bool flocked = fs != NULL && (fs->locked ||
                                  (sound_layout_slot_locate(focus, &fcard, &fslot) && collapsed_bit(fcard)));
    if (!s_hint_valid || s_hint_shown.flat != focus || s_hint_shown.value != fval ||
        s_hint_shown.zoom != zoom || s_hint_shown.shown != fshown || s_hint_shown.locked != flocked) {
        s_hint_shown.flat = focus;
        s_hint_shown.value = fval;
        s_hint_shown.zoom = zoom;
        s_hint_shown.shown = fshown;
        s_hint_shown.locked = flocked;
        s_hint_valid = true;
        /* ZOOM 关的静态提示只画一次；从读数切回提示时再失效一次 */
        if (zoom || s_hint_zoom_shown) {
            s_hint_zoom_shown = zoom;
            if (zoom) {
                if (fshown && !flocked) {
                    char value[24];
                    knob_format_value(&fs->desc, fs->value, value, sizeof(value));
                    snprintf(s_readout_text, sizeof(s_readout_text), "%s  %s",
                             fs->desc.label != NULL ? fs->desc.label : "?", value);
                } else {
                    snprintf(s_readout_text, sizeof(s_readout_text), "%s  --",
                             fs != NULL && fs->desc.label != NULL ? fs->desc.label : "");
                }
            }
            lv_obj_invalidate(s_hint_obj);
        }
    }

    page_sound_picker_tick(st);
}

void page_sound_forget(void)
{
    page_sound_picker_forget(); /* 面板随页面根已删：只清状态与指针 */
    memset(s_cards, 0, sizeof(s_cards));
    s_scroll_obj = NULL;
    s_sel_obj = NULL;
    s_hint_obj = NULL;
    s_scroll = 0;
    s_view_top = -1;
    s_gesture_active = false;
    s_gesture_flat = -1;
    s_gesture_moved = false;
    s_gesture_click_used = false;
    s_gesture_title_card = -1;
    s_drag_flat = -1;
    memset(s_present, 0, sizeof(s_present));
    for (uint16_t flat = 0; flat < SOUND_SLOT_TABLE_MAX; flat++) s_slot[flat].shown = false;
    s_ctx_valid = false; /* 焦点与折叠位保留：focus_resolve 在重解析前不动它们 */
    s_focus_shown = -1;
    s_hint_valid = false;
}

/* 标题行点击 / TAP / `UI SOUND CARDTOGGLE` 的唯一共同路径：只翻位，重排在下一拍 */
static void card_toggle(uint8_t card)
{
    collapsed_set(collapsed_get() ^ (uint16_t)(1u << card));
}

bool page_sound_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (s_scroll_obj == NULL || s_view_top < 0) return false;
    /* 选择器打开：面板盖住卡片区，落在面板内的点归它——命中 = 已执行；空白 = miss 回显但不落到被盖住
     * 的卡片上；面板外照常往下 */
    if (page_sound_picker_active()) {
        int8_t hit = page_sound_picker_tap(x, y, out);
        if (hit != SLOOP_PICK_TAP_OUTSIDE) return hit == SLOOP_PICK_TAP_HIT;
    }
    /* 选轨条在视口之外：先于视口裁剪命中 */
    if (rect_hit(x, y, SEL_X, SEL_Y, SEL_W, SEL_H)) {
        uint8_t k = (uint8_t)((x - SEL_X) / SEL_CELL_W);
        sound_select_track(k);
        if (out != NULL) {
            out->kind = SLOOP_TAP_TRACK_CARD;
            out->a = k;
        }
        return true;
    }
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
            card_toggle(c);
            if (out != NULL) {
                out->kind = SLOOP_TAP_SOUND_CARD;
                out->a = c;
                out->v = collapsed_bit(c) ? 1 : 0; /* 翻转后的折叠位 */
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
    return collapsed_get();
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

/* ------------------------------------------- 焦点 / 折叠（都不取 LVGL 锁） --- */
/* 键盘路由在输入任务里调，TAP/console 在 LVGL 任务里调：以下只读写页面状态（焦点经临界区、
 * adjust 发一条状态层命令），对象由下一拍 update 处理。键盘路由不碰折叠位：card_toggle 的调用方
 * （标题行回调、TAP、console）都在 LVGL 锁内，读改写不与别的任务竞争。 */

void page_sound_focus_move(int8_t delta)
{
    if (delta == 0) return;
    if (page_sound_picker_active()) { /* 选择器打开：←/→ 移 chip 高亮 */
        page_sound_picker_move(delta, 0);
        return;
    }
    uint16_t f = focus_get();
    uint16_t next;
    if (f < SOUND_SLOT_TABLE_MAX && !present_bit(f)) f = focus_resolve(); /* 悬空：先按每拍的规则重指 */
    if (f >= SOUND_SLOT_TABLE_MAX) {
        /* 尚无焦点：落到首个存在槽（不把 SOUND_SLOT_NONE 强转成 flat_move 的 int16 起点） */
        if (sound_layout_first_present(s_present, &next)) focus_set(next);
        return;
    }
    if (sound_layout_flat_move(s_present, collapsed_get(), (int16_t)f, delta, &next)) focus_set(next);
}

bool page_sound_adjust_value(int8_t delta, int16_t *applied)
{
    if (page_sound_picker_active()) {
        /* 选择器打开：↑/↓ 与页级 ± 都移列表高亮（触摸与 TAP 同一条路）。delta 是调值口径（↓/− = −1），
         * 列表高亮按屏幕方向走：↓/− = 下一行 */
        page_sound_picker_move(0, (int8_t)-delta);
        return false;
    }
    uint16_t flat = focus_resolve();
    if (delta == 0 || flat >= SOUND_SLOT_TABLE_MAX) return false;
    uint8_t card, slot;
    if (!sound_layout_slot_locate(flat, &card, &slot)) return false;
    const sound_slot_t *m = &k_sound_cards[card].slots[slot];
    /* 窄读当前值（输入任务里没有状态副本）；越界由引擎钳位，同参数页的 param_step */
    int16_t v = m->scope == SLOOP_DESC_GLOBAL ? state_read_g(m->id) : state_read_p(m->id);
    return sound_slot_cmd(flat, (int16_t)(v + delta), applied);
}

bool page_sound_adjust(int8_t delta)
{
    return page_sound_adjust_value(delta, NULL);
}

uint16_t page_sound_focus_get(void)
{
    return focus_resolve();
}

void page_sound_focus_set(uint16_t flat)
{
    focus_set(flat < SOUND_SLOT_TABLE_MAX ? flat : SOUND_SLOT_NONE);
}

void page_sound_card_toggle(uint8_t card)
{
    if (card < SOUND_CARDS_MAX) card_toggle(card);
}

bool page_sound_card_collapsed(uint8_t card)
{
    return collapsed_bit(card);
}

void page_sound_focus_reveal(void)
{
    if (s_scroll_obj == NULL || s_view_top < 0) return;
    uint16_t f = focus_resolve();
    if (f < SOUND_SLOT_TABLE_MAX) focus_reveal(f);
}
