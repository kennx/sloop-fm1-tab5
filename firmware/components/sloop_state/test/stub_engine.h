/* host 单测的引擎替身：实现 sloop_state 调用的 sloop_core/sloop_seq 函数并记录调用，
 * 以及状态层存储命令用的 sloop_store_* 替身
 * （固定返回码，见文件末尾）。
 * 快照/步表/参数就是 stub 的状态，state_sync_from_engine 走真实的 sloop_core_*
 * 读取路径。 */
#ifndef STUB_ENGINE_H
#define STUB_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "sloop_core.h"
#include "sloop_seq.h"
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
    sloop_dstep_t raw_dstep[STUB_ENGINE_STEPS];
    uint8_t lock_present[4][STUB_ENGINE_STEPS];
    esp_err_t seq_ret;
    bool seq_frozen;
    uint32_t seq_step_edit_calls, seq_pattern_calls, seq_micro_calls, seq_fill_calls;
    uint32_t seq_lock_set_calls, seq_lock_del_calls, seq_dstep_write_calls;
    sloop_step_edit_t last_step_edit;
    uint32_t last_pattern_track;
    sloop_pattern_op_t last_pattern_op;
    int32_t last_pattern_arg;
    uint32_t last_seq_track, last_seq_step, last_seq_param;
    int32_t last_seq_value;
    sloop_dstep_t last_dstep;
    /* P4-C Task 6：录音/走带/鼓演奏命令替身 */
    sloop_rec_snapshot_t rec;      /* sloop_seq_rec_read 的返回值（sync 与命令后投影） */
    uint32_t rec_read_calls, rec_toggle_calls, rec_config_calls, fill_control_calls;
    uint32_t drum_hit_calls, play_button_calls, note_del_calls;
    uint32_t rec_last_quant, rec_last_tempo, rec_last_count;
    uint32_t fill_last_held, fill_last_next;
    uint32_t drum_last_lane, drum_last_vel;
    uint32_t play_button_last_down;
    uint32_t note_del_last_track, note_del_last_step, note_del_last_note;
    esp_err_t note_del_ret;
    /* P4-C Task 8：sections / chain / SONG 命令替身 */
    sloop_song_snapshot_t song_snap; /* sloop_seq_song_read 的返回值 */
    uint32_t song_read_calls, section_calls, chain_begin_calls, chain_commit_calls;
    uint32_t song_mode_calls, song_rec_calls, song_set_calls;
    uint32_t section_last_slot, section_last_op; /* 0 store / 1 load / 2 erase / 3 request */
    uint32_t chain_last_first, chain_last_count;
    uint8_t chain_last_scenes[8];
    uint32_t song_mode_last;
    sloop_song_config_t song_set_last;
    esp_err_t section_ret, chain_ret, song_ret;
    bool song_dirty, sections_dirty;
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
    /* P4-D Task 6：DSYN / FM6 patch / 采样替身后端 */
    sloop_dsyn_sound_t dsyn[4][16]; /* sloop_core_dsyn_read/set 后端（22B 原样） */
    char dsyn_name[4][9];           /* kit 名（8 字符 + NUL） */
    uint8_t dsyn_crush[4], dsyn_src[4];
    uint32_t dsyn_factory_count;    /* src 上限（模拟 DS_NKITS） */
    uint32_t dsyn_read_calls, dsyn_set_calls, dsyn_load_calls, dsyn_audition_calls;
    uint32_t last_dsyn_kit, last_dsyn_lane, last_dsyn_field, last_dsyn_src, last_dsyn_vel;
    int32_t last_dsyn_value;
    bool dsu_dirty;                 /* sloop_store_dsu_dirty 的返回值 */
    esp_err_t dsu_store_ret;
    uint32_t dsu_store_calls;
    bool store_ready;               /* sloop_store_ready 的返回值（默认 true） */
    uint8_t fm6_patch[3][155];      /* 合成轨的 live patch 后端 */
    uint8_t fm6_slot[3];
    bool fm6_ok;                    /* 选中轨引擎是否 FM6（false → NOT_ALLOWED / 哨兵） */
    uint32_t fm6_patch_calls, fm6_set_calls;
    uint32_t last_fm6_track, last_fm6_op, last_fm6_field;
    int32_t last_fm6_value;
    esp_err_t sample_erase_ret;
    uint32_t sample_erase_calls, last_sample_slot;
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
