/*
 * sloop_ui 主题与共享布局常量（组件内部头文件）
 *
 * 视觉规范取自阶段 3 spec §3.3：四轨主题色 蓝/绿/黄/橙、背景 0x101418、
 * 顶部页签 PLAY|TRACKS|STEP|TRANSPORT 常驻。横屏 1280×720：页签条占
 * y∈[0,72)，页面区 y∈[72,720)，各页对象坐标相对页面区左上角。
 *
 * 截屏像素抽检（scripts/screenshot.py --check）的锚点由这里的常量推导；
 * 改布局须同步更新验证锚点与本文件注释。
 */
#pragma once

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

/* 页签几何：4 × 176px，间距 8px，起点 x=16、y=10（条高 72）。注册页多于 4 时按
 * 可用宽度收窄（UI_TAB_RIGHT_RESERVE 给右侧 SLOOP 标题/层提示留位）：7 页时
 * 140px（7×140 + 6×8 = 1028，右端 1044）；9 页时 107px
 * （9×107 + 8×8 = 1027，右端 1043；右侧 127px 给 16pt 层提示，宽度封顶 +
 * 省略号截断）。10 页时只剩 95px < "TRANSPORT" 的 16pt 标签 101.6px——页签不够，
 * P4-B 加页前必须先做分组导航决策（ADR-005「导航信息架构」节）。 */
#define UI_TAB_X0  16
#define UI_TAB_W   176
#define UI_TAB_H   52
#define UI_TAB_GAP 8
#define UI_TAB_Y   10
#define UI_TAB_RIGHT_RESERVE 220

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

