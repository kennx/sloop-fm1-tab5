# P4-F 阶段 4 收口 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把阶段 4 收到 roadmap §6 的完成判据：修掉自检堆泄漏（DI-26）、落成功能对齐清单、补齐参数族覆盖（12 引擎 + 每轨/全局/FX 族 + TOOLS）、全页面截屏矩阵、3h soak（含 INA226/温度），证据入库；**不合并 main**（用户明示）。

**Architecture:** 新增六张「族页」——SOUND（通用引擎页，取代 ANALOG/FM6）与五张 chip 页（MOD/SCALE/VOICE/FX/GLOBAL），全部由 `page_layout.c` 的 X 宏表驱动（每页 ≤4 chip、每 chip ≤8 槽，数据只来自 `sloop_core_desc`）；新增 MIX 组承载 GLOBAL/FX/TOOLS（9 组 / 18 页 / 组签 104px）。TOOLS 三动作走状态层命令 + 上游语义适配。DI-26 用二分优先的堆定位法。

**Tech Stack:** ESP-IDF 5.5.5 / FreeRTOS / LVGL / INA226(I2C 0x41) / `temperature_sensor` / pyserial + Pillow。

**Spec:** `docs/superpowers/specs/2026-11-24-p4f-phase4-closeout-design.md`

## Global Constraints

- `third_party/sloop-fm1/**` 只读（MANIFEST 233/233）；适配只在 `*_tab5.c`；标签镜像须注释指向上游行号并由 host 测试钉住（A9）。
- 不引入新的**范围**表：所有槽的 label/min/max/fmt/enums 来自 `sloop_core_desc`（ENGINE scope 经 `e->desc` 钩子）；INSERT/ENV SYNC 只允许标签/名/格式的小镜像。
- LVGL 池 64KB 不扩；每页构建前 `free ≥ est`；est 上限：SOUND 6,144、MOD/SCALE/VOICE 5,120、FX/GLOBAL/TOOLS 7,168；有序对不变量 `base + (源 Σ + 目标首建) + 4,096 ≤ 63,120`（最坏 SEQ→PRESETS 60,776 不得被超过；FM6P 44B tripwire 不动）。
- 导航终态：组顺序 `PLAY/TRACKS/EDIT/MIX/SYNTH/SEQ/SONG/FILE/SYS`；EDIT = SOUND/MOD/SCALE/VOICE；MIX = GLOBAL/FX/TOOLS；`UI_GROUP_W` 122→104（9×104+8×8=1,000，右端 1,016 ≤ 1,064）；`APP_PAGE_*` 只追加（既有 id 不变）；TAB 轮转 18 页；`UI fm6`/`UI analog` 保留为 SOUND 的 console 别名。
- 产品写路径一律经 `state_apply`；console 是诊断执行器；flash 擦写不持引擎锁；走带中拒绝擦写类动作。
- `ENG SELFTEST` console 语义不变；DI-26 不得用掩盖性修法（自检后重建堆、放宽断言）。
- 截屏/听感/触摸/LED：机器只做像素抽检与 PNG 解码校验，**人眼/人耳是外部门禁**，文档不得声称已目检。
- 提交格式 AGENTS.md §7.2（`<type>(<模块>): 中文摘要` + 四标签；fix 加 `[Root_Cause]/[Blast_Radius]`）；按逻辑单元拆分；每任务 review clean 后 push。
- **不合并 main**（用户 2026-11-24 明示）：Task 10 止于 `feat/phase4f` → `dev` 与收口报告；roadmap 的 dev→main 里程碑在报告里标为待办。

## Review Focus

1. **鼓轨上的族页**：选中轨为鼓轨时，SOUND 锁定，但 MOD/SCALE/VOICE/FX 各 chip 必须按上游 `page_for_drum` 逐 chip 决定可编辑/锁定，写被拒时不得改引擎 → Task 4/7 用例。
2. **非 FM6 引擎时 SOUND 底行缺席**：bank 行坐标 TAP ⇒ miss；鼓轨 SAVE 的状态层拒绝不变 → Task 5/7。
3. **TOOLS 误触**：CLEAR SEQ / INIT SOUND / NEW PROJECT 必须二次确认（上游 arm 后 ~1.5s 内再按），走带中/冻结时拒绝且状态可见 → Task 6/7。
4. **连续两次全量自检后的堆**：第二次前后 `internal_free` 差 ≤1,024B 且 ≥60KB；若走兜底，`selftest-heap.py` 的记录模式不得把失败判成通过 → Task 2。
5. **chip 页切 chip 时的脏检查**：切 chip 后 8 个旋钮的 label/值/锁定态全部刷新，旧 chip 的值不得残留；每 30ms 拍零分配 → Task 4/7。

---

### Task 0: 基线与分支

**Files:** 无代码。

- [ ] `git checkout dev && git pull --ff-only origin dev && git checkout -b feat/phase4f`
- [ ] `scripts/build.sh && scripts/build.sh && scripts/seq-test.sh && scripts/state-test.sh && scripts/knob-test.sh && scripts/synth-test.sh` → 全 0，第二遍 0 警告（日志 `/tmp/p4f_baseline_*.log`）。

### Task 1: 功能对齐清单（审计先行）

**Files:**
- Create: `docs/design/012-feature-parity-checklist.md`
- Modify: spec §4（若审计与上游事实冲突）、`docs/deferred-issues.md`（新增缺口 DI）、`AGENTS.md` §3（design 列表加 012）

**Interfaces:**
- Produces: 清单表（列：`上游特性 | 入口（页/层键/菜单）| Tab5 落点 | 状态 ✅/◐/○ | 证据 | 处置`）与「缺口处置表」（P4-F 任务号 / DI 编号 / 非目标），以及给 Task 3–6 的**逐 chip 槽位权威表**（每 chip 的参数 id 序列、鼓轨可否编辑）。

- [ ] Step 1: 只读抓取上游手册到 `/tmp/upstream-docs/`（`SLOOP.md`、`GUIDE.md` @ bac9c05，raw GitHub；不入库、不改 MANIFEST）。
- [ ] Step 2: 从 `params.c PAGES[]`（~40 页）、`ui.c` 屏、`ui_input.c` 层键、`ui_menu.c`、`ui_studio.c`/`ui_song.c`/`ui_vis.c`/`ui_input.c:568-612`（TOOLS）枚举上游特性行；逐行在仓库里找 Tab5 入口与证据（注入场景/host 测试/console）。
- [ ] Step 3: 写清单与处置表；按 spec A8 落位 PATTERN→VOICE、鼓轨 `page_for_drum` 结论；VIS/punch-in/LIVE/LEARN/MIDI 设置页等登记 DI（必填五字段）。
- [ ] Step 4: 核对 spec §3/§4：若族页槽位与上游冲突，修订 spec 并记录裁决。
- [ ] Step 5: 提交 `docs(design): 落成阶段 4 功能对齐清单与缺口处置`（清单 + DI + spec 修订）。

### Task 2: DI-26 堆泄漏定位修复 + `selftest-heap.py` + worker 栈收缩（DI-43(1)）

**Files:**
- Modify: 视根因而定（候选：`firmware/components/sloop_core/src/selftest.c`、`store_tab5.c`/`settings_tab5.c`/`sound_tab5.c` 中被快照窗口回滚的堆相关指针、`firmware/main/sys_selftest.c`（`SYS_SELFTEST_STACK`）、`firmware/main/main.c`）
- Create: `scripts/selftest-heap.py`
- Modify: `docs/deferred-issues.md`（DI-26 删除或改写为定位结论；DI-43(1) 收口）、`AGENTS.md` §5.1/§5.5

**Interfaces:**
- Produces: `python3 scripts/selftest-heap.py PORT [--record-only]`：硬复位 → `ENG MEM` → 全量自检 → `ENG MEM` → 全量自检 → `ENG MEM`；默认断言第二次前后 `internal_free` 差 ≤1,024B 且 ≥61,440B；`--record-only` 只断言两次自检 `result=PASS` 并打印差值（兜底模式，退出码不因差值非 0）。worker 收尾日志 `sys_selftest: stack hwm=<bytes>`。

- [ ] Step 1: 写 `selftest-heap.py` 并在现固件上跑出 RED（差值 ≈40KB）。
- [ ] Step 2: 二分定位（spec A3）：单用例 ×20 的堆差 → 收敛到 job 名单；`heap_caps_get_info` 按 cap（INTERNAL/DMA/SPIRAM）分类；对照 `selftest_restore()` 前后可疑全局；必要时 `esp_heap_trace` 诊断构建（不入库）。
- [ ] Step 3: 最小修复；若两轮内证明在 IDF/newlib 内部且无干净修法 → 按兜底：记录结论、`--record-only` 进矩阵、DI-26 保持开放（裁决写入 ledger）。
- [ ] Step 4: worker 栈：收尾打印 `uxTaskGetStackHighWaterMark(NULL)`，按实测下调 `SYS_SELFTEST_STACK`（取实测 +1KB，512 对齐）；确认栈仍在 L2MEM（启动日志 `stack=`）。
- [ ] Step 5: 验证：`scripts/build.sh && scripts/build.sh`（0 警告）→ `scripts/flash.sh` → `selftest-heap.py` PASS → `python3 scripts/device-selftest.py` 182/182 → `bash scripts/inject/sys.sh`（chip 仍 182/182、perf 健康）→ `ENG MEM` 开机读数记录（预期比 69KB 多 ~10KB）。
- [ ] Step 6: 提交 `fix(components/sloop_core): <根因一句话>`（+ `[Root_Cause]/[Blast_Radius]`）、`feat(scripts): 增加连续自检堆泄漏门禁脚本`、`docs(deferred-issues): …`。

### Task 3: 描述符钩子与标签镜像

**Files:**
- Modify: `firmware/components/sloop_core/src/*`（`sloop_core_desc` 实现处：ENGINE scope 追加 `ENGINES[e]->desc(t, id)` 调用）、`include/sloop_core.h`
- Create: `firmware/components/sloop_core/src/labels_tab5.h`（INSERT A/B/C 与 ENV SYNC 音名的标签镜像，`static inline`，注释引用 `params.c` 行号）
- Test: `scripts/synth-test.sh` 或 `scripts/state-test.sh` 所在 host 套件（取最贴近的一个）+ `scripts/desc-check.py` 设备抽样

**Interfaces:**
- Produces: `sloop_core_desc(SLOOP_DESC_ENGINE, engine, id, out)` 的 label/unit/fmt/enums 跟随引擎模式钩子（范围不变）；`bool sloop_core_desc_track_live(uint8_t track, uint32_t id, sloop_desc_t *out)`：在 TRACK scope 之上叠加状态相关标签（INSERT 按 `P_ITYPE`、ATK/DEC/REL 在 ENV SYNC 时显示音名），范围不变；`const char *const *` 镜像表与上游逐项相等。

- [ ] Step 1: host 失败测试：LOFI/NOISE/PHYS 各模式的 ENGINE 标签 = 钩子输出；INSERT 在 ITYPE 的两个取值下 A/B/C 标签不同且范围相同；ESYNC 开/关 ATK 标签不同；镜像表逐项 == 上游 `ins_desc`/`ESYNC_DESC` 字面量（硬编码期望）。
- [ ] Step 2: 实现并跑到 GREEN；`scripts/desc-check.py` 增 2 项抽样（INSERT/ESYNC）。
- [ ] Step 3: 提交 `feat(components/sloop_core): 描述符跟随引擎钩子并镜像 INSERT/ENV SYNC 标签`。

### Task 4: chip 页框架 + MOD / SCALE / VOICE

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_layout.{h,c}`（X 宏：每页 ≤4 chip，每 chip ≤8 槽；新增三页的槽表，权威来源 = Task 1 的逐 chip 槽位表）、`page_params.{h,c}`（chip 行、`UI <page> CHIP <n>`、鼓轨逐 chip 锁定、切 chip 零分配刷新）、`theme.h`、`include/sloop_ui.h`、`ui.c`（注册）、`firmware/components/sloop_state/include/sloop_state.h`（追加 `APP_PAGE_MOD/SCALE/VOICE`）、`firmware/main/console.c`（页名、`UI … CHIP`）
- Test: `scripts/knob-test.sh`（纯函数：chip 表完整性——槽数 ≤8、id 合法、chip 名唯一）、`scripts/inject/params.sh`（Task 7 统一扩展，本任务先跑现有 `params`/`nav` 回归）

**Interfaces:**
- Produces: `param_page_t` 增 `chips[4]`（`{name, slots[8], slot_count}`）；`void sloop_ui_params_chip_set(uint8_t page, uint8_t chip)` / `uint8_t sloop_ui_params_chip_get(uint8_t page)`（页内局部态，30ms 拍重配）；`bool sloop_ui_params_slot_locked(uint8_t page, uint8_t chip, uint8_t slot)`（鼓轨规则）；TAP 回显 `-> PARAMS <page> chip=<n> select` 与既有 `PARAM slot=…` 并存；页 est：MOD/SCALE/VOICE 5,120。

- [ ] Step 1: knob host 失败测试（表完整性 + est 常量 + 组表顺序）。
- [ ] Step 2: 实现框架与三页；构建 ×2 0 警告；烧录后 `UI MEM` 三页 `delta ≤ 5,120`；切 chip 后 8 槽 label 与 `ENG DESC` 一致；鼓轨上逐 chip 锁定与 Task 1 结论一致；10 轮切 chip `pool_free` 首末差 ≤128B。
- [ ] Step 3: 提交 `feat(components/sloop_ui): 参数页 chip 框架与 MOD/SCALE/VOICE 族页`（按 sloop_state/console 拆分）。

### Task 5: SOUND 页（取代 ANALOG+FM6）+ GLOBAL/FX chip 页 + 导航重组

**Files:**
- Modify: `page_layout.c`（SOUND = 选中轨引擎 edit[0..7]；GLOBAL chips GLO/REV/DRUMS/TEXTURE；FX chips FX/SLICER/INSERT/INSERT 2）、`page_params.c`（SOUND 标题显示引擎名、FM6 底行条件化、SAMPLE 不重复槽行）、`ui.c`（删 FM6 页注册、`k_groups` 重组、`UI fm6`/`UI analog` 别名）、`theme.h`（`UI_GROUP_W 104`、断言）、`sloop_state.h`（追加 `APP_PAGE_SOUND`；`APP_PAGE_ANALOG/FM6` 保留为别名常量）、`console.c`、`scripts/inject/inject.py`（`PAGES`、`UI fm6`/`UI analog` 用例改指向 SOUND）、`scripts/soak.py`
- Test: `scripts/knob-test.sh`、`scripts/inject/{nav,params,store,synth}.sh`

**Interfaces:**
- Consumes: Task 3 `sloop_core_desc_track_live`、Task 4 chip 框架。
- Produces: 9 组 / 18 页；`UI MEM GROUP MIX`；SOUND 底行规则（A10）；`UI fm6` → `UI page=sound (id)`；`store` ⓰ / `params` 的「NOT FM6」锁定用例改为「底行缺席 ⇒ TAP miss」。

- [ ] Step 1: 更新 `nav` 期望（18 页轮转、9 组签、`UI TAB mix`/`UI SUB 0..2`）与 `PAGES` 表；跑 `nav` 得 RED。
- [ ] Step 2: 实现；构建 ×2；烧录；`UI MEM`：SOUND ≤6,144、FX/GLOBAL ≤7,168；12 引擎轮换（`ENG TRACK 0 <e> 0`）后 SOUND 8 槽 label 与 `ENG DESC` 一致、鼓轨锁定；FM6 轨底行 SAVE/ERASE 往返、非 FM6 轨底行 TAP miss；有序对 EDIT→STEP / MIX→STEP / SEQ→MIX `池剩余 ≥4,096`。
- [ ] Step 3: 回归 `nav`/`params`/`store`/`synth` 全绿。
- [ ] Step 4: 提交 `feat(components/sloop_ui): SOUND 通用引擎页、GLOBAL/FX chip 页与九组导航`。

### Task 6: TOOLS 页与三动作

**Files:**
- Create: `firmware/components/sloop_ui/src/page_tools.c/.h`
- Modify: `firmware/components/sloop_core/src/seq_tab5.c` 或 `store_tab5.c`（上游 `G_CLRSEQ/G_INITSND/G_NEWPRJ` 语义适配：`sloop_core_tools_*`，持引擎锁）、`include/sloop_core.h`、`sloop_state.{h,c}`（`CMD_TOOLS_CLRSEQ/INITSOUND/NEWPRJ` + `tools_status`）、`ui.c`（注册）、`console.c`（`TOOLS <act>` 诊断 + TAP 回显）、`page_layout.c`（TOOLS 条目）

**Interfaces:**
- Produces: `esp_err_t sloop_core_tools(uint32_t action)`（action 0 CLRSEQ / 1 INITSOUND / 2 NEWPRJ；走带中 `ESP_ERR_NOT_ALLOWED`；冻结 `ESP_ERR_INVALID_STATE`）；`CMD_TOOLS{a=action}`；页面：三个 chip，第一次按 = 预备（chip 高亮 + 状态行 `AGAIN TO CONFIRM`，1.5s 内再按才执行，超时取消）；TAP 回显 `-> TOOLS <CLRSEQ|INITSOUND|NEWPRJ> armed|value=<a> status="…"`。

- [ ] Step 1: host 失败测试（`seq-test`/`state-test`）：三动作效果（步表清空 / 声音回默认 / 工程回默认的逐项断言，以 Task 1 审计出的上游语义为准）、走带中/冻结拒绝、state 映射。
- [ ] Step 2: 实现；`UI MEM` TOOLS ≤7,168；`params` 场景：未确认 ⇒ 状态不变；确认 ⇒ 生效；走带中 ⇒ `STOP FIRST`。
- [ ] Step 3: 提交（core / state / ui / console 拆分）。

### Task 7: `params.sh` 全扩展 + `nav` 定稿

**Files:** Modify: `scripts/inject/inject.py`（`run_params`：SOUND 12 引擎轮换、六页各 chip 字段逐一对照 `ENG DESC`、写后回读、鼓轨逐 chip 规则、TOOLS 确认态、chip 切换池往返；`run_nav`：18 页/9 组/104px 组签像素抽检、新有序对峰值）、`scripts/inject/params.sh`、`scripts/soak.py`（页表 + 组签表 + `POOL_RT_GROUPS`）

- [ ] Step 1: 按本任务 Interfaces 写断言（先 RED 若功能缺失）。
- [ ] Step 2: `bash scripts/inject/params.sh && bash scripts/inject/nav.sh` 全绿；`python3 scripts/soak-selftest.py`。
- [ ] Step 3: 提交 `test(scripts/inject): 参数族页与九组导航的注入矩阵`。

### Task 8: 全页面截屏矩阵 + 联系表

**Files:**
- Create: `scripts/screenshot_matrix.py`（复用 `inject.py` 的 `Session` 与 `screenshot.py` 的读取/重试；矩阵定义 `MATRIX`：页 × 状态 → 归一/置态命令序列；产物 `firmware/build/verify/matrix-<ts>/{<page>-<state>.png, matrix.csv, sheet-<group>.png}`）

**Interfaces:**
- Produces: 退出码 0 = 全部 PNG 解码为 1280×720 RGB、`matrix.csv` 每行含页/状态/置态命令/3 个锚点像素读数、每组一张联系表（≤4×5 格，Pillow，标注页名与状态）。

- [ ] Step 1: 写矩阵定义（18 页 × 默认；SOUND/MOD/SCALE/VOICE/FX/GLOBAL × 编辑中；STEP/TRANSPORT × 播放中；SOUND × 鼓轨锁定；TOOLS × 预备态；SYSTEM × SELFTEST PASS 结果行）。
- [ ] Step 2: 跑通；像素抽检锚点（页签选中色/状态行颜色）与预期一致；`--selftest` 离线模式。
- [ ] Step 3: 提交 `feat(scripts): 全页面截屏矩阵与联系表生成`。

### Task 9: `ENG POWER`（INA226 + 芯片温度）与 soak 列

**Files:**
- Create: `firmware/main/power_sensor.c/.h`（INA226 @0x41：总线电压寄存器 0x02；分流电压 0x01；电流仅在查到分流电阻值时换算，否则输出 `shunt_uv` 并在帮助里注明）
- Modify: `firmware/main/console.c`（`ENG POWER` → `vbus_mv=.. shunt_uv=.. [cur_ma=..] temp_c=..`）、`scripts/soak.py`（采样列 + 摘要的 min/max）、`scripts/soak-selftest.py`

- [ ] Step 1: 查分流电阻值（原理图/手册；查不到则按降级口径）；实现并读回合理值（USB 供电 ~5V；温度 35–60℃ 量级）。
- [ ] Step 2: soak.py 增列；`soak-selftest.py` 通过；短跑 `soak.py --duration 300 --interval 60` 验证列。
- [ ] Step 3: 提交 `feat(firmware/main): ENG POWER 读数与 soak 的功耗温度采样`。

### Task 10: 文档、证据、全矩阵、3h soak、收口报告（**不合并 main**）

**Files:**
- Modify: `AGENTS.md`（§3/§5.1/§5.4.1/§5.5）、`docs/design/005-ui-architecture.md`（§1 组表 9 组、§2 组签 104px、§5.3 P4-F 预算与实测）、`docs/design/008-param-pages.md`（chip 页框架、SOUND）、`docs/design/012-feature-parity-checklist.md`（终态打勾）、`docs/design/002-port-roadmap.md`（P4-F 状态）、parity spec P4-F 行、`docs/dev/verification-log.md`
- Create: `docs/dev/verify/4-pf-<日期>/`（readme + SHA256SUMS）、`docs/dev/closeout-phase4.md`（收口报告）

**Interfaces:**
- Produces: 收口报告（给用户）：门禁→结论、清单状态（✅/DI 开放项）、soak 数值（含功率/温度）、**人测待办清单**（联系表路径、DI-4/16/7/39、LED 颜色、亮度/音量）、**dev→main 标为待办**（不在本阶段）与开放 DI 列表。

- [ ] Step 1: host 全矩阵 + MANIFEST；烧录 ×3；182；十一条注入场景（含复跑）；`selftest-heap.py`。
- [ ] Step 2: `screenshot_matrix.py`；联系表入库。
- [ ] Step 3: 硬复位 → 后台 `soak.py --duration 10800 --interval 60 --save-window 900`（断连判失败重试一次），期间完成文档。
- [ ] Step 4: 证据晋升 + SHA256SUMS + verification-log；提交 `docs(design)` / `docs(dev/verify)`。
- [ ] Step 5: 整分支终审 → 修复波 → 合并 `feat/phase4f` → `dev` 并重跑合并后门禁 → push dev。
- [ ] Step 6: **停**：输出收口报告（含人测待办清单与开放 DI）；不合并 main。

---

## 自审记录

- Spec 覆盖：目标 1→Task 2；2→Task 1/10；3→Task 3/4/5/6/7；4→Task 8/10；5→Task 9/10；6→Task 2；7→Task 10 Step 6。A8/A9/A10→Task 1/3/5。
- 类型一致：`sloop_core_desc_track_live`、`sloop_ui_params_chip_*`、`sloop_core_tools`、`CMD_TOOLS` 在 Task 3–7 命名一致。
- 比例：计划 ≈ spec 的 0.9 倍。
