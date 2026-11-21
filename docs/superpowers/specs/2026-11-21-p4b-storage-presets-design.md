# 阶段 4 / P4-B：存储与预置设计规格（P4-B Spec）

> 日期：2026-11-21 ｜ 状态：草案（待项目负责人评审）
> 上位文档：`docs/design/002-port-roadmap.md` §6（P4-B 行，权威）；`docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md` §3/§4.3（P4-B 的框架定义，本 spec 取代其 §4.3 的「FATFS 文件内 A/B」措辞，见 §3.1）；
> ADR-005「导航信息架构」、ADR-008（池预算）、`docs/deferred-issues.md` DI-17。
> 前置结论：P4-A 已把页签推到 9 张（107px/张）并实测 9 页全建 `pool_free` 2.2–2.7KB；**P4-B 必须先把导航信息架构与容量决策落地**（ADR-005 已登记为 P4-B 决策门）。

## 1. 目标与非目标

**目标**（roadmap §6 P4-B）：让 Tab5 独立模式拥有与 FM-1 上 SLOOP 等价的持久化能力：

1. 工程 A–D 存/读/擦（上游 `FUN6` 格式，读兼容 `FUN5/4/3/2/1`）；
2. 用户预置 32 槽（存当前选中轨音色 / 载入到任意轨 / 擦除 / 浏览）；
3. FM6 预置银行 27 槽（存/取/擦，按上游 `FM6B` 记录布局）；
4. 工厂预置/鼓 kit 浏览（引擎 `presets[].name` 与 kit 名可直接读，无需新表）；
5. 掉电中断不破坏旧副本（沿用上游 A/B + CRC + 回读校验语义）；
6. 导航：分组页签 + 组内子页签 + **组作用域页面生命周期**，解开 DI-17 的 9 页天花板，为 P4-B–E 的新页面让路；
7. 机器可复现的验证入口：console `STORE …` 命令族 + 设备侧 torn-write 故障注入套件 + 注入脚本/截屏。

**非目标（本阶段明确不做）**：

- 自动存档（上游 `autosave_tick`/`autosave_resume`）——归 P4-C（与编排/工程生命周期一起做）；
- 上游 settings 对象（`persist_t`/panel/DSYN kit/MIDI LEARN）——归 P4-E（NVS + 设置页）；
- 编排 sections/song 落盘（`sections_write`、`arranger_scene`）——归 P4-C；
- FM6 patch 编辑面与 DSYN 编辑面——归 P4-D（本阶段只做 bank 的存/取/擦）；
- 采样槽上传/擦除——归 P4-D（本阶段只为 FATFS 分区保留布局）；
- 联动模式（编辑器协议的文件类命令）——阶段 5；本阶段的对象 id/格式已按上游对齐，届时可直接复用。

**反目标**：把上游 `project.c` 整文件编入并连带 `ui.c/panel.c`（240×240 UI 与硬件假设）；自造第二套工程/预置格式；在 UI 任务里直接写 flash；用扩大 LVGL 池解决页面容量（ADR-008 已实测 96KB 链接溢出）。

## 2. 现状与差距（本次侦察实证）

| 项 | 现状 | 差距 |
|---|---|---|
| 存储 | 无。`sloop_hal_tab5/src/storage_ram.c` 只有 4 个用户采样槽的 PSRAM 缓冲；`partitions.csv` 有 7.75MB FAT 分区（未挂载、无数据） | 无分区/挂载/读写；无 `st_read/st_erase/st_prog` 后端 |
| 上游存储 TU | `storage.c`（232 行：A/B 双扇区 + 32B 提交头 + zlib CRC + 回读校验 + side record + `st_xip`）、`project.c`（1220 行：FUN1–6 转换、`proj_capture/proj_apply`、autosave、settings/sections）、`upreset.c`（386 行）、`fm6_bank.c`（112 行） | 均未编入（`sloop_core` 单 TU 只含 `regress.c` → `hostsim.c` 的引擎集） |
| 引擎 API | `sloop_core` 有 read/snapshot/step/param/desc/mute/solo 等 | 无「整工程捕获/应用」「预置存/取」「FM6 bank 存/取」入口 |
| 状态层 | `sloop_state` 有 15 条编辑命令（唯一写者 = input 任务） | 无存储类命令；`app_state_t` 无存储列表投影 |
| UI | 9 张平铺页签；页面「首次访问构建、之后保留」；`app_page_t` 与 `APP_STATE_PAGES` 有静态断言 | 无 FILE 页面；页签宽度在 N=10 时为 95px < `TRANSPORT` 101.6px，加页不可行；页面无销毁路径 |
| 验证 | host `storage_test/project_test/upreset_test/fm6_test` 全绿（`scripts/host-test.sh`） | 设备侧无存储；无 torn-write 故障注入入口 |

上游关键契约（精读结论，`third_party/sloop-fm1/firmware/src/`）：

- **storage.c**：每个对象一对 4KB 扇区；写入 = 擦非当前副本 → 从偏移 256 起写 payload → **最后**写 32B 头（提交记录）；加载取「头合法且 seq 最大」的副本；`st_save` 写后回读比对（写保护/坏片不得报 SAVED）；`st_xip` 需要在 flash 中**原地读** payload（`upreset.c`/`fm6_bank.c` 依赖，省 6,160B RAM）；`st_buf` 用主循环 scratch（3,840B，**必须能在关闭 cache 期间访问 → 内部 SRAM**）。
- **flash map**（绝对偏移）：settings `0xFC000/0xFD000`，projects `0x97000..0x9EFFF`，user samples `0xA0000..0xDBFFF`，user preset banks `0xDC000..0xDFFFF`，autosave A `0x9F000` / B `0xFE000`，FM6 bank `0xE5000/0xE6000`。本阶段实际用到 `[0x97000, 0xFF000)`（= 416KB，含空洞）。
- **project.c**：`PROJ_HOST` 以上部分（≈554 行）是纯格式层 + `proj_capture/proj_apply`，`tests/project_test.c` 已证明它只需 `core.h + params.c + drums.c + engines + trk_def_engine` 即可编译；`PROJ_HOST` 以下（settings/panel/autosave/sections/arranger/DSYN/MIDI）依赖上游 UI 与硬件，**不采用**。
- **upreset.c**：`UP_HOST` 以上只有纯记录工具；**写/载逻辑在 `UP_HOST` 以下**，需 `TSEL/sound_change/arp_is_sounds/param_kept/p_lockable/fm6_track_loaded/sync_reload/ui_message/ui_say/ui.force/up_gen/up_stage/flash_ok/st_xip`——前 8 个在 `ui.c/seq.c`，未编译，需薄 shim；`up_stage` 由 project.c 在 `FELUCCA_FLASH` 下提供，`PROJ_HOST` 下没有，需我们定义。
- **fm6_bank.c**：由 `project.c` 在 `FELUCCA_FLASH && !PROJ_HOST` 下 include；依赖 `proj_tmp`、`st_current/st_sector/st_buf`、`fm6_slot/fm6_unpack/fm6_pack/fm6_bank_read`（`eng_fm6.c`，已编入）、`flash_ok`、`FM6_BANK_XIP`。
- **`PROJ_HOST` 的静态开销陷阱**：`project.c` 的 `proj_slot[4]`（4×3,816=**15,264B**，`section(".noinit")`）与 `proj_v3_tmp`（**2,584B**，旧格式转换用）都在 `#ifndef PROJ_HOST` **之上**，定义 `PROJ_HOST` 不会去掉它们。`proj_slot` 在 P4-B 用不到（sections 属 P4-C），且 `.noinit` 与 `selftest.c` 的 `ctl/s_quiesce_hook` 同段、无法被 `--gc-sections` 单独回收；`proj_v3_tmp` 被 `proj_from_v2/v1` 真实引用。解法（不碰 vendored）：include 前用宏把它们重定向成**指针**，实体放 PSRAM（`proj_tmp` 同法），见 §4.2。
- **ESP-IDF 5.5.5（esp32p4）**：`esp_partition_mmap()` 对任意数据分区可用（64KB MMU 页，内部按页对齐），`esp_flash_write/erase` 完成后按地址范围失效 cache（`flash_end_flush_cache`），因此「原始分区 + mmap」可以同时提供 `st_read` 与 `st_xip` 语义。擦除/写入期间 cache 关闭、双核停顿（上游注释的 ~50ms），音频需静音。

## 3. 方案（Approaches）

### 3.1 存储后端

| 选项 | 评估 |
|---|---|
| **A. 原始自定义分区 + mmap + 上游 storage.c 原样（采纳）** | 分区 `sloopstore`（data/0x99）覆盖 flash map 的 `[0x97000,0xFF000)`；`st_read`=映射区 memcpy，`st_erase`=`esp_partition_erase_range`（先 codec mute），`st_prog`=`esp_partition_write`；`ST_XIP_PTR(off)`=映射基址+偏移。**真擦写语义（A/B/torn-write 与硬件一致）、`st_xip` 零成本、上游 4 个 TU 不动逻辑**；代价：分区表调整（FAT 尚无数据，可安全重排） |
| B. FATFS 单文件内 A/B 双槽 | 文件层 A/B 与 storage.c 的 A/B 重复；无 mmap → `st_xip` 必须改成 6KB RAM 镜像（`upreset.c/fm6_bank.c` 需改）；整镜像/扇区写入无原子性，掉电窗口比原始扇区更大；FATFS 元数据损坏面更大 |
| C. NVS 分块存 blob | 无擦除语义（storage.c 的 `st_erase` 语义要另造）；NVS 容量/寿命/键数限制；无 XIP；不选 |

**决策 A**。配套：`partitions.csv` 调整为 `sloopstore` @ `0x810000` size `0x68000`，FAT `storage` 移到 `0x878000` size `0x788000`（均 4KB 对齐；FAT 本阶段不使用，P4-D 采样资产用）；NVS 保持 `0x9000/0x6000`（P4-E 设置用）。分区表变更需 `scripts/build.sh --clean` + 首次烧录前 `erase-flash`（仅一次，FAT 无数据）。

> 该决策取代 phase-4 spec §4.3 的「FATFS 分区文件内 A/B 双槽 + CRC」：**语义完全保留上游（A/B + CRC + 回读），承载介质改为原始分区**。FATFS 只承担 P4-D 的用户采样资产；NVS 只承担 P4-E 的小设置。

### 3.2 上游代码复用方式

| 选项 | 评估 |
|---|---|
| **A. 同 TU include + 薄 shim（采纳）** | 在 `sloop_core` 的单 TU（`src/selftest.c` 末尾）include：`storage.c`（原样，提供三个 hook）、`project.c`（定义 `PROJ_HOST`，只取格式层 + capture/apply）、`upreset.c`（原样，提供 shim）、`fm6_bank.c`（原样，前置 `proj_tmp/flash_ok` 等）。shim 约 10 个小符号（`trk_def_engine/ui_message/ui_say/ui.force/up_gen/sync_reload/flash_ok/up_stage/ST_XIP_PTR/FM6_BANK_XIP`）。上游 vendored 目录零修改，便于 diff 跟随 |
| B. 复制并改写 project.c/upreset.c | 修改面大、上游升级要人工对齐；违反 AGENTS §8「补丁/适配层隔离」 |
| C. 只借格式常量，重写全部逻辑 | 重复实现 A/B、CRC、旧格式转换（1,200+ 行），回归风险最高 |

**决策 A**。`project.c` 用 `PROJ_HOST` 意味着**不编入** settings/panel/autosave/sections/arranger/DSYN/MIDI 部分（见非目标）；`proj_capture/proj_apply` 在 `PROJ_HOST` 之前，可用。

### 3.3 导航信息架构与页面容量（DI-17 硬前置）

| 选项 | 评估 |
|---|---|
| **A. 分组页签 + 组内子页签 + 组作用域生命周期（采纳）** | 顶层 5 组：`PLAY / TRACKS / EDIT / SEQ / FILE`（`SYS` 待 P4-E 有页面时加入）。**子页签放在现有 72px 页签条内做成两行**（上行组、下行子页，各约 34/30px，16pt 标签 ~20px 高），**页面区起点仍是 y=72，现有九页布局零改动**。EDIT = GLOBAL/FX/ANALOG/FM6/SAMPLE（后续引擎页），SEQ = STEP/TRANSPORT（后续 SONG），FILE = PROJECTS/PRESETS。**离开组时销毁该组已建页面**（回 UNBUILT），进入时懒建并保留。内存不变量从「9 页全建」变为「基础占用 + 单组 Σ est ≤ 池可用」，FILE 只花 8KB，未来页面有确定预算 |
| B. 保留平铺页签 + 预算失败时 LRU 驱逐 | 不改页签语义；但驱逐时机不可预测（用户切到第 10 页才失败/重建），焦点状态丢失不可控，且页签宽度墙（N=10=95px）仍然无解 |
| C. 两行页签（页面区下移 44px） | 现有页把页面区用到像素级（参数页 `52+2×298=648` = `UI_H−72`；STEP 数字 y=568、提示 y=606），下移会裁掉底行并迫使九页重排 + 全部 TAP/探针几何重算；否决。**采纳方案 A 的两行页签是「在 72px 页签条内分两行」，不是本项** |
| D. 溢出菜单 | 需要新的焦点/菜单语义与可发现性设计，P4-B 不引入 |

**决策 A**。键盘 `TAB` 语义改为「按组顺序的扁平轮转」：组内最后一页的下一张 = 下一组第一页（保留现有「tab 轮转」心智与 `nav.sh` 用例形态，更新期望序列）；点顶层页签进入该组（记住组内上次子页），点子页签切换。页签宽度 N=5 时 min(176, 160px)，标签 `PLAY/TRACKS/EDIT/SEQ/FILE` 均远小于 160px。

**DI-17 收口**：组作用域生命周期把「9 页 = 池顶」变成可维护的不变量：
`base(≈7.0KB) + max_group Σ est + 4KB 余量 ≤ 63,120B`，且每页仍保留 `free ≥ est` 的构建前检查。
当前最大值 SEQ = `23,040 + 4,608 = 27,648B`，FILE 预计 ≤ 8KB。ADR-005 记录决策与不变量，ADR-008 记录组作用域对预算表的影响；**DI-17 删除**（条目要求「解决后删除；方案选型先沉淀 ADR」）。

### 3.4 写入路径与并发

| 选项 | 评估 |
|---|---|
| **A. 全部写/载操作经状态层命令（采纳）** | `CMD_PROJECT_* / CMD_PRESET_* / CMD_BANK_*` 由 input 任务（唯一写者）执行；UI 只发命令、只读列表查询。保持阶段 3 的「UI 不直接写引擎」不变量，阶段 5 可把执行端换成 FM-1 会话 |
| B. UI 任务直调 `sloop_store_*` | 少一层转接，但破坏唯一写者模型；加载工程会与 input 任务编辑竞争 |

**决策 A**。只读的列表/名称查询（`used/name/engine/count`）允许 UI 直接调用（无写、mmap 读、无锁；写操作期间 UI 任务本就被 cache 停顿），在 ADR-009 记录为例外。
**console 例外**：`STORE …` 诊断命令由 REPL 任务直接调用同一套 `sloop_store_*`（需要返回值做脚本断言，`state_apply` 是 fire-and-forget 无法回传），与 UI 路径共用 `s_store_lock` + 引擎锁，行为与 CMD 完全相同；这与 `ENG`/`PARAM RAW` 属同一「诊断执行器」类别，ADR-009 记明。

**并发纪律**（与上游一致）：
- 保存：引擎锁内 `proj_capture`（≈3.8KB 拷贝）→ 释放锁 → `st_save`（擦/写 flash，期间 codec mute）；
- 加载：无锁 `st_load + proj_import` → 引擎锁内 `proj_apply(all=1)` + `song.sel` → 释放（等价上游 IRQ-off 语义）；
- 播放中拒写：`song.playing || transport_req` 时返回 `ESP_ERR_INVALID_STATE`（对齐上游 "STOP BEFORE SAVE"）；
- 存储串行化：另设 `s_store_lock`，避免两条命令并发擦写；所有 `sloop_store_*` 不得在 ISR 调用。

## 4. 组件与接口设计

### 4.1 HAL：`sloop_hal_tab5`（新文件 `src/storage_flash.c` + `include/sloop_hal.h` 扩展）

```c
/* 原始分区 + mmap。失败（找不到分区/mmap 失败）时 ready=false，上层只读 API 返回空。 */
esp_err_t sloop_hal_storage_init(void);
bool      sloop_hal_storage_ready(void);
const uint8_t *sloop_hal_storage_map(void);      /* 映射基址（对应 flash 偏移 0x97000） */
uint32_t  sloop_hal_storage_base(void);          /* 0x97000 */
uint32_t  sloop_hal_storage_span(void);          /* 0x68000 */
int       sloop_hal_storage_read(uint32_t off, void *dst, uint32_t n);
int       sloop_hal_storage_erase(uint32_t off); /* 4KB 对齐；擦前 codec mute，擦后 unmute */
int       sloop_hal_storage_prog(uint32_t off, const void *src, uint32_t n);
/* 故障注入（设备 torn-write 套件）：第 n 次写操作（erase/prog）失败；half=1 时 prog 只写一半。 */
void      sloop_hal_storage_fault_arm(uint32_t nth, bool half);
uint32_t  sloop_hal_storage_fault_taken(void);
void      sloop_hal_storage_fault_clear(void);
```
- `init` 用 `esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x99, "sloopstore")` + `esp_partition_mmap(part, 0, span, ESP_PARTITION_MMAP_DATA, …)`；映射句柄常驻。
- 抹音：`esp_codec_dev_set_out_mute(codec, true)` → 操作 → `false`（通过 `sloop_hal_audio_codec()`；音频流未开启时为空操作）。

### 4.2 引擎侧：`sloop_core` 新公共头 `include/sloop_store.h`

```c
#define SLOOP_STORE_PROJECTS 4u
#define SLOOP_STORE_PRESETS  32u
#define SLOOP_STORE_BANK_SLOTS 27u

typedef struct {
    bool ready; uint32_t objects_ok, faults_armed, faults_taken;
} sloop_store_stats_t;

esp_err_t sloop_store_init(void);            /* sloop_core_init 内调用；HAL init + flash_ok */
bool      sloop_store_ready(void);
void      sloop_store_stats(sloop_store_stats_t *out);

/* 工程 A–D（0..3）。hash = 存储副本 project_t 的 FNV-1a（0=空/无效）。 */
bool      sloop_store_project_used(uint32_t slot);
uint32_t  sloop_store_project_hash(uint32_t slot);
uint32_t  sloop_store_live_hash(void);       /* 当前引擎捕获（不写 flash） */
esp_err_t sloop_store_project_save(uint32_t slot);
esp_err_t sloop_store_project_load(uint32_t slot);   /* 停走带 + panic 全部轨 */
esp_err_t sloop_store_project_erase(uint32_t slot);

/* 用户预置 0..31（写 = 当前选中轨；读可指定轨）。名字大写 13 字节。 */
uint32_t sloop_store_preset_count(void);
bool     sloop_store_preset_used(uint32_t slot);
bool     sloop_store_preset_name(uint32_t slot, char out[13]);
uint8_t  sloop_store_preset_engine(uint32_t slot);
esp_err_t sloop_store_preset_store(uint32_t slot);
esp_err_t sloop_store_preset_load(uint32_t slot, uint32_t track);
esp_err_t sloop_store_preset_erase(uint32_t slot);

/* FM6 银行（packed 128B 记录，槽 0..26）。 */
bool     sloop_store_bank_used(uint32_t slot);
bool     sloop_store_bank_get(uint32_t slot, uint8_t out[128]);
esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed);
esp_err_t sloop_store_bank_erase(uint32_t slot);
void     sloop_store_bank_current(uint8_t *packed);  /* 选中轨当前 patch 打包（存 bank 用） */

/* 设备 torn-write 套件：用 OBJ_AUTOSAVE（P4-B 尚未使用）跑 6 个场景。破坏性，仅诊断。 */
typedef struct { uint32_t cases, passed; const char *fail_case; } sloop_store_test_t;
esp_err_t sloop_store_test(sloop_store_test_t *out);
```
返回码约定：`ESP_OK`；`ESP_ERR_NOT_FOUND`（槽空/无存储）；`ESP_ERR_INVALID_STATE`（播放中/存储未就绪）；`ESP_ERR_INVALID_ARG`（槽/轨越界、鼓轨存预置）；`ESP_ERR_NOT_FINISHED`（flash 写失败/回读不符）。

实现落点：`components/sloop_core/src/store_tab5.c`（由 `selftest.c` 末尾 include，保证与引擎同 TU）：
1. 定义 shim（§3.2 列表；多数符号 `seq.c/voice.c/eng_fm6.c` 已在 TU 内，实际只需 `trk_def_engine/ui_message/ui_say/ui.force/up_gen/sync_reload/flash_ok/up_stage/ST_XIP_PTR/FM6_BANK_XIP` 等薄封装）与 `main_scratch`（**内部 SRAM**，3,840B）；
2. **PSRAM 重定向（不碰 vendored）**：在 include `project.c` **前**定义宏，让上游的**声明本身**退化为指针（类型由上游行内给出，无需提前知道类型）：
   ```c
   #define proj_v3_tmp   (*s_v3_ps)     /* static project_v3_t proj_v3_tmp; → static project_v3_t (*s_v3_ps); */
   #define proj_slot     (*s_slot_ps)   /* project_t proj_slot[4] …;          → project_t (*s_slot_ps)[4] …;  */
   ```
   两行各只占 4B（`.noinit` 的 15,264B 与 `.bss` 的 2,584B 都消失）；`sloop_store_init` 里 `s_v3_ps = heap_caps_malloc(sizeof *s_v3_ps, MALLOC_CAP_SPIRAM)`，`s_slot_ps` 保持 NULL（P4-B 无引用，sections 属 P4-C）。宏包裹的是**整个对象**，上游全部引用形如 `&obj[...]`/`sizeof obj[i]`（已逐行核对），替换后取址与 `sizeof` 语义不变。
3. 自有暂存（PSRAM，init 时 `heap_caps_malloc(4096, MALLOC_CAP_SPIRAM)`）：
   ```c
   typedef union { project_v5_t v5; uint8_t raw[4096]; } proj_stage_t;
   static proj_stage_t *s_stage;  #define proj_tmp (*s_stage)   /* preset bank / fm6 bank / import */
   ```
   静态断言 `sizeof(proj_tmp) >= sizeof(fm6_bank_t) && >= sizeof(project_t)`。
4. include `storage.c` → `project.c`（`#define PROJ_HOST`）→ `upreset.c` → `fm6_bank.c`（前置 `flash_ok` 等）；
5. 实现 `sloop_store_*`（capture/apply/存读/擦、hash、故障套件）。

**内部 RAM 账（实测后复核）**：新增仅 `main_scratch` 3,840B + 指针/小静态 ≈4.2KB；当前 soak `internal_free≈70.4KB`、门禁地板 61,440B，预计剩余 ≈66KB。**门禁**：`nm`/`idf.py size-components` + 启动 `ENG MEM` 确认 `proj_slot/proj_tmp/proj_v3_tmp` 不在 DRAM，且 idle 内部堆 ≥61,440B。

### 4.3 状态层（`sloop_state`）

新增命令（`edit_cmd_type_t` 尾部追加，`cmd.c` 分发）：

| 命令 | 字段 | 执行 |
|---|---|---|
| `CMD_PROJECT_SAVE/LOAD/ERASE` | `a=slot(0..3)` | 播放中拒绝；成功后 `state_sync_from_engine()` 整份重取（load 后 sel/参数/步全部变） |
| `CMD_PRESET_STORE/ERASE` | `a=slot(0..31)` | store = 选中轨；鼓轨返回错误并写状态行提示 |
| `CMD_PRESET_LOAD` | `a=slot, b=track(0..3)` | 载入后整份重取投影；FM6 轨同步 patch |
| `CMD_BANK_PUT/ERASE` | `a=slot(0..26)` | put 取选中轨当前 patch；播放中拒绝 |

`app_state_t` 不新增存储列表（UI 只读直查，见 §3.4），只新增 `uint8_t store_status;`（最近一次存储动作结果，UI 状态行显示，0=idle/1=ok/2=empty/3=playing/4=error）。

### 4.4 console（`main/console.c`）

```
STORE STATS                      # ready/分区/mmap/对象统计/故障与 xrun 增量
STORE PROJ LIST|SAVE n|LOAD n|ERASE n|HASH n|FIELDS n
STORE PRESET LIST|SAVE n|LOAD n [track]|ERASE n
STORE BANK LIST|PUT n|GET n|LIVE|ERASE n    # PUT 取选中轨当前 live patch；GET/LIVE 打印 128B hex
STORE LIVEHASH                   # 当前引擎捕获 hash（往返门禁用）
STORE RAW BEGIN <len>            # 分块上传任意对象 payload（旧格式 fixture 用）
STORE RAW <hex>                  # 追加一块（≤480 hex 字符）；可重复
STORE RAW END <obj> <slot>       # 以 st_save 原样写入该 payload（CRC/头由 storage.c 生成）
STORE RAW ABORT
STORE TEST                       # 设备 torn-write 套件（破坏 OBJ_AUTOSAVE）
STORE FAULT <nth> [half]|CLEAR   # 故障注入（供脚本制造掉电窗口）
```

### 4.5 UI（`sloop_ui`）

- **导航壳**：`ui.c` 增加组模型（`k_groups[]`：标题 + 页面 id 列表）；**两行页签复用现有 72px 页签条**（上行 group 按钮 ~34px，下行 sub 按钮 ~30px；只有 1 页的组下行留空、右侧继续显示层键提示）；页面区起点与尺寸不变（`UI_TAB_BAR_H` 保持 72），hit-test 原点不变。
- **组作用域生命周期**：切组/切页时，`page_leave_group(group)` 销毁该组内**非当前**已建页面（`lv_obj_del` + 复位 `s_page_obj/state/delta`），当前页保留。PLAY 组单页仍在启动时构建。
- **PROJECTS 页**（FILE 组）：4 张卡片（A–D，标签 + 状态「EMPTY / USED」+ hash 前 4 位），每卡 3 个按钮 `SAVE / LOAD / ERASE`；播放中按钮动作显示 `STOP BEFORE SAVE/LOAD/ERASE`；点击卡片设选中槽（键盘 ←/→ 也可）。
- **PRESETS 页**（FILE 组）：32 槽 4×8 网格，显示 `U01..U32` + 引擎名/空；选中槽后 `LOAD`（载入选中轨）/`SAVE`（存选中轨，鼓轨提示 `DRUM TRACK: NO SOUND`）/`ERASE`。
- **FM6 页**：现有 8 旋钮页底部加一行 **bank 目标槽选择（`BANK - / +`，0..26）+ `BANK SAVE / BANK ERASE`**。`BANK SAVE` 把**当前 live patch**（`fm6_patch[track]` 经 `fm6_pack`，可来自工厂槽 F1–F8 或宏改动后的声音）写入目标槽；`PTCH` 旋钮仍是**音源选择**（PTCH = 8 + bank 槽）。状态行显示 `SAVED B12 / ERASED B12 / BANK FULL?` 等。
- **页 id**：`app_page_t` 追加 `APP_PAGE_PROJECTS, APP_PAGE_PRESETS`（`APP_STATE_PAGES` 9→11）；console `UI` 页名表与 `UI MEM` 增加组字段（组名、组内页、组作用域下当前已建集合）。
- **键盘**：`TAB` = 扁平轮转（组内子页 → 下一组）；`ESC` = 回 PLAY（组与子页同时复位）；FILE 页 `←/→` 选槽、FM6 页 `←→` 在旋钮间移动（沿用参数页），`ENTER` = LOAD（PROJECTS/PRESETS）。SAVE/ERASE 仅触摸按钮 + console（避免与演奏键冲突）。

## 5. 约束

- **门禁**：`scripts/build.sh` ×2 零新增警告；`scripts/host-test.sh` 全绿（上游 4 个存储相关测试本就覆盖 A/B/torn-write）；设备 `ENG SELFTEST` 182/182 不得回退；`scripts/device-selftest.py` 全量；`scripts/state-test.sh`/`knob-test.sh` 绿。
- **池**：FILE 组两页 est 初始预算 `PROJECTS 4,608 / PRESETS 5,120`（构建前 `free ≥ est`）；新不变量 `base + max_group Σest + 4KB ≤ 63,120`；`UI MEM` 增加分组 Σ。
- **内部 RAM**：新增内部 SRAM 仅 `main_scratch` 3,840B + 指针/小静态（≈4.2KB 总计）；`proj_tmp`/`proj_v3_tmp` 在 PSRAM（宏重定向，§4.2），`proj_slot` 实体不分配；不改 LVGL 池；idle 内部堆地板 61,440B 不变。
- **时序**：保存动作（擦+写+回读）实测目标 ≤ 150ms（4KB 擦 + 3.8KB 写 + 回读）；擦/写期间 codec mute。**xrun 口径**：擦除会关 cache、音频任务停顿，`xrun` 必然增长——门禁改为「无存储动作窗口 `xrun=0`；每次保存的 `xrun` 增量 ≤ 16 且随后 `streaming=1`、渲染恢复」，`STORE STATS` 报告每次保存的 xrun 增量（ADR-009 记录；`soak.py` 增加显式保存窗口参数）。
- **数据兼容**：工程写入 `FUN6`；读兼容 `FUN5/4/3/2/1`（上游 `proj_import`）；用户预置 `UPB1`/`UP_VER 1`；FM6 `FM6B`/ver 1。**不得**改上游记录布局；阶段 5 的编辑器协议对象 id 与之一一对应。
- **只读 vendored**：`third_party/sloop-fm1/**` 不修改（manifest 校验）。
- **人测外置**：听感（保存/加载后音色变化）、物理触摸按钮、视觉复检按 AGENTS §5.4 清单外部门禁。

## 6. 验证策略

1. **host（已有 + 新增）**：`host-test.sh` 的 `storage_test/project_test/upreset_test/fm6_test` 必须保持全绿（它们验证上游 A/B/torn-write/格式转换）；新增 `state-test` 用例覆盖存储命令（stub `sloop_store_*`：成功/空槽/播放中/鼓轨/越界）。
2. **设备存储自检**：`STORE TEST` 6 个场景——①两次提交后 A/B 选择 seq 大者；②擦除失败→旧副本可读；③payload 写一半失败→旧副本可读；④头写失败→旧副本可读；⑤回读校验失败→报错不报 SAVED；⑥擦除提交（`len=0`）→对象为空。全过打印 `STORE TEST PASS 6/6`。
3. **工程往返（机器门禁）**：
   - 基线：改 BPM/参数/步/选轨，`STORE LIVEHASH` = h0；`STORE PROJ SAVE 0`；`STORE PROJ HASH 0` == h0；
   - 破坏：再改 BPM/参数/步 → `STORE LIVEHASH` = h1 ≠ h0；
   - 恢复：`STORE PROJ LOAD 0` → `STORE LIVEHASH` == h0；`STATE SNAP` 的 p/g 与保存前逐项一致；
   - **旧格式兼容**：`scripts/gen_project_fixtures.py` 生成 FUN5/FUN4/FUN1 三个 fixture（逻辑取自上游 `tests/project_test.c` 的构造器），`scripts/store_raw.py` 经 `STORE RAW BEGIN/<hex>/END` 写入槽 3，`STORE PROJ LOAD 3` 后 `STORE PROJ FIELDS 3` 与生成器给出的期望字段逐项比对（引擎/预置/全局/首步/参数代表值），并确认 `proj_import` 转换成功（槽 3 为诊断专用，不参与用户数据断言）。
4. **预置**：存槽 k（选中轨 ANALOG）→ 改 8 个参数 → load → `STATE SNAP` p[] 与存档前一致；`STORE PRESET LIST` 显示 used/name/engine；鼓轨 store 返回 `DRUM TRACK` 错误；跨轨 load（track 1）只改该轨、不动选中轨参数。
5. **FM6 bank**：设 bank 目标槽 3 → `STORE BANK PUT 3`（当前 live patch）→ `STORE BANK GET 3` 与 `STORE BANK LIVE`（同一 patch 的另一路导出）字节一致 → 重新上电后 `BANK LIST` 仍 used；`PTCH` 选到该槽（PTCH=11）后 `ENG TRIG` 出声且 `ENG SNAP` 的 preset/patch 指示正确；对无 patch 的槽 `BANK GET` 返回 empty。
6. **UI（§5.4）**：`UI file` → `UI projects` 截屏（空/used/播放中拒绝）→ `TAP` 点 SAVE/LOAD/ERASE → `SCR` + `--check` 像素抽检；`UI presets` 同；`UI MEM` 断言 FILE 组 Σdelta ≤ 预算、组作用域（离开 FILE 后 PROJECTS/PRESETS 回 UNBUILT）；新 `scripts/inject/store.sh` 退出码 0；`nav.sh` 四用例按新语义更新并通过。
7. **稳定性**：`scripts/soak.py` 120s（其中**独立窗口**做一次保存/加载周期）无 WDT/panic、内部堆不单调下降；xrun 判据按 §5：保存窗口外 `xrun=0`，保存窗口内增量 ≤16 并在窗口后恢复 `streaming=1`；`ENG SELFTEST` 182/182 在存储初始化后仍全绿（存储不触碰 golden）。
8. **证据**：`docs/dev/verify/4-pb-<日期>/`（截屏、日志、STORE TEST 输出、往返 hash、UI MEM、SHA256SUMS）+ `verification-log.md` 登记。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 擦除期间 cache 关闭导致音频 xrun/爆音 | codec mute 包住 erase/prog；xrun 判据见 §5（保存窗口内 ≤16、窗口外 0）；人测听感；若爆音不可接受，改「停流→存→启流」（回退开关，ADR-009 记录） |
| 分区表调整让旧固件数据失效 | 仅一次性的布局变更（FAT 无数据）；`partitions.csv` 注释写死偏移；烧录脚本文档加 `erase-flash` 提示 |
| `esp_partition_mmap` 与 FATFS/其他 mmap 区域冲突 | 416KB/64KB=7 页，P4 vaddr 空间充裕；若失败 → 存储 `ready=false`，UI 显示 `NO STORAGE`（失败可见，不崩溃） |
| 上游静态吃掉内部 RAM（`proj_slot` 15,264B / `proj_v3_tmp` 2,584B） | include 前宏重定向为指针 + PSRAM 实体（§4.2）；`nm`/`ENG MEM` 门禁确认；`st_buf` 强制内部 SRAM |
| 两行页签挤压标题/层键提示、或改变页签命中几何 | 两行都在现有 72px 内，页面区不动；右侧标题上行、层键提示下行；`TAP` 注入仍按 y<72 判页签区；调整既有截屏探针并归档新基线 |
| 旧格式 fixture 无入设备通道 | `STORE RAW` 分块上传 + `scripts/store_raw.py` + `scripts/gen_project_fixtures.py`（§6.3）；槽 3 诊断专用 |
| `PROJ_HOST` 编译缺符号（上游升级） | 编译期即暴露；shim 清单集中在 `store_tab5.c` 顶部，升级时只补此文件 |
| 组作用域生命周期引入切页重建延迟/状态丢失 | 切页计时探针（`UI MEM` 增加 build_ms）；目标 ≤100ms；子页焦点由 state 重建；`nav.sh` 覆盖往返 |
| UI 直接读存储列表与写操作竞争 | 列表查询只读 mmap、无写；写期间 UI 任务被 cache 停顿，不存在半写视图 |
| 保存时正在录音/播放 | 统一 `ESP_ERR_INVALID_STATE` + UI 状态行 `STOP BEFORE …`；console 脚本断言错误码 |

## 8. 留给实施者（Assumptions / 我替负责人做的决定）

1. **分区重排**（§3.1）在 P4-B 内一次完成；若负责人不希望动 FAT 偏移，回退方案是 `sloopstore` 放在 `0x810000` 之后、FAT 整体后移同样的量（效果等价）。
2. **组作用域销毁**策略：离开组即销毁该组已建页面（不是 LRU）。若实测切页重建 >100ms，退化为「每组保留最近 1 页 + 销毁其余」。
3. **自动存档、settings 对象、sections、arranger** 均留给后续子里程碑（明确非目标），P4-B 只保证显式存/读。
4. **用户预置的写入口径 = 当前选中轨**（上游 `up_store` 亦是 `TSEL`），UI 需先选轨；读可指定任意合成轨（上游 `up_load_to`）。
5. **FM6 bank 的 UI 入口**放在现有 FM6 页底部：**bank 目标槽**（0..26）与 `PTCH` 音源选择分离；`BANK SAVE` 写当前 live patch（工厂 patch 或宏改动后可存入用户槽）。P4-D 做 patch 编辑面时再扩展。
6. `STORE TEST` 使用 `OBJ_AUTOSAVE` 作为破坏性测试对象（P4-B 不启用 autosave），不进用户数据。
7. 页 id/页名、console 语法、注入脚本名（`scripts/inject/store.sh`）为本文约定，实施时如与现有命名冲突可在计划中微调并在证据中记录。
