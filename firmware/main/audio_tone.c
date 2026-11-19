/*
 * sloop-fm1-tab5 — ES8388 扬声器正弦音输出（AUD TONE 自检后端）
 *
 * bsp_audio_codec_speaker_init() 内部完成 I2C/I2S/PA 初始化；codec 以
 * 44100Hz/2ch/16bit 打开——44100 是上游 SLOOP 引擎的采样率假设
 * （docs/research/sloop-fm1-firmware.md §4.3），写 48000 即错。
 * 正弦幅度 -6dBFS（峰值 16000）：扬声器经 NS4150B（1W@8Ω）驱动，
 * 保守幅度防削顶；首/末 512 帧块线性淡入淡出防爆音/咔哒。
 */
#include "audio_tone.h"

#include <math.h>

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sloop_audio";

#define AUDIO_TONE_SAMPLE_RATE (44100)
#define AUDIO_TONE_FRAMES      (512)    /* 每写入块帧数（int16 交错立体声 2048B） */
#define AUDIO_TONE_AMPLITUDE   (16000)  /* -6dBFS，防 NS4150B 削顶 */
#define AUDIO_TONE_OUT_VOL     (70)     /* codec_dev 音量 0..100（默认 0 近乎静音） */

static esp_codec_dev_handle_t s_spk_dev;

/* 写入缓冲存 static：REPL 单任务调用，避免占用命令任务栈 */
static int16_t s_block[AUDIO_TONE_FRAMES * 2];

esp_err_t audio_tone_init(void)
{
    if (s_spk_dev != NULL) {
        return ESP_OK;
    }

    s_spk_dev = bsp_audio_codec_speaker_init();
    if (s_spk_dev == NULL) {
        ESP_LOGE(TAG, "扬声器 codec 初始化失败（bsp_audio_codec_speaker_init 返回 NULL）");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_TONE_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    int rc = esp_codec_dev_open(s_spk_dev, &fs);
    ESP_LOGI(TAG, "speaker codec open 44100Hz/2ch/16bit rc=%d", rc);
    if (rc != 0) {
        ESP_LOGE(TAG, "codec 打开失败 rc=%d", rc);
        return ESP_FAIL;
    }

    rc = esp_codec_dev_set_out_vol(s_spk_dev, AUDIO_TONE_OUT_VOL);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置输出音量 %d 失败 rc=%d", AUDIO_TONE_OUT_VOL, rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_tone_play(uint32_t freq_hz, uint32_t duration_ms)
{
    if (s_spk_dev == NULL) {
        ESP_LOGE(TAG, "codec 未初始化（audio_tone_init 未调用或失败）");
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz == 0 || freq_hz > AUDIO_TONE_SAMPLE_RATE / 2) {
        ESP_LOGE(TAG, "频率 %u Hz 超出 (0, %d]，拒绝播放", (unsigned)freq_hz,
                 AUDIO_TONE_SAMPLE_RATE / 2);
        return ESP_ERR_INVALID_ARG;
    }

    const float two_pi = 6.28318530718f;
    const float step = two_pi * (float)freq_hz / (float)AUDIO_TONE_SAMPLE_RATE;
    const int64_t duration_us = (int64_t)duration_ms * 1000;
    const int64_t block_us = (int64_t)AUDIO_TONE_FRAMES * 1000000 / AUDIO_TONE_SAMPLE_RATE;
    float phase = 0.0f;
    bool first_block = true;

    ESP_LOGI(TAG, "tone %uHz %ums 开始", (unsigned)freq_hz, (unsigned)duration_ms);
    /* 以流逝时间界定播放时长；esp_codec_dev_write 按 DMA 节流天然让出 CPU */
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < duration_us) {
        int64_t elapsed = esp_timer_get_time() - t0;
        bool last_block = elapsed + block_us >= duration_us;
        for (int i = 0; i < AUDIO_TONE_FRAMES; i++) {
            float env = 1.0f;
            if (first_block) {
                env *= (float)(i + 1) / AUDIO_TONE_FRAMES;
            }
            if (last_block) {
                env *= (float)(AUDIO_TONE_FRAMES - i) / AUDIO_TONE_FRAMES;
            }
            int16_t s = (int16_t)(sinf(phase) * AUDIO_TONE_AMPLITUDE * env);
            s_block[2 * i] = s;
            s_block[2 * i + 1] = s;
            phase += step;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }
        int rc = esp_codec_dev_write(s_spk_dev, s_block, sizeof(s_block));
        if (rc != 0) {
            ESP_LOGE(TAG, "codec 写入失败 rc=%d（已播 %lldms）", rc,
                     (long long)((esp_timer_get_time() - t0) / 1000));
            return ESP_FAIL;
        }
        first_block = false;
    }
    ESP_LOGI(TAG, "tone 结束");
    return ESP_OK;
}
