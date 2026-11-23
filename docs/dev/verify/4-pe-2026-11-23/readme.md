# P4-E 设置页阶段收口证据包（2026-11-24）

> 阶段 4 / P4-E：SYSTEM 设置页、settings 生效副本与 PER3 设置字段、NVS 平台三键、键盘 RGB、
> 运行时 palette/theme 与 ZOOM、SELFTEST chip、RAW settings/SIDE、D7 golden 归零（`feat/phase4e`，Task 8 收口）
> 固件提交：`8963fc8`（`task-8-boot-8963fc8.log` 的 `App version: 8963fc8`；矩阵期间工作树仅 `docs/` 改动）
> 设备：Tab5 `/dev/ttyACM0`；ESP-IDF 5.5.5，target `esp32p4`
> 门禁口径：AGENTS §5.5；顺序按任务 brief：clean → 增量 ×2 → host → 烧录 ×3 → 自检 → 十注入 + 三复跑 → 复位 → soak。
> `task-6-*` / `task-7-*` 是前序任务的证据（原样保留，`SHA256SUMS` 条目未变）。

## 1. 门禁矩阵（门禁 → 命令 → 结论）

| # | 门禁 | 命令 | 结果 | 结论 | 证据 |
|---|---|---|---|---|---|
| 1 | 构建（clean） | `scripts/build.sh --clean` | exit 0；仅 §5.5 豁免的 kconfig CMake 提示对（`build.cmake:631` / `project.cmake:765`） | ✅ | `task-8-build-clean.log` |
| 2–3 | 构建（增量 ×2） | `scripts/build.sh` ×2 | exit 0；两次 warning/error 检索均 **0** | ✅ | `task-8-build-{1,2}.log` |
| 4 | 上游 host | `scripts/host-test.sh` | exit 0；`ALL HOST TESTS PASSED (subset)` | ✅ | `task-8-host-test.log` |
| 5 | 音序 / PER3 / settings | `scripts/seq-test.sh` | exit 0；`SEQ HOST TEST PASSED (0 failures)` + `persist-host: PASS (0 failures)` | ✅ | `task-8-seq-test.log` |
| 6 | 状态层 | `scripts/state-test.sh` | exit 0；**37 passed / 0 failed** | ✅ | `task-8-state-test.log` |
| 7 | 旋钮 / 输入 / theme | `scripts/knob-test.sh` | exit 0；**10 passed / 0 failed** | ✅ | `task-8-knob-test.log` |
| 8 | DSYN / FM6 / 采样 | `scripts/synth-test.sh` | exit 0；`SYNTH HOST TEST PASSED` | ✅ | `task-8-synth-test.log` |
| 9 | 截屏工具 | `python3 scripts/screenshot.py --selftest` | exit 0 | ✅ | `task-8-screenshot-selftest.log` |
| 10 | soak 工具 | `python3 scripts/soak-selftest.py` | exit 0；`结果: PASS` | ✅ | `task-8-soak-selftest.log` |
| 11 | 采样上传工具 | `python3 scripts/sample_upload.py --selftest` | exit 0 | ✅ | `task-8-sample-selftest.log` |
| 12 | 设置 fixture 工具 | `python3 scripts/gen_settings_fixture.py --selftest` | exit 0；per3=88 side=80 | ✅ | `task-8-settings-fixture-selftest.log` |
| 13 | vendored 完整性 | `cd third_party && sha256sum -c MANIFEST.sha256` | **233/233 OK** | ✅ | `task-8-manifest.log` |
| 14 | 烧录 ×3 | `scripts/flash.sh /dev/ttyACM0` ×3 | 三次各 3 段 `Hash of data verified` | ✅ | `task-8-flash-{1,2,3}.log` |
| 15 | 启动 | esptool 硬复位后 10s 串口捕获（soak 前） | `App version: 8963fc8`；无 fatal/abort/backtrace/panic/WDT | ✅ | `task-8-boot-8963fc8.log` |
| 16 | 设备 golden | `python3 scripts/device-selftest.py /dev/ttyACM0` | `cases=182 identical=182 mismatch=0 checks=7 deterministic=1 result=PASS` | ✅ | `task-8-device-selftest.log` |
| 17 | 十条注入场景 + 三复跑 | `bash scripts/inject/<name>.sh /dev/ttyACM0`（同一会话） | 十三次全 exit 0；**5,458 PASS / 0 FAIL** | ✅ | `task-8-injection-summary.txt`、`task-8-inject-*.log`、`task-8-<场景>-transcript.log`、64 张 PNG |
| 18 | 阶段 soak | 硬复位、等 30s 后 `python3 scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 60 --save-window 900` | exit 0；实测 **1801s**、31 次采样；`VERDICT: PASS`（断言失败 0） | ✅ | `task-8-soak-summary.txt`、`task-8-soak-transcript.log`、`task-8-soak-stdout.log` |

`main_rc=1` 是既有口径（设备侧不跑 J_CPU 批量，上游把跳过的 job 记为崩溃；不参与判定）。

## 2. 注入场景计数

PASS 数 = 场景 stdout 的 `[PASS]` 行数（含末行汇总）。

| 场景 | 命令（均带 `/dev/ttyACM0`） | PASS / FAIL | PNG（最新一轮） |
|---|---|---:|---|
| transport | `bash scripts/inject/transport.sh` | 312 / 0 | 5 |
| step_edit | `bash scripts/inject/step_edit.sh` | 196 / 0 | 8 |
| tracks | `bash scripts/inject/tracks.sh` | 11 / 0 | 1 |
| nav | `bash scripts/inject/nav.sh` | 157 / 0 | 7 |
| params | `bash scripts/inject/params.sh` | 197 / 0 | 9 |
| store | `bash scripts/inject/store.sh` | 903 / 0 | 8 |
| song | `bash scripts/inject/song.sh` | 354 / 0 | 9 |
| sequence | `bash scripts/inject/sequence.sh` | 562 / 0 | 3 |
| synth | `bash scripts/inject/synth.sh` | 790 / 0 | 3 |
| sys | `bash scripts/inject/sys.sh` | 312 / 0 | 11 |
| sequence 复跑 | 同上 | 562 / 0 | 3 |
| synth 复跑 | 同上 | 790 / 0 | 3 |
| sys 复跑 | 同上 | 312 / 0 | 11 |

首轮十场景 **3,794 PASS / 0 FAIL**；三条复跑 **1,664 PASS / 0 FAIL**。transport 首轮含一次截屏传输失败的
复位重跑（`[RETRY] … 场景从头重跑`，脚本既有口径）。`task-8-inject-*.log` 是主机 stdout（sequence / synth /
sys 的两次运行都保留，`-2` 为复跑）；transcript 与 PNG 只保留最新一轮：
`task-8-<场景>-transcript.log` 对应 `inject-<场景>-<时间戳>.log`（transport 050939 / step_edit 051214 /
tracks 051451 / nav 051508 / params 051735 / store 052027 / song 052648 / sequence 054324 / synth 054609 /
sys 054900），PNG 保留原始时间戳文件名。目录里的 `inject-sys-20261123-214210-*.png` 是 Task 6/7 的历史证据。

## 3. P4-E 关键读数

- **SYSTEM 页与容量**：`UI MEM` est **10,240B** / delta **5,464B** / GROUP build_ms **10ms**；全部最终矩阵
  transcript 的页面构建增量 **5,212–5,504B**、耗时 **7–10ms**。8 个组签各 **122px**（`sys` ② 对第 8 个
  组签 SYS 区域 (926,4)–(1048,36) 的高亮色像素抽检）。有序对：SEQ→SYS 池剩余 **15,396B**、SYS→STEP
  **23,140B**；最低跨组余量仍是 transport 的 SEQ→PRESETS **8,024B**（song 的 FILE→STEP 8,736B、synth 的
  SYNTH→STEP 28,428B），均 ≥4,096B。池往返：nav PLAY↔TRACKS ×10 Δ−28B、song SEQ↔SONG Δ−24B /
  SONG↔PLAY 0B、synth 四组往返 Δ−24B、soak 五组 ×10 Δ+32B，均 ≤128B。
- **D7 golden 归零**：lowcut=1 / reso=6 下 `device-selftest.py` **182/182**，自检后 settings 仍为 1/6（随后恢复）。
- **持久化**：settings / NVS 三键 / REC 三项硬复位保持；RAW settings + SIDE 的 THM2 主题 accent
  `(248,0,248)` 与硬复位保持；`STORE TEST PASS 6/6`。
- **soak**（复位后，`task-8-soak-summary.txt`）：31 次采样 / 1801s；内部堆 68,755 → 68,739B（Δ−16B）、
  `internal_min` **66,016B**（门限 60KB）；streaming=1、**xrun 0**；duty_mean 最大 **12.41%**（上限 18.88%）；
  音频栈最小 **2,868B**（门限 2KB）；KEY 事件 0→300（每拍增量 == 期望）；五组 ×10 往返 pool_free
  46,304 → 46,336（Δ+32B、单次最小 −24B）；save 窗口 900s（停走带 `STORE PROJ SAVE 0` + `LOAD 0`）
  xrun 0 → 0、窗口后 streaming=1；原始日志无 panic/WDT；断言 3,223 PASS / 0 FAIL。
- **SELFTEST chip 耗时**：既有全量实测约 **163s**（182 例 golden 本身），状态行轮询上限 180s——见 §4 的覆盖缺口。

## 4. 未覆盖项与已知异常（如实记录，不构成门禁通过项）

1. **chip 全路径在本矩阵里没有执行**：`sys` ⑧ 的两次最终运行（及 Task 7 的归档运行）在 ⑧ 开头的硬复位后，
   第一次点 chip 就回 `RESET FIRST`（`task-8-sys-transcript.log` 的 TAP 行；stdout 为「本 boot 已有自检：
   SCR 重试不重复跑」分支），即 `.noinit` 的「本 boot 已跑」标记跨 esptool 硬复位存活（本次供电周期内 chip
   早已跑过，`esp_reset_reason()` 不是 POWERON/BROWNOUT）。`SELFTEST 182/182 PASS` 截图显示的是 `.noinit`
   缓存的上一次结果。因此 worker 的 `RUNNING → 182/182 → AUTO 回 on → LIVEHASH 恢复 → 流恢复` 五条断言
   在最终固件上**没有任何归档日志**；`task-7-sys-red-120s.log` 只证明 chip 能进入 `SELFTEST RUNNING`。
   复现探针 `task-8-diag-probe1.py`（`task-8-diag-probe1-transcript.log` 首按即 `RESET FIRST`）。
   要拿到证据须**物理断电重上电**（清 `.noinit`）后重跑 `bash scripts/inject/sys.sh`。
2. **全量 `ENG SELFTEST` 之后音频重启进入持续超预算态**（矩阵三次全量自检后均出现：`task-8-device-selftest.log`
   `ENG AUDIO streaming=1 xrun=680`、`task-8-inject-sys{,-2}.log` ⑤ 后 xrun 415/416）。探针
   `task-8-diag-probe{1,4}.py`：复位后正常（xrun 0、duty ≈9.5%）；全量自检后 `ENG AUDIO ON` 的响应被
   饿死 5.6–35s，随后 **每一块都超预算**（xrun == render_blocks，mean ≈12.6–13ms vs 预算 5.8ms，
   duty ≈220%），`playing=0`、四轨 voices=0，持续数分钟不恢复；停流再起流**不恢复**；`KEY PANIC` 未测到
   恢复；每次全量自检后内部堆 69KB → 33KB（DI-26）。复位后恢复。单例自检（`probe3`）不触发。
   P4-D 的同类日志（`4-pd-2026-11-23/task-12-selftest.log`）自检后 ON 为 xrun 0。
   这会直接影响 SYSTEM chip 自检结束后的听感，须在合并前定位（DI-40）。
3. **视觉**：十场景的像素抽检全过；64 张 PNG 经 Pillow 校验均为 1280×720 RGB、≥64 色、亮度满量程。
   本轮接管者**没有**做图像工具/人眼读图；前任实现者在 `task-8-report` 中自报曾用图像工具读过
   system / selftest-pass / raw-theme 三张（未复核）。人眼读图、物理触摸、亮度/音量/RGB 与听感是外部门禁。

## 5. 文件与校验

- `task-8-*`：本最终矩阵（构建/host/工具/烧录/自检日志、十场景 stdout + 最新 transcript + PNG、soak 摘要与
  原始日志、启动日志、§4 的诊断探针脚本与 transcript）。
- `task-6-*` / `task-7-*`：P4-E 前序任务证据。
- 全部文件（`readme.md` 与 `SHA256SUMS` 自身除外）列入 `SHA256SUMS`，`sha256sum -c SHA256SUMS` 141 项全 OK。
