#include "stub_engine.h"

#include <string.h>

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
    if (id == 0) {
        g_stub.snap.bpm = (uint16_t)value; /* G_BPM */
        g_stub.param_g[0] = value;         /* 参数投影同源 */
    }
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
