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
        /* 停流之后、自检之前先落盘（终审 I1）：自检的 restore 把引擎回滚到工厂态，收尾的
         * resume/reload 以 flash 为准——不先写，最后一次自动存档之后（≤20s gap）的编辑与
         * 1.5s/5s 延迟写窗口内的设置修改就丢了。流已停 ⇒ 擦写不会让渲染 xrun；停止态由
         * chip 的 STOP FIRST 守卫保证，这里再由存储层拒绝一次。失败不阻断自检，只记日志
         * （没写下去的部分随自检丢弃）。 */
        esp_err_t fr = sloop_store_flush_all();
        if (fr != ESP_OK)
            ESP_LOGE(TAG, "自检前落盘失败: %s（未落盘的编辑/设置将丢失）", esp_err_to_name(fr));
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
