/* SPDX-License-Identifier: GPL-3.0-only */
/* song_persist.h — OBJ_SETTINGS 的 PER3 歌曲子集（P4-C Task 7，ADR-009 §6 / ADR-010 §4）
 *
 * 上游把歌曲顺序（arr_config_t，36B）放进设置对象 persist_t 的偏移 48：
 *
 *   persist_t（88B，OBJ_SETTINGS 的 payload 开头）
 *     0   magic "PER3"（uint32）
 *     4   palette   8 lowcut   12 zoom
 *     16  panel_t（32B，PAN5 + 14 个按钮 + 7 个编码器 + 7 个方向）
 *     48  arr_config_t（36B：count/loop/reserved + 16×(scene,bars)）
 *     84  lights（uint32）
 *     88  dsu_bank_t（1,464B，默认 SYN kit 银行）      ⇒ canonical = 1,552B
 *     1552.. 可选的 MIDI LEARN 尾段（完整 40B；2.5/2.6 测试写过 32/36，也合法）
 *                 尾段 byte38（= payload 的倒数第二字节）bits 2..5 = flt_reso（2.7.2）
 *
 * 本模块读写的段：48..83（歌曲顺序，整段替换）、88..1,551（DSU，整段替换）、palette/lowcut/
 * zoom（直写）、lights 的 5 个 owned 位与尾段 byte38 的 bits 2..5（RMW，见文件尾「settings
 * 字段」节）；panel 16..47 与其余字节从不触碰。
 *
 * 侧记录（side record，80B = theme 40B + "FAV1" + 16×收藏 + CRC）不在 payload 里：
 * storage.c 的 st_save3 把它写在副本扇区偏移 64..143，随 A/B 副本一起提交。侧区全 0xFF
 * （从未写过）时写默认记录；否则**原样保留**（含 2.7 beta 的「只有主题」记录）。
 *
 * 本文件只做**字节级**变换（不碰 flash、不取锁）：调用方负责 st_load/st_save3。
 * 必须在 storage.c（st_crc32）与 drums.c → drum_synth.c（dsu/dsu_defaults）之后
 * include；scripts/persist-host-test.c 用同一顺序编译，两侧因此看到同一份默认 DSU
 * 与同一个 CRC 实现。
 *
 * 写规则（ADR-009 §6，逐条对应上游 persist_boot/settings_write/dsu_boot）：
 *   - 没有对象：canonical PER3（默认 panel、lights=0、当前歌曲）+ 当前 RAM 的 DSU + 默认 side；
 *   - 合法 PER3（84/88/1,552/1,584/1,588/1,592）：整段保留，只替换 48..83；88..1,551 一律
 *     写成当前 RAM 的 DSU（84/88 因此升级到 1,552B，P4-D Task 5）；已有尾段绝不重写；
 *   - PER2（48B）/PER1（40B）：按 persist_boot 口径迁移字段，别的取默认，不用空白覆盖；
 *   - 未知 magic 或长度不是任何历史格式：SONG_PER_BAD——调用方**不得写 flash**。
 *
 * 本模块不持久化 `arrangement_enabled`（歌曲模式）与 quick chain/SONG REC 临时态：
 * 上游同样不存（ADR-010 §4）。 */
#ifndef SONG_PERSIST_H
#define SONG_PERSIST_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sloop_store.h" /* song_settings_t / song_theme_view_t（公共头，Task 2 起暴露给 store_tab5.c 的调用方） */

/* 布局常量：与上游 project.c 的 persist_t/arr_config_t 与 ui.c 的 side_rec_t 逐字节相同 */
#define SONG_PER_MAGIC  0x50455233u /* "PER3"：含歌曲顺序 */
#define SONG_PER2_MAGIC 0x50455232u /* "PER2"：palette + panel */
#define SONG_PER1_MAGIC 0x50455231u /* "PER1"：palette + panel（2.0） */
#define SONG_PER_V22    84u         /* 2.2 的 persist_t：没有 lights */
#define SONG_PER_BASE   88u         /* sizeof(persist_t) */
#define SONG_PER2_LEN   48u         /* 16 + sizeof(panel_t) */
#define SONG_PER1_LEN   40u         /* 8 + sizeof(panel_t) */
#define SONG_PANEL_OFF  16u
#define SONG_PANEL_LEN  32u
#define SONG_PANEL_MAGIC 0x50414E35u /* "PAN5"（panel.c:16；上游升级时同步这里与下面的字面量） */
#define SONG_ARR_OFF    48u         /* 上游静态断言：web editor 的 Song 页编辑 48..83 */
#define SONG_ARR_LEN    36u         /* sizeof(arr_config_t) */
#define SONG_LIGHTS_OFF 84u
#define SONG_DSU_OFF    SONG_PER_BASE
#define SONG_DSU_LEN    1464u /* sizeof(dsu_bank_t) */
#define SONG_PER_CANON  (SONG_PER_BASE + SONG_DSU_LEN) /* 1,552B：上游首次 settings_save 的长度 */
#define SONG_MIDI_32    32u   /* 2.6 测试写过的尾段长度（只有 LEARN 表） */
#define SONG_MIDI_36    36u   /* 2.6 测试写过的尾段长度（LEARN + 通道） */
#define SONG_MIDI_MAX   40u   /* 2*ML_N + 8（2.7 的完整 MIDI 部分） */
#define SONG_SIDE_LEN   80u   /* sizeof(side_rec_t) */
#define SONG_FAVS_OFF   40u   /* theme_t 之后 */
#define SONG_FAVS_MAGIC 0x31564146u /* "FAV1"（ui.c:892） */
#define SONG_FAVS_N     16u
#define SONG_FAV_NONE   0xFFFFu
#define SONG_FAVS_CRC   76u /* fav 之后 */

/* 上游 persist_t 的布局镜像：只用于尺寸/偏移的编译期断言，不参与读写 */
typedef struct {
    uint32_t magic, palette, lowcut, zoom;
    uint8_t panel[SONG_PANEL_LEN];
    uint8_t arrangement[SONG_ARR_LEN];
    uint32_t lights;
} song_persist_t;

_Static_assert(sizeof(song_persist_t) == SONG_PER_BASE, "persist_t 布局漂移");
_Static_assert(sizeof(arr_config_t) == SONG_ARR_LEN, "arr_config_t 布局漂移");
_Static_assert(sizeof(dsu_bank_t) == SONG_DSU_LEN, "dsu_bank_t 布局漂移");
_Static_assert(__builtin_offsetof(song_persist_t, arrangement) == SONG_ARR_OFF,
               "arrangement 必须在上游的偏移 48（web editor 的 Song 页）");

/* 上游 panel.c 的 PANEL_DEFAULT 字面量（那个 .c 不在本 TU：与 store_tab5.c 的 TRK_DEF
 * 同法固定在此，规格变更时编译期/测试会发现）。 */
static const uint8_t SONG_PANEL_DEF[SONG_PANEL_LEN] = {
    0x35, 0x4E, 0x41, 0x50, /* "PAN5"（小端） */
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1, /* btn[NB]：PLAY = 12、REC = 13 */
    0, 1, 6, 2, 3, 4, 5,                            /* enc[NE] */
    1, 1, 1, 1, 1, 1, 1                             /* dir[NE] */
};

typedef enum {
    SONG_PER_NEW = 0, /* 没有对象：本次组装出 canonical */
    SONG_PER3,        /* 合法 PER3（含历史长度/尾段） */
    SONG_PER2,        /* 合法 PER2（迁移） */
    SONG_PER1,        /* 合法 PER1（迁移） */
    SONG_PER_BAD,     /* 未知 magic / 长度不是任何历史格式：拒绝，不覆盖 */
} song_per_kind_t;

static uint32_t song_per_word(const uint8_t *p)
{
    uint32_t w;
    memcpy(&w, p, sizeof w);
    return w;
}

static void song_per_put_word(uint8_t *p, uint32_t w)
{
    memcpy(p, &w, sizeof w);
}

/* 上游 arr_valid（arranger.h）去掉 ready 位：这里只判结构（set 侧与读回共用）。
 * 引用的 A–D 是否真的存有工程由播放/歌曲页用 arrangement_ready() 判（Task 8/12）。 */
static int song_arr_valid(const arr_config_t *c)
{
    uint32_t i;
    if (c == 0 || !c->count || c->count > ARR_STEPS || c->loop > 1u) return 0;
    for (i = 0; i < c->count; i++)
        if (c->entry[i].scene >= ARR_SCENES || !c->entry[i].bars || c->entry[i].bars > 64u) return 0;
    return 1;
}

/* 合法 PER3 的长度：上游 persist_boot 读 84/88，dsu_boot 读 88+1,464+{0,32,36,40} */
static int song_per3_len(uint32_t len)
{
    return len == SONG_PER_V22 || len == SONG_PER_BASE || len == SONG_PER_CANON ||
           len == SONG_PER_CANON + SONG_MIDI_32 || len == SONG_PER_CANON + SONG_MIDI_36 ||
           len == SONG_PER_CANON + SONG_MIDI_MAX;
}

/* 分类（不写 *arr）：payload/len 是**当前 flash 对象**的字节；len = 0 表示没有对象 */
static song_per_kind_t song_per_kind(const uint8_t *payload, uint32_t len)
{
    if (payload == 0 || len == 0) return SONG_PER_NEW;
    if (song_per_word(payload) == SONG_PER_MAGIC) return song_per3_len(len) ? SONG_PER3 : SONG_PER_BAD;
    if (song_per_word(payload) == SONG_PER2_MAGIC) return len == SONG_PER2_LEN ? SONG_PER2 : SONG_PER_BAD;
    if (song_per_word(payload) == SONG_PER1_MAGIC) return len == SONG_PER1_LEN ? SONG_PER1 : SONG_PER_BAD;
    return SONG_PER_BAD;
}

/* 上游 persist_boot 的歌曲段：**先** arr_defaults，只有合法的 PER3 才覆盖（读回值）。这里把
 * 「读回结果」映射成生效歌曲：NEW/PER1/PER2 与对象损坏（SONG_PER_BAD）都落上游默认
 * （count=4，entry 0..3 = A/B/C/D 各 4 小节），PER3 取 loaded（结构非法时 song_per_read
 * 已落默认）。返回 false = 对象损坏：调用方**不得**把它当空设置覆盖（保存仍拒绝）。
 *
 * 为什么要有这一步：`arrangement` 是 arranger.c 的 static `.bss`，上电/自检 restore 后是
 * 全零（count=0 的非法歌曲）——「读不到就保持默认」必须真的把默认写进去，否则日志说
 * 「保持默认歌曲」而实际是空歌曲，歌曲模式的 ready 判定与 SONG 页会看到假状态。 */
static bool song_per_loaded(song_per_kind_t kind, const arr_config_t *loaded, arr_config_t *out)
{
    arr_defaults(out);
    if (kind == SONG_PER_BAD) return false;
    if (kind == SONG_PER3 && loaded != 0) *out = *loaded;
    return true;
}

/* 读回歌曲：PER3 取 48..83（结构非法 → 默认歌曲，上游 persist_boot 同口径）；
 * PER1/PER2 没有歌曲 → 默认。SONG_PER_BAD 时 *arr 不被写。 */
static song_per_kind_t song_per_read(const uint8_t *payload, uint32_t len, arr_config_t *arr)
{
    song_per_kind_t kind = song_per_kind(payload, len);
    if (kind == SONG_PER_BAD) return kind;
    if (kind == SONG_PER3) {
        memcpy(arr, payload + SONG_ARR_OFF, SONG_ARR_LEN);
        if (!song_arr_valid(arr)) arr_defaults(arr);
        return kind;
    }
    arr_defaults(arr);
    return kind;
}

/* 默认 side record（上游 ui.c side_fill：theme 全 0 = 没有主题、空收藏、自校验 CRC） */
static void song_side_default(uint8_t side[SONG_SIDE_LEN])
{
    uint32_t i;
    memset(side, 0, SONG_SIDE_LEN);
    song_per_put_word(side + SONG_FAVS_OFF, SONG_FAVS_MAGIC);
    for (i = 0; i < SONG_FAVS_N; i++) {
        song_per_put_word(side + SONG_FAVS_OFF + 4u + 2u * i, SONG_FAV_NONE);
    }
    song_per_put_word(side + SONG_FAVS_CRC, st_crc32(side + SONG_FAVS_OFF, 4u + 2u * SONG_FAVS_N));
}

/* 现有 side 是否是一份有效记录（magic + CRC）。诊断面（`STORE SONG INFO`）用它报告。 */
static bool song_side_valid(const uint8_t *side)
{
    return song_per_word(side + SONG_FAVS_OFF) == SONG_FAVS_MAGIC &&
           song_per_word(side + SONG_FAVS_CRC) ==
               st_crc32(side + SONG_FAVS_OFF, 4u + 2u * SONG_FAVS_N);
}

/* 侧区是不是从未写过（全 0xFF，P4-B 及更早的保存都不碰它）。是 ⇒ 本次保存写默认记录；
 * 否 ⇒ **原样保留**，即使自校验不过（2.7 beta 只写了 40B 主题、没有 FAV1/CRC：本固件与
 * P4-E 都没有主题状态，用默认值覆盖等于删掉用户的主题——「side 原样随副本提交」优先）。 */
static bool song_side_blank(const uint8_t *side)
{
    for (uint32_t i = 0; i < SONG_SIDE_LEN; i++)
        if (side[i] != 0xFFu) return false;
    return true;
}

/* 把**当前 RAM 的 DSU**（drum_synth.c 的 `dsu`，1,464B）写进 payload 的 88..1,551
 * （P4-D Task 5，spec §3.4）：**任何** settings 保存都携带它——显式 `STORE SONG SAVE`、
 * sections/歌曲延迟写、`DSYN STORE`（三者在 store_tab5.c 里走同一个 `song_write_locked`）。
 * 上游 settings_write 同法：`if (!dsu_valid(&dsu)) dsu_defaults();` 再写 &dsu——坏掉的
 * RAM 银行不落盘。`len < SONG_PER_CANON` 时什么都不做（canonical 是任何保存的下限，
 * 调用方在最终长度确定后调用）。 */
static void song_per_put_dsu(uint8_t *payload, uint32_t len)
{
    if (payload == 0 || len < SONG_PER_CANON) return;
    if (!dsu_valid(&dsu)) dsu_defaults();
    memcpy(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN);
}

/* flash 提交返回后调用（与 song_pending_done 同一位置，store_tab5.c 的 song_write_locked）：
 * 只有「写成功」且「刚提交的字节里就是当前 RAM 的 DSU」才清 dsu_dirty。擦写窗口里又来的
 * DSYN 编辑让 payload 过时——此刻 RAM 与盘上不一致，dirty 必须保留（下一次显式 STORE 才
 * 落盘）。`dirty == 0` 因此精确表示「RAM 与最近一次成功写盘一致」（spec §3.4）。 */
static void song_per_saved(bool ok, const uint8_t *payload, uint32_t len)
{
    if (!ok || payload == 0 || len < SONG_PER_CANON) return;
    if (memcmp(payload + SONG_DSU_OFF, &dsu, SONG_DSU_LEN) == 0) dsu_dirty = 0;
}

/* 组装一次保存（就地改 payload；*len 入 = 现有长度（0 = 无对象）、出 = 新长度）。
 * cap = payload 缓冲上限（store_tab5.c 传暂存大小）。side 入 = 现有 side 字节 + 侧区是否
 * 有内容（`!song_side_blank`），出 = 随新副本提交的 80B。返回组装前对象的种类（日志/诊断用）；SONG_PER_BAD 表示拒绝，
 * 此时调用方不得写 flash（payload/side 可能已部分改动，但没有被提交）。 */
static song_per_kind_t song_per_build(uint8_t *payload, uint32_t *len, uint32_t cap,
                                      const arr_config_t *arr, uint8_t side[SONG_SIDE_LEN], bool side_present)
{
    uint32_t n = *len;
    song_per_kind_t kind = song_per_kind(payload, n);

    if (payload == 0 || len == 0 || arr == 0 || side == 0) return SONG_PER_BAD;
    if (!song_arr_valid(arr)) return SONG_PER_BAD;
    if (kind == SONG_PER_BAD) return SONG_PER_BAD;
    if (cap < SONG_PER_CANON) return SONG_PER_BAD; /* 任何一条路径最长就是 canonical */

    switch (kind) {
    case SONG_PER_NEW:
        memset(payload, 0, SONG_PER_CANON);
        song_per_put_word(payload, SONG_PER_MAGIC);
        memcpy(payload + SONG_PANEL_OFF, SONG_PANEL_DEF, SONG_PANEL_LEN);
        memcpy(payload + SONG_ARR_OFF, arr, SONG_ARR_LEN);
        n = SONG_PER_CANON;
        break;
    case SONG_PER3:
        memcpy(payload + SONG_ARR_OFF, arr, SONG_ARR_LEN);
        if (n == SONG_PER_V22) { /* 2.2：补 lights（上游 persist_boot 把 2.2 的当 lights=0） */
            song_per_put_word(payload + SONG_LIGHTS_OFF, 0);
            n = SONG_PER_BASE;
        }
        if (n == SONG_PER_BASE) n = SONG_PER_CANON; /* 没有 DSU：下面统一补当前 RAM 的 */
        break;
    case SONG_PER2: {
        uint8_t keep[SONG_PER2_LEN];
        memcpy(keep, payload, sizeof keep);
        memset(payload, 0, SONG_PER_CANON);
        song_per_put_word(payload, SONG_PER_MAGIC);
        memcpy(payload + 4, keep + 4, 12); /* palette/lowcut/zoom */
        if (song_per_word(keep + SONG_PANEL_OFF) == SONG_PANEL_MAGIC)
            memcpy(payload + SONG_PANEL_OFF, keep + SONG_PANEL_OFF, SONG_PANEL_LEN);
        else
            memcpy(payload + SONG_PANEL_OFF, SONG_PANEL_DEF, SONG_PANEL_LEN); /* panel_init 会重置 */
        memcpy(payload + SONG_ARR_OFF, arr, SONG_ARR_LEN);
        n = SONG_PER_CANON;
        break;
    }
    case SONG_PER1: {
        uint8_t keep[SONG_PER1_LEN];
        memcpy(keep, payload, sizeof keep);
        memset(payload, 0, SONG_PER_CANON);
        song_per_put_word(payload, SONG_PER_MAGIC);
        memcpy(payload + 4, keep + 4, 4); /* palette（PER1 没有 lowcut/zoom：0） */
        if (song_per_word(keep + 8) == SONG_PANEL_MAGIC)
            memcpy(payload + SONG_PANEL_OFF, keep + 8, SONG_PANEL_LEN);
        else
            memcpy(payload + SONG_PANEL_OFF, SONG_PANEL_DEF, SONG_PANEL_LEN);
        memcpy(payload + SONG_ARR_OFF, arr, SONG_ARR_LEN);
        n = SONG_PER_CANON;
        break;
    }
    case SONG_PER_BAD:
    default:
        return SONG_PER_BAD;
    }
    /* 88..1,551 统一写当前 RAM 的 DSU（P4-D Task 5）：canonical/84/88B 的「补 DSU」与
     * 已有 DSU 的 PER3「替换 DSU」是同一条规则——写盘后 settings 里的 SYN kit 就是此刻
     * 正在发声的那一份。已有 MIDI 尾段（1,552 之后）不在这段里，逐字节保留。 */
    song_per_put_dsu(payload, n);
    if (!side_present) song_side_default(side); /* 否则原样提交（见 song_side_blank） */
    *len = n;
    return kind;
}

/* ---- dirty 记账 ---------------------------------------------------------------- */
/* 上游 sections_flush 在写之前就清 song_dirty（写失败被静默丢掉）；这里改成「写成功且
 * 期间没有新的修改」才清：失败保留，下个窗口按停止/安静/idle 门限重试（ADR-010 §4
 * 的可靠性修正，不改变成功路径的字节）。gen 让 flush 期间的并发修改不会被清掉。 */
typedef struct {
    uint8_t dirty;
    uint32_t gen;
} song_pending_t;

static void song_pending_mark(song_pending_t *p)
{
    p->dirty = 1;
    p->gen++;
}

static uint32_t song_pending_gen(const song_pending_t *p)
{
    return p->gen;
}

static void song_pending_done(song_pending_t *p, bool ok, uint32_t gen)
{
    if (ok && p->gen == gen) p->dirty = 0;
}

/* ---- settings 字段（P4-E Task 1，spec §8.1）-------------------------------------- */
/* 上游 persist_boot/settings_write 散在 project.c/panel.c 里的那几处读写的字节镜像。
 * 写侧只动**自己拥有**的字段/位：lights word 的其余位（keys/notes/usb/sync/vis/cpu1/
 * knob_accel）本固件没有对应语义，按上游默认留在盘上，不因保存而被清掉。 */
#define SONG_PAL_OFF    4u
#define SONG_LOWCUT_OFF 8u
#define SONG_ZOOM_OFF   12u
/* lights word（panel.c lights_word）：lights_lvl 0..3 / rec_tempo 9 / rec_count 10 /
 * theme_on 30 / rec_quant_off 31 */
#define SONG_SET_LIGHTS_MASK (0xFu | 1u << 9 | 1u << 10 | 1u << 30 | 1u << 31)
#define SONG_MIDI_RESO_OFF (SONG_MIDI_MAX - 2u) /* 尾段 byte38 = payload 倒数第二字节 */
#define SONG_RESO_MASK  0x3Cu /* bits 2..5 */
#define SONG_RESO_MAX   10u
#define SONG_THEME_MAGIC 0x324D4854u /* "THM2"（gfx.c；"THM1" 是 36B 的 beta 格式，不认） */
#define SONG_THEME_NAME  4u
#define SONG_THEME_C     12u  /* c[5] RGB565 */
#define SONG_THEME_TRK   22u  /* trk[4] RGB565 */
#define SONG_THEME_BG    30u
#define SONG_THEME_HI    32u
#define SONG_THEME_CRC   36u  /* 覆盖 0..35 */

static uint8_t song_reso_byte(uint8_t b, uint32_t reso)
{
    if (reso > SONG_RESO_MAX) reso = SONG_RESO_MAX;
    return (uint8_t)((b & (uint8_t)~SONG_RESO_MASK) | (reso << 2));
}

/* 在 song_per_build/put_dsu **之后**调用（payload 已是合法 PER3，len ≥ 88）。就地写
 * palette/lowcut/zoom 与 lights 的 owned 位；flt_reso 写进尾段 byte38：
 *   - 已有 40B 尾段（len ≥ 1,592）⇒ RMW（byte 在 payload 的倒数第二字节，不假定 1,592）；
 *   - 尾段不足 40B（没有 / 2.6 的 32B、36B）且 reso ≠ 0 ⇒ 就地扩到 40B：已有的尾段字节
 *     保留，缺的后缀取 tail_def（上游 midi_part_fill 的 40B 默认尾段，设备侧由调用方生成）
 *     再打点——与上游 settings_write「非默认才写尾段」同语义；tail_def == NULL 或 cap
 *     不够 ⇒ 不扩，reso 丢弃（返回 len 不变）；reso = 0 ⇒ 不扩。
 * 返回新 len。len < 88（不是 PER3）⇒ 什么都不写。 */
static uint32_t song_per_put_settings(uint8_t *payload, uint32_t len, uint32_t cap,
                                      const song_settings_t *f, const uint8_t *tail_def)
{
    uint32_t w, tail;
    if (payload == 0 || f == 0 || len < SONG_PER_BASE) return len;
    song_per_put_word(payload + SONG_PAL_OFF, f->palette);
    song_per_put_word(payload + SONG_LOWCUT_OFF, f->lowcut);
    song_per_put_word(payload + SONG_ZOOM_OFF, f->zoom);
    w = (song_per_word(payload + SONG_LIGHTS_OFF) & ~SONG_SET_LIGHTS_MASK) |
        ((f->lights_lvl & 0xFu) | (f->rec_tempo & 1u) << 9 | (f->rec_count & 1u) << 10 |
         (f->theme_on & 1u) << 30 | (f->rec_quant_off & 1u) << 31);
    song_per_put_word(payload + SONG_LIGHTS_OFF, w);
    if (len < SONG_PER_CANON) return len; /* 没有 DSU（song_per_build 之后不会到这）：尾段无处可放 */
    tail = len - SONG_PER_CANON;
    if (tail < SONG_MIDI_MAX) {
        if (f->flt_reso == 0u || tail_def == 0 || cap < SONG_PER_CANON + SONG_MIDI_MAX) return len;
        memcpy(payload + len, tail_def + tail, SONG_MIDI_MAX - tail); /* 已有短尾段原样，只补后缀 */
        len = SONG_PER_CANON + SONG_MIDI_MAX;
    }
    payload[len - SONG_MIDI_MAX + SONG_MIDI_RESO_OFF] =
        song_reso_byte(payload[len - SONG_MIDI_MAX + SONG_MIDI_RESO_OFF], f->flt_reso);
    return len;
}

/* 读回 settings 字段（上游 persist_boot 口径）。false = 没有对象或对象损坏（未知 magic/
 * 长度），*out 不动。PER1（40B）只有 palette；PER2（48B）有 palette/lowcut/zoom；两者都没有
 * lights/尾段 ⇒ 0。PER3 不足 88B（2.2）⇒ lights 读 0；只有 40B 尾段才有 flt_reso
 * （bits 2..5，>10 钳到 10），32/36B 尾段与无尾段 ⇒ 0。 */
static bool song_per_get_settings(const uint8_t *payload, uint32_t len, song_settings_t *out)
{
    song_per_kind_t kind;
    uint32_t w;
    if (payload == 0 || out == 0) return false;
    kind = song_per_kind(payload, len);
    if (kind == SONG_PER_NEW || kind == SONG_PER_BAD) return false;
    out->palette = song_per_word(payload + SONG_PAL_OFF);
    out->lowcut = 0;
    out->zoom = 0;
    out->lights_lvl = 0;
    out->rec_tempo = 0;
    out->rec_count = 0;
    out->theme_on = 0;
    out->rec_quant_off = 0;
    out->flt_reso = 0;
    if (kind == SONG_PER1) return true;
    out->lowcut = song_per_word(payload + SONG_LOWCUT_OFF);
    out->zoom = song_per_word(payload + SONG_ZOOM_OFF);
    if (len < SONG_PER_BASE) return true; /* 2.2 的 84B：没有 lights（上游当 0） */
    w = song_per_word(payload + SONG_LIGHTS_OFF);
    out->lights_lvl = (w & 15u) < 4u ? (w & 15u) : 0u; /* 上游 lights_from_word：≥ LIGHTS_N(4) ⇒ OFF */
    out->rec_tempo = (w >> 9) & 1u;
    out->rec_count = (w >> 10) & 1u;
    out->theme_on = (w >> 30) & 1u;
    out->rec_quant_off = (w >> 31) & 1u;
    if (len >= SONG_PER_CANON + SONG_MIDI_MAX) {
        out->flt_reso = (payload[len - SONG_MIDI_MAX + SONG_MIDI_RESO_OFF] >> 2) & 0xFu;
        if (out->flt_reso > SONG_RESO_MAX) out->flt_reso = SONG_RESO_MAX;
    }
    return true;
}

/* RGB565 → RGB888（5/6/5 位各自 ×255 满量程）。 */
static void song_rgb888(uint16_t v, uint8_t out[3])
{
    out[0] = (uint8_t)(((v >> 11) & 31u) * 255u / 31u);
    out[1] = (uint8_t)(((v >> 5) & 63u) * 255u / 63u);
    out[2] = (uint8_t)((v & 31u) * 255u / 31u);
}

/* side record 开头的 theme_t（40B）：magic "THM2" + CRC-32（0..35）都对才算有效主题
 * （只做 magic + CRC：上游 gfx.c theme_check 另判的名字可打印/与背景对比度是呈现层
 * 关切，由 UI 应用主题时处理）。
 * false ⇒ *out 不动。 */
static bool song_per_side_theme(const uint8_t *side, song_theme_view_t *out)
{
    uint32_t i;
    if (side == 0 || out == 0) return false;
    if (song_per_word(side) != SONG_THEME_MAGIC ||
        song_per_word(side + SONG_THEME_CRC) != st_crc32(side, SONG_THEME_CRC))
        return false;
    memcpy(out->name, side + SONG_THEME_NAME, sizeof out->name - 1u);
    out->name[sizeof out->name - 1u] = 0;
    for (i = 0; i < 5; i++) song_rgb888((uint16_t)song_per_word(side + SONG_THEME_C + 2u * i), out->c[i]);
    for (i = 0; i < 4; i++) song_rgb888((uint16_t)song_per_word(side + SONG_THEME_TRK + 2u * i), out->trk[i]);
    song_rgb888((uint16_t)song_per_word(side + SONG_THEME_BG), out->bg);
    song_rgb888((uint16_t)song_per_word(side + SONG_THEME_HI), out->hi);
    return true;
}

#endif
