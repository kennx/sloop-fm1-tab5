# P4-F 功能对齐阶段证据包（2026-11-24/25）

> 阶段 4 / P4-F：DI-26 修复、功能对齐清单、参数族覆盖（chip 框架 / SOUND / MOD·SCALE·VOICE /
> GLOBAL·FX / TOOLS / REC 配置 / 键盘参数锁）、导航 18 页九组、全页面截屏矩阵、`ENG POWER`。
> 分支 `feat/phase4f`（**不合并 main**，用户 2026-11-24 明示）。设备 Tab5 `/dev/ttyACM0`，
> ESP-IDF 5.5.5，target `esp32p4`；门禁口径 AGENTS §5.5。
> **状态**：Task 2–9 各自的门禁已通过并在此入库；**阶段收口的最终设备矩阵由 Task 10b 在同一固件
> `1e8587b` 上重跑并通过（§6：host 全绿、烧录 ×3、182/182、`selftest-heap.py` PASS、十场景首轮
> 4,954/0 + 复跑 2,293/0、截屏矩阵 run 3 475/0）；3h soak 的读数 pending（§6 末）**。
> **最终固件是 `d317a72`**（终审修复波之后）：Task 10c 在它上面重跑了全矩阵并全绿（§7：182/182、
> heap PASS、十场景 5,007/0 + 复跑、截屏矩阵 481/0）；最终 3h soak **运行中**（§7 末）。
> 本文件与 `SHA256SUMS` 不入校验清单；`SHA256SUMS` 覆盖目录内其余全部文件（Task 10c-2 重算，586 项全 OK）。

## 1. 门禁矩阵（门禁 → 命令 → 结论）

逐任务的证据时点不同（各任务在各自的最终固件上跑）；「最终固件」列是**该任务**的受测提交，
不是阶段终态。Task 7 是唯一一次五场景同会话的复测，固件 `e481469`。

| 任务 | 受测固件 | 门禁 → 命令 | 结果 | 结论 | 证据 |
|---|---|---|---|---|---|
| 2（DI-26 / `selftest-heap.py` / worker 栈） | `d22a656`+`c7ee880`+`c4af6c6` | `selftest-heap.py` / 182 例 / `sys` | 两次全量 −268B / −36B；182/182；`sys` 348/0 | ✅ | `task-2-*`（见 `task-2-readme.md`） |
| 3（`sloop_core_desc_track_live`） | `b6120ef` | `desc-check.py` / `synth-test.sh` / `state-test` / `knob-test` / `params.sh` / 182 例 | 10/10 + live 2/2 + 越界 7/7；synth 229 ok；state 37/37；`params` 197/0；182/182 | ✅ | `task-3-*` |
| 4（chip 框架 / MOD·SCALE·VOICE / 九组导航） | `2b3a0b3` | `knob-test.sh` / `params.sh` / `nav.sh` / `transport.sh` / `synth.sh` / 设备探针 / 像素探针 | 15/15；`params` 349/0；`nav` 179/0；`transport` 288/0；`synth` 790/0；探针 179/0；像素探针 | ✅ | `task-4-*` |
| 5（SOUND / GLOBAL·FX chip 页 / 页眉命令；含 R10 修复轮） | `29f8642` | `knob`/`state`/`synth`/`seq` host / `params.sh` / `nav.sh` / 182 例 | 16/16、**41/41**、PASSED；`params` **523/0**；`nav` **180/0**；182/182；`lvgl_stack_free` 5,900B | ✅ | `task-5-*-r10.log`、`task-5-params-stdout.log`、`task-5-nav-stdout.log`、`task-5-device-selftest.log`、`task-5-lvgl-stack-{before,after}.txt` |
| 5 首版（R10 之前，`dcd516f`） | `dcd516f` | `store.sh` / `synth.sh` / `transport.sh` | 907/0、790/0、288/0 | ✅（这三条**未在 `29f8642` 重跑**） | `task-5-{store,synth,transport}-stdout.log` |
| 6（TOOLS / REC 配置行 / 键盘锁·fill；含 R12/R13 与 C1 修复轮） | `4d6825b` | `seq-test`（`test_tools_*`、`test_lock_del_canonical`）/ `state-test` 45/45 / `knob-test` 17/17 / `params` 713/0 / `synth` 794/0 / `nav` 183/0 / `song` 354/0 / 182 | 全过 | ✅ | `task-6-*`（评审 clean） |
| 7（注入矩阵定稿；固件未改） | `e481469` | `params.sh` / `nav.sh` / `tracks.sh` / `step_edit.sh` / `sys.sh`（同会话）/ `soak-selftest.py` / 182 例 | **713/0、583/0、84/0、314/0、354/0**；72/72 有序对 ≥4,096（最紧 6,408–6,424B）；soak-selftest rc=0；182/182 | ✅ | `task-7-*-stdout.log` + `task-7-*-transcript.log`、`task-7-device-selftest.log`、`task-7-nav-09-tab-*.png` |
| 8（全页面截屏矩阵；含评审修复轮） | `e481469` | `screenshot_matrix.py --selftest` / 设备全量 ×2 / `--inject-drift` / REC 拒跑 | 33 条目 / 155 锚点 / 33 条逐条目哈希核对 / 468 PASS / 0 FAIL（run A、B 逐项相同）；漂移 ⇒ exit 1 且 `autosave=0`；REC armed ⇒ 拒跑 | ✅ | `task-8-matrix/`、`task-8-matrix-run{A,B}-stdout.log`、`task-8-drift-*`、`task-8-rec-refuse-*` |
| 9（`ENG POWER` / soak 功耗列） | 源码 = `8788d30`（设备版本串 `d8056e1-dirty`） | `build.sh` ×2 / `flash.sh` / 探针 `p4f-task9-power_probe.py` / `soak-selftest.py` / `soak.py --duration 300 --interval 60` | 0 warning；`ina=ok`；流式下 20 次连读 xrun 0→0；soak-selftest 34 PASS；soak **PASS**（301s、min 66,316B、xrun 0、duty ≤13.54%） | ✅（功耗列仅记录） | `task-9-*` |
| 10a（证据晋升 / 文档 ADR / 注释清扫） | `cd10eb2` | `build.sh` ×2 / `knob` / `state` / `seq` / `synth` / `soak-selftest.py` / `screenshot_matrix.py --selftest` | 第二遍 0 warning；17/17、45/45、PASSED、PASSED、PASS、OK | ✅（未上设备） | `task-10a-gates-summary.txt` |
| 10b（最终矩阵） | `1e8587b`（矩阵锚点修正 `e488b75` 只改脚本） | host 全矩阵 + MANIFEST + 烧录 ×3 / `device-selftest.py` / `selftest-heap.py` / `desc-check.py` / 十场景（含 `sequence`/`synth`/`sys`/`nav` 复跑）/ `screenshot_matrix.py` ×3 / 3h soak | 构建 ×3 第二、三遍 0 warning、MANIFEST 233/233；182/182；heap −260/−44B；desc 10/10+2/2+7/7；首轮 **4,954/0**、复跑 **2,293/0**；矩阵 run 1/2 各 468/1（锚点撞播放头，R16）、run 3 **475/0** | ✅（soak ⏳） | `task-10b-*`（见 §6） |

## 2. 各任务文件说明

`*-stdout.log` 是场景脚本的标准输出（逐条 PASS/FAIL 与末尾汇总）；`*-transcript.log` 是串口
原始回显（只带 `# [inject] …` 注记）。PASS 计数口径：缩进的 `[PASS]` 行数 + 末尾未缩进的
场景汇总行。

**Task 3**（10 项）：`desc-check.log`（10 项静态 + 2 项 live + 7 条越界的 transcript）、
`synth-host-red.log`（桩实现 ⇒ `SYNTH HOST TEST FAILED (1 failure)`，12 个 live 断言失败）/
`synth-host-green.log`（229 ok）、`host-{state,knob,seq}.log`、`device-selftest.log`（182/182）、
`params-stdout.log`（197/0）、`build2.log`、`flash.log`。

**Task 4**（31 项）：`knob-red.log`（10 passed / 5 failed）/ `knob-green.log`（15/15）、
`params-stdout.log`、`nav-stdout.log`、`synth-stdout.log`、`transport-stdout.log`、
`probe-stdout.log`（设备探针 179/0）与 `probe.py`、`pixels-transcript.log`（像素探针的串口回显）
与 `pixels-probe.py`、`build2.log`、`flash.log`、`shot-*.png`（MOD 5 chip / SCALE 4 chip /
VOICE 4 chip 的 chip 行 + MOD、VOICE 三态与 SEQ 子签，共 23 张）。

**Task 5**（30 项）：R10 修复轮的 `params/nav-stdout.log`（523/0、180/0）、`device-selftest.log`、
`lvgl-stack-{before,after}.txt`（点击路径诊断固件的栈高水位：372B → 5,900B）、`stack-hwm.txt`
（定时器路径 2,036B）、`clickpath-diag-stdout.log`、`mem-before.log`（开机 `internal_free`
77,919 → 74,831）、`peaks.txt`、`build2.log`、`flash-r10.log`；R10 之前的 `dcd516f` 上
`store/synth/transport-stdout.log`（907/0、790/0、288/0）与 `flash-final-dcd516f.log`；host 的
RED/GREEN（`host-red-*.log`、`r10-state-{red,green}.log`、`host-golden.log`、`host-seq.log`）；
一次性探针 `probe.py` + `probe-stdout.log`；`shot-*.png`（SOUND 三态、GLOBAL/FX chip、鼓轨）。

**Task 7**（39 项）：五场景的 stdout + transcript。**两段会话**——`params` 与 `sys` 的读数来自
首轮会话（`task-7-params-*`、`task-7-sys-*`）；`tracks`、`step_edit`、`nav` 在评审修复轮改了
断言后**复跑**，`task-7-nav-stdout.log` / `task-7-nav-transcript.log` 是复跑，首轮的 nav 另存
为 `task-7-nav-session1-{stdout,transcript}.log`（两轮都是 583/0；72 对有序对的读数逐对差
≤204B，最紧 SEQ→PRESETS 6,424B / 6,408B，两轮的完整表见 ADR-005 §5.5）。截图：九组签
`task-7-nav-09-tab-<g>.png`、组作用域与 TAB 轮转、GLO/SEL 层键、EDIT 层擦除、TOOLS 预备态。其余：`device-selftest.log`（五场景之后同一开机
182/182）、`soak-selftest.log`、`soak-smoke-{stdout,summary,transcript}.log/txt`（90s 冒烟：
420 断言 0 FAIL、池往返 +32B；判定行里唯一的 FAIL 是 t=1s 首采样的 duty 瞬态，AGENTS §5.1
已记录的「复位后不要立刻起跑」）、`fix-red-frozen-step_edit.log`（冻结用例的 RED 对照：
把 `ENG FREEZE ON` 改成 OFF ⇒ 312 PASS / 1 FAIL）、`build2.log`、`flash.log`。

**Task 8**：`task-8-matrix/` = 修复轮 run B（最新一次全量）：33 张 `<page>-<state>.png`、
`matrix.csv`（逐条目置态命令与锚点判定）、`sheet-<group>.png` ×9 + `sheet-all.png`、
`matrix.log`（串口回显）。`task-8-matrix-run{A,B}-stdout.log`（run A 另带 `-stderr.log`）。
漂移测试与 REC 拒跑：`task-8-drift-attempt{1,2}-stdout.log`（第 1 次尝试的注入钩子位置错、
由收尾核对判漂移；第 2 次是定稿行为）及其产物目录 `task-8-drift-attempt1/`、
`task-8-drift-test/`；`task-8-rec-refuse-stdout.log` + `task-8-rec-refuse/`。事故证据：
`task-8-incident-run1-{stdout,stderr}.log`、`task-8-incident-run1-matrix.{csv,log}`（见 §3）。
首版之后的 run 2 / run 3：`task-8-run{2,3}-stdout.log`。

**Task 9**（14 项）：`build{1,2}.log`、`flash.log`、`power-readings.log`（三次 `ENG POWER` +
两个 3s 窗口各 20 次连读）与 `power-summary.txt`、`power-probe.py`、
`power-readings-v1-4byte-read-FAIL.log`（一笔 4 字节读 ⇒ `ina=err` 的对照）与
`power-readings-v2-range20-100.log`（改温度量程前的读数）、`schematic-ina226.png`（原理图
U31/R39 区域位图）、`soak-smoke-{stdout,summary,transcript}.log/txt`、`soak-selftest.log` 与
`soak-selftest-red-vs-head.log`。

**Task 10a**：`task-10a-gates-summary.txt`（注释清扫后的构建与 host 门禁尾部）。

**Task 10b**（`task-10b-*`，门禁表 = `task-10b-gates-summary.txt`）：
- 构建/host：`build-{clean,2,3}.log`、`host-{golden,seq,state,knob,synth}.log`、`host-summary.txt`、
  `selftest-{screenshot,soak,sample,matrix,matrix-fix}.log`、`manifest.log`。
- 设备：`flash-{1,2,3}.log` + `boot-{1,2,3}.log`（`App version: 1e8587b`）、`device-pre-selftest.log` /
  `device-mid.log` / `device-end.log`（会话三个时点的 `LIVEHASH`/`ENG MEM`）、
  `device-selftest-{stdout,transcript}.log`、`selftest-heap-{stdout,transcript}.log`、
  `desc-check-{stdout,transcript}.log`。
- 十场景首轮：`<场景>-stdout.log` + `<场景>-transcript.log`（最新一次 transcript，原始回显）；
  `scn1-summary.txt` / `scn2-summary.txt` 是运行器的逐场景 rc/耗时。复跑：`{sequence,synth,sys,nav}-rerun-{stdout,transcript}.log`、
  `rerun-summary.txt`；`sys-rerun-killed-stdout.log` 是第一次 sys 复跑被 agent 超时杀掉的半截 stdout。
- 截屏矩阵：`matrix-run{1,2,3}-stdout.log`；`task-10b-matrix/` = run 3（修正后，**最新一次全量**：
  33 张 PNG + `matrix.csv` + `matrix.log` + `sheet-*.png` ×10）；`task-10b-matrix-run{1,2}-fail/` =
  修正前的两次失败运行（同样含全部 PNG 与联系表）。诊断：`anchor-fix-check.log`（旧/新锚点在
  run 1/2 帧上的离线复判 + 反向对照）、`probe-transport.{py,log}`（起播后 `pos=` 轮询）、
  `probe-playhead{.py,-scr.log}`（逐帧数播放头列；该探针的帧传输变慢到 ≈21s/帧，记录在案）。
- soak：`soak-launch.txt`（PID/起跑时间/命令/语义）、`boot-pre-soak.log`。

## 3. Task 8 事故记录（已恢复）

提交前的首版 `screenshot_matrix.py`（未入库的版本）在一次全量运行中把设备工作工程从
`LIVEHASH 2F7BF8A6` 改成 `49B8A18D`（`task-8-incident-run1-stdout.log` 末尾
`[FAIL] LIVEHASH 49B8A18D == 起点 2F7BF8A6`）。根因三处，都是脚本缺陷：

1. `read_vector` 读的是**当前选中轨**，上一条目留下的是鼓轨 ⇒ 向量为空且未报错 ⇒
   `ENG TRACK 0 0 0` 把轨 0 的参数留在预置默认（`VCE` id37：0 → 2）；
2. `sample-usr` 命中了设备自己的 SAMPLE 轨（轨 2），TAP U1 写了 `SET`（id63：5 → 8）却没登记还原；
3. `PARAM BATCH` 每命令 15 对被 esp_console 拒绝，「整份回写」看似执行、实际前 60 项没写
   （本机轨 0 的原始值恰等于预置默认，故此项未额外造成差异）。

**恢复**：esptool `read_flash` 只读 autosave 对象的 A/B 两个副本扇区（A = 事故前，seq 1237；
B = 事故后，seq 1238），用 vendored `project.c` 解码后**全字段只差上述两项**；`PARAM 37 0` +
`PARAM 63 5` 后 `LIVEHASH == 2F7BF8A6`，与事故前逐位一致。随后的全量运行（run 2/3、修复轮
run A/B）autosave tick 已用正确状态覆盖副本 B（`autosave_hash 2F7BF8A6`）。

**防复发**（已入库）：`read_vector` 先选轨且读不满 71 项即报错拒跑；`sample-usr` 的 SET 写
登记还原；BATCH 每命令 14 对；**逐条目 `LIVEHASH` 闸门** + 唯一收口路径 `close_bracket`——
已知漂移时不做任何还原、不碰 flash、autosave 留 OFF 并 exit 1（`--inject-drift` 在设备上
演练，`task-8-drift-attempt2-stdout.log`）。

**残留**（都在工程哈希之外）：恢复用的两次 esptool 复位让 UI 局部态回到开机值；脚本每次运行
会留下不还原的 UI 局部态（被编辑页的**选中 chip**、**旋钮焦点**、状态层的 **`last_param`**）。
未在设备上演练的终止路径：截屏传输失败后的复位重跑（→ 漂移判定）、Ctrl-C 收口、起点
autosave=OFF 的还原分支、设备无响应——只有代码审读与离线 selftest。

## 4. INA226 读数（裁决 R15）

`ENG POWER vbus_mv=918 shunt_uv=-5 cur_ma=-1 temp_c=27.1 ina=ok`。**`vbus_mv` 不是 USB 电压**：
INA226 的 VBUS 引脚接电池侧节点 `SYS_BAT_PRE`（官方原理图第 5 页：`J120 pin1 → Q50 →
SYS_BAT_PRE → R39 → SYS_BAT_2S`），本机由主机 USB-C 供电、未装电池 ⇒ ≈0.9V / 0mA。
**USB 输入功耗不可由这颗 INA226 观测**；`vbus_mv`/`cur_ma` 只有装上 NP-F550 后才有信息
（放电为正是按图推断、未实测）。分流电阻 **R39 = 5mΩ（R005/1%）已查到**，`cur_ma =
shunt_uv / 5`，没有臆造校准值。温度 27–28℃ 是轻载 + 室温的正常读数。soak 的功耗/温度列
**仅记录、不设门禁**。

官方原理图 PDF（2.3MB，第三方文档）不入库：<https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf>，
SHA256 `13cf3fd9954d39aa198e57b1bb94ec56af7d49107c80a245acc5bd90ac8ea329`；
`task-9-schematic-ina226.png` 是其 U31/R39 区域的位图裁剪。查证过程见
`docs/dev/device-notes.md` §10。

## 5. 诚实边界

- **人眼读图是外部门禁**：全部截图只有像素探针/锚点结论，没有做过视觉检查；联系表
  （`task-8-matrix/sheet-*.png`）是给用户复核的材料。
- **输入任务栈余量未被机器验证**：`KEY STATS` 的 `input_stack_free` 断言只在 `phys>0` 时生效，
  上述全部运行都是 console `KEY` 注入（`phys=0`）⇒ 场景显式跳过。需要有人手按一轮
  Aa+k / Aa+l / Aa+9 / 擦除 / 演奏键后再读 `KEY STATS`。
- **物理点击路径的 LVGL 栈**只由 Task 5 的临时诊断固件量过 chip 点击这一条链（5,900B），
  页眉行/±/bank 回调与之同形但未逐条实测。
- **时点**：Task 2–9 的读数各来自其受测固件；`store`/`synth`/`transport`/`sequence`/`song` 当时未在
  P4-F 最终固件上重跑——**已由 Task 10b 在 `1e8587b` 上统一重跑全绿（§6）**。
- **池账目**：base 重测 ≈14.0KB 后，最坏有序对的纸面账略超 63,120B（实测 72/72 对仍
  ≥4,096B）——ADR-005 §5.5、DI-56。
- `step_edit` 的清空断言只查 `time != 0`（`ST_TIE = 1` 也被接受），是否可能留下 TIE 是开放问题。
- Task 6 首轮 `transport` 的一次「还原后 LIVEHASH ≠ 快照」未复现、根因未证明（同固件复跑 288/0）。

## 6. 10b 设备矩阵（2026-11-25，固件 `1e8587b`）

完整门禁表（命令、退出码、耗时、关键读数）在 `task-10b-gates-summary.txt`；本节只给结论与需要知道的边界。
受测固件 = 干净树 `1e8587b` 的 clean 构建（boot `App version: 1e8587b`，无 `-dirty`）；
矩阵期间唯一的代码改动是 `e488b75`（`scripts/screenshot_matrix.py` 的 `step-playing` 锚点选位，固件不变）。

| 门禁 | 结果 |
|---|---|
| host：`build.sh --clean` → `build.sh` ×2、五个 host 门禁、四个 `--selftest`、MANIFEST | 全部 exit 0；第二、三遍构建 0 warning；state 45/0、knob 17/0、seq/synth/golden PASSED；MANIFEST 233 OK / 0 non-OK |
| 烧录 ×3 + 启动日志 | 每次 3× `Hash of data verified`；0 fatal/abort/backtrace/panic |
| `device-selftest.py` | 182/182（`identical=182 mismatch=0 deterministic=1 result=PASS`） |
| `selftest-heap.py` | PASS：run 1 −260B、**run 2 −44B**（门限 ≤1,024B）、收尾 69,955B（门限 ≥61,440B） |
| `desc-check.py` | 10/10 + live 2/2 + 越界 7/7 |
| 十场景首轮（按序、同会话） | transport **288/0**、step_edit **314/0**、tracks **84/0**、nav **583/0**、params **713/0**、store **908/0**、song **354/0**、sequence **562/0**、synth **794/0**、sys **354/0** —— 合计 **4,954 / 0** |
| 复跑 | sequence 562/0、synth 794/0、sys 354/0、nav 583/0 —— **2,293 / 0**，四条两轮同数 |
| `screenshot_matrix.py` run 3 | **475 / 0**、exit 0：33 条目全部解码 1280×720、158 个锚点全中、33/33 条目后 `LIVEHASH == B17A591C`、起点还原 |

关键读数：有序对（nav 首轮 / 复跑）SEQ→PRESETS 6,436 / 6,456B（全矩阵最紧）、EDIT→STEP 12,148 / 12,060B、
MIX→STEP 17,940 / 17,832B、SEQ→MIX 15,592 / 15,460B；`lvgl_stack_free` 最小 5,116 / 5,052B（门限 2,048）；
开机 `internal_free` 70,299–70,319B；全部 18 页 `delta ≤ est`（明细见 `task-10b-gates-summary.txt` §6）。
`sys` 复跑的 DI-40 回归：console 全量自检后 `duty_mean=11.47%`、worker 恢复流式后 9.55%，xrun 增量 0。

**矩阵 run 1 / run 2 的红（已由控制器裁决 R16 定性为工具锚点问题）**：两次都是 468 PASS / 1 FAIL，失败点是
`step-playing.png (89,208)`。该锚点取焦点格左边框（3px 白），播放头落到同一列时以 5px accent 框盖过它；
矩阵脚本不归一焦点轨的 LEN/DIV，当时工作工程的轨 0 是 `len=64 / div=8`，起播后约 1s 播放头停在列 0，
而截屏在起播后约 1.1s（与 Task 8 同延迟）⇒ 确定性撞上。Task 8 的工程轨 0 为 `div=2`（播放头在列 6），
所以同一锚点当时通过。传输本身正常（`probe-transport.log`）。修正 `e488b75` 把播放态的页内锚点移到网格之外
（顶栏 / 行标签 / 列号 / 底部提示），停止态条目仍覆盖网格内像素；离线复判见 `anchor-fix-check.log`。
**评审（Task 10）指出这样移完之后 `step-playing` 的五个页内锚点全是静态家具**——`anchor-fix-check.log` 里
它们在停止帧 `step-default.png` 上的命中计数完全相同，即该条目对「其实没在播」没有判别力；STEP 页也没有
任何播放字样可取。`370a059` 补了**播放头存在性**锚点：`page_step.c` 给播放头所在列的格画 5px accent 框、
停止时不画，取轨行 0/1/3 顶边 4px 带内的 accent 像素——`step-playing` 要求 ≥100、`step-default` 同框
要求 ==0（设备帧实测 224 / 0；不取轨 2，MONO 的 accent 与 track2 同色）；TRANSPORT 的状态行另加
text 像素的 present/absent 对。离线自测对已晋升的 8 张设备帧做了正反向复判（命中 + 反向不命中）。
**新锚点已在设备上验证**（Task 10c，最终固件 `d317a72`：481/0、166 锚点，见 §7；下段是 10b 时点的原注记）；
下表 run 3 的 475/0、158 锚点是加锚点之前的口径（现为 166）。
两次失败的完整目录保留为 `task-10b-matrix-run{1,2}-fail/`。

**边界与需要知道的**：
- 无视觉检查：全部是像素探针/锚点结论。给用户人眼复核的材料是 **`task-10b-matrix/sheet-*.png`**
  （最新一次全量，含 10a 改过提示串之后的 STEP 页三行提示 `Aa+K/…`）；run 1/2 的联系表在 `-fail/` 目录里。
- 输入任务栈断言仍因 `phys=0` 跳过（console `KEY` 注入），人测项不变。
- **工作工程被场景改动（按设计）**：开机基线 `LIVEHASH D6962C61`；矩阵三轮回合内逐条目不变
  （`B17A591C`）；`sys` 复跑的归一 SAVE 之后是 `16FC5CB6`，且 `autosave_hash == LIVEHASH`——flash 的 autosave
  副本现在是这个状态。注入场景本就破坏性/自归一（例如 `sequence` 的自由录音把轨 0 留成 `len=64`）。
  若要回到本会话之前的工程，只能靠工程槽 A–D 里的存档。
- **xrun 读数（与 transcript 逐行核对过）**：`sequence` ⑦ `STORE TEST PASS 6/6` 之后的第一条
  `ENG AUDIO` 是 `xrun=6`（`task-10b-sequence-transcript.log:2581`）——该场景在此之前不读 xrun，
  所以这 6 次落在哪个窗口**不能由 transcript 确定**（最可能是 ⑦ `STORE TEST` 在流式下的两个
  4KB 扇区擦写；存储写窗口按 soak 的口径允许 ≤16，`sequence` 自身对 xrun 没有断言）。
  `sys` 首轮进入场景时 `xrun=1`（`task-10b-sys-transcript.log:7`），复跑 ③ 的窗口是 `2 → 2`
  （`task-10b-sys-rerun-transcript.log:530`）——都是同一开机内更早场景留下的计数，③ 断言的是
  窗口增量 0，两轮都 PASS。这些是**不被任何场景门禁的存储写 / 流重启窗口**；本节只陈述读数，
  不外推「无害」——soak 的 xrun 门禁以它自己复位后的基线为准（AGENTS §5.5）。此前本条写的
  「只见过一次 xrun=1、其后全部 xrun=0」与 transcript 不符，已更正。
- 我的一次手工探针里 SCR 帧传输慢到 ≈21s/帧（DI-5 一类），矩阵脚本自己的 99 次截屏没有失败或重试；
  仅记录。

**3h soak（信号，已出结果：PASS）**——跑在修复前固件 `1e8587b` 上，按裁决 R17 只作信号：3h/10,801s/181 采样，
内部堆 69,831→69,811B、xrun 0、duty_mean ≤12.49%、池 45,912→45,888（Δ−24B）、save 窗口 xrun 增量 0
（`task-10b-soak-signal-summary.txt`、`task-10b-soak-signal-transcript.log`）。最终固件上的 3h soak 见 §7 末。
原始启动记录：`soak.py /dev/ttyACM0 --duration 10800 --interval 60 --save-window 3600`，
PID 441876，起跑 2026-11-25 17:54:38（硬复位 + 30s 之后），stdout `/tmp/p4f_10b/soak-3h.log`，
transcript/摘要 `firmware/build/verify/soak{,-summary}-20261125-175438.{log,txt}`（gitignored，结果由 Task 10c 晋升）。
语义：3,600s 的 save 窗口在最前（停走带、`STORE AUTO OFF`、一次 `PROJ SAVE 0 + LOAD 0`）且计入总时长，
其后约 7,200s 是播放 + 动作循环；与 AGENTS §5.5 的 1800/900 同比例。预期结束 ≈ 20:55。

## 7. 10c 最终固件（`d317a72`）矩阵

逐门禁的完整表在 `task-10c-matrix-summary.txt`；本节只给结论、欠账验证与需要知道的边界。
受测固件 = 干净树 `d317a72` 的 clean 构建（boot `App version: d317a72`，无 `-dirty`）；`d317a72` 之后的
提交只改 `scripts/` 与文档。10c-1 的运行在 2026-11-25 21:09–22:55，10c-2 的复跑在 23:00–23:21。

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建：`build.sh --clean` → `build.sh` ×2 | 全部 exit 0；第二、三遍 0 warning | `task-10c-build-{clean,2,3}.log` |
| host：五个门禁 + 四个 `--selftest` + MANIFEST | 全部 exit 0；state 46/46、knob 17/17、seq/synth/golden PASSED；MANIFEST 233/233 | `task-10c-host-*`、`task-10c-self-*`、`task-10c-manifest.log` |
| 烧录 ×3 + 启动日志 | 每次 3× `Hash of data verified`；0 fatal/abort/backtrace/panic | `task-10c-flash-{1,2,3}.log`、`task-10c-boot-{1,2,3}.log` |
| `device-selftest.py` | 182/182（`result=PASS`，163s） | `task-10c-device-selftest-{stdout,transcript}.log` |
| `selftest-heap.py` | PASS：run 1 −268B、run 2 −36B、收尾 69,887B | `task-10c-selftest-heap-{stdout,transcript}.log` |
| `desc-check.py` | 10/10 + live 2/2 + 越界 7/7 | `task-10c-desc-check-{stdout,transcript}.log` |
| 十条注入场景首轮 | params **768/0**、step_edit **312/0**、tracks **84/0**、nav **583/0**、transport **288/0**、store **908/0**、song **354/0**、sequence **562/0**、synth **794/0**、sys **354/0** —— **5,007 / 0** | `task-10c-m1-*-stdout.log`、`task-10c-owed-params-stdout.log`、`task-10c-<场景>-transcript.log` |
| 复跑 | sequence 562/0、synth 794/0、nav 583/0、sys 354/0（10c-2） | `task-10c-*-rerun-*.log` |
| `screenshot_matrix.py` | **481 / 0**：33 条目 / 166 锚点 / 逐条目后 `LIVEHASH` 33/33 不变；播放头存在性锚点首次在设备上命中 | `task-10c-matrix/`、`task-10c-matrix-stdout.log` |

关键读数：nav 有序对 72/72，最紧 SEQ→FILE 6,620B（presets）、SEQ→SONG 7,152B（≥4,096）；`lvgl_stack_free` 5,116B；
`sys`（首轮 / 复跑）的 DI-40 回归 duty_mean 11.42% / 11.77%、worker 恢复流式后 9.48% / 9.27%。

**终审修复波的欠账验证 a–f**（逐项的原始行在 `task-10c-matrix-summary.txt`）：

- **a（A1 用户预置页眉）通过**：`STORE PRESET SAVE 0` 后改值再 `LOAD` ⇒ 值来自用户预置；页眉 PRESET 名字区在
  用户/工厂两帧间 706 像素不同；`TAP PRESET +/−` 的回显是工厂名（`user` 已清）。**`USER 01` 字面没有被机器读出**
  （无 OCR）——机读证据是 `UI SOUND CHIP`/`TAP` 回显与像素差（裁决 R22）；PNG `task-10c-owed1-a1-user-preset.png`
  留给目检。
- **b（B-I1 槽标签随引擎槽值重解析）通过，GREEN-only**：`params` 768/0，⑫ 段 47 PASS（LOFI/PHYS/NOISE 各档的
  8 槽 label == `ENG DESC live`）；PRESET ± 子用例在正则修正后有判别力。**未在修复前固件上做 RED**。
- **c（播放头锚点）通过**：见上表；`step-playing` 三条轨行带 accent present 各 224 px、`transport-playing`
  状态行 text present 2,038；default 条目的同框 ABSENT 对照全中。§6 里「新锚点尚未在设备上运行」的注记由此作废。
- **d（C-I1 重启路径）pos ✓ / neg ✓**：`step_edit` ②g 在真实设备上走 `TransferError → esptool 复位 → 场景从头重跑`；
  pos 的重跑沿用第一轮快照（不再 SAVE）；neg（快照之后把槽 3 改写）在开括号终止、非零退出。
  **`transport` ⑩g 与 `params` ⑪ 的重启路径未演练**（⑩g 括号内没有截屏）。
- **e（C-I2 歌曲还原）通过**：非默认歌曲（count=2 loop=1 `0:2 1:3`、mode=loop）为入口跑 `params` ⇒ 逐项还原。
- **f（隐藏 chip / CHIP 静默令牌 / TOOLS 预备过期）通过，带一条已知边界**：`UI QUIESCE on` 期间
  `UI MOD CHIP 1` ⇒ `UI ERR quiesced`；TOOLS 预备 2.0s 后再点仍 `armed`、窗口内第三击执行。隐藏 chip 的回显里
  没有 `chip=255`；**同批次 console `TAP` 落在 30ms 同步拍之前时回 `chip=3 select`（不是 miss）**，一拍后 `TAP miss`
  ——按裁决 R21 记为 ADR-008 §8.12 的已知边界（只有同批 console 注入可达）。探针自身有两处错（`SEQ DSTEP` 是鼓轨
  dump；UNDO 不换回 lock/fill 的假设），30 PASS / 2 FAIL 原样保留在 `task-10c-owed2-probe-f*.log`。

**`step_edit` 的红与修复（裁决 R20，只改脚本 `51d2f58`，断言不变）**：10c-1 在真实残留（入口工程轨 0
`mute=1 level=1`）上 ②g `[FAIL] 无 Aa 按住 c：轨 0 voices 峰值 0`；同一固件在默认工程上全绿 ⇒ 场景没有归一
发声前提。修复后 ②g 在快照之后把轨 0 写成 `P_MUTE=0`/`P_LEVEL=104`，收口之后写回入口值。设备验证：

| 入口 | 结果 | 证据 |
|---|---|---|
| 轨 0 `mute=1 level=1`（用 `PARAM` 复原残留——10c-2 起跑时设备入口已是默认工程，残留不在了） | rc=0，**321 PASS / 0 FAIL**；`voices 峰值 1`；场景之后轨 0 仍是 `level=1 mute=1`、LIVEHASH == 入口 | `task-10c-step_edit-rerun-muted-{stdout,transcript,pre,set,post}.log` |
| 默认工程、MONO | rc=0，**321 PASS / 0 FAIL**；LIVEHASH FCE9B26C 前后不变 | `task-10c-step_edit-rerun-{stdout,transcript}.log`、`task-10c-rerun-final-state.log` |
| 默认工程、设备 palette=2 CYAN | rc=1，306 PASS / **14 FAIL**——全是像素探针配色不符 | `task-10c-step_edit-rerun-cyan-{stdout,transcript}.log` |

CYAN 是怎么来的：10c-1 的一次被误起又中止的 `sys` 复跑停在 ③ 的 COLOR 轮换中途（`progress.md` 的「自伤记录」）。
`step_edit` 的像素常量假定 MONO 且场景**不**像 `sys` 那样自归一 palette ⇒ 设备停在别的 palette 时会有十几条像素 FAIL，
控制台断言不受影响。这一轮没有改脚本（范围外），收口报告 §4 记为已知限制；随后 `sys` 复跑按设计把设备归一回 MONO。
10c-1 的矩阵批是在 MONO 上跑的，§7 的十场景读数不受影响。

**CYAN 红的修复（裁决 R23，只改脚本 `93f8f96`，断言不变；设备验证欠给最终 soak 之后）**：
`inject.py` 的 `main()` 给十条场景（含 `sys`）套同一个调色板括号——每轮入口 `palette_normalise` 读
`STORE SETTINGS INFO`，非 MONO（palette 4）或主题开着时 `SET palette 4` / `theme_on 0` 并立即
`STORE SETTINGS SAVE`（SET 只改 RAM，场景开头的硬复位会赶在 1s 任务的静默窗口之前），`finally`
里 `palette_restore` 写回入口值并落盘（断言未过 / `InjectError` 异常终止同样还原）。默认 MONO 入口
零写命令、断言计数不变；`sys` 的调色板轮换用例不变。离线证据：真实 `main()` + 假串口演练 10 例
（MONO 零写 / CYAN 归一·还原 / 异常终止·断言未过仍还原 / ScenarioRestart 重跑 / SAVE 被拒后复位
重新归一 / theme_on / 播放中入口 / init 未完重试），`soak-selftest.py` 与 `screenshot_matrix.py
--selftest` 通过。**尚未在设备上跑**：soak 结束后欠 (1) CYAN 入口的判别运行——先 `STORE SETTINGS SET
palette 2` + `STORE SETTINGS SAVE`，跑 `step_edit.sh`，期望 rc=0、PASS/FAIL 与 MONO 入口同数（≈321/0），
场景之后 `STORE SETTINGS INFO palette=2`；(2) MONO 入口回归：十场景各一遍，PASS/FAIL 计数与 §7 表相同
（括号零断言）；(3) 任一非 MONO 入口（例如 CYAN 上的 `store`，开头就硬复位）证明归一跨复位成立。
`screenshot_matrix.py` 的 `close_bracket` 经复核本就还原入口 palette/theme_on（漂移/复位丢失两条
中止路径按设计不还原）；`soak.py` 没有像素探针，无 MONO 假设。

**边界与需要知道的**：

- 无视觉检查：全部是像素探针/锚点结论。人眼复核的材料是 **`task-10c-matrix/sheet-*.png`**（10 张）。
- 输入任务栈断言仍因 `phys=0` 跳过（console `KEY` 注入）。
- 矩阵结束设备态（10c-1）：`xrun=1`（SELFTEST 条目的停流/起流；硬复位后 0）、工程槽 0–3 全空（`store` 场景按设计擦了槽 0）。
  10c-2 结束时：LIVEHASH FCE9B26C、autosave=1、palette=4、`internal_free` 69,747 / `internal_min` 66,232、xrun=0。
- 10c-2 起跑时的工作工程是 10c-1 矩阵批留下的默认工程（四轨 engine 0 / preset 0 / level 104、BPM 90→120），
  不是用户的工程；开发机的工程槽自 P4-C 起就是场景测试态（收口报告 §7）。
- 一次性探针与重启包装器入库为 `task-10c-probes.py` / `task-10c-restart_wrap.py` / `task-10c-tool-*.py`（没有固化进 `scripts/inject/`）。

**最终 3h soak：运行中**。`soak.py /dev/ttyACM0 --duration 10800 --interval 60 --save-window 3600`，PID 601697，
起跑 2026-11-25 23:24:03（硬复位 + 30s 之后；`task-10c-presoak-boot.log`），stdout `/tmp/p4f_10c2/soak-3h-final.log`，
transcript/摘要 `firmware/build/verify/soak{,-summary}-20261125-232403.{log,txt}`（gitignored，结果出来后晋升）。
**在它出结果之前，阶段 4 的收口门禁是未闭合的**。语义同 §6 末的信号 soak（那一次跑在修复前固件 `1e8587b` 上：
PASS，内部堆 Δ−20B、xrun 0、duty_mean ≤12.49%、池往返 Δ−24B；按裁决 R17 只作信号）。预计结束 ≈ 02:25。
