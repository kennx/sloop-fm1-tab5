# P4-D 阶段收口证据包（2026-11-23）

> 阶段 4 / P4-D：采样槽、DSYN 鼓合成与 FM6 深度 patch（`feat/phase4d`，Task 12 收口）
> 固件提交：`8420627`（构建时工作树仅 `docs/` 改动；boot 段 `App version: 8420627`）
> 设备：Tab5 `/dev/ttyACM0`（P4 v1.3 样片，ST7121 批次）；ESP-IDF 5.5.5，target `esp32p4`
> 门禁口径：AGENTS §5.5；构建顺序按任务 brief：clean → 增量 → host → 烧录 → 自检 → 九注入 → soak。

## 1. 门禁矩阵（命令 → 结果 → 结论）

| # | 门禁 | 命令 | 结果 | 结论 |
|---|---|---|---|---|
| 1 | 构建（clean） | `scripts/build.sh --clean` | exit 0；编译警告 **0** | ✅ |
| 2 | 构建（增量，零警告） | `scripts/build.sh` | exit 0；编译警告 **0**（无 kconfig 首遍提示对） | ✅ |
| 3 | host 回归 | `scripts/host-test.sh` | exit 0；`ALL HOST TESTS PASSED (subset)`（182 golden 0 changed/0 gone） | ✅ |
| 4 | SYNTH host 门禁 | `scripts/synth-test.sh` | exit 0；`SYNTH HOST TEST PASSED` | ✅ |
| 5 | 状态层 | `scripts/state-test.sh` | exit 0；`30 passed, 0 failed` | ✅ |
| 6 | 音序/持久化 | `scripts/seq-test.sh` | exit 0；`SEQ HOST TEST PASSED (0 failures)` + `persist-host: PASS (0 failures)` | ✅ |
| 7 | 旋钮/输入纯函数 | `scripts/knob-test.sh` | exit 0；`7 passed, 0 failed` | ✅ |
| 8 | 截屏工具离线 | `python3 scripts/screenshot.py --selftest` | exit 0；`selftest OK`（13 组断言） | ✅ |
| 9 | soak 工具离线 | `python3 scripts/soak-selftest.py` | exit 0；`结果: PASS` | ✅ |
| 10 | vendored 完整性 | `cd third_party && sha256sum -c MANIFEST.sha256` | exit 0；**233/233 OK** | ✅ |
| 11 | 烧录 | `scripts/flash.sh` | exit 0；**3×** `Hash of data verified` | ✅ |
| 12 | 启动 | 复位后 10s 串口捕获 | `App version: 8420627`；`UI 就绪: 14 页注册/1 已建`；内部堆 94,307B；无 fatal/abort/backtrace/panic/WDT | ✅ |
| 13 | 设备自检 | `python3 scripts/device-selftest.py /dev/ttyACM0` | exit 0；`cases=182 identical=182 mismatch=0 ... deterministic=1 result=PASS` | ✅ |
| 14 | 九条注入场景（同一会话） | `scripts/inject/{transport,step_edit,tracks,nav,params,store,song,sequence,synth}.sh` | 全部 exit 0；**3,455 PASS / 0 FAIL** | ✅ |
| 15 | 阶段 soak（复位后） | `python3 scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 60 --save-window 900` | exit 0；实测 1801s、31 次采样；`VERDICT: PASS`（断言失败 0） | ✅ |

自检前脚本自动 `STORE AUTO OFF` + `ENG AUDIO OFF`、结束 `ENG AUDIO ON`；自检后**复位**再跑注入
与 soak（DI-26：全量自检每次净减 ~40KB 内部堆）。

## 2. 九条注入场景（同会话、无额外重启；各脚本自带归一/设计内复位）

| 场景 | 断言数（PASS/FAIL） | 关键结论（归档 transcript） |
|---|---|---|
| `transport` | **286 / 0** | 走带 PLAY/STOP/BPM±；REC 五态、fill held/next、LVL/RAT/MIC/SPAN/SLIDE、pattern 六动作与 UNDO；SEQ→PRESETS 过渡与播放头 |
| `step_edit` | **196 / 0** | 步窗列编辑与绝对步号、TIE/CONT 深度呈现、鼓 lane 位图（选中/切换/冻结拒绝）、像素抽检 |
| `tracks` | **11 / 0** | 四轨卡选轨与像素 |
| `nav` | **156 / 0** | 7 组/14 页轮转、`UI TAB/SUB`、同拍 tab 语义、组作用域 built=0、PLAY↔TRACKS 池往返 |
| `params` | **197 / 0** | EDIT 四页 + SYNTH 三页（SAMPLE/DSYN/FM6P）编辑、锁定页拒绝、`UI KNOB` 触摸接线、组池水位 |
| `store` | **903 / 0** | 工程 A–D / 用户预置 / FM6 银行 / 自动存档 / `STORE TEST` 6/6 / 旧格式 fixture / FILE 页 / 换音 71/71 |
| `song` | **354 / 0** | SONG 页 16 项配置、section A–D、chain、SONG REC、空 scene 可见拒绝、FILE→STEP 峰值 |
| `sequence` | **562 / 0** | 深度步/锁/工具、REC 与 16 lane 鼓、live sections、quick chain、SONG/loop、硬复位恢复 |
| `synth` | **790 / 0** | 采样上传（SINE 2s、USED→EMPTY→CRC 拒绝）、DSYN 四字段+CRUSH+STORE+复位/自检一致、FM6P ALG/OL/R1 与 bank 往返、三页截屏与池 |

合计 **3,455 PASS / 0 FAIL**。

## 3. 关键读数（归档 transcript / `UI MEM` / 构建日志）

- **SYNTH 组三页**（`UI MEM` delta，跨快照 min–max，Task 9–12 全归档 + soak 预热）：
  SAMPLE **4,360–4,508B**（est 6,144；`页面构建` 4,148–4,508）、DSYN **6,344–6,452B**
  （est 7,168；构建 6,100–6,452）、FM6P **6,980–7,124B**（est 7,168；构建 6,832–7,124；
  最薄余量 44B，刻意 tripwire）。
  **不同快照的 delta 不相加**；逐页门禁是 `delta ≤ est`。
- **SYNTH `sum_delta`（`built=3` 同拍）**：**17,696–17,936B**（`sum_est` 20,480）。
- **跨组峰值**：SYNTH 全建 → STEP 首建 `pool_free 11,244B`（STEP 池增量 22,188B、耗时 30ms；
  Task 11 复跑 27ms）；最终矩阵最低为 `song` ⑩ FILE→STEP **8,924B**（池历史漂移，均 ≥4,096B）。
- **组往返**：10 轮 SEQ/SONG/FILE/SYNTH（代表页轮换）`47,092 → 47,028`（Δ−64B）；
  Task 11 归档两轮为 −84B/−48B（单次最大 92B），均 ≤128B。
- **采样**：`SMP INFO USR1 name=SINE zones=1 kb=21`；上传后按住键 `ENG SNAP voices≥1`；
  ERASE / BEGIN-不-END / CRC 坏数据后 `voices=0` 且 `EMPTY`；CRC 的 `SMP END` 回 `crc`；
  走带中 `SMP BEGIN/ERASE` 回 `playing`。
- **DSYN**：SET WAVE=2/PITCH=12/DECAY=20/LEVEL=180 + CRUSH=40 → `dsu_dirty=1` → `STORE`
  → 硬复位与 `ENG SELFTEST` 后 22B 逐字节一致。
- **FM6P**：`FM6P INFO alg=12 macro=0`；ALG@134 / R1@0 / OL@16 绝对偏移回读；`STORE BANK PUT`
  与 `GET == LIVE`；硬复位后 bank 槽保持；非 FM6 轨 `locked` 且 patch 不变。

## 4. Soak（复位后；`--duration 1800 --interval 60 --save-window 900`）

- **结果**：`VERDICT: PASS`（实测 1801s / 请求 1800s，31 次采样，断言失败 0）。
- **内部堆**：`internal_free 89,171 → 89,187B`（首末差 **+16B**），`internal_min 83,780B`
  （HEAP_FLOOR 61,440B）；无连续下滑。
- **音频**：`streaming=1`、**xrun=0**（全程）；音频任务栈余量 3,132B（≥2KB）；
  `duty_mean` 最大 **14.54%**（上限 18.88%），`duty_max 89.82%`；render 320,114 块。
- **save 窗口（900s，停走带）**：`STORE PROJ SAVE 0` + `LOAD 0`，xrun `0 → 0`（增量 0 ≤16），
  窗口后 `streaming=1`，窗口外累计 xrun 不增长。
- **KEY 事件**：0 → 300（30 轮 × 10，增量 == 期望，无丢键）。
- **跨组池**：PLAY 稳态 10 轮（transport→song→projects→dsyn→play）
  `pool_free 46,756 → 46,816`（Δ **+60B**，单次最小 −56B ≤128B）。
- 无 panic/WDT/backtrace；摘要 `task-12-soak-summary.txt`、原始 transcript
  `task-12-soak-transcript.log`。


## 5. 证据清单与哈希

- 本目录同时保留 Task 0–11 的过程证据（`task-0-baseline.txt`、`task-10-*`、`task-11-*`）、
  Task 12 最终矩阵证据（`task-12-*`）与全分支评审修复轮证据（`task-12-fix-*`）。
- `SHA256SUMS` 覆盖目录内全部非 `SHA256SUMS` 文件；`sha256sum -c SHA256SUMS` 应 100% OK。
- 失败截图规则：文件名加 `-fail`；本轮无失败证据。

## 6. 外部门禁（机器证据不能替代）

- 人耳听感：DSYN `AUDITION`、采样实际出声、FM6 patch 音色（机器只证 voices/状态行）。
- 物理触摸手感（DI-16）与 70 键矩阵（DI-4）、人眼读图（§5.4(2)）。
- 机器已做像素/结构抽检（`task-12-visual-check.txt`），不冒充人眼结论。

## 7. 与上游的有意差异

- **走带中拒绝采样上传**（`SMP ERR playing`；上游允许）。理由：Tab5 擦写停 cache，80KiB
  上传必造成可闻中断；按「STOP BEFORE SAVE」纪律处理（ADR-011 §5）。
- 采样槽不做 A/B、无自定义对象；DSU 仍写 PER3 88..1,551；FM6 live patch 不单独持久化
  （必须存 bank）——均与上游一致。

## 8. 全分支评审修复轮（2026-11-23）

修复范围（代码 + 文档，均不改上游数据布局）：

- **I1（数据安全）**：`STORE RAW` 改用**专用 4,096B PSRAM 暂存**（`s_raw_stage`，init 分配；
  失败 → `ready=false`），不再与 `proj_tmp` 共用。settings/DSU/song 的 `st_load` 与 1s tick
  不会再覆盖未完成上传；`END` 直接把自己的字节交给 `st_save`。console 的「其它 STORE
  子命令先 abort」保留为会话约定（R27）。
- **M1**：实测区间按归档日志修正——DSYN `页面构建` 6,100–6,452B（6,100 来自 soak 预热）、
  FM6P `UI MEM`/构建 6,980–7,124B（7,124 来自 Task 10 fix，7,104 来自 soak）；
  **FM6P 最薄余量 44B 刻意保留为 tripwire**（est 仍 7,168；改 7,680 虽仍在最坏有序对内，
  但没有必要）。
- **M2/M3**：`state_internal.h`/`state.c` 的 FM6 投影注释改为「非 FM6 轨（含鼓）保留 patch、
  engine_ok=0；越界/冻结清零」；`page_dsyn.h` 值路径补齐 `CMD_DSYN_CRUSH` 并区分
  `CMD_DSYN_LOAD_KIT`（KIT SRC）。
- **M4**：spec §4.4 回写实现偏差（`SET U<n>`、`LANE 01..16`、`SND1/2/3`）。
- **M5**：SAMPLE 槽 ERASE 的 20 扇区/80KiB 同步擦写在 AGENTS §3、ADR-008 §3、ADR-011 §4
  记录 UI 停顿；**未单独测时**（量级按 ≤150ms 级；`STORE STATS last_save_ms` 不覆盖采样擦除）。
- **M6**：console `UI DSYN KIT/LANE` 调 `sloop_ui_dsyn_clear_action()`，与页面 chip 同语义。
- **M7**：DSYN 延迟写条目改号 **DI-38**（DI-35 是已删除的旧 CRUSH 条目）。

聚焦门禁（修复后重跑；证据 `task-12-fix-*`）：

| 门禁 | 命令 | 结果 |
|---|---|---|
| 构建 ×2 | `scripts/build.sh` ×2 | 均 exit 0、编译警告 **0** |
| host 五项 | `host-test` / `synth-test` / `state-test` / `seq-test` / `knob-test` | 全 exit 0 |
| 截屏离线 | `python3 scripts/screenshot.py --selftest` | exit 0 |
| 烧录 | `scripts/flash.sh` | exit 0、**3×** `Hash of data verified` |
| 启动 | 复位捕获 | `App version: 7899459-dirty`、14 页、无 fatal/abort/backtrace |
| **I1 设备冒烟** | `STORE RAW BEGIN 688` + 6 块 → `ENG SELFTEST preset/ANALOG/00_808_BOOM` → `STORE RAW END project 3` | **18 PASS / 0 FAIL**（stdout `task-12-fix-i1-smoke-stdout.txt`；原始串口 `task-12-fix-i1-raw-selftest.log`）；`saved len=688`、`FIELDS 3` 与 FUN1 `expected.json` 逐项一致、槽 3 收口 ERASE |

I1 冒烟的关键判别：自检收尾会 `song_reload + dsu_reload`（旧实现把 `proj_tmp` 覆盖成
settings 字节），修复后 END 仍提交上传的 FUN1 字节并通过格式导入。transcript：
`task-12-fix-i1-smoke-stdout.txt`（断言）与 `task-12-fix-i1-raw-selftest.log`（串口原文）。
