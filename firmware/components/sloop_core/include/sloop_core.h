#ifndef SLOOP_CORE_H
#define SLOOP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 线程/所有权规则：sloop_core_render 与所有改变引擎状态的 API（track/note/hit/
 * play/set_global/write_step）以及只读的 read/read_step 共享内部互斥锁，可从
 * 不同任务调用；不得在 ISR 中调用。selftest 与音频
 * 流互斥（流式期间返回 busy）。注意 selftest 会把引擎恢复到 sloop_core_init
 * 快照并丢弃运行期编辑——阶段 3 UI 接管状态后，这条对用户可见。 */
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
 * len，停止时 0）。step[][] 为前 16 列视窗，只覆盖三条合成轨；鼓轨（track 3）
 * 的步内容由 dstep[] 聚合给出，step[3] 恒为零（上游 step_t 与 dstep_t 是同一
 * union，按 step_t 解读鼓步无意义）。 */
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

/* 单步读写：完整上游 step_t 语义（n/note/vel/time/flags 原样，lvl/rat 为每音符
 * 2bit 打包），外加 micro（-32..31）与 fill（0..3）保留字段。读改写由调用方做；
 * write 是丢字段的全量写。鼓轨（track >= 3）不可用这个结构表达：read 返回全零，
 * write 忽略。越界（track/idx）同样忽略/返回全零。 */
typedef struct {
    uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
    uint8_t lvl; uint8_t rat;
    int8_t micro; uint8_t fill;
} sloop_step_t;
void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out);
void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in);

/* 全局参数写：按上游 GP[id] 的 min/max 钳位后写 song.g[id]（id 越界忽略）。
 * 走引擎锁；只写值，不做上游 UI 的联动副作用（如 G_BPM 的显示刷新不需要）。 */
void sloop_core_set_global(uint32_t id, int16_t value);

#endif
