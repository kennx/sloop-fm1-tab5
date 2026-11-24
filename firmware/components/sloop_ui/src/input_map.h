/*
 * InputManager 的鼓演奏键位表（无 LVGL/FreeRTOS）：选中鼓轨时，低行白键
 * z x c v b n m . → lane 0..7，高行白键 q w e r t y u i → lane 8..15
 * （spec §4.4 / ADR-006）。其它键（半音黑键、走带、导航、页面动作）在鼓轨上
 * 不发声、**不回落合成轨 0**。input.c 与 host 单测共用本表，映射只此一份。
 *
 * 键码 / input_action_t / 绑定表 k_bindings / 查表 input_map_find_binding（P4-F Task 6 从
 * input.c 原样移入）：仍是纯数据、无 LVGL/FreeRTOS，input.c 的输入任务与 host 单测
 * （scripts/knob-test.sh）共用同一张表。查表两遍——先找「所持层」的条目（GLO > SEL >
 * EDIT），没有再落基础层（layer 0）；因此同一 (键, 层) 在表里至多一条（host 单测断言）。
 * 层内功能覆盖同键的基础功能（如 EDIT 层 c/d/e/g 是音名键而不是演奏键），而方向键/走带
 * 这类未进层的键在任何层下都可用。
 */
#ifndef SLOOP_UI_INPUT_MAP_H
#define SLOOP_UI_INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "sloop_state.h" /* APP_LAYER_*（层键位掩码） */

#define INPUT_MAP_NO_LANE (-1)
#define INPUT_MAP_DRUM_TRACK 3u /* APP_STATE_TRACKS - 1：最后一条轨是鼓轨 */

static inline int8_t input_map_drum_lane(uint8_t code)
{
    static const uint8_t k_low[8] = { 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49 };
    static const uint8_t k_high[8] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28 };
    for (int8_t i = 0; i < 8; i++) {
        if (code == k_low[i]) return i;
        if (code == k_high[i]) return (int8_t)(8 + i);
    }
    return INPUT_MAP_NO_LANE;
}

/* 演奏键的路由（input.c 的 act_note 与 host 单测共用）：
 *  - 按下：选中鼓轨时鼓键 → DRUM、其它键 → IGNORE（不回落合成轨 0）；合成轨 → NOTE。
 *  - **释放永远回 NOTE**：note-off 用按下时记录的轨号，即使期间切到鼓轨也不能被
 *    鼓分支吞掉（否则音符挂住）。 */
enum { INPUT_ROUTE_NOTE = 0, INPUT_ROUTE_DRUM, INPUT_ROUTE_IGNORE };

static inline int input_map_key_route(uint8_t selected_track, uint8_t code, bool down)
{
    if (!down) return INPUT_ROUTE_NOTE;
    if (selected_track >= INPUT_MAP_DRUM_TRACK)
        return input_map_drum_lane(code) >= 0 ? INPUT_ROUTE_DRUM : INPUT_ROUTE_IGNORE;
    return INPUT_ROUTE_NOTE;
}

/* 键码（tab5_keyboard.c 的 k_key_names，row<<4|col） */
#define K_ESC   0x00
#define K_1     0x01
#define K_2     0x02
#define K_3     0x03
#define K_4     0x04
#define K_5     0x05
#define K_6     0x06
#define K_7     0x07
#define K_8     0x08
#define K_0     0x0A
#define K_MINUS 0x0B
#define K_PLUS  0x0C
#define K_TAB   0x20
#define K_Q     0x21
#define K_W     0x22
#define K_E     0x23
#define K_R     0x24
#define K_T     0x25
#define K_Y     0x26
#define K_U     0x27
#define K_I     0x28
#define K_SYM   0x30
#define K_AA    0x31
#define K_A     0x32
#define K_S     0x33
#define K_D     0x34
#define K_F     0x35
#define K_G     0x36
#define K_H     0x37
#define K_J     0x38
#define K_K     0x39
#define K_L     0x3A
#define K_UP    0x3B
#define K_CTRL  0x40
#define K_ALT   0x41
#define K_Z     0x42
#define K_X     0x43
#define K_C     0x44
#define K_V     0x45
#define K_B     0x46
#define K_N     0x47
#define K_M     0x48
#define K_COMMA 0x49 /* 键名表印 "."，DAW 低八度末键 */
#define K_LEFT  0x4A
#define K_DOWN  0x4B
#define K_RIGHT 0x4C
#define K_SPACE 0x4D
#define K_ENTER 0x3D /* 键名表第 3 行第 13 列（"enter"）：FILE 页 = LOAD */
#define K_9     0x09
#define K_SEMI  0x2B /* 键名表第 2 行第 12 列（";"） */

typedef enum {
    ACT_LAYER,       /* param=APP_LAYER_*，按住置位/松开清位 */
    ACT_NOTE,        /* param=相对 INPUT_BASE_NOTE 的半音偏移（鼓轨走 lane 表） */
    ACT_PLAY_TOGGLE, /* space：物理 PLAY 键（free take drop；升/降两沿） */
    ACT_REC,         /* 0：REC 切换（上游 rec_toggle） */
    ACT_FILL_HOLD,   /* -：FILL 保持（按下/松开两沿） */
    ACT_FILL_NEXT,   /* +：FILL NEXT BAR（按下翻转一次） */
    ACT_PAGE_PLAY,   /* esc */
    ACT_PAGE_NEXT,   /* tab */
    ACT_FOCUS_STEP,  /* 左右：param=±1（FILE 页改为选槽，见 ACT_FILE 路由） */
    ACT_FOCUS_TRACK, /* 上下：param=±1（只走三条合成轨） */
    ACT_FILE_LOAD,   /* enter：FILE 页 = LOAD 选中槽（其它页无动作） */
    ACT_SELECT,      /* SEL 层 1–4：param=轨号 */
    ACT_MUTE,        /* GLO 层 1–4：param=轨号 */
    ACT_SOLO,        /* GLO 层 5–8：param=轨号 */
    ACT_ERASE,       /* EDIT 层 a–g：param=音级（0..11） */
    /* EDIT 层步工具（P4-F Task 6）：param 未用；动作在 page_step.c 的 sloop_ui_step_tool */
    ACT_STEP_LOCK,
    ACT_STEP_UNLOCK,
    ACT_STEP_FILL,
} input_action_t;

typedef struct {
    uint8_t code;
    uint8_t layer; /* 所需层位（APP_LAYER_*）；0 = 基础层 */
    uint8_t action;
    int8_t param;
} key_binding_t;

static const key_binding_t k_bindings[] = {
    /* ---- 层内功能（优先于基础层）---- */
    {K_1, APP_LAYER_GLO, ACT_MUTE, 0},  {K_2, APP_LAYER_GLO, ACT_MUTE, 1},
    {K_3, APP_LAYER_GLO, ACT_MUTE, 2},  {K_4, APP_LAYER_GLO, ACT_MUTE, 3},
    {K_5, APP_LAYER_GLO, ACT_SOLO, 0},  {K_6, APP_LAYER_GLO, ACT_SOLO, 1},
    {K_7, APP_LAYER_GLO, ACT_SOLO, 2},  {K_8, APP_LAYER_GLO, ACT_SOLO, 3},
    {K_1, APP_LAYER_SEL, ACT_SELECT, 0}, {K_2, APP_LAYER_SEL, ACT_SELECT, 1},
    {K_3, APP_LAYER_SEL, ACT_SELECT, 2}, {K_4, APP_LAYER_SEL, ACT_SELECT, 3},
    /* EDIT 层 a–g = 音名（A/B/C/D/E/F/G），按音级擦除聚焦步里的同名音 */
    {K_A, APP_LAYER_EDIT, ACT_ERASE, 9},  {K_B, APP_LAYER_EDIT, ACT_ERASE, 11},
    {K_C, APP_LAYER_EDIT, ACT_ERASE, 0},  {K_D, APP_LAYER_EDIT, ACT_ERASE, 2},
    {K_E, APP_LAYER_EDIT, ACT_ERASE, 4},  {K_F, APP_LAYER_EDIT, ACT_ERASE, 5},
    {K_G, APP_LAYER_EDIT, ACT_ERASE, 7},
    /* EDIT 层步工具（P4-F Task 6，ADR-006；只在本层、基础层与其它层都空闲的键）：k = 把
     * 「焦点轨上最后碰过的参数」锁到聚焦步、l = 解锁、9 / ; = 聚焦步的 fill 条件循环
     * （sloop_ui_step_tool，作用于 STEP 页）。 */
    {K_K, APP_LAYER_EDIT, ACT_STEP_LOCK, 0},
    {K_L, APP_LAYER_EDIT, ACT_STEP_UNLOCK, 0},
    {K_9, APP_LAYER_EDIT, ACT_STEP_FILL, 0},
    {K_SEMI, APP_LAYER_EDIT, ACT_STEP_FILL, 0},

    /* ---- 层键（任何层下都可叠加/释放）---- */
    {K_SYM, 0, ACT_LAYER, APP_LAYER_FX},
    {K_AA, 0, ACT_LAYER, APP_LAYER_EDIT},
    {K_CTRL, 0, ACT_LAYER, APP_LAYER_SEL},
    {K_ALT, 0, ACT_LAYER, APP_LAYER_GLO},

    /* ---- 演奏：低八度行 C3..C4 ---- */
    {K_Z, 0, ACT_NOTE, 0},      {K_S, 0, ACT_NOTE, 1},  {K_X, 0, ACT_NOTE, 2},
    {K_D, 0, ACT_NOTE, 3},      {K_C, 0, ACT_NOTE, 4},  {K_V, 0, ACT_NOTE, 5},
    {K_G, 0, ACT_NOTE, 6},      {K_B, 0, ACT_NOTE, 7},  {K_H, 0, ACT_NOTE, 8},
    {K_N, 0, ACT_NOTE, 9},      {K_J, 0, ACT_NOTE, 10}, {K_M, 0, ACT_NOTE, 11},
    {K_COMMA, 0, ACT_NOTE, 12},
    /* ---- 演奏：高八度行 C4..B4 ---- */
    {K_Q, 0, ACT_NOTE, 12},     {K_2, 0, ACT_NOTE, 13}, {K_W, 0, ACT_NOTE, 14},
    {K_3, 0, ACT_NOTE, 15},     {K_E, 0, ACT_NOTE, 16}, {K_R, 0, ACT_NOTE, 17},
    {K_5, 0, ACT_NOTE, 18},     {K_T, 0, ACT_NOTE, 19}, {K_6, 0, ACT_NOTE, 20},
    {K_Y, 0, ACT_NOTE, 21},     {K_7, 0, ACT_NOTE, 22}, {K_U, 0, ACT_NOTE, 23},
    {K_I, 0, ACT_NOTE, 24},

    /* ---- 走带 / 页面 / 焦点 ---- */
    {K_SPACE, 0, ACT_PLAY_TOGGLE, 0},
    {K_0, 0, ACT_REC, 0},
    {K_MINUS, 0, ACT_FILL_HOLD, 0},
    {K_PLUS, 0, ACT_FILL_NEXT, 0},
    {K_ESC, 0, ACT_PAGE_PLAY, 0},
    {K_TAB, 0, ACT_PAGE_NEXT, 0},
    {K_LEFT, 0, ACT_FOCUS_STEP, -1},
    {K_RIGHT, 0, ACT_FOCUS_STEP, 1},
    {K_UP, 0, ACT_FOCUS_TRACK, -1},
    {K_DOWN, 0, ACT_FOCUS_TRACK, 1},
    {K_ENTER, 0, ACT_FILE_LOAD, 0},
};

#define INPUT_MAP_BINDING_COUNT (sizeof(k_bindings) / sizeof(k_bindings[0]))

static inline const key_binding_t *input_map_find_binding(uint8_t code, uint8_t layers)
{
    for (size_t i = 0; i < INPUT_MAP_BINDING_COUNT; i++) {
        const key_binding_t *b = &k_bindings[i];
        if (b->code == code && b->layer != 0 && (layers & b->layer) != 0) return b;
    }
    for (size_t i = 0; i < INPUT_MAP_BINDING_COUNT; i++) {
        const key_binding_t *b = &k_bindings[i];
        if (b->code == code && b->layer == 0) return b;
    }
    return NULL;
}

#endif
