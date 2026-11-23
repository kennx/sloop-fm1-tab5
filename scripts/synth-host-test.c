/* SPDX-License-Identifier: GPL-3.0-only
 * P4-D Task 2: Tab5 DSYN 24 字段适配的同源 host 门禁。
 * hostsim.c 提供真实上游引擎/音序/鼓；project.c(PROJ_HOST) 是 P4-B 起的同源格式层；
 * sound_tab5.c 在末尾 include（与固件 selftest.c 的 TU 位置相同），所以上游 statics
 * （dsu/dsu_kit/dsu_from_factory/dsu_fix_sound/dsu_fix_kit/dsu_dirty/dsu_aud_*、
 * DS_KITS、DECAY_K/CUTOFF_HZ）都可见。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define main hostsim_main
#include "../third_party/sloop-fm1/tests/hostsim.c"
#undef main

#define PROJ_HOST 1
/* 设备的 TRK_DEF 映射（上游 ui.c：{ANALOG 0, DIGITAL 1, SAMPLE 4}）：轨 0/1/2 的默认
 * 引擎，轨 3 是鼓轨（无引擎）。project.c 的默认值/引擎回填按它走。 */
static uint32_t trk_def_engine(uint32_t i)
{
    static const uint8_t def[NPART] = {0, 1, 4};
    return i < NPART ? def[i] : 0u;
}
#include "../third_party/sloop-fm1/firmware/src/project.c"
#undef PROJ_HOST

/* 与固件同名的锁桩（host 单线程）：只计数，验证「一次动作一次引擎锁」。 */
static bool s_frozen;
static uint32_t engine_lock_calls, engine_unlock_calls;
static void engine_lock(void) { engine_lock_calls++; }
static void engine_unlock(void) { engine_unlock_calls++; }

#include "../firmware/components/sloop_core/src/sound_tab5.c"

static int fails;

static void check(int ok, const char *what)
{
    printf("synth-host: %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static esp_err_t raw_sound(uint32_t kit, uint32_t lane, uint8_t out[22])
{
    sloop_dsyn_sound_t s;
    esp_err_t ret = sloop_core_dsyn_read(kit, lane, &s, NULL, NULL, NULL);
    if (ret == ESP_OK)
        memcpy(out, s.sound, 22);
    return ret;
}

static void test_dsyn_fields(void)
{
    static const char *const want[SLOOP_DSYN_FIELDS] = {
        "WAVE", "SRC", "CLAP", "PITCH", "FINE", "BEND", "BTIME", "HOLD",
        "DECAY", "TLEV", "T2", "T2LEV", "CLICK", "NLEV", "NHOLD", "NDEC",
        "FLT_MODE", "FLT_RES", "FCUT", "FENV", "HPF", "CHIP", "DRIVE", "LEVEL",
    };
    sloop_dsyn_sound_t s;
    char name[9];
    uint8_t crush = 0xFF, src = 0xFF, b[22];
    sloop_desc_t d;
    uint32_t i;
    int ok;

    /* 字段表：恰 24 项、顺序即协议 */
    check(SLOOP_DSYN_FIELDS == 24u, "SLOOP_DSYN_FIELDS == 24");
    ok = 1;
    for (i = 0; i < SLOOP_DSYN_FIELDS; i++) {
        if (!sloop_core_dsyn_desc(i, &d) || strcmp(d.label, want[i]) != 0)
            ok = 0;
    }
    check(ok, "desc: 24 labels in protocol order");
    check(!sloop_core_dsyn_desc(SLOOP_DSYN_FIELDS, &d) && !sloop_core_dsyn_desc(0, NULL),
          "desc rejects field 24 and NULL");
    check(sloop_core_dsyn_desc(SLOOP_DSYN_FLT_MODE, &d) && d.fmt == SLOOP_DESC_FMT_ENUM && d.min == 0 &&
              d.max == 7 && d.enum_count == 8 && strcmp(d.enums[5], "LP+ALL") == 0,
          "desc FLT_MODE: enum 0..7, LP+ALL at 5");
    check(sloop_core_dsyn_desc(SLOOP_DSYN_LEVEL, &d) && d.min == 0 && d.max == 255 && d.fmt == SLOOP_DESC_FMT_DB,
          "desc LEVEL: 0..255 dB");
    check(sloop_core_dsyn_desc(SLOOP_DSYN_FENV, &d) && d.min == -128 && d.max == 127, "desc FENV: -128..127");

    check(!sloop_core_dsyn_dirty(), "dsu_dirty starts clear");

    /* 工厂 kit：名字/crush/src 与 22B 逐字节等于上游 DS_KITS[src] 的该 lane */
    check(sloop_core_dsyn_load_factory(0, 0) == ESP_OK, "load_factory(0, 0)");
    check(sloop_core_dsyn_dirty(), "load_factory marks dsu_dirty");
    memset(&s, 0, sizeof s);
    check(sloop_core_dsyn_read(0, 0, &s, name, &crush, &src) == ESP_OK && s.used == 1u &&
              memcmp(s.sound, &DS_KITS[0].s[0], 22) == 0 && strcmp(name, "808") == 0 &&
              crush == DS_KITS[0].crush && src == 0u,
          "factory kit: sound/name/crush/src = DS_KITS[0]");

    /* 24 字段的原始字节偏移 oracle（**独立于实现表**，按 dsnd_t 声明顺序手写）：
     * 逐字段写一个互不相同的值再读 22B，能抓到描述符表里的转置/重复偏移。
     * SRC/CLAP 共用 byte1、FLT_MODE/FLT_RES 共用 byte15，按各自的位段断言。 */
    {
        static const uint8_t want_off[SLOOP_DSYN_FIELDS] = {
            0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 16, 17, 18, 19, 20, 21,
        };
        static const int16_t want_val[SLOOP_DSYN_FIELDS] = {
            2, 3, 1, 61, 13, 37, 71, 77, 41, 99, 17, 83, 55, 91, 23, 67, 6, 21, 88, -60, 44, 72, 29, 200,
        };
        char msg[96];
        for (i = 0; i < SLOOP_DSYN_FIELDS; i++) {
            int16_t applied = sloop_core_dsyn_set(0, 0, i, want_val[i]);
            int okf;
            raw_sound(0, 0, b);
            switch (i) {
            case SLOOP_DSYN_SRC: okf = (b[1] & 15u) == (uint8_t)want_val[i]; break;
            case SLOOP_DSYN_CLAP: okf = ((b[1] & 0x10u) != 0) == (want_val[i] != 0); break;
            case SLOOP_DSYN_FLT_MODE: okf = (b[15] & 7u) == (uint8_t)want_val[i]; break;
            case SLOOP_DSYN_FLT_RES: okf = (b[15] >> 3) == (uint8_t)want_val[i]; break;
            default: okf = b[want_off[i]] == (uint8_t)want_val[i]; break;
            }
            snprintf(msg, sizeof msg, "field %2u %-8s writes raw[%u] = %d", (unsigned)i, want[i],
                     (unsigned)want_off[i], (int)want_val[i]);
            check(applied == want_val[i] && okf, msg);
        }
    }

    /* 单字段写：返回值 = 钳位后的值，原始 22B 的对应字节随之改变 */
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_WAVE, 1) == 1 && raw_sound(0, 0, b) == ESP_OK && b[0] == 1u,
          "WAVE=1 -> raw[0]");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_PITCH, 60) == 60 && raw_sound(0, 0, b) == ESP_OK && b[2] == 60u,
          "PITCH=60 -> raw[2]");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FINE, 15) == 15 && raw_sound(0, 0, b) == ESP_OK && b[3] == 15u,
          "FINE=15 -> raw[3]");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_BEND, 96) == 96 && raw_sound(0, 0, b) == ESP_OK && b[4] == 96u,
          "BEND=96 -> raw[4]");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_T2, 31) == 31 && raw_sound(0, 0, b) == ESP_OK && b[9] == 31u,
          "T2=31 -> raw[9]");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_LEVEL, 255) == 255 && raw_sound(0, 0, b) == ESP_OK && b[21] == 255u,
          "LEVEL=255 -> raw[21]");

    /* FLT：MODE 与 RES 分属低 3 位 / 高 5 位，互不覆盖 */
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FLT_MODE, 1) == 1, "FLT_MODE=LP(1)");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FLT_RES, 31) == 31, "FLT_RES=31");
    raw_sound(0, 0, b);
    check(b[15] == ((31u << 3) | 1u) && sloop_core_dsyn_field(0, 0, SLOOP_DSYN_FLT_MODE) == 1 &&
              sloop_core_dsyn_field(0, 0, SLOOP_DSYN_FLT_RES) == 31,
          "FLT_RES keeps MODE; raw flt == (31 << 3) | 1");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FLT_MODE, 5) == 5 && raw_sound(0, 0, b) == ESP_OK &&
              b[15] == ((31u << 3) | 5u) && sloop_core_dsyn_field(0, 0, SLOOP_DSYN_FLT_RES) == 31,
          "FLT_MODE=LP+ALL keeps RES");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FLT_MODE, 4) == 0 && raw_sound(0, 0, b) == ESP_OK &&
              b[15] == (31u << 3),
          "FLT_MODE=4 (ALL alone) normalises to OFF, keeps RES");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FLT_MODE, 1) == 1, "FLT_MODE back to LP");

    /* CLAP 是 SRC 的 bit4：写一个不动另一个 */
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_SRC, 2) == 2 && raw_sound(0, 0, b) == ESP_OK &&
              (b[1] & 15u) == 2u,
          "SRC=2 -> raw[1] low nibble");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_CLAP, 1) == 1 && raw_sound(0, 0, b) == ESP_OK && b[1] == 0x12u &&
              sloop_core_dsyn_field(0, 0, SLOOP_DSYN_SRC) == 2,
          "CLAP=1 sets src bit4, SRC unchanged");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_SRC, 4) == 4 && raw_sound(0, 0, b) == ESP_OK && b[1] == 0x14u &&
              sloop_core_dsyn_field(0, 0, SLOOP_DSYN_CLAP) == 1,
          "SRC keeps the CLAP bit");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_CLAP, 0) == 0 && raw_sound(0, 0, b) == ESP_OK && b[1] == 4u,
          "CLAP=0 clears src bit4");

    /* 钳位：dsu_fix_sound 不钳 hold/nhold/t2/level/flt/fenv，描述符范围是唯一闸门 */
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_HOLD, 200) == 127 && raw_sound(0, 0, b) == ESP_OK && b[6] == 127u,
          "HOLD=200 clamps to 127");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_BEND, 200) == 96, "BEND=200 clamps to 96");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FINE, -5) == 0, "FINE=-5 clamps to 0");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_T2, 60) == 31, "T2=60 clamps to 31");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_LEVEL, 300) == 255, "LEVEL=300 clamps to 255");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FENV, -128) == -128 &&
              sloop_core_dsyn_field(0, 0, SLOOP_DSYN_FENV) == -128,
          "FENV=-128 round-trips as a signed byte");
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FENV, 200) == 127, "FENV=200 clamps to 127");

    /* 纯解码（UI 投影用）：不取锁、不改数据 */
    {
        uint8_t raw[22] = {0};
        raw[1] = 0x13;               /* src 3 + CLAP */
        raw[15] = (9u << 3) | 2u;    /* BP, res 9 */
        raw[17] = 0xFF;              /* fenv -1 */
        raw[21] = 192;               /* level */
        check(sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_CLAP) == 1 &&
                  sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_SRC) == 3 &&
                  sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_FLT_MODE) == 2 &&
                  sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_FLT_RES) == 9 &&
                  sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_FENV) == -1 &&
                  sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_LEVEL) == 192 &&
                  sloop_dsyn_field_from_sound(raw, SLOOP_DSYN_FIELDS) == SLOOP_DSYN_INVALID,
              "field_from_sound decodes bytes (and rejects field 24)");
    }

    /* dirty 记账（上游 ED_DSYN_PUT：任何成功的单字段写都置 dsu_dirty） */
    dsu_dirty = 0;
    check(!sloop_core_dsyn_dirty(), "dsu_dirty can be cleared (test hook)");
    (void)sloop_core_dsyn_set(0, 0, SLOOP_DSYN_DRIVE, 11);
    check(sloop_core_dsyn_dirty(), "a single set marks dsu_dirty");

    /* 试听队列（drums.c drum_audition_poll 的输入）；velocity 0/越界按上游 ED_DSYN_PLAY 拒绝 */
    sloop_core_dsyn_audition(2, 5, 100);
    check(dsu_aud_k == 2 && dsu_aud_lane == 5 && dsu_aud_vel == 100, "audition queues kit/lane/velocity");
    sloop_core_dsyn_audition(2, 6, 0);
    check(dsu_aud_k == 2 && dsu_aud_lane == 5 && dsu_aud_vel == 100, "audition rejects velocity 0");
    sloop_core_dsyn_audition(2, 6, 200);
    check(dsu_aud_k == 2 && dsu_aud_lane == 5 && dsu_aud_vel == 100, "audition rejects velocity > 127");
    sloop_core_dsyn_audition(4, 0, 100);
    check(dsu_aud_k == 2 && dsu_aud_lane == 5 && dsu_aud_vel == 100, "audition rejects kit 4");
    dsu_aud_vel = 0;

    /* 工厂 kit 整体覆盖该 kit 的编辑 */
    check(sloop_core_dsyn_load_factory(1, 5) == ESP_OK, "load_factory(1, 5)");
    check(sloop_core_dsyn_read(1, 0, &s, name, &crush, &src) == ESP_OK &&
              memcmp(s.sound, &DS_KITS[5].s[0], 22) == 0 && strncmp(name, DS_KITS[5].name, 8) == 0 &&
              src == 5u,
          "factory kit 1 = DS_KITS[5]");

    /* 非法参数：越界字段不写、不改数据 */
    check(sloop_core_dsyn_read(4, 0, &s, name, &crush, &src) == ESP_ERR_INVALID_ARG, "read rejects kit 4");
    check(sloop_core_dsyn_read(0, SLOOP_DSYN_LANES, &s, name, &crush, &src) == ESP_ERR_INVALID_ARG,
          "read rejects lane 16");
    check(sloop_core_dsyn_load_factory(0, DS_NKITS) == ESP_ERR_INVALID_ARG, "load_factory rejects src 32");
    raw_sound(0, 0, b);
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FIELDS, 1) == SLOOP_DSYN_INVALID, "set rejects field 24");
    {
        uint8_t after[22];
        raw_sound(0, 0, after);
        check(memcmp(b, after, 22) == 0, "invalid field leaves the sound unchanged");
    }

    /* 诊断冻结：读/写都拒绝（不取锁） */
    s_frozen = true;
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_WAVE, 3) == SLOOP_DSYN_INVALID &&
              sloop_core_dsyn_read(0, 0, &s, name, &crush, &src) == ESP_ERR_INVALID_STATE,
          "frozen: set/read refused");
    s_frozen = false;

    /* 一次动作一次引擎锁 */
    {
        uint32_t l = engine_lock_calls, u = engine_unlock_calls;
        (void)sloop_core_dsyn_set(0, 0, SLOOP_DSYN_DRIVE, 10);
        check(engine_lock_calls == l + 1u && engine_unlock_calls == u + 1u, "set takes the engine lock once");
    }

    /* 文本：索引→ms/Hz、LEVEL dB、FLT_MODE 名；未覆盖的字段留给通用格式化 */
    {
        char t[16];
        check(sloop_core_dsyn_text(SLOOP_DSYN_FLT_MODE, 5, t, sizeof t) > 0 && strcmp(t, "LP+ALL") == 0,
              "text FLT_MODE 5 = LP+ALL");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FLT_MODE, 4, t, sizeof t) > 0 && strcmp(t, "OFF") == 0,
              "text FLT_MODE 4 = OFF");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 128, t, sizeof t) > 0 && strcmp(t, "0.0dB") == 0,
              "text LEVEL 128 = 0.0dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 152, t, sizeof t) > 0 && strcmp(t, "6.0dB") == 0,
              "text LEVEL 152 = 6.0dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 0, t, sizeof t) > 0 && strcmp(t, "-32.0dB") == 0,
              "text LEVEL 0 = -32.0dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FCUT, 0, t, sizeof t) > 0 && strcmp(t, "30Hz") == 0,
              "text FCUT 0 = 30Hz");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FCUT, 127, t, sizeof t) > 0 && strcmp(t, "16.0kHz") == 0,
              "text FCUT 127 = 16.0kHz");
        check(sloop_core_dsyn_text(SLOOP_DSYN_DECAY, 0, t, sizeof t) > 0 && strcmp(t, "5ms") == 0,
              "text DECAY 0 = 5ms");
        check(sloop_core_dsyn_text(SLOOP_DSYN_CHIP, 60, t, sizeof t) > 0 && strcmp(t, "C4") == 0,
              "text CHIP 60 = C4");
        check(sloop_core_dsyn_text(SLOOP_DSYN_WAVE, 1, t, sizeof t) == 0,
              "text: enum field defers to knobs' generic formatting");
    }
}

/* P4-D Task 3：FM6 155B patch 的字段描述符与写入口。
 * 布局 oracle 独立于实现表：算子 21 字段就是 `FP_R1..FP_DET` 在 k*FP_OP 块内的偏移，
 * 全局 29 项 = `FP_PR1..FP_TRNSP`（126..144，19 项）+ 名字 10 字节（145..154，只读）。
 * 范围 oracle 用上游 `fm6_max()` 的同一张表（`fm6_sanitize` 的唯一闸门）。 */
static void test_fm6_fields(void)
{
    static const char *const want_op[SLOOP_FM6_OP_FIELDS] = {
        "R1", "R2", "R3", "R4", "L1", "L2", "L3", "L4", "BP", "LD", "RD",
        "LC", "RC", "RS", "AMS", "KVS", "OL", "MODE", "FC", "FF", "DET",
    };
    static const char *const want_global[SLOOP_FM6_GLOBAL_FIELDS] = {
        "PR1", "PR2", "PR3", "PR4", "PL1", "PL2", "PL3", "PL4", "ALG", "FB",
        "OKS", "LFS", "LFD", "LPMD", "LAMD", "LKS", "LFW", "LPMS", "TRNSP",
        "NAME1", "NAME2", "NAME3", "NAME4", "NAME5",
        "NAME6", "NAME7", "NAME8", "NAME9", "NAME10",
    };
    /* 上游 eng_fm6.c 里的字段编号（独立手抄，防止实现里 op/field 转置） */
    enum { T_R1 = 0, T_L4 = 7, T_LC = 11, T_OL = 16, T_MODE = 17, T_FC = 18, T_DET = 20 };
    enum {
        G_PR1 = 0, G_PL4 = 7, G_ALG = 8, G_FB = 9, G_LKS = 15, G_LFW = 16, G_LPMS = 17, G_TRNSP = 18,
        G_NAME1 = 19, G_NAME10 = 28,
    };
    uint8_t v[FP_SIZE + 1u], back[FP_SIZE + 1u], pk[128];
    sloop_fm6_patch_snapshot_t snap, locked;
    sloop_desc_t d;
    uint32_t op, i;
    int ok;
    uint8_t save_req0 = trk[0].eng_req, save_req1 = trk[1].eng_req;
    int16_t save_e0 = trk[0].p[P_E0], save_e7 = trk[0].p[P_E7];
    uint8_t save_patch[FP_SIZE + 1u], save_slot = fm6_slot[0];

    check(SLOOP_FM6_PATCH_SIZE == 155u && SLOOP_FM6_OP_FIELDS == 21u && SLOOP_FM6_GLOBAL_FIELDS == 29u,
          "FM6 sizes: 155B, 6 x 21 op fields, 29 global fields");

    /* 描述符：6 x 21 个算子字段 + 29 个全局字段，顺序即协议 */
    ok = 1;
    for (op = 0; op < 6u; op++) {
        for (i = 0; i < SLOOP_FM6_OP_FIELDS; i++) {
            if (!sloop_core_fm6_desc(op, i, &d) || strcmp(d.label, want_op[i]) != 0)
                ok = 0;
        }
    }
    check(ok, "desc: 6 x 21 operator fields in FP_R1..FP_DET order");
    ok = 1;
    for (i = 0; i < SLOOP_FM6_GLOBAL_FIELDS; i++) {
        if (!sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, i, &d) || strcmp(d.label, want_global[i]) != 0)
            ok = 0;
    }
    check(ok, "desc: 29 global fields (19 voice + 10 read-only name bytes)");
    check(!sloop_core_fm6_desc(7u, 0, &d) && !sloop_core_fm6_desc(0, SLOOP_FM6_OP_FIELDS, &d) &&
              !sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, SLOOP_FM6_GLOBAL_FIELDS, &d) &&
              !sloop_core_fm6_desc(0, 0, NULL),
          "desc rejects op 7 / field 21 / global field 29 / NULL");

    /* 范围 = 上游 fm6_max()：LC/RC 3、RS 7、MODE 1、FC 31、DET 14；全局 ALG 31、FB 7、LFW 5、LPMS 7、TRNSP 48 */
    check(sloop_core_fm6_desc(0, T_DET, &d) && d.min == 0 && d.max == 14 && d.fmt == SLOOP_DESC_FMT_INT &&
              sloop_core_fm6_desc(0, T_FC, &d) && d.max == 31 &&
              sloop_core_fm6_desc(0, T_LC, &d) && d.max == 3 &&
              sloop_core_fm6_desc(0, T_MODE, &d) && d.max == 1 && d.fmt == SLOOP_DESC_FMT_ONOFF,
          "desc op: DET 14 / FC 31 / LC 3 / MODE onoff");
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_ALG, &d) && d.min == 0 && d.max == 31 &&
              sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_FB, &d) && d.max == 7 &&
              sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_LFW, &d) && d.max == 5 &&
              sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_TRNSP, &d) && d.max == 48 &&
              sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_PR1, &d) && d.max == 99,
          "desc global: ALG 31 / FB 7 / LFW 5 / TRNSP 48 / PR1 99");
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_NAME1, &d) && d.min == 32 && d.max == 126,
          "desc NAME1: printable 32..126 (read-only)");

    /* MODE 是每个算子块里的同一字段：按 (op, field) 判，不能只看绝对偏移（只命中 op 0） */
    ok = 1;
    for (op = 0; op < 6u; op++) {
        if (!sloop_core_fm6_desc(op, T_MODE, &d) || d.fmt != SLOOP_DESC_FMT_ONOFF || d.max != 1 ||
            strcmp(d.label, "MODE") != 0)
            ok = 0;
    }
    check(ok, "desc MODE onoff for all six operators");
    check(sloop_core_fm6_desc(1, T_MODE, &d) && d.fmt == SLOOP_DESC_FMT_ONOFF &&
              sloop_core_fm6_desc(5, T_MODE, &d) && d.fmt == SLOOP_DESC_FMT_ONOFF &&
              sloop_core_fm6_desc(1, T_MODE - 1, &d) && d.fmt == SLOOP_DESC_FMT_INT,
          "desc op 1 / op 5 MODE onoff (OL stays INT)");
    /* 反向：全局 LFW（142）/LPMS（143）不能被误判成 MODE/onoff——LPMS % 21 == 17 的假阳性 */
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_LPMS, &d) && d.fmt == SLOOP_DESC_FMT_ENUM &&
              sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_LFW, &d) && d.fmt == SLOOP_DESC_FMT_ENUM,
          "desc global LFW/LPMS are enums, not MODE/onoff");

    /* 枚举表（spec §4.1）：ALG 0..31 -> "1".."32"，LFW 0..5 -> 上游 fm6_lfo_sample 的波形，
     * LPMS 0..7 -> 灵敏度级别 */
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_ALG, &d) && d.enums != NULL && d.enum_count == 32 &&
              strcmp(d.enums[0], "1") == 0 && strcmp(d.enums[31], "32") == 0 && d.fmt == SLOOP_DESC_FMT_ENUM,
          "desc ALG: 32 enum names, \"1\"..\"32\"");
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_LFW, &d) && d.enums != NULL && d.enum_count == 6 &&
              strcmp(d.enums[0], "TRI") == 0 && strcmp(d.enums[1], "SAW DN") == 0 &&
              strcmp(d.enums[2], "SAW UP") == 0 && strcmp(d.enums[3], "SQR") == 0 &&
              strcmp(d.enums[4], "SIN") == 0 && strcmp(d.enums[5], "S&H") == 0,
          "desc LFW: 6 waves in fm6_lfo_sample order");
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_LPMS, &d) && d.enums != NULL && d.enum_count == 8 &&
              strcmp(d.enums[0], "0") == 0 && strcmp(d.enums[7], "7") == 0,
          "desc LPMS: 8 sensitivity levels \"0\"..\"7\"");
    check(sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, G_LKS, &d) && d.fmt == SLOOP_DESC_FMT_ONOFF &&
              d.max == 1 && d.enums == NULL,
          "desc LKS: onoff, no enum table");
    check(sloop_core_fm6_desc(0, T_DET, &d) && d.enums == NULL && d.enum_count == 0,
          "desc plain field: no enum table");

    /* 描述符失败不得改 out（注入前 Sentinels 逐个字段） */
    {
        sloop_desc_t before;
        memset(&before, 0x5A, sizeof before);
        d = before;
        (void)sloop_core_fm6_desc(7u, 0, &d);
        check(memcmp(&d, &before, sizeof d) == 0, "failed desc leaves out untouched (op)");
        (void)sloop_core_fm6_desc(0, SLOOP_FM6_OP_FIELDS, &d);
        check(memcmp(&d, &before, sizeof d) == 0, "failed desc leaves out untouched (field)");
        (void)sloop_core_fm6_desc(SLOOP_FM6_GLOBAL_OP, SLOOP_FM6_GLOBAL_FIELDS, &d);
        check(memcmp(&d, &before, sizeof d) == 0, "failed desc leaves out untouched (global field)");
    }

    /* 基线：轨 0 换成 FM6、PTCH = 2，patch = 上游 FM6_INIT（packed 128B -> 155B） */
    memcpy(save_patch, fm6_patch[0], sizeof save_patch);
    fm6_unpack(FM6_INIT, v);
    fm6_set_patch(0, v);
    fm6_slot[0] = 2u;
    trk[0].eng_req = (uint8_t)ENGI_FM6;
    trk[0].p[P_E0] = 5;      /* ALG 宏非 PAT：深度写 ALG 必须把它清回 PAT */
    trk[0].p[P_E7] = 2;

    /* 单字段写 -> 155B 的对应字节（算子块 k*21 + field） */
    check(sloop_core_fm6_set(0, 0, T_R1, 99) == 99 && sloop_core_fm6_set(0, 0, T_L4, 99) == 99 &&
              sloop_core_fm6_set(0, 0, T_OL, 99) == 99 && sloop_core_fm6_set(0, 0, T_DET, 14) == 14 &&
              sloop_core_fm6_set(0, 0, T_FC, 31) == 31,
          "set op1 R1/L4/OL/DET/FC accepted");
    check(sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.engine_ok == 1u && snap.slot == 2u &&
              snap.patch[0 * FP_OP + T_R1] == 99u && snap.patch[0 * FP_OP + T_L4] == 99u &&
              snap.patch[0 * FP_OP + T_OL] == 99u && snap.patch[0 * FP_OP + T_DET] == 14u &&
              snap.patch[0 * FP_OP + T_FC] == 31u && memcmp(snap.patch, fm6_patch[0], 155) == 0,
          "patch(): op1 bytes at k*FP_OP + field, engine_ok/slot");
    /* 第 6 个算子（op 5）走同一张表 */
    check(sloop_core_fm6_set(0, 5, T_R1, 42) == 42 &&
              sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.patch[5 * FP_OP + T_R1] == 42u &&
              snap.patch[4 * FP_OP + T_R1] != 42u,
          "set op6 writes patch[5*FP_OP + R1] only");

    /* 钳位：描述符/fm6_sanitize 同一张 fm6_max() 表 */
    check(sloop_core_fm6_set(0, 0, T_OL, 200) == 99, "OL=200 clamps to 99");
    check(sloop_core_fm6_set(0, 0, T_DET, 99) == 14, "DET=99 clamps to 14");
    check(sloop_core_fm6_set(0, 0, T_MODE, 5) == 1, "MODE=5 clamps to 1");
    check(sloop_core_fm6_set(0, 0, T_R1, -3) == 0, "R1=-3 clamps to 0");

    /* 全局字段：PL4 在 133、TRNSP 在 144、ALG 在 134 且同时清 ALG 宏 */
    check(sloop_core_fm6_set(0, SLOOP_FM6_GLOBAL_OP, G_PL4, 99) == 99 &&
              sloop_core_fm6_set(0, SLOOP_FM6_GLOBAL_OP, G_TRNSP, 99) == 48 &&
              sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.patch[FP_PL1 + 3] == 99u &&
              snap.patch[FP_TRNSP] == 48u,
          "set global PL4 -> patch[133], TRNSP=99 -> 48");
    check(sloop_core_fm6_set(0, SLOOP_FM6_GLOBAL_OP, G_ALG, 31) == 31 &&
              sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.patch[FP_ALG] == 31u && trk[0].p[P_E0] == 0,
          "set global ALG=31 -> patch[FP_ALG], clears P_E0 (PAT)");
    trk[0].p[P_E0] = 5;   /* 模拟用户把 ALG 宏调回 1..32 */
    check(sloop_core_fm6_set(0, 0, T_R1, 99) == 99 && trk[0].p[P_E0] == 5,
          "a non-ALG field leaves P_E0 alone");

    /* 名字 10 字节只读（首尾都测）：写被拒、155B 逐字节不变 */
    check(sloop_core_fm6_patch(0, &snap) == ESP_OK, "patch() before name write");
    check(sloop_core_fm6_set(0, SLOOP_FM6_GLOBAL_OP, G_NAME1, 'A') == SLOOP_FM6_INVALID &&
              sloop_core_fm6_set(0, SLOOP_FM6_GLOBAL_OP, G_NAME10, 'A') == SLOOP_FM6_INVALID &&
              sloop_core_fm6_patch(0, &locked) == ESP_OK && memcmp(snap.patch, locked.patch, 155) == 0,
          "set NAME1 / NAME10 are read-only (sentinel, patch unchanged)");

    /* 纯解码：算子/全局/名字的绝对偏移（名字可解码、不可写） */
    check(sloop_fm6_field_from_patch(fm6_patch[0], 3u, T_OL) == fm6_patch[0][3 * FP_OP + T_OL] &&
              sloop_fm6_field_from_patch(fm6_patch[0], SLOOP_FM6_GLOBAL_OP, G_TRNSP) == fm6_patch[0][FP_TRNSP] &&
              sloop_fm6_field_from_patch(fm6_patch[0], SLOOP_FM6_GLOBAL_OP, G_NAME1) == fm6_patch[0][FP_NAME] &&
              sloop_fm6_field_from_patch(fm6_patch[0], 7u, 0) == SLOOP_FM6_INVALID &&
              sloop_fm6_field_from_patch(fm6_patch[0], 0, SLOOP_FM6_OP_FIELDS) == SLOOP_FM6_INVALID &&
              sloop_fm6_field_from_patch(NULL, 0, 0) == SLOOP_FM6_INVALID,
          "field_from_patch decodes offsets, rejects op 7 / field 21 / NULL");
    check(sloop_core_fm6_field(0, 0, T_DET) == 14 && sloop_core_fm6_field(0, SLOOP_FM6_GLOBAL_OP, G_ALG) == 31 &&
              sloop_core_fm6_field(0, SLOOP_FM6_GLOBAL_OP, G_NAME1) == (int16_t)fm6_patch[0][FP_NAME],
          "field() reads the live patch (name bytes readable)");

    /* 非 FM6 轨：patch() 报 NOT_ALLOWED、engine_ok=0，写/读都回哨兵且不改 patch */
    {
        uint8_t before = fm6_patch[1][T_R1];
        trk[1].eng_req = 0u;   /* ANALOG */
        memset(&locked, 0xA5, sizeof locked);
        check(sloop_core_fm6_patch(1, &locked) == ESP_ERR_NOT_ALLOWED && locked.engine_ok == 0u,
              "patch() on an ANALOG track: ESP_ERR_NOT_ALLOWED, engine_ok=0");
        check(sloop_core_fm6_set(1, 0, T_R1, (int16_t)(before == 99u ? 98u : 99u)) == SLOOP_FM6_INVALID &&
                  sloop_core_fm6_field(1, 0, T_R1) == SLOOP_FM6_INVALID && fm6_patch[1][T_R1] == before,
              "set/field on an ANALOG track: sentinel, patch unchanged");
    }

    /* 越界轨（鼓轨在 host 里就是 track >= NPART）与非法 op/field */
    check(sloop_core_fm6_patch(NPART, &snap) == ESP_ERR_INVALID_ARG && sloop_core_fm6_set(NPART, 0, 0, 1) ==
              SLOOP_FM6_INVALID && sloop_core_fm6_patch(0, NULL) == ESP_ERR_INVALID_ARG,
          "rejects track NPART and NULL out");
    check(sloop_core_fm6_set(0, 7u, 0, 1) == SLOOP_FM6_INVALID &&
              sloop_core_fm6_set(0, 0, SLOOP_FM6_OP_FIELDS, 1) == SLOOP_FM6_INVALID &&
              sloop_core_fm6_set(0, SLOOP_FM6_GLOBAL_OP, SLOOP_FM6_GLOBAL_FIELDS, 1) == SLOOP_FM6_INVALID,
          "rejects op 7 / field 21 / global field 29");

    /* PTCH 语义：写成功后 fm6_slot[track] == p[P_E7]，fm6_poll 不会用槽 patch 覆盖编辑。
     * 反向控制（防空洞通过）：把 PTCH 换到空 bank 槽（8 = B1，host 无银行 ⇒ FM6_INIT，
     * 其 R1 = 99），fm6_poll 必须真的换掉 patch；再在新槽下重做编辑，poll 又不再覆盖。 */
    trk[0].p[P_E7] = 2;
    check(sloop_core_fm6_set(0, 0, T_R1, 50) == 50 && fm6_slot[0] == 2u,
          "a write sets fm6_slot[track] = p[P_E7]");
    fm6_poll();
    check(sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.patch[T_R1] == 50u,
          "the edit survives fm6_poll (PTCH unchanged)");
    trk[0].p[P_E7] = 8;
    fm6_poll();
    check(sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.patch[T_R1] == 99u && fm6_slot[0] == 8u,
          "turning PTCH does reload the slot patch (control: R1 back to 99)");
    check(sloop_core_fm6_set(0, 0, T_R1, 50) == 50 && fm6_slot[0] == 8u,
          "re-edit after the PTCH turn re-syncs fm6_slot");
    fm6_poll();
    check(sloop_core_fm6_patch(0, &snap) == ESP_OK && snap.patch[T_R1] == 50u,
          "the edit survives fm6_poll in the new slot");

    /* 诊断冻结：读/写都回哨兵、不取锁 */
    {
        uint32_t l = engine_lock_calls, u = engine_unlock_calls;
        s_frozen = true;
        check(sloop_core_fm6_set(0, 0, T_R1, 3) == SLOOP_FM6_INVALID &&
                  sloop_core_fm6_field(0, 0, T_R1) == SLOOP_FM6_INVALID &&
                  sloop_core_fm6_patch(0, &snap) == ESP_ERR_INVALID_STATE,
              "frozen: set/field/patch refused");
        s_frozen = false;
        check(engine_lock_calls == l && engine_unlock_calls == u, "frozen paths take no engine lock");
    }

    /* 一次动作一次引擎锁 */
    {
        uint32_t l = engine_lock_calls, u = engine_unlock_calls;
        (void)sloop_core_fm6_set(0, 0, T_R1, 60);
        check(engine_lock_calls == l + 1u && engine_unlock_calls == u + 1u, "set takes the engine lock once");
        l = engine_lock_calls;
        u = engine_unlock_calls;
        (void)sloop_core_fm6_patch(0, &snap);
        check(engine_lock_calls == l + 1u && engine_unlock_calls == u + 1u, "patch takes the engine lock once");
    }

    /* 128B packed 往返：fm6_pack -> fm6_unpack 与 patch() 逐字节一致 */
    check(sloop_core_fm6_patch(0, &snap) == ESP_OK, "patch() before pack round-trip");
    fm6_pack(snap.patch, pk);
    fm6_unpack(pk, back);
    check(memcmp(back, snap.patch, 155) == 0, "fm6_pack/unpack round-trips the edited patch");

    /* 还原现场（其余用例共享同一进程状态） */
    memcpy(fm6_patch[0], save_patch, sizeof save_patch);
    fm6_slot[0] = save_slot;
    trk[0].eng_req = save_req0;
    trk[0].p[P_E0] = save_e0;
    trk[0].p[P_E7] = save_e7;
    trk[1].eng_req = save_req1;
}

int main(void)
{
    test_dsyn_fields();
    test_fm6_fields();
    if (fails) {
        printf("SYNTH HOST TEST FAILED (%d failure%s)\n", fails, fails == 1 ? "" : "s");
        return 1;
    }
    printf("SYNTH HOST TEST PASSED\n");
    return 0;
}
