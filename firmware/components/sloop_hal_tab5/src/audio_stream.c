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
/* 协作停流等待上限：正常一次写块 ≤ 数十 ms（DMA 节流），1s 只有任务卡死在
 * codec 写时才触发兜底强删 */
#define AUDIO_STOP_TIMEOUT_MS 1000

static const char *TAG = "sloop_audio_stream";
static esp_codec_dev_handle_t codec;
static i2c_master_dev_handle_t codec_i2c;
static TaskHandle_t audio_task;
static volatile bool audio_running;     /* 任务存活且尚未退出渲染循环（start 置位；任务退出循环/被删时清除） */
static volatile bool stop_requested;
static sloop_hal_render_fn_t render_fn;
static int32_t render_buf[2][AUDIO_HALF_FRAMES * 2u];
static int16_t output_buf[2][AUDIO_HALF_FRAMES * 2u];
static volatile uint32_t xruns;
/* 渲染耗时统计（自开机累计；块数/和/峰值）。仅供诊断读取，读取方 console
 * 容忍单块级撕裂（均值误差 < 1 块）。 */
static volatile uint32_t render_blocks;
static volatile uint64_t render_us_sum;
static volatile uint32_t render_us_max;

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

void sloop_hal_audio_render_stats(uint32_t *mean_us, uint32_t *max_us, uint32_t *blocks)
{
    /* 先读块数与和、后读峰值：任务并发写时最多把均值算得略高（不会低估成本） */
    uint32_t blocks_now = render_blocks;
    uint64_t sum = render_us_sum;
    uint32_t max_now = render_us_max;
    if (mean_us != NULL) *mean_us = blocks_now ? (uint32_t)(sum / blocks_now) : 0;
    if (max_us != NULL) *max_us = max_now;
    if (blocks != NULL) *blocks = blocks_now;
}

static void audio_task_run(void *arg)
{
    (void)arg;
    uint32_t half = 0;
    const int64_t budget_us = (int64_t)AUDIO_HALF_FRAMES * 1000000 / AUDIO_RATE;
    while (!stop_requested) {
        int64_t started = esp_timer_get_time();
        render_fn(render_buf[half], AUDIO_HALF_FRAMES);
        int64_t render_us = esp_timer_get_time() - started;
        render_blocks++;
        render_us_sum += (uint64_t)render_us;
        if ((uint32_t)render_us > render_us_max) render_us_max = (uint32_t)render_us;
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
    /* 退出写路径（i2s 驱动的 binary 信号量已归还）后清除运行标志并自挂起：
     * 由 stop 的 vTaskDelete 收尾，避免终止阻塞在写里的任务而丢失 I2S 驱动锁 */
    audio_running = false;
    vTaskSuspend(NULL);
}

esp_err_t sloop_hal_audio_start(sloop_hal_render_fn_t render)
{
    if (codec == NULL || render == NULL) return ESP_ERR_INVALID_STATE;
    if (audio_task != NULL) return ESP_OK;
    stop_requested = false;
    render_fn = render;
    audio_running = true;
    if (xTaskCreate(audio_task_run, "sloop_audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &audio_task) != pdPASS) {
        audio_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "stream started, half=%u frames", (unsigned)AUDIO_HALF_FRAMES);
    return ESP_OK;
}

/* 停流（DI-8）：置停止请求 → 等任务跳出渲染循环（写块收尾、I2S 驱动锁归还）
 * → vTaskDelete → 句柄置 NULL；codec 保持打开（AUD TONE 仍可用）。
 * 不可直接 vTaskDelete 阻塞在 esp_codec_dev_write 里的任务：那会把 i2s_channel
 * 的 binary 信号量永久留在被删任务手里（实测 AUD TONE 报 "channel is not
 * enabled"）。缓冲与 render_fn 均为 static，无任务局部对象。
 * 调用上下文：console REPL（ENG TRIG/ENG AUDIO）是启停的常规上下文；main.c 开机
 * 序列另调一次 start，与 console 首条命令之间只有数十 ms 启动窗口，故 start/stop
 * 未加锁（该窗口内的并发启停未防护；正常时序下 start 在首条命令前已返回）。 */
esp_err_t sloop_hal_audio_stop(void)
{
    if (audio_task == NULL) return ESP_OK;
    stop_requested = true;
    /* 1 tick = 10ms（CONFIG_FREERTOS_HZ=100）；用 esp_timer 定时限，避免
     * pdMS_TO_TICKS(小于 1 tick) 归零导致立即判超时 */
    const int64_t deadline = esp_timer_get_time() + (int64_t)AUDIO_STOP_TIMEOUT_MS * 1000;
    while (audio_running && esp_timer_get_time() < deadline) {
        vTaskDelay(1);
    }
    const bool forced = audio_running;
    if (forced) {
        /* 任务卡在 codec 写里不返回（DMA 停摆）时的兜底；I2S 驱动锁可能随任务
         * 丢失，此路径后 AUD TONE 需复位设备才能恢复 */
        ESP_LOGW(TAG, "stop timeout: force delete, I2S 驱动锁可能丢失");
    }
    vTaskDelete(audio_task);   /* 任务此时已自挂起，或（超时兜底）仍在写里 */
    audio_running = false;     /* 任务已删除：正常路径由任务自身清除，强制路径这里补 */
    audio_task = NULL;
    stop_requested = false;
    if (forced) {
        ESP_LOGE(TAG, "stream force-stopped, codec 保持打开但 I2S 驱动可能已损坏（需复位设备）");
    } else {
        ESP_LOGI(TAG, "stream stopped, codec 保持打开");
    }
    return ESP_OK;
}
