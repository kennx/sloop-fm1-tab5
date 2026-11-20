#ifndef SLOOP_HAL_H
#define SLOOP_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"

typedef void (*sloop_hal_render_fn_t)(int32_t *block, uint32_t frames);

#define SLOOP_HAL_USER_SLOT_COUNT 4u
#define SLOOP_HAL_USER_SLOT_SIZE  0x14000u

esp_err_t sloop_hal_init(void);
uint8_t *sloop_hal_user_samples(void);
esp_err_t sloop_hal_audio_init(void);
esp_err_t sloop_hal_audio_start(sloop_hal_render_fn_t render);
esp_codec_dev_handle_t sloop_hal_audio_codec(void);
esp_err_t sloop_hal_audio_max_all(void);
uint32_t sloop_hal_audio_xruns(void);
bool sloop_hal_audio_streaming(void);

#endif
