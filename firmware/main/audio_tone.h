/*
 * sloop-fm1-tab5 — ES8388 扬声器正弦音自检（Phase 1 P1-6）
 *
 * audio_tone_init() 一次初始化 codec（句柄存 static 复用），
 * 之后可反复 audio_tone_play() 阻塞播放正弦纯音（AUD TONE 命令后端）。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_tone_init(void);   /* 一次初始化扬声器 codec（main 启动时调用） */

esp_err_t audio_tone_play(uint32_t freq_hz, uint32_t duration_ms);   /* 阻塞至播完 */
