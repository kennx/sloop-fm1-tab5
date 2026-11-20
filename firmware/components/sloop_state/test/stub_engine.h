/* host 单测的引擎替身：实现 sloop_state 调用的 9 个引擎函数并记录调用。
 * 快照/步表就是 stub 的状态，state_sync_from_engine 走真实的 sloop_core_read 路径。 */
#ifndef STUB_ENGINE_H
#define STUB_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "sloop_core.h"

#define STUB_ENGINE_TRACKS 3u  /* 引擎的合成轨（鼓轨不在 read_step/write_step 范围） */
#define STUB_ENGINE_STEPS 64u

typedef struct {
    sloop_core_snapshot_t snap;                    /* sloop_core_read 的快照（步窗由 step[] 覆盖） */
    sloop_step_t step[STUB_ENGINE_TRACKS][STUB_ENGINE_STEPS]; /* read_step/write_step 后端 */
    uint32_t read_calls, read_step_calls, write_step_calls;
    uint32_t note_calls, play_calls, set_global_calls;
    uint32_t set_mute_calls, set_solo_calls, select_calls;
    bool last_play;
    struct { uint32_t track, note; bool on; } last_note;
    struct { uint32_t id; int16_t value; } last_global;
    struct { uint32_t track; bool mute; } last_mute;
    uint8_t last_solo_mask;
    uint32_t last_select;
} stub_engine_t;

extern stub_engine_t g_stub;

/* 清空记录并把快照/步表恢复为上游默认（bpm=90，空步 time=REST）。 */
void stub_engine_reset(void);

#endif
