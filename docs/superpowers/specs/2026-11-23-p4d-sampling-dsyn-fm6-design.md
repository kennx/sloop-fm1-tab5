# 阶段 4 / P4-D：采样槽、DSYN 鼓合成与 FM6 深度 patch 设计规格

> 日期：2026-11-23 ｜ 状态：待实施
> 上位文档：`docs/design/002-port-roadmap.md` §6；`2026-11-21-phase4-feature-parity-design.md` §3 P4-D。
> 上游基线：SLOOP 2.8.5 `bac9c05`（P4-C 收口时核对；开工前再核对远端 HEAD）。
> 决策原则：fidelity-first——数据布局、范围钳位、持久化对象与编辑语义以上游为准；Tab5 只重排交互与容量，不另造采样/鼓合成/patch 数据模型。

## 1. 目标、非目标与反目标

### 目标

1. **USR1–USR4 采样槽闭环**：把上游 4×80KiB 原始槽（flash 0xA0000/0xB4000/0xC8000/0xE7000，header 480B + data@512）接进 Tab5：开机装载、串口分块上传/擦除、CRC 与 zone 校验、SAMPLE/GRAIN/鼓 KIT 选到 USR 后立即出声。
2. **DSYN（SYN1–SYN4）22 字节音色编辑面**：16 lane × 4 kit 的 `dsnd_t` 可读、可改、可试听、可套用工厂 kit；修改立即被鼓声部使用；显式 STORE 写入上游 PER3 settings 的 `dsu_bank_t`（偏移 88，1,464B），硬复位与 `ENG SELFTEST` 后恢复。
3. **FM6 深度 patch 编辑面**：以 155B 单音色布局（6 算子 × 21B + 29B voice）编辑算法、算子包络/电平/比率/键盘缩放/速度/固定频率、pitch EG、LFO、transpose；改动立即影响该轨 FM6 声部；可经既有 FM6 bank 27 槽持久化。
4. **原生 UI + 完整诊断面**：新增 `SYNTH` 顶层组（SAMPLE / DSYN / FM6P 三页），触摸/键盘可完成选 kit/lane/算子/字段页与调值；console 提供 `SMP`/`DSYN`/`FM6P` 诊断执行器与 `TAP` 回显，供注入脚本断言。
5. **容量与稳定性**：不扩 LVGL 池；按 ADR-005 §5 的有序对重算并实测；全量 host/state/knob/设备 182/182、注入矩阵、复位后 soak 通过。

### 非目标

- WAV 文件导入 UI、SD 卡采样浏览、麦克风录音：本阶段只提供串口分块上传（上游 `SMP_*` 的镜像）与设备端擦除；文件解析/ADPCM 编码在 PC 脚本侧。
- 采样/DSYN/patch 名称的屏上文本编辑：保留已有 8/10 字符名，改名列为后续条目。
- 设置页（亮度/主题/音频/系统/lights）、MIDI LEARN、双模式调度：P4-E / P5。
- 改变上游声音算法、扩展 `dsnd_t`/`dsu_bank_t`/`fp` 布局、写入 NVS 或新建私有持久化对象。
- 性能优化；P4-D 只保证既有 xrun/占空比/堆门禁不退化。

### 反目标

- 把采样槽改成自定义 A/B 对象、或只存 PSRAM 不上 flash（掉电即丢）。
- 把 DSU 放进 `project_t`/autosave 或独立对象（上游只在 settings；工程载入不得改 SYN kit）。
- 用 `project LOAD` 或 `fm6_patch` 之外的路径模拟“换 patch”，或让 PTCH 宏在编辑后把 patch 覆盖回槽值。
- 为容纳新页面扩大 LVGL 池、突破组作用域、或把 8 算子/16 lane 全展开成几百个对象。
- 在音频路径擦写 flash、分配内存或操作 LVGL；擦写期间持引擎锁。
- 采样上传/擦除在走带中执行（见 §3.5 的显式 Tab5 安全差异）。

## 2. 现状与关键事实

- **采样槽现在是“未接线”状态**：`selftest.c` 在 include `regress.c` 前把 `SMP_USER_XIP(k)` 重定向到 `sloop_hal_user_samples()`（4×80KiB PSRAM 镜像）；`sloop_core_init` 把镜像填 0xFF，但**没有任何代码把 flash 槽装进镜像、也没有任何代码调用 `smp_user_scan(k)`**。因此 SAMPLE 页的 USR1–4 永远静音；`SAMPLE`/`GRAIN`/鼓 `KIT USR*` 都在等这一步。
- 上游槽格式（`eng_sample.c`）：`smp_user_hdr_t` 480B（magic `FSMP`、version 1、`nz` 1..16、name 8、`data_len`、CRC32、16×`smp_zone_t` 28B），data 从槽内 +512 起，IMA ADPCM 4bit；`smp_zone_ok` 的合法性检查与 `st_crc32` 可直接复用（同一 TU）。
- 上游 flash map：settings 0xFC000/0xFD000；project A–D 0x97000..0x9EFFF；autosave 0x9F000/0xFE000；**USR1–3 0xA0000/0xB4000/0xC8000，USR4 0xE7000，各 0x14000**；用户预置 0xDC000..0xDFFFF；FM6 bank 0xE5000/0xE6000。Tab5 `sloopstore` 的 mmap 窗口 `[0x97000,0xFF000)` 完整覆盖采样区；HAL 已有 erase/prog/read 与故障注入，现有 `st_save` 已证明写后经 mmap 回读一致。
- **DSYN 银行是 settings 的第二段**：`persist_t`（88B）之后是 `dsu_bank_t`（1,464B）＝ 4 kit ×（name8+crush+src+2 rsv+16×22B）。P4-C 的 `song_persist.h` 已把 canonical 长度定为 1,552B，并在“无 DSU 时补默认”；但 `song_write_locked` 目前只替换 arrangement 36B，**从不把 RAM 里的 `dsu` 写回**，也没有开机 `dsu_boot`。DSYN 编辑因此只能存活到掉电。
- `dsu` API 可直接用：`dsu_kit(k)`（未初始化时落默认）、`dsu_defaults/dsu_from_factory/dsu_fix_sound/dsu_fix_kit/dsu_valid`、`dsu_dirty`、试听队列 `dsu_aud_k/lane/vel`（`drums.c` 的 `drum_audition_poll` 消费）。
- **FM6 patch 是 155B voice + 128B packed**：`FP_R1=0..FP_OP=21` 每算子 21B；`FP_PR1=126..FP_TRNSP` 全局；`FP_NAME=145`、`FP_SIZE=155`。`fm6_pack/fm6_unpack/fm6_sanitize/fm6_set_patch` 已在同一 TU；`fm6_set_patch` 只写 `fm6_patch[t]` 并递增 `fm6_pgen`，**调用方必须把 `fm6_slot[t] = trk[t].p[P_E7]`**，否则 `fm6_poll` 会在 PTCH 变化时用槽 patch 覆盖编辑（上游 `ED_FM6_PUT` 同法）。
- 现有 FM6 宏页（EDIT 组）只编辑 8 个宏（ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH）；深度 patch 编辑面是新增页面，不替代宏页。
- **容量现状**（P4-C 收口）：12 页 / 6 组；EDIT Σ est 19,968B（GLOBAL/FX/ANALOG 各 3,584 + FM6 6,144 + SAMPLE 3,072）；SEQ 32,256；FILE 20,992；SONG 12,288；base 11,624B；最坏有序对 SEQ→PRESETS 60,776B，余 2,344B。组签 168px × 6。
- 上游 host 门禁已含 `dsyn_test`、`fm6_test`、`drumkit_test`；`sample_pitch_test` 尚未接入。
- `ENG SELFTEST` 的 `.bss` 回滚会清掉 `dsu`、`usr_zone/usr_nz`、`fm6_patch/fm6_slot`；P4-C 已在自检收尾重载 arrangement，本阶段必须把 DSU 与采样槽扫描接进同一收尾。

## 3. 方案与取舍

### 3.1 采样：flash 原始槽 + PSRAM 镜像（采纳）

| 方案 | 取舍 |
|---|---|
| **A. 保留 PSRAM 镜像，flash 为权威副本（采纳）** | 引擎读路径不变（`SMP_USER_XIP` 仍指向镜像），避开音频线程直接读 mmap 的缓存/停顿不确定性；开机把 4×80KiB flash 槽拷进镜像，上传时写 flash 后同步镜像并 `smp_user_scan`。320KiB PSRAM 镜像已分配。 |
| B. `SMP_USER_XIP` 直指 mmap | 少一次拷贝，但音频线程每次 ADPCM 取字节都走 flash cache，上传擦写期间的停顿与 golden 基线都更难界定；selftest 仍需单独镜像。 |
| C. 把采样放进 A/B 对象 | 与上游槽布局不兼容，编辑器协议/备份对象无法互通，且 80KiB 放不进 4KB 对象。 |

**采用 A。** flash 侧按上游原始槽读写，不做 A/B；上传中断的语义与上游一致：`BEGIN` 先擦 header 扇区 ⇒ 槽立即失效，`END` 最后写 header（CRC 校验通过才算成功）。镜像与 flash 的同步只在开机、上传/擦除收尾、`ENG SELFTEST` 收尾做。

### 3.2 导航与容量：新增 SYNTH 组（采纳）

| 方案 | 取舍 |
|---|---|
| **A. 新增顶层组 `SYNTH`，并把 SAMPLE 参数页移入（采纳）** | 组 = SAMPLE / DSYN / FM6P；EDIT 缩为 4 页（GLOBAL/FX/ANALOG/FM6）。EDIT Σ 降到 16,896，SYNTH Σ 20,480；最坏有序对仍是 SEQ→PRESETS 60,776，SYNTH→STEP 59,240，余量健康。 |
| B. 全部塞进 EDIT（6–7 个子页） | 子签固定 5 槽的布局要改，EDIT Σ 涨到 27–30KB，EDIT→STEP 有序对 63–67KB，越过池门禁。 |
| C. 新组只放 DSYN/FM6P，SAMPLE 留 EDIT 并内嵌槽管理器 | EDIT Σ 涨到 23,040，EDIT→STEP 账面 61,800B（余 1,320B），成为第二坏；容量余量过薄。 |

**采用 A。** 组顺序 `PLAY/TRACKS/EDIT/SYNTH/SEQ/SONG/FILE`（7 组），组签宽 168→**142px**（7×142+6×8=1,042 ≤ 1,064 右端）；子行 5 槽 176px 不变。`APP_STATE_PAGES` 12→**14**，新增 `APP_PAGE_DSYN=12`、`APP_PAGE_FM6P=13`；既有页 id 不变。`TAB` 顺序变为 14 页扁平轮转。SAMPLE 参数页仍是 6 旋钮 + 底部 4 槽管理行（估 6,144B）。

### 3.3 DSYN/FM6P：每页 8 旋钮 + 页内选择器（采纳）

| 方案 | 取舍 |
|---|---|
| **A. 一页 8 旋钮 + KIT/LANE/字段页选择器（采纳）** | 复用 `knob.c`/`page_params` 的交互与格式化；DSYN 24 个虚拟字段分 3 页 + KIT 页，FM6P 6 个字段页 × 6 算子；页面对象数约 20–25，est 7,168/页。 |
| B. 每个 lane/算子一个子页面（几十个 app page） | 页面注册表与组签爆炸，池预算与导航都不成立。 |
| C. 全字段平铺（16 lane × 24 字段 / 6×21 算子字段） | 数百个对象，远超池；且屏上不可读。 |

**采用 A。** 字段页是**页内 UI 局部态**（不新增 app page）：`UI DSYN PAGE 0..3`、`UI DSYN KIT/LANE`、`UI FM6P OP 0..5`、`UI FM6P PAGE 0..5`。8 个旋钮槽由 core 提供的自定义描述符表驱动；空槽不建旋钮。

### 3.4 DSYN 持久化：写回 PER3 的 DSU 段（采纳）

| 方案 | 取舍 |
|---|---|
| **A. 继续用 `OBJ_SETTINGS` 的 88..1551（采纳）** | 与上游 `settings_write/dsu_boot` 逐字节一致；song 保存与 DSYN 保存共用同一 A/B 对象、同一把存储锁；`STORE TEST` 的整扇区快照天然覆盖它。 |
| B. 独立对象/新 magic | 上游编辑器备份对象 1（settings）读不到 SYN kit，破坏互通。 |
| C. 只在 RAM（掉电丢） | 不满足“最大程度移植”。 |

**采用 A。** `song_per_build` 改为：**任何** settings 保存（显式 `STORE SONG SAVE`、sections/歌曲延迟写、`DSYN STORE`）都把**当前 RAM `dsu`** 写进 88..1551；合法旧 PER3 的 MIDI 尾段与 side 原样保留；无 DSU 的 84/88B 对象升级为 canonical 1,552B。保存成功后清 `dsu_dirty`。因此 `dirty=0` 表示「RAM 与最近一次成功写盘一致」，不区分是显式 STORE 还是被歌曲保存顺带写回。**DSYN-only 修改不会自行触发 1s 延迟写**（上游也只在显式 `DSYN_STORE` 或下一次 settings 写时落盘）；`DSYN` 页的 `STORE` 是主路径，歌曲/段落保存是已被上游验证的顺带路径。autosave 是 `project_t`，不含 DSU，不参与。

### 3.5 采样上传：console 镜像上游 `SMP_*`（采纳）

| 方案 | 取舍 |
|---|---|
| **A. `SMP INFO/BEGIN/DATA/HDR/END/ERASE` + PC 脚本（采纳）** | 与上游协议命令 32–36 同语义，只是传输从 SysEx 换成 console 行；脚本可复用 vendored `tools/sampleio.py` 生成 header/ADPCM。 |
| B. 把 80KiB 塞进 `STORE RAW` 对象 | 对象上限 3,840B、且会破坏原始槽布局。 |
| C. 新增 SD 卡导入 | 需要 FATFS 与文件选择 UI，超出 P4-D。 |

**采用 A。** 受 USJ RX 环 256B 约束，每行最多 120B 数据（240 hex），header 480B 分 4 行 `SMP HDR` 累积。**Tab5 安全差异（明确接受并记录）**：`SMP BEGIN/DATA/HDR/END/ERASE` 在走带中一律拒绝（`SMP ERR playing`）——上游允许播放中上传；Tab5 的擦写会让两核停 cache，80KiB 上传必然造成可闻中断，故按“STOP BEFORE SAVE”纪律处理。擦写期间持存储锁、不持引擎锁；完成后同步 PSRAM 镜像并 `smp_user_scan`。

### 3.6 FM6 patch：直接编辑 live patch，bank 持久化（采纳）

- 读：`fm6_patch[track]` 的 155B（选中轨引擎必须是 FM6）。
- 写：单字段 → `fm6_sanitize` 范围 → `fm6_set_patch` → `fm6_slot[track] = trk[track].p[P_E7]`（保持“编辑后不被 PTCH 立刻覆盖”）。
- 存：复用既有 FM6 bank 27 槽（`STORE BANK PUT` / FM6 宏页的 BANK 行）。FM6P 页只加一个 `SAVE` chip 指向同一目标槽，避免两套槽状态。
- 不持久化 live patch 本身（上游同：patch 是编辑器的会话状态，除非存入 bank）。

## 4. 接口与组件设计

### 4.1 `sloop_core`（`sloop_core.h` + 单 TU 适配）

```c
/* ---- DSYN ---- */
#define SLOOP_DSYN_KITS 4u
#define SLOOP_DSYN_LANES 16u
#define SLOOP_DSYN_FIELDS 24u   /* 22 字节 + FLT 拆 MODE/RES 两虚拟字段 */
#define SLOOP_DSYN_NAME 8u
typedef struct {
    uint8_t used;                 /* 该 lane 有音色数据（kit 总是有 16 个） */
    uint8_t sound[22];            /* 原始 dsnd_t 字节（与上游/编辑器逐字节一致） */
} sloop_dsyn_sound_t;
typedef struct {
    uint8_t kit, lane;            /* 当前选择（state 的 UI 局部态；见 §4.3） */
    char name[SLOOP_DSYN_NAME + 1];
    uint8_t crush, src;
    uint8_t dirty;
    sloop_dsyn_sound_t sound;
} sloop_dsyn_snapshot_t;

bool sloop_core_dsyn_desc(uint32_t field, sloop_desc_t *out);      /* 24 字段自定义表 */
esp_err_t sloop_core_dsyn_read(uint32_t kit, uint32_t lane, sloop_dsyn_sound_t *out,
                               char name[9], uint8_t *crush, uint8_t *src);
int16_t sloop_core_dsyn_field(uint32_t kit, uint32_t lane, uint32_t field); /* 读一个虚拟字段（引擎锁） */
int16_t sloop_core_dsyn_set(uint32_t kit, uint32_t lane, uint32_t field, int16_t v); /* 返回钳位后的值 */
int16_t sloop_dsyn_field_from_sound(const uint8_t sound[22], uint32_t field); /* 纯解码，UI 投影用 */
int16_t sloop_core_dsyn_text(uint32_t field, int16_t v, char *out, uint32_t n); /* 可选：索引→ms/Hz */
esp_err_t sloop_core_dsyn_load_factory(uint32_t kit, uint32_t src);
void sloop_core_dsyn_audition(uint32_t kit, uint32_t lane, uint32_t velocity);
bool sloop_core_dsyn_dirty(void);

/* ---- FM6 patch ---- */
#define SLOOP_FM6_PATCH_SIZE 155u
#define SLOOP_FM6_OP_FIELDS 21u
#define SLOOP_FM6_GLOBAL_FIELDS 29u
typedef struct {
    uint8_t patch[SLOOP_FM6_PATCH_SIZE]; /* FM6 155B voice，未 pack */
    uint8_t slot;                        /* 当前 PTCH（P_E7），只读 */
    uint8_t engine_ok;                   /* 选中轨引擎 == FM6 */
} sloop_fm6_patch_snapshot_t;
bool sloop_core_fm6_desc(uint32_t op, uint32_t field, sloop_desc_t *out); /* op 0..5，6 = global */
esp_err_t sloop_core_fm6_patch(uint32_t track, sloop_fm6_patch_snapshot_t *out);
int16_t sloop_core_fm6_field(uint32_t track, uint32_t op, uint32_t field);
int16_t sloop_core_fm6_set(uint32_t track, uint32_t op, uint32_t field, int16_t v);
int16_t sloop_fm6_field_from_patch(const uint8_t patch[155], uint32_t op, uint32_t field); /* 纯解码 */
```

- DSYN 虚拟字段表（**恰 24 项**，顺序固定，编号即协议）：
  `WAVE / SRC / CLAP / PITCH / FINE / BEND / BTIME / HOLD / DECAY / TLEV / T2 / T2LEV / CLICK / NLEV / NHOLD / NDEC / FLT_MODE / FLT_RES / FCUT / FENV / HPF / CHIP / DRIVE / LEVEL`。
  `CLAP` 是 `SRC` 的 bit4 开关（`DN_CLAP`，单独一项便于触摸编辑；`SRC` 本身只 0..4）；`FLT_MODE` 是 `flt` 低 3 位的合法组合 `OFF/LP/BP/HP/LP+ALL/BP+ALL/HP+ALL`（`DF_ALL=4` 必须与 0..3 组合，单独 4 是无效的“只过滤音头”）；`FLT_RES` 是 bit3..7 的 0..31。写回时两者合成原始字节，读时拆分，其余位不得被另一字段改写。
  范围：WAVE 0..5、SRC 0..4、PITCH 0..127、FINE 0..15、BEND 0..96、BTIME 0..127、HOLD 0..127（2ms）、DECAY 0..127（索引）、TLEV/T2LEV/CLICK/NLEV 0..127、T2 0..31（比率 ×32）、NHOLD 0..127、NDEC 0..127、FLT_RES 0..31、FCUT 0..127、FENV -128..127、HPF 0..127、CHIP 0..127、DRIVE 0..127、LEVEL 0..255（128 = 0dB，4/ dB）。这些边界取自 `dsnd_t` 的字节宽度与 `dsu_fix_sound` 的钳位方向；上游 web 编辑器的字段元数据未随仓库 vendored，故以“字节宽度 + 上游钳位”为权威，未在 `dsu_fix_sound` 里显式钳的 `hold/nhold/t2/level` 按 `uint8`/注释语义定界（`t2` 注释明确 ×32 比率，故 0..31）。
- KIT 页（不属于 24 个声音字段）：`CRUSH` 0..255（低 nibble 丢位数、高 nibble S&H−1）、`KIT SRC`（工厂 kit 索引 0..DS_NKITS−1，套用即覆盖）、名称与来源只读。
- 描述符由 `sloop_core_dsyn_desc(field, ...)` 给出（WAVE/SRC/FLT 枚举名、`unit`）；索引类字段（BTIME/DECAY/NDEC/FCUT/HPF/CHIP）在 `sloop_core_dsyn_text()` 中把 DECAY/NDEC/BTIME 转成 ms（`DECAY_K`）、FCUT/HPF 转成 Hz（`CUTOFF_HZ`），表取自上游 `felucca_tables.h`，host 单测固定值覆盖；UI 其余字段走 `knob_math` 的通用格式化。
- 所有写入口持引擎锁、检查 `s_frozen`；`dsu_fix_sound`/`dsu_fix_kit` 负责最终钳位；`sloop_core_dsyn_load_factory` 调 `dsu_from_factory` 并置 `dsu_dirty`；`CRUSH`/`KIT SRC` 写后同样过 `dsu_fix_kit`。
- FM6 描述符表：每算子 21 项 + 全局 29 项；枚举表（LFW 0..5、LPMS 0..7、ALG 0..31 + PAT 语义对照 `N_FM6_ALG`）、范围取 `fm6_max()`；写入口调 `fm6_sanitize` + `fm6_set_patch`，并对 `track>=NPART`/非 FM6 轨/冻结拒绝。**ALG 宏覆盖**：上游 `fm6_sync` 在 `P_E0`（ALG 宏）≠ 0 时用它覆盖 patch 的 `FP_ALG`；因此深度页写 ALG 时，适配层在同一次引擎锁内把 `P_E0` 置 0（PAT），否则编辑不可听。其余宏（FB/MLVL/MRAT/MEG/VMOD/DTUN）是叠加语义，保留。

### 4.2 `sloop_store`（`sloop_store.h` + `store_tab5.c`）

```c
/* ---- 采样槽（原始 flash，无 A/B） ---- */
#define SLOOP_STORE_SAMPLES 4u
#define SLOOP_STORE_SAMPLE_SIZE 0x14000u
#define SLOOP_STORE_SAMPLE_DATA 512u
typedef struct { uint8_t used, zones; char name[9]; uint16_t data_kb; } sloop_store_sample_info_t;
esp_err_t sloop_store_sample_info(uint32_t slot, sloop_store_sample_info_t *out);
esp_err_t sloop_store_sample_begin(uint32_t slot);            /* 擦 header 扇区，槽失效 */
esp_err_t sloop_store_sample_hdr_append(const uint8_t *p, uint32_t n); /* 累计 ≤480B */
esp_err_t sloop_store_sample_write(uint32_t slot, uint32_t off, const uint8_t *data, uint32_t n);
esp_err_t sloop_store_sample_end(uint32_t slot);                /* 校验 magic/zone/CRC，最后写 header */
esp_err_t sloop_store_sample_erase(uint32_t slot);
void sloop_store_sample_reload(uint32_t slot);                  /* flash→镜像 + smp_user_scan */

/* ---- DSU（PER3 的 88..1551） ---- */
esp_err_t sloop_store_dsu_reload(void);                        /* 开机/自检后：settings→dsu */
esp_err_t sloop_store_dsu_store(void);                         /* 显式 STORE：settings 写回，停止态 */
bool sloop_store_dsu_dirty(void);
```

- 采样写路径：`sloop_hal_storage_erase/prog`（逻辑偏移 `SMP_USER_OFF(slot)`），每 4KB 边界先擦；`n ≤ 120`（console 上限）但 API 接受 ≤256；写后 `memcpy` 到 PSRAM 镜像对应位置；`END` 先读回 data 算 `st_crc32`，校验 `smp_zone_ok`，再 `prog` header，最后 `smp_user_scan`。
- `sloop_store_sample_begin/…/end/erase` 的公共门：`sloop_store_ready()`、非冻结、**非走带**、持 `s_store_lock`；BEGIN 清掉未完成上传状态；同一时点只允许一个 open slot（`s_smp_open`）。
- **BEGIN 必须同时使镜像失效**（与上游 `ed_smp_erase` 同语义）：`usr_nz[k]=0`、`usr_zone[k][*].n=0`、镜像 header 置 0xFF，再擦 flash header 扇区；否则 `SMP INFO` 会继续报告旧采样，正在发声的 voice 也可能读到半覆盖数据。`SMP DATA` 每块写 flash 后同步拷贝进镜像对应偏移（不动 header）。`END` 校验通过后写 flash header、把整槽 flash→镜像再同步一次（覆盖 header），最后 `smp_user_scan(k)`。`SMP INFO` 与引擎读路径一样**只读镜像**（上游 `smp_user_xip` 同源）；flash 只在开机装载与 END/ERASE 收尾使用。
- 开机 `sloop_store_init` 在 HAL ready 后：`memcpy` 四槽 flash→PSRAM 镜像（逐槽 80KiB），`smp_user_scan(k)`；`sloop_store_song_reload` 之后新增 `sloop_store_dsu_reload()`（读 settings 88..1551，`dsu_valid` 则 memcpy + `dsu_fix_kit`，否则 `dsu_defaults`，清 `dsu_dirty`）。
- `song_per_build` 增补：canonical/已有 DSU 的 payload 都写入当前 `dsu`；`len` 至少 1,552；未知 magic 仍拒绝；保存成功清 `dsu_dirty`。
- `STORE TEST` 不动（已快照完整 settings payload）；新增断言：测试前后 DSU 区逐字节不变（由既有整扇区恢复保证）。
- `ENG SELFTEST` 收尾在 `sloop_store_song_reload()` 后调 `sloop_store_dsu_reload()` 与四槽 `sloop_store_sample_reload()`（`.bss` 回滚后恢复镜像/扫描）。

### 4.3 状态层（`sloop_state.h` / `cmd.c` / `state.c`）

新增命令：

| 命令 | 字段 | 语义 |
|---|---|---|
| `CMD_DSYN_SET` | `a=kit,b=lane,n=field,v=value` | 单字段写（adapter 钳位），返回码写 `synth_status`；成功同拍重取 `dsyn` 快照 |
| `CMD_DSYN_LOAD_KIT` | `a=kit,b=src` | 套用工厂 kit（破坏该 kit 的编辑），置 dirty |
| `CMD_DSYN_STORE` | — | `sloop_store_dsu_store()`；走带中 → BLOCKED |
| `CMD_DSYN_AUDITION` | `a=kit,b=lane,v=velocity` | 试听（不改数据） |
| `CMD_FM6_SET` | `a=track,b=op,n=field,v=value` | 单字段写；非 FM6 轨 → LOCKED |
| `CMD_SAMPLE_ERASE` | `a=slot` | `sloop_store_sample_erase`；走带中 → BLOCKED |

新增投影：`app_state_t.dsyn`（kit/lane/name/crush/src/dirty/sound[22]）与 `app_state_t.fm6p`（patch[155]/slot/engine_ok），由 `state_sync_from_engine` 与相关命令同拍刷新；新增 `synth_status`（idle/ok/invalid/full/blocked/frozen/locked/error）。UI 只读副本，不直接调用 core/store（FILE 页的只读例外不扩到本阶段）。

**DSYN 选择器归属**：kit/lane 是 **state 保存的 UI 局部态**（与 `step_window_t`/`focus_step` 同类），不是引擎真值。新增 `state_set_dsyn_select(kit, lane)`（UI 任务写路径，钳位 0..3 / 0..15），console `UI DSYN KIT <0..3>` / `UI DSYN LANE <0..15>` 与页面 chip 共用它；`state_sync_from_engine` 用当前选择去读 `sloop_core_dsyn_read`，因此 `STATE SNAP` 能直接验证屏幕正在编辑哪个 kit/lane。`CMD_DSYN_SET` 仍显式携带 kit/lane（不隐式读选择），越界不调引擎。console `UI DSYN KIT/LANE` 与页面 chip 一样清掉「上一次动作结果」（P4-D 评审 M6；否则状态行会保留旧 lane 的 EDIT OK）。FM6P 的算子/字段页只是页面显示分组，不进入 state：`CMD_FM6_SET` 显式携带 `op/field`，`UI FM6P OP/PAGE` 只切 UI 静态量并回显，供 `TAP`/截屏对齐。

### 4.4 UI（`sloop_ui`）

- **组表**：`SLOOP_UI_GROUPS 6→7`；`k_groups[]` 插入 `{"SYNTH", 3, {APP_PAGE_SAMPLE, APP_PAGE_DSYN, APP_PAGE_FM6P}}`；EDIT 改为 4 页。`UI_GROUP_W 168→142`，静态断言覆盖右端 ≤1,064。
- **SAMPLE 页**（既有参数页 + 槽管理行）：6 个旋钮不变；底部 `U1..U4` 四个单对象 label 显示 `USED name zones KB` 或 `EMPTY`；点槽 = 把 `SET` 宏（`P_E0`）切到 **`SMP_NSETS + k`（= 8..11，不是 32）**，经 `CMD_SET_PARAM` 下发；`ERASE` chip 对选中 USR 槽发 `CMD_SAMPLE_ERASE`；状态行显示 `SET U<n>`/`ERASE OK`/`NOT USR`/`STOP BEFORE ERASE`/`NO STORAGE`/`ERASE ERROR`（**实现偏差已回写**：初稿写的是 `SAVE OK`；实际状态行由 `theme.h` 的 `ui_sample_status_text` 单一映射）。est 3,072→**6,144**。
- **DSYN 页**：页眉选择行 `SYN1..4`（4 chip）+ `LANE 01..16`（`−`/`+`）+ 字段页 chip（`SND1/SND2/SND3` + `KIT`）+ `AUDITION` + `STORE` + 状态行（**实现偏差已回写**：初稿写的是 `LANE K01..K16`/`SOUND1/2/3`）；8 个旋钮由 24 字段的当前页驱动，空槽不建。字段页布局：SOUND1 = WAVE/SRC/CLAP/PITCH/FINE/BEND/BTIME/HOLD；SOUND2 = DECAY/TLEV/T2/T2LEV/CLICK/NLEV/NHOLD/NDEC；SOUND3 = FLT_MODE/FLT_RES/FCUT/FENV/HPF/CHIP/DRIVE/LEVEL；KIT = CRUSH/SRC 工厂来源 + 名称/来源只读。est **7,168**。
- **FM6P 页**：页眉 `OP 1..6`（chip 环）+ `PAGE EG/LEVEL/PITCH/PITCH-EG/VOICE/LFO` + `SAVE`（目标 bank 槽 `−`/`+`）+ 状态行；8 旋钮按页映射 21/29 字段，空槽不建。锁定（选中轨非 FM6）时全部 `--`、chip 拒绝。**ALG 编辑**（PAGE VOICE 的首槽）写 patch 的同时清 `P_E0=0`（PAT），保证立即可听；页面状态行在 `P_E0 != 0` 时显示 `ALG MACRO n` 提示，直到用户编辑 ALG 或把宏调到 PAT。est **7,168**。
- 键盘：沿用参数页的 ←/→ 焦点、↑/↓ 调值；页内选择器经触摸/console（`UI DSYN …`/`UI FM6P …`），不新增演奏键冲突。
- `TAP` hit-test 新 kind：`SLOOP_TAP_DSYN_SEL`、`SLOOP_TAP_FM6P_SEL`、`SLOOP_TAP_SAMPLE_SLOT/ACTION`；回显与屏幕状态行同源。
- `UI MEM` 新组行；`UI` 页名表追加 `dsyn/fm6p`；`UI TAB synth` / `UI SUB 0..2`。

### 4.5 console / 脚本

```
SMP INFO
SMP BEGIN <0..3>
SMP DATA <slot> <off> <hex≤240>
SMP HDR <hex≤240>
SMP END <slot>
SMP ERASE <slot>
DSYN INFO | GET <kit> <lane> | SET <kit> <lane> <field> <value> | KIT <kit> <src> | STORE | AUDITION <kit> <lane> [vel]
FM6P INFO [track] | GET [track] | SET <track> <op> <field> <value> | ALG <track> <0..32>
```

- 错误令牌：`SMP ERR <no_storage|playing|frozen|size|header|crc|zones|flash|state>`；`DSYN/FM6P ERR <invalid|locked|frozen|playing|error>`。
- 新增 `scripts/sample_upload.py`（复用 `third_party/sloop-fm1/tools/sampleio.py` 的 `user_slot`，纯 Python，无 numpy）：
  `info | load SLOT NAME file.wav[:root[:lo-hi]] … | erase SLOT`；另有 `--sine` 自测模式生成 1,000 点正弦 fixture（无需外部 WAV），`--selftest` 离线验证 header/CRC。
- `scripts/inject/synth.sh`（场景逻辑入 `inject.py` 的 `run_synth`）：采样上传→TRIG 出声→擦除→静音；DSYN 编辑/试听/STORE/硬复位恢复/自检恢复；FM6P 编辑/锁定拒绝/SAVE→bank→GET 往返；三页截屏与像素抽检；组作用域池水位。
- `nav.sh` 更新 7 组/14 页/TAB 顺序；`params.sh` 的 EDIT Σ 改 16,896，新增 SYNTH 页 delta/锁定断言；`soak.py` 页访问表加三页与 SYNTH 组。

## 5. 测试策略与完成判据

### host / 状态层

- `scripts/host-test.sh` 增加 `sample_pitch_test`（上游 SAMPLE 音高/循环）；既有 `dsyn_test`/`fm6_test`/`drumkit_test` 必须继续全绿。
- 新增 `scripts/synth-test.sh`：同源 hostsim 编入 `sloop_core` 适配与真实 `storage.c` 假 flash，断言
  1. DSYN 24 字段读/写/钳位、`FLT` 两字段组合不改其它位、工厂 kit 套用、dirty 记账；
  2. PER3 保存写回 live DSU、旧 84/88B 升级为 1,552B、尾段/side 逐字节保留、未知 magic 拒绝；
  3. FM6 21/29 字段范围、`fm6_pack/unpack` 往返、`fm6_set_patch` 后 `fm6_slot` 语义、非 FM6 轨拒绝；
  4. 采样 header 解析（magic/version/nz/zone/data_len/CRC）与 `SMP` 分块写入的边界（off<512、非 256 对齐、超 0x14000、CRC 错、zone 越界）。
- `scripts/state-test.sh`：新命令透传、边界、`synth_status` 映射、三份投影刷新、取消/非法不改投影。
- `scripts/knob-test.sh`：DSYN/FM6P 字段格式化与索引→文本（DECAY ms、CUTOFF Hz）纯函数。

### 设备

`scripts/inject/synth.sh`（与既有场景同会话、自带归一、可重复）：
- 采样：`SMP INFO` 空 → `sample_upload.py --sine` 上传 USR1 → `INFO` name/zones/KB/CRC → `ENG TRACK 0 4 0`（SAMPLE 引擎）+ `PARAM 63 8`（P_E0 = SET = USR1）+ `ENG TRIG synth 0 60` 使 `voices>0` → `SMP ERASE 1` → 触发静音/`INFO EMPTY`；上传中断（BEGIN 后不复位）→ `INFO` 为空**且触发静音**（BEGIN 已清镜像/zone）；CRC 故意改坏 → `SMP END` 回 `crc` 且槽仍空；走带中 BEGIN/ERASE 回 `playing`。
- DSYN：`DSYN GET 0 0` 22B → `SET` WAVE/PITCH/DECAY/LEVEL → `GET` 回读 → `AUDITION` 使鼓 voices>0 → `STORE`（`DSYN INFO dirty=0`）→ 硬复位 `GET` 逐字节相同 → `ENG SELFTEST` 后 `GET` 仍相同（DSU reload）→ `KIT 0 <factory>` 恢复。
- FM6P：选轨 FM6 → `FM6P GET` 155B → `SET` ALG/OP1 OL/R1 → `FM6P GET` 回读 → `STORE BANK PUT` → `BANK GET` 128B 往返 → 非 FM6 轨 `FM6P SET` 回 `locked` → 硬复位后 bank 槽仍在（既有 bank 门禁）。
- UI：`SAMPLE/DSYN/FM6P` 三页截屏 + 像素抽检（槽卡/旋钮环/字段页标签/锁定 `--`）、`UI MEM` 的 `GROUP SYNTH built/sum_est/sum_delta`（Σ est 20,480）、跨组峰值（SYNTH 全建 → STEP 首建 `pool_free ≥4,096`）、`TAP` 回显与状态行同源。
- `nav.sh` 7 组/14 页轮转、`UI TAB synth`/`UI SUB`、离开 SYNTH 后 built=0；`params.sh` SYNTH 三页；`soak.py` 新页与组往返（pool_free 首末差 ≤128B）。

### 阶段收口

1. 两次构建零新增警告；烧录 `Hash of data verified`；启动无 fatal/abort/backtrace。
2. `host-test`/`synth-test`/`state-test`/`knob-test`/`screenshot.py --selftest` 全绿；MANIFEST 233/233。
3. 设备 `ENG SELFTEST` 182/182；九条注入场景（原八条 + `synth`）同会话全绿；三页截屏视觉/像素通过。
4. 复位后 `soak.py --duration 1800 --interval 60 --save-window 900` PASS；SYNTH 跨组往返池差 ≤128B；内部堆/xrun/栈沿用 AGENTS §5.5。
5. DSYN DSU 与采样槽在硬复位、`ENG SELFTEST` 之后均恢复；`STORE TEST` 前后 DSU/side 逐字节不变。
6. 文档（ADR-011、ADR-005/008/009、roadmap、parity spec、AGENTS）与 deferred（新遗留）同阶段收口；整分支终审 clean 后合回 dev 并 push。
7. 人耳/物理手感/人眼读图仍列为外部门禁，不冒充机器已验证。

## 6. 风险与应对

| 风险 | 应对 |
|---|---|
| 采样上传擦写造成音频停顿/xrun | 走带中拒绝；擦写包 codec mute；门禁只测停止态上传；文档记录与上游的差异。 |
| PSRAM 镜像与 flash 不同步 | 只在开机/上传收尾/擦除/自检收尾同步；每次 `SMP INFO` 以镜像 + header CRC 校验为准。 |
| DSYN 保存覆盖歌曲/尾段/side | 复用 P4-C 的 `song_per_build` 保留规则，只替换 48..83 与 88..1551；整扇区 `STORE TEST` 回归。 |
| `ENG SELFTEST` 回滚 dsu/usr_zone | 自检收尾显式 `dsu_reload` + 四槽 `sample_reload`，设备用例判别。 |
| FM6 编辑被 PTCH/fm6_poll 覆盖 | 写后设 `fm6_slot[t]=p[E7]`，host/设备用例覆盖。 |
| 新组撑破池 | 移出 SAMPLE、每页 8 旋钮 + 选择器、est 硬上限；有序对与实测 peak 双门禁。 |
| 采样 header/CRC 校验不严导致坏槽可播 | 复用 `smp_zone_ok` + `st_crc32`；设备注入覆盖 CRC/zone 负例。 |
| 字段索引→物理量显示错误 | `synth-test`/`knob-test` 固定表值，对照上游 `DECAY_K`/`CUTOFF_HZ`。 |

## 7. 实施者边界与既定假设

- 本规格由项目负责人此前授权“按推荐方式取舍、最大程度移植”后编写；新增 `SYNTH` 组与移动 SAMPLE 页是容量决策的一部分（ADR-005 更新）。
- DSYN 的 24 个虚拟字段已定稿（`CLAP` 独立、`FLT` 拆 `MODE/RES`、`CRUSH` 归 KIT 页）：这是 UI 抽象，写回仍是原始 22 字节，不改变上游数据布局；字段编号一旦实现即冻结，后续新增只能追加。
- 采样上传在走带中拒绝是**有意的 Tab5 安全差异**（上游允许）；不因此修改上游引擎。
- FM6 patch 名称保留但不提供屏上编辑；DSYN kit 名称同样只读。改名/文本输入列为 deferred。
- UI 视觉可重排（坐标/文案留给实施者）；字段范围、持久化偏移、PTCH 语义、锁序与门禁方向不得自行改变。
