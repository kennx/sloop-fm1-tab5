/* sloop_state 组件内部接口：状态容器的互斥与持锁访问（state.c ↔ cmd.c）。
 * 不得被组件外包含。 */
#ifndef SLOOP_STATE_INTERNAL_H
#define SLOOP_STATE_INTERNAL_H

#include "sloop_state.h"

/* 设备：FreeRTOS mutex；HOST_TEST：空实现。不递归，只由公开 API 最外层取用。 */
void state_lock(void);
void state_unlock(void);

/* 取状态容器（仅持 state_lock 期间使用）。 */
app_state_t *state_locked(void);

/* 自动存档的「无输入」时间源（设备：FreeRTOS tick / 100Hz；HOST_TEST：恒 0）。
 * 与键盘/TAP 的调用方同一时钟（ui/input 也用 xTaskGetTickCount）。 */
uint32_t state_now_ms(void);

/* 整份刷新投影（一次窗口快照 + 一次参数回读，与 state_sync_from_engine 同口径），
 * **调用方必须已持 state_lock**。用于存储命令成功后立即采纳引擎真值（工程/预置
 * 载入会改选中轨/参数/图案）；不自行取锁，避免在 state_apply 内重入死锁。 */
void state_refresh_locked(void);

#endif
