/* host 单测用的 esp_codec_dev.h 替身：状态层经真实 sloop_hal.h 取平台设置的原型，而
 * 该头只为音频句柄类型包含 esp_codec_dev.h。host 上无 ESP-IDF / BSP；状态层不调用任何
 * codec 函数，这里只提供 sloop_hal.h 用到的 typedef。 */
#ifndef ESP_CODEC_DEV_H
#define ESP_CODEC_DEV_H

typedef struct esp_codec_dev *esp_codec_dev_handle_t;

#endif
