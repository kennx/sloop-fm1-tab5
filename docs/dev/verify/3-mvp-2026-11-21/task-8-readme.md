# 阶段 3 MVP 收口证据（2026-11-21）

> 受测固件：**2065a68**（构建时工作树干净，启动日志 `App version: 2065a68` 自证）。
> 采集机时：2026-11-21 00:16–01:02 CST；Tab5 USB-C → `/dev/ttyACM0`。
> 登记见 `docs/dev/verification-log.md`；本目录 `SHA256SUMS` 覆盖全部文件。
> 阶段 3 的过程证据（Task 4–7）在 `docs/dev/verify/3-mvp-2026-11-20/`，本目录只放收口复跑与 soak。

## 1. 门禁结果

| 门禁（AGENTS §5.5） | 结果 | 证据 |
|---|---|---|
| 固件代码（构建 ×2，clean + 增量） | ✅ 0 警告；首遍仅 §5.5 豁免的 kconfig 提示对 | `task-8-build-clean.log`、`task-8-build-incremental.log` |
| 烧录验证 | ✅ `Hash of data verified` ×3 | `task-8-flash.log` |
| 启动日志 | ✅ 无 fatal/abort/Backtrace/task_wdt；键盘 0x6D、UI 4 页、InputManager 55 映射 | `task-8-boot-log.txt` |
| 引擎/DSP 回归 | ✅ `ALL HOST TESTS PASSED (subset)`；182 golden 0 changed/0 gone | `task-8-host-test.log` |
| 状态层单测 | ✅ 7 passed | `task-8-state-test.log` |
| scripts 工具 | ✅ `--selftest` 7 组 | `task-8-screenshot-selftest.log` |
| 注入脚本 ×3（真机） | ✅ 三条各退出码 0（断言 + 像素抽检） | `task-8-{transport,step_edit,tracks}.log` + PNG + `-run.txt`（stdout 断言摘要） |
| 内存门禁（idle 内部堆 ≥60KB） | ✅ `internal_free=113887`（min_since_boot 98612） | `task-8-mem-transcript.log` |
| 设备 SELFTEST 单例 | ✅ `preset/ANALOG/00_808_BOOM 0b447c66765e1780` identical=1 | `task-8-selftest-case.log` |
| 阶段收口 soak（30min） | ✅（见 §3） | `task-8-soak.log`、`task-8-soak-summary.txt` |
| third_party 完整性 | ✅ `sha256sum -c MANIFEST.sha256` 233/233 | 本目录 `task-8-gates.txt` §5 |

全量 182/182 同源设备等价沿用阶段 2 post-fix 证据（`2-engine-2026-11-20/task-6-selftest-full-postfix.log`）：
本阶段没有改动 DSP/引擎代码，收口只做单例复核（脚本自动停流/恢复）。

## 2. 截屏矩阵（四页 × 关键状态）

| 页面 | 状态 | 证据 | SHA256（16 位） | 像素抽检 |
|---|---|---|---|---|
| PLAY | idle（无层、无音符） | `task-8-play.png`（本次采集，选轨归一后） | bde48cc8a3b7ffe0… | `--check` 5/5 |
| PLAY | 按住 EDIT 层 | `3-mvp-2026-11-20/task-6-play-layer-edit.png` | 65a43064… | accent 像素 0→371 |
| PLAY | 音符指示（KEY 注入） | `3-mvp-2026-11-20/task-6-play-flash.png` | d69bf755… | 轨色框 78,135 px |
| TRACKS | 选中卡 3（轨 2）、其余未选中 | `task-8-tracks-sel3.png` | b0a188cf488ec703… | 5/5 |
| STEP | 空网格（仅列号） | `3-mvp-2026-11-20/task-5-step-empty.png` | 70383618… | 5/5 |
| STEP | 轨 0 步 2 + 轨 1 步 5 亮，焦点 (1,5)，停止 | `task-8-step-two-on.png` | fd14f09ca7c778c8… | 5/5 |
| STEP | 播放中播放头两帧（引擎 pos 锚定） | `task-8-step-playhead-{a,b}.png` | c8543abf25a36609… / cc6cbd450c6ba27f… | 区域计数 |
| TRANSPORT | 播放中、BPM 90 | `task-8-transport.png` | 9608ad10dbe7b13b… | 4/4 |

本次三张截屏与 Task 7 证据的关系（证明 TAP 注入点改为控件矩形中心后行为未变）：

- `task-8-step-two-on.png`、`task-8-tracks-sel3.png` 与 Task 7 证据**逐字节相同**
  （`fd14f09c…` / `b0a188cf…`）；
- `task-8-transport.png` 与 Task 7 的差异仅 1,535 px，全部落在状态行「TRACK n STEP x / 16」
  文本区（bbox 48,392–250,411）——播放中抓帧的步位置不同所致，非渲染回归。

## 3. 30min soak

- 驱动：`scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 300 --cycle 12`
  （播放中 → STEP 翻格 → TRACKS 选卡 → KEY 音符 → 回 TRANSPORT，每 12s 一轮；
  每 300s 采样 `ENG MEM` / `ENG PERF` / `KEY STATS` / `STATE SNAP`）。
- 判定：内部堆非单调下降且 ≥60KB；每次采样 `streaming=1`、`xrun=0`；音频任务栈余量 ≥2KB；
  串口日志无 panic/WDT/fatal/backtrace；切页回显与 STATE 页面断言全过；时长 ≥1800s。
- 结果：**PASS**（7 次采样，t=0/300/600/900/1200/1500/1800s；150 个动作循环、150 次步翻转、
  150 次选轨、150 组 KEY note on/off（300 事件）；数据表与判定原文 `task-8-soak-summary.txt`）。

```
    t_s  internal_free  internal_min  psram_free  streaming  xrun  audio_stack_free  key_mean_us  key_max_us  input_stack_free  page
      0         114367         98612    30128928          1     0              3148         1028        1909              3752     3
    300         114367         98612    30128928          1     0              3148          910        1909              3752     3
    600         114371         98612    30128928          1     0              3148          884        1909              3752     3
    900         114371         98612    30128928          1     0              3148          892        1909              3752     3
   1200         114363         98612    30128928          1     0              3148          890        1909              3752     3
   1500         114363         98612    30128928          1     0              3148          882        1909              3752     3
   1800         114351         98612    30128928          1     0              3148          885        1909              3752     3
```

- 判定逐条：
  1. 内部堆非单调下降：114,367 → 114,351B（净 −16B，抖动 ±20B；600/900s 处回升），
     min_since_boot 全程 98,612B 不变；≥60KB 门禁余量 54KB → 通过；
  2. 每次采样 `streaming=1`、`xrun=0`（累计计数，30min 未增）→ 通过；
  3. `audio_stack_free` 恒定 3,148B（≥2KB）→ 通过；
  4. 串口日志无 panic / Guru Meditation / task_wdt / abort / Backtrace（grep 计数 0）→ 通过；
  5. 150 轮切页（每轮 4 次 `UI` + `STATE SNAP` 断言）全部落地，无 miss、无页面漂移 → 通过；
  6. 时长 1801.8s ≥1800s → 通过。
- 输入延迟记录：KEY 注入累计均值 884–1,028µs、最大 1,909µs（1.91ms，远低于 ≤20ms 门禁；
  高于 Task 6 隔离测量的 1.37ms，解释为 soak 中每轮连续 4 次切页/状态写后的突发排队，
  非退化趋势——均值在 30min 内平稳，最大值为累计值只增不减）。
- 未覆盖：soak 不含 SCR 截屏（避免 DI-5 干扰与 1.8MB×N 带宽），截屏矩阵由 §2 独立采集。

### 3.1 占空比（评审修复轮补测，2026-11-21）

30min soak 时 `ENG PERF` 流式分支尚无占空比字段（判据「占空比不劣化」未量化）。
评审修复轮在 `sloop_hal_audio_render_stats()`（音频任务每块更新 mean/max/blocks）
基础上，让 `ENG PERF` 流式分支按块预算 5804µs 打印 `duty_mean/duty_max`，并补测：

| 场景 | render_mean_us | duty_mean | duty_max | 证据 |
|---|---|---|---|---|
| 阶段 2 静默渲染基线（离线 100 块） | 548 | 9.44% | 11.73% | `2-engine-2026-11-20/task-4-audio-perf.log` |
| UI live，idle（启动后） | 548 → 538 | 9.44% → 9.27% | 52.60% | `task-8-fix-render-duty.txt` |
| UI live + 播放 + 每 12s 步/选轨/切页/音符（2min 冒烟） | 574 → 589 | 9.89 → 10.03 → 10.15% | 68.87 → 74.62% | `task-8-soak-mini-summary.txt` |

判定：`duty_mean ≤ 2× 基线 = 18.88%`（`scripts/soak.py` DUTY_MAX_PCT）。实测最坏 10.15%
（1.08× 基线），无劣化；`duty_max` 为启动首块与状态写后的块级峰值（<100%，未超预算，xrun=0）。
soak.py 冒烟（`--duration 120 --interval 60 --cycle 12`）**VERDICT: PASS**，同时验证了
新的内部堆判据（HEAP_FLOOR/HEAP_NET_FLOOR/HEAP_JITTER）、internal_min 判定与
「KEY 事件计数增量 == 期望」断言。


## 4. 失败证据（DI-5 复发）

`task-8-transport-scr-stall-fail.log`：transport 脚本首轮在截屏帧传输
208800/1843200 字节处 PC 侧超时；直接读串口证实设备仍在发帧但速率约 8.4KB/s
（正常 ≈1MB/s），无 panic/WDT。手工 esptool 硬复位（`--after hard_reset`，当次手段；脚本级
自动重试见 `docs/dev/device-notes.md` §8.1）后同一脚本 48.8s 通过（见 §1 表格）。
详细观测与处置见 `docs/deferred-issues.md` DI-5（已删除，见 `docs/dev/device-notes.md` §8.1）
与 `docs/dev/device-notes.md` §8。

## 5. 文件索引

- 门禁汇总：`task-8-gates.txt`；构建：`task-8-build-{clean,incremental}.log`；
  烧录 `task-8-flash.log`；启动 `task-8-boot-log.txt`；引擎诊断 `task-8-mem-transcript.log`、
  `task-8-selftest-case.log`；回归 `task-8-host-test.log`、`task-8-state-test.log`、
  `task-8-screenshot-selftest.log`
- 注入：`task-8-{transport,step_edit,tracks}.log`（原始串口回显）、`*-run.txt`（断言摘要）、
  `task-8-{transport,step-two-on,tracks-sel3,step-playhead-a,step-playhead-b}.png`
- soak：`task-8-soak.log`、`task-8-soak-summary.txt`（30min，2026-11-21 00:25）
- 评审修复轮：`task-8-fix-build-gates.txt`（构建/烧录/启动 + 回归）、`task-8-fix-boot-log.txt`、
  `task-8-fix-render-duty.txt`（占空比补测）、`task-8-soak-mini.log`、`task-8-soak-mini-summary.txt`
- 截屏抽检记录：`task-8-screenshot-checks.txt`（PLAY 两次尝试 + 三脚本抽样口径）
- 失败：`task-8-transport-scr-stall-fail.log`（原始回显；首轮 stdout 未单独留档）
