# 阶段 4 / P4-B：存储与预置设计规格（P4-B Spec）

> 日期：2026-11-21 ｜ 状态：草案（待项目负责人评审）
> 上位文档：`docs/design/002-port-roadmap.md` §6（P4-B 行，权威）；`docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md` §3/§4.3（P4-B 的框架定义，本 spec 取代其 §4.3 的「FATFS 文件内 A/B」措辞，见 §3.1）；
> ADR-005「导航信息架构」、ADR-008（池预算）、ADR-009（存储后端）；原「9 页参数页框架已达 LVGL 池容量天花板」延迟条目已关闭（决策见 ADR-005「导航决策」）。
> 前置结论：P4-A 已把页签推到 9 张（107px/张）并实测 9 页全建 `pool_free` 2.2–2.7KB；**P4-B 必须先把导航信息架构与容量决策落地**（ADR-005 已登记为 P4-B 决策门）。

## 1. 目标与非目标

**目标**（roadmap §6 P4-B）：让 Tab5 独立模式拥有与 FM-1 上 SLOOP 等价的持久化能力：

1. 工程 A–D 存/读/擦（上游 `FUN6` 格式，读兼容 `FUN5/4/3/2/1`），并按上游语义保留 4 个 **RAM 工程槽**（PSRAM）与开机 fetch；
2. **自动存档与开机恢复**（上游 `autosave_tick/autosave_resume`：停走带、无输入 2.5s、两次保存间隔 20s、无发声；上电回到上次工作工程）；
3. 用户预置 32 槽（存当前选中轨音色 / 载入到任意轨 / 擦除 / 浏览），开机扫描银行（`up_boot`）；
4. FM6 预置银行 27 槽（存/取/擦，按上游 `FM6B` 记录布局），开机扫描（`fm6_bank_boot`）；
5. **工厂预置/鼓 kit 浏览真正改变声音**：移植上游 `apply_preset_to`/`set_engine_of`/`TRK_DEF`/`preset_extras`/`param_kept`/`arp_is_sounds` 语义，修正 `sloop_core_track`（现在只切索引、不载参数——见 §2）；
6. 掉电中断不破坏旧副本（沿用上游 A/B + CRC + 回读校验语义）；
7. 导航：分组页签 + 组内子页签 + **组作用域页面生命周期**，解开 9 页池容量天花板，为 P4-B–E 的新页面让路；
8. 机器可复现的验证入口：console `STORE …` 命令族 + 设备侧 torn-write 故障注入套件 + 注入脚本/截屏。

**非目标（本阶段明确不做）**：

- 上游 settings 对象（`persist_t`/panel/DSYN kit/MIDI LEARN）——归 P4-E；**决策：P4-E 也走 `OBJ_SETTINGS` 上游格式（保证与 FM-1 备份/编辑器互通），NVS 不承担 SLOOP 状态**；
- 编排 sections/song 落盘（`sections_write`、`arranger_scene`）——归 P4-C；
- FM6 patch 编辑面与 DSYN 编辑面——归 P4-D（本阶段只做 bank 的存/取/擦）；
- 采样槽上传/擦除——归 P4-D（本阶段只为上游 USR1–4 原始槽保留布局：USR1–3 `0xA0000..0xDC000`、USR4 `0xE7000..0xFB000`，见 §3.1）；
- 联动模式（编辑器协议的文件类命令）——阶段 5；本阶段的对象 id/格式已按上游对齐，届时可直接复用。

**反目标**：把上游 `project.c` 整文件编入并连带 `ui.c/panel.c`（240×240 UI 与硬件假设）；自造第二套工程/预置格式；在 UI 任务里直接写 flash；用扩大 LVGL 池解决页面容量（ADR-008 已实测 96KB 链接溢出）。

## 2. 现状与差距（本次侦察实证）

| 项 | 现状 | 差距 |
|---|---|---|
| 存储 | 无。`sloop_hal_tab5/src/storage_ram.c` 只有 4 个用户采样槽的 PSRAM 缓冲；`partitions.csv` 有 7.75MB FAT 分区（未挂载、无数据） | 无分区/挂载/读写；无 `st_read/st_erase/st_prog` 后端 |
| 上游存储 TU | `storage.c`（232 行：A/B 双扇区 + 32B 提交头 + zlib CRC + 回读校验 + side record + `st_xip`）、`project.c`（1220 行：FUN1–6 转换、`proj_capture/proj_apply`、autosave、settings/sections）、`upreset.c`（386 行）、`fm6_bank.c`（112 行） | 均未编入（`sloop_core` 单 TU 只含 `regress.c` → `hostsim.c` 的引擎集） |
| 引擎 API | `sloop_core` 有 read/snapshot/step/param/desc/mute/solo 等 | 无「整工程捕获/应用」「预置存/取」「FM6 bank 存/取」入口。**已发现的真缺陷**：`sloop_core_track(k, engine, preset)` 只写 `eng_req/preset` 索引，**不加载预置参数**（上游 `set_engine`/`apply_preset` 走 `apply_preset_to` 把 `p[]` 写成预置值）；音频路径只读 `p[]`，故现在换工厂预置/换引擎不会换声音。P4-B 必须补上 |
| 状态层 | `sloop_state` 有 15 条编辑命令（唯一写者 = input 任务） | 无存储类命令；`app_state_t` 无存储列表投影 |
| UI | 9 张平铺页签；页面「首次访问构建、之后保留」；`app_page_t` 与 `APP_STATE_PAGES` 有静态断言 | 无 FILE 页面；页签宽度在 N=10 时为 95px < `TRANSPORT` 101.6px，加页不可行；页面无销毁路径 |
| 验证 | host `storage_test/project_test/upreset_test/fm6_test` 全绿（`scripts/host-test.sh`） | 设备侧无存储；无 torn-write 故障注入入口 |

上游关键契约（精读结论，`third_party/sloop-fm1/firmware/src/`）：

- **storage.c**：每个对象一对 4KB 扇区；写入 = 擦非当前副本 → 从偏移 256 起写 payload → **最后**写 32B 头（提交记录）；加载取「头合法且 seq 最大」的副本；`st_save` 写后回读比对（写保护/坏片不得报 SAVED）；`st_xip` 需要在 flash 中**原地读** payload（`upreset.c`/`fm6_bank.c` 依赖，省 6,160B RAM）；`st_buf` 用主循环 scratch（3,840B，**必须能在关闭 cache 期间访问 → 内部 SRAM**）。
- **flash map**（逻辑绝对偏移，区间含首不含尾；权威 `hal/fm1_flash.h` 的 `FL_*` 与 `src/eng_sample.c` 的 USR 基址）：`FL_DATA [0x97000,0xE0000)` = projects `0x97000..0x9F000` + autosave A `0x9F000..0xA0000` + USR1–3 `0xA0000..0xDC000` + 用户预置银行 `0xDC000..0xE0000`；`FL_OTA [0xE0000,0xE5000)`（升级 loader staging，本阶段**未使用**，禁止 FM-1 更新）；`FL_FM6 [0xE5000,0xE7000)`（A/B `0xE5000/0xE6000`）；`FL_USR4 [0xE7000,0xFB000)`；空闲扇区 `0xFB000..0xFC000`；`FL_GLOB [0xFC000,0xFF000)` = settings A/B `0xFC000/0xFD000` + autosave B `0xFE000..0xFF000`。本阶段实际用到 `[0x97000, 0xFF000)`（= 416KB，含空洞）。
- **project.c**：`PROJ_HOST` 以上部分（≈554 行）是纯格式层 + `proj_capture/proj_apply`，`tests/project_test.c` 已证明它只需 `core.h + params.c + drums.c + engines + trk_def_engine` 即可编译；`PROJ_HOST` 以下（settings/panel/autosave/sections/arranger/DSYN/MIDI）依赖上游 UI 与硬件，**不整段编入**——其中 **autosave（690–754 行）与 `proj_foreign_parts`（639–647 行）按原语义移植到 adapter**（见 §1 目标 2/5），settings/sections/arranger/DSYN/MIDI 留阶段 4 后续。
- **upreset.c**：`UP_HOST` 以上只有纯记录工具；**写/载逻辑在 `UP_HOST` 以下**，需 `TSEL/sound_change/arp_is_sounds/param_kept/p_lockable/fm6_track_loaded/sync_reload/ui_message/ui_say/ui.force/up_gen/up_stage/flash_ok/st_xip`——前 8 个在 `ui.c/seq.c`，未编译，需薄 shim；`up_stage` 由 project.c 在 `FELUCCA_FLASH` 下提供，`PROJ_HOST` 下没有，需我们定义。**`up_bank_find` 在 `#if FELUCCA_FLASH` 内才走 `st_xip` → TU 必须 `#define FELUCCA_FLASH 1`，并在 init 调 `up_boot()`**；否则预置永远读为空。
- **fm6_bank.c**：由 `project.c` 在 `FELUCCA_FLASH && !PROJ_HOST` 下 include；依赖 `proj_tmp`、`st_current/st_sector/st_buf`、`fm6_slot/fm6_unpack/fm6_pack/fm6_bank_read`（`eng_fm6.c`，已编入）、`flash_ok`、`FM6_BANK_XIP`。**init 必须调 `fm6_bank_boot()`**（安装 `fm6_bank_read` 钩子并扫描 used 位），否则 PTCH≥8 播 init voice。
- **`apply_preset_to`/`set_engine_of`/`TRK_DEF`/`param_kept`/`arp_is_sounds`/`user_of` 在 `ui.c`**（未编入）：这是「换预置=换声音」的唯一实现（`sloop_core_track` 缺的正是它），必须原样移植到 adapter；`preset_extras`/`preset_trim`/`locks_restore`/`sound_change`/`steps_clear` 已在 `params.c/seq.c/voice.c`，可直接用。
- **`PROJ_HOST` 的静态开销陷阱**：`project.c` 的 `proj_slot[4]`（4×3,816=**15,264B**，`section(".noinit")`）与 `proj_v3_tmp`（**2,584B**，旧格式转换用）都在 `#ifndef PROJ_HOST` **之上**，定义 `PROJ_HOST` 不会去掉它们。`proj_slot` 就是 P4-B 的 4 个工程 RAM 槽（sections 属 P4-C，但槽本身在 P4-B 即用），且 `.noinit` 与 `selftest.c` 的 `ctl/s_quiesce_hook` 同段、无法被 `--gc-sections` 单独回收；`proj_v3_tmp` 被 `proj_from_v2/v1` 真实引用。解法（不碰 vendored）：include 前用宏把它们重定向成**指针**，实体放 PSRAM（`proj_tmp` 同法），见 §4.2。
- **ESP-IDF 5.5.5（esp32p4）**：`esp_partition_mmap()` 对任意数据分区可用（64KB MMU 页，内部按页对齐），`esp_flash_write/erase` 完成后按地址范围失效 cache（`flash_end_flush_cache`），因此「原始分区 + mmap」可以同时提供 `st_read` 与 `st_xip` 语义。擦除/写入期间 cache 关闭、双核停顿（上游注释的 ~50ms），音频需静音。

## 3. 方案（Approaches）

### 3.1 存储后端

| 选项 | 评估 |
|---|---|
| **A. 原始自定义分区 + mmap + 上游 storage.c 原样（采纳）** | 分区 `sloopstore`（data/0x99）覆盖 flash map 的 `[0x97000,0xFF000)`；`st_read`=映射区 memcpy，`st_erase`=`esp_partition_erase_range`（先 codec mute），`st_prog`=`esp_partition_write`；`ST_XIP_PTR(off)`=映射基址+偏移。**真擦写语义（A/B/torn-write 与硬件一致）、`st_xip` 零成本、上游 4 个 TU 不动逻辑**；代价：分区表调整（FAT 尚无数据，可安全重排） |
| B. FATFS 单文件内 A/B 双槽 | 文件层 A/B 与 storage.c 的 A/B 重复；无 mmap → `st_xip` 必须改成 6KB RAM 镜像（`upreset.c/fm6_bank.c` 需改）；整镜像/扇区写入无原子性，掉电窗口比原始扇区更大；FATFS 元数据损坏面更大 |
| C. NVS 分块存 blob | 无擦除语义（storage.c 的 `st_erase` 语义要另造）；NVS 容量/寿命/键数限制；无 XIP；不选 |

**决策 A**。配套：`partitions.csv` 调整为 `sloopstore` @ `0x810000` size `0x68000`，FAT `storage` 移到 `0x878000` size `0x788000`（均 4KB 对齐；FAT 本阶段不使用，留给非 SLOOP 资产；**P4-D 的用户采样按上游原样走 USR1–4 原始槽**：USR1–3 `0xA0000..0xDC000`、USR4 `0xE7000..0xFB000`，不依赖 FAT）。NVS 保持 `0x9000/0x6000` 但不承载 SLOOP 状态（P4-E 的设置也用 `OBJ_SETTINGS`）。分区表变更需 `scripts/build.sh --clean`；`sloopstore` 首次写入前由 storage.c 自行擦除，无需整片 erase-flash。

> 该决策取代 phase-4 spec §4.3 的「FATFS 分区文件内 A/B 双槽 + CRC」：**语义完全保留上游（A/B + CRC + 回读），承载介质改为原始分区**。FATFS 不再承担 SLOOP 数据；NVS 不承担 SLOOP 状态（P4-E 设置走 `OBJ_SETTINGS`，与 FM-1 备份/编辑器互通）。

### 3.2 上游代码复用方式

| 选项 | 评估 |
|---|---|
| **A. 同 TU include + 薄 shim（采纳）** | 在 `sloop_core` 的单 TU（`src/selftest.c` 末尾）include：`storage.c`（原样，提供三个 hook）、`project.c`（`PROJ_HOST`，格式层 + capture/apply）、`upreset.c`（原样）、`fm6_bank.c`（原样）。shim：`trk_def_engine/user_of/param_kept/arp_is_sounds/ui_message/ui_say/ui.force/up_gen/sync_reload/flash_ok/up_stage/ST_XIP_PTR/FM6_BANK_XIP`；另移植 `apply_preset_to/set_engine_of/TRK_DEF`（换预置换声音）与 `autosave_tick/autosave_resume/proj_foreign_parts`（上游 `!PROJ_HOST` 段，语义照抄）。上游 vendored 目录零修改 |
| B. 复制并改写 project.c/upreset.c | 修改面大、上游升级要人工对齐；违反 AGENTS §8「补丁/适配层隔离」 |
| C. 只借格式常量，重写全部逻辑 | 重复实现 A/B、CRC、旧格式转换（1,200+ 行），回归风险最高 |

**决策 A**。`project.c` 用 `PROJ_HOST` 意味着不整段编入 settings/panel/sections/arranger/DSYN/MIDI 部分（见非目标）；`proj_capture/proj_apply` 在 `PROJ_HOST` 之前可用；**autosave 与 `proj_foreign_parts` 按原语义移植到 adapter**（目标 2/5）。adapter 另需 `#define FELUCCA_FLASH 1`（供 `upreset.c` 的 `up_bank_find` 与 `fm6_bank.c` 的 flash 分支），并在 init 调 `up_boot()`/`fm6_bank_boot()`。

### 3.3 导航信息架构与页面容量（池容量硬前置）

| 选项 | 评估 |
|---|---|
| **A. 分组页签 + 组内子页签 + 组作用域生命周期（采纳）** | 顶层 5 组：`PLAY / TRACKS / EDIT / SEQ / FILE`（`SYS` 待 P4-E 有页面时加入）。**子页签放在现有 72px 页签条内做成两行**（上行组、下行子页，各约 34/30px，16pt 标签 ~20px 高），**页面区起点仍是 y=72，现有九页布局零改动**。EDIT = GLOBAL/FX/ANALOG/FM6/SAMPLE（后续引擎页），SEQ = STEP/TRANSPORT（后续 SONG），FILE = PROJECTS/PRESETS。**离开组时销毁该组已建页面**（先建/切目标页，再销毁源组**全部**已建页，含原当前页 → 回 UNBUILT），进入时懒建并保留。内存不变量从「9 页全建」变为「基础占用 + 单组 Σ est + 单页 est ≤ 池可用（跨组切换的过渡峰值，见 §5）」，FILE 两页 est 合计 9,728B（4,608 + 5,120），未来页面有确定预算 |
| B. 保留平铺页签 + 预算失败时 LRU 驱逐 | 不改页签语义；但驱逐时机不可预测（用户切到第 10 页才失败/重建），焦点状态丢失不可控，且页签宽度墙（N=10=95px）仍然无解 |
| C. 两行页签（页面区下移 44px） | 现有页把页面区用到像素级（参数页 `52+2×298=648` = `UI_H−72`；STEP 数字 y=568、提示 y=606），下移会裁掉底行并迫使九页重排 + 全部 TAP/探针几何重算；否决。**采纳方案 A 的两行页签是「在 72px 页签条内分两行」，不是本项** |
| D. 溢出菜单 | 需要新的焦点/菜单语义与可发现性设计，P4-B 不引入 |

**决策 A**。键盘 `TAB` 语义改为「按组顺序的扁平轮转」：组内最后一页的下一张 = 下一组第一页（保留现有「tab 轮转」心智与 `nav.sh` 用例形态，更新期望序列）；点顶层页签进入该组（记住组内上次子页），点子页签切换。页签宽度 N=5 时 `min(176, (1280−32−220−4×8)/5) = 176px`，标签 `PLAY/TRACKS/EDIT/SEQ/FILE` 均远小于该宽度。组作用域生命周期的执行顺序固定为「建目标页 → 切到目标页 → 销毁源组全部已建页（含原当前页）」，故离开组后源组 `built=0`（见 §4.5）。

**容量天花板条目收口**：组作用域生命周期把「9 页 = 池顶」变成可维护的不变量：
`base(≈7.0KB) + max_group Σ est + max_single_page_est + 4KB ≤ 63,120B`，且每页仍保留 `free ≥ est` 的构建前检查。
当前最大值 SEQ = `23,040 + 4,608 = 27,648B`，`max_single_page_est` 保守取 STEP **23,040B**：
`7,016 + 27,648 + 23,040 + 4,096 = 61,800 ≤ 63,120`（余 **1,320B**；真实最坏对 SEQ→EDIT 更小）。
源组页面在切换后才释放（见 §4.5），峰值是瞬态但必须被上式覆盖。ADR-005 记录决策与不变量，ADR-008 记录组作用域对预算表的影响；**原延迟条目已删除**（条目要求「解决后删除；方案选型先沉淀 ADR」）。

### 3.4 写入路径与并发

| 选项 | 评估 |
|---|---|
| **A. 全部写/载操作经状态层命令（采纳）** | `CMD_PROJECT_* / CMD_PRESET_* / CMD_BANK_*` 由 input 任务（唯一写者）执行；UI 只发命令、只读列表查询。保持阶段 3 的「UI 不直接写引擎」不变量，阶段 5 可把执行端换成 FM-1 会话 |
| B. UI 任务直调 `sloop_store_*` | 少一层转接，但破坏唯一写者模型；加载工程会与 input 任务编辑竞争 |

**决策 A**。只读的列表/名称查询（`used/name/engine/count`）允许 UI 直接调用：不写任何介质、只读 mmap，也**不取引擎锁**；但要取 `s_store_lock`——每次查询先重扫一次银行索引（微秒级），若恰好有保存/擦除在写 flash，读者会**排在这次擦写之后**（实测一次保存 ≈50–150ms）。这是有意的：宁可等，也不让 UI 读到擦写中的副本。ADR-009 §3 记为该例外（Task 4 实测口径）。
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
#define SLOOP_STORE_BANK_PACKED 128u   /* fm6_pack 的 128B packed 记录（Task 7） */

typedef struct {
    bool ready; uint32_t objects_ok, faults_armed, faults_taken;
    uint32_t autosave_writes, autosave_hash; bool autosave_on;   /* P4-B Task 6 扩展（STORE STATS 同源） */
} sloop_store_stats_t;

esp_err_t sloop_store_init(void);            /* sloop_core_init 内调用：HAL init + PSRAM 槽/暂存
                                              * + 4 槽工程 fetch + up_boot + fm6_bank_boot
                                              * + autosave_resume 之后由 core 调 */
bool      sloop_store_ready(void);
void      sloop_store_stats(sloop_store_stats_t *out);

/* 工程 A–D（0..3）：RAM 槽（PSRAM，4×3,816B）+ flash 双层，与上游 project.c 同语义。
 * save = capture 到 RAM 槽 + st_save；load = RAM 槽有效则直接用，否则 st_load+import；
 * boot 时四槽全部 fetch 一遍。hash = project_t.sum（0=空/无效）。 */
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

/* FM6 银行（packed 128B 记录，槽 0..26；PTCH = 8 + 槽号）。入口在存储锁内重扫
 * fm6_bank_boot()（.bss 回滚，ADR-009 §8）；put/erase 走带中拒绝。 */
bool     sloop_store_bank_used(uint32_t slot);
bool     sloop_store_bank_get(uint32_t slot, uint8_t out[SLOOP_STORE_BANK_PACKED]);
esp_err_t sloop_store_bank_put(uint32_t slot, const uint8_t *packed);   /* 1→INVALID_ARG, 2→NOT_FINISHED */
esp_err_t sloop_store_bank_erase(uint32_t slot);
void     sloop_store_bank_current(uint8_t *packed);  /* 选中轨当前 patch 打包（存 bank 用；鼓轨全 0） */

/* 自动存档（上游 autosave_tick/autosave_resume 语义）：停走带 + 无输入 ≥2.5s +
 * 距上次保存 ≥20s + 无发声 + 捕获 hash 与上次不同才写 OBJ_AUTOSAVE；
 * note_activity() 由输入派发调用（键盘/触摸/console 注入都算）；tick 由 1s 周期
 * 任务调用（不占 LVGL 锁）。boot_resume 在 snapshot 之后由 core 调。
 * 诊断开关 `STORE AUTO ON|OFF`（默认 ON）对应 sloop_store_autosave_enable()；
 * `STORE STATS` 另带 autosave/autosave_hash/autosave_writes 三项。 */
void      sloop_store_note_activity(uint32_t now_ms);
void      sloop_store_autosave_tick(uint32_t now_ms);
esp_err_t sloop_store_autosave_resume(void);

/* 工厂预置应用（上游 apply_preset_to/set_engine_of；换引擎/预置=换声音）。
 * 注：这三个声明在 sloop_core.h（引擎 API）；sloop_store.h 只放存储。 */
void      sloop_core_apply_preset(uint32_t track, uint32_t preset);
void      sloop_core_set_engine(uint32_t track, uint32_t engine);
/* 预览：同一 p[] 基底上的预置写值，无副作用（console `ENG PRESET` 用它做期望值）。
 * 实际声明（Task 5 落地）返回 bool：false = 诊断冻结（console 回 `ENG PRESET ERR frozen`，
 * 不打印零向量）或参数越界，out 已清零；true = out 填好 71 项。 */
bool      sloop_core_preset_preview(uint32_t track, uint32_t engine, uint32_t preset, int16_t out[71]);
/* sloop_core_track(k,e,p) 语义修正为 set_engine_of + apply_preset_to(p)，与上游一致。 */

/* 设备 torn-write 套件：对象 = OBJ_SETTINGS（P4-B 不写它；测试前快照、测试后恢复）。 */
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
   两行各只占 4B（`.noinit` 的 15,264B 与 `.bss` 的 2,584B 都消失）；`sloop_store_init` 里 `s_v3_ps`（2,584B）、`s_slot_ps`（4×project_t = 15,264B，工程 RAM 槽）与 `s_stage`（4,096B）全部 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`。宏包裹的是**整个对象**，上游全部引用形如 `&obj[...]`/`sizeof obj[i]`（已逐行核对），替换后取址与 `sizeof` 语义不变。
3. 自有暂存（PSRAM，init 时 `heap_caps_malloc(4096, MALLOC_CAP_SPIRAM)`）：
   ```c
   typedef union { project_v5_t v5; uint8_t raw[4096]; } proj_stage_t;
   static proj_stage_t *s_stage;  #define proj_tmp (*s_stage)   /* preset bank / fm6 bank / import */
   ```
   静态断言 `sizeof(proj_tmp) >= sizeof(fm6_bank_t) && >= sizeof(project_t)`。
4. include `storage.c` → `project.c`（`#define PROJ_HOST`）→ `upreset.c` → `fm6_bank.c`（前置 `flash_ok` 等）；
5. 实现 `sloop_store_*`（capture/apply/存读/擦、hash、故障套件、autosave、boot 恢复）。
6. 移植 `apply_preset_to/set_engine_of/TRK_DEF/param_kept/arp_is_sounds/user_of` 与
   `proj_foreign_parts`（逐行对照上游 `ui.c:581-691`、`project.c:639-647`）；
   `sloop_core_track` 改为 `set_engine_of`（引擎变时）+ `apply_preset_to`（预置），
   引擎锁内执行、`fm1_irq_off/on` 由引擎锁替代。

**内存账（实测后复核）**：内部 RAM 新增仅 `main_scratch` 3,840B + 指针/小静态 ≈4.2KB（当前 soak `internal_free≈70.4KB`、地板 61,440B → 预计 ≈66KB）；PSRAM 新增 `s_slot_ps` 15,264B + `s_stage` 4,096B + `s_v3_ps` 2,584B ≈22KB（32MB PSRAM 充裕）。**门禁**：`nm`/`idf.py size-components` + 启动 `ENG MEM` 确认三者不在 DRAM，且 idle 内部堆 ≥61,440B。

**启动顺序**（`sloop_core_init`）：HAL → 引擎默认 → `selftest_snapshot_init()`（工厂态基线，golden 不变）→ `sloop_store_init()`（PSRAM 槽/暂存、mmap、4 槽 fetch、`up_boot`、`fm6_bank_boot`；不建任务）→ `sloop_store_autosave_resume()`（proj_import + proj_apply + `proj_foreign_parts`，`trk[n].engine = eng_req` 无淡入）→ `sloop_store_autosave_start()`（autosave 1s 周期任务）。**snapshot 必须在 resume 之前**，否则 `ENG SELFTEST` 的基线会变成用户工程、golden 失配；**任务必须在 resume 之后**，否则先起任务的那一拍会把工厂态写进 autosave、覆盖刚读回的工作工程。

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
STORE STATS                      # ready/分区/mmap/对象统计/故障与 xrun 增量（含 autosave 字段）
STORE AUTO ON|OFF                # 自动存档开关（默认 ON；测试用）
STORE PROJ LIST|SAVE n|LOAD n|ERASE n|HASH n|FIELDS n
STORE PRESET LIST|SAVE n|LOAD n [track]|ERASE n
STORE BANK LIST|PUT n|GET n|LIVE|ERASE n    # PUT 取选中轨当前 live patch；GET/LIVE 打印 128B hex
STORE LIVEHASH                   # 当前引擎捕获 hash（往返门禁用）
STORE RAW BEGIN <len>            # 分块上传任意对象 payload（≤3,840 = 一个 flash 对象的 payload 上限）
STORE RAW <hex>                  # 追加一块（≤240 hex 字符 = 120B）；可重复
STORE RAW END <obj> <slot>       # 以 st_save 原样写入该 payload（CRC/头由 storage.c 生成）
STORE RAW ABORT
STORE TEST                       # 设备 torn-write 套件（破坏 OBJ_AUTOSAVE）
STORE FAULT <nth> [half]|CLEAR   # 故障注入（供脚本制造掉电窗口）
```

> 块大小的真正上限不是 console 行缓冲（256）而是 **USB-Serial-JTAG 驱动 RX 环 256B**：
> 整行要在一次突发里装进环里，设备忙（LVGL/音频）时环溢出会静默丢字符（Task 9 设备实测：
> 490 字符的行被截到 256）。240 hex + `STORE RAW ` + `\n` = 251B < 256B，与调度时序无关。

### 4.5 UI（`sloop_ui`）

- **导航壳**：`ui.c` 增加组模型（`k_groups[]`：标题 + 页面 id 列表）；**两行页签复用现有 72px 页签条**（上行 group 按钮 ~34px，下行 sub 按钮 ~30px；只有 1 页的组下行留空、右侧继续显示层键提示）；页面区起点与尺寸不变（`UI_TAB_BAR_H` 保持 72），hit-test 原点不变。
- **组作用域生命周期**：切组时按固定顺序执行——**建目标页 → 切到目标页 → 销毁源组的全部已建页**（含刚离开的当前页；`lv_obj_del` + 复位 `s_page_obj/state/delta`）。因此离开组后源组 `built=0`。顺序不可交换：先销毁会在切页时出现空白帧、输入路径在跨组切换期间失去活页；代价是切换瞬间的峰值 = 源组已建 Σ + 目标页 est（源组页面在切换后才释放，峰值是瞬态，由 §5 的峰值不变量覆盖）。组内首次访问兄弟页时构建它；同组后续切换只改显隐。PLAY 组单页仍在启动时构建。≤100ms 构建预算适用于跨组切换与组内首次构建兄弟页。
- **PROJECTS 页**（FILE 组）：4 张卡片（A–D，标签 + 状态「EMPTY / USED」+ hash 前 4 位），每卡 3 个按钮 `SAVE / LOAD / ERASE`；播放中按钮动作显示 `STOP BEFORE SAVE/LOAD/ERASE`；点击卡片设选中槽（键盘 ←/→ 也可）。
- **PRESETS 页**（FILE 组）：32 槽 4×8 网格，显示 `U01..U32` + 引擎名/空；选中槽后 `LOAD`（载入选中轨）/`SAVE`（存选中轨，鼓轨提示 `DRUM TRACK: NO SOUND`）/`ERASE`。
- **FM6 页**：现有 8 旋钮页底部加一行 **bank 目标槽选择（`BANK - / +`，0..26）+ `BANK SAVE / BANK ERASE`**。`BANK SAVE` 把**当前 live patch**（`fm6_patch[track]` 经 `fm6_pack`，可来自工厂槽 F1–F8 或宏改动后的声音）写入目标槽；`PTCH` 旋钮仍是**音源选择**（PTCH = 8 + bank 槽）。状态行显示 `SAVED B12 / ERASED B12 / BANK FULL?` 等。
- **页 id**：`app_page_t` 追加 `APP_PAGE_PROJECTS, APP_PAGE_PRESETS`（`APP_STATE_PAGES` 9→11）；console `UI` 页名表与 `UI MEM` 增加组字段（组名、组内页、组作用域下当前已建集合）。
- **键盘**：`TAB` = 扁平轮转（组内子页 → 下一组）；`ESC` = 回 PLAY（组与子页同时复位）；FILE 页 `←/→` 选槽、FM6 页 `←→` 在旋钮间移动（沿用参数页），`ENTER` = LOAD（PROJECTS/PRESETS）。SAVE/ERASE 仅触摸按钮 + console（避免与演奏键冲突）。

## 5. 约束

- **门禁**：`scripts/build.sh` ×2 零新增警告；`scripts/host-test.sh` 全绿（上游 4 个存储相关测试本就覆盖 A/B/torn-write）；设备 `ENG SELFTEST` 182/182 不得回退；`scripts/device-selftest.py` 全量；`scripts/state-test.sh`/`knob-test.sh` 绿。
- **池**：FILE 组两页 est 初始预算 `PROJECTS 4,608 / PRESETS 5,120`（构建前 `free ≥ est`）；跨组切换的**过渡峰值**不变量 `base + max_group Σest + max_single_page_est + 4KB ≤ 63,120`（当前 `7,016 + 27,648 + 23,040 + 4,096 = 61,800 ≤ 63,120`，余 1,320B）；`UI MEM` 增加分组 Σ。
- **内部 RAM**：新增内部 SRAM 仅 `main_scratch` 3,840B + 指针/小静态（≈4.2KB 总计）；`proj_slot`（4 槽 = 15,264B）、`proj_tmp`、`proj_v3_tmp` 在 PSRAM（宏重定向，§4.2）；不改 LVGL 池；idle 内部堆地板 61,440B 不变。
- **自动存档**：仅 `song.playing || transport_req || !audio_quiet() || store 忙 || s_frozen` 任一为真时不写；两次写间隔 ≥20s、无输入 ≥2.5s；写前快照 hash 与上次相同不写；只写 `OBJ_AUTOSAVE`，不碰 A–D。
- **换音语义**：`sloop_core_track(k,e,p)` 与 `sloop_core_set_engine/apply_preset` 必须在引擎锁内完成 `locks_restore + sound_change + p[] 写入 + fm6_track_loaded`，不得只写索引（现状缺陷）。
- **时序**：保存动作（擦+写+回读）实测目标 ≤ 150ms（4KB 擦 + 3.8KB 写 + 回读）；擦/写期间 codec mute。**xrun 口径**：擦除会关 cache、音频任务停顿，`xrun` 必然增长——门禁改为「无存储动作窗口 `xrun=0`；每次保存的 `xrun` 增量 ≤ 16 且随后 `streaming=1`、渲染恢复」，`STORE STATS` 报告每次保存的 xrun 增量（ADR-009 记录；`soak.py` 增加显式保存窗口参数）。
- **数据兼容**：工程写入 `FUN6`；读兼容 `FUN5/4/3/2/1`（上游 `proj_import`）；用户预置 `UPB1`/`UP_VER 1`；FM6 `FM6B`/ver 1。**不得**改上游记录布局；阶段 5 的编辑器协议对象 id 与之一一对应。
- **只读 vendored**：`third_party/sloop-fm1/**` 不修改（manifest 校验）。
- **人测外置**：听感（保存/加载后音色变化）、物理触摸按钮、视觉复检按 AGENTS §5.4 清单外部门禁。

## 6. 验证策略

1. **host（已有 + 新增）**：`host-test.sh` 的 `storage_test/project_test/upreset_test/fm6_test` 必须保持全绿（它们验证上游 A/B/torn-write/格式转换）；新增 `state-test` 用例覆盖存储命令（stub `sloop_store_*`：成功/空槽/播放中/鼓轨/越界）。
2. **设备存储自检**：`STORE TEST` 6 个场景（对象 = `OBJ_SETTINGS`，测试前快照、结束后恢复；P4-B 不写 settings）——①两次提交后 A/B 选择 seq 大者；②擦除失败→旧副本可读；③payload 写一半失败→旧副本可读；④头写失败→旧副本可读；⑤回读校验失败→报错不报 SAVED；⑥擦除提交（`len=0`）→对象为空。全过打印 `STORE TEST PASS 6/6`。
3. **工程往返（机器门禁）**：
   - 基线：改 BPM/参数/步/选轨，`STORE LIVEHASH` = h0；`STORE PROJ SAVE 0`；`STORE PROJ HASH 0` == h0；
   - 破坏：再改 BPM/参数/步 → `STORE LIVEHASH` = h1 ≠ h0；
   - 恢复：`STORE PROJ LOAD 0` → `STORE LIVEHASH` == h0；`STATE SNAP` 的 p/g 与保存前逐项一致；
   - **旧格式兼容**：`scripts/gen_project_fixtures.py` 生成 FUN5/FUN4/FUN1 三个 fixture（逻辑取自上游 `tests/project_test.c` 的构造器），`scripts/store_raw.py` 经 `STORE RAW BEGIN/<hex>/END` 写入槽 3，`STORE PROJ LOAD 3` 后 `STORE PROJ FIELDS 3` 与生成器给出的期望字段逐项比对（引擎/预置/全局/首步/参数代表值），并确认 `proj_import` 转换成功（槽 3 为诊断专用，不参与用户数据断言）。生成器的期望值用 Python **独立实现**上游格式转换从生成的字节算出（不是固件输出）；`gen_project_fixtures.py --host-check` 另用 vendored `project.c` 的宿主程序导入同一批字节并交叉校验（随 `scripts/host-test.sh` 跑）。
4. **预置与工厂预置**：`ENG PRESET 0 2 1` 输出该预置在同一 `p[]` 基底上的完整期望向量（与 `preset_apply` 共用 `preset_values`，无副作用），`ENG TRACK 0 2 1` 后 `STATE SNAP` 的 p[] 必须逐项等于它（71/71）——这是「换预置=换声音」的机器断言；再存槽 k（选中轨 ANALOG）→ 改 8 个参数 → load → p[] 与存档前一致；`STORE PRESET LIST` 显示 used/name/engine；鼓轨 store 返回 `DRUM TRACK` 错误；跨轨 load（track 1）只改该轨、不动选中轨参数。
5. **FM6 bank**：设 bank 目标槽 3 → `STORE BANK PUT 3`（当前 live patch）→ `STORE BANK GET 3` 与 `STORE BANK LIVE`（同一 patch 的另一路导出）字节一致 → 重新上电后 `BANK LIST` 仍 used；`PTCH` 选到该槽（PTCH=11）后 `ENG TRIG` 出声且 `ENG SNAP` 的 preset/patch 指示正确；对无 patch 的槽 `BANK GET` 返回 empty。
6. **UI（§5.4）**：`UI file` → `UI projects` 截屏（空/used/播放中拒绝）→ `TAP` 点 SAVE/LOAD/ERASE → `SCR` + `--check` 像素抽检；`UI presets` 同；`UI MEM` 断言 FILE 组 Σdelta ≤ 预算、组作用域（离开 FILE 后 PROJECTS/PRESETS 回 UNBUILT）；新 `scripts/inject/store.sh` 退出码 0；`nav.sh` 四用例按新语义更新并通过。
7. **自动存档与上电恢复**：改 BPM/步 → 等 ≥3s（无输入、停止）→ `STORE STATS` 显示 autosave 已写且 hash 与 `LIVEHASH` 一致；再改 → 等 20s → 第二次写；播放中等待 25s → hash/计数不变；同一状态不重复写（hash 相同跳过）；`STORE PROJ LOAD 0` 后工作工程变化 → 下一个 20s 窗口写一次、之后不再写；断电重启（`flash.sh` 后）→ `LIVEHASH` == 重启前 autosave hash，`STATE SNAP` 的 bpm/步正确。
8. **稳定性**：`scripts/soak.py` 120s（其中**独立窗口**做一次保存/加载周期）无 WDT/panic、内部堆不单调下降；xrun 判据按 §5：保存窗口外 `xrun=0`，保存窗口内增量 ≤16 并在窗口后恢复 `streaming=1`；`ENG SELFTEST` 182/182 在存储初始化后仍全绿（存储不触碰 golden）。
9. **证据**：`docs/dev/verify/4-pb-<日期>/`（截屏、日志、STORE TEST 输出、往返 hash、UI MEM、SHA256SUMS）+ `verification-log.md` 登记。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 擦除期间 cache 关闭导致音频 xrun/爆音 | codec mute 包住 erase/prog；xrun 判据见 §5（保存窗口内 ≤16、窗口外 0）；人测听感；若爆音不可接受，改「停流→存→启流」（回退开关，ADR-009 记录） |
| 分区表调整让旧固件数据失效 | 仅一次性的布局变更（FAT 无数据）；`partitions.csv` 注释写死偏移；storage.c 写入前自擦，无需整片 erase-flash |
| `esp_partition_mmap` 与 FATFS/其他 mmap 区域冲突 | 416KB/64KB=7 页，P4 vaddr 空间充裕；若失败 → 存储 `ready=false`，UI 显示 `NO STORAGE`（失败可见，不崩溃） |
| 上游静态吃掉内部 RAM（`proj_slot` 15,264B / `proj_v3_tmp` 2,584B） | include 前宏重定向为指针 + PSRAM 实体（§4.2）；`nm`/`ENG MEM` 门禁确认；`st_buf` 强制内部 SRAM |
| 两行页签挤压标题/层键提示、或改变页签命中几何 | 两行都在现有 72px 内，页面区不动；右侧标题上行、层键提示下行；`TAP` 注入仍按 y<72 判页签区；调整既有截屏探针并归档新基线 |
| 旧格式 fixture 无入设备通道 | `STORE RAW` 分块上传 + `scripts/store_raw.py` + `scripts/gen_project_fixtures.py`（§6.3）；槽 3 诊断专用 |
| 自动存档的 flash 寿命 | 与上游同阈值（≥20s 间隔、仅静默且 hash 变化时写）；`STORE STATS` 暴露写入计数；soak 观察（每次写 = 1 扇区擦 + 2 次 prog） |
| 自动存档与 selftest/命令竞态 | 全程 `s_store_lock`；`s_frozen`/播放/发声/存储忙时跳过；selftest quiesce 期间 UI/输入已停，tick 任务也跳过 |
| `PROJ_HOST` 编译缺符号（上游升级） | 编译期即暴露；shim 清单集中在 `store_tab5.c` 顶部，升级时只补此文件 |
| 组作用域生命周期引入切页重建延迟/状态丢失 | 切页计时探针（`UI MEM` 增加 build_ms）；跨组切换与组内首次构建兄弟页目标 ≤100ms（同组后续切换零重建）；子页焦点由 state 重建；`nav.sh` 覆盖往返 |
| UI 直接读存储列表与写操作竞争 | 列表查询只读 mmap、无写；写期间 UI 任务被 cache 停顿，不存在半写视图 |
| 保存时正在录音/播放 | 统一 `ESP_ERR_INVALID_STATE` + UI 状态行 `STOP BEFORE …`；console 脚本断言错误码 |

## 8. 决策记录（fidelity-first；负责人已授权按推荐取舍）

目标函数：**最大程度移植 SLOOP FM-1**（功能与数据语义等价优先于实现省事）。据此定案：

1. **存储后端 = 原始分区 + mmap + 上游 storage.c 原样**。FATFS 文件方案、NVS 方案否决（前者重复 A/B 且丢 XIP，后者无擦除语义）。FAT 分区保留但不再承载 SLOOP 数据；P4-D 用户采样走上游 USR1–4 原始槽（USR1–3 `0xA0000..0xDC000`、USR4 `0xE7000..0xFB000`）。
2. **设置/偏好走 `OBJ_SETTINGS` 上游格式**（P4-E 实施），NVS 不承载 SLOOP 状态——保证与 FM-1 备份、编辑器协议对象互通。
3. **本阶段做自动存档与上电恢复**（上游 2.5s/20s/静默/停走带/hash 变化判据），不再推迟到 P4-C。启动顺序：工厂态 snapshot → store init → autosave resume（§4.2）。
4. **本阶段补「换预置=换声音」**：移植 `apply_preset_to/set_engine_of/TRK_DEF/param_kept/arp_is_sounds/user_of`，修正 `sloop_core_track`。这是既有缺陷修复，也是工厂预置浏览的前提。
5. **工程 4 槽保留 RAM 缓存**（PSRAM，`proj_slot` 语义不变），开机 fetch 全部；A–D 的 save/load 与上游双层语义一致。
6. **组作用域销毁**：切组顺序固定为「建目标页 → 切到目标页 → 销毁源组全部已建页（含原当前页）」；不是 LRU。若实测切页重建 >100ms（跨组切换与组内首次构建兄弟页），退化为「每组保留最近 1 页」。
7. **两行页签复用现有 72px**，页面区与九页布局零改动；`TAB` 为组顺序扁平轮转。
8. **用户预置写入口径 = 当前选中轨**（上游 `up_store` 亦是 `TSEL`）；读可指定任意合成轨。
9. **FM6 bank**：bank 目标槽（0..26）与 `PTCH` 音源分离；`BANK SAVE` 写当前 live patch。
10. **`STORE TEST` 对象 = `OBJ_SETTINGS`**：测试前快照、结束后恢复（不再用 autosave，因 P4-B 已启用 autosave）。
11. **settings/sections/arranger/DSYN/MIDI/采样槽上传** 仍留 P4-C/D/E（非目标），但所有格式/对象 id 与上游一致，后续直接复用。
12. 页 id/页名、console 语法、注入脚本名（`scripts/inject/store.sh`）为本文约定；实施中如与现有命名冲突可微调并在证据中记录。
