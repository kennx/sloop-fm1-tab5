#ifndef SLOOP_HAL_H
#define SLOOP_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"

typedef void (*sloop_hal_render_fn_t)(int32_t *block, uint32_t frames);

#define SLOOP_HAL_USER_SLOT_COUNT 4u
#define SLOOP_HAL_USER_SLOT_SIZE  0x14000u

/* 引擎渲染速率与半缓冲帧数（codec 打开格式、渲染块尺寸）；sloop_core 的
 * SLOOP_CORE_RATE/SLOOP_CORE_FRAMES 须与之相等，由 selftest.c 静态断言兜底 */
#define SLOOP_HAL_AUDIO_RATE   44100u
#define SLOOP_HAL_AUDIO_FRAMES 256u

esp_err_t sloop_hal_init(void);
uint8_t *sloop_hal_user_samples(void);
esp_err_t sloop_hal_audio_init(void);
/* start：启流（codec 须已 init）；清零渲染统计（窗口 = 本次会话）；上次停流
 * 走了超时强删（broken）时拒绝并返回 ESP_ERR_INVALID_STATE。
 * stop：协作停流（任务自挂起后删除，codec 保持打开）；超时强删返回
 * ESP_ERR_INVALID_STATE 并锁死后续 start，需复位设备。 */
esp_err_t sloop_hal_audio_start(sloop_hal_render_fn_t render);
esp_err_t sloop_hal_audio_stop(void);
esp_codec_dev_handle_t sloop_hal_audio_codec(void);
esp_err_t sloop_hal_audio_max_all(void);
uint32_t sloop_hal_audio_xruns(void);
bool sloop_hal_audio_streaming(void);
uint32_t sloop_hal_audio_stack_high_water(void);

/* 渲染耗时统计（窗口 = 本次流会话）：均值/峰值 µs 与渲染块数；任一出参可为 NULL。
 * 由音频任务在每块渲染后更新（与 xrun 判定同一次测量），console `ENG PERF`
 * 用它给出流式占空比（预算 = SLOOP_HAL_AUDIO_FRAMES / SLOOP_HAL_AUDIO_RATE）。
 * `sloop_hal_audio_start` 清零三者，故 duty_* 只覆盖本次启流之后的块——
 * 首块可能因冷缓存/首次渲染偏高（实测 ~4.3ms），计入峰值但不排除。 */
void sloop_hal_audio_render_stats(uint32_t *mean_us, uint32_t *max_us, uint32_t *blocks);

/* 存储后端（原始分区 sloopstore + mmap，ADR-009 §2/§3）。
 * 偏移一律是**上游 flash map 逻辑偏移**，有效区间 [base, base+span) =
 * [0x97000, 0xFF000)，分区内偏移 = off − base；不是芯片绝对地址。
 * 失败（找不到分区/mmap 失败）时 ready=false，上层 API 按「空存储」返回。 */
esp_err_t sloop_hal_storage_init(void);   /* 幂等；仅限单线程开机路径（无锁，见实现） */
bool sloop_hal_storage_ready(void);
const uint8_t *sloop_hal_storage_map(void); /* 映射基址（对应逻辑偏移 base），未就绪为 NULL */
uint32_t sloop_hal_storage_base(void);      /* 0x97000（常量，与 ready 无关） */
uint32_t sloop_hal_storage_span(void);      /* 0x68000（常量，与 ready 无关） */

/* 读 = 映射区 memcpy（st_xip 同源）。0 = 成功；-1 = 未就绪/注入失败；
 * -8 = 越界或参数非法（沿用上游 host hook 约定）。 */
int sloop_hal_storage_read(uint32_t off, void *dst, uint32_t n);
/* 擦除一个 4KB 扇区（off 须 4KB 对齐）；擦前 codec mute、擦后 unmute（codec 未开时跳过）。 */
int sloop_hal_storage_erase(uint32_t off);
/* 编程；写前同样包 mute（ADR-009 §3「两者都包在 mute 里」）。 */
int sloop_hal_storage_prog(uint32_t off, const void *src, uint32_t n);

/* 故障注入（设备 torn-write 套件）：**写操作**（erase/prog）共用一个计数器，
 * 第 nth 次写命中一次——erase 返回 -1；prog 在 half 时只写前 n/2 字节再返回 -1，
 * 否则不写返回 -1。arm 期间 read 一律返回 -1，但**不消耗计数**（spec §4.1：
 * 计数只由写操作递增，读失败可见且不影响写序号定位）。命中后自动解除（nth=0）。 */
void sloop_hal_storage_fault_arm(uint32_t nth, bool half);
uint32_t sloop_hal_storage_fault_taken(void); /* 自 arm/clear 起命中次数 */
void sloop_hal_storage_fault_clear(void);

#endif
