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

/* 单份投影的同拍刷新（调用方持 state_lock）：DSYN 按当前 kit/lane 选择读 22B + kit
 * 元数据 + DSU dirty；FM6 patch 按选中轨读——**非 FM6 合成轨保留 patch、只把
 * engine_ok 置 0**（core 回 NOT_ALLOWED，页面据 engine_ok 显示锁定态）；鼓轨
 * （TRK_DRUM = NPART ⇒ 越界）、越界轨（INVALID_ARG）与诊断冻结（INVALID_STATE）
 * 把快照清零（没有可显示的 live patch）。
 * CMD_DSYN_* 与 CMD_FM6_SET 成功时用它们避免整份窗口快照。 */
void state_refresh_dsyn_locked(void);
void state_refresh_fm6_locked(void);

/* P4-E Task 4：settings 生效副本与平台设置的同拍刷新（调用方持 state_lock）。
 * settings 取 sloop_store_settings()（不取锁的字对齐单次读；palette/lowcut/zoom/lights/
 * theme_on/reso/theme_valid/dirty/theme_name）；sys 取 HAL 的 RAM 三值 + nvs_dirty。
 * init / state_sync_from_engine / state_refresh_locked 各调一次，CMD_SETTINGS_SET /
 * CMD_SYS_* 成功时再调对应的一个。 */
void state_refresh_settings_locked(void);
void state_refresh_sys_locked(void);

/* P4-F Task 5：SOUND 页投影的同拍刷新（调用方持 state_lock）：按**选中轨**读
 * sloop_core_sound_read（引擎/预置/名字；鼓轨 = kit 名）。init / sync /
 * state_refresh_locked 各调一次，CMD_SET_TRACK 与 CMD_SET_ENGINE/PRESET 成功时再调。 */
void state_refresh_sound_locked(void);

#endif
