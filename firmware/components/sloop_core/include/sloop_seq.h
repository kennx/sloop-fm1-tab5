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

/* 从指定步里按**音高**找到 index 并删除（一次引擎锁内完成查找 + 上游 note_del）：
 * 实时录音会并发改步，状态层「先读步算 index、再另取锁删」会让 index 在两步之间
 * 失效。鼓轨/非法步/音高 0 或 >127 回 ESP_ERR_INVALID_ARG，空步或无匹配音回
 * ESP_ERR_NOT_FOUND，诊断冻结回 ESP_ERR_INVALID_STATE。 */
esp_err_t sloop_seq_note_del(uint32_t track, uint32_t step, uint32_t note);

/* ---- 实时录音与鼓演奏（spec §4.1/§4.3）------------------------------------
 * 状态：OFF 未录音；ARMED 停止态待命（首个音符或 PLAY 启动）；RECORDING 走带中
 * 录选中轨；COUNT_IN COUNT 模式的预备小节；FREE_TAKE 空工程的自由录音。
 * track：ARMED/COUNT_IN = 选中轨；RECORDING = song.rec 位；FREE_TAKE = take 轨。
 * quant_off/tempo_mode/count_mode 是运行期配置（本阶段不持久化）。
 * take_bars 是**一次性**结果：0 无，0xFF 丢弃，其余为关闭的小节数；`rec_go` 与
 * `ft_bars` 由快照消费（不在绘制函数里清）。诊断冻结时快照全零且不消费。 */
typedef enum { SEQ_REC_OFF, SEQ_REC_ARMED, SEQ_REC_RECORDING, SEQ_REC_COUNT_IN,
               SEQ_REC_FREE_TAKE } sloop_rec_state_t;
typedef struct { uint8_t state, track, quant_off, tempo_mode, count_mode, take_bars; } sloop_rec_snapshot_t;
void sloop_seq_rec_read(sloop_rec_snapshot_t *out);
/* 上游 rec_toggle：停止态 arm；走带中立即开录；再按一次关录音（**不停播放**）；
 * 自由录音进行中按 REC = 关闭它（与上游 editor 的 REC 同一路径 transport_req=1）。 */
esp_err_t sloop_seq_rec_toggle(void);
esp_err_t sloop_seq_rec_config(uint8_t quant_off, uint8_t tempo_mode, uint8_t count_mode);
/* 上游 fill 表演控制：held = FILL 保持电平；next_bar = 一次 NEXT BAR 按键（翻转
 * 下一小节 fill 的待命）。STOP 由上游清两者。 */
esp_err_t sloop_seq_fill_control(bool held, bool next_bar);
/* 鼓演奏：lane 0..15，velocity 1..127（按 vel_lvl 记进鼓步）；velocity 0 = 松开
 * （自由录音的鼓音符段在此结束，与上游 KS_DRUM 的 key-up 同口径）。走
 * drum_input()/rec_hit() 同一路径；选中鼓轨时**不回落合成轨 0**。 */
esp_err_t sloop_seq_drum_hit(uint32_t lane, uint32_t velocity);
/* 物理 PLAY 键（上游面板按钮口径）：自由录音中按下 = **drop**（丢弃，工程不变）；
 * 其它情况与按钮一样请求走带。API PLAY（sloop_core_play / CMD_PLAY）仍是 close。 */
esp_err_t sloop_seq_play_button(bool down);

#endif
