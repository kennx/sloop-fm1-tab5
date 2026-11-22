/*
 * PROJECTS 页（FILE 组，P4-B Task 12）：工程槽 A–D 的状态 + SAVE/LOAD/ERASE
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 状态行 (24,20) 28pt（`SAVE OK` / `EMPTY` / `STOP BEFORE SAVE` / `SAVE ERROR` /
 *   `NO STORAGE`，映射见 theme.h 的 ui_file_status_text）；页名由下行子签承担；
 * - 4 张卡 x = 24 + k*312，y = 100，296×400：字母 A..D（40pt）(20,16)、
 *   状态/哈希两行 (20,84) 20pt、三个按钮 SAVE/LOAD/ERASE（100×40，x=98，
 *   y=170/234/298，间距 24）。选中 = 6px outline（UI_ACCENT）+ UI_PANEL_SEL 底；
 *   outline 画在盒外，选中/取消不会挪动卡内子对象（与 TRACKS 同一取舍）。
 *
 * 动作：点卡设选中槽（局部 s_slot，初值 = 无选中；键盘 ←/→ 由 InputManager 经
 * sloop_ui_file_slot_move 改同一变量，从「无选中」按 → 落到 A、按 ← 落到 D），按钮发
 * CMD_PROJECT_{SAVE,LOAD,ERASE}（a=该卡槽号）；结果由状态层写
 * app_state_t.store_status，本页把它与「本页最后一次动作」配对映射成状态行。控件回调
 * 与 console `TAP` 注入共用 projects_action()——命中矩形由建控件同一组布局宏派生
 * （没有第二份坐标表）。
 *
 * 播放中：SAVE **和 LOAD** 都被存储层拒绝（`store_playing()`；project LOAD 会整份替换
 * 引擎状态并停走带，上游同样要求先停），状态行 `STOP BEFORE <ACT>`；ERASE 亦然。
 * ——这是有意的，不是漏判。注意 PRESETS 的 LOAD 是例外（只读、不写 flash，且
 * `up_load_to` 自己 panic 该轨），见 page_presets.c 的说明。
 *
 * 只读列表（sloop_store_project_{used,hash}）直查存储层——P4-B 的文档化例外：
 * 列表查询不是编辑命令（只读 RAM 槽、无 flash 访问）；**所有变更**一律经状态层。
 * 这些读取在 30ms UI 定时器里**不取存储锁**（有意）：`proj_slot[]` 由 SAVE 在存储锁内
 * 捕获，最坏读到一拍内撕裂的 hash（显示用，仅 hash 前 4 位），不会崩、不会写。
 * 脏检查：每卡缓存 (used,hash)、选中态与状态行三元组 (act,status,ready)，
 * 只在变化时改文本/样式（30ms 一拍无条件重设会让 LVGL 持续失效重绘）。
 *
 * 内存：样式用静态共享 lv_style_t（styles_init 只初始化一次——组作用域下页面会被
 * 销毁重建，每次重设样式会漏掉 lv_style 的 props 数组）；按钮用「可点击的 label」
 * 单对象（lv_button + 子 label 会翻倍对象数，PRESETS 32 格时池预算装不下）。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_store.h"

/* 布局宏：几何同时用于建控件与 TAP 命中（scripts/inject/inject.py 的 PROJ_* 同名同值） */
#define PROJ_CARD_X0    24
#define PROJ_CARD_Y     100
#define PROJ_CARD_W     296
#define PROJ_CARD_H     400
#define PROJ_CARD_PITCH (PROJ_CARD_W + 16)
#define PROJ_BTN_X      98 /* 卡内（296 宽 -> 98..198，居中偏右留字母区） */
#define PROJ_BTN_Y0     170
#define PROJ_BTN_W      100
#define PROJ_BTN_H      40
#define PROJ_BTN_PITCH  64 /* 170 / 234 / 298，间距 24 */

typedef struct {
    uint8_t used;
    uint32_t hash;
} proj_slot_view_t;

static lv_obj_t *s_cards[SLOOP_STORE_PROJECTS];
static lv_obj_t *s_detail[SLOOP_STORE_PROJECTS];
static lv_obj_t *s_status;

static lv_style_t s_st_card;
static lv_style_t s_st_card_sel;
static lv_style_t s_st_btn[3];
static lv_style_t s_st_letter;
static lv_style_t s_st_detail;

static proj_slot_view_t s_cache[SLOOP_STORE_PROJECTS];
static bool s_cache_valid;
static int8_t s_sel_shown = -1;
static uint32_t s_status_key = 0xFFFFFFFFu;
static char s_status_buf[32];  /* 状态行文本（LVGL 任务写、喂给 label） */
static char s_status_echo[32]; /* console TAP 回显专用（另一任务，避免与定时器交错写） */
/* 选中槽（UI 局部态；键盘 ←/→ 与点卡都改它）。初值 = 无选中（PROJ_NO_SLOT）：
 * 卡 A 不再在「SELECT A SLOT」提示旁边被描边成已选——选中态只由用户动作产生。 */
#define PROJ_NO_SLOT 0xFFu
static uint8_t s_slot = PROJ_NO_SLOT;
static uint8_t s_act = 0xFF; /* 本页最后一次动作：0 save / 1 load / 2 erase / 0xFF 无 */

static void styles_init(void)
{
    /* 只初始化一次：组作用域下页面会反复销毁/重建，lv_style_init 会 memzero 掉
     * 已分配的 props 数组（LVGL 在 ASSERT_STYLE 下也会告警「Potential memory leak」） */
    static bool done;
    if (done) return;
    done = true;

    lv_style_init(&s_st_card);
    lv_style_set_radius(&s_st_card, 20);
    lv_style_set_bg_opa(&s_st_card, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_card, lv_color_hex(UI_PANEL));
    lv_style_set_border_width(&s_st_card, 0);
    lv_style_set_outline_color(&s_st_card, lv_color_hex(UI_PANEL_HI));
    lv_style_set_outline_opa(&s_st_card, LV_OPA_COVER);
    lv_style_set_outline_pad(&s_st_card, 0);
    lv_style_set_outline_width(&s_st_card, 3);

    lv_style_init(&s_st_card_sel);
    lv_style_set_bg_color(&s_st_card_sel, lv_color_hex(UI_PANEL_SEL));
    lv_style_set_outline_color(&s_st_card_sel, lv_color_hex(UI_ACCENT));
    lv_style_set_outline_width(&s_st_card_sel, 6);

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
        lv_style_set_pad_top(&s_st_btn[i], 11); /* 16pt 行高 ~18px：上下各 ~11 居中 */
    }

    lv_style_init(&s_st_letter);
    lv_style_set_text_font(&s_st_letter, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_letter, lv_color_hex(UI_TEXT));

    lv_style_init(&s_st_detail);
    lv_style_set_text_font(&s_st_detail, &lv_font_montserrat_20);
    lv_style_set_text_color(&s_st_detail, lv_color_hex(UI_TEXT_DIM));
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_projects_update(&st);
}

/* 发命令（不碰控件：键盘路径在输入任务上下文，控件由 30ms 定时器的 update() 刷新） */
static void projects_issue(uint8_t slot, uint8_t act)
{
    if (slot >= SLOOP_STORE_PROJECTS || act > 2) return;
    s_slot = slot;
    s_act = act;
    edit_cmd_type_t type = act == 0   ? CMD_PROJECT_SAVE
                           : act == 1 ? CMD_PROJECT_LOAD
                                      : CMD_PROJECT_ERASE;
    const edit_cmd_t cmd = {.type = type, .a = slot};
    state_apply(&cmd);
}

/* 动作函数（LVGL 上下文、调用方已持锁）：TAP 注入与按钮/卡片回调的唯一共同路径。 */
static void projects_action(uint8_t slot, uint8_t act)
{
    projects_issue(slot, act);
    refresh(); /* TAP 立即刷新（回显/截屏时序确定），键盘路径等下一拍 */
}

static void projects_select(uint8_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return;
    s_slot = slot;
    refresh();
}

static void card_clicked_cb(lv_event_t *e)
{
    projects_select((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void btn_clicked_cb(lv_event_t *e)
{
    uintptr_t id = (uintptr_t)lv_event_get_user_data(e);
    projects_action((uint8_t)(id / 3u), (uint8_t)(id % 3u));
}

/* 单对象按钮（可点击 label）：100×40、居中文本、底色/文本色由共享样式给。 */
static lv_obj_t *make_btn(lv_obj_t *parent, uint8_t slot, uint8_t act, const char *text)
{
    lv_obj_t *btn = lv_label_create(parent);
    if (btn == NULL) return NULL;
    lv_obj_set_clickable(btn, true);
    lv_obj_set_pos(btn, PROJ_BTN_X, PROJ_BTN_Y0 + (int32_t)act * PROJ_BTN_PITCH);
    lv_obj_set_size(btn, PROJ_BTN_W, PROJ_BTN_H);
    lv_obj_add_style(btn, &s_st_btn[act], 0);
    lv_label_set_text(btn, text);
    lv_obj_add_event_cb(btn, btn_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(slot * 3u + act));
    return btn;
}

lv_obj_t *page_projects_build(lv_obj_t *parent)
{
    styles_init();
    /* 懒建/组作用域重建：控件是全新的，复位脏检查缓存与局部选中态 */
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_valid = false;
    s_sel_shown = -1;
    s_status_key = 0xFFFFFFFFu;
    s_slot = PROJ_NO_SLOT;
    s_act = 0xFF;

    /* 页名由下行子签承担（PROJECTS），页内只留状态行——每页少两个 label，
     * FILE 组与 SEQ 的最坏有序对（ADR-005 §5）才留得住余量。 */
    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_label_set_text(s_status, "SELECT A SLOT");
    lv_obj_set_size(s_status, 1200, 40);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(s_status, 24, 20);

    static const char *const k_letters[SLOOP_STORE_PROJECTS] = {"A", "B", "C", "D"};
    static const char *const k_btn_text[3] = {"SAVE", "LOAD", "ERASE"};
    for (uint8_t k = 0; k < SLOOP_STORE_PROJECTS; k++) {
        lv_obj_t *card = lv_obj_create(parent);
        UI_BUILD_CHECK(card);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_pos(card, PROJ_CARD_X0 + (int32_t)k * PROJ_CARD_PITCH, PROJ_CARD_Y);
        lv_obj_set_size(card, PROJ_CARD_W, PROJ_CARD_H);
        lv_obj_add_style(card, &s_st_card, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)k);
        s_cards[k] = card;

        lv_obj_t *letter = lv_label_create(card);
        UI_BUILD_CHECK(letter);
        lv_label_set_text(letter, k_letters[k]);
        lv_obj_add_style(letter, &s_st_letter, 0);
        lv_obj_set_pos(letter, 20, 16);

        s_detail[k] = lv_label_create(card);
        UI_BUILD_CHECK(s_detail[k]);
        lv_label_set_text(s_detail[k], "EMPTY");
        lv_obj_add_style(s_detail[k], &s_st_detail, 0);
        lv_obj_set_pos(s_detail[k], 20, 84);

        for (uint8_t a = 0; a < 3; a++) {
            if (make_btn(card, k, a, k_btn_text[a]) == NULL) return NULL;
        }
    }
    return parent;
}

void page_projects_update(const app_state_t *st)
{
    bool has_sel = s_slot < SLOOP_STORE_PROJECTS;
    int8_t sel = has_sel ? (int8_t)s_slot : -1;
    if (sel != s_sel_shown) {
        for (uint8_t k = 0; k < SLOOP_STORE_PROJECTS; k++) {
            if (has_sel && k == (uint8_t)sel) lv_obj_add_style(s_cards[k], &s_st_card_sel, 0);
            else lv_obj_remove_style(s_cards[k], &s_st_card_sel, 0);
        }
        s_sel_shown = sel;
    }

    for (uint8_t k = 0; k < SLOOP_STORE_PROJECTS; k++) {
        proj_slot_view_t cv = {.used = sloop_store_project_used(k) ? 1u : 0u,
                               .hash = 0};
        if (cv.used) cv.hash = sloop_store_project_hash(k);
        if (s_cache_valid && memcmp(&cv, &s_cache[k], sizeof(cv)) == 0) continue;
        if (cv.used) {
            char buf[24];
            snprintf(buf, sizeof(buf), "USED\n%04X", (unsigned)((cv.hash >> 16) & 0xFFFFu));
            lv_label_set_text(s_detail[k], buf);
            lv_obj_set_style_text_color(s_detail[k], lv_color_hex(UI_TEXT), 0);
        } else {
            lv_label_set_text(s_detail[k], "EMPTY");
            lv_obj_set_style_text_color(s_detail[k], lv_color_hex(UI_TEXT_DIM), 0);
        }
        s_cache[k] = cv;
    }
    s_cache_valid = true;

    /* 状态行：store_status 是「最近一次存储命令」的全局结果，与本页最后一次动作配对
     * 显示；三要素（act/status/ready）不变就不动文本。 */
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

bool page_projects_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (uint8_t k = 0; k < SLOOP_STORE_PROJECTS; k++) {
        const int32_t cx = PROJ_CARD_X0 + (int32_t)k * PROJ_CARD_PITCH;
        for (uint8_t a = 0; a < 3; a++) {
            const int32_t bx = cx + PROJ_BTN_X;
            const int32_t by = PROJ_CARD_Y + PROJ_BTN_Y0 + (int32_t)a * PROJ_BTN_PITCH;
            if (x < bx || x >= bx + PROJ_BTN_W || y < by || y >= by + PROJ_BTN_H) continue;
            projects_action(k, a);
            if (out != NULL) {
                out->kind = SLOOP_TAP_FILE_ACTION;
                out->a = APP_PAGE_PROJECTS;
                out->b = k;
                out->v = a;
            }
            return true;
        }
        if (x < cx || x >= cx + PROJ_CARD_W || y < PROJ_CARD_Y || y >= PROJ_CARD_Y + PROJ_CARD_H) {
            continue;
        }
        projects_select(k);
        if (out != NULL) {
            out->kind = SLOOP_TAP_FILE_SLOT;
            out->a = APP_PAGE_PROJECTS;
            out->b = k;
        }
        return true;
    }
    return false;
}

void page_projects_slot_move(int8_t delta)
{
    int32_t v;
    if (s_slot >= SLOOP_STORE_PROJECTS) {
        /* 无选中：→ 落到 A、← 落到 D（之后正常环绕） */
        v = delta > 0 ? 0 : (int32_t)SLOOP_STORE_PROJECTS - 1;
    } else {
        v = (int32_t)s_slot + delta;
        while (v < 0) v += (int32_t)SLOOP_STORE_PROJECTS;
        while (v >= (int32_t)SLOOP_STORE_PROJECTS) v -= (int32_t)SLOOP_STORE_PROJECTS;
    }
    s_slot = (uint8_t)v; /* 输入任务上下文：不碰控件，描边由下一拍 update() 落地 */
}

void page_projects_load(void)
{
    if (s_slot >= SLOOP_STORE_PROJECTS) return; /* 无选中槽：ENTER 不动作（不默认选 A） */
    projects_issue(s_slot, 1);
}

const char *page_projects_status_text(void)
{
    app_state_t st;
    state_get_copy(&st);
    return ui_file_status_text(s_status_echo, sizeof(s_status_echo), s_act, st.store_status,
                               sloop_store_ready());
}
