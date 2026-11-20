/*
 * sloop-fm1-tab5 — ES8388 扬声器正弦音自检（Phase 1 P1-6）
 *
 * codec 由 sloop_hal_tab5/audio_stream.c 单一持有并初始化；
 * audio_tone_play() 经共享句柄反复阻塞播放正弦纯音（AUD TONE 命令后端）。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_tone_init(void);   /* 一次初始化扬声器 codec（main 启动时调用） */

esp_err_t audio_tone_play(uint32_t freq_hz, uint32_t duration_ms);   /* 阻塞至播完 */

void audio_tone_dump_regs(void);    /* AUD REG：dump ES8388 全部寄存器（排障用） */
void audio_tone_dump_pa(void);     /* AUD PA：dump PI4IO 扩展器（SPK_EN 判定） */
esp_err_t audio_tone_set_l2vol(uint8_t val);  /* AUD L2：写 LOUT2/ROUT2 音量（排障实验） */
esp_err_t audio_tone_max_all(void);   /* AUD MAX：全输出音量全开（排障实验） */
