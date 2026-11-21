/* host 单测用的 esp_err.h 替身：knob_math.h → sloop_core.h 只用到 esp_err_t，
 * host 上没有 ESP-IDF。 */
#ifndef ESP_ERR_H
#define ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK 0

#endif
