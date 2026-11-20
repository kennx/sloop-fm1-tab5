#include "sloop_hal.h"

#include <string.h>

#include "esp_heap_caps.h"

static uint8_t *user_samples;

esp_err_t sloop_hal_init(void)
{
    if (user_samples != NULL) return ESP_OK;

    uint8_t *samples = heap_caps_malloc(SLOOP_HAL_USER_SLOT_COUNT * SLOOP_HAL_USER_SLOT_SIZE,
                                        MALLOC_CAP_SPIRAM);
    if (samples == NULL) return ESP_ERR_NO_MEM;
    memset(samples, 0xFF, SLOOP_HAL_USER_SLOT_COUNT * SLOOP_HAL_USER_SLOT_SIZE);
    user_samples = samples;
    return ESP_OK;
}

uint8_t *sloop_hal_user_samples(void)
{
    return user_samples;
}
