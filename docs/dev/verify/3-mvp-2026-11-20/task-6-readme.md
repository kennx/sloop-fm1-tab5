# 阶段 3 / Task 6 证据（InputManager：键盘 → 层模型 → EditCmd）

日期：2026-11-20 ｜ 受测固件提交：`bb6f193`（`8b796b6` feat + `54a672c` 挂音修复 +
`bb6f193` 音符指示无锁邮箱；clean 树构建，启动日志 `App version: bb6f193`）。

## A. 终版构型（scratch 注入已撤除，clean 树构建 + 烧录）

| 文件 | 内容 |
|---|---|
| `task-6-boot-log.txt` | RTS 复位后启动日志：`App version bb6f193`、键盘 0x6D/FW 0x01、`UI 就绪`、`InputManager 就绪：55 条映射，轮询 5ms`、`ENG INFO`/`KB`/`UI`/`ENG PERF`（streaming=1 xrun=0）；无 fatal/abort/backtrace |
| `task-6-build-gates.txt` | 全量构建 + 第二遍（0 警告 0 错误，首遍仅豁免的 kconfig 提示对）、镜像 0x16f380、host-test 182 golden、state-test 7 passed、manifest 233/233、烧录三段 Hash。注：首轮 `--clean` 的 configure 一次 components 注册表 TLS 抖动失败（环境），重跑完成全量构建 |
| `task-6-key-transcript.txt` | `KEY` 注入原始 transcript：z note-on/off → `last_note=48`；**按住 c 时按 Aa、松 c 仍发 note-off**（`54a672c` 修复）；Alt+1/5 → `mute k0=1 solo k0=1`；Ctrl+3 → `selected_track=2 focus_track=2`；space playing 1→0；esc/tab 页面 play→tracks；←/↓ 焦点 step 1 / track 2；`KEY 0x81`/错参数打用法并返回非 0；`KEY STATS` |
| `task-6-latency-burst.log` | 100 次**交替** note-on/off（每条 `elapsed_us`；同日志内 note-on/off 均 100 条，确保每次 down 都是真实发声）：note-on mean 0.50ms、p50 0.42ms、p95 0.67ms、max 1.56ms；`KEY STATS count=200 mean_us=496 max_us=1559` |
| `task-6-first-note-per-track.log` | 每轨首音：track 0 0.65ms、切到 track 2 后首音 1.48ms、track 1 首音 0.68ms → 无「每轨首音冷启动」开销（此前的 ≈21ms 归因于旧 flash 实现等 LVGL 帧，已在 `bb6f193` 消除） |
| `task-6-perf-xrun.txt` | 注入后 `ENG PERF`：`streaming=1 xrun=0 audio_stack_free=3148`、`STATE SNAP`、`ENG MEM internal_free=114479` |
| `task-6-play.png` | PLAY 页（无层）回归：与 Task 5 证据 `task-5-play-regress.png` **逐字节相同**（`bde48cc8…`，层提示标签空文本不渲染） |
| `task-6-play-layer-edit.png` | 按住 Aa（`layer_held=0x02`）后 PLAY：页签条右侧 `LAYER EDIT`（UI_ACCENT） |
| `task-6-play-flash.png` | 音符指示：`KEY 0x21 down` 后等 70ms（30ms UI 定时器消费邮箱并渲染），再 `SCR` 取帧；指示框为 track 0 蓝色（78,135 px），证明输入路径的 `sloop_ui_flash_note()` 到达 UI |
| `task-6-tracks.png` / `task-6-step.png` | TRACKS/STEP 回归：`--check` 各 5/5，PNG 与 Task 5 证据逐字节相同 |
| `task-6-screenshot-checks.txt` | 四次截屏的 `--check` 输出 + 区域分析（层提示带 accent 像素 idle 0 / EDIT 371；差异 700 px；指示框轨色像素 78,135） |

## B. scratch 构型（`STATE TOGGLE` 临时命令，**提交前已撤除**）

终版没有「造步内容」的入口（步由 STEP 页触摸创建，`TAP` 注入属 Task 7），EDIT 层擦除的
正向断言需要一条临时命令；与 Task 5 的 scratch 做法一致：

| 临时命令 | 作用 |
|---|---|
| `STATE TOGGLE <k> <idx>` | `state_apply(CMD_TOGGLE_STEP)` 后回读同源步内容，用来造「聚焦步含 A3」的前置态 |

| 文件 | 内容 |
|---|---|
| `task-6-erase-scratch-transcript.txt` | 原始 transcript（采集于 `8b796b6` 的 scratch 构型；擦除路径未被其后两处修复触及）：z 弹 A3(57) → `STATE TOGGLE 0 0`（n=1 note=57）→ Aa 按下（layer=0x02）→ a 按下 → `erase track=0 step=0 note=57` → `ENG STEP n=0 time=2`；再于 focus_step=3 复现；其间 Aa 按住时按 z **不发声**（`last_note` 保持 57） |

撤除证据：终版 `grep -rn "SCRATCH" firmware/{main,components}` 为空；临时命令只在采集构型存在，
终版在 `bb6f193` 上重新构建/烧录/复核（见 `task-6-build-gates.txt`、`task-6-boot-log.txt`）。

## C. 过程发现（两处修复，均含在 `bb6f193`）

1. **挂音**（`54a672c`）：输入入口先按层路由，`c/d/e/g/b` 同时是 EDIT 层音名键；按住音符时
   再按 Aa，松开该键会命中 `ACT_ERASE`（只按下沿动作），note-off 被吞。修复：入口先释放
   「已按下演奏键」再做层路由。
2. **音符指示阻塞输入 21ms**（`bb6f193`）：`sloop_ui_flash_note()` 同步取 LVGL 锁，而 LVGL
   每次失效要等面板帧（实测一次 ≈20ms），导致每条 note-on 的派发被拖到 ≈21ms（instrumented
   构建实测 state_apply 183–187µs、锁+失效 441–688µs）。改为无锁邮箱（30ms 定时器消费）后
   note-on 派发均值 0.50ms、max 1.56ms。

## D. 现场限制

- **DI-4 人测**：本会话无人手按压键盘，`KB` 只证 0x6D 在位、INT 空闲、无积压事件。修饰键
  组合的可行性以键盘内部固件源码核对为准（ADR-006「DI-4 子集核对」），物理核对仍挂 DI-4。
- **人眼目检**：截屏判读全部由像素抽检 + 区域分析完成，观感/可读性人眼项按惯例挂账。
