#include "sys_selftest.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sloop_core.h"
#include "sloop_hal.h"
#include "sloop_state.h"
#include "sloop_store.h"

#define SYS_SELFTEST_STACK 16384u
#define SYS_SELFTEST_PRIO 2u

static const char *TAG = "sys_selftest";

static void sys_selftest_task(void *arg)
{
    (void)arg;
    while (!state_take_selftest_request()) vTaskDelay(pdMS_TO_TICKS(20));
    /* 让触发 chip 的 console TAP 先收完提示符，再开始打印自检输出。 */
    vTaskDelay(pdMS_TO_TICKS(50));

    bool ok = true;
    uint32_t ran = 0, failed = 0;
    sloop_store_autosave_enable(false);

    esp_err_t ret = sloop_hal_audio_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "停音频流失败: %s", esp_err_to_name(ret));
        ok = false;
    } else {
        int rc = sloop_core_selftest(NULL);
        sloop_core_selftest_stats(&ran, &failed);
        if (rc != 0) ok = false;
    }

    ret = sloop_hal_audio_start(sloop_core_render);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "恢复音频流失败: %s", esp_err_to_name(ret));
        ok = false;
    }
    ret = sloop_store_autosave_resume();
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "没有 autosave 对象，保持自检恢复后的工厂态");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "恢复工作工程失败: %s", esp_err_to_name(ret));
        ok = false;
    }
    sloop_store_autosave_enable(true);

    if (!ok && failed == 0) failed = 1;
    state_set_selftest_result(ran, failed);
    ESP_LOGI(TAG, "结果 ran=%u failed=%u", (unsigned)ran, (unsigned)failed);
    vTaskDelete(NULL);
}

esp_err_t sys_selftest_start(void)
{
    if (xTaskCreate(sys_selftest_task, "sys_selftest", SYS_SELFTEST_STACK, NULL,
                    SYS_SELFTEST_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
