#ifndef POWER_SENSOR_H
#define POWER_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* INA226 读数无效时的哨兵（无应答 / ID 不符 / I2C 错误），console 打印为 na。 */
#define POWER_SENSOR_NA INT32_MIN

typedef struct {
    bool ina_ok; /* 本次 INA226 读数有效（下列三项才有意义） */
    int32_t vbus_mv; /* 总线电压 = VBUS 引脚 = IN+ 侧（SYS_BAT_PRE），mV */
    int32_t shunt_uv; /* 分流电压（IN+ − IN−），µV */
    int32_t cur_ma; /* = shunt_uv / 5mΩ，正 = PRE → 2S */
    bool temp_ok; /* 芯片温度有效；失败时 temp_c = NAN */
    float temp_c; /* 芯片温度，℃ */
} power_sample_t;

/* 一次读数（单调用者：console 任务）。INA226 无效 ⇒ ESP_ERR_INVALID_STATE 且三个 INA
 * 字段为 POWER_SENSOR_NA；芯片温度失败只置 temp_ok=false，不改返回码。 */
esp_err_t power_sensor_read(power_sample_t *out);

#endif
