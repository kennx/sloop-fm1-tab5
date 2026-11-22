#ifndef SLOOP_SEQ_H
#define SLOOP_SEQ_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sloop_store.h" /* sloop_song_config_t（歌曲快照内嵌，Task 8） */

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
 * `ft_bars` 由快照消费（不在绘制函数里清）。`rec_go` 没有独立投影字段：ARMED→
 * RECORDING 的状态迁移已表达「刚开始录音」，消费它是为了不把一次性标志留给之后
 * 无关的读者。`fill_held`/`fill_arm` 投影上游两个独立变量（FILL 保持键电平与
 * NEXT BAR 待命），供设备断言 fill 表演态（Task 6 deferral）。诊断冻结时快照全零
 * 且不消费。 */
typedef enum { SEQ_REC_OFF, SEQ_REC_ARMED, SEQ_REC_RECORDING, SEQ_REC_COUNT_IN,
               SEQ_REC_FREE_TAKE } sloop_rec_state_t;
typedef struct { uint8_t state, track, quant_off, tempo_mode, count_mode, take_bars;
                 uint8_t fill_held, fill_arm; } sloop_rec_snapshot_t;
void sloop_seq_rec_read(sloop_rec_snapshot_t *out);
/* 上游 rec_toggle：停止态 arm；走带中立即开录；再按一次关录音（**不停播放**）；
 * 自由录音进行中按 REC = 关闭它（与上游 editor 的 REC 同一路径 transport_req=1）。 */
esp_err_t sloop_seq_rec_toggle(void);
esp_err_t sloop_seq_rec_config(uint8_t quant_off, uint8_t tempo_mode, uint8_t count_mode);
/* 上游 fill 表演控制：两个**独立**变量——held 只由 FILL 保持键的 down/up 写；
 * next_bar 是 NEXT BAR 按键的脉冲，只翻转 fill_arm，不读不写 held（按住 FILL 时
 * 点 NEXT 不会清掉保持电平）。STOP 由上游清两者。 */
esp_err_t sloop_seq_fill_control(bool held, bool next_bar);
/* 鼓演奏：lane 0..15，velocity 1..127（按 vel_lvl 记进鼓步）；velocity 0 = 松开
 * （自由录音的鼓音符段在此结束，与上游 KS_DRUM 的 key-up 同口径）。走
 * drum_input()/rec_hit() 同一路径；选中鼓轨时**不回落合成轨 0**。 */
esp_err_t sloop_seq_drum_hit(uint32_t lane, uint32_t velocity);
/* 物理 PLAY 键（上游面板按钮口径）：自由录音中按下 = **drop**（丢弃，工程不变）；
 * 其它情况与按钮一样请求走带。API PLAY（sloop_core_play / CMD_PLAY）仍是 close。 */
esp_err_t sloop_seq_play_button(bool down);

/* ---- arranger：live sections / quick chain / SONG / SONG REC（P4-C Task 8）----
 * 快照是上游运行态的一份一致视图（引擎锁内取；config 是 arrangement 36B 的 POD 投影）：
 *   enabled     SONG 模式（arrangement_enabled；PLAY 时按它决定 SONG 或普通 loop）
 *   running     SONG 正在播放（arrangement_clock.running；此时 live section 动作被拒）
 *   index/bar   SONG 当前条目与条目内小节（停止后保留最后一次值）
 *   ready       A–D 有效工程槽的位掩码（bit0 = A；`proj_ok` 口径，与上游 arrangement_ready 同）
 *   live        正在/最近一个 live section（0..3），0xFF = 无（= 上游 live_sec）
 *   dirty       有尚未落盘的 section 或歌曲修改（Task 7 的 1s 门限写）
 *   chain_count quick chain 项数（0 = 无 chain）
 *   chain_index/chain_bars 当前 chain 项与它的小节数（上游 chain_i/chain_bars）
 *   srec        SONG REC 状态：0 off / 1 armed（下一个小节起）/ 2 recording
 *   srec_done   最近一次 SONG REC 的结果：0 无、0xFF 什么都没录到（NO SONG）、否则项数
 * 诊断冻结时快照全零（不读、不消费）。dirty 的歌曲部分由 `srec_done` 对账：一次
 * take 结束（含 STOP 收尾）后置 song dirty，保证延迟写把新歌曲顺序落盘。 */
typedef struct {
    uint8_t enabled, running, index, bar, ready, live, dirty, chain_count, srec;
    uint8_t chain_index, chain_bars, srec_done;
    uint8_t chain_scenes[8]; /* 正在跑/已安装的 quick chain（0..3；chain_count 之外未定义） */
    sloop_song_config_t config;
} sloop_song_snapshot_t;
void sloop_seq_song_read(sloop_song_snapshot_t *out);

/* section 动作（A–D = 槽 0..3；与上游 project.c 的 live sections 同义）：
 *   store=true  立即捕获当前工作 loop 到 RAM 槽（**播放中也允许**，引擎锁内完成），
 *               置 section dirty，flash 由 1s 周期任务在停止+安静+idle 后写；
 *   erase=true  仅停止态（走带中 ESP_ERR_NOT_ALLOWED）：写空对象 + 清 RAM 槽；歌曲里
 *               引用该 scene 的项被 `arr_drop_scene` 移除（无项剩回默认歌曲）并置 song
 *               dirty；空槽 ESP_ERR_NOT_FOUND 且不写 flash；
 *   两者皆 false = load（仅停止态）：把 RAM 槽应用到引擎、live_sec = slot。**不是**
 *   project LOAD（不回落 flash、不写存储）。
 * 槽号越界 invalid、诊断冻结 invalid state。 */
esp_err_t sloop_seq_section(uint32_t slot, bool store, bool erase);
/* live 播放中请求一个 section：下一个小节生效（上游 live_req → arrangement_apply 在
 * 渲染块边界）。只在「live loop 正在播放且 SONG 未播放」时接受；空槽 not found、
 * 走带中/其它情形 not allowed（= console 的 blocked）。请求同时取消正在跑的 quick chain
 * （上游第一次 section tap 的语义）。**绝不**用 project LOAD 做播放中切换。 */
esp_err_t sloop_seq_section_request(uint32_t slot);
/* quick chain（SAVE 按住的多 section tap）：begin(first) 立即把 first 作为普通
 * live_req 排队并清旧 chain；commit 以 1..8 项原子安装（首项保持已请求/正在播放，
 * 之后每项播放 `section_bars()` 小节并循环）。count 0 = **cancel**（清 chain，保留
 * 已排队的请求——这就是「单 section tap 取消 chain、该 section 继续播放」）。
 * 任一 scene 空/非法则整份拒绝（原子，不安装半条 chain）。 */
esp_err_t sloop_seq_chain_begin(uint32_t first);
esp_err_t sloop_seq_chain_commit(const uint8_t *scenes, uint32_t count);
/* SONG 模式切换（上游 SONG MODE / LOOP MODE）：SONG REC 进行中拒绝（not allowed）。 */
esp_err_t sloop_seq_song_mode(bool enabled);
/* SONG REC（上游 ui_layers.c 的 section w=13）：未录音时强制 loop 模式并从下一个小节
 * 开始录制实际播放过的 section（每个 section 播放的小节数）；录音中/armed 时 = 停止并
 * 把顺序写进 arrangement（空录制 = NO SONG，不覆盖原歌曲）。SONG 正在播放时拒绝
 * （not allowed，= STOP FIRST）。 */
esp_err_t sloop_seq_song_rec(void);

#endif
