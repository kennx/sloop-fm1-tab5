#include "stub_engine.h"

#include <string.h>

stub_engine_t g_stub;

void stub_engine_reset(void)
{
    memset(&g_stub, 0, sizeof(g_stub));
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

void sloop_core_read(sloop_core_snapshot_t *out)
{
    g_stub.read_calls++;
    *out = g_stub.snap;
    for (uint32_t k = 0; k < STUB_ENGINE_TRACKS; k++) {
        for (uint32_t i = 0; i < 16; i++) {
            const sloop_step_t *s = &g_stub.step[k][i];
            out->step[k][i].n = s->n;
            memcpy(out->step[k][i].note, s->note, sizeof(out->step[k][i].note));
            out->step[k][i].vel = s->vel;
            out->step[k][i].time = s->time;
            out->step[k][i].flags = s->flags;
        }
    }
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
