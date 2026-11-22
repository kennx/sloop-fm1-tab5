/* SPDX-License-Identifier: GPL-3.0-only */
/* song_persist.h — OBJ_SETTINGS 的 PER3 歌曲子集（P4-C Task 7，ADR-009 §6 / ADR-010 §4）
 *
 * 上游把歌曲顺序（arr_config_t，36B）放进设置对象 persist_t 的偏移 48：
 *
 *   persist_t（88B，OBJ_SETTINGS 的 payload 开头）
 *     0   magic "PER3"（uint32）
 *     4   palette   8 lowcut   12 zoom
 *     16  panel_t（32B，PAN5 + 14 个按钮 + 7 个编码器 + 7 个方向）
 *     48  arr_config_t（36B：count/loop/reserved + 16×(scene,bars)）← 本模块唯一读写的一段
 *     84  lights（uint32）
 *     88  dsu_bank_t（1,464B，默认 SYN kit 银行）      ⇒ canonical = 1,552B
 *     1552.. 可选的 MIDI LEARN 尾段（完整 40B；2.5/2.6 测试写过 32/36，也合法）
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
 *   - 没有对象：canonical PER3（默认 panel、lights=0、当前歌曲）+ 默认 DSU + 默认 side；
 *   - 合法 PER3（84/88/1,552/1,584/1,588/1,592）：整段保留，只替换 48..83；尚无 DSU 的
 *     补默认 DSU；已有尾段绝不重写；
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

/* 默认 DSU 银行：上游 settings_write 先 `if (!dsu_valid(&dsu)) dsu_defaults();` 再写 &dsu */
static void song_dsu_default(uint8_t *dst)
{
    if (!dsu_valid(&dsu)) dsu_defaults();
    memcpy(dst, &dsu, SONG_DSU_LEN);
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
        song_dsu_default(payload + SONG_DSU_OFF);
        n = SONG_PER_CANON;
        break;
    case SONG_PER3:
        memcpy(payload + SONG_ARR_OFF, arr, SONG_ARR_LEN);
        if (n == SONG_PER_V22) { /* 2.2：补 lights（上游 persist_boot 把 2.2 的当 lights=0） */
            song_per_put_word(payload + SONG_LIGHTS_OFF, 0);
            n = SONG_PER_BASE;
        }
        if (n == SONG_PER_BASE) { /* 没有 DSU：补默认银行（已有尾段的对象不走这里） */
            song_dsu_default(payload + SONG_DSU_OFF);
            n = SONG_PER_CANON;
        }
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
        song_dsu_default(payload + SONG_DSU_OFF);
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
        song_dsu_default(payload + SONG_DSU_OFF);
        n = SONG_PER_CANON;
        break;
    }
    case SONG_PER_BAD:
    default:
        return SONG_PER_BAD;
    }
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

#endif
