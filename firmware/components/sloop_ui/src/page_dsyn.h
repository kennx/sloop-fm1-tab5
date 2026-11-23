/*
 * DSYN 鼓合成编辑页（sloop_ui 内部头文件；P4-D Task 9，spec §3.3/§4.4）。
 *
 * 一页 = 页眉选择行（SYN1..4 四 kit chip、LANE −/Knn/+、字段页 chip
 * SND1/SND2/SND3/KIT、AUDITION、STORE、状态行）+ 8 个旋钮（与参数页同一 4×2 网格
 * 与 touch 语义）+ 底部细调 −/+。字段页是**页内 UI 局部态**（`UI DSYN PAGE` 与页面
 * chip 共用），不新增 app page；kit/lane 是状态层保存的 UI 局部态
 * （`state_set_dsyn_select`），芯片/`UI DSYN KIT/LANE` 共用。
 *
 * 值路径：读 `app_state_t.dsyn`（22B 的纯解码 `sloop_dsyn_field_from_sound`；kit 级
 * crush/src 直取投影），写一条状态层命令：24 个声音字段 = `CMD_DSYN_SET`；KIT 页
 * `CRUSH`（kit 级 0..255）= `CMD_DSYN_CRUSH`；KIT SRC 的值变化 = `CMD_DSYN_LOAD_KIT`
 * （套用工厂 kit、覆盖该 kit）；另有 `CMD_DSYN_AUDITION` / `CMD_DSYN_STORE`。成功即用
 * 命令同拍刷新的投影重绘本页。
 * 格式化只走 `sloop_core_dsyn_text`（ms/Hz/dB/音名），UI 不引入上游表。
 *
 * 线程：build/update/tap/forget 由 ui.c 在 LVGL 锁内调用；`sloop_ui_dsyn_*` 键盘
 * 路由在输入任务上下文（不取 LVGL 锁，只写状态层/页内局部态，控件由 30ms 定时器
 * 刷新）；`page_dsyn_status_text` 可从 console（REPL 任务）调用（内部临界区拷贝）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

/* 页构建/刷新（ui_page_t 的 build/update）。 */
lv_obj_t *page_dsyn_build(lv_obj_t *parent);
void page_dsyn_update(const app_state_t *st);
/* 页面控件已销毁（组作用域 / 强制失败重建）：清本页控件指针（状态文本访问器的
 * 「未建 → NULL」契约依赖它）。 */
void page_dsyn_forget(void);

/* TAP 注入（坐标为页面区相对坐标；调用方已扣页签条）：与控件回调同一动作函数。 */
bool page_dsyn_tap(int16_t x, int16_t y, sloop_tap_result_t *out);

/* 最近一次动作的状态行文本（console `TAP` 回显与屏幕同源：两处都读本页同一缓冲）。
 * 页未建/已随组作用域销毁返回 NULL（`s_status == NULL`；调用方按「没有可回显的页面
 * 状态」处理，与 BANK/SAMPLE 行同一口径）。 */
const char *page_dsyn_status_text(void);

/* 字段码 → 显示名（console `TAP` 回显的 `field=`）：0..23 取 core 描述符的 label，
 * KIT 页两个伪字段返回 `CRUSH` / `KIT SRC`。未知码返回 NULL。 */
const char *sloop_ui_dsyn_field_name(uint8_t code);
