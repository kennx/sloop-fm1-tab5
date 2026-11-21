/*
 * 旋钮控件（参数页的最小组合件，ADR-008 §8）：弧线 + 名称/数值文本。
 *
 * 一个旋钮 = **1 个** LVGL 对象：普通 lv_obj + LV_EVENT_DRAW_MAIN 自绘（lv_draw_arc
 * 画底弧/指示弧、lv_draw_label 画两行文本）。交互语义（ADR-008 §8.2）：
 *   - 按下（PRESSED）：只设焦点 + 记录锚点角度/锚点值，**不改值**；
 *   - 拖动（PRESSING）：相对锚点的角度差 → 值差（270° 扫角 = 整个值域，见
 *     knob_math.c 的 knob_drag_value），钳位；触摸点须在半径 55..95px 的环带内
 *     （中心与盒角不响应，避免跳变）；
 *   - 细调：页面级 ± 按钮或键盘 ↑/↓（每步 1 个原始单位）。
 * TAP 注入不能拖拽：旋钮命中只设焦点，± 按钮命中走与控件回调相同的动作函数。
 * 调试注入（console `UI KNOB`）用合成坐标走**同一**指针处理路径（见 knob_test_*）。
 *
 * 纯数学/格式化在 knob_math.{h,c}（无 LVGL，host 单测 scripts/knob-test.sh）。
 *
 * 不用 lv_arc + lv_label：实测 662B/旋钮/页，8 旋钮 × 3 页在四基础页之后撑爆懒建
 * 预算；单对象自绘约 300B/旋钮（见 ADR-008 的预算表）。
 *
 * 控件本身不调状态层/引擎：值变化经 on_change 交给页面（page_params.c），焦点变化
 * 经 on_focus。「可编辑」由页面决定（引擎页与所选轨引擎不符时只显示 --）。
 *
 * 线程：全部入口在 LVGL 锁内调用（页面 build/update 与控件事件都在 LVGL 任务上下文）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "knob_math.h"
#include "lvgl.h"
#include "sloop_core.h"

/* 旋钮单元内的几何（均由页面布局宏派生；TAP 命中矩形与建控件同源）：
 * cell_x/cell_y 是单元左上角（屏幕页面区坐标，不含页签条）。 */
#define KNOB_OBJ_DX   70 /* 旋钮盒（弧线 + 两行文本）左上角（单元内） */
#define KNOB_OBJ_DY   26
#define KNOB_OBJ_SIZE 160

typedef struct knob knob_t;
typedef void (*knob_change_cb_t)(knob_t *k, int16_t value);
typedef void (*knob_focus_cb_t)(knob_t *k);

struct knob {
    lv_obj_t *obj;
    sloop_desc_t desc; /* 描述符副本（label/enums 指向上游静态数据） */
    void *user;        /* 页面上下文（page_params 的页/槽；由调用方传入，knob 不改写） */
    knob_change_cb_t on_change;
    knob_focus_cb_t on_focus;
    int16_t value;     /* 最近绘制/下发的值 */
    bool shown;        /* 当前 label/arc 是否已有值（false = 尚未绘制） */
    bool locked;       /* 当前以 -- 显示（不可编辑） */
    bool active;       /* 焦点态（强调色） */
    bool dragging;     /* 已按下且在环带内：锚点有效 */
    int32_t anchor_deg;   /* 按下点角度（拖拽基准） */
    int16_t anchor_value; /* 按下时的值（相对拖拽基准） */
};

/* 在 parent 内建旋钮（cell_x/cell_y 为单元左上角）。desc/user/回调都由调用方
 * 传入并保存——本函数只初始化自己的字段（不会再清掉调用方已设的上下文）。
 * 任一控件分配失败返回 false（调用方中止本页构建，半成品容器由 ui.c 回收）。 */
bool knob_create(knob_t *k, lv_obj_t *parent, int16_t cell_x, int16_t cell_y,
                 const sloop_desc_t *desc, void *user, knob_change_cb_t on_change,
                 knob_focus_cb_t on_focus);
/* 按值重绘（弧线角度 + 两行文本）；值未变时页面应自行跳过（脏检查在页面侧）。 */
void knob_show(knob_t *k, int16_t value);
/* -- 状态（引擎页上下文不符）：弧线回到起点、文本显示 "--"。 */
void knob_show_locked(knob_t *k);
/* 焦点态样式（弧线指示色/文本色）。 */
void knob_set_active(knob_t *k, bool active);

/* 调试注入（console `UI KNOB <slot> press|drag <dx> <dy>`）：用相对旋钮中心的
 * 合成坐标走与物理触摸相同的处理路径（press = 焦点 + 锚点；drag = 相对拖拽）。
 * 返回事件是否被接受（press：环带内锚点已建立；drag：锚点有效且在环带内）。 */
bool knob_test_press(knob_t *k, int32_t dx, int32_t dy);
bool knob_test_drag(knob_t *k, int32_t dx, int32_t dy);
