/*
 * TRANSPORT 页：走带 + 录音/fill + 聚焦步字段 + pattern 工具区（P4-C Task 10）
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H）：
 * - PLAY (48,48,320,180) 轨绿；STOP (400,48,320,180) 轨橙；
 * - 走带状态 + 上一次工具动作 (48,264) 28pt 单行：`PLAYING` / `STOPPED`，有动作后
 *   追加 `   <动作>: <结果>`（`LVL: HARD` / `ROTATE: FULL` / `SPAN: N/A (DRUM)`）；
 *   失败红字、N/A 黄字（seq_status 与「不适用」在屏幕可见）；"take 结果"不在这里
 *   （它在 REC chip 上，避免与同一拍的动作结果互相覆盖）；
 * - 步位置 + 聚焦步字段 (48,320) 20pt 两行：
 *     `TRACK n   STEP pos / len`                        ← 选中轨的播放头 / LEN
 *     `T<ft+1> S<abs>  LVL N  RAT x1  MIC +0  SPAN 1  SLD -`
 *   第二行是**工具的目标**：`T<ft+1>` = 焦点轨、`S<abs+1>` = 引擎绝对步（1 起，与
 *   STEP 页列号同口径；第一行的 `STEP pos / len` 是引擎 step_pos 原值 = 0 起），其后
 *   是字段真值（鼓轨：`T4 S<n>  DRM L<lane>  LVL .. RAT .. MIC ..`）。label 颜色取
 *   **焦点轨色**（工具写的那条轨）——第一行的 TRACK 号仍是选中轨（走带），两者可以
 *   不同（←/→/↑/↓ 只动焦点、不动选中轨）；
 * - 录音/fill 行 y=384（56 高）：REC (48,140) / FILL (200,120) / NEXT (328,120)；
 *   REC 文本即**五态**（`REC OFF` / `REC ARM` / `REC ON T<n>` / `REC CNT T<n>` /
 *   `FREE TAKE`），FILL 显示保持电平（`FILL` / `FILL ON`），NEXT 显示待命
 *   （`NEXT` / `NEXT ARM`）；
 * - REC 配置行（P4-F Task 6）与录音/fill 行同高 y=384：QNT (456,100) / MODE (564,100) /
 *   START (672,100)——落在 NEXT 右侧到 pattern 区（x=792）之前的空带，不挤其它行。文本即
 *   当前值（`QNT ON` / `QNT OFF`、`FREE` / `TEMPO`、`NOTE` / `COUNT`，读 app_state_t.rec）；
 *   点一次翻转该项并发**整份** CMD_REC_CONFIG（另两项取 rec 投影）。不在键盘焦点环内；
 * - 聚焦步字段工具行 y=464（56 高，88 宽、间距 8，x = 24 + i*96）：
 *   LVL / RAT / MIC- / MIC+ / SPAN / SLIDE。LVL 与 RAT 是**循环按钮**（点一次
 *   前进一档：LVL 响度序 GHOST<SOFT<NORM<HARD、RAT x1..x4），SPAN 点一次 +1
 *   小节数（到 LEN 后回 1），SLIDE 点一次切换；MIC-/+ 是 ±1（nudge 需要双向）。
 *   键盘 ↑/↓ 对全部字段都有双向（见下）——触摸只保留循环/双向的常用方向；
 * - pattern 区（x 792 起）：y=292 `ROT-`/`ROT+`/`LEN x2`/`LEN /2`（100 宽，
 *   间距 8）、y=360 `TRANS-`/`TRANS+`（120）+`UNDO`/`REDO`（84）、y=428
 *   `CLEAR T<n>`（432 宽，破坏性动作单独一行）；
 * - BPM (900,40) 标题 + (880,72) 数值 + (860/1000,140) −/+（既有，保持）。
 *
 * 动作（控件回调、TAP 注入与键盘调值共用同一个 `tr_action`）：
 * - 每条动作 = **一条 EditCmd**（REC/fill 走 CMD_REC_TOGGLE/CMD_FILL_CONTROL，字段走
 *   CMD_STEP_EDIT / CMD_SET_MICRO，鼓轨 LVL/RAT 读改写原始 16-lane POD 后
 *   CMD_DRUM_STEP_WRITE，pattern 走 CMD_PATTERN）；绝不直接碰引擎/上游全局。
 * - 值取自快照、写后**回读**真值（TAP 回显与屏幕状态行都报回读值，失败时报写前
 *   真值 + seq_status），不按本地计算结果伪造成功。
 * - LVL/RAT 作用在聚焦步的第 1 个音符（index 0）：STEP 页没有音符光标（扩音符光标
 *   是后续 UI 工作，见 docs/deferred-issues.md DI-30）。
 * - SPAN 以**聚焦步可见的 TIE 链长度**为基准（+1 / 到 LEN 回 1）：POLY 音符长到后续
 *   和弦下方（CONT）时快照里没有精确 span（同 DI-30）。
 * - 鼓轨：LVL/RAT 改**选中 lane**（STEP 页 `UI LANE`/↑↓/点 dot 的同一字节），
 *   MIC 是整步 nudge；SPAN/SLIDE 不适用（不发命令，回 `N/A (DRUM)`）。
 * - pattern 工具作用于**选中轨**（引擎真值，非焦点轨）；TRANSPOSE 在鼓轨由上游拒绝
 *   （`TRANSPOSE: INVALID`）。
 *
 * 键盘（ADR-006）：TRANSPORT 页 ←/→ 移工具焦点（LVL..TRANS 环），↑/↓ 调焦点字段
 * （LVL/RAT 双向循环、MIC ±1、SPAN ±1、SLIDE 切换、ROT ±1、LEN ×2/÷2、TRANS ±1），
 * ENTER = +1/切换。焦点描边画在该字段的 chip 上（与参数页的焦点模型一致）。
 *
 * 内存：全部按钮是**单对象可点击 label**（不用 lv_button + 子 label），底色/字体/
 * 内边距来自静态共享 lv_style_t（styles_init 每次开机只跑一次——组作用域会反复销毁/
 * 重建本页；重跑 lv_style_init 会漏掉 props 数组）。状态变化只换/加共享样式指针或改
 * 文本，脏检查避免每拍重设。工具动作的结果复用走带状态 label（不新增状态行对象），
 * 页面净增 18 + 3（REC 配置）= 21 个单对象 chip（实测见 ADR-008 的 TRANSPORT 行；est 仍为 9,216）。
 */
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sloop_state.h"

/* ------------------------------------------------------------ 几何（TAP 同源） --- */

/* 既有：PLAY/STOP/BPM */
#define TRANSPORT_PLAY_X  48
#define TRANSPORT_PLAY_Y  48
#define TRANSPORT_PLAY_W  320
#define TRANSPORT_PLAY_H  180
#define TRANSPORT_STOP_X  400
#define TRANSPORT_STOP_Y  48
#define TRANSPORT_STOP_W  320
#define TRANSPORT_STOP_H  180
#define TRANSPORT_BPM_MINUS_X 860
#define TRANSPORT_BPM_PLUS_X  1000
#define TRANSPORT_BPM_Y       140
#define TRANSPORT_BPM_W       100
#define TRANSPORT_BPM_H       100

/* 录音/fill 行 */
#define TR_REC_X  48
#define TR_REC_Y  384
#define TR_REC_W  140
#define TR_REC_H  56
#define TR_FILL_X 200
#define TR_FILL_W 120
#define TR_NEXT_X 328

/* REC 配置行（与录音/fill 行同 y；宽 100、间距 108，右端 772 < pattern 区 792） */
#define TR_CFG_X0     456
#define TR_CFG_W      100
#define TR_CFG_PITCH  108

/* 聚焦步字段工具行 */
#define TR_STEP_Y     464
#define TR_STEP_H     56
#define TR_STEP_X0    24
#define TR_STEP_W     88
#define TR_STEP_PITCH 96

/* pattern 区 */
#define TR_PAT_X0     792
#define TR_PAT_ROW0   292
#define TR_PAT_ROW1   360
#define TR_PAT_ROW2   428
#define TR_PAT_H      56
#define TR_PAT_W      100
#define TR_PAT_PITCH  108
#define TR_PAT_W_GAP  120 /* TRANS-/TRANS+ */
#define TR_PAT_W_SM   84  /* UNDO/REDO */
#define TR_PAT_X_TR2  920
#define TR_PAT_X_UNDO 1048
#define TR_PAT_X_REDO 1140
#define TR_PAT_W_WIDE 432 /* CLEAR：单独一行 */

/* 上游 core.h 的步编码（不引入 vendored 头）：time = ST_NOTE/ST_TIE；flags 的
 * SF_SLIDE 位。与 page_step.c 的同名局部常量同义。 */
#define STEP_TIME_TIE   1u
#define STEP_FLAG_SLIDE 0x02u
#define TR_STEPS_MAX    (APP_STATE_STEPS * 4u)

/* 工具焦点字段（键盘 ←/→ 环，UI 局部态） */
enum { TR_F_LVL = 0, TR_F_RAT, TR_F_MIC, TR_F_SPAN, TR_F_SLIDE,
       TR_F_ROT, TR_F_LEN, TR_F_TRANS, TR_F_COUNT };

/* chip 下标（文本/样式按此定位；顺序 = 建控件顺序 = TAP 命中表顺序） */
enum {
    CHIP_REC = 0, CHIP_FILL, CHIP_NEXT,
    CHIP_LVL, CHIP_RAT, CHIP_MIC_DN, CHIP_MIC_UP, CHIP_SPAN, CHIP_SLIDE,
    CHIP_ROT_DN, CHIP_ROT_UP, CHIP_LEN_X2, CHIP_LEN_D2, CHIP_TRANS_DN, CHIP_TRANS_UP,
    CHIP_UNDO, CHIP_REDO, CHIP_CLEAR,
    CHIP_QNT, CHIP_MODE, CHIP_START, /* REC 配置（追加在末尾：既有 chip 下标不变） */
    CHIP_COUNT
};

typedef struct {
    int16_t x, y, w, h;
    uint8_t act; /* sloop_transport_act_t */
    const char *text;
} tr_chip_t;

/* 建控件与 TAP 命中都只用这张表（几何/动作/初始文本的唯一来源） */
static const tr_chip_t k_chips[CHIP_COUNT] = {
    {TR_REC_X, TR_REC_Y, TR_REC_W, TR_REC_H, SLOOP_TR_REC, "REC OFF"},
    {TR_FILL_X, TR_REC_Y, TR_FILL_W, TR_REC_H, SLOOP_TR_FILL_HOLD, "FILL"},
    {TR_NEXT_X, TR_REC_Y, TR_FILL_W, TR_REC_H, SLOOP_TR_FILL_NEXT, "NEXT"},
    {TR_STEP_X0 + 0 * TR_STEP_PITCH, TR_STEP_Y, TR_STEP_W, TR_STEP_H, SLOOP_TR_LVL_UP, "LVL"},
    {TR_STEP_X0 + 1 * TR_STEP_PITCH, TR_STEP_Y, TR_STEP_W, TR_STEP_H, SLOOP_TR_RAT_UP, "RAT"},
    {TR_STEP_X0 + 2 * TR_STEP_PITCH, TR_STEP_Y, TR_STEP_W, TR_STEP_H, SLOOP_TR_MIC_DN, "MIC-"},
    {TR_STEP_X0 + 3 * TR_STEP_PITCH, TR_STEP_Y, TR_STEP_W, TR_STEP_H, SLOOP_TR_MIC_UP, "MIC+"},
    {TR_STEP_X0 + 4 * TR_STEP_PITCH, TR_STEP_Y, TR_STEP_W, TR_STEP_H, SLOOP_TR_SPAN_UP, "SPAN"},
    {TR_STEP_X0 + 5 * TR_STEP_PITCH, TR_STEP_Y, TR_STEP_W, TR_STEP_H, SLOOP_TR_SLIDE, "SLIDE"},
    {TR_PAT_X0 + 0 * TR_PAT_PITCH, TR_PAT_ROW0, TR_PAT_W, TR_PAT_H, SLOOP_TR_ROT_DN, "ROT-"},
    {TR_PAT_X0 + 1 * TR_PAT_PITCH, TR_PAT_ROW0, TR_PAT_W, TR_PAT_H, SLOOP_TR_ROT_UP, "ROT+"},
    {TR_PAT_X0 + 2 * TR_PAT_PITCH, TR_PAT_ROW0, TR_PAT_W, TR_PAT_H, SLOOP_TR_LEN_X2, "LEN x2"},
    {TR_PAT_X0 + 3 * TR_PAT_PITCH, TR_PAT_ROW0, TR_PAT_W, TR_PAT_H, SLOOP_TR_LEN_D2, "LEN /2"},
    {TR_PAT_X0, TR_PAT_ROW1, TR_PAT_W_GAP, TR_PAT_H, SLOOP_TR_TRANS_DN, "TRANS-"},
    {TR_PAT_X_TR2, TR_PAT_ROW1, TR_PAT_W_GAP, TR_PAT_H, SLOOP_TR_TRANS_UP, "TRANS+"},
    {TR_PAT_X_UNDO, TR_PAT_ROW1, TR_PAT_W_SM, TR_PAT_H, SLOOP_TR_UNDO, "UNDO"},
    {TR_PAT_X_REDO, TR_PAT_ROW1, TR_PAT_W_SM, TR_PAT_H, SLOOP_TR_REDO, "REDO"},
    {TR_PAT_X0, TR_PAT_ROW2, TR_PAT_W_WIDE, TR_PAT_H, SLOOP_TR_CLEAR, "CLEAR"},
    {TR_CFG_X0 + 0 * TR_CFG_PITCH, TR_REC_Y, TR_CFG_W, TR_REC_H, SLOOP_TR_REC_QNT, "QNT ON"},
    {TR_CFG_X0 + 1 * TR_CFG_PITCH, TR_REC_Y, TR_CFG_W, TR_REC_H, SLOOP_TR_REC_MODE, "TEMPO"},
    {TR_CFG_X0 + 2 * TR_CFG_PITCH, TR_REC_Y, TR_CFG_W, TR_REC_H, SLOOP_TR_REC_START, "NOTE"},
};

/* chip → 焦点字段（-1 = 不属于焦点环：REC/fill/UNDO/REDO/CLEAR） */
static const int8_t k_chip_field[CHIP_COUNT] = {
    -1, -1, -1,
    TR_F_LVL, TR_F_RAT, TR_F_MIC, TR_F_MIC, TR_F_SPAN, TR_F_SLIDE,
    TR_F_ROT, TR_F_ROT, TR_F_LEN, TR_F_LEN, TR_F_TRANS, TR_F_TRANS,
    -1, -1, -1,
    -1, -1, -1,
};

/* ---------------------------------------------------------------- 静态对象/样式 --- */

static lv_obj_t *s_status_label;
static lv_obj_t *s_step_label;
static lv_obj_t *s_bpm_value;
static lv_obj_t *s_chip[CHIP_COUNT];

static lv_style_t s_st_big_play;
static lv_style_t s_st_big_stop;
static lv_style_t s_st_big_bpm;
static lv_style_t s_st_chip;
static lv_style_t s_st_chip_accent;
static lv_style_t s_st_chip_rec;
static lv_style_t s_st_chip_count;
static lv_style_t s_st_chip_free;
static lv_style_t s_st_chip_focus;

/* 脏检查缓存（-1/0xFF = 尚未刷新） */
static int16_t s_bpm = -1;
static char s_status_cache[72];
static uint32_t s_status_color_shown = 0xFFFFFFFFu;
static char s_step_cache[96];
static char s_rec_cache[16];
static char s_take_txt[14];   /* 最近一次 take 结果（OFF 时显示在 REC chip 上） */
static char s_fill_cache[8];
static char s_next_cache[12];
static char s_cfg_cache[3][12]; /* REC 配置三 chip 的文本缓存 */
static uint8_t s_rec_style = 0xFF;  /* 已应用的录音态底色（0..4） */
static int8_t s_focus_shown = -1;   /* 已画描边的字段 */
static int8_t s_drum_shown = -1;    /* SPAN/SLIDE 的可用态（鼓轨 = 不可用） */
static int8_t s_clear_track = -1;   /* CLEAR 文本的目标轨 */
static int8_t s_fill_shown = -1;    /* FILL 保持电平的底色 */
static int8_t s_next_shown = -1;    /* NEXT 待命的底色 */
static int8_t s_slide_shown = -1;   /* SLIDE 开关的底色 */

/* UI 局部态（输入任务/console 写，LVGL 任务读）：工具焦点、上一次动作结果。
 * 用临界区保护（与 page_params.c 的旋钮焦点同一纪律）。 */
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_field;             /* 工具焦点字段（跨组页面重建保留） */
static char s_status_text[40];
static uint8_t s_status_color;      /* 0 灰 / 1 主文本 / 2 失败红 / 3 强调黄 */

static void styles_init(void)
{
    /* 只初始化一次（ADR-008 §3 R34）：组作用域生命周期会反复销毁/重建本页，
     * lv_style_init 是 memzero——重跑会丢掉静态样式已分配的 props 数组。 */
    static bool done;
    if (done) {
        lv_style_set_bg_color(&s_st_big_play, lv_color_hex(ui_track_color(1)));
        lv_style_set_text_color(&s_st_big_play, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_big_stop, lv_color_hex(ui_track_color(3)));
        lv_style_set_text_color(&s_st_big_stop, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_big_bpm, lv_color_hex(UI_PANEL_HI));
        lv_style_set_text_color(&s_st_big_bpm, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
        lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip_accent, lv_color_hex(UI_ACCENT));
        lv_style_set_text_color(&s_st_chip_accent, lv_color_hex(UI_BG));
        lv_style_set_bg_color(&s_st_chip_rec, lv_color_hex(UI_ERROR));
        lv_style_set_text_color(&s_st_chip_rec, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip_count, lv_color_hex(ui_track_color(3)));
        lv_style_set_text_color(&s_st_chip_count, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip_free, lv_color_hex(ui_track_color(1)));
        lv_style_set_text_color(&s_st_chip_free, lv_color_hex(UI_BG));
        lv_style_set_outline_color(&s_st_chip_focus, lv_color_hex(UI_TEXT));
        return;
    }
    done = true;

    /* 大字按钮（PLAY/STOP/BPM ±）：圆角 16、28pt 居中（内边距把单行文本压到中心）。 */
    lv_style_init(&s_st_big_play);
    lv_style_set_radius(&s_st_big_play, 16);
    lv_style_set_bg_opa(&s_st_big_play, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_big_play, lv_color_hex(ui_track_color(1)));
    lv_style_set_text_font(&s_st_big_play, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_big_play, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_big_play, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_big_play, 72); /* 180 高、28pt 行高 ~36：上下各 72 */

    lv_style_init(&s_st_big_stop);
    lv_style_set_radius(&s_st_big_stop, 16);
    lv_style_set_bg_opa(&s_st_big_stop, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_big_stop, lv_color_hex(ui_track_color(3)));
    lv_style_set_text_font(&s_st_big_stop, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_big_stop, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_big_stop, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_big_stop, 72);

    lv_style_init(&s_st_big_bpm);
    lv_style_set_radius(&s_st_big_bpm, 16);
    lv_style_set_bg_opa(&s_st_big_bpm, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_big_bpm, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_big_bpm, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_big_bpm, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_big_bpm, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_big_bpm, 32); /* 100 高、28pt 行高 ~36 */

    /* 工具 chip（16pt 居中）：56 高、行高 ~20 → 上边距 18。 */
    lv_style_init(&s_st_chip);
    lv_style_set_radius(&s_st_chip, 12);
    lv_style_set_bg_opa(&s_st_chip, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_chip, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_chip, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_chip, 18);

    /* 底色覆盖样式（叠加在 s_st_chip 之上，按状态 add/remove） */
    lv_style_init(&s_st_chip_accent);
    lv_style_set_bg_color(&s_st_chip_accent, lv_color_hex(UI_ACCENT));
    lv_style_set_text_color(&s_st_chip_accent, lv_color_hex(UI_BG));
    lv_style_init(&s_st_chip_rec);
    lv_style_set_bg_color(&s_st_chip_rec, lv_color_hex(UI_ERROR));
    lv_style_set_text_color(&s_st_chip_rec, lv_color_hex(UI_TEXT));
    lv_style_init(&s_st_chip_count);
    lv_style_set_bg_color(&s_st_chip_count, lv_color_hex(ui_track_color(3)));
    lv_style_set_text_color(&s_st_chip_count, lv_color_hex(UI_TEXT));
    lv_style_init(&s_st_chip_free);
    lv_style_set_bg_color(&s_st_chip_free, lv_color_hex(ui_track_color(1)));
    lv_style_set_text_color(&s_st_chip_free, lv_color_hex(UI_BG));

    /* 焦点描边（键盘 ←/→ 落在哪个字段）：3px 白框，画在 chip 外缘。 */
    lv_style_init(&s_st_chip_focus);
    lv_style_set_outline_width(&s_st_chip_focus, 3);
    lv_style_set_outline_opa(&s_st_chip_focus, LV_OPA_COVER);
    lv_style_set_outline_color(&s_st_chip_focus, lv_color_hex(UI_TEXT));
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_transport_update(&st);
}

/* ---------------------------------------------------------------- 值换算 --- */

static uint8_t window_len_of(const app_state_t *st)
{
    uint8_t wlen = st->window.window_len;
    if (wlen < 1u) wlen = 1u;
    if (wlen > APP_STATE_STEPS) wlen = APP_STATE_STEPS;
    return wlen;
}

/* 聚焦步（焦点轨 × 窗口列）→ 引擎绝对步号 */
static uint8_t focus_of(const app_state_t *st, uint8_t *ft, uint8_t *fs)
{
    *ft = st->focus_track < APP_STATE_TRACKS ? st->focus_track : 0u;
    *fs = st->focus_step < window_len_of(st) ? st->focus_step : 0u;
    return step_window_abs(&st->window, *fs);
}

/* LVL 的响度序：raw 1 GHOST < 2 SOFT < 0 NORM < 3 HARD（默认 0 = NORM）。 */
static uint8_t lvl_step(uint8_t raw, bool up)
{
    static const uint8_t order[4] = {1u, 2u, 0u, 3u};
    uint8_t i = 0;
    for (uint8_t k = 0; k < 4u; k++) {
        if (order[k] == (raw & 3u)) i = k;
    }
    i = (uint8_t)((i + (up ? 1u : 3u)) & 3u);
    return order[i];
}

static uint8_t dstep_bits(uint8_t packed, uint8_t lane)
{
    return (uint8_t)((packed >> (2u * (lane % 4u))) & 3u);
}

static void dstep_set_bits(uint8_t *packed, uint8_t lane, uint8_t value)
{
    const uint8_t sh = (uint8_t)(2u * (lane % 4u));
    packed[lane / 4u] = (uint8_t)((packed[lane / 4u] & ~(3u << sh)) | (uint8_t)(value << sh));
}

/* 聚焦步的 TIE 链长度（步数，含本步）：SPAN 的基准。窗口/TIE 之外不可见（DI-30）。 */
static uint8_t span_base(const app_state_t *st, uint8_t ft, uint8_t fs)
{
    const uint8_t wlen = window_len_of(st);
    uint8_t len = st->core.trk[ft].len;
    if (len < 1u) len = 1u;
    if (len > TR_STEPS_MAX) len = TR_STEPS_MAX;
    uint8_t n = 1;
    while ((uint8_t)(fs + n) < wlen &&
           (uint8_t)(st->window.window_start + fs + n) < len &&
           st->core.step[ft][fs + n].time == STEP_TIME_TIE) {
        n++;
    }
    return n;
}

static uint8_t drum_lane(void)
{
    const uint8_t lane = sloop_ui_step_lane_get();
    return lane > 15u ? 0u : lane;
}

/* TAP 回显用：焦点轨是鼓轨时 LVL/RAT 实际走 CMD_DRUM_STEP_WRITE。 */
static bool focus_track_is_drum(void)
{
    app_state_t st;
    state_get_copy(&st);
    return st.focus_track == APP_STATE_TRACKS - 1u;
}

/* ---------------------------------------------------------------- 动作 --- */

typedef struct {
    uint8_t err;   /* 0 OK；app_seq_status_t；SLOOP_TR_ERR_NA */
    int16_t value; /* 写后回读值（失败时 = 写前真值） */
} tr_result_t;

static const char *const k_seq_screen[8] = {"", "OK", "EMPTY", "INVALID",
                                            "FULL", "BLOCKED", "FROZEN", "ERROR"};
static const char *const k_act_name[SLOOP_TR_ACT_COUNT] = {
    "REC", "FILL", "NEXT",
    "LVL", "LVL", "RAT", "RAT", "MIC", "MIC", "SPAN", "SPAN", "SLIDE",
    "ROTATE", "ROTATE", "LEN", "LEN", "TRANSPOSE", "TRANSPOSE",
    "CLEAR", "UNDO", "REDO",
    "QNT", "MODE", "START",
};
static const char *const k_lvl_name[4] = {"NORM", "GHOST", "SOFT", "HARD"};

static void status_store(const char *text, uint8_t color)
{
    portENTER_CRITICAL(&s_ui_mux);
    snprintf(s_status_text, sizeof(s_status_text), "%s", text);
    s_status_color = color;
    portEXIT_CRITICAL(&s_ui_mux);
}

/* 动作结果 → 状态文本（屏幕与 console 回显同源：都来自 tr_action 的返回值）。 */
static void status_note(uint8_t act, const tr_result_t *r, uint8_t track)
{
    char b[40];
    if (r->err == SLOOP_TR_ERR_NA) {
        snprintf(b, sizeof(b), "%s: N/A (DRUM)", k_act_name[act]);
        status_store(b, 3);
        return;
    }
    if (r->err != 0u) {
        snprintf(b, sizeof(b), "%s: %s", k_act_name[act],
                 r->err < 8u ? k_seq_screen[r->err] : "ERROR");
        status_store(b, 2);
        return;
    }
    switch (act) {
    case SLOOP_TR_REC: snprintf(b, sizeof(b), "REC: TOGGLED"); break;
    case SLOOP_TR_FILL_HOLD: snprintf(b, sizeof(b), "FILL: %s", r->value ? "ON" : "OFF"); break;
    case SLOOP_TR_FILL_NEXT: snprintf(b, sizeof(b), "NEXT: PULSE"); break;
    case SLOOP_TR_LVL_DN:
    case SLOOP_TR_LVL_UP:
        snprintf(b, sizeof(b), "LVL: %s", k_lvl_name[(uint8_t)r->value & 3u]);
        break;
    case SLOOP_TR_RAT_DN:
    case SLOOP_TR_RAT_UP: snprintf(b, sizeof(b), "RAT: x%u", (unsigned)r->value + 1u); break;
    case SLOOP_TR_MIC_DN:
    case SLOOP_TR_MIC_UP: snprintf(b, sizeof(b), "MIC: %+d", (int)r->value); break;
    case SLOOP_TR_SPAN_DN:
    case SLOOP_TR_SPAN_UP: snprintf(b, sizeof(b), "SPAN: %u", (unsigned)r->value); break;
    case SLOOP_TR_SLIDE: snprintf(b, sizeof(b), "SLIDE: %s", r->value ? "ON" : "OFF"); break;
    case SLOOP_TR_ROT_DN: snprintf(b, sizeof(b), "ROTATE: LEFT"); break;
    case SLOOP_TR_ROT_UP: snprintf(b, sizeof(b), "ROTATE: RIGHT"); break;
    case SLOOP_TR_LEN_X2:
    case SLOOP_TR_LEN_D2: snprintf(b, sizeof(b), "LEN: %u", (unsigned)r->value); break;
    case SLOOP_TR_TRANS_DN: snprintf(b, sizeof(b), "TRANSPOSE: -1"); break;
    case SLOOP_TR_TRANS_UP: snprintf(b, sizeof(b), "TRANSPOSE: +1"); break;
    case SLOOP_TR_CLEAR: snprintf(b, sizeof(b), "CLEAR: T%u", (unsigned)track + 1u); break;
    case SLOOP_TR_REC_QNT: snprintf(b, sizeof(b), "QNT: %s", r->value ? "OFF" : "ON"); break;
    case SLOOP_TR_REC_MODE: snprintf(b, sizeof(b), "MODE: %s", r->value ? "TEMPO" : "FREE"); break;
    case SLOOP_TR_REC_START: snprintf(b, sizeof(b), "START: %s", r->value ? "COUNT" : "NOTE"); break;
    case SLOOP_TR_UNDO: snprintf(b, sizeof(b), "UNDO: OK"); break;
    case SLOOP_TR_REDO: snprintf(b, sizeof(b), "REDO: OK"); break;
    default: snprintf(b, sizeof(b), "%s", k_act_name[act]); break;
    }
    status_store(b, 1);
}

/* 动作的唯一实现（LVGL 回调、TAP、键盘调值都走这里）：读快照 → 计算 → 一条
 * EditCmd → 回读真值/seq_status → 记状态文本。不碰控件（由调用方 refresh 或下一拍
 * update 落地）。 */
static tr_result_t tr_action(uint8_t act)
{
    tr_result_t r = {.err = 0, .value = 0};
    if (act >= SLOOP_TR_ACT_COUNT) {
        r.err = SEQ_STATUS_INVALID;
        return r;
    }
    app_state_t st;
    state_get_copy(&st);
    uint8_t ft = 0, fs = 0;
    const uint8_t abs = focus_of(&st, &ft, &fs);
    const bool drum = ft == APP_STATE_TRACKS - 1u;
    const uint8_t track = st.core.selected_track < APP_STATE_TRACKS ? st.core.selected_track : 0u;

    edit_cmd_t cmd = {.type = CMD_NOTE_ON}; /* 占位；has_cmd 决定是否下发 */
    bool has_cmd = true;
    sloop_step_edit_t edit = {0};
    sloop_dstep_t dstep = {0};

    switch (act) {
    case SLOOP_TR_REC:
        cmd.type = CMD_REC_TOGGLE;
        break;
    case SLOOP_TR_FILL_HOLD:
        /* 电平取自 rec 投影（30ms 一拍；TAP 往返 ~50ms、物理点按 ≥100ms，足够）——
         * 不在这里重取 rec 快照，`sloop_seq_rec_read` 会消费一次性 take 结果。 */
        cmd.type = CMD_FILL_CONTROL;
        cmd.a = st.rec.fill_held ? 0u : 1u;
        break;
    case SLOOP_TR_FILL_NEXT:
        cmd.type = CMD_FILL_CONTROL;
        cmd.b = 1;
        break;
    case SLOOP_TR_LVL_DN:
    case SLOOP_TR_LVL_UP:
    case SLOOP_TR_RAT_DN:
    case SLOOP_TR_RAT_UP: {
        const bool is_lvl = act == SLOOP_TR_LVL_DN || act == SLOOP_TR_LVL_UP;
        const bool up = act == SLOOP_TR_LVL_UP || act == SLOOP_TR_RAT_UP;
        if (drum) {
            /* 鼓轨：选中 lane 的 2-bit 字段（原始 16-lane POD 读改写，一条命令）。 */
            const uint8_t lane = drum_lane();
            dstep = st.core.dstep_raw[fs];
            const uint8_t cur = is_lvl ? dstep_bits(dstep.lvl[lane / 4u], lane)
                                       : dstep_bits(dstep.rat[lane / 4u], lane);
            /* RAT 是 x1..x4 的循环（触摸 chip 只有前进，4 拍回到原点）。 */
            const uint8_t next = is_lvl ? lvl_step(cur, up) : (uint8_t)((cur + (up ? 1u : 3u)) & 3u);
            if (is_lvl) dstep_set_bits(dstep.lvl, lane, next);
            else dstep_set_bits(dstep.rat, lane, next);
            cmd.type = CMD_DRUM_STEP_WRITE;
            cmd.b = abs;
            cmd.p = &dstep;
            break;
        }
        const uint8_t cur = is_lvl ? (uint8_t)(st.core.step[ft][fs].lvl & 3u)
                                   : (uint8_t)(st.core.step[ft][fs].rat & 3u);
        const uint8_t next = is_lvl ? lvl_step(cur, up) : (uint8_t)((cur + (up ? 1u : 3u)) & 3u);
        edit.track = ft;
        edit.step = abs;
        edit.index = 0; /* 聚焦步的第一个音符（无音符光标，见页面头部注释/DI-30） */
        edit.op = is_lvl ? SEQ_STEP_LVL : SEQ_STEP_RAT;
        edit.value = (int16_t)next;
        cmd.type = CMD_STEP_EDIT;
        cmd.p = &edit;
        break;
    }
    case SLOOP_TR_MIC_DN:
    case SLOOP_TR_MIC_UP: {
        const int8_t cur = drum ? (int8_t)st.core.dstep[fs].micro
                                : (int8_t)st.core.step[ft][fs].micro;
        const int16_t v = (int16_t)((int32_t)cur + (act == SLOOP_TR_MIC_UP ? 1 : -1));
        cmd.type = CMD_SET_MICRO;
        cmd.a = ft;
        cmd.b = abs;
        cmd.v = (uint16_t)(int16_t)(v < -32 ? -32 : v > 31 ? 31 : v);
        break;
    }
    case SLOOP_TR_SPAN_DN:
    case SLOOP_TR_SPAN_UP: {
        if (drum) {
            r.err = SLOOP_TR_ERR_NA; /* 鼓步没有 span */
            has_cmd = false;
            break;
        }
        /* 循环基准 = 本轨 LEN：+1 到 LEN 后回 1，-1 到 1 前回 LEN（触摸只有循环）。 */
        uint8_t len = st.core.trk[ft].len;
        if (len < 1u) len = 1u;
        if (len > TR_STEPS_MAX) len = TR_STEPS_MAX;
        const uint8_t base = span_base(&st, ft, fs);
        const uint8_t want = act == SLOOP_TR_SPAN_UP
                                 ? (uint8_t)(base >= len ? 1u : base + 1u)
                                 : (uint8_t)(base <= 1u ? len : base - 1u);
        edit.track = ft;
        edit.step = abs;
        edit.op = SEQ_STEP_SPAN;
        edit.value = (int16_t)want;
        cmd.type = CMD_STEP_EDIT;
        cmd.p = &edit;
        break;
    }
    case SLOOP_TR_SLIDE: {
        if (drum) {
            r.err = SLOOP_TR_ERR_NA;
            has_cmd = false;
            break;
        }
        const uint8_t next = (st.core.step[ft][fs].flags & STEP_FLAG_SLIDE) ? 0u : 1u;
        edit.track = ft;
        edit.step = abs;
        edit.op = SEQ_STEP_SLIDE;
        edit.value = (int16_t)next;
        cmd.type = CMD_STEP_EDIT;
        cmd.p = &edit;
        break;
    }
    case SLOOP_TR_REC_QNT:
    case SLOOP_TR_REC_MODE:
    case SLOOP_TR_REC_START: {
        /* 整份三元组：另两项取 rec 投影（CMD_REC_CONFIG 成功后状态层同拍重取投影） */
        uint8_t q = st.rec.quant_off ? 1u : 0u, m = st.rec.tempo_mode ? 1u : 0u,
                c = st.rec.count_mode ? 1u : 0u;
        if (act == SLOOP_TR_REC_QNT) q ^= 1u;
        else if (act == SLOOP_TR_REC_MODE) m ^= 1u;
        else c ^= 1u;
        cmd.type = CMD_REC_CONFIG;
        cmd.a = q;
        cmd.b = m;
        cmd.v = c;
        break;
    }
    case SLOOP_TR_ROT_DN:
    case SLOOP_TR_ROT_UP:
    case SLOOP_TR_LEN_X2:
    case SLOOP_TR_LEN_D2:
    case SLOOP_TR_TRANS_DN:
    case SLOOP_TR_TRANS_UP:
    case SLOOP_TR_CLEAR:
    case SLOOP_TR_UNDO:
    case SLOOP_TR_REDO: {
        static const struct {
            uint8_t act;
            uint8_t op;
            int16_t arg;
        } k_map[] = {
            {SLOOP_TR_ROT_DN, SEQ_PATTERN_ROTATE, -1},
            {SLOOP_TR_ROT_UP, SEQ_PATTERN_ROTATE, 1},
            {SLOOP_TR_LEN_X2, SEQ_PATTERN_LENGTH, 1},
            {SLOOP_TR_LEN_D2, SEQ_PATTERN_LENGTH, -1},
            {SLOOP_TR_TRANS_DN, SEQ_PATTERN_TRANSPOSE, -1},
            {SLOOP_TR_TRANS_UP, SEQ_PATTERN_TRANSPOSE, 1},
            {SLOOP_TR_CLEAR, SEQ_PATTERN_CLEAR, 0},
            {SLOOP_TR_UNDO, SEQ_PATTERN_UNDO, 0},
            {SLOOP_TR_REDO, SEQ_PATTERN_REDO, 0},
        };
        for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
            if (k_map[i].act != act) continue;
            cmd.type = CMD_PATTERN;
            cmd.a = track;
            cmd.b = k_map[i].op;
            cmd.v = (uint16_t)(int16_t)k_map[i].arg;
            r.value = k_map[i].arg; /* LEN 由写后回读覆盖 */
            break;
        }
        break;
    }
    default: has_cmd = false; r.err = SEQ_STATUS_INVALID; break;
    }

    /* 触摸点字段 chip 也把键盘焦点落过去（REC/fill 不在焦点环内）。 */
    for (uint8_t i = 0; i < CHIP_COUNT; i++) {
        if (k_chips[i].act != act || k_chip_field[i] < 0) continue;
        portENTER_CRITICAL(&s_ui_mux);
        s_field = (uint8_t)k_chip_field[i];
        portEXIT_CRITICAL(&s_ui_mux);
        break;
    }

    if (!has_cmd) {
        status_note(act, &r, track);
        return r;
    }
    state_apply(&cmd);

    /* 写后回读：seq_status + 字段真值（失败时投影未变，回显即写前真值）。 */
    state_get_copy(&st);
    r.err = st.seq_status == SEQ_STATUS_OK ? 0u : (uint8_t)st.seq_status;
    (void)focus_of(&st, &ft, &fs); /* 写后仍按同一焦点读回（焦点不因命令改变） */
    switch (act) {
    case SLOOP_TR_FILL_HOLD:
        /* CMD_FILL_CONTROL 不重取 rec 投影（sloop_seq_rec_read 会消费一次性 take
         * 结果），所以回显用命令写入的电平；失败时报投影里的现值。 */
        r.value = r.err == 0u ? (int16_t)cmd.a : (int16_t)st.rec.fill_held;
        break;
    case SLOOP_TR_FILL_NEXT:
        /* 脉冲：回显 1（= 已下发一次翻转）；arm 的新电平由 NEXT chip 文本/`SEQ REC` 读。 */
        r.value = r.err == 0u ? 1 : 0;
        break;
    case SLOOP_TR_LVL_DN:
    case SLOOP_TR_LVL_UP:
    case SLOOP_TR_RAT_DN:
    case SLOOP_TR_RAT_UP: {
        const bool is_lvl = act == SLOOP_TR_LVL_DN || act == SLOOP_TR_LVL_UP;
        if (ft == APP_STATE_TRACKS - 1u) {
            const uint8_t lane = drum_lane();
            r.value = is_lvl ? (int16_t)dstep_bits(st.core.dstep_raw[fs].lvl[lane / 4u], lane)
                             : (int16_t)dstep_bits(st.core.dstep_raw[fs].rat[lane / 4u], lane);
        } else {
            r.value = is_lvl ? (int16_t)(st.core.step[ft][fs].lvl & 3u)
                             : (int16_t)(st.core.step[ft][fs].rat & 3u);
        }
        break;
    }
    case SLOOP_TR_MIC_DN:
    case SLOOP_TR_MIC_UP:
        r.value = ft == APP_STATE_TRACKS - 1u ? (int16_t)(int8_t)st.core.dstep[fs].micro
                                             : (int16_t)(int8_t)st.core.step[ft][fs].micro;
        break;
    case SLOOP_TR_SPAN_DN:
    case SLOOP_TR_SPAN_UP: r.value = (int16_t)span_base(&st, ft, fs); break;
    case SLOOP_TR_SLIDE:
        r.value = (st.core.step[ft][fs].flags & STEP_FLAG_SLIDE) ? 1 : 0;
        break;
    case SLOOP_TR_REC_QNT: r.value = st.rec.quant_off ? 1 : 0; break;
    case SLOOP_TR_REC_MODE: r.value = st.rec.tempo_mode ? 1 : 0; break;
    case SLOOP_TR_REC_START: r.value = st.rec.count_mode ? 1 : 0; break;
    case SLOOP_TR_LEN_X2:
    case SLOOP_TR_LEN_D2:
        r.value = (int16_t)(st.core.trk[track < APP_STATE_TRACKS ? track : 0u].len);
        break;
    default: break;
    }

    status_note(act, &r, track);
    return r;
}

static void chip_clicked_cb(lv_event_t *e)
{
    const uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    (void)tr_action(k_chips[idx].act);
    refresh();
}

/* 既有动作（PLAY/STOP/BPM）：控件回调与 TAP 的唯一共同路径。 */
static void transport_play(void)
{
    const edit_cmd_t cmd = {.type = CMD_PLAY};
    state_apply(&cmd);
    refresh();
}

static void transport_stop(void)
{
    const edit_cmd_t cmd = {.type = CMD_STOP};
    state_apply(&cmd);
    refresh();
}

static void transport_bpm_step(int delta)
{
    app_state_t st;
    state_get_copy(&st);
    int32_t bpm = (int32_t)st.core.bpm + delta;
    if (bpm < 0) bpm = 0; /* 范围钳位在 state_apply（40..240）内 */
    const edit_cmd_t cmd = {.type = CMD_SET_BPM, .v = (uint16_t)bpm};
    state_apply(&cmd);
    refresh();
}

static void play_clicked_cb(lv_event_t *e)
{
    (void)e;
    transport_play();
}

static void stop_clicked_cb(lv_event_t *e)
{
    (void)e;
    transport_stop();
}

static void bpm_clicked_cb(lv_event_t *e)
{
    transport_bpm_step((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---------------------------------------------------------------- 键盘入口 --- */

static uint8_t field_get(void)
{
    portENTER_CRITICAL(&s_ui_mux);
    const uint8_t f = s_field;
    portEXIT_CRITICAL(&s_ui_mux);
    return f < TR_F_COUNT ? f : 0u;
}

bool sloop_ui_transport_page_active(void)
{
    return sloop_ui_current_page() == APP_PAGE_TRANSPORT;
}

/* 当前 TRANSPORT 页状态行文本（console `TAP` 回显的 status="…" 与屏幕同源）。
 * 复制到本文件独立的 echo 缓冲：console 任务与 LVGL 任务交错写同一块会撕裂文本
 * （与 page_projects/presets 的 status_echo 同一纪律）。非 TRANSPORT 页返回 NULL。 */
const char *sloop_ui_transport_status_text(void)
{
    static char echo[40];
    if (sloop_ui_current_page() != APP_PAGE_TRANSPORT) return NULL;
    portENTER_CRITICAL(&s_ui_mux);
    snprintf(echo, sizeof(echo), "%s", s_status_text);
    portEXIT_CRITICAL(&s_ui_mux);
    return echo;
}

uint8_t sloop_ui_transport_tool_get(void)
{
    return field_get();
}

bool sloop_ui_transport_tool_set(uint8_t field)
{
    if (field >= TR_F_COUNT) return false;
    portENTER_CRITICAL(&s_ui_mux);
    s_field = field;
    portEXIT_CRITICAL(&s_ui_mux);
    return true;
}

void sloop_ui_transport_focus_move(int8_t delta)
{
    if (delta == 0) return;
    portENTER_CRITICAL(&s_ui_mux);
    s_field = (uint8_t)((s_field + TR_F_COUNT + delta) % TR_F_COUNT);
    portEXIT_CRITICAL(&s_ui_mux);
}

/* ↑/↓（delta>0 = 上调）：字段 → 动作。SLIDE 任一方向都是切换（上游布尔位）。 */
void sloop_ui_transport_adjust(int8_t delta)
{
    if (delta == 0 || !sloop_ui_transport_page_active()) return;
    static const uint8_t k_up[TR_F_COUNT] = {
        SLOOP_TR_LVL_UP, SLOOP_TR_RAT_UP, SLOOP_TR_MIC_UP, SLOOP_TR_SPAN_UP, SLOOP_TR_SLIDE,
        SLOOP_TR_ROT_UP, SLOOP_TR_LEN_X2, SLOOP_TR_TRANS_UP,
    };
    static const uint8_t k_dn[TR_F_COUNT] = {
        SLOOP_TR_LVL_DN, SLOOP_TR_RAT_DN, SLOOP_TR_MIC_DN, SLOOP_TR_SPAN_DN, SLOOP_TR_SLIDE,
        SLOOP_TR_ROT_DN, SLOOP_TR_LEN_D2, SLOOP_TR_TRANS_DN,
    };
    const uint8_t f = field_get();
    /* 输入任务上下文：不 refresh（不碰 LVGL），控件由 30ms UI 定时器刷新。 */
    (void)tr_action(delta > 0 ? k_up[f] : k_dn[f]);
}

/* ---------------------------------------------------------------- TAP --- */

/* 既有走带控件 + 工具表的命中；矩形全部取自建控件的同一组宏/同一张表。 */
typedef struct {
    int16_t x, y, w, h;
    uint8_t kind; /* sloop_tap_kind_t */
    int8_t v;
} transport_hit_t;

static const transport_hit_t k_hits[] = {
    {TRANSPORT_PLAY_X, TRANSPORT_PLAY_Y, TRANSPORT_PLAY_W, TRANSPORT_PLAY_H,
     SLOOP_TAP_TRANSPORT_PLAY, 0},
    {TRANSPORT_STOP_X, TRANSPORT_STOP_Y, TRANSPORT_STOP_W, TRANSPORT_STOP_H,
     SLOOP_TAP_TRANSPORT_STOP, 0},
    {TRANSPORT_BPM_MINUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W, TRANSPORT_BPM_H,
     SLOOP_TAP_TRANSPORT_BPM, -1},
    {TRANSPORT_BPM_PLUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W, TRANSPORT_BPM_H,
     SLOOP_TAP_TRANSPORT_BPM, 1},
};

bool page_transport_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (size_t i = 0; i < sizeof(k_hits) / sizeof(k_hits[0]); i++) {
        const transport_hit_t *hit = &k_hits[i];
        if (x < hit->x || x >= hit->x + hit->w || y < hit->y || y >= hit->y + hit->h) continue;
        switch (hit->kind) {
        case SLOOP_TAP_TRANSPORT_PLAY: transport_play(); break;
        case SLOOP_TAP_TRANSPORT_STOP: transport_stop(); break;
        case SLOOP_TAP_TRANSPORT_BPM: transport_bpm_step(hit->v); break;
        default: return false;
        }
        if (out != NULL) {
            out->kind = (sloop_tap_kind_t)hit->kind;
            out->v = hit->v;
        }
        return true;
    }
    for (uint8_t i = 0; i < CHIP_COUNT; i++) {
        const tr_chip_t *c = &k_chips[i];
        if (x < c->x || x >= c->x + c->w || y < c->y || y >= c->y + c->h) continue;
        const tr_result_t r = tr_action(c->act);
        refresh();
        if (out != NULL) {
            out->kind = SLOOP_TAP_TRANSPORT_TOOL;
            out->a = c->act;
            /* b：LVL/RAT 的实际落点（0 合成轨 CMD_STEP_EDIT / 1 鼓轨
             * CMD_DRUM_STEP_WRITE），console 据此回显真实命令名。 */
            out->b = (c->act == SLOOP_TR_LVL_DN || c->act == SLOOP_TR_LVL_UP ||
                      c->act == SLOOP_TR_RAT_DN || c->act == SLOOP_TR_RAT_UP)
                         ? (uint8_t)(focus_track_is_drum() ? 1u : 0u)
                         : 0u;
            out->v = r.value;
            out->d = (c->act == SLOOP_TR_LVL_UP || c->act == SLOOP_TR_RAT_UP ||
                      c->act == SLOOP_TR_MIC_UP || c->act == SLOOP_TR_SPAN_UP ||
                      c->act == SLOOP_TR_ROT_UP || c->act == SLOOP_TR_LEN_X2 ||
                      c->act == SLOOP_TR_TRANS_UP)
                         ? 1
                         : (c->act == SLOOP_TR_LVL_DN || c->act == SLOOP_TR_RAT_DN ||
                            c->act == SLOOP_TR_MIC_DN || c->act == SLOOP_TR_SPAN_DN ||
                            c->act == SLOOP_TR_ROT_DN || c->act == SLOOP_TR_LEN_D2 ||
                            c->act == SLOOP_TR_TRANS_DN)
                               ? -1
                               : 0;
            out->err = r.err;
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- 建页 --- */

static lv_obj_t *make_chip(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           lv_style_t *style, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *chip = lv_label_create(parent);
    if (chip == NULL) return NULL;
    lv_obj_set_clickable(chip, true);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_size(chip, w, h);
    lv_obj_add_style(chip, style, 0);
    lv_label_set_text(chip, text);
    if (cb != NULL) lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user);
    return chip;
}

lv_obj_t *page_transport_build(lv_obj_t *parent)
{
    styles_init();
    /* 懒建/组作用域重建：控件是全新的，复位脏检查缓存（局部焦点/状态文本保留）。 */
    s_bpm = -1;
    s_status_cache[0] = '\0';
    s_status_color_shown = 0xFFFFFFFFu;
    s_step_cache[0] = '\0';
    s_rec_cache[0] = '\0';
    s_take_txt[0] = '\0';
    s_fill_cache[0] = '\0';
    s_next_cache[0] = '\0';
    s_rec_style = 0xFF;
    s_focus_shown = -1;
    s_drum_shown = -1;
    s_clear_track = -1;
    s_fill_shown = -1;
    s_next_shown = -1;
    s_slide_shown = -1;
    memset(s_cfg_cache, 0, sizeof(s_cfg_cache));
    memset(s_chip, 0, sizeof(s_chip));
    {
        static bool first = true;
        if (first) {
            first = false;
            status_store("TOOLS: FOCUSED STEP", 0);
        }
    }

    if (make_chip(parent, TRANSPORT_PLAY_X, TRANSPORT_PLAY_Y, TRANSPORT_PLAY_W,
                  TRANSPORT_PLAY_H, &s_st_big_play, "PLAY", play_clicked_cb, NULL) == NULL) {
        return NULL;
    }
    if (make_chip(parent, TRANSPORT_STOP_X, TRANSPORT_STOP_Y, TRANSPORT_STOP_W,
                  TRANSPORT_STOP_H, &s_st_big_stop, "STOP", stop_clicked_cb, NULL) == NULL) {
        return NULL;
    }

    s_status_label = lv_label_create(parent);
    UI_BUILD_CHECK(s_status_label);
    lv_label_set_text(s_status_label, "STOPPED");
    lv_obj_set_width(s_status_label, 760);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(s_status_label, 48, 264);

    s_step_label = lv_label_create(parent);
    UI_BUILD_CHECK(s_step_label);
    lv_label_set_text(s_step_label, "TRACK 1   STEP 0 / 16");
    lv_obj_set_width(s_step_label, 720);
    lv_obj_set_style_text_font(s_step_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_step_label, lv_color_hex(ui_track_color(0)), 0);
    lv_obj_set_pos(s_step_label, 48, 320);

    lv_obj_t *bpm_caption = lv_label_create(parent);
    UI_BUILD_CHECK(bpm_caption);
    lv_label_set_text(bpm_caption, "BPM");
    lv_obj_set_style_text_font(bpm_caption, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bpm_caption, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_pos(bpm_caption, 900, 40);

    s_bpm_value = lv_label_create(parent);
    UI_BUILD_CHECK(s_bpm_value);
    lv_label_set_text(s_bpm_value, "0");
    lv_obj_set_pos(s_bpm_value, 880, 72);
    lv_obj_set_size(s_bpm_value, 140, 40);
    lv_obj_set_style_text_align(s_bpm_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_bpm_value, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_bpm_value, lv_color_hex(UI_TEXT), 0);

    if (make_chip(parent, TRANSPORT_BPM_MINUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W,
                  TRANSPORT_BPM_H, &s_st_big_bpm, "-", bpm_clicked_cb,
                  (void *)(intptr_t)-1) == NULL) {
        return NULL;
    }
    if (make_chip(parent, TRANSPORT_BPM_PLUS_X, TRANSPORT_BPM_Y, TRANSPORT_BPM_W,
                  TRANSPORT_BPM_H, &s_st_big_bpm, "+", bpm_clicked_cb,
                  (void *)(intptr_t)1) == NULL) {
        return NULL;
    }

    /* 工具 chip：几何/文本/动作全部来自 k_chips（与 TAP 命中同源）。 */
    for (uint8_t i = 0; i < CHIP_COUNT; i++) {
        const tr_chip_t *c = &k_chips[i];
        s_chip[i] = make_chip(parent, c->x, c->y, c->w, c->h, &s_st_chip, c->text,
                              chip_clicked_cb, (void *)(uintptr_t)i);
        UI_BUILD_CHECK(s_chip[i]);
    }
    return parent;
}

/* ---------------------------------------------------------------- 刷新 --- */

/* 文本脏检查：变了才写（cache 由本页独占）。 */
static bool text_changed(char *cache, size_t n, const char *buf)
{
    if (strcmp(cache, buf) == 0) return false;
    snprintf(cache, n, "%s", buf);
    return true;
}

static lv_style_t *rec_style_of(uint8_t state)
{
    switch (state) {
    case SEQ_REC_ARMED: return &s_st_chip_accent;
    case SEQ_REC_RECORDING: return &s_st_chip_rec;
    case SEQ_REC_COUNT_IN: return &s_st_chip_count;
    case SEQ_REC_FREE_TAKE: return &s_st_chip_free;
    default: return NULL;
    }
}

static void chip_style_set(uint8_t idx, lv_style_t *old_st, lv_style_t *new_st)
{
    if (s_chip[idx] == NULL || old_st == new_st) return;
    if (old_st != NULL) lv_obj_remove_style(s_chip[idx], old_st, 0);
    if (new_st != NULL) lv_obj_add_style(s_chip[idx], new_st, 0);
}

/* 聚焦步字段摘要：**先报工具的目标**（`T<焦点轨+1> S<绝对步>`），再报字段值。
 * 第一行（选中轨的播放头）与本行可以指向不同的轨：PLAYHEAD 属选中轨、工具写焦点轨。 */
static void build_detail(const app_state_t *st, uint8_t ft, uint8_t fs, uint8_t abs, char *b,
                         size_t n)
{
    static const char *const lvl_tok[4] = {"N", "G", "S", "H"};
    if (ft == APP_STATE_TRACKS - 1u) {
        const uint8_t lane = drum_lane();
        const sloop_dstep_t *d = &st->core.dstep_raw[fs];
        snprintf(b, n, "T%u S%u  DRM L%u  LVL %s  RAT x%u  MIC %+d", (unsigned)(ft + 1u),
                 (unsigned)(abs + 1u), (unsigned)(lane + 1u),
                 lvl_tok[dstep_bits(d->lvl[lane / 4u], lane)],
                 (unsigned)dstep_bits(d->rat[lane / 4u], lane) + 1u,
                 (int)(int8_t)st->core.dstep[fs].micro);
        return;
    }
    snprintf(b, n, "T%u S%u  LVL %s  RAT x%u  MIC %+d  SPAN %u  SLD %s", (unsigned)(ft + 1u),
             (unsigned)(abs + 1u), lvl_tok[st->core.step[ft][fs].lvl & 3u],
             (unsigned)(st->core.step[ft][fs].rat & 3u) + 1u,
             (int)(int8_t)st->core.step[ft][fs].micro, (unsigned)span_base(st, ft, fs),
             (st->core.step[ft][fs].flags & STEP_FLAG_SLIDE) ? "ON" : "-");
}

void page_transport_update(const app_state_t *st)
{
    /* 走带状态 + 上一次工具动作（单行；失败红字/N/A 黄字由状态色表达）。 */
    {
        char tool[40];
        uint8_t tool_color;
        portENTER_CRITICAL(&s_ui_mux);
        snprintf(tool, sizeof(tool), "%s", s_status_text);
        tool_color = s_status_color;
        portEXIT_CRITICAL(&s_ui_mux);
        char buf[72];
        snprintf(buf, sizeof(buf), "%s   %s", st->core.playing ? "PLAYING" : "STOPPED", tool);
        uint32_t color = st->core.playing ? UI_TEXT : UI_TEXT_DIM;
        if (tool_color == 1u) color = UI_TEXT; /* 有工具动作/结果：保持主文本亮度 */
        else if (tool_color == 2u) color = UI_ERROR;
        else if (tool_color == 3u) color = UI_ACCENT;
        if (text_changed(s_status_cache, sizeof(s_status_cache), buf) ||
            color != s_status_color_shown) {
            s_status_color_shown = color;
            lv_label_set_text(s_status_label, buf);
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
        }
    }

    if ((int16_t)st->core.bpm != s_bpm) {
        s_bpm = (int16_t)st->core.bpm;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)s_bpm);
        lv_label_set_text(s_bpm_value, buf);
    }

    const uint8_t k = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0u;
    const uint8_t pos = st->core.step_pos[k];
    const uint8_t len = st->core.trk[k].len;
    uint8_t ft = 0, fs = 0;
    const uint8_t abs = focus_of(st, &ft, &fs);

    {
        char detail[72];
        build_detail(st, ft, fs, abs, detail, sizeof(detail));
        char buf[112];
        snprintf(buf, sizeof(buf), "TRACK %u   STEP %u / %u\n%s", (unsigned)(k + 1u),
                 (unsigned)pos, (unsigned)len, detail);
        if (text_changed(s_step_cache, sizeof(s_step_cache), buf)) {
            lv_label_set_text(s_step_label, buf);
            /* 颜色 = **焦点轨**（工具写的那条）：与第二行的 `T<ft+1>` 同源，选中轨
             * 只在第一行的 TRACK 号 / 播放头里出现。 */
            lv_obj_set_style_text_color(s_step_label, lv_color_hex(ui_track_color(ft)), 0);
        }
    }

    /* REC 五态（文本 + 底色）。take 结果（一次性投影，读即消费）latch 在 REC chip
     * 上（`TAKE n BAR` / `TAKE DROPPED`，仅 OFF 时显示）——不占用工具状态行，避免
     * 与同一拍的动作结果互相覆盖；下一次 REC 动作（state != OFF）清 latch。 */
    if (st->rec.take_bars != 0u) {
        if (st->rec.take_bars == 0xFFu) snprintf(s_take_txt, sizeof(s_take_txt), "TAKE DROP");
        else snprintf(s_take_txt, sizeof(s_take_txt), "TAKE %u BAR", (unsigned)st->rec.take_bars);
        s_rec_cache[0] = '\0'; /* 强制重写文本 */
    }
    if (st->rec.state != SEQ_REC_OFF) s_take_txt[0] = '\0';
    {
        char buf[16];
        if (st->rec.state == SEQ_REC_OFF && s_take_txt[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", s_take_txt);
        } else {
            switch (st->rec.state) {
            case SEQ_REC_ARMED: snprintf(buf, sizeof(buf), "REC ARM"); break;
            case SEQ_REC_RECORDING:
                snprintf(buf, sizeof(buf), "REC ON T%u",
                         (unsigned)(st->rec.track % APP_STATE_TRACKS) + 1u);
                break;
            case SEQ_REC_COUNT_IN:
                snprintf(buf, sizeof(buf), "REC CNT T%u",
                         (unsigned)(st->rec.track % APP_STATE_TRACKS) + 1u);
                break;
            case SEQ_REC_FREE_TAKE: snprintf(buf, sizeof(buf), "FREE TAKE"); break;
            default: snprintf(buf, sizeof(buf), "REC OFF"); break;
            }
        }
        if (text_changed(s_rec_cache, sizeof(s_rec_cache), buf) && s_chip[CHIP_REC] != NULL) {
            lv_label_set_text(s_chip[CHIP_REC], buf);
        }
        const uint8_t state = st->rec.state <= SEQ_REC_FREE_TAKE ? st->rec.state : 0u;
        if (state != s_rec_style) {
            lv_style_t *old_st = s_rec_style < 5u ? rec_style_of(s_rec_style) : NULL;
            lv_style_t *new_st = rec_style_of(state);
            chip_style_set(CHIP_REC, old_st, new_st);
            s_rec_style = state;
        }
    }
    {
        const char *fill_txt = st->rec.fill_held ? "FILL ON" : "FILL";
        if (text_changed(s_fill_cache, sizeof(s_fill_cache), fill_txt) &&
            s_chip[CHIP_FILL] != NULL) {
            lv_label_set_text(s_chip[CHIP_FILL], fill_txt);
        }
        /* FILL 的保持电平用强调底色表达（与键盘 `-` 的 held 同一变量）。 */
        const int8_t held = st->rec.fill_held ? 1 : 0;
        if (held != s_fill_shown) {
            chip_style_set(CHIP_FILL, s_fill_shown > 0 ? &s_st_chip_accent : NULL,
                           held > 0 ? &s_st_chip_accent : NULL);
            s_fill_shown = held;
        }
        const char *next_txt = st->rec.fill_arm ? "NEXT ARM" : "NEXT";
        if (text_changed(s_next_cache, sizeof(s_next_cache), next_txt) &&
            s_chip[CHIP_NEXT] != NULL) {
            lv_label_set_text(s_chip[CHIP_NEXT], next_txt);
        }
        const int8_t arm = st->rec.fill_arm ? 1 : 0;
        if (arm != s_next_shown) {
            chip_style_set(CHIP_NEXT, s_next_shown > 0 ? &s_st_chip_accent : NULL,
                           arm > 0 ? &s_st_chip_accent : NULL);
            s_next_shown = arm;
        }
    }

    /* CLEAR 的目标轨（pattern 工具作用于选中轨）。 */
    if ((int8_t)k != s_clear_track && s_chip[CHIP_CLEAR] != NULL) {
        s_clear_track = (int8_t)k;
        char buf[12];
        snprintf(buf, sizeof(buf), "CLEAR T%u", (unsigned)(k + 1u));
        lv_label_set_text(s_chip[CHIP_CLEAR], buf);
    }

    /* SLIDE chip 的开关底色（值本身在第二行的 SLD 字段）。 */
    {
        const int8_t slide = (st->core.step[ft][fs].flags & STEP_FLAG_SLIDE) && ft < 3u ? 1 : 0;
        if (slide != s_slide_shown) {
            chip_style_set(CHIP_SLIDE, s_slide_shown > 0 ? &s_st_chip_accent : NULL,
                           slide > 0 ? &s_st_chip_accent : NULL);
            s_slide_shown = slide;
        }
    }

    /* 鼓轨：SPAN/SLIDE 不适用 → 灰字（点按回 `N/A (DRUM)`，不发命令）。 */
    const int8_t drum = ft == APP_STATE_TRACKS - 1u ? 1 : 0;
    if (drum != s_drum_shown) {
        s_drum_shown = drum;
        const uint32_t col = drum ? 0x5A6068u : UI_TEXT; /* 灰 = 与 STEP 的 GHOST 同色 */
        for (uint8_t i = CHIP_SPAN; i <= CHIP_SLIDE; i++) {
            if (s_chip[i] != NULL) lv_obj_set_style_text_color(s_chip[i], lv_color_hex(col), 0);
        }
    }

    /* REC 配置三 chip：文本 = rec 投影的当前值（CMD_REC_CONFIG 成功后同拍刷新） */
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "QNT %s", st->rec.quant_off ? "OFF" : "ON");
        if (text_changed(s_cfg_cache[0], sizeof(s_cfg_cache[0]), buf) && s_chip[CHIP_QNT] != NULL)
            lv_label_set_text(s_chip[CHIP_QNT], buf);
        snprintf(buf, sizeof(buf), "%s", st->rec.tempo_mode ? "TEMPO" : "FREE");
        if (text_changed(s_cfg_cache[1], sizeof(s_cfg_cache[1]), buf) && s_chip[CHIP_MODE] != NULL)
            lv_label_set_text(s_chip[CHIP_MODE], buf);
        snprintf(buf, sizeof(buf), "%s", st->rec.count_mode ? "COUNT" : "NOTE");
        if (text_changed(s_cfg_cache[2], sizeof(s_cfg_cache[2]), buf) && s_chip[CHIP_START] != NULL)
            lv_label_set_text(s_chip[CHIP_START], buf);
    }

    /* 键盘工具焦点描边。 */
    const uint8_t field = field_get();
    if ((int8_t)field != s_focus_shown) {
        s_focus_shown = (int8_t)field;
        for (uint8_t i = 0; i < CHIP_COUNT; i++) {
            if (s_chip[i] == NULL || k_chip_field[i] < 0) continue;
            if ((uint8_t)k_chip_field[i] == field) {
                lv_obj_add_style(s_chip[i], &s_st_chip_focus, 0);
            } else {
                lv_obj_remove_style(s_chip[i], &s_st_chip_focus, 0);
            }
        }
    }
}
