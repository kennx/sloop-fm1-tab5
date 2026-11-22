/*
 * SONG 编排页（P4-C Task 11，独立顶层组 SONG；等价上游 ui_song.c）
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H=72）：
 * - 状态行 (24,20) 宽 1232、20pt **两行**：第一行 `SONG <SONG|LOOP> <STOP|PLAY> BAR n
 *   LIVE <A-D|-> READY <ABCD 位掩码>`，第二行 `REC <OFF|ARMED|RECORDING> CHAIN <n/i|->
 *   LAST <none|NO SONG|N> | <本页最后一次动作的结果>`（状态行文本是 TAP 回显的同一
 *   缓冲，见 page_song_status_text）。失败/空 scene 用红字（UI_ERROR），空闲灰。
 *   存储未就绪时第一行是 `SONG  NO STORAGE`（动作回显若也是它则不重复），所有动作
 *   只回状态、不发命令；走带中（live loop/SONG）的 MODE 与 16 项配置编辑由页面守卫
 *   回 `STOP FIRST`（上游 SONG 屏语义），section/chain 的播放语义不受影响。
 * - A–D section 卡：x = 24 + k*304，y = 96，288×104；单对象可点击 label（两/三行
 *   `A\nUSED[\nLIVE]`），选中 = 6px UI_ACCENT outline + UI_PANEL_SEL 底。
 * - section 动作行 y=216（4 个 140×48 chip，x = 24 + i*148）：STORE / LOAD / ERASE /
 *   JUMP（JUMP = live loop 播放中的下一小节 section 请求；**绝不用 project LOAD**）。
 * - 歌曲动作行 y=216（x = 640 起，同样 4 槽）：SONG（模式切换）/ CHAIN（begin→commit）/
 *   CANCEL（链中可见）/ REC（SONG REC）。
 * - 16 项歌曲顺序：2 行 × 8 列，x = 24 + c*153，y = 300 + r*70，148×60 单对象
 *   `01 A 4`（序号 **1 起** + section 字母 + bars）；count 之外的槽显示 `01 --`（暗色），
 *   选中 = 4px UI_ACCENT outline。**索引口径**：屏幕/状态文本/测试断言用 **1 起**
 *   （`01`、`ENTRY 2`），console `TAP` 回显的 `entry=` 与 `STORE SONG` 一样用 **0 起**
 *   （page_song_tap 填 0..15），两者相差 1 是刻意的（与既有诊断面同口径）。
 * - 配置行 y=456（6 个 140×52 chip，x = 24 + i*148）：SCENE（循环 A→D）、BARS-/+、
 *   COUNT-/+、LOOP。每条动作 = **一条 CMD_SONG_SET**（整份 cfg 随命令下发），
 *   不直接写上游全局；新增项默认 `A4`，count/bars 的钳位由状态层/引擎做（页面不造值）。
 *
 * 动作（控件回调、TAP 注入与键盘路由共用同一 `song_action`）：STORE/LOAD/ERASE/JUMP =
 * CMD_SECTION_*，SONG = CMD_SONG_MODE，REC = CMD_SONG_REC，CHAIN/CANCEL = CMD_CHAIN_*，
 * 六个配置 chip = CMD_SONG_SET。CHAIN 的触摸语义 = 上游 SAVE 层：**第一次按 = begin**
 * （立即把选中 section 作为普通 live_req 排队、清旧 chain）；链中 tap A–D 卡只进 UI
 * 临时列表（≤8，空 scene 也在列表里，commit 时整份拒绝）；**再按 = commit**（一次原子
 * 安装，`sloop_seq_chain_commit` 首项自行请求）；CANCEL = 清临时列表与运行 chain。
 * 播放中按 CHAIN/未播放时 commit 由适配层回 blocked，页面显示 `CHAIN: BLOCKED`。
 * **临时链的丢弃**：commit 失败（空 scene/走带中）与页面被组作用域销毁/重建都会清空
 * 采集态与临时列表——不自动重试、不跨页保留（与上游「SAVE 松开即提交一次」一致；
 * 失败时用户须重新 CHAIN 起链）。
 *
 * 键盘（InputManager）：←/→ 在 count 内的条目间移动、↑/↓ 在 A–D card 间移动、
 * ENTER = LOAD 选中 section（SAVE/ERASE/CHAIN 只走触摸 + console，避免与演奏键冲突）。
 *
 * 上限来源：条目/count = `SLOOP_SONG_ENTRIES`、bars = `SLOOP_SONG_BARS_MAX`、chain =
 * `SLOOP_SONG_CHAIN_MAX`、section 数 = `SLOOP_STORE_PROJECTS`（不各写一份数字）。
 *
 * 内存：全部控件是**单对象可点击 label**（35 个对象；est 12,288B 是 spec §4.4 的
 * 计划上限，实测 delta 必须 ≤ 它——超了先改对象设计，不抬 est/不扩池）。样式是静态
 * 共享 lv_style_t，`styles_init` 每次开机只跑一次（组作用域会反复销毁/重建本页；
 * 重跑 lv_style_init 会丢掉 props 数组，见 ADR-008 §3 R34）。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_store.h"

/* 布局宏：几何同时用于建控件与 TAP 命中（scripts/inject/inject.py 的 SONG_* 同名同值） */
#define SONG_STATUS_X 24
#define SONG_STATUS_Y 20
#define SONG_STATUS_W 1232
#define SONG_STATUS_H 60

#define SONG_CARD_X0    24
#define SONG_CARD_Y     96
#define SONG_CARD_W     288
#define SONG_CARD_H     104
#define SONG_CARD_PITCH 304

#define SONG_SEC_Y      216
#define SONG_SEC_X0     24
#define SONG_MODE_X0    640
#define SONG_SEC_PITCH  148
#define SONG_CHIP_W     140
#define SONG_CHIP_H     48

#define SONG_ENTRY_X0       24
#define SONG_ENTRY_Y0       300
#define SONG_ENTRY_W        148
#define SONG_ENTRY_H        60
#define SONG_ENTRY_PITCH_X  153
#define SONG_ENTRY_PITCH_Y  70

#define SONG_CFG_X0     24
#define SONG_CFG_Y      456
#define SONG_CFG_PITCH  148
#define SONG_CFG_W      140
#define SONG_CFG_H      52

/* 共享上限（不各写一份）：条目/count 见 sloop_store.h，chain 见 sloop_seq.h。
 * section 数复用工程槽数（A–D）。 */
#define SONG_SECTIONS   SLOOP_STORE_PROJECTS

/* 控件下标（顺序 = 建控件顺序 = TAP 命中表顺序） */
enum {
    SONG_SEC_STORE = 0, SONG_SEC_LOAD, SONG_SEC_ERASE, SONG_SEC_JUMP,
    SONG_TOP_MODE = 0, SONG_TOP_CHAIN, SONG_TOP_CANCEL, SONG_TOP_REC,
    SONG_CFG_SCENE = 0, SONG_CFG_BARS_DN, SONG_CFG_BARS_UP,
    SONG_CFG_COUNT_DN, SONG_CFG_COUNT_UP, SONG_CFG_LOOP,
    SONG_SEC_COUNT = 4, SONG_TOP_COUNT = 4, SONG_CFG_COUNT = 6,
};

static const char *const k_sec_text[SONG_SEC_COUNT] = {"STORE", "LOAD", "ERASE", "JUMP"};
static const char *const k_top_text[SONG_TOP_COUNT] = {"SONG", "CHAIN", "CANCEL", "REC"};
static const char *const k_cfg_text[SONG_CFG_COUNT] = {"SCENE", "BARS-", "BARS+",
                                                       "COUNT-", "COUNT+", "LOOP"};
/* 配置 chip → 动作（sloop_song_act_t）；section/top 行按下标映射 */
static const uint8_t k_cfg_act[SONG_CFG_COUNT] = {
    SLOOP_SONG_SCENE, SLOOP_SONG_BARS_DN, SLOOP_SONG_BARS_UP,
    SLOOP_SONG_COUNT_DN, SLOOP_SONG_COUNT_UP, SLOOP_SONG_LOOP,
};
static const uint8_t k_sec_act[SONG_SEC_COUNT] = {
    SLOOP_SONG_STORE, SLOOP_SONG_LOAD, SLOOP_SONG_ERASE, SLOOP_SONG_JUMP,
};
static const uint8_t k_top_act[SONG_TOP_COUNT] = {
    SLOOP_SONG_MODE, SLOOP_SONG_CHAIN, SLOOP_SONG_CANCEL, SLOOP_SONG_REC,
};

static lv_obj_t *s_status;
static lv_obj_t *s_card[4];
static lv_obj_t *s_sec_chip[SONG_SEC_COUNT];
static lv_obj_t *s_top_chip[SONG_TOP_COUNT];
static lv_obj_t *s_entry_chip[SLOOP_SONG_ENTRIES];
static lv_obj_t *s_cfg[SONG_CFG_COUNT];

static lv_style_t s_st_card;
static lv_style_t s_st_card_sel;
static lv_style_t s_st_chip;
static lv_style_t s_st_chip_accent;
static lv_style_t s_st_chip_sel;

/* UI 局部态：选中项/链采集由输入任务或 LVGL 任务单写、LVGL 任务读（与 FILE/
 * TRANSPORT 页的单字节纪律一致）；**状态文本**跨任务读写，用临界区保护。 */
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_sec;    /* 选中 section（A–D） */
static uint8_t s_entry;  /* 选中歌曲条目 0..15 */
static bool s_collect;   /* chain 采集中（SAVE 层按下） */
static uint8_t s_chain_list[SLOOP_SONG_CHAIN_MAX];
static uint8_t s_chain_n;
static char s_status_text[64];
static uint8_t s_status_color; /* 0 灰 / 1 主文本 / 2 失败红 / 3 强调黄 */
static char s_status_echo[64]; /* console TAP 回显专用（另一任务） */

/* 脏检查缓存（0xFF/空 = 尚未刷新） */
static uint8_t s_card_shown[4];
static uint8_t s_card_sel_shown;
static char s_entry_cache[SLOOP_SONG_ENTRIES][10];
static uint8_t s_entry_sel_shown;
static int8_t s_collect_shown = -1;
static uint8_t s_mode_shown = 0xFF;
static uint8_t s_rec_shown = 0xFF;
static uint8_t s_loop_shown = 0xFF;
static char s_status_cache[192];
static uint32_t s_status_color_shown = 0xFFFFFFFFu;

static const char *const k_seq_token[8] = {"IDLE", "OK", "EMPTY", "INVALID",
                                           "FULL", "BLOCKED", "FROZEN", "ERROR"};

static void styles_init(void)
{
    /* 只初始化一次：组作用域会反复销毁/重建本页（ADR-008 §3 R34） */
    static bool done;
    if (done) return;
    done = true;

    lv_style_init(&s_st_card);
    lv_style_set_radius(&s_st_card, 16);
    lv_style_set_bg_opa(&s_st_card, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_card, lv_color_hex(UI_PANEL));
    lv_style_set_border_width(&s_st_card, 0);
    lv_style_set_outline_color(&s_st_card, lv_color_hex(UI_PANEL_HI));
    lv_style_set_outline_opa(&s_st_card, LV_OPA_COVER);
    lv_style_set_outline_pad(&s_st_card, 0);
    lv_style_set_outline_width(&s_st_card, 3);
    lv_style_set_text_font(&s_st_card, &lv_font_montserrat_20);
    lv_style_set_text_align(&s_st_card, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_card, 26); /* 两/三行 20pt ≈ 26..72px */

    lv_style_init(&s_st_card_sel);
    lv_style_set_bg_color(&s_st_card_sel, lv_color_hex(UI_PANEL_SEL));
    lv_style_set_outline_color(&s_st_card_sel, lv_color_hex(UI_ACCENT));
    lv_style_set_outline_width(&s_st_card_sel, 6);

    lv_style_init(&s_st_chip);
    lv_style_set_radius(&s_st_chip, 10);
    lv_style_set_bg_opa(&s_st_chip, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_chip, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_chip, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_chip, 14); /* 48 高、16pt 行高 ~20 */

    lv_style_init(&s_st_chip_accent);
    lv_style_set_bg_color(&s_st_chip_accent, lv_color_hex(UI_ACCENT));
    lv_style_set_text_color(&s_st_chip_accent, lv_color_hex(UI_BG));

    lv_style_init(&s_st_chip_sel);
    lv_style_set_bg_color(&s_st_chip_sel, lv_color_hex(UI_PANEL_SEL));
    lv_style_set_outline_color(&s_st_chip_sel, lv_color_hex(UI_ACCENT));
    lv_style_set_outline_opa(&s_st_chip_sel, LV_OPA_COVER);
    lv_style_set_outline_pad(&s_st_chip_sel, 0);
    lv_style_set_outline_width(&s_st_chip_sel, 4);
}

/* ------------------------------------------------------------ 小工具 --- */

static char scene_letter(uint8_t scene)
{
    return scene < SONG_SECTIONS ? (char)('A' + scene) : '-';
}

static void status_store(const char *text, uint8_t color)
{
    portENTER_CRITICAL(&s_ui_mux);
    snprintf(s_status_text, sizeof(s_status_text), "%s", text);
    s_status_color = color;
    portEXIT_CRITICAL(&s_ui_mux);
}

static uint8_t count_of(const app_state_t *st)
{
    uint8_t count = st->song.config.count;
    if (count < 1u) count = 1u;
    if (count > SLOOP_SONG_ENTRIES) count = SLOOP_SONG_ENTRIES;
    return count;
}

static bool storage_ok(void)
{
    return sloop_store_ready();
}

/* 最近一次动作的状态文本（TAP 回显与屏幕同源）。调用方负责缓冲生命周期。 */
const char *page_song_status_text(void)
{
    portENTER_CRITICAL(&s_ui_mux);
    snprintf(s_status_echo, sizeof(s_status_echo), "%s", s_status_text);
    portEXIT_CRITICAL(&s_ui_mux);
    return s_status_echo;
}

/* ---------------------------------------------------------------- 动作 --- */

typedef struct {
    uint8_t err;   /* 0 成功；否则 app_seq_status_t（页面守卫回 BLOCKED，未下发命令） */
    int16_t value; /* 写后回读值（失败时 = 写前真值） */
} song_result_t;

static void status_seq(uint8_t err, const char *what)
{
    char b[64];
    snprintf(b, sizeof(b), "%s: %s", what, err < 8u ? k_seq_token[err] : "ERROR");
    status_store(b, err == SEQ_STATUS_IDLE ? 0u : 2u);
}

/* 把选中 section 的字母拼进状态（`STORE B: EMPTY`） */
static void status_section(uint8_t act, uint8_t sec, uint8_t err)
{
    static const char *const k_names[4] = {"STORE", "LOAD", "ERASE", "JUMP"};
    char b[64];
    snprintf(b, sizeof(b), "%s %c: %s", k_names[act & 3u], scene_letter(sec),
             err == 0u ? "OK" : (err < 8u ? k_seq_token[err] : "ERROR"));
    status_store(b, err == 0u ? 1u : 2u);
}

static void chain_preview(void)
{
    char b[64];
    snprintf(b, sizeof(b), "CHAIN:");
    size_t n = strlen(b);
    for (uint8_t i = 0; i < s_chain_n && n + 3u < sizeof(b); i++) {
        b[n++] = ' ';
        b[n++] = scene_letter(s_chain_list[i]);
    }
    b[n] = '\0';
    status_store(b, 3);
}

static song_result_t song_action(uint8_t act);

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_song_update(&st);
}

/* 选中 section（触摸/TAP/键盘共用；链采集时 tap 卡 = 进临时列表） */
static void song_select_section(uint8_t sec)
{
    if (sec >= SONG_SECTIONS) return;
    s_sec = sec;
    if (s_collect) {
        if (s_chain_n >= SLOOP_SONG_CHAIN_MAX) {
            status_store("CHAIN: FULL", 2);
        } else {
            s_chain_list[s_chain_n++] = sec;
            chain_preview();
        }
    }
}

static void song_select_entry(uint8_t entry)
{
    if (entry >= SLOOP_SONG_ENTRIES) return;
    s_entry = entry;
}

/* 状态层的 seq_status：成功是 SEQ_STATUS_OK(1)，0 = 尚无命令——统一折算成
 * 「0 = 成功 / 其它 = app_seq_status_t」的动作结果口径（TAP 回显的 err 字段）。 */
static uint8_t seq_rc(uint8_t status)
{
    return status == SEQ_STATUS_OK ? 0u : status;
}

/* 一条 CMD_SONG_SET：整份 cfg 随命令下发（配置编辑不直接写上游全局） */
static song_result_t song_apply_cfg(const sloop_song_config_t *cfg, uint8_t act, uint8_t entry,
                                    int16_t value)
{
    song_result_t r = {0, value};
    edit_cmd_t cmd = {.type = CMD_SONG_SET, .p = cfg};
    state_apply(&cmd);
    app_state_t st;
    state_get_copy(&st);
    r.err = seq_rc(st.seq_status);
    if (r.err != 0u) {
        status_seq(r.err, k_cfg_text[act - SLOOP_SONG_SCENE]);
        return r;
    }
    /* 写后回读真值（引擎/状态层钳位后），不按本地计算结果伪造 */
    const sloop_song_config_t *c = &st.song.config;
    switch (act) {
    case SLOOP_SONG_SCENE: r.value = c->entry[entry].scene; break;
    case SLOOP_SONG_BARS_DN:
    case SLOOP_SONG_BARS_UP: r.value = c->entry[entry].bars; break;
    case SLOOP_SONG_COUNT_DN:
    case SLOOP_SONG_COUNT_UP: r.value = c->count; break;
    default: r.value = c->loop; break;
    }
    char b[64];
    switch (act) {
    case SLOOP_SONG_SCENE:
        snprintf(b, sizeof(b), "ENTRY %u SCENE %c", (unsigned)entry + 1u,
                 scene_letter((uint8_t)r.value));
        break;
    case SLOOP_SONG_BARS_DN:
    case SLOOP_SONG_BARS_UP:
        snprintf(b, sizeof(b), "ENTRY %u BARS %d", (unsigned)entry + 1u, (int)r.value);
        break;
    case SLOOP_SONG_COUNT_DN:
    case SLOOP_SONG_COUNT_UP: snprintf(b, sizeof(b), "COUNT %d", (int)r.value); break;
    default: snprintf(b, sizeof(b), "LOOP %s", r.value ? "ON" : "OFF"); break;
    }
    status_store(b, 1);
    return r;
}

static song_result_t song_action_cfg(uint8_t act)
{
    song_result_t r = {0, 0};
    app_state_t st;
    state_get_copy(&st);
    if (st.core.playing || st.song.running) {
        /* 上游 SONG 屏：歌曲顺序编辑要求停止走带（STOP FIRST），页面守卫不发命令 */
        status_store("STOP FIRST", 2);
        r.err = SEQ_STATUS_BLOCKED;
        return r;
    }
    uint8_t count = count_of(&st);
    sloop_song_config_t cfg = st.song.config;
    cfg.count = count;
    uint8_t entry = s_entry < SLOOP_SONG_ENTRIES ? s_entry : 0u;
    if (act == SLOOP_SONG_SCENE || act == SLOOP_SONG_BARS_DN || act == SLOOP_SONG_BARS_UP) {
        if (entry >= cfg.count) {
            /* count 之外的槽是可见但不可编辑的（与屏幕上 `01 --` 同义） */
            char b[64];
            snprintf(b, sizeof(b), "ENTRY %u: EMPTY", (unsigned)entry + 1u);
            status_store(b, 3);
            return r;
        }
    }
    switch (act) {
    case SLOOP_SONG_SCENE:
        cfg.entry[entry].scene = (uint8_t)((cfg.entry[entry].scene + 1u) & 3u);
        return song_apply_cfg(&cfg, act, entry, 0);
    case SLOOP_SONG_BARS_DN:
    case SLOOP_SONG_BARS_UP: {
        int32_t bars = (int32_t)cfg.entry[entry].bars + (act == SLOOP_SONG_BARS_UP ? 1 : -1);
        if (bars < 1) bars = 1;
        if (bars > (int32_t)SLOOP_SONG_BARS_MAX) bars = (int32_t)SLOOP_SONG_BARS_MAX;
        cfg.entry[entry].bars = (uint8_t)bars;
        return song_apply_cfg(&cfg, act, entry, 0);
    }
    case SLOOP_SONG_COUNT_DN:
    case SLOOP_SONG_COUNT_UP: {
        int32_t n = (int32_t)cfg.count + (act == SLOOP_SONG_COUNT_UP ? 1 : -1);
        if (n < 1) n = 1;
        if (n > (int32_t)SLOOP_SONG_ENTRIES) n = (int32_t)SLOOP_SONG_ENTRIES;
        if (n > (int32_t)cfg.count) {
            /* 新增项默认 A4（与上游 arr_defaults 的条目口径一致、可预测） */
            for (uint32_t i = cfg.count; i < (uint32_t)n; i++) {
                cfg.entry[i].scene = 0u;
                cfg.entry[i].bars = 4u;
            }
        }
        cfg.count = (uint8_t)n;
        return song_apply_cfg(&cfg, act, entry, 0);
    }
    default:
        cfg.loop = cfg.loop ? 0u : 1u;
        return song_apply_cfg(&cfg, act, entry, 0);
    }
}

static song_result_t song_action(uint8_t act)
{
    song_result_t r = {0, 0};
    app_state_t st;
    state_get_copy(&st);

    if (!storage_ok()) {
        status_store("NO STORAGE", 2);
        r.err = SEQ_STATUS_ERROR;
        return r;
    }

    /* 页面输入守卫（上游 SONG 屏：所有歌曲顺序/模式改动要求停止走带）：
     * 走带中（live loop 或 SONG）拒绝 MODE 与 16 项配置编辑；不发命令、回 BLOCKED。
     * section/chain 动作有自己的播放语义（STORE/JUMP/CHAIN 允许播放中），不在此拦。 */
    const bool transport_active = st.core.playing || st.song.running;

    switch (act) {
    case SLOOP_SONG_STORE:
    case SLOOP_SONG_LOAD:
    case SLOOP_SONG_ERASE:
    case SLOOP_SONG_JUMP: {
        edit_cmd_t cmd = {.a = s_sec};
        cmd.type = act == SLOOP_SONG_STORE   ? CMD_SECTION_STORE
                   : act == SLOOP_SONG_LOAD  ? CMD_SECTION_LOAD
                   : act == SLOOP_SONG_ERASE ? CMD_SECTION_ERASE
                                             : CMD_SECTION_REQUEST;
        state_apply(&cmd);
        state_get_copy(&st);
        r.err = seq_rc(st.seq_status);
        status_section(act, s_sec, r.err);
        break;
    }
    case SLOOP_SONG_MODE: {
        if (transport_active) {
            status_store("STOP FIRST", 2);
            r.err = SEQ_STATUS_BLOCKED;
            break;
        }
        edit_cmd_t cmd = {.type = CMD_SONG_MODE, .a = st.song.enabled ? 0u : 1u};
        state_apply(&cmd);
        state_get_copy(&st);
        r.err = seq_rc(st.seq_status);
        if (r.err != 0u) {
            status_seq(r.err, "MODE");
        } else {
            char b[16];
            snprintf(b, sizeof(b), "MODE: %s", st.song.enabled ? "SONG" : "LOOP");
            status_store(b, 1);
            r.value = st.song.enabled;
        }
        break;
    }
    case SLOOP_SONG_REC: {
        edit_cmd_t cmd = {.type = CMD_SONG_REC};
        state_apply(&cmd);
        state_get_copy(&st);
        r.err = seq_rc(st.seq_status);
        if (r.err != 0u) {
            status_seq(r.err, "REC");
        } else {
            static const char *const k_rec[] = {"OFF", "ARMED", "RECORDING"};
            char b[24];
            uint8_t srec = st.song.srec <= 2u ? st.song.srec : 0u;
            snprintf(b, sizeof(b), "REC: %s", k_rec[srec]);
            status_store(b, srec ? 3u : 1u);
            r.value = srec;
        }
        break;
    }
    case SLOOP_SONG_CHAIN:
        if (!s_collect) {
            /* SAVE 层按下：begin() 立即把选中 section 作为普通 live_req 排队 */
            edit_cmd_t cmd = {.type = CMD_CHAIN_BEGIN, .a = s_sec};
            state_apply(&cmd);
            state_get_copy(&st);
            r.err = seq_rc(st.seq_status);
            if (r.err != 0u) {
                status_seq(r.err, "CHAIN");
            } else {
                s_chain_list[0] = s_sec;
                s_chain_n = 1;
                s_collect = true;
                chain_preview();
                r.value = 1;
            }
        } else {
            /* SAVE 层松开：一次原子安装（任一 scene 空/非法整份拒绝） */
            edit_cmd_t cmd = {.type = CMD_CHAIN_COMMIT, .p = s_chain_list, .n = s_chain_n};
            uint8_t n = s_chain_n;
            state_apply(&cmd);
            s_collect = false;
            s_chain_n = 0;
            state_get_copy(&st);
            r.err = seq_rc(st.seq_status);
            if (r.err != 0u) {
                status_seq(r.err, "CHAIN");
            } else {
                char b[64];
                snprintf(b, sizeof(b), "CHAIN:");
                size_t len = strlen(b);
                for (uint8_t i = 0; i < n && len + 3u < sizeof(b); i++) {
                    b[len++] = ' ';
                    b[len++] = scene_letter(s_chain_list[i]);
                }
                b[len] = '\0';
                status_store(b, 1);
                r.value = n;
            }
        }
        break;
    case SLOOP_SONG_CANCEL: {
        edit_cmd_t cmd = {.type = CMD_CHAIN_CANCEL};
        state_apply(&cmd);
        s_collect = false;
        s_chain_n = 0;
        state_get_copy(&st);
        r.err = seq_rc(st.seq_status);
        if (r.err != 0u) status_seq(r.err, "CHAIN");
        else status_store("CHAIN: CANCEL", 1);
        break;
    }
    default: r = song_action_cfg(act); break;
    }
    return r;
}

/* ------------------------------------------------------------- 事件回调 --- */

static void card_clicked_cb(lv_event_t *e)
{
    uint8_t k = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    song_select_section(k);
    refresh();
}

static void entry_clicked_cb(lv_event_t *e)
{
    song_select_entry((uint8_t)(uintptr_t)lv_event_get_user_data(e));
    refresh();
}

static void chip_clicked_cb(lv_event_t *e)
{
    (void)song_action((uint8_t)(uintptr_t)lv_event_get_user_data(e));
    refresh();
}

/* ---------------------------------------------------------------- 建控件 --- */

static lv_obj_t *make_chip(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *chip = lv_label_create(parent);
    if (chip == NULL) return NULL;
    lv_obj_set_clickable(chip, true);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_size(chip, w, h);
    lv_obj_add_style(chip, &s_st_chip, 0);
    lv_label_set_text(chip, text);
    lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user);
    return chip;
}

lv_obj_t *page_song_build(lv_obj_t *parent)
{
    styles_init();
    /* 重建复位：缓存、链采集态、局部选中 */
    memset(s_card_shown, 0xFF, sizeof(s_card_shown));
    s_card_sel_shown = 0xFF;
    memset(s_entry_cache, 0, sizeof(s_entry_cache));
    s_entry_sel_shown = 0xFF;
    s_collect_shown = -1;
    s_mode_shown = 0xFF;
    s_rec_shown = 0xFF;
    s_loop_shown = 0xFF;
    s_status_cache[0] = '\0';
    s_status_color_shown = 0xFFFFFFFFu;
    s_sec = 0;
    s_entry = 0;
    s_collect = false;
    s_chain_n = 0;
    status_store("", 0);

    /* 页名由下行子签承担（SONG 组单页时子行留空、上行组签选中）——页内只留两行状态 */
    s_status = lv_label_create(parent);
    UI_BUILD_CHECK(s_status);
    lv_label_set_text(s_status, "");
    lv_obj_set_pos(s_status, SONG_STATUS_X, SONG_STATUS_Y);
    lv_obj_set_size(s_status, SONG_STATUS_W, SONG_STATUS_H);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM), 0);

    for (uint8_t k = 0; k < SONG_SECTIONS; k++) {
        lv_obj_t *card = lv_label_create(parent);
        UI_BUILD_CHECK(card);
        lv_obj_set_clickable(card, true);
        lv_obj_set_pos(card, SONG_CARD_X0 + (int32_t)k * SONG_CARD_PITCH, SONG_CARD_Y);
        lv_obj_set_size(card, SONG_CARD_W, SONG_CARD_H);
        lv_obj_add_style(card, &s_st_card, 0);
        lv_label_set_text(card, "A\nEMPTY");
        lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)k);
        s_card[k] = card;
    }

    for (uint8_t i = 0; i < SONG_SEC_COUNT; i++) {
        s_sec_chip[i] = make_chip(parent, SONG_SEC_X0 + (int32_t)i * SONG_SEC_PITCH, SONG_SEC_Y,
                                  SONG_CHIP_W, SONG_CHIP_H, k_sec_text[i], chip_clicked_cb,
                                  (void *)(uintptr_t)k_sec_act[i]);
        UI_BUILD_CHECK(s_sec_chip[i]);
    }
    for (uint8_t i = 0; i < SONG_TOP_COUNT; i++) {
        s_top_chip[i] = make_chip(parent, SONG_MODE_X0 + (int32_t)i * SONG_SEC_PITCH, SONG_SEC_Y,
                                  SONG_CHIP_W, SONG_CHIP_H, k_top_text[i], chip_clicked_cb,
                                  (void *)(uintptr_t)k_top_act[i]);
        UI_BUILD_CHECK(s_top_chip[i]);
    }
    lv_obj_set_hidden(s_top_chip[SONG_TOP_CANCEL], true); /* 只在链采集中显示 */

    for (uint8_t i = 0; i < SLOOP_SONG_ENTRIES; i++) {
        lv_obj_t *chip = make_chip(parent, SONG_ENTRY_X0 + (int32_t)(i % 8u) * SONG_ENTRY_PITCH_X,
                                   SONG_ENTRY_Y0 + (int32_t)(i / 8u) * SONG_ENTRY_PITCH_Y,
                                   SONG_ENTRY_W, SONG_ENTRY_H, "01 --", entry_clicked_cb,
                                   (void *)(uintptr_t)i);
        UI_BUILD_CHECK(chip);
        s_entry_chip[i] = chip;
    }
    for (uint8_t i = 0; i < SONG_CFG_COUNT; i++) {
        s_cfg[i] = make_chip(parent, SONG_CFG_X0 + (int32_t)i * SONG_CFG_PITCH, SONG_CFG_Y,
                             SONG_CFG_W, SONG_CFG_H, k_cfg_text[i], chip_clicked_cb,
                             (void *)(uintptr_t)k_cfg_act[i]);
        UI_BUILD_CHECK(s_cfg[i]);
    }
    return parent;
}

/* ---------------------------------------------------------------- 刷新 --- */

void page_song_update(const app_state_t *st)
{
    const bool ready = storage_ok();
    const sloop_song_snapshot_t *s = &st->song;
    uint8_t count = ready ? count_of(st) : 0u; /* 未就绪：16 槽全部显示 `--` */
    char b[192];

    /* A–D 卡：USED（ready 位）/ LIVE / 选中描边。样式只在选中态变化时 add/remove。 */
    for (uint8_t k = 0; k < SONG_SECTIONS; k++) {
        uint8_t key = (uint8_t)((ready && (s->ready & (1u << k)) ? 1u : 0u) |
                                (s->live == k ? 2u : 0u) | (s_sec == k ? 4u : 0u));
        if (key == s_card_shown[k]) continue;
        s_card_shown[k] = key;
        const bool used = (key & 1u) != 0u;
        const bool live = (key & 2u) != 0u;
        if (used && live) snprintf(b, sizeof(b), "%c\nUSED\nLIVE", scene_letter(k));
        else if (used) snprintf(b, sizeof(b), "%c\nUSED", scene_letter(k));
        else if (live) snprintf(b, sizeof(b), "%c\n--\nLIVE", scene_letter(k));
        else snprintf(b, sizeof(b), "%c\nEMPTY", scene_letter(k));
        lv_label_set_text(s_card[k], b);
        lv_obj_set_style_text_color(s_card[k],
                                    lv_color_hex(used ? UI_TEXT : UI_TEXT_DIM), 0);
    }
    if (s_card_sel_shown != s_sec) {
        for (uint8_t k = 0; k < SONG_SECTIONS; k++) {
            if (k == s_sec) lv_obj_add_style(s_card[k], &s_st_card_sel, 0);
            else lv_obj_remove_style(s_card[k], &s_st_card_sel, 0);
        }
        s_card_sel_shown = s_sec;
    }

    /* 16 项歌曲顺序：`NN A B`（count 之外 `NN --`）。文字只在变化时重设。 */
    for (uint8_t i = 0; i < SLOOP_SONG_ENTRIES; i++) {
        char eb[10];
        if (i < count) {
            snprintf(eb, sizeof(eb), "%02u %c %u", (unsigned)i + 1u,
                     scene_letter(s->config.entry[i].scene), (unsigned)s->config.entry[i].bars);
        } else {
            snprintf(eb, sizeof(eb), "%02u --", (unsigned)i + 1u);
        }
        if (strcmp(eb, s_entry_cache[i]) != 0) {
            snprintf(s_entry_cache[i], sizeof(s_entry_cache[i]), "%s", eb);
            lv_label_set_text(s_entry_chip[i], s_entry_cache[i]);
            lv_obj_set_style_text_color(s_entry_chip[i],
                                        lv_color_hex(i < count ? UI_TEXT : UI_TEXT_DIM), 0);
        }
    }
    if (s_entry_sel_shown != s_entry) {
        for (uint8_t i = 0; i < SLOOP_SONG_ENTRIES; i++) {
            if (i == s_entry) lv_obj_add_style(s_entry_chip[i], &s_st_chip_sel, 0);
            else lv_obj_remove_style(s_entry_chip[i], &s_st_chip_sel, 0);
        }
        s_entry_sel_shown = s_entry;
    }

    /* 动作/状态 chip 的视觉态：SONG 模式、链采集、REC、LOOP */
    uint8_t mode = s->enabled ? 1u : 0u;
    if (mode != s_mode_shown) {
        s_mode_shown = mode;
        if (mode) lv_obj_add_style(s_top_chip[SONG_TOP_MODE], &s_st_chip_accent, 0);
        else lv_obj_remove_style(s_top_chip[SONG_TOP_MODE], &s_st_chip_accent, 0);
    }
    uint8_t rec = ready ? (s->srec > 2u ? 0u : s->srec) : 0u;
    if (rec != s_rec_shown) {
        s_rec_shown = rec;
        if (rec) lv_obj_add_style(s_top_chip[SONG_TOP_REC], &s_st_chip_accent, 0);
        else lv_obj_remove_style(s_top_chip[SONG_TOP_REC], &s_st_chip_accent, 0);
    }
    uint8_t loop = ready ? (s->config.loop ? 1u : 0u) : 0u;
    if (loop != s_loop_shown) {
        s_loop_shown = loop;
        lv_label_set_text(s_cfg[SONG_CFG_LOOP], loop ? "LOOP ON" : "LOOP OFF");
        if (loop) lv_obj_add_style(s_cfg[SONG_CFG_LOOP], &s_st_chip_accent, 0);
        else lv_obj_remove_style(s_cfg[SONG_CFG_LOOP], &s_st_chip_accent, 0);
    }
    int8_t collect = s_collect ? 1 : 0;
    if (collect != s_collect_shown) {
        s_collect_shown = collect;
        lv_label_set_text(s_top_chip[SONG_TOP_CHAIN], collect ? "COMMIT" : "CHAIN");
        if (collect) {
            lv_obj_set_hidden(s_top_chip[SONG_TOP_CANCEL], false);
            lv_obj_add_style(s_top_chip[SONG_TOP_CHAIN], &s_st_chip_accent, 0);
        } else {
            lv_obj_set_hidden(s_top_chip[SONG_TOP_CANCEL], true);
            lv_obj_remove_style(s_top_chip[SONG_TOP_CHAIN], &s_st_chip_accent, 0);
        }
    }

    /* 两行状态行：运行态 + 本页最后一次动作结果（同一缓冲） */
    char action[64];
    uint8_t color;
    portENTER_CRITICAL(&s_ui_mux);
    snprintf(action, sizeof(action), "%s", s_status_text);
    color = s_status_color;
    portEXIT_CRITICAL(&s_ui_mux);

    if (!ready) {
        /* 未就绪：第一行已明确写 NO STORAGE；动作回显若也是它，不再重复一次 */
        if (action[0] == '\0' || strcmp(action, "NO STORAGE") == 0) {
            snprintf(b, sizeof(b), "SONG  NO STORAGE");
        } else {
            snprintf(b, sizeof(b), "SONG  NO STORAGE\n| %s", action);
        }
    } else {
        char ready_mask[5];
        for (uint8_t k = 0; k < SONG_SECTIONS; k++)
            ready_mask[k] = (s->ready & (1u << k)) ? scene_letter(k) : '-';
        ready_mask[4] = '\0';
        char last[16] = "none";
        if (s->srec_done == 0xFFu) snprintf(last, sizeof(last), "NO SONG");
        else if (s->srec_done != 0u) snprintf(last, sizeof(last), "%u", (unsigned)s->srec_done);
        static const char *const k_rec[] = {"OFF", "ARMED", "RECORDING"};
        snprintf(b, sizeof(b),
                 "SONG %s  %s  BAR %u  LIVE %c  READY %s\n"
                 "REC %s  CHAIN %u/%u  LAST %s  | %s",
                 s->enabled ? "SONG" : "LOOP", s->running ? "PLAY" : "STOP",
                 (unsigned)s->bar, scene_letter(s->live), ready_mask,
                 k_rec[s->srec <= 2u ? s->srec : 0u], (unsigned)s->chain_count,
                 (unsigned)s->chain_index, last, action);
    }
    if (strcmp(b, s_status_cache) != 0) {
        snprintf(s_status_cache, sizeof(s_status_cache), "%s", b);
        lv_label_set_text(s_status, s_status_cache);
    }
    uint32_t color_key = action[0] != '\0' ? color : 0u;
    if (color_key != s_status_color_shown) {
        s_status_color_shown = color_key;
        uint32_t rgb = color_key == 2u ? UI_ERROR : color_key == 3u ? UI_ACCENT
                       : color_key == 1u                       ? UI_TEXT
                                                               : UI_TEXT_DIM;
        lv_obj_set_style_text_color(s_status, lv_color_hex(rgb), 0);
    }
}

/* ------------------------------------------------------------- TAP 命中 --- */

bool page_song_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (uint8_t i = 0; i < SONG_SEC_COUNT; i++) {
        const int32_t bx = SONG_SEC_X0 + (int32_t)i * SONG_SEC_PITCH;
        if (x < bx || x >= bx + SONG_CHIP_W || y < SONG_SEC_Y || y >= SONG_SEC_Y + SONG_CHIP_H) {
            continue;
        }
        song_result_t r = song_action(k_sec_act[i]);
        if (out != NULL) {
            out->kind = SLOOP_TAP_SONG_ACTION;
            out->a = APP_PAGE_SONG;
            out->b = k_sec_act[i];
            out->v = r.value;
            out->d = (int8_t)s_sec;
            out->err = r.err;
        }
        return true;
    }
    for (uint8_t i = 0; i < SONG_TOP_COUNT; i++) {
        const int32_t bx = SONG_MODE_X0 + (int32_t)i * SONG_SEC_PITCH;
        if (x < bx || x >= bx + SONG_CHIP_W || y < SONG_SEC_Y || y >= SONG_SEC_Y + SONG_CHIP_H) {
            continue;
        }
        if (i == SONG_TOP_CANCEL && !s_collect) return false; /* 隐藏控件不可命中 */
        song_result_t r = song_action(k_top_act[i]);
        if (out != NULL) {
            out->kind = SLOOP_TAP_SONG_ACTION;
            out->a = APP_PAGE_SONG;
            out->b = k_top_act[i];
            out->v = r.value;
            out->d = (int8_t)s_sec;
            out->err = r.err;
        }
        return true;
    }
    for (uint8_t i = 0; i < SONG_CFG_COUNT; i++) {
        const int32_t bx = SONG_CFG_X0 + (int32_t)i * SONG_CFG_PITCH;
        if (x < bx || x >= bx + SONG_CFG_W || y < SONG_CFG_Y || y >= SONG_CFG_Y + SONG_CFG_H) {
            continue;
        }
        song_result_t r = song_action(k_cfg_act[i]);
        if (out != NULL) {
            out->kind = SLOOP_TAP_SONG_ACTION;
            out->a = APP_PAGE_SONG;
            out->b = k_cfg_act[i];
            out->v = r.value;
            out->d = (int8_t)s_sec;
            out->err = r.err;
        }
        return true;
    }
    for (uint8_t k = 0; k < SONG_SECTIONS; k++) {
        const int32_t cx = SONG_CARD_X0 + (int32_t)k * SONG_CARD_PITCH;
        if (x < cx || x >= cx + SONG_CARD_W || y < SONG_CARD_Y || y >= SONG_CARD_Y + SONG_CARD_H) {
            continue;
        }
        song_select_section(k);
        if (out != NULL) {
            out->kind = SLOOP_TAP_SONG_SECTION;
            out->a = APP_PAGE_SONG;
            out->b = k;
        }
        return true;
    }
    for (uint8_t i = 0; i < SLOOP_SONG_ENTRIES; i++) {
        const int32_t cx = SONG_ENTRY_X0 + (int32_t)(i % 8u) * SONG_ENTRY_PITCH_X;
        const int32_t cy = SONG_ENTRY_Y0 + (int32_t)(i / 8u) * SONG_ENTRY_PITCH_Y;
        if (x < cx || x >= cx + SONG_ENTRY_W || y < cy || y >= cy + SONG_ENTRY_H) continue;
        song_select_entry(i);
        if (out != NULL) {
            out->kind = SLOOP_TAP_SONG_ENTRY;
            out->a = APP_PAGE_SONG;
            out->b = i;
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------- InputManager 路由 --- */

bool sloop_ui_song_page_active(void)
{
    return sloop_ui_current_page() == APP_PAGE_SONG;
}

void sloop_ui_song_entry_move(int8_t delta)
{
    app_state_t st;
    state_get_copy(&st);
    uint8_t count = count_of(&st);
    int32_t v = (int32_t)s_entry + delta;
    while (v < 0) v += count;
    while (v >= (int32_t)count) v -= (int32_t)count;
    s_entry = (uint8_t)v; /* 输入任务上下文：不碰控件，描边由下一拍 update 落地 */
}

void sloop_ui_song_section_move(int8_t delta)
{
    int32_t v = (int32_t)s_sec + delta;
    while (v < 0) v += SONG_SECTIONS;
    while (v >= SONG_SECTIONS) v -= SONG_SECTIONS;
    s_sec = (uint8_t)v;
}

void sloop_ui_song_load(void)
{
    song_result_t r = song_action(SLOOP_SONG_LOAD); /* 输入任务上下文：只写状态层/状态文本 */
    (void)r;
}
