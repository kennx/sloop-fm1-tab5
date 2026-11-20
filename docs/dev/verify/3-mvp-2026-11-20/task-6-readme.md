# 阶段 3 / Task 6 证据（InputManager：键盘 → 层模型 → EditCmd）

日期：2026-11-20 ｜ 受测固件提交：`8b796b6`（`feat(components/sloop_ui): InputManager 70 键映射与层模型`）；
clean 树构建时 HEAD=`66b86f8`（docs 提交），故启动日志 `App version: 66b86f8`。

## A. 终版构型（scratch 注入已撤除，clean 树构建 + 烧录）

| 文件 | 内容 |
|---|---|
| `task-6-boot-log.txt` | RTS 复位后启动日志：`App version 66b86f8`、键盘 0x6D/FW 0x01、`UI 就绪`、`InputManager 就绪：55 条映射，轮询 5ms`、`ENG INFO`/`KB`/`UI`/`ENG PERF`（streaming=1 xrun=0）；无 fatal/abort/backtrace |
| `task-6-build-gates.txt` | `build.sh --clean` + 第二遍（0 警告 0 错误，首遍仅豁免的 kconfig 提示对）、镜像大小、host-test 182 golden、state-test 7 passed、manifest 233/233、烧录三段 Hash |
| `task-6-key-transcript.txt` | `KEY` 注入原始 transcript（复位后一个会话）：z note-on/off → `STATE SNAP last_note=48`；Alt+1/5 → `mute k0=1 solo k0=1`；Ctrl+3 → `selected_track=2 focus_track=2`；space playing 1→0；esc/tab 页面 play→tracks；←/↓ 焦点 step 1 / track 2；`KEY 0x81`/错参数打用法；`KEY STATS` |
| `task-6-latency-burst.log` | 100 note-on + 100 note-off 的 `KEY` 输出（每条 `elapsed_us`）+ `KEY STATS`：count=100 mean 222µs max 20,718µs；count=200 mean 121µs max 20,718µs；除首个 note-on 外 p99 577µs |
| `task-6-cold-first-note.log` | 冷启动归因：切到 track 2 后该轨**首个** note-on 21,666µs、同轨第二个 1,435µs（Track 0 同现象）→ 每轨一次的上游 voice/patch 懒加载（DI-11） |
| `task-6-perf-xrun.txt` | 注入后 `ENG PERF`：`streaming=1 xrun=0 audio_stack_free=3148`、`STATE SNAP`、`ENG MEM internal_free=114555` |
| `task-6-play.png` | PLAY 页（无层）回归截屏：与 Task 5 证据 `task-5-play-regress.png` **逐字节相同**（`bde48cc8…`，新标签空文本不渲染） |
| `task-6-play-layer-edit.png` | 按住 Aa（`layer_held=0x02`）后 PLAY：页签条右侧 `LAYER EDIT`（UI_ACCENT） |
| `task-6-play-flash.png` | 音符指示：`KEY 0x21 down` 与 `SCR` 同连接发出，帧内指示框为 track 0 蓝色（78,135 px），证明输入路径调用了 `sloop_ui_flash_note()` |
| `task-6-tracks.png` / `task-6-step.png` | TRACKS/STEP 回归：`--check` 各 5/5，PNG 与 Task 5 证据逐字节相同 |
| `task-6-screenshot-checks.txt` | 四次截屏的 `--check` 输出 + 区域分析（层提示带 accent 像素 idle 0 / EDIT 371；差异 700 px；指示框轨色像素 78,135） |

## B. scratch 构型（`STATE TOGGLE` 临时命令，**提交前已撤除**）

终版固件没有「造步内容」的入口（步由 STEP 页触摸创建，`TAP` 注入属 Task 7），因此 EDIT 层
擦除的正向断言需要一条临时命令；与 Task 5 的 scratch 做法一致：

| 临时命令 | 作用 |
|---|---|
| `STATE TOGGLE <k> <idx>` | `state_apply(CMD_TOGGLE_STEP)` 后回读 `ENG STEP` 同源步内容，用来造「聚焦步含 A3」的前置态 |

| 文件 | 内容 |
|---|---|
| `task-6-erase-scratch-transcript.txt` | 原始 transcript：z 弹 A3(57) → `STATE TOGGLE 0 0`（n=1 note=57）→ Aa 按下（layer=0x02）→ a 按下 → `erase track=0 step=0 note=57` → `ENG STEP n=0 time=2`；再于 focus_step=3 复现（右移 3 次后 Aa+a → n=0/time=REST）；其间 Aa 按住时按 z **不发声**（`last_note` 保持 57，无 note-on 日志） |

撤除证据：终版 `grep -rn "SCRATCH" firmware/{main,components}` 为空；临时命令仅在上述采集
构型存在，终版在 `8b796b6` 上重新构建/烧录/复核（见 `task-6-build-gates.txt`、`task-6-boot-log.txt`）。

## 现场限制

- **DI-4 人测**：本会话无人手按压键盘，`KB` 只证 0x6D 在位、INT 空闲、无积压事件。修饰键
  组合的可行性以键盘内部固件源码核对为准（ADR-006「DI-4 子集核对」），物理核对仍挂 DI-4。
- **人眼目检**：截屏判读全部由像素抽检 + 区域分析完成，观感/可读性人眼项按惯例挂账。
