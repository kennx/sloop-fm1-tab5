/*
 * 旋钮控件（参数页的最小组合件，ADR-008 §8）：弧线 + 名称/数值文本。
 *
 * 一个旋钮 = **1 个** LVGL 对象：普通 lv_obj + LV_EVENT_DRAW_MAIN 自绘（lv_draw_arc
 * 画底弧/指示弧、lv_draw_label 画两行文本）。物理触摸按下/拖动按角度设值（粗调：
 * 扫过 270°，底部 90° 死区就近取端）；细调用页面级 ± 按钮或键盘 ↑/↓（每步 1 个
 * 原始单位）。TAP 注入不能拖拽：旋钮命中只设焦点，± 按钮命中走与控件回调相同的
 * 动作函数。
 *
 * 不用 lv_arc + lv_label：实测 662B/旋钮/页，8 旋钮 × 3 页在四基础页之后撑爆懒建
 * 预算；单对象自绘约 300B/旋钮（见 ADR-008 的预算表）。
 *
 * 控件本身不调状态层/引擎：值变化经 on_change 交给页面（page_params.c），焦点变化
 * 经 on_focus。「可编辑」由页面决定（引擎页与所选轨引擎不符时只显示 --）。
 *
 * 格式化（fmt → 文本，faithful & simple；单位由 fmt 决定，desc.unit 只用于 F_INT/
 * F_ENUM）：F_ENUM 枚举名；F_PCT/F_BIPCT 百分比；F_DB dB（0 = OFF）；F_TIME 短时间
 * （1.0ms / 250ms / 1.25s）；F_CUTOFF Hz/kHz；F_SEMI 半音（+n st）；F_BPM/F_NOTE/
 * F_ONOFF/F_STEPS/F_SWING/F_FILT 同上游 param_format 口径；其余（F_OCT 等）回落为
 * 原始整数（+desc.unit）。F_TIME/F_CUTOFF/F_LFOHZ 用上游生成表的指数曲线近似
 * （TIME_MS_X10 = 10·1.0752^v 0.1ms、CUTOFF_HZ = 30·1.0507^v Hz、LFO_HZ_X100 =
 * 5·1.0483^v 0.01Hz），显示到与上游 param_format 相同的可读精度。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "sloop_core.h"

/* 旋钮单元内的几何（均由页面布局宏派生；TAP 命中矩形与建控件同源）：
 * cell_x/cell_y 是单元左上角（屏幕页面区坐标，不含页签条）。 */
#define KNOB_OBJ_DX   70 /* 旋钮盒（弧线 + 两行文本）左上角（单元内） */
#define KNOB_OBJ_DY   26
#define KNOB_OBJ_SIZE 160
/* 弧线内部值域：0..1000 线性映射到 [desc.min, desc.max]（原始步进按范围换算）。 */
#define KNOB_ARC_RANGE 1000

typedef struct knob knob_t;
typedef void (*knob_change_cb_t)(knob_t *k, int16_t value);
typedef void (*knob_focus_cb_t)(knob_t *k);

struct knob {
    lv_obj_t *obj;
    sloop_desc_t desc; /* 描述符副本（label/enums 指向上游静态数据） */
    void *user;        /* 页面上下文（page_params 的页/槽） */
    knob_change_cb_t on_change;
    knob_focus_cb_t on_focus;
    int16_t value;     /* 最近绘制/下发的值 */
    bool shown;        /* 当前 label/arc 是否已有值（false = 尚未绘制） */
    bool locked;       /* 当前以 -- 显示（不可编辑） */
    bool active;       /* 焦点态（强调色） */
};

/* 在 parent 内建旋钮（cell_x/cell_y 为单元左上角）。任一控件分配失败返回 false
 * （调用方中止本页构建，半成品容器由 ui.c 回收）。 */
bool knob_create(knob_t *k, lv_obj_t *parent, int16_t cell_x, int16_t cell_y,
                 const sloop_desc_t *desc);
/* 按值重绘（弧线角度 + 两行文本）；值未变时页面应自行跳过（脏检查在页面侧）。 */
void knob_show(knob_t *k, int16_t value);
/* -- 状态（引擎页上下文不符）：弧线回到起点、文本显示 "--"。 */
void knob_show_locked(knob_t *k);
/* 焦点态样式（弧线指示色/文本色）。 */
void knob_set_active(knob_t *k, bool active);

/* 纯函数（无 LVGL）：值 ↔ 弧线值 ↔ 显示文本。 */
int32_t knob_value_to_arc(const sloop_desc_t *d, int16_t v);
int16_t knob_arc_to_value(const sloop_desc_t *d, int32_t arc);
void knob_format_value(const sloop_desc_t *d, int16_t v, char *buf, size_t n);
