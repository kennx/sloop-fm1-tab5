/*
 * FM6P 深度 patch 编辑页（sloop_ui 内部头文件；P4-D Task 10，spec §3.6/§4.1/§4.4）。
 *
 * 一页 = 页眉选择行（OP1..OP6 六个算子 chip、字段页 chip EG/LEVEL/PITCH/PEG/VOICE/
 * LFO、BANK `−  Bnn  +` 三区对象、SAVE、状态行）+ 8 个旋钮（与参数页/DSYN 页同一
 * 4×2 网格与 touch 语义）+ 底部细调 −/+。字段页与算子是**页内 UI 局部态**
 * （`sloop_ui_fm6p_page_*` / `sloop_ui_fm6p_op_*`，console `UI FM6P OP/PAGE` 与页面
 * chip 共用），不新增 app page——一次 app page 只承载 8 个旋钮槽，靠页内选择器
 * 覆盖 6 算子 × 21 字段 + 全局 19 字段。
 *
 * 值路径：读 `app_state_t.fm6p.patch`（`sloop_fm6_field_from_patch` 纯解码），写一条
 * `CMD_FM6_SET`（a=track b=op n=field），成功即用命令同拍刷新的投影重绘本页。
 * ALG（VOICE 页首槽，op 6 field 8）由适配层在同一次引擎锁内清 `P_E0`（ALG 宏 →
 * PAT），因此写 ALG 立即可听；`P_E0 != 0` 时状态行显示 `ALG MACRO n` 提示（宏会
 * 覆盖 patch 的算法）。SAVE = `CMD_BANK_PUT`（目标槽是页内 0..26，与 FM6 宏页的
 * bank 行同一套 27 槽）。
 *
 * 锁定（`st->fm6p.engine_ok == 0`：选中轨不是 FM6，含鼓轨）：旋钮全部 `--`、
 * 选择器/SAVE/± 一律拒绝（**不下发命令**），状态行 `NOT FM6`。
 *
 * 线程：build/update/tap/forget 由 ui.c 在 LVGL 锁内调用；`sloop_ui_fm6p_*` 键盘
 * 路由在输入任务上下文（不取 LVGL 锁，只写状态层/页内局部态，控件由 30ms 定时器
 * 刷新）；`page_fm6p_status_text` 可从 console（REPL 任务）调用（内部临界区拷贝）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

/* 页构建/刷新（ui_page_t 的 build/update）。 */
lv_obj_t *page_fm6p_build(lv_obj_t *parent);
void page_fm6p_update(const app_state_t *st);
/* 页面控件已销毁（组作用域 / 强制失败重建）：清本页控件指针（状态文本访问器的
 * 「未建 → NULL」契约依赖它）。 */
void page_fm6p_forget(void);

/* TAP 注入（坐标为页面区相对坐标；调用方已扣页签条）：与控件回调同一动作函数。 */
bool page_fm6p_tap(int16_t x, int16_t y, sloop_tap_result_t *out);

/* 最近一次动作的状态行文本（console `TAP` 回显与屏幕同源：两处都读本页同一缓冲）。
 * 页未建/已随组作用域销毁返回 NULL。 */
const char *page_fm6p_status_text(void);

/* (op, field) → 显示名（console `TAP` 回显的 `field=`）：取 core 描述符的 label，
 * 空槽/越界返回 NULL。 */
const char *sloop_ui_fm6p_field_name(uint8_t op, uint8_t field);
