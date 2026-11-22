#ifndef SLOOP_SEQ_H
#define SLOOP_SEQ_H

#include <stdint.h>

#include "esp_err.h"

/* 原始鼓步与参数锁。所有写入口内部持引擎锁；诊断冻结时写回 ESP_ERR_INVALID_STATE，
 * void 读接口返回全零、lock_list 返回 0。轨/步/参数越界回 ESP_ERR_INVALID_ARG。
 * lock_set 的第 25 个不同 (step,param) 键回 ESP_ERR_NO_MEM；同键覆写不占新槽。
 * lock_del 的 param==P_COUNT 清除该步全部锁，其余值只删指定键。 */
typedef struct { uint8_t on[2], lvl[4], rat[4]; } sloop_dstep_t;
typedef struct { uint8_t step, param; int16_t value; } sloop_lock_t;
void sloop_seq_read_dstep(uint32_t step, sloop_dstep_t *out);
esp_err_t sloop_seq_write_dstep(uint32_t step, const sloop_dstep_t *in);
esp_err_t sloop_seq_set_micro(uint32_t track, uint32_t step, int8_t value);
esp_err_t sloop_seq_set_fill(uint32_t track, uint32_t step, uint8_t value);
esp_err_t sloop_seq_lock_set(uint32_t track, uint32_t step, uint32_t param, int16_t value);
esp_err_t sloop_seq_lock_del(uint32_t track, uint32_t step, uint32_t param);
uint32_t sloop_seq_lock_list(uint32_t track, sloop_lock_t *out, uint32_t cap);

#endif
