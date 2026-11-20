#include "sloop_hal.h"

#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 速率/块尺寸单一来源在 sloop_hal.h；与引擎 SLOOP_CORE_RATE/FRAMES 的相等性由
 * selftest.c 的静态断言（两个头都可见的 TU）兜底，避免此处再复制一份数字。 */
#define AUDIO_RATE SLOOP_HAL_AUDIO_RATE
#define AUDIO_HALF_FRAMES SLOOP_HAL_AUDIO_FRAMES
#define AUDIO_OUT_VOL 70
#define AUDIO_TASK_STACK 4096u
#define AUDIO_TASK_PRIORITY 5u

static const char *TAG = "sloop_audio_stream";
static esp_codec_dev_handle_t codec;
static i2c_master_dev_handle_t codec_i2c;
static TaskHandle_t audio_task;
static sloop_hal_render_fn_t render_fn;
static int32_t render_buf[2][AUDIO_HALF_FRAMES * 2u];
static int16_t output_buf[2][AUDIO_HALF_FRAMES * 2u];
static volatile uint32_t xruns;

esp_err_t sloop_hal_audio_max_all(void)
{
    if (codec_i2c == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x10,
            .scl_speed_hz = 100000,
        };
        esp_err_t ret = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &codec_i2c);
        if (ret != ESP_OK) return ret;
    }
    static const uint8_t writes[][2] = {
        {0x1A, 0x00}, {0x1B, 0x00},
        {0x2E, 0x21}, {0x2F, 0x21}, {0x30, 0x21}, {0x31, 0x21},
    };
    esp_err_t ret = ESP_OK;
    for (uint32_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        esp_err_t one = i2c_master_transmit(codec_i2c, writes[i], 2, 20);
        if (one != ESP_OK) ret = one;
    }
    return ret;
}

esp_err_t sloop_hal_audio_init(void)
{
    if (codec != NULL) return ESP_OK;
    esp_codec_dev_handle_t dev = bsp_audio_codec_speaker_init();
    if (dev == NULL) return ESP_FAIL;

    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    /* 任何一步失败都关掉句柄再返回：否则句柄残留会让下次 init 在顶部守卫直接
     * ESP_OK，start 在未打开的 codec 上跑渲染任务（TRIG 报 OK 而 xrun 攀升）。 */
    if (esp_codec_dev_open(dev, &format) != 0) goto fail;
    if (esp_codec_dev_set_out_vol(dev, AUDIO_OUT_VOL) != 0) goto fail;
    if (sloop_hal_audio_max_all() != ESP_OK) goto fail;
    codec = dev;
    ESP_LOGI(TAG, "codec 44100Hz/2ch/16bit ready");
    return ESP_OK;

fail:
    esp_codec_dev_close(dev);
    ESP_LOGE(TAG, "codec 初始化失败，句柄已关闭（下次调用重试）");
    return ESP_FAIL;
}

esp_codec_dev_handle_t sloop_hal_audio_codec(void)
{
    return codec;
}

uint32_t sloop_hal_audio_xruns(void)
{
    return xruns;
}

bool sloop_hal_audio_streaming(void)
{
    return audio_task != NULL;
}

/* 音频任务栈历史最小余量（字节）；无任务时 0。任务永不 panic 的前提是余量够用，
 * 由 ENG PERF 打印、按预设实测（ANALOG/FM6/PHYS）。 */
uint32_t sloop_hal_audio_stack_high_water(void)
{
    return audio_task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(audio_task) : 0;
}

static void audio_task_run(void *arg)
{
    (void)arg;
    uint32_t half = 0;
    const int64_t budget_us = (int64_t)AUDIO_HALF_FRAMES * 1000000 / AUDIO_RATE;
    while (1) {
        int64_t started = esp_timer_get_time();
        render_fn(render_buf[half], AUDIO_HALF_FRAMES);
        int64_t render_us = esp_timer_get_time() - started;
        if (render_us > budget_us) xruns++;
        for (uint32_t i = 0; i < AUDIO_HALF_FRAMES * 2u; i++) {
            int32_t sample = render_buf[half][i];
            output_buf[half][i] = (int16_t)(sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample);
        }
        if (esp_codec_dev_write(codec, output_buf[half], sizeof(output_buf[half])) != 0) {
            xruns++;
            ESP_LOGE(TAG, "codec write failed");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        half ^= 1u;
    }
}

esp_err_t sloop_hal_audio_start(sloop_hal_render_fn_t render)
{
    if (codec == NULL || render == NULL) return ESP_ERR_INVALID_STATE;
    if (audio_task != NULL) return ESP_OK;
    render_fn = render;
    if (xTaskCreate(audio_task_run, "sloop_audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &audio_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "stream started, half=%u frames", (unsigned)AUDIO_HALF_FRAMES);
    return ESP_OK;
}
