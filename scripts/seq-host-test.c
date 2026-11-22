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
static uint32_t engine_lock_calls;
static uint32_t engine_unlock_calls;

static void engine_lock(void)
{
    engine_lock_calls++;
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
    s_frozen = false;
    engine_lock_calls = engine_unlock_calls = 0;
}

static const sloop_lock_t *find_lock(const sloop_lock_t *locks, uint32_t n, uint32_t step, uint32_t param)
{
    for (uint32_t i = 0; i < n; i++)
        if (locks[i].step == step && locks[i].param == param) return &locks[i];
    return NULL;
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

int main(void)
{
    _Static_assert(sizeof(sloop_dstep_t) == sizeof(dstep_t), "public drum step has the upstream packed layout");
    test_dstep();
    test_micro_fill();
    test_locks();
    printf("SEQ HOST TEST %s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
