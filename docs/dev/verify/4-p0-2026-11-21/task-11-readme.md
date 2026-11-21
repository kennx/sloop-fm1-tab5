# Task 11 — 五参数页（+FM6/SAMPLE）与 P4-A 收口

日期：2026-11-21 ｜ 阶段：P4-A（框架证明收口）｜ 受测固件：`758b81d-dirty`（工作区 =
本任务源码；`task-11-boot-log.txt` 第 6 行 `App version: 758b81d-dirty`）

> 本目录的任务 11 证据均在**同一固件**（clean 构建 → 烧录 → 启动后连续跑完）上采集；
> `task-11-*` 全部文件的 SHA256 见本目录 `SHA256SUMS`。
>
> **修复轮**（评审 Important 1–4 + minors）：受测固件 **`a00df79-dirty`**，证据为
> `task-11-fix-*`，见 §6；§1–§5 的矩阵是首轮（`758b81d-dirty`）证据。

## 1. 结论

- **P4-A 框架证明口径达成**：3 个引擎页（ANALOG/FM6/SAMPLE）+ GLOBAL + 家族页（FX）
  共 5 个参数页可编辑并生效；注册表 9 页全建，池 free 2,816B（非负）。
- **FM6**（`ENGINES[9]->edit[0..7]`）：ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH 8 槽；
  `ENG DESC engine 9 0` = `ALG F_INT 0..32 enums=33 first=PAT last=32`（33 名 1:1 映射）；
  TAP ±、键盘 ↑/↓、`UI KNOB` 相对拖拽三条编辑路径均改到引擎真值（`PARAM 63/64` 回读）。
- **SAMPLE**（`ENGINES[4]->edit[0..7]`）：SET/TUNE/BITS/LOOP/CUT/DRV 6 个真实槽；
  上游 `edit[5]/edit[7]` 是占位（label `-`、min=max=0），布局表用空槽不建旋钮
  （截屏探针：两处环顶像素 = 页面背景）；`ENG DESC engine 4 0` =
  `SET F_ENUM 0..11 enums=12 first=PIANO last=USR4`。
- **引擎页锁定路径**：选中轨引擎与页声明不符时页眉红字 N/A、旋钮 `--`、TAP ± 回
  `PARAM slot=n locked`、`UI KNOB` 回 **`ignored (locked)`**（与环带外
  `ignored (ring band)` 令牌分开；本轮 carry）。
- **格式化截断口径**（本轮 carry）：`knob_math` 的定点小数改为与上游 `libc.c` 的
  `fmt_fix` 相同的**截断**（TIME 104 → `1.88s`、CUTOFF 90 → `2.5kHz`、LFOHZ 105 →
  `12.5Hz`），三个值由 `knob-test.sh` 钉住。
- **池容量**：9 页 Σ est = 56,320B，`reserve` 1,560B；STEP est 从 24,576 下调到 24,064
  （delta + 10%），因为**其它八页建完、STEP 尚未建时** free 只有约 24.2KB
  （`task-11-fix-steplast.log` 实测 24,272B），旧 est 落在池历史 ±400B 抖动带上出现过
  一次误拒。**9 页是当前池的容量天花板**，登记 DI-17。
- 全门禁绿：build ×2 零警告、knob/state/host 测试、`screenshot.py --selftest`、
  `desc-check`、**全量 SELFTEST 182/182（UI live + quiesce，157.2s）**、soak 120s、
  五条注入脚本（tracks 11 / transport 28 / step_edit 75 / nav 40 / params **142** PASS，0 FAIL）。

## 2. 实现摘要

| 层 | 文件 | 内容 |
|---|---|---|
| 布局表 | `components/sloop_ui/src/page_layout.{c,h}` | 追加 FM6（engine 9，8 槽）与 SAMPLE（engine 4，6 槽 + 2 空槽）；`PARAM_PAGE_COUNT` 3→5；est 3,584/3,072 |
| 状态 | `components/sloop_state/include/sloop_state.h` | `APP_STATE_PAGES` 7→9、`app_page_t` 追加 `APP_PAGE_FM6/APP_PAGE_SAMPLE`（静态断言改 `APP_PAGE_SAMPLE + 1`） |
| 页 | `components/sloop_ui/src/page_params.c` | `page_params_test_knob` 对锁定引擎页返回 `ESP_ERR_NOT_ALLOWED`（新令牌） |
| 框架 | `components/sloop_ui/src/ui.c`、`theme.h`、`include/sloop_ui.h` | STEP est 24,576→24,064（含容量注）；9 页页签 114px 注释；`UI KNOB` 返回码文档 |
| 旋钮 | `components/sloop_ui/src/knob_math.{c,h}`、`test/knob_test.c` | 定点小数改上游 `fmt_fix` 截断（`format_fixed`），host 期望值与可辨例更新 |
| console | `firmware/main/console.c` | 页名表 9 项；`UI` 帮助/ERR 文本；`UI KNOB` 的 `ignored (locked)` |
| 注入 | `scripts/inject/inject.py`、`params.sh` | 9 页页签几何（114px/左缘探针 24,146,…,1000）；params 场景新增锁定路径 + FM6/SAMPLE 编辑 + 五页截屏/探针 + 九页池水位；`SELECT_TRACK`/`knob_locked` 辅助 |

### 2.1 槽位取舍（为什么这 8 槽）

- **FM6**：上游 `eng_fm6.c` 的 EDIT 1/2 正好 8 个宏（ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/
  PTCH），全收——编辑宏时算法与调制量通常一起看，不拆「OPS/PATCH」两页。
  ALG 与 PTCH 都是 `F_INT + 0 终止 names`（33 名 / 35 名），走 `name_for_int` 的
  值域均分（这里恰好 1:1），由 `ENG DESC` + 截屏字形复核。
- **SAMPLE**：6 个真实槽（SET/TUNE/BITS/LOOP/CUT/DRV）全收；`edit[5]/edit[7]` 上游
  定义为 `{"-", F_INT, 0, 0, 0, 0, 0}` 占位，建旋钮只会得到一个 0..0 的假控件，
  故用 `0xFF` 空槽（焦点跳过 / TAP 不命中 / 不占池）。
- **家族页**取 GLOBAL + FX 两页（任务 brief 的「GLOBAL + 一个家族页」口径由
  GLOBAL（全局/主延时）+ FX（轨道 FX + 输出条）共同满足；上游 MASTER 的宏是全局/
  轨道混排，其语义已被这两页覆盖）。理由写入 ADR-008 §8.1。

## 3. 池预算（`UI MEM`，九页全建，同一次输出）

| 页 | est | 实测 delta | 余量 |
|---|---|---|---|
| PLAY | 3,072 | 2,508 | +22% |
| TRACKS | 7,168 | 6,040 | +19% |
| STEP | 24,064 | 21,836 | +10% |
| TRANSPORT | 4,608 | 3,788 | +22% |
| GLOBAL | 3,584 | 2,856 | +25% |
| FX | 3,584 | 2,856 | +25% |
| ANALOG | 3,584 | 2,868 | +25% |
| **FM6** | 3,584 | **2,864** | +25% |
| **SAMPLE** | 3,072 | **2,432** | +26% |

`UI MEM pool_used=55064 pool_free=2816 pool_total=57880 registered=9 built=9 failed=0
sum_est=56320 reserve=1560`（`task-11-inject-params.log` 第 455–464 行）。

**STEP est 的调整依据**：其它八页建完、STEP 尚未建时 free 只有约 24.2KB；修复轮归档了
**STEP-last 实测 transcript**（`task-11-fix-steplast.log`）：`built=8`、STEP `unbuilt`
时 `pool_free=24,272`，`est=24,064` → 构建成功（池增量 21,864B，构建后 free 2,408B）。
若 est 仍是 24,576，同一状态 24,272 < 24,576 会被预算拒绝；调参前同顺序**实际出现过
一次** `页面构建失败 page=STEP(2) 原因=budget 池剩余=24248B 预算=24576B`（工具 stdout，
未留存原始 transcript——本轮归档的 24,272B transcript 是同一状态的实测复现，算术上
必然拒绝）。改为 24,064（delta 21,836B + 10.2%）后本场景与参数页注入两次复跑均通过，
实际构建余量 ~2.4KB。该项与「9 页容量天花板」一起写入 ADR-008 §3/§5 与 DI-17。

## 4. 验证矩阵（最终固件 `758b81d-dirty`）

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 ×2、零新增警告 | clean rc=0 / 增量 rc=0；`warning:` 0 / 0；clean 首遍仅 §5.5 豁免的 kconfig 提示对，增量遍 0 提示；镜像 `0x176b90`（82% 余量） | `task-11-build-clean.log` f3ed879d…、`task-11-build-incremental.log` f8cfef87… |
| 烧录 + 启动 | esptool 三段 `Hash of data verified`；`App version: 758b81d-dirty`；`UI 就绪: 9 页注册/1 已建`（池 15,556/62,820B）、`InputManager 55 条映射`、内部堆 115,483B；无 fatal/abort/backtrace/task_wdt | `task-11-flash.log` a82b3820…、`task-11-boot-log.txt` 7338a9f3… |
| 旋钮纯函数 host 单测 | `knob-test.sh` ×2 `5 passed, 0 failed`，两日志逐字节相同；含截断可辨例（TIME 104→1.88s、CUTOFF 90→2.5kHz、LFOHZ 105→12.5Hz） | `task-11-knob-test-1.log` 2cf13b59…、`task-11-knob-test-2.log` 2cf13b59… |
| 状态层单测 | `state-test.sh` ×2 `15 passed, 0 failed`，逐字节相同 | `task-11-state-test-green1.log` d22d9432…、`-green2.log` d22d9432… |
| host 回归 | `ALL HOST TESTS PASSED (subset)`；182 golden 0 changed/0 gone | `task-11-host-test.log` d5d54e1f… |
| 工具门禁 | `screenshot.py --selftest` 全过（解码/协议/重试/失败证据 12 组） | `task-11-screenshot-selftest.log` 5f8e2b45… |
| 描述符投影 | `desc-check.py` 10/10 正例 + 4/4 越界拒绝；含 FM6 ALG 33 名、SAMPLE SET 12 名 | `task-11-desc-check.log` a6c06356…、`task-11-desc-check-transcript.log` 4d84dcb0… |
| **全量 SELFTEST（UI live + quiesce）** | `cases=182 identical=182 mismatch=0 health_fail=0 checks=7 check_fail=0 deterministic=1 result=PASS`；运行 157.2s（96,708→253,868ms）；内部成对 quiesce on/off，`丢弃 0 条键盘事件`；已知 DI-12 诊断期一条 `task_wdt`（非卡死，Task 2 起既有） | `task-11-selftest-full-transcript.log` 9965e201…（205 行原始回显）、`task-11-selftest-full.log` aaca81ee… |
| soak 120s | `VERDICT: PASS`：3 采样，内部堆 70,299→70,363B、xrun=0、duty_mean 最大 11.11% ≤ 18.88%、KEY 计数增量符合、无 panic/WDT | `task-11-soak.log` b10df695…、`task-11-soak-summary.txt` fa7d3808…、`task-11-soak-run.txt` 227e34bf… |
| 注入脚本（五条） | 退出码均 0：tracks 11 / transport 28 / step_edit 75 / nav 40 / **params 142** PASS，0 FAIL | `task-11-inject-{tracks,transport,step_edit,nav,params}.log` + `-run.txt`，sha 见 `SHA256SUMS` |
| 失败可见性（9 页口径） | `UI FORCEFAIL fm6` → `UI FORCEFAIL page=fm6 (7) on state=failed` + ERROR + 错误状态条；`UI MEM` FM6 `state=failed`/delta=0（free 回收 2,752B）；`UI fm6` 重建成功 delta 2,852B ≤ est 3,584B；`PARAM 63` 值不变 | `task-11-forcefail.log` 38b2969d… |
| 上游完整性 | `sha256sum -c MANIFEST.sha256` 233/233 OK | `task-11-manifest.txt` 11007999… |
| 证据清单 | `SHA256SUMS`（本目录 task-11-* 全量，`sha256sum -c` 通过） | 本目录 `SHA256SUMS` |

### 4.1 新页截屏 + 像素抽检（`params` 场景）

| 图 | 抽检 |
|---|---|
| `task-11-inject-params-fm6-locked.png` | 页眉红字 1,005 px（≥200）；旋钮 `--`、TAP + 回 locked |
| `task-11-inject-params-fm6.png` | FM6 页签 accent；旋钮 0 环顶 `UI_PANEL_HI`；ALG 名称/数值文本（字形复原 `ALG` / `PAT`） |
| `task-11-inject-params-fm6-alg.png` | ALG=12 改值态：页签 accent、环顶、数值文本（字形复原 `12`，指示弧随值展开） |
| `task-11-inject-params-sample.png` | SAMPLE 页签 accent；环顶；空槽 5/7 环顶像素 = 页面背景 `UI_BG`（无旋钮）；字形复原 `SET` / `PIANO` |
| `task-11-inject-params-sample-set.png` | SET=1 改值态：页签 accent、环顶、数值文本（字形复原 `BASS`） |

其余三页（GLOBAL/FX/ANALOG）同场景复跑，抽检矩阵与 Task 10 相同（页签/环顶/± 按钮/
数值文本）。

### 4.2 FM6/SAMPLE 编辑闭环（`task-11-inject-params-run.txt`）

- **FM6**（`ENG TRACK 1 9 0` → 选轨 1 → `UI fm6`）：
  - `ENG DESC engine 9 0` → `ALG fmt=0 0..32 enums=33 first="PAT" last="32"`；
  - TAP + → `PARAM slot=0 id=63 delta=+1 value=1`，`PARAM 63`=1；TAP − 回基线 0；
  - 触摸：`UI KNOB 0 press (0,-73)` 不改值；`drag (73,0)`（+90° = +11，range 33）→ 12，
    `PARAM 63`=12；拖回锚点 → 0；
  - 键盘：→ 到槽 1（FB），↑ → `PARAM 64`=1，↓ → 0。
- **SAMPLE**（`ENG TRACK 1 4 0` → `UI sample`）：
  - `ENG DESC engine 4 0` → `SET fmt=8 0..11 enums=12 first="PIANO" last="USR4"`；
  - SET：TAP + → `PARAM 63`=1，TAP − → 0；
  - LOOP（槽 3）：+ → `PARAM 66`=1，− → 0；
  - CUT（槽 4）：+ → 1；触摸 `press`/`drag (73,0)` → 43（127 range：+90° ≈ 42.33 → 43），
    拖回 + TAP − → 0；
  - 改值态 SET=1（`BASS`）截屏后回基线。
- **收尾复位**：`PARAM 63/64/66/67 0`（共享 P_E* 槽位）→ `ENG TRACK 1 0 0` → 选回轨 0 →
  `UI play`；整个 params 场景可重复运行。

## 5. 未覆盖 / 限制

- **DSP 听感未断言**：FM6/SAMPLE 的宏值改动只证明写入引擎 `trk[].p[P_E0..]`（`PARAM`
  回读即状态层经 `sloop_core_read_params` 读到的引擎真值）；宏对声音的作用属 P4-D
  （FM6 patch 编辑面/SAMPLE 槽管理）与听感门禁。SAMPLE 的 SET/LOOP/CUT 在无采样槽
  内容时只改引擎参数，不改变「预设/kit」行为——本任务不声称可听变化。
- `ENG SNAP` 不含引擎 edit 槽位（只有 level/preset 等），所以引擎页编辑的断言入口是
  `PARAM <abs id>` 回读 + `STATE SNAP`（选中轨投影）；本任务已在 readme 与 ADR 中
  写明该口径。
- 物理触摸手感（DI-16）、人眼目检、听感仍为外部门测项（本会话无图像查看工具；
  截屏用像素统计 + 二值化字形复原核对，与 Task 10 同口径）。
- 9 页容量天花板（DI-17）：P4-B–F 加页前须先做容量设计决策。

## 6. 修复轮（评审 Important 1–4 + minors，2026-11-21）

受测固件 **`a00df79-dirty`**（dev HEAD；`task-11-fix-boot-log.txt` 第 6 行
`App version: a00df79-dirty`）。改动仅显示层（`knob_math.c` 定点小数符号）与注释/
布局表排版/文档口径；不动 DSP、不动页面结构。

### 6.1 修复项

| # | 问题 | 修复 | 证据 |
|---|---|---|---|
| I1 | `format_fixed` 丢负数且 |v|<一个显示单位时的符号（F_DB raw 111 显示 `0.5dB`，上游给 `-.5`） | `format_fixed` 在 `v<0 && ip==0` 时单独补 `-`（输出 `-0.5dB`，同值可读）；`knob_test` 新增 raw 111（`-0.5dB`）与 raw 110 边界（`-1.0dB`） | `task-11-fix-knob-test-red.log`（RED：`got "0.5dB" want "-0.5dB"`）、`-green{1,2}.log`（逐字节相同 5 passed） |
| I2 | ADR-008 §8.3 `CUT 90 → 2.6kHz` 与 §8.2/测试的 2.5kHz 矛盾 | 改为 `2.5kHz`（截断） | ADR-008 §8.3 |
| I3 | 「9 页全建后 free ≈24.2KB」与九页全建 2,816B 矛盾 | 三处改为「**其它八页建完、STEP 尚未建时** free ≈24.2KB」 | ADR-008 §3、`ui.c` 注释、本 readme §1/§3 |
| I4 | 容量拒绝无归档 transcript、params 场景并非 STEP-last | 新增 STEP-last 实测 transcript：`UI tracks`→`transport`→`global`→`fx`→`analog`→`fm6`→`sample` → `UI MEM`（built=8、STEP unbuilt、free 24,272）→ `UI step`（池增量 21,864B，预算 24,064）→ `UI MEM`（built=9、free 2,788） | `task-11-fix-steplast.log` 95dc8572…、`-run.txt` 7c564cfe… |
| m5 | `params.sh` 头注「三张 PNG」 | 改为五页 + 八张 PNG（global/fx/analog/fm6/fm6-alg/fm6-locked/sample/sample-set） | `scripts/inject/params.sh` |
| m6 | ADR §5 的 `pool_total 57,804–58,880` 无归档出处 | 改为 Task 11 归档 `UI MEM` 实测 **57,876–59,436 B** 并点名日志 | ADR-008 §5 |
| m7 | `ui.c` 写 delta 21,808B、ADR 写 21,836B | 统一为归档值 **21,836B（+10.2%）**（21,836/24,064 = +10.20%） | `ui.c`、ADR-008 §3 |
| m8 | `page_layout.c` SAMPLE 续行反斜杠未对齐（97 vs 95 列） | 逐槽一行、反斜杠对齐 95 列 | `page_layout.c` |
| m9 | DI-17「基础占用约 8–13KB」口径模糊 | 补同一次 `UI MEM` 算术 **7,016B = pool_used 55,064 − Σdelta 48,048** | DI-17、ADR-008 §5 |

补充：F_DB 是唯一带负值的定点格式（TIME/CUTOFF/LFOHZ 的生成表全为非负），故
次单位负数只有 raw 111 一个可辨值；raw 110（`-1.0dB`）钉住 `ip=-1` 边界。

### 6.2 验证（`a00df79-dirty`）

| 门禁 | 结果 | 证据 |
|---|---|---|
| knob-test RED→GREEN×2 | RED：`4 passed, 1 failed`（k=5 `DB 111 got "0.5dB"`）；GREEN×2：`5 passed, 0 failed`，两日志逐字节相同 | `task-11-fix-knob-test-red.log` fb607870…、`-green{1,2}.log` 2cf13b59… |
| build ×2 | clean rc=0 / 增量 rc=0；`warning:` 0 / 0；增量遍 0 kconfig 提示；镜像 `0x176bb0`（82% 余量） | `task-11-fix-build-clean.log` 5b98b3f1…、`-build-incremental.log` 8612bb57… |
| 烧录 + 启动 | 三段 `Hash of data verified`；`App version: a00df79-dirty`；`UI 就绪: 9 页注册/1 已建`（15,556/62,820）、内部堆 115,483B；无 fatal/abort/WDT | `task-11-fix-flash.log` 28ec88b6…、`-boot-log.txt` 43d2a8b8… |
| STEP-last 容量实测 | 见 §6.1 I4 | `task-11-fix-steplast.log` 95dc8572… |
| FX LEVEL=111 显示 smoke | `PARAM 0 111` → `applied=111`；`UI KNOB 5 press` value=111；截屏值文本字形复原为 `- 0 . 5 d B`（负号独立 glyph 在最左，随后 `0.5dB`） | `task-11-fix-smoke-fx-level111.png` 38ab103d…、`task-11-fix-smoke.log` ea3ca7ca…、`-smoke-run.txt` 8097b83c… |
| 参数页注入回归 | `params.sh` 退出码 0：**142 PASS / 0 FAIL**（五页编辑、九页池水位、锁定令牌、截屏抽检全过） | `task-11-fix-inject-params.log` df8a031e…、`-run.txt` 043895f7… |
| state / host / 工具 / 完整性 | state-test ×2 `15 passed` 逐字节相同；host-test `ALL HOST TESTS PASSED (subset)`；`screenshot.py --selftest` 全过；manifest 233/233 | `task-11-fix-state-test-green{1,2}.log` d22d9432…、`-host-test.log` 6b965530…、`-screenshot-selftest.log` bf7448d8…、`-manifest.txt` 11007999… |

### 6.3 未覆盖

- 修复只改**显示文本**：值路径（`PARAM` 回读）与引擎真值不变，故未重跑全量 SELFTEST
  与 soak（首轮 182/182 + 120s 的证据仍对同一代码路径有效；本次改动不触及引擎/音频）。
- `-0.5dB` 的设备侧判读用像素字形复原完成（本会话无图像查看工具）；人眼目检仍为外部
  门禁。
