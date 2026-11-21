# P4-B 存储与预置 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Tab5 独立模式具备 SLOOP 等价的持久化能力——工程 A–D、用户预置 32 槽、FM6 银行 27 槽、工厂预置/kit 浏览，并把导航改成「分组 + 组作用域页面」以解开 9 页池天花板。

**Architecture:** 引擎单 TU 内 include 上游 `storage.c`（A/B+CRC 原样）、`project.c`（`PROJ_HOST` 格式层 + capture/apply）、`upreset.c`、`fm6_bank.c`，用薄 shim 与 PSRAM 宏重定向接上 Tab5；底层是自定义原始 flash 分区 `sloopstore` + `esp_partition_mmap`（`st_xip` 零成本）。UI 侧两行页签（组/子页）复用现有 72px 页签条，离开组即销毁该组页面；FILE 组新增 PROJECTS/PRESETS 两页，FM6 页加 bank 存取行。

**Tech Stack:** ESP-IDF 5.5.5 / ESP32-P4、esp_partition+spi_flash、LVGL 9.6、FreeRTOS、上游 SLOOP 2.8.5（`third_party/sloop-fm1`，只读）、Python 验证脚本（pyserial）。

**Spec:** `docs/superpowers/specs/2026-11-21-p4b-storage-presets-design.md`（本计划的全部决策来源；实施者必须同时读）

## Global Constraints

- 构建：`scripts/build.sh` 连续两遍退出码 0，第二遍**零新增**警告/错误（AGENTS §5.5；kconfig 首 pass 的 CMake 豁免仅限已知那条）。
- 烧录：`scripts/flash.sh /dev/ttyACM0` 输出含 `Hash of data verified`；启动日志无 fatal/abort/backtrace/task_wdt panic。
- 引擎回归：`scripts/host-test.sh` 全绿；设备 `scripts/device-selftest.py /dev/ttyACM0` 全量 182/182（自动停/复流）。
- 存储 TU：`third_party/sloop-fm1/**` **零修改**（manifest 233/233）；所有适配只进 `firmware/components/sloop_core/src/store_tab5.c`。
- 内部 RAM：idle `internal_free ≥ 61,440B`；`proj_slot`/`proj_v3_tmp` 实体不得进 DRAM；`main_scratch`（3,840B）必须内部 SRAM。
- LVGL 池：新不变量 `base + max_group Σest + 4KB ≤ 63,120`；每页构建前仍检查 `free ≥ est`；不得扩大 `CONFIG_LV_MEM_SIZE`。
- 并发：所有存储写/载经引擎锁与 `s_store_lock`；播放中（`song.playing || transport_req`）拒绝写；console `STORE` 是诊断执行器（与 UI 的 CMD 共用同一 API）。
- 提交：AGENTS §7 格式（`<type>(<模块路径>): 中文摘要` + `[Motivation]/[Change_Set]/[Impact_AST]/[Infra_State]`；`fix` 另加 `[Root_Cause]/[Blast_Radius]`）。每个 Task 结束后 review clean → 立即 push `origin feat/phase4b`。
- 工作流：子代理驱动，**不用 worktree**；每个 Task 一个新的 implementer + 一个 reviewer；reviewer 通过后才 push。

## Review Focus

1. **存储未就绪**（分区丢失/mmap 失败/初始化未跑）：任何 `STORE` 命令返回 `store ERR no_storage`，UI 显示 `NO STORAGE`，引擎/音频照常工作，设备不崩溃。（Task 2/3 的测试）
2. **空槽 / 越界 / 鼓轨**：LOAD/ERASE 空槽返回 `EMPTY` 且不改引擎；slot ≥ 4/32/27 被拒；鼓轨存预置返回 `DRUM TRACK` 错误。（Task 3/4/5/8）
3. **播放中保存**：返回 `ERR playing`、存储内容不变；停止后同一操作成功。（Task 3/4/5）
4. **掉电窗口**：擦除失败 / payload 半写 / 头写失败时旧副本仍被选中并完整加载。（Task 6）
5. **损坏或截断的旧格式 payload**（FUN1–FUN5）：`proj_import` 拒绝，当前工程与其它槽不受影响，该槽读为空。（Task 7）
6. **切组销毁页面时的在途输入**（同拍 tab+key / PANIC）：不得访问已销毁对象；`nav.sh` 四用例在新语义下通过。（Task 9）

---

### Task 0: 基线

**Files:**
- Create: `docs/dev/verify/4-pb-2026-11-21/task-0-baseline.txt`

**Interfaces:**
- Consumes: 当前 `dev`（`3cfff13` 之后）
- Produces: 分支 `feat/phase4b`；后续 Task 的对照基线

- [ ] **Step 1: 建分支**

```bash
git checkout dev && git pull --ff-only origin dev
git checkout -b feat/phase4b
```

- [ ] **Step 2: 记录基线门禁**

```bash
scripts/build.sh && scripts/build.sh
scripts/host-test.sh
python3 scripts/device-selftest.py /dev/ttyACM0
python3 scripts/screenshot.py /dev/ttyACM0 docs/dev/verify/4-pb-2026-11-21/task-0-play.png
```
把三条命令的尾部输出（构建警告数、`ALL HOST TESTS PASSED`、`SELFTEST PASS 182/182`、截图路径与 SHA256）写进 `task-0-baseline.txt`。

- [ ] **Step 3: 提交并 push**

```bash
git add docs/dev/verify/4-pb-2026-11-21/task-0-baseline.txt docs/dev/verify/4-pb-2026-11-21/task-0-play.png
git commit -m "chore(dev/verify): P4-B 基线门禁与 PLAY 截图"
git push origin feat/phase4b
```

---

### Task 1: 设计决策落文档（ADR-005 导航 + ADR-009 存储 + 关 DI-17）

**Files:**
- Create: `docs/design/009-storage-backend.md`
- Modify: `docs/design/005-ui-architecture.md`（「导航信息架构」节：写入选定方案与不变量）
- Modify: `docs/design/008-param-pages.md`（§3/§5：组作用域生命周期对预算表的影响，指向 ADR-005）
- Modify: `docs/deferred-issues.md`（**删除 DI-17 整条**）
- Modify: `docs/design/002-port-roadmap.md`（阶段 4 状态行：P4-B 开工）
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（§4.3 改为指向 ADR-009，避免与 P4-B spec §3.1 冲突）

**Interfaces:**
- Consumes: P4-B spec §3.1/§3.3/§4.2/§5
- Produces: ADR-005 的「两行页签 + 组作用域生命周期 + 新内存不变量」决策；ADR-009 的存储后端决策

- [ ] **Step 1: 写 ADR-009**

按仓库现有 ADR 结构（背景/选项/决策/后果）写 `docs/design/009-storage-backend.md`，内容取自 spec §3.1/§3.2/§3.4/§4.1/§4.2：原始分区 vs FATFS 文件 vs NVS 的对照与取舍、flash map 与分区表、mmap/XIP、擦写与 codec mute、xrun 判据、console 诊断执行器例外、PSRAM 宏重定向、只读 vendored。

- [ ] **Step 2: 更新 ADR-005**

把 spec §3.3 的决策写入 ADR-005「导航信息架构」节：5 组（PLAY/TRACKS/EDIT/SEQ/FILE）、两行页签复用 72px、组作用域销毁、`TAB` 扁平轮转新顺序（PLAY → TRACKS → GLOBAL → FX → ANALOG → FM6 → SAMPLE → STEP → TRANSPORT → PROJECTS → PRESETS）、新不变量 `base + max_group Σest + 4KB ≤ 63,120`。保留旧节的历史数据（别删 9 页实测）。

- [ ] **Step 3: 关 DI-17**

删除 `docs/deferred-issues.md` 的 DI-17 整条（背景/影响面/…/登记）。**同一条目不得残留**；`grep -rn 'DI-17' docs/` 只允许命中 `docs/dev/verification-log.md` 与 `docs/dev/verify/**` 的历史记录。

- [ ] **Step 4: 同步 roadmap / 阶段 4 spec / ADR-008**

roadmap 阶段 4 状态行加「P4-B 开工（`feat/phase4b`），导航/容量决策见 ADR-005/009，DI-17 关闭」；phase-4 spec §4.3 改为一句「存储后端由 ADR-009 定义（原始分区 + mmap，取代本文原 FATFS 文件措辞）」；ADR-008 §3/§5 加一段「组作用域生命周期：预算按组核算，`UI MEM` 增加分组 Σ」。

- [ ] **Step 5: 检查并提交**

```bash
grep -rn 'DI-17' docs/ | grep -v verification-log | grep -v 'dev/verify/'
git diff --check
git add docs/
git commit -m "docs(design): P4-B 导航与存储后端决策（ADR-005/009），关闭 DI-17"
git push origin feat/phase4b
```

---

### Task 2: 分区与 HAL 存储后端

**Files:**
- Modify: `firmware/partitions.csv`
- Create: `firmware/components/sloop_hal_tab5/src/storage_flash.c`
- Modify: `firmware/components/sloop_hal_tab5/include/sloop_hal.h`
- Modify: `firmware/components/sloop_hal_tab5/CMakeLists.txt`
- Modify: `firmware/main/console.c`（`STORE STATS` / `STORE RAWTEST` 最小命令）

**Interfaces:**
- Consumes: 无
- Produces（spec §4.1 逐字）:
  `esp_err_t sloop_hal_storage_init(void); bool sloop_hal_storage_ready(void);`
  `const uint8_t *sloop_hal_storage_map(void); uint32_t sloop_hal_storage_base(void); uint32_t sloop_hal_storage_span(void);`
  `int sloop_hal_storage_read(uint32_t off, void *dst, uint32_t n); int sloop_hal_storage_erase(uint32_t off); int sloop_hal_storage_prog(uint32_t off, const void *src, uint32_t n);`
  `void sloop_hal_storage_fault_arm(uint32_t nth, bool half); uint32_t sloop_hal_storage_fault_taken(void); void sloop_hal_storage_fault_clear(void);`

- [ ] **Step 1: 改分区表**

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x6000,
phy_init,   data, phy,     0xF000,   0x1000,
factory,    app,  factory, 0x10000,  0x800000,
sloopstore, data, 0x99,    0x810000, 0x68000,
storage,    data, fat,     0x878000, 0x788000,
```
文件头注释写死：`sloopstore` 承载上游 flash map `0x97000..0xFF000`（内部偏移 = flash 偏移 − 0x97000），不得移动；`storage`（FAT）留给 P4-D 采样资产。

- [ ] **Step 2: 写 HAL**

`storage_flash.c`：`init` 幂等；`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x99, "sloopstore")` → `esp_partition_mmap(part, 0, span, ESP_PARTITION_MMAP_DATA, &map, &handle)`；`base=0x97000`、`span=0x68000`；`read` = 越界检查后 `memcpy(map + off - base)`；`erase` = 4KB 对齐检查 → `codec mute`（`sloop_hal_audio_codec()` 为 NULL 时跳过）→ `esp_partition_erase_range(part, off-base, 0x1000)` → `unmute`；`prog` = 越界检查 → `esp_partition_write`；失败路径返回 -1/-8 并保持 `ready=false`（init 失败时）。故障注入是三个 hook 共用的原子计数器：命中时 `read` 返回 -1、`erase` 返回 -1、`prog` 在 `half` 时只写 `n/2` 再返回 -1，否则不写返回 -1。

- [ ] **Step 3: console 最小命令**

`store_command`：`STORE STATS` 打印 `STORE STATS ready=1 base=0x97000 span=0x68000 map=0x… objects=0 faults=0`（未 ready 打印 `ready=0`）；`STORE RAWTEST` 对最后一个扇区（flash 偏移 `0xFE000`）做 erase → 写 0x5A 模式 → 校验 → 再 erase，打印 `STORE RAWTEST PASS`/`FAIL`。此命令是破坏性的，`help` 里注明。

- [ ] **Step 4: 构建 + 烧录**

```bash
scripts/build.sh --clean && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
```
分区表变了：首次烧录后确认启动日志里 `sloopstore` 分区被找到、无 `partition` 错误。

- [ ] **Step 5: 验证**

```bash
python3 scripts/device-selftest.py /dev/ttyACM0            # 182/182
bash scripts/inject/nav.sh /dev/ttyACM0                     # 旧 9 页语义，应仍绿
python3 scripts/screenshot.py /dev/ttyACM0 docs/dev/verify/4-pb-2026-11-21/task-2-play.png
cmp docs/dev/verify/4-pb-2026-11-21/task-0-play.png docs/dev/verify/4-pb-2026-11-21/task-2-play.png
```
`cmp` 应无输出（像素零变化）；`STORE STATS`/`STORE RAWTEST` 输出用 `inject.py` 的 `Session.cmd` 抓取并归档。

- [ ] **Step 6: 提交**

```bash
git add firmware/partitions.csv firmware/components/sloop_hal_tab5 firmware/main/console.c docs/dev/verify/4-pb-2026-11-21/task-2-*
git commit -m "feat(components/sloop_hal_tab5): 新增原始分区存储后端与 mmap 读窗口"
git push origin feat/phase4b
```

---

### Task 3: 存储核心 + 工程 A–D 存读

**Files:**
- Create: `firmware/components/sloop_core/include/sloop_store.h`
- Create: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/components/sloop_core/src/selftest.c`（末尾 include；`sloop_core_init` 调 `sloop_store_init`）
- Modify: `firmware/components/sloop_core/CMakeLists.txt`（`PRIV_REQUIRES` 加 `esp_partition`）
- Modify: `firmware/main/console.c`（`STORE PROJ …` / `STORE LIVEHASH`）
- Modify: `scripts/inject/inject.py`（新 scenario `store`）+ Create `scripts/inject/store.sh`

**Interfaces:**
- Consumes: Task 2 的 HAL（§4.1）
- Produces（spec §4.2，本期只含这些）:
  `esp_err_t sloop_store_init(void); bool sloop_store_ready(void); void sloop_store_stats(sloop_store_stats_t *out);`
  `bool sloop_store_project_used(uint32_t slot); uint32_t sloop_store_project_hash(uint32_t slot); uint32_t sloop_store_live_hash(void);`
  `esp_err_t sloop_store_project_save(uint32_t slot); esp_err_t sloop_store_project_load(uint32_t slot); esp_err_t sloop_store_project_erase(uint32_t slot);`

- [ ] **Step 1: 写失败测试（设备注入）**

在 `scripts/inject/inject.py` 注册 `SCENARIOS["store"] = run_store` 并新增 `store.sh`（与 `nav.sh` 同构：source `env.sh` → `exec python3 inject.py --scenario store`）。`run_store` 第一批断言：
```python
# STORE PROJ LIST -> "PROJ 0 EMPTY" …（全新分区）
# 改 BPM -> STORE LIVEHASH = h0
# STORE PROJ SAVE 0 -> "PROJ 0 SAVED"
# STORE PROJ HASH 0 == h0
# 改 BPM -> LIVEHASH = h1 != h0
# STORE PROJ LOAD 0 -> LIVEHASH == h0；STATE SNAP 的 bpm 与保存前一致
# STORE PROJ ERASE 0 -> LIST 显示 EMPTY；LIVEHASH 不变
```
命令统一用 `Session.cmd()`，失败抛 `InjectError`。

- [ ] **Step 2: 运行确认失败**

```bash
scripts/build.sh && scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
```
Expected: FAIL（`STORE` 未知命令 / 无 `PROJ` 输出）。

- [ ] **Step 3: 写 `sloop_store.h`**

按 spec §4.2 的注释风格（中文契约注释：线程、返回码、播放中语义）声明本 Task 的 API + `sloop_store_stats_t`（`ready/objects_ok/faults_armed/faults_taken`）。

- [ ] **Step 4: 写 `store_tab5.c` 骨架（shim + hooks + 上游 include）**

顺序严格如下（细节见 spec §4.2）：
1. `#include "scratch.h"`（`main_scratch`，内部 SRAM；`st_buf` 由 storage.c 静态断言对齐）。
2. PSRAM 重定向宏：`#define proj_v3_tmp (*s_v3_ps)`、`#define proj_slot (*s_slot_ps)`（仅声明退化，实体不分配）。
3. shim：`flash_ok`（= `sloop_hal_storage_ready()`）、`ST_XIP_PTR(off)`、`FM6_BANK_XIP(copy)`、`trk_def_engine`、`user_of`、`param_kept`、`arp_param`、`arp_is_sounds`（抄上游 `ui.c:24/592/605/607` 的语义）、`sound_change` 用 `voice.c` 已有实现、`ui_message/ui_say`（写 `s_ui_msg` 并 printf 供脚本断言）、`ui.force`（4 字节占位结构）、`up_gen`、`sync_reload`（`up_stage` 在 item 7 定义，依赖 `proj_tmp`）。
4. hooks：`st_read/st_erase/st_prog` 转 `sloop_hal_storage_*`（codec mute 由 HAL 负责）。
5. `#include "storage.c"`（必须在本步之前已有 `ST_XIP_PTR` 与 hooks；`main_scratch` 已就位）。
6. `#define PROJ_HOST 1` → `#include "project.c"` → `#undef PROJ_HOST`。
7. 自有 PSRAM 暂存：`typedef union { project_v5_t v5; uint8_t raw[4096]; } proj_stage_t; static proj_stage_t *s_stage; #define proj_tmp (*s_stage)`，再定义 `static void *up_stage(void) { return &proj_tmp; }`（Task 4 的 upreset.c 用它）。静态断言 `sizeof(proj_tmp) >= sizeof(project_t) && >= sizeof(fm6_bank_t)`（后者在 Task 5 加，届时 fm6 类型已可见）。

- [ ] **Step 5: 实现工程 API（`sloop_store_*`）**

- `sloop_store_init`：`s_v3_ps = heap_caps_malloc(sizeof *s_v3_ps, MALLOC_CAP_SPIRAM)`、`s_stage = heap_caps_malloc(sizeof *s_stage, MALLOC_CAP_SPIRAM)`；失败 `ESP_ERR_NO_MEM`；HAL init；`ready = 两个分配成功 && sloop_hal_storage_ready()`。
- `project_save(slot)`：越界 `ESP_ERR_INVALID_ARG`；未 ready `ESP_ERR_INVALID_STATE`；引擎锁内检查 `song.playing || transport_req`（是→`ESP_ERR_INVALID_STATE`）并 `proj_capture(&proj_tmp.v6)`；释放锁后 `st_save(OBJ_PROJECT0 + slot, &proj_tmp.v6, sizeof(project_t))`，非 0 → `ESP_ERR_NOT_FINISHED`。
- `project_load(slot)`：播放中拒绝；`int n = st_load(OBJ_PROJECT0 + slot, s_stage->raw, sizeof(project_t))`；`n < 0` → `ESP_ERR_NOT_FOUND`；`proj_import(&p, s_stage->raw, n)` 失败 → `ESP_ERR_INVALID_ARG`；引擎锁内 `proj_apply(&p, 1)`、`song.sel` 钳位。
- `project_erase(slot)`：`st_save(OBJ_PROJECT0 + slot, NULL, 0)`。
- `project_used(slot)` / `project_hash(slot)`：`st_load` → `proj_import` → `proj_ok`；hash 返回 `p->sum`（空/坏 = 0）。
- `live_hash()`：引擎锁内 `proj_capture(&p)`，返回 `p.sum`。

- [ ] **Step 6: 接进启动与控制台**

`selftest.c` 末尾 `#include "store_tab5.c"`；`sloop_core_init` 在引擎初始化后调 `sloop_store_init()`（失败只记 WARN，不阻断音频）；`console.c` 注册 `STORE`：`STATS|PROJ LIST|PROJ SAVE n|PROJ LOAD n|PROJ ERASE n|PROJ HASH n|LIVEHASH`，错误统一打印 `store ERR <name>`（`no_storage/playing/empty/invalid/save`），脚本按此断言。

- [ ] **Step 7: 构建 + 设备门禁**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0        # PASS（工程往返）
python3 scripts/device-selftest.py /dev/ttyACM0  # 182/182
```
内存门禁：`ENG MEM` 的 `internal_free ≥ 61440`；`grep -E 'proj_slot|proj_v3_tmp|s_v3_ps' firmware/build/sloop_fm1_tab5.map` 确认没有 15KB 级的 `.noinit`/`.bss` 实体。

- [ ] **Step 8: 提交**

```bash
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "feat(components/sloop_core): 接上游 storage/project 格式层，工程 A-D 存读"
git push origin feat/phase4b
```

---

### Task 4: 用户预置 32 槽

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/main/console.c`
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 3 的 `proj_tmp`/`up_stage`/shims
- Produces:
  `uint32_t sloop_store_preset_count(void); bool sloop_store_preset_used(uint32_t slot); bool sloop_store_preset_name(uint32_t slot, char out[13]); uint8_t sloop_store_preset_engine(uint32_t slot);`
  `esp_err_t sloop_store_preset_store(uint32_t slot); esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track); esp_err_t sloop_store_preset_erase(uint32_t slot);`

- [ ] **Step 1: 追加失败断言**

`run_store` 增加：选中轨 ANALOG → 改 8 个参数（`PARAM`）→ 记录 `STATE SNAP` 的 p[] → `STORE PRESET SAVE 0` → 改参数 → `STORE PRESET LOAD 0 0` → p[] 与记录逐项相等；`STORE PRESET LIST` 含 `U01` + 引擎名；`STORE PRESET ERASE 0` 后 LIST 为空；鼓轨（`STATE SET_TRACK 3` 或 `CMD` 路径）`STORE PRESET SAVE 1` → `store ERR drum`。运行确认 FAIL。

- [ ] **Step 2: include 上游并实现 API**

`store_tab5.c` 在 `project.c` 之后 `#include "upreset.c"`（不定义 `UP_HOST`）。`preset_store` = 引擎锁内校验选中轨非鼓轨 → `up_store(slot, 0)`；`preset_load` = `up_load_to(&trk[track], slot)`（越界/鼓轨拒绝）；`preset_erase` = `up_put(slot, NULL)`；`used/name/engine/count` 直接读 `up_rec/up_valid/up_name`（XIP，无需锁）。错误映射：`up_put` 返回 2→`ESP_ERR_NOT_FINISHED`、3→`ESP_ERR_INVALID_STATE`；空槽→`ESP_ERR_NOT_FOUND`；鼓轨→`ESP_ERR_INVALID_ARG` + `ui_message("DRUM TRACK: NO SOUND")`。

- [ ] **Step 3: console 命令**

`STORE PRESET LIST|SAVE n|LOAD n [track]|ERASE n`，输出行格式：`PRESET <k> USED <ENGINE> <NAME>` / `PRESET <k> EMPTY`；`LOAD` 默认 track = `song.sel`。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0    # 工程 + 预置全部 PASS
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "feat(components/sloop_core): 用户预置 32 槽存取（upreset.c 接入）"
git push origin feat/phase4b
```

---

### Task 5: FM6 银行 27 槽

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/main/console.c`
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 3 的 storage 符号与 `proj_tmp`
- Produces:
  `bool sloop_store_bank_used(uint32_t slot); bool sloop_store_bank_get(uint32_t slot, uint8_t out[128]); esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed); esp_err_t sloop_store_bank_erase(uint32_t slot); void sloop_store_bank_current(uint8_t *packed);`

- [ ] **Step 1: 追加失败断言**

`run_store`：`ENG TRACK 0 9 0`（选中轨切 FM6 工厂 patch）→ `STORE BANK LIVE` 得 hex L → `STORE BANK PUT 3` → `STORE BANK GET 3` == L；`STORE BANK LIST` 含 `B3 USED`；断电重启（`scripts/flash.sh` 后）`BANK LIST` 仍 USED；`BANK ERASE 3` 后 GET 返回 `store ERR empty`；`ENG TRACK 0 9 11`（PTCH=11 → bank 槽 3）后 `ENG TRIG 0 60` 无错误。运行确认 FAIL。

- [ ] **Step 2: include 上游并实现**

`store_tab5.c` 在 `upreset.c` 之后：定义 `flash_ok`（已有）、`FM6_BANK_XIP(copy)`（= `sloop_hal_storage_map() + (0xE5000 + copy*4096) - base`）与 4KB 对齐的 `proj_tmp` 前置条件，然后 `#include "fm6_bank.c"`。API 包装 `fm6_bank_put/fm6_bank_get/has`（static，已在 TU 内），`bank_current` 在引擎锁内 `fm6_pack(fm6_patch[track], out)`（`fm6_patch` 是 `eng_fm6.c` 的 static，同 TU 可见）。播放中拒绝；`fm6_bank_put` 返回 1→`ESP_ERR_INVALID_ARG`、2→`ESP_ERR_NOT_FINISHED`。

- [ ] **Step 3: console**

`STORE BANK LIST|PUT n|GET n|LIVE|ERASE n`；`PUT` 取选中轨当前 patch（`bank_current`），打印 128B hex 供脚本比对。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "feat(components/sloop_core): FM6 银行 27 槽存取（fm6_bank.c 接入）"
git push origin feat/phase4b
```

---

### Task 6: 设备侧 torn-write 故障注入套件

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`（`sloop_store_test_t` + `sloop_store_test`）
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/main/console.c`（`STORE TEST` / `STORE FAULT`）
- Modify: `scripts/inject/inject.py`（`run_store` 的 torn 段）

**Interfaces:**
- Consumes: Task 2 的 `sloop_hal_storage_fault_*`；Task 3 的 storage 符号
- Produces: `esp_err_t sloop_store_test(sloop_store_test_t *out)`（`{cases,passed,fail_case}`）

- [ ] **Step 1: 失败测试**

`run_store` 追加 `STORE TEST` 断言输出 `STORE TEST PASS 6/6`。6 个场景用 `OBJ_AUTOSAVE`：
1. 写 v1、写 v2 → 读回 v2（seq 大者）；
2. `FAULT 1`（擦除失败）→ 写 v3 失败 → 读回仍 v2；
3. `FAULT half`（payload 半写）→ 写 v3 失败 → 读回 v2；
4. `FAULT 头写`（对 header 的 prog）→ 写 v3 失败 → 读回 v2；
5. 写后人为破坏 payload（`STORE RAW` 或测试内 `st_prog`）→ 加载返回错误而不是坏数据；
6. `st_save(…, len=0)` → 对象读为空。
每场景独立 arm/clear，失败打印 `STORE TEST FAIL <name>`。

- [ ] **Step 2: 运行确认失败**

`STORE TEST` 尚不存在 → FAIL。

- [ ] **Step 3: 实现**

`store_tab5.c` 内实现六个场景（直接用 `st_save/st_load` 与 `st_sector`，payload 为 64B 递增模式）；`sloop_store_test` 全程持 `s_store_lock`，结束后 `sloop_hal_storage_fault_clear()` 并恢复 autosave 为空。console 转发并打印结果。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0        # 含 PASS 6/6
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "test(components/sloop_core): 设备侧 A/B torn-write 故障注入套件"
git push origin feat/phase4b
```

---

### Task 7: 旧格式兼容门禁（FUN5/FUN4/FUN1）与 RAW 上传通道

**Files:**
- Modify: `firmware/main/console.c`（`STORE RAW …`、`STORE PROJ FIELDS n`）
- Modify: `firmware/components/sloop_core/src/store_tab5.c`（RAW 暂存与 FIELDS 输出）
- Create: `scripts/gen_project_fixtures.py`
- Create: `scripts/store_raw.py`
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 3 的 `project_load`、`proj_import`
- Produces: `scripts/gen_project_fixtures.py --out DIR`（写 `fun5.bin/fun4.bin/fun1.bin` 与 `expected.json`）；`scripts/store_raw.py PORT FILE --obj project --slot 3`

- [ ] **Step 1: 生成器 + 上传器**

`gen_project_fixtures.py`：照上游 `tests/project_test.c` 的构造函数生成三种 payload（不依赖该测试的二进制），`expected.json` 记录 `STORE PROJ FIELDS 3` 会打印的字段期望（sel、bpm、4 轨 engine/preset、p[P_LEVEL]/p[P_ATT]/…、首 8 步 n/note、全局代表值）。`store_raw.py`：打开串口，`STORE RAW BEGIN <len>` → 每行 ≤480 hex 的 `STORE RAW <hex>` → `STORE RAW END project 3`，逐行等回显，失败非零退出。

- [ ] **Step 2: 失败测试**

`run_store` 追加：对三个 fixture 依次 `store_raw.py` 上传 → `STORE PROJ LOAD 3` → `STORE PROJ FIELDS 3` 与 `expected.json` 逐项比对 → 再上传一个**截断** fixture（取 fun5 前 100 字节）→ `STORE PROJ LOAD 3` 返回 `store ERR invalid`，且当前工程 hash 不变。运行确认 FAIL（RAW 未实现）。

- [ ] **Step 3: 实现**

`STORE RAW BEGIN` 记长度并把后续 hex 解码进 `s_stage->raw`（容量 4096，超长即 `ERR size`）；`END` 按 `obj` 映射（`project`→`OBJ_PROJECT0+slot`）用 `st_save` 原样写；`ABORT` 复位；任一步失败复位。`STORE PROJ FIELDS n` 从槽 n `st_load`+`proj_import` 后打印一行 key=value（顺序固定，脚本按空白切分），**不改引擎**。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
git add firmware scripts/gen_project_fixtures.py scripts/store_raw.py scripts/inject
git commit -m "test(components/sloop_core): 旧格式工程 fixture 上传与兼容门禁"
git push origin feat/phase4b
```

---

### Task 8: 状态层存储命令与页 id 扩容

**Files:**
- Modify: `firmware/components/sloop_state/include/sloop_state.h`
- Modify: `firmware/components/sloop_state/src/cmd.c`
- Modify: `firmware/components/sloop_state/src/state.c`
- Modify: `firmware/components/sloop_state/test/stub_engine.c` + `state_test.c`
- Modify: `firmware/main/console.c`（`k_page_names` 加 `projects`/`presets`，静态断言保持）

**Interfaces:**
- Consumes: Task 3/4/5 的 `sloop_store_*`
- Produces: `CMD_PROJECT_SAVE/LOAD/ERASE`（`a=slot`）、`CMD_PRESET_STORE/LOAD/ERASE`（`a=slot,b=track`）、`CMD_BANK_PUT/ERASE`（`a=slot`）；`app_state_t.store_status`（0 idle/1 ok/2 empty/3 playing/4 error/5 drum）；`APP_PAGE_PROJECTS, APP_PAGE_PRESETS`，`APP_STATE_PAGES 11`

- [ ] **Step 1: 写失败测试（host）**

`state_test.c` 增加：stub 的 `sloop_store_*` 返回固定码；断言 `CMD_PROJECT_SAVE` 成功后 `st.store_status==1`；`LOAD` 空槽→2；播放中→3；鼓轨预置→5；`CMD_PRESET_LOAD` 后 `state_sync_from_engine` 被调用（stub 计数 +1）；越界 a 不改状态。运行 `scripts/state-test.sh` 确认 FAIL。

- [ ] **Step 2: 实现**

`edit_cmd_type_t` 尾部追加命令；`cmd.c` 分发到 `sloop_store_*` 并把返回值映射成 `store_status`（在 `state_locked()` 内写）；`cmd_project_save/load/erase` 成功后调 `state_sync_from_engine()` 内部版本（避免重入，先释放再拉取或直接调用已有刷新函数——按现有 `cmd_select_track` 的写法）。`STORE` 相关写操作前先读 `cmd` 槽位校验，越界直接忽略（不改状态）。

- [ ] **Step 3: 页 id**

`app_page_t` 追加两页；`APP_STATE_PAGES 11`；console `k_page_names` 追加 `"projects","presets"`；`sloop_ui.h` 的页枚举注释更新（页面尚未注册 → `UI projects` 返回 `not_registered`，属预期）。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/state-test.sh
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_state firmware/main/console.c
git commit -m "feat(components/sloop_state): 存储命令与 11 页 id 扩容"
git push origin feat/phase4b
```

---

### Task 9: 导航重构（两行页签 + 组作用域生命周期）

**Files:**
- Modify: `firmware/components/sloop_ui/src/ui.c`
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`（组 API、页名、`UI MEM` 组字段）
- Modify: `firmware/components/sloop_ui/src/input.c`（TAB 扁平轮转；ESC 回 PLAY）
- Modify: `firmware/main/console.c`（`UI MEM` 打印分组；`UI <page>` 保持）
- Modify: `scripts/inject/inject.py`（`run_nav` 期望序列）
- Modify: `docs/design/006-keyboard-mapping.md`（TAB 语义）

**Interfaces:**
- Consumes: Task 8 的页 id
- Produces: `ui_group_t`/`k_groups[]`；`sloop_ui_group_count(void)`；`UI MEM` 行 `GROUP <name> built=<n> sum_est=<B> sum_delta=<B>`

- [ ] **Step 1: 写失败断言**

`run_nav` 更新：①页签条内、页签矩形外的 TAP → miss；②鼓轨只读；③同拍 tab+PANIC → 不跳页；④同拍 3 次 tab → 只前进一页，且**新顺序**为 PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT（FILE 页在 Task 10 前未注册，轮转止于 TRANSPORT→PLAY）。另加：从 EDIT 组切到 SEQ 组后 `UI MEM` 显示 EDIT 组 built=0（组作用域生效）。运行确认 FAIL。

- [ ] **Step 2: 两行页签**

`create_tab_bar` 改为：上行 group 按钮（`y=4,h=32`，宽度按**已注册页非空的组数** `(UI_W-2*16-220-(n-1)*8)/n`，16pt 居中；没有已注册页的组不显示，故 FILE 在 Task 10 前不出现）；下行 sub 按钮（`y=38,h=30`，宽度 176 固定/按子页数收窄），只显示当前组的**已注册**子页；`SLOOP` 标题放上行右侧（`align TOP_RIGHT -24, 6`），层键提示放下行右侧（`-110, 40`，宽度按子页右端计算，`LONG_DOT`）；页面区起点/尺寸**不动**。顶层 group 点击 = 进入该组上次子页（默认首个子页）；子页点击 = 切页。

- [ ] **Step 3: 组作用域销毁**

`show_page(page)` 中：目标页组 ≠ 当前组时，`leave_group(old)` 销毁旧组所有已建页（`lv_obj_del`、`s_page_obj=NULL`、state=UNBUILT、delta=0），再建/显示目标页；同组切换只隐藏。销毁必须在 LVGL 锁内、且先作废待执行切页。`UI MEM` 增加每组建/预算/实测 Σ，并保持旧字段兼容（脚本仍可解析 `pool_free`）。

- [ ] **Step 4: 输入层**

`input.c` 的 `ACT_PAGE_NEXT` 改为按 `sloop_ui_page_order_next()`（组顺序扁平轮转），一次 tick 只前进一页的既有语义保留；`K_ESC` 回 `APP_PAGE_PLAY` 并复位组/子页。`docs/design/006-keyboard-mapping.md` 同步。

- [ ] **Step 5: 门禁**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/nav.sh /dev/ttyACM0
bash scripts/inject/params.sh /dev/ttyACM0
bash scripts/inject/tracks.sh /dev/ttyACM0
bash scripts/inject/transport.sh /dev/ttyACM0
bash scripts/inject/step_edit.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
```
截屏：`UI play|tracks|global|fx|analog|fm6|sample|step|transport` 各一张，检查两行页签文字不重叠、页面底行完整；与旧截屏的差异只允许页签区；`UI MEM` 记录分组水位。证据入 `4-pb-2026-11-21/`。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_ui firmware/main/console.c scripts/inject docs/design/006-keyboard-mapping.md docs/dev/verify/4-pb-2026-11-21
git commit -m "feat(components/sloop_ui): 分组导航与组作用域页面生命周期"
git push origin feat/phase4b
```

---

### Task 10: PROJECTS / PRESETS 页面

**Files:**
- Create: `firmware/components/sloop_ui/src/page_projects.c`
- Create: `firmware/components/sloop_ui/src/page_presets.c`
- Modify: `firmware/components/sloop_ui/src/ui.c`（注册 FILE 组两页 + est）
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`（TAP kind 扩展）
- Modify: `firmware/components/sloop_ui/src/input.c`（FILE 页 ←/→/ENTER）
- Modify: `scripts/inject/inject.py`（store 场景 UI 段）
- Modify: `docs/design/008-param-pages.md`（预算表加 FILE 组）

**Interfaces:**
- Consumes: Task 8 的命令；Task 9 的组模型
- Produces: `page_projects_build/page_projects_update`、`page_presets_build/page_presets_update`；TAP kind `SLOOP_TAP_FILE_SLOT`（`a=` 组内页、`b=` 槽位）与 `SLOOP_TAP_FILE_ACTION`（`v=` 动作 0 save/1 load/2 erase）

- [ ] **Step 1: 失败断言**

`run_store` UI 段：`UI projects` → `SCR` 探针（`PROJECTS` 标题、4 卡、按钮文字）→ `TAP` 点 SAVE/LOAD/ERASE → 用 `STORE PROJ LIST` 与 `STATE SNAP` 验证动作落地；`UI presets` → 32 槽网格探针 → TAP LOAD/SAVE/ERASE；播放中 TAP SAVE → 状态行 `STOP BEFORE SAVE` 且存储不变；`UI MEM` 断言 PROJECTS/PRESETS delta ≤ est；离开 FILE 组后两页 `built=0`。运行确认 FAIL（页未注册）。

- [ ] **Step 2: 实现 PROJECTS**

4 张卡片横排（`A/B/C/D`），每卡：标题、`EMPTY|USED`、hash 前 4 位（`sloop_store_project_hash`），下方 `SAVE/LOAD/ERASE` 三个按钮（`lv_button`，宽 100/高 40）。点击卡片设选中槽（局部态 `s_slot`），按钮发 CMD（`state_apply`）并把 `store_status` 映射为状态行文本。`update()` 只在 `state_get_copy().store_status` 或槽列表变化时重绘文本（避免每 30ms 刷屏）。键盘 ←/→ 改选中槽（input.c 在 FILE 页路由）。

- [ ] **Step 3: 实现 PRESETS**

4×8 网格（槽 144×72），每格 `U01` + 引擎名/`EMPTY` + 选中描边；下方 `LOAD/SAVE/ERASE` 按钮。`SAVE` 目标 = 当前选中轨（状态层 CMD，鼓轨→状态行 `DRUM TRACK: NO SOUND`）；`LOAD` 载入到选中轨；列表直查 `sloop_store_preset_*`（只读例外）。

- [ ] **Step 4: 注册与预算**

`k_groups[]` 的 FILE 组加入两页；`k_builtin_pages[]` 追加 `{APP_PAGE_PROJECTS,"PROJECTS",…,4608}` 与 `{APP_PAGE_PRESETS,"PRESETS",…,5120}`；实测 delta 后按 ADR-008 规则调整 est（±10–23%、512 对齐），并把数值写进 ADR-008 预算表与 `docs/dev/verify` 证据。**`run_nav` 的轮转期望更新为最终 11 页顺序**：PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT→PROJECTS→PRESETS→PLAY。

- [ ] **Step 5: 门禁**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0        # 含 UI 段
bash scripts/inject/nav.sh /dev/ttyACM0          # 新顺序含 FILE
python3 scripts/device-selftest.py /dev/ttyACM0
```
截屏归档：`task-10-projects-empty.png`、`-projects-used.png`、`-presets.png`、`-save-playing.png`；人眼检查布局/可读性/无重叠（AGENTS §5.4）。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_ui scripts/inject docs/design/008-param-pages.md docs/dev/verify/4-pb-2026-11-21
git commit -m "feat(components/sloop_ui): PROJECTS 与 PRESETS 页面（FILE 组）"
git push origin feat/phase4b
```

---

### Task 11: FM6 页 bank 存取行

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_params.c`
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`（TAP kind `SLOOP_TAP_BANK_SLOT`/`SLOOP_TAP_BANK_STORE`/`SLOOP_TAP_BANK_ERASE`）
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 5 的 bank API；Task 8 的 `CMD_BANK_PUT/ERASE`
- Produces: FM6 页底部 `BANK - / B12 / + | SAVE | ERASE` 行

- [ ] **Step 1: 失败断言**

`run_store`：`UI fm6` → `SCR` 探针（`BANK` 行存在）→ `TAP BANK +` 三次 → 状态行显示槽号 → `TAP BANK SAVE` → `STORE BANK GET <槽>` 与 `STORE BANK LIVE` 一致；`TAP BANK ERASE` → GET empty；播放中 SAVE → `STOP BEFORE SAVE` 且 bank 不变。运行确认 FAIL。

- [ ] **Step 2: 实现**

FM6 页（`page_params.c` 的 FM6 布局表）底部加一行控件（y 由现有行距留出；`EDIT` 页底部原无控件，确认不与旋钮环重叠）；槽号局部态 `s_bank_slot`（0..26），`+/-` 改槽，SAVE/ERASE 发 CMD；`update()` 显示 `B<slot+1>` 与 `store_status`。TAP kind 与 hit-test 矩形表同步。

- [ ] **Step 3: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
bash scripts/inject/params.sh /dev/ttyACM0        # FM6 页既有断言不回退
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_ui scripts/inject docs/dev/verify/4-pb-2026-11-21
git commit -m "feat(components/sloop_ui): FM6 页 bank 存取行"
git push origin feat/phase4b
```

---

### Task 12: 收口（soak 新判据、全门禁、证据、合 dev）

**Files:**
- Modify: `scripts/soak.py`（`--save-window` 参数：窗口内允许 xrun 增量，窗口外必须 0）
- Modify: `docs/dev/verification-log.md`
- Modify: `docs/design/002-port-roadmap.md`（P4-B ✅）
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（P4-B 状态）
- Create: `docs/dev/verify/4-pb-2026-11-21/readme.md` + `SHA256SUMS`

**Interfaces:**
- Consumes: 全部前置 Task
- Produces: P4-B 证据包；`dev` 上的合并提交

- [ ] **Step 1: soak 判据**

`soak.py` 加 `--save-window N`：在窗口内执行一次 `STORE PROJ SAVE 0` + `LOAD 0`，窗口内 `xrun` 增量 ≤16 且窗口后 `streaming=1`；窗口外仍要求 `xrun=0`。默认行为不变（无参数 = 旧判据）。

- [ ] **Step 2: 全门禁**

```bash
scripts/build.sh --clean && scripts/build.sh && scripts/build.sh
scripts/host-test.sh
scripts/state-test.sh
scripts/knob-test.sh
python3 scripts/inject/inject.py --scenario nav --port /dev/ttyACM0
bash scripts/inject/params.sh /dev/ttyACM0
bash scripts/inject/tracks.sh /dev/ttyACM0
bash scripts/inject/transport.sh /dev/ttyACM0
bash scripts/inject/step_edit.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
python3 scripts/soak.py /dev/ttyACM0 --duration 300 --save-window 60
python3 scripts/screenshot.py --selftest
```
全部退出码 0；soak 报告 `VERDICT: PASS`。

- [ ] **Step 3: 证据晋升**

把 `firmware/build/verify/` 的本轮截屏/日志复制到 `docs/dev/verify/4-pb-2026-11-21/`（含失败件加 `-fail`），写 `readme.md`（每个门禁的命令、结论、证据文件），生成 `SHA256SUMS` 并在 `verification-log.md` 登记（阶段/门禁/日期/提交号/证据 SHA256/结论）。

- [ ] **Step 4: 文档状态**

roadmap 阶段 4 状态行：P4-B ✅（日期、提交号、关键结论：往返 hash、torn-write 6/6、旧格式 3 种、FILE 组预算、组作用域）；phase-4 spec P4-B 行改 ✅；`docs/design/002-port-roadmap.md` §6 状态列同步。

- [ ] **Step 5: 提交、review、合 dev**

```bash
git add scripts/soak.py docs/
git commit -m "docs(dev/verify): P4-B 收口门禁与证据"
git push origin feat/phase4b
```
终审 reviewer 对整个分支做一次 review；修复波后：
```bash
git checkout dev && git pull --ff-only origin dev
git merge --no-ff feat/phase4b -m "merge: P4-B 存储与预置（工程 A-D / 用户预置 / FM6 银行 / 分组导航）"
scripts/build.sh && scripts/build.sh && scripts/host-test.sh
git push origin dev
```

---

## 完成判据对照（spec §6）

| spec 条目 | 覆盖 Task | 门禁 |
|---|---|---|
| 存储后端（原始分区 + mmap + A/B） | 1,2,3 | `STORE STATS`/`RAWTEST`、启动日志 |
| 工程 A–D 存读（FUN6） | 3 | `store.sh` 往返 hash + `STATE SNAP` |
| 旧格式 FUN5/4/1 兼容 | 7 | fixtures 上传 + `PROJ FIELDS` |
| 用户预置 32 槽 | 4,10 | `store.sh` 预置段 + PRESETS 页截屏 |
| FM6 银行 27 槽 | 5,11 | `BANK GET/LIVE` 字节一致 + 断电后 used |
| torn-write 不破坏旧副本 | 6 | `STORE TEST PASS 6/6` |
| 导航/容量（DI-17） | 1,9 | ADR-005/009、`UI MEM` 分组、`nav.sh` |
| 状态层与写入路径 | 8 | `state-test` 存储用例 |
| 文档/证据/合 dev | 12 | verification-log + roadmap + merge |

## 遗留与后续（不在本计划）

- autosave / settings 对象 / sections / arranger（P4-C、P4-E）；
- FM6 patch 编辑面、DSYN、采样槽上传（P4-D）；
- 物理触摸按钮、听感（保存/加载后音色变化）、视觉复检的人测项（AGENTS §5.4 外部门禁）；
- 本计划不处理的历史 DI：DI-2/4/6/7/10/12/13/15/16（见 `docs/deferred-issues.md`）。
