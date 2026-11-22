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
#include "sloop_ui.h"

#define UI_W 1280
#define UI_H 720
#define UI_TAB_BAR_H 72

/* 配色 */
#define UI_BG       0x101418 /* 全局背景（页面区） */
#define UI_PANEL    0x1B2026 /* 页签条/面板底 */
#define UI_PANEL_HI 0x242B33 /* 未选中页签/次级按钮/空步格 */
#define UI_PANEL_SEL 0x2C3540 /* 选中态的卡片底（比 UI_PANEL 亮一档） */
#define UI_TEXT     0xF2F5F8 /* 主文本 */
#define UI_TEXT_DIM 0x8A949E /* 次级文本 */
#define UI_ACCENT   0xE0B32A /* 选中页签/播放头（强调色） */
#define UI_ERROR    0xE0542A /* 页面构建失败的可见错误（仅失败路径出现） */

/* 两行页签（ADR-005「导航决策」）：上行 = 组签 y∈[4,36)，下行 = 当前组的子签
 * y∈[38,68)，两行都在 72px 页签条内（页面区起点仍是 UI_TAB_BAR_H=72）。组行最多
 * 6 个组签（PLAY/TRACKS/EDIT/SEQ/SONG/FILE，P4-C Task 11），168px 宽
 * （6×168+5×8=1,048，右端 1,064 < 上行 SLOOP 标题起点 ~1,191）；子行复用固定
 * UI_GROUP_MAX 个槽（176px），只显示当前组已注册页（EDIT 5 页是最大组，右端 928）。
 * 右侧 UI_TAB_RIGHT_RESERVE=220 给上行 SLOOP 标题与下行层键提示留位（提示盒
 * 起点 936 > 子行右端 928，宽度封顶 + 省略号截断，绝不压到子签）。 */
#define UI_TAB_X0  16
#define UI_TAB_GAP 8
#define UI_TAB_RIGHT_RESERVE 220
#define UI_GROUP_W 168
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

/* 四轨主题色（track 0..3 = 蓝/绿/黄/橙，同时是鼓轨第四行色） */
static inline uint32_t ui_track_color(uint8_t k)
{
    static const uint32_t colors[APP_STATE_TRACKS] = {0x3B7BD8, 0x3FA34D, 0xE0B32A, 0xE06A1B};
    return colors[k < APP_STATE_TRACKS ? k : 0];
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
void page_song_entry_move(int8_t delta);
void page_song_section_move(int8_t delta);
void page_song_load(void);
const char *page_song_status_text(void);

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

