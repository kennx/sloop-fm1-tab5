/*
 * PRESETS 页（FILE 组，P4-B Task 12）：用户预置 32 槽（4×8 网格）+ LOAD/SAVE/ERASE
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 状态行 (24,20) 28pt（映射见 theme.h 的 ui_file_status_text）；页名由下行子签承担；
 * - 4×8 网格：格 144×72，x = 30 + c*152，y = 88 + r*80（右端 1238，底 400）——
 *   每格两行 "U01" + 引擎名/EMPTY（16pt，单对象可点击 label）；选中 = 4px
 *   outline（UI_ACCENT）+ UI_PANEL_SEL 底；
 * - 底部 LOAD/SAVE/ERASE（100×40，y=460，x=24/140/256）。
 *
 * 动作：点格设选中槽（局部 s_sel；键盘 ←/→ 同源），按钮作用于**选中槽**；SAVE 的
 * 目标轨 = 状态层的选中轨（鼓轨 ⇒ 状态层回 drum，状态行 `DRUM TRACK: NO SOUND`），
 * LOAD 载入选中轨（CMD_PRESET_LOAD 的 b = 选中轨）。所有变更经状态层命令；控件
 * 回调与 console `TAP` 注入共用 presets_action()。
 *
 * 播放中：LOAD **允许**（不写 flash，上游 MIDI PC 语义），SAVE/ERASE 被存储层拒绝
 * （状态行 `STOP BEFORE <ACT>`）——有意为之。
 *
 * 只读列表（sloop_store_preset_{used,engine}）直查存储层——P4-B 的文档化例外；
 * 但每个查询都要取存储锁并重扫银行索引（`up_boot`），故**不在每拍调用**：构建时/
 * 本页动作后/store_status 变化（其它写入者）时刷新，另有 2s 兜底（见 update()）。
 * 兜底刷新与其它刷新一样可能等在一次 flash 擦写之后（存储锁，≤150ms，spec §4.3 接受
 * 的 UI 读停顿；擦写期间不持引擎锁）；这是有意的取舍，不是卡死。
 *
 * 内存：32 个格子用单对象（可点击 label），不是 object+label 两层——组预算下
 * （FILE 与 SEQ 的最坏有序对）翻倍会超 ADR-005 §5 的不变量。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_store.h"

/* 布局宏：几何同时用于建控件与 TAP 命中（scripts/inject/inject.py 的 PRE_* 同名同值） */
#define PRE_CELL_X0      30
#define PRE_CELL_Y0      88
#define PRE_CELL_W       144
#define PRE_CELL_H       72
#define PRE_CELL_PITCH_X 152
#define PRE_CELL_PITCH_Y 80
#define PRE_BTN_X0       24
#define PRE_BTN_Y        460
#define PRE_BTN_W        100
#define PRE_BTN_H        40
#define PRE_BTN_PITCH    116 /* LOAD 24 / SAVE 140 / ERASE 256（100 宽 + 16 间距） */
#define PRE_COLS         8
#define PRE_LIST_TICKS   64 /* 兜底刷新周期（30ms × 64 ≈ 2s） */

/* 引擎名镜像（上游 params.c 的 N_ENGNAME；引擎名没有进 sloop_core 公共 API，
 * console 的 STORE_ENGINE_NAMES 是同一份镜像）——数量由静态断言锁死。 */
static const char *const k_engine_names[SLOOP_CORE_NENGINES] = {
    "ANALOG", "DIGITAL", "PHASE", "LOFI", "SAMPLE", "VOICE",
    "TRIO",   "WHEEL",   "GRAIN", "FM6",  "PHYS",   "NOISE",
};
_Static_assert(sizeof(k_engine_names) / sizeof(k_engine_names[0]) == SLOOP_CORE_NENGINES,
               "引擎名表与 SLOOP_CORE_NENGINES 一致");

static lv_obj_t *s_cells[SLOOP_STORE_PRESETS];
static lv_obj_t *s_status;

static lv_style_t s_st_cell;
static lv_style_t s_st_cell_sel;
static lv_style_t s_st_btn[3];

/* 列表缓存（used/engine）与每格的脏检查键 */
static uint8_t s_used[SLOOP_STORE_PRESETS];
static uint8_t s_eng[SLOOP_STORE_PRESETS];
static uint8_t s_shown_used[SLOOP_STORE_PRESETS];
static uint8_t s_shown_eng[SLOOP_STORE_PRESETS];
static bool s_cache_valid;
static bool s_list_dirty = true;
static uint32_t s_ticks;

static int8_t s_sel_shown = -1;
static uint32_t s_status_key = 0xFFFFFFFFu;
static char s_status_buf[32];  /* 状态行文本（LVGL 任务写、喂给 label） */
static char s_status_echo[32]; /* console TAP 回显专用（另一任务） */
static uint8_t s_sel;          /* 选中槽（UI 局部态） */
static uint8_t s_act = 0xFF;   /* 本页最后一次动作：0 save / 1 load / 2 erase / 0xFF 无 */
static uint8_t s_status_seen = 0xFF;

static void styles_init(void)
{
    /* 只初始化一次（组作用域下页面反复销毁/重建；重设 lv_style 会漏 props 数组） */
    static bool done;
    if (done) return;
    done = true;

    lv_style_init(&s_st_cell);
    lv_style_set_radius(&s_st_cell, 12);
    lv_style_set_bg_opa(&s_st_cell, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_cell, lv_color_hex(UI_PANEL));
    lv_style_set_border_width(&s_st_cell, 0);
    lv_style_set_text_font(&s_st_cell, &lv_font_montserrat_16);
    lv_style_set_text_align(&s_st_cell, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_cell, 17); /* 两行 16pt ≈ 38px：上下各 ~17 */
    lv_style_set_outline_color(&s_st_cell, lv_color_hex(UI_ACCENT));
    lv_style_set_outline_opa(&s_st_cell, LV_OPA_COVER);
    lv_style_set_outline_pad(&s_st_cell, 0);
    lv_style_set_outline_width(&s_st_cell, 0);

    lv_style_init(&s_st_cell_sel);
    lv_style_set_bg_color(&s_st_cell_sel, lv_color_hex(UI_PANEL_SEL));
    lv_style_set_outline_width(&s_st_cell_sel, 4);

    /* 按**动作语义**索引（0 save / 1 load / 2 erase，与 sloop_tap_result_t 的 v 一致），
     * 不是按版面顺序——版面是 LOAD/SAVE/ERASE（见 k_btn_act） */
    const uint32_t btn_bg[3] = {ui_track_color(1), UI_ACCENT, ui_track_color(3)};
    const uint32_t btn_fg[3] = {UI_TEXT, UI_BG, UI_TEXT};
    for (int i = 0; i < 3; i++) {
        lv_style_init(&s_st_btn[i]);
        lv_style_set_radius(&s_st_btn[i], 12);
        lv_style_set_bg_opa(&s_st_btn[i], LV_OPA_COVER);
        lv_style_set_bg_color(&s_st_btn[i], lv_color_hex(btn_bg[i]));
        lv_style_set_text_color(&s_st_btn[i], lv_color_hex(btn_fg[i]));
        lv_style_set_text_font(&s_st_btn[i], &lv_font_montserrat_16);
        lv_style_set_text_align(&s_st_btn[i], LV_TEXT_ALIGN_CENTER);
        lv_style_set_pad_top(&s_st_btn[i], 11);
    }
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_presets_update(&st);
}

/* 列表缓存刷新：每槽 1–2 次只读查询（取存储锁 + 重扫银行索引），只在需要时调用 */
static void preset_list_refresh(void)
{
    bool ready = sloop_store_ready();
    for (uint8_t k = 0; k < SLOOP_STORE_PRESETS; k++) {
        uint8_t used = (ready && sloop_store_preset_used(k)) ? 1u : 0u;
        s_used[k] = used;
        s_eng[k] = used ? sloop_store_preset_engine(k) : 0xFFu;
    }
    s_list_dirty = false;
}

/* 发命令（不碰控件：键盘路径在输入任务上下文） */
static void presets_issue(uint8_t slot, uint8_t act)
{
    if (slot >= SLOOP_STORE_PRESETS || act > 2) return;
    s_sel = slot;
    s_act = act;
    edit_cmd_t cmd = {.a = slot};
    if (act == 0) {
        cmd.type = CMD_PRESET_STORE; /* 目标轨 = 选中轨（状态层按上游 SAVE > USER 语义） */
    } else if (act == 1) {
        app_state_t st;
        state_get_copy(&st);
        cmd.type = CMD_PRESET_LOAD;
        cmd.b = st.core.selected_track < APP_STATE_TRACKS ? st.core.selected_track : 0u;
    } else {
        cmd.type = CMD_PRESET_ERASE;
    }
    state_apply(&cmd);
    s_list_dirty = true; /* 下一次 update() 重新读列表（写入/擦除已落地） */
}

/* 动作函数（LVGL 上下文、调用方已持锁）：TAP 与按钮回调的唯一共同路径。 */
static void presets_action(uint8_t slot, uint8_t act)
{
    presets_issue(slot, act);
    refresh();
}

static void presets_select(uint8_t slot)
{
    if (slot >= SLOOP_STORE_PRESETS) return;
    s_sel = slot;
    refresh();
}

static void cell_clicked_cb(lv_event_t *e)
{
    presets_select((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void btn_clicked_cb(lv_event_t *e)
{
    presets_action(s_sel, (uint8_t)(uintptr_t)lv_event_get_user_data(e)); /* user = 动作语义 */
}

/* 版面顺序（LOAD/SAVE/ERASE）→ 动作语义（0 save / 1 load / 2 erase）：
 * sloop_tap_result_t 的 v 与状态层的命令选择都用语义下标，版面顺序只是视觉排布
 * （脚本的 PRE_BTN_* 几何也按版面 x 排布）。 */
static const uint8_t k_btn_act[3] = {1, 0, 2};

static lv_obj_t *make_btn(lv_obj_t *parent, uint8_t layout, const char *text)
{
    const uint8_t act = k_btn_act[layout];
    lv_obj_t *btn = lv_label_create(parent);
    if (btn == NULL) return NULL;
    lv_obj_set_clickable(btn, true);
    lv_obj_set_pos(btn, PRE_BTN_X0 + (int32_t)layout * PRE_BTN_PITCH, PRE_BTN_Y);
    lv_obj_set_size(btn, PRE_BTN_W, PRE_BTN_H);
    lv_obj_add_style(btn, &s_st_btn[act], 0);
    lv_label_set_text(btn, text);
    lv_obj_add_event_cb(btn, btn_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)act);
    return btn;
}

lv_obj_t *page_presets_build(lv_obj_t *parent)
{
    styles_init();
    memset(s_used, 0, sizeof(s_used));
    memset(s_eng, 0, sizeof(s_eng));
    memset(s_shown_used, 0xFF, sizeof(s_shown_used)); /* 0xFF = 未显示（强制首帧写文本） */
    memset(s_shown_eng, 0xFF, sizeof(s_shown_eng));
    s_cache_valid = false;
    s_list_dirty = true;
    s_ticks = 0;
    s_sel_shown = -1;
    s_status_key = 0xFFFFFFFFu;
    s_sel = 0;
    s_act = 0xFF;
    s_status_seen = 0xFF;

    /* 页名由下行子签承担（PRESETS），页内只留状态行——每页少两个 label，
     * FILE 组与 SEQ 的最坏有序对（ADR-005 §5）才留得住余量。 */
    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_label_set_text(s_status, "SELECT A SLOT");
    lv_obj_set_size(s_status, 1200, 40);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(s_status, 24, 20);

    for (uint8_t k = 0; k < SLOOP_STORE_PRESETS; k++) {
        lv_obj_t *cell = lv_label_create(parent);
        UI_BUILD_CHECK(cell);
        lv_obj_set_clickable(cell, true);
        lv_obj_set_pos(cell, PRE_CELL_X0 + (int32_t)(k % PRE_COLS) * PRE_CELL_PITCH_X,
                       PRE_CELL_Y0 + (int32_t)(k / PRE_COLS) * PRE_CELL_PITCH_Y);
        lv_obj_set_size(cell, PRE_CELL_W, PRE_CELL_H);
        lv_obj_add_style(cell, &s_st_cell, 0);
        char buf[24];
        snprintf(buf, sizeof(buf), "U%02u\nEMPTY", (unsigned)(k + 1u));
        lv_label_set_text(cell, buf);
        lv_obj_set_style_text_color(cell, lv_color_hex(UI_TEXT_DIM), 0);
        lv_obj_add_event_cb(cell, cell_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)k);
        s_cells[k] = cell;
    }

    static const char *const k_btn_text[3] = {"LOAD", "SAVE", "ERASE"}; /* 版面顺序 */
    for (uint8_t layout = 0; layout < 3; layout++) {
        if (make_btn(parent, layout, k_btn_text[layout]) == NULL) return NULL;
    }
    return parent;
}

void page_presets_update(const app_state_t *st)
{
    /* 其它写入者（console `STORE PRESET …`）改 store_status：列表要跟着变。
     * 列表查询要取存储锁，故不在每拍调用——状态变化/本页动作/2s 兜底才刷新。 */
    if (st->store_status != s_status_seen) {
        s_status_seen = st->store_status;
        s_list_dirty = true;
    }
    s_ticks++;
    if (s_list_dirty || s_ticks >= PRE_LIST_TICKS) {
        s_ticks = 0;
        preset_list_refresh();
    }

    uint8_t sel = s_sel < SLOOP_STORE_PRESETS ? s_sel : 0;
    if ((int8_t)sel != s_sel_shown) {
        for (uint8_t k = 0; k < SLOOP_STORE_PRESETS; k++) {
            if (k == sel) lv_obj_add_style(s_cells[k], &s_st_cell_sel, 0);
            else lv_obj_remove_style(s_cells[k], &s_st_cell_sel, 0);
        }
        s_sel_shown = (int8_t)sel;
    }

    for (uint8_t k = 0; k < SLOOP_STORE_PRESETS; k++) {
        if (s_cache_valid && s_shown_used[k] == s_used[k] && s_shown_eng[k] == s_eng[k]) continue;
        char buf[24];
        if (s_used[k]) {
            const char *name = s_eng[k] < SLOOP_CORE_NENGINES ? k_engine_names[s_eng[k]] : "?";
            snprintf(buf, sizeof(buf), "U%02u\n%s", (unsigned)(k + 1u), name);
            lv_obj_set_style_text_color(s_cells[k], lv_color_hex(UI_TEXT), 0);
        } else {
            snprintf(buf, sizeof(buf), "U%02u\nEMPTY", (unsigned)(k + 1u));
            lv_obj_set_style_text_color(s_cells[k], lv_color_hex(UI_TEXT_DIM), 0);
        }
        lv_label_set_text(s_cells[k], buf);
        s_shown_used[k] = s_used[k];
        s_shown_eng[k] = s_eng[k];
    }
    s_cache_valid = true;

    bool ready = sloop_store_ready();
    uint32_t key = (uint32_t)s_act | ((uint32_t)st->store_status << 8) |
                   ((uint32_t)(ready ? 1u : 0u) << 16);
    if (key != s_status_key) {
        s_status_key = key;
        ui_file_status_text(s_status_buf, sizeof(s_status_buf), s_act, st->store_status, ready);
        lv_label_set_text(s_status, s_status_buf);
        lv_obj_set_style_text_color(
            s_status, lv_color_hex(ui_file_status_color(s_act, st->store_status, ready)), 0);
    }
}

bool page_presets_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (uint8_t k = 0; k < SLOOP_STORE_PRESETS; k++) {
        const int32_t cx = PRE_CELL_X0 + (int32_t)(k % PRE_COLS) * PRE_CELL_PITCH_X;
        const int32_t cy = PRE_CELL_Y0 + (int32_t)(k / PRE_COLS) * PRE_CELL_PITCH_Y;
        if (x < cx || x >= cx + PRE_CELL_W || y < cy || y >= cy + PRE_CELL_H) continue;
        presets_select(k);
        if (out != NULL) {
            out->kind = SLOOP_TAP_FILE_SLOT;
            out->a = APP_PAGE_PRESETS;
            out->b = k;
        }
        return true;
    }
    for (uint8_t layout = 0; layout < 3; layout++) {
        const int32_t bx = PRE_BTN_X0 + (int32_t)layout * PRE_BTN_PITCH;
        if (x < bx || x >= bx + PRE_BTN_W || y < PRE_BTN_Y || y >= PRE_BTN_Y + PRE_BTN_H) continue;
        const uint8_t act = k_btn_act[layout];
        presets_action(s_sel, act);
        if (out != NULL) {
            out->kind = SLOOP_TAP_FILE_ACTION;
            out->a = APP_PAGE_PRESETS;
            out->b = s_sel;
            out->v = act;
        }
        return true;
    }
    return false;
}

void page_presets_slot_move(int8_t delta)
{
    int32_t v = (int32_t)s_sel + delta;
    while (v < 0) v += (int32_t)SLOOP_STORE_PRESETS;
    while (v >= (int32_t)SLOOP_STORE_PRESETS) v -= (int32_t)SLOOP_STORE_PRESETS;
    s_sel = (uint8_t)v; /* 输入任务上下文：不碰控件，描边由下一拍 update() 落地 */
}

void page_presets_load(void)
{
    presets_issue(s_sel, 1);
}

const char *page_presets_status_text(void)
{
    app_state_t st;
    state_get_copy(&st);
    return ui_file_status_text(s_status_echo, sizeof(s_status_echo), s_act, st.store_status,
                               sloop_store_ready());
}
