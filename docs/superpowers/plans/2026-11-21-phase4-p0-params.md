# 阶段 4 启动：P4-0 前置清账 + P4-A 参数页框架 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 清掉阶段 3 欠账（DI-9 可听 mute/solo、DI-11 全量 SELFTEST 与 UI 共存、LVGL 懒建/预算、步窗抽象、DI-5 硬化、nav.sh），并落地描述符驱动的参数页框架（3 引擎页 + GLOBAL + 家族页可编辑生效）。

**Architecture:** 引擎补 `sloop_core_desc`（投影 params.c 描述符）与 `sloop_core_set_params`（批量）；状态层补 `CMD_SET_PARAM/BATCH` 与选中轨参数投影；`sloop_ui` 增加页面注册表 + 懒建预算 + 旋钮页生成器，布局表由 UI 持有（引擎不暴露 page 表）。

**Tech Stack:** ESP-IDF v5.5.5（不升级——DI-11 在 5.5.5 内 firmware 侧解决）/ LVGL 9.6 / sloop_core·sloop_state·sloop_ui / Python 注入脚本。

**Spec:** `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（含 builder-check 裁决）+ ADR-005/006/007。执行前必读。

## Global Constraints

- 构建/烧录走 scripts/；build×2 第二遍零新增警告；烧录 Hash verified、启动无 fatal。
- third_party 只读（manifest 233/233）；接入上游 TU 仅经 CMake/组合 TU，禁改上游文件。
- 设备等价门禁：P4-0 Task 2 完成后，全量 `ENG SELFTEST`（quiesce 流程）为准；在此之前单例 + 阶段 2 transcript 双轨（AGENTS §5.5 同步）。
- 内存门禁：idle 内部堆 ≥60KB；LVGL 池水位每页进入前后记录；PSRAM 用于大表。
- 锁次序不变：LVGL→state→engine；input 任务不触 LVGL（邮箱/请求通道）；音频任务优先级 5 不被阻塞。
- 每任务 review clean 后 push origin `feat/phase4`；P4-0+P4-A 收尾合 dev（阶段 4 内继续用 feat 分支推进后续子里程碑）。
- 人测项（听感/手感/视觉）入收尾清单，不作为本计划退出判据。

## Review Focus（测试已钉进对应任务）

1. **mute/solo 写入与快照一致性**（state-owned 残留导致 UI 显示与声音不符）→ Task 1 单测：写后 `sloop_core_read` 反映；移除 sync 例外后再断言。
2. **quiesce 流程的挂起/恢复竞态**（input 任务挂起时在途事件、UI 定时器暂停期间状态陈旧）→ Task 2 注入序列：运行中按键/切页后恢复，状态一致、无丢音。
3. **批量设置的生命周期**（`CMD_SET_PARAM_BATCH` 指针在跨任务使用时的悬垂）→ Task 9 单测：apply 同步完成、复制语义明确、越界 ids 忽略。
4. **描述符投影正确性**（F_ENUM 枚举名/单位丢失导致页面显示错值）→ Task 8 逐项对照 params.c 抽样 10 个参数（含 enum/db/time/bipct）。
5. **懒建失败的可见性**（池耗尽时静默 NULL → 空页）→ Task 10/11 强制失败注入（如临时把池压到极小）确认错误路径有明确提示；池水位入档。

---

## 文件结构

```
firmware/components/sloop_core/      # Task 1/2/8：set_track_sel/mute/solo、desc、set_params、quiesce 适配
firmware/components/sloop_state/     # Task 1/5/9：Cmd 扩展、step window、param 投影
firmware/components/sloop_ui/        # Task 3/4/10/11：懒建注册表、旋钮页、布局表、nav 用例钩子
firmware/main/{console.c,main.c}     # 诊断命令与 boot 编排
scripts/inject/nav.sh                # Task 6（新）
scripts/{device-selftest.py,soak.py,lib} # Task 2/3：quiesce 调用、SCR 重试
docs/design/008-param-pages.md       # Task 10（ADR：框架/预算/布局）
docs/dev/verify/4-*                  # 证据
```

---

### Task 0: 基线

**Files:** 无
- [ ] `git checkout dev && git checkout -b feat/phase4`；`scripts/build.sh` 绿；`scripts/host-test.sh`/`state-test.sh` 绿；记录基线行。

### Task 1: DI-9 — mute/solo/selected 引擎写路径（可听）

**Files:**
- Modify: `firmware/components/sloop_core/{include/sloop_core.h,src/selftest.c}`
- Modify: `firmware/components/sloop_state/{src/cmd.c,src/state.c,test/state_test.c}`
- Modify: `firmware/main/console.c`（`STATE SNAP` 说明）

**Interfaces:**
- Produces: `void sloop_core_set_mute(uint32_t k, bool mute);`、`void sloop_core_set_solo(uint8_t mask);`、`void sloop_core_select(uint32_t k);`（写 `trk[k].p[P_MUTE]`、`song.solo`、`song.sel`，均经 `s_engine_lock`）。
- Consumes: 现有 `state_apply`、`sloop_core_read`。

- [ ] **Step 1 (RED):** `state_test.c` 加断言：`CMD_MUTE/CMD_SOLO/CMD_SELECT_TRACK` 后 stub 记录引擎调用（mute/solo/sel 写函数被调用）；`state_sync_from_engine` 不再用状态值覆盖引擎值（移除例外后 stub 快照反映引擎真值）。
- [ ] **Step 2:** 跑 `state-test.sh` → RED（stub 无写函数/断言失败）。
- [ ] **Step 3:** 实现引擎三写函数 + `cmd.c` 分发改造 + 删除 `state.c` 的 mute/solo/sel 例外（改为引擎真值同步）；`sloop_state.h` 契约更新；DI-9 条目删除（docs 同组提交）。
- [ ] **Step 4:** GREEN；设备：`KEY <Alt down>` → `KEY 1` → `STATE SNAP` 与 `ENG SNAP` 的 mute 一致；`ENG SNAP` 显示 solo/selected；build×2/flash/boot clean。
- [ ] **Step 5:** Commit `feat(components/sloop_core): mute/solo/selected 引擎写路径（DI-9）`。

### Task 2: DI-11 — 全量 SELFTEST 与 UI 共存（quiesce 流程）

**Files:**
- Modify: `firmware/components/sloop_core/src/selftest.c`、`firmware/components/sloop_ui/{include/sloop_ui.h,src/ui.c,src/input.c}`、`firmware/main/console.c`
- Modify: `scripts/device-selftest.py`、`AGENTS.md`（§5.5 口径）
- Test: 设备全量 182 运行 + 恢复现场注入

**Interfaces:**
- Produces: `void sloop_ui_quiesce(bool on);`（暂停/恢复 UI 定时器与 input 任务处理——input 任务挂起/恢复需安全：用通知/挂起原语，不在持锁时挂起）；selftest 内部调用 quiesce+freeze，结束后恢复。
- Consumes: 现有 `sloop_core_diag_freeze`。

- [ ] **Step 1:** 实现 `sloop_ui_quiesce`：暂停 UI 定时器（`lv_timer_pause`）、input 任务置暂停标志（事件丢弃并计数）、TAP 拒绝并提示；恢复后清零计数。
- [ ] **Step 2:** `sloop_core_selftest` 全量路径：quiesce(on) → freeze → run 182 → freeze off → quiesce(off)；busy 语义改为「诊断进行中」。
- [ ] **Step 3:** 验证：boot（UI live）→ `ENG SELFTEST`（无参）→ 182/182 PASS（原始 transcript 入库）；运行中（后台注入按键/切页）→ 结束后 UI 恢复、无丢音/挂死；`mismatch=0`；之后 `ENG TRIG` 正常。
- [ ] **Step 4:** `scripts/device-selftest.py` 默认走全量（无需 AUDIO OFF 手工步骤：脚本先 `ENG AUDIO OFF`，selftest 内部 quiesce）；AGENTS §5.5 更新；DI-11 删除。
- [ ] **Step 5:** Commit `fix(components/sloop_core): 诊断期 quiesce 使全量 SELFTEST 与 UI 共存（DI-11）`。

### Task 3: DI-5 — SCR 重试硬化（P4-F 无人值守前置）

**Files:**
- Modify: `scripts/screenshot.py`（重试参数与 RTS 复位）、`scripts/inject/inject.py`（复用）、`docs/deferred-issues.md`（DI-5 更新/删除条件）
- Test: 故障注入（人为打断一次传输：拔插/复位诱发或 mock）——无法稳定复现时以「重试路径单测 + 一次真机长批量回归」代替。

- [ ] **Step 1:** `screenshot.py` 增加 `--retry N`（默认 3）：传输失败/尾标记缺失时，经 esptool `--after hard_reset` 复位设备后重试；超限非零退出并保留 `-fail` 证据。
- [ ] **Step 2:** `--selftest` 增加重试逻辑的离线用例（fake serial 首轮坏、次轮好 → 成功）。
- [ ] **Step 3:** 真机：连续 20 次截屏批量（含 2 次强制中断模拟）稳定通过；`scripts/inject/*` 自动受益。
- [ ] **Step 4:** Commit `fix(scripts): SCR 传输失败自动复位重试（DI-5 硬化）`。

### Task 4: LVGL 懒建/预算框架

**Files:**
- Modify: `firmware/components/sloop_ui/src/ui.c`、`include/sloop_ui.h`
- Create: `docs/design/008-param-pages.md`（ADR：懒建策略、每页预算表、失败可见性）

**Interfaces:**
- Produces: `typedef struct { uint8_t id; const char *title; lv_obj_t *(*build)(lv_obj_t *parent); void (*update)(const app_state_t*); uint32_t est_bytes; } ui_page_t;` 页面注册表 API：`ui_page_register(const ui_page_t*)`；首次进入时 `build()`（记录池水位 delta 与构建失败日志），离开不销毁（受预算保护）；`sloop_ui_page_pool_stats()` 供日志/命令。
- Consumes: 现有四页改造为注册表条目（PLAY 等迁入，回归像素不变）。

- [ ] **Step 1:** 注册表 + 懒建 + 失败路径（`lv_obj` NULL → 状态条错误文本 + ERROR 日志 + `UI ERR page_build`）实现；四页迁移。
- [ ] **Step 2:** 每页进入前/后 `lv_mem_monitor` 记录（命令 `UI MEM`）；ADR-008 写入实测与预算规则（进入前预留、失败不静默、池 64KB 上限）。
- [ ] **Step 3:** 验证：四页截屏像素与阶段 3 证据一致；`UI MEM` 显示 delta；池水位记录；故意把某页 build 返回 NULL 一次（临时）确认错误可见后还原。
- [ ] **Step 4:** Commit `feat(components/sloop_ui): LVGL 页面懒建与预算框架（ADR-008）`。

### Task 5: 步窗抽象 `{bank, window_start, window_len}`

**Files:**
- Modify: `firmware/components/sloop_state/{include/sloop_state.h,src/state.c,src/cmd.c,test/state_test.c}`、`firmware/components/sloop_ui/src/page_step.c`、`scripts/inject/inject.py`（几何由窗口派生）

**Interfaces:**
- Produces: `typedef struct { uint8_t bank, window_start, window_len; } step_window_t;`（`step_window()` 读取；`CMD_SET_WINDOW`；默认 16/0/16）；STEP 页列 = window_len；hit-test 表由窗口派生；注入坐标同步。
- Consumes: Task 1 的引擎写路径（无直接依赖）；现有 STEP 页。

- [ ] **Step 1 (RED):** state-test：默认窗口 16/0/16；`CMD_SET_WINDOW{bank=1,start=16,len=16}` 后帧数/步号映射正确；越界 clamp（len 1..16、start 对齐 bank）。
- [ ] **Step 2:** 实现 + STEP 页消费 + 注入几何派生；`STATE SNAP` 打印窗口。
- [ ] **Step 3:** 验证：窗口切到 bank1 后点格写第 16+idx 步（`ENG STEP` 断言）；截屏（列标题 17..32）；state-test/host-test 绿。
- [ ] **Step 4:** Commit `feat(components/sloop_state): 步窗抽象与 STEP 页分窗`。

### Task 6: nav.sh 四导航用例

**Files:**
- Create: `scripts/inject/nav.sh` + `scripts/inject/inject.py` 场景 `nav`（或独立小脚本）
- Test: 四用例全绿

- [ ] **Step 1:** 用例：① `TAP` 页签外区域 miss（无状态变化）；② STEP 鼓轨行点按只读（`STATE SNAP focus` 不变、无写）；③ `KEY tab` 与 `KEY PANIC` 同 tick（最终页面一致、无回跳）；④ 单 tick 三次 `KEY tab`（只前进一页，ADR-006 行为断言）。
- [ ] **Step 2:** 真机跑 `nav.sh` 退出码 0；原始 transcript 入库 `docs/dev/verify/4-nav/`（或 3-* 目录延续命名）。
- [ ] **Step 3:** Commit `test(scripts): nav.sh 四导航回归用例`。

### Task 7: 阶段 3 机器侧残留小项

**Files:** `docs/dev/verify/3-*` 措辞、`docs/deferred-issues.md`（DI-10 记录）、`AGENTS.md`、`docs/design/005-ui-architecture.md`
- [ ] 修正 step-two-on「逐字节相同」→「网格像素不变，hint 文案按 M4 变更」；AGENTS §5.5/§5.1 登记 quiesce 全量口径（Task 2 完成后）；「无等待者」措辞改为「置位后新到不取锁」；DI-11 基线复现日志归档（若 Task 2 删除 DI-11 则跳过）；5804µs 统一。
- [ ] Commit `docs(.): 阶段 3 机器侧残留口径修正`。

### Task 8: `sloop_core_desc` 描述符读取

**Files:**
- Modify: `firmware/components/sloop_core/{include/sloop_core.h,src/selftest.c}`
- Test: 设备 `ENG DESC <scope> <engine> <id>` 抽样 + host 编译

**Interfaces:**
- Produces（spec §4.1 逐字）：
```c
typedef enum { SLOOP_DESC_TRACK, SLOOP_DESC_GLOBAL, SLOOP_DESC_ENGINE } sloop_desc_scope_t;
typedef struct { const char *label, *unit; uint8_t fmt; int16_t min, max, def;
                 const char *const *enums; uint8_t enum_count; } sloop_desc_t;
bool sloop_core_desc(sloop_desc_scope_t scope, uint32_t engine, uint32_t id, sloop_desc_t *out);
```

- [ ] **Step 1:** 实现（TRACK→`TP[]`；GLOBAL→`GP[]`；ENGINE→`ENGINES[engine]->edit[]`；鼓轨 id==P_E0→`DRUM_KIT_DESC`，与 params.c `is_drum` 口径一致）；`ENG DESC` 打印。
- [ ] **Step 2:** 设备抽样对照 params.c 10 项：`P_LEVEL`(F_DB 0..127 def104)、`P_ATK`(F_TIME)、`P_ED_FLT`(F_BIPCT -64..63)、`G_BPM`(F_BPM 40..240 def90)、某引擎 F_ENUM 项（枚举名非空）等——逐项断言（脚本比对预期表）。
- [ ] **Step 3:** Commit `feat(components/sloop_core): 参数描述符读取 API 与 ENG DESC`。

### Task 9: `sloop_core_set_params` + 状态层参数命令

**Files:**
- Modify: `firmware/components/sloop_core/{include,src}`、`firmware/components/sloop_state/{include,src/test}`

**Interfaces:**
- Produces: `void sloop_core_set_params(uint32_t track, const uint8_t *ids, const int16_t *values, uint32_t n);`（一次锁内，GP 范围钳位，越界 id 忽略）；`CMD_SET_PARAM{a=track,b=id,v=value}`、`CMD_SET_PARAM_BATCH{p=数组,n}`（`edit_cmd_t` 增 `const void *p; uint32_t n;` 字段——**同步 apply 语义**，数组在 apply 返回前由调用方保活）；`app_state_t` 增 `int16_t p[P_COUNT]; int16_t g[G_COUNT];`（选中轨投影 + 全局），`state_sync_from_engine` 填充。
- Consumes: Task 8 的 desc（测试断言用）。

- [ ] **Step 1 (RED):** state-test：单 SET 生效且钳位；BATCH N 对一次引擎调用（stub 计数=1）；越界 id 忽略；`CMD_SET_ENGINE` 走 `sloop_core_track`。
- [ ] **Step 2:** 实现 + `STATE SNAP` 打印选中轨若干参数；GREEN ×2。
- [ ] **Step 3:** 设备：`ENG DESC` + `CMD` 注入（console `PARAM <id> <value>` 临时命令或 `KEY` 路径）→ `STATE SNAP` 反映；build×2/flash/boot。
- [ ] **Step 4:** Commit `feat(components/sloop_state): 参数命令与批量设置投影`。

### Task 10: 参数页框架（旋钮页 + 布局表）

**Files:**
- Create: `firmware/components/sloop_ui/src/page_params.c`、`src/page_layout.c`（策展布局表）、`src/knob.c`
- Modify: `docs/design/008-param-pages.md`（定稿 ADR）
- Test: 页面构建/更新/池水位

**Interfaces:**
- Consumes: Task 8/9 的 API、Task 4 的页面注册表。
- Produces: `page_params_build(group)`（8 槽旋钮：触摸拖拽粗调、长按/键盘细调）；布局表 `{group,title,slots[{scope,engine,id}]}` 由 UI 持有（映射上游 PAGES 语义）；值显示用 desc fmt 格式化（F_ENUM 显示枚举名、F_DB 显示 dB、F_TIME 显示时间感）。

- [ ] **Step 1:** 旋钮控件（弧线 + 数值文本；拖拽映射参数范围；响应 ≤16ms/步）；页面注册（懒建）。
- [ ] **Step 2:** 布局表初版：GLOBAL + MASTER + ANALOG 引擎组（3–4 页规模）；`UI MEM` 记录池 delta。
- [ ] **Step 3:** 验证：参数编辑经 BATCH 下发 → `STATE SNAP`/`ENG SNAP` 生效；截屏 + 探针（旋钮弧线/数值文本区域）；池水位入 ADR。
- [ ] **Step 4:** Commit `feat(components/sloop_ui): 描述符驱动参数页与旋钮控件`。

### Task 11: 3 引擎页 + GLOBAL/家族页 + P4-A 收口证据

**Files:** `page_layout.c`（ANALOG/FM6/SAMPLE + GLOBAL + MASTER 或 FX 页）、`docs/dev/verify/4-params/`

- [ ] **Step 1:** 布局表补 ANALOG（`ENGINES[0]->edit[]` 槽位）、FM6（`ENGINES[9]`，含 F_ENUM 宏）、SAMPLE（`ENGINES[4]`）三页 + GLOBAL + MASTER 家族页；每页 8 槽按上游语义挑选（记录取舍表）。
- [ ] **Step 2:** 逐页验证：改值 → 引擎生效（ENG SNAP/STATE SNAP）；截屏矩阵（每页 2 状态：默认/改值）+ 探针；池水位表；`UI MEM`。
- [ ] **Step 3:** 全门禁复跑（build×2、host、state、全量 SELFTEST quiesce、三注入脚本、nav.sh）；证据晋升 `docs/dev/verify/4-params/`（含 SHA256SUMS）；verification-log 登记。
- [ ] **Step 4:** 文档：ADR-008 定稿；AGENTS §3（新页/新命令）；roadmap §1 阶段 4 子进度备注（P4-0/P4-A 完成）；spec §3 状态列更新。
- [ ] **Step 5:** `git checkout dev && git merge --no-ff feat/phase4`；push dev（网络错误重试）。
- [ ] **Step 6:** Commit `docs(.): P4-0/P4-A 收口与参数页框架证据入档`。

---

## 完成判据对照（本计划范围）

1. P4-0：DI-9/DI-11 删除、SCR 重试硬化、懒建预算、步窗、nav.sh、残留口径——全部机器侧绿；
2. P4-A 框架口径：3 引擎页 + GLOBAL + 家族页可编辑生效，截屏+探针+池水位证据入库；
3. 全量 SELFTEST（quiesce）182/182；host/state 测试绿；build×2 零警告。

## 遗留与后续

- P4-B（存储/预置）、P4-C（音序/编排）、P4-D（采样/DSYN/FM6）、P4-E（设置）、P4-F（全页面矩阵/数小时 soak/INA226/dev→main）——各自开工时另立计划。
- 人测清单延续：键盘手感/DI-4、物理触摸+DI-10、视觉、听感（含 mute/solo 可听性）。
