# 阶段 4 / P4-C：音序与编排设计规格

> 日期：2026-11-22 ｜ 状态：已完成（机器侧，2026-11-23 Task 13 收口；人测项见 §5「阶段收口实测」末条）
> 上位文档：`docs/design/002-port-roadmap.md` §6；`2026-11-21-phase4-feature-parity-design.md` §3 P4-C。
> 上游基线：SLOOP 2.8.5 `bac9c05`（2026-11-22 再次核对远端 HEAD，未变化）。
> 决策原则：fidelity-first——功能、数据布局、时序与编辑语义等价优先；原生 Tab5 UI 可重排，但不得另造一套音序/歌曲数据模型。

## 1. 目标、非目标与反目标

### 目标

1. 完成 64 步 pattern 编辑：四 bank 视窗、每轨 LEN/DIV、TIE/CONT/SLIDE、每音/每鼓 lane 的 lvl/rat、micro ±1/64 步、fill 条件。
2. 完成参数锁：每轨 24 个 `(step,param,value)`，锁定白名单、值钳位、base 恢复、STOP/载入恢复及 FUN6 持久化均与上游一致。
3. 完成 pattern 工具：note span/delete/move/copy、rotate、length ×2/÷2、transpose、clear、上游单步 undo/redo 的真实覆盖范围。
4. 完成实时录音：arm、播放中 overdub、首音启动、count-in、QUANT OFF、长音/独立释放、free take；普通合成与鼓输入都进入上游录音状态机。
5. 完成 arranger：A–D live sections、播放中小节边界切换、quick chain、SONG 播放/loop/自然结束、SONG REC，以及停止后恢复原工作 loop。
6. 歌曲顺序使用上游 `OBJ_SETTINGS` PER3 布局持久化；A–D 内容继续使用 P4-B 的 FUN6 对象。旧设置、未知尾段与 side record 不被歌曲保存破坏。
7. 提供原生触摸/键盘入口与完整 console/注入诊断面；用 host 时序测试、状态层单测、设备字段/事件 smoke、截屏和 soak 共同证明，不以截图代替音序正确性。

### 非目标

- USR 采样上传、DSYN/FM6 深度编辑面：P4-D。
- 完整 panel/theme/lights/MIDI LEARN 设置 UI：P4-E；本阶段只实现 arrangement 所需的最小 PER3 读写与旧版本迁移。
- 联动协议与远程执行端：阶段 5；本阶段所有编辑仍经 AppState/EditCmd，避免形成本地专用 UI 写路径。
- 性能优化、FM-1 更新帧、BLE MIDI。

### 反目标

- 把 arrangement 附加到 FUN6/autosave、新建私有对象或写入 NVS。
- 把 36B `arr_config_t` 直接当成整个 settings payload。
- 用普通 `sloop_store_project_load()` 做播放中 scene 切换。
- 在音频路径擦写 flash、分配内存或操作 LVGL；在擦写期间持引擎锁。
- 为容纳新页面扩大 LVGL 池，或绕过组作用域峰值预算。

## 2. 现状与关键事实

- `seq.c` 已随 `regress.c → hostsim.c` 编入单 TU；缺的是安全公开入口、完整投影和上游 UI 动作函数，不需要重写调度器。
- 当前 `sloop_core_read_step/write_step` 已保留合成步 `micro/fill/lvl/rat`；窗口快照未投影这些字段，鼓轨只有聚合视图，参数锁没有公共读写入口。
- 当前 toggle/erase 的 TIE 折叠是 MVP 子集，不能处理 `SF_CONT` 跨后续 NOTE 的长音。P4-C 改用上游 `note_span/note_span_set/note_del/note_shift` 语义。
- 上游 `arranger.c` 只含启动逻辑；纯顺序计时在 `arranger.h`，scene 应用在 `arranger_scene.c`，live/chain/SONG REC 状态机在 `seq.c`。
- `arr_config_t` 为 36B、16 项、4 scene；在 PER3 主 payload 偏移 48，entries 从 52 开始。A–D 与 autosave 不保存歌曲顺序；quick chain、SONG REC 临时态、`arrangement_enabled` 均不持久化。
- P4-B `STORE TEST` 只保存/恢复 settings 主 payload。本阶段开始真实使用 settings 前，必须先把测试快照扩成两个完整 4KB 副本，保护 side record。
- 上游 `fm1_ms` 在本固件没有周期推进；sections 延迟写入必须使用 `esp_timer` 的毫秒时钟。
- `ENG SELFTEST` 回滚本 TU `.data/.bss`；arrangement 与适配层运行状态必须采用显式恢复或 `.noinit` + 冷启动初始化策略，不能自检后悄悄回默认歌曲。

## 3. 方案与取舍

### 3.1 上游接入与 settings

| 方案 | 取舍 |
|---|---|
| **A. 保留 `PROJ_HOST project.c`，adapter 补最小 PER3/sections/arranger（采纳）** | 保留 P4-B 已验证的 FUN1–6、autosave、预置路径；显式编入 `arranger_scene.c`，只补歌曲所需设置子集。格式兼容且依赖面可控。 |
| B. 完整编入 `!PROJ_HOST project.c` | 可直接复用全部设置/sections，但会同时引入 panel/theme/lights/MIDI/旧 flash 启动路径，实质提前实施 P4-E，并与现有 autosave/暂存/预置适配冲突。 |
| C. 私有 SONG 对象或内存-only arrangement | 实现最短，但重启丢歌或失去 FM-1 备份/编辑器互通，违背最大保真目标。 |

**采用 A。** 在 `regress.c` include 前启用 `FELUCCA_ARRANGER=1`，保证 `arranger.c → seq.c` 顺序；进入 P4-B `project.c(PROJ_HOST)` 前按适配层需要控制宏作用域，随后显式 include `arranger_scene.c`。vendored 文件保持只读。

PER3 写规则：

- 无 settings：按上游首次正常 `settings_save()` 构造 canonical PER3：**88B `persist_t` + 1,464B 默认 DSU bank = 1,552B**，默认 MIDI 尾段省略；同时生成上游默认 80B side record（theme + 空收藏 + CRC）。首次修改歌曲时才写 flash。合法但尚未带 DSU 的 PER3 在下一次歌曲保存时补默认 DSU，已有尾段不得重写。
- 合法 PER3（含历史长度与 MIDI 尾段）：保留整个 payload，只替换 arrangement 36B；side record 原样随新 A/B 副本提交。
- PER1/PER2：按上游 `persist_boot` 兼容口径迁移，未识别字段取上游默认，不用空白覆盖。
- 未知 magic/损坏对象：拒绝歌曲保存并返回 `invalid`，不得当成空设置覆盖。
- 保存失败保留 `song_dirty`，后续按上游停止/安静/idle 门限重试；这是对上游「清 dirty 过早」缺陷的可靠性修正，文档化，不改变成功路径格式。

### 3.2 编辑命令粒度

采用「一条 EditCmd = 一个完整上游动作 = 一次引擎锁」；不让状态层通过几十次 `read_step/write_step` 模拟 span/rotate/lock。公共 API 只暴露 POD 命令/快照和 `esp_err_t`，不暴露 `track_t` 或上游 static 函数。

- `STEP_EDIT`：span、note delete/move/copy、lvl、rat、micro、fill、slide、clear。
- `PATTERN`：rotate、length、transpose、clear、undo/redo。
- `LOCK`：set/delete/clear-step/list；第 25 项响亮返回 `full`，不可锁参数返回 `invalid`。
- `REC`：toggle/arm/off、quant/count/tempo 运行期配置、fill 表演控制。
- `SONG`：section store/load/erase/request、chain begin/commit/cancel、song mode/play/stop、SONG REC、arrangement entry 编辑与 loop。chain begin(first) 立即发普通 `live_req` 并清旧 chain；begin 后的中间点击只进 UI 的临时列表，commit 携带完整 1..8 项数组原子安装 `chain_sec/chain_n`（首项保持已请求状态），cancel 清临时列表和运行 chain。console 的 `CHAIN <scene...>` 在同一公共 API 内依次执行 begin+commit；触摸在 SAVE 层按下/释放分别对应 begin/commit。

所有公共写入口检查诊断冻结、轨/步/参数边界，并由 adapter 持引擎锁。上游 `fm1_irq_off/on` 在 hostsim 是空 shim，不能当作 Tab5 并发保护。

### 3.3 精确上游语义

- LEN 1..64；DIV 使用上游 9 项编码，不在 UI 另建网格时钟。
- `rat=0..3` 表示 x1..x4；lvl 为 NORM/GHOST/SOFT/HARD；micro -32..31，单位 1/64 步。
- fill 为 normal/fill-only/no-fill；条件失败时该步不触发音符也不执行参数锁。
- 清空一个步采用上游 SEQ 清步口径：同时清该步 lock/micro/fill；P4-C 更新旧 MVP 测试，不继续保留「删音但留下不可见锁」的旧行为。
- rotate 与 pattern length 只移动/复制 `step[]`；micro/fill/lock 锚定原绝对步。transpose 对全部 64 步有效、鼓轨拒绝。note move/copy 则按上游携带 micro/fill 并重定位/复制锁。
- undo/redo 严格保持上游范围：一轨的 64 步、micro 与 LEN；**不包括 fill、lock 和其它参数**。UI 不宣称完整工程 undo。
- 场景切换调用 `arrangement_apply()`：释放旧话音/ARP/roll，音频块边界 `proj_apply(scene, all=0)`；BPM 与普通全局 FX 不随 scene 改变。
- live section STORE 播放中允许：立即捕获 RAM 槽并置 dirty；停止、无发声、输入 idle ≥1.5s 后落盘。普通 PROJECT SAVE 仍是停止态立即写。
- quick chain 最多 8 项、可重复、按 `section_bars()` 自动时长并循环；SONG 最多 16 项、每项 1..64 bars。SONG REC 的超长拆分、16 项自动完成、空录制 `NO SONG` 与 STOP 结算均保持上游。

## 4. 接口与组件设计

### 4.1 `sloop_core` / `sloop_store`

新增 `include/sloop_seq.h`，由 `src/seq_tab5.c` 实现；`store_tab5.c` 继续承载存储与 PER3/sections 适配。接口分四组：

1. **详细步与锁快照**：窗口内合成步补 `lvl/rat/micro/fill`；新增原始鼓 16-lane 读写、24 锁枚举、录音状态快照。
2. **编辑执行**：POD op + 参数结构，返回 `ESP_OK/INVALID_ARG/NOT_FOUND/NO_MEM/INVALID_STATE`；console 将 `NO_MEM` 映射为 `full`，UI 显示同一状态文本。
3. **录音**：状态至少区分 `OFF/ARMED/RECORDING/COUNT_IN/FREE_TAKE`，含目标轨、quant/count/tempo mode、一次性 take 结果。选轨调用上游 `rec_follow()`。物理 PLAY 对 free take 是 drop；外部/API PLAY 是 close，两个入口不得混用。
4. **arranger**：只读快照含 enabled、running、index/bar、16 项配置、ready/live/dirty/chain/srec；编辑与动作经状态层命令。

诊断命令新增 `SEQ STEP/DSTEP/LOCK/PATTERN/REC/FILL/SONG/CHAIN/SECTION` 子族；与 UI 使用同一 API。增加块级事件计数/小型跟踪缓冲，至少能判别 ratchet 触发次数、micro 顺序、SLIDE 是否重新 attack；串口轮询 `voices` 不作为时序证明。

> **落地差异（Task 11 终审接受，host-only）**：块级事件计数/小型跟踪缓冲**未实现**；
> ratchet 次数、micro 顺序与 SLIDE 重新 attack 的时序证明由 host 同源 `scripts/seq-test.sh`
> （真实上游 `seq.c` 的块函数 + 逐样本断言）承担，设备侧只覆盖状态/投影/回显/像素。
> 这是**明确接受**的 host-only 缺口，不是静默省略；设备注入不以串口 `voices` 轮询冒充时序证据。

### 4.2 存储与延迟任务

- PER3/sections 写与手动 STORE、autosave 共用 `s_store_lock`；捕获在引擎锁内，擦写在锁外。
- sections/autosave 的周期任务使用 `esp_timer_get_time()`，不依赖 `fm1_ms`；诊断冻结时不写。
- `song_keep`（3,816B）和新增大型暂存放 PSRAM；若上游 `.pool` 声明需要宏重定向，采用 P4-B 同法。内部堆门禁不放宽。
- `STORE TEST` 先读两个原始扇区，结束后逐字节恢复并校验；测试前后 arrangement、payload 尾段、side record 均不变。
- SELFTEST 结束后从 settings 重载 arrangement 并恢复必要钩子；新适配状态不得因 BSS 回滚形成半旧半新的视图。

### 4.3 状态层

`app_state_t` 增加详细窗口步、录音状态、arranger 只读投影和最近 `seq_status`。写入仍只有 `state_apply`；console 诊断执行器与既有 STORE 例外一致。LOAD/SONG STOP 等引擎侧改选轨时同步修正 `focus_track`，关闭 DI-24，避免步编辑目标与选中轨分离。

stub host 测试必须覆盖：命令校验、完整字段透传、锁满/非法参数、工具返回码、录音/歌曲投影、scene 载入后焦点与选中轨一致、存储失败不改投影。

### 4.4 UI 与容量

- `APP_STATE_PAGES` 11→12，新增 `APP_PAGE_SONG`。
- SONG 使用**独立顶层组**，组顺序 `PLAY/TRACKS/EDIT/SEQ/SONG/FILE`；SONG 组只有 SONG 一页。这样不增加 SEQ 稳态占用，也不改变最坏 FILE→STEP 有序对。6 个组签仍在 72px 页签条内，16pt 字体不变。
- STEP 保持 64 格对象复用；用格内/边框与状态行呈现 TIE/SLIDE/lvl/rat/micro/fill，不增加每格 label。鼓格进入 lane 选择/编辑模式，不再显示“PHASE 4”只读提示。
- TRANSPORT 扩成走带 + REC/fill + 聚焦步编辑 + pattern 工具区，使用单对象可点击 label，复用现有 ±/焦点模型。预算上限暂定 9,216B，实施后按实测收紧。
- SONG 页包含 A–D section 卡、16 项顺序、bars/loop、SONG/CHAIN/REC/STORE/ERASE 控件与状态行；初估 12,288B，最终 est 必须来自 `UI MEM` 实测。
- 以 est 上限复核最坏有序对（Task 11 落地实测 base **11,624B**，页签单对象化）：最坏 SEQ（23,040+9,216）→PRESETS 为 **60,776B**、次坏 SEQ→SONG **60,264B**、FILE→STEP **59,752B**、SONG→PRESETS **40,808B**，均满足 `11,624 + … + 4,096 ≤ 63,120`；设备实测峰值 SEQ→PRESETS `pool_free 8,324B`、FILE→STEP `9,744–9,860B`。任何超预算先改对象设计，不直接抬 est/池。
- `TAB` 顺序为 PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT→SONG→PROJECTS→PRESETS→PLAY；`UI TAB song` 与物理组签共用动作函数。
- 触摸覆盖全部新编辑动作；实体键盘至少覆盖演奏、REC/PLAY/STOP、fill、导航、焦点与聚焦步编辑，不要求为 16 个歌曲项各设专用键。选中鼓轨时**取消回退合成轨 0**：低行 `z x c v b n m .` 映射 lane 0..7，高行 `q w e r t y u i` 映射 lane 8..15，经 `CMD_DRUM_HIT` 进入同一 `drum_input()/rec_hit()` 路径；其它演奏音阶键在鼓轨忽略。ADR-006 同步该表，注入测试覆盖 16 lane down/up 与实时录音。

## 5. 测试策略与完成判据

### host / 状态层

`scripts/host-test.sh` 增加上游 `hold_rec_test`、`len_test`、`studio_drums_test`、`arranger_test`、`song_audio_test`；已有 `seq2_test` 的 micro/fill/lock/quick-chain 段成为必跑门禁。新增 adapter PER3 与 pattern 工具测试，断言：

- PER3 偏移/长度、PER1/2 迁移、尾段与 side 保留、损坏拒绝、torn write；
- POLY/CONT span、note 删除/移动/复制、工具锚定字段、undo 范围；
- REC 首音/overdub/独立释放/free take/count-in；
- SONG A2+B3 自然结束与 loop 恢复、缺 scene 拒绝、chain 与 SONG REC。

### 设备

新增 `scripts/inject/sequence.sh`，与既有六场景同会话执行。覆盖：四 bank 编辑、LEN/DIV、TIE/CONT/SLIDE、lvl/rat/micro/fill、24/25 锁、pattern 工具与 undo、录音状态与落步、live section dirty/flush、A/B/A chain、SONG A2+B3、SONG REC、重启恢复 arrangement 且不恢复临时态、`STORE TEST` 与 SELFTEST 后歌曲保持。

UI 证据至少含 STEP 深度态、TRANSPORT 录音/工具态、SONG 编辑态、SONG 播放 scene 切换前后、错误/满锁/空场景状态；PNG 必须视觉读取。原始 transcript、截图与 SHA256 晋升到 `docs/dev/verify/4-pc-2026-11-22/`。

阶段收口：

1. 两次构建零新增警告；烧录 Hash verified，启动无 fatal/abort/backtrace。
2. host/state/knob/screenshot selftest 全绿；设备 182/182。
3. 七条注入场景（原六条 + sequence）全绿；SONG/STEP/TRANSPORT 截屏视觉通过。
4. 复位后 `soak.py --duration 1800 --interval 60`（含 storage save window）通过；新组往返池差 ≤128B，内部堆/xrun/栈沿用 AGENTS §5.5。
5. manifest 233/233；文档、deferred issues、verification-log 与证据同阶段收口。
6. 整分支终审 clean 后合回 dev 并 push；人耳、物理键盘/触摸手感继续列为外部门禁，不冒充机器已验证。

### 阶段收口实测（2026-11-23，Task 13）

最终树 `2f370d8`（clean 工作树）上按本清单跑完机器门禁，证据包
`docs/dev/verify/4-pc-2026-11-22/`（`readme.md` 门禁→命令→结论 + `SHA256SUMS` 127 项，
`sha256sum -c` 全过）：

1. 构建 clean + 增量：rc=0、**0 警告**（clean 首遍仅 §5.5 豁免的 kconfig 提示对）；镜像
   `0x192a70`（1,649,264B）；烧录三段 `Hash of data verified`；启动 `App version: 2f370d8`、
   12 页注册、内部堆 98,179B、无 fatal/panic/abort/WDT。
2. `host-test` 全绿（含 `len`/`hold_rec`/`studio_drums`/`arranger`/`song_audio` 与 fixture
   `--host-check`）、`state-test` 28/0、`seq-test`（含 persist 段）0 failures、`knob-test`
   7/0、`screenshot.py --selftest` OK、MANIFEST 233/233。
3. 设备 `ENG SELFTEST` **182/182**（`checks=7 check_fail=0 deterministic=1 result=PASS`；
   6 行单 job `task_wdt` 为 DI-12 已知诊断噪声）。
4. 八条注入场景**同会话**：**2,580 PASS / 0 FAIL**（transport 288、step_edit 196、tracks 11、
   nav 140、params 149、store 903、song 331、sequence 562），全部 exit 0；`store` 内部 5 次、
   `sequence` 内部 1 次按设计硬复位，脚本之间不重启。
5. 复位后 `soak.py --duration 1800 --interval 60 --save-window 900` **PASS**（DI-26 顺序：
   先 182 自检 → esptool 硬复位 → soak）：31 次采样/1801s、`internal_free` 93,011→92,991B
   （`internal_min` 86,148 ≥60KB）、`streaming=1 xrun=0`（窗口增量 0 ≤16）、`duty_mean`
   最大 12.54% ≤18.88%、音频栈 3,120–3,168B、跨组池往返 Δ−52B（单次 −40B ≤128B）、
   无 panic/WDT。
6. 人测项（物理键盘/触摸手感、结截图目检、听感）仍是外部门禁；整分支评审与 dev 合并/推送
   由控制器在证据包之后执行，本任务不含。

## 6. 风险与应对

| 风险 | 应对 |
|---|---|
| include 顺序只编了 arranger 启动、漏掉 seq live/chain | 编译期符号断言 + host quick-chain/song-audio 用例。 |
| settings 覆盖写丢 DSU/MIDI/side | 完整 payload 保留 + 整扇区测试快照 + 字节级回归。 |
| scene 切换递归取锁或写 flash | 渲染路径只调内部 locked 函数；dirty writer 停止后才工作。 |
| SELFTEST 回滚歌曲状态 | 自检后显式重载 + 设备判别用例。 |
| 新页面撑破池 | SONG 独立组；TRANSPORT/SONG 硬 est；枚举全部有序对并实测峰值。 |
| 截图绿但时序错 | host 块级断言 + 设备事件跟踪；截图只验呈现。 |
| DI-26 自检泄漏影响 soak | 本阶段不掩盖；selftest 后复位再 soak，P4-F 前必须修。 |

## 7. 实施者边界与既定假设

- 本规格由项目负责人此前授权“按推荐方式取舍、最大程度移植”后直接执行；实时录音纳入 P4-C，而不是另立子里程碑。
- 为保真而切换清步/删除语义，允许更新旧 MVP 测试期望；不得放宽断言来适配错误实现。
- REC 的 quant/count/tempo 配置本阶段可运行期修改，持久化随完整 panel 设置归 P4-E；arrangement 本身必须本阶段持久化。
- UI 视觉可重排，具体文案/坐标留给实施者；上游数据语义、锁序、持久化布局和测试方向不得自行改变。
