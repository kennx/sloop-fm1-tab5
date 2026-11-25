/*
 * preset_tab5.h — 「载入声音」机制的唯一实现（上游 ui.c:581-691 的 TRK_DEF /
 * param_kept / arp_is_sounds / arp_param / preset_values / apply_preset_to /
 * set_engine_of，按原语义移植）。
 *
 * P4-F Task 6 从 store_tab5.c **原样抽出**成头文件：固件侧仍由 store_tab5.c include 进
 * sloop_core 单一 TU（位置前移到 include 上游文件之前，函数体一字不改）；同一份源码由
 * scripts/seq-host-test.c 同源编译——TOOLS 的 INIT SOUND / NEW 与 sloop_core_set_engine
 * 都走这里的 set_engine_of_locked / preset_apply_locked，host 门禁才能比对「等价」。
 * 先例：song_persist.h（SONG_PANEL_DEF）、desc_tab5.h。全部是 static，可被多个 TU 各自
 * include。
 *
 * 包含方必须已提供（顺序见 store_tab5.c / seq-host-test.c）：
 *  - 上游 core.h、params.c（TP）、engines.c（ENGINES、preset_extras、preset_trim.h）、
 *    seq.c（arp_keep_req、locks_restore、panic_req）、voice.c（sound_change）、
 *    eng_fm6.c（fm6_track_loaded）；
 *  - `sync_reload`：载入后置位的重载开关（上游 ui.c；store_tab5.c 的 shim）；
 *  - `up_used` / `up_arp_of`：用户预置银行的只读查询（upreset.c；只在该轨确实是用户
 *    预置（t->user != 0）时才被调到——host 测试不带银行，给恒空的替身即可）。
 *
 * 锁：`*_locked` 变体**要求调用方已持引擎锁**（上游用 fm1_irq_off/on；Tab5 的渲染与
 * 全部写 API 共用引擎锁，互斥锁不可重入）。公共入口（sloop_core_set_engine 等）在
 * 锁内调用它们。
 */
#ifndef SLOOP_PRESET_TAB5_H
#define SLOOP_PRESET_TAB5_H

/* 上游 ui.c:592：上电时各合成轨的（引擎，预置）——"ANALOG 808 BOOM / DIGITAL RHODES /
 * SAMPLE LOFI FLUTE"。project.c 的 FUN1 导入需要它（其余三轨的默认声音）。 */
static const uint8_t TRK_DEF[NPART][2] = {{0, 0}, {1, 0}, {4, 5}};
static uint32_t trk_def_engine(uint32_t i)
{
    return i < NPART ? TRK_DEF[i][0] : 0u;
}

/* 上游 ui.c:592 的 param_kept：载入声音时**不**被覆盖的参数（混音、图案参数、调性）。
 * upreset.c 的 up_load_to 与工厂预置的预设应用（preset_values/preset_apply_locked）
 * 都用它逐项保留轨道自己的值。 */
static int param_kept(uint32_t i)
{
    return i == P_LEVEL || i == P_PAN || i == P_MUTE || (i >= P_SLEN && i <= P_SGATE) ||
           (i >= P_ROOT && i <= P_QUANT) || i == P_CHORD || i == P_TFLT || i == P_STRUM || i == P_VLEAD;
}

/* 上游 ui.c:24：轨的声音来自哪个用户预置（UP_SLOTS = 不是用户预置） */
static uint32_t user_of(const track_t *t)
{
    return t->user && up_used(t->user - 1u) ? t->user - 1u : UP_SLOTS;
}

/* 上游 ui.c:607（逐行同义）：正在跑的琶音是不是这个声音自带的（2.7.1）——载入新声音
 * 时只有「不是它带来的」琶音才保留。up_load_to（upreset.c）用它：包含方在本头文件之后
 * 才 include upreset.c，它用到的 up_used / up_arp_of 已在包含方先行声明。 */
static int arp_is_sounds(const track_t *t)
{
    int16_t a[8];
    uint32_t i, k = user_of(t);
    if (k < UP_SLOTS) {
        if (up_arp_of(k, a))
            return 0;
    } else {
        const engine_t *e = ENGINES[t->engine % NENGINES];
        const preset_t *pr;
        if (t->user || !e->npresets)
            return 0;
        pr = &e->presets[t->preset % e->npresets];
        for (i = 0; i < 8u; i++)
            a[i] = (int16_t)(i < 4u && pr->arp[i] ? pr->arp[i] - 1 : TP[P_AMODE + i].def);
    }
    for (i = 0; i < 8u; i++)
        if (t->p[P_AMODE + i] != a[i])
            return 0;
    return 1;
}

/* ------------------------------------------------ 换引擎/预置 = 换声音 --- */
/* 上游 ui.c:581-691 的预置机制（TRK_DEF/trk_def_engine/param_kept/arp_param/
 * arp_is_sounds/apply_preset_to/set_engine_of）按原语义移植到本适配层。这是「换工厂
 * 预置 / 换引擎 = 换声音」的唯一实现：音频路径（voice.c/fx.c）读的是 t->p[]，只写
 * eng_req/preset 索引不会改声音（P4-B spec §2 的真缺陷）。
 *
 * 锁：上游在 FM-1 上用 fm1_irq_off/on 挡住音频 ISR；Tab5 上渲染走引擎锁
 * （sloop_core_render 与所有写 API 共用），故下面的 `*_locked` 变体**要求调用方已持
 * 引擎锁**——公共 API 与 sloop_store_project_load 都在锁内调它们，绝不递归取锁
 * （xSemaphoreCreateMutex 不可重入）。 */

/* 上游 ui.c:594 的 arp_param：琶音参数（MODE…ORDER）。在跑琶音时它们不被下一个声音
 * 覆盖（2.7：你按下的琶音设置继续用），由 keep_arp 决定 */
static int arp_param(uint32_t i)
{
    return i >= P_AMODE && i <= P_AORDER;
}

/* 上游 apply_preset_to 的**纯写值**部分（抽成一份实现，供应用与预览共用）：v = base->p[]
 * 的拷贝，再按引擎 e 的预置 pi 覆写——param_kept 保留项、keep_arp 下的 arp 参数、
 * kbd_hold 下的 P_AHOLD 不写；P_E0 以下其余回到 TP 默认；P_E0..P_E7、包络、ENV→FLT、
 * level trim、VOICE、fx[4]、arp[4] 与 preset_extras 取预置值。引擎没有预置时上游在
 * 写索引与清默认之前就返回：p[] 一个字节都不写（v 即 base 的拷贝）。 */
static void preset_values(int16_t *v, const track_t *base, uint32_t e, uint32_t pi, int keep_arp,
                          int kbd_hold)
{
    const engine_t *en = ENGINES[e % NENGINES];
    const preset_t *pr;
    uint32_t i;
    memcpy(v, base->p, sizeof base->p);
    if (!en->npresets) return;
    pi %= en->npresets;
    pr = &en->presets[pi];
    for (i = 0; i < P_E0; i++) /* 声音的其余部分回到默认：预置才是声音（不是图案、不是混音） */
        if (!param_kept(i) && !(keep_arp && arp_param(i)) && !(kbd_hold && i == P_AHOLD))
            v[i] = TP[i].def;
    for (i = 0; i < 8u; i++) v[P_E0 + i] = (int16_t)pr->e[i];
    v[P_ATK] = pr->env[0];
    v[P_DEC] = pr->env[1];
    v[P_SUS] = pr->env[2];
    v[P_REL] = pr->env[3];
    v[P_ED_FLT] = pr->fenv;
    v[P_ED_FX] = preset_trim(e % NENGINES, pi); /* 电平对齐（tools/level_presets.py） */
    v[P_VOICE] = pr->mono ? V_LEGATO : V_POLY;  /* mono 预置保留 legato 手感 */
    {
        static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
        for (i = 0; i < 4u; i++) {
            v[P_DIST + i] = (int16_t)(pr->fx[i] ? pr->fx[i] - 1 : FX_DEF[i]);
            if (!keep_arp)
                v[P_AMODE + i] = (int16_t)(pr->arp[i] ? pr->arp[i] - 1 : TP[P_AMODE + i].def);
        }
        preset_extras(v, pr); /* glide、pitch / LFO 调制、voice mode */
    }
}

/* 上游 apply_preset_to（ui.c:620-673，逐行同义）的副作用半段：琶音保留请求（必须在
 * panic 之前）→ sound_change（旧声音淡出，无爆音）→ panic 本轨（MONO/POLY 可能变）→
 * user 清 0 → 选中轨置 sync_reload → 预置索引 → 纯写值 → FM6 载入预置的 patch。
 * 调用方持引擎锁（上游此处以 fm1_irq_off/on 保护）。 */
static void preset_apply_locked(track_t *t, uint32_t pi)
{
    const engine_t *e = ENGINES[t->eng_req % NENGINES];
    int16_t v[P_COUNT];
    int keep_arp, kbd_hold;
    if (is_drum(t)) return;
    keep_arp = t->p[P_AMODE] != 0 && !arp_is_sounds(t); /* 2.7：在跑的琶音不是声音自带的才保留 */
    kbd_hold = !t->p[P_AMODE] && t->p[P_AHOLD];         /* 2.7：KBD HOLD 保持开 */
    if (keep_arp) arp_keep_req |= (uint8_t)(1u << trk_index(t));
    sound_change(t);
    panic_req |= (uint8_t)(1u << trk_index(t));
    t->user = 0;
    if (t == &trk[song.sel]) sync_reload = 1;
    if (!e->npresets) return;
    pi %= e->npresets;
    t->preset = (uint8_t)pi;
    preset_values(v, t, t->eng_req % NENGINES, pi, keep_arp, kbd_hold);
    memcpy(t->p, v, sizeof v);
    fm6_track_loaded(t);
}

/* 上游 set_engine_of（ui.c:677-689）：锁先还原（在锁里的参数先回基准）→ sound_change →
 * 记下新引擎 → 引擎默认值 → 预置 0。上游的 fm1_irq_off/on 在这里就是引擎锁。 */
static void set_engine_of_locked(track_t *t, uint32_t ei)
{
    const engine_t *e = ENGINES[ei % NENGINES];
    uint32_t i;
    if (is_drum(t)) return;
    locks_restore(t);
    sound_change(t);
    t->eng_req = (uint8_t)(ei % NENGINES);
    for (i = 0; i < 8u; i++) t->p[P_E0 + i] = e->edit[i].def;
    preset_apply_locked(t, 0);
}

#endif /* SLOOP_PRESET_TAB5_H */
