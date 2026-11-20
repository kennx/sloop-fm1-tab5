/*
 * sloop-fm1-tab5 — sloop_ui 公共接口（LVGL 四屏框架）
 *
 * 单容器屏 + 顶部页签 PLAY|TRACKS|STEP|TRANSPORT：页对象只创建一次，
 * 切页只改显隐（不销毁重建）。四页均已落地——PLAY/TRACKS/STEP/TRANSPORT
 * （page_play/page_tracks/page_step/page_transport）。
 *
 * 线程：所有 LVGL 调用在 bsp_display_lock 内（esp_lvgl_port 递归锁，与
 * display.c 同一纪律）；UI 定时器 30ms 运行在 LVGL 任务里（已持锁）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 创建页签 + 四屏对象并注册 30ms UI 定时器；须在 state_init() 与显示初始化之后调用。
 * 成功时接管控件树（清掉 bring-up 测试图）；失败时调用方回退 app_display_draw_test_pattern()。 */
esp_err_t sloop_ui_start(void);

/* 切页：0=PLAY 1=TRACKS 2=STEP 3=TRANSPORT（同 app_page_t）。
 * 可从任意任务调用（内部加锁）。返回：ESP_OK = 已落地；ESP_ERR_INVALID_ARG =
 * 非法页；ESP_ERR_INVALID_STATE = UI 未启动；ESP_ERR_TIMEOUT = LVGL 锁超时
 * （**未切页**，与成功明确区分，console `UI` 据此打印 `UI ERR lock_timeout`）。 */
esp_err_t sloop_ui_switch_page(uint8_t page);

/* 请求切页（异步）：只记目标页，30ms UI 定时器在 LVGL 上下文执行。可从任意任务
 * 调用、不取任何锁；输入层（esc/tab）用它避免在输入路径上等 LVGL 锁。
 * 越界忽略；连续请求保留最后一个。 */
void sloop_ui_request_page(uint8_t page);

/* 当前页；UI 未启动返回 0xFF。 */
uint8_t sloop_ui_current_page(void);

/* 音符触发指示（PLAY 页大号指示）：把音符交给 30ms UI 定时器点亮，120ms 后熄灭。
 * 契约：不取 LVGL/state/引擎锁——可从任意任务调用（输入任务、console 注入）；
 * 只保留最新一个音符（无队列、无阻塞），点亮最多延迟一拍 UI（30ms）。 */
void sloop_ui_flash_note(uint8_t note);

/* InputManager 入口（input.c）：code = tab5_keyboard 事件码（row<<4|col），
 * down = 按下/释放。物理键盘（input 任务 5ms 轮询）与 console `KEY` 注入共用；
 * 可从任意任务调用（内部互斥 + 状态层互斥），不得在 ISR 中调用。 */
void sloop_ui_input_key(uint8_t code, bool down);

/* 全停（console `KEY PANIC`）：释放输入层跟踪的所有已按下音符（逐条 note-off）、
 * 清 layer_held、撤销待执行切页。可从任意任务调用，不取 LVGL 锁。 */
void sloop_ui_input_panic(void);

/* 撤销待执行切页（清空异步切页邮箱）。console 同步切页（`UI`）与 `KEY PANIC`
 * 共用：避免「读当前页→请求同页」在切页中途读到旧值而漏撤销。不取 LVGL 锁。 */
void sloop_ui_cancel_pending_page(void);

/* TAP 注入（console `TAP <x> <y>`）：坐标 = 屏幕绝对坐标（含页签条，与触摸 indev
 * 同口径）。按当前页 hit-test 表命中矩形，执行与控件回调**相同的动作函数**
 * （EditCmd 层等价，见 ADR-005；不声称原始指针事件等价）。
 * 返回值区分三条路径：OK（命中，动作/参数填进 out，out 可为 NULL）、MISS（矩形
 * 表未命中，不改状态）、LOCK_TIMEOUT（未派发——console 打印
 * `TAP ERR lvgl_lock_timeout`，脚本不得把它与正常 miss 混判）。
 * 内部取 LVGL 锁（命中后要改控件文本/立即刷新），仅 console 注入路径调用。 */
typedef enum {
    SLOOP_TAP_MISS = 0,
    SLOOP_TAP_STEP_CELL,     /* a=track b=step：设焦点 + CMD_TOGGLE_STEP */
    SLOOP_TAP_STEP_DRUM,     /* b=step：鼓轨只读，只写状态行提示 */
    SLOOP_TAP_TRACK_CARD,    /* a=track：CMD_SELECT_TRACK */
    SLOOP_TAP_TRANSPORT_PLAY,
    SLOOP_TAP_TRANSPORT_STOP,
    SLOOP_TAP_TRANSPORT_BPM, /* v=±1：CMD_SET_BPM */
} sloop_tap_kind_t;

typedef struct {
    sloop_tap_kind_t kind;
    uint8_t a, b;
    int16_t v;
} sloop_tap_result_t;

typedef enum {
    SLOOP_TAP_RESULT_OK = 0,
    SLOOP_TAP_RESULT_MISS,        /* 矩形表未命中（正常未命中，不改状态） */
    SLOOP_TAP_RESULT_NOT_STARTED, /* UI 未启动 */
    SLOOP_TAP_RESULT_LOCK_TIMEOUT /* LVGL 锁超时：未派发 */
} sloop_tap_result_status_t;

sloop_tap_result_status_t sloop_ui_input_tap(int16_t x, int16_t y, sloop_tap_result_t *out);

/* 输入任务栈余量（高水位，字节；未启动返回 0）。诊断/门禁用。 */
uint32_t sloop_ui_input_stack_free(void);


