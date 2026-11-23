/*
 * sys_settings.c — Tab5 平台设置：背光亮度 / 输出音量 / 输出路由 + NVS（P4-E Task 3）
 *
 * 三项 Tab5 原生设置存 NVS 命名空间 `tab5`：bright(u8，默认 100) / vol(u8，默认 70) /
 * out(u8，默认 0 = speaker)。接口契约见 sloop_hal.h。
 *
 * 写路径两段式：set_* = 钳位 → 平台副作用立即生效 → 记 RAM 值并置 nvs_dirty，不写
 * flash；sloop_hal_settings_flush() 只在 1s 存储任务的静默窗口（停止 + 无发声 + 空闲）里
 * nvs_set + commit——flash 擦写会 stall 音频路径，与 sloopstore 写入同一纪律
 * （ADR-009 §3）。dirty 用代次守卫：提交期间又有新修改时保留 dirty，下个窗口重写。
 *
 * 输出路由（spec §5 D10）在本板**不能按寄存器路由**，降级为 speaker-only
 * （headphone ⇒ ESP_ERR_NOT_SUPPORTED，状态不变）。依据官方原理图
 * Tab5_Schematics_PDF.pdf 第 3 页（U14 ES8388 / C67 / C68 / U15 NS4150B）：
 *   LOUT1 (pin 12) ─ C68 22µF ─ HPOUT_L ─┬─ 耳机左声道
 *                                        └─ 100nF ─ R43 68K ─ NS4150B INP（INN 经 R44 接 AGND）
 *   ROUT1 (pin 11) ─ C67 22µF ─ HPOUT_R ─── 耳机右声道
 *   LOUT2 / ROUT2 (pin 15 / 14)          未连接
 * 扬声器与耳机左声道共用 LOUT1，不存在只靠 ES8388 音量寄存器就能分开两者的组合；
 * 把 LOUT1/ROUT1 写 0（-33.5dB）会让扬声器与耳机一起近静音——阶段 1 默认态人测无声
 * 即此（docs/dev/device-notes.md §5、DI-6）。开机态保持四路 0x21（sloop_hal_audio_init）。
 */
#include "sloop_hal.h"

#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tab5_keyboard.h"

#define NVS_NAMESPACE "tab5"
#define BRIGHT_MIN 10u
#define BRIGHT_MAX 100u
#define BRIGHT_DEFAULT 100u
#define VOLUME_MAX 100u
#define VOLUME_DEFAULT 70u
#define OUTPUT_SPEAKER 0u

/* ES8388 寄存器 46..49（0x2E..0x31）= LOUT1VOL / ROUT1VOL / LOUT2VOL / ROUT2VOL
 * （ES8388 数据手册 Register 46–49；esp_codec_dev 的 es8388_reg.h 同址
 * ES8388_DACCONTROL24..27）。esp_codec_dev 的音量 API 只写 DAC 数字音量 0x1A/0x1B
 * （es8388.c es8388_set_vol），不碰这四个寄存器。 */
#define ES8388_OUT_L1 0x2Eu
#define ES8388_OUT_R1 0x2Fu
#define ES8388_OUT_L2 0x30u
#define ES8388_OUT_R2 0x31u
#define ES8388_I2C_ADDR 0x10
#define ES8388_I2C_TIMEOUT_MS 20

static const char *TAG = "sloop_settings";

static uint8_t s_bright = BRIGHT_DEFAULT;
static uint8_t s_vol = VOLUME_DEFAULT;
static uint8_t s_out = OUTPUT_SPEAKER;
static uint8_t s_kbd_level;
static bool s_nvs_ready;
static i2c_master_dev_handle_t s_es8388;

/* s_dirty / s_gen：setter（console / 输入任务）与 flush（1s 存储任务）跨任务访问 */
static volatile bool s_dirty;
static volatile uint32_t s_gen;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void mark_dirty(void)
{
    portENTER_CRITICAL(&s_mux);
    s_gen++;
    s_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t sloop_hal_settings_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需擦除重建: %s", esp_err_to_name(ret));
        esp_err_t erase = nvs_flash_erase();
        if (erase != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败: %s", esp_err_to_name(erase));
            return erase;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s（设置只在本次开机内有效）", esp_err_to_name(ret));
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间尚未建立：全部默认 */
        s_nvs_ready = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开 %s 失败: %s", NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }
    /* 三键逐个读，读不到的保持默认（首次开机命名空间已建但键不全时只会缺部分） */
    bool ok = nvs_get_u8(nvs, "bright", &s_bright) == ESP_OK;
    ok = nvs_get_u8(nvs, "vol", &s_vol) == ESP_OK && ok;
    ok = nvs_get_u8(nvs, "out", &s_out) == ESP_OK && ok;
    nvs_close(nvs);
    if (!ok) {
        ESP_LOGW(TAG, "NVS 设置键不完整，缺失项按默认: bright=%u vol=%u out=%u", (unsigned)s_bright,
                 (unsigned)s_vol, (unsigned)s_out);
    }
    /* 存储值只信到钳位范围（越界值不得原样下发给硬件，口径同 set_*） */
    if (s_bright < BRIGHT_MIN) s_bright = BRIGHT_MIN;
    if (s_bright > BRIGHT_MAX) s_bright = BRIGHT_MAX;
    if (s_vol > VOLUME_MAX) s_vol = VOLUME_MAX;
    s_nvs_ready = true;
    ESP_LOGI(TAG, "settings: bright=%u vol=%u out=%s", (unsigned)s_bright, (unsigned)s_vol,
             s_out == OUTPUT_SPEAKER ? "speaker" : "headphone");
    return ESP_OK;
}

uint8_t sloop_hal_settings_bright(void)
{
    return s_bright;
}

uint8_t sloop_hal_settings_volume(void)
{
    return s_vol;
}

uint8_t sloop_hal_settings_output(void)
{
    return s_out;
}

bool sloop_hal_settings_nvs_dirty(void)
{
    return s_dirty;
}

/* 平台调用失败时状态不变（RAM / NVS / 硬件保持一致）。 */
esp_err_t sloop_hal_settings_set_bright(uint8_t v)
{
    if (v < BRIGHT_MIN) v = BRIGHT_MIN;
    if (v > BRIGHT_MAX) v = BRIGHT_MAX;
    esp_err_t ret = bsp_display_brightness_set(v);
    if (ret != ESP_OK) return ret;
    s_bright = v;
    mark_dirty();
    return ESP_OK;
}

esp_err_t sloop_hal_settings_set_volume(uint8_t v)
{
    if (v > VOLUME_MAX) v = VOLUME_MAX;
    esp_codec_dev_handle_t codec = sloop_hal_audio_codec();
    if (codec != NULL && esp_codec_dev_set_out_vol(codec, v) != 0) {
        return ESP_FAIL;
    }
    s_vol = v;
    mark_dirty();
    return ESP_OK;
}

esp_err_t sloop_hal_settings_set_output(uint8_t v)
{
    if (v != OUTPUT_SPEAKER) return ESP_ERR_NOT_SUPPORTED;
    return ESP_OK; /* 已是开机态；输出寄存器由 sloop_hal_audio_init 固定 */
}

/* 与 audio_tone.c 同法：BSP 板载总线（G31/G32）上挂临时设备访问 ES8388（0x10） */
static esp_err_t es8388_dev_ensure(void)
{
    if (s_es8388 != NULL) return ESP_OK;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_es8388);
}

esp_err_t sloop_hal_settings_output_regs(uint8_t out[4])
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = es8388_dev_ensure();
    if (ret != ESP_OK) return ret;
    static const uint8_t regs[4] = {ES8388_OUT_L1, ES8388_OUT_R1, ES8388_OUT_L2, ES8388_OUT_R2};
    for (uint32_t i = 0; i < 4; i++) {
        ret = i2c_master_transmit_receive(s_es8388, &regs[i], 1, &out[i], 1, ES8388_I2C_TIMEOUT_MS);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

void sloop_hal_settings_flush(void)
{
    if (!s_dirty || !s_nvs_ready) return;
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 提交失败（打开）: %s", esp_err_to_name(ret));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    uint32_t gen0 = s_gen;
    uint8_t bright = s_bright;
    uint8_t vol = s_vol;
    uint8_t out = s_out;
    portEXIT_CRITICAL(&s_mux);
    ret = nvs_set_u8(nvs, "bright", bright);
    if (ret == ESP_OK) ret = nvs_set_u8(nvs, "vol", vol);
    if (ret == ESP_OK) ret = nvs_set_u8(nvs, "out", out);
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 提交失败: %s（保留 dirty，下个窗口重试）", esp_err_to_name(ret));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_gen == gen0) s_dirty = false;
    portEXIT_CRITICAL(&s_mux);
}

void sloop_hal_settings_erase(void)
{
    portENTER_CRITICAL(&s_mux);
    s_gen++;
    s_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    if (!s_nvs_ready) {
        ESP_LOGE(TAG, "NVS 未就绪，无法擦除设置键");
        return;
    }
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return; /* 本来就没有设置键 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 擦除失败（打开）: %s", esp_err_to_name(ret));
        return;
    }
    ret = nvs_erase_key(nvs, "bright");
    if (ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND) ret = nvs_erase_key(nvs, "vol");
    if (ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND) ret = nvs_erase_key(nvs, "out");
    if (ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND) ret = nvs_commit(nvs);
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 擦除失败: %s", esp_err_to_name(ret));
    }
}

esp_err_t sloop_hal_keyboard_rgb(uint8_t level, uint32_t rgb888)
{
    esp_err_t ret = tab5_keyboard_rgb_set(level, (uint8_t)(rgb888 >> 16), (uint8_t)(rgb888 >> 8),
                                          (uint8_t)rgb888);
    if (ret == ESP_OK) s_kbd_level = level;
    return ret;
}

uint8_t sloop_hal_keyboard_rgb_level(void)
{
    return s_kbd_level;
}
