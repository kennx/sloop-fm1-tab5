/*
 * FM6P 深度 patch 编辑页（P4-D Task 10，spec §3.6/§4.1/§4.4；FM6 155B voice）
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H=72）：
 * - 页眉一行 y∈[4,36)：`OP1..OP6` 六个算子 chip（x=16+60k，56px）、字段页 chip
 *   `EG/LEVEL/PITCH/PEG/VOICE/LFO`（x=380+68p，64px）、BANK 三区对象
 *   `−  Bnn  +`（792..916；左右 40px 是 −/+ 区、中段是槽号标签）、`SAVE`
 *   （924，72px）、状态行（1004..1264，16pt；文本来自 theme.h 的
 *   ui_fm6p_status_text 单一映射）。
 * - 旋钮网格与参数页/DSYN 页同值：4 列 × 2 行，cell 300×290，x = 16 + c·316，
 *   y = 52 + r·298；旋钮盒 160×160 在单元内 (70,26)（自绘见 knob.c）。8 个旋钮
 *   对象**常驻**，当前字段页没有字段的槽 `lv_obj_set_hidden(true)`（视觉/命中/TAP
 *   都与「未建」等价——池峰值按 8 个建）。
 * - 底部细调：**一个三区对象** `−        +`（16..184，高 44，页面区 y=552..596；
 *   旋钮盒底边 536 之下）。左右各 72px 是 −/+（焦点槽 ±1 个原始单位），**中段
 *   24px（72..96）是死区**（TAP 命中按 miss，避免误触单步写）。DSYN 页用两个独立
 *   对象；这里合成一个以守住 est 7,168B（24 个对象，见 ADR-008 §3 的 FM6P 行）。
 *
 * 字段页（**页内 UI 局部态**，`UI FM6P PAGE` 与页面 chip 共用；不新增 app page）。
 * 算子在 EG/LEVEL/PITCH 三页决定编辑哪个算子块（op 0..5），PEG/VOICE/LFO 三页是
 * **全局块**（op 6，与算子无关，算子 chip 只影响前三个页面）：
 *   EG    = R1..R4 / L1..L4            （op 字段 0..7）
 *   LEVEL = BP/LD/RD/LC/RC/RS/KVS/OL   （op 字段 8..13,15,16；AMS=14 在 PITCH 页）
 *   PITCH = MODE/FC/FF/DET/AMS + 3 空槽（op 字段 17..20,14）
 *   PEG   = PR1..PR4 / PL1..PL4        （全局 0..7）
 *   VOICE = ALG/FB/OKS/TRNSP + 4 空槽 （全局 8/9/10/18）
 *   LFO   = LFS/LFD/LPMD/LAMD/LKS/LFW/LPMS + 1 空槽（全局 11..17）
 * 字段编号 = Task 3 冻结的编号（op 0..20 = FP_R1..FP_DET；全局 0..18 =
 * FP_PR1..FP_TRNSP），名称/范围/枚举全部来自 `sloop_core_fm6_desc` + `fm6_max()`，
 * UI 不引入上游表；全局 19..28 是名称字节（只读），不进任何页。
 *
 * 值路径（唯一）：读 `app_state_t.fm6p`（选中轨的 155B live patch 投影；纯解码
 * `sloop_fm6_field_from_patch`）→ 一条状态层命令（`CMD_FM6_SET` / `CMD_BANK_PUT`）
 * → 命令同拍刷新的投影 → 重绘本页。数字文本走 knob_format_value（枚举/ONOFF 由
 * desc 决定，无需 core 的索引→文本表）。
 *
 * ALG 宏覆盖（spec §4.1）：ALG = 全局字段 8（`SLOOP_FM6_ALG_FIELD` = `FP_ALG - FP_PR1`），
 * 写 ALG 时适配层在同一次引擎锁内清 `P_E0`（ALG 宏 → PAT），否则上游 `fm6_sync`
 * 会用宏覆盖 patch 的算法、编辑听不到。`st->p[P_E0] != 0` 时状态行显示
 * `ALG MACRO n`：宏在覆盖算法，深度页的 ALG 编辑要「实测」才听得到。提示的优先级
 * 高于成功结果（成功是瞬时的，覆盖警告是持续事实），低于失败（失败必须先被看见）；
 * 编辑 ALG 成功后宏已被清，提示自然消失。
 *
 * 锁定（`st->fm6p.engine_ok == 0`：选中轨不是 FM6，含鼓轨）：8 个旋钮一律 `--`，
 * OP/PAGE/BANK chip、SAVE 与底部 ± 全部拒绝（**不下发命令**、不改选择），状态行
 * `NOT FM6`（红字）。解锁时清掉上一次的动作结果（新上下文不显示旧结果）。
 *
 * SAVE = `CMD_BANK_PUT`（选中轨的 live patch 写入页内目标槽 0..26，与 FM6 宏页的
 * bank 行同一套 27 槽、同一映射：播放中 `STOP BEFORE SAVE`、未就绪 `NO STORAGE`）；
 * 擦写在 LVGL 回调里，最坏阻塞一次整银行擦写窗口（spec §4.3 接受）。
 *
 * 线程：build/update/tap/forget 在 LVGL 锁内（ui.c）；`sloop_ui_fm6p_*` 键盘路由在
 * 输入任务上下文，不取 LVGL 锁；console `UI FM6P OP/PAGE` 在 REPL 任务。页/算子/
 * 焦点是临界区保护的单字节，状态文本跨任务读写也走同一临界区（与 SONG/DSYN 页
 * 同纪律）。样式只在开机初始化一次（组作用域会反复销毁/重建本页，ADR-008 §3 R34）。
 */
#include "page_fm6p.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "knob.h"
#include "sloop_core.h"
#include "theme.h"

/* ------------------------------------------------------------ 几何/字段 --- */
#define FM6P_OPS 6u
#define FM6P_PAGES 6u
#define FM6P_SLOTS 8u

#define FM6P_HEAD_Y      4
#define FM6P_CHIP_H      32
#define FM6P_OP_X0       16
#define FM6P_OP_W        56
#define FM6P_OP_PITCH    60
#define FM6P_PAGE_X0     380
#define FM6P_PAGE_W      64
#define FM6P_PAGE_PITCH  68
/* BANK 行是一个**三区**对象（`−  Bnn  +`，宽 124px）：池预算下把槽号与两个 ± 合成
 * 1 个对象（est 7,168B 见 ADR-008 §3）。左右各 40px 是 −/+ 区，中段是槽号标签
 * （TAP 命中中段按 miss 处理）。 */
#define FM6P_BANK_X      792
#define FM6P_BANK_W      124
#define FM6P_BANK_ZONE   40
#define FM6P_SAVE_X      924
#define FM6P_SAVE_W      72
#define FM6P_STATUS_X    1004
#define FM6P_STATUS_W    260

/* 与 page_params.c / page_dsyn.c 的网格同值（knob.c 的单元内偏移是共享的） */
#define FM6P_GRID_X0    16
#define FM6P_GRID_Y0    52
#define FM6P_CELL_PITCH 316
#define FM6P_ROW_PITCH  298

/* 底部细调：一个三区对象（左 −/中死区/右 +）。作用区 72px、对象宽 168 ⇒ 中间
 * 72..96 是 24px 死区（`rel >= ZONE && rel < W - ZONE` 成立）；把 ZONE 设成半个
 * 宽度会让整条都变成 −/+，中点误触即写值。 */
#define FM6P_ADJ_Y      552
#define FM6P_ADJ_X      16
#define FM6P_ADJ_W      168
#define FM6P_ADJ_H      44
#define FM6P_ADJ_ZONE   72

/* ALG 宏在 p[] 里的绝对 id（上游 P_E0 = P_COUNT − 8，与 page_params 的引擎槽起点
 * 同一常量；`CMD_FM6_SET` 写 ALG 时由适配层清它）。 */
#define FM6P_ALG_MACRO ((uint8_t)(SLOOP_CORE_P_COUNT - 8u))
#define FM6P_NONE 0xFFu

/* 单元格 = (全局块?, 字段号)。global=0 时 op 取当前选中的算子（OP1..OP6），
 * global=1 时 op 固定为 SLOOP_FM6_GLOBAL_OP（6）。 */
typedef struct {
    uint8_t global;
    uint8_t field;
} fm6p_cell_t;

static const fm6p_cell_t k_cells[FM6P_PAGES][FM6P_SLOTS] = {
    /* EG：R1..R4 / L1..L4 */
    {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}},
    /* LEVEL：BP/LD/RD/LC/RC/RS/KVS/OL（AMS 在 PITCH 页） */
    {{0, 8}, {0, 9}, {0, 10}, {0, 11}, {0, 12}, {0, 13}, {0, 15}, {0, 16}},
    /* PITCH：MODE/FC/FF/DET/AMS + 3 空槽 */
    {{0, 17}, {0, 18}, {0, 19}, {0, 20}, {0, 14},
     {0, FM6P_NONE}, {0, FM6P_NONE}, {0, FM6P_NONE}},
    /* PEG：PR1..PR4 / PL1..PL4（全局 0..7） */
    {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6}, {1, 7}},
    /* VOICE：ALG/FB/OKS/TRNSP + 4 空槽（全局 8/9/10/18） */
    {{1, SLOOP_FM6_ALG_FIELD}, {1, 9}, {1, 10}, {1, 18},
     {1, FM6P_NONE}, {1, FM6P_NONE}, {1, FM6P_NONE}, {1, FM6P_NONE}},
    /* LFO：LFS/LFD/LPMD/LAMD/LKS/LFW/LPMS + 1 空槽（全局 11..17） */
    {{1, 11}, {1, 12}, {1, 13}, {1, 14}, {1, 15}, {1, 16}, {1, 17},
     {1, FM6P_NONE}},
};

static const char *const k_page_text[FM6P_PAGES] = {"EG", "LEVEL", "PITCH",
                                                    "PEG", "VOICE", "LFO"};

/* ------------------------------------------------------------ 页内状态 --- */
static knob_t s_knobs[FM6P_SLOTS];
static bool s_present[FM6P_SLOTS]; /* 当前字段页的槽有字段（控件已显式） */
static bool s_hidden[FM6P_SLOTS];  /* 控件当前是隐藏态 */
static uint8_t s_slot_op[FM6P_SLOTS];    /* 该槽解析出的 op（0..5 或 6） */
static uint8_t s_slot_field[FM6P_SLOTS]; /* 该槽的字段号 */
static lv_obj_t *s_op_chip[FM6P_OPS];
static lv_obj_t *s_page_chip[FM6P_PAGES];
static lv_obj_t *s_bank_chip;
static lv_obj_t *s_save;
static lv_obj_t *s_status;
static lv_obj_t *s_adj;

static lv_style_t s_st_chip;
static lv_style_t s_st_chip_on;
static lv_style_t s_st_adj;

/* UI 局部态（跨任务读写）：s_page/s_op/s_focus 是单字节；状态文本是字符串，故一起
 * 放在同一临界区（与 SONG/DSYN 页的 s_mux 同纪律）。 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_page;         /* 字段页 0..5（跨组页面重建保留） */
static uint8_t s_op;           /* 算子 0..5（OP1..OP6；只影响 EG/LEVEL/PITCH） */
static uint8_t s_focus;        /* 焦点槽 0..7 */
static char s_status_text[48]; /* 当前屏幕状态行文本（console TAP 回显同源） */
static char s_status_echo[48]; /* console TAP 回显专用（REPL 任务） */

/* BANK 目标槽（页内 UI 局部态，0..26，默认 0；TAP 与控件回调都在 LVGL 上下文读写）。
 * 与 FM6 宏页的 bank 行独立（spec §3.6：FM6P 只加一个 SAVE 指向同一套槽）。 */
static uint8_t s_bank_slot;

/* 最近一次动作（页内局部态，0xFF = 尚无；换页/换算子/锁定态变化时复位）。
 * 单字节写，读侧按值使用（与 DSYN/SONG 页同纪律）。 */
static uint8_t s_act = 0xFF;
/* 脏检查缓存 */
static uint8_t s_shown_page = 0xFF;
static uint8_t s_shown_op = 0xFF;
static uint8_t s_shown_bank = 0xFF;
static int8_t s_shown_locked = -1;
static uint8_t s_op_shown = 0xFF;
static uint8_t s_page_shown[FM6P_PAGES];
static uint8_t s_focus_shown = 0xFF;
static char s_status_cache[48];
static uint32_t s_status_color_shown = 0xFFFFFFFFu;

/* ------------------------------------------------------------ 小工具 --- */
static uint8_t page_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t p = s_page;
    portEXIT_CRITICAL(&s_mux);
    return p < FM6P_PAGES ? p : 0u;
}

static uint8_t op_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t op = s_op;
    portEXIT_CRITICAL(&s_mux);
    return op < FM6P_OPS ? op : 0u;
}

static uint8_t focus_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t f = s_focus;
    portEXIT_CRITICAL(&s_mux);
    return f < FM6P_SLOTS ? f : 0u;
}

static void focus_set(uint8_t slot)
{
    if (slot >= FM6P_SLOTS) return;
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

/* 单元格当前值（纯读投影，不取引擎锁）。空槽返回 SLOOP_FM6_INVALID。 */
static int16_t cell_value(const app_state_t *st, uint8_t slot)
{
    if (slot >= FM6P_SLOTS || !s_present[slot]) return SLOOP_FM6_INVALID;
    return sloop_fm6_field_from_patch(st->fm6p.patch, s_slot_op[slot], s_slot_field[slot]);
}

const char *page_fm6p_status_text(void)
{
    if (s_status == NULL) return NULL; /* 页未建/已随组作用域销毁（与 BANK/SAMPLE 行同口径） */
    portENTER_CRITICAL(&s_mux);
    snprintf(s_status_echo, sizeof(s_status_echo), "%s", s_status_text);
    portEXIT_CRITICAL(&s_mux);
    return s_status_echo;
}

const char *sloop_ui_fm6p_field_name(uint8_t op, uint8_t field)
{
    sloop_desc_t d;
    return sloop_core_fm6_desc(op, field, &d) ? d.label : NULL;
}

uint8_t sloop_ui_fm6p_page_get(void)
{
    return page_get();
}

void sloop_ui_fm6p_page_set(uint8_t page)
{
    if (page >= FM6P_PAGES) return;
    portENTER_CRITICAL(&s_mux);
    s_page = page;
    portEXIT_CRITICAL(&s_mux);
    /* 换字段页是上下文切换：不保留上一页的动作结果 */
    s_act = 0xFF;
}

uint8_t sloop_ui_fm6p_op_get(void)
{
    return op_get();
}

void sloop_ui_fm6p_op_set(uint8_t op)
{
    if (op >= FM6P_OPS) return;
    portENTER_CRITICAL(&s_mux);
    s_op = op;
    portEXIT_CRITICAL(&s_mux);
    s_act = 0xFF;
}

/* ------------------------------------------------------------ 动作函数 --- */
static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_fm6p_update(&st);
}

/* 目标 bank 槽 ±1（环绕 0..26）：只改页内局部态，不碰存储/引擎、不下发命令。 */
static void bank_move(int8_t delta)
{
    int32_t v = (int32_t)s_bank_slot + delta;
    while (v < 0) v += SLOOP_STORE_BANK_SLOTS;
    while (v >= (int32_t)SLOOP_STORE_BANK_SLOTS) v -= (int32_t)SLOOP_STORE_BANK_SLOTS;
    s_bank_slot = (uint8_t)v;
    refresh();
}

typedef struct {
    bool ok;         /* 命令成功（synth_status == OK） */
    int16_t applied; /* 写后回读（投影里的真值；失败 = 写前真值） */
} fm6p_write_t;

/* 快照纪律（R13）：键盘路径 sloop_ui_fm6p_adjust → fm6p_step → fm6p_write 跑在输入任务
 * （栈 8,192B）上，整条链**只持一份** app_state_t——由最外层持有、往下传同一个缓冲，命令后
 * 原位刷新（不叠第二份）。LVGL 上下文（旋钮拖拽、TAP、底部 ±）用不带 `_st` 的薄封装，各自
 * 持一份。 */
static fm6p_write_t fm6p_write_st(uint8_t slot, int16_t value, bool render, app_state_t *st);

static fm6p_write_t fm6p_write(uint8_t slot, int16_t value, bool render)
{
    app_state_t st;
    return fm6p_write_st(slot, value, render, &st);
}

/* 焦点槽 ±1（底部 ±、键盘 ↑/↓ 共用）：与旋钮拖拽走同一个 fm6p_write。 */
static fm6p_write_t fm6p_step_st(int8_t delta, bool render, app_state_t *st)
{
    fm6p_write_t r = {false, 0};
    uint8_t slot = focus_get();
    if (slot >= FM6P_SLOTS || !s_present[slot]) return r;
    state_get_copy(st);
    int16_t v = cell_value(st, slot);
    r.applied = v;
    if (delta == 0 || v == SLOOP_FM6_INVALID) return r;
    return fm6p_write_st(slot, (int16_t)(v + delta), render, st);
}

static fm6p_write_t fm6p_step(int8_t delta, bool render)
{
    app_state_t st;
    return fm6p_step_st(delta, render, &st);
}

/* 单字段写（旋钮拖拽、键盘 ↑/↓、底部 ± 共用）：一条 `CMD_FM6_SET`。
 * render = 调用方是否在 LVGL 上下文（控件回调/TAP）：是则立即重绘本页；键盘路由在
 * 输入任务上下文，**不得**碰控件，只写状态层，由 30ms UI 定时器下一拍刷新
 * （与 page_params/page_dsyn 同一纪律）。st = 调用方的快照缓冲，入口与命令后各原位刷新一次。 */
static fm6p_write_t fm6p_write_st(uint8_t slot, int16_t value, bool render, app_state_t *st)
{
    fm6p_write_t r = {false, value};
    if (slot >= FM6P_SLOTS || !s_present[slot]) return r;
    state_get_copy(st);
    r.applied = cell_value(st, slot);
    if (!st->fm6p.engine_ok) return r; /* 锁定态：不下发命令（防御；调用方已拒绝） */
    s_act = UI_FM6P_ACT_FIELD;
    const edit_cmd_t cmd = {.type = CMD_FM6_SET,
                            .a = st->core.selected_track,
                            .b = s_slot_op[slot],
                            .n = s_slot_field[slot],
                            .v = (uint16_t)value};
    state_apply(&cmd);
    state_get_copy(st);
    r.applied = cell_value(st, slot);
    r.ok = st->synth_status == APP_SYNTH_STATUS_OK;
    if (render) refresh();
    return r;
}

/* SAVE = 选中轨的 live patch 写入页内目标槽（CMD_BANK_PUT）。锁定页（选中轨不是
 * FM6，含鼓轨）直接拒绝：不发命令、不动存储，状态行 `NOT FM6`。 */
static void fm6p_save(void)
{
    app_state_t st;
    state_get_copy(&st);
    s_act = UI_FM6P_ACT_SAVE;
    if (!st.fm6p.engine_ok) {
        refresh(); /* 本地拒绝：状态行由 locked 分支给出 NOT FM6 */
        return;
    }
    const edit_cmd_t cmd = {.type = CMD_BANK_PUT, .a = s_bank_slot};
    state_apply(&cmd);
    refresh();
}

/* ------------------------------------------------------------ 事件回调 --- */
/* 控件下标编码：op << 8 | arg（chip 一族共用本回调） */
enum {
    FM6P_CB_OP = 0,   /* arg = 算子 0..5 */
    FM6P_CB_PAGE = 1, /* arg = 字段页 0..5 */
    FM6P_CB_SAVE = 2,
};

static bool page_locked_now(void)
{
    app_state_t st;
    state_get_copy(&st);
    return st.fm6p.engine_ok == 0;
}

/* BANK / 底部细调的三区点击：读指针的页面区相对 x 判定 −/中/+ —— 中段是标签/留白，
 * 不动作（TAP 命中中段按 miss）。 */
static void zone_cb(lv_event_t *e)
{
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t zone = (uint8_t)(ctx & 0xFFu); /* 0 BANK / 1 ADJ */
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    int32_t rel = p.x - c.x1;
    int32_t w = zone == 0u ? FM6P_BANK_W : FM6P_ADJ_W;
    int32_t band = zone == 0u ? FM6P_BANK_ZONE : FM6P_ADJ_ZONE;
    if (rel >= band && rel < w - band) return; /* 中段：标签/留白 */
    int8_t delta = rel < band ? -1 : 1;
    if (page_locked_now()) return; /* 锁定页：chip 拒绝、不下发命令 */
    if (zone == 0u) {
        bank_move(delta);
    } else {
        (void)fm6p_step(delta, true);
    }
}

static void chip_cb(lv_event_t *e)
{
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t op = (uint8_t)(ctx >> 8);
    uint8_t arg = (uint8_t)(ctx & 0xFFu);
    switch (op) {
    case FM6P_CB_OP:
        if (page_locked_now()) return; /* 锁定页：chip 拒绝 */
        sloop_ui_fm6p_op_set(arg);
        refresh();
        break;
    case FM6P_CB_PAGE:
        if (page_locked_now()) return;
        sloop_ui_fm6p_page_set(arg);
        refresh();
        break;
    case FM6P_CB_SAVE: fm6p_save(); break;
    default: break;
    }
}

static void knob_change_cb(knob_t *k, int16_t value)
{
    uint8_t slot = 0;
    for (uint8_t i = 0; i < FM6P_SLOTS; i++) {
        if (&s_knobs[i] == k) {
            slot = i;
            break;
        }
    }
    (void)fm6p_write(slot, value, true);
}

static void knob_focus_cb(knob_t *k)
{
    uint8_t slot = 0;
    for (uint8_t i = 0; i < FM6P_SLOTS; i++) {
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

lv_obj_t *page_fm6p_build(lv_obj_t *parent)
{
    if (parent == NULL) return NULL;
    styles_init();
    /* 重建复位：控件指针/脏检查；s_page/s_op/s_focus/s_bank_slot 是跨组保留的
     * UI 局部态 */
    memset(s_knobs, 0, sizeof(s_knobs));
    memset(s_present, 0, sizeof(s_present));
    memset(s_hidden, 0, sizeof(s_hidden));
    memset(s_slot_op, 0, sizeof(s_slot_op));
    memset(s_slot_field, 0xFF, sizeof(s_slot_field));
    memset(s_page_shown, 0xFF, sizeof(s_page_shown));
    s_shown_page = 0xFF;
    s_shown_op = 0xFF;
    s_shown_bank = 0xFF;
    s_shown_locked = -1;
    s_op_shown = 0xFF;
    s_focus_shown = 0xFF;
    s_status_cache[0] = '\0';
    s_status_color_shown = 0xFFFFFFFFu;
    s_act = 0xFF;
    status_store("");

    /* 8 个旋钮常驻（当前字段页的 desc/显隐由 update 的 apply_page 落地；
     * FM6P 的回调按控件指针找槽位，不用 knob_t.user 编码） */
    for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) {
        int16_t cx = (int16_t)(FM6P_GRID_X0 + (slot % 4u) * FM6P_CELL_PITCH);
        int16_t cy = (int16_t)(FM6P_GRID_Y0 + (slot / 4u) * FM6P_ROW_PITCH);
        sloop_desc_t d = {"?", NULL, SLOOP_DESC_FMT_INT, 0, 1, 0, NULL, 0};
        if (!knob_create(&s_knobs[slot], parent, cx, cy, &d, NULL, knob_change_cb,
                         knob_focus_cb)) {
            return NULL;
        }
        lv_obj_set_hidden(s_knobs[slot].obj, true);
        s_hidden[slot] = true;
    }

    for (uint8_t k = 0; k < FM6P_OPS; k++) {
        char text[8];
        snprintf(text, sizeof(text), "OP%u", (unsigned)(k + 1u));
        s_op_chip[k] = make_chip(parent, (int16_t)(FM6P_OP_X0 + (int16_t)k * FM6P_OP_PITCH),
                                FM6P_HEAD_Y, FM6P_OP_W, FM6P_CHIP_H, text, &s_st_chip,
                                ((uintptr_t)FM6P_CB_OP << 8) | k);
        UI_BUILD_CHECK(s_op_chip[k]);
    }

    for (uint8_t p = 0; p < FM6P_PAGES; p++) {
        s_page_chip[p] = make_chip(parent, (int16_t)(FM6P_PAGE_X0 + (int16_t)p * FM6P_PAGE_PITCH),
                                   FM6P_HEAD_Y, FM6P_PAGE_W, FM6P_CHIP_H, k_page_text[p],
                                   &s_st_chip, ((uintptr_t)FM6P_CB_PAGE << 8) | p);
        UI_BUILD_CHECK(s_page_chip[p]);
    }

    /* BANK 三区对象：`−  Bnn  +`（点击左右区 = 槽 ∓1；中段是槽号标签）。
     * 不用 make_chip：它挂的是 chip_cb（op/arg 编码），这里需要读指针位置的专用回调。 */
    s_bank_chip = lv_label_create(parent);
    UI_BUILD_CHECK(s_bank_chip);
    lv_obj_set_pos(s_bank_chip, FM6P_BANK_X, FM6P_HEAD_Y);
    lv_obj_set_size(s_bank_chip, FM6P_BANK_W, FM6P_CHIP_H);
    lv_obj_add_style(s_bank_chip, &s_st_chip, 0);
    lv_label_set_text(s_bank_chip, "-  B01  +");
    lv_obj_set_clickable(s_bank_chip, true);
    lv_obj_add_event_cb(s_bank_chip, zone_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0);

    s_save = make_chip(parent, FM6P_SAVE_X, FM6P_HEAD_Y, FM6P_SAVE_W, FM6P_CHIP_H, "SAVE",
                       &s_st_chip, (uintptr_t)FM6P_CB_SAVE << 8);
    UI_BUILD_CHECK(s_save);

    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_obj_set_pos(s_status, FM6P_STATUS_X, FM6P_HEAD_Y);
    lv_obj_set_size(s_status, FM6P_STATUS_W, FM6P_CHIP_H);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(s_status, 6, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_status, "");

    /* 底部细调：一个三区对象（左 −/中留白/右 +），与 DSYN 的 ± 同一语义 */
    s_adj = lv_label_create(parent);
    UI_BUILD_CHECK(s_adj);
    lv_obj_set_pos(s_adj, FM6P_ADJ_X, FM6P_ADJ_Y);
    lv_obj_set_size(s_adj, FM6P_ADJ_W, FM6P_ADJ_H);
    lv_obj_add_style(s_adj, &s_st_adj, 0);
    lv_label_set_text(s_adj, "-          +");
    lv_obj_set_clickable(s_adj, true);
    lv_obj_add_event_cb(s_adj, zone_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1);

    return parent;
}

/* ---------------------------------------------------------------- 刷新 --- */
/* 字段页/算子变化：重配 8 个旋钮的 desc/显隐，并把焦点夹到当前页存在的槽。 */
static void apply_page(uint8_t page, uint8_t op)
{
    for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) {
        const fm6p_cell_t *c = &k_cells[page][slot];
        knob_t *k = &s_knobs[slot];
        uint8_t cop = c->global != 0 ? (uint8_t)SLOOP_FM6_GLOBAL_OP : op;
        sloop_desc_t d;
        bool have = c->field != FM6P_NONE && sloop_core_fm6_desc(cop, c->field, &d);
        s_present[slot] = have;
        s_slot_op[slot] = cop;
        s_slot_field[slot] = c->field;
        if (!have) {
            if (!s_hidden[slot]) {
                lv_obj_set_hidden(k->obj, true);
                s_hidden[slot] = true;
            }
            continue;
        }
        k->desc = d;
        k->text_cb = NULL; /* 枚举/ONOFF/整数都由 desc 决定，不需要页面文本回调 */
        k->shown = false; /* 新描述符：强制重绘名称/数值 */
        if (s_hidden[slot]) {
            lv_obj_set_hidden(k->obj, false);
            s_hidden[slot] = false;
        }
        lv_obj_invalidate(k->obj);
    }
    /* 隐藏槽不会被 update 的焦点循环覆盖：先全部清焦点态，再让本轮重新落地 */
    for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) knob_set_active(&s_knobs[slot], false);
    uint8_t f = focus_get();
    if (!s_present[f]) {
        for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) {
            if (s_present[slot]) {
                focus_set(slot);
                break;
            }
        }
    }
}

void page_fm6p_update(const app_state_t *st)
{
    if (st == NULL || s_status == NULL) return;
    const bool ready = sloop_store_ready();
    const bool locked = st->fm6p.engine_ok == 0;
    uint8_t page = page_get();
    uint8_t op = op_get();
    int16_t macro = st->p[FM6P_ALG_MACRO];
    uint8_t macro_n = (macro > 0 && macro <= 32) ? (uint8_t)macro : 0u;

    if (page != s_shown_page || op != s_shown_op) {
        s_shown_page = page;
        s_shown_op = op;
        apply_page(page, op);
        s_focus_shown = 0xFF; /* 焦点可能被夹过 */
    }
    /* 锁定态变化：清掉上一次的动作结果（新上下文不显示旧结果） */
    if ((int8_t)locked != s_shown_locked) {
        s_shown_locked = (int8_t)locked;
        s_act = 0xFF;
    }

    /* 算子/字段页选择行 */
    if (s_op_shown != op) {
        for (uint8_t k = 0; k < FM6P_OPS; k++) {
            if (k == op) lv_obj_add_style(s_op_chip[k], &s_st_chip_on, 0);
            else lv_obj_remove_style(s_op_chip[k], &s_st_chip_on, 0);
        }
        s_op_shown = op;
    }
    for (uint8_t p = 0; p < FM6P_PAGES; p++) {
        uint8_t on = p == page ? 1u : 0u;
        if (s_page_shown[p] == on) continue;
        s_page_shown[p] = on;
        if (on) lv_obj_add_style(s_page_chip[p], &s_st_chip_on, 0);
        else lv_obj_remove_style(s_page_chip[p], &s_st_chip_on, 0);
    }
    if (s_bank_slot != s_shown_bank) {
        char text[16];
        s_shown_bank = s_bank_slot;
        snprintf(text, sizeof(text), "-  B%02u  +", (unsigned)(s_bank_slot + 1u));
        lv_label_set_text(s_bank_chip, text);
    }

    /* 旋钮值（读投影；空槽已隐藏）：锁定态全部 `--` */
    for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) {
        if (!s_present[slot]) continue;
        knob_t *k = &s_knobs[slot];
        if (locked) {
            if (!k->shown || !k->locked) knob_show_locked(k);
            continue;
        }
        int16_t v = cell_value(st, slot);
        if (v == SLOOP_FM6_INVALID) continue;
        if (!k->shown || k->locked || k->value != v) knob_show(k, v);
    }
    uint8_t focus = focus_get();
    if (focus != s_focus_shown) {
        for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) {
            if (s_present[slot]) knob_set_active(&s_knobs[slot], slot == focus);
        }
        s_focus_shown = focus;
    }
    sloop_ui_zoom_knob(s_present[focus] ? &s_knobs[focus] : NULL);

    /* 状态行（theme.h 的 ui_fm6p_status_text 单一映射：锁定 > 动作结果/失败 >
     * ALG 宏覆盖提示 > 成功结果 > 空） */
    char text[48];
    ui_fm6p_status_text(text, sizeof(text), s_act, st->synth_status, st->store_status, ready,
                        locked, macro_n);
    uint32_t color = ui_fm6p_status_color(s_act, st->synth_status, st->store_status, ready, locked,
                                          macro_n);
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

void page_fm6p_forget(void)
{
    memset(s_knobs, 0, sizeof(s_knobs));
    memset(s_present, 0, sizeof(s_present));
    memset(s_hidden, 0, sizeof(s_hidden));
    memset(s_slot_op, 0, sizeof(s_slot_op));
    memset(s_slot_field, 0xFF, sizeof(s_slot_field));
    memset(s_op_chip, 0, sizeof(s_op_chip));
    memset(s_page_chip, 0, sizeof(s_page_chip));
    s_bank_chip = NULL;
    s_save = s_status = s_adj = NULL;
    s_shown_page = 0xFF;
    s_shown_op = 0xFF;
    s_shown_bank = 0xFF;
    s_shown_locked = -1;
    s_op_shown = 0xFF;
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

static void out_select(sloop_tap_result_t *out, uint8_t op, uint8_t page)
{
    if (out == NULL) return;
    out->kind = SLOOP_TAP_FM6P_SEL;
    out->a = op;
    out->b = page;
    out->v = 1;
}

static void out_field(sloop_tap_result_t *out, uint8_t op, uint8_t field,
                      const fm6p_write_t *r, bool refused)
{
    if (out == NULL) return;
    out->kind = SLOOP_TAP_FM6P_FIELD;
    out->a = op;
    out->b = field;
    out->v = r->applied;
    out->err = refused ? 1u : (r->ok ? 0u : 2u); /* 1 = 锁定页拒绝（未下发命令） */
}

bool page_fm6p_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (s_status == NULL) return false;
    app_state_t st;
    state_get_copy(&st);
    uint8_t page = page_get();
    uint8_t op = op_get();
    const bool locked = st.fm6p.engine_ok == 0;

    for (uint8_t k = 0; k < FM6P_OPS; k++) {
        if (!rect_hit(x, y, FM6P_OP_X0 + (int16_t)k * FM6P_OP_PITCH, FM6P_HEAD_Y, FM6P_OP_W,
                      FM6P_CHIP_H)) {
            continue;
        }
        if (!locked) {
            sloop_ui_fm6p_op_set(k);
            op = op_get();
        }
        out_select(out, op, page);
        return true;
    }
    for (uint8_t p = 0; p < FM6P_PAGES; p++) {
        if (!rect_hit(x, y, FM6P_PAGE_X0 + (int16_t)p * FM6P_PAGE_PITCH, FM6P_HEAD_Y,
                      FM6P_PAGE_W, FM6P_CHIP_H)) {
            continue;
        }
        if (!locked) {
            sloop_ui_fm6p_page_set(p);
            page = page_get();
        }
        out_select(out, op, page);
        return true;
    }
    if (rect_hit(x, y, FM6P_BANK_X, FM6P_HEAD_Y, FM6P_BANK_W, FM6P_CHIP_H)) {
        int32_t rel = x - FM6P_BANK_X;
        if (rel >= FM6P_BANK_ZONE && rel < FM6P_BANK_W - FM6P_BANK_ZONE) return false; /* 槽号标签 */
        int8_t delta = rel < FM6P_BANK_ZONE ? -1 : 1;
        if (!locked) bank_move(delta);
        if (out != NULL) {
            out->kind = SLOOP_TAP_FM6P_BANK;
            out->a = s_bank_slot;
            out->d = delta;
        }
        return true;
    }
    if (rect_hit(x, y, FM6P_SAVE_X, FM6P_HEAD_Y, FM6P_SAVE_W, FM6P_CHIP_H)) {
        fm6p_save();
        if (out != NULL) {
            out->kind = SLOOP_TAP_FM6P_SAVE;
            out->a = s_bank_slot;
            out->v = locked ? 1u : 0u; /* v=1 = 锁定页拒绝（未下发 CMD_BANK_PUT） */
        }
        return true;
    }
    if (rect_hit(x, y, FM6P_ADJ_X, FM6P_ADJ_Y, FM6P_ADJ_W, FM6P_ADJ_H)) {
        int32_t rel = x - FM6P_ADJ_X;
        if (rel >= FM6P_ADJ_ZONE && rel < FM6P_ADJ_W - FM6P_ADJ_ZONE) return false; /* 死区 */
        uint8_t slot = focus_get();
        if (slot >= FM6P_SLOTS || !s_present[slot]) return false; /* 空槽：焦点不落空槽，防御 */
        uint8_t cop = s_slot_op[slot];
        uint8_t field = s_slot_field[slot];
        fm6p_write_t r;
        memset(&r, 0, sizeof(r));
        r.applied = cell_value(&st, slot);
        if (locked) {
            out_field(out, cop, field, &r, true);
            return true;
        }
        r = fm6p_step(rel < FM6P_ADJ_ZONE ? -1 : 1, true);
        out_field(out, cop, field, &r, false);
        return true;
    }
    for (uint8_t slot = 0; slot < FM6P_SLOTS; slot++) {
        if (!s_present[slot]) continue;
        int16_t cx = (int16_t)(FM6P_GRID_X0 + (slot % 4u) * FM6P_CELL_PITCH);
        int16_t cy = (int16_t)(FM6P_GRID_Y0 + (slot / 4u) * FM6P_ROW_PITCH);
        if (!rect_hit(x, y, cx + KNOB_OBJ_DX, cy + KNOB_OBJ_DY, KNOB_OBJ_SIZE, KNOB_OBJ_SIZE)) {
            continue;
        }
        focus_set(slot);
        refresh();
        out_select(out, op, page);
        return true;
    }
    return false;
}

/* ------------------------------------------------------- InputManager 路由 --- */
bool sloop_ui_fm6p_page_active(void)
{
    return sloop_ui_current_page() == APP_PAGE_FM6P;
}

void sloop_ui_fm6p_focus_move(int8_t delta)
{
    if (delta == 0) return;
    uint8_t page = page_get();
    uint8_t f = focus_get();
    for (uint8_t n = 0; n < FM6P_SLOTS; n++) {
        f = (uint8_t)((f + FM6P_SLOTS + delta) % FM6P_SLOTS);
        if (k_cells[page][f].field != FM6P_NONE) break;
    }
    focus_set(f);
    /* 输入任务上下文：不碰控件，描边由下一拍 update 落地 */
}

void sloop_ui_fm6p_adjust(int8_t delta)
{
    if (delta == 0) return;
    app_state_t st; /* 输入任务路径上唯一的一份快照：往下传给 fm6p_step_st / fm6p_write_st */
    state_get_copy(&st);
    if (!st.fm6p.engine_ok) return; /* 锁定页：不下发命令 */
    (void)fm6p_step_st(delta, false, &st); /* 输入任务上下文：不碰控件，下一拍 UI 刷新 */
}
