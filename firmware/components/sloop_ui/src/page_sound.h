/*
 * Sound 仪表盘页面（sloop_ui 内部头文件；5B-1，5B 规格 §5.2；ADR-008 末节）。
 *
 * 主体 = 一个滚动容器 + 13 张自绘卡片（成员表与布局 = sound_layout.h 的纯函数）：选中轨的
 * 全部参数组各占一张卡，卡内每行 4 个槽（140×132，96px 的环）。它作为 PARAM_FEAT_DASHBOARD
 * 挂在参数页框架上——页眉行、FM6 银行行、页级 ± 按钮仍由 page_params.c 建，本模块只管卡片
 * 区，外加两条固定行：右上角的 4 格选轨条与页眉下方的提示/读数行（ZOOM 开时显示焦点槽的
 * 大号读数）。槽按上下文解析成存在位图（不存在的槽是空白格），存在的槽显示值、鼓轨上按
 * drum_ok 锁定，环带内拖动改值；焦点槽画描边，←/→ 在存在槽间环绕移动焦点、↑/↓/ENTER 与
 * 页级 ± 对焦点槽 ±1；点卡片标题行翻折叠位，下一拍重排（焦点若在被折叠的卡里，按「下一个
 * 可聚焦的槽」重指；焦点与折叠位都跨页面重建保留）。
 *
 * 坐标：入口收到的 (x, y) 是页面区相对坐标；sound_layout_* 给的是**内容坐标**（原点 = 列 0 /
 * y 0），两者只差「视口左上角 − 滚动量」，换算在 page_sound.c 的一处（content_point）。
 * console 的矩形/注入入口给的是屏幕绝对坐标 = 页面区坐标 + (0, UI_TAB_BAR_H)。
 *
 * 手势：容器与卡片都不让 LVGL 自己滚（SCROLLABLE/SCROLL_CHAIN/GESTURE_BUBBLE 清掉），滚动
 * 由它们共用的事件回调驱动 lv_obj_scroll_by_bounded——这样「环带内按下 = 旋钮、其余 = 滚动」
 * 的归属在按下那一刻就定了，不经过 LVGL 的滚动判定。
 *
 * 线程：build/update/tap 与持锁的注入入口由 page_params.c / ui.c 在 LVGL 锁内调用，或在 LVGL
 * 事件回调里跑；slot_total / slot_info 不取锁（console 任务直读 PSRAM 里的槽状态）。焦点与折叠
 * 入口（下方一组）同样不取 LVGL 锁——键盘路由在输入任务里调：焦点是 portMUX 保护的 PSRAM
 * 静态，adjust 只发状态层命令；滚动到可见、重排与重绘都留给下一拍 update。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_ui.h"

/* 在 parent（页面根）内建滚动容器 + 13 张卡片；任一对象分配失败返回 NULL（半成品由 ui.c
 * 随页面根删除回收，控件指针由紧随的 page_sound_forget 清掉）。视口先按「银行行隐藏」放，
 * 紧随的首拍 update 按选中轨引擎挪位（构建路径不取状态副本）。 */
lv_obj_t *page_sound_build(lv_obj_t *parent);
/* 每拍：银行行可见性（调用方的谓词，page_params.c 的 bank_row_visible）翻转 ⇒ 挪容器；
 * 上下文（选中轨/引擎/引擎槽值/ENV SYNC/INSERT TYPE）变 ⇒ 重解析全部槽并整卡重绘；
 * 折叠位变 ⇒ 重排 13 张卡片、重钳滚动；否则只刷值变了的槽。最后保证焦点落在存在的槽上、
 * 焦点变化时滚进视口，并按需重绘选轨条与提示/读数行。 */
void page_sound_update(const app_state_t *st, bool bank_visible);
/* 页面销毁（组作用域 / 构建失败）：清控件指针、手势态、存在位图与焦点。 */
void page_sound_forget(void);
/* TAP 注入（页面区相对坐标）：命中选轨条第 k 格 ⇒ 发 CMD_SELECT_TRACK 并回
 * SLOOP_TAP_TRACK_CARD{a=k}（回显同 TRACKS 页）；命中的卡片标题行 ⇒ 翻该卡折叠位并回
 * SLOOP_TAP_SOUND_CARD{a=卡片 v=翻转后的折叠位}；命中**存在**槽的矩形 ⇒ 设焦点并回
 * SLOOP_TAP_SOUND_FOCUS{a=卡片 b=槽 v=扁平下标}；视口外/空白处/不存在的槽未命中返回 false。 */
bool page_sound_tap(int16_t x, int16_t y, sloop_tap_result_t *out);

/* ---- 焦点 / 折叠（5B-1 Task 4）：都不取 LVGL 锁，只写/读页面状态，布局与重绘在下一拍 ---- */
/* ←/→：焦点移到下/上一个**可聚焦**的槽（存在且所在卡片没折叠），环绕；尚无焦点（或焦点悬空）
 * ⇒ 落到首个存在槽。无存在槽时不动。 */
void page_sound_focus_move(int8_t delta);
/* ↑/↓/ENTER、页级 ±：焦点槽 ±1（引擎钳位）。槽不存在/锁定/所在卡片折叠，或页面状态说的不是
 * 此刻的选中轨/引擎（换轨后下一拍之前、页面已销毁）返回 false 且不发命令。 */
bool page_sound_adjust(int8_t delta);
bool page_sound_adjust_value(int8_t delta, int16_t *applied); /* 同上；applied（可 NULL）= 写后投影真值 */
/* 焦点槽；恒为一个可聚焦的槽 = 存在且所在卡片没折叠（悬空的焦点按 update 同一条规则重指：扁平序
 * 下一个可聚焦的槽，环绕；全部折叠则保持原焦点、编辑惰性——焦点槽已不存在时才落到首个存在槽）。无存在槽 = SOUND_SLOT_NONE。页面重建后
 * 首拍重解析之前只回显、不重指。 */
uint16_t page_sound_focus_get(void);
void page_sound_focus_set(uint16_t flat); /* 只写焦点（不校验存在性）；滚到可见在下一拍 */
/* 焦点与折叠位是页内 UI 局部态：跨组页面重建保留。翻折叠位（调用方持 LVGL 锁）；重排卡片在下一拍 */
void page_sound_card_toggle(uint8_t card);
bool page_sound_card_collapsed(uint8_t card);
/* 把焦点槽滚进视口（持锁）：sloop_ui_sound_focus_set 在写焦点后立即调，不等下一拍 */
void page_sound_focus_reveal(void);

/* ---- console `UI SOUND …` 的页面层入口（5B-1 Task 3；公共外壳 sloop_ui_sound_* 在 ui.c） ---
 * 槽下标一律是成员表扁平下标。返回码：页未建 ESP_ERR_INVALID_STATE；下标越界
 * ESP_ERR_INVALID_ARG；槽不存在（或所在卡片折叠）ESP_ERR_NOT_FOUND。 */
uint16_t page_sound_slot_total(void); /* 当前上下文存在的槽数；页未建 = 0。无锁 */
uint16_t page_sound_collapsed(void);  /* 折叠位掩码（一位一张卡）。无锁 */
/* 逐槽诊断；card/slot/scope/id 对存在的槽总有效，其余字段只对存在的槽写。无锁 */
esp_err_t page_sound_slot_info(uint16_t flat, uint8_t *card, uint8_t *slot, uint8_t *scope,
                               uint8_t *id, const char **label, bool *locked, int16_t *value);
/* 槽矩形（屏幕绝对坐标）；失败时四个输出清零。持锁 */
esp_err_t page_sound_slot_rect(uint16_t flat, int16_t *x, int16_t *y, int16_t *w, int16_t *h);
esp_err_t page_sound_scroll_get(int16_t *y, int16_t *max_y); /* 持锁 */
esp_err_t page_sound_scroll_set(int16_t y);                   /* 钳位 [0, max]。持锁 */
/* 槽级旋钮注入：合成点 = 槽圆心 + (dx, dy)，走与物理触摸相同的 sound_handle_pressed/pressing
 * （press = 记锚点不改值；drag = 相对锚点的角度差 → 值差）。ESP_OK = 事件被接受；
 * NOT_FOUND = 环带外/无锚点（或槽不存在）；NOT_ALLOWED = 锁定槽。*value 回填槽当前值。持锁 */
esp_err_t page_sound_test_knob(uint16_t flat, int32_t dx, int32_t dy, bool press, int16_t *value);
