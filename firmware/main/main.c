/*
 * sloop-fm1-tab5 — bring-up 自检入口（阶段 1）
 *
 * 阶段 1 平台 bring-up 已落地（2026-11-19，机器侧门禁全绿）：MIPI-DSI 显示与
 * 测试图（display.c）、console SCR/KB/AUD 命令（console.c，AGENTS §5.3）、
 * Tab5 Keyboard 驱动（components/tab5_keyboard，I2C 0x6D @ G0/G1 + INT G50，
 * FW 0x01）、ES8388 扬声器正弦自检（audio_tone.c，44.1kHz/2ch/16bit）。
 * 键盘全矩阵/触摸校准/听音为人工验证项（docs/dev/verification-log.md）。
 * 下一阶段：阶段 2 sloop_core 引擎移植（docs/design/002-port-roadmap.md §4）。
 */
#include "audio_tone.h"
#include "console.h"
#include "display.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_keyboard.h"

static const char *TAG = "sloop_tab5";

static void log_hardware_summary(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "SoC: ESP32-P4 rev v%d.%d, %d cores", chip.revision / 100, chip.revision % 100,
             chip.cores);

    uint32_t flash_bytes = 0;
    esp_err_t ret = esp_flash_get_size(NULL, &flash_bytes);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %u MB %s, PSRAM: %s", (unsigned)(flash_bytes / (1024 * 1024)),
                 (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external",
                 CONFIG_SPIRAM ? "enabled" : "disabled");
    } else {
        ESP_LOGW(TAG, "Flash: 容量读取失败 (%s), PSRAM: %s", esp_err_to_name(ret),
                 CONFIG_SPIRAM ? "enabled" : "disabled");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "sloop-fm1-tab5 bootstrap (IDF %s)", esp_get_idf_version());
    log_hardware_summary();

    esp_err_t ret = app_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败: %s", esp_err_to_name(ret));
    }

    ret = app_console_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "console 初始化失败: %s", esp_err_to_name(ret));
    }

    /* Tab5 Keyboard 自检（0x6D 无应答不算致命——键盘可能未接 Ext.Port1，
     * 驱动就位等待硬件接入；全键矩阵核对见 §5.4/任务报告人测清单）*/
    ret = tab5_keyboard_init();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "键盘初始化失败: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "keyboard: %s", tab5_keyboard_probe() ? "OK (0x6D)" : "NOT FOUND");

    /* 扬声器 codec 自检（ES8388，44.1kHz/2ch/16bit；失败不算致命——
     * 仅损失 AUD TONE 自检能力，等待硬件复查）*/
    ret = audio_tone_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(ret));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
