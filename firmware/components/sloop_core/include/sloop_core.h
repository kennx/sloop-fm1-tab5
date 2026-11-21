#ifndef SLOOP_CORE_H
#define SLOOP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 线程/所有权规则：sloop_core_render 与所有改变引擎状态的 API（track/note/hit/
 * play/set_global/write_step/set_mute/set_solo/select）以及只读的 read/read_step
 * 共享内部互斥锁，可从不同任务调用；不得在 ISR 中调用。selftest 与音频流互斥
 * （流式期间返回 busy）。
 * 诊断模式（sloop_core_diag_freeze，console `ENG SELFTEST` 用）：冻结后 read 返回
 * 冻结快照、read_step 返回全零、写 API 直接丢弃（均不取锁），selftest 因此可整段持
 * 引擎锁而不制造等待者——UI 在自检期间停在最后一帧、键盘编辑不落地，结束后下一拍
 * 自动对齐。selftest 开始/结束各恢复一次 init 快照，会丢弃运行期编辑——阶段 3 UI
 * 接管状态后，这条对用户可见。 */
#define SLOOP_CORE_NENGINES 12u
#define SLOOP_CORE_P_COUNT  71u
#define SLOOP_CORE_G_COUNT  50u
#define SLOOP_CORE_RATE     44100u
#define SLOOP_CORE_FRAMES   256u

esp_err_t sloop_core_init(void);
esp_err_t sloop_core_status(void);
void sloop_core_render(int32_t *block, uint32_t frames);
void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset);
void sloop_core_note(uint32_t track, uint32_t note, bool on);
void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity);
void sloop_core_play(bool on);
uint32_t sloop_core_rate(void);
uint32_t sloop_core_frames(void);
int sloop_core_selftest(const char *case_name);

/* 只读快照：MVP 屏所需状态一次性拷出（内部持引擎锁，数百字节拷贝）。
 * 字段与编辑器协议词表对齐：bpm 是全局 BPM（上游 G_BPM，即编辑器协议里的
 * TEMPO）；trk[k].len/div 是轨道的 LEN/DIV；step_pos[k] 是播放头（seq_idx 模
 * len，停止时 0）。step[][] 与 dstep[] 是**16 列步视窗**：sloop_core_read 从
 * 引擎步 0 起取，sloop_core_read_window 从 window_start 起取（绝对步
 * window_start+c，见 sloop_state 的 step_window_t）。step[][] 只覆盖三条合成轨；
 * 鼓轨（track 3）的步内容由 dstep[] 聚合给出，step[3] 恒为零（上游 step_t 与
 * dstep_t 是同一 union，按 step_t 解读鼓步无意义）。鼓轨的 trk[3].engine/preset
 * 同样无意义（上游对鼓轨报 NENGINES，它没有引擎与预设）；这里保留上游原值、
 * 不置零，显示层按轨别决定是否使用。 */
typedef struct {
    bool playing;
    uint16_t bpm;
    uint8_t selected_track;
    struct { uint8_t engine, preset, level, mute, armed, solo; uint8_t len, div; } trk[4];
    struct { uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags; } step[4][16];
    struct { uint8_t on; uint8_t lvl; uint8_t rat; } dstep[16];
    uint8_t step_pos[4];
} sloop_core_snapshot_t;
void sloop_core_read(sloop_core_snapshot_t *out);

/* 步窗快照：与 sloop_core_read 同，但 step[k][c]/dstep[c] 取引擎绝对步
 * start + c（start 向下对齐到 16 的倍数并钳位到 0..48，即 4 个 bank 之一）。
 * 一次引擎锁内完成——状态层每拍只需一次取锁就能把 STEP 页窗口列对齐到引擎真值，
 * 不必逐列 read_step。冻结态（诊断期）与 sloop_core_read 相同：返回冻结快照
 * （基 0），UI 本就在 quiesce 中暂停。 */
void sloop_core_read_window(uint32_t start, sloop_core_snapshot_t *out);

/* 单步读写：完整上游 step_t 语义（n/note/vel/time/flags 原样，lvl/rat 为每音符
 * 2bit 打包），外加 micro（-32..31）与 fill（0..3）保留字段。读改写由调用方做；
 * write 是不丢字段的全量写（调用方须把读到的 lvl/rat/micro/fill 原样带回）。
 * 鼓轨（track >= 3）不可用这个结构表达：read 返回全零，write 忽略。越界
 * （track/idx）同样忽略/返回全零。 */
typedef struct {
    uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
    uint8_t lvl; uint8_t rat;
    int8_t micro; uint8_t fill;
} sloop_step_t;
void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out);
void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in);

/* 全局参数写：按上游 GP[id] 的 min/max 钳位后写 song.g[id]（id 越界忽略）。
 * 走引擎锁；只写值，不做上游 UI 的联动副作用（如 G_BPM 的显示刷新不需要）。
 * 不要用它执行 GO/动作全局（G_LOAD/G_SAVE/G_CLRSEQ/G_INITSND/G_NEWPRJ/G_ENGSEL/
 * G_INFO 等）——上游这些是瞬时的 UI 按钮/查询，写 song.g[] 只会改值、不执行动作。 */
void sloop_core_set_global(uint32_t id, int16_t value);

/* 选轨 / 静音 / 独奏写（SEL/GLO 层的可听路径）：分别写 song.sel、trk[k].p[P_MUTE]
 * （0/1，上游 N_ONOFF）与 song.solo（低 4 位独奏掩码）。走引擎锁，越界
 * （k>=NTRK）与冻结态直接丢弃。听感口径是上游 trk_silent()：P_MUTE 置位，或
 * 「有独奏且本位未置」即静音。 */
void sloop_core_set_mute(uint32_t k, bool mute);
void sloop_core_set_solo(uint8_t mask);
void sloop_core_select(uint32_t k);

/* 诊断模式（`ENG SELFTEST` 内部成对调用，不必由调用方执行）：freeze(true) 先持锁
 * 取一份快照再置位，之后 read 返回该冻结快照、read_step 返回全零、所有改变引擎
 * 状态的 API 直接丢弃（不取锁、不阻塞）；freeze(false) 清位。效果 = 自检期间 UI
 * 冻结在最后一帧、键盘编辑不落地。freeze 只保证「置位后新到的调用不取锁/不写」；
 * 全量自检另由 quiesce 钩子把 UI 定时器/LVGL 定时器/输入派发停掉（见下），保证
 * 自检期间系统里没有其它引擎使用方（见 ADR-007）。不得在 ISR 调用。 */
void sloop_core_diag_freeze(bool on);

/* 诊断期静默钩子：sloop_ui 启动成功后注册 sloop_ui_quiesce（暂停 30ms UI 定时器与
 * LVGL 定时器、输入任务阻塞丢弃事件）。sloop_core_selftest 在诊断期前后成对调用；
 * 未注册（UI 未启动、host 自测）时为空操作。只保留一个钩子（后注册覆盖）；
 * 不得在 ISR 调用。 */
void sloop_core_diag_quiesce_hook(void (*hook)(bool on));

#endif
