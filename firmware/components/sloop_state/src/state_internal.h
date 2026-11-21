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

#endif
