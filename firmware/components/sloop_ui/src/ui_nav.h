/*
 * 分组导航的纯数据与几何（ADR-005「导航决策」；sloop_ui 内部头文件）。
 *
 * 组表 UI_GROUP_LIST 是导航的唯一来源：组顺序 = 上行组签顺序 = `TAB` 扁平轮转顺序，
 * 组内顺序 = 下行子签顺序。ui.c 由它生成 k_groups[]，host 单测（scripts/knob-test.sh）
 * 由同一张表校验顺序/成员/每页可达。两行页签的几何常量与静态断言也在这里（不依赖
 * LVGL），theme.h 包含本文件后沿用。
 */
#pragma once

#include "sloop_state.h"

#define UI_W 1280
#define UI_H 720

/* 组表：X(名, 成员页 id…)，成员 ≤ UI_GROUP_MAX（宏体内不放注释：每行都要续行）。
 * EDIT = 每轨 SOUND 页（页 id 沿用 APP_PAGE_ANALOG；P4-F Task 5 取代 ANALOG + FM6 两页，
 * APP_PAGE_FM6 是空位 id、不进组表）。5B-1：SOUND 仪表盘承载原 MOD/SCALE/VOICE 与
 * GLOBAL/FX 五张族页的全部参数 ⇒ 这五页退役，EDIT 与 MIX 各收成单页（MIX 只余 TOOLS；
 * P4-F Task 6 它是 MIX 的第 3 张子签）。子签行对单页组不画（ui.c）。 */
#define UI_GROUP_LIST(X)                                                                         \
    X(PLAY, APP_PAGE_PLAY)                                                                       \
    X(TRACKS, APP_PAGE_TRACKS)                                                                   \
    X(EDIT, APP_PAGE_ANALOG)                                                                     \
    X(MIX, APP_PAGE_TOOLS)                                                                       \
    X(SYNTH, APP_PAGE_SAMPLE, APP_PAGE_DSYN, APP_PAGE_FM6P)                                     \
    X(SEQ, APP_PAGE_STEP, APP_PAGE_TRANSPORT)                                                   \
    X(SONG, APP_PAGE_SONG)                                                                       \
    X(FILE, APP_PAGE_PROJECTS, APP_PAGE_PRESETS)                                                \
    X(SYS, APP_PAGE_SYSTEM)

/* 组数（由组表数出；sloop_ui.h 的公共常量 SLOOP_UI_GROUPS 在 theme.h 里与它静态比对）。 */
#define UI_NAV_X_COUNT(NAME, ...) 1 +
#define UI_GROUP_COUNT (0 + UI_GROUP_LIST(UI_NAV_X_COUNT) 0)

/* 两行页签：上行 = 组签 y∈[4,36)，下行 = 当前组的子签 y∈[38,68)（行高/行 y 在 theme.h）。 */
#define UI_TAB_X0  16
#define UI_TAB_GAP 8
#define UI_TAB_RIGHT_RESERVE 216
#define UI_GROUP_W 104
#define UI_SUB_W   176
#define UI_GROUP_MAX 5
_Static_assert(UI_TAB_X0 + UI_GROUP_MAX * (UI_SUB_W + UI_TAB_GAP) - UI_TAB_GAP <=
                   UI_W - UI_TAB_RIGHT_RESERVE,
               "子签槽位超出行宽（与右侧层提示重叠）");
_Static_assert(UI_TAB_X0 + (UI_GROUP_COUNT * (UI_GROUP_W + UI_TAB_GAP)) - UI_TAB_GAP <=
                   UI_W - UI_TAB_RIGHT_RESERVE,
               "组签槽位超出行宽（与右侧 SLOOP 标题重叠）");
