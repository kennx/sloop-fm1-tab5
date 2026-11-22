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
    check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_SRC, 2) == 2 && raw_sound(0, 0, b) == ESP_OK && b[1] == 2u,
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

    /* 试听队列（drums.c drum_audition_poll 的输入） */
    sloop_core_dsyn_audition(2, 5, 100);
    check(dsu_aud_k == 2 && dsu_aud_lane == 5 && dsu_aud_vel == 100, "audition queues kit/lane/velocity");
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

int main(void)
{
    test_dsyn_fields();
    if (fails) {
        printf("SYNTH HOST TEST FAILED (%d failure%s)\n", fails, fails == 1 ? "" : "s");
        return 1;
    }
    printf("SYNTH HOST TEST PASSED\n");
    return 0;
}
