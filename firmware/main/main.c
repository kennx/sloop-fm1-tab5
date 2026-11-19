/*
 * sloop-fm1-tab5 — firmware bootstrap
 *
 * 当前阶段：工程骨架与依赖就位（BSP: espressif/m5stack_tab5）。
 * 后续按 AGENTS.md §3 规划逐步落地：显示/音频/键盘 bring-up → SCR 截屏命令（§5.3）
 * → sloop_core 移植 → 编辑器协议 / USB host 接入 FM-1。
 */
#include "esp_chip_info.h"
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
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    ESP_LOGI(TAG, "SoC: ESP32-P4 rev v%d.%d, %d cores", chip.revision / 100, chip.revision % 100,
             chip.cores);
    ESP_LOGI(TAG, "Flash: %u MB %s, PSRAM: %s", (unsigned)(flash_bytes / (1024 * 1024)),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external",
             CONFIG_SPIRAM ? "enabled" : "disabled");
}

void app_main(void)
{
    ESP_LOGI(TAG, "sloop-fm1-tab5 bootstrap (IDF %s)", esp_get_idf_version());
    log_hardware_summary();

    /* TODO(bring-up): bsp_display_start / bsp_audio / Tab5 Keyboard I2C (0x6D, G0/G1)
     *                  + 启动自检（含键盘 I2C 扫描，见 docs/tab5-keyboard-hardware.md §6）
     * TODO(ui):        SCR 截屏命令（AGENTS.md §5.3 协议，screenshot.py 对端）
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
