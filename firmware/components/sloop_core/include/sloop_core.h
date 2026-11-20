#ifndef SLOOP_CORE_H
#define SLOOP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 线程/所有权规则：sloop_core_render 与所有改变引擎状态的 API（track/note/hit/
 * play）共享内部互斥锁，可从不同任务调用；不得在 ISR 中调用。selftest 与音频
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

#endif
