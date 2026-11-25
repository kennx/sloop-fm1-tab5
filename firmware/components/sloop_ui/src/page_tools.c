/*
 * TOOLS 页（MIX 组，P4-F Task 6）：上游 TOOLS 页的三个 GO 按钮——CLEAR SEQ / INIT SOUND /
 * NEW（ui_input.c:568-612；清单 012 §4）。
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - 状态行 (24,20) 28pt：`READY` / `AGAIN TO CONFIRM` / 上游的三句结果
 *   （`PATTERN CLEARED` / `SOUND INIT` / `NEW LOOP, A-D KEPT`）与拒绝
 *   （`STOP THE SONG FIRST` / `DRUM TRACK: NO SOUND` / `FROZEN`），映射见 theme.h 的
 *   ui_tools_status_text（屏幕与 console `TAP` 回显同源）；
 * - 三张大 chip y=120、360×140、x = 24 + i*384：CLEAR SEQ / INIT SOUND / NEW；chip 下方
 *   (y=280) 一行 20pt 说明（前两项作用于**选中轨**，说明随选轨更新）；
 * - 底部提示 (24,596) 16pt。
 *
 * 两段式确认（上游 ui.arm / ui.arm_t = 90 ≈ 1.5s）：第一次点 chip = **预备**——chip 换
 * 强调底色、状态行 `AGAIN TO CONFIRM`；1.5s 内再点同一 chip 才执行（一条 CMD_TOOLS），
 * 超时自动取消；预备期间点另一张 chip = 改为预备那一张。动作语义、走带守卫与返回码
 * 都在状态层/sloop_core_tools，本页只做确认态与文案。
 *
 * 动作的唯一实现是 tools_press()：控件回调与 console `TAP` 注入共用（都持 LVGL 锁），
 * 预备态/超时是页内 UI 局部态（跨组页面重建时复位——离开 MIX 组页面即销毁）。
 * 时钟用 esp_timer 毫秒（上游 ui.arm_t 由 fm1_ms 每帧递减；本页在 30ms UI 拍里比较）。
 *
 * 内存：状态行 + 3 chip + 3 说明 + 提示共 8 个单对象 label，样式为静态共享 lv_style_t
 * （styles_init 只初始化一次——组作用域下页面会被销毁重建，重跑 lv_style_init 会漏掉
 * props 数组）。
 */
#include "page_tools.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "theme.h"

/* 布局宏：几何同时用于建控件与 TAP 命中（scripts/inject/inject.py 的 TOOLS_* 同名同值） */
#define TOOLS_CHIP_X0    24
#define TOOLS_CHIP_Y     120
#define TOOLS_CHIP_W     360
#define TOOLS_CHIP_H     140
#define TOOLS_CHIP_PITCH 384
#define TOOLS_DESC_Y     280
#define TOOLS_HINT_Y     596
#define TOOLS_ARM_MS     1500u /* 上游 ui.arm_t = 90 帧 ≈ 1.5s */

#define TOOLS_NO_ACT 0xFFu

enum { TOOLS_CLR = 0, TOOLS_INIT = 1, TOOLS_NEW = 2, TOOLS_COUNT = 3 };

/* chip → sloop_tool_t：顺序即 CMD_TOOLS 的 action 编号（控制台回显按它取名） */
_Static_assert((int)SLOOP_TOOL_CLRSEQ == TOOLS_CLR && (int)SLOOP_TOOL_INITSOUND == TOOLS_INIT &&
                   (int)SLOOP_TOOL_NEWPRJ == TOOLS_NEW,
               "chip 下标必须等于 sloop_tool_t 的动作编号");

static const char *const k_chip_text[TOOLS_COUNT] = {"CLEAR SEQ", "INIT SOUND", "NEW"};

static lv_obj_t *s_status;
static lv_obj_t *s_chip[TOOLS_COUNT];
static lv_obj_t *s_desc[TOOLS_COUNT];

static lv_style_t s_st_chip;
static lv_style_t s_st_chip_armed;
static lv_style_t s_st_desc;
static lv_style_t s_st_hint;

/* 页内 UI 局部态：预备中的动作（TOOLS_NO_ACT = 无）与其时刻、最近一次执行的动作 */
static uint8_t s_arm = TOOLS_NO_ACT;
static uint32_t s_arm_ms;
static uint8_t s_act = TOOLS_NO_ACT;

/* 脏检查缓存 */
static int8_t s_arm_shown = -1;
static int8_t s_track_shown = -1;
static uint32_t s_status_key = 0xFFFFFFFFu;
static char s_status_buf[48];  /* 状态行文本（LVGL 任务写、喂给 label） */
static char s_status_echo[48]; /* console TAP 回显专用（另一任务，避免与定时器交错写） */

static void styles_init(void)
{
    /* 只初始化一次：组作用域下页面会反复销毁/重建，lv_style_init 会 memzero 掉
     * 已分配的 props 数组（LVGL 在 ASSERT_STYLE 下也会告警「Potential memory leak」） */
    static bool done;
    if (done) {
        lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
        lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip_armed, lv_color_hex(UI_ACCENT));
        lv_style_set_text_color(&s_st_chip_armed, lv_color_hex(UI_BG));
        lv_style_set_text_color(&s_st_desc, lv_color_hex(UI_TEXT_DIM));
        lv_style_set_text_color(&s_st_hint, lv_color_hex(UI_TEXT_DIM));
        return;
    }
    done = true;

    /* 大 chip：28pt 居中（140 高、行高 ~36：上下各 52） */
    lv_style_init(&s_st_chip);
    lv_style_set_radius(&s_st_chip, 16);
    lv_style_set_bg_opa(&s_st_chip, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_chip, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_chip, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_chip, 52);

    /* 预备态：强调底色（叠加在 s_st_chip 之上，按状态 add/remove） */
    lv_style_init(&s_st_chip_armed);
    lv_style_set_bg_color(&s_st_chip_armed, lv_color_hex(UI_ACCENT));
    lv_style_set_text_color(&s_st_chip_armed, lv_color_hex(UI_BG));

    lv_style_init(&s_st_desc);
    lv_style_set_text_font(&s_st_desc, &lv_font_montserrat_20);
    lv_style_set_text_color(&s_st_desc, lv_color_hex(UI_TEXT_DIM));

    lv_style_init(&s_st_hint);
    lv_style_set_text_font(&s_st_hint, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_hint, lv_color_hex(UI_TEXT_DIM));
}

static uint32_t tools_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_tools_update(&st);
}

/* 动作的唯一实现（LVGL 上下文、调用方已持锁；控件回调与 TAP 的共同路径）：
 * 返回 true = 本次点按进入/保持预备态（未执行）；false = 已执行（一条 CMD_TOOLS）。 */
static bool tools_press(uint8_t act)
{
    const uint32_t now = tools_now_ms();
    /* 窗口在这里也判（不只是 update 拍里清预备）：两击之间若没有 UI 拍——quiesce 期间、或
     * 两次注入同一拍——过了 1.5s 的旧预备不得执行（终审 B）。 */
    if (s_arm != act || now - s_arm_ms >= TOOLS_ARM_MS) {
        s_arm = act;
        s_arm_ms = now;
        return true;
    }
    s_arm = TOOLS_NO_ACT;
    s_act = act;
    const edit_cmd_t cmd = {.type = CMD_TOOLS, .a = act};
    state_apply(&cmd);
    return false;
}

static void chip_clicked_cb(lv_event_t *e)
{
    (void)tools_press((uint8_t)(uintptr_t)lv_event_get_user_data(e));
    refresh();
}

bool page_tools_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (uint8_t i = 0; i < TOOLS_COUNT; i++) {
        const int32_t cx = TOOLS_CHIP_X0 + (int32_t)i * TOOLS_CHIP_PITCH;
        if (x < cx || x >= cx + TOOLS_CHIP_W || y < TOOLS_CHIP_Y ||
            y >= TOOLS_CHIP_Y + TOOLS_CHIP_H)
            continue;
        const bool armed = tools_press(i);
        refresh();
        if (out != NULL) {
            out->kind = SLOOP_TAP_TOOLS;
            out->a = i;
            out->v = armed ? 1 : 0; /* 1 = 进入/保持预备态（未执行） */
        }
        return true;
    }
    return false;
}

lv_obj_t *page_tools_build(lv_obj_t *parent)
{
    styles_init();
    /* 懒建/组作用域重建：控件是全新的，复位脏检查缓存、预备态（离开本页即取消）与
     * 「最近一次执行」（与 FILE 页同一取舍：重新进页不重放上一次的结果，状态行回 READY）。 */
    s_arm = TOOLS_NO_ACT;
    s_act = TOOLS_NO_ACT;
    memset(s_chip, 0, sizeof(s_chip));
    memset(s_desc, 0, sizeof(s_desc));
    s_arm_shown = -1;
    s_track_shown = -1;
    s_status_key = 0xFFFFFFFFu;

    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_label_set_text(s_status, "READY");
    lv_obj_set_size(s_status, 1200, 40);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(s_status, 24, 20);

    for (uint8_t i = 0; i < TOOLS_COUNT; i++) {
        lv_obj_t *chip = lv_label_create(parent);
        UI_BUILD_CHECK(chip);
        lv_obj_set_clickable(chip, true);
        lv_obj_set_pos(chip, TOOLS_CHIP_X0 + (int32_t)i * TOOLS_CHIP_PITCH, TOOLS_CHIP_Y);
        lv_obj_set_size(chip, TOOLS_CHIP_W, TOOLS_CHIP_H);
        lv_obj_add_style(chip, &s_st_chip, 0);
        lv_label_set_text(chip, k_chip_text[i]);
        lv_obj_add_event_cb(chip, chip_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_chip[i] = chip;

        s_desc[i] = lv_label_create(parent);
        UI_BUILD_CHECK(s_desc[i]);
        lv_obj_add_style(s_desc[i], &s_st_desc, 0);
        lv_obj_set_pos(s_desc[i], TOOLS_CHIP_X0 + (int32_t)i * TOOLS_CHIP_PITCH + 8,
                       TOOLS_DESC_Y);
        lv_obj_set_width(s_desc[i], TOOLS_CHIP_W - 16);
    }

    lv_obj_t *hint = lv_label_create(parent);
    UI_BUILD_CHECK(hint);
    lv_label_set_text(hint,
                      "TAP = ARM, TAP AGAIN WITHIN 1.5 S = RUN   |   SECTIONS A-D AND SETTINGS "
                      "ARE KEPT");
    lv_obj_add_style(hint, &s_st_hint, 0);
    lv_obj_set_pos(hint, 24, TOOLS_HINT_Y);
    return parent;
}

void page_tools_update(const app_state_t *st)
{
    /* 预备超时：1.5s 内没有第二次点按 ⇒ 取消（芯片回普通底色、状态行回上一次结果） */
    if (s_arm != TOOLS_NO_ACT && tools_now_ms() - s_arm_ms > TOOLS_ARM_MS)
        s_arm = TOOLS_NO_ACT;

    if ((int8_t)s_arm != s_arm_shown) {
        s_arm_shown = (int8_t)s_arm;
        for (uint8_t i = 0; i < TOOLS_COUNT; i++) {
            if (s_chip[i] == NULL) continue;
            if (i == s_arm) lv_obj_add_style(s_chip[i], &s_st_chip_armed, 0);
            else lv_obj_remove_style(s_chip[i], &s_st_chip_armed, 0);
        }
    }

    /* 说明行：CLEAR/INIT 作用于选中轨（NEW 是全部轨） */
    const uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0u;
    if ((int8_t)track != s_track_shown) {
        s_track_shown = (int8_t)track;
        char buf[72];
        snprintf(buf, sizeof(buf), "TRACK %u  ·  UNDO IN TRANSPORT", (unsigned)track + 1u);
        if (s_desc[TOOLS_CLR] != NULL) lv_label_set_text(s_desc[TOOLS_CLR], buf);
        snprintf(buf, sizeof(buf), "TRACK %u  ·  DEFAULTS, PRESET 1", (unsigned)track + 1u);
        if (s_desc[TOOLS_INIT] != NULL) lv_label_set_text(s_desc[TOOLS_INIT], buf);
        if (s_desc[TOOLS_NEW] != NULL)
            lv_label_set_text(s_desc[TOOLS_NEW], "EVERY TRACK  ·  LOOP MODE");
    }

    /* 状态行：预备 > 最近一次执行的结果；四要素不变就不动文本。 */
    const bool drum = track == APP_STATE_TRACKS - 1u;
    uint32_t key = (uint32_t)s_arm | ((uint32_t)s_act << 8) | ((uint32_t)st->tools_status << 16) |
                   ((uint32_t)drum << 24);
    if (key != s_status_key) {
        s_status_key = key;
        const uint8_t shown = s_arm != TOOLS_NO_ACT ? s_arm : s_act;
        ui_tools_status_text(s_status_buf, sizeof(s_status_buf), shown, s_arm != TOOLS_NO_ACT,
                             st->tools_status, drum);
        lv_label_set_text(s_status, s_status_buf);
        lv_obj_set_style_text_color(
            s_status, lv_color_hex(ui_tools_status_color(shown, s_arm != TOOLS_NO_ACT,
                                                         st->tools_status)),
            0);
    }
}

const char *page_tools_status_text(void)
{
    app_state_t st;
    state_get_copy(&st);
    const uint8_t track = st.core.selected_track < APP_STATE_TRACKS ? st.core.selected_track : 0u;
    const uint8_t shown = s_arm != TOOLS_NO_ACT ? s_arm : s_act;
    return ui_tools_status_text(s_status_echo, sizeof(s_status_echo), shown, s_arm != TOOLS_NO_ACT,
                                st.tools_status, track == APP_STATE_TRACKS - 1u);
}
