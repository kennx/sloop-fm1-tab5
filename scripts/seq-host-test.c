/* SPDX-License-Identifier: GPL-3.0-only
 * P4-C Task 3: Tab5 步/鼓/参数锁适配的同源 host 测试。
 * hostsim.c 提供真实上游 seq.c/drums.c；锁桩只替代 FreeRTOS 互斥并计数。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define main hostsim_main
#include "../third_party/sloop-fm1/tests/hostsim.c"
#undef main

#include "sloop_seq.h"

static bool s_frozen;
static bool freeze_on_lock;
static uint32_t engine_lock_calls;
static uint32_t engine_unlock_calls;

static void engine_lock(void)
{
    engine_lock_calls++;
    if (freeze_on_lock) s_frozen = true;
}

static void engine_unlock(void)
{
    engine_unlock_calls++;
}

#include "../firmware/components/sloop_core/src/seq_tab5.c"

static int fails;

static void check(int ok, const char *what)
{
    printf("seq-host: %-70s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static void reset(void)
{
    host_tracks_init();
    for (uint32_t k = 0; k < NTRK; k++) trk[k].engine = trk[k].eng_req = 0;
    memset(&undo, 0, sizeof(undo));
    s_frozen = false;
    freeze_on_lock = false;
    engine_lock_calls = engine_unlock_calls = 0;
}

static const sloop_lock_t *find_lock(const sloop_lock_t *locks, uint32_t n, uint32_t step, uint32_t param)
{
    for (uint32_t i = 0; i < n; i++)
        if (locks[i].step == step && locks[i].param == param) return &locks[i];
    return NULL;
}

static int fill_locks_to(uint32_t track, uint32_t used, uint32_t source_step, uint32_t target_step)
{
    sloop_lock_t locks[NLOCK];
    for (uint32_t step = 0; step < NSTEP && sloop_seq_lock_list(track, locks, NLOCK) < used; step++) {
        if (step == source_step || step == target_step) continue;
        if (sloop_seq_lock_set(track, step, P_DIST, (int16_t)step) != ESP_OK) return 0;
    }
    return sloop_seq_lock_list(track, locks, NLOCK) == used;
}

static void test_dstep(void)
{
    sloop_dstep_t want = {0}, got;
    dstep_t packed = {0};
    reset();
    for (uint32_t lane = 0; lane < DRUM_LANES; lane++)
        dstep_set(&packed, lane, (lane + 1u) & 3u, (lane * 3u + 1u) & 3u);
    memcpy(&want, &packed, sizeof(want));

    check(sloop_seq_write_dstep(63, &want) == ESP_OK, "raw drum step accepts the last step");
    sloop_seq_read_dstep(63, &got);
    check(memcmp(&got, &want, sizeof(got)) == 0, "all 16 drum lanes preserve on/lvl/rat packing");
    check(dstep_has(&TDRUM->dstep[63], 0) && dstep_has(&TDRUM->dstep[63], 15) &&
              dstep_lvl(&TDRUM->dstep[63], 5) == 2u && dstep_rat(&TDRUM->dstep[63], 14) == 3u,
          "public raw drum write reaches the real upstream packed step");
    check(engine_lock_calls == 2u && engine_unlock_calls == 2u, "raw drum read/write each use the engine lock");
    check(sloop_seq_write_dstep(NSTEP, &want) == ESP_ERR_INVALID_ARG &&
              sloop_seq_write_dstep(0, NULL) == ESP_ERR_INVALID_ARG,
          "raw drum write rejects an invalid step or null input");

    memset(&got, 0xA5, sizeof(got));
    sloop_seq_read_dstep(NSTEP, &got);
    sloop_dstep_t zero = {0};
    check(memcmp(&got, &zero, sizeof(got)) == 0, "invalid raw drum read returns a zeroed step");

    s_frozen = true;
    check(sloop_seq_write_dstep(63, &want) == ESP_ERR_INVALID_STATE, "frozen raw drum write returns invalid state");
    memset(&got, 0xA5, sizeof(got));
    sloop_seq_read_dstep(63, &got);
    check(memcmp(&got, &zero, sizeof(got)) == 0, "frozen raw drum read returns a zeroed step");
}

static void test_micro_fill(void)
{
    reset();
    check(sloop_seq_set_micro(0, 4, -32) == ESP_OK && trk[0].micro[4] == -32,
          "micro accepts -32");
    check(sloop_seq_set_micro(0, 4, 31) == ESP_OK && trk[0].micro[4] == 31,
          "micro accepts 31");
    check(sloop_seq_set_micro(0, 4, -33) == ESP_ERR_INVALID_ARG &&
              sloop_seq_set_micro(0, 4, 32) == ESP_ERR_INVALID_ARG && trk[0].micro[4] == 31,
          "micro rejects -33/32 without changing the step");
    check(sloop_seq_set_micro(NTRK, 4, 0) == ESP_ERR_INVALID_ARG &&
              sloop_seq_set_micro(0, NSTEP, 0) == ESP_ERR_INVALID_ARG,
          "micro rejects invalid track/step");

    for (uint32_t value = 0; value <= 2; value++) {
        check(sloop_seq_set_fill(0, 4, (uint8_t)value) == ESP_OK && step_fill(&trk[0], 4) == value,
              value == 0 ? "fill accepts normal" : value == 1 ? "fill accepts fill-only" : "fill accepts no-fill");
    }
    check(sloop_seq_set_fill(0, 4, 3) == ESP_OK && step_fill(&trk[0], 4) == 3u,
          "fill preserves the upstream two-bit value 3");
    fill_now = 0;
    int plays_without_fill = step_plays(&trk[0], 4);
    fill_now = 1;
    check(plays_without_fill && step_plays(&trk[0], 4), "upstream treats stored fill value 3 as normal");
    check(sloop_seq_set_fill(0, 4, 4) == ESP_ERR_INVALID_ARG &&
              sloop_seq_set_fill(NTRK, 4, 0) == ESP_ERR_INVALID_ARG &&
              sloop_seq_set_fill(0, NSTEP, 0) == ESP_ERR_INVALID_ARG,
          "fill rejects values above 3 and invalid track/step");

    s_frozen = true;
    check(sloop_seq_set_micro(0, 4, 0) == ESP_ERR_INVALID_STATE &&
              sloop_seq_set_fill(0, 4, 0) == ESP_ERR_INVALID_STATE,
          "frozen micro/fill writes return invalid state");
}

static void test_locks(void)
{
    sloop_lock_t locks[NLOCK];
    reset();
    check(sloop_seq_lock_set(0, 3, P_E0, 4) == ESP_OK &&
              sloop_seq_lock_set(0, 3, P_DIST, 90) == ESP_OK,
          "different parameters coexist on one step");
    uint32_t n = sloop_seq_lock_list(0, locks, NLOCK);
    const sloop_lock_t *e0 = find_lock(locks, n, 3, P_E0);
    const sloop_lock_t *dist = find_lock(locks, n, 3, P_DIST);
    check(n == 2u && e0 != NULL && e0->value == 4 && dist != NULL && dist->value == 90,
          "lock list returns both same-step locks and values");

    check(sloop_seq_lock_set(0, 3, P_DIST, 91) == ESP_OK, "same-key lock overwrite succeeds");
    n = sloop_seq_lock_list(0, locks, NLOCK);
    dist = find_lock(locks, n, 3, P_DIST);
    check(n == 2u && dist != NULL && dist->value == 91, "same-key overwrite consumes no additional slot");
    check(sloop_seq_lock_list(0, locks, 1) == 1u, "lock list honors its output capacity");

    uint32_t step = 0;
    while (sloop_seq_lock_list(0, locks, NLOCK) < NLOCK) {
        if (step == 3u) step++;
        check(sloop_seq_lock_set(0, step, P_DIST, (int16_t)(step + 1)) == ESP_OK,
              "lock allocation accepts a unique key up to 24 slots");
        step++;
    }
    check(sloop_seq_lock_set(0, 63, P_DIST, 12) == ESP_ERR_NO_MEM, "the 25th lock returns no memory");
    check(sloop_seq_lock_set(0, 3, P_SLEN, 16) == ESP_ERR_INVALID_ARG &&
              sloop_seq_lock_set(0, 3, P_COUNT, 1) == ESP_ERR_INVALID_ARG &&
              sloop_seq_lock_set(NTRK, 3, P_DIST, 1) == ESP_ERR_INVALID_ARG &&
              sloop_seq_lock_set(0, NSTEP, P_DIST, 1) == ESP_ERR_INVALID_ARG,
          "unlockable parameter and invalid track/step/param are invalid");
    n = sloop_seq_lock_list(0, locks, NLOCK);
    check(n == NLOCK && find_lock(locks, n, 63, P_DIST) == NULL, "a full-table rejection leaves all 24 locks intact");

    check(sloop_seq_lock_del(0, 3, P_E0) == ESP_OK, "single-key lock delete succeeds");
    n = sloop_seq_lock_list(0, locks, NLOCK);
    check(n == NLOCK - 1u && find_lock(locks, n, 3, P_E0) == NULL &&
              find_lock(locks, n, 3, P_DIST) != NULL,
          "single-key delete leaves the other same-step lock");
    check(sloop_seq_lock_del(0, 3, P_COUNT) == ESP_OK, "P_COUNT clears every lock on the step");
    n = sloop_seq_lock_list(0, locks, NLOCK);
    check(find_lock(locks, n, 3, P_DIST) == NULL, "clear-step removes the remaining same-step lock");
    check(sloop_seq_lock_del(0, 3, P_COUNT + 1u) == ESP_ERR_INVALID_ARG &&
              sloop_seq_lock_del(NTRK, 3, P_COUNT) == ESP_ERR_INVALID_ARG &&
              sloop_seq_lock_del(0, NSTEP, P_COUNT) == ESP_ERR_INVALID_ARG,
          "lock delete rejects invalid track/step/param");

    s_frozen = true;
    check(sloop_seq_lock_set(0, 1, P_DIST, 1) == ESP_ERR_INVALID_STATE &&
              sloop_seq_lock_del(0, 1, P_DIST) == ESP_ERR_INVALID_STATE,
          "frozen lock writes return invalid state");
    memset(locks, 0xA5, sizeof(locks));
    check(sloop_seq_lock_list(0, locks, NLOCK) == 0u, "frozen lock list is empty");
}

static esp_err_t edit_step(uint8_t track, uint8_t step, sloop_step_op_t op,
                           uint8_t index, uint8_t target, int16_t value)
{
    sloop_step_edit_t cmd = {track, step, index, target, value, op};
    return sloop_seq_step_edit(&cmd);
}

static void put_note(track_t *t, uint32_t step, uint32_t note, uint32_t ties)
{
    memset(&t->step[step], 0, sizeof(t->step[step]));
    t->step[step].note[0] = (uint8_t)note;
    t->step[step].n = 1;
    t->step[step].time = ST_NOTE;
    for (uint32_t k = 1; k <= ties; k++) {
        memset(&t->step[step + k], 0, sizeof(t->step[step + k]));
        t->step[step + k].time = ST_TIE;
    }
}

static int has_note(const step_t *st, uint32_t note, int cont)
{
    for (uint32_t k = 0; k < st->n && k < 4u; k++)
        if (st->note[k] == note && !!(st->flags & SF_CONT(k)) == cont) return 1;
    return 0;
}

static void make_melody(track_t *t)
{
    put_note(t, 0, 36, 1);
    put_note(t, 2, 48, 1);
    put_note(t, 4, 50, 1);
    put_note(t, 6, 52, 1);
}

static void test_complex_span(void)
{
    track_t *t = &trk[0];
    reset();
    host_preset(t, 1, 0);
    t->p[P_SLEN] = 16;
    make_melody(t);
    check(edit_step(0, 0, SEQ_STEP_SPAN, 0, 0, 10) == ESP_OK &&
              has_note(&t->step[2], 36, 1) && has_note(&t->step[2], 48, 0) &&
              has_note(&t->step[4], 36, 1) && has_note(&t->step[4], 50, 0) &&
              has_note(&t->step[6], 36, 1) && has_note(&t->step[6], 52, 0) &&
              has_note(&t->step[8], 36, 1) && t->step[9].time == ST_TIE,
          "C1 spans C2/D2/E2 with CONT and a continuation-only note at step 8");

    track_t before = *t;
    esp_err_t move_ret = edit_step(0, 0, SEQ_STEP_NOTE_MOVE, 0, 1, 0);
    esp_err_t copy_ret = edit_step(0, 0, SEQ_STEP_NOTE_COPY, 0, 10, 0);
    check(move_ret == ESP_ERR_NOT_ALLOWED && copy_ret == ESP_ERR_NOT_ALLOWED &&
              memcmp(&before, t, sizeof(before)) == 0,
          "unsafe CONT move/copy return blocked without changing the track");

    check(edit_step(0, 0, SEQ_STEP_NOTE_DEL, 0, 0, 0) == ESP_OK &&
          t->step[0].time == ST_REST && t->step[0].n == 0 &&
          has_note(&t->step[2], 48, 0) && t->step[2].n == 1 &&
          has_note(&t->step[4], 50, 0) && t->step[4].n == 1 &&
          has_note(&t->step[6], 52, 0) && t->step[6].n == 1 &&
          t->step[8].time == ST_REST && t->step[8].n == 0,
          "deleting C1 removes every CONT residue and preserves C2/D2/E2");
    for (uint32_t i = 0; i < NSTEP; i++)
        check(!has_note(&t->step[i], 36, 0) && !has_note(&t->step[i], 36, 1),
              "deleted C1 has no residual note in the pattern");
}

static void test_note_move_copy(void)
{
    sloop_lock_t locks[NLOCK];
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 2, 60, 1);
    t->micro[2] = -7;
    step_fill_set(t, 2, FC_NOFILL);
    check(sloop_seq_lock_set(0, 2, P_DIST, 77) == ESP_OK, "move fixture has a source lock");
    check(edit_step(0, 2, SEQ_STEP_NOTE_MOVE, 0, 4, 0) == ESP_OK &&
          t->step[2].time == ST_REST && t->step[3].time == ST_REST &&
          has_note(&t->step[4], 60, 0) && t->step[5].time == ST_TIE &&
          t->micro[4] == -7 && t->micro[2] == 0 &&
          step_fill(t, 4) == FC_NOFILL && step_fill(t, 2) == FC_NORM,
          "note move carries its span, micro and fill and clears the source metadata");
    uint32_t n = sloop_seq_lock_list(0, locks, NLOCK);
    check(n == 1u && find_lock(locks, n, 4, P_DIST) != NULL &&
              find_lock(locks, n, 4, P_DIST)->value == 77,
          "note move relocates its parameter lock");

    reset();
    t = &trk[0];
    t->p[P_SLEN] = 16;
    put_note(t, 2, 62, 1);
    t->micro[2] = 11;
    step_fill_set(t, 2, FC_FILL);
    check(sloop_seq_lock_set(0, 2, P_DIST, 88) == ESP_OK, "copy fixture has a source lock");
    check(edit_step(0, 2, SEQ_STEP_NOTE_COPY, 0, 4, 0) == ESP_OK &&
          has_note(&t->step[2], 62, 0) && has_note(&t->step[4], 62, 0) &&
          t->step[3].time == ST_TIE && t->step[5].time == ST_TIE &&
          t->micro[2] == 11 && t->micro[4] == 11 &&
          step_fill(t, 2) == FC_FILL && step_fill(t, 4) == FC_FILL,
          "note copy duplicates its span, micro and fill without clearing the source");
    n = sloop_seq_lock_list(0, locks, NLOCK);
    check(n == 2u && find_lock(locks, n, 2, P_DIST) != NULL &&
              find_lock(locks, n, 4, P_DIST) != NULL,
          "note copy duplicates its parameter lock");
}

static void test_copy_capacity(void)
{
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 2, 62, 1);
    t->micro[2] = 11;
    step_fill_set(t, 2, FC_FILL);
    check(sloop_seq_lock_set(0, 2, P_DIST, 88) == ESP_OK && fill_locks_to(0, NLOCK, 2, 4),
          "full-table copy fixture reaches 24 locks");
    track_t before = *t;
    esp_err_t copy_ret = edit_step(0, 2, SEQ_STEP_NOTE_COPY, 0, 4, 0);
    check(copy_ret == ESP_ERR_NO_MEM, "copy with a full lock table returns no memory");
    check(memcmp(&before, t, sizeof(before)) == 0,
          "full-table copy rejection leaves the whole track unchanged");

    reset();
    t = &trk[0];
    t->p[P_SLEN] = 16;
    put_note(t, 2, 64, 1);
    t->micro[2] = -5;
    step_fill_set(t, 2, FC_NOFILL);
    check(sloop_seq_lock_set(0, 2, P_DIST, 91) == ESP_OK &&
              sloop_seq_lock_set(0, 2, P_E0, 7) == ESP_OK && fill_locks_to(0, NLOCK - 1u, 2, 4),
          "insufficient-capacity copy fixture has two source locks and one free slot");
    before = *t;
    copy_ret = edit_step(0, 2, SEQ_STEP_NOTE_COPY, 0, 4, 0);
    check(copy_ret == ESP_ERR_NO_MEM, "copy preflight rejects two locks with one free slot");
    check(memcmp(&before, t, sizeof(before)) == 0,
          "insufficient-capacity copy rejection preserves the whole track");
}

static void test_occupied_target(void)
{
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 0, 60, 0);
    put_note(t, 1, 64, 0);
    t->micro[0] = 6;
    step_fill_set(t, 0, FC_FILL);
    check(sloop_seq_lock_set(0, 0, P_DIST, 66) == ESP_OK, "occupied-target fixture has a source lock");
    track_t before = *t;
    esp_err_t move_ret = edit_step(0, 0, SEQ_STEP_NOTE_MOVE, 0, 1, 0);
    esp_err_t copy_ret = edit_step(0, 0, SEQ_STEP_NOTE_COPY, 0, 1, 0);
    check(move_ret == ESP_ERR_NOT_ALLOWED && copy_ret == ESP_ERR_NOT_ALLOWED &&
              memcmp(&before, t, sizeof(before)) == 0,
          "occupied move/copy targets return blocked without changing the track");
}

static void test_step_fields_clear(void)
{
    sloop_lock_t locks[NLOCK];
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 4, 60, 0);
    t->step[4].note[1] = 64;
    t->step[4].n = 2;
    check(edit_step(0, 4, SEQ_STEP_LVL, 1, 0, LV_HARD) == ESP_OK &&
              ((t->step[4].lvl >> 2) & 3u) == LV_HARD &&
              edit_step(0, 4, SEQ_STEP_RAT, 1, 0, 3) == ESP_OK &&
              ((t->step[4].rat >> 2) & 3u) == 3u &&
              edit_step(0, 4, SEQ_STEP_SLIDE, 0, 0, 1) == ESP_OK &&
              (t->step[4].flags & SF_SLIDE) != 0 &&
              edit_step(0, 4, SEQ_STEP_SLIDE, 0, 0, 0) == ESP_OK &&
              (t->step[4].flags & SF_SLIDE) == 0,
          "step edit writes per-note lvl/rat and toggles slide");
    check(edit_step(0, 4, SEQ_STEP_LVL, 2, 0, LV_HARD) == ESP_ERR_INVALID_ARG &&
              edit_step(0, 4, SEQ_STEP_RAT, 0, 0, 4) == ESP_ERR_INVALID_ARG &&
              edit_step(0, 4, SEQ_STEP_SLIDE, 0, 0, 2) == ESP_ERR_INVALID_ARG,
          "step field edits reject an absent note index and out-of-range values");
    t->micro[4] = 13;
    step_fill_set(t, 4, FC_NOFILL);
    check(sloop_seq_lock_set(0, 4, P_DIST, 91) == ESP_OK, "step clear fixture has a lock");
    check(edit_step(0, 4, SEQ_STEP_CLEAR, 0, 0, 0) == ESP_OK &&
          t->step[4].time == ST_REST && t->step[4].n == 0 && t->micro[4] == 0 &&
          step_fill(t, 4) == FC_NORM && sloop_seq_lock_list(0, locks, NLOCK) == 0 &&
          t->p[P_SLEN] == 16,
          "step clear removes notes, lock, micro and fill without changing LEN");
}

static void test_pattern_rotate_length(void)
{
    sloop_lock_t locks[NLOCK];
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 4;
    put_note(t, 0, 60, 0);
    put_note(t, 3, 64, 0);
    t->micro[0] = 5;
    t->micro[3] = -4;
    step_fill_set(t, 0, FC_FILL);
    step_fill_set(t, 3, FC_NOFILL);
    check(sloop_seq_lock_set(0, 0, P_DIST, 10) == ESP_OK &&
              sloop_seq_lock_set(0, 3, P_DIST, 30) == ESP_OK,
          "rotate fixture has anchored locks");
    int8_t micro[NSTEP];
    uint8_t fill[NSTEP / 4];
    plock_t lock[NLOCK];
    step_t before[NSTEP];
    memcpy(micro, t->micro, sizeof(micro));
    memcpy(fill, t->fill, sizeof(fill));
    memcpy(lock, t->lock, sizeof(lock));
    check(sloop_seq_pattern(0, SEQ_PATTERN_ROTATE, 1) == ESP_OK &&
          has_note(&t->step[0], 64, 0) && has_note(&t->step[1], 60, 0) &&
          t->step[3].time == ST_REST &&
          memcmp(micro, t->micro, sizeof(micro)) == 0 &&
          memcmp(fill, t->fill, sizeof(fill)) == 0 && memcmp(lock, t->lock, sizeof(lock)) == 0,
          "rotate wraps the last step to the first while micro/fill/locks stay anchored");
    check(sloop_seq_lock_list(0, locks, NLOCK) == 2u &&
              find_lock(locks, 2, 0, P_DIST)->value == 10 && find_lock(locks, 2, 3, P_DIST)->value == 30,
          "rotate leaves lock steps and values unchanged");

    reset();
    t = &trk[0];
    t->p[P_SLEN] = 16;
    for (uint32_t i = 0; i < 16; i++) put_note(t, i, 40 + i, 0);
    t->micro[0] = 7;
    t->micro[20] = -9;
    step_fill_set(t, 0, FC_FILL);
    step_fill_set(t, 20, FC_NOFILL);
    memcpy(micro, t->micro, sizeof(micro));
    memcpy(fill, t->fill, sizeof(fill));
    memcpy(lock, t->lock, sizeof(lock));
    check(sloop_seq_pattern(0, SEQ_PATTERN_LENGTH, 1) == ESP_OK && t->p[P_SLEN] == 32 &&
              memcmp(&t->step[16], &t->step[0], 16 * sizeof(step_t)) == 0 &&
              memcmp(micro, t->micro, sizeof(micro)) == 0 &&
              memcmp(fill, t->fill, sizeof(fill)) == 0 && memcmp(lock, t->lock, sizeof(lock)) == 0,
          "LEN 16 to 32 copies only step data and anchors micro/fill/locks");

    reset();
    t = &trk[0];
    t->p[P_SLEN] = 3;
    for (uint32_t i = 0; i < 6; i++) put_note(t, i, 50 + i, 0);
    memcpy(before, t->step, sizeof(before));
    check(sloop_seq_pattern(0, SEQ_PATTERN_LENGTH, -1) == ESP_OK && t->p[P_SLEN] == 1 &&
              memcmp(before, t->step, sizeof(before)) == 0,
          "LEN 3 to 1 uses upstream integer halving and does not move steps");
}

static void test_pattern_transpose_clear_undo(void)
{
    sloop_lock_t locks[NLOCK];
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 0, 0, 0);
    put_note(t, 1, 127, 0);
    put_note(t, 63, 60, 0);
    check(sloop_seq_pattern(0, SEQ_PATTERN_TRANSPOSE, 1) == ESP_OK &&
          t->step[0].note[0] == 1 && t->step[1].note[0] == 127 && t->step[63].note[0] == 61,
          "transpose saturates at 127 and scans step 63 beyond LEN");
    reset();
    t = &trk[0];
    put_note(t, 0, 0, 0);
    put_note(t, 1, 127, 0);
    check(sloop_seq_pattern(0, SEQ_PATTERN_TRANSPOSE, -1) == ESP_OK &&
          t->step[0].note[0] == 0 && t->step[1].note[0] == 126,
          "transpose saturates at zero");
    put_note(&trk[3], 0, 36, 0);
    step_t drum_before = trk[3].step[0];
    check(sloop_seq_pattern(3, SEQ_PATTERN_TRANSPOSE, 1) == ESP_ERR_INVALID_ARG &&
              memcmp(&drum_before, &trk[3].step[0], sizeof(drum_before)) == 0,
          "transpose rejects the drum track without changing it");

    reset();
    t = &trk[0];
    t->p[P_SLEN] = 16;
    put_note(t, 0, 60, 1);
    t->micro[0] = 9;
    t->micro[1] = -3;
    step_fill_set(t, 0, FC_NOFILL);
    step_fill_set(t, 1, FC_FILL);
    check(sloop_seq_lock_set(0, 0, P_DIST, 44) == ESP_OK &&
              sloop_seq_lock_set(0, 1, P_E0, 5) == ESP_OK,
          "clear/undo fixture has locks on both steps");
    step_t saved[NSTEP];
    memcpy(saved, t->step, sizeof(saved));
    check(sloop_seq_pattern(0, SEQ_PATTERN_CLEAR, 0) == ESP_OK && t->p[P_SLEN] == 16 &&
              t->step[0].time == ST_REST && t->step[1].time == ST_REST &&
              t->micro[0] == 0 && t->micro[1] == 0 && step_fill(t, 0) == FC_NORM &&
              step_fill(t, 1) == FC_NORM && sloop_seq_lock_list(0, locks, NLOCK) == 0,
          "pattern clear removes steps, locks, micro and fill without changing LEN");
    check(sloop_seq_pattern(0, SEQ_PATTERN_UNDO, 0) == ESP_OK &&
              memcmp(saved, t->step, sizeof(saved)) == 0 && t->micro[0] == 9 && t->micro[1] == -3 &&
              t->p[P_SLEN] == 16 && step_fill(t, 0) == FC_NORM && step_fill(t, 1) == FC_NORM &&
              sloop_seq_lock_list(0, locks, NLOCK) == 0,
          "undo restores only steps, micro and LEN, not fill or locks");
    check(sloop_seq_pattern(0, SEQ_PATTERN_REDO, 0) == ESP_OK &&
              t->step[0].time == ST_REST && t->step[1].time == ST_REST &&
              t->micro[0] == 0 && t->micro[1] == 0 && step_fill(t, 0) == FC_NORM &&
              sloop_seq_lock_list(0, locks, NLOCK) == 0,
          "redo swaps the same limited scope back to the cleared pattern");
}

static void test_new_entry_locking(void)
{
    track_t *t = &trk[0];
    reset();
    put_note(t, 0, 60, 0);
    sloop_step_edit_t invalid = {.op = (sloop_step_op_t)-1};
    track_t invalid_before = *t;
    check(sloop_seq_step_edit(NULL) == ESP_ERR_INVALID_ARG &&
              sloop_seq_step_edit(&invalid) == ESP_ERR_INVALID_ARG &&
              sloop_seq_pattern(0, (sloop_pattern_op_t)-1, 0) == ESP_ERR_INVALID_ARG &&
              memcmp(&invalid_before, t, sizeof(invalid_before)) == 0,
          "null and negative operations are rejected without changing the track");
    uint32_t locks = engine_lock_calls;
    check(edit_step(0, 0, SEQ_STEP_SLIDE, 0, 0, 1) == ESP_OK &&
              engine_lock_calls == locks + 1u && engine_unlock_calls == locks + 1u,
          "step edit takes and releases the engine lock exactly once");
    locks = engine_lock_calls;
    check(sloop_seq_pattern(0, SEQ_PATTERN_ROTATE, 1) == ESP_OK &&
              engine_lock_calls == locks + 1u && engine_unlock_calls == locks + 1u,
          "pattern edit takes and releases the engine lock exactly once");

    reset();
    put_note(t, 0, 60, 0);
    step_t before = t->step[0];
    freeze_on_lock = true;
    esp_err_t frozen_step = edit_step(0, 0, SEQ_STEP_SLIDE, 0, 0, 1);
    s_frozen = false;
    esp_err_t frozen_pattern = sloop_seq_pattern(0, SEQ_PATTERN_CLEAR, 0);
    check(frozen_step == ESP_ERR_INVALID_STATE && frozen_pattern == ESP_ERR_INVALID_STATE &&
              memcmp(&before, &t->step[0], sizeof(before)) == 0 && engine_lock_calls == 2u &&
              engine_unlock_calls == 2u,
          "step/pattern entries recheck freeze after locking and leave data unchanged");
}

int main(void)
{
    _Static_assert(sizeof(sloop_dstep_t) == sizeof(dstep_t), "public drum step has the upstream packed layout");
    test_dstep();
    test_micro_fill();
    test_locks();
    test_complex_span();
    test_note_move_copy();
    test_copy_capacity();
    test_occupied_target();
    test_step_fields_clear();
    test_pattern_rotate_length();
    test_pattern_transpose_clear_undo();
    test_new_entry_locking();
    printf("SEQ HOST TEST %s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
