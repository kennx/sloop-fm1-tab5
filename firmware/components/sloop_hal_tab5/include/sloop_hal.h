#ifndef SLOOP_HAL_H
#define SLOOP_HAL_H

#include <stdint.h>

#include "esp_err.h"

#define SLOOP_HAL_USER_SLOT_COUNT 4u
#define SLOOP_HAL_USER_SLOT_SIZE  0x14000u

esp_err_t sloop_hal_init(void);
uint8_t *sloop_hal_user_samples(void);

#endif
