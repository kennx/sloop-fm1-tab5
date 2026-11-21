# ADR-009: 存储后端 — 原始自定义分区 + mmap/XIP（上游 A/B + CRC 原样）

> 状态：已接受（阶段 4 P4-B 入口决策，P4-B Task 1）｜ 日期：2026-11-21
> 关联：spec `docs/superpowers/specs/2026-11-21-p4b-storage-presets-design.md`
> §2/§3.1/§3.2/§3.4/§4/§5/§8（本 ADR 是其中存储后端选型的定稿）；
> ADR-004（vendored 只读）、ADR-005（导航/容量）、ADR-008（LVGL 池预算）；
> AGENTS §5.1（脚本）、§5.5（门禁）、§8（上游跟进）；
> 上游代码 `third_party/sloop-fm1/firmware/src/{storage.c,project.c,upreset.c,fm6_bank.c}`

## 背景

P4-B 之前本仓库没有任何持久化：`sloop_hal_tab5/src/storage_ram.c` 只有 4 个用户采样槽的
PSRAM 缓冲，`partitions.csv` 的 7.75MB FAT 分区未挂载、无数据。P4-B 要让独立模式拥有与
FM-1 上 SLOOP 等价的持久化：工程 A–D（RAM 槽 + flash）、自动存档与上电恢复、用户预置
32 槽、FM6 银行 27 槽；P4-E 的设置对象与 P4-D 的采样槽也落在同一套介质上。

上游 `storage.c` 的契约（精读结论）：

- **对象 = 一对 4KB 扇区**。写入顺序：擦非当前副本 → 从偏移 256 起写 payload → **最后**
  写 32B 头（提交记录，含 seq 与 CRC）；加载取「头合法且 seq 最大」的副本；写后回读
  比对，写保护/坏片不得报 `SAVED`；`st_save(obj, NULL, 0)` = 擦除提交。
- **`st_xip` 需要 payload 在 flash 中原地可读**：`upreset.c` 的 `up_bank_find`（`#if
  FELUCCA_FLASH`）与 `fm6_bank.c` 依赖它，省掉 6,160B 的 RAM 镜像。
- **`st_buf` 是主循环 scratch（3,840B）**：擦/写期间 cache 关闭时仍要被访问 → 必须落在
  **内部 SRAM**，不能进 PSRAM。

上游 flash map（FM-1 的逻辑绝对偏移；权威：`third_party/sloop-fm1/firmware/hal/fm1_flash.h`
的 `FL_*` 常量与 `firmware/src/eng_sample.c` 的 USR1–4 基址；本阶段用到
`[0x97000, 0xFF000)` = 416KB，含空洞）：

- `FL_DATA` `[0x97000, 0xE0000)`：projects（`0x97000..0x9F000`）、autosave A（`0x9F000..0xA0000`）、
  用户采样 USR1–3（`0xA0000..0xDC000`，3×80KiB）、用户预置银行（`0xDC000..0xE0000`）；
- `FL_OTA` `[0xE0000, 0xE5000)`：M-UPGRADE loader staging（本项目禁止 FM-1 更新，**不使用**）；
- `FL_FM6` `[0xE5000, 0xE7000)`：FM6 银行 A/B（`0xE5000/0xE6000`）；
- `FL_USR4` `[0xE7000, 0xFB000)`：用户采样 USR4（80KiB）；
- `[0xFB000, 0xFC000)`：1 个**空闲**扇区（`STORE RAWTEST` 的目标，见 §9）；
- `FL_GLOB` `[0xFC000, 0xFF000)`：settings A/B（`0xFC000/0xFD000`）、autosave B（`0xFE000..0xFF000`）。

平台约束（ESP-IDF 5.5.5 / esp32p4）：

- `esp_partition_mmap()` 对任意数据分区可用（64KB MMU 页，内部按页对齐）；P4 的 vaddr
  空间充裕（416KB/64KB = 7 页）。
- `esp_flash_write/erase` 完成后按地址范围失效 cache；擦/写期间 cache 关闭、双核停顿
  （上游注释 ~50ms），音频任务必然停顿——需静音并用单独的 xrun 判据（见下）。

目标函数（fidelity-first，spec §8）：数据语义与上游等价优先。A/B 提交、CRC、回读校验、
XIP 必须原样保留；`third_party/sloop-fm1/**` 零修改。

## 选项

| 选项 | 评估 |
|---|---|
| **A. 原始自定义分区 + mmap + 上游 storage.c 原样（采纳）** | 分区 `sloopstore`（data/0x99）承载上游 flash map 的 `[0x97000, 0xFF000)`；`st_read` = 映射区 memcpy、`st_erase` = `esp_partition_erase_range`（先 codec mute）、`st_prog` = `esp_partition_write`、`ST_XIP_PTR(off)` = 映射基址 + 偏移。**真擦写语义（A/B/torn-write 与硬件一致）、`st_xip` 零成本、上游 4 个 TU 不动逻辑**；代价是一次性分区表调整（FAT 尚无数据） |
| B. FATFS 单文件内 A/B 双槽 | 文件层 A/B 与 storage.c 的 A/B 重复；无 mmap → `st_xip` 必须改成 6KB RAM 镜像（`upreset.c`/`fm6_bank.c` 需改逻辑或另写 shim）；整镜像/扇区写入无原子性，掉电窗口比原始扇区更大；FATFS 元数据损坏面更大 |
| C. NVS 分块存 blob | 无擦除语义（storage.c 的 `st_erase`/空对象语义要另造）；NVS 容量/寿命/键数限制；无 XIP；不选 |

## 决策

**A**（原始分区 + mmap，上游 `storage.c` 原样编入）。落地形态：

### 1. 分区表与 flash map

`partitions.csv` 调整为：

| Name | Type | SubType | Offset | Size | 说明 |
|---|---|---|---|---|---|
| `nvs` | data | nvs | `0x9000` | `0x6000` | 平台级 NVS；**不承载 SLOOP 状态**（见 §6） |
| `phy_init` | data | phy | `0xF000` | `0x1000` | 不变 |
| `factory` | app | factory | `0x10000` | `0x800000` | 单 app，无 OTA A/B（P1-1 姿态不变） |
| `sloopstore` | data | `0x99` | `0x810000` | `0x68000` | 承载上游 flash map `[0x97000, 0xFF000)`；内部偏移 = 上游偏移 − `0x97000` |
| `storage` | data | fat | `0x878000` | `0x788000` | FAT 保留给非 SLOOP 资产；**P4-B–E 不承载 SLOOP 数据** |

- 偏移均 4KB 对齐，`sloopstore` 结束于 `0x878000`，FAT 结束于 `0x1000000`（16MB flash）。
- `sloopstore` 承载的是**重定位后的上游逻辑布局**（FM-1 的 `0x97000..0xFF000` 在 Tab5 上
  与 8MB app 区冲突，不能按绝对地址放置）；HAL 对外仍以**上游绝对偏移**为参数：
  `base = 0x97000` 是逻辑基址，`span = 0x68000`，分区内偏移 = `off − base`。
- `partitions.csv` 头注释写死这两个分区的偏移与映射关系，不得移动 `sloopstore`。
- 分区表变更需 `scripts/build.sh --clean`；`sloopstore` 首次写入前由 `storage.c` 自行擦除，
  **不需要**整片 erase-flash；FAT 当前无数据，重排无迁移成本。
- P4-D 的用户采样按上游原样走 USR1–4 原始槽（USR1–3 `0xA0000..0xDC000`、USR4
  `0xE7000..0xFB000`，均在 `sloopstore` 内），不依赖 FAT；FAT 仅留给非 SLOOP 资产。
- OTA staging（`[0xE0000, 0xE5000)`）虽在 `sloopstore` 覆盖范围内，但属于 FM-1 的升级
  loader 区：本项目禁止 FM-1 更新，**任何 SLOOP 对象都不得写入该区**（只读映射无害）。

逻辑对象与分区内偏移（写入本文作为唯一对照表；区间均为含首不含尾 `[lo, hi)`，A/B
扇区对在说明列注明）：

| 区域 | 上游绝对偏移 | sloopstore 内偏移 | 说明 |
|---|---|---|---|
| projects A–D | `0x97000..0x9F000` | `0x00000..0x08000` | 4 个对象，各一对扇区 |
| autosave A | `0x9F000..0xA0000` | `0x08000..0x09000` | |
| 用户采样 USR1–3 | `0xA0000..0xDC000` | `0x09000..0x45000` | 3×80KiB |
| 用户预置银行 | `0xDC000..0xE0000` | `0x45000..0x49000` | A/B 扇区对 |
| OTA staging | `0xE0000..0xE5000` | `0x49000..0x4E000` | **未使用**（禁止 FM-1 更新） |
| FM6 银行 | `0xE5000..0xE7000` | `0x4E000..0x50000` | A/B 扇区对（`0xE5000/0xE6000`） |
| 用户采样 USR4 | `0xE7000..0xFB000` | `0x50000..0x64000` | 80KiB |
| 空闲（RAWTEST） | `0xFB000..0xFC000` | `0x64000..0x65000` | 1 扇区，见 §9 |
| settings | `0xFC000..0xFE000` | `0x65000..0x67000` | A/B 扇区对（`0xFC000/0xFD000`） |
| autosave B | `0xFE000..0xFF000` | `0x67000..0x68000` | |

### 2. mmap 与 XIP

- `sloop_hal_storage_init()`：`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x99,
  "sloopstore")` → `esp_partition_mmap(part, 0, 0x68000, ESP_PARTITION_MMAP_DATA, &map,
  &handle)`；映射句柄常驻，`sloop_hal_storage_map()` 返回基址。
- `st_read` = 边界检查后从映射区 `memcpy`；`ST_XIP_PTR(off)` = `map + (off - 0x97000)`；
  `FM6_BANK_XIP(copy)` 同法落到 `0xE5000 + copy×4096`。这样上游「从 flash 原地读 payload」
  的语义**零成本保留**，不需要 6,160B RAM 镜像，也不改 `upreset.c`/`fm6_bank.c`。
- mmap 失败（找不到分区/映射越界）→ `ready=false`，所有上层 API 按「空存储」返回：
  UI 显示 `NO STORAGE`、console `STORE ERR no_storage`；引擎与音频照常，不崩溃、不重启。

### 3. 擦写路径、并发与静音

- 擦除 = `esp_partition_erase_range`（4KB 对齐检查）；写入 = `esp_partition_write`；
  两者都包在 codec mute 里（`esp_codec_dev_set_out_mute`，经 `sloop_hal_audio_codec()`；
  音频流未开启时为空操作），完成后 unmute。回读校验由上游 `st_save` 保留。
- **唯一写者 = input 任务**：所有存/读/擦经状态层命令 `CMD_PROJECT_* / CMD_PRESET_* /
  CMD_BANK_*`，UI 只发命令。保持「UI 不直接写引擎/存储」的既有不变量，阶段 5 可把执行端
  换成 FM-1 会话。
- 锁序与上游语义：保存 = 引擎锁内 `proj_capture` → 释放 → `st_save`（擦/写 flash）；
  加载 = 无锁 `st_load + proj_import` → 引擎锁内 `proj_apply(all=1)` + `song.sel`。另设
  `s_store_lock` 串行化存储动作（手动命令、自动存档、console 共用），避免并发擦写。
- 播放中拒写：`song.playing || transport_req` 时返回 `ESP_ERR_INVALID_STATE`（对齐上游
  "STOP BEFORE SAVE"）；所有 `sloop_store_*` 不得在 ISR 调用。
- **只读查询例外**：列表/名称查询（`used/name/engine/count`）允许 UI 直接调用——只读
  mmap、无写、无锁；写操作期间 UI 任务本就被 cache 停顿，不存在半写视图。
- **console 诊断执行器例外**：`STORE …` 命令由 REPL 任务直接调用同一套 `sloop_store_*`，
  与 UI 命令共用 `s_store_lock` + 引擎锁，行为完全相同。理由：`state_apply` 是
  fire-and-forget，无法把返回码回传给脚本，而存储断言需要 `SAVED/EMPTY/playing/…` 这类
  返回值；这与既有的 `ENG`/`PARAM RAW` 同属「诊断执行器」类别，不构成第二条写路径。
- **掉电不破坏旧副本**：设备侧 `STORE TEST` 6 个场景（对象 = `OBJ_SETTINGS`，P4-B 不写它；
  测试前 `st_load` 快照、结束后 `st_save` 恢复）——①两次提交取 seq 大者；②擦除失败 → 旧
  副本可读；③payload 半写失败 → 旧副本可读；④头写失败 → 旧副本可读；⑤回读校验失败 →
  报错不报 `SAVED`；⑥擦除提交（`len=0`）→ 对象为空。故障注入在 HAL 层（第 n 次
  erase/prog 失败；`half` 只写一半），不修改上游逻辑。

### 4. xrun 判据（擦写 stall 是设计内现象）

擦/写期间 cache 关闭、音频任务停顿，`xrun` **必然**增长——沿用「xrun=0」旧门禁会把
正确的存储设计判为失败。判据改为：

- **无存储动作窗口**：`xrun=0`（沿用阶段 2/3 口径）。
- **保存窗口内**：单次保存的 `xrun` 增量 ≤ 16，窗口结束后 `streaming=1`、渲染恢复；
  `STORE STATS` 报告每次保存的 xrun 增量；`scripts/soak.py` 增加显式保存窗口参数
  （`--save-window`），窗口内做一次 SAVE+LOAD。
- 音频栈余量 ≥2KB 与内部堆门禁不变。
- **保存时延预算**：保存动作（4KB 擦 + ≈3.8KB 写 + 回读校验）目标 **≤150ms**；实测值由
  `STORE STATS`/soak 记录。超过预算时先查擦除次数与回读路径，不得改上游 A/B 语义。
- 回退开关：若「codec mute 包住擦写」仍有可闻爆音，改为「停流 → 存 → 启流」（上游
  `sloop_hal_audio_stop/start` 已具备），以人测听感为准；此变更须回写本 ADR。

### 5. 自动存档阈值

沿用上游 `autosave_tick/autosave_resume` 语义，只写 `OBJ_AUTOSAVE`，不碰 A–D：

- 条件（全部满足）：非播放（`!song.playing && !transport_req`）、无输入 ≥ **2.5s**、
  距上次保存 ≥ **20s**、无发声（`audio_quiet()`：合成/鼓 voice 均 inactive）、本轮捕获
  hash 与上次写入不同、存储未忙、未处于诊断冻结。
- 1s 周期任务（优先级 1、栈 3072、内部 RAM；不取 LVGL 锁）执行 tick；`note_activity()`
  由输入派发调用（物理/注入键盘、TAP 命中、成功的 `state_apply` 都算活动）。
- 相同状态不重复写（hash 跳过）→ flash 寿命与上游同量级；`STORE STATS` 暴露写入计数，
  `STORE AUTO ON|OFF` 为测试开关（默认 ON）。
- 启动顺序（`sloop_core_init`）：HAL → 引擎默认 → `selftest_snapshot_init()`（工厂态
  golden 基线）→ `sloop_store_init()`（PSRAM 槽/暂存、mmap、四槽 fetch、`up_boot`、
  `fm6_bank_boot`）→ `sloop_store_autosave_resume()`（`proj_import` + `proj_apply` +
  `proj_foreign_parts`）→ 创建 autosave 周期任务。**snapshot 必须在 resume 之前**，
  否则 `ENG SELFTEST` 的基线会变成用户工程、golden 失配。

### 6. 设置对象归属（P4-E）与 NVS 定位

- **设置/偏好走上游 `OBJ_SETTINGS`**（`0xFC000/0xFD000`，`persist_t`/panel/DSYN kit/MIDI
  LEARN），由 P4-E 按上游格式实施。理由：与 FM-1 备份、编辑器协议对象 id 互通；擦除、
  A/B、CRC 语义与其它对象一致。
- **NVS 不承载 SLOOP 状态**：NVS 无擦除提交语义、无 XIP、容量/键数受限，且无法与
  FM-1 备份互通。NVS 保持 `0x9000/0x6000`，只留给平台级（非 SLOOP 数据模型）用途。
- P4-B 不写 settings 对象，只把它当作 torn-write 测试对象（快照/恢复）。

### 7. 内存：PSRAM 宏重定向与内部 SRAM 边界

上游 `project.c` 的 `proj_slot[4]` 与 `proj_v3_tmp` 定义在 `#ifndef PROJ_HOST` **之上**，
定义 `PROJ_HOST` 不会去掉它们，且 `.noinit` 段无法被 `--gc-sections` 单独回收（另有自有
暂存 `proj_tmp` 与必须留在内部 SRAM 的 `main_scratch`）：

| 实体 | 大小 | 去处 | 方式 |
|---|---|---|---|
| `proj_slot[4]`（4 个 RAM 工程槽） | 15,264B | PSRAM | include 前 `#define proj_slot (*s_slot_ps)`，声明退化为 4B 指针 |
| `proj_v3_tmp`（旧格式转换暂存） | 2,584B | PSRAM | `#define proj_v3_tmp (*s_v3_ps)` |
| 自有 `proj_tmp`（preset bank / fm6 bank / import 暂存） | 4,096B | PSRAM | `#define proj_tmp (*s_stage)`（union 覆盖 `project_v5_t`/`fm6_bank_t`，静态断言） |
| `main_scratch`（上游 `st_buf`） | 3,840B | **内部 SRAM** | 必须能在 cache 关闭期间访问，禁止进 PSRAM |

- 宏包裹的是**整个对象**，上游全部引用形如 `&obj[...]` / `sizeof obj[i]`，替换后取址与
  `sizeof` 语义不变；`third_party/` 零修改（AGENTS §8）。上游若改名/改形，编译期即暴露。
- 实体在 `sloop_store_init` 用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 分配；任一失败
  → `ready=false`（不崩溃）。
- 内部 RAM 新增仅 `main_scratch` 3,840B + 指针/小静态，总计 ≈4.2KB；PSRAM 新增 ≈22KB
  （32MB 充裕）。门禁：`nm`/`idf.py size-components` + 启动 `ENG MEM` 确认三个大实体不在
  DRAM，且 idle 内部堆 ≥61,440B。

### 8. 上游复用边界（vendored 只读）

- 在 `sloop_core` 的单 TU（`src/selftest.c` 末尾 include）内按顺序编入：`storage.c`（原样，
  提供 `st_read/st_erase/st_prog` 三个 hook）、`project.c`（`#define PROJ_HOST`，格式层 +
  `proj_capture/proj_apply`）、`upreset.c`（`#define FELUCCA_FLASH 1`，否则 `up_bank_find`
  永远读空）、`fm6_bank.c`（原样；init 必须调 `fm6_bank_boot()` 安装 `fm6_bank_read` 钩子）。
- 缺失符号（`trk_def_engine/ui_message/ui_say/ui.force/up_gen/sync_reload/flash_ok/
  up_stage/ST_XIP_PTR/FM6_BANK_XIP/arp_is_sounds` 等）全部在 `store_tab5.c` 顶部做薄 shim；
  上游 `!PROJ_HOST` 段（autosave、`proj_foreign_parts`）与 `ui.c` 的预置应用逻辑
  （`apply_preset_to/set_engine_of/TRK_DEF/param_kept/user_of`）**按原语义移植到 adapter**，
  不改 vendored 文件。
- 数据兼容：工程写 `FUN6`、读兼容 `FUN5/4/3/2/1`；用户预置 `UPB1`/`UP_VER 1`；FM6
  `FM6B`/ver 1。**不得改上游记录布局**；阶段 5 的编辑器协议对象 id 与之一一对应。
- `third_party/MANIFEST.sha256` 校验保持 233/233；上游升级只补 `store_tab5.c` 的 shim 清单。

### 9. 失败可见性与诊断面

- `sloop_store_stats_t { ready, objects_ok, faults_armed, faults_taken, autosave_* }` +
  `STORE STATS`（`ready/base/span/map/objects/faults/autosave_*`）是现场核查入口。该结构体
  在 spec §4.2 的定义上增加 `autosave_writes/autosave_hash/autosave_on`（P4-B Task 6；
  spec §4.2 已同步）。
- 未就绪（分区缺失/mmap 失败/PSRAM 分配失败）时：只读 API 返回空/空槽，写 API 返回
  `ESP_ERR_INVALID_STATE`；UI 状态行 `NO STORAGE`；console `STORE ERR no_storage`。
- `STORE RAWTEST`：在空闲逻辑扇区 **`0xFB000`**（`sloopstore` 内偏移 `0x64000`）执行
  erase → 写 0x5A 模式 → 校验 → 再 erase。该参数是**逻辑 flash-map 偏移**（与 `STORE`
  其它偏移、`ST_XIP_PTR` 同口径，HAL 内部按 `off − 0x97000` 映射到分区），**不是芯片
  绝对地址**；OTA staging（`[0xE0000, 0xE5000)`）不属于 SLOOP 数据区，测试不得触碰。
- 写失败（擦/写/回读不符）返回 `ESP_ERR_NOT_FINISHED`，RAM 槽与旧 flash 副本保持可读
  （与上游一致）；不静默报成功。

## 后果

- **收益**：A/B + CRC + 回读 + torn-write 语义在真机上成立（不是文件层模拟）；`st_xip`
  零成本；上游四个 TU 逻辑零修改；416KB 覆盖 P4-B–E 全部对象；设置对象与 FM-1 备份/编辑器
  互通；FAT/NVS 不再与 SLOOP 数据模型纠缠。
- **代价与风险**：
  - 一次性分区表变更（FAT 无数据，可安全重排）；需 `build.sh --clean`，回滚 = 重烧。
  - 擦/写期间 cache 关闭 → 音频停顿时长 ≈ 上游量级；xrun 门禁改为「窗口外 0、窗口内
    ≤16 且恢复流式」，并为可能的听感问题预留「停流→存→启流」回退开关。
  - PSRAM 宏重定向依赖上游对象名与访问形态（`&obj[...]`/`sizeof`）；上游升级时由编译期
    暴露，属可接受的可控耦合。
  - flash 寿命：沿用上游阈值（≥20s、仅静默且 hash 变化时写），写入计数可见，soak 观察。
  - mmap 失败时功能降级（无存储）而非崩溃；UI/console 有明确令牌。
- **被否决方案**：FATFS 文件内 A/B（重复 A/B、丢 XIP、掉电窗口更大）、NVS blob（无擦除
  语义、无 XIP、不互通）。本文取代 phase-4 spec §4.3 的「FATFS 分区文件内 A/B 双槽 + CRC」
  措辞：语义完全保留上游，承载介质改为原始分区。
- **验证门禁**（P4-B）：`host-test.sh` 的上游 `storage_test/project_test/upreset_test/
  fm6_test` 全绿；设备 `STORE TEST PASS 6/6`；工程往返 hash（`STORE LIVEHASH` ↔
  `STORE PROJ HASH`）+ `STATE SNAP` 逐项一致；旧格式 FUN5/4/1 fixture 经 `STORE RAW` 上传
  后字段逐项比对；自动存档/上电恢复；soak 带保存窗口；`ENG SELFTEST` 182/182 不回退
  （存储不触碰 golden）。
- **后续归属**：settings（P4-E，`OBJ_SETTINGS`）、sections/song（P4-C）、采样槽上传与
  FM6/DSYN 编辑面（P4-D）沿用本 ADR 的分区、mmap 与写入纪律，不再另立后端。
