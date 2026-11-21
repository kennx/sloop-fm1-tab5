/*
 * store_tab5.c — 上游 storage.c / project.c 的 Tab5 适配层（ADR-009 §7/§8）
 *
 * 本文件由 selftest.c **末尾 include**，与引擎/DSP 同属一个翻译单元——所以可以直接把
 * vendored 的 storage.c（A/B 双扇区 + 32B 提交头 + CRC + 回读校验，原样）、project.c
 * （#define PROJ_HOST：格式层 FUN1..6 + proj_capture/proj_apply）与 upreset.c（用户预置
 * 32 槽；#define FELUCCA_FLASH 1 才走 st_xip）编进来，缺的符号在本文件顶部做薄 shim。
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
 * upreset.c 的 up_load_to 用它逐项保留轨道自己的值（Task 4 起为活代码；工厂预置的
 * apply_preset_to 在 Task 5 落地时同样用它）。 */
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
