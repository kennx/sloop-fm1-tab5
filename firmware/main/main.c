/*
 * sloop-fm1-tab5 — firmware bootstrap
 *
 * 当前阶段：Phase 1 bring-up——MIPI-DSI 显示已点亮（display.c，横屏 1280×720 + 测试图）、
 * console 通道与 SCR 截屏命令已落地（console.c，AGENTS §5.3）。
 * 后续按 AGENTS.md §3 规划逐步落地：音频/键盘 bring-up（KB/AUD 命令）
 * → sloop_core 移植 → 编辑器协议 / USB host 接入 FM-1。
 */
#include "console.h"
#include "display.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    /* TODO(bring-up): bsp_audio / Tab5 Keyboard I2C (0x6D, G0/G1)
     *                  + 启动自检（含键盘 I2C 扫描，见 docs/tab5-keyboard-hardware.md §6；
     *                  KB/AUD 自检命令在 console.c 追加注册）
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
