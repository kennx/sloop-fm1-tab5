/* sloop_state 的 EditCmd 分发：执行端 = 本地引擎（sloop_core）。
 * 阶段 5 联动模式替换本文件的分发目标（FM-1 会话），AppState 与接口不变。 */
#include "state_internal.h"

#include <string.h>

/* 上游常量镜像（本组件不包含上游 core.h：host 单测不编译上游源码）：
 * core.h 的 enum { G_BPM, ... } → G_BPM = 0；params.c 的 GP[G_BPM] 范围 40..240；
 * core.h 的 enum { ST_NOTE, ST_TIE, ST_REST }；NSTEP = 步表长度 64。 */
#define STATE_G_BPM 0u
#define STATE_BPM_MIN 40
#define STATE_BPM_MAX 240
#define STATE_ST_NOTE 0u
#define STATE_ST_REST 2u
#define STATE_STEPS_MAX 64u
#define STATE_SYNTH_TRACKS 3u /* read_step/write_step 只覆盖合成轨（鼓轨只读） */
#define STATE_NOTE_MAX 127u

static void cmd_note(app_state_t *s, uint32_t track, uint16_t note, bool on)
{
    if (track >= APP_STATE_TRACKS || note > STATE_NOTE_MAX) return;
    sloop_core_note(track, note, on);
    /* last_note 用 0 表示「还没弹过」：note 0 照常发声，但不记忆（见 sloop_state.h） */
    if (on && note != 0) s->last_note = (uint8_t)note;
}

static void cmd_play(app_state_t *s, bool on)
{
    sloop_core_play(on);
    s->core.playing = on;
}

static void cmd_select_track(app_state_t *s, uint32_t track)
{
    if (track >= APP_STATE_TRACKS) return;
    s->core.selected_track = (uint8_t)track;
    s->focus_track = (uint8_t)track;
}

static void cmd_toggle_step(app_state_t *s, uint32_t track, uint32_t idx)
{
    if (track >= STATE_SYNTH_TRACKS || idx >= STATE_STEPS_MAX) return;

    sloop_step_t st;
    sloop_core_read_step(track, idx, &st);
    /* 「有音」与上游 sp_on 同口径（seq.c）：time == NOTE 且 n > 0；TIE/REST 都算空 */
    bool on = st.n > 0 && st.time == STATE_ST_NOTE;
    if (!on) {
        memset(st.note, 0, sizeof(st.note));
        st.note[0] = s->last_note != 0 ? s->last_note : APP_STATE_NOTE_DEFAULT;
        st.n = 1;
        st.vel = APP_STATE_VEL_DEFAULT;
        st.time = STATE_ST_NOTE;
        st.flags = 0;
    } else {
        st.n = 0;
        st.time = STATE_ST_REST;
    }
    sloop_core_write_step(track, idx, &st); /* lvl/rat/micro/fill 原样带回 */

    if (idx < APP_STATE_STEPS) { /* 快照只覆盖前 16 列视窗 */
        s->core.step[track][idx].n = st.n;
        memcpy(s->core.step[track][idx].note, st.note, sizeof(st.note));
        s->core.step[track][idx].vel = st.vel;
        s->core.step[track][idx].time = st.time;
        s->core.step[track][idx].flags = st.flags;
    }
}

static void cmd_set_bpm(app_state_t *s, uint16_t bpm)
{
    int32_t v = bpm;
    if (v < STATE_BPM_MIN) v = STATE_BPM_MIN;
    if (v > STATE_BPM_MAX) v = STATE_BPM_MAX;
    sloop_core_set_global(STATE_G_BPM, (int16_t)v);
    s->core.bpm = (uint16_t)v;
}

static void cmd_track_mix(app_state_t *s, edit_cmd_type_t type, uint32_t track)
{
    if (track >= APP_STATE_TRACKS) return;
    /* 引擎无 mute/solo 写入口：MVP 由本层持有（见 sloop_state.h 的 MVP 约定） */
    if (type == CMD_MUTE) s->core.trk[track].mute ^= 1u;
    else s->core.trk[track].solo ^= 1u;
}

void state_apply(const edit_cmd_t *cmd)
{
    if (cmd == NULL) return;
    state_lock();
    app_state_t *s = state_locked();
    switch (cmd->type) {
    case CMD_NOTE_ON: cmd_note(s, cmd->a, cmd->v, true); break;
    case CMD_NOTE_OFF: cmd_note(s, cmd->a, cmd->v, false); break;
    case CMD_PLAY: cmd_play(s, true); break;
    case CMD_STOP: cmd_play(s, false); break;
    case CMD_SELECT_TRACK: cmd_select_track(s, cmd->a); break;
    case CMD_TOGGLE_STEP: cmd_toggle_step(s, cmd->a, cmd->b); break;
    case CMD_SET_BPM: cmd_set_bpm(s, cmd->v); break;
    case CMD_SET_TRACK: cmd_select_track(s, cmd->a); break;
    case CMD_MUTE:
    case CMD_SOLO: cmd_track_mix(s, cmd->type, cmd->a); break;
    default: break;
    }
    state_unlock();
}
