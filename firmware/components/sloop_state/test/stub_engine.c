#include "stub_engine.h"

#include <string.h>

stub_engine_t g_stub;
void (*g_stub_on_read_window)(uint32_t start);

void stub_engine_reset(void)
{
    memset(&g_stub, 0, sizeof(g_stub));
    g_stub_on_read_window = NULL;
    g_stub.snap.bpm = 90; /* 上游 GP[G_BPM].def */
    for (uint32_t k = 0; k < 4; k++) {
        g_stub.snap.trk[k].len = 16;
        g_stub.snap.trk[k].div = 2;
    }
    for (uint32_t k = 0; k < STUB_ENGINE_TRACKS; k++) {
        for (uint32_t i = 0; i < STUB_ENGINE_STEPS; i++)
            g_stub.step[k][i].time = 2; /* ST_REST：上游空合成步 */
    }
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
    if (id == 0) g_stub.snap.bpm = (uint16_t)value; /* G_BPM */
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
