/*
 * 旋钮的纯函数（无 LVGL）：弧线映射、相对拖拽换算、环带判定、值格式化。
 *
 * 单独成文件是为了 host 单测（`scripts/knob-test.sh`）：本文件只依赖
 * `sloop_core.h` 的 `sloop_desc_t` 与上游生成表 `felucca_tables.h`（数据，无代码），
 * 不需要 LVGL/FreeRTOS。knob.c 只负责 LVGL 对象与事件接线。
 *
 * 值显示与上游 `params.c` 的 `param_format` 同一口径、同一生成表
 * （TIME_MS_X10 / CUTOFF_HZ / LFO_HZ_X100 / LEVEL_DB_X10，来自
 * `third_party/sloop-fm1/build/gen/felucca_tables.h`，由 tools/gen_tables.py 生成）：
 *   F_ENUM          枚举名（+desc.unit）
 *   F_PCT           v*100/max %（四舍五入）
 *   F_BIPCT         v*100/64 %（仅 v>0 加 '+'）
 *   F_TIME          TIME_MS_X10[v]：<10ms 1 位小数 / <1s 整数 ms / <10s 2 位小数 s / 其余 1 位小数 s
 *   F_LFOHZ         LFO_HZ_X100[v]：<10Hz 2 位小数 / 其余 1 位小数
 *   F_CUTOFF        CUTOFF_HZ[v]：<1k 整数 Hz / <10k 1 位小数 kHz / 其余整数 kHz
 *   F_DB            v<=0 = OFF；LEVEL_DB_X10[v]（>-10.0dB 1 位小数，其余整数）
 *   F_SEMI          ±n st（仅 v>0 加 '+'）
 *   F_BPM/F_NOTE/F_ONOFF/F_STEPS/F_SWING/F_FILT   同上游
 *   F_INT（含 0 终止 names）与未知 fmt              原始整数（+desc.unit）
 *
 * 拖拽语义（ADR-008 §8.2）：按**角度差**换算值差，扫角 270° = 整个值域；
 * 只响应半径 55..95px 的环带（中心与盒角不产生跳变）。按下不改变值。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sloop_core.h"

/* 弧线内部值域：0..1000 线性映射到 [desc.min, desc.max]。 */
#define KNOB_ARC_RANGE 1000
/* 拖拽换算：整圈扫角（270°）对应整个值域。 */
#define KNOB_DRAG_SPAN_DEG 270
/* 环带（半径像素）：环线中心 73、线宽 14 → 内 66/外 80；留余量取 55..95。 */
#define KNOB_RING_MIN_R 55
#define KNOB_RING_MAX_R 95

/* 值 ↔ 弧线值（0..1000；越界钳位、四舍五入）。 */
int32_t knob_value_to_arc(const sloop_desc_t *d, int16_t v);
int16_t knob_arc_to_value(const sloop_desc_t *d, int32_t arc);

/* 触摸点相对旋钮中心的 (dx,dy) 是否落在环带内（半径 55..95px）。 */
bool knob_ring_hit(int32_t dx, int32_t dy);

/* 角度差（度）：把 to-from 归一化到 (-180, 180]，跨越底部死区时取短路径。 */
int32_t knob_angle_delta(int32_t from_deg, int32_t to_deg);

/* 相对拖拽：anchor + delta_deg×range/270（四舍五入、钳位到 [min,max]）。 */
int16_t knob_drag_value(const sloop_desc_t *d, int16_t anchor, int32_t delta_deg);

/* 值显示文本（见文件头的格式化表）。 */
void knob_format_value(const sloop_desc_t *d, int16_t v, char *buf, size_t n);
