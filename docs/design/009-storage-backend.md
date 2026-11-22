# ADR-009: 存储后端 — 原始自定义分区 + mmap/XIP（上游 A/B + CRC 原样）

> 状态：已接受（阶段 4 P4-B 入口决策，P4-B Task 1；P4-C Task 6 自动存档、Task 7 PER3 歌曲
> 设置与整扇区安全测试落地）｜ 日期：2026-11-21（2026-11-22 追加 §3/§6/§8）
> 关联：spec `docs/superpowers/specs/2026-11-21-p4b-storage-presets-design.md`
> §2/§3.1/§3.2/§3.4/§4/§5/§8（本 ADR 是其中存储后端选型的定稿）；
> ADR-004（vendored 只读）、ADR-005（导航/容量）、ADR-008（LVGL 池预算）、
> ADR-010（P4-C arranger 单 TU、song_keep 与 PER3 子集）；
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
  `FM6_BANK_XIP(copy)` = `ST_XIP_PTR(0xE5000 + copy×4096 + **256**)`——payload 从扇区偏移
  `ST_PAYLOAD_OFF = 256` 起（32B 提交头之后），与上游 `fm6_bank.c` 的兜底
  `fm1_xip_ptr(st_sector(OBJ_FM6BANK, copy) + ST_PAYLOAD_OFF)` 同义。**少的这 256 字节
  是一个错位视图**：槽 k 读到别的槽、写路径从错位副本起笔把提交头/留空区的 0xFF 带进
  payload，整个银行校验失败读成空（Task 7 的 GREEN 首轮实测，ADR §8 同款修正）。这样
  上游「从 flash 原地读 payload」的语义**零成本保留**，不需要 6,160B RAM 镜像，也不改
  `upreset.c`/`fm6_bank.c`。
- mmap 失败（找不到分区/映射越界）→ `ready=false`，所有上层 API 按「空存储」返回：
  UI 显示 `NO STORAGE`、console `STORE ERR no_storage`；引擎与音频照常，不崩溃、不重启。

### 3. 擦写路径、并发与静音

- 擦除 = `esp_partition_erase_range`（4KB 对齐检查）；写入 = `esp_partition_write`；
  两者都包在 codec mute 里（`esp_codec_dev_set_out_mute`，经 `sloop_hal_audio_codec()`；
  音频流未开启时为空操作），完成后 unmute。回读校验由上游 `st_save` 保留。
- **唯一写者 = input 任务**：所有存/读/擦经状态层命令 `CMD_PROJECT_* / CMD_PRESET_* /
  CMD_BANK_*`，UI 只发命令。保持「UI 不直接写引擎/存储」的既有不变量，阶段 5 可把执行端
  换成 FM-1 会话。
  **落地（Task 10）**：`CMD_PROJECT_SAVE/LOAD/ERASE{a=slot}`、`CMD_PRESET_STORE/LOAD/
  ERASE{a=slot, b=track（仅 LOAD）}`、`CMD_BANK_PUT/ERASE{a=slot}`。槽号/轨号越界在状态层
  直接拒绝（**不调存储 API、不改 `store_status`**——命令无效不等于存储失败）；返回码映射
  `app_state_t.store_status`（0 idle/1 ok/2 empty/3 playing/4 error/5 drum；`ESP_ERR_INVALID_ARG`
  只有目标为鼓轨时映射 drum：选中鼓轨的 preset SAVE、LOAD 的 `b==3`、选中鼓轨的 bank PUT
  （该路径连 `bank_current` 都不调），其余为 error）。**成功即整份刷新投影**（一次窗口快照 +
  一次参数回读，与 `state_sync_from_engine` 同口径）——工程/预置载入后 UI 立刻看到新的
  选中轨/参数/图案；失败不改投影。存储调用在状态锁内（单写者串行化），UI 读副本最多等
  一次擦写（≤150ms；§4 的 xrun 判据不变）。`store_status` 的 3 = `ESP_ERR_INVALID_STATE`
  **同时**涵盖「播放中」与「存储未就绪」（§9 的 `NO STORAGE`）——需要区分时调用方必须另查
  `sloop_store_ready()`；drum 在预置命令里由存储层先判就绪（未就绪回 3），在 bank PUT 里由
  状态层先判（未就绪也回 5）。
- 锁序与上游语义：保存 = 引擎锁内 `proj_capture` → 释放 → `st_save`（擦/写 flash）；
  加载 = 无锁 `st_load + proj_import` → 引擎锁内 `proj_apply(all=1)` + `song.sel`。另设
  `s_store_lock` 串行化存储动作（手动命令、自动存档、console 共用），避免并发擦写。
  **用户预置同理**（Task 4）：SAVE = 引擎锁内捕获 `up_rec_t`（`preset_capture`）→ 释放 →
  `up_put` 擦写；LOAD = `up_load_to` 在引擎锁内（纯 RAM，无 flash）；ERASE = 引擎锁内清
  `trk[].user` 引用 → 释放 → `up_put(k, NULL)`。**擦写期间绝不持引擎锁**——本板音频渲染
  就持引擎锁（`sloop_core_render`），持锁 50ms 等于必然 xrun。
- 播放中拒写：`song.playing || transport_req` 时返回 `ESP_ERR_INVALID_STATE`（对齐上游
  "STOP BEFORE SAVE"）；所有 `sloop_store_*` 不得在 ISR 调用。
- **只读查询例外**：列表/名称查询（`used/name/engine/count`）不取引擎锁、不写任何介质
  （只读 mmap）；但**取 `s_store_lock`**——它们要先重扫银行索引（见 §8 的 `.bss` 窗口
  说明），且一次查询内不得看到并发的擦写中视图。代价要说清：该锁在保存/擦除期间是
  **跨越 flash 擦写**持有的，故读者最坏会排在一次擦写之后（一次保存 ≈4KB 擦 + 3.8KB
  写 + 回读校验，目标 ≤150ms，见 §4）。这是有意的取舍：宁可让查询等，也不读半写副本；
  UI 在写操作期间本就被 cache 停顿。Task 4 实测（`store` 场景每次 LIST 33 次重扫仍在
  毫秒内）。
- **console 诊断执行器例外**：`STORE …` 命令由 REPL 任务直接调用同一套 `sloop_store_*`，
  与 UI 命令共用 `s_store_lock` + 引擎锁，行为完全相同。理由：`state_apply` 是
  fire-and-forget，无法把返回码回传给脚本，而存储断言需要 `SAVED/EMPTY/playing/…` 这类
  返回值；这与既有的 `ENG`/`PARAM RAW` 同属「诊断执行器」类别，不构成第二条写路径。
- **掉电不破坏旧副本**：设备侧 `STORE TEST` 6 个场景（对象 = `OBJ_SETTINGS`；测试前
  **快照两个完整 4KB 副本扇区**、结束后逐字节恢复并校验，P4-C Task 7 起替换 P4-B 的
  payload-only 快照）——①两次提交取 seq 大者；②擦除失败 → 旧
  副本可读；③payload 半写失败 → 旧副本可读；④头写失败 → 旧副本可读；⑤回读校验失败 →
  报错不报 `SAVED`；⑥擦除提交（`len=0`）→ 对象为空。故障注入在 HAL 层（第 n 次
  erase/prog 失败；`half` 只写一半），不修改上游逻辑。
  - **落地要点（Task 8）**：HAL 的 arm 期间**读也失败**（不消耗写序号）⇒ `st_save` 开头的
    `st_current` 看不到有效副本、写目标恒为 copy A、seq 从 0 起算。故每个失败场景先
    `sloop_hal_storage_erase` 擦净两个扇区，再按 v1→A（cur=-1）、v2→B（cur=0）提交，arm
    前断言 v2 逐字节可读——旧副本先在 B，失败写才只碰 A。若省掉这一步，当前副本在 A 时
    失败的擦除会把「旧副本」一起毁掉，测试变成验一个被自己毁掉的对象。
  - **故障命中的操作必须可判**：`faults_taken` 只说明某次写失败，分不出 erase 与 prog。
    故 arm 后读回目标扇区的**原始字节**（失败时扇区可能只有半个 payload，不能走头校验）：
    erase 失败 ⇒ A 的头+payload 与 arm 前逐字节相同；payload 半写 ⇒ A 的头为擦除态、payload
    前 32B = 新值其余 0xFF；头写失败 ⇒ payload 完整、头为擦除态。三个版本逐字节不同，
    「旧副本可读」的断言不给新旧值相同的假绿。
  - 全程持 `s_store_lock`（自动存档/歌曲 tick 的 trylock 跳过），结束后先 `fault_clear`
    再恢复快照；未就绪/走带中回 `ESP_ERR_INVALID_STATE`（console
    `STORE ERR no_storage|playing`）。快照读取失败以 `fail_case = "snapshot-settings"`、
    恢复或逐字节校验失败以 `"restore-settings"` 归入失败（不静默）。
  - **快照 = 两个完整 4KB 副本扇区（P4-C Task 7 收口评审 R26）**：`st_load` 只搬对象的
    `len` 字节，不含副本扇区 64..255 的 side record（`ST_SIDE_OFF=64`、`ST_SIDE_MAX=192`；
    `st_save3` 在 payload 之后、提交头之前写入，随副本提交；`st_side_load` 读当前副本）
    与未知尾段。自 P4-C 起 settings 有真实写入者（PER3 歌曲 + side record），只恢复 payload
    会在一次 `STORE TEST` 里静默丢掉主题/收藏；故套件改为：`sloop_hal_storage_read` 两个
    扇区原始字节 → 六场景 → 每扇区擦一次 + 按 256B 块原样写回（含 0xFF 区）→ 再逐字节
    读回校验。这同时覆盖 arrangement（payload 48..83）、DSU/MIDI 尾段与 side record，
    即「六场景不破坏歌曲设置」的判据本身。8KB 快照是诊断路径的一次性 PSRAM 分配。

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
- 相同状态不重复写（hash 跳过）→ flash 寿命与上游同量级；`STORE STATS` 暴露
  `autosave=<0|1> autosave_hash=<8hex> autosave_writes=<n>`，`STORE AUTO ON|OFF` 为测试
  开关（默认 ON；关掉只停 tick 的写，开机恢复照常）。resume 把读回的 hash 记下 ⇒ 开机第一拍
  不会把刚读回的东西原样写一遍（实测：复位后再等过 boot 的 20s gap，`autosave_writes` 仍 0）。
- **与 `ENG SELFTEST` 的交互（已知，DI-20，机制推导）**：自检的 `selftest_restore()` 把
  `.data/.bss` 窗口与 DSP 状态回滚到 init 快照（引擎 = 工厂态），而 `s_auto_hash` 在
  `.noinit`（有意不被回滚）——两者不同 ⇒ 自检后的第一个窗口会把工厂态写进 autosave。
  诊断命令不是用户动作，但会覆盖用户上次的工作工程；修法留 P4-E（DI-20 给了三个方向），
  门禁不受影响（脚本显式控制 `STORE AUTO`，且每次硬复位后重新下发）。
- 启动顺序（`sloop_core_init`）：HAL → 引擎默认 → `selftest_snapshot_init()`（工厂态
  golden 基线）→ `sloop_store_init()`（PSRAM 槽/暂存、mmap、四槽 fetch、`up_boot`、
  `fm6_bank_boot`；**不建任务**）→ `sloop_store_autosave_resume()`（`proj_import` +
  `proj_apply` + `proj_foreign_parts`）→ `sloop_store_autosave_start()`（1s 周期任务）。
  两条顺序都不可倒：**snapshot 必须在 resume 之前**（否则 `ENG SELFTEST` 的基线会变成
  用户工程、golden 失配）；**任务必须在 resume 之后**（先起任务的那一拍会把工厂态写进
  autosave，覆盖刚读回的工作工程）。`STORE AUTO OFF` 只是运行期诊断开关，**不跨复位**
  （每次上电 init 都回到默认 ON）。
- **落地形态（Task 6，`store_tab5.c` 的「自动存档与上电恢复」小节）**：捕获/恢复暂存
  `s_auto`（`project_t`，3,816B，PSRAM，与 `proj_tmp` 分开）与全部状态
  （`autosave_hash/ms/checked/writes`、`last_input_ms`、开关、`busy`）在 `.noinit`
  ——本 TU 的 `.bss` 在 `ENG SELFTEST` 的恢复窗口里，清零后自检结束的第一拍就会把工厂态
  写回 autosave、覆盖用户工程。tick 在**一把引擎锁内**完成 `proj_capture` + 走带/发声
  （`audio_quiet()`：合成/鼓 voice 均 inactive）判定，擦写 `st_save` 在锁外；拿不到存储锁
  （`s_busy`/trylock）就跳过本拍，不排队。上游没有的 `rec_wait/ft_on/ui.menu` 换成
  「存储忙」与「诊断冻结（`s_frozen`）」两项。启动时 `st_load` 无有效副本 →
  `ESP_ERR_NOT_FOUND`（工厂态开始，不报错）。`autosave_ms` 从 0 起算 ⇒ **开机首个写只能落在
  uptime ≥20s 之后**（上游同），所以「空闲 2.5s 就写」只在已有写入之后成立。

### 6. 设置对象归属（P4-C 歌曲子集 / P4-E 完整设置）与 NVS 定位

- **设置/偏好走上游 `OBJ_SETTINGS`**（`0xFC000/0xFD000`，`persist_t`/panel/DSYN kit/MIDI
  LEARN）。P4-C 按 **ADR-010 §4** 落地了 arrangement 所需的最小 PER3 读写与 PER1/PER2
  迁移（下表）；完整 panel/theme/lights/MIDI 设置 UI 仍归 P4-E。理由：与 FM-1 备份、
  编辑器协议对象 id 互通；擦除、A/B、CRC 语义与其它对象一致。
- **NVS 不承载 SLOOP 状态**：NVS 无擦除提交语义、无 XIP、容量/键数受限，且无法与
  FM-1 备份互通。NVS 保持 `0x9000/0x6000`，只留给平台级（非 SLOOP 数据模型）用途。
- P4-B 当时不写 settings，只把它当作 torn-write 测试对象；自 P4-C 起该旧边界不再适用。

#### 6.1 PER3 歌曲子集的落地形态（P4-C Task 7）

字节层全部在 `firmware/components/sloop_core/src/song_persist.h`（纯函数，**不碰 flash**，
由 `scripts/persist-host-test.c` 同源编译）；flash/锁/时间戳在 `store_tab5.c` 的
「PER3 歌曲与延迟写」小节。

| 项 | 值 |
|---|---|
| payload 布局 | `persist_t` 88B（magic@0 / palette@4 / lowcut@8 / zoom@12 / panel@16 / **arrangement@48（36B）** / lights@84）+ `dsu_bank_t` 1,464B @88 |
| canonical | 88 + 1,464 = **1,552B**，默认 MIDI 尾段省略；默认 80B side（theme 0 + `FAV1` + 16×0xFFFF + CRC） |
| 合法 PER3 长度 | 84（2.2，无 lights）/ 88（无 DSU）/ 1,552 / 1,584 / 1,588 / 1,592（`2*ML_N+8=40` 的完整尾段，32/36 是 2.6 测试长度） |
| 保存规则 | 无对象 → canonical；合法 PER3 → 只替换 48..83，其余（含 DSU/MIDI 尾段）逐字节保留，缺 DSU 的补默认；PER2（48B）/PER1（40B）→ 按 `persist_boot` 口径迁移字段后写 canonical |
| 拒绝 | 未知 magic 或长度不是历史格式 ⇒ `ESP_ERR_INVALID_ARG`（`STORE ERR invalid`），原扇区一个字节不动，dirty 保留 |
| side record | 侧区**有内容就原样**随新 A/B 副本提交（含自校验不过的 2.7 beta「只有 40B 主题」记录——本固件与 P4-E 之前没有主题状态，覆盖等于删用户主题）；全 0xFF（P4-B 及更早从未写过）⇒ 写默认 80B 记录 |
| 延迟写 | `sloop_store_sections_tick`（1s 任务，esp_timer 毫秒）：停止 + 无发声 + 输入空闲 ≥1.5s + 距上次尝试 ≥5s。`arrangement_save` 同义入口 `sloop_store_song_save()` 立即写（播放中 `playing`） |
| dirty | `.noinit` 的 `song_pending_t{dirty,gen}`：**写成功且期间无新修改**才清；失败保留、下个窗口重试（上游在写前就清，被此修正替代） |
| SELFTEST | `arrangement` 是 `arranger.c` 的 static `.bss`，自检的 restore 会把它回滚到默认 ⇒ 自检结束后 `sloop_store_song_reload()` 从 settings 重读（以 flash 为准、清 dirty；对象损坏则不覆盖 RAM） |
| 不做 | `arrangement_enabled`（歌曲模式）、quick chain、SONG REC 临时态都不持久化（上游同）；sections A–D 的内容仍是 FUN6 工程对象（`st_save(OBJ_PROJECT0+s)`），其 dirty 门限由 Task 8 的 section 命令接入同一 tick |

- 新增 `.noinit` 状态：`s_song`（5B）、`s_song_tried_ms`、`s_input_ms`、`s_song_refused`
  ——本 TU 的 `.bss` 在 `ENG SELFTEST` 的快照窗口里，dirty/时间戳留在 `.bss` 会被一次自检
  清掉。PER3 的 payload 组装复用 `proj_tmp`（4,096B PSRAM，与 preset/import/RAW 同一块，
  全程持存储锁），不新增常驻缓冲。

### 7. 内存：PSRAM 宏重定向与内部 SRAM 边界

上游 `project.c` 的 `proj_slot[4]` 与 `proj_v3_tmp` 定义在 `#ifndef PROJ_HOST` **之上**，
定义 `PROJ_HOST` 不会去掉它们，且 `.noinit` 段无法被 `--gc-sections` 单独回收（另有自有
暂存 `proj_tmp` 与必须留在内部 SRAM 的 `main_scratch`）：

| 实体 | 大小 | 去处 | 方式 |
|---|---|---|---|
| `proj_slot[4]`（4 个 RAM 工程槽） | 15,264B | PSRAM | include 前 `#define proj_slot (*s_slot_ps)`，声明退化为 4B 指针 |
| `proj_v3_tmp`（旧格式转换暂存） | 2,584B | PSRAM | `#define proj_v3_tmp (*s_v3_ps)` |
| `song_keep`（SONG 前的工作 loop，P4-C ADR-010） | 3,816B | PSRAM | `#define song_keep (*s_song_keep_ps)`；指针为 `.noinit`，仅 init 分配 |
| 自有 `proj_tmp`（preset bank / fm6 bank / import 暂存） | 4,096B | PSRAM | `#define proj_tmp (*s_stage)`（union 覆盖 `project_v5_t`/`fm6_bank_t`，静态断言） |
| `main_scratch`（上游 `st_buf`） | 3,840B | **内部 SRAM** | 必须能在 cache 关闭期间访问，禁止进 PSRAM |

- 宏包裹的是**整个对象**，上游全部引用形如 `&obj[...]` / `sizeof obj[i]`，替换后取址与
  `sizeof` 语义不变；`third_party/` 零修改（AGENTS §8）。上游若改名/改形，编译期即暴露。
- 实体在 `sloop_store_init` 用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 分配；任一失败
  → `ready=false`（不崩溃）。
- 内部 RAM 新增仅 `main_scratch` 3,840B + 指针/小静态，总计 ≈4.2KB；PSRAM 在 P4-B
  ≈22KB 基础上增加 `song_keep` 3,816B（32MB 充裕）。门禁：`nm`/`objdump`/
  `idf.py size-components` + 启动 `ENG MEM` 确认各大实体不在 DRAM、`s_song_keep_ps`
  仅为 4B `.noinit` 指针，且 idle 内部堆 ≥61,440B。

### 8. 上游复用边界（vendored 只读）

- 在 `sloop_core` 的单 TU（`src/selftest.c` 末尾 include）内按顺序编入：先在
  `regress.c → hostsim.c` 前定义 `FELUCCA_ARRANGER=1`，保证 `arranger.c → seq.c`，并在
  进入存储适配前撤销宏；随后是 `storage.c`（原样，提供 `st_read/st_erase/st_prog` 三个
  hook）、`project.c`（`#define PROJ_HOST`，格式层 + `proj_capture/proj_apply`）、显式
  `arranger_scene.c`（ADR-010，场景应用/section bars/SONG 工作 loop）、`upreset.c`
  （`#define FELUCCA_FLASH 1`，否则 `up_bank_find` 永远读空）、`fm6_bank.c`（原样；init
  必须调 `fm6_bank_boot()` 安装 `fm6_bank_read` 钩子）。宏不得漏进 `project.c` 而拉出
  `!PROJ_HOST` settings/旧 flash 路径。
- 缺失符号（`trk_def_engine/ui_message/ui_say/ui.force/up_gen/sync_reload/flash_ok/
  up_stage/ST_XIP_PTR/FM6_BANK_XIP/arp_is_sounds` 等）全部在 `store_tab5.c` 顶部做薄 shim；
  上游 `!PROJ_HOST` 段（autosave、`proj_foreign_parts`）与 `ui.c` 的预置应用逻辑
  （`apply_preset_to/set_engine_of/TRK_DEF/param_kept/user_of`）**按原语义移植到 adapter**，
  不改 vendored 文件。
- **换引擎/预置的锁与写值形态（Task 5）**：`apply_preset_to` 在 FM-1 上由 `fm1_irq_off/on`
  保护，Tab5 上改为**调用方持引擎锁**——`preset_apply_locked/set_engine_of_locked` 是
  `*_locked` 变体，公共入口（`sloop_core_track/apply_preset/set_engine`）与
  `sloop_store_project_load` 在同一次取锁内调它们（mutex 不可重入，绝不在锁内再取）。
  上游的「纯写值」抽成 `preset_values`，应用与 `sloop_core_preset_preview`（无副作用）
  共用同一实现，故设备侧可以用 `ENG PRESET` 的 71 项向量逐项判等
  `ENG TRACK` 之后的 p[]。`project_load` 另补上游 `project_apply` 的后两步：格式 1 的
  `preset == 0xFF` 轨取默认声音 + 清图案、`proj_foreign_parts`（未知引擎 → 该轨默认
  引擎+预置；Task 6 的 autosave_resume 复用同一函数）。
- 数据兼容：工程写 `FUN6`、读兼容 `FUN5/4/3/2/1`；用户预置 `UPB1`/`UP_VER 1`；FM6
  `FM6B`/ver 1。**不得改上游记录布局**；阶段 5 的编辑器协议对象 id 与之一一对应。
- **`.bss` 与 SELFTEST 快照窗口（Task 4 实测）**：`selftest.c.obj` 的 `.bss` 被
  `linker.lf` 的 `SURROUND(sloop_selftest_bss)` 包住，`ENG SELFTEST` 的 restore 会把
  整个窗口回滚到 init 之前的快照。因此本 TU 内的**功能性**静态一律放 `.noinit`
  （`s_v3_ps/s_slot_ps/s_stage/s_song_keep_ps`、锁与哨兵，以及 shim 的 `flash_ok`）；上游 `upreset.c`
  的银行索引 `up_bank_xip[2]`（普通 `.bss`，vendored 声明无 section 属性、宏重定向也搬
  不走）只能**每次公共预置入口在存储锁内重扫** `up_boot()`（两次头校验 + 3,080B CRC，
  微秒级）。**写入口也必须重扫**：`up_put` 把整银行重写一遍、从当前 XIP 副本起笔，索引为
  NULL 时会静默抹掉同银行里已用的其它槽（设备实测：selftest 后直接 SAVE 1 丢掉槽 0，
  `4-pb-2026-11-21/task-4-saveafter-red-run.txt`）。**FM6 银行有同型风险，Task 7 裁决
  (a)**：`fm6_bank.c` 的 `fm6_bank_cur/fm6_bank_used` 与 `eng_fm6.c` 的 `fm6_bank_read`
  钩子也是普通 `.bss`（同一快照窗口）——restore 之后银行读成空、`fm6_bank_put` 从空副本
  起笔会抹掉同银行其它槽（与预置两个症状完全同型）。**每个公共入口（used/get/put/erase）
  在 `s_store_lock` 内重跑 `fm6_bank_boot()`**（两次头校验 + 3,472B CRC，微秒级），不给
  vendored 声明加 section 属性（ADR-004 零修改）；写入口是必须的，读入口顺带让「刚写下
  的槽立刻可见」（与预置同一取舍）。判别口是 `store` 场景的 ⑫d：自检后**紧跟**
  `STORE BANK PUT 5`（中间无银行读）⇒ LIST 仍 B3+B4+B5（不重扫时只剩 B5）。另一个已知
  的良性竞态：`fm6_bank_put` 末尾的 `fm6_bank_changed()`（vendored）直接写 `fm6_slot[]`，
  而本板的 `fm6_poll` 在渲染路径的引擎锁内——这次字节写落在锁外（擦写不能持引擎锁），
  最坏结果是某一轨这次没被标脏、继续用 `fm6_patch` 里的旧声音，下次 PTCH 变动即恢复；
  无内存安全风险（详见 `store_tab5.c` 的 bank 小节注释）。实测：`nm` 显示
  `up_bank_xip` 地址落在
  `_sloop_selftest_bss_start..end` 内，`flash_ok` 在窗口之前的 `.noinit`
  （`docs/dev/verify/4-pb-2026-11-21/task-4-memory.txt`）。
  纯展示性 shim（`up_gen/ui.force/sync_reload`）回滚无害，仍留 `.bss`。
- **settings 的 side record 曾不在 `STORE TEST` 的快照范围（评审 R26，P4-C Task 7 已解决）**：
  P4-B 的套件用 `st_load`/`st_save` 只搬 payload，side record（`ST_SIDE_OFF=64..255`）与未知
  尾段会被场景的擦除一并清掉；P4-B 无写入者故当时不可达。P4-C Task 7 在**第一次真实
  settings 写入之前**把快照扩成两个完整 4KB 扇区、结束逐字节恢复并校验（§3），`sloop_store.h`
  的 API 契约与 console `STORE TEST` 帮助已同步。
- `third_party/MANIFEST.sha256` 校验保持 233/233；上游升级只补 `store_tab5.c` 的 shim 清单。

### 9. 失败可见性与诊断面

- `sloop_store_stats_t { ready, objects_ok, faults_armed, faults_taken, autosave_*,
  last_save_xrun, last_save_ms }` + `STORE STATS`（`ready/base/span/map/objects/faults/
  autosave_* last_xrun last_save_ms`）是现场核查入口。该结构体在 spec §4.2 的定义上增加
  `autosave_writes/autosave_hash/autosave_on`（P4-B Task 6）与写代价两项
  `last_save_xrun/last_save_ms`（Task 14 终审；每次 save/erase/preset/bank/RAW/autosave 写
  动作的 xrun 增量与耗时，spec §5 的 ≤150ms 口径）。
- 未就绪（分区缺失/mmap 失败/PSRAM 分配失败）时：只读 API 返回空/空槽，写 API 返回
  `ESP_ERR_INVALID_STATE`；UI 状态行 `NO STORAGE`；console `STORE ERR no_storage`。
- **诊断冻结**（`ENG FREEZE` / `ENG SELFTEST` 期间）：所有写/载入入口在改状态前返回
  `ESP_ERR_INVALID_STATE`；此时存储本身 ready，console 映射为 `STORE ERR playing`
  （`store_err()` 只在 !ready 时才报 `no_storage`）。
- **空槽擦除**：`PROJ ERASE` 对 RAM 槽无效的槽**不写 flash**、回 `ESP_ERR_NOT_FOUND`
  （console `STORE ERR empty`）——上游 `project_erase` 的 `EMPTY SECTION` 早退；UI 状态行
  显示 `EMPTY`。
- `STORE RAWTEST`：在空闲逻辑扇区 **`0xFB000`**（`sloopstore` 内偏移 `0x64000`）执行
  erase → 写 0x5A 模式 → 校验 → 再 erase。该参数是**逻辑 flash-map 偏移**（与 `STORE`
  其它偏移、`ST_XIP_PTR` 同口径，HAL 内部按 `off − 0x97000` 映射到分区），**不是芯片
  绝对地址**；OTA staging（`[0xE0000, 0xE5000)`）不属于 SLOOP 数据区，测试不得触碰。
- 写失败（擦/写/回读不符）返回 `ESP_ERR_NOT_FINISHED`，RAM 槽与旧 flash 副本保持可读
  （与上游一致）；不静默报成功。
- **旧格式 fixture 的入设备通道（Task 9）**：`STORE RAW BEGIN <len≤3,840> | <hex> | END
  <obj> <slot> | ABORT` 把任意对象 payload 分块搬进 PSRAM 暂存（3,840 = `ST_PAYLOAD_MAX`，
  即一个 flash 对象能写的 payload 上限；超限在 BEGIN 当场回 `size`，不会等到 END 才回
  `save`），`END` 以 `st_save` 原样写入**请求的那一个对象**（`project` →
  `OBJ_PROJECT0 + slot`；工程槽的 RAM 缓存随后按开机 fetch 的语义重新 `proj_import`）。
  写操作持存储锁、不取引擎锁、播放中拒绝。**暂存与 `up_stage()`/`proj_tmp` 是同一块
  4,096B**：任何其它 `STORE` 子命令入口都先 abort 未完成的上传（R27；穿插命令后 END 回
  `invalid`、不落盘），`sloop_store_init` 也把 RAW 状态归零（`.noinit` 冷启动垃圾）。一行 ≤240 hex = 120B：
  真正的上限是 **USB-Serial-JTAG 驱动 RX 环 256B**（整行须一次入环；设备忙时溢出会静默丢
  字符，Task 9 实测 490 字符的行被截到 256），不是 console 行缓冲。
- `STORE PROJ FIELDS n` 是 `st_load` + `proj_import` 的**只读投影**（不动引擎/RAM 槽），按固定
  顺序打印 fmt/sel/bpm/swing/dust/drdly + 4 轨 engine/preset/P_LEVEL/P_ATK/P_E0/P_E1 + 前 8 步，
  供 `scripts/gen_project_fixtures.py` 的 `expected.json` 逐项比对（FUN5/FUN4/FUN1 的设备侧
  兼容门禁）。空槽/空提交 = `empty`，对象存在但任何已知格式都不匹配（截断/损坏）= `invalid`
  ——`project_load` 用同一口径（导入失败时引擎从未被 `proj_apply`）。**CRC 损坏的对象与
  空槽同口径回 `empty`**（`st_load` 读不出「头合法 + CRC 对」的副本），不从坏字节里猜字段。
- **测试专用诊断**：`STORE PROJ SYNTH <fmt1|foreign>`（`sloop_store_project_synth`）
  以当前引擎状态捕获内存工程、改写出 `project_load` 的两条 fallback 边界（格式 1 的
  `preset == 0xFF`、引擎号 ≥ NENGINES）、存到**诊断槽 3**（破坏性；槽 0–2 不碰）、清
  RAM 槽强制 flash 回落（`proj_import` 校验 FUN6 序列化保留 `0xFF`/越界引擎号），再走
  公共 LOAD 并回读各轨 `engine/preset/steps/atk/e0`。它是这两条分支在真机上的唯一
  覆盖入口（`project_load` 的 fallback 在正常使用中不可达）。

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
- **后续归属**：P4-C 的 sections/song 与最小 PER3 设置读写见 ADR-010，完整 settings UI
  （P4-E）、采样槽上传与 FM6/DSYN 编辑面（P4-D）沿用本 ADR 的分区、mmap 与写入纪律，
  不再另立后端。
