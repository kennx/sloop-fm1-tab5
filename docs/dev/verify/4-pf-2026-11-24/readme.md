# P4-F 功能对齐阶段证据包（2026-11-24/25）

> 阶段 4 / P4-F：DI-26 修复、功能对齐清单、参数族覆盖（chip 框架 / SOUND / MOD·SCALE·VOICE /
> GLOBAL·FX / TOOLS / REC 配置 / 键盘参数锁）、导航 18 页九组、全页面截屏矩阵、`ENG POWER`。
> 分支 `feat/phase4f`（**不合并 main**，用户 2026-11-24 明示）。设备 Tab5 `/dev/ttyACM0`，
> ESP-IDF 5.5.5，target `esp32p4`；门禁口径 AGENTS §5.5。
> **状态**：Task 2–9 各自的门禁已通过并在此入库；**阶段收口的最终设备矩阵（十场景 + 182 例 +
> `selftest-heap.py` + 截屏矩阵 + 长时 soak）由 Task 10b 在同一固件上重跑，读数待 10b 追加**。
> 本文件与 `SHA256SUMS` 不入校验清单；`SHA256SUMS` 覆盖目录内其余全部文件（Task 10a 重算，215 项全 OK）。

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
| 10b（最终矩阵） | 待定 | 十场景 / 182 / `selftest-heap.py` / 截屏矩阵 / 3h soak | **待 10b** | ⏳ | — |

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
- **时点**：`store`/`synth`/`transport` 的最近一次全绿在 Task 5 的 `dcd516f` / Task 6 的
  `e3c262e`–`4d6825b`，未在 P4-F 的最终固件上重跑；`sequence`/`song` 同理。Task 10b 统一重跑。
- **池账目**：base 重测 ≈14.0KB 后，最坏有序对的纸面账略超 63,120B（实测 72/72 对仍
  ≥4,096B）——ADR-005 §5.5、DI-56。
- `step_edit` 的清空断言只查 `time != 0`（`ST_TIE = 1` 也被接受），是否可能留下 TIE 是开放问题。
- Task 6 首轮 `transport` 的一次「还原后 LIVEHASH ≠ 快照」未复现、根因未证明（同固件复跑 288/0）。
