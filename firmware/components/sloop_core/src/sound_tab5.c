/* P4-D：上游 drum_synth.c / editor_dsyn.c 的 DSYN（SYN1..SYN4）字段适配（Task 2）、
 * FM6 155B patch 字段适配（Task 3）与用户采样槽 USR1..4 的 flash+镜像存储（Task 4）。
 * 由 selftest.c 末尾 include（在 seq_tab5.c 之后），因此上游 statics
 * （dsu/dsu_kit/dsu_from_factory/dsu_fix_sound/dsu_fix_kit/dsu_dirty/dsu_aud_k/lane/vel、
 * fm6_patch/fm6_slot 与 fm6_sanitize 等、eng_sample.c 的 smp_user_.../usr_zone/usr_nz）与生成表
 * （DS_KITS/DECAY_K/CUTOFF_HZ、params.c 的 N_NOTE）都可见；存储侧沿用 store_tab5.c
 * 的 st_read/st_erase/st_prog、st_crc32 与 store_lock/unlock（不复制锁）。
 * 数据布局与钳位方向以上游为准；虚拟字段拆分见 sloop_core.h 的 DSYN/FM6 段与 spec §4.1。 */
#include "sloop_core.h"
#include "sloop_store.h"

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
    /* 上游 ED_DSYN_PLAY：k/lane 越界或 velocity == 0 都拒绝（0 是「无声」，不是「试听」） */
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES || velocity == 0 || velocity > 127 || s_frozen)
        return;
    engine_lock();
    if (!s_frozen) {
        dsu_aud_k = (uint8_t)kit;
        dsu_aud_lane = (uint8_t)lane;
        dsu_aud_vel = (uint8_t)velocity;   /* 最后写：poll 见 vel 即消费 */
    }
    engine_unlock();
}

bool sloop_core_dsyn_dirty(void)
{
    return dsu_dirty != 0;
}

/* ---------------------------------------------------------------- FM6 patch ---
 * P4-D Task 3：上游 `fm6_patch[t]` 的 155B voice 单字段编辑。字段编号与偏移见
 * sloop_core.h 的 FM6 段；范围取 `fm6_max()`（`fm6_sanitize` 的同一张表），写路径
 * 与上游 `ED_FM6_PUT` 同序：钳位 → `fm6_set_patch`（sanitize + `fm6_pgen`）→
 * `fm6_slot = p[P_E7]`，ALG 额外清 ALG 宏（`fm6_sync` 会用它覆盖 patch 的算法）。 */
_Static_assert(FP_SIZE == 155 && FP_OP == 21 && FP_PR1 == 126 && FP_TRNSP == 144 && FP_NAME == 145,
               "FM6 patch layout 与公开的 155/21/29 字段编号不一致");

/* 全局块里可编辑的 voice 字段数（`FP_PR1..FP_TRNSP`）；其后的 10 项是名称字节。 */
#define FM6_GLOBAL_VOICE (SLOOP_FM6_GLOBAL_FIELDS - SLOOP_FM6_NAME_FIELDS)

static const char *const FM6_N_OP[SLOOP_FM6_OP_FIELDS] = {
    "R1", "R2", "R3", "R4", "L1", "L2", "L3", "L4", "BP", "LD", "RD",
    "LC", "RC", "RS", "AMS", "KVS", "OL", "MODE", "FC", "FF", "DET",
};
static const char *const FM6_N_GLOBAL[SLOOP_FM6_GLOBAL_FIELDS] = {
    "PR1", "PR2", "PR3", "PR4", "PL1", "PL2", "PL3", "PL4", "ALG", "FB",
    "OKS", "LFS", "LFD", "LPMD", "LAMD", "LKS", "LFW", "LPMS", "TRNSP",
    "NAME1", "NAME2", "NAME3", "NAME4", "NAME5",
    "NAME6", "NAME7", "NAME8", "NAME9", "NAME10",
};
/* ALG：补丁里的 `FP_ALG` 是 0..31 的算法索引（fm6_sync 用 `& 31` 查 FM6_ALG），
 * 编辑器/面板从 1 起编号（ALG 宏 1..32，0 另作 PAT），故名字为 "1".."32"。 */
static const char *const FM6_N_ALG[32] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16",
    "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32",
};
/* LFW：fm6_core.c 的 `fm6_lfo_sample` 的波形编号 0..5（triangle / saw down / saw up /
 * square / sine / sample & hold）。 */
static const char *const FM6_N_LFW[6] = {"TRI", "SAW DN", "SAW UP", "SQR", "SIN", "S&H"};
/* LPMS：LFO 音高调制灵敏度，`FM6_PMS[]` 的索引 0..7（0 = 无调制）。 */
static const char *const FM6_N_LPMS[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};

/* 字段 -> 155B 内的绝对偏移；越界返回 -1。 */
static int32_t fm6_field_offset(uint32_t op, uint32_t field)
{
    if (op < SLOOP_FM6_GLOBAL_OP)
        return field < SLOOP_FM6_OP_FIELDS ? (int32_t)(op * FP_OP + field) : -1;
    if (op != SLOOP_FM6_GLOBAL_OP || field >= SLOOP_FM6_GLOBAL_FIELDS)
        return -1;
    return field < FM6_GLOBAL_VOICE ? (int32_t)(FP_PR1 + field) : (int32_t)(FP_NAME + field - FM6_GLOBAL_VOICE);
}

bool sloop_core_fm6_desc(uint32_t op, uint32_t field, sloop_desc_t *out)
{
    int32_t off = fm6_field_offset(op, field);
    if (out == NULL || off < 0)
        return false;
    bool global = op == SLOOP_FM6_GLOBAL_OP;
    bool name = global && field >= FM6_GLOBAL_VOICE;
    out->label = global ? FM6_N_GLOBAL[field] : FM6_N_OP[field];
    out->unit = NULL;
    out->fmt = SLOOP_DESC_FMT_INT;
    out->min = (int16_t)(name ? 32 : 0);
    out->max = (int16_t)(name ? 126 : fm6_max((uint32_t)off));
    out->def = 0;
    out->enums = NULL;
    out->enum_count = 0;
    if (!global && field == FP_MODE) {
        /* MODE 是**每个**算子块里的同一个字段（off == FP_MODE 只覆盖 op 0），按 (op, field) 判 */
        out->fmt = SLOOP_DESC_FMT_ONOFF;
    } else if (global) {
        switch (field) {
        case FP_ALG - FP_PR1:
            out->fmt = SLOOP_DESC_FMT_ENUM;
            out->enums = FM6_N_ALG;
            out->enum_count = 32;
            break;
        case FP_LFW - FP_PR1:
            out->fmt = SLOOP_DESC_FMT_ENUM;
            out->enums = FM6_N_LFW;
            out->enum_count = 6;
            break;
        case FP_LPMS - FP_PR1:
            out->fmt = SLOOP_DESC_FMT_ENUM;
            out->enums = FM6_N_LPMS;
            out->enum_count = 8;
            break;
        case FP_OKS - FP_PR1:
        case FP_LKS - FP_PR1:
            out->fmt = SLOOP_DESC_FMT_ONOFF;
            break;
        default:
            break;
        }
    }
    return true;
}

esp_err_t sloop_core_fm6_patch(uint32_t track, sloop_fm6_patch_snapshot_t *out)
{
    if (track >= NPART || out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_frozen)
        return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(out->patch, fm6_patch[track], SLOOP_FM6_PATCH_SIZE);
    out->slot = (uint8_t)trk[track].p[P_E7];
    out->engine_ok = (uint8_t)(trk[track].eng_req == ENGI_FM6);
    esp_err_t ret = out->engine_ok ? ESP_OK : ESP_ERR_NOT_ALLOWED;
    engine_unlock();
    return ret;
}

int16_t sloop_core_fm6_field(uint32_t track, uint32_t op, uint32_t field)
{
    int32_t off = fm6_field_offset(op, field);
    if (track >= NPART || off < 0 || s_frozen)
        return SLOOP_FM6_INVALID;
    engine_lock();
    int16_t ret = SLOOP_FM6_INVALID;
    if (!s_frozen && trk[track].eng_req == ENGI_FM6)
        ret = (int16_t)fm6_patch[track][off];
    engine_unlock();
    return ret;
}

int16_t sloop_core_fm6_set(uint32_t track, uint32_t op, uint32_t field, int16_t v)
{
    int32_t off = fm6_field_offset(op, field);
    /* 名称字节只读（屏幕不改名，spec §1 非目标） */
    if (track >= NPART || off < 0 || s_frozen || (op == SLOOP_FM6_GLOBAL_OP && field >= FM6_GLOBAL_VOICE))
        return SLOOP_FM6_INVALID;
    engine_lock();
    int16_t ret = SLOOP_FM6_INVALID;
    if (!s_frozen && trk[track].eng_req == ENGI_FM6) {
        uint8_t patch[FP_SIZE];
        memcpy(patch, fm6_patch[track], FP_SIZE);
        patch[off] = (uint8_t)clamp((int32_t)v, 0, (int32_t)fm6_max((uint32_t)off));
        fm6_set_patch(track, patch);
        /* 上游 ED_FM6_PUT：这个 patch 现在就是该轨自己的，PTCH 不变则 fm6_poll 不再覆盖 */
        fm6_slot[track] = (uint8_t)trk[track].p[P_E7];
        /* ALG 宏（P_E0）≠ PAT 时 fm6_sync 用它覆盖 patch 的算法：写 ALG 同锁清回 PAT */
        if (off == FP_ALG)
            trk[track].p[P_E0] = 0;
        ret = (int16_t)fm6_patch[track][off];
    }
    engine_unlock();
    return ret;
}

int16_t sloop_fm6_field_from_patch(const uint8_t patch[SLOOP_FM6_PATCH_SIZE], uint32_t op, uint32_t field)
{
    int32_t off = fm6_field_offset(op, field);
    if (patch == NULL || off < 0)
        return SLOOP_FM6_INVALID;
    return (int16_t)patch[off];
}

/* ---- 采样槽 USR1..USR4（P4-D Task 4，spec §3.1/§4.2）--------------------------------
 * 上游 eng_sample.c 的原始槽：header 480B（magic/version/nz/name/data_len/crc + 16×zone）+
 * data@512，IMA ADPCM 4bit。flash 是持久副本（SMP_USER_OFF），引擎只读 PSRAM 镜像
 * （selftest.c 把 SMP_USER_XIP 指到 sloop_hal_user_samples()）。
 *
 * 锁序（ADR-009 §3）：写入口持**存储锁**、擦写期间不持引擎锁；只有 `smp_user_scan` 与
 * 镜像失效的那一小段在引擎锁内——上游用 fm1_irq_off/on 排空音频中断，那在本固件是空壳，
 * 引擎锁才是真正的保护（voice 读 usr_zone/usr_nz 都在渲染路径）。锁函数本身来自
 * store_tab5.c（同一 TU 的 static；不复制第二把锁），host 门禁给出同签名的单线程桩。 */
uint8_t *sloop_hal_user_samples(void); /* 声明见 sloop_hal.h；host TU 不 include 它（带 esp_codec_dev.h） */

#define SMP_HDR_SIZE 480u                 /* smp_user_hdr_t：32B 固定头 + 16×28B zone */
#define SMP_SECTOR 4096u
#define SMP_SECTORS (SLOOP_STORE_SAMPLE_SIZE / SMP_SECTOR) /* 20 个 4KB 扇区 = 80KiB */
#define SMP_WRITE_MAX 256u                /* API 上限（console 每行 ≤120B） */

_Static_assert(SLOOP_STORE_SAMPLES == SMP_USER_SLOTS, "与上游 SMP_USER_SLOTS 不一致");
_Static_assert(SLOOP_STORE_SAMPLE_SIZE == SMP_USER_SIZE, "与上游 SMP_USER_SIZE 不一致");
_Static_assert(SLOOP_STORE_SAMPLE_DATA == SMP_USER_DATA, "与上游 SMP_USER_DATA 不一致");
_Static_assert(sizeof(smp_user_hdr_t) == SMP_HDR_SIZE, "smp_user_hdr_t 不是 480B");
_Static_assert(SMP_SECTORS <= 32u, "扇区位图按 32 位算");

static uint8_t s_smp_open = (uint8_t)SLOOP_STORE_SAMPLES; /* 已 BEGIN 未 END 的槽；== 4 = 无 */
static uint32_t s_smp_hdr_n;                              /* header 已累积字节数 */
static uint32_t s_smp_erased;                             /* 本次上传已擦除的扇区位图 */
static uint32_t s_smp_hdr[SMP_HDR_SIZE / 4];              /* 480B header 暂存（4 字节对齐） */

static uint8_t *smp_slot_mirror(uint32_t slot)
{
    return sloop_hal_user_samples() + slot * SLOOP_STORE_SAMPLE_SIZE;
}

/* 参数/就绪门（顺序：槽号 → 存储就绪 → 冻结 → 走带）。write=false 是只读诊断路径：
 * 冻结只管写（与 bank 的读入口同口径：UI 诊断期仍可显示槽信息）。 */
static esp_err_t smp_gate(uint32_t slot, bool write)
{
    if (slot >= SLOOP_STORE_SAMPLES) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready() || sloop_hal_user_samples() == NULL) return ESP_ERR_INVALID_STATE;
    if (write && s_frozen) return ESP_ERR_INVALID_STATE;
    if (write && store_playing()) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

/* header 的静态合法性（与 smp_user_scan 同一判据：zone 用上游 smp_zone_ok）。 */
static bool smp_hdr_ok(const smp_user_hdr_t *h)
{
    uint32_t i;
    if (h->magic != SMP_USER_MAGIC || h->version != 1u || h->nz == 0u || h->nz > 16u ||
        h->data_len > SMP_USER_SIZE - SMP_USER_DATA)
        return false;
    for (i = 0; i < h->nz; i++)
        if (!smp_zone_ok(&h->zone[i], h->data_len)) return false;
    return true;
}

/* 擦除 [first..last] 中本次上传还没擦过的扇区（每槽首次写入前擦，跨边界的两次都擦）。 */
static int smp_erase_sectors(uint32_t slot, uint32_t first, uint32_t last)
{
    uint32_t s = first;
    int rc = 0;
    for (; s <= last && rc == 0; s++) {
        if (s_smp_erased & (1u << s)) continue;
        rc = st_erase(SMP_USER_OFF(slot) + s * SMP_SECTOR);
        if (rc == 0) s_smp_erased |= 1u << s;
    }
    return rc;
}

/* 槽失效（引擎锁内）：不再有新的 voice 取用该槽，zone 表清空（上游 ed_smp_erase 语义）。 */
static void smp_slot_invalidate_locked(uint32_t slot)
{
    memset(usr_zone[slot], 0, sizeof usr_zone[slot]);
    smp_user_scan(slot); /* 读镜像 header（调用方已置 0xFF）⇒ usr_nz=0、usr_gen++ */
}

esp_err_t sloop_store_sample_info(uint32_t slot, sloop_store_sample_info_t *out)
{
    esp_err_t ret = smp_gate(slot, false);
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (ret != ESP_OK) return ret;
    memset(out, 0, sizeof *out);
    const smp_user_hdr_t *h = (const smp_user_hdr_t *)smp_slot_mirror(slot);
    if (!smp_hdr_ok(h)) return ESP_OK; /* 空/不可用：used=0（不是错误） */
    if (st_crc32((const uint8_t *)h + SMP_USER_DATA, h->data_len) != h->crc) return ESP_OK;
    out->used = 1;
    out->zones = h->nz;
    memcpy(out->name, h->name, sizeof h->name);
    out->name[sizeof h->name] = '\0';
    out->data_kb = (uint16_t)(h->data_len / 1024u);
    return ESP_OK;
}

esp_err_t sloop_store_sample_begin(uint32_t slot)
{
    esp_err_t ret = smp_gate(slot, true);
    if (ret != ESP_OK) return ret;
    uint8_t *m = smp_slot_mirror(slot);
    store_lock();
    /* 上一次未完成的上传一律丢弃：新 BEGIN 重新给 header 与扇区位图 */
    s_smp_open = (uint8_t)slot;
    s_smp_hdr_n = 0;
    s_smp_erased = 0;
    memset(s_smp_hdr, 0xFF, sizeof s_smp_hdr);
    /* 镜像先失效（BEGIN 后 INFO 立即为空；正在发声的 voice 不再取新 zone） */
    memset(m, 0xFF, SMP_USER_DATA);
    engine_lock();
    smp_slot_invalidate_locked(slot);
    engine_unlock();
    /* 擦 header 扇区 ⇒ 槽在 flash 侧也立即失效（END 最后才写 header = 提交点） */
    int rc = st_erase(SMP_USER_OFF(slot));
    if (rc == 0) s_smp_erased = 1u;
    else s_smp_open = (uint8_t)SLOOP_STORE_SAMPLES;
    store_unlock();
    return rc == 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

esp_err_t sloop_store_sample_hdr_append(const uint8_t *p, uint32_t n)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (p == NULL || n == 0) return ESP_ERR_INVALID_ARG;
    store_lock();
    esp_err_t ret = ESP_OK;
    if (s_smp_open >= SLOOP_STORE_SAMPLES)
        ret = ESP_ERR_INVALID_STATE;
    else if (n > SMP_HDR_SIZE - s_smp_hdr_n)
        ret = ESP_ERR_INVALID_SIZE;
    else {
        memcpy((uint8_t *)s_smp_hdr + s_smp_hdr_n, p, n);
        s_smp_hdr_n += n;
    }
    store_unlock();
    return ret;
}

esp_err_t sloop_store_sample_write(uint32_t slot, uint32_t off, const uint8_t *data, uint32_t n)
{
    esp_err_t ret = smp_gate(slot, true);
    if (ret != ESP_OK) return ret;
    if (data == NULL || n == 0) return ESP_ERR_INVALID_ARG;
    if (n > SMP_WRITE_MAX || off < SLOOP_STORE_SAMPLE_DATA || (off & 255u) != 0u ||
        off > SLOOP_STORE_SAMPLE_SIZE - n)
        return ESP_ERR_INVALID_SIZE;
    store_lock();
    if (s_smp_open != slot) {
        store_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    int rc = smp_erase_sectors(slot, off / SMP_SECTOR, (off + n - 1u) / SMP_SECTOR);
    if (rc == 0) rc = st_prog(SMP_USER_OFF(slot) + off, data, n);
    if (rc == 0) memcpy(smp_slot_mirror(slot) + off, data, n); /* header 不动（END 才提交） */
    store_unlock();
    return rc == 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

esp_err_t sloop_store_sample_end(uint32_t slot)
{
    esp_err_t ret = smp_gate(slot, true);
    if (ret != ESP_OK) return ret;
    const smp_user_hdr_t *h = (const smp_user_hdr_t *)s_smp_hdr;
    uint8_t *m = smp_slot_mirror(slot);
    store_lock();
    if (s_smp_open != slot) {
        store_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_smp_hdr_n != SMP_HDR_SIZE || !smp_hdr_ok(h)) {
        store_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    /* 数据以 flash 回读为准：整槽搬进镜像（顺带保证 END 之后 mirror == flash），再算 CRC */
    int rc = st_read(SMP_USER_OFF(slot), m, SLOOP_STORE_SAMPLE_SIZE);
    if (rc != 0) {
        store_unlock();
        return ESP_ERR_NOT_FINISHED; /* header 未提交 ⇒ 槽仍空 */
    }
    uint32_t crc = st_crc32(m + SMP_USER_DATA, h->data_len);
    if (crc != h->crc) { /* 数据与 header 不符：保持 open（可重发数据后重试），槽仍空 */
        store_unlock();
        return ESP_ERR_INVALID_CRC;
    }
    rc = st_prog(SMP_USER_OFF(slot), s_smp_hdr, SMP_HDR_SIZE); /* 提交点：最后写 header */
    if (rc == 0) rc = st_read(SMP_USER_OFF(slot), m, SMP_USER_DATA); /* 镜像 header 与 flash 一致 */
    if (rc == 0) {
        engine_lock();
        smp_user_scan(slot);
        engine_unlock();
        s_smp_open = (uint8_t)SLOOP_STORE_SAMPLES;
    }
    store_unlock();
    return rc == 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

esp_err_t sloop_store_sample_erase(uint32_t slot)
{
    esp_err_t ret = smp_gate(slot, true);
    if (ret != ESP_OK) return ret;
    uint8_t *m = smp_slot_mirror(slot);
    store_lock();
    if (s_smp_open == slot) {
        s_smp_open = (uint8_t)SLOOP_STORE_SAMPLES;
        s_smp_hdr_n = 0;
    }
    memset(m, 0xFF, SLOOP_STORE_SAMPLE_SIZE);
    engine_lock();
    smp_slot_invalidate_locked(slot);
    engine_unlock();
    int rc = 0;
    for (uint32_t s = 0; s < SMP_SECTORS && rc == 0; s++)
        rc = st_erase(SMP_USER_OFF(slot) + s * SMP_SECTOR);
    store_unlock();
    return rc == 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

void sloop_store_sample_reload(uint32_t slot)
{
    if (slot >= SLOOP_STORE_SAMPLES || !sloop_store_ready() || sloop_hal_user_samples() == NULL) return;
    uint8_t *m = smp_slot_mirror(slot);
    store_lock();
    if (st_read(SMP_USER_OFF(slot), m, SLOOP_STORE_SAMPLE_SIZE) != 0)
        memset(m, 0xFF, SMP_USER_DATA); /* 读失败：镜像 header 不撒谎（槽按空） */
    engine_lock();
    smp_user_scan(slot);
    engine_unlock();
    store_unlock();
}
