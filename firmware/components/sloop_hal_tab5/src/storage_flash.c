/*
 * storage_flash.c — 原始自定义分区 sloopstore 的 HAL 存储后端（ADR-009 §1/§2/§3）
 *
 * sloopstore（data/0x99 @0x810000, 0x68000B）承载上游 flash map 的
 * [0x97000, 0xFF000)：对外参数一律是上游逻辑偏移，分区内偏移 = off − 0x97000。
 * mmap 句柄常驻，读路径直接从映射区 memcpy（Task 3 的 ST_XIP_PTR 同源）。
 * 擦/写期间 cache 关闭、双核停顿 → 包在 codec mute 里（codec 未开时跳过）。
 * 失败约定沿用上游 host hooks：0 = 成功；-1 = 未就绪/硬件/注入失败；
 * -8 = 越界或参数非法。
 */
#include "sloop_hal.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_partition.h"

#define STORE_PART_LABEL "sloopstore"
#define STORE_PART_SUBTYPE 0x99
#define STORE_BASE 0x97000u /* 上游 flash map 逻辑基址（FL_DATA 起） */
#define STORE_SPAN 0x68000u /* [0x97000, 0xFF000) */
#define STORE_SECTOR 0x1000u

static const char *TAG = "sloop_hal_store";

static const esp_partition_t *s_part;
static const uint8_t *s_map;
static esp_partition_mmap_handle_t s_map_handle;
static bool s_ready;

/* 共用写操作计数器：erase/prog 每次调用取一个序号，第 nth 次写命中一次后自动解除；
 * read 在 arm 期间直接失败，但不取序号（spec §4.1「第 n 次写操作」）。 */
static atomic_uint s_fault_nth;
static atomic_uint s_fault_seen;
static atomic_uint s_fault_taken;
static atomic_bool s_fault_half;

/* init 幂等但无锁：只允许单线程开机路径调用（app_console_init 或 Task 3 的
 * sloop_store_init），不做并发保护。 */
esp_err_t sloop_hal_storage_init(void)
{
    if (s_ready) return ESP_OK;

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, STORE_PART_SUBTYPE, STORE_PART_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "分区 %s (data/0x%02x) 不存在", STORE_PART_LABEL, STORE_PART_SUBTYPE);
        return ESP_ERR_NOT_FOUND;
    }
    if (part->size < STORE_SPAN) {
        ESP_LOGE(TAG, "分区 %s 大小不足: 0x%lx < 0x%lx", STORE_PART_LABEL,
                 (unsigned long)part->size, (unsigned long)STORE_SPAN);
        return ESP_ERR_INVALID_SIZE;
    }

    const void *map = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t ret = esp_partition_mmap(part, 0, STORE_SPAN, ESP_PARTITION_MMAP_DATA, &map, &handle);
    if (ret != ESP_OK || map == NULL) {
        if (ret == ESP_OK) ret = ESP_FAIL;
        ESP_LOGE(TAG, "esp_partition_mmap 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_part = part;
    s_map = map;
    s_map_handle = handle;
    s_ready = true;
    ESP_LOGI(TAG, "sloopstore ready: part=0x%lx size=0x%lx map=0x%lx handle=%lu base=0x%lx span=0x%lx",
             (unsigned long)part->address, (unsigned long)part->size, (unsigned long)(uintptr_t)map,
             (unsigned long)s_map_handle, (unsigned long)STORE_BASE, (unsigned long)STORE_SPAN);
    return ESP_OK;
}

bool sloop_hal_storage_ready(void)
{
    return s_ready;
}

const uint8_t *sloop_hal_storage_map(void)
{
    return s_map;
}

uint32_t sloop_hal_storage_base(void)
{
    return STORE_BASE;
}

uint32_t sloop_hal_storage_span(void)
{
    return STORE_SPAN;
}

/* arm 期间 read 失败，但不消耗写序号 */
static bool store_fault_armed(void)
{
    return atomic_load(&s_fault_nth) != 0;
}

/* 写操作（erase/prog）是否命中注入（一次性；命中后 nth 归零，避免后续操作连带失败） */
static bool store_fault_write_hit(void)
{
    uint32_t nth = atomic_load(&s_fault_nth);
    if (nth == 0) return false;
    uint32_t seen = atomic_fetch_add(&s_fault_seen, 1u) + 1u;
    if (seen != nth) return false;
    atomic_store(&s_fault_nth, 0u);
    atomic_fetch_add(&s_fault_taken, 1u);
    return true;
}

void sloop_hal_storage_fault_arm(uint32_t nth, bool half)
{
    atomic_store(&s_fault_seen, 0u);
    atomic_store(&s_fault_taken, 0u);
    atomic_store(&s_fault_half, half);
    atomic_store(&s_fault_nth, nth);
}

uint32_t sloop_hal_storage_fault_taken(void)
{
    return atomic_load(&s_fault_taken);
}

void sloop_hal_storage_fault_clear(void)
{
    atomic_store(&s_fault_nth, 0u);
    atomic_store(&s_fault_seen, 0u);
    atomic_store(&s_fault_taken, 0u);
}

/* [off, off+n) 全落在 [base, base+span) 内 */
static bool store_range_ok(uint32_t off, uint32_t n)
{
    if (off < STORE_BASE) return false;
    uint32_t rel = off - STORE_BASE;
    return rel <= STORE_SPAN && n <= STORE_SPAN - rel;
}

int sloop_hal_storage_read(uint32_t off, void *dst, uint32_t n)
{
    if (!s_ready) return -1;
    /* n == 0 是空操作（dst 可为 NULL，与 prog 同口径）；越界仍拒绝 */
    if ((dst == NULL && n > 0) || !store_range_ok(off, n)) return -8;
    if (n == 0) return 0;
    if (store_fault_armed()) return -1;
    memcpy(dst, s_map + (off - STORE_BASE), n);
    return 0;
}

/* 擦/写期间 cache 关闭、双核停顿：用 codec mute 包住（ADR-009 §3；codec 未开时跳过） */
static void store_mute(bool on)
{
    esp_codec_dev_handle_t codec = sloop_hal_audio_codec();
    if (codec != NULL) esp_codec_dev_set_out_mute(codec, on);
}

int sloop_hal_storage_erase(uint32_t off)
{
    if (!s_ready) return -1;
    if ((off & (STORE_SECTOR - 1u)) != 0 || !store_range_ok(off, STORE_SECTOR)) return -8;
    if (store_fault_write_hit()) return -1;

    store_mute(true);
    esp_err_t ret = esp_partition_erase_range(s_part, off - STORE_BASE, STORE_SECTOR);
    store_mute(false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase 0x%lx 失败: %s", (unsigned long)off, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

int sloop_hal_storage_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!s_ready) return -1;
    if ((src == NULL && n > 0) || !store_range_ok(off, n)) return -8;
    const uint32_t part_off = off - STORE_BASE;

    if (store_fault_write_hit()) {
        /* 半写：payload 前缀落盘后失败，模拟掉电中的 torn payload */
        if (atomic_load(&s_fault_half) && n > 1) {
            store_mute(true);
            (void)esp_partition_write(s_part, part_off, src, n / 2u);
            store_mute(false);
        }
        return -1;
    }

    store_mute(true);
    esp_err_t ret = esp_partition_write(s_part, part_off, src, n);
    store_mute(false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "prog 0x%lx len=%lu 失败: %s", (unsigned long)off, (unsigned long)n,
                 esp_err_to_name(ret));
        return -1;
    }
    return 0;
}
