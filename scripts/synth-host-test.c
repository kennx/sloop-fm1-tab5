/* SPDX-License-Identifier: GPL-3.0-only
 * P4-D Task 2/4: Tab5 DSYN 24 字段与采样槽适配的同源 host 门禁。
 * hostsim.c 提供真实上游引擎/音序/鼓；project.c(PROJ_HOST) 是 P4-B 起的同源格式层；
 * storage.c 提供真实的 st_crc32（采样 header 的 CRC 与固件同一实现）；
 * sound_tab5.c 在末尾 include（与固件 selftest.c 的 TU 位置相同），所以上游 statics
 * （dsu/dsu_kit/dsu_from_factory/dsu_fix_sound/dsu_fix_kit/dsu_dirty/dsu_aud_*、
 * DS_KITS、DECAY_K/CUTOFF_HZ、usr_nz/usr_zone/smp_user_scan）都可见。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 采样槽镜像：与固件 selftest.c 同法把上游 SMP_USER_XIP 重定向到 4×80KiB RAM 镜像，
 * 并让 HAL 的 sloop_hal_user_samples() 返回同一块（设备上那是 PSRAM）。
 * `smp_user_scan` 把 zone 的 off 重定基到 SMP_DATA（32 位偏移空间），故镜像必须落在
 * SMP_DATA 之上且在 4 GiB 内——与上游 tests/slice_test.c 的要求相同。 */
#define H_SAMPLE_SLOTS 4u
#define H_SAMPLE_SIZE  0x14000u
static uint8_t h_samples[H_SAMPLE_SLOTS * H_SAMPLE_SIZE];
uint8_t *sloop_hal_user_samples(void)
{
    return h_samples;
}
bool sloop_hal_storage_ready(void)
{
    return true;
}
#define SMP_USER_XIP(k) ((const uint8_t *)h_samples + (uint32_t)(k) * H_SAMPLE_SIZE)

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

/* ---------------------------------------------------------------- 假 flash ---
 * 设备的 HAL 存储后端（sloopstore 分区 + mmap）不在本可执行文件里：这里用一块覆盖
 * 上游逻辑区间 [0x97000, 0xFF000) 的 RAM 镜像代替，语义与 storage_flash.c 相同
 * （按扇区擦、NOR 只能把 1 写成 0）。采样槽 USR1..4 都落在这段区间内。 */
#define H_FLASH_BASE 0x97000u
#define H_FLASH_SPAN 0x68000u
#define H_SECTOR 0x1000u
static uint8_t h_flash[H_FLASH_SPAN];

static int st_off(uint32_t off, uint32_t n)
{
    if (off < H_FLASH_BASE || n > H_FLASH_SPAN || off - H_FLASH_BASE > H_FLASH_SPAN - n) return -1;
    return (int)(off - H_FLASH_BASE);
}

static int st_read(uint32_t off, void *dst, uint32_t n)
{
    int at = st_off(off, n);
    if (at < 0 || (dst == NULL && n > 0)) return -1;
    if (n > 0) memcpy(dst, h_flash + at, n);
    return 0;
}

static int st_erase(uint32_t off)
{
    if (st_off(off, H_SECTOR) < 0 || (off & (H_SECTOR - 1u)) != 0u) return -1;
    memset(h_flash + (off - H_FLASH_BASE), 0xFF, H_SECTOR);
    return 0;
}

static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    int at = st_off(off, n);
    const uint8_t *b = src;
    uint32_t i;
    if (at < 0 || (b == NULL && n > 0)) return -1;
    for (i = 0; i < n; i++) h_flash[(uint32_t)at + i] &= b[i];
    return 0;
}

#include "scratch.h" /* main_scratch（storage.c 的 st_buf） */
#include "../third_party/sloop-fm1/firmware/src/storage.c"

/* 与固件同名的锁/门桩（host 单线程）：只计数，验证「一次动作一次引擎锁」。
 * 设备侧这些是 store_tab5.c 的 static（同一 TU，单一来源，不复制锁）与公共 ready 门。 */
static bool s_frozen;
static bool h_store_ready = true;
static uint32_t engine_lock_calls, engine_unlock_calls;
static uint32_t store_lock_calls, store_unlock_calls;
static void engine_lock(void) { engine_lock_calls++; }
static void engine_unlock(void) { engine_unlock_calls++; }
static void store_lock(void) { store_lock_calls++; }
static void store_unlock(void) { store_unlock_calls++; }
static bool store_playing(void) { return song.playing || transport_req; }
bool sloop_store_ready(void) { return h_store_ready; }

#include "../firmware/components/sloop_core/src/sound_tab5.c"
/* 参数描述符投影（sloop_core_desc / sloop_core_desc_track_live）：与固件 selftest.c 同一实现。 */
#include "../firmware/components/sloop_core/src/desc_tab5.h"

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

    /* kit 级 CRUSH（P4-D Task 9 fix）：0/255 边界、>255 与负值钳位、脏位、dsu_fix_kit
     * 不动 16 lane/name/src；越界 kit / 冻结回哨兵；一次动作一次引擎锁。 */
    check(sloop_core_dsyn_load_factory(2, 3) == ESP_OK, "crush: load_factory(2, 3)");
    dsu_dirty = 0;
    check(sloop_core_dsyn_set_crush(2, 0) == 0 && dsu.k[2].crush == 0u && dsu_dirty,
          "crush 0 round-trips and marks dsu_dirty");
    check(sloop_core_dsyn_set_crush(2, 255) == 255 && dsu.k[2].crush == 255u, "crush 255 round-trips");
    check(sloop_core_dsyn_set_crush(2, 256) == 255 && dsu.k[2].crush == 255u, "crush 256 clamps to 255");
    check(sloop_core_dsyn_set_crush(2, 300) == 255, "crush 300 clamps to 255");
    check(sloop_core_dsyn_set_crush(2, -5) == 0 && dsu.k[2].crush == 0u, "crush -5 clamps to 0");
    {
        uint8_t lanes[sizeof dsu.k[2].s];
        char nm[8];
        memcpy(lanes, dsu.k[2].s, sizeof lanes);
        memcpy(nm, dsu.k[2].name, sizeof nm); /* name 是 8 字节，第 9 字节是 crush 本身 */
        check(sloop_core_dsyn_set_crush(2, 0x5A) == 0x5A && dsu.k[2].crush == 0x5Au &&
                  memcmp(dsu.k[2].s, lanes, sizeof lanes) == 0 && memcmp(dsu.k[2].name, nm, sizeof nm) == 0 &&
                  dsu.k[2].src == 3u,
              "crush 0x5A: dsu_fix_kit keeps 16 lanes/name/src");
    }
    check(sloop_core_dsyn_set_crush(SLOOP_DSYN_KITS, 10) == SLOOP_DSYN_INVALID,
          "crush rejects kit 4");
    s_frozen = true;
    {
        int16_t frozen_ret = sloop_core_dsyn_set_crush(0, 10);
        s_frozen = false;
        check(frozen_ret == SLOOP_DSYN_INVALID, "crush frozen -> SLOOP_DSYN_INVALID");
    }
    {
        uint32_t l = engine_lock_calls, u = engine_unlock_calls;
        (void)sloop_core_dsyn_set_crush(0, 1);
        check(engine_lock_calls == l + 1u && engine_unlock_calls == u + 1u,
              "crush takes the engine lock once");
    }

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

    /* 文本：索引→ms/Hz、LEVEL dB、FLT_MODE 名；未覆盖的字段留给通用格式化。
     * Task 9 补齐全部索引字段：DECAY 类用 DECAY_K 反解（下表是**测试侧**独立实现，
     * 与 sound_tab5.c 的 decay_ms 同公式 `10254/(65536-k)`、钳 5..4000ms），
     * LEVEL 按上游注释「128 + 4 × dB」精确到 0.25 dB（v=255 → +31.75dB）。 */
    {
        char t[16];
        uint32_t want_ms[5] = {0};
        static const uint32_t idx[5] = {0, 32, 64, 96, 127};
        int mono = 1, same = 1;
        for (i = 0; i < 5u; i++) {
            uint32_t d = 65536u - DECAY_K[idx[i] & 127u];
            uint32_t ms = 10254u / (d == 0u ? 1u : d);
            if (ms < 5u) ms = 5u;
            if (ms > 4000u) ms = 4000u;
            want_ms[i] = ms;
            if (i > 0u && want_ms[i] <= want_ms[i - 1u]) mono = 0;
        }
        check(mono, "text DECAY ms strictly increases with the DECAY_K index");
        for (i = 0; i < 5u; i++) {
            char want[16];
            if (want_ms[i] < 1000u) snprintf(want, sizeof want, "%ums", (unsigned)want_ms[i]);
            else
                snprintf(want, sizeof want, "%u.%us", (unsigned)(want_ms[i] / 1000u),
                         (unsigned)((want_ms[i] % 1000u) / 100u));
            if (sloop_core_dsyn_text(SLOOP_DSYN_DECAY, (int16_t)idx[i], t, sizeof t) <= 0 ||
                strcmp(t, want) != 0)
                same = 0;
            /* BTIME/NDEC 是同一张表（上游都是 DECAY_K index），文本必须相同 */
            if (sloop_core_dsyn_text(SLOOP_DSYN_BTIME, (int16_t)idx[i], t, sizeof t) <= 0 ||
                strcmp(t, want) != 0)
                same = 0;
            if (sloop_core_dsyn_text(SLOOP_DSYN_NDEC, (int16_t)idx[i], t, sizeof t) <= 0 ||
                strcmp(t, want) != 0)
                same = 0;
        }
        check(same, "text DECAY/BTIME/NDEC = 10254/(65536-DECAY_K) ms at 0/32/64/96/127");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FLT_MODE, 5, t, sizeof t) > 0 && strcmp(t, "LP+ALL") == 0,
              "text FLT_MODE 5 = LP+ALL");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FLT_MODE, 4, t, sizeof t) > 0 && strcmp(t, "OFF") == 0,
              "text FLT_MODE 4 = OFF");
        {
            /* 8 个索引逐一对照（4 = 只有 DF_ALL 位，非法，读作 OFF） */
            static const char *const want_flt[8] = {"OFF", "LP", "BP", "HP", "OFF",
                                                    "LP+ALL", "BP+ALL", "HP+ALL"};
            int okf = 1;
            for (int f = 0; f < 8; f++) {
                if (sloop_core_dsyn_text(SLOOP_DSYN_FLT_MODE, (int16_t)f, t, sizeof t) <= 0 ||
                    strcmp(t, want_flt[f]) != 0)
                    okf = 0;
            }
            check(okf, "text FLT_MODE 0..7 = OFF/LP/BP/HP/OFF/LP+ALL/BP+ALL/HP+ALL");
            check(sloop_core_dsyn_set(0, 0, SLOOP_DSYN_FLT_MODE, 4) == 0,
                  "FLT_MODE 4 (ALL alone) normalises to OFF on write");
        }
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 128, t, sizeof t) > 0 && strcmp(t, "0.0dB") == 0,
              "text LEVEL 128 = 0.0dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 152, t, sizeof t) > 0 && strcmp(t, "+6.0dB") == 0,
              "text LEVEL 152 = +6.0dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 0, t, sizeof t) > 0 && strcmp(t, "-32.0dB") == 0,
              "text LEVEL 0 = -32.0dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 255, t, sizeof t) > 0 &&
                  strcmp(t, "+31.75dB") == 0,
              "text LEVEL 255 = +31.75dB (quarter dB steps)");
        check(sloop_core_dsyn_text(SLOOP_DSYN_LEVEL, 129, t, sizeof t) > 0 &&
                  strcmp(t, "+0.25dB") == 0,
              "text LEVEL 129 = +0.25dB");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FCUT, 0, t, sizeof t) > 0 && strcmp(t, "30Hz") == 0,
              "text FCUT 0 = 30Hz");
        check(sloop_core_dsyn_text(SLOOP_DSYN_FCUT, 127, t, sizeof t) > 0 &&
                  strcmp(t, "16.0kHz") == 0 && CUTOFF_HZ[127] == 16000u,
              "text FCUT 127 = CUTOFF_HZ[127] = 16.0kHz");
        check(sloop_core_dsyn_text(SLOOP_DSYN_HPF, 0, t, sizeof t) > 0 && strcmp(t, "30Hz") == 0,
              "text HPF 0 = CUTOFF_HZ[0] = 30Hz");
        check(sloop_core_dsyn_text(SLOOP_DSYN_CLAP, 1, t, sizeof t) == 0,
              "text: CLAP (onoff) defers to the knobs' generic ON/OFF");
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

/* ---------------------------------------------------------------- 采样槽（Task 4） --- */

static void sample_env_reset(void)
{
    memset(h_flash, 0xFF, sizeof h_flash);
    memset(h_samples, 0xFF, sizeof h_samples);
    memset(usr_nz, 0, sizeof usr_nz);
    memset(usr_zone, 0, sizeof usr_zone);
    /* 上传暂存是 sound_tab5.c 的 static：不显式关掉，后面的用例可能继承一个 open 上传 */
    s_smp_open = (uint8_t)SLOOP_STORE_SAMPLES;
    s_smp_hdr_n = 0;
    s_smp_erased = 0;
    s_frozen = false;
    h_store_ready = true;
    song.playing = 0;
    transport_req = 0;
}

/* 上游 smp_user_hdr_t：1 个合法 zone（off=0，n=2*data_len 采样 = len 字节 4bit ADPCM）。
 * bad：1 = zone n=0，2 = zone off 越过 data_len，3 = magic 不对。 */
static void sample_hdr(uint8_t hdr[480], const char *name, const uint8_t *data, uint32_t len,
                       uint32_t crc_delta, int bad)
{
    smp_user_hdr_t h;
    size_t nl = strlen(name);

    memset(&h, 0, sizeof h);
    h.magic = SMP_USER_MAGIC;
    h.version = 1;
    h.nz = 1;
    if (nl > sizeof h.name) nl = sizeof h.name;
    memcpy(h.name, name, nl);
    h.data_len = len;
    h.crc = st_crc32(data, len) + crc_delta;
    h.zone[0].off = 0;
    h.zone[0].n = len * 2u;
    h.zone[0].ls = 0;
    h.zone[0].le = h.zone[0].n - 1u;
    h.zone[0].rate = 1u << 16;
    h.zone[0].lo = 0;
    h.zone[0].hi = 127;
    if (bad == 1) h.zone[0].n = 0;
    if (bad == 2) h.zone[0].off = len;
    if (bad == 3) h.magic = 0x12345678u;
    memcpy(hdr, &h, sizeof h);
}

/* 一次完整上传：BEGIN → 480B header（4×120，console 的行上限）→ 数据按 256B 块 →
 * END。`send` 可给出与 CRC 不同的字节流（CRC 失败用例）。 */
static esp_err_t sample_upload(uint32_t slot, const char *name, const uint8_t *data, uint32_t len,
                               uint32_t crc_delta, int bad_hdr, const uint8_t *send, int do_end)
{
    uint8_t hdr[480];
    const uint8_t *src = send != NULL ? send : data;
    esp_err_t ret;
    uint32_t at;

    sample_hdr(hdr, name, data, len, crc_delta, bad_hdr);
    ret = sloop_store_sample_begin(slot);
    if (ret != ESP_OK) return ret;
    for (at = 0; at < (uint32_t)sizeof hdr; at += 120) {
        uint32_t n = (uint32_t)sizeof hdr - at < 120u ? (uint32_t)sizeof hdr - at : 120u;
        ret = sloop_store_sample_hdr_append(hdr + at, n);
        if (ret != ESP_OK) return ret;
    }
    for (at = 0; at < len; at += 256) {
        uint32_t n = len - at < 256u ? len - at : 256u;
        ret = sloop_store_sample_write(slot, 512u + at, src + at, n);
        if (ret != ESP_OK) return ret;
    }
    return do_end ? sloop_store_sample_end(slot) : ESP_OK;
}

static int sample_mirror_is_flash(uint32_t slot)
{
    return memcmp(h_samples + slot * H_SAMPLE_SIZE, h_flash + (SMP_USER_OFF(slot) - H_FLASH_BASE),
                  H_SAMPLE_SIZE) == 0;
}

static int sample_flash_hdr_ff(uint32_t slot)
{
    uint8_t hdr[480];
    uint32_t i;
    if (st_read(SMP_USER_OFF(slot), hdr, sizeof hdr) != 0) return 0;
    for (i = 0; i < sizeof hdr; i++)
        if (hdr[i] != 0xFF) return 0;
    return 1;
}

static void test_sample_slot(void)
{
    static uint8_t data[1024], hdr[480];
    sloop_store_sample_info_t info;
    uint32_t base, i, l, u, e, x;
    int ok;

    sample_env_reset();
    base = SMP_USER_OFF(0);

    /* smp_user_scan 把 zone 的 off 重定基到 SMP_DATA（32 位偏移）：镜像必须在它之上且可达
     * （上游 tests/slice_test.c 同口径），否则扫描出的 zone 指不到镜像。 */
    check((uintptr_t)h_samples > (uintptr_t)SMP_DATA &&
              (uintptr_t)h_samples - (uintptr_t)SMP_DATA <= 0xF0000000u,
          "sample mirror addressable from SMP_DATA (32-bit zone rebase)");
    check(sizeof(smp_user_hdr_t) == 480u && SLOOP_STORE_SAMPLE_DATA == 512u, "header is 480B, data at +512");

    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 37u + 11u);

    /* 空槽 */
    memset(&info, 0xAA, sizeof info);
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 0u && info.zones == 0u,
          "info: an untouched slot is empty");

    /* 没有 open slot 时：没有可写的目标（先于任何 begin） */
    memset(hdr, 0xFF, sizeof hdr);
    check(sloop_store_sample_hdr_append(hdr, 4) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_write(0, 512, data, 256) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_end(0) == ESP_ERR_INVALID_STATE,
          "hdr_append/write/end without begin -> state");

    /* 合法上传 */
    check(sample_upload(0, "TONE", data, sizeof data, 0, 0, NULL, 1) == ESP_OK,
          "upload: begin/hdr/write/end");
    memset(&info, 0, sizeof info);
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 1u && info.zones == 1u &&
              strcmp(info.name, "TONE") == 0 && info.data_kb == 1u,
          "info: used/name/zones/KB after END");
    check(usr_nz[0] == 1u && usr_zone[0][0].n == 2048u && usr_zone[0][0].off != 0u,
          "end scans the slot (nz=1, n=2048, zone off rebased)");
    check(sample_mirror_is_flash(0), "mirror == flash after END (whole 80KiB slot)");

    /* reload = 开机装载路径：flash 是权威副本 */
    memset(h_samples, 0x5A, H_SAMPLE_SIZE);
    memset(usr_nz, 0, sizeof usr_nz);
    sloop_store_sample_reload(0);
    check(sample_mirror_is_flash(0) && usr_nz[0] == 1u, "reload: flash -> mirror + scan (boot path)");
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 1u, "reload keeps the slot visible");

    /* BEGIN 不 END，在**已填充**的槽上：镜像 header 0xFF、zone/nz 清空、flash header 扇区被擦
     * （在空槽上做同样断言是空洞的：0xFF/0 本来就成立） */
    check(sloop_store_sample_begin(0) == ESP_OK, "begin(0) again on a filled slot");
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 0u && h_samples[0] == 0xFF &&
              h_samples[479] == 0xFF && usr_nz[0] == 0u,
          "re-begin without end: info empty, mirror header 0xFF, nz cleared");
    check(sample_flash_hdr_ff(0), "re-begin erases the header sector of a filled slot");
    /* 恢复：同一槽再上传一次，读回一份完整采样 */
    check(sample_upload(0, "TONE2", data, sizeof data, 0, 0, NULL, 1) == ESP_OK,
          "recovery upload after an interrupted begin");
    memset(&info, 0, sizeof info);
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 1u && strcmp(info.name, "TONE2") == 0 &&
              usr_nz[0] == 1u && sample_mirror_is_flash(0),
          "recovered: new header, nz rescanned, mirror == flash");

    /* 写边界：off ≥ 512、256 对齐、off+n ≤ 0x14000、n ≤ 256、必须是 open slot */
    check(sloop_store_sample_begin(1) == ESP_OK, "begin(1) for boundary cases");
    check(sloop_store_sample_write(1, 0, data, 256) == ESP_ERR_INVALID_SIZE, "write: off < 512 -> size");
    check(sloop_store_sample_write(1, 513, data, 256) == ESP_ERR_INVALID_SIZE,
          "write: off not 256-aligned -> size");
    check(sloop_store_sample_write(1, 0x13F80u, data, 256) == ESP_ERR_INVALID_SIZE,
          "write: off + n past the slot end -> size");
    check(sloop_store_sample_write(1, 512, data, 257) == ESP_ERR_INVALID_SIZE, "write: n > 256 -> size");
    check(sloop_store_sample_write(2, 512, data, 256) == ESP_ERR_INVALID_STATE,
          "write: another slot than the open one -> state");
    check(sloop_store_sample_write(1, 0x13F00u, data, 256) == ESP_OK,
          "write: the last 256B of the slot is accepted");
    check(sloop_store_sample_hdr_append(hdr, 256) == ESP_OK &&
              sloop_store_sample_hdr_append(hdr, 256) == ESP_ERR_INVALID_SIZE,
          "hdr_append: past 480B -> size");
    check(sloop_store_sample_end(1) == ESP_ERR_INVALID_ARG, "end: short header -> invalid arg");
    check(sloop_store_sample_info(1, &info) == ESP_OK && info.used == 0u && sample_flash_hdr_ff(1),
          "a refused end leaves slot 1 empty in the mirror and flash");

    /* CRC 坏：header 的 CRC 与写进去的数据不一致 */
    check(sample_upload(1, "BAD", data, sizeof data, 1, 0, NULL, 1) == ESP_ERR_INVALID_CRC,
          "upload with a wrong CRC -> INVALID_CRC");
    check(sloop_store_sample_info(1, &info) == ESP_OK && info.used == 0u && sample_flash_hdr_ff(1),
          "CRC failure: slot stays empty, no header committed");

    /* 非法 header：zone n=0 / zone 越过 data / magic */
    check(sample_upload(2, "Z0", data, sizeof data, 0, 1, NULL, 1) == ESP_ERR_INVALID_ARG,
          "zone n=0 -> invalid arg");
    check(sloop_store_sample_info(2, &info) == ESP_OK && info.used == 0u && sample_flash_hdr_ff(2),
          "zone n=0: slot stays empty");
    check(sample_upload(2, "Z1", data, sizeof data, 0, 2, NULL, 1) == ESP_ERR_INVALID_ARG,
          "zone off past data_len -> invalid arg");
    check(sample_upload(2, "Z2", data, sizeof data, 0, 3, NULL, 1) == ESP_ERR_INVALID_ARG,
          "wrong magic -> invalid arg");

    /* 走带中拒绝（spec §3.5 的 Tab5 安全差异）：hdr_append 也在内；读仍可用。
     * 停走带后同一个 hdr_append 必须成功——证明拦下它的就是走带门，而不是别的错误。 */
    check(sloop_store_sample_begin(1) == ESP_OK, "begin(1) before the playing cases");
    song.playing = 1;
    check(sloop_store_sample_begin(0) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_erase(0) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_hdr_append(hdr, 4) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_write(1, 512, data, 256) == ESP_ERR_INVALID_STATE,
          "begin/erase/hdr_append/write while playing -> state");
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 1u,
          "info is readable while playing (and the slot survived)");
    song.playing = 0;
    check(sloop_store_sample_hdr_append(hdr, 4) == ESP_OK,
          "hdr_append works again after STOP (control: the gate was the transport)");
    transport_req = 1;
    check(sloop_store_sample_begin(0) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_hdr_append(hdr, 4) == ESP_ERR_INVALID_STATE,
          "begin/hdr_append with a transport request -> state");
    transport_req = 0;

    /* 诊断冻结：写/擦/累积拒绝，读不受影响 */
    s_frozen = true;
    check(sloop_store_sample_begin(0) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_erase(0) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_hdr_append(hdr, 4) == ESP_ERR_INVALID_STATE,
          "begin/erase/hdr_append while frozen -> state");
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 1u, "frozen does not block info");
    s_frozen = false;

    /* 存储未就绪 */
    h_store_ready = false;
    check(sloop_store_sample_info(0, &info) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_begin(0) == ESP_ERR_INVALID_STATE &&
              sloop_store_sample_hdr_append(hdr, 4) == ESP_ERR_INVALID_STATE,
          "not ready -> state");
    h_store_ready = true;

    /* ERASE：镜像与 flash 整槽 0xFF，扫描后槽为空 */
    check(sloop_store_sample_erase(0) == ESP_OK, "erase(0)");
    memset(&info, 0, sizeof info);
    check(sloop_store_sample_info(0, &info) == ESP_OK && info.used == 0u && usr_nz[0] == 0u,
          "erase: info empty, nz cleared");
    ok = 1;
    for (i = 0; i < H_SAMPLE_SIZE; i++) {
        if (h_samples[i] != 0xFF) ok = 0;
        if (h_flash[base - H_FLASH_BASE + i] != 0xFF) ok = 0;
    }
    check(ok, "erase: mirror and flash slot both 0xFF");

    /* USR4（槽 3）的基址是 0xE7000，不是 0xA0000 + k*0x14000：单独走一遍完整往返 */
    check(sample_upload(3, "USR4", data, sizeof data, 0, 0, NULL, 1) == ESP_OK,
          "upload to slot 3 (USR4 @ 0xE7000)");
    memset(&info, 0, sizeof info);
    check(sloop_store_sample_info(3, &info) == ESP_OK && info.used == 1u && strcmp(info.name, "USR4") == 0 &&
              usr_nz[3] == 1u && sample_mirror_is_flash(3),
          "slot 3: info, scan and mirror == flash");
    check(sloop_store_sample_erase(3) == ESP_OK &&
              sloop_store_sample_info(3, &info) == ESP_OK && info.used == 0u && usr_nz[3] == 0u,
          "erase(3)");

    /* 参数越界 */
    check(sloop_store_sample_info(4, &info) == ESP_ERR_INVALID_ARG &&
              sloop_store_sample_begin(4) == ESP_ERR_INVALID_ARG &&
              sloop_store_sample_erase(4) == ESP_ERR_INVALID_ARG &&
              sloop_store_sample_write(4, 512, data, 256) == ESP_ERR_INVALID_ARG &&
              sloop_store_sample_info(0, NULL) == ESP_ERR_INVALID_ARG,
          "slot >= 4 / NULL out -> invalid arg");

    /* 锁的次数：写入口各取一次存储锁；只有会 `smp_user_scan` 的动作另取一次引擎锁
     * （smp_user_scan 的保护：设备上 fm1_irq_off 是空壳）；hdr_append/info/write 不取引擎锁 */
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    x = engine_unlock_calls;
    (void)sloop_store_sample_begin(0);
    check(store_lock_calls == l + 1u && store_unlock_calls == u + 1u && engine_lock_calls == e + 1u &&
              engine_unlock_calls == x + 1u,
          "begin: one store + one engine lock");
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    (void)sloop_store_sample_hdr_append(hdr, 4);
    check(store_lock_calls == l + 1u && store_unlock_calls == u + 1u && engine_lock_calls == e,
          "hdr_append: one store lock, no engine lock");
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    (void)sloop_store_sample_info(0, &info);
    check(store_lock_calls == l + 1u && store_unlock_calls == u + 1u && engine_lock_calls == e,
          "info: one store lock (no torn header), no engine lock");
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    (void)sloop_store_sample_write(0, 512, data, 256);
    check(store_lock_calls == l + 1u && store_unlock_calls == u + 1u && engine_lock_calls == e,
          "write: one store lock, no engine lock");
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    x = engine_unlock_calls;
    (void)sloop_store_sample_erase(0);
    check(store_lock_calls == l + 1u && store_unlock_calls == u + 1u && engine_lock_calls == e + 1u &&
              engine_unlock_calls == x + 1u,
          "erase: one store + one engine lock");
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    x = engine_unlock_calls;
    sloop_store_sample_reload(0);
    check(store_lock_calls == l + 1u && store_unlock_calls == u + 1u && engine_lock_calls == e + 1u &&
              engine_unlock_calls == x + 1u,
          "reload: one store + one engine lock");
    /* END 的成功提交：一次存储锁 + 一次扫描（header 回读失败也照此收口） */
    sample_hdr(hdr, "ONE", data, 256, 0, 0);
    check(sloop_store_sample_begin(2) == ESP_OK && sloop_store_sample_hdr_append(hdr, 480) == ESP_OK &&
              sloop_store_sample_write(2, 512, data, 256) == ESP_OK,
          "mini upload to slot 2 for the end lock count");
    l = store_lock_calls;
    u = store_unlock_calls;
    e = engine_lock_calls;
    x = engine_unlock_calls;
    check(sloop_store_sample_end(2) == ESP_OK && store_lock_calls == l + 1u && store_unlock_calls == u + 1u &&
              engine_lock_calls == e + 1u && engine_unlock_calls == x + 1u,
          "end: one store + one engine lock (commit)");

    sample_env_reset();
}

/* ---------------------------------------------------------- 描述符（P4-F Task 3）---
 * sloop_core_desc_track_live：随轨道状态取上游描述符。期望值逐项硬编码自上游源码
 * （params.c 的 INS_DESC/ESYNC_DESC、eng_lofi.c 的 LOFI_WAVNUM/LOFI_DCY、eng_noise.c 的
 * NOISE_LONG/NOISE_META/NOISE_CLK、eng_phys.c 的 PHYS_D_*），不取自被测函数的输出。 */
#define T_P_E0 P_E0 /* 上游引擎槽起点（= SLOOP_CORE_P_COUNT - 8） */

static int desc_is(const sloop_desc_t *d, const char *label, int fmt, int min, int max, int def,
                   const char *unit, unsigned enums, const char *first, const char *last)
{
    static const char dash[] = "-";
    const char *u = d->unit != NULL ? d->unit : dash;
    const char *f = d->enum_count > 0u ? d->enums[0] : dash;
    const char *l = d->enum_count > 0u ? d->enums[d->enum_count - 1u] : dash;
    return d->label != NULL && strcmp(d->label, label) == 0 && (int)d->fmt == fmt &&
           d->min == min && d->max == max && d->def == def && strcmp(u, unit) == 0 &&
           d->enum_count == enums && strcmp(f, first) == 0 && strcmp(l, last) == 0;
}

/* 与静态表投影逐字段相等（live 在无状态依赖时不得偏离 sloop_core_desc） */
static int desc_same_as_base(const sloop_desc_t *d, sloop_desc_scope_t scope, uint32_t engine, uint32_t id)
{
    sloop_desc_t b;
    return sloop_core_desc(scope, engine, id, &b) && d->label == b.label && d->unit == b.unit &&
           d->fmt == b.fmt && d->min == b.min && d->max == b.max && d->def == b.def &&
           d->enums == b.enums && d->enum_count == b.enum_count;
}

/* 拒绝：返回 false 且 out 一字节不动（哨兵 memcmp） */
static int desc_rejects(uint8_t track, uint32_t id)
{
    sloop_desc_t d;
    memset(&d, 0xA5, sizeof(d));
    if (sloop_core_desc_track_live(track, id, &d)) return 0;
    for (uint32_t i = 0; i < sizeof(d); i++)
        if (((const uint8_t *)&d)[i] != 0xA5u) return 0;
    return 1;
}

static int live_is(uint8_t track, uint32_t id, const char *label, int fmt, int min, int max, int def,
                   const char *unit, unsigned enums, const char *first, const char *last)
{
    sloop_desc_t d;
    memset(&d, 0, sizeof(d));
    return sloop_core_desc_track_live(track, id, &d) &&
           desc_is(&d, label, fmt, min, max, def, unit, enums, first, last);
}

static void test_desc_live(void)
{
    /* engines.c ENGINES[] 的序（eng_lofi.c 是第 4 个引擎；ENGI_PHYS/ENGI_NOISE 见 core.h） */
    enum { ENG_ANALOG_ = 0, ENG_LOFI_ = 3, ENG_PHYS_ = ENGI_PHYS, ENG_NOISE_ = ENGI_NOISE };
    const int F_INT_ = SLOOP_DESC_FMT_INT, F_PCT_ = SLOOP_DESC_FMT_PCT, F_TIME_ = SLOOP_DESC_FMT_TIME,
              F_LFOHZ_ = SLOOP_DESC_FMT_LFOHZ, F_CUTOFF_ = SLOOP_DESC_FMT_CUTOFF,
              F_DB_ = SLOOP_DESC_FMT_DB, F_ENUM_ = SLOOP_DESC_FMT_ENUM;
    const char dash[] = "-";
    sloop_desc_t d;
    uint8_t save_req = trk[0].eng_req;
    int16_t save_itype = trk[0].p[P_ITYPE], save_esync = trk[0].p[P_ESYNC], save_e0 = trk[0].p[P_E0],
            save_e1 = trk[0].p[P_E1];
    uint32_t k, type;
    int ok;

    /* 无状态依赖的轨参数与引擎槽 = 静态表（sloop_core_desc 的 TRACK/ENGINE 投影） */
    ok = 1;
    trk[0].eng_req = (uint8_t)ENG_ANALOG_;
    trk[0].p[P_ITYPE] = 0;
    trk[0].p[P_ESYNC] = 0;
    for (k = 0; k < T_P_E0; k++)
        ok = ok && sloop_core_desc_track_live(0, k, &d) && desc_same_as_base(&d, SLOOP_DESC_TRACK, 0, k);
    for (k = 0; k < 8u; k++)
        ok = ok && sloop_core_desc_track_live(0, T_P_E0 + k, &d) && desc_same_as_base(&d, SLOOP_DESC_ENGINE, ENG_ANALOG_, k);
    check(ok, "desc live: ANALOG track 0, every id == static TP[]/edit[] projection");
    check(sloop_core_desc_track_live(0, 0, NULL), "desc live: out == NULL is allowed (true, nothing written)");

    /* (a) 引擎钩子：LOFI——WAVE=WRAM(4) 时 edit[2] DUTY→WAV#（16 个 wave RAM 名），
     * CHIP=STEP(4) 时 edit[3] CRSH→DCY（32 个包络名）；其余槽与不命中时落回 edit[] */
    trk[0].eng_req = (uint8_t)ENG_LOFI_;
    trk[0].p[T_P_E0] = 0;
    trk[0].p[T_P_E0 + 1u] = 0;
    check(live_is(0, T_P_E0 + 2u, "DUTY", F_INT_, 0, 127, 64, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 3u, "CRSH", F_PCT_, 0, 127, 0, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 0u, "CHIP", F_ENUM_, 0, 4, 0, dash, 5, "4BIT", "STEP"),
          "desc live: LOFI default mode keeps edit[] (DUTY/CRSH/CHIP)");
    trk[0].p[T_P_E0 + 1u] = 4; /* RW_WRAM */
    check(live_is(0, T_P_E0 + 2u, "WAV#", F_INT_, 0, 127, 64, dash, 16, "SQ50", "STAIR") &&
              live_is(0, T_P_E0 + 3u, "CRSH", F_PCT_, 0, 127, 0, dash, 0, dash, dash),
          "desc live: LOFI WAVE=WRAM: edit[2] DUTY -> WAV# (N_WRAM), edit[3] untouched");
    trk[0].p[T_P_E0] = 4; /* CHIP_STEP */
    check(live_is(0, T_P_E0 + 3u, "DCY", F_INT_, 0, 127, 0, dash, 32, "CONST", "L15"),
          "desc live: LOFI CHIP=STEP: edit[3] CRSH -> DCY (N_DCY)");

    /* (a) NOISE——MODE>=LFSR(2)：edit[5] DENS→LEN（LFSR 8 个寄存器长 / META 8 个序列），
     * edit[7] CRSH→CLK（单位 st）；ANLG/DUST 落回 edit[] */
    trk[0].eng_req = (uint8_t)ENG_NOISE_;
    trk[0].p[T_P_E0] = 0;
    check(live_is(0, T_P_E0 + 5u, "DENS", F_PCT_, 0, 127, 0, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 7u, "CRSH", F_PCT_, 0, 127, 0, dash, 0, dash, dash),
          "desc live: NOISE ANLG keeps edit[] (DENS/CRSH)");
    trk[0].p[T_P_E0] = 1; /* DUST */
    check(live_is(0, T_P_E0 + 5u, "DENS", F_PCT_, 0, 127, 0, dash, 0, dash, dash),
          "desc live: NOISE DUST keeps edit[] (DENS)");
    trk[0].p[T_P_E0] = 2; /* LFSR */
    check(live_is(0, T_P_E0 + 5u, "LEN", F_INT_, 0, 127, 0, dash, 8, "L23", "L7") &&
              live_is(0, T_P_E0 + 7u, "CLK", F_INT_, 0, 127, 0, "st", 0, dash, dash),
          "desc live: NOISE LFSR: DENS -> LEN (N_NOISE_LONG), CRSH -> CLK (unit st)");
    trk[0].p[T_P_E0] = 3; /* META */
    check(live_is(0, T_P_E0 + 5u, "LEN", F_INT_, 0, 127, 0, dash, 8, "127", "7") &&
              live_is(0, T_P_E0 + 7u, "CLK", F_INT_, 0, 127, 0, "st", 0, dash, dash),
          "desc live: NOISE META: DENS -> LEN (N_NOISE_META), CRSH -> CLK");

    /* (a) PHYS——edit[] = MODEL/STRC/BRIT/DAMP/POS/ACC/BOW/EXC；PHYS_DESC[SYMP(3)] 把槽 1/4/6
     * 换成 CHRD/SYMP/BUZZ（其余落回 edit[]），[MEMB(2)] 把槽 1/6 换成 HARM/BEND；
     * MODAL(0)：全部 edit[]。范围与默认值同 edit[] */
    trk[0].eng_req = (uint8_t)ENG_PHYS_;
    trk[0].p[T_P_E0] = 3; /* PM_SYMP */
    check(live_is(0, T_P_E0 + 1u, "CHRD", F_INT_, 0, 127, 64, dash, 8, "OCT", "ROOT") &&
              live_is(0, T_P_E0 + 4u, "SYMP", F_PCT_, 0, 127, 20, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 6u, "BUZZ", F_PCT_, 0, 127, 0, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 2u, "BRIT", F_PCT_, 0, 127, 80, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 0u, "MODEL", F_ENUM_, 0, 3, 0, dash, 4, "MODAL", "SYMP"),
          "desc live: PHYS SYMP: slot1->CHRD, slot4->SYMP, slot6->BUZZ (BRIT/MODEL from edit[])");
    trk[0].p[T_P_E0] = 2; /* PM_MEMB */
    check(live_is(0, T_P_E0 + 1u, "HARM", F_PCT_, 0, 127, 64, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 6u, "BEND", F_PCT_, 0, 127, 0, dash, 0, dash, dash) &&
              live_is(0, T_P_E0 + 4u, "POS", F_PCT_, 0, 127, 20, dash, 0, dash, dash),
          "desc live: PHYS MEMB: slot1->HARM, slot6->BEND (POS from edit[])");
    trk[0].p[T_P_E0] = 0; /* PM_MODAL */
    ok = 1;
    for (k = 0; k < 8u; k++)
        ok = ok && sloop_core_desc_track_live(0, T_P_E0 + k, &d) && desc_same_as_base(&d, SLOOP_DESC_ENGINE, ENG_PHYS_, k);
    check(ok, "desc live: PHYS MODAL: every slot == edit[]");

    /* (b) INSERT：P_IA/P_IB/P_IC 的标签/格式随 P_ITYPE；min/max 恒等于 TP[P_IA..P_IC]（0..127） */
    trk[0].eng_req = (uint8_t)ENG_ANALOG_;
    ok = 1;
    for (type = 0; type <= 9u; type++) { /* 0..8 = N_P_ITYPE，9 = 越界类型（上游落回 TP） */
        trk[0].p[P_ITYPE] = (int16_t)type;
        for (k = 0; k < 3u; k++) {
            if (!sloop_core_desc_track_live(0, P_IA + k, &d)) { ok = 0; continue; }
            ok = ok && d.min == TP[P_IA + k].min && d.max == TP[P_IA + k].max;
        }
    }
    check(ok, "desc live: INSERT A/B/C min/max == TP[] for every P_ITYPE (incl. out-of-range)");
    trk[0].p[P_ITYPE] = 0; /* OFF */
    check(live_is(0, P_IA, "INS A", F_PCT_, 0, 127, 64, dash, 0, dash, dash) &&
              live_is(0, P_IB, "INS B", F_PCT_, 0, 127, 96, dash, 0, dash, dash) &&
              live_is(0, P_IC, "INS C", F_PCT_, 0, 127, 96, dash, 0, dash, dash),
          "desc live: INSERT OFF keeps TP[] (INS A/B/C)");
    trk[0].p[P_ITYPE] = 1; /* SOFT */
    check(live_is(0, P_IA, "DRIVE", F_PCT_, 0, 127, 64, dash, 0, dash, dash) &&
              live_is(0, P_IB, "TONE", F_CUTOFF_, 0, 127, 96, dash, 0, dash, dash) &&
              live_is(0, P_IC, "LEVEL", F_DB_, 0, 127, 96, dash, 0, dash, dash),
          "desc live: INSERT SOFT: DRIVE/TONE/LEVEL");
    trk[0].p[P_ITYPE] = 4; /* FUZZ：同一张表 */
    check(live_is(0, P_IA, "DRIVE", F_PCT_, 0, 127, 64, dash, 0, dash, dash),
          "desc live: INSERT FUZZ: same table as SOFT (DRIVE)");
    trk[0].p[P_ITYPE] = 5; /* CRUSH */
    check(live_is(0, P_IA, "BITS", F_INT_, 0, 127, 64, dash, 16, "1", "16") &&
              live_is(0, P_IB, "RATE", F_INT_, 0, 127, 96, "Hz", 16, "689", "44k") &&
              live_is(0, P_IC, "LPF", F_CUTOFF_, 0, 127, 96, dash, 0, dash, dash),
          "desc live: INSERT CRUSH: BITS/RATE(Hz)/LPF");
    trk[0].p[P_ITYPE] = 6; /* PHASR */
    check(live_is(0, P_IA, "RATE", F_LFOHZ_, 0, 127, 64, dash, 0, dash, dash) &&
              live_is(0, P_IB, "DEPTH", F_PCT_, 0, 127, 96, dash, 0, dash, dash) &&
              live_is(0, P_IC, "FDBK", F_PCT_, 0, 127, 96, dash, 0, dash, dash),
          "desc live: INSERT PHASR: RATE/DEPTH/FDBK");
    trk[0].p[P_ITYPE] = 8; /* CHOR */
    check(live_is(0, P_IA, "RATE", F_LFOHZ_, 0, 127, 64, dash, 0, dash, dash) &&
              live_is(0, P_IC, "FDBK", F_PCT_, 0, 127, 96, dash, 0, dash, dash),
          "desc live: INSERT CHOR: same table as PHASR (RATE/FDBK)");

    /* (c) ENV SYNC：P_ESYNC 开 ⇒ ATK/DEC/REL 换成音符名表（F_INT，25 名，范围/默认值不变）；
     * SUS 与其它参数不受影响；静态 sloop_core_desc 不随状态变 */
    trk[0].p[P_ITYPE] = 0;
    trk[0].p[P_ESYNC] = 0;
    check(live_is(0, P_ATK, "ATK", F_TIME_, 0, 127, 10, dash, 0, dash, dash) &&
              live_is(0, P_DEC, "DEC", F_TIME_, 0, 127, 70, dash, 0, dash, dash) &&
              live_is(0, P_REL, "REL", F_TIME_, 0, 127, 60, dash, 0, dash, dash),
          "desc live: ENV SYNC off: ATK/DEC/REL == TP[] (F_TIME)");
    trk[0].p[P_ESYNC] = 1;
    check(live_is(0, P_ATK, "ATK", F_INT_, 0, 127, 10, dash, 25, "0", "4BAR") &&
              live_is(0, P_DEC, "DEC", F_INT_, 0, 127, 70, dash, 25, "0", "4BAR") &&
              live_is(0, P_REL, "REL", F_INT_, 0, 127, 60, dash, 25, "0", "4BAR"),
          "desc live: ENV SYNC on: ATK/DEC/REL -> F_INT + ESYNC_NAMES, range/def as TP[]");
    ok = sloop_core_desc_track_live(0, P_SUS, &d) && desc_same_as_base(&d, SLOOP_DESC_TRACK, 0, P_SUS) &&
         sloop_core_desc_track_live(0, 0, &d) && desc_same_as_base(&d, SLOOP_DESC_TRACK, 0, 0);
    check(ok, "desc live: ENV SYNC on leaves SUS/LEVEL on TP[]");
    ok = sloop_core_desc(SLOOP_DESC_TRACK, 0, P_ATK, &d) && d.fmt == SLOOP_DESC_FMT_TIME && d.enum_count == 0u &&
         d.min == TP[P_ATK].min && d.max == TP[P_ATK].max;
    check(ok, "desc live: static sloop_core_desc stays F_TIME while ENV SYNC on");
    trk[0].p[P_ESYNC] = 0;

    /* (d) 鼓轨：P_E0 = kit 描述符（= ENGINE 哨兵 12 id 0），P_E1..P_E7 无描述符 */
    check(sloop_core_desc_track_live(3, T_P_E0, &d) && desc_same_as_base(&d, SLOOP_DESC_ENGINE, SLOOP_CORE_NENGINES, 0) &&
              desc_is(&d, "KIT", F_ENUM_, 0, 45, 0, dash, 46, "ACOUSTIC", "SYN4"),
          "desc live: drum track P_E0 == DRUM_KIT_DESC (KIT, 46 kits)");
    ok = 1;
    for (k = 1; k < 8u; k++)
        ok = ok && desc_rejects(3, T_P_E0 + k);
    check(ok, "desc live: drum track P_E1..P_E7 rejected, out untouched");

    /* (e) 越界：轨号 ≥ 4、id ≥ P_COUNT 回 false 且 out 不动；合法 id P_E7 可读 */
    ok = desc_rejects(4, 0) && desc_rejects(4, T_P_E0) && desc_rejects(255, P_ATK) &&
         desc_rejects(0, SLOOP_CORE_P_COUNT) && desc_rejects(0, 255) && desc_rejects(3, SLOOP_CORE_P_COUNT);
    check(ok, "desc live: track >= 4 / id >= P_COUNT rejected, out untouched");
    check(sloop_core_desc_track_live(0, SLOOP_CORE_P_COUNT - 1u, &d) &&
              desc_same_as_base(&d, SLOOP_DESC_ENGINE, ENG_ANALOG_, 7),
          "desc live: id P_E7 (last) readable on a synth track");

    trk[0].eng_req = save_req;
    trk[0].p[P_ITYPE] = save_itype;
    trk[0].p[P_ESYNC] = save_esync;
    trk[0].p[P_E0] = save_e0;
    trk[0].p[P_E1] = save_e1;
}

int main(void)
{
    test_dsyn_fields();
    test_fm6_fields();
    test_sample_slot();
    test_desc_live();
    if (fails) {
        printf("SYNTH HOST TEST FAILED (%d failure%s)\n", fails, fails == 1 ? "" : "s");
        return 1;
    }
    printf("SYNTH HOST TEST PASSED\n");
    return 0;
}
