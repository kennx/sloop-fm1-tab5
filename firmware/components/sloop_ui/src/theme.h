/*
 * sloop_ui 主题与共享布局常量（组件内部头文件）
 *
 * 视觉规范取自阶段 3 spec §3.3：四轨主题色 蓝/绿/黄/橙、背景 0x101418、
 * 顶部两行页签（组签 + 当前组子签）常驻。横屏 1280×720：页签条占
 * y∈[0,72)，页面区 y∈[72,720)，各页对象坐标相对页面区左上角。
 *
 * 截屏像素抽检（scripts/screenshot.py --check）的锚点由这里的常量推导；
 * 改布局须同步更新验证锚点与本文件注释。
 */
#pragma once

#include <stddef.h>
#include <stdio.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_store.h"
#include "sloop_ui.h"
#include "theme_derive.h"

#define UI_W 1280
#define UI_H 720
#define UI_TAB_BAR_H 72

/* 运行时配色（P4-E Task 5）：MONO 派生结果与旧编译期常量逐项相同。 */
const ui_theme_t *ui_theme(void);
#define UI_BG        (ui_theme()->bg)       /* 全局背景（页面区） */
#define UI_PANEL     (ui_theme()->panel)    /* 页签条/面板底 */
#define UI_PANEL_HI  (ui_theme()->panel_hi) /* 未选中页签/次级按钮/空步格 */
#define UI_PANEL_SEL (ui_theme()->panel_sel)/* 选中态的卡片底 */
#define UI_TEXT      (ui_theme()->text)     /* 主文本 */
#define UI_TEXT_DIM  (ui_theme()->text_dim) /* 次级文本 */
#define UI_ACCENT    (ui_theme()->accent)   /* 选中页签/播放头（强调色） */
#define UI_ERROR     (ui_theme()->error)    /* 可见错误（仅失败路径出现） */

/* 两行页签（ADR-005「导航决策」）：上行 = 组签 y∈[4,36)，下行 = 当前组的子签
 * y∈[38,68)，两行都在 72px 页签条内（页面区起点仍是 UI_TAB_BAR_H=72）。组行 8 个组签
 * （PLAY/TRACKS/EDIT/SYNTH/SEQ/SONG/FILE/SYS，P4-E Task 6），122px 宽
 * （8×122+7×8=1,032，右端 16+1,032=1,048 ≤ 1,280−216=1,064）；子行复用固定
 * UI_GROUP_MAX 个槽（176px），只显示当前组已注册页（EDIT 4 页是最大组，右端 744）。
 * 右侧 UI_TAB_RIGHT_RESERVE=216 给上行 SLOOP 标题与下行层键提示留位：组行右端
 * 1,048（≤1,064 = 1,280−216）；子行右端 928，提示盒从 936 起，宽度封顶 +
 * 省略号截断，绝不压到子签。 */
#define UI_TAB_X0  16
#define UI_TAB_GAP 8
#define UI_TAB_RIGHT_RESERVE 216
#define UI_GROUP_W 122
#define UI_GROUP_H 32
#define UI_GROUP_Y 4
#define UI_SUB_W   176
#define UI_SUB_H   30
#define UI_SUB_Y   38
/* 单组成员上限 = 子行可容纳的子签数：5 张（5×176+4×8=912，右端 928；
 * 层提示盒从 936 起）。组内加第 6 页须先拆分组的布局决策（ADR-005 §2）。 */
#define UI_GROUP_MAX 5
_Static_assert(UI_TAB_X0 + UI_GROUP_MAX * (UI_SUB_W + UI_TAB_GAP) - UI_TAB_GAP <=
                   UI_W - UI_TAB_RIGHT_RESERVE,
               "子签槽位超出行宽（与右侧层提示重叠）");
/* 组签槽位：宽度按组数取（§2 的 min(176, …) 结果），右端不得越过 reserve 线
 * （SLOOP 标题从 ~1,191 起；8 组时右端 1,048）。 */
_Static_assert(UI_TAB_X0 + (SLOOP_UI_GROUPS * (UI_GROUP_W + UI_TAB_GAP)) - UI_TAB_GAP <=
                   UI_W - UI_TAB_RIGHT_RESERVE,
               "组签槽位超出行宽（与右侧 SLOOP 标题重叠）");

/* 四轨主题色（track 0..3 = 蓝/绿/黄/橙，同时是鼓轨第四行色） */
static inline uint32_t ui_track_color(uint8_t k)
{
    return ui_theme()->track[k < APP_STATE_TRACKS ? k : 0];
}

/* 页面构建函数（懒建，ADR-008）：在 parent 内建本页控件，成功返回 parent；
 * 任一控件分配失败（LVGL 池耗尽）返回 NULL——由 ui.c 显示错误状态条并记 ERROR，
 * 绝不留静默空白页。每页用 UI_BUILD_CHECK() 逐个检查分配结果。
 * 同一页可能因失败后重试而再次调用：build 必须复位本页脏检查缓存。 */
lv_obj_t *page_play_build(lv_obj_t *parent);
void page_play_update(const app_state_t *st);
void page_play_flash(uint8_t note); /* 调用方已持 bsp_display_lock；PLAY 未建时不调用 */

lv_obj_t *page_tracks_build(lv_obj_t *parent);
void page_tracks_update(const app_state_t *st);

lv_obj_t *page_step_build(lv_obj_t *parent);
void page_step_update(const app_state_t *st);

lv_obj_t *page_transport_build(lv_obj_t *parent);
void page_transport_update(const app_state_t *st);

/* FILE 组两页（P4-B Task 12）：PROJECTS / PRESETS（组表 k_groups[] 的 FILE 成员）。
 * build/update 契约同上；slot_move/load 是 InputManager 的 FILE 页路由（←/→ 选槽、
 * ENTER = LOAD），在输入任务上下文调用、**不碰控件**（局部态由下一拍 update 渲染）。 */
lv_obj_t *page_projects_build(lv_obj_t *parent);
void page_projects_update(const app_state_t *st);
bool page_projects_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
void page_projects_slot_move(int8_t delta);
void page_projects_load(void);
const char *page_projects_status_text(void); /* console TAP 回显（状态行文本，同源） */

lv_obj_t *page_presets_build(lv_obj_t *parent);
void page_presets_update(const app_state_t *st);
bool page_presets_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
void page_presets_slot_move(int8_t delta);
void page_presets_load(void);
const char *page_presets_status_text(void);

/* SONG 编排页（P4-C Task 11，独立 SONG 组）：A–D section 卡 / 16 项歌曲顺序 /
 * bars·loop 配置 / STORE·LOAD·ERASE·JUMP·SONG·CHAIN·REC 控件。build/update 契约
 * 同上；entry_move/section_move/load 是 InputManager 的键盘路由（←/→ 选条目、
 * ↑/↓ 选 section、ENTER = LOAD），在输入任务上下文调用、**不碰控件**（描边由下一拍
 * update 渲染）。song_status_text 是 console `TAP` 回显的 `status="…"`（与屏幕同源）。 */
lv_obj_t *page_song_build(lv_obj_t *parent);
void page_song_update(const app_state_t *st);
bool page_song_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
const char *page_song_status_text(void); /* console TAP 回显（状态行文本，同源） */

/* SYSTEM 页状态行（P4-E Task 6）：屏幕与 TAP 回显共用这一映射。 */
typedef enum {
    UI_SYS_ACT_EDIT = 0,
    UI_SYS_ACT_SAVE,
    UI_SYS_ACT_SELFTEST,
    UI_SYS_ACT_NONE = 0xFF,
} ui_sys_act_t;

static inline const char *ui_sys_status_text(char *buf, size_t n, uint8_t act, uint8_t status,
                                             bool ready)
{
    if (act == UI_SYS_ACT_SELFTEST) {
        snprintf(buf, n, "SELFTEST: N/A");
        return buf;
    }
    if (act == UI_SYS_ACT_NONE) {
        if (n > 0) buf[0] = '\0';
        return buf;
    }
    switch (status) {
    case APP_SETTINGS_OK: snprintf(buf, n, "%s", act == UI_SYS_ACT_SAVE ? "SAVED" : "OK"); break;
    case APP_SETTINGS_INVALID: snprintf(buf, n, "N/A"); break;
    case APP_SETTINGS_BLOCKED: snprintf(buf, n, "%s", ready ? "RESET FIRST" : "NO STORAGE"); break;
    case APP_SETTINGS_ERROR: snprintf(buf, n, "%s", ready ? "SAVE ERROR" : "NO STORAGE"); break;
    default: if (n > 0) buf[0] = '\0'; break;
    }
    return buf;
}

static inline uint32_t ui_sys_status_color(uint8_t act, uint8_t status)
{
    if (act == UI_SYS_ACT_SELFTEST) return UI_ACCENT;
    if (act == UI_SYS_ACT_NONE || status == APP_SETTINGS_IDLE) return UI_TEXT_DIM;
    if (status == APP_SETTINGS_OK) return ui_track_color(1);
    return UI_ERROR;
}

/* FILE 页状态行的单一映射（屏幕文本与 console `TAP … status="…"` 回显都从这里出）：
 * act = 本页最后一次动作（0 SAVE / 1 LOAD / 2 ERASE / 其它 = 尚无动作）；
 * status = app_state_t.store_status；ready = sloop_store_ready()——status 3 同时涵盖
 * 「走带中」与「存储未就绪」（sloop_state.h 的契约），后者必须显示 NO STORAGE 而不是
 * STOP BEFORE …。返回 buf（静态调用的调用方负责缓冲区生命周期）。 */
static inline const char *ui_file_status_text(char *buf, size_t n, uint8_t act, uint8_t status,
                                              bool ready)
{
    static const char *const k_actions[] = {"SAVE", "LOAD", "ERASE"};
    if (act > 2) {
        snprintf(buf, n, "SELECT A SLOT");
        return buf;
    }
    switch (status) {
    case APP_STORE_STATUS_OK: snprintf(buf, n, "%s OK", k_actions[act]); break;
    case APP_STORE_STATUS_EMPTY: snprintf(buf, n, "EMPTY"); break;
    case APP_STORE_STATUS_PLAYING:
        if (!ready) snprintf(buf, n, "NO STORAGE");
        else snprintf(buf, n, "STOP BEFORE %s", k_actions[act]);
        break;
    case APP_STORE_STATUS_ERROR: snprintf(buf, n, "%s ERROR", k_actions[act]); break;
    case APP_STORE_STATUS_DRUM: snprintf(buf, n, "DRUM TRACK: NO SOUND"); break;
    default: snprintf(buf, n, "SELECT A SLOT"); break;
    }
    return buf;
}

/* 状态行颜色（同一映射的视觉侧）：OK 绿 / DRUM 强调黄 / 走带与失败红 / 其余灰。 */
static inline uint32_t ui_file_status_color(uint8_t act, uint8_t status, bool ready)
{
    if (act > 2) return UI_TEXT_DIM;
    switch (status) {
    case APP_STORE_STATUS_OK: return ui_track_color(1);
    case APP_STORE_STATUS_DRUM: return UI_ACCENT;
    case APP_STORE_STATUS_PLAYING: return UI_ERROR; /* 走带中或存储未就绪：都是红字 */
    case APP_STORE_STATUS_ERROR: return UI_ERROR;
    default: return UI_TEXT_DIM;
    }
}

/* SAMPLE 页槽管理行的状态行（P4-D Task 8；单一口径——屏幕文本与 console `TAP`
 * 回显都从这里出）。状态是**页面本地**结果，不写 app_state_t（与 FM6 bank 行的
 * NOT FM6 同口径）：
 *   SET        槽选中已把 SET 宏（P_E0 = base + k）写入选中轨 → `SET U<n>`（slot = 0..3）
 *   ERASE_OK   CMD_SAMPLE_ERASE 成功 → `ERASE OK`
 *   NOT_USR    页面拒绝：当前 SET 不在 USR1..USR4，或选中轨引擎 ≠ SAMPLE → `NOT USR`
 *              （**未下发命令**）
 *   PLAYING    走带中（synth_status = BLOCKED）→ `STOP BEFORE ERASE`
 *   NO_STORAGE 存储未就绪 → `NO STORAGE`
 *   ERROR      其它失败（含诊断冻结）→ `ERASE ERROR`
 *   NONE       尚无动作 → 空串（不显示过期结果）
 * 「槽选中」与「擦除」是两种动作，故文案分开（`SET U<n>` 不是存储写成功；
 * `ERASE OK` 与 FILE/BANK 行对 ERASE 动作的文案一致）。 */
typedef enum {
    UI_SAMPLE_STATUS_NONE = 0,
    UI_SAMPLE_STATUS_SET,
    UI_SAMPLE_STATUS_ERASE_OK,
    UI_SAMPLE_STATUS_NOT_USR,
    UI_SAMPLE_STATUS_PLAYING,
    UI_SAMPLE_STATUS_NO_STORAGE,
    UI_SAMPLE_STATUS_ERROR,
} ui_sample_status_t;

/* slot 只对 `SET U<n>` 有意义（0..3；越界防御性显示 `SET U-`）。 */
static inline const char *ui_sample_status_text(char *buf, size_t n, uint8_t status, uint8_t slot)
{
    switch (status) {
    case UI_SAMPLE_STATUS_SET:
        if (slot < SLOOP_STORE_SAMPLES) snprintf(buf, n, "SET U%u", (unsigned)(slot + 1u));
        else snprintf(buf, n, "SET U-");
        break;
    case UI_SAMPLE_STATUS_ERASE_OK: snprintf(buf, n, "ERASE OK"); break;
    case UI_SAMPLE_STATUS_NOT_USR: snprintf(buf, n, "NOT USR"); break;
    case UI_SAMPLE_STATUS_PLAYING: snprintf(buf, n, "STOP BEFORE ERASE"); break;
    case UI_SAMPLE_STATUS_NO_STORAGE: snprintf(buf, n, "NO STORAGE"); break;
    case UI_SAMPLE_STATUS_ERROR: snprintf(buf, n, "ERASE ERROR"); break;
    default: if (n > 0) buf[0] = '\0'; break;
    }
    return buf;
}

/* 状态行颜色（同一映射的视觉侧）：SET/ERASE OK 绿 / NOT USR 强调黄 / 走带与失败红 /
 * 其余灰。 */
static inline uint32_t ui_sample_status_color(uint8_t status)
{
    switch (status) {
    case UI_SAMPLE_STATUS_SET:
    case UI_SAMPLE_STATUS_ERASE_OK: return ui_track_color(1);
    case UI_SAMPLE_STATUS_NOT_USR: return UI_ACCENT;
    case UI_SAMPLE_STATUS_PLAYING:
    case UI_SAMPLE_STATUS_NO_STORAGE:
    case UI_SAMPLE_STATUS_ERROR: return UI_ERROR;
    default: return UI_TEXT_DIM;
    }
}

/* DSYN 页状态行（P4-D Task 9；屏幕文本与 console `TAP` 回显都从这里出）。act = 本页
 * 最后一次动作（页内局部态，不写 app_state_t），status = `app_state_t.synth_status`，
 * dirty = `app_state_t.dsyn.dirty`（RAM 的 SYN kit 与最近一次成功写盘不一致）：
 *   act 0 SET      → `EDIT OK`（写值已落地；CRUSH 也走 SET）
 *   act 1 STORE    → `STORE OK`（dirty 随之归 0）
 *   act 2 KIT      → `KIT LOADED`（工厂 kit 整体覆盖：CMD_DSYN_LOAD_KIT）
 *   act 3 AUDITION → `AUDITION`（只写试听队列，不改数据）
 * 尚无动作：dirty 时 `DIRTY`，否则空串。失败按 synth_status 给固定令牌：`INVALID` /
 * `FULL` / `STOP BEFORE STORE`（BLOCKED = 走带中，DSYN 只有 STORE 受走带门控）/
 * `FROZEN` / `LOCKED` / `STORE ERROR`，存储未就绪的 ERROR 显示 `NO STORAGE`。
 * 有未保存修改时**不论成功或失败**都在文本后追加 ` DIRTY`（`EDIT OK DIRTY`、
 * `STOP BEFORE STORE DIRTY`）——脏位不能被某个分支静默吞掉。 */
typedef enum {
    UI_DSYN_ACT_SET = 0,
    UI_DSYN_ACT_STORE,
    UI_DSYN_ACT_KIT,
    UI_DSYN_ACT_AUDITION,
} ui_dsyn_act_t;

static inline const char *ui_dsyn_status_text(char *buf, size_t n, uint8_t act, uint8_t status,
                                              bool dirty, bool ready)
{
    const char *mark = dirty ? " DIRTY" : "";
    if (act > UI_DSYN_ACT_AUDITION) {
        snprintf(buf, n, "%s", dirty ? "DIRTY" : "");
        return buf;
    }
    switch (status) {
    case APP_SYNTH_STATUS_OK:
        if (act == UI_DSYN_ACT_STORE) snprintf(buf, n, "STORE OK%s", mark);
        else if (act == UI_DSYN_ACT_KIT) snprintf(buf, n, "KIT LOADED%s", mark);
        else if (act == UI_DSYN_ACT_AUDITION) snprintf(buf, n, "AUDITION%s", mark);
        else snprintf(buf, n, "EDIT OK%s", mark);
        break;
    case APP_SYNTH_STATUS_INVALID: snprintf(buf, n, "INVALID%s", mark); break;
    case APP_SYNTH_STATUS_FULL: snprintf(buf, n, "FULL%s", mark); break;
    case APP_SYNTH_STATUS_BLOCKED: snprintf(buf, n, "STOP BEFORE STORE%s", mark); break;
    case APP_SYNTH_STATUS_FROZEN: snprintf(buf, n, "FROZEN%s", mark); break;
    case APP_SYNTH_STATUS_LOCKED: snprintf(buf, n, "LOCKED%s", mark); break;
    case APP_SYNTH_STATUS_ERROR:
        snprintf(buf, n, "%s%s", ready ? "STORE ERROR" : "NO STORAGE", mark);
        break;
    default: snprintf(buf, n, "%s", dirty ? "DIRTY" : ""); break;
    }
    return buf;
}

/* 状态行颜色（同一映射的视觉侧）：成功绿 / 失败红 / 仅 DIRTY 时强调黄 / 其余灰。 */
static inline uint32_t ui_dsyn_status_color(uint8_t act, uint8_t status, bool dirty)
{
    if (act <= UI_DSYN_ACT_AUDITION && status == APP_SYNTH_STATUS_OK) return ui_track_color(1);
    switch (status) {
    case APP_SYNTH_STATUS_INVALID:
    case APP_SYNTH_STATUS_FULL:
    case APP_SYNTH_STATUS_BLOCKED:
    case APP_SYNTH_STATUS_FROZEN:
    case APP_SYNTH_STATUS_LOCKED:
    case APP_SYNTH_STATUS_ERROR: return UI_ERROR;
    default: return dirty ? UI_ACCENT : UI_TEXT_DIM;
    }
}

/* FM6P 页状态行（P4-D Task 10；屏幕文本与 console `TAP` 回显都从这里出）。
 * act = 本页最后一次动作（页内局部态，不写 app_state_t）：
 *   SAVE  → 存储结果（复用 ui_file_status_text 的 act 0 文案；本页**可达**的只有
 *           `SAVE OK` / `STOP BEFORE SAVE` / `NO STORAGE` / `SAVE ERROR`——
 *           `DRUM TRACK: NO SOUND` 走不到，鼓轨恒被锁定分支先拦成 `NOT FM6`）
 *   FIELD → synth_status：`EDIT OK` / `INVALID` / `FULL` / `BLOCKED` / `FROZEN` /
 *           `LOCKED` / `EDIT ERROR`（存储未就绪时 `NO STORAGE`）
 *   其它（尚无动作）→ `ALG MACRO n`（P_E0 ≠ 0，宏在覆盖 patch 的算法）或空串
 * **锁定优先**（选中轨不是 FM6，含鼓轨）：任何动作结果都不显示，固定 `NOT FM6`。
 * **ALG 提示优先级**：失败 > `ALG MACRO n` > 成功结果——覆盖警告是持续事实，成功的
 * `EDIT OK` 是瞬时的；编辑 ALG 成功后适配层清 P_E0，提示自然消失（spec §4.1）。
 * macro = `P_E0` 的 1..32（0 = PAT，不提示）。 */
typedef enum {
    UI_FM6P_ACT_SAVE = 0,
    UI_FM6P_ACT_FIELD = 1,
} ui_fm6p_act_t;
#define UI_FM6P_ACT_NONE 0xFFu

static inline const char *ui_fm6p_status_text(char *buf, size_t n, uint8_t act,
                                              uint8_t synth_status, uint8_t store_status,
                                              bool ready, bool locked, uint8_t macro)
{
    if (locked) {
        snprintf(buf, n, "NOT FM6");
        return buf;
    }
    if (act == UI_FM6P_ACT_SAVE) return ui_file_status_text(buf, n, 0, store_status, ready);
    if (act == UI_FM6P_ACT_FIELD) {
        switch (synth_status) {
        case APP_SYNTH_STATUS_OK:
            if (macro != 0) snprintf(buf, n, "ALG MACRO %u", (unsigned)macro);
            else snprintf(buf, n, "EDIT OK");
            break;
        case APP_SYNTH_STATUS_INVALID: snprintf(buf, n, "INVALID"); break;
        case APP_SYNTH_STATUS_FULL: snprintf(buf, n, "FULL"); break;
        case APP_SYNTH_STATUS_BLOCKED: snprintf(buf, n, "BLOCKED"); break;
        case APP_SYNTH_STATUS_FROZEN: snprintf(buf, n, "FROZEN"); break;
        case APP_SYNTH_STATUS_LOCKED: snprintf(buf, n, "LOCKED"); break;
        case APP_SYNTH_STATUS_ERROR:
            snprintf(buf, n, "%s", ready ? "EDIT ERROR" : "NO STORAGE");
            break;
        default:
            /* 尚无成功/失败分类（IDLE）：同「无动作」分支，只显示宏提示 */
            if (macro != 0) snprintf(buf, n, "ALG MACRO %u", (unsigned)macro);
            else if (n > 0) buf[0] = '\0';
            break;
        }
        return buf;
    }
    if (macro != 0) snprintf(buf, n, "ALG MACRO %u", (unsigned)macro);
    else if (n > 0) buf[0] = '\0';
    return buf;
}

/* 状态行颜色（同一映射的视觉侧）：锁定/失败红 / SAVE 与 FILE 页同色 /
 * 成功绿 / ALG 提示强调黄 / 其余灰。 */
static inline uint32_t ui_fm6p_status_color(uint8_t act, uint8_t synth_status,
                                            uint8_t store_status, bool ready, bool locked,
                                            uint8_t macro)
{
    if (locked) return UI_ERROR;
    if (act == UI_FM6P_ACT_SAVE) return ui_file_status_color(0, store_status, ready);
    if (act == UI_FM6P_ACT_FIELD) {
        if (synth_status == APP_SYNTH_STATUS_OK) return macro != 0 ? UI_ACCENT : ui_track_color(1);
        return UI_ERROR;
    }
    return macro != 0 ? UI_ACCENT : UI_TEXT_DIM;
}

/* 构建期分配检查：NULL 即中止本页构建（半成品容器由 ui.c 删除回收） */
#define UI_BUILD_CHECK(obj) do { if ((obj) == NULL) return NULL; } while (0)

/* hit-test（console `TAP` 注入）：坐标为页面区相对坐标（调用方已扣 UI_TAB_BAR_H）。
 * 命中：调用与控件事件回调**相同**的动作函数并填 out，返回 true；未命中返回 false。
 * 调用方（ui.c）已持 bsp_display_lock，动作里的控件写/立即刷新都在该锁内。
 * 矩形几何与建控件用同一组布局宏，不得另抄一份数字。 */
bool page_tracks_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
bool page_step_tap(int16_t x, int16_t y, sloop_tap_result_t *out);
bool page_transport_tap(int16_t x, int16_t y, sloop_tap_result_t *out);

/* InputManager 任务（input.c）：建互斥并启动轮询任务（周期取 tick 地板，
 * 100Hz 下 10ms）；由 sloop_ui_start() 在 LVGL 锁外调用（键盘驱动须已初始化）。 */
esp_err_t input_start(void);

/* 输入静默（诊断期，sloop_ui_quiesce 调用）：
 *  - begin：置丢弃标志，并等 input 任务回到「批次之间」确认（最多 timeout_ms；
 *    超时返回 false——在途批次可能仍未收尾）。无 input 任务时立即返回 true。
 *  - end：清标志与计数，唤醒 input 任务并返回本轮丢弃的事件数（诊断日志用）。
 *  - dropped：KEY STATS 回读（诊断期丢弃计数）。 */
bool input_quiesce_begin(uint32_t timeout_ms);
uint32_t input_quiesce_end(void);
uint32_t input_quiesce_dropped(void);

