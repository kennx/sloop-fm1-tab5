/*
 * power_sensor.c — INA226 电源监测 + 芯片温度（P4-F 阶段 4 收口 Task 9，spec A5）
 *
 * INA226（I2C 0x41）挂在 BSP 板载总线（G31/G32，`bsp_i2c_get_handle()`，与 ES8388 /
 * 触摸同总线；Tab5 Keyboard 及其 RGB 在独立的 I2C 总线 G0/G1 上，不在本总线）——只在该
 * 总线上挂一个设备句柄，不建第二条总线；总线自身的锁串行化各设备的访问，这里不另加锁。驱动只**读** INA226，不写任何寄存器：沿用上电默认
 * 配置 0x4127（分流 + 总线连续转换；INA226 数据手册 Register Maps）；校准寄存器复位为 0
 * ⇒ 电流寄存器 0x04 恒读 0，故不读它，电流由分流电压寄存器（LSB 2.5µV）换算。
 *
 * 连线（官方原理图 Tab5_Schematics_PDF.pdf 第 5 页，U31 / R39 / C146，矢量连线核对）：
 *   pin 8 VBUS ─┬─ pin 10 IN+ ── R39（R005/1% = 5mΩ）── pin 9 IN− ── SYS_BAT_2S
 *               └─ SYS_BAT_PRE                （C146 1µF 并联在 IN+/IN− 之间）
 *   pin 1 A1 = GND、pin 2 A0 = 3V3 ⇒ 从地址 0x41。
 * 电流 = 分流电压 / 5mΩ，正方向 SYS_BAT_PRE → IN+ → IN− → SYS_BAT_2S；分流电阻值出自
 * 原理图，查不到时不得换算（spec A5）——改这个常数前先给出同样等级的来源。
 */
#include "power_sensor.h"

#include <math.h>
#include <stddef.h>

#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"

#define INA226_I2C_ADDR 0x41
#define INA226_REG_SHUNT_VOLTAGE 0x01
#define INA226_REG_BUS_VOLTAGE 0x02
#define INA226_REG_MANUFACTURER_ID 0xFE
#define INA226_MANUFACTURER_ID 0x5449
#define INA226_BUS_LSB_QUARTER_MV 5 /* 总线电压 LSB = 1.25mV（以 0.25mV 计） */
#define INA226_SHUNT_LSB_HALF_UV 5 /* 分流电压 LSB = 2.5µV（以 0.5µV 计） */
#define POWER_SENSOR_SHUNT_MOHM 5
#define INA226_XFER_TIMEOUT_MS 20

static const char *TAG = "power_sensor";

static i2c_master_dev_handle_t s_ina226;
static temperature_sensor_handle_t s_tsens;
static bool s_ina_bad; /* 上一拍的 INA226 状态：只在翻转时记日志，避免 soak 采样刷屏 */

static esp_err_t ina226_ensure(void)
{
    if (s_ina226 != NULL) return ESP_OK;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_ina226);
}

/* 寄存器指针不自动递增（INA226 数据手册 7.5.5.3）：每个寄存器单独一笔 2 字节读。 */
static esp_err_t ina226_read(uint8_t reg, uint16_t *val)
{
    uint8_t buf[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_ina226, &reg, 1, buf, 2, INA226_XFER_TIMEOUT_MS);
    if (ret == ESP_OK) *val = (uint16_t)((buf[0] << 8) | buf[1]);
    return ret;
}

static esp_err_t tsens_ensure(void)
{
    if (s_tsens != NULL) return ESP_OK;
    /* P4 的量程档位表里误差最小（1℃）的一档，覆盖冷台面到降频阈值附近 */
    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t ret = temperature_sensor_install(&cfg, &s_tsens);
    if (ret != ESP_OK) return ret;
    ret = temperature_sensor_enable(s_tsens);
    if (ret != ESP_OK) {
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
    }
    return ret;
}

static int32_t round_div(int32_t num, int32_t den)
{
    return num >= 0 ? (num + den / 2) / den : (num - den / 2) / den;
}

/* 单调用者（console 任务）；INA226 无效 ⇒ ESP_ERR_INVALID_STATE 且三个 INA 字段为哨兵，
 * 芯片温度失败 ⇒ temp_ok=false / temp_c=NAN（不影响返回码）。 */
esp_err_t power_sensor_read(power_sample_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    out->ina_ok = false;
    out->vbus_mv = POWER_SENSOR_NA;
    out->shunt_uv = POWER_SENSOR_NA;
    out->cur_ma = POWER_SENSOR_NA;
    out->temp_ok = false;
    out->temp_c = NAN;

    uint16_t id = 0, shunt_raw = 0, bus_raw = 0;
    bool ok = ina226_ensure() == ESP_OK && ina226_read(INA226_REG_MANUFACTURER_ID, &id) == ESP_OK &&
              id == INA226_MANUFACTURER_ID &&
              ina226_read(INA226_REG_SHUNT_VOLTAGE, &shunt_raw) == ESP_OK &&
              ina226_read(INA226_REG_BUS_VOLTAGE, &bus_raw) == ESP_OK;
    if (ok) {
        out->ina_ok = true;
        out->vbus_mv = round_div(bus_raw * INA226_BUS_LSB_QUARTER_MV, 4);
        int32_t shunt = (int32_t)(int16_t)shunt_raw * INA226_SHUNT_LSB_HALF_UV;
        out->shunt_uv = round_div(shunt, 2);
        out->cur_ma = round_div(shunt, 2 * POWER_SENSOR_SHUNT_MOHM);
    }
    if (ok == s_ina_bad) {
        s_ina_bad = !ok;
        if (!ok) {
            ESP_LOGW(TAG, "INA226 @0x%02X 无应答或 ID 不符（id=%04X，应为 %04X）", INA226_I2C_ADDR,
                     (unsigned)id, INA226_MANUFACTURER_ID);
        } else {
            ESP_LOGI(TAG, "INA226 恢复: vbus_mv=%ld shunt_uv=%ld", (long)out->vbus_mv,
                     (long)out->shunt_uv);
        }
    }

    if (tsens_ensure() == ESP_OK && temperature_sensor_get_celsius(s_tsens, &out->temp_c) == ESP_OK) {
        out->temp_ok = true;
    } else {
        out->temp_c = NAN;
    }
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}
