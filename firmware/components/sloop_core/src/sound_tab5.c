/* P4-D Task 2：上游 drum_synth.c / editor_dsyn.c 的 DSYN（SYN1..SYN4）薄适配。
 * 由 selftest.c 末尾 include（在 seq_tab5.c 之后），因此上游 statics
 * （dsu/dsu_kit/dsu_from_factory/dsu_fix_sound/dsu_fix_kit/dsu_dirty/dsu_aud_k/lane/vel）
 * 与生成表（DS_KITS/DECAY_K/CUTOFF_HZ、params.c 的 N_NOTE）都可见。数据布局与钳位方向
 * 以上游为准；虚拟字段拆分见 sloop_core.h 的 DSYN 段与 spec §4.1。 */
#include "sloop_core.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(dsnd_t) == 22u, "dsnd_t 与公开的 sound[22] 不同尺寸");

/* 虚拟字段的读取种类：整字节（无符号/有符号）、src 低 4 位 / bit4（CLAP）、
 * flt 的低 3 位 / 高 5 位。 */
enum { DSYN_K_U8, DSYN_K_S8, DSYN_K_SRC, DSYN_K_CLAP, DSYN_K_FLT_MODE, DSYN_K_FLT_RES };
typedef struct {
    const char *label;
    uint8_t fmt, kind, off;   /* off = 22 字节 sound[] 里的偏移（CLAP/FLT_* 指向组合字节） */
    int16_t min, max;
} dsyn_desc_t;

static const char *const DSYN_N_WAVE[] = {"NONE", "SINE", "TRI", "SQR", "FM", "BELL"};
static const char *const DSYN_N_SRC[] = {"NONE", "WHITE", "METAL", "CYM", "CHIP"};
/* 上游 flt 的 7 个合法组合（DF_ALL 必须与 1..3 组合）。索引 4（只有 ALL 位）非法：
 * 读作 OFF，写 4 也被归一为 OFF（见 dsyn_put）。 */
static const char *const DSYN_N_FLT[] = {"OFF", "LP", "BP", "HP", "OFF", "LP+ALL", "BP+ALL", "HP+ALL"};

/* 顺序 = sloop_dsyn_field_t（spec §4.1 固定顺序，编号即协议）。 */
static const dsyn_desc_t DSYN_DESC[SLOOP_DSYN_FIELDS] = {
    [SLOOP_DSYN_WAVE] = {"WAVE", SLOOP_DESC_FMT_ENUM, DSYN_K_U8, 0, 0, 5},
    [SLOOP_DSYN_SRC] = {"SRC", SLOOP_DESC_FMT_ENUM, DSYN_K_SRC, 1, 0, 4},
    [SLOOP_DSYN_CLAP] = {"CLAP", SLOOP_DESC_FMT_ONOFF, DSYN_K_CLAP, 1, 0, 1},
    [SLOOP_DSYN_PITCH] = {"PITCH", SLOOP_DESC_FMT_INT, DSYN_K_U8, 2, 0, 127},
    [SLOOP_DSYN_FINE] = {"FINE", SLOOP_DESC_FMT_INT, DSYN_K_U8, 3, 0, 15},
    [SLOOP_DSYN_BEND] = {"BEND", SLOOP_DESC_FMT_INT, DSYN_K_U8, 4, 0, 96},
    [SLOOP_DSYN_BTIME] = {"BTIME", SLOOP_DESC_FMT_INT, DSYN_K_U8, 5, 0, 127},
    [SLOOP_DSYN_HOLD] = {"HOLD", SLOOP_DESC_FMT_INT, DSYN_K_U8, 6, 0, 127},
    [SLOOP_DSYN_DECAY] = {"DECAY", SLOOP_DESC_FMT_INT, DSYN_K_U8, 7, 0, 127},
    [SLOOP_DSYN_TLEV] = {"TLEV", SLOOP_DESC_FMT_INT, DSYN_K_U8, 8, 0, 127},
    [SLOOP_DSYN_T2] = {"T2", SLOOP_DESC_FMT_INT, DSYN_K_U8, 9, 0, 31},
    [SLOOP_DSYN_T2LEV] = {"T2LEV", SLOOP_DESC_FMT_INT, DSYN_K_U8, 10, 0, 127},
    [SLOOP_DSYN_CLICK] = {"CLICK", SLOOP_DESC_FMT_INT, DSYN_K_U8, 11, 0, 127},
    [SLOOP_DSYN_NLEV] = {"NLEV", SLOOP_DESC_FMT_INT, DSYN_K_U8, 12, 0, 127},
    [SLOOP_DSYN_NHOLD] = {"NHOLD", SLOOP_DESC_FMT_INT, DSYN_K_U8, 13, 0, 127},
    [SLOOP_DSYN_NDEC] = {"NDEC", SLOOP_DESC_FMT_INT, DSYN_K_U8, 14, 0, 127},
    [SLOOP_DSYN_FLT_MODE] = {"FLT_MODE", SLOOP_DESC_FMT_ENUM, DSYN_K_FLT_MODE, 15, 0, 7},
    [SLOOP_DSYN_FLT_RES] = {"FLT_RES", SLOOP_DESC_FMT_INT, DSYN_K_FLT_RES, 15, 0, 31},
    [SLOOP_DSYN_FCUT] = {"FCUT", SLOOP_DESC_FMT_CUTOFF, DSYN_K_U8, 16, 0, 127},
    [SLOOP_DSYN_FENV] = {"FENV", SLOOP_DESC_FMT_INT, DSYN_K_S8, 17, -128, 127},
    [SLOOP_DSYN_HPF] = {"HPF", SLOOP_DESC_FMT_CUTOFF, DSYN_K_U8, 18, 0, 127},
    [SLOOP_DSYN_CHIP] = {"CHIP", SLOOP_DESC_FMT_NOTE, DSYN_K_U8, 19, 0, 127},
    [SLOOP_DSYN_DRIVE] = {"DRIVE", SLOOP_DESC_FMT_INT, DSYN_K_U8, 20, 0, 127},
    [SLOOP_DSYN_LEVEL] = {"LEVEL", SLOOP_DESC_FMT_DB, DSYN_K_U8, 21, 0, 255},
};

static int16_t dsyn_get(const uint8_t b[22], uint32_t field)
{
    const dsyn_desc_t *d = &DSYN_DESC[field];
    switch (d->kind) {
    case DSYN_K_S8: return (int8_t)b[d->off];
    case DSYN_K_SRC: return (int16_t)(b[d->off] & 15u);
    case DSYN_K_CLAP: return (b[d->off] & 0x10u) ? 1 : 0;
    case DSYN_K_FLT_MODE: return (int16_t)(b[d->off] & 7u);
    case DSYN_K_FLT_RES: return (int16_t)(b[d->off] >> 3);
    default: return (int16_t)b[d->off];
    }
}

/* 按描述符范围钳位后写一个虚拟字段，只动它自己的位。返回写后回读值。 */
static int16_t dsyn_put(uint8_t b[22], uint32_t field, int16_t v)
{
    const dsyn_desc_t *d = &DSYN_DESC[field];
    int32_t x = clamp((int32_t)v, d->min, d->max);
    switch (d->kind) {
    case DSYN_K_S8: b[d->off] = (uint8_t)(int8_t)x; break;
    case DSYN_K_SRC: b[d->off] = (uint8_t)((b[d->off] & 0x10u) | (uint8_t)x); break;
    case DSYN_K_CLAP: b[d->off] = (uint8_t)((b[d->off] & (uint8_t)~0x10u) | (x ? 0x10u : 0u)); break;
    case DSYN_K_FLT_MODE:
        if (!(x & 3)) x &= 3;   /* 只有 DF_ALL 位（4）不是合法组合：归一为 OFF */
        b[d->off] = (uint8_t)((b[d->off] & (uint8_t)~7u) | (uint8_t)x);
        break;
    case DSYN_K_FLT_RES: b[d->off] = (uint8_t)((b[d->off] & 7u) | ((uint8_t)x << 3)); break;
    default: b[d->off] = (uint8_t)x; break;
    }
    return dsyn_get(b, field);
}

bool sloop_core_dsyn_desc(uint32_t field, sloop_desc_t *out)
{
    if (out == NULL || field >= SLOOP_DSYN_FIELDS)
        return false;
    const dsyn_desc_t *d = &DSYN_DESC[field];
    out->label = d->label;
    out->unit = NULL;
    out->fmt = d->fmt;
    out->min = d->min;
    out->max = d->max;
    out->def = 0;
    out->enums = NULL;
    out->enum_count = 0;
    if (field == SLOOP_DSYN_WAVE) {
        out->enums = DSYN_N_WAVE;
        out->enum_count = 6;
    } else if (field == SLOOP_DSYN_SRC) {
        out->enums = DSYN_N_SRC;
        out->enum_count = 5;
    } else if (field == SLOOP_DSYN_FLT_MODE) {
        out->enums = DSYN_N_FLT;
        out->enum_count = 8;
    }
    return true;
}

esp_err_t sloop_core_dsyn_read(uint32_t kit, uint32_t lane, sloop_dsyn_sound_t *out,
                               char name[9], uint8_t *crush, uint8_t *src)
{
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES)
        return ESP_ERR_INVALID_ARG;
    if (s_frozen)
        return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    (void)dsu_kit(kit);   /* 无 flash 时惰性落默认 kit（上游 editor_dsyn 入口同法） */
    if (out != NULL) {
        out->used = 1;
        memcpy(out->sound, &dsu.k[kit].s[lane], sizeof out->sound);
    }
    if (name != NULL) {
        memcpy(name, dsu.k[kit].name, SLOOP_DSYN_NAME);
        name[SLOOP_DSYN_NAME] = '\0';
    }
    if (crush != NULL)
        *crush = dsu.k[kit].crush;
    if (src != NULL)
        *src = dsu.k[kit].src;
    engine_unlock();
    return ESP_OK;
}

int16_t sloop_core_dsyn_field(uint32_t kit, uint32_t lane, uint32_t field)
{
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES || field >= SLOOP_DSYN_FIELDS || s_frozen)
        return SLOOP_DSYN_INVALID;
    engine_lock();
    int16_t ret = SLOOP_DSYN_INVALID;
    if (!s_frozen) {
        (void)dsu_kit(kit);
        ret = dsyn_get((const uint8_t *)&dsu.k[kit].s[lane], field);
    }
    engine_unlock();
    return ret;
}

int16_t sloop_core_dsyn_set(uint32_t kit, uint32_t lane, uint32_t field, int16_t v)
{
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES || field >= SLOOP_DSYN_FIELDS || s_frozen)
        return SLOOP_DSYN_INVALID;
    engine_lock();
    int16_t ret = SLOOP_DSYN_INVALID;
    if (!s_frozen) {
        (void)dsu_kit(kit);
        uint8_t b[sizeof(dsnd_t)];
        memcpy(b, &dsu.k[kit].s[lane], sizeof b);
        (void)dsyn_put(b, field, v);
        dsnd_t d;
        memcpy(&d, b, sizeof d);
        dsu_fix_sound(&d);   /* 与上游 DSYN_PUT 同序：写值 → fix → 声音立即读到 */
        memcpy(&dsu.k[kit].s[lane], &d, sizeof d);
        dsu_dirty = 1;
        ret = dsyn_get((const uint8_t *)&d, field);
    }
    engine_unlock();
    return ret;
}

int16_t sloop_dsyn_field_from_sound(const uint8_t sound[22], uint32_t field)
{
    if (sound == NULL || field >= SLOOP_DSYN_FIELDS)
        return SLOOP_DSYN_INVALID;
    return dsyn_get(sound, field);
}

static int16_t text_len(char *out, uint32_t n)
{
    size_t l = strlen(out);
    return (int16_t)(l < n ? l : n - 1u);
}

/* DECAY_K（drum_synth.c 的衰减系数，Q16）→ 到 -60 dB 的毫秒数。表由生成器按
 * 65536·exp(-6900/(44100·ms)) 取整，一阶反解 ms ≈ 10254/(65536 - DECAY_K)；
 * 索引取整在长衰减端有十几%的显示误差（仅供 UI 文本，不参与声音）。 */
static uint32_t decay_ms(uint32_t idx)
{
    uint32_t k = DECAY_K[idx & 127u];
    uint32_t d = 65536u - k;
    if (d == 0)
        d = 1;
    uint32_t ms = 10254u / d;
    if (ms < 5u)
        ms = 5u;
    if (ms > 4000u)
        ms = 4000u;
    return ms;
}

int16_t sloop_core_dsyn_text(uint32_t field, int16_t v, char *out, uint32_t n)
{
    if (out == NULL || n == 0 || field >= SLOOP_DSYN_FIELDS)
        return 0;
    switch (field) {
    case SLOOP_DSYN_BTIME:
    case SLOOP_DSYN_DECAY:
    case SLOOP_DSYN_NDEC: {
        uint32_t ms = decay_ms((uint32_t)clamp(v, 0, 127));
        if (ms < 1000u)
            snprintf(out, n, "%ums", (unsigned)ms);
        else
            snprintf(out, n, "%u.%us", (unsigned)(ms / 1000u), (unsigned)((ms % 1000u) / 100u));
        return text_len(out, n);
    }
    case SLOOP_DSYN_FCUT:
    case SLOOP_DSYN_HPF: {
        uint32_t hz = CUTOFF_HZ[(uint32_t)clamp(v, 0, 127)];
        if (hz < 1000u)
            snprintf(out, n, "%uHz", (unsigned)hz);
        else
            snprintf(out, n, "%u.%ukHz", (unsigned)(hz / 1000u), (unsigned)((hz % 1000u) / 100u));
        return text_len(out, n);
    }
    case SLOOP_DSYN_LEVEL: {
        /* level = 128 + 4 × dB（0.25 dB 一步）；显示到 0.1 dB，负值截断到零方向 */
        int32_t tenths = ((int32_t)v - 128) * 5 / 2;
        int32_t mag = tenths < 0 ? -tenths : tenths;
        snprintf(out, n, "%s%d.%ddB", tenths < 0 ? "-" : "", (int)(mag / 10), (int)(mag % 10));
        return text_len(out, n);
    }
    case SLOOP_DSYN_FLT_MODE: {
        int32_t m = clamp(v, 0, 7);
        if (!(m & 3))
            m &= 3;
        snprintf(out, n, "%s", DSYN_N_FLT[m]);
        return text_len(out, n);
    }
    case SLOOP_DSYN_CHIP: {
        uint32_t note = (uint32_t)clamp(v, 0, 127);   /* CHIP 噪声的时钟音高（MIDI 音名） */
        snprintf(out, n, "%s%d", N_NOTE[note % 12u], (int)(note / 12u) - 1);
        return text_len(out, n);
    }
    default:
        return 0;   /* 其余字段走通用整数/枚举格式化 */
    }
}

esp_err_t sloop_core_dsyn_load_factory(uint32_t kit, uint32_t src)
{
    if (kit >= SLOOP_DSYN_KITS || src >= DS_NKITS)
        return ESP_ERR_INVALID_ARG;
    if (s_frozen)
        return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    (void)dsu_kit(kit);
    dsu_from_factory(kit, src);
    dsu_fix_kit(&dsu.k[kit]);
    dsu_dirty = 1;
    engine_unlock();
    return ESP_OK;
}

void sloop_core_dsyn_audition(uint32_t kit, uint32_t lane, uint32_t velocity)
{
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES || s_frozen)
        return;
    engine_lock();
    if (!s_frozen) {
        dsu_aud_k = (uint8_t)kit;
        dsu_aud_lane = (uint8_t)lane;
        dsu_aud_vel = (uint8_t)clamp((int32_t)velocity, 0, 127);   /* 最后写：poll 见 vel 即消费 */
    }
    engine_unlock();
}

bool sloop_core_dsyn_dirty(void)
{
    return dsu_dirty != 0;
}
