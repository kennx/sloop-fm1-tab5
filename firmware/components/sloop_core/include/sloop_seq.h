#ifndef SLOOP_SEQ_H
#define SLOOP_SEQ_H

#include <stdbool.h>
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
/* 诊断冻结只读查询：供状态层在 void 读/写入口（toggle/erase 的预读）之前区分
 * frozen 与 empty，避免把被丢弃的写报成 OK/NOT_FOUND。 */
bool sloop_seq_frozen(void);

/* 一条命令是一个完整上游动作、一次引擎锁。SPAN 的 value 是目标长度 1..64；
 * NOTE_MOVE/COPY 的 target 是目标起点；LVL/RAT 写 index 音符的原始 2-bit 值，
 * SLIDE 的 value 只接受 0/1。CLEAR 清该步音符、lock/micro/fill，不改 LEN。
 * PATTERN 的 rotate/length 只动 step[]，transpose 扫描 64 步且拒绝鼓轨；
 * undo/redo 只交换 steps/micro/LEN，不含 fill/lock。COPY 在任意写入前预检锁容量，
 * 不足回 ESP_ERR_NO_MEM；不安全 CONT/目标占用回 ESP_ERR_NOT_ALLOWED（console: blocked），
 * ESP_ERR_INVALID_STATE 只表示诊断冻结。 */
typedef enum { SEQ_STEP_SPAN, SEQ_STEP_NOTE_DEL, SEQ_STEP_NOTE_MOVE, SEQ_STEP_NOTE_COPY,
               SEQ_STEP_LVL, SEQ_STEP_RAT, SEQ_STEP_SLIDE, SEQ_STEP_CLEAR } sloop_step_op_t;
typedef struct { uint8_t track, step, index, target; int16_t value; sloop_step_op_t op; } sloop_step_edit_t;
esp_err_t sloop_seq_step_edit(const sloop_step_edit_t *cmd);
typedef enum { SEQ_PATTERN_ROTATE, SEQ_PATTERN_LENGTH, SEQ_PATTERN_TRANSPOSE,
               SEQ_PATTERN_CLEAR, SEQ_PATTERN_UNDO, SEQ_PATTERN_REDO } sloop_pattern_op_t;
esp_err_t sloop_seq_pattern(uint32_t track, sloop_pattern_op_t op, int32_t arg);

#endif
