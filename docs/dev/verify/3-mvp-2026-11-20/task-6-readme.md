# 阶段 3 / Task 6 证据（InputManager：键盘 → 层模型 → EditCmd）

日期：2026-11-20 ｜ 受测固件提交：`6cd7d39`（`8b796b6` feat + `54a672c` 挂音修复 +
`bb6f193` 无锁音符指示 + `6cd7d39` 评审修复：异步切页/PANIC/栈余量；clean 树构建，
启动日志 `App version: 6cd7d39`）。

## A. 终版构型（scratch 注入已撤除，clean 树构建 + 烧录）

| 文件 | 内容 |
|---|---|
| `task-6-boot-log.txt` | RTS 复位后启动日志：`App version 6cd7d39`、键盘 0x6D/FW 0x01、`UI 就绪`、`InputManager 就绪`、`ENG INFO`/`KB`/`UI`/`ENG PERF`（streaming=1 xrun=0）/`KEY STATS input_stack_free=3752`；无 fatal/abort/backtrace |
| `task-6-build-gates.txt` | `build.sh --clean` + 第二遍（0 警告 0 错误，首遍仅豁免的 kconfig 提示对）、镜像 0x16f640、host-test 182 golden、state-test 7 passed、manifest 233/233、烧录三段 Hash |
| `task-6-key-transcript.txt` | `KEY` 注入原始 transcript：z note-on/off；**按住 c 时按 Aa、松 c 仍发 note-off**；Alt+1/5→`mute k0=1 solo k0=1`；Ctrl+3→`selected_track=2 focus_track=2`；space→playing 1→0；esc/tab 页面；←/↓ 焦点；非法 `KEY` 打用法；`KEY STATS` |
| `task-6-latency-burst.log` | 100 次**交替** note-on/off（日志各 100 条）：note-on mean 0.51ms、p50 0.42ms、p95 0.68ms、max 1.37ms；note-off max 1.32ms；`KEY STATS count=200 mean_us=490 max_us=1367 input_stack_free=3752` |
| `task-6-first-note-per-track.log` | 每轨首音：track 0 0.65ms、track 2 1.48ms、track 1 0.68ms → 无「每轨首音冷启动」开销（旧 ≈21ms 归因于同步 flash 等 LVGL 帧，已在 `bb6f193` 消除） |
| `task-6-page-switch-transcript.txt` | **异步切页**：tab 派发 72/65µs、页面 tracks→step、esc 22/61µs → play；`KEY STATS count=6 mean_us=52 max_us=72 input_stack_free=3752`（旧路径一次 tab 174,648µs） |
| `task-6-noteoff-during-switch.log` | 连发 `note-on → tab → note-off → note-on → note-off`：四事件全部处理（无丢键/挂音），`STATE SNAP playing=0 layer_held=0` |
| `task-6-panic-transcript.txt` | `KEY PANIC`：FX 层 + 已按下音符 → 释放 1 个音符、`layer_held 0x01→0x00`；tab 后立即 PANIC → 待切页被撤销（下一拍页面不变） |
| `task-6-perf-xrun.txt` | 注入后 `ENG PERF`：`streaming=1 xrun=0 audio_stack_free=3148`、`ENG MEM internal_free=114483`、`KEY STATS input_stack_free=3752` |
| `task-6-play.png` | PLAY 页（无层）回归：与 Task 5 证据 `task-5-play-regress.png` **逐字节相同**（`bde48cc8…`） |
| `task-6-play-layer-edit.png` | 按住 Aa（`layer_held=0x02`）后 PLAY：页签条右侧 `LAYER EDIT`（UI_ACCENT） |
| `task-6-play-flash.png` | 音符指示：`KEY 0x21 down` 后等 70ms 再 `SCR` 取帧；指示框为 track 0 蓝色（78,135 px） |
| `task-6-tracks.png` | TRACKS 页（由 `KEY tab` **异步切页**到达）：与 Task 5 证据逐字节相同（`9c20f58d…`），证明异步切页渲染正确 |
| `task-6-step.png` | STEP 页：与 Task 5 证据逐字节相同（`70383618…`） |
| `task-6-screenshot-checks.txt` | 四次截屏的 `--check` 输出 + 区域分析（层提示带 accent 像素 idle 0 / EDIT 371，差异 700 px；指示框轨色像素 78,135） |

## B. scratch 构型（`STATE TOGGLE` 临时命令，**提交前已撤除**）

| 临时命令 | 作用 |
|---|---|
| `STATE TOGGLE <k> <idx>` | `state_apply(CMD_TOGGLE_STEP)` 后回读同源步内容，用来造「聚焦步含 A3」的前置态 |

| 文件 | 内容 |
|---|---|
| `task-6-erase-scratch-transcript.txt` | 原始 transcript（采集于 `8b796b6` 的 scratch 构型；擦除路径未被其后修复触及）：z 弹 A3(57) → `STATE TOGGLE 0 0`（n=1 note=57）→ Aa+a → `ENG STEP n=0 time=2`；再于 focus_step=3 复现；Aa 按住按 z 不发声 |

撤除证据：终版 `grep -rn "SCRATCH" firmware/{main,components}` 为空。

## C. 修复轮（评审反馈，含在 `6cd7d39`）

1. **页切换移入 UI 定时器**：esc/tab 只写待切页邮箱（`sloop_ui_request_page`），30ms UI 定时器
   在 LVGL 上下文执行；输入路径不再在持有 input 互斥时等 LVGL 锁（旧路径实测一次 tab
   174,648µs，note-off 可排队/丢）。实测派发 22–72µs，页面下一拍生效。
2. **`KEY PANIC`**：释放输入层跟踪的所有音符、清 `layer_held`、撤销待切页。
3. 输入任务栈高水位 3752/4096B 由 `KEY STATS` 输出（门禁证据）。
4. 焦点步轴不再把鼓轨焦点（3）压回 0；`flash_note` 恢复 `s_started` 守卫；`input_start` 幂等。
5. 状态层测试加强：三音和弦删中间音用**非零邻居 lvl/rat** + **被删音符的 CONT 位**断言字段随移。

## D. 现场限制

- **DI-4 人测**：本会话无人手按压键盘，`KB` 只证 0x6D 在位、INT 空闲、无积压事件。修饰键
  组合的可行性以键盘内部固件源码核对为准（ADR-006「DI-4 子集核对」），物理核对仍挂 DI-4。
- **人眼目检**：截屏判读全部由像素抽检 + 区域分析完成，观感/可读性人眼项按惯例挂账。
