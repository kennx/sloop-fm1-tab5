/*
 * store_tab5.c — 上游 storage.c / project.c / ui.c 的 Tab5 适配层（ADR-009 §7/§8）
 *
 * 本文件由 selftest.c **末尾 include**，与引擎/DSP 同属一个翻译单元——所以可以直接把
 * vendored 的 storage.c（A/B 双扇区 + 32B 提交头 + CRC + 回读校验，原样）、project.c
 * （#define PROJ_HOST：格式层 FUN1..6 + proj_capture/proj_apply）、arranger_scene.c
 * （A–D RAM 场景应用与 SONG 工作 loop 暂存）与 upreset.c（用户预置
 * 32 槽；#define FELUCCA_FLASH 1 才走 st_xip）编进来，缺的符号在本文件顶部做薄 shim。
 * ui.c 的预置机制（apply_preset_to/set_engine_of/TRK_DEF/param_kept/arp_is_sounds）也
 * 按原语义移植在这里（换引擎/预置 = 换声音，见「换引擎/预置」小节）。
 * third_party/ 零修改。
 *
 * 内存（ADR-009 §7，fidelity-first）：
 *   - main_scratch（上游 st_buf，3,840B）留在**内部 SRAM**：擦/写期间 cache 关闭仍要访问；
 *   - proj_slot[4]（4×3,816=15,264B）、proj_v3_tmp（2,584B）、song_keep（3,816B）与
 *     自有暂存 proj_tmp（4,096B）全部在 PSRAM：include 上游声明**之前**用宏把对象退化成指针
 *     （`static project_v3_t proj_v3_tmp;` → `static project_v3_t (*s_v3_ps);`）。宏包裹的
 *     是整个对象，上游全部引用形如 `&obj[...]` / `sizeof obj[i]`，替换后取址与 sizeof
 *     语义不变；
 *   - 各 PSRAM 指针 + 存储锁 + ready/flash_ok 放 `.noinit`：`ENG SELFTEST` 的 selftest_restore()
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
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
 * 依赖它，省掉 RAM 镜像）。Tab5 上就是 sloopstore 的 mmap 窗口（ADR-009 §2）。
 * FM6 银行的 payload 从扇区偏移 **256** 起（storage.c 的 ST_PAYLOAD_OFF：32B 提交头 +
 * 提交头与 payload 之间的 side/留空区）——上游 fm6_bank.c 的宿主兜底正是
 * `fm1_xip_ptr(st_sector(OBJ_FM6BANK, copy) + ST_PAYLOAD_OFF)`。少了这 256 字节就是
 * 一个错位视图：GET 读到别的槽（前两槽落在提交头/空区），写路径从错位副本起笔会把
 * 0xFF 带进 payload、让整个银行校验失败（读成空）。 */
#define ST_XIP_PTR(off) (sloop_hal_storage_map() + ((off) - sloop_hal_storage_base()))
#define FM6_BANK_XIP(copy) (ST_XIP_PTR(0xE5000u + (copy) * 4096u + 256u))

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
_Static_assert(SLOOP_STORE_RAW_MAX == ST_PAYLOAD_MAX,
               "STORE RAW 的长度上限必须等于一个 flash 对象的 payload 上限");

#define PROJ_HOST 1
#include "project.c"
#undef PROJ_HOST

/* project.c 的 !PROJ_HOST 段不编入，故 arranger_scene.c 在这里显式补入。
 * song_keep 与 proj_v3_tmp 同法退化为 PSRAM 指针；原声明的 .pool 属性只适用于
 * 实体对象，不能留在指针上，故仅包住此 include 去掉，随后由后置 .noinit 声明定位。 */
#define song_keep (*s_song_keep_ps)
#define __attribute__(x)
#include "arranger_scene.c"
#undef __attribute__
#undef song_keep

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

/* RAW 上传专用暂存（4,096B PSRAM，P4-D 评审 I1）：**不能**与 proj_tmp 共用——
 * dsu_reload/song_reload/sections_tick/song_write_locked 都会把 settings 读进
 * proj_tmp，若共用，`STORE RAW BEGIN … → 自检/歌曲 tick → END` 会把被覆盖的字节当成
 * 上传内容写进 flash（CRC 合法但内容是错的）。独立缓冲让 RAW 的半截上传跨过这些
 * 读操作与 `ENG SELFTEST`（状态在 .noinit，指针也跨自检存活）。失败时 ready=false。 */
#define STORE_RAW_STAGE_SIZE 4096u
static uint8_t *s_raw_stage __attribute__((section(".noinit")));
_Static_assert(STORE_RAW_STAGE_SIZE >= SLOOP_STORE_RAW_MAX, "RAW 暂存装得下一个 flash 对象");

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

/* ---- 上游 FM6 银行（fm6_bank.c，27 槽 / 一个 A/B 对象） --------------------------- */
/* fm6_bank.c 只被上游 project.c 在 `FELUCCA_FLASH && !PROJ_HOST` 下 include；本 TU 用
 * PROJ_HOST 编 project.c，故这里显式编入（上面的 FM6_BANK_XIP 把 0xE5000 + copy×4096
 * 的 payload 落到 sloopstore 的 mmap 窗口）。它需要：proj_tmp（暂存 ≥ 3,472B，文件内有
 * 静态断言）、flash_ok、st_buf/st_current/st_save（storage.c）与 eng_fm6.c 的
 * fm6_unpack/fm6_pack/fm6_slot/fm6_bank_read（本 TU 由 engines.c → eng_fm6.c 编入，
 * 都在上面可见）。`fm6_bank_read` 钩子由 fm6_bank_boot() 安装（下面 init 里调）。 */
#include "fm6_bank.c"

/* PER3 歌曲子集（Task 7，ADR-009 §6）：纯字节层放在 song_persist.h，必须在 storage.c
 * （st_crc32）与 drums.c → drum_synth.c（dsu/dsu_defaults）之后 include——
 * scripts/persist-host-test.c 用同一顺序编译，故固件与 host 测试看到同一份默认 DSU/CRC。 */
#include "song_persist.h"

/* settings 生效副本与尾段默认布局（P4-E Task 2）：实体在 settings_tab5.c（本文件之后
 * include），song_write_locked 在引擎锁内读它们，故先给出前向的 tentative 定义。 */
static sloop_settings_t s_set;
static uint8_t s_midi_def[SONG_MIDI_MAX];
static void settings_written_locked(bool ok, const song_settings_t *written);

_Static_assert(SLOOP_STORE_BANK_SLOTS == FM6_BANK_N, "公共 API 的银行槽数与上游 fm6_bank.c 一致");
_Static_assert(SLOOP_STORE_BANK_PACKED == FM6_PACKED, "公共 API 的 packed 记录长度与上游一致");

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
static project_t *s_song_keep_ps __attribute__((section(".noinit")));

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

/* ---- 自动存档（上游 project.c 的 autosave_tick/autosave_resume，Task 6） ----------- */
/* 全部状态放 `.noinit`：本 TU 的 .bss 在 `ENG SELFTEST` 的快照窗口里，restore 会把
 * hash/时间戳清零——清零后自检结束的第一拍就会把工厂态写回 autosave（覆盖用户工程）。 */
#define AUTOSAVE_IDLE       2500u /* 上游 AUTOSAVE_IDLE：ms without input */
#define AUTOSAVE_GAP        20000u /* 上游 AUTOSAVE_GAP：ms between two saves at least */
#define AUTOSAVE_PERIOD     1000u /* 上游 now - autosave_checked < 1000u */
#define AUTOSAVE_TASK_STACK 3072
#define AUTOSAVE_TASK_PRIO  1

/* 捕获/恢复暂存（上游 autosave_buf，3,816B）：与 proj_tmp（st_load/up_put 的暂存）
 * 分开，tick 的 st_save 与其它存储操作才不会互相踩。PSRAM（ADR-009 §7）。 */
static project_t *s_auto __attribute__((section(".noinit")));
static uint32_t s_auto_hash __attribute__((section(".noinit")));    /* 上次写入/开机恢复的 hash */
static uint32_t s_auto_ms __attribute__((section(".noinit")));      /* 上次尝试写（gap 起点） */
static uint32_t s_auto_checked __attribute__((section(".noinit"))); /* 上次 tick 检查（1s 节流） */
static uint32_t s_auto_writes __attribute__((section(".noinit")));  /* 本次开机的写入计数 */
static uint32_t s_last_input_ms __attribute__((section(".noinit"))); /* note_activity 的时间戳 */
static volatile bool s_auto_on __attribute__((section(".noinit")));
static volatile bool s_busy __attribute__((section(".noinit"))); /* 有线程持存储锁（tick 的「store busy」） */

/* ---- PER3 歌曲（Task 7）：dirty 记账与延迟写的时间戳（全部 .noinit） ---------------
 * `s_song` 是 song_persist.h 的 song_pending_t（dirty + 代次）：本 TU 的 .bss 在
 * `ENG SELFTEST` 的快照窗口里，restore 会把 dirty 清掉 ⇒ 一次自检就会丢掉「歌曲改了
 * 还没落盘」。`s_input_ms` 用 esp_timer 毫秒（与 autosave 的 FreeRTOS tick 各记各的，
 * 见 sloop_store_sections_tick 的说明）。`s_song_refused` 让「设置对象损坏」只告警一次。 */
static song_pending_t s_song __attribute__((section(".noinit")));
static uint32_t s_sections_tried_ms __attribute__((section(".noinit")));
static uint32_t s_input_ms __attribute__((section(".noinit")));
static bool s_song_refused __attribute__((section(".noinit")));
/* live sections A–D 的 dirty 位（P4-C Task 8）：捕获在 seq_tab5.c（引擎锁内），flash
 * 与歌曲共用下面 tick 的停止/安静/idle 门限；.noinit 与 s_song 同理（自检 restore 不得
 * 把「还没落盘」抹成已落盘）。 */
static volatile uint8_t s_sec_dirty __attribute__((section(".noinit")));
/* section capture 的代次：erase 擦写期间若又有 live capture（gen 变化），保留新捕获与
 * dirty 位，不 memset 掉它（flash 里的空对象由下一个 tick 重写）。与 s_sec_dirty 同一把
 * 引擎锁访问；只需在单次 erase 调用内比较，故留在 .bss。 */
static volatile uint32_t s_sec_gen[SLOOP_STORE_PROJECTS];

/* 上一次写动作（st_save / up_put / fm6_bank_put）的代价：spec §4.4/§5 要求 STORE STATS
 * 报告每次保存的 xrun 增量与耗时（擦除会关 cache、音频任务停顿，xrun 必然增长）。
 * .noinit：自检 restore 不回滚，诊断值跨自检保留；冷启动由 init 归零。 */
static int32_t s_last_xrun __attribute__((section(".noinit")));
static uint32_t s_last_save_ms __attribute__((section(".noinit")));

/* 延迟写（sections/歌曲）用的毫秒时钟：esp_timer 单调、不依赖 fm1_ms（本固件只在流式
 * 渲染时推进 fm1_ms，停流就冻住）。autosave 仍用 FreeRTOS tick 毫秒（它的 idle 门限与
 * `sloop_store_note_activity` 的调用方同一时钟），两者各记各的时间戳，互不换算。 */
static uint32_t store_esp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void store_note_write(uint32_t xr0, int64_t us0)
{
    s_last_xrun = (int32_t)((int32_t)sloop_hal_audio_xruns() - (int32_t)xr0);
    s_last_save_ms = (uint32_t)((esp_timer_get_time() - us0) / 1000);
}
static TaskHandle_t s_auto_task __attribute__((section(".noinit"))); /* 1s 任务句柄（失败路径读栈余量） */

static void autosave_task(void *arg);

static void store_lock(void)
{
    if (s_store_lock != NULL) xSemaphoreTake(s_store_lock, portMAX_DELAY);
    s_busy = true;
}
static void store_unlock(void)
{
    s_busy = false;
    if (s_store_lock != NULL) xSemaphoreGive(s_store_lock);
}
/* tick 用：拿不到锁就跳过（下个周期再来），不阻塞 1s 任务也不与写者排队 */
static bool store_trylock(void)
{
    if (s_store_lock == NULL || xSemaphoreTake(s_store_lock, 0) != pdTRUE) return false;
    s_busy = true;
    return true;
}

/* 走带中拒写/拒载：对齐上游 "STOP BEFORE SAVE" */
static bool store_playing(void)
{
    return song.playing || transport_req;
}

/* 所有公共 API 的唯一门（load 也在内）：哨兵 + init 完成 + 五类 PSRAM 实体在位 + HAL 分区
 * 就绪。HAL 的 ready 在 .bss，热复位后为 false ⇒ 即使 .noinit 的哨兵/指针还是上一次会话
 * 的，init 之前一律 no_storage（不碰悬垂指针与信号量）。 */
bool sloop_store_ready(void)
{
    return s_init_magic == STORE_INIT_MAGIC && s_ready && sloop_hal_storage_ready() &&
           s_slot_ps != NULL && s_stage != NULL && s_raw_stage != NULL && s_v3_ps != NULL &&
           s_auto != NULL && s_song_keep_ps != NULL;
}

esp_err_t sloop_store_init(void)
{
    /* 上游 persist_boot 的第一步（那个函数在本 TU 不编）：歌曲**先**取默认。本函数下面
     * 任何一步失败（互斥量/PSRAM/分区）都会提前返回，而 `arrangement` 是 arranger.c 的
     * static `.bss`——不留默认就是 count=0 的非法歌曲，日志却说「保持默认歌曲」。
     * 默认 = arr_defaults：count=4、entry 0..3 = A/B/C/D 各 4 小节。 */
    arr_defaults(&arrangement);
    s_init_magic = 0;
    s_ready = false;
    flash_ok = 0;
    s_auto_on = true; /* 默认 ON（上游行为）；console `STORE AUTO OFF` 是测试开关 */
    s_auto_hash = 0;
    s_auto_ms = 0;
    s_auto_checked = 0;
    s_auto_writes = 0;
    s_last_xrun = 0;
    s_last_save_ms = 0;
    s_last_input_ms = 0;
    s_input_ms = 0;
    memset(&s_song, 0, sizeof s_song);
    s_sections_tried_ms = 0;
    s_song_refused = false;
    s_sec_dirty = 0;
    memset((void *)s_sec_gen, 0, sizeof s_sec_gen);
    s_busy = false;
    s_auto_task = NULL;
    sloop_store_raw_abort(); /* R27：RAW 上传状态在开机时归零（.noinit 是冷启动垃圾） */
    /* 开机单次调用（ADR-009 §5 启动顺序），不做重复调用保护（与 HAL init 同约定） */
    s_store_lock = xSemaphoreCreateMutex();
    if (s_store_lock == NULL) return ESP_ERR_NO_MEM;

    /* 上游大实体进 PSRAM（ADR-009 §7、ADR-010）；任一失败 → ready=false（不崩溃） */
    s_v3_ps = heap_caps_malloc(sizeof *s_v3_ps, MALLOC_CAP_SPIRAM);
    s_slot_ps = heap_caps_malloc(SLOOP_STORE_PROJECTS * sizeof(project_t), MALLOC_CAP_SPIRAM);
    s_stage = heap_caps_malloc(sizeof *s_stage, MALLOC_CAP_SPIRAM);
    s_raw_stage = heap_caps_malloc(STORE_RAW_STAGE_SIZE, MALLOC_CAP_SPIRAM);
    s_auto = heap_caps_malloc(sizeof *s_auto, MALLOC_CAP_SPIRAM);
    s_song_keep_ps = heap_caps_malloc(sizeof *s_song_keep_ps, MALLOC_CAP_SPIRAM);
    if (s_v3_ps == NULL || s_slot_ps == NULL || s_stage == NULL || s_raw_stage == NULL ||
        s_auto == NULL || s_song_keep_ps == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配失败: v3=%p slots=%p stage=%p raw=%p auto=%p song_keep=%p",
                 (void *)s_v3_ps, (void *)s_slot_ps, (void *)s_stage, (void *)s_raw_stage,
                 (void *)s_auto, (void *)s_song_keep_ps);
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
    /* FM6 银行同理（上游 persist_boot）：找到当前副本、扫 used 位、装好 fm6_bank_read
     * 钩子——否则 PTCH ≥ 8 播 init voice。 */
    fm6_bank_boot();
    /* 采样槽开机装载（P4-D Task 4，spec §4.2）：四槽 flash → PSRAM 镜像 + `smp_user_scan`。
     * 在公开 READY（`s_init_magic`）置位**之前**跑完（半装载的镜像不能被公共 API 看见；
     * reload 只要求 HAL 与镜像在位）。镜像已由 sloop_core_init 填 0xFF，读失败的槽保持空。 */
    for (uint32_t i = 0; i < SLOOP_STORE_SAMPLES; i++) sloop_store_sample_reload(i);
    ESP_LOGI(TAG, "采样槽装载: %u 槽 flash→镜像", (unsigned)SLOOP_STORE_SAMPLES);
    s_init_magic = STORE_INIT_MAGIC; /* 最后置位：半途失败的 init 不能被看见 */
    /* 歌曲顺序从 settings 读回（上游 persist_boot 的 arrangement 段）：必须在 READY
     * 置位之后（reload 走公共门），且不影响 `ENG SELFTEST` 的工厂态基线（快照在 init
     * 之前拍完）。读不到/对象损坏时 reload 自己落回函数开头那个默认歌曲（不覆盖对象）。 */
    esp_err_t song_ret = sloop_store_song_reload();
    if (song_ret != ESP_OK && song_ret != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "歌曲设置读取失败: %s（保持默认歌曲 count=%u loop=%u）", esp_err_to_name(song_ret),
                 (unsigned)arrangement.count, (unsigned)arrangement.loop);
    /* SYN kit 银行同理（P4-D Task 5，spec §3.4，上游 dsu_boot）：settings 88..1,551 → dsu。
     * 不走公共写门也能跑（只读 flash + 覆盖 RAM 银行），但要在 READY 之后——与歌曲同一
     * 个「开机以 flash 为准」的时点。读不到就是工厂 SYN kit（sloop_core_init 已先 dsu_defaults）。 */
    esp_err_t dsu_ret = sloop_store_dsu_reload();
    if (dsu_ret != ESP_OK && dsu_ret != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "SYN kit 读取失败: %s（落工厂 SYN kit）", esp_err_to_name(dsu_ret));
    /* settings 生效副本（P4-E Task 2，spec §8.2）：palette/lowcut/zoom/lights/尾段 reso +
     * side theme → RAM 副本，lowcut/reso/REC 三项应用到上游 static；没有对象 = 默认。
     * 同样在 READY 之后（公共门）。 */
    sloop_store_settings_reload();
    /* 1s 周期任务**不在这里创建**：它必须在 sloop_store_autosave_resume 之后才起跑，
     * 否则「resume 之前的那一拍」可能把工厂态写进 autosave（覆盖随后要恢复的工程）。
     * 入口是 sloop_store_autosave_start()，由 sloop_core_init 在 resume 之后调用。 */
    ESP_LOGI(TAG, "ready: slots=%p(%uB) stage=%p v3=%p(%uB) auto=%p(%uB) base=0x%lx span=0x%lx",
             (void *)s_slot_ps, (unsigned)(SLOOP_STORE_PROJECTS * sizeof(project_t)), (void *)s_stage,
             (void *)s_v3_ps, (unsigned)sizeof *s_v3_ps, (void *)s_auto, (unsigned)sizeof *s_auto,
             (unsigned long)sloop_hal_storage_base(), (unsigned long)sloop_hal_storage_span());
    return ESP_OK;
}

void sloop_store_stats(sloop_store_stats_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->ready = sloop_store_ready();
    out->faults_armed = sloop_hal_storage_fault_armed() ? 1u : 0u;
    out->faults_taken = sloop_hal_storage_fault_taken();
    if (!out->ready) return; /* 未 init 时 autosave_* 保持 memset 的 0（.noinit 是冷启动垃圾） */
    out->autosave_on = s_auto_on;
    out->autosave_hash = s_auto_hash;
    out->autosave_writes = s_auto_writes;
    out->last_save_xrun = s_last_xrun;
    out->last_save_ms = s_last_save_ms;
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
    if (s_frozen) return ESP_ERR_INVALID_STATE; /* 诊断冻结：不做任何状态改动 */
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    store_lock();
    engine_lock();
    proj_capture(&proj_slot[slot]);
    engine_unlock();
    /* 擦/写 flash 期间不持引擎锁（ADR-009 §3 锁序）；失败时 RAM 槽保留新值（上游语义） */
    int rc = st_save(OBJ_PROJECT0 + slot, &proj_slot[slot], sizeof(project_t));
    /* 不清 section dirty 位：擦写期间可能又有 live STORE 捕获同一槽（上游 project_save
     * 同样不碰 sec_dirty）。多写一次是安全的，清掉新捕获才是丢数据。 */
    store_unlock();
    store_note_write(xr0, us0);
    if (rc != 0) {
        ESP_LOGE(TAG, "PROJ %u SAVE 失败 rc=%d", (unsigned)slot, rc);
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

/* 上游 project.c 的 project_apply（UI 级：停走带 + panic + 应用整份工程 + 两条 fallback）。
 * 调用方必须持存储锁且**不持引擎锁**；本函数内部取一次引擎锁。section_load 与普通
 * PROJECT LOAD 共用，避免第二份「应用工程」语义。 */
static void store_project_apply_locked(const project_t *p)
{
    engine_lock();
    /* 接下来换成另一份工程，正在发声的音符必须释放、走带请求置停，鼓静音与 MIDI 表情
     * 不随工程走（上游 project_apply）。 */
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
}

esp_err_t sloop_store_project_load(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE; /* 诊断冻结：LOAD 会改引擎/存储缓存 */
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    project_t *p = &proj_slot[slot];
    if (!proj_ok(p)) { /* RAM 槽无效才回落 flash（上游 project_load） */
        int n = st_load(OBJ_PROJECT0 + slot, &proj_tmp, sizeof proj_tmp);
        if (n <= 0) { /* 没有有效副本 / 空对象（len=0）：空槽（与「对象存在但格式不认识」区分） */
            store_unlock();
            return ESP_ERR_NOT_FOUND;
        }
        if (!proj_import(p, &proj_tmp, n)) {
            p->magic = 0;
            store_unlock();
            return ESP_ERR_INVALID_ARG; /* 截断/损坏的旧格式 fixture：LOAD 回 invalid */
        }
    }
    if (!proj_ok(p)) {
        store_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    store_project_apply_locked(p);
    store_unlock();
    return ESP_OK;
}

esp_err_t sloop_store_project_erase(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    /* 上游 project_erase：槽本来就空 ⇒ 只提示 EMPTY SECTION、**不写 flash**（省一次
     * 擦写；也是「擦空槽」的幂等语义）。空槽在 flash 里是 len=0 的空对象。 */
    if (!proj_ok(&proj_slot[slot])) {
        store_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    engine_lock();
    uint32_t gen0 = s_sec_gen[slot];
    engine_unlock();
    int rc = st_save(OBJ_PROJECT0 + slot, NULL, 0);
    if (rc == 0) {
        /* 上游 project_erase 的 IRQ off 段：清 RAM 槽/dirty 位、撤 live_sec、把歌曲里
         * 引用该 scene 的项拿走（一段不剩回默认歌曲）并置 song dirty。擦写期间若又有
         * section capture（gen 变化），保留新捕获与它的 dirty 位，由 tick 重写 flash。 */
        engine_lock();
        if (s_sec_gen[slot] == gen0) {
            memset(&proj_slot[slot], 0, sizeof(project_t));
            s_sec_dirty &= (uint8_t)~(1u << slot);
            if (live_sec == (int8_t)slot) live_sec = -1;
            if (arr_drop_scene(&arrangement, slot)) song_pending_mark(&s_song);
        }
        engine_unlock();
    }
    store_unlock();
    store_note_write(xr0, us0);
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
    if (s_frozen) return ESP_ERR_INVALID_STATE; /* 诊断冻结：SYNTH 会写诊断槽 3 */
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
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    int rc = st_save(OBJ_PROJECT0 + slot, p, sizeof *p);
    if (rc == 0) memset(p, 0, sizeof *p); /* 强制 load 走 flash 回落（st_load + proj_import） */
    store_unlock();
    store_note_write(xr0, us0);
    if (rc != 0) {
        /* 失败时 RAM 槽 3 不能留着「改写过的合成工程」：其它 LOAD 会把它当成有效槽
         * （hash/sum 已重算、内容却是 diagnostic 的），下次读取将读到不存在的东西。
         * RAM 槽由存储锁串行化（与 LOAD 的回落路径同一把锁）。 */
        store_lock();
        memset(&proj_slot[slot], 0, sizeof(project_t));
        store_unlock();
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

/* ---- 旧格式/工程字段诊断 + RAW 上传（spec §4.4/§6.3，Task 9） --------------------- */
/* 两个入口共用一条纪律：只读/只写调用方指定的那个 flash 对象；`s_store_lock` 串行化，
 * **不取引擎锁**（FIELDS 连 RAM 槽都不写；RAW END 的擦写期间持引擎锁必然 xrun）。 */

/* FIELDS 的导入目标：复用自动存档捕获缓冲 s_auto（3,816B）。本函数持存储锁，而
 * autosave_tick/autosave_resume 的每一次 s_auto 访问都在同一把锁内（拿不到锁就跳过），
 * 故不会并发使用。不给 FIELDS 另开 3.8KB（ADR-009 §7 的 PSRAM 预算）。 */
esp_err_t sloop_store_project_fields(uint32_t slot, sloop_store_fields_t *out)
{
    sloop_store_fields_t f; /* 成功才写 *out：失败路径不碰调用方的缓冲（头文件契约） */
    if (out == NULL || slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    memset(&f, 0, sizeof f);
    store_lock();
    int n = st_load(OBJ_PROJECT0 + slot, &proj_tmp, sizeof proj_tmp);
    if (n <= 0) {
        store_unlock();
        return ESP_ERR_NOT_FOUND; /* 空槽：没有有效副本（或空提交 len=0） */
    }
    /* 原始格式号必须在导入**之前**从对象魔数判定（导入后一律是 FUN6） */
    uint32_t magic;
    memcpy(&magic, &proj_tmp, sizeof magic);
    f.fmt = magic == PROJ_MAGIC ? 6u : magic == PROJ_MAGIC_V5 ? 5u : magic == PROJ_MAGIC_V4 ? 4u :
               magic == PROJ_MAGIC_V3 ? 3u : magic == PROJ_MAGIC_V2 ? 2u : magic == PROJ_MAGIC_V1 ? 1u : 0u;
    project_t *p = s_auto;
    if (!proj_import(p, &proj_tmp, n)) {
        store_unlock();
        return ESP_ERR_INVALID_ARG; /* 对象存在，但不是任何已知格式（截断/损坏） */
    }
    f.sel = p->sel;
    f.bpm = p->g[G_BPM];
    f.swing = p->g[G_SWING];
    f.dust = p->g[G_DUST];
    f.drdly = p->drdly;
    for (uint32_t k = 0; k < NTRK; k++) {
        const proj_trk_t *t = &p->t[k];
        f.trk[k].engine = t->engine;
        f.trk[k].preset = t->preset;
        f.trk[k].level = pt_get(t, P_LEVEL);
        f.trk[k].atk = pt_get(t, P_ATK);
        f.trk[k].e0 = t->e[0];
        f.trk[k].e1 = t->e[1];
        for (uint32_t i = 0; i < 8u; i++) {
            if (k == TRK_DRUM) {
                uint16_t m = (uint16_t)(t->dstep[i].on[0] | (uint16_t)t->dstep[i].on[1] << 8);
                uint32_t c = 0;
                for (uint32_t b = 0; b < 16u; b++) c += (m >> b) & 1u;
                f.trk[k].n[i] = (uint8_t)c;
                f.trk[k].v[i] = m;
            } else {
                f.trk[k].n[i] = t->step[i].n;
                f.trk[k].v[i] = t->step[i].note[0];
            }
        }
    }
    store_unlock();
    *out = f;
    return ESP_OK;
}

/* RAW 状态：`.noinit`（本 TU 的 .bss 会被 `ENG SELFTEST` 的 restore 回滚；上传中间夹
 * 一次自检时，半截状态必须保留——否则 END 会把未收满的暂存写进 flash）。 */
static uint32_t s_raw_len __attribute__((section(".noinit")));
static uint32_t s_raw_got __attribute__((section(".noinit")));
static bool s_raw_on __attribute__((section(".noinit")));

void sloop_store_raw_abort(void)
{
    s_raw_on = false;
    s_raw_len = 0;
    s_raw_got = 0;
}

esp_err_t sloop_store_raw_begin(uint32_t len)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > SLOOP_STORE_RAW_MAX) {
        sloop_store_raw_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    s_raw_len = len;
    s_raw_got = 0;
    s_raw_on = true;
    return ESP_OK;
}

esp_err_t sloop_store_raw_append(const uint8_t *data, uint32_t n)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (!s_raw_on || data == NULL) return ESP_ERR_INVALID_ARG;
    /* got > len 是内部状态异常的守卫：不先判会让 s_raw_len - s_raw_got 下溢成巨大值，
     * 后面的 memcpy 就越过暂存写坏内存（R27）。 */
    if (s_raw_got > s_raw_len || n > s_raw_len - s_raw_got)
        return ESP_ERR_INVALID_SIZE; /* 状态保持：调用方可 ABORT */
    memcpy(s_raw_stage + s_raw_got, data, n);
    s_raw_got += n;
    return ESP_OK;
}

uint32_t sloop_store_raw_received(void)
{
    return s_raw_on ? s_raw_got : 0u;
}

esp_err_t sloop_store_raw_end(const char *obj, uint32_t slot)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    if (!s_raw_on || s_raw_got != s_raw_len || obj == NULL) return ESP_ERR_INVALID_ARG;
    /* END 是上传的终点：**对象名/槽号错误也退出上传态**（否则后续 append 会继续写进
     * 已声明「结束」的暂存，下一次 END 可能把上一批字节当成本次内容）。 */
    uint32_t len = s_raw_len;
    s_raw_on = false;
    s_raw_len = 0;
    s_raw_got = 0;
    bool is_settings = strcmp(obj, "settings") == 0;
    if (is_settings ? slot != 0u : (strcmp(obj, "project") != 0 || slot >= SLOOP_STORE_PROJECTS))
        return ESP_ERR_INVALID_ARG;
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    store_lock();
    int rc;
    if (is_settings) {
        /* P4-E（spec §8.3）：payload 原样提交、**现有 side 原样随新副本**（没有/全 0xFF ⇒
         * 默认 80B）；没有 magic 防线（RAW 的语义就是任意字节，坏对象的后果与上游一致：
         * reload 落默认、INFO 报 invalid）。RAM 侧重载见函数尾（R15）。 */
        uint8_t side[SONG_SIDE_LEN];
        if (!(st_side_load(OBJ_SETTINGS, side, SONG_SIDE_LEN) == 0 && !song_side_blank(side)))
            song_side_default(side);
        st_side = side;
        st_side_len = SONG_SIDE_LEN;
        rc = st_save3(OBJ_SETTINGS, s_raw_stage, len, 0, 0, 0, 0);
        st_side = 0;
        st_side_len = 0;
    } else {
        rc = st_save(OBJ_PROJECT0 + slot, s_raw_stage, len);
    }
    if (rc == 0 && !is_settings) {
        /* RAW 对象绕过了 proj_capture：按开机 fetch 的语义把槽的 RAM 缓存对齐到 flash
         * （导入失败 = 槽无效，之后的 LOAD 会走 flash 回落并回 invalid）。**只对工程对象**：
         * `settings 0` 的 slot 也是 0，但 payload 不是工程——走进来会把 A 段的 RAM 缓存
         * memset 成空（flash 不动，`STORE PROJ HASH 0` 却报 empty，live JUMP A 应用零）。 */
        if (!proj_import(&proj_slot[slot], s_raw_stage, (int)len)) memset(&proj_slot[slot], 0, sizeof(project_t));
    }
    store_unlock();
    store_note_write(xr0, us0);
    if (rc != 0) {
        ESP_LOGE(TAG, "RAW END %s %u 失败 rc=%d", obj, (unsigned)slot, rc);
        return ESP_ERR_NOT_FINISHED;
    }
    if (!is_settings) return ESP_OK;
    /* R15：上传的对象就是 settings 的全部——歌曲顺序、SYN kit 银行与生效副本都要按它重载
     * （与 `ENG SELFTEST` 收尾同一序列）。否则 RAM 里还是上传前的 arrangement/dsu：下一次
     * 歌曲/DSYN 保存会把旧 RAM 写回去，等于悄悄撤销这次上传。三个 reload 各自取存储锁与
     * 引擎锁（store → engine），故在**释放存储锁之后**调用（与自检收尾同：不持锁进入）。
     * 对象损坏时 song_reload 落默认歌曲并回 INVALID_ARG、dsu_reload 回 NOT_FOUND——上传
     * 本身已成功，只告警（INFO 报 invalid），回 OK。 */
    esp_err_t song_ret = sloop_store_song_reload();
    esp_err_t dsu_ret = sloop_store_dsu_reload();
    sloop_store_settings_reload();
    if (song_ret != ESP_OK && song_ret != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "RAW settings 之后歌曲重载: %s（落默认歌曲）", esp_err_to_name(song_ret));
    if (dsu_ret != ESP_OK && dsu_ret != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "RAW settings 之后 SYN kit 重载: %s（落工厂 SYN kit）", esp_err_to_name(dsu_ret));
    return ESP_OK;
}

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
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    up_rec_t r;
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
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
    store_note_write(xr0, us0);
    return preset_put_rc(rc, "SAVE", slot);
}

/* 上游 up_ui 的 LOAD 同样先拦鼓轨并出 DRUM TRACK: NO SOUND（upreset.c:360-363），空槽
 * 再回 EMPTY SLOT。本接口可指定任意轨，故按**目标轨**判定（缺省轨 = 选中轨，由 console
 * 传入）。load 不写 flash，因此不受播放守卫（上游 MIDI PC 语义）。 */
esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track)
{
    if (slot >= SLOOP_STORE_PRESETS || track >= NTRK) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE; /* 诊断冻结：LOAD 会改引擎 */
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
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
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
    store_note_write(xr0, us0);
    return preset_put_rc(rc, "ERASE", slot);
}

/* ------------------------------------------------ FM6 银行 0..26（fm6_bank.c） --- */
/* 上游语义（fm6_bank.c）：27 个 128B packed patch 都在**一个** storage.c 对象里
 * （OBJ_FM6BANK，A/B 扇区 0xE5000/0xE6000）；读是 XIP 原地（FM6_BANK_XIP），写 = 从当前
 * 副本拷进 proj_tmp → 改一个槽 → st_save（整银行 3,472B + 回读校验）；`fm6_bank_read`
 * 钩子（fm6_bank_boot 安装）供 eng_fm6.c 的 fm6_slot_get 在 PTCH ≥ 8 时读取。
 *
 * `.bss` 回滚（ADR-009 §8，与 Task 4 的 up_bank_xip 同型）：`fm6_bank_cur/fm6_bank_used` 是
 * fm6_bank.c 的普通静态，本 TU 的 .bss 在 `ENG SELFTEST` 的 SURROUND(sloop_selftest_bss)
 * 快照窗口里，restore 会清成 -1/0——`fm6_bank_read` 钩子指针（eng_fm6.c）同理被清成
 * NULL。读路径清成 -1 ⇒ 银行读成空；写路径更危险：`fm6_bank_put` 从 cur=-1 起笔会
 * memset 整银行，把同一银行里已用的其它槽静默抹掉。上游的声明没有 section 属性、宏
 * 重定向搬不走，故**每个**公共入口在存储锁内 `fm6_bank_boot()` 重扫（两次头校验 +
 * 3,472B CRC，微秒级）。这也让「刚写下的银行立刻可见」。注意同一回滚也清掉
 * `fm6_bank_read` 钩子：自检之后到下一次 bank 入口（或重启）之前，PTCH ≥ 8 载入的是
 * `FM6_INIT`（DI-20 的第二个症状；存储里的银行字节不受影响）。
 *
 * 锁序同工程/预置（ADR-009 §3）：存储锁 → 擦/写 flash 期间**不持引擎锁**（本板渲染持
 * 引擎锁，一次擦写 50ms 必然 xrun）；live patch 的 128B 拷贝在引擎锁内。
 * 已知的良性竞态：`fm6_bank_put` 末尾的 `fm6_bank_changed()` 是 vendored 代码，直接写
 * `fm6_slot[]`（把引用银行槽的轨标成 0xFF 让 fm6_poll 重载），而本板的 fm6_poll 在渲染
 * 路径的引擎锁内——这次的字节写落在锁外（擦写不能持引擎锁）。最坏结果是某一轨这次没被
 * 标脏、继续用它 fm6_patch 里的旧声音（音频本来就发的是那个声音），下一次 PTCH 变动即
 * 恢复；无内存安全风险。 */

bool sloop_store_bank_used(uint32_t slot)
{
    if (slot >= SLOOP_STORE_BANK_SLOTS || !sloop_store_ready()) return false;
    store_lock();
    fm6_bank_boot();
    bool used = fm6_bank_has(slot) != 0;
    store_unlock();
    return used;
}

/* 失败（空槽/槽号越界/未就绪/out == NULL）时 **out 不被写**：调用方不能拿上一次的内容
 * 当结果（console 只在返回 true 时打印 hex）。 */
bool sloop_store_bank_get(uint32_t slot, uint8_t out[SLOOP_STORE_BANK_PACKED])
{
    if (out == NULL || slot >= SLOOP_STORE_BANK_SLOTS || !sloop_store_ready()) return false;
    store_lock();
    fm6_bank_boot();
    bool got = fm6_bank_get(slot, out) == 0;
    store_unlock();
    return got;
}

/* 选中轨的 live patch → 128B packed（PUT 的输入；console 的 `STORE BANK LIVE`）。
 * 只读引擎状态（fm6_patch，不改任何东西），128B 拷贝在引擎锁内。**不经过存储层**：没有
 * 分区/mmap 时 live patch 照样可读（诊断面与 P4-D 的「导出当前音色」都需要），故不判
 * sloop_store_ready()；PUT 才需要存储。
 * 鼓轨（song.sel ≥ NPART）没有 patch：fm6_patch 只对 NPART 个合成轨存在，输出全 0
 * （console 在调用前就回 drum）。 */
void sloop_store_bank_current(uint8_t *packed)
{
    if (packed == NULL) return;
    memset(packed, 0, SLOOP_STORE_BANK_PACKED);
    engine_lock(); /* 取锁前 core 未 init 时是空操作（s_engine_lock == NULL），song.sel 是 .bss 的 0 */
    if (song.sel < NPART) fm6_pack(fm6_patch[song.sel], packed);
    engine_unlock();
}

/* fm6_bank_put 的返回码（0 ok / 1 槽号坏 / 2 flash 错或未就绪，银行保持原样）→ esp_err_t。
 * rc=1 在这里是**防御**：两个公共入口都已先判 `slot < SLOOP_STORE_BANK_SLOTS`，上游的
 * rc=1 不可达；保留映射是为了不把「坏槽号」误报成 flash 失败（上游若改判定也不静默）。 */
static esp_err_t bank_put_rc(int rc, const char *what, uint32_t slot)
{
    if (rc == 0) return ESP_OK;
    if (rc == 1) return ESP_ERR_INVALID_ARG;
    ESP_LOGE(TAG, "BANK %u %s 失败 rc=%d（银行保持原样）", (unsigned)slot, what, rc);
    return ESP_ERR_NOT_FINISHED;
}

/* PUT n：packed（128B）→ 槽 n。写入口的 fm6_bank_boot() 是必须的：上游从「当前副本」
 * 起笔，索引为空（自检 restore 之后）会 memset 整银行。 */
esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed)
{
    if (slot >= SLOOP_STORE_BANK_SLOTS || packed == NULL) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    store_lock();
    fm6_bank_boot();
    int rc = fm6_bank_put(slot, packed); /* 擦/写 flash：不持引擎锁 */
    store_unlock();
    store_note_write(xr0, us0);
    return bank_put_rc(rc, "PUT", slot);
}

/* ERASE n = 上游 `fm6_bank_put(n, NULL)`（整银行重写，槽 n 清空、used 位清）。 */
esp_err_t sloop_store_bank_erase(uint32_t slot)
{
    if (slot >= SLOOP_STORE_BANK_SLOTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    store_lock();
    fm6_bank_boot(); /* 同上：擦除也是整银行重写，必须从当前副本起笔 */
    int rc = fm6_bank_put(slot, NULL);
    store_unlock();
    store_note_write(xr0, us0);
    return bank_put_rc(rc, "ERASE", slot);
}

/* ------------------------------------------------ 自动存档与上电恢复（Task 6） --- */
/* 上游 project.c:686-754（`#ifndef PROJ_HOST` 段）按原语义移植到 adapter：
 *   - autosave_tick 在「安静」时把工作工程写进 OBJ_AUTOSAVE（`project_t` 原样，
 *     与工程 A–D 同一格式层，故编辑器/备份可互通）；
 *   - autosave_resume 开机读回：`st_load` → `proj_import` → `proj_apply(all=1)` →
 *     `song.sel` 钳位 → `proj_foreign_parts` → `trk[n].engine = eng_req`（上电无声，
 *     不淡入）——"SLOOP 从上次离开的地方开始"。上游同一函数里 2.5→2.6 的
 *     `drch_seed`/`midi_set` 段是**设置**对象（G_DRCH）的迁移，归 P4-E，不在这。
 *
 * 与上游条件的逐条对应（detail 见 ADR-009 §5）：上游的 `rec_wait/ft_on/ui.menu`
 * （录音/文件传输/菜单打开）在本机没有对应物；「存储忙」（别的线程正拿着存储锁做
 * 工程/预置的擦写）与「诊断冻结」（`ENG SELFTEST`）是本机多出来的两项。
 * 锁序（ADR-009 §3）：存储锁 → 引擎锁；**擦/写 flash 期间不持引擎锁**（本板音频
 * 渲染持它，持锁 50ms 必然 xrun）。tick 拿不到存储锁就跳过，不与写者排队。 */
static int audio_quiet(void) /* 上游 project.c:700-709：合成/鼓 voice 均 inactive */
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            if (trk[p].v[i].active) return 0;
    for (i = 0; i < NDRUM; i++)
        if (drums.v[i].active) return 0;
    return 1;
}

/* 输入派发的唯一入口（物理/注入键盘、TAP 命中、成功的 state_apply）：只记时间戳，
 * 不取任何锁（可能在 input 任务持 input 互斥时被调）。 */
void sloop_store_note_activity(uint32_t now_ms)
{
    s_last_input_ms = now_ms;
    s_input_ms = store_esp_ms(); /* 延迟写（歌曲 sections_tick）的 1.5s idle 门限 */
}

bool sloop_store_autosave_enable(bool on)
{
    s_auto_on = on;
    return s_auto_on;
}

void sloop_store_autosave_tick(uint32_t now_ms)
{
    if (!sloop_store_ready() || !s_auto_on || !flash_ok || s_frozen || s_busy) return;
    if (now_ms - s_auto_ms < AUTOSAVE_GAP) return;
    if (now_ms - s_last_input_ms < AUTOSAVE_IDLE) return;
    if (now_ms - s_auto_checked < AUTOSAVE_PERIOD) return;
    if (!store_trylock()) return;

    uint32_t h;
    engine_lock();
    /* 走带先判、再捕获（上游同序）：播放中不做 3.8KB 的 proj_capture，也不动 flash。
     * 整段都在引擎锁内 ⇒ 这份捕获与两个判据是同一时刻的状态（否则可能在「刚开始播放」
     * 与「捕获」之间抢跑，把播放中的半截状态写进 flash）。 */
    if (song.playing || transport_req) {
        engine_unlock();
        store_unlock();
        return; /* 上游在这一项上早退，不推进 autosave_checked（下一拍再看） */
    }
    proj_capture(s_auto);
    h = s_auto->sum;
    bool do_write = audio_quiet() && h != s_auto_hash;
    engine_unlock();
    s_auto_checked = now_ms;
    if (!do_write) {
        store_unlock();
        return;
    }
    /* 擦/写 flash：不持引擎锁（st_save 自带回读校验，失败时旧副本保持可读） */
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    int rc = st_save(OBJ_AUTOSAVE, s_auto, sizeof *s_auto);
    store_note_write(xr0, us0);
    if (rc == 0) {
        s_auto_hash = h;
        s_auto_writes++;
        ESP_LOGD(TAG, "autosave 写入 hash=%08lX（本次开机第 %u 次）", (unsigned long)h,
                 (unsigned)s_auto_writes);
    } else {
        /* 失败路径顺带报 1s 任务的栈余量（一次调用，微秒级）：这条路径最可能在
         * 存储后端异常时反复触发，栈余量是排障时最先要看的一个数。 */
        ESP_LOGW(TAG, "autosave 写入失败 rc=%d（下一窗口重试；任务栈余量 %uB）", rc,
                 (unsigned)(s_auto_task ? uxTaskGetStackHighWaterMark(s_auto_task) * sizeof(StackType_t)
                                        : 0u));
    }
    s_auto_ms = now_ms; /* 与上游同：尝试过就推进 gap（成功与否） */
    store_unlock();
}

/* 上电恢复（sloop_core_init 在 sloop_store_init 之后、UI/音频启动之前调用）：
 * 工厂态快照已拍完（README/ADR-009 §5 的启动顺序），resume 的写值不影响
 * `ENG SELFTEST` 的 golden 基线。 */
esp_err_t sloop_store_autosave_resume(void)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    int n;
    store_lock();
    n = st_load(OBJ_AUTOSAVE, &proj_tmp, sizeof proj_tmp);
    if (!proj_import(s_auto, &proj_tmp, n)) {
        store_unlock();
        ESP_LOGI(TAG, "autosave 无有效副本（从工厂态开始）");
        return ESP_ERR_NOT_FOUND;
    }
    s_auto_hash = s_auto->sum; /* 第一拍 tick 不得把刚读回的东西原样写一遍（flash 寿命） */
    engine_lock();
    proj_apply(s_auto, 1);
    song.sel = (uint8_t)(s_auto->sel < NTRK ? s_auto->sel : 0u);
    proj_foreign_parts(s_auto); /* 本固件没有的引擎 → 该轨默认声音（project.c:639-647） */
    for (n = 0; n < NPART; n++) trk[n].engine = trk[n].eng_req; /* 上电无声：不淡入 */
    engine_unlock();
    store_unlock();
    ESP_LOGI(TAG, "autosave 恢复：hash=%08lX sel=%u bpm=%d", (unsigned long)s_auto_hash,
             (unsigned)song.sel, (int)song.g[G_BPM]);
    return ESP_OK;
}

/* 1s 周期任务的入口（sloop_core_init 在 sloop_store_autosave_resume 之后调用）：
 * 栈 3072 内部 RAM、优先级 1（低于音频 5 与输入 4）。任务只调 tick（内部自取存储锁 +
 * 引擎锁、不取 LVGL 锁）。放在 resume 之后是硬要求：先起任务的话，resume 还没写回
 * 工作工程时的那一拍会把工厂态写进 autosave（覆盖刚读出来的东西）。
 * 创建失败只告警：自动存档停摆，工程 A–D 与预置不受影响。 */
esp_err_t sloop_store_autosave_start(void)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_auto_task != NULL) return ESP_OK; /* 幂等 */
    if (xTaskCreate(autosave_task, "sloop_autosave", AUTOSAVE_TASK_STACK, NULL, AUTOSAVE_TASK_PRIO,
                    &s_auto_task) != pdPASS) {
        s_auto_task = NULL;
        ESP_LOGE(TAG, "autosave 任务创建失败（内部 RAM 不足？自动存档停摆）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 1s 周期任务：时间戳用 FreeRTOS tick（100Hz ⇒ 10ms 粒度，对 2.5s/20s 的门限足够），
 * 与 note_activity 的调用方同一时钟。 */
static void autosave_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(AUTOSAVE_PERIOD));
        sloop_store_autosave_tick((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        sloop_store_sections_tick(store_esp_ms());
    }
}

/* ------------------------------------------------ PER3 歌曲与延迟写（Task 7） --- */
/* 上游 project.c 的 persist_t 段（`#ifndef PROJ_HOST`，本 TU 不编）在 Tab5 上的最小
 * 子集：只读写 arrangement 36B，其余字节原样保留；格式/迁移/side 的字节级逻辑全在
 * song_persist.h（host 测试同源）。锁序与其它写入口一致（ADR-009 §3）：存储锁 →
 * 引擎锁；**擦/写 flash 期间不持引擎锁**（本板渲染持它，一次擦写必然 xrun）。
 *
 * 与上游的三处差异（都是可靠性修正，成功路径的字节不变）：
 *   1) dirty 只在「写成功且期间没有新的修改」后清（song_pending_done），上游在写之前
 *      就清；失败保留 dirty，下个窗口重试。
 *   2) 设置对象损坏（未知 magic/长度）时拒绝保存并保留 dirty，上游会把它当空设置覆盖。
 *   3) 延迟写的时间戳用 esp_timer，不用 fm1_ms。 */
#define SECTIONS_IDLE 1500u /* 上游 sections_flush：fm1_ms - ui_input_ms < 1500 */
#define SECTIONS_GAP  5000u /* 上游 sections_flush：失败后 5s 再试，不每拍重试 */

_Static_assert(SLOOP_SONG_ENTRIES == ARR_STEPS && ARR_SCENES == 4u,
               "公共歌曲 POD 与上游 arr_config_t 的 16 项 / 4 scene 一致");
_Static_assert(SONG_SIDE_LEN <= ST_SIDE_MAX, "settings 的 side record 放得进副本扇区的侧区");

/* 持存储锁调用：把当前 arrangement 写进 OBJ_SETTINGS。停止/安静门限由调用方判
 * （sections_tick 的窗口或 arrangement_save 同义入口）；这里只做字节组装 + 擦写。 */
static esp_err_t song_write_locked(void)
{
    /* 现有对象 + side 先读出来：合法 PER3 的尾段/side 必须原样随新副本提交 */
    int n = st_load(OBJ_SETTINGS, &proj_tmp.raw, sizeof proj_tmp.raw);
    uint8_t side[SONG_SIDE_LEN];
    /* 侧区有内容就原样随新副本提交（含自校验不过的 2.7 beta 主题记录）；全 0xFF 才写默认 */
    bool side_present = st_side_load(OBJ_SETTINGS, side, SONG_SIDE_LEN) == 0 && !song_side_blank(side);
    uint32_t len = n > 0 ? (uint32_t)n : 0u;
    uint32_t gen;
    song_per_kind_t kind;
    song_settings_t set_written;

    engine_lock();
    gen = song_pending_gen(&s_song);
    kind = song_per_build(proj_tmp.raw, &len, sizeof proj_tmp.raw, &arrangement, side, side_present);
    /* settings 字段（P4-E Task 2，spec §8.2/D5）：palette/lowcut/zoom 直写、lights 的 owned
     * 位 RMW、reso 进尾段 byte38——与 song_per_build 同一次引擎锁，读的是同一份 RAM。 */
    if (kind != SONG_PER_BAD) {
        set_written = s_set.f;
        len = song_per_put_settings(proj_tmp.raw, len, sizeof proj_tmp.raw, &set_written, s_midi_def);
    }
    engine_unlock();
    if (kind == SONG_PER_BAD) {
        if (!s_song_refused)
            ESP_LOGW(TAG, "歌曲保存拒绝：settings 对象的 magic/长度不认识（不覆盖，保留 dirty）");
        s_song_refused = true;
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    st_side = side;
    st_side_len = SONG_SIDE_LEN;
    int rc = st_save3(OBJ_SETTINGS, proj_tmp.raw, len, 0, 0, 0, 0);
    st_side = 0;
    st_side_len = 0;
    store_note_write(xr0, us0);
    /* 与 song_pending_mark 同一把引擎锁：check(gen)+clear 必须原子，否则「gen 相符 → 清
     * dirty」之间挤进来的 song_set 会被这次 clear 吞掉（修改丢了、也不再有重试）。
     * dsu_dirty 同一位置清：写成功且刚提交的 88..1,551 就是当前 RAM 的 DSU 才清
     * （擦写窗口里又来的 DSYN 编辑让 payload 过时 → 保留 dirty，spec §3.4）。 */
    engine_lock();
    song_pending_done(&s_song, rc == 0, gen);
    song_per_saved(rc == 0, proj_tmp.raw, len);
    settings_written_locked(rc == 0, &set_written);
    engine_unlock();
    if (rc != 0) {
        ESP_LOGE(TAG, "歌曲保存失败 rc=%d（保留 dirty，下个窗口重试）", rc);
        return ESP_ERR_NOT_FINISHED;
    }
    s_song_refused = false;
    ESP_LOGI(TAG, "歌曲已保存 len=%u 来源=%u", (unsigned)len, (unsigned)kind);
    return ESP_OK;
}

esp_err_t sloop_store_song_get(sloop_song_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    engine_lock(); /* 写侧（song_set）与读侧都在引擎锁内：音频线程读的是同一份 arrangement */
    out->count = arrangement.count;
    out->loop = arrangement.loop;
    for (uint32_t i = 0; i < SLOOP_SONG_ENTRIES; i++) {
        out->entry[i].scene = arrangement.entry[i].scene;
        out->entry[i].bars = arrangement.entry[i].bars;
    }
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_store_song_set(const sloop_song_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    arr_config_t a;
    memset(&a, 0, sizeof a);
    a.count = cfg->count;
    a.loop = cfg->loop;
    for (uint32_t i = 0; i < ARR_STEPS; i++) {
        a.entry[i].scene = cfg->entry[i].scene;
        a.entry[i].bars = cfg->entry[i].bars;
    }
    /* 结构校验（上游 arr_valid 去掉 ready 位）：引用的 A–D 是否有工程由播放/歌曲页判 */
    if (!song_arr_valid(&a)) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) { /* 取锁后复核（与其它写入口同口径） */
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    arrangement = a;
    song_pending_mark(&s_song); /* dirty + 代次：flush 期间的并发修改不会被误清 */
    s_song_refused = false;
    engine_unlock();
    s_input_ms = store_esp_ms(); /* 一次用户动作：idle 门限从这一刻起算 */
    return ESP_OK;
}

/* 上游 arrangement_save 的同义入口：停止态立即写；播放中 "STOP BEFORE SAVE"。
 * 失败保留 dirty（后续由 sections_tick 按同一门限重试）。 */
esp_err_t sloop_store_song_save(void)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    esp_err_t err = song_write_locked();
    store_unlock();
    s_sections_tried_ms = store_esp_ms(); /* 尝试过就推进 gap（失败也按 5s 窗口重试） */
    return err;
}

/* 从 settings 读回歌曲（开机与 SELFTEST 之后）：**以 flash 为准**，读成功后清 dirty。
 * 这在自检之后是必须的：`arrangement` 是 arranger.c 的 static `.bss`，selftest_restore()
 * 会把它回滚到 init 快照（= 零）——不重载就会悄悄换成默认歌曲。代价：恰好在自检那一刻
 * 还没落盘的歌曲修改（正常 ≤5s 的延迟窗口内）随回滚丢弃；诊断命令不是用户路径。
 *
 * 读不到时**真的落默认**（song_per_loaded）：`!ready`（存储不可用）与对象损坏都回上游
 * 默认歌曲，而不是把 `.bss` 的全零留在 arrangement 里。损坏对象不被当成空设置写回
 * （清 dirty 后保存不再尝试覆盖它），返回 invalid。
 * 代次守卫：读 flash 期间如果不巧来了 song_set（另一次修改），RAM 比 flash 新 ⇒ 不覆盖、
 * 也不清 dirty。 */
esp_err_t sloop_store_song_reload(void)
{
    uint8_t count, loop;
    arr_config_t a;
    if (!sloop_store_ready()) {
        engine_lock();
        arr_defaults(&arrangement); /* 读不到：上游默认歌曲（不写 flash、不留全零） */
        s_song.dirty = 0;
        count = arrangement.count;
        loop = arrangement.loop;
        engine_unlock();
        ESP_LOGW(TAG, "歌曲设置不可读（存储未就绪）：保持默认歌曲 count=%u loop=%u", (unsigned)count,
                 (unsigned)loop);
        return ESP_ERR_INVALID_STATE;
    }
    engine_lock();
    uint32_t gen = song_pending_gen(&s_song);
    engine_unlock();

    store_lock();
    int n = st_load(OBJ_SETTINGS, &proj_tmp.raw, sizeof proj_tmp.raw);
    song_per_kind_t kind = song_per_read(proj_tmp.raw, n > 0 ? (uint32_t)n : 0u, &a);
    store_unlock();

    engine_lock();
    if (song_pending_gen(&s_song) != gen) { /* 读 flash 期间又来了一次修改：RAM 更新 */
        engine_unlock();
        ESP_LOGI(TAG, "歌曲设置重载跳过：读取期间有新的修改（保留 RAM）");
        return ESP_OK;
    }
    bool ok = song_per_loaded(kind, &a, &arrangement); /* 损坏/无对象 → 上游默认歌曲 */
    s_song.dirty = 0; /* RAM 与 flash（或默认）一致了；与 mark 同一把锁 */
    s_song_refused = false;
    count = arrangement.count;
    loop = arrangement.loop;
    engine_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "歌曲设置对象损坏（magic/长度不认识）：保持默认歌曲 count=%u loop=%u",
                 (unsigned)count, (unsigned)loop);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "歌曲设置: 来源=%u count=%u loop=%u", (unsigned)kind, (unsigned)count, (unsigned)loop);
    return kind == SONG_PER_NEW ? ESP_ERR_NOT_FOUND : ESP_OK; /* 空设置 = 默认歌曲，不是错误 */
}

esp_err_t sloop_store_song_info(sloop_store_song_info_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!sloop_store_ready()) {
        out->kind = SLOOP_SONG_EMPTY;
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t side[SONG_SIDE_LEN];
    arr_config_t a;
    arr_defaults(&a); /* 对象损坏时 song_per_read 不写 *a：诊断面给默认歌曲而不是垃圾 */
    store_lock();
    int n = st_load(OBJ_SETTINGS, &proj_tmp.raw, sizeof proj_tmp.raw);
    uint32_t len = n > 0 ? (uint32_t)n : 0u;
    song_per_kind_t kind = song_per_read(proj_tmp.raw, len, &a);
    out->side = st_side_load(OBJ_SETTINGS, side, SONG_SIDE_LEN) == 0 && song_side_valid(side) ? 1u : 0u;
    store_unlock();
    out->len = (uint16_t)(len > 0xFFFFu ? 0xFFFFu : len);
    out->kind = kind == SONG_PER_NEW ? SLOOP_SONG_EMPTY :
                kind == SONG_PER3 ? SLOOP_SONG_PER3 :
                kind == SONG_PER2 ? SLOOP_SONG_PER2 :
                kind == SONG_PER1 ? SLOOP_SONG_PER1 : SLOOP_SONG_INVALID;
    out->count = a.count;
    out->loop = a.loop;
    return ESP_OK;
}

bool sloop_store_song_dirty(void)
{
    return sloop_store_ready() && s_song.dirty != 0;
}

void sloop_store_song_dirty_mark(void)
{
    engine_lock(); /* 与 song_pending_mark 的其它调用点同一把锁（gen 记账原子） */
    song_pending_mark(&s_song);
    engine_unlock();
}

/* ---- SYN kit 银行（DSU）：settings 的 88..1,551（P4-D Task 5，spec §3.4）-----------
 * `dsu`/`dsu_dirty` 是 drum_synth.c 的 static：鼓声部在引擎锁内读它，故所有写入都在引擎
 * 锁内；flash 侧只有 OBJ_SETTINGS 一个对象（与歌曲/side 同一把存储锁、同一次 A/B 提交）。
 * 上游对照：project.c 的 dsu_boot（读）与 settings_write（写，成功才清 dsu_dirty）。 */

/* 开机与 `ENG SELFTEST` 之后：settings 88..1,551 → dsu。上游 dsu_boot 同序——先
 * dsu_defaults，只有「长度够 + dsu_valid」才覆盖，每个 kit 过一遍 dsu_fix_kit（外部来源
 * 的字节不可信）。没有对象/长度不足/银行非法都落工厂 SYN kit，回 NOT_FOUND。
 * 锁序 store → engine（ADR-009 §3），与其它写入口一致；st_load 不擦写、只是读。 */
esp_err_t sloop_store_dsu_reload(void)
{
    if (!sloop_store_ready()) {
        engine_lock();
        dsu_defaults();
        dsu_dirty = 0;
        engine_unlock();
        ESP_LOGW(TAG, "SYN kit 不可读（存储未就绪）：落工厂 SYN kit");
        return ESP_ERR_INVALID_STATE;
    }
    store_lock();
    int n = st_load(OBJ_SETTINGS, &proj_tmp.raw, sizeof proj_tmp.raw);
    uint32_t len = n > 0 ? (uint32_t)n : 0u;
    engine_lock();
    /* 只接受**真正带 DSU 的**合法 PER3 长度：song_per3_len 还包含 84/88B 的历史对象，
     * 那两种长度没有 88..1,551 的字节（memcpy 会读到暂存里的残留），故另要求
     * len ≥ canonical。字节布局与 magic 的判定同源 song_persist.h。 */
    bool from_flash = len >= SONG_PER_CANON && song_per3_len(len) &&
                      dsu_valid((const dsu_bank_t *)(proj_tmp.raw + SONG_DSU_OFF));
    if (from_flash) {
        memcpy(&dsu, proj_tmp.raw + SONG_DSU_OFF, sizeof dsu);
        for (uint32_t k = 0; k < DSU_N; k++) dsu_fix_kit(&dsu.k[k]);
    } else {
        dsu_defaults();
    }
    dsu_dirty = 0; /* RAM 与 flash（或工厂默认）一致了 */
    engine_unlock();
    store_unlock();
    ESP_LOGI(TAG, "SYN kit: %s（settings len=%u）", from_flash ? "settings 88..1551" : "工厂默认",
             (unsigned)len);
    return from_flash ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* 显式 STORE（DSYN 页的 STORE chip / CMD_DSYN_STORE 经状态层）：停止态把**当前 RAM 的
 * DSU**（与当前歌曲、side 一起）写进 settings——上游 settings_write 一次写整份 persist_t，
 * 没有「只写 DSU」的对象。失败保留 dirty，由下一次显式 STORE 或歌曲/段落延迟写重试；
 * 与 song_save 同：尝试过就推进延迟写的 gap 时间戳。
 * `ENG SELFTEST`/`ENG FREEZE` 的诊断冻结回 INVALID_STATE（与 song_save 同口径）。 */
esp_err_t sloop_store_dsu_store(void)
{
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE;
    store_lock();
    esp_err_t err = song_write_locked();
    store_unlock();
    s_sections_tried_ms = store_esp_ms();
    return err;
}

/* RAM 的 SYN kit 与最近一次成功写盘是否不一致（DSYN 编辑置位；任何成功的 settings 保存
 * ——显式 DSU STORE、显式歌曲保存、sections/歌曲延迟写——在写成功且字节未过时清）。 */
bool sloop_store_dsu_dirty(void)
{
    return dsu_dirty != 0;
}

/* ---- live sections A–D（P4-C Task 8）---------------------------------------- */

/* 调用方持引擎锁（sloop_seq_section 的 capture 临界区）：置 dirty 与 ++gen 必须和
 * proj_capture 原子，否则 erase 可能在这两步之间擦掉刚捕获的槽。 */
void sloop_store_section_dirty_mark_locked(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return;
    s_sec_dirty |= (uint8_t)(1u << slot);
    s_sec_gen[slot]++;
}

bool sloop_store_sections_dirty(void)
{
    return sloop_store_ready() && s_sec_dirty != 0;
}

/* 停止态把 **RAM 槽** 变成工作 loop（上游 section_load：project_apply(&proj_slot[s])，
 * 不回落 flash、不写存储）。播放中的 section 切换只能走 sloop_seq_section_request。 */
esp_err_t sloop_store_section_load(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_NOT_ALLOWED;
    store_lock();
    project_t *p = &proj_slot[slot];
    if (!proj_ok(p)) {
        store_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    store_project_apply_locked(p);
    engine_lock();
    live_sec = (int8_t)slot;
    engine_unlock();
    store_unlock();
    return ESP_OK;
}

/* 停止态擦除一个 section（上游 project_erase）：空槽不写 flash；成功写空对象、清 RAM
 * 槽/dirty 位、撤 live_sec，并从歌曲顺序里拿走该 scene（有项被删 → song dirty）。 */
esp_err_t sloop_store_section_erase(uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return ESP_ERR_INVALID_ARG;
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_NOT_ALLOWED;
    store_lock();
    if (!proj_ok(&proj_slot[slot])) {
        store_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    engine_lock();
    uint32_t gen0 = s_sec_gen[slot];
    engine_unlock();
    uint32_t xr0 = sloop_hal_audio_xruns();
    int64_t us0 = esp_timer_get_time();
    int rc = st_save(OBJ_PROJECT0 + slot, NULL, 0);
    if (rc == 0) {
        /* 擦写期间的新 capture（gen 变化）保留：不清位、不 memset、不撤 live_sec/歌曲项；
         * flash 里的空对象由下一个 tick 用新 RAM 槽重写。 */
        engine_lock();
        if (s_sec_gen[slot] == gen0) {
            memset(&proj_slot[slot], 0, sizeof(project_t));
            s_sec_dirty &= (uint8_t)~(1u << slot);
            if (live_sec == (int8_t)slot) live_sec = -1;
            if (arr_drop_scene(&arrangement, slot)) song_pending_mark(&s_song);
        }
        engine_unlock();
    }
    store_unlock();
    store_note_write(xr0, us0);
    if (rc != 0) {
        ESP_LOGE(TAG, "SECTION %u ERASE 失败 rc=%d", (unsigned)slot, rc);
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

/* 延迟写（1s 任务）：停止 + 无发声 + 输入空闲 ≥1.5s + 距上次尝试 ≥5s 才落盘。
 * dirty 段与歌曲同一拍、同一门限（ADR-009 §6.1）：段先写（上游 sections_write 顺序），
 * 再走 PER3 歌曲。段的快照在引擎锁内拷进暂存，擦写期间不持引擎锁；拿不到存储锁就
 * 跳过本拍（不与写者排队，也不阻塞 1s 任务）。
 * settings dirty（P4-E Task 2，spec §5 D4）也搭这一次写：字段随 song_write_locked 落进
 * 同一个对象，写成功后由 settings_written_locked 清。 */
void sloop_store_sections_tick(uint32_t now_ms)
{
    if (!sloop_store_ready() || !flash_ok || s_frozen || s_busy) return;
    if (!s_song.dirty && !s_sec_dirty && !s_set.dirty) return;
    if (now_ms - s_sections_tried_ms < SECTIONS_GAP) return;
    if (now_ms - s_input_ms < SECTIONS_IDLE) return;
    if (!store_trylock()) return;
    engine_lock();
    bool blocked = song.playing || transport_req || !audio_quiet();
    engine_unlock();
    if (!blocked) {
        for (uint32_t i = 0; i < SLOOP_STORE_PROJECTS; i++) {
            uint8_t bit = (uint8_t)(1u << i);
            if (!(s_sec_dirty & bit)) continue;
            /* 先清位再快照：捕获若在快照期间发生会重新置位（下一窗口再写新值），
             * 而失败要能重试。 */
            engine_lock();
            s_sec_dirty &= (uint8_t)~bit;
            memcpy(&proj_tmp.v6, &proj_slot[i], sizeof(project_t));
            engine_unlock();
            uint32_t xr0 = sloop_hal_audio_xruns();
            int64_t us0 = esp_timer_get_time();
            int rc = st_save(OBJ_PROJECT0 + i, &proj_tmp.v6, sizeof(project_t));
            store_note_write(xr0, us0);
            if (rc != 0) {
                engine_lock();
                s_sec_dirty |= bit;
                engine_unlock();
                ESP_LOGE(TAG, "SECTION %u 延迟写失败 rc=%d（保留 dirty，下个窗口重试）", (unsigned)i, rc);
            }
        }
        if (s_song.dirty || s_set.dirty) {
            esp_err_t err = song_write_locked(); /* 门限已在上面判过 */
            if (err != ESP_OK) ESP_LOGE(TAG, "歌曲延迟写失败: %s", esp_err_to_name(err));
        }
        s_sections_tried_ms = now_ms; /* 上游同：尝试过就推进 gap（失败按 5s 窗口重试） */
    }
    store_unlock();
}

/* ------------------------------------------------ 设备 torn-write 套件（P4-B Task 8） --- */
/* ADR-009 §3 的「掉电不破坏旧副本」变成机器判据。对象 = OBJ_SETTINGS：P4-B 不写设置
 * （P4-E 才用），故它是唯一「有 storage 对象语义、但没有当前用户数据」的目标；套件开始
 * 快照、结束 fault_clear + 原样恢复（**破坏性**诊断，console 帮助里写明）。
 *
 * **快照 = 两个完整 4KB 副本扇区**（提交头 + payload + side record + 擦除态），不再是
 * 「只快照 payload」（P4-B 的豁免）。P4-C 起 settings 有真实写入者（PER3 歌曲、side
 * record 随副本提交），场景的擦除会清掉 side record 与未知尾段；恢复后逐字节校验，
 * 不成立即响亮报 restore-settings（ADR-009 §3/§8 的 R26，Task 7 收口）。
 *
 * HAL 契约（storage_flash.c）给测试带来一个必须处理的前提：**arm 期间 read 也失败**
 * （读不消耗写序号）。于是 st_save 开头的 st_current 看不到任何有效副本 ⇒ cur=-1 ⇒
 * 写目标恒为 copy A、seq 从 0 起算。若当前副本恰好在 A，失败的擦除就会把「旧副本」一起
 * 毁掉——测试变成在验一个被自己毁掉的对象。故每个失败场景先 wipe 两个扇区，再按
 * v1→A、v2→B 提交（wipe 后第一次提交 cur=-1 落 A，第二次 cur=0 落 B），arm 之前断言
 * v2 已逐字节读回；之后失败的写只碰 A，B 里的 v2 才是「旧副本仍可读」。
 *
 * 每个 arm 场景还断言故障命中的是**预期写操作**：faults_taken 只说明「某一次写失败」，
 * 分不出 erase 与 prog。故逐场景读回 A 扇区的原始字节（不经过头校验——失败时扇区可能
 * 只有半个 payload）：
 *   - erase 失败：A 的头+payload 与 arm 前逐字节相同（扇区没被擦）；
 *   - payload 半写：A 的头是擦除态（erase 成功、提交记录没写）、payload 前 32B = 新值、
 *     其余 0xFF（prog 只落了 n/2）；
 *   - 头写失败：A 的 payload 完整 = 新值、头是擦除态（erase+prog 成功、最后一步失败）。
 * 这样「旧副本仍可读」不会因为新旧值恰好相同而假绿：三个版本逐字节不同（下面显式断言）。
 */
#define ST_TEST_LEN 64u /* 场景 payload 长度（一个 prog 块的一半落在 0..63） */
#define STORE_TEST_SECTOR 4096u /* 一个副本扇区（storage.c 的 ST_SECTOR） */
#define STORE_TEST_SECTORS 2u
_Static_assert(STORE_TEST_SECTOR == ST_SECTOR, "副本扇区大小与 storage.c 一致");

typedef struct {
    const char *name;
    bool (*fn)(void);
} store_test_case_t;

/* 递增模式，首字节 = 版本号（v + i）：0x11..0x50，不含 0xFF（擦除态可区分） */
static void store_test_payload(uint8_t *p, uint8_t v)
{
    for (uint32_t i = 0; i < ST_TEST_LEN; i++) p[i] = (uint8_t)(v + i);
}

static bool store_test_wipe(void)
{
    return sloop_hal_storage_erase(st_sector(OBJ_SETTINGS, 0)) == 0 &&
           sloop_hal_storage_erase(st_sector(OBJ_SETTINGS, 1)) == 0;
}

static bool store_test_save(const uint8_t *p)
{
    return st_save(OBJ_SETTINGS, p, ST_TEST_LEN) == 0;
}

/* 「读回」= st_load 的长度与 64 字节（这几个场景里对象都是 64B） */
static bool store_test_reads(const uint8_t *want)
{
    uint8_t got[ST_TEST_LEN];
    int n = st_load(OBJ_SETTINGS, got, sizeof got);
    return n == (int)ST_TEST_LEN && memcmp(got, want, sizeof got) == 0;
}

static bool store_test_raw(uint32_t off, void *dst, uint32_t n)
{
    return sloop_hal_storage_read(off, dst, n) == 0;
}

static bool store_test_ff(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] != 0xFFu) return false;
    return true;
}

/* 失败场景的公共前提：wipe → v1→A → v2→B → v2 逐字节可读，且当前副本确实是 B、seq 2。
 * 前提任何一条不成立都判失败（不靠「碰巧相同」的缓冲，也不在错误的副本上做失败注入）。 */
static bool store_test_two_commits(const uint8_t *v1, const uint8_t *v2)
{
    st_hdr_t h;
    if (memcmp(v1, v2, ST_TEST_LEN) == 0) return false; /* 新旧必须可区分 */
    if (!store_test_wipe() || !store_test_save(v1) || !store_test_save(v2)) return false;
    if (!store_test_reads(v2)) return false;
    return st_current(OBJ_SETTINGS, &h) == 1 && h.seq == 2u && h.len == ST_TEST_LEN;
}

/* 失败之后：胜出的副本必须是 B（copy 1）且逐字节 = v2——不是 A 上的 v1、也不是新值 v3。
 * 这是「旧副本仍可读」的判据本身：copy 下标把「碰巧读到同一份缓冲」排除掉。
 * 返回 copy 下标（0/1）；不满足返回 -1（供断言与诊断日志共用）。 */
static int store_test_old_copy(const uint8_t *v2)
{
    st_hdr_t h;
    int c = st_current(OBJ_SETTINGS, &h);
    if (c != 1 || h.len != ST_TEST_LEN || memcmp(st_buf, v2, ST_TEST_LEN) != 0) return -1;
    return c;
}

static const char *store_test_copy_name(int c)
{
    return c == 0 ? "copy0" : c == 1 ? "copy1" : "none";
}

/* 每场景一行可 triage 的诊断日志：故障分类（erase / prog-*）、faults 计数、胜出副本。 */
static void store_test_log(const char *name, const char *op, uint32_t faults, bool armed, int survivor)
{
    ESP_LOGI(TAG, "STORE TEST %s: faults=%u armed=%d op=%s survivor=%s", name, (unsigned)faults,
             armed ? 1 : 0, op, store_test_copy_name(survivor));
}

/* ① 三次提交取 seq 大者：擦净（无有效副本）→ v1（A, seq 1）→ v2（B, seq 2）⇒
 * 读回 v2；再写 v3 回 A（seq 3）⇒ 读回 v3。两个方向都覆盖（不是「永远偏好 B」）：
 * 第二次提交必须让 copy 1 胜，第三次必须让 copy 0 胜——A 只在 seq 更高时赢。 */
static bool store_test_seq(void)
{
    uint8_t v1[ST_TEST_LEN], v2[ST_TEST_LEN], v3[ST_TEST_LEN], got[ST_TEST_LEN];
    st_hdr_t h;
    int w2, w3;
    store_test_payload(v1, 0x11);
    store_test_payload(v2, 0x22);
    store_test_payload(v3, 0x33);
    if (!store_test_wipe()) return false;
    if (st_load(OBJ_SETTINGS, got, sizeof got) >= 0) return false; /* 擦净：没有有效副本 */
    if (!store_test_save(v1) || !store_test_reads(v1)) return false;
    if (!store_test_save(v2) || !store_test_reads(v2)) return false;
    w2 = st_current(OBJ_SETTINGS, &h);
    bool ok2 = w2 == 1 && h.seq == 2u && h.len == ST_TEST_LEN;
    if (!ok2 || !store_test_save(v3) || !store_test_reads(v3)) {
        ESP_LOGI(TAG, "STORE TEST seq: v2 winner=%s ok=%d v3=skipped", store_test_copy_name(w2),
                 ok2 ? 1 : 0);
        return false;
    }
    w3 = st_current(OBJ_SETTINGS, &h);
    bool ok3 = w3 == 0 && h.seq == 3u && h.len == ST_TEST_LEN;
    ESP_LOGI(TAG, "STORE TEST seq: v2 winner=%s seq=2 ok=%d; v3 winner=%s seq=3 ok=%d",
             store_test_copy_name(w2), ok2 ? 1 : 0, store_test_copy_name(w3), ok3 ? 1 : 0);
    return ok2 && ok3;
}

/* ② 擦除失败（第 1 次写）⇒ v3 写失败 ⇒ v2 仍完整可读；A 扇区一个字节都没动。 */
static bool store_test_erase_fail(void)
{
    uint8_t v1[ST_TEST_LEN], v2[ST_TEST_LEN], v3[ST_TEST_LEN];
    uint8_t before[sizeof(st_hdr_t) + ST_TEST_LEN], after[sizeof before];
    const uint32_t a = st_sector(OBJ_SETTINGS, 0);
    store_test_payload(v1, 0x31);
    store_test_payload(v2, 0x32);
    store_test_payload(v3, 0x33);
    if (!store_test_two_commits(v1, v2)) return false;
    if (memcmp(v2, v3, ST_TEST_LEN) == 0) return false; /* 旧副本的字节必须可区分 */
    if (!store_test_raw(a, before, sizeof(st_hdr_t)) ||
        !store_test_raw(a + ST_PAYLOAD_OFF, before + sizeof(st_hdr_t), ST_TEST_LEN))
        return false;
    sloop_hal_storage_fault_arm(1, false);
    int rc = st_save(OBJ_SETTINGS, v3, ST_TEST_LEN);
    uint32_t faults = sloop_hal_storage_fault_taken();
    bool armed = sloop_hal_storage_fault_armed();
    bool untouched = store_test_raw(a, after, sizeof(st_hdr_t)) &&
                     store_test_raw(a + ST_PAYLOAD_OFF, after + sizeof(st_hdr_t), ST_TEST_LEN) &&
                     memcmp(before, after, sizeof before) == 0;
    int survivor = store_test_old_copy(v2);
    store_test_log("erase-fail", "erase", faults, armed, survivor);
    /* 故障命中 erase 的判据：A 扇区头+payload 与 arm 前逐字节相同（擦没发生）。 */
    return rc != 0 && faults == 1u && !armed && untouched && survivor == 1;
}

/* ③ payload 半写（第 2 次写 = prog 命中 half）⇒ v3 写失败 ⇒ v2 仍完整可读；
 * A 的头是擦除态、payload 前 32B = v3、后 32B 仍是 0xFF。 */
static bool store_test_half_write(void)
{
    uint8_t v1[ST_TEST_LEN], v2[ST_TEST_LEN], v3[ST_TEST_LEN];
    uint8_t hdr[sizeof(st_hdr_t)], pay[ST_TEST_LEN];
    const uint32_t a = st_sector(OBJ_SETTINGS, 0);
    store_test_payload(v1, 0x41);
    store_test_payload(v2, 0x42);
    store_test_payload(v3, 0x43);
    if (!store_test_two_commits(v1, v2)) return false;
    if (memcmp(v2, v3, ST_TEST_LEN) == 0) return false;
    sloop_hal_storage_fault_arm(2, true);
    int rc = st_save(OBJ_SETTINGS, v3, ST_TEST_LEN);
    uint32_t faults = sloop_hal_storage_fault_taken();
    bool armed = sloop_hal_storage_fault_armed();
    bool saw = store_test_raw(a, hdr, sizeof hdr) &&
               store_test_raw(a + ST_PAYLOAD_OFF, pay, sizeof pay);
    bool hdr_ff = saw && store_test_ff(hdr, sizeof hdr);
    bool pay_half = saw && memcmp(pay, v3, ST_TEST_LEN / 2u) == 0 &&
                    store_test_ff(pay + ST_TEST_LEN / 2u, ST_TEST_LEN / 2u);
    int survivor = store_test_old_copy(v2);
    store_test_log("half-write", "prog-payload", faults, armed, survivor);
    /* 故障命中 payload prog 的判据：头擦除态（erase 已跑）+ payload 恰好半个新值。 */
    return rc != 0 && faults == 1u && !armed && hdr_ff && pay_half && survivor == 1;
}

/* ④ 头写失败（第 3 次写 = 提交记录）⇒ v3 写失败 ⇒ v2 仍完整可读；
 * A 的 payload 完整 = v3、头是擦除态（st_head 拒绝：没有提交记录）。 */
static bool store_test_header_fail(void)
{
    uint8_t v1[ST_TEST_LEN], v2[ST_TEST_LEN], v3[ST_TEST_LEN];
    uint8_t hdr[sizeof(st_hdr_t)], pay[ST_TEST_LEN];
    const uint32_t a = st_sector(OBJ_SETTINGS, 0);
    st_hdr_t h;
    store_test_payload(v1, 0x51);
    store_test_payload(v2, 0x52);
    store_test_payload(v3, 0x53);
    if (!store_test_two_commits(v1, v2)) return false;
    if (memcmp(v2, v3, ST_TEST_LEN) == 0) return false;
    sloop_hal_storage_fault_arm(3, false);
    int rc = st_save(OBJ_SETTINGS, v3, ST_TEST_LEN);
    uint32_t faults = sloop_hal_storage_fault_taken();
    bool armed = sloop_hal_storage_fault_armed();
    bool saw = store_test_raw(a, hdr, sizeof hdr) &&
               store_test_raw(a + ST_PAYLOAD_OFF, pay, sizeof pay);
    bool hdr_ff = saw && store_test_ff(hdr, sizeof hdr);
    bool pay_full = saw && memcmp(pay, v3, ST_TEST_LEN) == 0;
    int survivor = store_test_old_copy(v2);
    store_test_log("header-fail", "prog-header", faults, armed, survivor);
    /* 故障命中头 prog 的判据：payload 完整但头擦除态（erase+prog 都成功）。 */
    return rc != 0 && faults == 1u && !armed && hdr_ff && pay_full &&
           st_head(OBJ_SETTINGS, 0, &h) != 0 && survivor == 1;
}

/* ⑤ 写后破坏 payload：头仍合法、CRC 失配 ⇒ st_load 报错（-1），不返回坏数据。
 * 唯一有效副本在 A（wipe 后只有一次提交），B 无提交记录 ⇒ 没有更旧的副本可回退。 */
static bool store_test_corrupt(void)
{
    uint8_t v4[ST_TEST_LEN], got[ST_TEST_LEN];
    const uint8_t zero = 0x00u;
    const uint32_t a = st_sector(OBJ_SETTINGS, 0);
    st_hdr_t h;
    store_test_payload(v4, 0x61);
    if (!store_test_wipe() || !store_test_save(v4) || !store_test_reads(v4)) return false;
    if (st_head(OBJ_SETTINGS, 0, &h) != 0 || h.len != ST_TEST_LEN) return false;
    if (st_head(OBJ_SETTINGS, 1, &h) == 0) return false; /* B 不得有副本可掩盖 */
    /* prog 只能把 1 写成 0：首字节改成 0x00（v4[0] = 0x61），CRC 必失配 */
    if (sloop_hal_storage_prog(a + ST_PAYLOAD_OFF, &zero, 1) != 0) return false;
    if (st_body(OBJ_SETTINGS, 0, &h) == 0) return false; /* CRC 必须拒绝 */
    return st_load(OBJ_SETTINGS, got, sizeof got) == -1;
}

/* ⑥ 擦除提交（st_save(obj, NULL, 0)）⇒ 对象为空（len=0，上游「无对象」语义）。 */
static bool store_test_empty_commit(void)
{
    uint8_t v5[ST_TEST_LEN], one;
    st_hdr_t h;
    store_test_payload(v5, 0x71);
    if (!store_test_wipe() || !store_test_save(v5) || !store_test_reads(v5)) return false;
    if (st_save(OBJ_SETTINGS, NULL, 0) != 0) return false;
    if (st_load(OBJ_SETTINGS, &one, 1) != 0) return false;
    return st_current(OBJ_SETTINGS, &h) >= 0 && h.len == 0u;
}

static const store_test_case_t STORE_TEST_CASES[] = {
    {"seq", store_test_seq},
    {"erase-fail", store_test_erase_fail},
    {"half-write", store_test_half_write},
    {"header-fail", store_test_header_fail},
    {"corrupt", store_test_corrupt},
    {"empty-commit", store_test_empty_commit},
};

esp_err_t sloop_store_test(sloop_store_test_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->cases = (uint32_t)(sizeof STORE_TEST_CASES / sizeof STORE_TEST_CASES[0]);
    if (!sloop_store_ready()) return ESP_ERR_INVALID_STATE;
    if (store_playing()) return ESP_ERR_INVALID_STATE; /* 与其它写 API 同一走带守卫 */

    /* 快照 = settings 对象的**两个完整 4KB 扇区**（提交头 + payload + side record + 擦除态）。
     * 六场景会擦副本扇区：只恢复 payload 会静默丢掉 side record（P4-E 的主题/收藏）与未知
     * 尾段——ADR-009 §3/§8 的 R26，自 P4-C 起 settings 有真实写入者，故必须整扇区保。
     * 8KB 暂存在 PSRAM（诊断路径的一次性分配，不进内部堆门限）。 */
    uint8_t *snap = heap_caps_malloc(STORE_TEST_SECTORS * STORE_TEST_SECTOR, MALLOC_CAP_SPIRAM);
    if (snap == NULL) {
        out->fail_case = "snapshot-settings"; /* 8KB PSRAM 都拿不到：响亮失败，不跑半套 */
        ESP_LOGE(TAG, "STORE TEST snapshot-settings FAIL（PSRAM 分配失败）");
        return ESP_ERR_NO_MEM;
    }
    store_lock(); /* 全程持锁：自动存档/歌曲 tick 的 trylock 会跳过（s_busy/Fault 不交错） */
    sloop_hal_storage_fault_clear(); /* console 可能留下未命中的 arm：read 会失败 */
    bool snap_ok = true;
    for (uint32_t c = 0; c < STORE_TEST_SECTORS && snap_ok; c++)
        snap_ok = sloop_hal_storage_read(st_sector(OBJ_SETTINGS, c), snap + c * STORE_TEST_SECTOR,
                                         STORE_TEST_SECTOR) == 0;
    if (!snap_ok) {
        store_unlock();
        heap_caps_free(snap);
        out->fail_case = "snapshot-settings";
        ESP_LOGE(TAG, "STORE TEST snapshot-settings FAIL");
        return ESP_ERR_NOT_FINISHED;
    }

    for (uint32_t i = 0; i < out->cases; i++) {
        sloop_hal_storage_fault_clear(); /* 上一场景未命中的注入不得带进来 */
        if (!STORE_TEST_CASES[i].fn()) {
            out->fail_case = STORE_TEST_CASES[i].name;
            ESP_LOGE(TAG, "STORE TEST %s FAIL", STORE_TEST_CASES[i].name);
            break;
        }
        out->passed++;
        ESP_LOGI(TAG, "STORE TEST %s ok", STORE_TEST_CASES[i].name);
    }
    sloop_hal_storage_fault_clear();

    /* 恢复：两个扇区各擦一次，再按 256B 块原样写回（含 0xFF 区），最后逐字节校验。
     * 校验覆盖 payload（arrangement 48..83 与 DSU/MIDI 尾段）与 side record——这就是
     * 「六场景不破坏歌曲设置」的判据本身，不靠 st_load 的 CRC/长度近似。 */
    bool restored = true;
    for (uint32_t c = 0; c < STORE_TEST_SECTORS && restored; c++) {
        const uint32_t sector = st_sector(OBJ_SETTINGS, c);
        const uint8_t *src = snap + c * STORE_TEST_SECTOR;
        if (sloop_hal_storage_erase(sector) != 0) {
            restored = false;
            break;
        }
        for (uint32_t off = 0; off < STORE_TEST_SECTOR; off += 256u)
            if (sloop_hal_storage_prog(sector + off, src + off, 256u) != 0) {
                restored = false;
                break;
            }
    }
    uint8_t chunk[256];
    for (uint32_t c = 0; c < STORE_TEST_SECTORS && restored; c++) {
        const uint32_t sector = st_sector(OBJ_SETTINGS, c);
        const uint8_t *src = snap + c * STORE_TEST_SECTOR;
        for (uint32_t off = 0; off < STORE_TEST_SECTOR && restored; off += sizeof chunk)
            restored = sloop_hal_storage_read(sector + off, chunk, sizeof chunk) == 0 &&
                       memcmp(chunk, src + off, sizeof chunk) == 0;
    }
    store_unlock();
    heap_caps_free(snap);
    if (!restored) {
        /* 恢复失败优先于任何单个场景：设置对象已被留在坏状态，这比某个场景挂掉更重要
         * （脚本按 fail_case 的令牌分诊，不能被场景名掩盖）。 */
        ESP_LOGE(TAG, "STORE TEST restore-settings FAIL（两个 4KB 扇区未逐字节恢复）");
        out->fail_case = "restore-settings";
    }
    if (out->fail_case != NULL) return ESP_ERR_NOT_FINISHED;
    return ESP_OK;
}
