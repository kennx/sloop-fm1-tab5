/*
 * DSYN 鼓合成编辑页（P4-D Task 9，spec §3.3/§4.4；SYN1..SYN4 的 24 个虚拟字段）
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H=72）：
 * - 页眉一行 y∈[4,36)：`SYN1..SYN4` 四 kit chip（x=16+70k，64px）、LANE 三区对象
 *   `-   LANE nn   +`（298..478；左右 42px 是 −/+ 区、中段是标签）、字段页 chip
 *   `SND1/SND2/SND3/KIT`（x=526+74i，68px）、`AUDITION`（828，116px）、
 *   `STORE`（952，88px）、状态行（1048..1264，16pt；文本来自 theme.h 的
 *   ui_dsyn_status_text 单一映射）。
 * - 旋钮网格与参数页同值：4 列 × 2 行，cell 300×290，x = 16 + c·316，
 *   y = 52 + r·298；旋钮盒 160×160 在单元内 (70,26)（自绘见 knob.c）。8 个旋钮
 *   对象**常驻**，当前字段页没有字段的槽 `lv_obj_set_hidden(true)`（视觉/命中/TAP
 *   都与「未建」等价——池峰值按 8 个建，组作用域下不重复分配）。
 * - 底部细调：`−`/`+`（16/104，80×44，页面区 y=552..596；旋钮盒底边 536 之下）。
 *   与参数页的 ± 同一语义（焦点槽 ±1 个原始单位），让 TAP 注入也能做单步写
 *   （TAP 不能拖拽；键盘 ↑/↓ 是同一动作）。KIT 页只有 CRUSH/KIT SRC 两个槽。
 * - 池预算（est 7,168B）压到 22 个对象：LANE 的标签/数值/± 合成一个三区对象；
 *   KIT 页的 kit 名称由状态行承载（`KIT <name>`，无动作结果时显示），不再单独建
 *   只读信息块。实测 delta **6,436B**（+11.4%，见 ADR-008 §3 与 Task 9 报告）。
 *
 * 字段页（**页内 UI 局部态**，`UI DSYN PAGE` 与页面 chip 共用；不新增 app page）：
 *   SND1 = WAVE/SRC/CLAP/PITCH/FINE/BEND/BTIME/HOLD
 *   SND2 = DECAY/TLEV/T2/T2LEV/CLICK/NLEV/NHOLD/NDEC
 *   SND3 = FLT_MODE/FLT_RES/FCUT/FENV/HPF/CHIP/DRIVE/LEVEL
 *   KIT  = CRUSH（0..255，kit 级）/ KIT SRC（工厂套用）
 *
 * 值路径（唯一）：读 `app_state_t.dsyn`（`sloop_dsyn_field_from_sound` 纯解码；kit 级
 * crush/src 直接取投影）→ 一条状态层命令（`CMD_DSYN_SET` / `CMD_DSYN_CRUSH` /
 * `CMD_DSYN_LOAD_KIT` / `CMD_DSYN_AUDITION` / `CMD_DSYN_STORE`）→ 命令同拍刷新的
 * 投影 → 重绘本页。
 * 数字文本只走 `sloop_core_dsyn_text`（DECAY/BTIME/NDEC → ms、FCUT/HPF → Hz、
 * LEVEL → 0.25dB、FLT_MODE 名、CHIP 音名），UI 不引入上游生成表；枚举/整数落回
 * `knob_format_value`。
 *
 * kit/lane 是**状态层保存的 UI 局部态**（`state_set_dsyn_select`，与 step_window 同类，
 * 不是引擎真值）：SYN/LANE chip 与 `UI DSYN KIT/LANE` 共用；TAP 之后 `STATE SNAP` 的
 * dsyn_kit/dsyn_lane 立即反映屏幕。DSYN 与选中轨无关，故本页**没有锁定态**（鼓轨也
 * 编辑同一份 SYN kit 库）。
 *
 * KIT SRC（工厂 kit 套用）：值变化（拖拽/↑/↓/±）**立即**发 `CMD_DSYN_LOAD_KIT`
 * （上游编辑器 DSYN_PUT part 17 同语义：套用即覆盖该 kit 的全部 16 lane 与名字/
 * crush/src）。点击旋钮只设焦点、不写破坏性命令（TAP 不能拖拽，避免误触清掉编辑）；
 * 页面/console 都不做二次确认（brief：「single tap is acceptable」，这里更保守一档）。
 * 成功后 KIT 页的状态行显示 `KIT <name>`（名称来自投影；来源数值就是 KIT SRC 旋钮的
 * 读数，`FACTORY F<src+1>` 不再单独占一个对象）。
 *
 * CRUSH（0..255）：kit 级 bit-crush / sample-and-hold（上游 `dkit_t.crush`），写路径与
 * 声音字段同一条 `dsyn_write`：值变化（拖拽/↑↓/±）→ `CMD_DSYN_CRUSH`（a=kit，v=值，
 * 状态层校验 0..255、core 钳位 + `dsu_fix_kit` + `dsu_dirty`）→ 投影同拍刷新 →
 * 旋钮/状态行重绘。不写 16 lane；与 KIT SRC 的工厂套用互相独立。
 *
 * 动作结果状态行（`s_act` × `synth_status` × `dsyn.dirty`）由 theme.h 的
 * ui_dsyn_status_text 单一映射给出；console `TAP` 的 `status="…"` 读同一缓冲
 * （page_dsyn_status_text），不另写一份词表。切换 kit/lane/字段页会清掉「上一次动作」
 * 的结果（新上下文不该继续显示旧 lane 的 EDIT OK），DIRTY 标记仍按投影显示。
 *
 * 线程：build/update/tap/forget 在 LVGL 锁内（ui.c）；`sloop_ui_dsyn_*` 键盘路由在
 * 输入任务上下文，不取 LVGL 锁；console `UI DSYN PAGE/KIT/LANE` 在 REPL 任务。页/焦点
 * 是临界区保护的单字节，状态文本跨任务读写也走同一临界区（与 SONG 页同纪律）。
 * 样式只在开机初始化一次（组作用域会反复销毁/重建本页，ADR-008 §3 R34）。
 */
#include "page_dsyn.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "knob.h"
#include "sloop_core.h"
#include "sloop_store.h"
#include "theme.h"

/* ------------------------------------------------------------ 几何/字段 --- */
#define DSYN_PAGES 4u
#define DSYN_SLOTS 8u

#define DSYN_HEAD_Y      4
#define DSYN_CHIP_H      32
#define DSYN_KIT_X0      16
#define DSYN_KIT_W       64
#define DSYN_KIT_PITCH   70
/* LANE 行是一个**三区**对象（`-   LANE nn   +`，宽 180px）：池预算下把标签、
 * 数值与两个 ± 按钮合成 1 个对象（单对象 ≈320B；est 7,168B 见 ADR-008 §3）。
 * 左右各 42px 是 −/+ 区，中段是纯标签（TAP 命中中段按 miss 处理）。 */
#define DSYN_LANE_X      298
#define DSYN_LANE_W      180
#define DSYN_LANE_ZONE   42
#define DSYN_PAGE_X0     526
#define DSYN_PAGE_W      68
#define DSYN_PAGE_PITCH  74
#define DSYN_AUD_X       828
#define DSYN_AUD_W       116
#define DSYN_STORE_X     952
#define DSYN_STORE_W     88
#define DSYN_STATUS_X    1048
#define DSYN_STATUS_W    216

/* 与 page_params.c 的网格同值（knob.c 的单元内偏移是共享的） */
#define DSYN_GRID_X0    16
#define DSYN_GRID_Y0    52
#define DSYN_CELL_PITCH 316
#define DSYN_ROW_PITCH  298

#define DSYN_ADJ_Y      552
#define DSYN_ADJ_W      80
#define DSYN_ADJ_H      44
#define DSYN_ADJ_DEC_X  16
#define DSYN_ADJ_INC_X  104

/* KIT 页的两个**伪字段**：不在 24 个声音字段里（crush/src 是 kit 级字节）。 */
#define DSYN_FIELD_CRUSH 0x80u
#define DSYN_FIELD_KITSRC 0x81u
#define DSYN_FIELD_NONE 0xFFu

static const uint8_t k_fields[DSYN_PAGES][DSYN_SLOTS] = {
    {SLOOP_DSYN_WAVE, SLOOP_DSYN_SRC, SLOOP_DSYN_CLAP, SLOOP_DSYN_PITCH,
     SLOOP_DSYN_FINE, SLOOP_DSYN_BEND, SLOOP_DSYN_BTIME, SLOOP_DSYN_HOLD},
    {SLOOP_DSYN_DECAY, SLOOP_DSYN_TLEV, SLOOP_DSYN_T2, SLOOP_DSYN_T2LEV,
     SLOOP_DSYN_CLICK, SLOOP_DSYN_NLEV, SLOOP_DSYN_NHOLD, SLOOP_DSYN_NDEC},
    {SLOOP_DSYN_FLT_MODE, SLOOP_DSYN_FLT_RES, SLOOP_DSYN_FCUT, SLOOP_DSYN_FENV,
     SLOOP_DSYN_HPF, SLOOP_DSYN_CHIP, SLOOP_DSYN_DRIVE, SLOOP_DSYN_LEVEL},
    {DSYN_FIELD_CRUSH, DSYN_FIELD_KITSRC, DSYN_FIELD_NONE, DSYN_FIELD_NONE,
     DSYN_FIELD_NONE, DSYN_FIELD_NONE, DSYN_FIELD_NONE, DSYN_FIELD_NONE},
};

static const char *const k_page_text[DSYN_PAGES] = {"SND1", "SND2", "SND3", "KIT"};

/* KIT 级两个伪字段的描述符（fmt 只影响落回通用整数格式化的路径） */
static const sloop_desc_t k_crush_desc = {"CRUSH", NULL, SLOOP_DESC_FMT_INT, 0, 255, 0, NULL, 0};
static const sloop_desc_t k_kitsrc_desc = {"KIT SRC", NULL, SLOOP_DESC_FMT_INT, 0,
                                           (int16_t)(SLOOP_DSYN_FACTORY_KITS - 1u), 0, NULL, 0};

/* ------------------------------------------------------------ 页内状态 --- */
static knob_t s_knobs[DSYN_SLOTS];
static bool s_present[DSYN_SLOTS]; /* 当前字段页的槽有字段（控件已显式） */
static bool s_hidden[DSYN_SLOTS];  /* 控件当前是隐藏态 */
static lv_obj_t *s_kit_chip[SLOOP_DSYN_KITS];
static lv_obj_t *s_lane_chip;
static lv_obj_t *s_page_chip[DSYN_PAGES];
static lv_obj_t *s_audition;
static lv_obj_t *s_store;
static lv_obj_t *s_status;
static lv_obj_t *s_adj_dec;
static lv_obj_t *s_adj_inc;

static lv_style_t s_st_chip;
static lv_style_t s_st_chip_on;
static lv_style_t s_st_adj;

/* UI 局部态（跨任务读写）：s_page/s_focus 是单字节；状态文本是字符串，故一起放在
 * 同一临界区（与 SONG 页的 s_ui_mux 同纪律）。 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_page;      /* 字段页 0..3（跨组页面重建保留，与 step_window 同类） */
static uint8_t s_focus;     /* 焦点槽 0..7 */
static char s_status_text[48];  /* 当前屏幕状态行文本（console TAP 回显同源） */
static char s_status_echo[48];  /* console TAP 回显专用（REPL 任务） */

/* 最近一次动作（页内局部态，0xFF = 尚无；选择 kit/lane/页时复位）。
 * 单字节写，读侧按值使用（与 s_sec 等既有页同纪律）。 */
static uint8_t s_act = 0xFF;
/* 脏检查缓存 */
static uint8_t s_shown_page = 0xFF;
static uint8_t s_shown_lane = 0xFF;
static uint8_t s_page_shown[DSYN_PAGES];
static uint8_t s_kit_sel_shown = 0xFF;
static uint8_t s_focus_shown = 0xFF;
static char s_status_cache[48];
static uint32_t s_status_color_shown = 0xFFFFFFFFu;

/* ------------------------------------------------------------ 小工具 --- */
static uint8_t page_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t p = s_page;
    portEXIT_CRITICAL(&s_mux);
    return p < DSYN_PAGES ? p : 0u;
}

static uint8_t focus_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t f = s_focus;
    portEXIT_CRITICAL(&s_mux);
    return f < DSYN_SLOTS ? f : 0u;
}

static void focus_set(uint8_t slot)
{
    if (slot >= DSYN_SLOTS) return;
    portENTER_CRITICAL(&s_mux);
    s_focus = slot;
    portEXIT_CRITICAL(&s_mux);
}

static void status_store(const char *text)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_status_text, sizeof(s_status_text), "%s", text);
    portEXIT_CRITICAL(&s_mux);
}

/* 字段码 → 当前显示值（纯读投影，不取引擎锁） */
static int16_t field_value(const app_state_t *st, uint8_t field)
{
    if (field == DSYN_FIELD_CRUSH) return st->dsyn.crush;
    if (field == DSYN_FIELD_KITSRC) return st->dsyn.src;
    return sloop_dsyn_field_from_sound(st->dsyn.sound, field);
}

static bool field_desc(uint8_t field, sloop_desc_t *out)
{
    if (field == DSYN_FIELD_CRUSH) {
        *out = k_crush_desc;
        return true;
    }
    if (field == DSYN_FIELD_KITSRC) {
        *out = k_kitsrc_desc;
        return true;
    }
    return sloop_core_dsyn_desc(field, out);
}

/* 数值文本：24 个声音字段走 core 的索引→ms/Hz/dB/音名；KIT 的两个伪字段返回 0
 * （落回 knob_format_value 的整数格式化）。 */
static int16_t dsyn_knob_text(const knob_t *k, int16_t value, char *buf, uint32_t n)
{
    uint8_t field = (uint8_t)((uintptr_t)k->user - 1u);
    if (field >= SLOOP_DSYN_FIELDS) return 0;
    return sloop_core_dsyn_text(field, value, buf, n);
}

const char *sloop_ui_dsyn_field_name(uint8_t code)
{
    if (code == DSYN_FIELD_CRUSH) return "CRUSH";
    if (code == DSYN_FIELD_KITSRC) return "KIT SRC";
    sloop_desc_t d;
    return sloop_core_dsyn_desc(code, &d) ? d.label : NULL;
}

const char *page_dsyn_status_text(void)
{
    if (s_status == NULL) return NULL; /* 页未建/已随组作用域销毁（与 BANK/SAMPLE 行同口径） */
    portENTER_CRITICAL(&s_mux);
    snprintf(s_status_echo, sizeof(s_status_echo), "%s", s_status_text);
    portEXIT_CRITICAL(&s_mux);
    return s_status_echo;
}

uint8_t sloop_ui_dsyn_page_get(void)
{
    return page_get();
}

void sloop_ui_dsyn_clear_action(void)
{
    portENTER_CRITICAL(&s_mux);
    s_act = 0xFF;
    portEXIT_CRITICAL(&s_mux);
}

void sloop_ui_dsyn_page_set(uint8_t page)
{
    if (page >= DSYN_PAGES) return;
    portENTER_CRITICAL(&s_mux);
    s_page = page;
    portEXIT_CRITICAL(&s_mux);
    /* 换字段页是上下文切换：不保留上一页的动作结果（状态行下一拍显示 DIRTY/空） */
    s_act = 0xFF;
}

/* ------------------------------------------------------------ 动作函数 --- */
static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_dsyn_update(&st);
}

/* kit/lane 选择（chip、LANE ±、console 共用同一状态层入口）：不写引擎数据。 */
static void dsyn_select(uint8_t kit, uint8_t lane)
{
    state_set_dsyn_select(kit, lane);
    s_act = 0xFF; /* 新上下文：不继续显示旧 kit/lane 的动作结果 */
    refresh();
}

static void dsyn_lane_move(int8_t delta)
{
    app_state_t st;
    state_get_copy(&st);
    int32_t lane = (int32_t)st.dsyn.lane + delta;
    while (lane < 0) lane += SLOOP_DSYN_LANES;
    while (lane >= (int32_t)SLOOP_DSYN_LANES) lane -= (int32_t)SLOOP_DSYN_LANES;
    dsyn_select(st.dsyn.kit, (uint8_t)lane);
}

typedef struct {
    bool ok;         /* 命令成功（synth_status == OK） */
    int16_t applied; /* 写后回读（投影里的真值） */
} dsyn_write_t;

/* 单字段写（旋钮拖拽、键盘 ↑/↓、± chip 共用）：一条状态层命令。
 * render = 调用方是否在 LVGL 上下文（控件回调/TAP）：是则立即重绘本页；键盘路由在
 * 输入任务上下文，**不得**碰控件，只写状态层，由 30ms UI 定时器下一拍刷新
 * （与 page_params 的 sloop_ui_param_adjust 同一纪律）。 */
static dsyn_write_t dsyn_write(uint8_t field, int16_t value, bool render)
{
    dsyn_write_t r = {false, value};
    app_state_t st;
    state_get_copy(&st);
    edit_cmd_t cmd = {0};
    if (field == DSYN_FIELD_KITSRC) {
        int32_t src = value;
        if (src < 0) src = 0;
        if (src >= (int32_t)SLOOP_DSYN_FACTORY_KITS) src = (int32_t)SLOOP_DSYN_FACTORY_KITS - 1;
        s_act = UI_DSYN_ACT_KIT;
        cmd.type = CMD_DSYN_LOAD_KIT;
        cmd.a = st.dsyn.kit;
        cmd.b = (uint32_t)src;
    } else if (field == DSYN_FIELD_CRUSH) {
        /* kit 级 CRUSH：状态层校验 0..255、core 钳位并过 dsu_fix_kit；与声音字段同一
         * 「一条命令 + 投影回读」路径（页面不另写动作函数） */
        int32_t crush = value;
        if (crush < 0) crush = 0;
        if (crush > 255) crush = 255;
        s_act = UI_DSYN_ACT_SET;
        cmd.type = CMD_DSYN_CRUSH;
        cmd.a = st.dsyn.kit;
        cmd.v = (uint16_t)crush;
    } else {
        s_act = UI_DSYN_ACT_SET;
        cmd.type = CMD_DSYN_SET;
        cmd.a = st.dsyn.kit;   /* 命令显式携带选择（不隐式读状态层的 kit/lane） */
        cmd.b = st.dsyn.lane;
        cmd.n = field;
        cmd.v = (uint16_t)value;
    }
    state_apply(&cmd);
    state_get_copy(&st);
    r.applied = field_value(&st, field);
    r.ok = st.synth_status == APP_SYNTH_STATUS_OK;
    if (render) refresh();
    return r;
}

/* 焦点槽 ±1（± chip、键盘 ↑/↓ 共用）：与旋钮拖拽走同一个 dsyn_write。 */
static dsyn_write_t dsyn_step(int8_t delta, bool render)
{
    dsyn_write_t r = {false, 0};
    app_state_t st;
    state_get_copy(&st);
    uint8_t slot = focus_get();
    uint8_t field = k_fields[page_get()][slot];
    if (field == DSYN_FIELD_NONE) return r;
    r.applied = field_value(&st, field);
    if (delta == 0) return r;
    return dsyn_write(field, (int16_t)(r.applied + delta), render);
}

static void dsyn_audition(void)
{
    app_state_t st;
    state_get_copy(&st);
    s_act = UI_DSYN_ACT_AUDITION;
    const edit_cmd_t cmd = {.type = CMD_DSYN_AUDITION,
                            .a = st.dsyn.kit,
                            .b = st.dsyn.lane,
                            .v = 100}; /* 固定力度（brief）：试听不读演奏层 */
    state_apply(&cmd);
    refresh();
}

static void dsyn_store(void)
{
    s_act = UI_DSYN_ACT_STORE;
    const edit_cmd_t cmd = {.type = CMD_DSYN_STORE};
    state_apply(&cmd);
    refresh();
}

/* ------------------------------------------------------------ 事件回调 --- */
/* 控件下标编码：op << 8 | arg（chip 一族共用本回调） */
enum {
    DSYN_OP_KIT = 0,   /* arg = kit 0..3 */
    DSYN_OP_PAGE = 2,  /* arg = 字段页 0..3 */
    DSYN_OP_AUDITION = 3,
    DSYN_OP_STORE = 4,
    DSYN_OP_ADJ = 5,   /* arg = 0 减 / 1 加（焦点槽 ±1） */
};

/* LANE 三区对象的点击：读指针的页面区相对 x 判定 −/中/− 之外的 + 区。 */
static void lane_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    int32_t rel = p.x - c.x1;
    if (rel < DSYN_LANE_ZONE) dsyn_lane_move(-1);
    else if (rel >= DSYN_LANE_W - DSYN_LANE_ZONE) dsyn_lane_move(1);
    /* 中段是标签：不动作 */
}

static void chip_cb(lv_event_t *e)
{
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t op = (uint8_t)(ctx >> 8);
    uint8_t arg = (uint8_t)(ctx & 0xFFu);
    app_state_t st;
    switch (op) {
    case DSYN_OP_KIT:
        state_get_copy(&st);
        dsyn_select(arg, st.dsyn.lane);
        break;
    case DSYN_OP_PAGE:
        sloop_ui_dsyn_page_set(arg);
        refresh();
        break;
    case DSYN_OP_AUDITION: dsyn_audition(); break;
    case DSYN_OP_STORE: dsyn_store(); break;
    case DSYN_OP_ADJ: (void)dsyn_step(arg == 1u ? 1 : -1, true); break;
    default: break;
    }
}

static void knob_change_cb(knob_t *k, int16_t value)
{
    uint8_t field = (uint8_t)((uintptr_t)k->user - 1u);
    (void)dsyn_write(field, value, true);
}

static void knob_focus_cb(knob_t *k)
{
    uint8_t slot = 0;
    for (uint8_t i = 0; i < DSYN_SLOTS; i++) {
        if (&s_knobs[i] == k) {
            slot = i;
            break;
        }
    }
    focus_set(slot);
    refresh();
}

/* ---------------------------------------------------------------- 构建 --- */
static void styles_init(void)
{
    static bool done;
    if (done) {
        lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
        lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip_on, lv_color_hex(UI_ACCENT));
        lv_style_set_text_color(&s_st_chip_on, lv_color_hex(UI_BG));
        lv_style_set_bg_color(&s_st_adj, lv_color_hex(UI_PANEL_HI));
        lv_style_set_text_color(&s_st_adj, lv_color_hex(UI_TEXT));
        return;
    }
    done = true;
    lv_style_init(&s_st_chip);
    lv_style_set_radius(&s_st_chip, 10);
    lv_style_set_bg_opa(&s_st_chip, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_chip, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_chip, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_chip, 6);

    lv_style_init(&s_st_chip_on);
    lv_style_set_bg_color(&s_st_chip_on, lv_color_hex(UI_ACCENT));
    lv_style_set_text_color(&s_st_chip_on, lv_color_hex(UI_BG));

    lv_style_init(&s_st_adj);
    lv_style_set_radius(&s_st_adj, 10);
    lv_style_set_bg_opa(&s_st_adj, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_adj, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_adj, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_adj, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_adj, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_adj, 4);
}

static lv_obj_t *make_chip(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h,
                           const char *text, lv_style_t *style, uintptr_t ctx)
{
    lv_obj_t *chip = lv_label_create(parent);
    if (chip == NULL) return NULL;
    lv_obj_set_clickable(chip, true);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_size(chip, w, h);
    lv_obj_add_style(chip, style, 0);
    lv_label_set_text(chip, text);
    lv_obj_add_event_cb(chip, chip_cb, LV_EVENT_CLICKED, (void *)ctx);
    return chip;
}

lv_obj_t *page_dsyn_build(lv_obj_t *parent)
{
    if (parent == NULL) return NULL;
    styles_init();
    /* 重建复位：控件指针/脏检查；s_page/s_focus 是跨组保留的 UI 局部态 */
    memset(s_knobs, 0, sizeof(s_knobs));
    memset(s_present, 0, sizeof(s_present));
    memset(s_hidden, 0, sizeof(s_hidden));
    memset(s_page_shown, 0xFF, sizeof(s_page_shown));
    s_kit_sel_shown = 0xFF;
    s_shown_page = 0xFF;
    s_shown_lane = 0xFF;
    s_focus_shown = 0xFF;
    s_status_cache[0] = '\0';
    s_status_color_shown = 0xFFFFFFFFu;
    s_act = 0xFF;
    status_store("");

    /* 8 个旋钮常驻（当前字段页的 desc/user/显隐由 update 的 apply_page 落地） */
    for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) {
        int16_t cx = (int16_t)(DSYN_GRID_X0 + (slot % 4u) * DSYN_CELL_PITCH);
        int16_t cy = (int16_t)(DSYN_GRID_Y0 + (slot / 4u) * DSYN_ROW_PITCH);
        sloop_desc_t d = {"?", NULL, SLOOP_DESC_FMT_INT, 0, 1, 0, NULL, 0};
        if (!knob_create(&s_knobs[slot], parent, cx, cy, &d, NULL, knob_change_cb,
                         knob_focus_cb)) {
            return NULL;
        }
        lv_obj_set_hidden(s_knobs[slot].obj, true);
        s_hidden[slot] = true;
    }

    for (uint8_t k = 0; k < SLOOP_DSYN_KITS; k++) {
        char text[8];
        snprintf(text, sizeof(text), "SYN%u", (unsigned)(k + 1u));
        s_kit_chip[k] = make_chip(parent, (int16_t)(DSYN_KIT_X0 + (int16_t)k * DSYN_KIT_PITCH),
                                  DSYN_HEAD_Y, DSYN_KIT_W, DSYN_CHIP_H, text, &s_st_chip,
                                  ((uintptr_t)DSYN_OP_KIT << 8) | k);
        UI_BUILD_CHECK(s_kit_chip[k]);
    }

    /* LANE 三区对象：`-   LANE nn   +`（点击左右区 = lane ∓1；中段是标签）。
     * 不用 make_chip：它挂的是 chip_cb（op/arg 编码），这里需要读指针位置的专用回调。 */
    s_lane_chip = lv_label_create(parent);
    UI_BUILD_CHECK(s_lane_chip);
    lv_obj_set_pos(s_lane_chip, DSYN_LANE_X, DSYN_HEAD_Y);
    lv_obj_set_size(s_lane_chip, DSYN_LANE_W, DSYN_CHIP_H);
    lv_obj_add_style(s_lane_chip, &s_st_chip, 0);
    lv_label_set_text(s_lane_chip, "-   LANE 01   +");
    lv_obj_set_clickable(s_lane_chip, true);
    lv_obj_add_event_cb(s_lane_chip, lane_cb, LV_EVENT_CLICKED, NULL);

    for (uint8_t p = 0; p < DSYN_PAGES; p++) {
        s_page_chip[p] = make_chip(parent, (int16_t)(DSYN_PAGE_X0 + (int16_t)p * DSYN_PAGE_PITCH),
                                   DSYN_HEAD_Y, DSYN_PAGE_W, DSYN_CHIP_H, k_page_text[p],
                                   &s_st_chip, ((uintptr_t)DSYN_OP_PAGE << 8) | p);
        UI_BUILD_CHECK(s_page_chip[p]);
    }

    s_audition = make_chip(parent, DSYN_AUD_X, DSYN_HEAD_Y, DSYN_AUD_W, DSYN_CHIP_H, "AUDITION",
                           &s_st_chip, (uintptr_t)DSYN_OP_AUDITION << 8);
    UI_BUILD_CHECK(s_audition);
    s_store = make_chip(parent, DSYN_STORE_X, DSYN_HEAD_Y, DSYN_STORE_W, DSYN_CHIP_H, "STORE",
                        &s_st_chip, (uintptr_t)DSYN_OP_STORE << 8);
    UI_BUILD_CHECK(s_store);

    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_obj_set_pos(s_status, DSYN_STATUS_X, DSYN_HEAD_Y);
    lv_obj_set_size(s_status, DSYN_STATUS_W, DSYN_CHIP_H);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(s_status, 6, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_status, "");

    s_adj_dec = make_chip(parent, DSYN_ADJ_DEC_X, DSYN_ADJ_Y, DSYN_ADJ_W, DSYN_ADJ_H, "-",
                          &s_st_adj, ((uintptr_t)DSYN_OP_ADJ << 8) | 0u);
    UI_BUILD_CHECK(s_adj_dec);
    s_adj_inc = make_chip(parent, DSYN_ADJ_INC_X, DSYN_ADJ_Y, DSYN_ADJ_W, DSYN_ADJ_H, "+",
                          &s_st_adj, ((uintptr_t)DSYN_OP_ADJ << 8) | 1u);
    UI_BUILD_CHECK(s_adj_inc);

    return parent;
}

/* ---------------------------------------------------------------- 刷新 --- */
/* 字段页变化：重配 8 个旋钮的 desc/user/显隐，并把焦点夹到当前页存在的槽。 */
static void apply_page(uint8_t page)
{
    for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) {
        uint8_t field = k_fields[page][slot];
        knob_t *k = &s_knobs[slot];
        sloop_desc_t d;
        bool have = field != DSYN_FIELD_NONE && field_desc(field, &d);
        s_present[slot] = have;
        if (!have) {
            if (!s_hidden[slot]) {
                lv_obj_set_hidden(k->obj, true);
                s_hidden[slot] = true;
            }
            continue;
        }
        k->desc = d;
        k->user = (void *)(uintptr_t)(field + 1u);
        k->text_cb = dsyn_knob_text;
        k->shown = false; /* 新描述符：强制重绘名称/数值 */
        if (s_hidden[slot]) {
            lv_obj_set_hidden(k->obj, false);
            s_hidden[slot] = false;
        }
        lv_obj_invalidate(k->obj);
    }
    /* 隐藏槽不会被 update 的焦点循环覆盖：先全部清焦点态，再让本轮重新落地 */
    for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) knob_set_active(&s_knobs[slot], false);
    uint8_t f = focus_get();
    if (!s_present[f]) {
        for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) {
            if (s_present[slot]) {
                focus_set(slot);
                break;
            }
        }
    }
}

void page_dsyn_update(const app_state_t *st)
{
    if (st == NULL || s_status == NULL) return;
    const bool ready = sloop_store_ready();
    const bool dirty = st->dsyn.dirty != 0;
    uint8_t page = page_get();

    if (page != s_shown_page) {
        s_shown_page = page;
        apply_page(page);
        s_focus_shown = 0xFF; /* 焦点可能被夹过 */
    }

    /* kit/lane 选择行 */
    if (st->dsyn.kit != s_kit_sel_shown) {
        for (uint8_t k = 0; k < SLOOP_DSYN_KITS; k++) {
            if (k == st->dsyn.kit) lv_obj_add_style(s_kit_chip[k], &s_st_chip_on, 0);
            else lv_obj_remove_style(s_kit_chip[k], &s_st_chip_on, 0);
        }
        s_kit_sel_shown = st->dsyn.kit;
    }
    if (s_shown_lane != st->dsyn.lane) {
        char text[24];
        s_shown_lane = st->dsyn.lane;
        snprintf(text, sizeof(text), "-   LANE %02u   +", (unsigned)(st->dsyn.lane + 1u));
        lv_label_set_text(s_lane_chip, text);
    }
    for (uint8_t p = 0; p < DSYN_PAGES; p++) {
        uint8_t on = p == page ? 1u : 0u;
        if (s_page_shown[p] == on) continue;
        s_page_shown[p] = on;
        if (on) lv_obj_add_style(s_page_chip[p], &s_st_chip_on, 0);
        else lv_obj_remove_style(s_page_chip[p], &s_st_chip_on, 0);
    }

    /* 旋钮值（读投影；空槽已隐藏） */
    for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) {
        if (!s_present[slot]) continue;
        int16_t v = field_value(st, k_fields[page][slot]);
        if (v == SLOOP_DSYN_INVALID) continue;
        knob_t *k = &s_knobs[slot];
        if (!k->shown || k->locked || k->value != v) knob_show(k, v);
    }
    uint8_t focus = focus_get();
    if (focus != s_focus_shown) {
        for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) {
            if (s_present[slot]) knob_set_active(&s_knobs[slot], slot == focus);
        }
        s_focus_shown = focus;
    }
    sloop_ui_zoom_knob(s_present[focus] ? &s_knobs[focus] : NULL);

    /* 状态行（单一映射：动作 × synth_status × dirty）。KIT 页没有动作结果时用状态行
     * 显示 kit 名称（`KIT <name>`，只读）：名称/来源在 spec 里属于 KIT 页的只读呈现，
     * 用已有的状态行承载可省一个对象（池预算见 ADR-008 §3 的 DSYN 行）。 */
    char text[48];
    ui_dsyn_status_text(text, sizeof(text), s_act, st->synth_status, dirty, ready);
    if (text[0] == '\0' && page == (DSYN_PAGES - 1u)) {
        snprintf(text, sizeof(text), "KIT %s", st->dsyn.name);
    }
    uint32_t color = ui_dsyn_status_color(s_act, st->synth_status, dirty);
    if (strcmp(text, s_status_cache) != 0) {
        snprintf(s_status_cache, sizeof(s_status_cache), "%s", text);
        lv_label_set_text(s_status, s_status_cache);
        status_store(text);
    }
    uint32_t key = text[0] != '\0' ? color : 0u;
    if (key != s_status_color_shown) {
        s_status_color_shown = key;
        lv_obj_set_style_text_color(s_status, lv_color_hex(key == 0u ? UI_TEXT_DIM : key), 0);
    }
}

void page_dsyn_forget(void)
{
    memset(s_knobs, 0, sizeof(s_knobs));
    memset(s_present, 0, sizeof(s_present));
    memset(s_hidden, 0, sizeof(s_hidden));
    memset(s_kit_chip, 0, sizeof(s_kit_chip));
    s_lane_chip = NULL;
    memset(s_page_chip, 0, sizeof(s_page_chip));
    s_audition = s_store = s_status = NULL;
    s_adj_dec = s_adj_inc = NULL;
    s_shown_page = 0xFF;
    s_kit_sel_shown = 0xFF;
    s_shown_lane = 0xFF;
    s_focus_shown = 0xFF;
    memset(s_page_shown, 0xFF, sizeof(s_page_shown));
    s_status_cache[0] = '\0';
    status_store("");
}

/* ---------------------------------------------------------------- TAP --- */
static bool rect_hit(int16_t x, int16_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void out_select(sloop_tap_result_t *out, const app_state_t *st, uint8_t page)
{
    if (out == NULL) return;
    out->kind = SLOOP_TAP_DSYN_SEL;
    out->a = st->dsyn.kit;
    out->b = st->dsyn.lane;
    out->d = (int8_t)page;
    out->v = 1;
}

static void out_field(sloop_tap_result_t *out, uint8_t field, const dsyn_write_t *r)
{
    if (out == NULL) return;
    out->kind = SLOOP_TAP_DSYN_FIELD;
    out->a = field;
    out->v = r->applied;
    out->err = r->ok ? 0u : 2u; /* 2 = 命令失败（synth_status != OK） */
}

static void out_kit(sloop_tap_result_t *out, uint8_t kit, int16_t src, bool ok)
{
    if (out == NULL) return;
    out->kind = SLOOP_TAP_DSYN_KIT;
    out->a = kit;
    out->b = (uint8_t)(src < 0 ? 0 : src);
    out->v = src;
    out->err = ok ? 0u : 2u;
}

bool page_dsyn_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (s_status == NULL) return false;
    app_state_t st;
    state_get_copy(&st);
    uint8_t page = page_get();

    for (uint8_t k = 0; k < SLOOP_DSYN_KITS; k++) {
        if (!rect_hit(x, y, DSYN_KIT_X0 + (int16_t)k * DSYN_KIT_PITCH, DSYN_HEAD_Y, DSYN_KIT_W,
                      DSYN_CHIP_H)) {
            continue;
        }
        dsyn_select(k, st.dsyn.lane);
        state_get_copy(&st);
        out_select(out, &st, page);
        return true;
    }
    if (rect_hit(x, y, DSYN_LANE_X, DSYN_HEAD_Y, DSYN_LANE_W, DSYN_CHIP_H)) {
        int32_t rel = x - DSYN_LANE_X;
        if (rel >= DSYN_LANE_ZONE && rel < DSYN_LANE_W - DSYN_LANE_ZONE) return false; /* 标签中段 */
        dsyn_lane_move(rel < DSYN_LANE_ZONE ? -1 : 1);
        state_get_copy(&st);
        out_select(out, &st, page);
        return true;
    }
    for (uint8_t p = 0; p < DSYN_PAGES; p++) {
        if (!rect_hit(x, y, DSYN_PAGE_X0 + (int16_t)p * DSYN_PAGE_PITCH, DSYN_HEAD_Y, DSYN_PAGE_W,
                      DSYN_CHIP_H)) {
            continue;
        }
        sloop_ui_dsyn_page_set(p);
        refresh();
        out_select(out, &st, p);
        return true;
    }
    if (rect_hit(x, y, DSYN_AUD_X, DSYN_HEAD_Y, DSYN_AUD_W, DSYN_CHIP_H)) {
        dsyn_audition();
        if (out != NULL) {
            out->kind = SLOOP_TAP_DSYN_AUDITION;
            out->a = st.dsyn.kit;
            out->b = st.dsyn.lane;
        }
        return true;
    }
    if (rect_hit(x, y, DSYN_STORE_X, DSYN_HEAD_Y, DSYN_STORE_W, DSYN_CHIP_H)) {
        dsyn_store();
        if (out != NULL) out->kind = SLOOP_TAP_DSYN_STORE;
        return true;
    }
    if (rect_hit(x, y, DSYN_ADJ_DEC_X, DSYN_ADJ_Y, DSYN_ADJ_W, DSYN_ADJ_H) ||
        rect_hit(x, y, DSYN_ADJ_INC_X, DSYN_ADJ_Y, DSYN_ADJ_W, DSYN_ADJ_H)) {
        bool inc = rect_hit(x, y, DSYN_ADJ_INC_X, DSYN_ADJ_Y, DSYN_ADJ_W, DSYN_ADJ_H);
        uint8_t field = k_fields[page][focus_get()];
        if (field == DSYN_FIELD_NONE) return false; /* 空槽：焦点不落在空槽，防御 */
        dsyn_write_t r = dsyn_step(inc ? 1 : -1, true);
        if (field == DSYN_FIELD_KITSRC) out_kit(out, st.dsyn.kit, r.applied, r.ok);
        else out_field(out, field, &r);
        return true;
    }
    for (uint8_t slot = 0; slot < DSYN_SLOTS; slot++) {
        if (!s_present[slot]) continue;
        int16_t cx = (int16_t)(DSYN_GRID_X0 + (slot % 4u) * DSYN_CELL_PITCH);
        int16_t cy = (int16_t)(DSYN_GRID_Y0 + (slot / 4u) * DSYN_ROW_PITCH);
        if (!rect_hit(x, y, cx + KNOB_OBJ_DX, cy + KNOB_OBJ_DY, KNOB_OBJ_SIZE, KNOB_OBJ_SIZE)) {
            continue;
        }
        focus_set(slot);
        refresh();
        out_select(out, &st, page);
        return true;
    }
    return false;
}

/* ------------------------------------------------------- InputManager 路由 --- */
bool sloop_ui_dsyn_page_active(void)
{
    return sloop_ui_current_page() == APP_PAGE_DSYN;
}

void sloop_ui_dsyn_focus_move(int8_t delta)
{
    if (delta == 0) return;
    uint8_t page = page_get();
    uint8_t f = focus_get();
    for (uint8_t n = 0; n < DSYN_SLOTS; n++) {
        f = (uint8_t)((f + DSYN_SLOTS + delta) % DSYN_SLOTS);
        if (k_fields[page][f] != DSYN_FIELD_NONE) break;
    }
    focus_set(f);
    /* 输入任务上下文：不碰控件，描边由下一拍 update 落地 */
}

void sloop_ui_dsyn_adjust(int8_t delta)
{
    if (delta == 0) return;
    (void)dsyn_step(delta, false); /* 输入任务上下文：不碰控件，下一拍 UI 刷新 */
}
