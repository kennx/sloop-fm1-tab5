# P4-B 存储与预置 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按「最大程度移植 SLOOP FM-1」做 P4-B——工程 A–D（RAM+flash 双层）、自动存档与上电恢复、用户预置 32 槽、FM6 银行 27 槽、工厂预置真正换声音，并把导航改成「分组 + 组作用域页面」以解开 9 页池天花板。

**Architecture:** 引擎单 TU 内 include 上游 `storage.c`（A/B+CRC 原样）、`project.c`（`PROJ_HOST` 格式层 + capture/apply，autosave 与 `proj_foreign_parts` 按原语义移植）、`upreset.c`、`fm6_bank.c`，用薄 shim、PSRAM 宏重定向和上游 `ui.c` 的预置应用逻辑接上 Tab5；底层是自定义原始 flash 分区 `sloopstore` + `esp_partition_mmap`（`st_xip` 零成本）。UI 侧两行页签（组/子页）复用现有 72px 页签条，离开组即销毁该组页面；FILE 组新增 PROJECTS/PRESETS 两页，FM6 页加 bank 存取行。

**Tech Stack:** ESP-IDF 5.5.5 / ESP32-P4、esp_partition+spi_flash、LVGL 9.6、FreeRTOS、上游 SLOOP 2.8.5（`third_party/sloop-fm1`，只读）、Python 验证脚本（pyserial）。

**Spec:** `docs/superpowers/specs/2026-11-21-p4b-storage-presets-design.md`（本计划的全部决策来源；实施者必须同时读）

## Global Constraints

- 构建：`scripts/build.sh` 连续两遍退出码 0，第二遍**零新增**警告/错误（AGENTS §5.5；kconfig 首 pass 的 CMake 豁免仅限已知那条）。
- 烧录：`scripts/flash.sh /dev/ttyACM0` 输出含 `Hash of data verified`；启动日志无 fatal/abort/backtrace/task_wdt panic。
- 引擎回归：`scripts/host-test.sh` 全绿；设备 `scripts/device-selftest.py /dev/ttyACM0` 全量 182/182（自动停/复流）。
- 存储 TU：`third_party/sloop-fm1/**` **零修改**（manifest 233/233）；所有适配只进 `firmware/components/sloop_core/src/store_tab5.c`。
- 内部 RAM：idle `internal_free ≥ 61,440B`；`proj_slot`/`proj_v3_tmp`/`proj_tmp` 实体不得进 DRAM（PSRAM，约 22KB）；`main_scratch`（3,840B）必须内部 SRAM。
- LVGL 池：跨组切换的过渡峰值不变量 `base + max_group Σest + max_single_page_est + 4KB ≤ 63,120`（当前 `7,016 + 27,648 + 23,040 + 4,096 = 61,800`，余 1,320B）；每页构建前仍检查 `free ≥ est`；不得扩大 `CONFIG_LV_MEM_SIZE`。
- 并发：所有存储写/载经引擎锁与 `s_store_lock`；播放中（`song.playing || transport_req`）拒绝写；console `STORE` 是诊断执行器（与 UI 的 CMD 共用同一 API）。
- 自动存档：仅停止 + 无输入 ≥2.5s + 距上次写 ≥20s + 无发声 + hash 变化时写 `OBJ_AUTOSAVE`；测试期用 `STORE AUTO OFF` 关闭。
- 提交：AGENTS §7 格式（`<type>(<模块路径>): 中文摘要` + `[Motivation]/[Change_Set]/[Impact_AST]/[Infra_State]`；`fix` 另加 `[Root_Cause]/[Blast_Radius]`）。每个 Task 结束后 review clean → 立即 push `origin feat/phase4b`。
- 工作流：子代理驱动，**不用 worktree**；每个 Task 一个新的 implementer + 一个 reviewer；reviewer 通过后才 push。

## Review Focus

1. **存储未就绪**（分区丢失/mmap 失败/初始化未跑）：任何 `STORE` 命令返回 `STORE ERR no_storage`，UI 显示 `NO STORAGE`，引擎/音频照常，设备不崩溃。（Task 2/3）
2. **空槽 / 越界 / 鼓轨**：LOAD/ERASE 空槽返回 `EMPTY` 且不改引擎；slot ≥ 4/32/27 被拒；鼓轨存预置返回 `DRUM TRACK`。（Task 3/4/7/10）
3. **播放中保存 / 自动存档竞态**：返回 `ERR playing`、存储不变；播放中 autosave 不写；tick 与手动命令不互踩。（Task 3/4/6/7）
4. **掉电窗口**：擦除失败 / payload 半写 / 头写失败时旧副本仍被选中并完整加载。（Task 8）
5. **损坏或截断的旧格式 payload**（FUN1–FUN5）：`proj_import` 拒绝，当前工程与其它槽不受影响，该槽读为空。（Task 9）
6. **换预置/换引擎的 p[] 写入口径**：与上游 `apply_preset_to` 逐项一致（env/fx/arp/extras，保留 mix/pattern 参数）。（Task 5）
7. **切组销毁页面时的在途输入**（同拍 tab+key / PANIC）：不得访问已销毁对象；`nav.sh` 四用例在新语义下通过。（Task 11）

---

### Task 0: 基线

**Files:**
- Create: `docs/dev/verify/4-pb-2026-11-21/task-0-baseline.txt`

**Interfaces:**
- Consumes: 当前 `dev`
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
把尾部输出（构建警告数、`ALL HOST TESTS PASSED`、`SELFTEST PASS 182/182`、截图 SHA256）写进 `task-0-baseline.txt`。

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
- Modify: `docs/design/005-ui-architecture.md`（「导航信息架构」节）
- Modify: `docs/design/008-param-pages.md`（§3/§5）
- Modify: `docs/deferred-issues.md`（**删除 DI-17 整条**）
- Modify: `docs/design/002-port-roadmap.md`（阶段 4 状态行）
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（§4.3 指向 ADR-009）

**Interfaces:**
- Consumes: P4-B spec §3/§4/§5/§8
- Produces: ADR-005 的导航/容量决策；ADR-009 的存储后端决策

- [ ] **Step 1: 写 ADR-009**

按仓库 ADR 结构（背景/选项/决策/后果）写 `docs/design/009-storage-backend.md`：原始分区 vs FATFS 文件 vs NVS；flash map 与分区表；mmap/XIP；擦写与 codec mute；xrun 判据；自动存档阈值；`OBJ_SETTINGS` 承载设置（P4-E）、NVS 不承载 SLOOP 状态；console 诊断执行器例外；PSRAM 宏重定向；只读 vendored。

- [ ] **Step 2: 更新 ADR-005**

写入 spec §3.3：5 组（PLAY/TRACKS/EDIT/SEQ/FILE）、两行页签复用 72px、组作用域销毁、`TAB` 扁平轮转新顺序（PLAY → TRACKS → GLOBAL → FX → ANALOG → FM6 → SAMPLE → STEP → TRANSPORT → PROJECTS → PRESETS）、跨组切换的过渡峰值不变量 `base + max_group Σest + max_single_page_est + 4KB ≤ 63,120`（当前 61,800，余 1,320B）。保留旧节历史数据（9 页实测）。

- [ ] **Step 3: 关 DI-17**

删除 `docs/deferred-issues.md` 的 DI-17 整条；`grep -rn 'DI-17' docs/`（排除 `verification-log`、`dev/verify/` 与本执行计划 `superpowers/plans/`）应为空。

- [ ] **Step 4: 同步 roadmap / 阶段 4 spec / ADR-008**

roadmap 阶段 4 状态行加「P4-B 开工（`feat/phase4b`），导航/容量决策见 ADR-005/009，原 9 页池容量天花板条目关闭」；phase-4 spec §4.3 改为一句「存储后端由 ADR-009 定义（原始分区 + mmap，取代本文原 FATFS 文件措辞）」；ADR-008 §3/§5 加「组作用域生命周期：预算按组核算，`UI MEM` 增加分组 Σ」。

- [ ] **Step 5: 检查并提交**

```bash
grep -rn 'DI-17' docs/ | grep -v verification-log | grep -v 'dev/verify/' | grep -v 'superpowers/plans/'
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
- Modify: `firmware/main/console.c`（`STORE STATS` / `STORE RAWTEST`）

**Interfaces:**
- Consumes: 无
- Produces（§4.1 逐字）:
  `esp_err_t sloop_hal_storage_init(void); bool sloop_hal_storage_ready(void);`
  `const uint8_t *sloop_hal_storage_map(void); uint32_t sloop_hal_storage_base(void); uint32_t sloop_hal_storage_span(void);`
  `int sloop_hal_storage_read/erase/prog(...);`
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
头注释写死：`sloopstore` 承载上游 flash map `0x97000..0xFF000`（内部偏移 = flash 偏移 − 0x97000），不得移动；`storage`（FAT）留给非 SLOOP 资产。

- [ ] **Step 2: 写 HAL**

`storage_flash.c`：`init` 幂等；`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x99, "sloopstore")` → `esp_partition_mmap(part, 0, span, ESP_PARTITION_MMAP_DATA, &map, &handle)`；`base=0x97000`、`span=0x68000`；`read` = 越界检查后 `memcpy(map + off - base)`；`erase` = 4KB 对齐检查 → `codec mute`（`sloop_hal_audio_codec()` 为 NULL 时跳过）→ `esp_partition_erase_range(part, off-base, 0x1000)` → `unmute`；`prog` = 越界检查 → `esp_partition_write`；失败返回 -1/-8。故障注入是共用原子计数器：命中时 `read/erase` 返回 -1、`prog` 在 `half` 时只写 `n/2` 再返回 -1，否则不写返回 -1。

- [ ] **Step 3: console 最小命令**

`STORE STATS` 打印 `STORE STATS ready=1 base=0x97000 span=0x68000 map=0x… objects=0 faults=0`（未 ready 打 `ready=0`）；`STORE RAWTEST` 对**空闲逻辑扇区 `0xFB000`**（`sloopstore` 内偏移 `0x64000`；参数是逻辑 flash-map 偏移，不是芯片绝对地址）erase → 写 0x5A 模式 → 校验 → 再 erase，打印 `STORE RAWTEST PASS/FAIL`（破坏性，`help` 注明）。

- [ ] **Step 4: 构建 + 烧录**

```bash
scripts/build.sh --clean && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
```
确认启动日志无分区查找错误。

- [ ] **Step 5: 验证**

```bash
python3 scripts/device-selftest.py /dev/ttyACM0            # 182/182
bash scripts/inject/nav.sh /dev/ttyACM0                     # 旧 9 页语义仍绿
python3 scripts/screenshot.py /dev/ttyACM0 docs/dev/verify/4-pb-2026-11-21/task-2-play.png
cmp docs/dev/verify/4-pb-2026-11-21/task-0-play.png docs/dev/verify/4-pb-2026-11-21/task-2-play.png
```
`cmp` 应无输出；`STORE STATS/RAWTEST` 输出用 `inject.py` 的 `Session.cmd` 抓取归档。

- [ ] **Step 6: 提交**

```bash
git add firmware/partitions.csv firmware/components/sloop_hal_tab5 firmware/main/console.c docs/dev/verify/4-pb-2026-11-21/task-2-*
git commit -m "feat(components/sloop_hal_tab5): 新增原始分区存储后端与 mmap 读窗口"
git push origin feat/phase4b
```

---

### Task 3: 存储核心 + 工程 A–D（RAM 槽 + flash 双层）

**Files:**
- Create: `firmware/components/sloop_core/include/sloop_store.h`
- Create: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/components/sloop_core/src/selftest.c`（末尾 include；`sloop_core_init` 调 `sloop_store_init`）
- Modify: `firmware/components/sloop_core/CMakeLists.txt`（`PRIV_REQUIRES` 加 `esp_partition`）
- Modify: `firmware/main/console.c`（`STORE PROJ …` / `STORE LIVEHASH`）
- Modify: `scripts/inject/inject.py`（新 scenario `store`）+ Create `scripts/inject/store.sh`

**Interfaces:**
- Consumes: Task 2 的 HAL（§4.1）
- Produces（§4.2 本期部分）:
  `esp_err_t sloop_store_init(void); bool sloop_store_ready(void); void sloop_store_stats(sloop_store_stats_t *out);`
  `bool sloop_store_project_used(uint32_t slot); uint32_t sloop_store_project_hash(uint32_t slot); uint32_t sloop_store_live_hash(void);`
  `esp_err_t sloop_store_project_save/load/erase(uint32_t slot);`

- [ ] **Step 1: 写失败测试（设备注入）**

`SCENARIOS["store"] = run_store` + `store.sh`（与 `nav.sh` 同构）。断言：
```python
# STORE PROJ LIST -> "PROJ 0 EMPTY" …（全新分区）
# 改 BPM -> STORE LIVEHASH = h0
# STORE PROJ SAVE 0 -> "PROJ 0 SAVED"
# STORE PROJ HASH 0 == h0
# 改 BPM -> LIVEHASH = h1 != h0
# STORE PROJ LOAD 0 -> LIVEHASH == h0；STATE SNAP 的 bpm 与保存前一致
# STORE PROJ ERASE 0 -> LIST 显示 EMPTY；LIVEHASH 不变
# 保存后重新上电（flash.sh）-> LIST 仍 USED、LOAD 0 恢复 h0（RAM 槽的开机 fetch）
```
失败抛 `InjectError`。

- [ ] **Step 2: 运行确认失败**

```bash
scripts/build.sh && scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
```
Expected: FAIL（`STORE` 未知命令 / 无 `PROJ` 输出）。

- [ ] **Step 3: 写 `sloop_store.h`**

按 §4.2 注释风格声明本期 API + `sloop_store_stats_t`（`ready/objects_ok/faults_armed/faults_taken/autosave_writes`）。

- [ ] **Step 4: 写 `store_tab5.c` 骨架（shim + hooks + 上游 include）**

顺序严格如下：
1. `#include "scratch.h"`（`main_scratch`，内部 SRAM）。
2. PSRAM 重定向宏：`#define proj_v3_tmp (*s_v3_ps)`、`#define proj_slot (*s_slot_ps)`（声明退化为指针）。
3. shim：`flash_ok`、`ST_XIP_PTR(off)`、`FM6_BANK_XIP(copy)`、`trk_def_engine`、`user_of`、`param_kept`（抄上游 `ui.c:24/592`）、`ui_message/ui_say`（写 `s_ui_msg` + printf）、`ui.force`（4 字节占位结构）、`up_gen`、`sync_reload`。
4. hooks：`st_read/st_erase/st_prog` 转 `sloop_hal_storage_*`。
5. `#include "storage.c"`。
6. `#define PROJ_HOST 1` → `#include "project.c"` → `#undef PROJ_HOST`。
7. PSRAM 暂存与 up_stage：
   ```c
   typedef union { project_v5_t v5; uint8_t raw[4096]; } proj_stage_t;
   static proj_stage_t *s_stage;  #define proj_tmp (*s_stage)
   static void *up_stage(void) { return &proj_tmp; }
   ```
   静态断言 `sizeof(proj_tmp) >= sizeof(project_t)`。

- [ ] **Step 5: 实现工程 API**

- `sloop_store_init`：PSRAM 分配 `s_v3_ps`（`sizeof *s_v3_ps`）、`s_slot_ps`（4×project_t = 15,264B）、`s_stage`（4,096B），任一失败 `ESP_ERR_NO_MEM` 且 `ready=false`；HAL init；`ready = sloop_hal_storage_ready()` + 分配成功；随后 **boot fetch**：4 个槽 `st_load → s_stage->raw → proj_import(&proj_slot[i])`，失败则 `proj_slot[i].magic=0`。
- `project_save(slot)`：越界/未 ready/播放中 → 错误；引擎锁内 `proj_capture(&proj_slot[slot])`；释放锁后 `st_save(OBJ_PROJECT0+slot, &proj_slot[slot], sizeof(project_t))`，失败 `ESP_ERR_NOT_FINISHED`（RAM 槽保持新值，与上游一致）。
- `project_load(slot)`：播放中拒绝；RAM 槽 `proj_ok` 否则 `st_load+proj_import` 回填；仍无效 → `ESP_ERR_NOT_FOUND`；引擎锁内 `proj_apply(&proj_slot[slot], 1)` + `song.sel` 钳位。
- `project_erase(slot)`：`st_save(..., NULL, 0)` + `memset(&proj_slot[slot])`。
- `project_used/hash`：直接读 RAM 槽（`proj_ok` / `sum`）；`live_hash`：引擎锁内 `proj_capture(&tmp)` 返回 `sum`。

- [ ] **Step 6: 接进启动与控制台**

`selftest.c` 末尾 `#include "store_tab5.c"`；`sloop_core_init` **在 `selftest_snapshot_init()` 之后**调 `sloop_store_init()`（失败只 WARN）；`console.c` 注册 `STORE`：`STATS|PROJ LIST|PROJ SAVE n|PROJ LOAD n|PROJ ERASE n|PROJ HASH n|LIVEHASH`，错误统一 `STORE ERR <name>`（`no_storage/playing/empty/invalid/save`）。

- [ ] **Step 7: 构建 + 设备门禁**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0        # PASS（工程往返 + 重启 fetch）
python3 scripts/device-selftest.py /dev/ttyACM0  # 182/182
```
内存：`ENG MEM` 的 `internal_free ≥ 61440`；`grep -E 'proj_slot|proj_v3_tmp|proj_tmp' firmware/build/sloop_fm1_tab5.map` 无 DRAM 大实体。

- [ ] **Step 8: 提交**

```bash
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "feat(components/sloop_core): 接上游 storage/project 格式层，工程 A-D 存读（RAM+flash）"
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
- Consumes: Task 3 的 storage 符号、`proj_tmp`、`up_stage`
- Produces:
  `uint32_t sloop_store_preset_count(void); bool sloop_store_preset_used(uint32_t slot); bool sloop_store_preset_name(uint32_t slot, char out[13]); uint8_t sloop_store_preset_engine(uint32_t slot);`
  `esp_err_t sloop_store_preset_store(uint32_t slot); esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track); esp_err_t sloop_store_preset_erase(uint32_t slot);`

- [ ] **Step 1: 追加失败断言**

选中轨 ANALOG → 改 8 参数 → 记录 `STATE SNAP` p[] → `STORE PRESET SAVE 0` → 改参数 → `STORE PRESET LOAD 0 0` → p[] 逐项相等；`STORE PRESET LIST` 含 `U01` + 引擎名；`ERASE 0` 后为空；鼓轨（`STATE SET_TRACK 3`）`STORE PRESET SAVE 1` → `STORE ERR drum`；重新上电后 `LIST` 仍 USED（验证 `up_boot`）。运行确认 FAIL。

- [ ] **Step 2: include 上游并实现**

`store_tab5.c` 在 `project.c` 之后：`#define FELUCCA_FLASH 1` → `#include "upreset.c"` → 定义 `arp_is_sounds`（照抄 `ui.c:607`，用 `user_of`、`up_arp_of`）。`sloop_store_init` 末尾加 `up_boot()`（两条银行扫描）。API：`preset_store` = 引擎锁内校验非鼓轨 → `up_store(slot, 0)`；`preset_load` = 引擎锁内 `up_load_to(&trk[track], slot)`；`preset_erase` = `up_put(slot, NULL)`；列表直读 `up_rec/up_valid/up_name`。错误映射：`up_put` 2→`ESP_ERR_NOT_FINISHED`、3→`ESP_ERR_INVALID_STATE`；空槽→`ESP_ERR_NOT_FOUND`；鼓轨→`ESP_ERR_INVALID_ARG` + `ui_message("DRUM TRACK: NO SOUND")`。

- [ ] **Step 3: console**

`STORE PRESET LIST|SAVE n|LOAD n [track]|ERASE n`；行格式 `PRESET <k> USED <ENGINE> <NAME>` / `PRESET <k> EMPTY`；`LOAD` 默认 track=`song.sel`。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "feat(components/sloop_core): 用户预置 32 槽存取（upreset.c 接入）"
git push origin feat/phase4b
```

---

### Task 5: 工厂预置应用与换音语义（修复 `sloop_core_track`）

**Files:**
- Modify: `firmware/components/sloop_core/src/store_tab5.c`（移植 `apply_preset_to/set_engine_of/TRK_DEF`）
- Modify: `firmware/components/sloop_core/src/selftest.c`（`sloop_core_track` 改用新实现）
- Modify: `firmware/components/sloop_core/include/sloop_core.h`（新增 `sloop_core_apply_preset/set_engine`；`sloop_core_track` 注释改为「换引擎+预置并载入参数」）
- Modify: `firmware/main/console.c`（`ENG TRACK` 帮助文本/回显；新增 `ENG PRESET <track> <engine> <preset>`）
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: `ENGINES[].presets/edit`、`param_kept/user_of/arp_is_sounds`、`preset_extras/preset_trim/locks_restore/sound_change/steps_clear`
- Produces: `void sloop_core_apply_preset(uint32_t track, uint32_t preset); void sloop_core_set_engine(uint32_t track, uint32_t engine);`（引擎锁内、`s_frozen` 丢弃）

- [ ] **Step 1: 失败断言**

`run_store` 增加（`ENG PRESET` 输出 `P<id>=<v>` 71 项，脚本解析成向量）：
```python
# baseline = STATE SNAP 的 p[]
# expected = ENG PRESET 0 2 1（同一个"当前 p[] + 引擎 2 + 预置 1"的写入口径）
# ENG TRACK 0 2 1 -> STATE SNAP 的 p[] 逐项 == expected（71/71）
# mix/pattern 保留：先 PARAM <P_LEVEL> 90 -> ENG TRACK 0 2 1 -> P_LEVEL 仍 90（expected 同）
# 引擎切换：ENG TRACK 0 1 0 -> p[P_E0..P_E7] == ENG DESC engine 1 <id> 的 def，p[P_ATK] == ENG PRESET 0 1 0 的 ATK
# 播放一个音（TRIG）后换预置 -> 无残留挂音（panic 路径），STATE SNAP 的 playing 不变
```
运行确认 FAIL（`ENG PRESET` 未实现 / 换预置不写 p[]）。

- [ ] **Step 2: 实现**

照抄上游 `ui.c:581-691`，但把「纯写值」部分抽成一份实现，供应用与预览共用：
```c
/* v = base->p[] 拷贝后，按引擎 e 的预置 pi 写入；keep_arp 同上游 */
static void preset_values(int16_t *v, const track_t *base, uint32_t e, uint32_t pi, int keep_arp, int kbd_hold);
/* 上游 apply_preset_to 的副作用（sound_change/panic/sync_reload/fm6_track_loaded/user=0/preset=pi） */
static void preset_apply(track_t *t, uint32_t pi);
```
`preset_values` 逐行对应上游：`param_kept` 保留项、`arp_param`、`P_AHOLD` 规则、`env[4]`、`edit[8]`、`P_ED_FLT`、`P_ED_FX=preset_trim`、`P_VOICE`、`fx[4]`、`arp[4]`、`preset_extras`。`TRK_DEF`/`trk_def_engine`/`set_engine_of` 一并移植（`set_engine_of` 用引擎锁替代 `fm1_irq_off/on`，`t == TSEL` 改 `t == &trk[song.sel]`）。
`sloop_core_track(k,e,p)`：引擎锁内，若 `e != t->eng_req` → `set_engine_of(t,e)`（preset 置 0）；再若 `p != t->preset` → `preset_apply(t,p)`。越界/冻结照旧丢弃。
`sloop_core_apply_preset/set_engine`：对指定轨调 `preset_apply`/`set_engine_of`。
`sloop_core_preset_preview`：引擎锁内以 `trk[track].p[]` 为 base 调 `preset_values` 输出（不改引擎、不触发 panic）。
`project_load` 增加上游 `project_apply` 两步：`p->t[k].preset == 0xFF` → `preset_apply(&trk[k], TRK_DEF[k][1])` + `steps_clear(&trk[k])`；随后 `proj_foreign_parts(p)`。`autosave_resume` 同样调 `proj_foreign_parts`。

- [ ] **Step 3: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
bash scripts/inject/tracks.sh /dev/ttyACM0        # TRACKS 页既有断言
python3 scripts/device-selftest.py /dev/ttyACM0   # 换音路径不碰 DSP，182 不变
git add firmware scripts/inject
git commit -m "fix(components/sloop_core): 换引擎/预置载入参数（apply_preset_to 移植）"
git push origin feat/phase4b
```

> 若该 Task 单独 review 时发现 `apply_preset_to` 的 `preset_extras` 需要 `P_*` 之外的引擎状态（`fm6_track_loaded`），确认 `eng_fm6.c` 已在本 TU 内可用；不得为通过测试而简化写入口径。

---

### Task 6: 自动存档与上电恢复

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/components/sloop_core/src/selftest.c`（init 顺序：snapshot → store → resume → 启 tick 任务）
- Modify: `firmware/main/console.c`（`STORE AUTO ON|OFF`，`STORE STATS` 加 autosave 字段）
- Modify: `firmware/components/sloop_ui/src/input.c` + `firmware/components/sloop_state/src/cmd.c`（`sloop_store_note_activity` 钩子）
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 3 的 `proj_capture/proj_apply/proj_import/st_save/st_load`
- Produces:
  `void sloop_store_note_activity(uint32_t now_ms); void sloop_store_autosave_tick(uint32_t now_ms);`
  `esp_err_t sloop_store_autosave_resume(void);`（`STORE STATS` 增 `autosave_hash/autosave_writes/autosave_on`）

- [ ] **Step 1: 失败断言**

`run_store` 的既有段开头加 `STORE AUTO OFF`（避免 autosave 干扰），本段先 `STORE AUTO ON`：改 BPM → 停止状态等 3.5s → `STORE STATS` 的 `autosave_writes` 增 1 且 `autosave_hash == LIVEHASH`；再改 BPM → 等 3s 内不应再写（gap 20s）→ 再等 20s 才写；`CMD_PLAY`（KEY space）播放中改参数 → 等 25s → 计数不变；`STORE AUTO OFF` 后等待不写。运行确认 FAIL。

- [ ] **Step 2: 实现**

- `audio_quiet()`：照抄上游（`trk[].v[].active` + `drums.v[].active`），本 TU 可见。
- `autosave_tick`：`flash_ok && auto_on && !song.playing && !transport_req && !s_frozen && !s_store_busy && now-autosave_ms>=20000 && now-last_input>=2500 && now-autosave_checked>=1000` → `proj_capture(&cas)`；hash 同上次则跳过；`audio_quiet()` 为假则跳过；`st_save(OBJ_AUTOSAVE, &cas, sizeof)` 成功后更新 hash/`autosave_writes`。
- `autosave_resume`：`st_load(OBJ_AUTOSAVE)` → `proj_import` → `proj_apply(all=1)` → `song.sel` 钳位 → `proj_foreign_parts`（照抄 `project.c:639-647`，用 Task 5 的 `set_engine_of/apply_preset_to`）→ `for n<NPART: trk[n].engine = trk[n].eng_req`。
- 1s 周期任务（`sloop_store_init` 创建，`xTaskCreate`，优先级 1、栈 3072、内部 RAM）：循环 `vTaskDelay(pdMS_TO_TICKS(1000))` 调 tick；`s_store_lock` 内执行。
- `note_activity`：`sloop_ui_input_key`（物理/注入键盘）与 `sloop_ui_input_tap` 命中、`state_apply` 成功路径各调一次；只写 `s_last_input_ms` 原子变量。
- `STORE AUTO`：诊断开关（默认 ON），测试脚本用。

- [ ] **Step 3: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
python3 scripts/soak.py /dev/ttyACM0 --duration 120     # 无存储动作窗口：xrun=0、堆不降
git add firmware scripts/inject
git commit -m "feat(components/sloop_core): 自动存档与上电恢复（上游 autosave 语义）"
git push origin feat/phase4b
```

---

### Task 7: FM6 银行 27 槽

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/main/console.c`
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 3 的 storage 符号与 `proj_tmp`；`eng_fm6.c` 的 `fm6_pack/fm6_patch/fm6_bank_read`
- Produces:
  `bool sloop_store_bank_used(uint32_t slot); bool sloop_store_bank_get(uint32_t slot, uint8_t out[128]); esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed); esp_err_t sloop_store_bank_erase(uint32_t slot); void sloop_store_bank_current(uint8_t *packed);`

- [ ] **Step 1: 失败断言**

`ENG TRACK 1 9 0`（FM6 工厂 patch）→ `STORE BANK LIVE` 得 hex L → `STORE BANK PUT 3` → `STORE BANK GET 3` == L；`BANK LIST` 含 `B3 USED`；重新上电后仍 USED；`BANK ERASE 3` 后 GET 返回 `STORE ERR empty`；`ENG TRACK 1 9 11`（PTCH=11 → bank 槽 3）+ `ENG TRIG` 无错误；无 patch 槽 GET 为空。运行确认 FAIL。

- [ ] **Step 2: include 上游并实现**

`store_tab5.c` 在 `upreset.c` 之后定义 `FM6_BANK_XIP(copy)`（= map + (0xE5000 + copy*4096) − base），`#include "fm6_bank.c"`；`sloop_store_init` 末尾加 `fm6_bank_boot()`。API 包装 `fm6_bank_put/fm6_bank_get/has`；`bank_current` 引擎锁内 `fm6_pack(fm6_patch[track], out)`（同 TU 可见）。播放中拒绝；`fm6_bank_put` 1→`ESP_ERR_INVALID_ARG`、2→`ESP_ERR_NOT_FINISHED`。

- [ ] **Step 3: console**

`STORE BANK LIST|PUT n|GET n|LIVE|ERASE n`；`PUT` 取选中轨 live patch；`GET/LIVE` 打印 128B hex。

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

### Task 8: 设备侧 torn-write 故障注入套件

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/main/console.c`（`STORE TEST` / `STORE FAULT`）
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: `sloop_hal_storage_fault_*`；storage 的 `st_save/st_load/st_sector/st_current`
- Produces: `esp_err_t sloop_store_test(sloop_store_test_t *out)`（`{cases,passed,fail_case}`）

- [ ] **Step 1: 失败测试**

`run_store` 追加 `STORE TEST` → `STORE TEST PASS 6/6`。对象用 `OBJ_SETTINGS`（测试前 `st_load` 快照、结束 `st_save` 恢复；`STORE AUTO OFF` 防干扰）。6 场景：
1. 写 v1、写 v2 → 读回 v2（seq 大者）；
2. 擦除失败（`FAULT 1`）→ 写 v3 失败 → 读回 v2；
3. payload 半写（`FAULT half`）→ 写 v3 失败 → 读回 v2；
4. header 写失败 → 写 v3 失败 → 读回 v2；
5. 写后破坏 payload → `st_load` 返回错误而非坏数据；
6. `st_save(..., 0)` → 对象为空。
失败打印 `STORE TEST FAIL <name>`。

- [ ] **Step 2: 运行确认失败** → `STORE TEST` 不存在。

- [ ] **Step 3: 实现**（六个场景直接用 `st_save/st_load/st_sector`，payload 为 64B 递增模式；全程 `s_store_lock`；结束 `fault_clear` 并恢复快照）。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware/components/sloop_core firmware/main/console.c scripts/inject
git commit -m "test(components/sloop_core): 设备侧 A/B torn-write 故障注入套件"
git push origin feat/phase4b
```

---

### Task 9: 旧格式兼容门禁（FUN5/FUN4/FUN1）与 RAW 上传通道

**Files:**
- Modify: `firmware/main/console.c`（`STORE RAW …`、`STORE PROJ FIELDS n`）
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Create: `scripts/gen_project_fixtures.py`
- Create: `scripts/store_raw.py`
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 3 的 `project_load/proj_import`
- Produces: `scripts/gen_project_fixtures.py --out DIR`（写 `fun5/fun4/fun1.bin` + `expected.json`）；`scripts/store_raw.py PORT FILE --obj project --slot 3`

- [ ] **Step 1: 生成器 + 上传器**

`gen_project_fixtures.py` 照上游 `tests/project_test.c` 的构造器生成三种 payload 与 `expected.json`（`FIELDS` 会打印的字段：sel、bpm、4 轨 engine/preset、p[P_LEVEL]/p[P_ATK]/…、首 8 步 n/note、全局代表值）。`store_raw.py`：`STORE RAW BEGIN <len>` → 每行 ≤480 hex → `STORE RAW END project 3`，逐行等回显。

- [ ] **Step 2: 失败测试**

`run_store`：三个 fixture 依次上传 → `STORE PROJ LOAD 3` → `STORE PROJ FIELDS 3` 与 `expected.json` 逐项比对 → 截断 fixture（fun5[:100]）→ `LOAD 3` 返回 `STORE ERR invalid` 且 `LIVEHASH` 不变。运行确认 FAIL。

- [ ] **Step 3: 实现**

`STORE RAW BEGIN` 记长度、后续 hex 解码进 `s_stage->raw`（容量 4096，超长 `ERR size`）；`END` 按 `obj` 映射（`project`→`OBJ_PROJECT0+slot`）`st_save` 原样写；`ABORT` 复位。`STORE PROJ FIELDS n`：`st_load`+`proj_import` 后打印固定顺序 `key=value`，**不改引擎**。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
git add firmware scripts
git commit -m "test(components/sloop_core): 旧格式工程 fixture 上传与兼容门禁"
git push origin feat/phase4b
```

---

### Task 10: 状态层存储命令与页 id 扩容

**Files:**
- Modify: `firmware/components/sloop_state/include/sloop_state.h`
- Modify: `firmware/components/sloop_state/src/cmd.c`
- Modify: `firmware/components/sloop_state/src/state.c`
- Modify: `firmware/components/sloop_state/test/stub_engine.c` + `state_test.c`
- Modify: `firmware/main/console.c`（`k_page_names` 加 `projects`/`presets`）

**Interfaces:**
- Consumes: Task 3/4/5/7 的 `sloop_store_*` 与 `sloop_core_apply_preset/set_engine`
- Produces: `CMD_PROJECT_SAVE/LOAD/ERASE`、`CMD_PRESET_STORE/LOAD/ERASE`、`CMD_BANK_PUT/ERASE`；`app_state_t.store_status`（0 idle/1 ok/2 empty/3 playing/4 error/5 drum）；`APP_PAGE_PROJECTS, APP_PAGE_PRESETS`，`APP_STATE_PAGES 11`

- [ ] **Step 1: 写失败测试（host）**

`state_test.c`：stub `sloop_store_*` 返回固定码 → 断言四类命令的 `store_status`（成功/空槽/播放中/鼓轨/越界不改）；`CMD_PRESET_LOAD` 后 `state_sync_from_engine` 计数 +1；`CMD_PROJECT_LOAD` 后投影整份刷新。`scripts/state-test.sh` 确认 FAIL。

- [ ] **Step 2: 实现**

`edit_cmd_type_t` 尾部追加；`cmd.c` 分发到 `sloop_store_*`，返回值映射 `store_status`（`state_locked()` 内写；存储调用前释放状态锁不现实——保持状态锁内执行，接受 ≤150ms UI 停顿，**不得**在引擎锁内做 flash 擦写）。成功后调内部 `state_sync_from_engine`（照 `cmd_select_track` 写法）。

- [ ] **Step 3: 页 id**

`app_page_t` 追加两页；`APP_STATE_PAGES 11`；console `k_page_names` 追加 `"projects","presets"`（此时 `UI projects` 返回 `not_registered`，属预期）。

- [ ] **Step 4: 门禁 + 提交**

```bash
scripts/state-test.sh
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware
git commit -m "feat(components/sloop_state): 存储命令与 11 页 id 扩容"
git push origin feat/phase4b
```

---

### Task 11: 导航重构（两行页签 + 组作用域生命周期）

**Files:**
- Modify: `firmware/components/sloop_ui/src/ui.c`
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`
- Modify: `firmware/components/sloop_ui/src/input.c`
- Modify: `firmware/main/console.c`（`UI MEM` 分组）
- Modify: `scripts/inject/inject.py`（`run_nav`）
- Modify: `docs/design/006-keyboard-mapping.md`

**Interfaces:**
- Consumes: Task 10 的页 id
- Produces: `k_groups[]`；`UI MEM` 行 `GROUP <name> built=<n> sum_est=<B> sum_delta=<B>`；`sloop_ui_page_order_next()`

- [ ] **Step 1: 写失败断言**

`run_nav` 更新：①页签条内、页签矩形外 TAP → miss；②鼓轨只读；③同拍 tab+PANIC → 不跳页；④同拍 3 次 tab → 只前进一页，新顺序 PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT（FILE 未注册时止于 TRANSPORT→PLAY）；另加：EDIT→SEQ 后 `UI MEM` 显示 EDIT 组 built=0。运行确认 FAIL。

- [ ] **Step 2: 两行页签**

`create_tab_bar`：上行 group 按钮（`y=4,h=32`，宽度按**已注册页非空的组数**，16pt 居中；无页的组不显示）；下行 sub 按钮（`y=38,h=30`，只显示当前组已注册子页）；`SLOOP` 标题上行右侧（`-24,6`）；层键提示下行右侧（`-110,40`，`LONG_DOT`）；页面区起点/尺寸不动。顶层点组 = 进该组上次子页；子页点 = 切页。

- [ ] **Step 3: 组作用域销毁**

`show_page(page)`：目标组 ≠ 当前组 → 固定顺序「**建目标页（目标组内懒建）→ 切到目标页 → 销毁源组全部已建页**（`lv_obj_del`、`s_page_obj=NULL`、state=UNBUILT、delta=0，**含原当前页**）」；因此离开组后源组 `built=0`。同组：首次访问兄弟页时构建，后续切换只隐藏。销毁在 LVGL 锁内、先作废待执行切页。≤100ms 预算适用于跨组切换与组内首次构建兄弟页。`UI MEM` 增加每组建/Σest/Σdelta 与 build_ms。

- [ ] **Step 4: 输入层**

`ACT_PAGE_NEXT` 改用 `sloop_ui_page_order_next()`（组顺序扁平轮转，一拍一页语义保留）；`K_ESC` 回 PLAY 并复位组/子页；`docs/design/006-keyboard-mapping.md` 同步。

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
截屏：`UI play|tracks|global|fx|analog|fm6|sample|step|transport` 各一张；两行页签不重叠、页面底行完整、差异只允许页签区；`UI MEM` 记录分组水位。证据入 `4-pb-2026-11-21/`。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_ui firmware/main/console.c scripts/inject docs/design/006-keyboard-mapping.md docs/dev/verify/4-pb-2026-11-21
git commit -m "feat(components/sloop_ui): 分组导航与组作用域页面生命周期"
git push origin feat/phase4b
```

---

### Task 12: PROJECTS / PRESETS 页面

**Files:**
- Create: `firmware/components/sloop_ui/src/page_projects.c`
- Create: `firmware/components/sloop_ui/src/page_presets.c`
- Modify: `firmware/components/sloop_ui/src/ui.c`（注册 FILE 组两页 + est）
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`（TAP kind）
- Modify: `firmware/components/sloop_ui/src/input.c`（FILE 页 ←/→/ENTER）
- Modify: `scripts/inject/inject.py`
- Modify: `docs/design/008-param-pages.md`（预算表）

**Interfaces:**
- Consumes: Task 10 的命令；Task 11 的组模型
- Produces: `page_projects_build/update`、`page_presets_build/update`；TAP kind `SLOOP_TAP_FILE_SLOT`（`a=` 页、`b=` 槽）与 `SLOOP_TAP_FILE_ACTION`（`v=` 0 save/1 load/2 erase）

- [ ] **Step 1: 失败断言**

`run_store` UI 段：`UI projects` → `SCR` 探针（标题/4 卡/按钮）→ TAP SAVE/LOAD/ERASE → `STORE PROJ LIST` 与 `STATE SNAP` 验证；`UI presets` → 32 槽探针 → TAP LOAD/SAVE/ERASE；播放中 TAP SAVE → 状态行 `STOP BEFORE SAVE` 且存储不变；`UI MEM` 断言两页 delta ≤ est；离开 FILE 后两页 `built=0`。运行确认 FAIL。

- [ ] **Step 2: PROJECTS**

4 卡横排（`A/B/C/D`），每卡标题 + `EMPTY|USED` + hash 前 4 位 + `SAVE/LOAD/ERASE` 按钮（100×40）。点卡设选中槽（局部 `s_slot`），按钮发 CMD 并把 `store_status` 映射为状态行。`update()` 只在状态/槽列表变化时改文本。

- [ ] **Step 3: PRESETS**

4×8 网格（槽 144×72），每格 `U01` + 引擎名/`EMPTY` + 选中描边；下方 `LOAD/SAVE/ERASE`。`SAVE` 目标=选中轨（鼓轨→`DRUM TRACK: NO SOUND`），`LOAD` 载入选中轨；列表直查 `sloop_store_preset_*`（只读例外）。

- [ ] **Step 4: 注册与预算**

`k_groups[]` FILE 组加两页；`k_builtin_pages[]` 追加 `{APP_PAGE_PROJECTS,"PROJECTS",…,4608}`、`{APP_PAGE_PRESETS,"PRESETS",…,5120}`；实测 delta 后按 ADR-008 调整 est 并写表。`run_nav` 轮转期望更新为最终 11 页顺序（…TRANSPORT→PROJECTS→PRESETS→PLAY）。

- [ ] **Step 5: 门禁**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
bash scripts/inject/nav.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
```
截屏归档：`task-12-projects-empty/-used/-presets/-save-playing.png`；人眼检查（§5.4）。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_ui scripts/inject docs/design/008-param-pages.md docs/dev/verify/4-pb-2026-11-21
git commit -m "feat(components/sloop_ui): PROJECTS 与 PRESETS 页面（FILE 组）"
git push origin feat/phase4b
```

---

### Task 13: FM6 页 bank 存取行

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_params.c`
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`
- Modify: `scripts/inject/inject.py`

**Interfaces:**
- Consumes: Task 7 的 bank API；Task 10 的 `CMD_BANK_PUT/ERASE`
- Produces: FM6 页底部 `BANK - / B12 / + | SAVE | ERASE` 行

- [ ] **Step 1: 失败断言**

`UI fm6` → `SCR` 探针（`BANK` 行）→ `TAP BANK +`×3 → `TAP BANK SAVE` → `STORE BANK GET <槽>` == `STORE BANK LIVE`；`TAP BANK ERASE` → empty；播放中 SAVE → `STOP BEFORE SAVE` 且不变。运行确认 FAIL。

- [ ] **Step 2: 实现**

FM6 布局表底部加一行（不与旋钮环重叠）；槽号局部态 0..26，`+/-` 改槽，SAVE/ERASE 发 CMD；`update()` 显示 `B<slot+1>` 与 `store_status`；TAP kind 与矩形表同步。

- [ ] **Step 3: 门禁 + 提交**

```bash
scripts/build.sh && scripts/build.sh
scripts/flash.sh /dev/ttyACM0
bash scripts/inject/store.sh /dev/ttyACM0
bash scripts/inject/params.sh /dev/ttyACM0
python3 scripts/device-selftest.py /dev/ttyACM0
git add firmware scripts/inject docs/dev/verify/4-pb-2026-11-21
git commit -m "feat(components/sloop_ui): FM6 页 bank 存取行"
git push origin feat/phase4b
```

---

### Task 14: 收口（soak 新判据、全门禁、证据、合 dev）

**Files:**
- Modify: `scripts/soak.py`（`--save-window`）
- Modify: `docs/dev/verification-log.md`
- Modify: `docs/design/002-port-roadmap.md`
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`
- Create: `docs/dev/verify/4-pb-2026-11-21/readme.md` + `SHA256SUMS`

**Interfaces:**
- Consumes: 全部前置 Task
- Produces: P4-B 证据包；`dev` 合并提交

- [ ] **Step 1: soak 判据**

`soak.py` 加 `--save-window N`：窗口内做一次 `STORE PROJ SAVE 0` + `LOAD 0`，窗口内 `xrun` 增量 ≤16、窗口后 `streaming=1`；窗口外仍 `xrun=0`；默认（无参数）行为不变。

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
全部退出码 0；soak `VERDICT: PASS`；`STORE TEST PASS 6/6`；autosave/resume 段绿。

- [ ] **Step 3: 证据晋升**

本轮截屏/日志复制到 `docs/dev/verify/4-pb-2026-11-21/`（失败件加 `-fail`），写 `readme.md`（门禁命令/结论/证据），生成 `SHA256SUMS`，`verification-log.md` 登记。

- [ ] **Step 4: 文档状态**

roadmap：P4-B ✅（日期/提交号/结论：往返 hash、torn-write 6/6、旧格式 3 种、autosave 恢复、换预置换声音、FILE 组预算、组作用域）；phase-4 spec P4-B 行 ✅；§6 状态列同步。

- [ ] **Step 5: 提交、review、合 dev**

```bash
git add scripts/soak.py docs/
git commit -m "docs(dev/verify): P4-B 收口门禁与证据"
git push origin feat/phase4b
```
终审 reviewer 审整分支；修复波后：
```bash
git checkout dev && git pull --ff-only origin dev
git merge --no-ff feat/phase4b -m "merge: P4-B 存储与预置（工程 A-D / 自动存档 / 用户预置 / FM6 银行 / 分组导航）"
scripts/build.sh && scripts/build.sh && scripts/host-test.sh
git push origin dev
```

---

## 完成判据对照（spec §6）

| spec 条目 | 覆盖 Task | 门禁 |
|---|---|---|
| 存储后端（原始分区 + mmap + A/B） | 1,2,3 | `STORE STATS`/`RAWTEST`、启动日志 |
| 工程 A–D 存读（FUN6，RAM+flash） | 3 | `store.sh` 往返 hash + 重启 fetch + `STATE SNAP` |
| 自动存档与上电恢复 | 3,6 | autosave 写入/间隔/播放中不写/重启恢复 |
| 旧格式 FUN5/4/1 兼容 | 9 | fixtures 上传 + `PROJ FIELDS` |
| 用户预置 32 槽 | 4,12 | `store.sh` 预置段 + `up_boot` + PRESETS 页 |
| 工厂预置真正换声音 | 5 | `ENG TRACK` 后 p[] 断言 + mix 保留 + 无挂音 |
| FM6 银行 27 槽 | 7,13 | `BANK GET/LIVE` 字节一致 + 断电后 used + `fm6_bank_boot` |
| torn-write 不破坏旧副本 | 8 | `STORE TEST PASS 6/6` |
| 导航/容量（DI-17） | 1,11 | ADR-005/009、`UI MEM` 分组、`nav.sh` |
| 状态层与写入路径 | 10 | `state-test` 存储用例 |
| 文档/证据/合 dev | 14 | verification-log + roadmap + merge |

## 遗留与后续（不在本计划）

- settings 对象 / sections / arranger（P4-C、P4-E；设置走 `OBJ_SETTINGS` 上游格式）；
- FM6 patch 编辑面、DSYN、采样槽上传（P4-D；采样走上游 USR1–4 原始槽：USR1–3 `0xA0000..0xDC000`、USR4 `0xE7000..0xFB000`）；
- 物理触摸按钮、听感（保存/加载后音色变化、换预置换声音）、视觉复检的人测项（AGENTS §5.4 外部门禁）；
- 历史 DI：DI-2/4/6/7/10/12/13/15/16（见 `docs/deferred-issues.md`）。
