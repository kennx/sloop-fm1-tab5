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
bool sloop_hal_storage_fault_armed(void);     /* 是否已 arm 且尚未命中（STORE STATS 报告） */
void sloop_hal_storage_fault_clear(void);

/* ------------------------------------------------ 平台设置（P4-E Task 3，spec §8.4） ---
 * 背光亮度 / 输出音量 / 输出路由的 RAM 状态 + NVS 命名空间 `tab5` 的三个 u8 键
 * （bright / vol / out）。NVS 只放平台级键，不承载任何 SLOOP 状态（ADR-009 §6）。
 *
 * 写路径两段式：set_* = 钳位 → 平台副作用立即生效（LEDC / codec）→ 记 RAM 值并置
 * nvs_dirty，**不写 flash**；落盘只走 sloop_hal_settings_flush()（1s 存储任务的静默
 * 窗口）。副作用调用不取引擎锁，走带中也可改。开机未等到窗口的改动会丢。 */
esp_err_t sloop_hal_settings_init(void); /* nvs_flash_init + 读三键（缺失 ⇒ 默认 100/70/0）；
                                          * 不应用任何东西，须在 display/audio init 之前调用 */
uint8_t sloop_hal_settings_bright(void); /* 10..100 */
uint8_t sloop_hal_settings_volume(void); /* 0..100 */
uint8_t sloop_hal_settings_output(void); /* 0 speaker / 1 headphone */
bool sloop_hal_settings_nvs_dirty(void);
esp_err_t sloop_hal_settings_set_bright(uint8_t v);  /* 钳位 10..100 + LEDC 立即 + nvs_dirty */
esp_err_t sloop_hal_settings_set_volume(uint8_t v);  /* 钳位 0..100 + 活句柄立即（未开流只存）+ nvs_dirty */
/* 仅 speaker 可用：headphone 回 ESP_ERR_NOT_SUPPORTED 且状态不变（本板扬声器与耳机共用
 * ES8388 LOUT1，无法按寄存器路由，见 sys_settings.c 文件头）。speaker 是开机态，无寄存器写。 */
esp_err_t sloop_hal_settings_set_output(uint8_t v);
void sloop_hal_settings_flush(void); /* nvs_dirty ⇒ nvs_set + commit 后清位；不持任何锁，写成功
                                      * 且期间无新修改才清（失败保留下个窗口重试） */
void sloop_hal_settings_erase(void); /* 诊断：删三键 + commit（下一次 boot 落默认） */

/* 诊断读回：ES8388 的 LOUT1/ROUT1/LOUT2/ROUT2 音量寄存器（0x2E..0x31）。esp_codec_dev_read_reg
 * 对 es8388 不透传，故走裸 I2C。codec/I2C 未就绪或读失败回错误码（out 不写）。 */
#define SLOOP_HAL_ES8388_OUT_ON 0x21u /* 开机固定的输出电平（+4.5dB，阶段 1 人测出声态） */
esp_err_t sloop_hal_settings_output_regs(uint8_t out[4]);

/* 键盘 RGB：level 0..3 ⇒ 亮度 0/20/60/100，颜色 0xRRGGBB（tab5_keyboard_rgb_set 的薄封装，
 * UI 传主题 accent）。rgb_level 是最近一次成功下发的档位（开机未下发前为 0）。 */
esp_err_t sloop_hal_keyboard_rgb(uint8_t level, uint32_t rgb888);
uint8_t sloop_hal_keyboard_rgb_level(void);

#endif
