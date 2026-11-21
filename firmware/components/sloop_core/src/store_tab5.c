/*
 * store_tab5.c — 上游 storage.c / project.c / ui.c 的 Tab5 适配层（ADR-009 §7/§8）
 *
 * 本文件由 selftest.c **末尾 include**，与引擎/DSP 同属一个翻译单元——所以可以直接把
 * vendored 的 storage.c（A/B 双扇区 + 32B 提交头 + CRC + 回读校验，原样）、project.c
 * （#define PROJ_HOST：格式层 FUN1..6 + proj_capture/proj_apply）与 upreset.c（用户预置
 * 32 槽；#define FELUCCA_FLASH 1 才走 st_xip）编进来，缺的符号在本文件顶部做薄 shim。
 * ui.c 的预置机制（apply_preset_to/set_engine_of/TRK_DEF/param_kept/arp_is_sounds）也
 * 按原语义移植在这里（换引擎/预置 = 换声音，见「换引擎/预置」小节）。
 * third_party/ 零修改。
 *
 * 内存（ADR-009 §7，fidelity-first）：
 *   - main_scratch（上游 st_buf，3,840B）留在**内部 SRAM**：擦/写期间 cache 关闭仍要访问；
 *   - proj_slot[4]（4×3,816=15,264B）、proj_v3_tmp（2,584B）与自有暂存 proj_tmp（4,096B）
 *     全部在 PSRAM：include project.c **之前**用宏把上游的**声明本身**退化成指针
 *     （`static project_v3_t proj_v3_tmp;` → `static project_v3_t (*s_v3_ps);`）。宏包裹的
 *     是整个对象，上游全部引用形如 `&obj[...]` / `sizeof obj[i]`，替换后取址与 sizeof
 *     语义不变；
 *   - 三个指针 + 存储锁 + ready/flash_ok 放 `.noinit`：`ENG SELFTEST` 的 selftest_restore()
 *     会把本 TU（selftest.c.obj）的 .data/.bss 回滚到 init 快照，而快照在
 *     sloop_store_init 之前拍（ADR-009 §5）——普通静态指针会被抹成 NULL，自检之后的
 *     任何存储调用都会解引用空指针。上游 proj_slot 自带 .noinit，宏展开后属性留给指针。
 *   - `.noinit` 还会**跨热复位存活**（软复位不清 DRAM），而 PSRAM 堆与 HAL 的 `s_ready`
 *     （.bss）都是全新的：故哨兵必须与 `sloop_hal_storage_ready()` 一起判（HAL 的 s_ready
 *     在 .bss，热复位后为 false），否则热复位后第一次 init 之前就会拿着上一次会话的
 *     悬垂指针/信号量去取锁。`sloop_store_ready()` 是所有公共 API 的**唯一入口门**。
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sloop_hal.h"
#include "sloop_store.h"

static const char *TAG = "sloop_store";

/* 上游 st_buf（payload 暂存）必须在 include storage.c 之前可见 */
#include "scratch.h"

/* ---- PSRAM 重定向（必须先于 include project.c）：声明退化为指针 ------------------- */
#define proj_v3_tmp (*s_v3_ps)
#define proj_slot (*s_slot_ps)

/* ---- shim：project.c / upreset.c 的 PROJ_HOST / FELUCCA_FLASH 段缺的上游符号 ------ */
/* 本文件即「适配层」：这些符号在上游分属 felucca.c/ui.c/seq.c，这里按原语义给出薄实现。
 * 目前已被消费的：flash_ok（upreset.c 的 up_bank_find）、param_kept（up_load_to）、
 * up_gen/ui.force/sync_reload（重绘/推送开关的占位，UI 接入时接线）。 */
/* flash_ok 放 .noinit：本 TU（selftest.c.obj）的 .bss 被 linker.lf 的
 * SURROUND(sloop_selftest_bss) 包住，`ENG SELFTEST` 的 restore 会把 .bss 回滚到 init
 * 之前的快照（= 0）——upreset.c 的 up_bank_find 只在 flash_ok 时读 XIP 窗口，回滚后
 * 预置就会永远读成空。其余 shim（up_gen/ui.force/sync_reload）只是重绘/推送开关，
 * 回滚无害。 */
static uint8_t flash_ok __attribute__((section(".noinit"))); /* 存储后端就绪（上游 felucca.c 的 JEDEC 探测） */
static uint32_t up_gen;   /* 用户预置银行变更代号（上游 ui.c） */
static uint8_t sync_reload; /* 载入引擎/预置/工程后置位（上游 ui.c） */
static struct { uint8_t force; uint8_t rsv[3]; } ui; /* ui.force 的 4 字节占位（上游 ui.c） */

static char s_ui_msg[32];
static void ui_message(const char *m)
{
    snprintf(s_ui_msg, sizeof s_ui_msg, "%s", m);
    printf("%s\n", m);
}
static void ui_say(const char *a, const char *b)
{
    snprintf(s_ui_msg, sizeof s_ui_msg, "%s%s", a, b);
    printf("%s%s\n", a, b);
}

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

/* XIP：st_xip 在上游 flash 里原地读 payload（upreset.c 的 up_bank_find / fm6_bank.c
 * 依赖它，省掉 RAM 镜像）。Tab5 上就是 sloopstore 的 mmap 窗口（ADR-009 §2）。 */
#define ST_XIP_PTR(off) (sloop_hal_storage_map() + ((off) - sloop_hal_storage_base()))
#define FM6_BANK_XIP(copy) (ST_XIP_PTR(0xE5000u + (copy) * 4096u))

/* ---- hooks：storage.c 的三个 flash 入口（偏移一律是上游 flash-map 逻辑偏移） ------ */
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    return sloop_hal_storage_read(off, dst, n);
}
static int st_erase(uint32_t off)
{
    return sloop_hal_storage_erase(off);
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    return sloop_hal_storage_prog(off, src, n);
}

#include "storage.c"

#define PROJ_HOST 1
#include "project.c"
#undef PROJ_HOST

/* ---- PSRAM 实体：暂存（preset bank / fm6 bank / import / 捕获） ------------------- */
typedef union {
    project_t v6;    /* import 后的当前格式（读 sum 用） */
    project_v5_t v5; /* 格式 5（3,840B = 一个 flash 对象的上限） */
    uint8_t raw[4096];
} proj_stage_t;

static proj_stage_t *s_stage __attribute__((section(".noinit")));
#define proj_tmp (*s_stage)
static void *up_stage(void)
{
    return &proj_tmp; /* upreset.c：待写入的用户预置银行（sloop-lab） */
}

_Static_assert(sizeof(proj_tmp) >= sizeof(project_t), "暂存装得下一个工程（storage.c ST_PAYLOAD_MAX = 3,840）");
_Static_assert(sizeof(proj_tmp) >= sizeof(project_v5_t), "暂存装得下格式 5");

/* ---- 上游用户预置（upreset.c，32 槽 / 2 个银行对象） ----------------------------- */
/* FELUCCA_FLASH 必须为 1：upreset.c 的 up_bank_find / up_put 只在
 * `#if FELUCCA_FLASH` 内走 st_xip（否则银行永远读空、写也不落 flash）。project.c 是在
 * 这个宏未定义的条件下编入的（PROJ_HOST 段本就与 flash 无关），故这里只包住 include。
 * up_load_to 会调 arp_is_sounds（上游在 ui.c:607）；它依赖本 include 的 up_arp_of 与
 * user_of，故先声明、在 include 之后定义（ui.c 的写法相同）。 */
static int arp_is_sounds(const track_t *t);

#define FELUCCA_FLASH 1
#include "upreset.c"
#undef FELUCCA_FLASH

_Static_assert(SLOOP_STORE_PRESETS == UP_SLOTS, "公共 API 的预置槽数与上游 upreset.c 一致");
_Static_assert(sizeof(proj_tmp) >= sizeof(up_bank_t), "暂存装得下用户预置银行（3,080B）");

/* 上游 ui.c:24：轨的声音来自哪个用户预置（UP_SLOTS = 不是用户预置） */
static uint32_t user_of(const track_t *t)
{
    return t->user && up_used(t->user - 1u) ? t->user - 1u : UP_SLOTS;
}

/* 上游 ui.c:607（逐行同义）：正在跑的琶音是不是这个声音自带的（2.7.1）——载入新声音
 * 时只有「不是它带来的」琶音才保留。up_load_to 用它，故必须在 include 之后定义。 */
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

/* 上游 project.c:639-647 的 proj_foreign_parts：工程里某轨的声音来自本固件没有的引擎
 * （Clipper 写的 HYBRID/FMX…）时，proj_apply 会把引擎号绕回另一个引擎并带上别人的参数；
 * 该轨改回默认声音，图案与混音保留。调用方持引擎锁（project_load 与 Task 6 的
 * autosave_resume）。 */
static void proj_foreign_parts(const project_t *p)
{
    uint32_t k;
    for (k = 0; k < NPART; k++)
        if (p->t[k].preset != 0xFFu && p->t[k].engine >= NENGINES) {
            set_engine_of_locked(&trk[k], TRK_DEF[k][0]);
            preset_apply_locked(&trk[k], TRK_DEF[k][1]);
        }
}

/* 上游 ui.c 的 apply_preset/set_engine（对指定轨）：本 TU 的公共入口在引擎锁内执行，
 * 越界/冻结丢弃（与其它写 API 同口径）。 */
void sloop_core_apply_preset(uint32_t track, uint32_t preset)
{
    if (track >= NTRK || s_frozen) return;
    engine_lock();
    preset_apply_locked(&trk[track], preset);
    engine_unlock();
}

void sloop_core_set_engine(uint32_t track, uint32_t engine)
{
    if (track >= NTRK || engine >= NENGINES || s_frozen) return;
    engine_lock();
    set_engine_of_locked(&trk[track], engine);
    engine_unlock();
}

/* 换引擎/预置 = 换声音（语义见 sloop_core.h）：引擎变 → 默认值 + 预置 0；预置变 →
 * 应用预置。与旧实现的差别：参数真的落到 p[]（音频路径读的就是它）。 */
void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset)
{
    if (k >= NPART || engine >= NENGINES || s_frozen) return;
    engine_lock();
    track_t *t = &trk[k];
    if (t->eng_req != engine) set_engine_of_locked(t, engine);
    if (t->preset != preset) preset_apply_locked(t, preset);
    engine_unlock();
}

/* 预览：同一 p[] 基底上的纯写值，无副作用（不 sound_change、不 panic、不碰 FM6 状态）。
 * 参数锁在生效时与 set_engine_of 的实际写值不同（后者先 locks_restore），此时只作参考。
 * 返回 false = 冻结态（console 打 `ENG PRESET ERR frozen`）或参数越界；out 已清零。 */
bool sloop_core_preset_preview(uint32_t track, uint32_t engine, uint32_t preset,
                               int16_t out[SLOOP_CORE_P_COUNT])
{
    const track_t *t;
    int keep_arp, kbd_hold;
    if (out == NULL) return false;
    memset(out, 0, SLOOP_CORE_P_COUNT * sizeof *out);
    if (track >= NTRK || engine >= NENGINES) return false;
    if (s_frozen) return false;
    engine_lock();
    if (s_frozen) { /* 取锁后复核（与 read_params 同口径）：冻结在持锁时置位 */
        engine_unlock();
        return false;
    }
    t = &trk[track];
    if (is_drum(t)) { /* 鼓轨没有「声音」（apply_preset_to 同样直接返回）：p[] 不变 */
        memcpy(out, t->p, sizeof t->p);
        engine_unlock();
        return true;
    }
    keep_arp = t->p[P_AMODE] != 0 && !arp_is_sounds(t);
    kbd_hold = !t->p[P_AMODE] && t->p[P_AHOLD];
    preset_values(out, t, engine, preset, keep_arp, kbd_hold);
    engine_unlock();
    return true;
}

/* 上游 project.c 把 proj_v3_tmp 声明成普通静态（.bss，在自检快照窗口内）——这里用
 * 后置 section 属性把它移进 .noinit（见文件头）。`sizeof *s_v3_ps` = 2,584B。 */
static project_v3_t *s_v3_ps __attribute__((section(".noinit")));

/* ---- 存储状态：全部 .noinit（自检 restore 会回滚本 TU 的 .data/.bss） ------------ */
/* 冷启动的 .noinit 内容是未定义的，而 console REPL 任务在 sloop_core_init 之前就已启动：
 * 用魔数哨兵把「init 已完成」与「.noinit 垃圾」分开。哨兵还必须与 HAL 的 ready 一起判
 * ——`.noinit` 跨热复位存活（哨兵/指针是上一次会话的），而 HAL 的 s_ready 在 .bss、
 * 热复位后是 false；只看哨兵的话热复位后、init 之前会拿悬垂指针与信号量去操作。
 * 因此 sloop_store_ready() 是所有公共 API 的唯一门。 */
#define STORE_INIT_MAGIC 0x534C4F50u /* "SLOP" */
static volatile uint32_t s_init_magic __attribute__((section(".noinit")));
static SemaphoreHandle_t s_store_lock __attribute__((section(".noinit")));
static bool s_ready __attribute__((section(".noinit")));

static void store_lock(void)
{
    if (s_store_lock != NULL) xSemaphoreTake(s_store_lock, portMAX_DELAY);
}
static void store_unlock(void)
{
    if (s_store_lock != NULL) xSemaphoreGive(s_store_lock);
}

/* 走带中拒写/拒载：对齐上游 "STOP BEFORE SAVE" */
static bool store_playing(void)
{
    return song.playing || transport_req;
}

/* 所有公共 API 的唯一门（load 也在内）：哨兵 + init 完成 + 三个 PSRAM 实体在位 + HAL 分区
 * 就绪。HAL 的 ready 在 .bss，热复位后为 false ⇒ 即使 .noinit 的哨兵/指针还是上一次会话
 * 的，init 之前一律 no_storage（不碰悬垂指针与信号量）。 */
bool sloop_store_ready(void)
{
    return s_init_magic == STORE_INIT_MAGIC && s_ready && sloop_hal_storage_ready() &&
           s_slot_ps != NULL && s_stage != NULL && s_v3_ps != NULL;
}

esp_err_t sloop_store_init(void)
{
    s_init_magic = 0;
    s_ready = false;
    flash_ok = 0;
    /* 开机单次调用（ADR-009 §5 启动顺序），不做重复调用保护（与 HAL init 同约定） */
    s_store_lock = xSemaphoreCreateMutex();
    if (s_store_lock == NULL) return ESP_ERR_NO_MEM;

    /* 上游三个大实体进 PSRAM（ADR-009 §7）；任一失败 → ready=false（不崩溃） */
    s_v3_ps = heap_caps_malloc(sizeof *s_v3_ps, MALLOC_CAP_SPIRAM);
    s_slot_ps = heap_caps_malloc(SLOOP_STORE_PROJECTS * sizeof(project_t), MALLOC_CAP_SPIRAM);
    s_stage = heap_caps_malloc(sizeof *s_stage, MALLOC_CAP_SPIRAM);
    if (s_v3_ps == NULL || s_slot_ps == NULL || s_stage == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配失败: v3=%p slots=%p stage=%p", (void *)s_v3_ps, (void *)s_slot_ps,
                 (void *)s_stage);
        return ESP_ERR_NO_MEM;
    }
    memset(s_slot_ps, 0, SLOOP_STORE_PROJECTS * sizeof(project_t));

    esp_err_t ret = sloop_hal_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "存储后端初始化失败: %s（ready=false，上层按空存储返回）", esp_err_to_name(ret));
        return ret;
    }

    /* 开机 fetch：四个槽 st_load → 暂存 → proj_import（上游 proj_fetch 同语义） */
    for (uint32_t i = 0; i < SLOOP_STORE_PROJECTS; i++) {
        int n = st_load(OBJ_PROJECT0 + i, &proj_tmp, sizeof proj_tmp);
        if (!proj_import(&proj_slot[i], &proj_tmp, n)) proj_slot[i].magic = 0;
    }

    s_ready = true;
    flash_ok = 1;
    /* 用户预置银行的开机扫描（上游 persist_boot 同语义）：把两个银行在 XIP 窗口里的
     * 当前副本地址装进 up_bank_xip。必须在 flash_ok=1 之后（up_bank_find 读它）。 */
    up_boot();
    s_init_magic = STORE_INIT_MAGIC; /* 最后置位：半途失败的 init 不能被看见 */
    ESP_LOGI(TAG, "ready: slots=%p(%uB) stage=%p v3=%p(%uB) base=0x%lx span=0x%lx",
             (void *)s_slot_ps, (unsigned)(SLOOP_STORE_PROJECTS * sizeof(project_t)), (void *)s_stage,
             (void *)s_v3_ps, (unsigned)sizeof *s_v3_ps, (unsigned long)sloop_hal_storage_base(),
             (unsigned long)sloop_hal_storage_span());
    return ESP_OK;
}

void sloop_store_stats(sloop_store_stats_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->ready = sloop_store_ready();
    out->faults_armed = sloop_hal_storage_fault_armed() ? 1u : 0u;
    out->faults_taken = sloop_hal_storage_fault_taken();
    if (!out->ready) return;
    /* objects：flash 里能读出「头合法 + payload CRC 对」且**非空提交**（h.len != 0）的工程
     * 对象数——erase 写的是 len=0 的空对象（与上游 st_save(obj,NULL,0) 同语义），不算对象。
     * st_current 借用 st_buf（main_scratch），故与写路径共用存储锁。 */
    store_lock();
    for (uint32_t i = 0; i < SLOOP_STORE_PROJECTS; i++) {
        st_hdr_t h;
        if (st_current(OBJ_PROJECT0 + i, &h) >= 0 && h.len != 0) out->objects_ok++;
    }
    store_unlock();
}

bool sloop_store_project_used(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS || !sloop_store_ready()) return false;
    return proj_ok(&proj_slot[slot]) != 0;
}

uint32_t sloop_store_project_hash(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS || !sloop_store_ready()) return 0;
    const project_t *p = &proj_slot[slot];
    return proj_ok(p) ? p->sum : 0u;
}

uint32_t sloop_store_live_hash(void)
{
    if (!sloop_store_ready()) return 0;
    store_lock();
    engine_lock();
    proj_capture(&proj_tmp.v6);
    engine_unlock();
    uint32_t sum = proj_tmp.v6.sum;
    store_unlock();
    return sum;
}

esp_err_t sloop_store_project_save(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    engine_lock();
    proj_capture(&proj_slot[slot]);
    engine_unlock();
    /* 擦/写 flash 期间不持引擎锁（ADR-009 §3 锁序）；失败时 RAM 槽保留新值（上游语义） */
    int rc = st_save(OBJ_PROJECT0 + slot, &proj_slot[slot], sizeof(project_t));
    store_unlock();
    if (rc != 0) {
        ESP_LOGE(TAG, "PROJ %u SAVE 失败 rc=%d", (unsigned)slot, rc);
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

esp_err_t sloop_store_project_load(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    project_t *p = &proj_slot[slot];
    if (!proj_ok(p)) { /* RAM 槽无效才回落 flash（上游 project_load） */
        int n = st_load(OBJ_PROJECT0 + slot, &proj_tmp, sizeof proj_tmp);
        if (!proj_import(p, &proj_tmp, n)) p->magic = 0;
    }
    if (!proj_ok(p)) {
        store_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    engine_lock();
    /* 上游 project_apply 的前置（spec §4.2：load = 停走带 + panic 全部轨）：接下来换成
     * 另一份工程，正在发声的音符必须释放、走带请求置停，鼓静音与 MIDI 表情不随工程走。 */
    transport_req = 2;
    panic_req = (uint8_t)((1u << NTRK) - 1u);
    drum_mute = 0;
    for (uint32_t k = 0; k < NTRK; k++) ex_off[k] = 0;
    proj_apply(p, 1);
    song.sel = (uint8_t)(p->sel < NTRK ? p->sel : 0u);
    /* 上游 project_apply 的后两步（project.c:663-668）：格式 1 工程里 preset == 0xFF
     * 的轨（旧格式没有预置号）取默认声音并清图案；随后把「本固件没有的引擎」的轨
     * 换回默认声音（proj_apply 已把引擎号绕回，参数是别人的）。 */
    for (uint32_t k = 0; k < NPART; k++)
        if (p->t[k].preset == 0xFFu) {
            preset_apply_locked(&trk[k], TRK_DEF[k][1]);
            steps_clear(&trk[k]);
        }
    proj_foreign_parts(p);
    engine_unlock();
    store_unlock();
    return ESP_OK;
}

esp_err_t sloop_store_project_erase(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    /* 空对象（len=0）＝编辑器的 restore 留下的空槽；proj_fetch 读作「无工程」 */
    int rc = st_save(OBJ_PROJECT0 + slot, NULL, 0);
    if (rc == 0) memset(&proj_slot[slot], 0, sizeof(project_t));
    store_unlock();
    if (rc != 0) {
        ESP_LOGE(TAG, "PROJ %u ERASE 失败 rc=%d", (unsigned)slot, rc);
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

/* ---- 诊断：内存工程 + 触发 project_load 的两条 fallback 分支（R18，测试专用） ------ */
/* 目录里没有「构造任意工程」的上游入口（FUN1..5 的旧格式 fixture 需要外部文件），而
 * project_load 的两条 fallback 在真机上原本不可达：
 *   ① 格式 1 工程的 `preset == 0xFF` 轨（旧格式没有预置号）→ 取 `TRK_DEF[k][1]` 的默认
 *      声音 + `steps_clear`（上游 project.c:663-667）；
 *   ② `proj_foreign_parts`：轨的引擎号 ≥ NENGINES（Clipper 写的 HYBRID/FMX…）→
 *      `proj_apply` 已把引擎号绕回，这里换回该轨默认引擎+预置、图案与混音保留（上游
 *      project.c:639-647）。
 * 本函数以当前引擎状态捕获一份工程 → 按 case 改写要验的那个边界 → `st_save` 到**诊断
 * 槽 3**（破坏性：覆盖该槽 RAM+flash 对象；槽 0–2 不碰）→ 清 RAM 槽强制走 flash 回落
 * （`st_load` + `proj_import`，顺带验证 FUN6 序列化保留 `preset == 0xFF` 与越界引擎号）
 * → 走公共 `sloop_store_project_load` → 回读各轨结果。console 只做回显。 */
esp_err_t sloop_store_project_synth(const char *case_name, sloop_store_synth_t *out)
{
    if (out == NULL || case_name == NULL) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    bool fmt1 = strcmp(case_name, "fmt1") == 0;
    bool foreign = strcmp(case_name, "foreign") == 0;
    if (!fmt1 && !foreign) return ESP_ERR_INVALID_ARG;

    const uint32_t slot = SLOOP_STORE_PROJECTS - 1u; /* 诊断槽 3 */
    project_t *p = &proj_slot[slot];
    store_lock();
    engine_lock();
    proj_capture(p);
    engine_unlock();
    if (fmt1) {
        p->t[0].engine = 0;       /* 格式 1 只缺「预置号」：引擎仍来自工程（ANALOG） */
        p->t[0].preset = 0xFFu;   /* 没有预置号 → load 必须取默认预置（TRK_DEF[0][1] = 0） */
        p->t[0].p[P_ATK] = 99;    /* 预置应用必须把它盖回 808 BOOM 的 0（可观测） */
        p->t[0].e[0] = 99;        /* 同上：ANALOG 的 WAVE 回到 3 */
        memset(&p->t[0].step[3], 0, sizeof p->t[0].step[3]);
        p->t[0].step[3].n = 1;    /* 工程里有一个音：fmt1 分支会 steps_clear（→ 0 步） */
        p->t[0].step[3].note[0] = 60;
        p->t[0].step[3].time = ST_NOTE;
        p->t[0].step[3].vel = 100;
        p->t[1].engine = 1;       /* 对照：轨 1 不带 0xFF，必须**保留**工程里的引擎/预置 */
        p->t[1].preset = 1;
    } else {
        p->t[1].engine = (uint8_t)(NENGINES + 2u); /* 本固件没有的引擎（proj_apply 会绕回 2） */
        p->t[1].preset = 3;
        p->t[1].p[P_ATK] = 99;    /* 未知引擎的参数必须被默认声音覆盖 */
        p->t[1].e[0] = 99;
        memset(&p->t[1].step[5], 0, sizeof p->t[1].step[5]);
        p->t[1].step[5].n = 1;    /* 图案保留（上游：its pattern and mix stay） */
        p->t[1].step[5].note[0] = 64;
        p->t[1].step[5].time = ST_NOTE;
        p->t[1].step[5].vel = 100;
    }
    p->sum = proj_sum(p);         /* proj_ok 校验 sum：改写后必须重算 */
    int rc = st_save(OBJ_PROJECT0 + slot, p, sizeof *p);
    if (rc == 0) memset(p, 0, sizeof *p); /* 强制 load 走 flash 回落（st_load + proj_import） */
    store_unlock();
    if (rc != 0) {
        ESP_LOGE(TAG, "PROJ %u SYNTH 保存失败 rc=%d", (unsigned)slot, rc);
        return ESP_ERR_NOT_FINISHED;
    }

    esp_err_t err = sloop_store_project_load(slot);
    if (err != ESP_OK) return err;

    memset(out, 0, sizeof *out);
    engine_lock();
    for (uint32_t k = 0; k < NTRK; k++) {
        const track_t *t = &trk[k];
        out->trk[k].engine = t->eng_req;
        out->trk[k].preset = t->preset;
        out->trk[k].atk = t->p[P_ATK];
        out->trk[k].e0 = t->p[P_E0];
        if (is_drum(t)) {
            for (uint32_t i = 0; i < NSTEP; i++)
                out->trk[k].steps += (t->dstep[i].on[0] | t->dstep[i].on[1]) != 0 ? 1u : 0u;
        } else {
            for (uint32_t i = 0; i < NSTEP; i++) out->trk[k].steps += t->step[i].n != 0 ? 1u : 0u;
        }
    }
    engine_unlock();
    return ESP_OK;
}

/* ---- 用户预置 0..31（upreset.c）--------------------------------------------------- */
/* 上游语义（upreset.c）：
 *   up_store(k, 0)      = 选中轨的声音 → 槽 k（鼓轨返回 1）；
 *   up_load_to(t, k)    = 槽 k → 轨 t 的声音（引擎 + 除混音/图案参数外的参数 + PTCH；
 *                         空槽/鼓轨返回 1；不写 flash，可在走带中调用）；
 *   up_put(k, NULL)     = 擦槽 k（重写整个银行对象，并把引用该槽的轨的 user 清 0）。
 * 锁序与工程一致（ADR-009 §3）：存储锁 → 引擎锁，**flash 擦写期间不持引擎锁**——本板
 * 的音频渲染就持引擎锁（sloop_core_render），持锁 50ms 必然 xrun。故捕获/应用到引擎的
 * 半段在引擎锁内，up_put 的擦写在锁外。
 * up_put 的返回码：0 成功 / 2 flash 擦写或回读失败 / 3 无 flash。
 *
 * 只读入口与写入口都先重扫一次银行索引：up_bank_xip[2] 是 upreset.c 的静态 .bss，本 TU
 * 的 .bss 在 `ENG SELFTEST` 的 SURROUND(sloop_selftest_bss) 快照窗口里，restore 会把它
 * 清成 NULL（flash_ok 同理，已放 .noinit），此后预置会全部读成空；而 up_put 是「把整
 * 银行重写一遍」，从 NULL 起笔会抹掉同银行的其它槽（实测：selftest 后 SAVE 1 把槽 0
 * 抹掉）。上游的声明没有 section 属性，宏重定向搬不走它，故每个入口在存储锁内 up_boot()
 * 一次（2 次头校验 + 3,080B CRC，微秒级）；这也让「刚写下的预置立刻可见」。 */

/* 上游 up_store 的「捕获」半段（upreset.c:259-286，逐行同义）：记录 = 引擎、名字
 * （默认引擎名 + 槽号，如 "ANALOG 01"）、P_COUNT 个参数、前 16 步图案。 */
static void preset_capture(const track_t *t, uint32_t k, up_rec_t *r)
{
    char b[16], l[4];
    uint32_t i;
    memset(r, 0, sizeof *r);
    r->used = UP_USED;
    r->ver = UP_VER;
    r->engine = t->eng_req;
    r->np = P_COUNT;
    str_cpy(b, ENGINES[t->eng_req]->name, 9);
    up_slot_label(l, k);
    str_cpy(b + str_len(b), " ", 2);
    str_cpy(b + str_len(b), l + 1, 3);
    for (i = 0; i < 12u && b[i]; i++)
        r->name[i] = b[i];
    for (i = 0; i < P_COUNT; i++)
        r->p[i] = t->p[i];
    up_pat_from(r, t->step);
}

/* up_put 的返回码 → esp_err_t（3 = 无 flash：本平台上只在 HAL 未就绪时出现，而公共
 * API 已由 sloop_store_ready() 挡住，保留映射以防后端降级）。 */
static esp_err_t preset_put_rc(int rc, const char *what, uint32_t slot)
{
    if (rc == 0) return ESP_OK;
    if (rc == 2) {
        ESP_LOGE(TAG, "PRESET %u %s 失败 rc=%d", (unsigned)slot, what, rc);
        return ESP_ERR_NOT_FINISHED;
    }
    ESP_LOGE(TAG, "PRESET %u %s 无 flash rc=%d", (unsigned)slot, what, rc);
    return ESP_ERR_INVALID_STATE;
}

uint32_t sloop_store_preset_count(void)
{
    if (!sloop_store_ready()) return 0;
    store_lock();
    up_boot();
    uint32_t n = up_count();
    store_unlock();
    return n;
}

bool sloop_store_preset_used(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PRESETS || !sloop_store_ready()) return false;
    store_lock();
    up_boot();
    bool used = up_used(slot) != 0;
    store_unlock();
    return used;
}

bool sloop_store_preset_name(uint32_t slot, char out[13])
{
    if (out == NULL) return false;
    out[0] = '\0';
    if (slot >= SLOOP_STORE_PRESETS || !sloop_store_ready()) return false;
    store_lock();
    up_boot();
    bool used = up_used(slot) != 0;
    if (used) up_name(slot, out); /* 上游 up_name：大写、0 结尾（13 字节） */
    store_unlock();
    return used;
}

uint8_t sloop_store_preset_engine(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PRESETS || !sloop_store_ready()) return 0xFF;
    store_lock();
    up_boot();
    uint8_t engine = up_used(slot) ? up_rec(slot)->engine : (uint8_t)0xFF;
    store_unlock();
    return engine;
}

/* 上游 up_ui（upreset.c:360-377）的判定顺序对 SAVE 是：先鼓轨（DRUM TRACK: NO SOUND），
 * 再播放守卫（STOP BEFORE SAVE）——顺序可观测：播放中选中鼓轨时回的是鼓轨令牌。
 * 故这里在引擎锁内一次判两件事（同一临界区里读 song.sel 与 song.playing，无 TOCTOU）。 */
esp_err_t sloop_store_preset_store(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PRESETS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    up_rec_t r;
    store_lock();
    up_boot(); /* 写路径也必须重扫：up_put 从当前 XIP 副本起笔，NULL 会抹掉同银行的其它槽 */
    engine_lock();
    if (is_drum(TSEL)) { /* 鼓轨没有「声音」可存（上游 SAVE > USER 页同一令牌，先于播放守卫） */
        engine_unlock();
        store_unlock();
        ui_message("DRUM TRACK: NO SOUND");
        return ESP_ERR_INVALID_ARG;
    }
    if (store_playing()) {
        engine_unlock();
        store_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    preset_capture(TSEL, slot, &r);
    engine_unlock();
    int rc = up_put(slot, &r); /* 擦/写 flash：不持引擎锁 */
    store_unlock();
    return preset_put_rc(rc, "SAVE", slot);
}

/* 上游 up_ui 的 LOAD 同样先拦鼓轨并出 DRUM TRACK: NO SOUND（upreset.c:360-363），空槽
 * 再回 EMPTY SLOT。本接口可指定任意轨，故按**目标轨**判定（缺省轨 = 选中轨，由 console
 * 传入）。load 不写 flash，因此不受播放守卫（上游 MIDI PC 语义）。 */
esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track)
{
    if (slot >= SLOOP_STORE_PRESETS || track >= NTRK) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (is_drum(&trk[track])) {
        ui_message("DRUM TRACK: NO SOUND");
        return ESP_ERR_INVALID_ARG;
    }
    store_lock();
    up_boot();
    bool used = up_used(slot) != 0;
    engine_lock();
    int rc = used ? up_load_to(&trk[track], slot) : 1; /* 引擎锁内：写 p[]、fade、panic 该轨 */
    engine_unlock();
    store_unlock();
    return rc == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sloop_store_preset_erase(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PRESETS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    up_boot(); /* 同上：擦除也是「把整银行重写一遍」，必须从当前副本起笔 */
    /* up_put 会清掉引用该槽的轨（trk[i].user）——那部分是引擎状态，先在引擎锁内做，
     * 于是 up_put 自己的清理循环成为空操作，flash 擦写落在锁外。 */
    engine_lock();
    for (uint32_t i = 0; i < NTRK; i++)
        if (trk[i].user == slot + 1u) trk[i].user = 0;
    engine_unlock();
    int rc = up_put(slot, NULL);
    store_unlock();
    return preset_put_rc(rc, "ERASE", slot);
}
