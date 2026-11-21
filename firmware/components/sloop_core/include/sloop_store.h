#ifndef SLOOP_STORE_H
#define SLOOP_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 存储层（P4-B）：上游 SLOOP 的 storage.c（对象 = 一对 4KB 扇区，A/B + 32B 提交头 +
 * CRC + 回读校验）与 project.c 的格式层（FUN6 写、FUN5/4/3/2/1 读）原样编入本组件，
 * 数据落在原始分区 sloopstore（ADR-009）。返回码约定：
 *   ESP_OK                 成功
 *   ESP_ERR_NOT_FOUND      槽为空（RAM 槽无效且 flash 无有效副本）
 *   ESP_ERR_INVALID_STATE  走带中（对齐上游 "STOP BEFORE SAVE"）或存储未就绪
 *   ESP_ERR_INVALID_ARG    槽号越界
 *   ESP_ERR_NOT_FINISHED   flash 擦/写/回读校验失败（RAM 槽保留新值，与上游一致）
 * 所有 API 不得在 ISR 中调用；写操作由存储锁串行化（console/UI/自动存档共用）。 */

#define SLOOP_STORE_PROJECTS 4u
#define SLOOP_STORE_PRESETS 32u

typedef struct {
    bool ready;               /* PSRAM 槽/暂存已分配且分区 mmap 就绪 */
    uint32_t objects_ok;      /* flash 中能读出有效副本的工程对象数（0..4） */
    uint32_t faults_armed;    /* HAL 故障注入是否待命中（1 = 已 arm 未触发） */
    uint32_t faults_taken;    /* 自上次 arm/clear 起命中次数 */
    uint32_t autosave_writes; /* 自动存档写入计数（P4-B Task 6 起填写，本任务恒 0） */
} sloop_store_stats_t;

/* sloop_core_init 内调用（ADR-009 §5：snapshot → store init → autosave resume）：
 * PSRAM 分配（RAM 工程槽 4×3,816B、旧格式暂存、4,096B 通用暂存）→ HAL 分区/mmap →
 * 四个工程槽开机 fetch。任一失败只让 ready=false（不崩溃、不阻断启动）。 */
esp_err_t sloop_store_init(void);
bool sloop_store_ready(void);
void sloop_store_stats(sloop_store_stats_t *out);

/* 工程 A–D（0..3）：RAM 槽（PSRAM）+ flash 双层，与上游 project.c 同语义。
 * save = 引擎锁内 capture 到 RAM 槽 → 释放锁 → st_save（擦写期间不持引擎锁）；
 * load = RAM 槽有效则直接用，否则 st_load + proj_import 回填，再引擎锁内 proj_apply；
 * erase = 写入空对象（len=0）+ 清 RAM 槽；hash = project_t.sum（0 = 空/无效）。 */
bool sloop_store_project_used(uint32_t slot);
uint32_t sloop_store_project_hash(uint32_t slot);
uint32_t sloop_store_live_hash(void); /* 当前引擎状态的捕获 hash（不写 flash） */
esp_err_t sloop_store_project_save(uint32_t slot);
esp_err_t sloop_store_project_load(uint32_t slot);
esp_err_t sloop_store_project_erase(uint32_t slot);

/* 用户预置 0..31（上游 upreset.c：32 槽 / 2 个银行对象，`UPB1` 记录布局原样）。
 * 写 = 当前**选中轨**的声音（引擎 + 除混音/图案参数外的全部参数 + 前 16 步图案）；
 * 读可指定任意合成轨；鼓轨没有声音可存 ⇒ 写/读都回 ESP_ERR_INVALID_ARG（写另出
 * ui_message("DRUM TRACK: NO SOUND")，与上游 SAVE > USER 页一致）。
 * 名字大写、0 结尾（13 字节，上游 up_name 口径）；空槽 not_found；整表查询只读 mmap，
 * 但会在存储锁内重扫银行索引（见 store_tab5.c 的 .bss 回滚说明）。
 * load 不写 flash、可在走带中调用（上游 MIDI PC 语义）；store/erase 走带中拒绝。 */
uint32_t sloop_store_preset_count(void); /* 已用槽数（0..32） */
bool sloop_store_preset_used(uint32_t slot);
bool sloop_store_preset_name(uint32_t slot, char out[13]);
uint8_t sloop_store_preset_engine(uint32_t slot); /* 引擎号；空槽 0xFF */
esp_err_t sloop_store_preset_store(uint32_t slot);
esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track);
esp_err_t sloop_store_preset_erase(uint32_t slot);

#endif
