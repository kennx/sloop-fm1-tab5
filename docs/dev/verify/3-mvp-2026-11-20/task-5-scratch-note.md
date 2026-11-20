# Task 5 scratch 注入构型说明（非终版构型的截屏出处）

日期：2026-11-20 ｜ 终版受测固件：`1e2d5bc`（评审修复版；首版为 `094c270`）

## 为什么有一批截屏不是终版固件拍的

Task 5 时点 `TAP`（Task 7）与 `KEY`（Task 6）注入尚未落地，物理触摸点击无法机器触发。
要让「选中轨 4 / 两个相邻亮格 / 播放头 / 鼓轨聚合 / TIE 步判空」这些**交互态**出现在
PNG 里，必须有一条能从串口构造状态的路径。于是临时加了一组只用于证据采集的命令，
烧录 scratch 构型采集，随后**完全撤除**（`grep 'SCRATCH(Task 5)' firmware/{main,components}` 为空），
在终版上重新 `build.sh --clean`×2 + 烧录并重采终版截屏。

评审修复轮（`1e2d5bc`：鼓轨格点击不再改焦点）后又重建 scratch 构型、重采了受影响的三张图
（`step-drum-msg`、`playhead-a`、`playhead-b`）与整份 transcript，并加了一行程序化断言：
`ASSERT drum tap 不改焦点: before=('0','3') after=('0','3') -> PASS`。

## 临时命令清单（全部已撤除）

| 命令 | 实现 | 验证的是什么 |
|---|---|---|
| `UI TAP card <0..3>` | `lv_obj_send_event(卡片对象, LV_EVENT_CLICKED, NULL)` | 卡片的 `LV_EVENT_CLICKED` 回调 → `CMD_SELECT_TRACK` → 选中/焦点轨 |
| `UI TAP step <r> <c>` | `lv_obj_send_event(格子对象, LV_EVENT_CLICKED, NULL)` | 格子的 `LV_EVENT_CLICKED` 回调 → `state_set_focus` + `CMD_TOGGLE_STEP`（含鼓轨只读分支） |
| `STATE PLAY` / `STATE STOP` | `state_apply({CMD_PLAY/CMD_STOP})` | 与 TRANSPORT 按钮同一条编辑命令路径（走带 → 播放头） |
| `ENG STEPW <k> <i> <n> <time>` | `sloop_core_read_step` 改 n/time 后 `write_step` | 构造 TIE 步（n=1,time=1），验证网格「亮」判定与状态层同口径 |
| `ENG DRUMSTEP <idx> <lane>` | 直接置 `TDRUM->dstep[idx].on` 位 | 鼓轨聚合行渲染（MVP 无鼓步编辑入口，见下） |
| `UI TOUCH ON\|OFF` | `lv_indev_enable(触摸 indev, …)` | 采集期隔离触摸面板的幽灵按压（见 `task-5-readme.md`「现场噪声」/ DI-10） |

探针代码：`sloop_ui_debug_click()`（`ui.c`）→ `page_tracks_debug_click()` / `page_step_debug_click()`
（各页），`sloop_core_debug_drum_step()`（`selftest.c`），`app_display_touch_debug_enable()`（`display.c`）。
scratch 分支名 `tmp/task5-scratch`（首轮）与 `tmp/task5-scratch2`（评审修复轮），两次都在采集后删除
（`git branch` 中不存在）。

## 这批截屏能证明什么、不能证明什么

- **能**证明：控件回调接线（`LV_EVENT_CLICKED` 就是触摸松手时 LVGL 派发的事件）、
  `CMD_SELECT_TRACK`/`CMD_TOGGLE_STEP` 的引擎与状态层效果、脏检查刷新、播放头/焦点/鼓轨聚合的
  渲染与配色。
- **不能**证明：**触摸命中几何**。`UI TAP` 是直接向对象派发事件，没有经过触摸坐标 →
  LVGL 命中测试（滚动容器、重叠对象、圆角边缘、坐标旋转）；也没有覆盖按压时序（长按、
  滑动误触、双击）。这些必须由 Task 7 的 `TAP <x> <y>`（坐标级注入）与真机人测覆盖。
- 采集时触摸 indev 被临时关闭，因此这批截屏**不代表**触摸可用性。

## 与终版构型的行为差异

| 面 | 终版（`1e2d5bc`） | scratch 构型 |
|---|---|---|
| 点击注入 | 无（物理触摸或 Task 7 `TAP`） | `UI TAP card/step` |
| 走带注入 | 无（TRANSPORT 按钮或 Task 6 `space`） | `STATE PLAY/STOP` |
| 鼓步构造 | 仅引擎 REC 录音路径（`seq.c rec_hit`） | `ENG DRUMSTEP` 直接置 dstep 位 |
| 步字段构造 | 无 | `ENG STEPW`（只改 n/time） |
| 触摸 indev | 常开 | 采集期 `UI TOUCH OFF` |
| 鼓轨格点击后的焦点 | 不变（焦点轨仍由合成轨行决定） | 不变（与终版同一代码路径） |
| UI 页代码 | **同一份**（scratch 只加探针，不改渲染逻辑） | 同左 |
| LVGL 池占用（就绪行） | 40,312/59,660B | 39,500–40,312/59,660B（文本长度差异，同一量级） |

## 相关文件

- 终版截屏：`task-5-tracks-sel1.png`、`task-5-step-empty.png`、`task-5-play-regress.png`
- scratch 截屏：`task-5-tracks-sel4.png`、`task-5-step-two-on.png`、`task-5-step-tie.png`、
  `task-5-step-drum.png`、`task-5-step-drum-msg.png`、`task-5-playhead-a.png`、`task-5-playhead-b.png`
- transcript：`task-5-final-transcript.txt`（终版）、`task-5-scratch-transcript.txt`（scratch）
- 抽检/判读：`task-5-screenshot-checks.txt`（A=终版，B=scratch）
- DI-10（幽灵按压未定性）：`docs/deferred-issues.md`
