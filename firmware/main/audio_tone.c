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
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sloop_hal.h"

static const char *TAG = "sloop_audio";

#define AUDIO_TONE_SAMPLE_RATE (44100)
#define AUDIO_TONE_FRAMES      (512)    /* 每写入块帧数（int16 交错立体声 2048B） */
#define AUDIO_TONE_AMPLITUDE   (16000)  /* -6dBFS，防 NS4150B 削顶 */

/* 写入缓冲存 static：REPL 单任务调用，避免占用命令任务栈 */
static int16_t s_block[AUDIO_TONE_FRAMES * 2];

esp_err_t audio_tone_init(void)
{
    return sloop_hal_audio_init();
}

esp_err_t audio_tone_play(uint32_t freq_hz, uint32_t duration_ms)
{
    esp_codec_dev_handle_t spk_dev = sloop_hal_audio_codec();
    if (spk_dev == NULL) {
        ESP_LOGE(TAG, "codec 未初始化（sloop_hal_audio_init 未调用或失败）");
        return ESP_ERR_INVALID_STATE;
    }
    if (sloop_hal_audio_streaming()) {
        ESP_LOGE(TAG, "引擎音频流运行中，拒绝并发 AUD TONE");
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
        int rc = esp_codec_dev_write(spk_dev, s_block, sizeof(s_block));
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

/* AUD REG 排障后端：esp_codec_dev_read_reg 对 es8388 未实现透传读（全部失败），
 * 改走裸 I2C：BSP 板载总线（G31/G32）挂临时设备读 ES8388（7 位地址 0x10）*/
void audio_tone_dump_regs(void)
{
    static i2c_master_dev_handle_t dev;
    if (dev == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x10,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
            printf("挂载 ES8388 I2C 设备失败\n");
            return;
        }
    }
    for (int base = 0; base <= 0x38; base += 8) {
        printf("%02x:", base);
        for (int reg = base; reg < base + 8 && reg <= 0x38; reg++) {
            uint8_t val = 0xAA;
            esp_err_t ret = i2c_master_transmit_receive(dev, (uint8_t[]){(uint8_t)reg}, 1, &val, 1, 20);
            printf(" %02x=%02x", reg, ret == ESP_OK ? val : 0xEE);
        }
        printf("\n");
    }
}

/* AUD PA 排障后端：dump 两颗 PI4IOE5V6408 扩展器（0x43/0x44）全部寄存器，
 * 判定 SPK_EN（E1.P1）等输出实际电平（寄存器语义同 M5Tab5-UserDemo m5stack_tab5.c）*/
void audio_tone_dump_pa(void)
{
    static const uint8_t regs[] = {0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x11, 0x13};
    static const uint8_t addrs[] = {0x43, 0x44};
    for (unsigned a = 0; a < sizeof(addrs); a++) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[a],
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
            printf("PI4IO 0x%02X 挂载失败\n", addrs[a]);
            continue;
        }
        printf("PI4IO 0x%02X:", addrs[a]);
        for (unsigned r = 0; r < sizeof(regs); r++) {
            uint8_t val = 0;
            esp_err_t ret = i2c_master_transmit_receive(dev, (uint8_t[]){regs[r]}, 1, &val, 1, 20);
            printf(" %02x=%02x", regs[r], ret == ESP_OK ? val : 0xEE);
        }
        printf("\n");
        i2c_master_bus_rm_device(dev);
    }
}

/* AUD L2 排障实验：写 ES8388 LOUT2/ROUT2 音量（0x30/0x31，出厂默认 0x00=-33.5dB
 * 近静音；0x1E=0dB，0x21=+4.5dB）——判定扬声器功放输入是否取自 L2/R2 */
esp_err_t audio_tone_set_l2vol(uint8_t val)
{
    static i2c_master_dev_handle_t dev;
    if (dev == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x10,
            .scl_speed_hz = 100000,
        };
        esp_err_t ret = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    uint8_t v = val;
    esp_err_t ret = i2c_master_transmit(dev, (uint8_t[]){0x30, v}, 2, 20);
    ret |= i2c_master_transmit(dev, (uint8_t[]){0x31, v}, 2, 20);
    printf("LOUT2/ROUT2 <- 0x%02X %s\n", v, ret == ESP_OK ? "OK" : esp_err_to_name(ret));
    return ret;
}

/* AUD MAX 排障实验：一切输出路径音量全开（DAC 数字 0dB + L1/R1/L2/R2 全 0x21）*/
esp_err_t audio_tone_max_all(void)
{
    esp_err_t ret = sloop_hal_audio_max_all();
    printf("MAX: DAC=0dB, L1/R1/L2/R2=0x21 %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));
    return ret;
}
