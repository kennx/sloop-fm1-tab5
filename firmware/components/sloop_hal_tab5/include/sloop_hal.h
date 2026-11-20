#ifndef SLOOP_HAL_H
#define SLOOP_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"

typedef void (*sloop_hal_render_fn_t)(int32_t *block, uint32_t frames);

#define SLOOP_HAL_USER_SLOT_COUNT 4u
#define SLOOP_HAL_USER_SLOT_SIZE  0x14000u

/* 引擎渲染速率与半缓冲帧数（codec 打开格式、渲染块尺寸）；sloop_core 的
 * SLOOP_CORE_RATE/SLOOP_CORE_FRAMES 须与之相等，由 selftest.c 静态断言兜底 */
#define SLOOP_HAL_AUDIO_RATE   44100u
#define SLOOP_HAL_AUDIO_FRAMES 256u

esp_err_t sloop_hal_init(void);
uint8_t *sloop_hal_user_samples(void);
esp_err_t sloop_hal_audio_init(void);
/* start：启流（codec 须已 init）；清零渲染统计（窗口 = 本次会话）；上次停流
 * 走了超时强删（broken）时拒绝并返回 ESP_ERR_INVALID_STATE。
 * stop：协作停流（任务自挂起后删除，codec 保持打开）；超时强删返回
 * ESP_ERR_INVALID_STATE 并锁死后续 start，需复位设备。 */
esp_err_t sloop_hal_audio_start(sloop_hal_render_fn_t render);
esp_err_t sloop_hal_audio_stop(void);
esp_codec_dev_handle_t sloop_hal_audio_codec(void);
esp_err_t sloop_hal_audio_max_all(void);
uint32_t sloop_hal_audio_xruns(void);
bool sloop_hal_audio_streaming(void);
uint32_t sloop_hal_audio_stack_high_water(void);

/* 渲染耗时统计（窗口 = 本次流会话）：均值/峰值 µs 与渲染块数；任一出参可为 NULL。
 * 由音频任务在每块渲染后更新（与 xrun 判定同一次测量），console `ENG PERF`
 * 用它给出流式占空比（预算 = SLOOP_HAL_AUDIO_FRAMES / SLOOP_HAL_AUDIO_RATE）。
 * `sloop_hal_audio_start` 清零三者，故 duty_* 只覆盖本次启流之后的块——
 * 首块可能因冷缓存/首次渲染偏高（实测 ~4.3ms），计入峰值但不排除。 */
void sloop_hal_audio_render_stats(uint32_t *mean_us, uint32_t *max_us, uint32_t *blocks);

#endif
