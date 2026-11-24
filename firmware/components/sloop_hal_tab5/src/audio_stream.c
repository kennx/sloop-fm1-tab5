#include "sloop_hal.h"

#include <string.h>

#include "esp_heap_caps.h"

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
/* 音频任务的栈必须整块落在内部 L2MEM：P4 的 32KB LP SRAM 是 MALLOC_CAP_INTERNAL 的低优先级
 * 兜底堆，L2MEM 拿不出连续的整块栈时（全量 ENG SELFTEST 之后，DI-26 的堆泄漏）xTaskCreate
 * 会把栈落到那里——该落点下栈上局部变量密集的 DSP 渲染实测慢 ~24 倍（推断：HP 核访问 LP SRAM
 * 不经 L1 cache；落点→变慢的因果已由「换回 L2MEM 即恢复」的干预实验证实）
 * （每块 13ms > 5.8ms 预算，持续 xrun，直到复位）。故首次启流时用 INTERNAL|DMA（LP SRAM
 * 没有 DMA 能力）显式分配，复位前常驻复用；TCB 静态。 */
static StackType_t *audio_stack;
static StaticTask_t audio_tcb;
static volatile uint32_t xruns;
/* 渲染耗时统计（窗口 = 本次流会话：sloop_hal_audio_start 清零，故 ENG PERF 的
 * duty_* 是「本次启流以来」而非自开机累计）。仅供诊断读取，读取方 console
 * 容忍单块级撕裂（均值误差 < 1 块）。 */
static volatile uint32_t render_blocks;
static volatile uint64_t render_us_sum;
static volatile uint32_t render_us_max;
/* 超时强删（DMA 停摆）后置位：I2S 驱动锁可能随被删任务丢失，此后拒绝再启流，
 * 只有复位设备能恢复（sloop_hal_audio_stop 返回 ESP_ERR_INVALID_STATE）。 */
static volatile bool broken;

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
    /* 输出音量取平台设置（NVS 存值，默认 70）；四路输出音量先全开，扬声器所在通路
     * （LOUT1，见 sys_settings.c 文件头）即此态。 */
    if (esp_codec_dev_open(dev, &format) != 0) goto fail;
    if (esp_codec_dev_set_out_vol(dev, sloop_hal_settings_volume()) != 0) goto fail;
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
    if (broken) {
        ESP_LOGE(TAG, "拒绝启流：上次停流超时强删，I2S 驱动锁可能已丢失（需复位设备）");
        return ESP_ERR_INVALID_STATE;
    }
    if (codec == NULL || render == NULL) return ESP_ERR_INVALID_STATE;
    if (audio_task != NULL) return ESP_OK;
    stop_requested = false;
    render_fn = render;
    /* 统计窗口 = 本次流会话：任务尚未创建（无并发写者），start 前清零即可让
     * duty_mean/duty_max 只覆盖本次开流之后的块。 */
    render_blocks = 0;
    render_us_sum = 0;
    render_us_max = 0;
    audio_running = true;
    if (audio_stack == NULL) audio_stack = heap_caps_malloc(AUDIO_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (audio_stack == NULL) {
        audio_running = false;
        return ESP_ERR_NO_MEM;
    }
    audio_task = xTaskCreateStatic(audio_task_run, "sloop_audio", AUDIO_TASK_STACK / sizeof(StackType_t), NULL,
                                    AUDIO_TASK_PRIORITY, audio_stack, &audio_tcb);
    if (audio_task == NULL) {
        audio_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "stream started, half=%u frames stack=%p", (unsigned)AUDIO_HALF_FRAMES,
             pxTaskGetStackStart(audio_task));
    return ESP_OK;
}

/* 停流（DI-8）：置停止请求 → 等任务跳出渲染循环（写块收尾、I2S 驱动锁归还）
 * → vTaskDelete → 句柄置 NULL；codec 保持打开（AUD TONE 仍可用）。
 * 不可直接 vTaskDelete 阻塞在 esp_codec_dev_write 里的任务：那会把 i2s_channel
 * 的 binary 信号量永久留在被删任务手里（实测 AUD TONE 报 "channel is not
 * enabled"）。缓冲、render_fn 与 TCB 均为 static，栈是首次启流分配的常驻 L2MEM 整块，无任务局部对象。
 * 返回：正常停流 ESP_OK；超时强删或已删任务 1s 内未被 IDLE 回收（静态 TCB 不能复用）
 * 返回 ESP_ERR_INVALID_STATE（同时置 broken，后续 sloop_hal_audio_start 拒绝启流，需复位设备）。
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
    const UBaseType_t tasks_after = uxTaskGetNumberOfTasks() - 1u;
    vTaskDelete(audio_task);   /* 任务此时已自挂起，或（超时兜底）仍在写里 */
    audio_running = false;     /* 任务已删除：正常路径由任务自身清除，强制路径这里补 */
    /* TCB 是 static 的：必须等 IDLE 把它从待清理表摘掉（任务数减一）才能复用，否则下一次
     * start 重新初始化它之后，IDLE 的 uxListRemove 摘掉的是**新任务**的状态表项。 */
    const int64_t reap_deadline = esp_timer_get_time() + (int64_t)AUDIO_STOP_TIMEOUT_MS * 1000;
    while (!forced && uxTaskGetNumberOfTasks() != tasks_after && esp_timer_get_time() < reap_deadline) {
        vTaskDelay(1);
    }
    audio_task = NULL;
    stop_requested = false;
    if (forced) {
        broken = true;
        ESP_LOGE(TAG, "stream force-stopped, codec 保持打开但 I2S 驱动可能已损坏（需复位设备）");
        return ESP_ERR_INVALID_STATE;
    }
    if (uxTaskGetNumberOfTasks() != tasks_after) {
        broken = true;
        ESP_LOGE(TAG, "stop: 已删任务未被 IDLE 回收，TCB 不可复用（需复位设备）");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "stream stopped, codec 保持打开");
    return ESP_OK;
}
