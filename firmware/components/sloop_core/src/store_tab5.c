/*
 * store_tab5.c — 上游 storage.c / project.c 的 Tab5 适配层（ADR-009 §7/§8）
 *
 * 本文件由 selftest.c **末尾 include**，与引擎/DSP 同属一个翻译单元——所以可以直接把
 * vendored 的 storage.c（A/B 双扇区 + 32B 提交头 + CRC + 回读校验，原样）与 project.c
 * （#define PROJ_HOST：格式层 FUN1..6 + proj_capture/proj_apply）编进来，缺的符号在本
 * 文件顶部做薄 shim。third_party/ 零修改。
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

/* ---- shim：project.c 的 PROJ_HOST 部分缺的上游符号 ------------------------------- */
/* 后续任务（预置银行/FM6 bank/自动存档）会用到；本任务只要求它们可编入。 */
static uint8_t flash_ok;  /* (2.6) 存储后端就绪（上游 felucca.c 的 JEDEC 探测） */
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
 * 本任务只编入（apply_preset_to 在预置任务里落地），保证 shim 与上游同源。 */
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
