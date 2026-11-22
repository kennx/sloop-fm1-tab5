#include "stub_engine.h"

#include <string.h>

#include "sloop_store.h" /* sloop_store_note_activity：host 单测的空实现（见文件末尾） */

stub_engine_t g_stub;
void (*g_stub_on_read_window)(uint32_t start);

void stub_engine_reset(void)
{
    memset(&g_stub, 0, sizeof(g_stub));
    g_stub_on_read_window = NULL;
    g_stub.snap.bpm = 90; /* 上游 GP[G_BPM].def */
    g_stub.param_g[0] = 90; /* G_BPM：参数投影与快照 bpm 同源 */
    for (uint32_t k = 0; k < 4; k++) {
        g_stub.snap.trk[k].len = 16;
        g_stub.snap.trk[k].div = 2;
    }
    for (uint32_t k = 0; k < STUB_ENGINE_TRACKS; k++) {
        for (uint32_t i = 0; i < STUB_ENGINE_STEPS; i++)
            g_stub.step[k][i].time = 2; /* ST_REST：上游空合成步 */
    }
}

void stub_engine_set_param(uint32_t track, uint32_t id, int16_t value)
{
    if (track >= 4u || id >= SLOOP_CORE_P_COUNT) return;
    g_stub.param[track][id] = value;
}

void stub_engine_set_global(uint32_t id, int16_t value)
{
    if (id >= SLOOP_CORE_G_COUNT) return;
    g_stub.param_g[id] = value;
}

/* 快照/窗口快照共用的窗口列填充：列 i ← 引擎绝对步 start+i。 */
static void stub_fill_window(sloop_core_snapshot_t *out, uint32_t start)
{
    for (uint32_t k = 0; k < STUB_ENGINE_TRACKS; k++) {
        for (uint32_t i = 0; i < 16; i++) {
            const sloop_step_t *s = &g_stub.step[k][start + i];
            out->step[k][i].n = s->n;
            memcpy(out->step[k][i].note, s->note, sizeof(out->step[k][i].note));
            out->step[k][i].vel = s->vel;
            out->step[k][i].time = s->time;
            out->step[k][i].flags = s->flags;
            out->step[k][i].lvl = s->lvl;
            out->step[k][i].rat = s->rat;
            out->step[k][i].micro = s->micro;
            out->step[k][i].fill = s->fill;
        }
    }
    for (uint32_t i = 0; i < 16; i++) {
        out->dstep[i].on = g_stub.dstep[start + i].on;
        out->dstep[i].lvl = g_stub.dstep[start + i].lvl;
        out->dstep[i].rat = g_stub.dstep[start + i].rat;
    }
}

void sloop_core_read(sloop_core_snapshot_t *out)
{
    g_stub.read_calls++;
    *out = g_stub.snap;
    stub_fill_window(out, 0);
}

void sloop_core_read_window(uint32_t start, sloop_core_snapshot_t *out)
{
    g_stub.read_window_calls++;
    if (g_stub_on_read_window != NULL) g_stub_on_read_window(start);
    start &= ~15u;
    if (start > STUB_ENGINE_STEPS - 16u) start = STUB_ENGINE_STEPS - 16u;
    *out = g_stub.snap;
    stub_fill_window(out, start);
}

void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out)
{
    g_stub.read_step_calls++;
    memset(out, 0, sizeof(*out));
    if (track >= STUB_ENGINE_TRACKS || idx >= STUB_ENGINE_STEPS) return;
    *out = g_stub.step[track][idx];
}

void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in)
{
    g_stub.write_step_calls++;
    if (track >= STUB_ENGINE_TRACKS || idx >= STUB_ENGINE_STEPS) return;
    g_stub.step[track][idx] = *in;
}

void sloop_core_set_global(uint32_t id, int16_t value)
{
    g_stub.set_global_calls++;
    g_stub.last_global.id = id;
    g_stub.last_global.value = value;
    if (id >= SLOOP_CORE_G_COUNT) return; /* 越界与真引擎同口径：不写 */
    g_stub.param_g[id] = value;           /* 参数投影后端（GP 钳位不在 stub 内模拟） */
    if (id == 0) g_stub.snap.bpm = (uint16_t)value; /* G_BPM：快照 bpm 与 g[0] 同源 */
}

void sloop_core_play(bool on)
{
    g_stub.play_calls++;
    g_stub.last_play = on;
    g_stub.snap.playing = on;
}

void sloop_core_note(uint32_t track, uint32_t note, bool on)
{
    g_stub.note_calls++;
    g_stub.last_note.track = track;
    g_stub.last_note.note = note;
    g_stub.last_note.on = on;
}

/* 三个混音写入口：与真引擎同语义——写进快照后端，随后 state_sync_from_engine
 * 经 sloop_core_read 读到的就是这些值（sync 不再有状态层例外可依赖）。 */
void sloop_core_set_mute(uint32_t k, bool mute)
{
    g_stub.set_mute_calls++;
    g_stub.last_mute.track = k;
    g_stub.last_mute.mute = mute;
    if (k >= 4u) return;
    g_stub.snap.trk[k].mute = mute ? 1u : 0u;
}

void sloop_core_set_solo(uint8_t mask)
{
    g_stub.set_solo_calls++;
    g_stub.last_solo_mask = mask;
    mask &= 0x0Fu;
    for (uint32_t k = 0; k < 4u; k++) g_stub.snap.trk[k].solo = (mask >> k) & 1u;
}

void sloop_core_select(uint32_t k)
{
    g_stub.select_calls++;
    g_stub.last_select = k;
    if (k >= 4u) return;
    g_stub.snap.selected_track = (uint8_t)k;
}

/* 简化的描述符钳位（模拟真引擎；真实范围由设备 PARAM/ENG DESC 门禁覆盖）：
 * id < P_E0 → 0..127（TP 多数为 0..127 类），id == P_E0（鼓 kit 槽）→ 0..45
 * （与上游 DRUM_KIT_DESC 一致），P_E0+1..P_E7 → 0..127。越界 id 逐条忽略、
 * 不中止整批（与真引擎同语义）。 */
static void stub_clamp_param(uint32_t id, int16_t *v)
{
    int16_t lo = 0, hi = 127;
    if (id == STUB_ENGINE_P_E0) hi = 45; /* DRUM_KIT_DESC：kit 0..45 */
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
}

void sloop_core_set_params(uint32_t track, const uint8_t *ids, const int16_t *values, uint32_t n)
{
    g_stub.set_params_calls++;
    g_stub.last_params_track = track;
    g_stub.last_params_n = n;
    if (track >= 4u || ids == NULL || values == NULL) return;
    for (uint32_t i = 0; i < n && i < STUB_ENGINE_PARAM_BATCH_MAX; i++) {
        g_stub.last_param_ids[i] = ids[i];
        g_stub.last_param_values[i] = values[i];
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = ids[i];
        if (id >= SLOOP_CORE_P_COUNT) {
            g_stub.invalid_param_ids++;
            continue;
        }
        int16_t v = values[i];
        stub_clamp_param(id, &v);
        g_stub.param[track][id] = v;
    }
}

void sloop_core_read_params(uint32_t track, int16_t *p, int16_t *g)
{
    g_stub.read_params_calls++;
    if (p != NULL) {
        if (track < 4u) memcpy(p, g_stub.param[track], sizeof(g_stub.param[track]));
        else memset(p, 0, sizeof(g_stub.param[track]));
    }
    if (g != NULL) memcpy(g, g_stub.param_g, sizeof(g_stub.param_g));
}

static void stub_clear_step(uint32_t track, uint32_t step)
{
    if (track < STUB_ENGINE_TRACKS) {
        memset(&g_stub.step[track][step], 0, sizeof(g_stub.step[track][step]));
        g_stub.step[track][step].time = 2;
        g_stub.lock_present[track][step] = 0;
        for (uint32_t i = step + 1u; i < 16u; i++) {
            sloop_step_t *next = &g_stub.step[track][i];
            if (next->time != 1u || next->n != 0u) break;
            int8_t micro = next->micro;
            uint8_t fill = next->fill;
            memset(next, 0, sizeof(*next));
            next->time = 2;
            next->micro = micro;
            next->fill = fill;
        }
    } else if (track == 3u) {
        memset(&g_stub.raw_dstep[step], 0, sizeof(g_stub.raw_dstep[step]));
        memset(&g_stub.dstep[step], 0, sizeof(g_stub.dstep[step]));
        g_stub.lock_present[track][step] = 0;
    }
}

static uint8_t stub_drop2(uint8_t value, uint32_t index)
{
    return (uint8_t)((value & ((1u << (2u * index)) - 1u)) |
                     ((value >> (2u * index + 2u)) << (2u * index)));
}

static uint8_t stub_drop_cont(uint8_t flags, uint32_t index)
{
    uint32_t cont = (flags & 0x78u) >> 3u;
    cont = (cont & ((1u << index) - 1u)) | ((cont >> (index + 1u)) << index);
    return (uint8_t)((flags & ~0x78u) | (cont << 3u));
}

bool sloop_seq_frozen(void)
{
    return g_stub.seq_frozen;
}

esp_err_t sloop_seq_step_edit(const sloop_step_edit_t *cmd)
{
    g_stub.seq_step_edit_calls++;
    if (cmd != NULL) g_stub.last_step_edit = *cmd;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (cmd == NULL || cmd->track >= 4u || cmd->step >= STUB_ENGINE_STEPS)
        return ESP_ERR_INVALID_ARG;
    if (cmd->op == SEQ_STEP_CLEAR) {
        stub_clear_step(cmd->track, cmd->step);
        return ESP_OK;
    }
    if (cmd->op != SEQ_STEP_NOTE_DEL || cmd->track >= STUB_ENGINE_TRACKS) return ESP_OK;
    sloop_step_t *st = &g_stub.step[cmd->track][cmd->step];
    if (cmd->index >= st->n) return ESP_ERR_INVALID_ARG;
    uint32_t n = st->n;
    for (uint32_t i = cmd->index; i + 1u < n; i++) st->note[i] = st->note[i + 1u];
    st->note[n - 1u] = 0;
    st->lvl = stub_drop2(st->lvl, cmd->index);
    st->rat = stub_drop2(st->rat, cmd->index);
    st->flags = stub_drop_cont(st->flags, cmd->index);
    st->n = (uint8_t)(n - 1u);
    if (st->n == 0u) stub_clear_step(cmd->track, cmd->step);
    return ESP_OK;
}

/* 按音高找 index 并删除（真引擎在同一把引擎锁内完成查找+删除；替身只模拟可观察
 * 结果，锁纪律由 seq-host-test 对真实上游断言）。 */
esp_err_t sloop_seq_note_del(uint32_t track, uint32_t step, uint32_t note)
{
    g_stub.note_del_calls++;
    g_stub.note_del_last_track = track;
    g_stub.note_del_last_step = step;
    g_stub.note_del_last_note = note;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    if (g_stub.note_del_ret != ESP_OK) return g_stub.note_del_ret;
    if (track >= STUB_ENGINE_TRACKS || step >= STUB_ENGINE_STEPS || note == 0u || note > 127u)
        return ESP_ERR_INVALID_ARG;
    sloop_step_t *st = &g_stub.step[track][step];
    uint32_t index = 0, n = st->n > 4u ? 4u : st->n;
    while (index < n && st->note[index] != note) index++;
    if (index == n) return ESP_ERR_NOT_FOUND;
    for (uint32_t i = index; i + 1u < n; i++) st->note[i] = st->note[i + 1u];
    st->note[n - 1u] = 0;
    st->lvl = stub_drop2(st->lvl, index);
    st->rat = stub_drop2(st->rat, index);
    st->flags = stub_drop_cont(st->flags, index);
    st->n = (uint8_t)(n - 1u);
    if (st->n == 0u) stub_clear_step(track, step);
    return ESP_OK;
}

esp_err_t sloop_seq_pattern(uint32_t track, sloop_pattern_op_t op, int32_t arg)
{
    g_stub.seq_pattern_calls++;
    g_stub.last_pattern_track = track;
    g_stub.last_pattern_op = op;
    g_stub.last_pattern_arg = arg;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (track >= 4u) return ESP_ERR_INVALID_ARG;
    if (op == SEQ_PATTERN_CLEAR) {
        for (uint32_t i = 0; i < STUB_ENGINE_STEPS; i++) stub_clear_step(track, i);
    }
    return ESP_OK;
}

esp_err_t sloop_seq_set_micro(uint32_t track, uint32_t step, int8_t value)
{
    g_stub.seq_micro_calls++;
    g_stub.last_seq_track = track;
    g_stub.last_seq_step = step;
    g_stub.last_seq_value = value;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (track >= 4u || step >= STUB_ENGINE_STEPS) return ESP_ERR_INVALID_ARG;
    if (track < STUB_ENGINE_TRACKS) g_stub.step[track][step].micro = value;
    return ESP_OK;
}

esp_err_t sloop_seq_set_fill(uint32_t track, uint32_t step, uint8_t value)
{
    g_stub.seq_fill_calls++;
    g_stub.last_seq_track = track;
    g_stub.last_seq_step = step;
    g_stub.last_seq_value = value;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (track >= 4u || step >= STUB_ENGINE_STEPS) return ESP_ERR_INVALID_ARG;
    if (track < STUB_ENGINE_TRACKS) g_stub.step[track][step].fill = value;
    return ESP_OK;
}

esp_err_t sloop_seq_lock_set(uint32_t track, uint32_t step, uint32_t param, int16_t value)
{
    g_stub.seq_lock_set_calls++;
    g_stub.last_seq_track = track;
    g_stub.last_seq_step = step;
    g_stub.last_seq_param = param;
    g_stub.last_seq_value = value;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (track >= 4u || step >= STUB_ENGINE_STEPS || param >= SLOOP_CORE_P_COUNT)
        return ESP_ERR_INVALID_ARG;
    g_stub.lock_present[track][step] = 1;
    return ESP_OK;
}

esp_err_t sloop_seq_lock_del(uint32_t track, uint32_t step, uint32_t param)
{
    g_stub.seq_lock_del_calls++;
    g_stub.last_seq_track = track;
    g_stub.last_seq_step = step;
    g_stub.last_seq_param = param;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (track >= 4u || step >= STUB_ENGINE_STEPS || param > SLOOP_CORE_P_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (param == SLOOP_CORE_P_COUNT) g_stub.lock_present[track][step] = 0;
    return ESP_OK;
}

void sloop_seq_read_dstep(uint32_t step, sloop_dstep_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (step < STUB_ENGINE_STEPS) *out = g_stub.raw_dstep[step];
}

esp_err_t sloop_seq_write_dstep(uint32_t step, const sloop_dstep_t *in)
{
    g_stub.seq_dstep_write_calls++;
    if (in != NULL) g_stub.last_dstep = *in;
    if (g_stub.seq_ret != ESP_OK) return g_stub.seq_ret;
    if (step >= STUB_ENGINE_STEPS || in == NULL) return ESP_ERR_INVALID_ARG;
    g_stub.raw_dstep[step] = *in;
    g_stub.last_seq_step = step;
    uint16_t on = (uint16_t)(in->on[0] | (uint16_t)in->on[1] << 8u);
    g_stub.dstep[step].on = on != 0u;
    g_stub.dstep[step].lvl = 0;
    g_stub.dstep[step].rat = 0;
    for (uint32_t lane = 0; lane < 16u; lane++) {
        if (((on >> lane) & 1u) == 0u) continue;
        uint8_t lvl = (in->lvl[lane / 4u] >> (2u * (lane % 4u))) & 3u;
        uint8_t rat = (in->rat[lane / 4u] >> (2u * (lane % 4u))) & 3u;
        if (lvl > g_stub.dstep[step].lvl) g_stub.dstep[step].lvl = lvl;
        if (rat > g_stub.dstep[step].rat) g_stub.dstep[step].rat = rat;
    }
    return ESP_OK;
}

/* ---- P4-C Task 6：录音 / 走带按钮 / 鼓演奏替身 ------------------------------- */
void sloop_seq_rec_read(sloop_rec_snapshot_t *out)
{
    g_stub.rec_read_calls++;
    if (out == NULL) return;
    *out = g_stub.rec;
}

esp_err_t sloop_seq_rec_toggle(void)
{
    g_stub.rec_toggle_calls++;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    return g_stub.seq_ret;
}

esp_err_t sloop_seq_rec_config(uint8_t quant_off, uint8_t tempo_mode, uint8_t count_mode)
{
    g_stub.rec_config_calls++;
    g_stub.rec_last_quant = quant_off;
    g_stub.rec_last_tempo = tempo_mode;
    g_stub.rec_last_count = count_mode;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    return g_stub.seq_ret;
}

esp_err_t sloop_seq_fill_control(bool held, bool next_bar)
{
    g_stub.fill_control_calls++;
    g_stub.fill_last_held = held ? 1u : 0u;
    g_stub.fill_last_next = next_bar ? 1u : 0u;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    return g_stub.seq_ret;
}

esp_err_t sloop_seq_drum_hit(uint32_t lane, uint32_t velocity)
{
    g_stub.drum_hit_calls++;
    g_stub.drum_last_lane = lane;
    g_stub.drum_last_vel = velocity;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    return g_stub.seq_ret;
}

esp_err_t sloop_seq_play_button(bool down)
{
    g_stub.play_button_calls++;
    g_stub.play_button_last_down = down ? 1u : 0u;
    if (g_stub.seq_frozen) return ESP_ERR_INVALID_STATE;
    return g_stub.seq_ret;
}

/* ---- P4-C Task 8：sections / chain / SONG 替身 ---------------------------------- */
void sloop_seq_song_read(sloop_song_snapshot_t *out)
{
    g_stub.song_read_calls++;
    if (out != NULL) *out = g_stub.song_snap;
}

esp_err_t sloop_seq_section(uint32_t slot, bool store, bool erase)
{
    g_stub.section_calls++;
    g_stub.section_last_slot = slot;
    g_stub.section_last_op = store ? 0u : erase ? 2u : 1u;
    return g_stub.section_ret;
}

esp_err_t sloop_seq_section_request(uint32_t slot)
{
    g_stub.section_calls++;
    g_stub.section_last_slot = slot;
    g_stub.section_last_op = 3u;
    return g_stub.section_ret;
}

esp_err_t sloop_seq_chain_begin(uint32_t first)
{
    g_stub.chain_begin_calls++;
    g_stub.chain_last_first = first;
    return g_stub.chain_ret;
}

esp_err_t sloop_seq_chain_commit(const uint8_t *scenes, uint32_t count)
{
    g_stub.chain_commit_calls++;
    g_stub.chain_last_count = count;
    if (scenes != NULL)
        for (uint32_t i = 0; i < count && i < sizeof g_stub.chain_last_scenes; i++)
            g_stub.chain_last_scenes[i] = scenes[i];
    return g_stub.chain_ret;
}

esp_err_t sloop_seq_song_mode(bool enabled)
{
    g_stub.song_mode_calls++;
    g_stub.song_mode_last = enabled ? 1u : 0u;
    return g_stub.song_ret;
}

esp_err_t sloop_seq_song_rec(void)
{
    g_stub.song_rec_calls++;
    return g_stub.song_ret;
}

/* 歌曲顺序落盘入口（CMD_SONG_SET）：状态层只做透传与结果映射。 */
esp_err_t sloop_store_song_set(const sloop_song_config_t *cfg)
{
    g_stub.song_set_calls++;
    if (cfg != NULL) g_stub.song_set_last = *cfg;
    return g_stub.store_ret;
}

bool sloop_store_song_dirty(void) { return g_stub.song_dirty; }
bool sloop_store_sections_dirty(void) { return g_stub.sections_dirty; }
void sloop_store_song_dirty_mark(void) { g_stub.song_dirty = 1; }
void sloop_store_section_dirty_mark(uint32_t slot)
{
    (void)slot;
    g_stub.sections_dirty = 1;
}

/* 自动存档的活动计时（state_apply 的每个命令）：host 单测不编自动存档，空实现 */
void sloop_store_note_activity(uint32_t now_ms)
{
    (void)now_ms;
}

/* ---- 存储 API 替身（状态层存储命令，P4-B Task 10） --------------------------------
 * 统一返回 g_stub.store_ret 并记录调用；不碰引擎后端。bank_current 用
 * 0xA0 | 选中轨 填充首字节——PUT 的入参缓冲即「用哪条轨的 live patch」的证据。 */
static esp_err_t stub_store_hit(uint32_t kind, uint32_t op, uint32_t slot, uint32_t track)
{
    g_stub.store_calls++;
    if (kind == 0) g_stub.store_proj_calls++;
    else if (kind == 1) g_stub.store_preset_calls++;
    else g_stub.store_bank_calls++;
    g_stub.store_last_op = op;
    g_stub.store_last_slot = slot;
    g_stub.store_last_track = track;
    return g_stub.store_ret;
}

esp_err_t sloop_store_project_save(uint32_t slot) { return stub_store_hit(0, 0, slot, 0xFFu); }
esp_err_t sloop_store_project_load(uint32_t slot) { return stub_store_hit(0, 1, slot, 0xFFu); }
esp_err_t sloop_store_project_erase(uint32_t slot) { return stub_store_hit(0, 2, slot, 0xFFu); }

esp_err_t sloop_store_preset_store(uint32_t slot) { return stub_store_hit(1, 0, slot, 0xFFu); }
esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track)
{
    return stub_store_hit(1, 1, slot, track);
}
esp_err_t sloop_store_preset_erase(uint32_t slot) { return stub_store_hit(1, 2, slot, 0xFFu); }

esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed)
{
    g_stub.bank_put_calls++;
    if (packed != NULL) memcpy(g_stub.last_bank_put, packed, sizeof(g_stub.last_bank_put));
    return stub_store_hit(2, 0, slot, 0xFFu); /* op 0 = save/PUT */
}
esp_err_t sloop_store_bank_erase(uint32_t slot) { return stub_store_hit(2, 2, slot, 0xFFu); }

void sloop_store_bank_current(uint8_t *packed)
{
    g_stub.bank_current_calls++;
    if (packed == NULL) return;
    memset(packed, 0, SLOOP_STORE_BANK_PACKED);
    packed[0] = (uint8_t)(0xA0u | g_stub.snap.selected_track);
}
