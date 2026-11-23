/* SPDX-License-Identifier: GPL-3.0-only
 * P4-C Task 7：PER3 歌曲设置的字节级 host 测试（ADR-009 §6 / ADR-010 §4）。
 *
 * 同源编译：hostsim.c 提供真实上游状态（含 drums.c → drum_synth.c 的 `dsu` 默认银行）、
 * storage.c 提供真实的 A/B 提交 + 32B 头 + CRC + 回读校验 + side record，之上只有
 * 两个钩子换成了本文件的 8KB 假 flash（0xFC000/0xFD000 两个扇区）。被测代码是
 * store_tab5.c 用的同一份 `src/song_persist.h`。
 *
 * 覆盖：canonical 1,552B + 默认 80B side；arrangement 偏移 48 / entries 52；PER3
 * 历史长度与 MIDI 尾段的逐字节保留；PER1/PER2 迁移；未知 magic / 损坏长度拒绝且
 * 不覆盖；非法 arrangement 回默认；dirty 记账（失败保留）；真实 st_save3 往返与
 * 掉电窗口下旧副本仍可读。
 * 退出码 = 失败项数。 */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "../third_party/sloop-fm1/tests/hostsim.c"
#undef main

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scratch.h" /* main_scratch（storage.c 的 st_buf） */

/* ---- 假 flash：只有 OBJ_SETTINGS 的两个扇区（0xFC000 / 0xFD000） ---------------- */
#define FL_BASE 0xFC000u
#define FL_SECTOR 4096u
static uint8_t s_flash[2u * FL_SECTOR];

static uint32_t s_writes;      /* 写操作计数（fault arm 用） */
static uint32_t s_fault_nth;   /* 0 = 不注入；第 nth 次写命中一次 */
static bool s_fault_half;
static uint32_t s_fault_taken;

static void flash_reset(void)
{
    memset(s_flash, 0xFF, sizeof s_flash);
    s_writes = 0;
    s_fault_nth = 0;
    s_fault_half = false;
    s_fault_taken = 0;
}

static int st_off(uint32_t off, uint32_t n)
{
    if (off < FL_BASE || off + n > FL_BASE + sizeof s_flash) return -1;
    return (int)(off - FL_BASE);
}

static bool flash_fault(void)
{
    if (s_fault_nth == 0) return false;
    if (++s_writes != s_fault_nth) return false;
    s_fault_taken++;
    s_fault_nth = 0;
    return true;
}

static int st_read(uint32_t off, void *dst, uint32_t n)
{
    int at = st_off(off, n);
    if (at < 0) return -1;
    memcpy(dst, s_flash + at, n);
    return 0;
}

static int st_erase(uint32_t off)
{
    if (st_off(off, 0) < 0 || (off % FL_SECTOR) != 0u) return -1;
    if (flash_fault()) return -1;
    memset(s_flash + (off - FL_BASE), 0xFF, FL_SECTOR);
    return 0;
}

static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    int at = st_off(off, n);
    const uint8_t *b = src;
    uint32_t i;
    if (at < 0) return -1;
    if (flash_fault()) {
        if (s_fault_half)
            for (i = 0; i < n / 2u; i++) s_flash[at + i] &= b[i];
        return -1;
    }
    for (i = 0; i < n; i++) s_flash[at + i] &= b[i]; /* NOR：只能把 1 写成 0 */
    return 0;
}

#include "../third_party/sloop-fm1/firmware/src/storage.c"

/* 上游 project.c 的格式层（PROJ_HOST）+ arranger_scene.c：与 seq2_test.c 同一套，
 * 只为给 hostsim 的 seq.c 提供 arrangement_apply/section_bars/song_*（链接需要）。 */
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NPART ? i : 0; }
#include "../third_party/sloop-fm1/firmware/src/project.c"
static struct { uint8_t force; } ui;
static uint8_t sync_reload;
#include "../third_party/sloop-fm1/firmware/src/arranger_scene.c"

#include "../firmware/components/sloop_core/src/song_persist.h"

/* ---- 断言 ---------------------------------------------------------------------- */
static int fails;

static void check(int ok, const char *what)
{
    printf("persist-host: %-74s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7u);
}

static uint32_t word(const uint8_t *p)
{
    uint32_t w;
    memcpy(&w, p, sizeof w);
    return w;
}

static void put_word(uint8_t *p, uint32_t w)
{
    memcpy(p, &w, sizeof w);
}

/* 构造一份历史 PER3 payload：arrangement 用给定的，其余（含尾段）用可区分字节 */
static void mk_per3(uint8_t *p, uint32_t len, const arr_config_t *arr)
{
    fill(p, len, 0x11);
    put_word(p, SONG_PER_MAGIC);
    memcpy(p + SONG_ARR_OFF, arr, SONG_ARR_LEN);
}

static arr_config_t arr_make(uint8_t count, uint8_t loop)
{
    arr_config_t a;
    arr_defaults(&a);
    a.count = count;
    a.loop = loop;
    return a;
}

int main(void)
{
    uint8_t payload[4096];
    uint8_t side[SONG_SIDE_LEN];
    uint32_t len;
    arr_config_t arr, back;

    /* ---------- 1. canonical：空设置 → 1,552B + 默认 80B side ---------- */
    flash_reset();
    memset(payload, 0xEE, sizeof payload);
    memset(side, 0xEE, sizeof side);
    len = 0;
    arr = arr_make(6, 1);
    arr.entry[0].scene = 2;
    arr.entry[0].bars = 3;
    song_per_kind_t kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    check(kind == SONG_PER_NEW, "canonical: kind = NEW");
    check(len == SONG_PER_CANON && len == 1552u, "canonical: 88B persist + 1,464B DSU = 1,552B");
    check(word(payload) == SONG_PER_MAGIC, "canonical: magic PER3");
    check(word(payload + 4) == 0u && word(payload + 8) == 0u && word(payload + 12) == 0u,
          "canonical: palette/lowcut/zoom = 0");
    check(word(payload + 16) == SONG_PANEL_MAGIC, "canonical: 默认 panel（PAN5）");
    check(word(payload + SONG_ARR_OFF) == (uint32_t)(6u | 1u << 8), "canonical: arrangement @48 = count/loop");
    check(payload[SONG_ARR_OFF + 4u] == 2u && payload[SONG_ARR_OFF + 5u] == 3u,
          "canonical: entries @52 = entry[0].scene/bars");
    check(word(payload + 84) == 0u, "canonical: lights = 0（2.2 迁移口径）");
    check(word(payload + SONG_DSU_OFF) == 0x31555344u && word(payload + SONG_DSU_OFF + 4u) == (1u | 4u << 16),
          "canonical: 默认 DSU 银行 @88（DSU1 / ver 1 / n 4）");
    check(side[0] == 0u && word(side + 40) == SONG_FAVS_MAGIC && word(side + 76) != 0u,
          "canonical: 默认 side（theme 0 + FAV1 + CRC）");
    check(song_side_valid(side), "canonical: side CRC 自校验通过");
    /* 侧区全 0xFF = 从未写过 ⇒ 写默认；有内容（哪怕自校验不过，如 2.7 beta 只写 40B 主题）
     * ⇒ 原样保留：P4-E 之前本固件没有主题状态，用默认值覆盖等于删掉用户的主题。 */
    {
        static uint8_t scratch[SONG_PER_CANON];
        uint8_t blank[SONG_SIDE_LEN], keep[SONG_SIDE_LEN], out_side[SONG_SIDE_LEN];
        arr_config_t a2 = arr_make(4, 0);
        uint32_t blen = 0;
        memset(blank, 0xFF, sizeof blank);
        check(song_side_blank(blank), "side: 全 0xFF = 空白侧区");
        song_per_kind_t k2 = song_per_build(scratch, &blen, sizeof scratch, &a2, out_side, !song_side_blank(blank));
        check(k2 == SONG_PER_NEW && song_side_valid(out_side), "side: 空白侧区 → 默认记录");
        memset(keep, 0xFF, sizeof keep); /* 2.7 beta：只有 40B 主题，没有 FAV1/CRC */
        memset(keep, 0, 40);
        check(!song_side_valid(keep), "side: 2.7 beta 主题记录确实自校验不过");
        memcpy(out_side, keep, sizeof out_side);
        uint32_t klen = 0;
        k2 = song_per_build(scratch, &klen, sizeof scratch, &a2, out_side, !song_side_blank(keep));
        check(k2 == SONG_PER_NEW && memcmp(out_side, keep, sizeof out_side) == 0,
              "side: 有内容的侧区原样保留（含自校验不过的主题记录）");
    }
    check(song_per_kind(payload, len) == SONG_PER3, "canonical: 读回分类为 PER3");
    memset(&back, 0, sizeof back);
    check(song_per_read(payload, len, &back) == SONG_PER3 && memcmp(&back, &arr, sizeof arr) == 0,
          "canonical: arrangement 读回（36B 含 reserved）");
    check(back.entry[0].scene == 2 && back.entry[0].bars == 3, "canonical: entry[0] 偏移 52 语义");

    /* ---------- 2. 合法 PER3：整段保留，只替换 arrangement 36B ---------- */
    arr = arr_make(2, 0);
    arr.entry[0].scene = 1;
    len = SONG_PER_CANON + 32u; /* 2.6 的 32B MIDI 尾段 */
    mk_per3(payload, len, &arr);
    uint8_t before[4096];
    memcpy(before, payload, len);
    fill(side, SONG_SIDE_LEN, 0x5A); /* 一份「用户」side（不校验 CRC：调用方给 side_valid=1） */
    uint8_t side_before[SONG_SIDE_LEN];
    memcpy(side_before, side, sizeof side);
    arr_config_t next = arr_make(3, 1);
    next.entry[1].scene = 3;
    next.entry[1].bars = 8;
    kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
    check(kind == SONG_PER3 && len == SONG_PER_CANON + 32u, "PER3+MIDI(32B): 长度保持 1,584B");
    check(memcmp(payload, before, SONG_ARR_OFF) == 0, "PER3+MIDI: 0..47（palette/panel）逐字节不变");
    check(memcmp(payload + 84, before + 84, 4u) == 0, "PER3+MIDI: lights @84 逐字节不变");
    check(memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0,
          "PER3+MIDI: 88..1,551 = 当前 dsu（P4-D：替换 flash 里的旧值）");
    check(memcmp(payload + SONG_PER_CANON, before + SONG_PER_CANON, 32u) == 0,
          "PER3+MIDI: MIDI 尾段（1,552..1,583）逐字节不变");
    check(memcmp(payload + SONG_ARR_OFF, &next, SONG_ARR_LEN) == 0, "PER3+MIDI: arrangement 已替换");
    check(memcmp(side, side_before, sizeof side) == 0, "PER3+MIDI: side 原样随副本提交");

    len = SONG_PER_CANON + 40u; /* 2.6/2.7 的完整 MIDI 尾段 */
    mk_per3(payload, len, &arr);
    memcpy(before, payload, len);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
    check(kind == SONG_PER3 && len == SONG_PER_CANON + 40u, "PER3+MIDI(40B): 长度保持 1,592B");
    check(memcmp(payload + SONG_PER_CANON, before + SONG_PER_CANON, 40u) == 0,
          "PER3+MIDI(40B): 尾段逐字节不变");

    len = SONG_PER_CANON; /* 无尾段：已有 DSU 被当前 RAM 的 dsu 替换（P4-D Task 5） */
    mk_per3(payload, len, &arr);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
    check(kind == SONG_PER3 && len == SONG_PER_CANON, "PER3(1,552B): 长度不变");
    check(memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0,
          "PER3(1,552B): 已有 DSU 被当前 dsu 替换");

    /* ---------- 3. 合法但尚未带 DSU 的 PER3 → 补默认 DSU ---------- */
    len = SONG_PER_BASE; /* 88B */
    mk_per3(payload, len, &arr);
    memcpy(before, payload, len);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
    check(kind == SONG_PER3 && len == SONG_PER_CANON, "PER3(88B): 下次保存补默认 DSU → 1,552B");
    check(memcmp(payload, before, SONG_ARR_OFF) == 0 && memcmp(payload + 84, before + 84, 4u) == 0,
          "PER3(88B): 头部与 lights 逐字节不变");
    check(word(payload + SONG_DSU_OFF) == 0x31555344u, "PER3(88B): 追加的 DSU 有效");

    len = SONG_PER_V22; /* 84B：2.2 的 persist_t（没有 lights） */
    mk_per3(payload, len, &arr);
    memcpy(before, payload, len);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
    check(kind == SONG_PER3 && len == SONG_PER_CANON, "PER3(84B, 2.2): 补 lights=0 + DSU → 1,552B");
    check(memcmp(payload, before, SONG_ARR_OFF) == 0 && word(payload + 84) == 0u,
          "PER3(84B): 头部不变、lights 补 0");

    /* ---------- 4. PER1/PER2 迁移 ---------- */
    memset(payload, 0, sizeof payload);
    len = SONG_PER2_LEN; /* "PER2": palette/panel，无歌曲 */
    put_word(payload, SONG_PER2_MAGIC);
    put_word(payload + 4, 3u);  /* palette */
    put_word(payload + 8, 1u);  /* lowcut */
    put_word(payload + 12, 1u); /* zoom */
    memcpy(payload + 16, SONG_PANEL_DEF, SONG_PANEL_LEN);
    payload[16 + 4] = 9; /* 让 panel 与默认不同（可观测「原样保留」） */
    kind = song_per_build(payload, &len, sizeof payload, &next, side, false);
    check(kind == SONG_PER2 && len == SONG_PER_CANON, "PER2 → PER3 canonical（1,552B）");
    check(word(payload) == SONG_PER_MAGIC, "PER2 迁移: magic 变 PER3");
    check(word(payload + 4) == 3u && word(payload + 8) == 1u && word(payload + 12) == 1u,
          "PER2 迁移: palette/lowcut/zoom 保留");
    check(payload[16 + 4] == 9 && word(payload + 16) == SONG_PANEL_MAGIC, "PER2 迁移: panel 原样保留");
    check(word(payload + 84) == 0u, "PER2 迁移: lights = 0");
    check(memcmp(payload + SONG_ARR_OFF, &next, SONG_ARR_LEN) == 0, "PER2 迁移: arrangement = 当前歌曲");
    check(word(payload + SONG_DSU_OFF) == 0x31555344u, "PER2 迁移: 追加默认 DSU");
    check(song_side_valid(side), "PER2 迁移: side = 默认（旧格式没有 side）");
    memset(&back, 0, sizeof back);
    check(song_per_read(payload, len, &back) == SONG_PER3, "PER2 迁移: 结果读回为 PER3");

    /* PER2 的 panel magic 坏 → 取默认 panel（上游 panel_init 会重置，随后的保存写默认） */
    len = SONG_PER2_LEN;
    put_word(payload, SONG_PER2_MAGIC);
    memset(payload + 16, 0x77, SONG_PANEL_LEN);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, false);
    check(kind == SONG_PER2 && memcmp(payload + 16, SONG_PANEL_DEF, SONG_PANEL_LEN) == 0,
          "PER2 迁移: 坏 panel → 默认 panel");

    len = SONG_PER1_LEN; /* "PER1": palette word + panel */
    memset(payload, 0, sizeof payload);
    put_word(payload, SONG_PER1_MAGIC);
    put_word(payload + 4, 5u);
    memcpy(payload + 8, SONG_PANEL_DEF, SONG_PANEL_LEN);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, false);
    check(kind == SONG_PER1 && len == SONG_PER_CANON, "PER1 → PER3 canonical");
    check(word(payload) == SONG_PER_MAGIC && word(payload + 4) == 5u && word(payload + 8) == 0u &&
              word(payload + 12) == 0u,
          "PER1 迁移: palette 保留、lowcut/zoom = 0");
    check(memcmp(payload + 16, SONG_PANEL_DEF, SONG_PANEL_LEN) == 0, "PER1 迁移: panel 原样保留");

    /* ---------- 5. 拒绝：未知 magic / 损坏长度，不写不覆盖 ---------- */
    len = SONG_PER_BASE;
    fill(payload, len, 0x21);
    put_word(payload, 0xDEADBEEFu);
    memcpy(before, payload, len);
    memset(side, 0x33, sizeof side);
    memcpy(side_before, side, sizeof side);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, false);
    check(kind == SONG_PER_BAD && len == SONG_PER_BASE, "未知 magic: 拒绝（长度不变）");
    check(memcmp(payload, before, SONG_PER_BASE) == 0 && memcmp(side, side_before, sizeof side) == 0,
          "未知 magic: payload 与 side 一个字节都没改");
    len = SONG_PER_BASE;
    mk_per3(payload, len, &arr);
    put_word(payload, SONG_PER_MAGIC);
    len = 200; /* PER3 的长度不是任何历史格式 */
    kind = song_per_build(payload, &len, sizeof payload, &next, side, false);
    check(kind == SONG_PER_BAD, "PER3 非法长度（200B）: 拒绝");

    /* ---------- 6. 非法 arrangement 回默认；set 侧校验 ---------- */
    arr_config_t bad = arr_make(4, 0);
    bad.count = 0;
    check(!song_arr_valid(&bad), "arrangement 校验: count = 0 拒绝");
    bad.count = 17;
    check(!song_arr_valid(&bad), "arrangement 校验: count = 17 拒绝");
    bad = arr_make(4, 2);
    check(!song_arr_valid(&bad), "arrangement 校验: loop = 2 拒绝");
    bad = arr_make(4, 0);
    bad.entry[2].scene = 4;
    check(!song_arr_valid(&bad), "arrangement 校验: scene = 4 拒绝");
    bad.entry[2].scene = 0;
    bad.entry[2].bars = 65;
    check(!song_arr_valid(&bad), "arrangement 校验: bars = 65 拒绝");
    bad.entry[2].bars = 0;
    check(!song_arr_valid(&bad), "arrangement 校验: bars = 0 拒绝");
    bad = arr_make(4, 0);
    bad.entry[7].scene = 9; /* count 之外：上游 arr_valid 不看，保真保留 */
    check(song_arr_valid(&bad), "arrangement 校验: count 之外的 entry 不参与（上游口径）");

    len = SONG_PER_CANON;
    mk_per3(payload, len, &arr);
    memcpy(payload + SONG_ARR_OFF, &bad, SONG_ARR_LEN); /* entry[7].scene = 9 但 count = 4：合法 */
    memset(&back, 0, sizeof back);
    check(song_per_read(payload, len, &back) == SONG_PER3 && back.count == 4u, "读回: 合法 arrangement 原样");
    put_word(payload + SONG_ARR_OFF, 0u); /* count = 0 */
    memset(&back, 0xff, sizeof back);
    check(song_per_read(payload, len, &back) == SONG_PER3 && back.count == 4u && back.loop == 0u &&
              back.entry[3].scene == 3u && back.entry[3].bars == 4u,
          "读回: 非法 arrangement → 默认歌曲（arr_defaults：4×4 bars）");
    /* set 侧：非法 arrangement 不组装、不写 */
    len = SONG_PER_CANON;
    mk_per3(payload, len, &arr);
    memcpy(before, payload, len);
    arr_config_t invalid = arr_make(0, 0);
    kind = song_per_build(payload, &len, sizeof payload, &invalid, side, true);
    check(kind == SONG_PER_BAD && memcmp(payload, before, len) == 0, "set: 非法 arrangement 不组装");

    /* ---------- 7. dirty 记账：失败与并发修改都保留 ---------- */
    song_pending_t pend;
    memset(&pend, 0, sizeof pend);
    check(!pend.dirty, "dirty: 初始干净");
    song_pending_mark(&pend);
    uint32_t gen = song_pending_gen(&pend);
    check(pend.dirty && gen == 1u, "dirty: 标记 → dirty、gen 1");
    song_pending_done(&pend, false, gen);
    check(pend.dirty, "dirty: 写失败保留（下个窗口重试）");
    song_pending_done(&pend, true, gen);
    check(!pend.dirty, "dirty: 写成功且无新修改 → 清");
    song_pending_mark(&pend);
    gen = song_pending_gen(&pend);
    song_pending_mark(&pend); /* flush 期间又来一次修改 */
    song_pending_done(&pend, true, gen);
    check(pend.dirty, "dirty: 写成功但期间有新修改 → 保留");

    /* ---------- 7b. 读不到歌曲时落到上游默认（persist_boot 的第一步） ----------
     * 上游 persist_boot 先 arr_defaults，只有合法 PER3 才覆盖；`arrangement` 是 `.bss`，
     * 上电/自检 restore 后全零（count=0 非法）——「保持默认歌曲」必须真的写进去。 */
    {
        arr_config_t def, src, out;
        memset(&def, 0xA5, sizeof def);
        memset(&out, 0, sizeof out); /* 已知非默认（count=0）：缺 arr_defaults 必被抓到 */
        check(!song_per_loaded(SONG_PER_BAD, &def, &out), "loaded: 损坏对象 → false（拒绝覆盖）");
        check(out.count == 4u && out.loop == 0u, "loaded: 损坏对象 → 默认歌曲 count=4 loop=0");
        check(out.entry[0].scene == 0u && out.entry[1].scene == 1u && out.entry[2].scene == 2u &&
                  out.entry[3].scene == 3u && out.entry[0].bars == 4u && out.entry[3].bars == 4u,
              "loaded: 默认歌曲 = A/B/C/D 各 4 小节（上游 arr_defaults）");
        check(song_arr_valid(&out), "loaded: 默认歌曲结构合法（count 非零，可被 song_set/播放接受）");
        memset(&out, 0, sizeof out);
        check(song_per_loaded(SONG_PER_NEW, &def, &out) && out.count == 4u &&
                  out.entry[2].scene == 2u && out.entry[2].bars == 4u,
              "loaded: 空设置 → 默认歌曲（true，不是错误）");
        memset(&src, 0, sizeof src);
        arr_defaults(&src);
        src.count = 2;
        src.entry[0].scene = 3;
        src.entry[0].bars = 7;
        check(song_per_loaded(SONG_PER3, &src, &out) && memcmp(&out, &src, sizeof src) == 0,
              "loaded: 合法 PER3 → 读回值原样生效");
    }

    /* ---------- 8. 真实 st_save3/st_load 往返（storage.c + 假 flash） ---------- */
    flash_reset();
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    st_side = side;
    st_side_len = sizeof side;
    int rc = st_save3(OBJ_SETTINGS, payload, len, 0, 0, 0, 0);
    st_side = 0;
    st_side_len = 0;
    check(rc == 0, "flash: canonical 写入（st_save3）");
    static uint8_t got[4096];
    int n = st_load(OBJ_SETTINGS, got, sizeof got);
    check(n == (int)SONG_PER_CANON && memcmp(got, payload, (size_t)n) == 0, "flash: 读回逐字节相同（1,552B）");
    static uint8_t side_got[ST_SIDE_MAX];
    check(st_side_load(OBJ_SETTINGS, side_got, SONG_SIDE_LEN) == 0 &&
              memcmp(side_got, side, SONG_SIDE_LEN) == 0,
          "flash: side record 随副本提交（80B 逐字节）");
    memset(&back, 0, sizeof back);
    check(song_per_read(got, (uint32_t)n, &back) == SONG_PER3 && memcmp(&back, &arr, sizeof arr) == 0,
          "flash: 读回 arrangement 与写入一致");

    /* 改歌曲再存：长度/尾段/side 保持 */
    check(st_side_load(OBJ_SETTINGS, side_got, SONG_SIDE_LEN) == 0, "flash: 读回 side 作为下次提交的输入");
    uint32_t got_len = (uint32_t)n;
    kind = song_per_build(got, &got_len, sizeof got, &next, side_got, true);
    len = got_len;
    st_side = side_got;
    st_side_len = SONG_SIDE_LEN;
    rc = st_save3(OBJ_SETTINGS, got, len, 0, 0, 0, 0);
    st_side = 0;
    st_side_len = 0;
    check(rc == 0 && len == SONG_PER_CANON, "flash: 第二次保存（改歌曲）");
    int n2 = st_load(OBJ_SETTINGS, got, sizeof got);
    memset(&back, 0, sizeof back);
    check(n2 == (int)SONG_PER_CANON && song_per_read(got, (uint32_t)n2, &back) == SONG_PER3 &&
              memcmp(&back, &next, sizeof next) == 0,
          "flash: 第二次保存后 arrangement 是新的");
    check(st_side_load(OBJ_SETTINGS, side_got, SONG_SIDE_LEN) == 0 &&
              memcmp(side_got, side, SONG_SIDE_LEN) == 0,
          "flash: 第二次保存 side 仍逐字节相同");
    {
        st_hdr_t h;
        check(st_current(OBJ_SETTINGS, &h) == 1 && h.seq == 2u, "flash: A/B 交替（seq = 2，写回 copy 1）");
    }

    /* 未知 magic 的对象：拒绝保存时扇区原样（不写） */
    uint8_t junk[64];
    fill(junk, sizeof junk, 0x41);
    put_word(junk, 0xDEADBEEFu);
    check(st_save(OBJ_SETTINGS, junk, sizeof junk) == 0, "flash: 写入未知 magic 对象");
    static uint8_t raw0[FL_SECTOR], raw1[FL_SECTOR];
    st_read(st_sector(OBJ_SETTINGS, 0), raw0, FL_SECTOR);
    st_read(st_sector(OBJ_SETTINGS, 1), raw1, FL_SECTOR);
    uint32_t junk_len = sizeof junk;
    uint8_t junk_buf[sizeof junk];
    memcpy(junk_buf, junk, sizeof junk);
    kind = song_per_build(junk_buf, &junk_len, sizeof junk_buf, &next, side, false);
    check(kind == SONG_PER_BAD, "flash: 未知 magic 对象 → 拒绝保存（invalid）");
    static uint8_t raw0b[FL_SECTOR], raw1b[FL_SECTOR];
    st_read(st_sector(OBJ_SETTINGS, 0), raw0b, FL_SECTOR);
    st_read(st_sector(OBJ_SETTINGS, 1), raw1b, FL_SECTOR);
    check(memcmp(raw0, raw0b, FL_SECTOR) == 0 && memcmp(raw1, raw1b, FL_SECTOR) == 0,
          "flash: 拒绝时不覆盖原扇区（两个 4KB 逐字节不变）");
    n = st_load(OBJ_SETTINGS, got, sizeof got);
    check(n == (int)sizeof junk && memcmp(got, junk, sizeof junk) == 0, "flash: 原对象仍可读");

    /* 掉电窗口：新副本写失败 → 旧副本（含 arrangement 与 side）仍完整可读 */
    flash_reset();
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    st_side = side;
    st_side_len = sizeof side;
    check(st_save3(OBJ_SETTINGS, payload, len, 0, 0, 0, 0) == 0, "torn: 先写一份完整 canonical");
    uint8_t old_payload[4096], new_payload[4096];
    int old_n = st_load(OBJ_SETTINGS, old_payload, sizeof old_payload);
    uint8_t old_side[SONG_SIDE_LEN];
    check(old_n > 0 && st_side_load(OBJ_SETTINGS, old_side, sizeof old_side) == 0, "torn: 旧副本与 side 可读");
    song_pending_t torn;
    memset(&torn, 0, sizeof torn);
    song_pending_mark(&torn);
    uint32_t tgen = song_pending_gen(&torn);
    uint32_t nlen = (uint32_t)old_n;
    memcpy(new_payload, old_payload, (size_t)old_n);
    song_per_build(new_payload, &nlen, sizeof new_payload, &next, old_side, true);
    s_fault_nth = 2; /* 第 2 次写 = payload 的 prog（第 1 次是擦除） */
    s_fault_half = true;
    st_side = old_side;
    st_side_len = SONG_SIDE_LEN;
    int trc = st_save3(OBJ_SETTINGS, new_payload, nlen, 0, 0, 0, 0);
    st_side = 0;
    st_side_len = 0;
    song_pending_done(&torn, trc == 0, tgen);
    check(trc != 0 && s_fault_taken == 1u, "torn: 写失败且故障命中一次");
    check(torn.dirty, "torn: 写失败保留 dirty（下次窗口重试）");
    int t_n = st_load(OBJ_SETTINGS, got, sizeof got);
    memset(&back, 0, sizeof back);
    check(t_n == old_n && memcmp(got, old_payload, (size_t)t_n) == 0 &&
              song_per_read(got, (uint32_t)t_n, &back) == SONG_PER3 && memcmp(&back, &arr, sizeof arr) == 0,
          "torn: 旧副本的 arrangement 仍可读（不是半截新值）");
    check(st_side_load(OBJ_SETTINGS, side_got, SONG_SIDE_LEN) == 0 && memcmp(side_got, side, SONG_SIDE_LEN) == 0,
          "torn: 旧副本的 side 仍可读");

    /* ---------- 9. DSU 随 PER3 持久化（P4-D Task 5，spec §3.4） ---------- */
    /* `dsu`/`dsu_dirty` 是 drum_synth.c 的 static：本 TU 经 hostsim → drums.c 编入，与固件
     * 是同一份。每次都从工厂银行起（前面的用例只碰 arrangement/side，但显式归一更稳）。 */
    dsu_defaults();
    dsu_dirty = 0;
    arr = arr_make(4, 0);
    next = arr_make(5, 1);

    /* 9a. canonical（没有对象）：88..1,551 逐字节 = 当前 dsu */
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    check(kind == SONG_PER_NEW && len == SONG_PER_CANON &&
              memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0,
          "dsu: canonical 的 88..1,551 逐字节 = 当前 dsu");

    /* 9b. song_per_put_dsu：改一个 lane → 直接写回 88..1,551 → 读回 42 */
    dsu.k[0].s[0].pitch = 42u;
    static dsu_bank_t got_dsu;
    song_per_put_dsu(payload, len);
    memcpy(&got_dsu, payload + SONG_DSU_OFF, sizeof got_dsu);
    check(dsu_valid(&got_dsu) && got_dsu.k[0].s[0].pitch == 42u,
          "dsu: put_dsu 把 pitch=42 写进 88..1,551（读回 42）");
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    memcpy(&got_dsu, payload + SONG_DSU_OFF, sizeof got_dsu);
    check(kind == SONG_PER_NEW && len == SONG_PER_CANON && got_dsu.k[0].s[0].pitch == 42u,
          "dsu: canonical 保存同样携带 pitch=42");
    {
        uint8_t short_buf[SONG_PER_BASE], short0[SONG_PER_BASE];
        fill(short_buf, sizeof short_buf, 0xA5);
        memcpy(short0, short_buf, sizeof short_buf);
        song_per_put_dsu(short_buf, sizeof short_buf);
        check(memcmp(short_buf, short0, sizeof short_buf) == 0,
              "dsu: len < 1,552 时 put_dsu 一个字节都不写");
    }
    /* RAM 里的银行坏了（magic 不对）：put_dsu 先 dsu_defaults（上游 settings_write 同法），
     * 坏字节不落盘。 */
    dsu.magic = 0;
    song_per_put_dsu(payload, len);
    memcpy(&got_dsu, payload + SONG_DSU_OFF, sizeof got_dsu);
    check(dsu_valid(&dsu) && dsu_valid(&got_dsu), "dsu: RAM 银行坏了 → 先落默认再写");

    /* 9c. 已有 DSU 的 PER3：88..1,551 换成当前 RAM，48..83 替换，其余不动 */
    dsu_defaults();
    dsu.k[1].s[3].pitch = 99u; /* flash 里那份「旧 DSU」是 mk_per3 的 0x11 填充 */
    len = SONG_PER_CANON;
    mk_per3(payload, len, &arr);
    memcpy(before, payload, len);
    kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
    check(kind == SONG_PER3 && len == SONG_PER_CANON, "dsu: PER3(1,552B) 长度不变");
    check(memcmp(payload, before, SONG_ARR_OFF) == 0 &&
              memcmp(payload + SONG_LIGHTS_OFF, before + SONG_LIGHTS_OFF, 4u) == 0,
          "dsu: PER3 的 0..47 与 84..87 逐字节不变");
    check(memcmp(payload + SONG_ARR_OFF, &next, SONG_ARR_LEN) == 0, "dsu: PER3 的 48..83 已替换");
    check(memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0,
          "dsu: PER3 的 88..1,551 = 当前 dsu（替换 flash 里的旧值）");
    memcpy(&got_dsu, payload + SONG_DSU_OFF, sizeof got_dsu);
    check(got_dsu.k[1].s[3].pitch == 99u, "dsu: 读回 pitch=99（替换而非保留旧字节）");

    /* 9d. 更长的 PER3：只换 48..83 与 88..1,551，MIDI 尾段逐字节保留 */
    for (uint32_t tail = SONG_MIDI_32; tail <= SONG_MIDI_MAX; tail += 4u) {
        char what[96];
        len = SONG_PER_CANON + tail;
        mk_per3(payload, len, &arr);
        memcpy(before, payload, len);
        dsu.k[2].s[5].pitch = (uint8_t)(100u + tail); /* 每次一个可区分的值 */
        kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
        snprintf(what, sizeof what, "dsu: PER3+%uB 尾段逐字节保留、DSU 已替换", (unsigned)tail);
        check(kind == SONG_PER3 && len == SONG_PER_CANON + tail &&
                  memcmp(payload + SONG_PER_CANON, before + SONG_PER_CANON, tail) == 0 &&
                  memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0,
              what);
    }

    /* 9e. 无 DSU 的 84/88B 旧对象：升级为 1,552B 并带**当前** dsu */
    dsu.k[0].s[0].pitch = 55u;
    for (uint32_t old = SONG_PER_V22; old <= SONG_PER_BASE; old += 4u) { /* 84, 88 */
        char what[96];
        len = old;
        mk_per3(payload, len, &arr);
        memcpy(before, payload, len);
        kind = song_per_build(payload, &len, sizeof payload, &next, side, true);
        snprintf(what, sizeof what, "dsu: PER3(%uB) → 1,552B 带当前 dsu", (unsigned)old);
        check(kind == SONG_PER3 && len == SONG_PER_CANON &&
                  memcmp(payload, before, SONG_ARR_OFF) == 0 &&
                  memcmp(payload + SONG_ARR_OFF, &next, SONG_ARR_LEN) == 0 &&
                  memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0,
              what);
    }

    /* 9f. 未知 magic：照旧拒绝，且组装不动 dsu_dirty */
    len = SONG_PER_BASE;
    fill(payload, len, 0x21);
    put_word(payload, 0xDEADBEEFu);
    dsu_dirty = 1;
    kind = song_per_build(payload, &len, sizeof payload, &next, side, false);
    check(kind == SONG_PER_BAD && dsu_dirty, "dsu: 未知 magic 仍拒绝（dirty 不被组装清掉）");

    /* 9g. dsu_dirty：只有「写成功 + 提交的就是当前 RAM」才清 */
    dsu_defaults();
    dsu.k[0].s[0].pitch = 7u;
    dsu_dirty = 1;
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    song_per_saved(false, payload, len); /* 写失败 */
    check(kind == SONG_PER_NEW && dsu_dirty, "dsu: 写失败保留 dirty");
    dsu.k[0].s[1].pitch = 8u; /* 擦写窗口里又编辑 → 刚组装的 payload 已过时 */
    song_per_saved(true, payload, len);
    check(dsu_dirty, "dsu: 写期间又被编辑 → 保留 dirty（payload 过时）");
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    song_per_saved(true, payload, len);
    check(kind == SONG_PER_NEW && !dsu_dirty, "dsu: 写成功且字节一致 → 清 dsu_dirty");

    /* 9h. 真实 A/B 提交：写成功清 dirty，flash 读回的 DSU 含保存时的编辑 */
    flash_reset();
    dsu_defaults();
    dsu.k[2].s[5].pitch = 77u;
    dsu_dirty = 1;
    len = 0;
    kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
    st_side = side;
    st_side_len = sizeof side;
    rc = st_save3(OBJ_SETTINGS, payload, len, 0, 0, 0, 0);
    st_side = 0;
    st_side_len = 0;
    song_per_saved(rc == 0, payload, len);
    check(rc == 0 && !dsu_dirty, "dsu: 真实 st_save3 成功 → dsu_dirty = 0");
    n = st_load(OBJ_SETTINGS, got, sizeof got);
    memcpy(&got_dsu, got + SONG_DSU_OFF, sizeof got_dsu);
    check(n == (int)SONG_PER_CANON && dsu_valid(&got_dsu) && got_dsu.k[2].s[5].pitch == 77u,
          "dsu: flash 读回的 DSU 含保存时的编辑（pitch=77）");

    /* 9i. flash 上的 88B 旧对象 → 保存升级为 1,552B（dsu_boot 口径：len ≥ 1,552 + DSU 有效） */
    flash_reset();
    dsu_defaults();
    dsu.k[3].s[9].pitch = 66u;
    dsu_dirty = 0;
    len = SONG_PER_BASE;
    mk_per3(payload, len, &arr);
    check(st_save(OBJ_SETTINGS, payload, len) == 0, "dsu: flash 写入 88B 旧 PER3");
    n = st_load(OBJ_SETTINGS, got, sizeof got);
    uint32_t glen = n > 0 ? (uint32_t)n : 0u;
    kind = song_per_build(got, &glen, sizeof got, &next, side, false);
    st_side = side;
    st_side_len = SONG_SIDE_LEN;
    rc = st_save3(OBJ_SETTINGS, got, glen, 0, 0, 0, 0);
    st_side = 0;
    st_side_len = 0;
    song_per_saved(rc == 0, got, glen);
    n = st_load(OBJ_SETTINGS, got, sizeof got);
    memcpy(&got_dsu, got + SONG_DSU_OFF, sizeof got_dsu);
    check(kind == SONG_PER3 && rc == 0 && n == (int)SONG_PER_CANON && !dsu_dirty &&
              got_dsu.k[3].s[9].pitch == 66u &&
              memcmp(got + SONG_ARR_OFF, &next, SONG_ARR_LEN) == 0,
          "dsu: flash 88B 旧对象 → 1,552B（当前 dsu + 新 arrangement）");

    /* ---------- 10. settings 字段 RMW + side theme 读取（P4-E Task 1，spec §8.1） ---------- */
    /* `song_per_put_settings` 在 song_per_build/put_dsu **之后**调用（只动自己拥有的
     * 字段/位：palette/lowcut/zoom 直写、lights word 的 5 个 owned 位、尾段 byte38 的
     * bits 2..5）；其余字节（panel/arrangement/DSU/尾段别的字节）逐字节保留。 */
    {
        static uint8_t a_pay[4096], b_pay[4096];
        uint8_t tdef[SONG_MIDI_MAX], tdef0[SONG_MIDI_MAX];
        uint32_t a_len, b_len;
        song_settings_t f, g;
        fill(tdef, sizeof tdef, 0x77); /* 上游 midi_part_fill 默认尾段的替身（byte38 = 0x81：bits 2..5 = 0） */
        memcpy(tdef0, tdef, sizeof tdef);

        /* (a) canonical：无对象 → build → put_dsu → put_settings（reso ≠ 0 ⇒ 追加尾段） */
        f.palette = 2, f.lowcut = 1, f.zoom = 1, f.lights_lvl = 3, f.theme_on = 1, f.rec_tempo = 1,
        f.rec_count = 0, f.rec_quant_off = 1, f.flt_reso = 5;
        len = 0;
        kind = song_per_build(payload, &len, sizeof payload, &arr, side, false);
        song_per_put_dsu(payload, len);
        a_len = song_per_put_settings(payload, len, sizeof payload, &f, tdef);
        check(kind == SONG_PER_NEW && a_len == SONG_PER_CANON + SONG_MIDI_MAX && a_len == 1592u,
              "settings_canonical: 无尾段 + reso=5 → 追加 40B 尾段 = 1,592B");
        check(word(payload + 4) == 2u && word(payload + 8) == 1u && word(payload + 12) == 1u,
              "settings_canonical: palette/lowcut/zoom @4/8/12 = 2/1/1");
        check(word(payload + SONG_LIGHTS_OFF) == 0xC0000203u,
              "settings_canonical: lights @84 = lvl3 | rec_tempo | theme_on | rec_quant_off，其余位 0");
        check((payload[a_len - 2u] & 0x3Cu) == (5u << 2) && payload[a_len - 2u] == ((tdef0[38] & ~0x3Cu) | (5u << 2)),
              "settings_canonical: 尾段 byte38 bits 2..5 = 5，其余位保持");
        check(memcmp(payload + SONG_PER_CANON, tdef0, 38u) == 0 && payload[a_len - 1u] == tdef0[39],
              "settings_canonical: 尾段其余 39 字节 = fixture");
        memcpy(a_pay, payload, a_len);

        /* (b) 外部位保持：已有 word@84 = 全 1、byte38 = 0xFF，只改 owned 位 */
        arr_config_t arr_b = arr_make(2, 0);
        b_len = SONG_PER_CANON + SONG_MIDI_MAX;
        mk_per3(payload, b_len, &arr_b);
        memcpy(payload + SONG_PANEL_OFF, SONG_PANEL_DEF, SONG_PANEL_LEN);
        put_word(payload + 4, 9u);
        put_word(payload + SONG_LIGHTS_OFF, 0xFFFFFFFFu);
        payload[b_len - 2u] = 0xFF;
        memcpy(before, payload, b_len);
        memset(&f, 0, sizeof f);
        f.palette = 4;
        check(song_per_put_settings(payload, b_len, sizeof payload, &f, tdef) == b_len,
              "settings_foreign_bits: 已有尾段 ⇒ 长度不变 1,592B");
        check(word(payload + 4) == 4u && word(payload + 8) == 0u && word(payload + 12) == 0u,
              "settings_foreign_bits: palette/lowcut/zoom 直写");
        check(word(payload + SONG_LIGHTS_OFF) == 0x3FFFF9F0u, /* = 0xFFFFFFFF & ~(0xF | 1<<9 | 1<<10 | 1<<30 | 1<<31) */
              "settings_foreign_bits: word@84 == 0x3FFFF9F0（外部位 4..8/11..29 全保持）");
        check(payload[b_len - 2u] == 0xC3u, "settings_foreign_bits: 尾段 byte38 == 0xC3（bits 2..5 清零、其余保持）");
        check(memcmp(payload + SONG_ARR_OFF, before + SONG_ARR_OFF, SONG_ARR_LEN) == 0,
              "settings_foreign_bits: 48..83 arrangement 逐字节不变");
        {
            int same = 1;
            for (uint32_t i = 0; i < b_len; i++)
                if ((i < 4u || i >= 16u) && (i < SONG_LIGHTS_OFF || i >= SONG_PER_BASE) && i != b_len - 2u &&
                    payload[i] != before[i])
                    same = 0;
            check(same, "settings_foreign_bits: 除 4..15/84..87/尾 byte38 外逐字节不变");
        }
        memcpy(b_pay, payload, b_len);

        /* (c) 无尾段：reso=0 不追加；reso≠0 追加 tail_def；tail_def=NULL 或 cap 不足不追加 */
        memset(&f, 0, sizeof f);
        f.palette = 4;
        len = SONG_PER_CANON;
        mk_per3(payload, len, &arr);
        memcpy(before, payload, len);
        memset(payload + len, 0xEE, SONG_MIDI_MAX);
        check(song_per_put_settings(payload, len, sizeof payload, &f, tdef) == SONG_PER_CANON &&
                  memcmp(payload + 16u, before + 16u, SONG_LIGHTS_OFF - 16u) == 0 &&
                  memcmp(payload + SONG_PER_BASE, before + SONG_PER_BASE, len - SONG_PER_BASE) == 0 &&
                  word(payload + SONG_LIGHTS_OFF) == (word(before + SONG_LIGHTS_OFF) & ~0xC000060Fu) &&
                  payload[len] == 0xEEu,
              "settings_reso_appends_tail: reso=0 ⇒ 长度仍 1,552B、不追加（lights 只清 owned 位）");
        f.flt_reso = 7;
        check(song_per_put_settings(payload, len, sizeof payload, &f, tdef) == SONG_PER_CANON + SONG_MIDI_MAX,
              "settings_reso_appends_tail: reso=7 + tail_def ⇒ 1,592B");
        check(memcmp(payload + SONG_PER_CANON, tdef0, 38u) == 0 && payload[len + 39u] == tdef0[39] &&
                  (payload[len + 38u] & 0x3Cu) == (7u << 2) && payload[len + 38u] == ((tdef0[38] & ~0x3Cu) | (7u << 2)),
              "settings_reso_appends_tail: 尾段 = fixture，byte38 bits 2..5 = 7");
        mk_per3(payload, len, &arr);
        memset(payload + len, 0xEE, SONG_MIDI_MAX);
        check(song_per_put_settings(payload, len, sizeof payload, &f, 0) == SONG_PER_CANON &&
                  payload[len] == 0xEEu,
              "settings_reso_appends_tail: tail_def=NULL ⇒ 不追加、不报错");
        check(song_per_put_settings(payload, len, SONG_PER_CANON + SONG_MIDI_MAX - 1u, &f, tdef) == SONG_PER_CANON &&
                  payload[len] == 0xEEu,
              "settings_reso_appends_tail: cap 不足 ⇒ 不追加");

        /* (d) 往返：(a)(b)(c) 各读回，字段逐项相等 */
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(a_pay, a_len, &g) && g.palette == 2u && g.lowcut == 1u && g.zoom == 1u &&
                  g.lights_lvl == 3u && g.theme_on == 1u && g.rec_tempo == 1u && g.rec_count == 0u &&
                  g.rec_quant_off == 1u && g.flt_reso == 5u,
              "settings_roundtrip: (a) 9 个字段逐项读回");
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(b_pay, b_len, &g) && g.palette == 4u && g.lowcut == 0u && g.zoom == 0u &&
                  g.lights_lvl == 0u && g.theme_on == 0u && g.rec_tempo == 0u && g.rec_count == 0u &&
                  g.rec_quant_off == 0u && g.flt_reso == 0u,
              "settings_roundtrip: (b) owned 位已写、外部位不入投影");
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(payload + 0, SONG_PER_CANON, &g) && g.palette == 4u && g.flt_reso == 0u,
              "settings_roundtrip: (c) 无尾段 ⇒ reso 读回 0");
        memcpy(payload, a_pay, a_len);
        payload[a_len - 2u] = 0xFF; /* bits 2..5 = 15：上游读回钳到 10 */
        memset(&g, 0, sizeof g);
        check(song_per_get_settings(payload, a_len, &g) && g.flt_reso == 10u, "settings_roundtrip: reso 读回 >10 钳到 10");
        /* 历史格式/长度：PER1/PER2 只读 palette；<88B 没有 lights；32/36B 尾段没有 byte38 */
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(payload, SONG_PER_V22, &g) && g.lights_lvl == 0u && g.rec_tempo == 0u &&
                  g.theme_on == 0u && g.rec_quant_off == 0u && g.flt_reso == 0u,
              "settings_roundtrip: 84B（2.2）⇒ lights word 读 0");
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(a_pay, SONG_PER_CANON + SONG_MIDI_32, &g) && g.flt_reso == 0u &&
                  g.lights_lvl == 3u,
              "settings_roundtrip: 32B 尾段（1,584B）没有 byte38 ⇒ reso 0");
        memset(payload, 0, SONG_PER2_LEN);
        put_word(payload, SONG_PER2_MAGIC);
        put_word(payload + 4, 3u);
        put_word(payload + 8, 1u);
        put_word(payload + 12, 1u);
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(payload, SONG_PER2_LEN, &g) && g.palette == 3u && g.lowcut == 0u && g.zoom == 0u &&
                  g.lights_lvl == 0u && g.flt_reso == 0u,
              "settings_roundtrip: PER2 只读 palette（其余 0）");
        memset(payload, 0, SONG_PER1_LEN);
        put_word(payload, SONG_PER1_MAGIC);
        put_word(payload + 4, 5u);
        memset(&g, 0xFF, sizeof g);
        check(song_per_get_settings(payload, SONG_PER1_LEN, &g) && g.palette == 5u && g.lights_lvl == 0u,
              "settings_roundtrip: PER1 只读 palette");
        mk_per3(payload, SONG_PER_BASE, &arr);
        len = 200; /* 损坏长度 */
        memset(&g, 0x5A, sizeof g);
        {
            song_settings_t g0 = g;
            check(!song_per_get_settings(payload, len, &g) && memcmp(&g, &g0, sizeof g) == 0,
                  "settings_roundtrip: 损坏对象 → false 且 *out 不动");
            memset(&g, 0x5A, sizeof g);
            g0 = g;
            check(!song_per_get_settings(a_pay, 0, &g) && memcmp(&g, &g0, sizeof g) == 0,
                  "settings_roundtrip: 无对象（len 0）→ false 且 *out 不动");
        }

        /* (e) side theme：THM2 + CRC36；THM1/坏 CRC/全 0xFF ⇒ false */
        static uint8_t srec[SONG_SIDE_LEN];
        static const uint16_t c5[5] = {0xF800, 0x07E0, 0x001F, 0x8410, 0x4208};
        static const uint16_t trk4[4] = {0xFFFF, 0x0000, 0x8410, 0x4208};
        song_theme_view_t tv;
        song_side_default(srec); /* FAVS 部分有效、theme 全 0 */
        put_word(srec, 0x324D4854u);
        memcpy(srec + 4, "MYTHEME\0", 8);
        for (uint32_t i = 0; i < 5; i++) put_word(srec + 12u + 2u * i, c5[i]);
        for (uint32_t i = 0; i < 4; i++) put_word(srec + 22u + 2u * i, trk4[i]);
        put_word(srec + 30u, 0x0000u);
        put_word(srec + 32u, 0xFFFFu);
        put_word(srec + 36u, st_crc32(srec, 36u));
        memset(&tv, 0xEE, sizeof tv);
        check(song_per_side_theme(srec, &tv) && strcmp(tv.name, "MYTHEME") == 0,
              "side_theme_read: THM2 + CRC36 → true、name NUL 结尾");
        check(tv.c[0][0] == 255 && tv.c[0][1] == 0 && tv.c[0][2] == 0 && tv.c[1][0] == 0 &&
                  tv.c[1][1] == 255 && tv.c[1][2] == 0 && tv.c[2][0] == 0 && tv.c[2][1] == 0 && tv.c[2][2] == 255,
              "side_theme_read: RGB565 → RGB888 纯红/绿/蓝");
        check(tv.c[3][0] == 131 && tv.c[3][1] == 129 && tv.c[3][2] == 131 && tv.c[4][0] == 65 &&
                  tv.c[4][1] == 64 && tv.c[4][2] == 65,
              "side_theme_read: RGB565 → RGB888 中间值 *255/31、*255/63");
        check(tv.trk[0][0] == 255 && tv.trk[0][1] == 255 && tv.trk[0][2] == 255 && tv.trk[1][0] == 0 &&
                  tv.trk[2][2] == 131 && tv.trk[3][1] == 64,
              "side_theme_read: trk[4] 展开");
        check(tv.bg[0] == 0 && tv.bg[1] == 0 && tv.bg[2] == 0 && tv.hi[0] == 255 && tv.hi[1] == 255 &&
                  tv.hi[2] == 255,
              "side_theme_read: bg/hi 展开");
        memcpy(srec + 4, "ABCDEFGH", 8); /* 满 8 字符：上游不结尾 */
        put_word(srec + 36u, st_crc32(srec, 36u));
        memset(&tv, 0xEE, sizeof tv);
        check(song_per_side_theme(srec, &tv) && strcmp(tv.name, "ABCDEFGH") == 0 && tv.name[8] == 0,
              "side_theme_read: 8 字符名 → char[9] 补 NUL");
        put_word(srec, 0x314D4854u); /* "THM1" */
        memset(&tv, 0xEE, sizeof tv);
        {
            song_theme_view_t tv0 = tv;
            check(!song_per_side_theme(srec, &tv) && memcmp(&tv, &tv0, sizeof tv) == 0,
                  "side_theme_read: THM1 → false 且 *out 不动");
        }
        put_word(srec, 0x324D4854u);
        srec[20] ^= 1u; /* 改 1 字节不更新 CRC */
        check(!song_per_side_theme(srec, &tv), "side_theme_read: 坏 CRC → false");
        srec[20] ^= 1u;
        check(song_per_side_theme(srec, &tv), "side_theme_read: 改回后 CRC 再通过（对照）");
        memset(srec, 0xFF, sizeof srec);
        check(!song_per_side_theme(srec, &tv), "side_theme_read: 全 0xFF（从未写）→ false");

        /* (f) (b) 的 payload 经真实 st_save3（含 side theme）往返：get 一致、panel 不变 */
        flash_reset();
        song_side_default(srec);
        put_word(srec, 0x324D4854u);
        memcpy(srec + 4, "MYTHEME\0", 8);
        for (uint32_t i = 0; i < 5; i++) put_word(srec + 12u + 2u * i, c5[i]);
        for (uint32_t i = 0; i < 4; i++) put_word(srec + 22u + 2u * i, trk4[i]);
        put_word(srec + 30u, 0x0000u);
        put_word(srec + 32u, 0xFFFFu);
        put_word(srec + 36u, st_crc32(srec, 36u));
        memcpy(payload, b_pay, b_len);
        memset(&g, 0, sizeof g);
        check(song_per_get_settings(payload, b_len, &g) && memcmp(payload + SONG_PANEL_OFF, SONG_PANEL_DEF, SONG_PANEL_LEN) == 0,
              "settings_real_st_save3: 写前 get 与 panel = SONG_PANEL_DEF");
        {
            song_settings_t g0 = g;
            st_side = srec;
            st_side_len = SONG_SIDE_LEN;
            rc = st_save3(OBJ_SETTINGS, payload, b_len, 0, 0, 0, 0);
            st_side = 0;
            st_side_len = 0;
            n = st_load(OBJ_SETTINGS, got, sizeof got);
            memset(&g, 0xFF, sizeof g);
            check(rc == 0 && n == (int)b_len && song_per_get_settings(got, (uint32_t)n, &g) &&
                      memcmp(&g, &g0, sizeof g) == 0,
                  "settings_real_st_save3: 往返后 get_settings 与写前逐项一致");
            check(memcmp(got + SONG_PANEL_OFF, SONG_PANEL_DEF, SONG_PANEL_LEN) == 0,
                  "settings_real_st_save3: SONG_PANEL_DEF 16..47 逐字节不变");
            check(st_side_load(OBJ_SETTINGS, side_got, SONG_SIDE_LEN) == 0 &&
                      song_per_side_theme(side_got, &tv) && strcmp(tv.name, "MYTHEME") == 0 &&
                      tv.c[3][0] == 131 && tv.trk[3][1] == 64,
                  "settings_real_st_save3: side theme 随副本提交并可读回");
        }
    }

    printf("persist-host: %s (%d failure%s)\n", fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    return fails;
}
