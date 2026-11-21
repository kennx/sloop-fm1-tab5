/* host 单测的引擎替身：实现 sloop_state 调用的 12 个引擎函数并记录调用
 * （read/read_window/read_step/write_step/set_global/play/note/set_mute/set_solo/
 * select/set_params/read_params），以及状态层存储命令用的 sloop_store_* 替身
 * （固定返回码，见文件末尾）。
 * 快照/步表/参数就是 stub 的状态，state_sync_from_engine 走真实的 sloop_core_*
 * 读取路径。 */
#ifndef STUB_ENGINE_H
#define STUB_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "sloop_core.h"
#include "sloop_store.h" /* SLOOP_STORE_BANK_PACKED（银行 PUT 的替身缓冲） */

#define STUB_ENGINE_TRACKS 3u  /* 引擎的合成轨（鼓轨不在 read_step/write_step 范围） */
#define STUB_ENGINE_STEPS 64u
/* 上游 P_E0（轨道参数表末尾的 8 个引擎槽起点）：71 - 8 = 63 */
#define STUB_ENGINE_P_E0 (SLOOP_CORE_P_COUNT - 8u)
#define STUB_ENGINE_PARAM_BATCH_MAX 128u

typedef struct {
    sloop_core_snapshot_t snap;                    /* sloop_core_read 的快照（步窗由 step[] 覆盖） */
    sloop_step_t step[STUB_ENGINE_TRACKS][STUB_ENGINE_STEPS]; /* read_step/write_step 后端 */
    struct { uint8_t on, lvl, rat; } dstep[STUB_ENGINE_STEPS]; /* 鼓轨聚合后端（窗口读） */
    /* 参数后端：set_params 写、read_params 读（与真引擎一样，钳位在写入口） */
    int16_t param[4][SLOOP_CORE_P_COUNT];
    int16_t param_g[SLOOP_CORE_G_COUNT];
    /* 存储 API 替身：统一返回 store_ret 并记录调用（不碰引擎后端——「命令后整份刷新
     * 投影」由测试先改上面的引擎后端再断言）。store_last_track = 0xFF 表示无轨参数。 */
    esp_err_t store_ret;
    uint32_t store_calls;         /* 三族存储 API 调用总数 */
    uint32_t store_proj_calls, store_preset_calls, store_bank_calls;
    uint32_t store_last_slot, store_last_track;
    uint32_t store_last_op;       /* 0 save/PUT、1 load、2 erase（每次存储调用更新） */
    uint32_t bank_put_calls, bank_current_calls;
    uint8_t last_bank_put[SLOOP_STORE_BANK_PACKED];
    uint32_t read_calls, read_window_calls, read_step_calls, write_step_calls;
    uint32_t note_calls, play_calls, set_global_calls;
    uint32_t set_mute_calls, set_solo_calls, select_calls;
    uint32_t set_params_calls, read_params_calls;
    uint32_t invalid_param_ids;                    /* set_params 收到越界 id 的条数（逐条忽略） */
    uint32_t last_params_track, last_params_n;
    uint8_t last_param_ids[STUB_ENGINE_PARAM_BATCH_MAX];     /* 最后一次批量调用的原样入参 */
    int16_t last_param_values[STUB_ENGINE_PARAM_BATCH_MAX];
    bool last_play;
    struct { uint32_t track, note; bool on; } last_note;
    struct { uint32_t id; int16_t value; } last_global;
    struct { uint32_t track; bool mute; } last_mute;
    uint8_t last_solo_mask;
    uint32_t last_select;
} stub_engine_t;

extern stub_engine_t g_stub;

/* 窗口读钩子（测试用）：在 sloop_core_read_window 入口按入参 start 调用。用于
 * 模拟「sync 取引擎快照期间 CMD_SET_WINDOW 落地」的交错（state 的窗口复核路径）。
 * 钩子内可安全调用 state_apply（此时 state 锁不在 state_sync 手里）。NULL = 不启用。 */
extern void (*g_stub_on_read_window)(uint32_t start);

/* 清空记录并把快照/步表恢复为上游默认（bpm=90，空步 time=REST）。 */
void stub_engine_reset(void);

/* 参数后端的外部改写入口（测试模拟「引擎侧外部改动」）：按轨写 p 槽，sync 后应在
 * 投影 p[] 里看到。越界 track/id 忽略。 */
void stub_engine_set_param(uint32_t track, uint32_t id, int16_t value);
void stub_engine_set_global(uint32_t id, int16_t value);

#endif
