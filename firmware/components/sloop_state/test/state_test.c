/* sloop_state host 单测（scripts/state-test.sh）：stub 引擎 + 真实状态层源码。
 * 覆盖 brief Task 3 的五组断言 + 边界/同步保持。 */
#include <stdio.h>
#include <string.h>

#include "sloop_state.h"
#include "stub_engine.h"

static int g_failures;
static int g_passed;
static int g_case_failed;
static int g_case_index; /* run() 的 1 起序号：失败行带 k，便于对上用例 */

static void check_ll(const char *what, long long got, long long want)
{
    if (got != want) {
        printf("  FAIL k=%d %s: got %lld want %lld\n", g_case_index, what, got, want);
        g_case_failed = 1;
    }
}

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            printf("  FAIL k=%d %s:%d: %s\n", g_case_index, __func__, __LINE__, \
                   #expr);                                                       \
            g_case_failed = 1;                                                   \
        }                                                                        \
    } while (0)

static void apply(edit_cmd_type_t type, uint32_t a, uint32_t b, uint16_t v)
{
    edit_cmd_t cmd = { .type = type, .a = a, .b = b, .v = v };
    state_apply(&cmd);
}

static void apply_param_batch(uint32_t track, const sloop_param_pair_t *pairs, uint32_t n)
{
    edit_cmd_t cmd = { .type = CMD_SET_PARAM_BATCH, .a = track, .p = pairs, .n = n };
    state_apply(&cmd);
}

static void run(const char *name, void (*fn)(void))
{
    g_case_failed = 0;
    g_case_index++;
    fn();
    if (g_case_failed) {
        printf("FAIL k=%d %s\n", g_case_index, name);
        g_failures++;
    } else {
        printf("PASS k=%d %s\n", g_case_index, name);
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

    /* note 0 照常发声但不记忆 last_note（0 是「还没弹过」哨兵） */
    apply(CMD_NOTE_ON, 0, 0, 0);
    state_get_copy(&s);
    check_ll("note 0 不覆盖 last_note", s.last_note, 64);

    uint32_t reads0 = g_stub.read_step_calls;
    uint32_t writes0 = g_stub.write_step_calls;
    apply(CMD_TOGGLE_STEP, 0, 0, 0);
    check_ll("RMW read_step ×1", g_stub.read_step_calls - reads0, 1);
    check_ll("RMW write_step ×1", g_stub.write_step_calls - writes0, 1);
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

/* ③ read-modify-write：micro/fill 逐字段保留；开步清空残留步内字段（上游先 memset）；
 * 关步整步清空（对齐上游 step_clear）；邻居步逐字节不变；TIE（n>0 但 time≠NOTE）
 * 按上游 sp_on 口径算「空」。 */
static void case_toggle_step_preserves(void)
{
    stub_engine_reset();
    state_init();

    /* 旧步：n=0（关）但 note[]/lvl/rat 留着上次多音的音符——重新开步时必须清掉 */
    sloop_step_t preset = { .n = 0, .note = { 60, 64, 0, 0 }, .vel = 0, .time = 2,
                            .flags = 0, .lvl = 0x0A, .rat = 0x05, .micro = -3, .fill = 2 };
    sloop_step_t neighbor = { .n = 2, .note = { 60, 64, 0, 0 }, .vel = 88, .time = 1,
                              .flags = 0x09, .lvl = 0x1B, .rat = 0x0E, .micro = 7, .fill = 3 };
    sloop_step_t tie = { .n = 2, .note = { 60, 67, 0, 0 }, .vel = 90, .time = 1,
                         .flags = 0x08, .lvl = 0x05, .rat = 0x02, .micro = 1, .fill = 1 };
    g_stub.step[0][3] = preset;
    g_stub.step[0][4] = neighbor;
    g_stub.step[0][6] = tie;

    uint32_t reads0 = g_stub.read_step_calls;
    uint32_t writes0 = g_stub.write_step_calls;
    apply(CMD_TOGGLE_STEP, 0, 3, 0);
    check_ll("RMW read_step ×1", g_stub.read_step_calls - reads0, 1);
    check_ll("RMW write_step ×1", g_stub.write_step_calls - writes0, 1);
    const sloop_step_t *st = &g_stub.step[0][3];
    check_ll("开 n", st->n, 1);
    check_ll("开 time", st->time, 0);
    check_ll("开 note0=默认 60", st->note[0], 60);
    check_ll("开清掉旧 note1", st->note[1], 0);
    check_ll("开清掉旧 note2", st->note[2], 0);
    check_ll("开清掉旧 note3", st->note[3], 0);
    check_ll("开 vel", st->vel, 100);
    check_ll("开 flags", st->flags, 0);
    check_ll("开清掉旧 lvl（上游新步 memset）", st->lvl, 0);
    check_ll("开清掉旧 rat", st->rat, 0);
    check_ll("开 micro 保留", st->micro, -3);
    check_ll("开 fill 保留", st->fill, 2);
    CHECK(memcmp(&g_stub.step[0][4], &neighbor, sizeof(neighbor)) == 0);
    CHECK(memcmp(&g_stub.step[0][6], &tie, sizeof(tie)) == 0);

    /* 关：整步清空（上游 step_clear）——vel/lvl/rat/flags 一并归零，micro/fill 保留 */
    apply(CMD_TOGGLE_STEP, 0, 3, 0);
    check_ll("关 n", st->n, 0);
    check_ll("关 time", st->time, 2);
    check_ll("关 vel 清零", st->vel, 0);
    check_ll("关 lvl 清零", st->lvl, 0);
    check_ll("关 rat 清零", st->rat, 0);
    check_ll("关 flags 清零", st->flags, 0);
    check_ll("关 note1..3 清零", st->note[1] | st->note[2] | st->note[3], 0);
    check_ll("关 micro 保留", st->micro, -3);
    check_ll("关 fill 保留", st->fill, 2);
    CHECK(memcmp(&g_stub.step[0][4], &neighbor, sizeof(neighbor)) == 0);

    /* 再开一次：关步留下的 note[] 不得漏进新步 */
    apply(CMD_TOGGLE_STEP, 0, 3, 0);
    check_ll("再开 n", st->n, 1);
    check_ll("再开 note0", st->note[0], 60);
    check_ll("再开 note1", st->note[1], 0);
    check_ll("再开 note2", st->note[2], 0);
    check_ll("再开 note3", st->note[3], 0);

    /* TIE 步（n>0, time=TIE）按上游 sp_on 不算「有音」：切换 → 开 */
    apply(CMD_TOGGLE_STEP, 0, 6, 0);
    const sloop_step_t *ti = &g_stub.step[0][6];
    check_ll("TIE→开 n", ti->n, 1);
    check_ll("TIE→开 time", ti->time, 0);
    check_ll("TIE→开 note0", ti->note[0], 60);
    check_ll("TIE→开 note1", ti->note[1], 0);
    check_ll("TIE→开 lvl 清零", ti->lvl, 0);
    check_ll("TIE→开 rat 清零", ti->rat, 0);
    check_ll("TIE→开 micro 保留", ti->micro, 1);
    check_ll("TIE→开 fill 保留", ti->fill, 1);
}

/* ③b 关步折叠 TIE 连缀（对齐上游 note_del 的 note_span_set(t,i,1)）：
 * 其后 n==0 的 TIE 折回 REST，微调/填充保留；遇非 TIE 或带音符的 TIE 即停。 */
static void case_toggle_step_tie_collapse(void)
{
    stub_engine_reset();
    state_init();

    sloop_step_t head = { .n = 1, .note = { 60, 0, 0, 0 }, .vel = 100, .time = 0,
                          .flags = 0, .lvl = 0x02, .rat = 0x01, .micro = -1, .fill = 3 };
    sloop_step_t tie_a = { .n = 0, .note = { 0 }, .vel = 0, .time = 1,
                           .flags = 0, .lvl = 0, .rat = 0, .micro = 5, .fill = 2 };
    sloop_step_t tie_b = tie_a;
    tie_b.micro = -4;
    /* 带音符的 TIE（旧格式）：上游保留，折叠必须在此停下 */
    sloop_step_t tie_held = { .n = 1, .note = { 72, 0, 0, 0 }, .vel = 90, .time = 1,
                              .flags = 0, .lvl = 0x01, .rat = 0, .micro = 2, .fill = 0 };
    sloop_step_t rest_after = { .n = 0, .note = { 0 }, .vel = 0, .time = 2,
                                .flags = 0, .lvl = 0, .rat = 0, .micro = 0, .fill = 0 };
    g_stub.step[0][4] = head;
    g_stub.step[0][5] = tie_a;
    g_stub.step[0][6] = tie_b;
    g_stub.step[0][7] = tie_held;
    g_stub.step[0][8] = rest_after;

    apply(CMD_TOGGLE_STEP, 0, 4, 0);
    check_ll("关步 n", g_stub.step[0][4].n, 0);
    check_ll("第一条 TIE 折回 REST", g_stub.step[0][5].time, 2);
    check_ll("第一条 TIE n=0", g_stub.step[0][5].n, 0);
    check_ll("第一条 TIE micro 保留", g_stub.step[0][5].micro, 5);
    check_ll("第一条 TIE fill 保留", g_stub.step[0][5].fill, 2);
    check_ll("第二条 TIE 折回 REST", g_stub.step[0][6].time, 2);
    check_ll("第二条 TIE micro 保留", g_stub.step[0][6].micro, -4);
    /* 带音符的 TIE 之后原样 */
    CHECK(memcmp(&g_stub.step[0][7], &tie_held, sizeof(tie_held)) == 0);
    CHECK(memcmp(&g_stub.step[0][8], &rest_after, sizeof(rest_after)) == 0);

    /* 折叠窗口 = 本轨 LEN（stub 默认 16）：列 1..15 的连续 TIE 全折回，列 16 不动 */
    sloop_step_t beyond = { .n = 0, .note = { 0 }, .vel = 0, .time = 1,
                            .flags = 0, .lvl = 0, .rat = 0, .micro = 0, .fill = 0 };
    g_stub.step[1][0] = head;
    for (uint32_t j = 1; j < 16u; j++) g_stub.step[1][j] = tie_a;
    g_stub.step[1][16] = beyond;
    apply(CMD_TOGGLE_STEP, 1, 0, 0);
    check_ll("LEN 内尾列 TIE 折回 REST", g_stub.step[1][15].time, 2);
    check_ll("LEN 内链首 TIE 折回 REST", g_stub.step[1][1].time, 2);
    check_ll("LEN 外 TIE 原样", g_stub.step[1][16].time, 1);
}

/* ④ MUTE/SOLO 走引擎写入口：翻转对应轨位并落引擎；sync 取引擎真值（无状态层例外）。 */
static void case_mute_solo(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    uint32_t mute0 = g_stub.set_mute_calls;
    apply(CMD_MUTE, 1, 0, 0);
    check_ll("MUTE 落引擎 ×1", g_stub.set_mute_calls - mute0, 1);
    check_ll("MUTE 引擎轨号", g_stub.last_mute.track, 1);
    check_ll("MUTE 引擎值", g_stub.last_mute.mute, 1);
    check_ll("MUTE 引擎快照", g_stub.snap.trk[1].mute, 1);
    state_get_copy(&s);
    check_ll("mute k=1", s.core.trk[1].mute, 1);
    check_ll("mute k=0 未变", s.core.trk[0].mute, 0);

    apply(CMD_MUTE, 1, 0, 0);
    check_ll("MUTE 再切引擎值", g_stub.last_mute.mute, 0);
    check_ll("MUTE 再切引擎快照", g_stub.snap.trk[1].mute, 0);
    state_get_copy(&s);
    check_ll("mute 再切", s.core.trk[1].mute, 0);

    apply(CMD_MUTE, 3, 0, 0);
    check_ll("MUTE 鼓轨落引擎", g_stub.snap.trk[3].mute, 1);

    uint32_t solo0 = g_stub.set_solo_calls;
    apply(CMD_SOLO, 2, 0, 0);
    check_ll("SOLO 落引擎 ×1", g_stub.set_solo_calls - solo0, 1);
    check_ll("SOLO 掩码", g_stub.last_solo_mask, 0x04);
    check_ll("SOLO 引擎快照", g_stub.snap.trk[2].solo, 1);
    state_get_copy(&s);
    check_ll("solo k=2", s.core.trk[2].solo, 1);

    /* 掩码是多轨真值：再独奏 k=0 时掩码 = 0x05，k=2 保持 */
    apply(CMD_SOLO, 0, 0, 0);
    check_ll("SOLO 掩码累积", g_stub.last_solo_mask, 0x05);
    state_get_copy(&s);
    check_ll("solo k=0", s.core.trk[0].solo, 1);
    check_ll("solo k=2 保持", s.core.trk[2].solo, 1);

    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后 mute 来自引擎", s.core.trk[3].mute, 1);
    check_ll("sync 后 solo 来自引擎", s.core.trk[2].solo, 1);
    check_ll("sync 后 solo k=0", s.core.trk[0].solo, 1);

    /* 引擎侧外部改动（阶段 5 FM-1 push / 直接编辑）经 sync 反映到 AppState：
     * 若 sync 仍保留状态层例外，这里会看到旧值 0。 */
    g_stub.snap.trk[1].mute = 1;
    g_stub.snap.trk[1].solo = 1;
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 采纳引擎 mute", s.core.trk[1].mute, 1);
    check_ll("sync 采纳引擎 solo", s.core.trk[1].solo, 1);
}

/* ⑤ state_get_copy 反映最近 cmd；sync 取引擎真值（含选轨/mute/solo）。 */
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
    check_ll("SET_TRACK 落引擎", g_stub.last_select, 3);
    state_get_copy(&s);
    check_ll("SET_TRACK 选中", s.core.selected_track, 3);
    check_ll("SET_TRACK 焦点", s.focus_track, 3);
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后选中来自引擎", s.core.selected_track, 3);

    uint32_t sel0 = g_stub.select_calls;
    apply(CMD_SELECT_TRACK, 1, 0, 0);
    check_ll("SELECT_TRACK 落引擎 ×1", g_stub.select_calls - sel0, 1);
    check_ll("SELECT_TRACK 引擎轨号", g_stub.last_select, 1);
    check_ll("SELECT_TRACK 引擎快照", g_stub.snap.selected_track, 1);
    state_get_copy(&s);
    check_ll("SELECT_TRACK 选中", s.core.selected_track, 1);

    /* 引擎侧外部改动经 sync 反映（状态层不再持有选轨） */
    g_stub.snap.selected_track = 2;
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 采纳引擎选轨", s.core.selected_track, 2);

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

/* ⑥ ERASE_NOTE：read-modify-write 删除**显式步**（a=track b=step v=note）里的指定
 * 音符；不读 focus_step。其余音符前移，每音符字段（lvl/rat 的 2bit、flags 的 CONT
 * 位）随之前移；最后一个音符删除后整步清空并折叠 TIE 连缀（对齐上游）；无匹配/
 * 越界一律不写引擎。上游常量镜像：SF_ACCENT=1、SF_CONT0=8。 */
static void case_erase_note(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    /* 和弦步：note 60/64；lvl k0=1/k1=2；rat k0=2/k1=1；flags ACcent + CONT(note0) */
    sloop_step_t chord = { .n = 2, .note = { 60, 64, 0, 0 }, .vel = 88, .time = 0,
                           .flags = 0x01u | 0x08u, .lvl = 0x09, .rat = 0x06,
                           .micro = -2, .fill = 1 };
    g_stub.step[0][5] = chord;
    /* 焦点故意放在别的步：ERASE_NOTE 必须用命令里的显式步号 */
    state_set_focus(0, 9);

    uint32_t reads0 = g_stub.read_step_calls;
    uint32_t writes0 = g_stub.write_step_calls;
    apply(CMD_ERASE_NOTE, 0, 5, 60);
    check_ll("RMW read_step ×1", g_stub.read_step_calls - reads0, 1);
    check_ll("RMW write_step ×1", g_stub.write_step_calls - writes0, 1);
    const sloop_step_t *st = &g_stub.step[0][5];
    check_ll("删首音后 n", st->n, 1);
    check_ll("删首音后 note0=原 note1", st->note[0], 64);
    check_ll("删首音后 note1 清零", st->note[1], 0);
    check_ll("lvl 随音符前移", st->lvl, 0x02);
    check_ll("rat 随音符前移", st->rat, 0x01);
    check_ll("CONT 位随移（无其余 CONT）", st->flags, 0x01);
    check_ll("time 保持 NOTE", st->time, 0);
    check_ll("vel 保留", st->vel, 88);
    check_ll("micro 保留", st->micro, -2);
    check_ll("fill 保留", st->fill, 1);
    state_get_copy(&s);
    check_ll("copy 步 n", s.core.step[0][5].n, 1);
    check_ll("copy 步 note0", s.core.step[0][5].note[0], 64);

    /* 无匹配音符：不写引擎 */
    writes0 = g_stub.write_step_calls;
    apply(CMD_ERASE_NOTE, 0, 5, 60);
    check_ll("无匹配不写", g_stub.write_step_calls - writes0, 0);

    /* 删除最后一个音符：整步清空（vel/lvl/rat/flags=0、time=REST），TIE 连缀折叠 */
    sloop_step_t tie = { .n = 0, .note = { 0 }, .vel = 0, .time = 1,
                         .flags = 0, .lvl = 0, .rat = 0, .micro = 4, .fill = 2 };
    g_stub.step[0][6] = tie;
    apply(CMD_ERASE_NOTE, 0, 5, 64);
    check_ll("删末音后 n", st->n, 0);
    check_ll("删末音后 time=REST", st->time, 2);
    check_ll("删末音后 note0", st->note[0], 0);
    check_ll("删末音后 vel 清零", st->vel, 0);
    check_ll("删末音后 lvl 清零", st->lvl, 0);
    check_ll("删末音后 rat 清零", st->rat, 0);
    check_ll("删末音后 flags 清零", st->flags, 0);
    check_ll("删末音后 micro 保留", st->micro, -2);
    check_ll("删末音后 fill 保留", st->fill, 1);
    check_ll("TIE 连缀折回 REST", g_stub.step[0][6].time, 2);
    check_ll("TIE micro 保留", g_stub.step[0][6].micro, 4);
    state_get_copy(&s);
    check_ll("copy 步关", s.core.step[0][5].n, 0);

    /* 三音和弦删中间音：note 60/64/67 → 60/67；每音符字段随移——
     * lvl k0=1/k1=2/k2=3、rat k0=3/k1=1/k2=2、CONT 位在 k1（被删）与 k2（保留）上、
     * accent 置位。断言非零邻居字段确实搬到了新位置（错位/漏拼则必失败）。 */
    sloop_step_t trio = { .n = 3, .note = { 60, 64, 67, 0 }, .vel = 90, .time = 0,
                          .flags = 0x01u | 0x10u | 0x20u, /* ACCENT + CONT(1) + CONT(2) */
                          .lvl = 0x39u /* k0=1 k1=2 k2=3 */,
                          .rat = 0x27u /* k0=3 k1=1 k2=2 */,
                          .micro = 0, .fill = 0 };
    g_stub.step[1][2] = trio;
    apply(CMD_ERASE_NOTE, 1, 2, 64);
    const sloop_step_t *trio_st = &g_stub.step[1][2];
    check_ll("中间音删后 n", trio_st->n, 2);
    check_ll("中间音删后 note0", trio_st->note[0], 60);
    check_ll("中间音删后 note1", trio_st->note[1], 67);
    check_ll("中间音删后 note2", trio_st->note[2], 0);
    check_ll("中间音删后 lvl（k0=1 k1=3）", trio_st->lvl, 0x0D);
    check_ll("中间音删后 rat（k0=3 k1=2）", trio_st->rat, 0x0B);
    check_ll("中间音删后 flags（ACCENT + 原 k2 的 CONT 移到 k1）", trio_st->flags, 0x11);

    /* 再删首音 60：67 前移为 k0，字段继续随移 */
    apply(CMD_ERASE_NOTE, 1, 2, 60);
    check_ll("删首音后 n", trio_st->n, 1);
    check_ll("删首音后 note0", trio_st->note[0], 67);
    check_ll("删首音后 lvl", trio_st->lvl, 0x03);
    check_ll("删首音后 rat", trio_st->rat, 0x02);
    check_ll("删首音后 flags（CONT 移到 k0）", trio_st->flags, 0x09);

    /* 删最后一个音：步转 REST 并整步清空 */
    apply(CMD_ERASE_NOTE, 1, 2, 67);
    check_ll("删末音后 n", trio_st->n, 0);
    check_ll("删末音后 time=REST", trio_st->time, 2);
    check_ll("删末音后 vel 清零", trio_st->vel, 0);

    /* 越界：鼓轨/步号越界/note 0/note 128 不读不写 */
    reads0 = g_stub.read_step_calls;
    writes0 = g_stub.write_step_calls;
    apply(CMD_ERASE_NOTE, 3, 0, 60);   /* 鼓轨 */
    apply(CMD_ERASE_NOTE, 0, 64, 60);  /* 步号越界 */
    apply(CMD_ERASE_NOTE, 0, 0, 0);    /* note 0 */
    apply(CMD_ERASE_NOTE, 0, 0, 200);  /* note 越界 */
    check_ll("越界不读", g_stub.read_step_calls - reads0, 0);
    check_ll("越界不写", g_stub.write_step_calls - writes0, 0);
}

/* ⑦ 步窗抽象 {bank, window_start, window_len}：默认 {0,0,16}；窗口列 c 映射到
 * 引擎绝对步 window_start + c（引擎步号恒为绝对索引，命令携带绝对值）；窗口投影
 * 在 STATE SNAP 与 STEP 页同源，窗口切换即重取。钳位：start 16 对齐、上限 48
 * （bank = start/16）、len 1..16、focus 夹到窗口内。 */
static void case_step_window(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;
    step_window_t w;

    step_window(&w);
    check_ll("默认 bank", w.bank, 0);
    check_ll("默认 start", w.window_start, 0);
    check_ll("默认 len", w.window_len, 16);
    check_ll("列 0 → 绝对步 0", step_window_abs(&w, 0), 0);
    check_ll("列 15 → 绝对步 15", step_window_abs(&w, 15), 15);

    /* 引擎侧 bank1 内容：步 16/17 + 鼓步 16/17 */
    g_stub.step[0][16] = (sloop_step_t){ .n = 1, .note = { 67, 0, 0, 0 }, .vel = 90, .time = 0 };
    g_stub.step[1][17] = (sloop_step_t){ .n = 2, .note = { 60, 64, 0, 0 }, .vel = 88, .time = 0 };
    g_stub.dstep[16].on = 1;
    g_stub.dstep[17].on = 1;

    apply(CMD_SET_WINDOW, 1, 16, 16); /* a=bank（冗余，由 start 派生） b=start v=len */
    step_window(&w);
    check_ll("窗口 bank", w.bank, 1);
    check_ll("窗口 start", w.window_start, 16);
    check_ll("窗口 len", w.window_len, 16);
    check_ll("bank1 列 0 → 绝对步 16", step_window_abs(&w, 0), 16);
    check_ll("bank1 列 15 → 绝对步 31", step_window_abs(&w, 15), 31);
    state_get_copy(&s);
    check_ll("copy window start", s.window.window_start, 16);
    check_ll("copy window len", s.window.window_len, 16);
    check_ll("窗口投影 列 0 n", s.core.step[0][0].n, 1);
    check_ll("窗口投影 列 0 note", s.core.step[0][0].note[0], 67);
    check_ll("窗口投影 列 1 空", s.core.step[0][1].n, 0);
    check_ll("窗口投影 列 1 n（轨 1 绝对步 17）", s.core.step[1][1].n, 2);
    check_ll("鼓轨窗口投影 列 0", s.core.dstep[0].on, 1);
    check_ll("鼓轨窗口投影 列 1", s.core.dstep[1].on, 1);
    check_ll("鼓轨窗口投影 列 2 空", s.core.dstep[2].on, 0);

    /* 写路径：列 c → 绝对步 window_start+c（命令携带绝对值），投影同步到窗口列 */
    uint32_t writes0 = g_stub.write_step_calls;
    apply(CMD_TOGGLE_STEP, 2, step_window_abs(&w, 5), 0); /* 列 5 → 绝对步 21 */
    check_ll("窗口列写引擎绝对步", g_stub.step[2][21].n, 1);
    check_ll("bank0 同列未被写", g_stub.step[2][5].n, 0);
    check_ll("写引擎 ×1", g_stub.write_step_calls - writes0, 1);
    state_get_copy(&s);
    check_ll("窗口投影 列 5", s.core.step[2][5].n, 1);

    /* 窗口外的绝对步可写（引擎绝对索引），但落在投影窗口外：投影不变 */
    apply(CMD_TOGGLE_STEP, 0, 3, 0);
    check_ll("窗口外绝对步写引擎", g_stub.step[0][3].n, 1);
    state_get_copy(&s);
    check_ll("窗口外写不进投影（列 3 = 绝对步 19）", s.core.step[0][3].n, 0);

    /* 同步：一次窗口快照把引擎窗口列（含鼓轨）拉进投影；窗口本身不被 sync 改 */
    uint32_t rw0 = g_stub.read_window_calls;
    state_sync_from_engine();
    check_ll("sync 用一次窗口快照", g_stub.read_window_calls - rw0, 1);
    state_get_copy(&s);
    check_ll("sync 窗口列 1 n", s.core.step[1][1].n, 2);
    check_ll("sync 窗口列 1 note1", s.core.step[1][1].note[1], 64);
    check_ll("sync 鼓轨列 1", s.core.dstep[1].on, 1);
    check_ll("sync 不改窗口 start", s.window.window_start, 16);
    check_ll("sync 不改窗口 len", s.window.window_len, 16);

    /* 钳位：start 向下 16 对齐、上限 48、bank = start/16、len 1..16 */
    apply(CMD_SET_WINDOW, 0, 17, 16);
    step_window(&w);
    check_ll("start 向下对齐", w.window_start, 16);
    check_ll("bank 由 start 派生（a 被忽略）", w.bank, 1);
    apply(CMD_SET_WINDOW, 0, 64, 16);
    step_window(&w);
    check_ll("start 上限 48", w.window_start, 48);
    check_ll("bank 上限 3", w.bank, 3);

    /* len 缩小：focus 夹到窗口内；之后 set_focus 也不得落在隐藏列 */
    state_set_focus(1, 9);
    apply(CMD_SET_WINDOW, 0, 16, 8);
    step_window(&w);
    check_ll("len 8", w.window_len, 8);
    state_get_copy(&s);
    check_ll("窗口缩小后 focus 夹到窗口内", s.focus_step, 7);
    state_set_focus(1, 9);
    state_get_copy(&s);
    check_ll("焦点落在窗口外列被忽略", s.focus_step, 7);
    check_ll("焦点轨仍生效", s.focus_track, 1);

    apply(CMD_SET_WINDOW, 0, 16, 0);
    step_window(&w);
    check_ll("len 下限 1", w.window_len, 1);
    apply(CMD_SET_WINDOW, 0, 16, 99);
    step_window(&w);
    check_ll("len 上限 16", w.window_len, 16);
}

/* ⑦b sync 与 CMD_SET_WINDOW 交错：引擎快照读完后窗口才变——sync 必须按新窗口
 * 重取一次，不能把旧基投影写回（EDIT 擦除读这份投影）。 */
static int s_hook_calls;

static void window_shift_hook(uint32_t start)
{
    (void)start;
    if (s_hook_calls++ != 0) return; /* 只插一次（嵌套的 SET_WINDOW 读不再触发） */
    apply(CMD_SET_WINDOW, 0, 16, 16); /* 模拟另一写路径在取快照期间落窗 */
}

static void case_window_sync_race(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    /* 引擎内容：绝对步 16（新基列 0）与绝对步 20（新基列 4） */
    g_stub.step[0][16] = (sloop_step_t){ .n = 1, .note = { 67, 0, 0, 0 }, .vel = 90, .time = 0 };
    g_stub.step[0][20] = (sloop_step_t){ .n = 1, .note = { 72, 0, 0, 0 }, .vel = 90, .time = 0 };
    apply(CMD_SET_WINDOW, 0, 0, 16); /* 先落在基 0 */

    uint32_t rw0 = g_stub.read_window_calls;
    s_hook_calls = 0;
    g_stub_on_read_window = window_shift_hook;
    state_sync_from_engine();
    g_stub_on_read_window = NULL;

    /* sync 读 1 次（基 0）→ 钩子内 SET_WINDOW 自己读 1 次（基 16）→ sync 复核后
     * 按新基重取 1 次，共 3 次；写回的一定是新基投影。 */
    check_ll("交错：窗口快照读取次数", g_stub.read_window_calls - rw0, 3);
    step_window_t w;
    step_window(&w);
    check_ll("交错：窗口已切 bank1", w.window_start, 16);
    check_ll("交错：窗口 len", w.window_len, 16);
    state_get_copy(&s);
    check_ll("交错：投影基 = 新窗口（列 0 = 绝对步 16）", s.core.step[0][0].note[0], 67);
    check_ll("交错：投影基 = 新窗口（列 4 = 绝对步 20）", s.core.step[0][4].note[0], 72);
    check_ll("交错：旧基内容未混入（列 5 空）", s.core.step[0][5].n, 0);
}

/* ⑧ 参数命令：单 SET 以批量入口（N=1）原样下发、钳位由引擎做；BATCH N 对只一次
 * 引擎调用且入参保序；越界 id 逐条忽略、不中止整批；track/p/n 越界整条丢弃。
 * stub 的范围表是简化模拟（0..127；P_E0=63 的 kit 槽 0..3），真实描述符范围由
 * 设备 PARAM/ENG DESC 门禁覆盖。 */
static void case_set_param(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    /* p[] 是**选中轨**投影：先选轨 1，后续写轨 1 */
    apply(CMD_SET_TRACK, 1, 0, 0);

    apply(CMD_SET_PARAM, 1, 1, 50);
    check_ll("单 SET 一次引擎调用", g_stub.set_params_calls, 1);
    check_ll("单 SET 轨号", g_stub.last_params_track, 1);
    check_ll("单 SET 对数", g_stub.last_params_n, 1);
    check_ll("单 SET 原样下发 id", g_stub.last_param_ids[0], 1);
    check_ll("单 SET 原样下发值", g_stub.last_param_values[0], 50);
    check_ll("单 SET 落引擎", g_stub.param[1][1], 50);
    state_get_copy(&s);
    check_ll("投影 p[1]", s.p[1], 50);

    /* 状态层不钳位：值原样进引擎，投影回读引擎钳位后的真值 */
    apply(CMD_SET_PARAM, 1, 1, 500);
    check_ll("钳位值仍原样下发", g_stub.last_param_values[0], 500);
    state_get_copy(&s);
    check_ll("投影采纳引擎钳位", s.p[1], 127);

    /* v 是 uint16：双极值以 16 位补码下发，引擎按 int16 解读 */
    apply(CMD_SET_PARAM, 1, 5, (uint16_t)(int16_t)-1);
    check_ll("补码原样下发", g_stub.last_param_values[0], -1);

    /* 不可描述的 id：引擎逐条忽略，投影不动 */
    uint32_t invalid0 = g_stub.invalid_param_ids;
    apply(CMD_SET_PARAM, 1, 200, 7);
    check_ll("越界 id 引擎忽略", g_stub.invalid_param_ids - invalid0, 1);
    state_get_copy(&s);
    check_ll("越界 id 不改投影", s.p[1], 127);

    /* 批量入口的 id 是 uint8：>255 的 id 无法表达，不调引擎 */
    uint32_t calls0 = g_stub.set_params_calls;
    apply(CMD_SET_PARAM, 1, 999, 7);
    check_ll("id>255 不调引擎", g_stub.set_params_calls - calls0, 0);

    /* 越界轨丢弃 */
    apply(CMD_SET_PARAM, 4, 0, 10);
    check_ll("越界轨不调引擎", g_stub.set_params_calls - calls0, 0);

    /* 鼓轨 P_E0 = kit：状态层不区分轨别，钳位由引擎（这里 stub 模拟 0..3） */
    apply(CMD_SET_PARAM, 3, STUB_ENGINE_P_E0, 99);
    check_ll("鼓轨 P_E0 落引擎（stub kit 0..45）", g_stub.param[3][STUB_ENGINE_P_E0], 45);
}

/* ⑨ 批量参数：真批量（一次引擎调用）+ 截断/空载荷/非选中轨语义。 */
static void case_set_param_batch(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    apply(CMD_SET_TRACK, 1, 0, 0); /* p[] 是选中轨投影：选轨 1 */

    const sloop_param_pair_t pairs[] = {
        { .id = 2, .v = 100 }, { .id = 1, .v = 20 }, { .id = 200, .v = 5 }, { .id = 29, .v = 33 },
    };
    uint32_t calls0 = g_stub.set_params_calls;
    apply_param_batch(1, pairs, 4);
    check_ll("批量只一次引擎调用", g_stub.set_params_calls - calls0, 1);
    check_ll("批量轨号", g_stub.last_params_track, 1);
    check_ll("批量对数", g_stub.last_params_n, 4);
    check_ll("批量 id 保序 0（非零，丢首项必失败）", g_stub.last_param_ids[0], 2);
    check_ll("批量 id 保序 1", g_stub.last_param_ids[1], 1);
    check_ll("批量 id 保序 2", g_stub.last_param_ids[2], 200);
    check_ll("批量 id 保序 3", g_stub.last_param_ids[3], 29);
    check_ll("批量值保序 0", g_stub.last_param_values[0], 100);
    check_ll("批量值保序 3", g_stub.last_param_values[3], 33);
    check_ll("批量越界 id 逐条忽略", g_stub.invalid_param_ids, 1);
    check_ll("批量有效项落地 2", g_stub.param[1][2], 100);
    check_ll("批量有效项落地 29", g_stub.param[1][29], 33);
    state_get_copy(&s);
    check_ll("批量投影 p[2]", s.p[2], 100);
    check_ll("批量投影 p[29]", s.p[29], 33);

    /* 写非选中轨：引擎落地，但 p[]（选中轨投影）不动 */
    const sloop_param_pair_t other[] = { { .id = 0, .v = 7 } };
    apply_param_batch(2, other, 1);
    check_ll("非选中轨值落引擎", g_stub.param[2][0], 7);
    state_get_copy(&s);
    check_ll("非选中轨写入不动投影", s.p[2], 100);

    /* 超上限：只应用前 APP_STATE_PARAM_BATCH_MAX 条（其余忽略 + 设备侧告警） */
    static sloop_param_pair_t big[APP_STATE_PARAM_BATCH_MAX + 1u];
    for (uint32_t i = 0; i < APP_STATE_PARAM_BATCH_MAX + 1u; i++) {
        big[i].id = 1;
        big[i].v = (int16_t)i;
    }
    calls0 = g_stub.set_params_calls;
    apply_param_batch(0, big, APP_STATE_PARAM_BATCH_MAX + 1u);
    check_ll("超上限仍一次调用", g_stub.set_params_calls - calls0, 1);
    check_ll("超上限截断到 MAX", g_stub.last_params_n, APP_STATE_PARAM_BATCH_MAX);

    /* 空载荷 / n=0 / 越界轨：整条丢弃 */
    calls0 = g_stub.set_params_calls;
    apply_param_batch(0, NULL, 4);
    apply_param_batch(0, pairs, 0);
    apply_param_batch(4, pairs, 1);
    check_ll("空/越界载荷不调引擎", g_stub.set_params_calls - calls0, 0);
}

/* ⑩ 参数投影：sync 一次 read_params 采纳引擎真值（外部改动）；p 跟随选中轨。 */
static void case_param_projection_sync(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    state_get_copy(&s);
    check_ll("init 投影 g_bpm", s.g[0], 90);

    stub_engine_set_param(0, 0, 42);
    stub_engine_set_param(0, 29, 12);
    stub_engine_set_global(1, 33);
    uint32_t rp0 = g_stub.read_params_calls;
    state_sync_from_engine();
    check_ll("sync 一次 read_params", g_stub.read_params_calls - rp0, 1);
    state_get_copy(&s);
    check_ll("sync 采纳引擎 p[0]", s.p[0], 42);
    check_ll("sync 采纳引擎 p[29]", s.p[29], 12);
    check_ll("sync 采纳引擎 g[1]", s.g[1], 33);

    /* 引擎侧外部改选轨 + 轨 2 参数：sync 后投影 p 换成轨 2 的值 */
    g_stub.snap.selected_track = 2;
    stub_engine_set_param(2, 1, 77);
    state_sync_from_engine();
    state_get_copy(&s);
    check_ll("sync 后选中轨 2", s.core.selected_track, 2);
    check_ll("投影跟随选中轨", s.p[1], 77);
}

/* ⑪ 选轨即刷参数投影：p[] 必须与选中轨同拍切换（不等下一拍 sync）——否则参数页/
 * STATE SNAP 可能落在「标题=新轨、参数=旧轨」的一帧里。 */
static void case_select_track_refresh(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    /* 各轨参数各不同（P_LEVEL=0 / P_ATK=1）：p[] 换轨必须换值 */
    stub_engine_set_param(0, 0, 11);
    stub_engine_set_param(1, 0, 22);
    stub_engine_set_param(2, 0, 33);
    stub_engine_set_param(2, 1, 44);
    state_sync_from_engine(); /* 选轨 0：p 取轨 0 */
    state_get_copy(&s);
    check_ll("基线 p[0]=轨 0", s.p[0], 11);

    /* 选轨 2：**不调 sync**，投影立即换成轨 2 的值 */
    apply(CMD_SET_TRACK, 2, 0, 0);
    state_get_copy(&s);
    check_ll("选轨即换投影 p[0]", s.p[0], 33);
    check_ll("选轨即换投影 p[1]", s.p[1], 44);
    check_ll("选中轨字段", s.core.selected_track, 2);

    /* CMD_SELECT_TRACK 走同一路径（TRACKS 卡点选） */
    apply(CMD_SELECT_TRACK, 1, 0, 0);
    state_get_copy(&s);
    check_ll("SELECT_TRACK 即换投影 p[0]", s.p[0], 22);

    /* 选回轨 0：投影回到轨 0 的值 */
    apply(CMD_SET_TRACK, 0, 0, 0);
    state_get_copy(&s);
    check_ll("选回轨 0 立即投影", s.p[0], 11);
}

/* ⑩ 全局参数：CMD_SET_GLOBAL 经引擎写入口落地并回读 g[]；G_BPM 同拍更新 core.bpm。 */
static void case_set_global(void)
{
    stub_engine_reset();
    state_init();
    app_state_t s;

    /* G_TUNE（id 3，F_INT -50..50）：值按 16 位补码下发，投影采纳引擎回读 */
    apply(CMD_SET_GLOBAL, 3, 0, (uint16_t)(int16_t)-20);
    check_ll("G_TUNE 一次 set_global", g_stub.set_global_calls, 1);
    check_ll("G_TUNE 原样下发 id", g_stub.last_global.id, 3);
    check_ll("G_TUNE 补码原样下发", g_stub.last_global.value, -20);
    state_get_copy(&s);
    check_ll("投影 g[3]", s.g[3], -20);
    check_ll("g[0] 不受影响", s.g[0], 90);

    /* G_BPM：core.bpm 与 g[0] 同拍跟随（不等下一拍 sync） */
    apply(CMD_SET_GLOBAL, 0, 0, 137);
    state_get_copy(&s);
    check_ll("G_BPM 落引擎", g_stub.last_global.value, 137);
    check_ll("core.bpm 同拍跟随", s.core.bpm, 137);
    check_ll("投影 g[0] 跟随", s.g[0], 137);

    /* 越界 id：不调引擎、不改投影 */
    uint32_t calls0 = g_stub.set_global_calls;
    apply(CMD_SET_GLOBAL, SLOOP_CORE_G_COUNT + 5u, 0, 1);
    check_ll("越界 id 不调引擎", g_stub.set_global_calls - calls0, 0);
    state_get_copy(&s);
    check_ll("越界 id 不改投影", s.g[0], 137);
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
    check_ll("越界 toggle 连读都不做", g_stub.read_step_calls, 0);

    apply(CMD_NOTE_ON, 4, 0, 60);
    apply(CMD_NOTE_ON, 0, 0, 200);
    check_ll("note 越界不调引擎", g_stub.note_calls, 0);

    uint32_t mix0 = g_stub.set_mute_calls + g_stub.set_solo_calls + g_stub.select_calls;
    apply(CMD_MUTE, 9, 0, 0);
    apply(CMD_SOLO, 9, 0, 0);
    apply(CMD_SET_TRACK, 7, 0, 0);
    check_ll("越界混音命令不调引擎",
             (long long)(g_stub.set_mute_calls + g_stub.set_solo_calls + g_stub.select_calls - mix0), 0);
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
    run("TOGGLE_STEP 关步折叠 TIE 连缀", case_toggle_step_tie_collapse);
    run("MUTE/SOLO 引擎写入口与 sync 真值", case_mute_solo);
    run("ERASE_NOTE 删除聚焦步音符（RMW/字段随移）", case_erase_note);
    run("步窗抽象：默认/映射/投影/钳位", case_step_window);
    run("步窗 sync 与 SET_WINDOW 交错按新基重取", case_window_sync_race);
    run("参数命令：单 SET 钳位/越界忽略", case_set_param);
    run("参数批量：一次引擎调用/截断/空载荷", case_set_param_batch);
    run("参数投影：sync 采纳引擎真值/跟随选轨", case_param_projection_sync);
    run("选轨即刷参数投影（不等 sync）", case_select_track_refresh);
    run("state_get_copy 反映 cmd 与 sync", case_copy_and_sync);
    run("全局参数 CMD_SET_GLOBAL 与投影", case_set_global);
    run("越界参数忽略", case_bounds);

    printf("\n%d passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
