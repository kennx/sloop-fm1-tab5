/* SPDX-License-Identifier: GPL-3.0-only */
/* 参数描述符投影：上游 param_desc_t → 公开 sloop_desc_t。由 selftest.c（固件）与
 * scripts/synth-host-test.c（host 门禁，scripts/synth-test.sh）各自在 hostsim.c 之后
 * include 一次——两个 TU 里上游 params.c 的 TP/GP/DRUM_KIT_DESC、静态 track_desc()
 * 与 engines.c 的 ENGINES[] 都已可见，所以不建任何标签镜像表（P4-F Task 3，R6）。 */
#ifndef SLOOP_DESC_TAB5_H
#define SLOOP_DESC_TAB5_H

#include <string.h>

#include "sloop_core.h"

/* 公开 fmt 镜像（sloop_core.h 的 sloop_desc_fmt_t）↔ 上游 core.h 的 F_*：换上游版本
 * 时编译期暴露，显示层（参数页）按镜像常量分支格式化。 */
_Static_assert((int)SLOOP_DESC_FMT_INT == (int)F_INT && (int)SLOOP_DESC_FMT_PCT == (int)F_PCT &&
                   (int)SLOOP_DESC_FMT_BIPCT == (int)F_BIPCT && (int)SLOOP_DESC_FMT_TIME == (int)F_TIME &&
                   (int)SLOOP_DESC_FMT_LFOHZ == (int)F_LFOHZ && (int)SLOOP_DESC_FMT_CUTOFF == (int)F_CUTOFF &&
                   (int)SLOOP_DESC_FMT_DB == (int)F_DB && (int)SLOOP_DESC_FMT_SEMI == (int)F_SEMI &&
                   (int)SLOOP_DESC_FMT_ENUM == (int)F_ENUM && (int)SLOOP_DESC_FMT_BPM == (int)F_BPM &&
                   (int)SLOOP_DESC_FMT_NOTE == (int)F_NOTE && (int)SLOOP_DESC_FMT_ONOFF == (int)F_ONOFF &&
                   (int)SLOOP_DESC_FMT_OCT == (int)F_OCT && (int)SLOOP_DESC_FMT_STEPS == (int)F_STEPS &&
                   (int)SLOOP_DESC_FMT_SWING == (int)F_SWING && (int)SLOOP_DESC_FMT_FILT == (int)F_FILT,
               "sloop_desc_fmt_t 与上游 core.h 的 F_* 编码不同步");

/* names 的非 F_ENUM 表以 0 终止（上游 param_format 同口径），F_ENUM 表按 PE() 约定
 * 「每值一名」= max-min+1，不依赖终止符。 */
static uint8_t desc_enum_count(const param_desc_t *d)
{
    if (d->names == NULL) return 0;
    if (d->fmt == F_ENUM) return (uint8_t)(d->max - d->min + 1);
    uint32_t n = 0;
    while (n < 64u && d->names[n] != NULL) n++;
    return (uint8_t)n;
}

/* 上游 param_desc_t → 公开 sloop_desc_t（契约见 sloop_core.h）。out 可为 NULL（只判有无）。 */
static void desc_from_param(const param_desc_t *d, sloop_desc_t *out)
{
    if (out == NULL) return;
    out->label = d->label;
    out->unit = d->unit;
    out->fmt = d->fmt;
    out->min = d->min;
    out->max = d->max;
    out->def = d->def;
    out->enums = d->names;
    out->enum_count = desc_enum_count(d);
}

bool sloop_core_desc(sloop_desc_scope_t scope, uint32_t engine, uint32_t id, sloop_desc_t *out)
{
    const param_desc_t *d;
    switch (scope) {
    case SLOOP_DESC_TRACK:
        if (id >= P_E0) return false; /* P_E0..P_E7 无轨道上下文，走 ENGINE */
        d = &TP[id];
        break;
    case SLOOP_DESC_GLOBAL:
        if (id >= G_COUNT) return false;
        d = &GP[id];
        break;
    case SLOOP_DESC_ENGINE:
        if (id >= 8u) return false;
        if (engine == SLOOP_CORE_NENGINES) { /* 鼓轨哨兵（上游 ed_eng 对鼓轨报 NENGINES） */
            if (id != 0u) return false;      /* 鼓轨只有 P_E0 = kit */
            d = &DRUM_KIT_DESC;
        } else if (engine < NENGINES) {
            d = &ENGINES[engine]->edit[id];
        } else {
            return false;
        }
        break;
    default:
        return false;
    }
    desc_from_param(d, out);
    return true;
}

bool sloop_core_desc_track_live(uint8_t track, uint32_t id, sloop_desc_t *out)
{
    const track_t *t;
    const param_desc_t *d;
    if (track >= NTRK || id >= P_COUNT) return false;
    t = &trk[track];
    /* 上游 track_desc() 只把鼓轨的 P_E0 当 kit，P_E1..P_E7 会落到引擎分支（params.c:238）：
     * 鼓轨没有引擎参数，这里先拒。 */
    if (id > P_E0 && id <= P_E7 && is_drum(t)) return false;
    d = track_desc(t, id);
    if (d == NULL) return false;
    desc_from_param(d, out);
    return true;
}

/* 静态串拷贝（截断到 n-1 字符；host 门禁钉住上游名字不会被截断） */
static void sound_copy_name(char *dst, uint32_t n, const char *src)
{
    uint32_t i = 0;
    if (src != NULL) {
        while (i + 1u < n && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

bool sloop_core_sound_read(uint8_t track, sloop_sound_snapshot_t *out)
{
    const track_t *t;
    const engine_t *e;
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->nengines = (uint8_t)NENGINES;
    if (track >= NTRK) return false;
    t = &trk[track];
    if (is_drum(t)) {
        /* kit 名表 = DRUM_KIT_DESC.names（drums.c 的 DRUM_KIT_NAMES，每 kit 一名）；值按
         * 上游 drum_kit() 钳到 0..DRUM_KITS-1。 */
        int32_t kit = t->p[P_E0];
        uint8_t kits = desc_enum_count(&DRUM_KIT_DESC);
        if (kit < 0) kit = 0;
        if (kits == 0u) return true;
        if (kit >= (int32_t)kits) kit = (int32_t)kits - 1;
        sound_copy_name(out->kit_name, sizeof(out->kit_name), DRUM_KIT_DESC.names[kit]);
        out->drum = 1u;
        return true;
    }
    e = ENGINES[t->eng_req % NENGINES];
    out->engine = t->eng_req % NENGINES;
    out->npresets = e->npresets;
    sound_copy_name(out->engine_name, sizeof(out->engine_name), e->name);
    if (e->npresets != 0u) {
        out->preset = (uint8_t)(t->preset % e->npresets);
        sound_copy_name(out->preset_name, sizeof(out->preset_name),
                        e->presets[out->preset].name);
    }
    return true;
}

#endif
