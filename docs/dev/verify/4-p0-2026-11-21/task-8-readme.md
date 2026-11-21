# Task 8 — `sloop_core_desc` 描述符读取

日期：2026-11-21｜分支：`feat/phase4`
代码提交：`89693c5`（`feat(components/sloop_core): 参数描述符读取 API 与 ENG DESC`）、
`5a1f572`（`test(scripts): ENG DESC 描述符抽样门禁脚本`）；证据/登记见本目录与
`docs/dev/verification-log.md`（docs 提交）。
受测固件：`89693c5` 源码（clean 构建 ×2 后重烧重采；构建时 HEAD = `0dd1ce3`、工作树含
未提交的新源码，启动日志 `App version: 0dd1ce3-dirty`）。

## 0. 结论

- **完成**。`sloop_core_desc` 按 spec §4.1 逐字签名落地；设备 `ENG DESC` 10 项抽样
  逐字段（label/fmt/min/max/def/unit/enums/first/last）与 `params.c` 期望表一致，
  4 条越界路径回 `ENG DESC ERR`；`scripts/desc-check.py` 退出码 0。
- 构建 ×2 警告 0（clean 首遍仅 §5.5 豁免的 kconfig 提示对）；三段 `Hash of data
  verified`；启动干净；host-test 绿；设备 SELFTEST 单例 PASS（未改 DSP，全量 182
  沿用 P4-0 Task 2 transcript）；manifest 233/233。
- 本任务未改 UI/DSP/状态层，§5.4 截屏视觉门禁不适用。

## 1. 映射决策（API 契约）

| 项 | 决策 | 依据 |
|---|---|---|
| `SLOOP_DESC_TRACK` | `TP[id]`，`id < P_E0`（=63）；`id >= P_E0` 返回 false | `TP[P_E0..P_E7]` 在 params.c 里是零初始化（无 label），上游 `track_desc()` 也把这 8 个 id 交给引擎表；没有轨道上下文时不能猜引擎 |
| `SLOOP_DESC_GLOBAL` | `GP[id]`，`id < G_COUNT` | spec §4.1 / brief Step 1 |
| `SLOOP_DESC_ENGINE` | `ENGINES[engine]->edit[id]`，`engine < NENGINES`、`id < 8` | 同上；`engine` 对 TRACK/GLOBAL 无意义（传 0） |
| **鼓轨哨兵** | `engine == SLOOP_CORE_NENGINES`（12，即 `NENGINES`）且 `id == 0` → `DRUM_KIT_DESC`；哨兵的 `id != 0` 返回 false | 签名没有 track 参数，无法用「当前选轨」判断；上游 `editor.c ed_eng()` 对鼓轨正是报 `NENGINES`（「无引擎」），`params.c track_desc()` 对 `is_drum(t) && id==P_E0` 返回 `DRUM_KIT_DESC`。哨兵值因此与上游同一口径，且与合法引擎号 0..11 不冲突（`_Static_assert(SLOOP_CORE_NENGINES == NENGINES)` 保证）。鼓轨没有 P_E1..P_E7，故 id!=0 拒绝 |
| `unit` | 原样投影上游 `param_desc_t.unit`（多数 NULL）；不改写 | PD() 宏只填 label/fmt/min/max/def，unit=0；PCT/DB/TIME/BPM 等显示单位由 `param_format()` 按 fmt（部分按值）决定，静态字符串无法忠实表达（如 F_TIME 的 ms/s、F_CUTOFF 的 Hz/kHz）。GP 全部走 PD()，**没有任何 G_\* 带 unit**（抽样项改为断言 `unit=-`）；engine 侧 `ANALOG edit[1] DTN` 的 `"ct"` 验证了 pass-through |
| `enums`/`enum_count` | `names != NULL` 时给出：F_ENUM 按 `PE()` 约定每值一名（`max-min+1`，不依赖终止符）；F_INT+names 为 0 终止表（上游 `param_format()` 同口径，值域均分映射） | 直接读 params.c/各引擎 edit[] 的两种用法；FM6 `ALG`（F_INT 0..32 + `N_FM6_ALG` 33 名）验证了后者 |
| 动态 `desc` 钩子 | 不投影（只给基础 `edit[]` 表） | 钩子签名 `(*desc)(track, k)` 依赖轨道状态（如 INSERT 的 A/B/C 随 TYPE 变化、ENV SYNC 的 ATK/DEC/REL），本 API 无 track 参数；留待参数页按轨上下文另议（见 §5） |
| 线程/内存 | 不取引擎锁；`out` 指针指向上游静态表 | 描述符表是 `static const`，与引擎状态无关；UI 可在任意任务读 |

`ENG DESC` 回显格式（`scripts/desc-check.py` 对端）：

```
ENG DESC scope=track engine=0 id=0 label="LVL" fmt=6 min=0 max=127 def=104 unit="-" enums=0 first="-" last="-"
ENG DESC ERR scope=engine engine=13 id=0
```

label/unit/first/last 加引号（上游 label 可含空格，如 `INS A`）；无单位/无枚举名打印 `-`。
参数解析放宽到 0..255，越界值由 `sloop_core_desc` 拒绝后统一回 `ENG DESC ERR`（避免
「参数非法」与「描述符不存在」两种回显混用）。

## 2. 抽样表 vs 设备输出（`task-8-desc-check.log` / `-transcript.log`）

期望值逐项取自 `third_party/sloop-fm1/firmware/src/params.c` 与 `eng_analog.c`/
`eng_fm6.c`/`eng_sample.c`、`drums.c`、生成头 `felucca_samples.h`
（`SMP_NALL=8+4`）与 `felucca_drumkits.h`（`DS_NKITS=32`）：

| # | 项 | label | fmt | min..max | def | unit | enums | first/last |
|---|---|---|---|---|---|---|---|---|
| 1 | track 0 `P_LEVEL` | LVL | 6 (F_DB) | 0..127 | 104 | - | 0 | - |
| 2 | track 1 `P_ATK` | ATK | 3 (F_TIME) | 0..127 | 10 | - | 0 | - |
| 3 | track 5 `P_ED_FLT` | FLT | 2 (F_BIPCT) | -64..63 | 0 | - | 0 | - |
| 4 | track 53 `P_ITYPE` | INSRT | 8 (F_ENUM) | 0..8 | 0 | - | 9 | OFF/CHOR |
| 5 | global 0 `G_BPM` | BPM | 9 (F_BPM) | 40..240 | 90 | - | 0 | - |
| 6 | engine 12 0（鼓轨哨兵） | KIT | 8 (F_ENUM) | 0..45 | 0 | - | 46 | ACOUSTIC/SYN4 |
| 7 | engine 0 `ANALOG` edit 0 | WAVE | 8 (F_ENUM) | 0..4 | 0 | - | 5 | SAW/PWM |
| 8 | engine 0 `ANALOG` edit 1 | DTN | 0 (F_INT) | 0..127 | 10 | **ct** | 0 | - |
| 9 | engine 9 `FM6` edit 0 | ALG | 0 (F_INT) | 0..32 | 0 | - | 33（0 终止表） | PAT/32 |
| 10 | engine 4 `SAMPLE` edit 0 | SET | 8 (F_ENUM) | 0..11 | 0 | - | 12 | PIANO/USR4 |

越界拒绝（4/4）：`track 0 63`（P_E0..P_E7 无轨道上下文）、`global 0 50`（G_COUNT 上界外）、
`engine 13 0`（引擎不存在）、`engine 12 1`（鼓轨只有 P_E0 = kit）。

脚本输出：`DESC CHECK passed=10/10 negative=4/4 result=PASS`（退出码 0）。
transcript 为串口原始回显（只含 `# [desc-check]` 工具注记），命令回显与响应行逐字节保留。

## 3. 门禁证据

| 门禁 | 证据 | 结果 |
|---|---|---|
| §5.5 固件代码（构建 ×2） | `task-8-build-clean.log`、`task-8-build-incremental.log` | `build.sh --clean` 与增量遍退出码 0；编译警告 **0**（clean 首遍仅 §5.5 豁免的 kconfig 提示对，增量遍 CMake 提示 0）；镜像 `0x172c10`（分区余量 82%） |
| §5.5 烧录验证 | `task-8-flash.log`、`task-8-boot-log.txt` | esptool 三段 `Hash of data verified`；启动 `App version: 0dd1ce3-dirty`、`stream started`、`UI 就绪: 4 页注册/1 已建`、`InputManager 55 条映射`、`触摸反馈 OFF`；无 fatal/abort/backtrace/panic/task_wdt |
| 描述符抽样（本任务新增门禁） | `task-8-desc-check.log`、`task-8-desc-check-transcript.log` | 10/10 正例逐字段一致 + 4/4 越界拒绝，`result=PASS`、退出码 0 |
| 引擎/DSP 回归 | `task-8-host-test.log` | `ALL HOST TESTS PASSED (subset)`（182 golden 0 changed/0 gone） |
| 设备 SELFTEST（单例） | `task-8-device-selftest.log` | `preset/ANALOG/00_808_BOOM`：`cases=1 identical=1 mismatch=0 result=PASS`；本任务未改 DSP，全量 182 口径沿用 P4-0 Task 2 transcript |
| 状态层回归 | `task-8-state-test.log` | 10 passed / 0 failed（本任务未改 sloop_state，回归确认） |
| scripts 工具门禁 | `task-8-screenshot-selftest.log` | `screenshot.py --selftest` 全组断言通过 |
| 第三方完整性 | `task-8-manifest.txt` | `sha256sum -c MANIFEST.sha256` 233/233 OK |
| UI/§5.4 | — | 不适用：UI 文件未改，无新页面/像素变化 |

## 4. 改动文件

- `firmware/components/sloop_core/include/sloop_core.h`（编辑）：`sloop_desc_scope_t`/
  `sloop_desc_t`/`sloop_core_desc` + 映射契约注释（含鼓轨哨兵）。
- `firmware/components/sloop_core/src/selftest.c`（编辑）：`desc_enum_count()` 与
  `sloop_core_desc()` 实现。
- `firmware/main/console.c`（编辑）：`ENG DESC` 子命令、usage/help/hint、文件头注释。
- `scripts/desc-check.py`（新建）：10 正例 + 4 越界用例，原始回显落盘。
- `AGENTS.md`（编辑，docs 提交）：§3 组件说明/脚本树、§5.1 脚本表、§5.5 描述符门禁行。
- `docs/dev/verification-log.md`（编辑，docs 提交）：Task 8 登记行。
- 本目录（docs 提交）：11 个证据文件 + 本 readme + `SHA256SUMS` 重算。

## 5. 自审与遗留

自审：
- 签名/字段顺序与 spec §4.1 逐字一致（`const char *label, *unit; uint8_t fmt; int16_t
  min, max, def; const char *const *enums; uint8_t enum_count;`）。
- 无新描述符表；无动态内存；无锁；`out == NULL` 时仅做存在性查询。
- 越界/未知 scope 一律 false 且不改 `out`（console 侧统一回 `ENG DESC ERR`）。
- `desc_enum_count` 对 F_INT 名字表设 64 上限，防上游表若误丢终止符时越界读。
- 脚本无 Pillow 依赖；期望表与源码逐项核对（含生成头里的 32 个采样 kit / 4 个合成 kit）。

Concerns / 后续（不阻塞本任务）：
1. **动态 `desc` 钩子未投影**：INSERT 的 A/B/C、ENV SYNC 的 ATK/DEC/REL、部分引擎
   的 mode-dependent 描述符需要 track 上下文。参数页（P4-A 后续任务）若要显示这些
   参数的动态 label/names，需要另加带 track 的查询 API 或在状态层做映射。
2. `SLOOP_DESC_TRACK` 对 id 53..62（P_ITYPE..P_LPOL）返回 TP 基础表，不反映
   INSERT/ENV SYNC 的运行时替换——同上，属 track 上下文问题。
3. 鼓轨哨兵的 id!=0 拒绝是保守选择（上游对鼓轨 P_E1..P_E7 的行为无定义/无意义）；
   若后续 UI 需要「鼓轨 P_E1..P_E7」的占位描述符，另议。
4. 全量 182 SELFTEST 未在本任务重跑（未改 DSP）；下一任务若触引擎连接关系需按
   §5.5 跑全量。
