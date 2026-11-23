# ADR-011: 声音设计面接入 — SYNTH 组、DSYN 24 字段、DSU 随 PER3、采样槽与 FM6 深度 patch

> 状态：已接受（阶段 4 P4-D 收口，Task 12 定稿）｜ 日期：2026-11-23
> 关联：P4-D spec `docs/superpowers/specs/2026-11-23-p4d-sampling-dsyn-fm6-design.md`；
> ADR-004（vendored 只读）、ADR-005（分组导航/池预算）、ADR-007（音频生命周期与锁）、
> ADR-008（参数页框架/每页预算）、ADR-009（存储后端）、ADR-010（音序与编排）。

## 背景

P4-A 到 P4-C 已把参数页框架、存储后端与音序/编排带到设备上，但声音设计面仍有三块空洞：

1. **采样槽未接线**：`SMP_USER_XIP` 已重定向到 `sloop_hal_user_samples()` 的 4×80KiB PSRAM
   镜像，但没有任何代码把 flash 的 USR1–USR4 原始槽装进镜像，也没有 `smp_user_scan()`
   调用——SAMPLE/GRAIN/鼓 KIT 的 USR 选项永远静音。
2. **DSYN（SYN1–SYN4）没有编辑面**：`dsu` 的 4 kit × 16 lane × 22B 只在引擎里，RAM 修改
   不持久化；P4-C 的 `song_persist.h` 已把 settings canonical 长度定为 1,552B 并在缺 DSU
   时补默认，但保存路径从不把 RAM `dsu` 写回，也没有开机 `dsu_boot`。
3. **FM6 深度 patch 不可编辑**：现有 FM6 宏页只有 8 个宏，155B voice（6 算子 × 21B +
   29B 全局）没有 UI/console 入口。

P4-D 要在不扩 LVGL 池、不改上游数据布局的前提下补齐这三块，并把阶段 4 的机器门禁
（构建/烧录/182 自检/九条注入场景/30min soak）一次跑绿。本文记录落地时的关键取舍与
实测容量，供 P4-E/P4-F 与联动模式参考。

## 决策

### 1. 新增 SYNTH 组，SAMPLE 从 EDIT 移入（容量取舍）

组顺序 `PLAY / TRACKS / EDIT / SYNTH / SEQ / SONG / FILE`（7 组），SYNTH = SAMPLE / DSYN /
FM6P；EDIT 缩为 GLOBAL / FX / ANALOG / FM6 四页。`APP_STATE_PAGES` 12→**14**
（`APP_PAGE_DSYN=12`、`APP_PAGE_FM6P=13`，既有页 id 不变），`TAB` 变 14 页扁平轮转。

被否方案：

- 全部塞进 EDIT（6–7 子页）：子签固定 5 槽要改版式，EDIT Σ 涨到 27–30KB，EDIT→STEP
  有序对 63–67KB，越过 63,120B 池门禁。
- 新组只放 DSYN/FM6P、SAMPLE 留 EDIT：EDIT Σ 23,040，EDIT→STEP 账面 61,800（余 1,320），
  成为第二坏有序对，余量过薄。

组 est：EDIT = 3×3,584 + 6,144 = **16,896**；SYNTH = 6,144 + 7,168 + 7,168 = **20,480**。
组签宽 168→**142px**（7×142+6×8=1,042 ≤ 1,064）。有序对按 ADR-005 §5 重算：最坏仍是
SEQ→PRESETS **60,776**（余 2,344B），新增 SYNTH→STEP **59,240**（余 3,880B）、
EDIT→STEP 55,656（余 7,464B）。槽管理器/FIELD 页都控制在 8 旋钮 + 页眉选择器，不把
16 lane × 24 字段或 6 算子 × 21 字段平铺。

### 2. DSYN：24 个虚拟字段 = UI 抽象，写回仍是原始 22B

`dsnd_t` 是 22 字节，但两个字节承载组合位，直接按字节开旋钮会让一个字段改写别的字段。
适配层把 UI/协议面固定为 **24 个虚拟字段**（顺序即协议、编号冻结、后续只能追加）：

```text
WAVE SRC CLAP PITCH FINE BEND BTIME HOLD
DECAY TLEV T2 T2LEV CLICK NLEV NHOLD NDEC
FLT_MODE FLT_RES FCUT FENV HPF CHIP DRIVE LEVEL
```

- `CLAP` 是 `SRC` 的 bit4 开关，独立一项便于触摸编辑；`SRC` 本身只 0..4。
- `FLT_MODE` 是 `flt` 低 3 位的合法组合 `OFF/LP/BP/HP/LP+ALL/BP+ALL/HP+ALL`（`DF_ALL=4`
  单独出现是无效的「只过滤音头」，读写都归一为 OFF）；`FLT_RES` 是 bit3..7 的 0..31。
- 写一个字段只改它自己的位，其余位逐字节不动；读用纯函数
  `sloop_dsyn_field_from_sound()`（UI 投影不取引擎锁）。
- 范围取自 `dsnd_t` 字节宽度与 `dsu_fix_sound` 的钳位方向；`dsu_fix_sound` 不钳的
  `hold/nhold/t2/level/flt/fenv` 由描述符范围无条件钳位，写后回读返回钳位值。
- 8 个字段/页（SND1/SND2/SND3）+ KIT 页（CRUSH 0..255、KIT SRC 工厂索引，套用即覆盖），
  `sloop_core_dsyn_text()` 只负责索引→物理量（DECAY/BTIME/NDEC 的 ms 用 `DECAY_K` 反解、
  FCUT/HPF 的 Hz 用 `CUTOFF_HZ`、LEVEL 的 0.25dB），UI 不引入上游表。
- 写入口持引擎锁、写后 `dsu_fix_sound` 再回读并置 `dsu_dirty`；冻结回
  `SLOOP_DSYN_INVALID`。kit/lane 选择是状态层 UI 局部态（`state_set_dsyn_select`），
  与 `step_window_t` 同类，不是引擎真值。

### 3. DSU 随 PER3 保存：任何 settings 保存都写回当前 RAM `dsu`

上游只在 settings 对象里存 DSU（88..1,551），Tab5 沿用同一对象、同一把存储锁：

- `song_per_put_dsu(payload,len)` 在每次保存组装收尾把当前 RAM `dsu` 写进 88..1,551
  （`len ≥ 1,552`）。canonical/84/88B 对象的「补 DSU」与已有 PER3 的「替换 DSU」是同一条
  规则；48..83 的 arrangement 与 1,552 之后的 MIDI 尾段、side record 逐字节保留。
- **任何** settings 保存都写回：显式 `STORE SONG SAVE`、sections/歌曲的 1s 延迟写、
  `DSYN STORE`。因此 `dirty=0` 的含义是「RAM 与最近一次成功写盘一致」，不区分是谁触发。
- **DSYN-only 修改不触发 1s 延迟写**（上游也不；`DSYN` 页的 `STORE` 是主路径，歌曲/段落
  保存是顺带路径）。这条已写入 deferred（DI-35）作为产品语义待定项。
- `song_per_saved(ok,payload,len)` 只在写成功**且**刚提交的字节就是当前 RAM（擦写窗口内
  无新编辑）时清 `dsu_dirty`；失败保留 dirty、下次重试。
- 开机 `sloop_store_init` 在工程 fetch/歌曲之后、`READY` 置位之前
  `sloop_store_dsu_reload()`：settings 88..1,551 → `dsu`（`dsu_valid` 才覆盖并逐 kit
  `dsu_fix_kit`，否则 `dsu_defaults`，清 dirty）。

被否方案：独立对象/新 magic（上游编辑器备份读不到 SYN kit，破坏互通）；只存 RAM
（掉电即丢，不满足最大程度移植）。

### 4. 采样槽：flash 原始槽为权威，PSRAM 镜像是引擎唯一读路径

- flash 侧按上游原始布局读写：USR1–3 `0xA0000/0xB4000/0xC8000`、USR4 `0xE7000`，各
  `0x14000`；header 480B（`smp_user_hdr_t`：magic `FSMP`、version 1、nz、name 8、
  data_len、CRC32、16×zone），data 从槽内 **+512** 起（IMA ADPCM 4bit）。**没有 A/B**。
- 引擎读路径不变：`SMP_USER_XIP` 指向 `sloop_hal_user_samples()` 的 4×80KiB PSRAM 镜像，
  避免音频线程逐字节走 flash cache。镜像与 flash 只在开机、上传/擦除收尾、自检收尾同步。
- **BEGIN 必须同时失效镜像**（与上游 `ed_smp_erase` 同语义）：`usr_nz[k]=0`、
  `usr_zone[k][*].n=0`、镜像 header 置 0xFF，再擦 flash header 扇区。否则 `SMP INFO` 会
  继续报告旧采样，正在发声的 voice 还可能读到半覆盖数据。「BEGIN 不 END」因此在两个
  视图上都等于空槽——设备用例以此判别。
- `DATA` 每块写 flash 后同步镜像对应偏移（不动 header）；每 4KB 扇区首次写入前擦除。
  `END` 先校验 magic/version/nz/`smp_zone_ok` 与 **flash 回读数据**的 `st_crc32`，再写
  header（提交点），最后整槽 flash→镜像并 `smp_user_scan(k)`；失败时槽仍空、上传保持
  open 可重试。`ERASE` 先失效（header 0xFF + 扫描递增 gen）再清镜像数据区与擦 flash。
- `SMP INFO` 与引擎同源只读镜像 + header CRC；4 槽信息读是文档化只读例外，页面最短
  250ms 刷新、key 稳定 1s，绝不每 30ms 做 4×80KiB CRC。
- 被否方案：`SMP_USER_XIP` 直指 mmap（音频线程的 cache 停顿与 golden 基线难界定）；
  把采样塞进 A/B 对象（0x14000 放不进 4KB 对象，且破坏上游/编辑器互通）。

### 5. 走带中拒绝采样上传：有意的 Tab5 安全差异

`SMP BEGIN/DATA/HDR/END/ERASE` 在走带中一律 `SMP ERR playing`；上游允许播放中上传。
理由：Tab5 的擦写会让两核停 cache，80KiB 上传必然造成可闻中断；这里按「STOP BEFORE
SAVE」纪律处理，并只测停止态上传。为减小停止态的干扰，擦写包 codec mute。这是**记录在
案的差异**，不因此改上游引擎。

### 6. FM6：ALG 是全局字段 8，写它必须同锁清宏

- 155B patch = 6 算子 × 21B（`FP_R1..FP_DET`，绝对偏移 `k*FP_OP + field`）+ 全局 29 项
  （`FP_PR1..FP_TRNSP` = 126..144，名字 145..154 只读）。op 0..5 是算子，op 6 是全局块。
- 全局 ALG 的绝对偏移是 `FP_ALG - FP_PR1` = **134 − 126 = 8**（公共常量
  `SLOOP_FM6_ALG_FIELD`）。上游 `fm6_sync` 在 `P_E0`（ALG 宏）≠ 0 时用它覆盖 patch 的
  `FP_ALG`；因此写 ALG 时适配层在**同一次引擎锁内**把 `P_E0` 置 0（PAT），否则编辑听不到。
  其余宏（FB/MLVL/MRAT/MEG/VMOD/DTUN）是叠加语义，保留。
- 写路径：钳位 → `fm6_sanitize` → `fm6_set_patch` → `fm6_slot[track] = p[P_E7]`（上游
  `ED_FM6_PUT` 同法）。少了最后一步，`fm6_poll` 会在 PTCH 变化时用槽 patch 覆盖编辑。
- 非 FM6 合成轨拒绝（`ESP_ERR_NOT_ALLOWED` / 页面 `NOT FM6`），鼓轨同；持久化只走既有
  FM6 bank 27 槽（`STORE BANK PUT/GET`，槽 k 的音源 = PTCH `8+k`）；**live patch 本身不
  持久化**（上游同：会话状态，除非存入 bank）——这条掉电语义记入 DI-36。

### 7. `ENG SELFTEST` 收尾重载 DSU 与采样槽

`selftest_restore()` 把本 TU 的 `.bss` 回滚到 init 快照，`dsu`、`usr_zone/usr_nz`、
`fm6_patch/fm6_slot` 都会回到默认。P4-C 已在自检收尾重载 arrangement；P4-D 把同一条
收尾扩为：

1. `sloop_store_song_reload()`（flash 为准；读不到则上游默认歌曲）；
2. `sloop_store_dsu_reload()`（settings 88..1,551 → `dsu`，或 `dsu_defaults` 并清 dirty）；
3. 四槽 `sloop_store_sample_reload(k)`（flash→镜像 + `smp_user_scan`）。

这样「自检后 DSYN 22B 与采样槽与自检前逐字节一致」成为设备用例可判别的事实（`synth`
场景 ②/① 断言）。FM6 bank 的入口重扫（ADR-009 §8 裁决 (a)）不变。

### 8. 实测容量（P4-D Task 11/12 归档证据）

LVGL 池 64KB（`CONFIG_LV_MEM_SIZE=65536`）、可用 63,120B。任务结束时实测（每页取跨快照
min–max，**不同快照的 delta 不相加**）：

| 项 | 实测 | est | 结论 |
|---|---|---|---|
| SAMPLE `UI MEM` delta | 4,360–4,508B | 6,144 | 用到 71–73%；est 是 spec §4.4 固定上限（预留不回收） |
| DSYN `UI MEM` delta | 6,344–6,452B | 7,168 | 余 716–824B |
| FM6P `UI MEM` delta | 6,980–7,092B | 7,168 | 余 76–188B（最薄，加控件前先减对象） |
| SYNTH `sum_delta`（`built=3` 同拍） | 17,696–17,936B | 20,480 | 同组三页共驻留 |
| SYNTH→STEP 过渡 `pool_free` | **11,244B**（`synth` ⑤；STEP 池增量 22,188B、耗时 31ms，复跑 27ms） | ≥4,096 | 过渡后稳态 28,800B |
| FILE→STEP 过渡 `pool_free` | **8,924B**（`song` ⑩；STEP 池增量 21,640B） | ≥4,096 | 最终矩阵的最低跨组余量（`nav` 的 FM6P→STEP 为 10,400B） |
| 组往返 `pool_free` 首末差 | −84B / −48B（Task 11 两轮）与 −64B（Task 12；10 轮 SEQ/SONG/FILE/SYNTH，代表页轮换；单次最大 92B） | ≤128B | 不泄漏 |

构建时 `build_ms`：SYNTH 组内 12–13ms、STEP 首建 27–31ms，均 ≤100ms。这些数字与
ADR-005 §5.1、ADR-008 §3 的表一致；后续改页面必须重跑同一门禁，不能只改 est。

## 后果

- 声音设计面三页（SAMPLE/DSYN/FM6P）与 SYNTH 组进入常态导航；EDIT 组缩为 4 页。
- 数据布局、钳位与持久化对象全部沿用上游：编辑器备份对象（settings + project + 采样原始
  槽）可与 Tab5 互通的方向保留。
- 走带中拒绝上传是唯一的显式行为差异，已记录并可被 `SMP ERR playing` 断言。
- 遗留（见 `docs/deferred-issues.md`）：SAMPLE 名称/DSYN kit 名/FM6 patch 名的屏上文本
  输入（DI-37）、采样 WAV/SD 导入 UI（DI-34）、DSYN-only 修改的延迟写策略（DI-35）、
  FM6 live patch 掉电语义（DI-36）、自检堆泄漏（DI-26，P4-F 前处理）。
- 人耳听感（DSYN 试听、采样出声、FM6 patch 音色）与物理触摸手感仍是外部门禁，不由机器
  证据冒充。
