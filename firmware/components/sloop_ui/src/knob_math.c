/*
 * 旋钮纯函数（见 knob_math.h 的契约与格式化表）。无 LVGL/FreeRTOS 依赖。
 */
#include "knob_math.h"

#include <stdio.h>

/* 上游生成表（tools/gen_tables.py → build/gen/felucca_tables.h，数据 + 一个
 * static inline）。显示口径与 params.c 的 param_format 同表同算法。 */
#include "felucca_tables.h"

static const char *const k_note_names[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                            "F#", "G",  "G#", "A",  "A#", "B"};

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int32_t knob_value_to_arc(const sloop_desc_t *d, int16_t v)
{
    int32_t range = (int32_t)d->max - d->min;
    if (range <= 0) return 0;
    int32_t x = clamp_i32((int32_t)v - d->min, 0, range);
    return (x * KNOB_ARC_RANGE + range / 2) / range;
}

int16_t knob_arc_to_value(const sloop_desc_t *d, int32_t arc)
{
    int32_t range = (int32_t)d->max - d->min;
    if (range <= 0) return d->min;
    arc = clamp_i32(arc, 0, KNOB_ARC_RANGE);
    return (int16_t)((int32_t)d->min + (arc * range + KNOB_ARC_RANGE / 2) / KNOB_ARC_RANGE);
}

bool knob_ring_hit(int32_t dx, int32_t dy)
{
    int32_t r2 = dx * dx + dy * dy;
    return r2 >= KNOB_RING_MIN_R * KNOB_RING_MIN_R && r2 <= KNOB_RING_MAX_R * KNOB_RING_MAX_R;
}

int32_t knob_angle_delta(int32_t from_deg, int32_t to_deg)
{
    int32_t d = (to_deg - from_deg) % 360;
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

int16_t knob_drag_value(const sloop_desc_t *d, int16_t anchor, int32_t delta_deg)
{
    int32_t range = (int32_t)d->max - d->min;
    if (range <= 0) return d->min;
    int32_t num = delta_deg * range; /* 四舍五入：|num|/270，半值远离零 */
    int32_t dv = num >= 0 ? (num + KNOB_DRAG_SPAN_DEG / 2) / KNOB_DRAG_SPAN_DEG
                          : -((-num + KNOB_DRAG_SPAN_DEG / 2) / KNOB_DRAG_SPAN_DEG);
    return (int16_t)clamp_i32((int32_t)anchor + dv, d->min, d->max);
}

/* ------------------------------------------------------------ 格式化 --- */
/* 与上游 param_format 同表同算法（params.c）；TIME/CUTOFF/LFO/DB 用生成表，
 * 不用指数近似。定点小数按上游 libc.c 的 fmt_fix **截断**（不是 printf 的
 * 四舍五入）：先整数分频再拼小数位，例如 ms10=18862 显示 "1.88s"（不是 1.89）。 */
static void format_fixed(char *buf, size_t n, int32_t v, int decimals)
{
    int32_t scale = decimals == 2 ? 100 : decimals == 1 ? 10 : 1;
    int32_t ip = v / scale;
    int32_t frac = v % scale;
    if (frac < 0) frac = -frac;
    if (decimals == 2) {
        snprintf(buf, n, "%d.%02d", (int)ip, (int)frac);
    } else if (decimals == 1) {
        snprintf(buf, n, "%d.%d", (int)ip, (int)frac);
    } else {
        snprintf(buf, n, "%d", (int)ip);
    }
}

static void format_time(int32_t v, char *buf, size_t n)
{
    char num[16];
    uint32_t ms10 = TIME_MS_X10[v & 127];
    if (ms10 < 100u) {
        format_fixed(num, sizeof(num), (int32_t)ms10, 1); /* 1.0 .. 9.9 ms（截断） */
        snprintf(buf, n, "%sms", num);
    } else if (ms10 < 10000u) {
        snprintf(buf, n, "%ums", (unsigned)((ms10 + 5u) / 10u)); /* 10 .. 999 ms（四舍五入到 ms） */
    } else if (ms10 < 100000u) {
        format_fixed(num, sizeof(num), (int32_t)(ms10 / 100u), 2); /* 1.00 .. 9.99 s（截断） */
        snprintf(buf, n, "%ss", num);
    } else {
        format_fixed(num, sizeof(num), (int32_t)(ms10 / 1000u), 1); /* 10.0 s */
        snprintf(buf, n, "%ss", num);
    }
}

static void format_lfohz(int32_t v, char *buf, size_t n)
{
    char num[16];
    uint32_t h100 = LFO_HZ_X100[v & 127];
    if (h100 < 1000u) {
        format_fixed(num, sizeof(num), (int32_t)h100, 2); /* 0.05 .. 9.99 Hz（截断） */
    } else {
        format_fixed(num, sizeof(num), (int32_t)(h100 / 10u), 1); /* 10.0 Hz+（截断） */
    }
    snprintf(buf, n, "%sHz", num);
}

static void format_cutoff(int32_t v, char *buf, size_t n)
{
    char num[16];
    uint32_t hz = CUTOFF_HZ[v & 127];
    if (hz < 1000u) {
        snprintf(buf, n, "%uHz", (unsigned)hz);
        return;
    }
    if (hz < 10000u) {
        format_fixed(num, sizeof(num), (int32_t)(hz / 100u), 1); /* 1.0 .. 9.9 kHz（截断） */
    } else {
        snprintf(num, sizeof(num), "%u", (unsigned)((hz + 500u) / 1000u)); /* 10 kHz+（四舍五入） */
    }
    snprintf(buf, n, "%skHz", num);
}

static void format_db(int32_t v, char *buf, size_t n)
{
    char num[16];
    if (v <= 0) {
        snprintf(buf, n, "OFF");
        return;
    }
    int32_t db10 = LEVEL_DB_X10[v > 127 ? 127 : v];
    if (db10 > -100) {
        format_fixed(num, sizeof(num), db10, 1);
    } else {
        snprintf(num, sizeof(num), "%d", (int)((db10 - 5) / 10));
    }
    snprintf(buf, n, "%sdB", num);
}

/* F_INT 的 0 终止名字表（ENV SYNC 音值等）：值域均分映射（上游 names_turn/format）。 */
static const char *name_for_int(const sloop_desc_t *d, int16_t v)
{
    uint32_t k = d->enum_count;
    int32_t range = (int32_t)d->max - d->min + 1;
    if (k == 0 || range <= 0 || d->enums == NULL) return NULL;
    int32_t x = clamp_i32((int32_t)v - d->min, 0, range - 1);
    uint32_t idx = (uint32_t)(x * (int32_t)k / range);
    if (idx >= k) idx = k - 1;
    return d->enums[idx];
}

void knob_format_value(const sloop_desc_t *d, int16_t v, char *buf, size_t n)
{
    if (buf == NULL || n == 0) return;
    buf[0] = '\0';
    const char *unit = d->unit;
    switch (d->fmt) {
    case SLOOP_DESC_FMT_ENUM: {
        int32_t idx = clamp_i32((int32_t)v - d->min, 0, (int32_t)d->enum_count - 1);
        snprintf(buf, n, "%s%s", d->enums != NULL && idx >= 0 ? d->enums[idx] : "?",
                 unit != NULL ? unit : "");
        return;
    }
    case SLOOP_DESC_FMT_PCT:
        snprintf(buf, n, "%d%%", d->max > 0 ? (int)(((int32_t)v * 100 + d->max / 2) / d->max) : (int)v);
        return;
    case SLOOP_DESC_FMT_BIPCT: {
        int32_t pct = (int32_t)v * 100 / 64;
        if (pct > 0) {
            snprintf(buf, n, "+%d%%", (int)pct);
        } else {
            snprintf(buf, n, "%d%%", (int)pct);
        }
        return;
    }
    case SLOOP_DESC_FMT_TIME:
        format_time(v, buf, n);
        return;
    case SLOOP_DESC_FMT_LFOHZ:
        format_lfohz(v, buf, n);
        return;
    case SLOOP_DESC_FMT_CUTOFF:
        format_cutoff(v, buf, n);
        return;
    case SLOOP_DESC_FMT_DB:
        format_db(v, buf, n);
        return;
    case SLOOP_DESC_FMT_SEMI:
        if (v > 0) {
            snprintf(buf, n, "+%dst", (int)v);
        } else {
            snprintf(buf, n, "%dst", (int)v);
        }
        return;
    case SLOOP_DESC_FMT_BPM:
        snprintf(buf, n, "%dBPM", (int)v);
        return;
    case SLOOP_DESC_FMT_NOTE: {
        int32_t idx = (int32_t)v % 12;
        snprintf(buf, n, "%s", k_note_names[idx < 0 ? 0 : idx]);
        return;
    }
    case SLOOP_DESC_FMT_ONOFF:
        snprintf(buf, n, "%s", v ? "ON" : "OFF");
        return;
    case SLOOP_DESC_FMT_STEPS:
        snprintf(buf, n, "%dSTEP", (int)v);
        return;
    case SLOOP_DESC_FMT_SWING:
        snprintf(buf, n, "%d", (int)clamp_i32(v, 0, 100));
        return;
    case SLOOP_DESC_FMT_FILT:
        if (v == 0) {
            snprintf(buf, n, "OFF");
        } else if (v < 0) {
            snprintf(buf, n, "LP%d%%", (int)((-(int32_t)v * 100 + 32) / 64));
        } else {
            snprintf(buf, n, "HP%d%%", (int)(((int32_t)v * 100 + 31) / 63));
        }
        return;
    default:
        break;
    }
    /* F_INT（含 0 终止名字表）/ 未知 fmt：原始整数 + desc.unit（上游 F_OCT 等） */
    if (d->fmt == SLOOP_DESC_FMT_INT) {
        const char *nm = name_for_int(d, v);
        if (nm != NULL) {
            snprintf(buf, n, "%s%s", nm, unit != NULL ? unit : "");
            return;
        }
    }
    snprintf(buf, n, "%d%s", (int)v, unit != NULL ? unit : "");
}
