/* sloop_state host 单测（scripts/state-test.sh）：stub 引擎 + 真实状态层源码。
 * 覆盖 brief Task 3 的五组断言 + 边界/同步保持。 */
#include <stdio.h>
#include <string.h>

#include "sloop_state.h"
#include "stub_engine.h"

static int g_failures;
static int g_passed;
static int g_case_failed;

static void check_ll(const char *what, long long got, long long want)
{
    if (got != want) {
        printf("  FAIL %s: got %lld want %lld\n", what, got, want);
        g_case_failed = 1;
    }
}

#define CHECK(expr)                                                    \
    do {                                                               \
        if (!(expr)) {                                                 \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #expr);   \
            g_case_failed = 1;                                         \
        }                                                              \
    } while (0)

static void apply(edit_cmd_type_t type, uint32_t a, uint32_t b, uint16_t v)
{
    edit_cmd_t cmd = { .type = type, .a = a, .b = b, .v = v };
    state_apply(&cmd);
}

static void run(const char *name, void (*fn)(void))
{
    g_case_failed = 0;
    fn();
    if (g_case_failed) {
        printf("FAIL %s\n", name);
        g_failures++;
    } else {
        printf("PASS %s\n", name);
        g_passed++;
    }
}

/* ① SET_BPM：越界钳位 40..240（上游 GP[G_BPM]，params.c）并落引擎调用。 */
static void case_set_bpm(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    apply(CMD_SET_BPM, 0, 0, 500);
    check_ll("G_BPM id", g_stub.last_global.id, 0);
    check_ll("上限钳位落引擎", g_stub.last_global.value, 240);
    state_get_copy(&s);
    check_ll("copy.bpm 上限", s.core.bpm, 240);

    apply(CMD_SET_BPM, 0, 0, 10);
    check_ll("下限钳位落引擎", g_stub.last_global.value, 40);
    state_get_copy(&s);
    check_ll("copy.bpm 下限", s.core.bpm, 40);

    apply(CMD_SET_BPM, 0, 0, 120);
    check_ll("范围内原样落引擎", g_stub.last_global.value, 120);
    check_ll("set_global 调用次数", g_stub.set_global_calls, 3);
}

/* ② TOGGLE_STEP 空步→开（last_note，无则 60）→ 再切→REST。 */
static void case_toggle_step_empty(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    apply(CMD_NOTE_ON, 0, 0, 64);
    check_ll("note on 落引擎", g_stub.last_note.note, 64);
    check_ll("note on on", g_stub.last_note.on, 1);
    state_get_copy(&s);
    check_ll("last_note", s.last_note, 64);

    apply(CMD_TOGGLE_STEP, 0, 0, 0);
    const sloop_step_t *st = &g_stub.step[0][0];
    check_ll("空→开 n", st->n, 1);
    check_ll("空→开 note0=last_note", st->note[0], 64);
    check_ll("空→开 note1", st->note[1], 0);
    check_ll("空→开 note2", st->note[2], 0);
    check_ll("空→开 note3", st->note[3], 0);
    check_ll("空→开 vel", st->vel, 100);
    check_ll("空→开 time=NOTE", st->time, 0);
    check_ll("空→开 flags", st->flags, 0);
    check_ll("write_step 次数", g_stub.write_step_calls, 1);
    state_get_copy(&s);
    check_ll("copy 步 n", s.core.step[0][0].n, 1);
    check_ll("copy 步 note0", s.core.step[0][0].note[0], 64);
    check_ll("copy 步 time", s.core.step[0][0].time, 0);

    apply(CMD_TOGGLE_STEP, 0, 0, 0);
    check_ll("开→关 n", st->n, 0);
    check_ll("开→关 time=REST", st->time, 2);
    state_get_copy(&s);
    check_ll("copy 关 n", s.core.step[0][0].n, 0);

    /* 从未弹奏时（last_note=0）默认写 60 */
    stub_engine_reset();
    state_init();
    apply(CMD_TOGGLE_STEP, 1, 1, 0);
    check_ll("无 last_note 默认 60", g_stub.step[1][1].note[0], 60);
}

/* ③ read-modify-write：lvl/rat/micro/fill 逐字段保留；邻居 TIE 步逐字节不变。 */
static void case_toggle_step_preserves(void)
{
    stub_engine_reset();
    state_init();

    sloop_step_t preset = { .n = 0, .vel = 0, .time = 2, .flags = 0,
                            .lvl = 0x0A, .rat = 0x05, .micro = -3, .fill = 2 };
    sloop_step_t neighbor = { .n = 2, .note = { 60, 64, 0, 0 }, .vel = 88, .time = 1,
                              .flags = 0x09, .lvl = 0x1B, .rat = 0x0E, .micro = 7, .fill = 3 };
    g_stub.step[0][3] = preset;
    g_stub.step[0][4] = neighbor;

    apply(CMD_TOGGLE_STEP, 0, 3, 0);
    const sloop_step_t *st = &g_stub.step[0][3];
    check_ll("开 n", st->n, 1);
    check_ll("开 time", st->time, 0);
    check_ll("开 lvl 保留", st->lvl, 0x0A);
    check_ll("开 rat 保留", st->rat, 0x05);
    check_ll("开 micro 保留", st->micro, -3);
    check_ll("开 fill 保留", st->fill, 2);
    CHECK(memcmp(&g_stub.step[0][4], &neighbor, sizeof(neighbor)) == 0);

    apply(CMD_TOGGLE_STEP, 0, 3, 0);
    check_ll("关 n", st->n, 0);
    check_ll("关 time", st->time, 2);
    check_ll("关 lvl 保留", st->lvl, 0x0A);
    check_ll("关 rat 保留", st->rat, 0x05);
    check_ll("关 micro 保留", st->micro, -3);
    check_ll("关 fill 保留", st->fill, 2);
    CHECK(memcmp(&g_stub.step[0][4], &neighbor, sizeof(neighbor)) == 0);
}

/* ④ MUTE/SOLO 翻转对应轨位，且跨 sync 保持（MVP 引擎无此写入口）。 */
static void case_mute_solo(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    apply(CMD_MUTE, 1, 0, 0);
    state_get_copy(&s);
    check_ll("mute k=1", s.core.trk[1].mute, 1);
    check_ll("mute k=0 未变", s.core.trk[0].mute, 0);
    apply(CMD_MUTE, 1, 0, 0);
    state_get_copy(&s);
    check_ll("mute 再切", s.core.trk[1].mute, 0);

    apply(CMD_MUTE, 3, 0, 0);
    apply(CMD_SOLO, 2, 0, 0);
    state_get_copy(&s);
    check_ll("mute 鼓轨", s.core.trk[3].mute, 1);
    check_ll("solo k=2", s.core.trk[2].solo, 1);

    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后 mute 保持", s.core.trk[3].mute, 1);
    check_ll("sync 后 solo 保持", s.core.trk[2].solo, 1);
}

/* ⑤ state_get_copy 反映最近 cmd；sync 取引擎真值并保持本层持有的选轨。 */
static void case_copy_and_sync(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    apply(CMD_SET_BPM, 0, 0, 150);
    state_get_copy(&s);
    check_ll("copy.bpm 立即反映", s.core.bpm, 150);
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后 bpm", s.core.bpm, 150);

    apply(CMD_NOTE_ON, 0, 0, 72);
    apply(CMD_NOTE_OFF, 0, 0, 72);
    check_ll("note off 落引擎", g_stub.last_note.on, 0);
    check_ll("note off note", g_stub.last_note.note, 72);
    state_get_copy(&s);
    check_ll("note off 不改 last_note", s.last_note, 72);

    apply(CMD_TOGGLE_STEP, 0, 5, 0);
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后步窗", s.core.step[0][5].n, 1);

    apply(CMD_SET_TRACK, 3, 0, 0);
    state_get_copy(&s);
    check_ll("SET_TRACK 选中", s.core.selected_track, 3);
    check_ll("SET_TRACK 焦点", s.focus_track, 3);
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后选中保持", s.core.selected_track, 3);

    apply(CMD_SELECT_TRACK, 1, 0, 0);
    state_get_copy(&s);
    check_ll("SELECT_TRACK 选中", s.core.selected_track, 1);

    apply(CMD_PLAY, 0, 0, 0);
    check_ll("play 落引擎", g_stub.last_play, 1);
    state_get_copy(&s);
    check_ll("copy.playing", s.core.playing, 1);
    apply(CMD_STOP, 0, 0, 0);
    check_ll("stop 落引擎", g_stub.last_play, 0);
    state_get_copy(&s);
    check_ll("copy.playing stop", s.core.playing, 0);

    state_set_page(2);
    state_get_copy(&s);
    check_ll("page", s.page, 2);
    state_set_page(9);
    state_get_copy(&s);
    check_ll("page 越界忽略", s.page, 2);

    state_set_focus(2, 7);
    state_get_copy(&s);
    check_ll("focus_track", s.focus_track, 2);
    check_ll("focus_step", s.focus_step, 7);
    state_set_focus(4, 9);
    state_get_copy(&s);
    check_ll("focus 越界轨忽略", s.focus_track, 2);
    check_ll("focus 越界步仍生效", s.focus_step, 9);

    state_set_layer(0x03);
    state_get_copy(&s);
    check_ll("layer_held", s.layer_held, 3);
}

/* 越界/未知命令一律忽略，不调引擎、不改状态。 */
static void case_bounds(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    apply(CMD_TOGGLE_STEP, 3, 0, 0); /* 鼓轨只读 */
    check_ll("鼓轨不写", g_stub.write_step_calls, 0);
    apply(CMD_TOGGLE_STEP, 0, 64, 0);
    check_ll("步越界不写", g_stub.write_step_calls, 0);
    apply(CMD_TOGGLE_STEP, 5, 0, 0);
    check_ll("轨越界不写", g_stub.write_step_calls, 0);

    apply(CMD_NOTE_ON, 4, 0, 60);
    apply(CMD_NOTE_ON, 0, 0, 200);
    check_ll("note 越界不调引擎", g_stub.note_calls, 0);

    apply(CMD_MUTE, 9, 0, 0);
    apply(CMD_SOLO, 9, 0, 0);
    apply(CMD_SET_TRACK, 7, 0, 0);
    state_get_copy(&s);
    for (uint32_t k = 0; k < 4; k++) {
        check_ll("越界 mute 不改", s.core.trk[k].mute, 0);
        check_ll("越界 solo 不改", s.core.trk[k].solo, 0);
    }
    check_ll("越界 SET_TRACK", s.core.selected_track, 0);

    edit_cmd_t unknown = { .type = (edit_cmd_type_t)99 };
    state_apply(&unknown);
    state_apply(NULL);
}

int main(void)
{
    run("SET_BPM 钳位 40..240 并落引擎", case_set_bpm);
    run("TOGGLE_STEP 空步↔关（last_note/默认 60）", case_toggle_step_empty);
    run("TOGGLE_STEP read-modify-write 保留字段", case_toggle_step_preserves);
    run("MUTE/SOLO 轨位翻转", case_mute_solo);
    run("state_get_copy 反映 cmd 与 sync", case_copy_and_sync);
    run("越界参数忽略", case_bounds);

    printf("\n%d passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
