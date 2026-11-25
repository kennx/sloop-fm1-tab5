/*
 * sloop-fm1-tab5 — 应用入口（阶段 1 bring-up + 阶段 2 引擎 + 阶段 3 UI）
 *
 * 阶段 1 平台 bring-up 已落地（2026-11-19，机器侧门禁全绿）：MIPI-DSI 显示与
 * 测试图（display.c）、console SCR/KB/AUD/ENG 命令（console.c，AGENTS §5.3）、
 * Tab5 Keyboard 驱动（components/tab5_keyboard，I2C 0x6D @ G0/G1 + INT G50，
 * FW 0x01）、ES8388 扬声器正弦自检（audio_tone.c，44.1kHz/2ch/16bit）。
 * 键盘全矩阵/触摸校准/听音为人工验证项（docs/dev/verification-log.md）。
 * 阶段 2 已接入 sloop_core 同源自测组合 TU、RAM 存储与 ES8388 引擎音频流
 * （阶段 3 起开机即启流，ENG AUDIO ON|OFF 可停启，DI-8）。
 * 阶段 3 起 sloop_ui（LVGL 页面框架；P4-B 5 组 / 11 页，P4-C Task 11 起 6 组 / 12 页，
 * P4-D Task 8 起 7 组 / 14 页）在引擎/状态层就绪后接管屏幕；启动失败
 * 回退 bring-up 测试图（app_display_draw_test_pattern）并记 ERROR。
 */
#include "audio_tone.h"
#include "console.h"
#include "display.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sloop_core.h"
#include "sloop_hal.h"
#include "sloop_state.h"
#include "sloop_ui.h"
#include "sys_selftest.h"
#include "tab5_keyboard.h"
#include "usb_fm1.h"

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

    /* 平台设置（NVS：亮度/音量/路由）先于显示与音频初始化——两者在各自 init 里取存值
     * 应用；失败不阻断启动（RAM 默认值照常生效，只是不持久化）。 */
    esp_err_t ret = sloop_hal_settings_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "平台设置初始化失败: %s", esp_err_to_name(ret));
    }

    ret = app_display_init();
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

    ret = sloop_hal_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SLOOP HAL 初始化失败: %s", esp_err_to_name(ret));
    }
    ret = sloop_core_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SLOOP 引擎初始化失败: %s", esp_err_to_name(ret));
    }
    /* AppState 须在 UI 的任何读/写之前初始化（sloop_state.h 的前置条件）：引擎
     * 成功路径取引擎快照，失败路径取全零快照——UI 仍可渲染；state_init 幂等。 */
    state_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SLOOP 引擎初始化完成: rate=%u frames=%u",
                 (unsigned)sloop_core_rate(), (unsigned)sloop_core_frames());
        /* 开机即启流（阶段 3 音频所有权，DI-8）：UI 渲染与音频并行；诊断命令
         * （ENG SELFTEST/AUD TONE/离线 PERF）需先 ENG AUDIO OFF。失败不阻断启动。*/
        esp_err_t audio_ret = sloop_hal_audio_start(sloop_core_render);
        if (audio_ret != ESP_OK) {
            ESP_LOGE(TAG, "音频流启动失败: %s（引擎静默，ENG AUDIO ON 可重试）",
                     esp_err_to_name(audio_ret));
        }
    }

    /* USB-A host 常驻（5A Task 1，ADR-002）：当前只做设备枚举记录，FM1 STATUS 可查；
     * 任务栈在 PSRAM。失败不阻断启动——联动模式不可用，独立模式照常。 */
    esp_err_t usb_ret = usb_fm1_start(&(usb_fm1_config_t){0});
    if (usb_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB host 启动失败: %s（FM1 联动不可用）", esp_err_to_name(usb_ret));
    }

    /* 阶段 3 UI：状态层就绪后接管屏幕（单容器屏 + 两行页签：7 组 / 14 页，P4-D Task 8）。
     * 失败回退 bring-up
     * 测试图并记 ERROR——不留空白屏；UI 成功时测试图不再是开机画面。 */
    ESP_LOGI(TAG, "UI 启动前内部堆剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ret = sloop_ui_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UI 启动失败: %s（回退 bring-up 测试图）", esp_err_to_name(ret));
        app_display_draw_test_pattern();
    } else {
        /* UI 接管屏幕：关掉 bring-up 触摸准星与坐标日志（会盖住生产页面/截图）；
         * DI-10 的幽灵按压事件仍可能到达，不在本任务修 */
        app_display_touch_feedback_enable(false);
    }

    /* SELFTEST chip 的唯一执行者：等状态层请求、跑完自删；在 UI 启动后创建，确保
     * sloop_core_selftest 的 quiesce 钩子已注册。 */
    ret = sys_selftest_start();
    state_set_selftest_available(ret == ESP_OK);
    if (ret != ESP_OK) ESP_LOGE(TAG, "SELFTEST 任务创建失败: %s", esp_err_to_name(ret));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
