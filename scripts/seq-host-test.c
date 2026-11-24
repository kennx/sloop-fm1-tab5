/* SPDX-License-Identifier: GPL-3.0-only
 * P4-C Task 3: Tab5 步/鼓/参数锁适配的同源 host 测试。
 * hostsim.c 提供真实上游 seq.c/drums.c；锁桩只替代 FreeRTOS 互斥并计数。
 */
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* P4-C Task 8：arranger/live sections/quick chain/SONG 是 seq.c 的 FELUCCA_ARRANGER
 * 段，场景应用在 arranger_scene.c（消费 project.c 的 proj_slot/proj_apply）。与上游
 * song_audio_test 同序编入：hostsim(FELUCCA_ARRANGER=1) → project.c(PROJ_HOST) →
 * arranger_scene.c。 */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "../third_party/sloop-fm1/tests/hostsim.c"
#undef main

/* P4-F Task 6：固件 store_tab5.c 的「载入声音」机制同源编译（TRK_DEF 现在是设备映射
 * {ANALOG 0, DIGITAL 1, SAMPLE 4}——与 synth-host-test 相同；project.c 的 FUN1 导入用它，
 * 本文件不测 FUN1）。用户预设银行（upreset.c）不在本可执行文件里：t->user 恒 0，
 * user_of/arp_is_sounds 走不到这两个查询，替身只为满足链接。 */
static uint8_t sync_reload;
static struct { uint8_t force; uint8_t rsv[3]; } ui;
static int up_used(uint32_t k) { (void)k; return 0; }
static int up_arp_of(uint32_t k, int16_t *a) { (void)k; (void)a; return 1; }
#include "../firmware/components/sloop_core/src/preset_tab5.h"

#define PROJ_HOST 1
/* 与固件 store_tab5.c 同法：proj_slot 退化为指针（PSRAM 槽），init 之前指针可以为
 * NULL/垃圾。no-storage 用例把指针置 NULL——没有 sloop_store_ready() 门的实现会在这里
 * 段错误，而不是在设备上偶发 panic。 */
#define proj_slot (*h_slot_ps)
#include "../third_party/sloop-fm1/firmware/src/project.c"
#undef PROJ_HOST
static project_t h_slot_mem[4];
#include "../third_party/sloop-fm1/firmware/src/arranger_scene.c"

#include "sloop_core.h"
#include "sloop_seq.h"
#include "sloop_store.h"

/* ---- Task 8 的存储替身（真实分区/flash 不在本可执行文件里）---------------------
 * section 的 RAM 捕获在 seq_tab5.c（引擎锁内），这里只提供 dirty 记账与
 * load/erase（真实实现在 store_tab5.c，设备 smoke 覆盖 flash 侧）。 */
static uint8_t h_sec_dirty, h_song_dirty;
static bool h_store_ready = true;

bool sloop_store_ready(void) { return h_store_ready; }
bool sloop_store_sections_dirty(void) { return h_sec_dirty != 0; }
bool sloop_store_song_dirty(void) { return h_song_dirty != 0; }
void sloop_store_song_dirty_mark(void) { h_song_dirty = 1; }
void sloop_store_section_dirty_mark_locked(uint32_t slot)
{
    if (slot < 4u) h_sec_dirty |= (uint8_t)(1u << slot);
}

esp_err_t sloop_store_section_load(uint32_t slot)
{
    if (slot >= 4u) return ESP_ERR_INVALID_ARG;
    if (song.playing || transport_req) return ESP_ERR_NOT_ALLOWED;
    project_t *p = &proj_slot[slot];
    if (!proj_ok(p)) return ESP_ERR_NOT_FOUND;
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    proj_apply(p, 1);
    song.sel = (uint8_t)(p->sel < NTRK ? p->sel : 0u);
    live_sec = (int8_t)slot;
    return ESP_OK;
}

esp_err_t sloop_store_section_erase(uint32_t slot)
{
    if (slot >= 4u) return ESP_ERR_INVALID_ARG;
    if (song.playing || transport_req) return ESP_ERR_NOT_ALLOWED;
    if (!proj_ok(&proj_slot[slot])) return ESP_ERR_NOT_FOUND;
    memset(&proj_slot[slot], 0, sizeof proj_slot[slot]);
    h_sec_dirty &= (uint8_t)~(1u << slot);
    if (live_sec == (int8_t)slot) live_sec = -1;
    if (arr_drop_scene(&arrangement, slot)) h_song_dirty = 1;
    return ESP_OK;
}

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
    /* 录音/走带/自由录音的上游运行态（seq.c 的 static）：每个用例从干净的停走带开始 */
    song.playing = 0;
    song.rec = 0;
    song.sel = 0;
    transport_req = 0;
    rec_wait = 0;
    rec_go = 0;
    ci_on = 0;
    ci_beat = 0;
    ci_u = 0;
    rec_quant_off = 0;
    rec_tempo = 0;
    rec_count = 0;
    ft_on = 0;
    ft_n = 0;
    ft_t = 0;
    ft_trk = 0;
    ft_bars = 0;
    ft_closed = 0;
    ft_close_ms = 0;
    ft_btn_prev = 0;
    ft_btn_mask = 0;
    ft_drop_mask = 0;
    fm1_in.buttons = 0;
    fm1_in.notes = 0;
    fill_held = 0;
    fill_arm = 0;
    fill_bar_on = 0;
    fill_now = 0;
    /* P4-C Task 8：arranger / live sections / chain / SONG REC 的上游运行态 */
    arr_defaults(&arrangement);
    arrangement_enabled = 0;
    memset(&arrangement_clock, 0, sizeof arrangement_clock);
    live_req = -1;
    live_sec = -1;
    live_bar = 0xFFFFFFFFu;
    chain_n = chain_i = chain_bars = 0;
    memset((void *)chain_sec, 0, sizeof chain_sec);
    srec = srec_n = srec_done = 0;
    memset(srec_e, 0, sizeof srec_e);
    clk_beat = 0;
    clk_pos = 0;
    h_sec_dirty = h_song_dirty = 0;
    s_srec_marked = 0;
    h_store_ready = true;
    h_slot_ps = &h_slot_mem;
    memset(proj_slot, 0, sizeof proj_slot);
}

/* 一个音频块（CTL 帧）；fm1_ms 按块推进——ft_owns_press 的 300ms 窗口与上游一样
 * 依赖毫秒时钟（host 的 mix_block 不推进它）。 */
static int32_t blk_out[CTL * 2];
static void blocks(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        fm1_ms++;
        mix_block(blk_out, CTL);
    }
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

static void test_move_blocked_far_target(void)
{
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 2, 60, 0);
    put_note(t, 5, 64, 0);   /* 路径中途的占用：2→8 的第 3 步会撞上它 */
    track_t before = *t;
    check(edit_step(0, 2, SEQ_STEP_NOTE_MOVE, 0, 8, 0) == ESP_ERR_NOT_ALLOWED,
          "far move blocked midway returns blocked");
    check(memcmp(&before, t, sizeof(before)) == 0,
          "far move blocked midway leaves the track unchanged（无半截移动）");
    check(sloop_seq_pattern(0, SEQ_PATTERN_UNDO, 0) == ESP_ERR_NOT_FOUND,
          "blocked far move does not consume an undo mark");
}

static void test_copy_blocked_far_target(void)
{
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 2, 62, 0);
    put_note(t, 7, 65, 0);   /* 拷贝到 8 的路径第 4 步撞上它 */
    track_t before = *t;
    check(edit_step(0, 2, SEQ_STEP_NOTE_COPY, 0, 8, 0) == ESP_ERR_NOT_ALLOWED,
          "far copy blocked midway returns blocked");
    check(memcmp(&before, t, sizeof(before)) == 0,
          "far copy blocked midway leaves the track unchanged（无半截拷贝）");
    check(sloop_seq_pattern(0, SEQ_PATTERN_UNDO, 0) == ESP_ERR_NOT_FOUND,
          "blocked far copy does not consume an undo mark");
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

static void test_note_del_atomic(void)
{
    track_t *t = &trk[0];
    reset();
    t->p[P_SLEN] = 16;
    put_note(t, 3, 60, 0);
    t->step[3].note[1] = 64;
    t->step[3].n = 2;
    uint32_t locks = engine_lock_calls;
    check(sloop_seq_note_del(0, 3, 64) == ESP_OK && t->step[3].n == 1 && t->step[3].note[0] == 60 &&
              engine_lock_calls == locks + 1u,
          "pitch erase finds and deletes the note under one engine lock");
    check(sloop_seq_note_del(0, 3, 64) == ESP_ERR_NOT_FOUND && t->step[3].n == 1,
          "erasing an absent pitch returns not found and leaves the step unchanged");
    check(sloop_seq_note_del(0, 4, 60) == ESP_ERR_NOT_FOUND,
          "erasing from an empty step returns not found");
    check(sloop_seq_note_del(3, 0, 60) == ESP_ERR_INVALID_ARG &&
              sloop_seq_note_del(NTRK, 0, 60) == ESP_ERR_INVALID_ARG &&
              sloop_seq_note_del(0, NSTEP, 60) == ESP_ERR_INVALID_ARG &&
              sloop_seq_note_del(0, 0, 0) == ESP_ERR_INVALID_ARG &&
              sloop_seq_note_del(0, 0, 128) == ESP_ERR_INVALID_ARG,
          "pitch erase rejects the drum track, invalid steps and invalid pitches");
    put_note(&trk[1], 2, 48, 0);
    check(sloop_seq_note_del(1, 2, 48) == ESP_OK && trk[1].step[2].time == ST_REST &&
              trk[1].step[2].n == 0,
          "erasing the last pitch clears the step through the upstream delete");
    s_frozen = true;
    check(sloop_seq_note_del(0, 3, 60) == ESP_ERR_INVALID_STATE && t->step[3].n == 1,
          "frozen pitch erase returns invalid state without touching the step");
}

static void rec_snapshot(sloop_rec_snapshot_t *out)
{
    memset(out, 0xA5, sizeof(*out));
    sloop_seq_rec_read(out);
}

static void test_rec_config(void)
{
    sloop_rec_snapshot_t r;
    reset();
    rec_snapshot(&r);
    check(r.state == SEQ_REC_OFF && r.track == 0 && r.quant_off == 0 && r.tempo_mode == 0 &&
              r.count_mode == 0 && r.take_bars == 0,
          "fresh transport reports REC off with note quant on and no take");
    check(sloop_seq_rec_config(2, 0, 0) == ESP_ERR_INVALID_ARG &&
              sloop_seq_rec_config(0, 2, 0) == ESP_ERR_INVALID_ARG &&
              sloop_seq_rec_config(0, 0, 2) == ESP_ERR_INVALID_ARG,
          "REC configuration accepts only 0/1 modes");
    check(sloop_seq_rec_config(1, 1, 1) == ESP_OK,
          "REC configuration accepts quant off / tempo / count");
    rec_snapshot(&r);
    check(r.quant_off == 1 && r.tempo_mode == 1 && r.count_mode == 1,
          "REC configuration is projected in the snapshot (runtime only, not persisted)");
    s_frozen = true;
    check(sloop_seq_rec_config(0, 0, 0) == ESP_ERR_INVALID_STATE &&
              sloop_seq_rec_toggle() == ESP_ERR_INVALID_STATE &&
              sloop_seq_fill_control(true, false) == ESP_ERR_INVALID_STATE &&
              sloop_seq_play_button(true) == ESP_ERR_INVALID_STATE,
          "frozen diagnostic mode rejects REC / fill / play button entries");
    rec_snapshot(&r);
    check(r.state == SEQ_REC_OFF && r.quant_off == 0 && r.tempo_mode == 0 && r.count_mode == 0,
          "frozen REC snapshot is zeroed and consumes no one-shot");
}

static void test_rec_arm_and_first_note(void)
{
    sloop_rec_snapshot_t r;
    track_t *t = &trk[0];
    reset();
    put_note(&trk[1], 0, 55, 0); /* 非空工程：首音 = 下拍，不是 free take */
    check(sloop_seq_rec_toggle() == ESP_OK, "REC toggles on while stopped");
    rec_snapshot(&r);
    check(r.state == SEQ_REC_ARMED && r.track == 0, "stopped REC arms and waits for a note");

    input_on(t, 60, 100);
    rec_snapshot(&r);
    check(r.state == SEQ_REC_RECORDING && r.track == 0,
          "the first note starts the loop and recording");
    check(song.playing && song.rec == 1u && !rec_wait && !ft_on,
          "the armed first note starts the transport instead of a free take");
    check(t->step[0].time == ST_NOTE && t->step[0].n == 1 && t->step[0].note[0] == 60,
          "the first note is recorded into step 0");
    input_off(t, 60);

    check(sloop_seq_rec_toggle() == ESP_OK && song.playing && song.rec == 0u && !rec_wait,
          "REC off stops recording but keeps the transport playing");
    rec_snapshot(&r);
    check(r.state == SEQ_REC_OFF, "REC off projects the off state");
    check(sloop_seq_rec_toggle() == ESP_OK && song.playing && song.rec == 1u,
          "REC while playing starts recording at once");
    check(sloop_seq_rec_toggle() == ESP_OK && song.rec == 0u, "a second REC press ends the take");

    song.rec = 1u;
    seq_rec_follow_locked(2);
    check(song.rec == 4u, "selecting another track moves the REC bit");
    song.rec = 0;
    seq_rec_follow_locked(1);
    check(song.rec == 0u, "a disarmed song stays disarmed on a track change");
}

static void test_rec_count_in(void)
{
    sloop_rec_snapshot_t r;
    reset();
    put_note(&trk[1], 0, 55, 0);
    check(sloop_seq_rec_config(0, 0, 1) == ESP_OK && sloop_seq_rec_toggle() == ESP_OK,
          "count-in fixture is armed with START=COUNT");
    transport_req = 1; /* API PLAY */
    blocks(1);
    rec_snapshot(&r);
    check(r.state == SEQ_REC_COUNT_IN && song.rec == 0u && !song.playing,
          "API PLAY while armed in COUNT mode runs the count-in first");
    uint32_t n = 0;
    while (ci_on && n < 4000u) {
        blocks(1);
        n++;
    }
    rec_snapshot(&r);
    check(n > 0u && n < 4000u && !ci_on && song.playing && song.rec == 1u &&
              r.state == SEQ_REC_RECORDING,
          "the four-beat count-in ends in the loop and the recording");
}

static uint32_t last_step_with(const track_t *t, uint32_t note)
{
    uint32_t i, k, last = 0;
    for (i = 0; i < (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP); i++)
        for (k = 0; k < t->step[i].n && k < 4u; k++)
            if (t->step[i].note[k] == note) last = i;
    return last;
}

static void test_rec_poly_independent_release(void)
{
    sloop_rec_snapshot_t r;
    uint32_t sb;
    reset();
    put_note(&trk[1], 0, 55, 0); /* 非空工程 */
    trk[0].p[P_SLEN] = 16;
    trk[0].p[P_VOICE] = V_POLY;
    sb = div_samples(2) / CTL;
    check(sloop_seq_rec_toggle() == ESP_OK, "POLY fixture is armed");
    input_on(&trk[0], 60, 100);
    input_on(&trk[0], 64, 100);
    blocks(sb * 2u);
    input_off(&trk[0], 60);
    blocks(sb * 2u);
    input_off(&trk[0], 64);
    blocks(1);
    rec_snapshot(&r);
    check(r.state == SEQ_REC_RECORDING && trk[0].step[0].n == 2,
          "a POLY take records both notes of the chord in step 0");
    uint32_t last60 = last_step_with(&trk[0], 60);
    uint32_t last64 = last_step_with(&trk[0], 64);
    check(last64 >= last60 + 2u,
          "releasing one POLY key ends only its note; the other keeps its length");
}

static void test_free_take_close_and_drop(void)
{
    sloop_rec_snapshot_t r;
    reset();
    check(project_empty(), "free take fixture starts from an empty project");
    check(sloop_seq_rec_toggle() == ESP_OK, "stopped REC arms an empty project");
    input_on(&trk[0], 60, 100);
    rec_snapshot(&r);
    check(r.state == SEQ_REC_FREE_TAKE && ft_on && !song.playing,
          "the first note of an empty project opens a free take");
    blocks(FS * 2u / CTL); /* one bar at 120 BPM */
    input_off(&trk[0], 60);
    blocks(2);
    check(sloop_seq_rec_toggle() == ESP_OK, "REC closes the free take");
    blocks(1);
    rec_snapshot(&r);
    check(!ft_on && song.playing && r.take_bars == 1u && r.state == SEQ_REC_OFF,
          "a valid free take closes into a one-bar loop and starts playing");
    check(trk[0].step[0].time == ST_NOTE && trk[0].step[0].n == 1 && trk[0].step[0].note[0] == 60,
          "the free take is quantised into step 0");
    rec_snapshot(&r);
    check(r.take_bars == 0u, "the take result is a one-shot consumed by the snapshot");
    transport_req = 2;
    blocks(1);

    /* 太短的 take：< 0.5 s 丢弃（ft_bars = 0xFF），工程不变、不启动走带 */
    reset();
    check(sloop_seq_rec_toggle() == ESP_OK, "a second free take fixture is armed");
    input_on(&trk[0], 62, 100);
    blocks(100);
    input_off(&trk[0], 62);
    check(sloop_seq_rec_toggle() == ESP_OK, "REC closes the short take");
    blocks(1);
    rec_snapshot(&r);
    check(!ft_on && !song.playing && r.take_bars == 0xFF && trk[0].step[0].n == 0,
          "a free take shorter than half a second is dropped and leaves the project empty");
}

static void test_play_button_vs_api_play(void)
{
    sloop_rec_snapshot_t r;
    reset();
    check(sloop_seq_rec_toggle() == ESP_OK, "physical PLAY fixture is armed");
    input_on(&trk[0], 60, 100);
    blocks(FS * 2u / CTL); /* a valid take, one bar */
    input_off(&trk[0], 60);
    check(ft_on && !song.playing, "the free take is running before the PLAY press");
    check(sloop_seq_play_button(true) == ESP_OK, "the physical PLAY key is accepted");
    blocks(1);
    check(!ft_on && !song.playing && transport_req == 0u && ft_bars == 0xFF,
          "the physical PLAY key drops the free take instead of closing it");
    check(sloop_seq_play_button(false) == ESP_OK, "releasing the physical PLAY key clears its button bit");
    rec_snapshot(&r);
    check(r.take_bars == 0xFF && trk[0].step[0].n == 0,
          "the dropped take leaves the project empty and reports the drop");
    blocks(350); /* 跨过 ft_owns_press 的 300ms 窗口（blocks() 每块推进 1ms） */

    /* 同一条 take 由 API PLAY 关闭（sloop_core_play → transport_req = 1） */
    reset();
    check(sloop_seq_rec_toggle() == ESP_OK, "API PLAY fixture is armed");
    input_on(&trk[0], 60, 100);
    blocks(FS * 2u / CTL);
    input_off(&trk[0], 60);
    transport_req = 1;
    blocks(1);
    rec_snapshot(&r);
    check(!ft_on && song.playing && r.take_bars == 1u && trk[0].step[0].n == 1,
          "API PLAY closes the free take into a loop (it does not drop it)");
    transport_req = 2;
    blocks(1);

    /* 非 take 时物理 PLAY 仍是普通走带按钮 */
    reset();
    check(sloop_seq_play_button(true) == ESP_OK, "physical PLAY starts the transport");
    blocks(1);
    check(transport_req == 0u && song.playing, "physical PLAY requests the transport when no take runs");
    sloop_seq_play_button(false);
    check(sloop_seq_play_button(true) == ESP_OK, "physical PLAY with the transport running stops it");
    blocks(1);
    check(!song.playing, "physical PLAY stops a running transport");
    sloop_seq_play_button(false);
}

static void test_drum_hit_lanes(void)
{
    reset();
    TDRUM->p[P_E0] = 0;
    check(sloop_seq_drum_hit(15, 100) == ESP_OK, "drum hit accepts the last lane");
    check(dstep_mask(&TDRUM->dstep[0]) == 0u && trk[0].step[0].n == 0,
          "a live drum hit outside recording records nothing and never falls back to track 0");
    check(sloop_seq_drum_hit(16, 100) == ESP_ERR_INVALID_ARG &&
              sloop_seq_drum_hit(0, 128) == ESP_ERR_INVALID_ARG &&
              sloop_seq_drum_hit(0, 0) == ESP_OK,
          "drum hit rejects lane 16 and velocity 128; velocity 0 is a release");

    reset();
    TDRUM->p[P_E0] = 0;
    song.playing = 1;
    song.rec = (uint8_t)(1u << TRK_DRUM);
    song.sel = TRK_DRUM;
    check(sloop_seq_drum_hit(15, 127) == ESP_OK, "a recorded drum hit on lane 15 succeeds");
    check(dstep_has(&TDRUM->dstep[0], 15) && dstep_lvl(&TDRUM->dstep[0], 15) == LV_HARD,
          "the recorded hit writes lane 15 at its velocity level");
    check(sloop_seq_drum_hit(3, 10) == ESP_OK && dstep_has(&TDRUM->dstep[0], 3) &&
              dstep_lvl(&TDRUM->dstep[0], 3) == LV_GHOST,
          "a second lane records separately at its own level");
    check(dstep_mask(&TDRUM->dstep[0]) == (uint16_t)((1u << 15) | (1u << 3)),
          "both recorded lanes share the step without touching the others");
    song.rec = 0;
    song.playing = 0;
    s_frozen = true;
    check(sloop_seq_drum_hit(1, 100) == ESP_ERR_INVALID_STATE,
          "frozen drum hits return invalid state");
}

static void test_fill_control(void)
{
    reset();
    check(sloop_seq_fill_control(true, false) == ESP_OK && fill_held == 1 && fill_arm == 0,
          "FILL held sets only the held flag");
    blocks(1);
    check(fill_now == 1, "the next rendered block plays the fill");
    check(sloop_seq_fill_control(false, true) == ESP_OK && fill_held == 1 && fill_arm == 1,
          "a FILL NEXT pulse toggles only the arm and leaves a held fill untouched");
    check(sloop_seq_fill_control(false, true) == ESP_OK && fill_held == 1 && fill_arm == 0,
          "FILL NEXT again cancels the arm without touching held");
    check(sloop_seq_fill_control(false, false) == ESP_OK && fill_held == 0,
          "releasing FILL clears only the held flag");
    check(sloop_seq_fill_control(true, false) == ESP_OK && fill_held == 1 &&
              sloop_seq_fill_control(false, true) == ESP_OK && fill_arm == 1 && fill_held == 1,
          "a held fill and a next-bar arm coexist before STOP");
    transport_req = 2;
    blocks(1);
    check(fill_held == 0 && fill_arm == 0 && fill_now == 0,
          "STOP ends a held or armed fill");
}

/* 回归（Task 6 fix 1）：FILL NEXT 是**脉冲**——不得把 held 锁到 STOP。arm 时 held=0，
 * 下一小节 fill 只持续一小节，之后自动落回 normal，held 全程为 0。 */
static void test_fill_next_independent_of_held(void)
{
    const uint32_t bar_blocks = 4u * BEAT_U / (120u * CTL);
    reset();
    song.g[G_BPM] = 120;
    step_fill_set(&trk[0], 0, FC_FILL);   /* fill-only 步：只在 fill 小节触发 */
    step_fill_set(&trk[1], 0, FC_NOFILL); /* no-fill 步：fill 小节静音 */
    transport_req = 1;
    blocks(1);
    check(sloop_seq_fill_control(false, true) == ESP_OK && fill_held == 0 && fill_arm == 1,
          "FILL NEXT with held off arms the next bar only");
    check(fill_now == 0, "an armed next-bar fill does not hold the current block");
    uint32_t run = 0, longest = 0, runs = 0, held_blocks = 0, cond_mismatch = 0;
    uint32_t total = 4u * bar_blocks + 8u;
    for (uint32_t i = 0; i < total; i++) {
        blocks(1);
        if (fill_now) {
            run++;
            if (run == 1u) runs++;
        } else {
            if (run > longest) longest = run;
            run = 0;
        }
        if (fill_held) held_blocks++;
        if (step_plays(&trk[0], 0) != (uint32_t)fill_now) cond_mismatch++;
        if (step_plays(&trk[1], 0) != (uint32_t)!fill_now) cond_mismatch++;
    }
    if (run > longest) longest = run;
    check(held_blocks == 0u && fill_held == 0u,
          "FILL NEXT never latches the held flag (held only follows down/up)");
    check(runs == 1u && longest >= bar_blocks - 3u && longest <= bar_blocks + 3u,
          "the next-bar fill plays for exactly one bar and then stops");
    check(cond_mismatch == 0u,
          "FC_FILL steps trigger and FC_NOFILL steps drop out exactly on the armed bar");
    check(fill_arm == 0u, "the armed fill is consumed by its bar");
    transport_req = 2;
    blocks(1);
}

/* 回归（Task 6 fix 3）：自由录音关闭/丢弃后 300ms 内，REC 按键被上游
 * ft_owns_press() 吞掉（不 arm、不改 rec 位）；窗口过后恢复。 */
static void test_rec_toggle_swallows_recent_take(void)
{
    sloop_rec_snapshot_t r;
    reset();
    fm1_ms = 1000;
    check(sloop_seq_rec_toggle() == ESP_OK, "swallow fixture is armed");
    input_on(&trk[0], 60, 100);
    blocks(FS * 2u / CTL); /* 一个有效的小节（>= 0.5s），关闭时成 loop 而不是丢弃 */
    input_off(&trk[0], 60);
    check(sloop_seq_rec_toggle() == ESP_OK, "REC closes the free take");
    blocks(1);
    rec_snapshot(&r);
    check(!ft_on && song.playing && r.take_bars == 1u && ft_closed &&
              (uint32_t)(fm1_ms - ft_close_ms) < 300u,
          "a valid take closes into a loop and marks ft_owns_press for 300 ms");
    check(sloop_seq_rec_toggle() == ESP_OK && !ft_on && song.rec == 0u && rec_wait == 0u,
          "a REC press within 300 ms after the take closed is swallowed");
    blocks(350); /* 模拟毫秒跨过 300ms 窗口 */
    check(sloop_seq_rec_toggle() == ESP_OK && song.playing && song.rec == 1u,
          "after the window REC records again (the transport kept playing)");
    transport_req = 2;
    blocks(1);

    reset();
    fm1_ms = 5000;
    check(sloop_seq_rec_toggle() == ESP_OK, "drop-swallow fixture is armed");
    input_on(&trk[0], 62, 100);
    blocks(100);
    input_off(&trk[0], 62);
    check(sloop_seq_play_button(true) == ESP_OK, "physical PLAY drops the take");
    blocks(1);
    sloop_seq_play_button(false);
    check(!ft_on && ft_closed && ft_bars == 0xFF,
          "the drop also marks ft_owns_press (nothing was closed)");
    check(sloop_seq_rec_toggle() == ESP_OK && !ft_on && !song.playing && song.rec == 0u &&
              rec_wait == 0u,
          "a REC press within 300 ms after the drop is swallowed too");
    blocks(350);
    check(sloop_seq_rec_toggle() == ESP_OK && rec_wait == 1u,
          "after the window REC arms again from the stopped transport");
    rec_wait = 0;
}

/* ------------------------------------------------- P4-C Task 8: sections / chain / SONG --- */
#define TEST_BPM 120u
static uint32_t bar_blocks(void)
{
    return (uint32_t)((double)FS * 60.0 / (double)TEST_BPM * 4.0 / CTL + 0.5);
}

/* 构造一个 section：轨 0 一个音、LEN/div 决定 section_bars（16/div2 = 1 bar，32 = 2 bars） */
static void build_section(uint32_t slot, uint8_t note, uint32_t len)
{
    for (uint32_t i = 0; i < NSTEP; i++) memset(&trk[0].step[i], 0, sizeof trk[0].step[i]);
    trk[0].p[P_SLEN] = (int16_t)len;
    trk[0].p[P_SDIV] = 2;
    put_note(&trk[0], 0, note, 0);
    proj_capture(&proj_slot[slot]);
}

/* 跑到绝对小节 bar 的开头（clk_beat 的 beat/4；按真实音频块推进，不伪造时钟） */
static void run_until_abs_bar(uint32_t bar)
{
    uint32_t guard = 0;
    while ((clk_beat >> 2) < bar && guard++ < 200000u) blocks(1);
    blocks(1); /* live_block 在小节的第一拍之后才看到新小节（clk_beat 在块尾推进） */
}

static void song_snapshot(sloop_song_snapshot_t *out)
{
    memset(out, 0xA5, sizeof(*out));
    sloop_seq_song_read(out);
}

static void test_song_snapshot_defaults(void)
{
    sloop_song_snapshot_t s;
    reset();
    song_snapshot(&s);
    check(s.enabled == 0 && s.running == 0 && s.index == 0 && s.bar == 0 && s.ready == 0 &&
              s.live == 0xFF && s.dirty == 0 && s.chain_count == 0 && s.srec == 0,
          "fresh snapshot: loop mode, no section, no chain, no SONG REC");
    check(s.config.count == 4 && s.config.loop == 0 && s.config.entry[0].scene == 0 &&
              s.config.entry[1].scene == 1 && s.config.entry[2].scene == 2 &&
              s.config.entry[3].scene == 3 && s.config.entry[0].bars == 4,
          "default song order is A/B/C/D, 4 bars each (upstream arr_defaults)");
    /* 停止时 arrangement_clock.index 会停在 count（loop=0 的最后一个 ARR_DONE），
     * count==16 的消费者不能拿它索引 entry[]。 */
    arrangement.count = 16;
    arrangement_clock.index = 16;
    arrangement_clock.running = 0;
    song_snapshot(&s);
    check(s.index == 15 && s.config.count == 16,
          "a stopped song clamps index to the last valid entry");
    arrangement.count = 2;
    arrangement_clock.index = 5;
    arrangement_clock.running = 1;
    song_snapshot(&s);
    check(s.index == 1, "index is clamped to count-1 for consumers");
    arrangement_clock.running = 0;
    arrangement_clock.index = 0;
    build_section(0, 40, 16);
    build_section(2, 44, 16);
    song_snapshot(&s);
    check(s.ready == 0x5u, "ready is the A-D bitmap of valid project slots");
    check(sloop_seq_song_mode(true) == ESP_OK, "SONG mode can be enabled");
    song_snapshot(&s);
    check(s.enabled == 1, "snapshot projects the SONG mode flag");
    check(sloop_seq_song_mode(false) == ESP_OK, "SONG mode can be disabled");
    srec = 1;
    check(sloop_seq_song_mode(true) == ESP_ERR_NOT_ALLOWED, "SONG mode is refused while SONG REC runs");
    srec = 0;
    s_frozen = true;
    song_snapshot(&s);
    check(s.enabled == 0 && s.config.count == 0, "frozen snapshot is zeroed");
    check(sloop_seq_song_mode(true) == ESP_ERR_INVALID_STATE &&
              sloop_seq_song_rec() == ESP_ERR_INVALID_STATE,
          "frozen arranger actions return invalid state");
}

/* 存储 init 失败/未就绪：proj_slot 指针无效。song_read 与所有 section/chain 动作必须
 * 在任何 proj_slot 解引用之前拒绝（固件里这是 NULL/垃圾 PSRAM 指针 ⇒ panic）。 */
static sloop_song_snapshot_t h_nr_snap;
static esp_err_t h_nr_ret;
static void h_nr_read(void) { song_snapshot(&h_nr_snap); }
static void h_nr_store(void) { h_nr_ret = sloop_seq_section(0, true, false); }
static void h_nr_load(void) { h_nr_ret = sloop_seq_section(0, false, false); }
static void h_nr_erase(void) { h_nr_ret = sloop_seq_section(0, false, true); }
static void h_nr_request(void) { h_nr_ret = sloop_seq_section_request(0); }
static void h_nr_chain_begin(void) { h_nr_ret = sloop_seq_chain_begin(0); }
static void h_nr_chain_commit(void) { h_nr_ret = sloop_seq_chain_commit((const uint8_t[]){0, 1}, 2); }

static sigjmp_buf h_guard_env;
static volatile sig_atomic_t h_guard_fault;
static void h_guard_handler(int sig)
{
    (void)sig;
    h_guard_fault = 1;
    siglongjmp(h_guard_env, 1);
}

/* 运行 fn；若它解引用无效 proj_slot（SIGSEGV）返回 0。 */
static int guard_run(void (*fn)(void))
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_guard_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old);
    h_guard_fault = 0;
    if (sigsetjmp(h_guard_env, 1) == 0) fn();
    sigaction(SIGSEGV, &old, NULL);
    return h_guard_fault == 0;
}

static void test_no_storage_gate(void)
{
    reset();
    build_section(0, 40, 16);
    build_section(1, 52, 16);
    h_store_ready = false;
    h_slot_ps = NULL; /* 固件未就绪时的 proj_slot（NULL/垃圾） */
    memset(&h_nr_snap, 0xA5, sizeof h_nr_snap);
    check(guard_run(h_nr_read), "storage not ready: song_read does not touch proj_slot");
    check(h_nr_snap.enabled == 0 && h_nr_snap.ready == 0 && h_nr_snap.config.count == 0 &&
              h_nr_snap.dirty == 0,
          "storage not ready: song snapshot is zeroed");
    h_nr_ret = ESP_OK;
    check(guard_run(h_nr_store) && h_nr_ret == ESP_ERR_INVALID_STATE,
          "storage not ready: section STORE returns invalid state (no capture)");
    h_nr_ret = ESP_OK;
    check(guard_run(h_nr_load) && h_nr_ret == ESP_ERR_INVALID_STATE,
          "storage not ready: section LOAD returns invalid state");
    h_nr_ret = ESP_OK;
    check(guard_run(h_nr_erase) && h_nr_ret == ESP_ERR_INVALID_STATE,
          "storage not ready: section ERASE returns invalid state");
    h_nr_ret = ESP_OK;
    check(guard_run(h_nr_request) && h_nr_ret == ESP_ERR_INVALID_STATE,
          "storage not ready: section REQUEST returns invalid state");
    h_nr_ret = ESP_OK;
    check(guard_run(h_nr_chain_begin) && h_nr_ret == ESP_ERR_INVALID_STATE,
          "storage not ready: CHAIN BEGIN returns invalid state");
    h_nr_ret = ESP_OK;
    check(guard_run(h_nr_chain_commit) && h_nr_ret == ESP_ERR_INVALID_STATE,
          "storage not ready: CHAIN COMMIT returns invalid state");
    check(sloop_seq_song_mode(true) == ESP_ERR_INVALID_STATE,
          "storage not ready: SONG mode cannot be enabled (audio path would call arrangement_ready)");
    check(sloop_seq_song_mode(false) == ESP_OK, "storage not ready: SONG mode can still be disabled");
    h_store_ready = true;
    h_slot_ps = &h_slot_mem;
}

static void test_song_a2b3_loop0(void)
{
    project_t live, after;
    sloop_song_snapshot_t s;
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    for (uint32_t k = 0; k < NTRK; k++) {
        trk[k].p[P_SLEN] = 16;
        trk[k].p[P_SDIV] = 2;
    }
    put_note(&trk[0], 0, 36, 0);
    song.sel = 1;
    proj_capture(&live); /* the working loop the song must give back */
    build_section(0, 40, 16); /* A: 1 bar */
    build_section(1, 52, 32); /* B: 2 bars */
    proj_apply(&live, 1); /* the working loop is the one playing (as section_load would) */
    song.sel = 1;
    arr_defaults(&arrangement);
    arrangement.count = 2;
    arrangement.loop = 0;
    arrangement.entry[0] = (arr_entry_t){0, 2};
    arrangement.entry[1] = (arr_entry_t){1, 3};
    check(sloop_seq_song_mode(true) == ESP_OK, "A2+B3 fixture is in SONG mode");
    transport_req = 1;
    uint32_t bar = bar_blocks(), t = 0, at_b = 0, at_stop = 0;
    for (uint32_t i = 0; i < 7u * bar && !at_stop; i++) {
        blocks(1);
        t++;
        if (arrangement_clock.index == 1u && !at_b) at_b = t;
        if (!song.playing) at_stop = t;
    }
    check(song.playing == 0 && arrangement_clock.running == 0, "loop=0 SONG stops by itself after the last bar");
    check(at_b >= 2u * bar && at_b < 2u * bar + 32u,
          "entry B starts exactly after A's 2 bars");
    check(at_stop >= 5u * bar && at_stop < 5u * bar + 32u,
          "A2+B3 ends after 5 bars in total");
    proj_capture(&after);
    check(after.sum == live.sum && song.sel == 1u && memcmp(&after, &live, sizeof after) == 0,
          "the working loop and its selected track come back after the song ends");
    check(trk[0].step[0].note[0] == 36, "the loop's pattern is restored, not the last section's");
    song_snapshot(&s);
    check(s.running == 0 && s.enabled == 1, "snapshot after the natural end: loop mode flag kept, no song running");
}

static void test_song_loop1_stop_restore(void)
{
    project_t live, after;
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    for (uint32_t k = 0; k < NTRK; k++) {
        trk[k].p[P_SLEN] = 16;
        trk[k].p[P_SDIV] = 2;
    }
    put_note(&trk[0], 0, 38, 0);
    song.sel = 2;
    proj_capture(&live);
    build_section(0, 41, 16);
    build_section(1, 53, 16);
    proj_apply(&live, 1);
    song.sel = 2;
    arr_defaults(&arrangement);
    arrangement.count = 2;
    arrangement.loop = 1;
    arrangement.entry[0] = (arr_entry_t){0, 1};
    arrangement.entry[1] = (arr_entry_t){1, 1};
    check(sloop_seq_song_mode(true) == ESP_OK, "loop=1 fixture is in SONG mode");
    transport_req = 1;
    blocks(bar_blocks() + bar_blocks() / 2u); /* 1.5 bars: B is playing */
    check(song.playing && arrangement_clock.index == 1u, "loop=1 wraps into entry B");
    transport_req = 2;
    blocks(2);
    proj_capture(&after);
    check(!song.playing && arrangement_clock.running == 0, "STOP ends loop=1 SONG");
    check(after.sum == live.sum && song.sel == 2u && memcmp(&after, &live, sizeof after) == 0,
          "STOP gives the working loop and selected track back");
}

static void test_song_missing_scene(void)
{
    project_t live, after;
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    for (uint32_t k = 0; k < NTRK; k++) {
        trk[k].p[P_SLEN] = 16;
        trk[k].p[P_SDIV] = 2;
    }
    put_note(&trk[0], 0, 39, 0);
    proj_capture(&live);
    build_section(0, 42, 16);
    proj_apply(&live, 1); /* the working loop is playing again */
    arr_defaults(&arrangement);
    arrangement.count = 2;
    arrangement.entry[0] = (arr_entry_t){0, 2};
    arrangement.entry[1] = (arr_entry_t){3, 2}; /* D: no project */
    check(sloop_seq_song_mode(true) == ESP_OK, "missing-scene fixture is in SONG mode");
    transport_req = 1;
    blocks(4);
    proj_capture(&after);
    check(!song.playing && arrangement_clock.running == 0 && arrangement_clock.error,
          "a missing later scene refuses the whole song (not even scene A starts)");
    check(after.sum == live.sum && memcmp(&after, &live, sizeof after) == 0,
          "the refused song leaves the working loop untouched");
}

static void test_section_live_store(void)
{
    sloop_song_snapshot_t s;
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    transport_req = 1;
    blocks(1);
    live_sec = 0;
    put_note(&trk[0], 4, 67, 0);
    uint32_t locks = engine_lock_calls;
    check(sloop_seq_section(0, true, false) == ESP_OK, "live STORE captures a section while playing");
    check(engine_lock_calls == locks + 1u, "section capture takes the engine lock exactly once");
    check(proj_ok(&proj_slot[0]) && proj_slot[0].t[0].step[4].note[0] == 67,
          "the captured section is immediately the RAM slot's content");
    check(live_sec == 0 && (h_sec_dirty & 1u) && h_song_dirty == 0,
          "STORE marks only the section dirty (flash happens later, outside the engine lock)");
    song_snapshot(&s);
    check(s.live == 0 && s.dirty == 1 && s.ready == 0x1u,
          "snapshot reports the live section and the pending flush");
    check(sloop_seq_section(4, true, false) == ESP_ERR_INVALID_ARG &&
              sloop_seq_section(0, true, true) == ESP_ERR_INVALID_ARG,
          "section store rejects invalid slots and store+erase together");
    step_t before = proj_slot[0].t[0].step[4];
    s_frozen = true;
    check(sloop_seq_section(0, true, false) == ESP_ERR_INVALID_STATE &&
              memcmp(&before, &proj_slot[0].t[0].step[4], sizeof before) == 0,
          "frozen section store returns invalid state without capturing");
    transport_req = 2;
    blocks(1);
}

static void test_section_load_erase(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(1, 50, 16);
    check(sloop_seq_section(1, false, false) == ESP_OK && live_sec == 1 &&
              trk[0].step[0].note[0] == 50,
          "stopped LOAD applies the RAM section and makes it live");
    blocks(2); /* 上游 project_apply 置 transport_req=2：渲染块消费掉它 */
    check(sloop_seq_section(2, false, false) == ESP_ERR_NOT_FOUND, "LOAD of an empty section is not found");
    check(sloop_seq_section(1, false, true) == ESP_OK, "ERASE clears a stopped section");
    check(!proj_ok(&proj_slot[1]) && live_sec == -1 && (h_sec_dirty & 2u) == 0,
          "ERASE clears the RAM slot, the live section and its dirty bit");
    check(sloop_seq_section(1, false, true) == ESP_ERR_NOT_FOUND, "ERASE of an empty section is not found");
    arrangement.count = 2;
    arrangement.entry[0] = (arr_entry_t){1, 3};
    arrangement.entry[1] = (arr_entry_t){1, 2};
    check(sloop_seq_section(0, false, true) == ESP_ERR_NOT_FOUND, "ERASE of an empty slot writes nothing");
    build_section(0, 51, 16);
    arrangement.count = 2;
    arrangement.entry[0] = (arr_entry_t){0, 3};
    arrangement.entry[1] = (arr_entry_t){1, 2};
    check(sloop_seq_section(0, false, true) == ESP_OK && arrangement.count == 1u,
          "ERASE drops the erased scene from the song order");
    check(arrangement.entry[0].scene == 1 && arrangement.entry[0].bars == 2 && h_song_dirty,
          "the surviving song entry is kept and the song is marked dirty");
    transport_req = 1;
    blocks(1);
    check(sloop_seq_section(0, false, false) == ESP_ERR_NOT_ALLOWED,
          "section LOAD is refused while the transport runs (scene changes go through live_req)");
    transport_req = 2;
    blocks(1);
}

static void test_section_request(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16);
    build_section(1, 52, 32);
    check(sloop_seq_section(0, false, false) == ESP_OK, "request fixture loads A");
    transport_req = 1;
    blocks(1);
    check(sloop_seq_section_request(1) == ESP_OK && live_req == 1 && chain_n == 0,
          "a live request is queued for the next bar and cancels any chain");
    blocks(bar_blocks() + 2u);
    check(live_sec == 1, "the queued section starts on the next bar");
    check(sloop_seq_section_request(3) == ESP_ERR_NOT_FOUND, "requesting an empty section is not found");
    check(sloop_seq_section_request(4) == ESP_ERR_INVALID_ARG, "requesting slot 4 is invalid");
    transport_req = 2;
    blocks(2);
    check(sloop_seq_section_request(0) == ESP_ERR_NOT_ALLOWED,
          "a stopped transport has no next bar to request for (LOAD instead)");
    transport_req = 1;
    blocks(1);
    arrangement_enabled = 1;
    arr_defaults(&arrangement);
    arrangement.count = 1;
    arrangement.entry[0] = (arr_entry_t){0, 1};
    transport_req = 2;
    blocks(1);
    transport_req = 1;
    blocks(2);
    check(arrangement_clock.running, "song fixture is playing");
    check(sloop_seq_section_request(1) == ESP_ERR_NOT_ALLOWED,
          "a live section request is refused while the song plays");
    transport_req = 2;
    blocks(1);
}

/* COMMIT 不依赖 begin：若第一个 scene 既未在播放也没有相同的 pending 请求，commit
 * 必须自行请求它，否则 standalone COMMIT 会从 entry 1 开始（跳过 entry 0）。 */
static void test_chain_commit_requests_first(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16);
    build_section(1, 52, 16);
    check(sloop_seq_section(0, false, false) == ESP_OK, "standalone COMMIT fixture loads A");
    blocks(2);
    transport_req = 1;
    blocks(1);
    check(live_sec == 0 && live_req < 0, "the fixture is playing A with no pending request");
    const uint8_t ba[] = {1, 0};
    uint32_t locks = engine_lock_calls;
    check(sloop_seq_chain_commit(ba, 2) == ESP_OK && engine_lock_calls == locks + 1u,
          "standalone COMMIT installs the chain with one engine lock");
    check(live_req == 1, "COMMIT independently requests its first scene");
    blocks(bar_blocks() + 2u);
    check(live_sec == 1, "the chain starts at entry 0 on the next bar");
    /* 已在播放的首项不需要再请求 */
    live_req = -1;
    const uint8_t bb[] = {1, 0};
    check(sloop_seq_chain_commit(bb, 2) == ESP_OK && live_req < 0,
          "COMMIT does not re-request a first scene that is already playing");
    const uint8_t one[] = {1};
    check(sloop_seq_chain_commit(one, 1) == ESP_OK && chain_n == 1u,
          "a one-entry chain is accepted (a looping single section)");
    transport_req = 2;
    blocks(2);
}

static void test_chain_auto_bars(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16); /* 1 bar */
    build_section(1, 52, 32); /* 2 bars */
    check(sloop_seq_section(0, false, false) == ESP_OK, "chain fixture loads A");
    transport_req = 1;
    blocks(bar_blocks() * 2u); /* settle mid bar 2 */
    check(sloop_seq_chain_begin(0) == ESP_OK && live_req == 0 && chain_n == 0,
          "chain begin queues the first section right away (and cancels an older chain)");
    const uint8_t ab[] = {0, 1};
    check(sloop_seq_chain_commit(ab, 2) == ESP_OK, "chain commit installs A/B atomically");
    sloop_song_snapshot_t s;
    song_snapshot(&s);
    check(s.chain_count == 2 && s.chain_index == 0 && s.chain_bars == 1,
          "the chain state follows: A first, one bar");
    uint32_t bar = bar_blocks(), t = 0, n = 0;
    uint8_t seen[3] = {1, 0, 0};
    uint32_t at[3] = {0, 0, 0};
    int8_t last = live_sec;
    for (uint32_t i = 0; i < 6u * bar + 4u && n < 3u; i++) {
        blocks(1);
        t++;
        if (live_sec != last) {
            last = live_sec;
            seen[n] = (uint8_t)live_sec;
            at[n++] = t;
        }
    }
    check(n == 3u && seen[0] == 1 && seen[1] == 0 && seen[2] == 1,
          "chain A/B/A keeps the order");
    check(n == 3u && at[0] + 32u >= bar && at[0] <= bar + 32u,
          "A plays its own bar count (B starts one bar in)");
    check(n == 3u && at[1] - at[0] + 32u >= 2u * bar && at[1] - at[0] <= 2u * bar + 32u,
          "B plays two bars (its longest pattern) before A returns");
    song_snapshot(&s);
    check(s.chain_count == 2 && s.chain_index == 1 && s.chain_bars == 2,
          "after the round the chain state follows: entry B, two bars");
    transport_req = 2;
    blocks(1);
    check(!song.playing && chain_n == 0, "STOP drops the chain");
}

static void test_chain_limits_and_cancel(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16);
    build_section(1, 52, 16);
    check(sloop_seq_chain_begin(0) == ESP_ERR_NOT_ALLOWED,
          "chain begin needs a playing live loop");
    check(sloop_seq_chain_commit((const uint8_t[]){0}, 1) == ESP_ERR_NOT_ALLOWED,
          "chain commit needs a playing live loop");
    check(sloop_seq_section(0, false, false) == ESP_OK, "chain-limit fixture loads A");
    transport_req = 1;
    blocks(1);
    const uint8_t two[] = {0, 1};
    check(sloop_seq_chain_begin(0) == ESP_OK && sloop_seq_chain_commit(two, 2) == ESP_OK,
          "chain fixture is playing A/B");
    const uint8_t nine[] = {0, 1, 0, 1, 0, 1, 0, 1, 0};
    check(sloop_seq_chain_commit(nine, 9) == ESP_ERR_INVALID_ARG && chain_n == 2u,
          "a nine-entry chain is refused and leaves the installed chain unchanged");
    check(sloop_seq_chain_commit(NULL, 1) == ESP_ERR_INVALID_ARG, "a null scene list is invalid");
    const uint8_t empty[] = {3};
    check(sloop_seq_chain_commit(empty, 1) == ESP_ERR_NOT_FOUND && chain_n == 2u,
          "a chain naming an empty section is refused atomically");
    check(sloop_seq_chain_commit(NULL, 0) == ESP_OK && chain_n == 0u,
          "count 0 cancels the chain");
    check(sloop_seq_chain_begin(0) == ESP_OK && sloop_seq_chain_commit(two, 2) == ESP_OK,
          "the cancelled chain can be rebuilt");
    blocks(bar_blocks());
    check(sloop_seq_chain_begin(0) == ESP_OK && chain_n == 0 && live_req == 0,
          "a single section tap cancels the chain and asks for A");
    int8_t first = live_sec;
    blocks(bar_blocks() * 3u);
    check(live_sec == 0 && first == 0, "the single tap keeps A playing (no chain comes back)");
    transport_req = 2;
    blocks(1);
}

static void test_srec_next_bar(void)
{
    sloop_song_snapshot_t s;
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16);
    build_section(1, 52, 16);
    check(sloop_seq_section(0, false, false) == ESP_OK, "SONG REC fixture loads A");
    blocks(2); /* 消费 section LOAD 的 transport_req=2 */
    transport_req = 1;
    blocks(1);
    uint32_t b0 = clk_beat >> 2;
    check(sloop_seq_song_rec() == ESP_OK && srec == 1 && arrangement_enabled == 0,
          "SONG REC arms on the next bar and forces loop mode");
    run_until_abs_bar(b0 + 1u); /* 下一个小节：take 开始 */
    check(srec == 2 && srec_n == 1u && srec_e[0].scene == 0 && srec_e[0].bars == 0u,
          "the take starts at the next bar, with the live section");
    run_until_abs_bar(b0 + 2u); /* A 完整播放了 1 小节 */
    check(srec_e[0].bars == 1u, "A's first bar is counted");
    check(sloop_seq_section_request(1) == ESP_OK, "B is asked for at the next bar");
    /* section 切换会 seq_reset_tracks（小节号归零），所以按「请求后再跑 3.25 小节」
     * 计时：B 在下一小节开始，播满 2 小节 + 停止时的当前小节 = 3 小节。 */
    blocks(3u * bar_blocks() + bar_blocks() / 4u);
    transport_req = 2;
    blocks(2);
    check(srec == 0 && srec_done == 2u, "STOP finishes the take with two parts");
    check(arrangement.count == 2u && arrangement.entry[0].scene == 0 && arrangement.entry[0].bars == 2u &&
              arrangement.entry[1].scene == 1 && arrangement.entry[1].bars == 3u,
          "SONG REC writes A 2 bars + B 3 bars (the bar playing counts)");
    song_snapshot(&s);
    check(s.srec == 0 && s.srec_done == 2u && s.dirty == 1,
          "the finished take is projected and marks the song dirty for the delayed writer");
    h_song_dirty = 0;
    s_srec_marked = 0; /* 模拟一次新的适配层视图：srec_done 对账应只发生一次 */
    song_snapshot(&s);
    check(h_song_dirty == 1, "the dirty mark is reconciled from srec_done once per take");
    h_song_dirty = 0;
    song_snapshot(&s);
    check(h_song_dirty == 0, "later 30ms snapshots do not re-mark the same take");
}

static void test_srec_64_bars_split(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16);
    check(sloop_seq_section(0, false, false) == ESP_OK, "64-bar fixture loads A");
    blocks(2);
    transport_req = 1;
    blocks(1);
    uint32_t b0 = clk_beat >> 2;
    check(sloop_seq_song_rec() == ESP_OK, "64-bar take arms");
    run_until_abs_bar(b0 + 1u);
    check(srec == 2 && srec_n == 1u, "the take is recording");
    run_until_abs_bar(b0 + 66u); /* 第 64 小节边界处拆分；再多跑两小节余量 */
    check(srec_n == 2u && srec_e[0].scene == 0 && srec_e[0].bars == 64u && srec_e[1].bars >= 1u,
          "64 bars of one section split into a second entry (never more than 64 bars)");
    blocks(bar_blocks() / 4u);
    transport_req = 2;
    blocks(2);
    check(srec == 0 && srec_done == 2u && arrangement.count == 2u && arrangement.entry[0].bars == 64u,
          "the split take finishes as a two-entry song");
}

static void test_srec_16_items_finish(void)
{
    reset();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    build_section(0, 40, 16);
    build_section(1, 52, 16);
    check(sloop_seq_section(0, false, false) == ESP_OK, "16-item fixture loads A");
    blocks(2);
    transport_req = 1;
    blocks(1);
    uint32_t b0 = clk_beat >> 2;
    check(sloop_seq_song_rec() == ESP_OK, "16-item take arms");
    run_until_abs_bar(b0 + 1u);
    check(srec == 2 && srec_n == 1u, "16-item take is recording");
    for (uint32_t rounds = 0; srec_n < 16u && rounds < 40u; rounds++) {
        uint32_t bar = clk_beat >> 2;
        check(sloop_seq_section_request(srec_n & 1u) == ESP_OK, "the next section is requested");
        run_until_abs_bar(bar + 1u);
    }
    check(srec == 2 && srec_n == 16u, "sixteen section changes fill the song");
    {
        uint32_t bar = clk_beat >> 2;
        sloop_seq_section_request(1);
        run_until_abs_bar(bar + 1u);
    }
    check(srec == 0 && srec_done == 16u && arrangement.count == 16u,
          "a seventeenth change finishes the take with 16 entries (upstream srec_finish)");
    transport_req = 2;
    blocks(1);
}

static void test_srec_empty_no_song(void)
{
    sloop_song_snapshot_t s;
    arr_config_t before;
    reset();
    before = arrangement;
    srec = 2;
    srec_n = 0;
    check(sloop_seq_song_rec() == ESP_OK, "stopping an empty take is accepted");
    check(srec == 0 && srec_done == 0xFFu, "an empty take reports NO SONG (0xFF), not a zero-part song");
    check(memcmp(&arrangement, &before, sizeof before) == 0, "an empty take leaves the song order untouched");
    song_snapshot(&s);
    check(s.srec_done == 0xFFu && s.dirty == 0, "NO SONG is projected and marks nothing dirty");
    srec = 1;
    check(sloop_seq_song_rec() == ESP_OK && srec == 0 && srec_done == 0xFFu,
          "cancelling an armed (not yet recording) take keeps the old result");
}

/* 上一轮 take 的结果若还没被 30ms sync 读走，开始下一轮前必须先对账，否则新 take
 * 清掉 srec_done 会让那次歌曲顺序永远不落盘。 */
static void test_srec_reconcile_before_new_take(void)
{
    reset();
    srec = 0;
    srec_done = 3;
    s_srec_marked = 0;
    h_song_dirty = 0;
    check(sloop_seq_song_rec() == ESP_OK, "a new take can start with a stale result pending");
    check(srec == 1 && srec_done == 0, "the new take clears the visible result");
    check(h_song_dirty == 1 && s_srec_marked == 0,
          "the stale take is reconciled (song dirty) before the reset; the new take shows none");
    /* NO SONG 也要推进已对账值：否则「2 项 → NO SONG → 2 项」会因 marked 仍为 2 而漏写 */
    srec = 0;
    srec_done = 0xFF;
    h_song_dirty = 0;
    sloop_seq_song_read(&(sloop_song_snapshot_t){0});
    check(s_srec_marked == 0xFF && h_song_dirty == 0, "NO SONG advances the reconciled value (no dirty)");
    srec_done = 2;
    sloop_seq_song_read(&(sloop_song_snapshot_t){0});
    check(h_song_dirty == 1 && s_srec_marked == 2,
          "a later take with the same part count is marked again after NO SONG");
    reset();
}

static void test_rec_fill_projection(void)
{
    sloop_rec_snapshot_t r;
    reset();
    check(sloop_seq_fill_control(true, false) == ESP_OK, "FILL held goes down");
    rec_snapshot(&r);
    check(r.fill_held == 1 && r.fill_arm == 0, "REC snapshot projects the FILL hold level");
    check(sloop_seq_fill_control(false, true) == ESP_OK, "FILL NEXT is a pulse");
    rec_snapshot(&r);
    check(r.fill_held == 1 && r.fill_arm == 1,
          "NEXT flips the arm without touching the hold level");
    check(sloop_seq_fill_control(false, false) == ESP_OK, "FILL released");
    rec_snapshot(&r);
    check(r.fill_held == 0 && r.fill_arm == 1, "release clears only the hold level");
}

static void test_play_release_without_lock(void)
{
    reset();
    check(sloop_seq_play_button(true) == ESP_OK, "PLAY press fixture starts the transport");
    blocks(1);
    fm1_in.buttons |= SLOOP_BTN_PLAY; /* 模拟自由录音期间的按钮电平 */
    s_frozen = true;
    uint32_t locks = engine_lock_calls;
    uint32_t unlocks = engine_unlock_calls;
    check(sloop_seq_play_button(false) == ESP_OK && (fm1_in.buttons & SLOOP_BTN_PLAY) == 0 &&
              engine_lock_calls == locks && engine_unlock_calls == unlocks,
          "PLAY release clears its button bit without the engine lock (also when frozen)");
    s_frozen = false;
    transport_req = 2;
    blocks(1);
}

/* ------------------------------------------------- P4-F Task 6: TOOLS 三动作 ---
 * sloop_core_tools 的同源门禁（上游 ui_input.c:568-612；清单 012 §4）。声音相关的期望不
 * 从被测函数反推：用 preset_tab5.h 的同一对 set_engine_of_locked/preset_apply_locked 在
 * **另一条轨**上独立构造（INIT SOUND），或与「上电默认」快照逐字段比对（NEW）。 */

/* 上电声音（felucca_init 的 Tab5 口径）：轨 0..2 套 TRK_DEF，鼓轨 808 kit。 */
static void tools_init_sounds(void)
{
    for (uint32_t k = 0; k < NTRK; k++) {
        if (k < NPART) {
            set_engine_of_locked(&trk[k], TRK_DEF[k][0]);
            preset_apply_locked(&trk[k], TRK_DEF[k][1]);
            trk[k].engine = trk[k].eng_req;
        }
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
}

static int steps_all_empty(uint32_t track)
{
    if (track == TRK_DRUM) {
        for (uint32_t i = 0; i < NSTEP; i++)
            if (memcmp(&trk[track].dstep[i], "\0\0\0\0\0\0\0\0\0\0", sizeof(dstep_t)) != 0) return 0;
        return 1;
    }
    for (uint32_t i = 0; i < NSTEP; i++)
        if (trk[track].step[i].n || trk[track].step[i].time != ST_REST) return 0;
    return 1;
}

static void test_tools_clrseq(void)
{
    step_t keep0[NSTEP];
    dstep_t keep3[NSTEP];
    sloop_lock_t locks[NLOCK];
    reset();
    tools_init_sounds();
    song.sel = 1;
    put_note(&trk[0], 3, 60, 1);
    put_note(&trk[1], 0, 64, 0);
    put_note(&trk[1], 9, 67, 2);
    trk[1].micro[9] = 7;
    memset(&trk[3].dstep[5], 0, sizeof(dstep_t));
    trk[3].dstep[5].on[0] = 0x81;
    check(sloop_seq_lock_set(1, 9, P_DIST, 20) == ESP_OK && sloop_seq_set_fill(1, 9, 2) == ESP_OK,
          "clrseq fixture: lock + fill on the selected track");
    memcpy(keep0, trk[0].step, sizeof keep0);
    memcpy(keep3, trk[3].dstep, sizeof keep3);
    step_t before[NSTEP];
    memcpy(before, trk[1].step, sizeof before);
    int16_t p_before[P_COUNT];
    memcpy(p_before, trk[1].p, sizeof p_before);

    song.g[G_BPM] = (int16_t)TEST_BPM;
    transport_req = 1;
    blocks(1);
    check(song.playing, "clrseq fixture: transport is playing");
    uint32_t locks_taken = engine_lock_calls, unlocks = engine_unlock_calls;
    check(sloop_core_tools(SLOOP_TOOL_CLRSEQ) == ESP_OK, "CLEAR SEQ on the selected track succeeds while playing");
    check(engine_lock_calls == locks_taken + 1u && engine_unlock_calls == unlocks + 1u,
          "CLEAR SEQ takes the engine lock exactly once");
    check(steps_all_empty(1), "CLEAR SEQ empties the selected track's step table (TIEs included)");
    check(sloop_seq_lock_list(1, locks, NLOCK) == 0, "CLEAR SEQ drops the selected track's parameter locks");
    check(trk[1].micro[9] == 0 && step_fill(&trk[1], 9) == FC_NORM, "CLEAR SEQ resets micro and fill conditions");
    check(memcmp(trk[0].step, keep0, sizeof keep0) == 0 && memcmp(trk[3].dstep, keep3, sizeof keep3) == 0,
          "CLEAR SEQ leaves the other tracks untouched");
    check(memcmp(trk[1].p, p_before, sizeof p_before) == 0, "CLEAR SEQ does not touch parameters");
    check(song.playing && transport_req == 0, "CLEAR SEQ while playing: the transport keeps running");

    check(sloop_seq_pattern(1, SEQ_PATTERN_UNDO, 0) == ESP_OK && memcmp(trk[1].step, before, sizeof before) == 0,
          "UNDO restores the cleared pattern");
    transport_req = 2;
    blocks(1);

    uint32_t l = engine_lock_calls;
    check(sloop_core_tools(3u) == ESP_ERR_INVALID_ARG && engine_lock_calls == l,
          "unknown action is rejected before taking the lock");
    s_frozen = true;
    check(sloop_core_tools(SLOOP_TOOL_CLRSEQ) == ESP_ERR_INVALID_STATE && engine_lock_calls == l,
          "frozen: CLEAR SEQ returns invalid state without touching the engine");
    s_frozen = false;
}

static void test_tools_initsound(void)
{
    reset();
    tools_init_sounds();
    /* 两条轨走同一条路：轨 1 经 TOOLS，轨 2 直接调 set_engine_of_locked（=
     * sloop_core_set_engine 在锁内做的事）。 */
    for (uint32_t k = 1; k <= 2u; k++) {
        set_engine_of_locked(&trk[k], 4);
        preset_apply_locked(&trk[k], 5);
        trk[k].p[P_DIST] = 99;
        trk[k].p[P_E0] = (int16_t)(trk[k].p[P_E0] ^ 1);
        trk[k].p[P_ATK] = 3;
    }
    trk[1].p[P_LEVEL] = 50;
    trk[2].p[P_LEVEL] = 50;
    set_engine_of_locked(&trk[2], trk[2].eng_req);
    song.sel = 1;
    check(trk[1].preset == 5 && trk[1].p[P_DIST] == 99, "initsound fixture: sound is dirty before the action");
    uint32_t l = engine_lock_calls;
    check(sloop_core_tools(SLOOP_TOOL_INITSOUND) == ESP_OK && engine_lock_calls == l + 1u,
          "INIT SOUND succeeds with one engine lock");
    check(trk[1].preset == 0 && trk[1].eng_req == 4, "INIT SOUND: engine kept, preset 0");
    check(memcmp(trk[1].p, trk[2].p, sizeof trk[1].p) == 0, "INIT SOUND p[] == sloop_core_set_engine on the same sound");
    check(trk[1].p[P_DIST] != 99 && trk[1].p[P_LEVEL] == 50, "INIT SOUND: sound params reset, mix (LEVEL) kept");

    int16_t p3[P_COUNT];
    memcpy(p3, TDRUM->p, sizeof p3);
    song.sel = TRK_DRUM;
    check(sloop_core_tools(SLOOP_TOOL_INITSOUND) == ESP_ERR_NOT_ALLOWED &&
              memcmp(TDRUM->p, p3, sizeof p3) == 0,
          "INIT SOUND on the drum track is not allowed (page_desc excludes it)");
    song.sel = 1;
    s_frozen = true;
    check(sloop_core_tools(SLOOP_TOOL_INITSOUND) == ESP_ERR_INVALID_STATE && engine_lock_calls == l + 2u,
          "frozen: INIT SOUND returns invalid state without the engine lock");
    s_frozen = false;

    /* 走带中允许且不中断 */
    reset();
    tools_init_sounds();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    transport_req = 1;
    blocks(1);
    check(sloop_core_tools(SLOOP_TOOL_INITSOUND) == ESP_OK && song.playing && transport_req == 0,
          "INIT SOUND while playing: the transport keeps running");
    transport_req = 2;
    blocks(1);
}

/* build_section 只动轨 0 的当前步表；这里需要四个互不覆盖的 section（NEW 不得碰它们）。 */
static void build_section_slot(uint32_t slot, uint8_t note, uint32_t len)
{
    static step_t keep[NSTEP];
    memcpy(keep, trk[0].step, sizeof keep);
    build_section(slot, note, len);
    memcpy(trk[0].step, keep, sizeof keep);
}

/* NEW 的逐项副作用（ui_input.c:462-490）。 */
static void test_tools_new(void)
{
    static project_t slots[4];
    sloop_lock_t locks[NLOCK];
    static arr_config_t arr;
    static const uint16_t midi_ids[] = {G_SLOT,  G_DRCH,  G_SYNC,   G_MIDI,   G_ROUTE,  G_CH1,
                                        G_CH2,   G_CH3,   G_CHOTH,  G_BEND,   G_FXCH,   G_DRMAP,
                                        G_RMCH,  G_CLKOUT, G_FXFADE};
    int16_t g_before[G_COUNT];
    reset();
    tools_init_sounds();
    for (uint32_t s = 0; s < 4u; s++) build_section_slot(s, (uint8_t)(40 + s), 16);
    memcpy(slots, proj_slot, sizeof slots);
    arr_defaults(&arrangement);
    arrangement.count = 2;
    arrangement.entry[0] = (arr_entry_t){1, 3};
    arrangement.entry[1] = (arr_entry_t){2, 2};
    memcpy(&arr, &arrangement, sizeof arr);
    h_sec_dirty = 0x5;
    h_song_dirty = 1;

    /* 弄脏一切 NEW 应该复位的东西 */
    for (uint32_t k = 0; k < NTRK; k++) {
        for (uint32_t i = 0; i < P_E0; i++) trk[k].p[i] = (int16_t)(TP[i].def + 3);
        put_note(&trk[k], 2, 60, 0);
        trk[k].micro[2] = -5;
    }
    set_engine_of_locked(&trk[0], 9);
    preset_apply_locked(&trk[0], 2);
    trk[0].engine = 9;
    check(sloop_seq_lock_set(0, 2, P_DIST, 40) == ESP_OK, "new fixture: a lock on track 0");
    TDRUM->p[P_E0] = (int16_t)(DRUM_DEFAULT_KIT + 1);
    memset(&TDRUM->dstep[4], 0, sizeof(dstep_t));
    TDRUM->dstep[4].on[0] = 0x0F;
    drum_tune[3] = 5;
    drum_dec[7] = -2;
    drum_mute = 0x00F0;
    song.solo = 0x5;
    song.octave = 2;
    for (uint32_t i = 0; i < G_COUNT; i++) song.g[i] = (int16_t)(GP[i].def + 1);
    for (size_t i = 0; i < sizeof midi_ids / sizeof midi_ids[0]; i++) {
        song.g[midi_ids[i]] = (int16_t)(GP[midi_ids[i]].def + 7);
        g_before[i] = song.g[midi_ids[i]];
    }
    song.g[G_BPM] = (int16_t)TEST_BPM;
    song.sel = 2;
    sync_reload = 0;
    uint32_t sess = undo_sess;
    transport_req = 1;
    blocks(1);
    check(song.playing && song.g[G_BPM] == (int16_t)TEST_BPM, "new fixture: transport is playing");

    uint32_t l = engine_lock_calls;
    check(sloop_core_tools(SLOOP_TOOL_NEWPRJ) == ESP_OK && engine_lock_calls == l + 1u,
          "NEW succeeds with one engine lock while the loop is playing");
    check(transport_req == 2 && panic_req == (1u << NTRK) - 1u,
          "NEW requests a transport stop and a panic on every track");
    check(undo_sess == sess + 16u && undo.valid && undo.trk == TRK_DRUM && undo.sess == (undo_sess | 3u),
          "NEW marks undo on every track (one session per track, drum last)");
    check(sync_reload == 1 && ui.force == 1, "NEW sets the reload/force flags");
    for (uint32_t k = 0; k < NPART; k++) {
        check(trk[k].eng_req == TRK_DEF[k][0] && trk[k].preset == TRK_DEF[k][1] && trk[k].pold == 1,
              "NEW: default sound of TRK_DEF on every synth track (old sound fades out)");
        check(steps_all_empty(k) && trk[k].p[P_LEVEL] == TP[P_LEVEL].def && trk[k].p[P_SLEN] == TP[P_SLEN].def,
              "NEW: empty pattern and default parameters on every synth track");
    }
    check(trk[2].p[P_LEVEL] != (int16_t)(TP[P_LEVEL].def + 3) && trk[0].eng_req != 9 && trk[0].preset != 2,
          "NEW: the dirty sound/pattern values are gone");
    check(sloop_seq_lock_list(0, locks, NLOCK) == 0, "NEW: parameter locks cleared");
    check(TDRUM->p[P_E0] == DRUM_DEFAULT_KIT && steps_all_empty(TRK_DRUM),
          "NEW: default kit and empty drum pattern");
    int tune_ok = 1, dec_ok = 1;
    for (uint32_t i = 0; i < DRUM_LANES; i++) {
        tune_ok &= drum_tune[i] == 0;
        dec_ok &= drum_dec[i] == 0;
    }
    check(tune_ok && dec_ok && drum_mute == 0, "NEW: drum tune/decay/live mutes reset");
    check(song.solo == 0 && song.octave == 0, "NEW: solo and octave cleared");
    int g_ok = 1, midi_ok = 1;
    for (uint32_t i = 0; i < G_COUNT; i++) {
        int excluded = 0;
        for (size_t m = 0; m < sizeof midi_ids / sizeof midi_ids[0]; m++) excluded |= midi_ids[m] == i;
        if (excluded) continue;
        g_ok &= song.g[i] == GP[i].def;
    }
    for (size_t m = 0; m < sizeof midi_ids / sizeof midi_ids[0]; m++)
        midi_ok &= song.g[midi_ids[m]] == g_before[m];
    check(g_ok, "NEW: every non-MIDI global back to its default (BPM/swing/fx too)");
    check(midi_ok, "NEW: the FM-1's MIDI-class settings are kept");
    check(song.sel == 2, "NEW: the selected track is kept");
    check(memcmp(slots, proj_slot, sizeof slots) == 0 && h_sec_dirty == 0x5 && h_song_dirty == 1,
          "NEW: sections A-D (RAM slots + dirty) untouched");
    check(memcmp(&arr, &arrangement, sizeof arr) == 0, "NEW: song arrangement untouched");
    blocks(1);
    check(!song.playing, "NEW: the transport stops (request consumed by the render block)");
}

/* NEW 的唯一守卫：SONG 播放中且 arrangement_enabled。 */
static void test_tools_new_guards(void)
{
    int16_t p_before[P_COUNT];
    reset();
    tools_init_sounds();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    trk[0].p[P_LEVEL] = 33;
    memcpy(p_before, trk[0].p, sizeof p_before);

    /* arrangement_enabled 但没在走带（loop 模式停止态）：NEW 照常 */
    arrangement_enabled = 1;
    check(sloop_core_tools(SLOOP_TOOL_NEWPRJ) == ESP_OK && trk[0].p[P_LEVEL] == TP[P_LEVEL].def,
          "NEW with arrangement enabled but stopped proceeds");

    /* SONG 播放中：拒绝，状态一字不动 */
    reset();
    tools_init_sounds();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    trk[0].p[P_LEVEL] = 33;
    memcpy(p_before, trk[0].p, sizeof p_before);
    build_section(0, 40, 16);
    arrangement_enabled = 1;
    arr_defaults(&arrangement);
    arrangement.count = 1;
    arrangement.entry[0] = (arr_entry_t){0, 1};
    transport_req = 1;
    blocks(2);
    check(arrangement_clock.running, "guard fixture: the song is playing");
    uint32_t l = engine_lock_calls;
    check(sloop_core_tools(SLOOP_TOOL_NEWPRJ) == ESP_ERR_NOT_ALLOWED && engine_lock_calls == l + 1u,
          "NEW while the song plays: not allowed");
    check(memcmp(trk[0].p, p_before, sizeof p_before) == 0 && song.playing && transport_req == 0,
          "NEW refused by the song guard: nothing changes, transport still running");
    /* 同一状态下 CLRSEQ / INIT SOUND 仍允许 */
    song.sel = 0;
    put_note(&trk[0], 1, 60, 0);
    check(sloop_core_tools(SLOOP_TOOL_CLRSEQ) == ESP_OK && sloop_core_tools(SLOOP_TOOL_INITSOUND) == ESP_OK &&
              song.playing && arrangement_clock.running,
          "CLEAR SEQ / INIT SOUND stay allowed while the song plays");
    transport_req = 2;
    blocks(1);

    /* 走带中（无 SONG）：NEW 自己停走带后执行 */
    reset();
    tools_init_sounds();
    song.g[G_BPM] = (int16_t)TEST_BPM;
    trk[0].p[P_LEVEL] = 33;
    put_note(&trk[0], 4, 60, 0);
    transport_req = 1;
    blocks(1);
    check(sloop_core_tools(SLOOP_TOOL_NEWPRJ) == ESP_OK && steps_all_empty(0) &&
              trk[0].p[P_LEVEL] == TP[P_LEVEL].def,
          "NEW while the loop plays proceeds (stops the transport itself)");
    blocks(1);
    check(!song.playing, "NEW: loop transport stopped");

    s_frozen = true;
    check(sloop_core_tools(SLOOP_TOOL_NEWPRJ) == ESP_ERR_INVALID_STATE, "frozen: NEW returns invalid state");
    s_frozen = false;
}


/* 解锁之后工程 SAVE→LOAD 必须无损：上游 lock_del 只把槽标回 LOCK_FREE、保留 param/val，
 * 而 proj_apply 把空闲槽归一成 (LOCK_FREE,0,0)——适配层在解锁后清零空闲槽的残留字节。 */
static void test_lock_del_canonical(void)
{
    project_t p;
    uint32_t sum0;
    reset();
    /* 前置：先把全部轨的锁表规范化（更早的用例直接调上游 lock_del，会在别的轨留下
     * 空闲槽残留；proj_apply 会清掉它们）。本用例只验证「经适配层解锁之后」的往返。 */
    proj_capture(&p);
    proj_apply(&p, 1);
    put_note(&trk[0], 5, 60, 0);
    check(sloop_seq_lock_set(0, 5, P_DIST, 40) == ESP_OK && sloop_seq_lock_set(0, 9, P_CHOR, 7) == ESP_OK,
          "canonical fixture: two locks");
    check(sloop_seq_lock_del(0, 5, P_DIST) == ESP_OK, "unlock one lock");
    proj_capture(&p);
    sum0 = p.sum;
    proj_apply(&p, 1);
    proj_capture(&p);
    check(p.sum == sum0, "SAVE -> LOAD after an unlock keeps the live hash (no residue in the free slot)");
    reset();
    put_note(&trk[0], 5, 60, 0);
    check(sloop_seq_lock_set(0, 5, P_DIST, 40) == ESP_OK, "canonical fixture 2: a locked step");
    {
        const sloop_step_edit_t edit = {.track = 0, .step = 5, .op = SEQ_STEP_CLEAR};
        check(sloop_seq_step_edit(&edit) == ESP_OK, "clear the locked step (lock_del with P_COUNT)");
    }
    proj_capture(&p);
    sum0 = p.sum;
    proj_apply(&p, 1);
    proj_capture(&p);
    check(p.sum == sum0, "SAVE -> LOAD after clearing a locked step keeps the live hash");
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
    test_move_blocked_far_target();
    test_copy_blocked_far_target();
    test_step_fields_clear();
    test_pattern_rotate_length();
    test_pattern_transpose_clear_undo();
    test_new_entry_locking();
    test_note_del_atomic();
    test_rec_config();
    test_rec_arm_and_first_note();
    test_rec_count_in();
    test_rec_poly_independent_release();
    test_free_take_close_and_drop();
    test_play_button_vs_api_play();
    test_drum_hit_lanes();
    test_fill_control();
    test_fill_next_independent_of_held();
    test_rec_toggle_swallows_recent_take();
    test_play_release_without_lock();
    test_song_snapshot_defaults();
    test_no_storage_gate();
    test_song_a2b3_loop0();
    test_song_loop1_stop_restore();
    test_song_missing_scene();
    test_section_live_store();
    test_section_load_erase();
    test_section_request();
    test_chain_commit_requests_first();
    test_chain_auto_bars();
    test_chain_limits_and_cancel();
    test_srec_next_bar();
    test_srec_64_bars_split();
    test_srec_16_items_finish();
    test_srec_empty_no_song();
    test_srec_reconcile_before_new_take();
    test_rec_fill_projection();
    test_tools_clrseq();
    test_tools_initsound();
    test_tools_new();
    test_tools_new_guards();
    test_lock_del_canonical();
    printf("SEQ HOST TEST %s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
