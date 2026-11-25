# 012 — 阶段 4 功能对齐清单（sloop-fm1 2.8.5 → Tab5）

> 建立日期：2026-11-24（P4-F Task 1）。口径见 spec `2026-11-24-p4f-phase4-closeout-design.md` A2/A8：
> **特性级对齐**——「上游有入口且 Tab5 有 UI 入口 + 机器证据」才算 ✅；仅 console `PARAM` 可写**不算**用户可达。
> 「全绿」= 无未处置行：✅ 有证据，或 ○/◐ 有明确处置（P4-F 任务 / DI / 非目标）。
> 来源：上游 `firmware/src/params.c` `PAGES[]`（:396）与 `page_for_drum`（:447）、`ui.c`、`ui_input.c`（层键 :232–360 of SLOOP.md；TOOLS :568–612）、`ui_menu.c`、`ui_studio.c`/`ui_song.c`/`ui_vis.c`；上游手册 `SLOOP.md`（bac9c05，只读抓取，未入库）。
> 本文档随各任务打勾；**终态于 P4-F Task 10a 定稿（2026-11-25）**：Task 4–7 交付的行全部有机器证据，
> 余下的 ○/◐ 都有 DI 或非目标处置（§5）。证据包 `docs/dev/verify/4-pf-2026-11-24/`。

## 1. 状态总览（P4-F 终态，Task 10a 定稿 2026-11-25）

| 状态 | 行数 | 处置 |
|---|---|---|
| ✅ 已对齐（机器证据） | **42** | —（其中 1 行 LIGHTS 为「✅ 键盘 RGB / 非目标 KEYS·NOTES」双状态，计入 ✅） |
| ◐ 部分 | **5** | 1 → DI-45（EDIT 层 live 擦除变体）；1 → DI-49（SEL 层键盘行为，参数已在 SCALE 页）；1 → 既有 DI-39（输出路由，硬件）；1 非目标（阶段 5 的 MIDI/SYNC/ROUTE，SYSTEM 页只留 INFO 版本行）；1 文档化（DRUM PREVIEW） |
| ○ 缺失 | **9** | DI-44/46/47/48/50/51/52/53/54 |
| 非目标（硬件/联动/阶段） | **8** | spec §2 |
| 合计 | **64** | Task 1 建立时为 ✅23 / ◐7 / ○26 / 非目标 8；其后评审复查把 6 行误标 ✅ 改回缺口并拆分/改写了若干行（§6.8），Task 4–7 再把这些缺口逐项交付并给出证据（行的增删拆并见提交 `04d0d6b`、`c6fc2a4`、`16d2c2b` 至 `eacb6d9`） |

Task 4–7 交付并打勾的缺口（不逐一计数——若干行在复查时被拆并）：§2.1 的 ENV/ENV DEST/
LFO×3/SEL×2/ARP×2/VOICE×2/PATTERN/FILTER、SLICER/INSERT×2、其余 9 引擎的 EDIT、鼓轨 kit；
§2.2 的 REV/CHO+MASTER、DRUMS、TEXTURE、TOOLS、工厂预置选择 + 换引擎；§2.3 的 REC 配置、
参数锁/fill；§2.5 的 ABOUT 版本行。既有 ✅ 行里由 Task 7 补上**设备**证据的：GLO mute/solo、
SEL 选轨、EDIT 层擦除（`tracks.sh` 84 / `step_edit.sh` 314）。

## 2. 清单

列：上游特性 | 上游入口 | Tab5 落点 | 状态 | 证据 | 处置。

### 2.1 参数编辑 — 每轨族（`PAGES[]` SC_TRACK）

> P4-F Task 4（2026-11-24）打勾：ENV/ENV DEST/LFO×3/SEL×2/ARP×2/VOICE×2/PATTERN/FILTER 八行
> 由 `params.sh` ⑩ 给出机器证据（每 chip 逐槽 label == `ENG DESC live`、首槽写后回读、鼓轨
> 逐槽规则、FILTER 条件 chip）；槽表本身由 `scripts/knob-test.sh` 对照本节 §3 钉住。
>
> P4-F Task 5（2026-11-24）打勾：SOUND 页（12 引擎的 edit[0..7]、页眉 ENGINE/PRESET、鼓轨
> KIT 行）由 `params.sh` 的 SOUND 段给出机器证据（12 引擎轮换 8 槽 label == `ENG DESC live`、
> 页眉 ± 经 TAP 的回读、KIT 行写 `P_E0`）；GLOBAL/FX 的 chip（REV/DRUMS/TEXTURE、SLICER/
> INSERT×2）由 `params.sh` ⑩ 给出证据（逐槽 label == 描述符、首槽写后回读、鼓轨逐槽）。
>
> P4-F Task 7（2026-11-25）在终版固件 `e481469` 上复跑 `params.sh`：**713 PASS / 0 FAIL**——
> 上述全部断言仍在（12 引擎轮换、五张 chip 页逐槽、鼓轨逐槽、FILTER 条件 chip、键盘
> ←/→/↑/↓ 与 ENTER、`lvgl_stack_free ≥2,048`）；证据 `docs/dev/verify/4-pf-2026-11-24/
> task-7-params-{stdout,transcript}.log`。

| 上游特性 | 上游入口 | Tab5 落点 | 状态 | 证据 | 处置 |
|---|---|---|---|---|---|
| ENV（ATK/DEC/SUS/REL） | GLO? 否：ENV 页 | MOD·ENV | ✅ | `params.sh` ⑩（`knob-test.sh` 槽表） | P4-F Task 4 |
| ENV DEST（ED_FLT/ED_PIT/ED_SHP/ESYNC） | ENV DEST 页 | MOD·ENV DEST | ✅ | `params.sh` ⑩ | P4-F Task 4 |
| LFO / LFO 2 / LFO DEST | 三页 | MOD·LFO / LFO 2 / LFO DEST | ✅ | `params.sh` ⑩ | P4-F Task 4 |
| FX（DIST/CHOR/DLY/REV）+ 轨道滤波 TFLT | FX / FILTER 页 | FX 页的 `FX` chip（8 槽，另加 LVL/PAN/ATK） | ✅ | `params.sh` | 保留 FX 页的扩展 8 槽（兼容）；P4-F Task 5 起是 chip 页 |
| SLICER（SLCR/SLPAT/SLRATE/SLDEPTH） | SLICER 页 | FX·SLICER | ✅ | `params.sh` ⑩（`knob-test.sh` 槽表） | P4-F Task 5 |
| INSERT / INSERT 2（ITYPE/IA/IB/IC；IMIX） | 两页 | FX·INSERT / INSERT 2 | ✅ | `params.sh` ⑩（TYPE=5 ⇒ BITS/RATE/LPF） | P4-F Task 5（标签经 Task 3 的 live 描述符） |
| SEL / SEL 2（ROOT/SCALE/QUANT/CHORD；TRANS/STRUM/VLEAD） | SEL 页 | SCALE·SEL / SEL 2 | ✅ | `params.sh` ⑩ | P4-F Task 4 |
| ARP / ARP 2（8 参数） | ARP 页 | SCALE·ARP / ARP 2 | ✅ | `params.sh` ⑩ | P4-F Task 4 |
| EDIT 1/2 — ANALOG / FM6 / SAMPLE 引擎 | EDIT 页 | SOUND / SAMPLE 页（FM6 的 bank 行在 SOUND 底部） | ✅ | `params.sh`、`synth.sh`、`store.sh` ⓰ | P4-F Task 5：ANALOG+FM6 两页并为 SOUND |
| EDIT 1/2 — 其余 9 引擎（DIGITAL/DRAWBAR/FORMANT/GRAIN/LOFI/NOISE/PHASE/PHYS/SLICE/TRIO） | EDIT 页 | SOUND 页（标签经引擎 `desc` 钩子） | ✅ | `params.sh`（12 引擎轮换 8 槽 label == `ENG DESC live`） | P4-F Task 5（SLICE 引擎不在本固件的 12 个里，见 core.h `FELUCCA_SLICE`） |
| FILTER TYPE（ANALOG 专属；FTYPE+E4/E5/E7） | EDIT>FILTER 页 | VOICE·FILTER（条件 chip） | ✅ | `params.sh` ⑩（DIGITAL 隐藏/ANALOG 出现） | P4-F Task 4 |
| VOICE / VOICE 2（VOICE/GLIDE/GLMODE/PRIO；ALLOC/DETUNE/PAN/MUTE） | 两页 | VOICE·VOICE / VOICE 2 | ✅ | `params.sh` ⑩ | P4-F Task 4 |
| PATTERN（SLEN/SDIV/SSWING/SGATE） | PATTERN 页 | VOICE·PATTERN | ✅ | `params.sh` ⑩ | P4-F Task 4（TRANSPORT 的 LEN 工具保留） |
| 鼓轨 kit 选择（`P_E0`，上游 DRUMS 屏 KNOB 1） | DRUMS 屏 | SOUND 页在鼓轨上的 KIT 行（DSYN kit 编辑不变） | ✅ | `params.sh`（KIT ± 写 `P_E0` 回读） | P4-F Task 5 |

### 2.2 参数编辑 — 全局族（SC_GLOBAL）

| 上游特性 | 上游入口 | Tab5 落点 | 状态 | 证据 | 处置 |
|---|---|---|---|---|---|
| GLOBAL（BPM/SWING/CLOCK/TUNE）+ DLY | GLOBAL/DLY 页 | GLOBAL 页 8 槽 | ✅ | `params.sh`、`transport.sh` | — |
| REV/CHO（RSIZE/RDAMP/CRATE/CDEPTH）+ MASTER（DUST/DUCK/FILT/ROLL） | REV/CHO、MASTER 页 | GLOBAL·REV | ✅ | `params.sh` ⑩ | P4-F Task 5 |
| DRUMS（DRCH/DRLVL/DRREV/DRDLY） | DRUMS 页 | GLOBAL·DRUMS | ✅ | `params.sh` ⑩（鼓轨上 DRCH 可编辑） | P4-F Task 5（DRCH 属 MIDI 通道，联动阶段生效） |
| TEXTURE / TEXTURE 2（7 参数） | 两页 | GLOBAL·TEXTURE | ✅ | `params.sh` ⑩ | P4-F Task 5 |
| TOOLS（CLEAR SEQ / INIT SOUND / NEW） | TOOLS 页 | MIX·TOOLS 页（三张 chip，两段式确认 1.5s） | ✅ | `params.sh` ⑪ TOOLS 段；`seq-test.sh` `test_tools_*` | P4-F Task 6（语义见 §4；ADR-008 §8.11 / ADR-010 §11） |
| SYSTEM（MIDI/SYNC/ROUTE/INFO） | SYSTEM 页 | INFO = SYSTEM 页版本行 | ◐ | `sys.sh` | MIDI/SYNC/ROUTE 非目标（阶段 5） |
| MIDI / MIDI 2 / MIDI 3 | 三页 | — | 非目标 | — | 阶段 5（DI-1） |
| 用户预置 32 槽存取（USER 页） | USER 页 | PRESETS 页（SAVE/LOAD/ERASE） | ✅ | `store.sh` | — |
| **工厂预置选择 + 换引擎**（PRESETS 页 KNOB 1/2） | PRESETS 页 | SOUND 页眉 `ENGINE − 名 +` / `PRESET − 名 +`（`CMD_SET_ENGINE`/`CMD_SET_PRESET`） | ✅ | `params.sh`（TAP ± 的回读与名字、`ENG SNAP`）、`state-test.sh` | P4-F Task 5（按名浏览全部预置见 DI-54） |
| 按名浏览全部工厂预置（列表） | PRESETS 页 | — | ○ | — | DI-54 |
| 收藏（fav，PRESETS KNOB 4，16 个） | PRESETS 页 | — | ○ | — | DI-44 |
| SECTION / PROJECT（A–D） | SECTION 页 | PROJECTS 页 + SONG 页 | ✅ | `store.sh`、`song.sh` | — |

### 2.3 播放 / 音序 / 录音 / 编排

> P4-F Task 7（2026-11-25）定稿：键盘层键的设备证据——GLO mute/solo、SEL 选轨（本节两行）与
> EDIT 层擦除（`step_edit.sh` ②g）。同任务把导航定稿在 `nav.sh`：18 页 TAB 轮转、九组 104px
> 组签逐组像素、多页组 `UI SUB` 范围（EDIT 4 / MIX 3 / SYNTH 3 / SEQ 2 / FILE 2）、组作用域
> 销毁、有序对全矩阵（9×8 组对逐目标页，构建日志 `池剩余 ≥ 4,096`）。

| 上游特性 | 上游入口 | Tab5 落点 | 状态 | 证据 | 处置 |
|---|---|---|---|---|---|
| PLAY/STOP；REC arm/record/free take | PLAY/REC、REC 屏 | TRANSPORT 页 | ✅ | `transport.sh`、`sequence.sh` | — |
| REC 配置：QUANT / MODE(FREE·TEMPO) / START(NOTE·COUNT)（count-in） | REC 屏 | TRANSPORT 页录音/fill 行右侧的 QNT / MODE / START 三格（每格翻转一项、整份 `CMD_REC_CONFIG`；单对象自绘） | ✅ | `params.sh` ⑪ REC 配置行；`state-test.sh`；`sys.sh`（持久化） | P4-F Task 6：TRANSPORT 的 QNT/MODE/START 三格（走已有 `CMD_REC_CONFIG`） |
| hold REC 清选中轨（~2s 环） | REC 长按 | TRANSPORT `CLEAR T<n>`（即时，无长按） | ✅ | `transport.sh` | 口径差异：Tab5 即时清，文档化 |
| SEQ steps 1–64 分四页；步 LEVEL/RATCHET/NUDGE/TIE/SLIDE | SEQ 层 | STEP 页 | ✅ | `step_edit.sh`、`sequence.sh` | — |
| EDIT 层旋钮 SHIFT(rotate)/LENGTH ×2÷/TRANSPOSE/UNDO/REDO | EDIT 层 | TRANSPORT 工具区 | ✅ | `transport.sh` | undo 粒度：每次调用一次（上游一次 hold 算一次） |
| EDIT 层 **erase**（按住 EDIT + 键，走带中 MPC 式擦除该音符/鼓声） | EDIT 层 | 键盘 EDIT 层 a–g 擦除**聚焦步**里的同名音（`input.c:166-169`）；非走带中逐拍 | ◐ | `step_edit.sh` ②g（P4-F Task 7，`KEY` 注入）：Aa+c/e/d 擦除聚焦步的同名音——单音步清空、两音步（NOTE+CONT）只摘一根、跨八度（C3 也被 Aa+c 擦）、无匹配音级不变、不按 Aa 时 c 是演奏键（聚焦 C4 步按住即发声 `voices≥1`、步表不变）、鼓轨无擦除语义（位图不变）、诊断冻结时同一个会擦的键（Aa+f 对准 F4 步）不写引擎（解冻后同键会擦——RED 已验证）；真值 `ENG STEP`；本段包在 `STORE PROJ SAVE 3`/`LOAD 3` 里，工作工程逐位还原 | DI-45（live 变体） |
| 参数锁（步 held + PRESETS 旋钮锁最近编辑的参数）/ fill 条件（步 held + OCT+） | SEQ 层 | 键盘 EDIT 层 **Aa+k 锁 / Aa+l 解锁 / Aa+9（或 `;`）fill 条件**，只在 STEP 页，作用于焦点轨 × 聚焦步（`CMD_LOCK_FOCUS` / `CMD_FILL_STEP`）；锁的对象 = 焦点轨上**最后一次能生效的参数写**（`last_param`，参数页旋钮/±/console `PARAM` 都算）；范围 = 聚焦步在本轨 LEN 之内（R12，空步与 TIE 步也可） | ✅ | `params.sh` ⑪ STEP 键盘锁段（Aa+k/l/9 的 `SEQ LOCK LIST`/`ENG STEP` 真值、`NOT LOCKABLE`、LEN 之外的 `NO STEP`、冻结下 `FROZEN` 且锁保持；Task 7 终版固件 713/0）；`state-test.sh`（45 例）；`seq-test.sh`（`test_lock_del_canonical`）；`sequence.sh`（console） | P4-F Task 6 + 评审修复轮（ADR-006「STEP 页步工具」、ADR-010 §11；零新增 LVGL 对象） |
| 鼓轨 MUTE mode（EDIT + 黑键：静音 Sequencer 里的某个鼓声） | EDIT 层 | — | ○ | — | DI-46 |
| ARP 层 **roll**（按住 ARP + 键 = note repeat，RATE 1/8…1/64） | ARP 层 | — | ○ | — | DI-47 |
| GLO 层 mute/solo 键；fill 保持 / fill next bar | GLO 层 | 键盘 GLO 层 1–8 键（`input.c:159-162`）；TRACKS 卡只显示状态；TRANSPORT FILL | ✅ | `state-test.sh`（mute/solo 命令路径，host）、`transport.sh`（fill）、`tracks.sh` 的 GLO 层 `KEY` 注入（P4-F Task 7：Alt+1..4 逐轨 mute 翻转、Alt+5..8 solo 翻转，`ENG SNAP trk` 真值 + TRACKS 卡 MUTE/SOLO 像素区；Alt+Ctrl 同按同键取 GLO；不按层键时数字键不是混音键；收口还原） | — |
| GLO 层 track levels ×4 | GLO 层 KNOB | FX 页 LEVEL 槽（逐轨） | ✅ | `params.sh`（LEVEL 写读） | — |
| GLO 层 **tap tempo** | GLO 层白键 16 | 仅 BPM± | ○ | — | DI-48 |
| SEL 层 参数（KEY/CHORD/SCALE/KEYS/TRANSPOSE） | SEL 层 | 参数 = SCALE·SEL / SEL 2 chip（ROOT/SCALE/QUANT/CHORD、TRANS/STRUM/VLEAD）；键盘「按音阶走」（KEYS SNAP/WHITE）无 | ◐ | `params.sh` ⑩（SCALE 页 SEL/SEL 2 逐槽） | 参数已由 P4-F Task 4 交付；键盘行为见 DI-49 |
| SAVE 层：section 播放 / chain / loop / song / SONG REC | SAVE 层 | SONG 页 | ✅ | `song.sh`、`sequence.sh` | — |
| 选轨（ALGORITHM） | 面板 | TRACKS 卡 / 键盘 SEL 层 1–4 | ✅ | `tracks.sh`（卡片 TAP）；SEL 层 `KEY` 注入（P4-F Task 7：Ctrl+1..4 ⇒ `STATE SNAP selected_track`/`focus_track`、重复按幂等、不改混音、松开 Ctrl 后数字键不选轨、卡 3 选中像素） | — |
| 八度（OCT−/+，合成轨） | 面板 | **无**：Tab5 键盘音高固定，无八度移位 | ○ | — | DI-53 |
| 鼓轨 16 lane 网格、ghost/hard（力度） | DRUMS 屏 | STEP 鼓格 / 键盘 lane | ✅ | `step_edit.sh`、`sequence.sh` | — |
| SONG 模式 / loop 恢复 | SONG 屏 | SONG 页 | ✅ | `song.sh`、`sequence.sh` | warm reset 见 DI-33 |
| 开机画面 / 开机音 | — | 无 | ○ | — | DI-50 |
| Rescue / recovery | recovery.c | — | 非目标 | — | FM-1 固件救援 |

### 2.4 声音设计

| 上游特性 | 上游入口 | Tab5 落点 | 状态 | 证据 | 处置 |
|---|---|---|---|---|---|
| FM6 深度 patch（6×21+29）+ DX7 bank 27 槽 | FM6 编辑屏 | FM6P 页 + bank 行 | ✅ | `synth.sh` | live patch 掉电 DI-36 |
| DSYN kit（4 kit × 16 lane × 22B）+ 工厂 kit | DRUMS/STUDIO 屏 | DSYN 页 | ✅ | `synth.sh` | 命名 DI-37、延迟写 DI-38 |
| 用户采样 USR1–4 | 采样上传 | SAMPLE 页 + `sample_upload.py` | ✅ | `synth.sh` | 导入 UI DI-34 |

### 2.5 设置 / 菜单（HOME 长按菜单）

| 上游菜单项 | Tab5 落点 | 状态 | 证据 | 处置 |
|---|---|---|---|---|
| COLOR（palette + theme）/ ZOOM | SYSTEM 页 COLOR/ZOOM + 运行时色表 | ✅ | `sys.sh` | — |
| KNOB ACCEL | Tab5 旋钮 = 环带粗/细双区（ADR-008） | 非目标 | `knob-test.sh` | 等效交互不同，文档化 |
| LIGHTS（背光档）/ KEYS / NOTES | LIGHTS → 键盘 RGB；KEYS/NOTES 非每键灯 | ✅ / 非目标 | `sys.sh` | 键盘颜色字节序人测（DI-43） |
| SPEAKER LOWCUT / FILTER RESO | SYSTEM 页 LOWCUT / RESO | ✅ | `sys.sh`、182/182（D7） | — |
| USB AUDIO / USB SERIAL / USB RATE / 2ND CORE | — | 非目标 | — | 平台差异 |
| SEQ > STEP ADVANCE | — | 非目标 | — | Tab5 无「松键后光标」模型 |
| SEQ > DRUM PREVIEW | DSYN AUDITION（显式试听） | ◐ | `synth.sh` | 显式试听等效，文档化 |
| HARDWARE CALIBRATION / MIDI LEARN CLEAR | — | 非目标 | — | FM-1 硬件 / 阶段 5 |
| ABOUT | SYSTEM 页版本行 | ✅ | `sys.sh` ②（P4-F Task 7）：版本行两行暗色文字的像素探针（`APP <version>` / `SLOOP 2.8.5 (bac9c05)`）、文字不越出 label 盒；① 断言启动日志 `App version` 为 git 短哈希并与主机 HEAD 注记（transcript 含启动日志原始回显） | — |
| （非上游项）屏幕亮度 / 输出音量 | — | SYSTEM 页 BRIGHT / VOLUME | ✅ Tab5 原生 | `sys.sh` | 听感人测 |
| 输出路由 | OUTPUT 行 | ◐ | `sys.sh`（`N/A`） | DI-39（硬件） |
| VIS 可视化（12 风格） | — | ○ | — | DI-51 |

### 2.6 现场层与联动

| 上游特性 | 上游入口 | Tab5 落点 | 状态 | 处置 |
|---|---|---|---|---|
| **Punch-in effects**（按住 FX + 键 = 滤波/dust/duck 等；KNOB：FILTER/DUST/DUCK/TRK FILT） | FX 层 | 引擎 `punch_process` 在编，UI 无 | ○ | DI-52 |
| LIVE 页 / 编辑器协议 / web editor | — | — | 非目标 | 阶段 5 |
| MIDI 键盘 / MIDI LEARN / SYNC / USB audio | — | — | 非目标 | 阶段 5 / 平台差异 |

## 3. 族页槽位权威表（Task 3–6 的唯一来源；id 为上游 `core.h` 枚举名）

鼓轨规则（`params.c:447` `page_for_drum`）：全局页除 PRESETS/USER 全可编辑（但 TOOLS 的 `G_INITSND` 对鼓轨排除，`params.c:466-468`）；TRACK 页仅 FAM_SEQ（PATTERN）、GR_SLCR（SLICER）、GR_INS（INSERT×2）、`id[0]==P_TFLT`；TRACK 页里的 LEVEL/PAN 属 SC_TRK（TRACKS 页「TRACK LEVEL LEN PAN」）故鼓轨可编辑；ENGINE 页不可。**Tab5 落实**：鼓轨上 SOUND 旋钮区锁定、改显示 kit 选择行；其余页**逐槽**判定——该参数出现在任一鼓轨可编辑的上游页即可编辑（下表「鼓」列）。

| 页 | chip | 槽（顺序即上游页序） | 鼓轨 | 备注 |
|---|---|---|---|---|
| SOUND | — | 页眉 `ENGINE −/+`、`PRESET −/+`（选中轨换引擎/预置）；主体 `ENGINES[sel_engine]->edit[0..7]`（TRACK scope `P_E0..P_E7`，经 `sloop_core_desc_track_live` 取 `e->desc` 钩子的标签） | 旋钮区锁；显示 `KIT −/+` 行（写 `P_E0`） | FM6 引擎时底部 bank 行（A10） |
| MOD | ENV | P_ATK P_DEC P_SUS P_REL | 锁 | |
| MOD | ENV DEST | P_ED_FLT P_ED_PIT P_ED_SHP P_ESYNC | 锁 | ESYNC 开/关时 ATK/DEC/REL 的标签经 `sloop_core_desc_track_live` 直接取上游 `ESYNC_DESC`（Task 3；不建镜像表，裁决 R6） |
| MOD | LFO | P_LRATE P_LWAVE P_LPHASE P_LFADE | 锁 | |
| MOD | LFO 2 | P_LSYNC P_LTRIG P_LPOL | 锁 | 3 槽 |
| MOD | LFO DEST | P_LD_PIT P_LD_FLT P_LD_SHP P_LD_AMP | 锁 | |
| SCALE | SEL | P_ROOT P_SCALE P_QUANT P_CHORD | 锁 | |
| SCALE | SEL 2 | P_TRANS P_STRUM P_VLEAD | 锁 | 3 槽 |
| SCALE | ARP | P_AMODE P_ARATE P_AOCT P_AGATE | 锁 | |
| SCALE | ARP 2 | P_ASWING P_APROB P_AHOLD P_AORDER | 锁 | |
| VOICE | VOICE | P_VOICE P_GLIDE P_GLMODE P_PRIO | 锁 | |
| VOICE | VOICE 2 | P_ALLOC P_DETUNE P_PAN P_MUTE | 仅 PAN 可编辑，其余锁 | 上游 TRACKS 页旋钮 = G_SWING / LEVEL(鼓轨为 `G_DRLVL`) / P_SLEN / P_PAN（`ui_input.c:291-316`），无 MUTE；鼓轨 mute 走 GLO 层键 |
| VOICE | PATTERN | P_SLEN P_SDIV P_SSWING P_SGATE | 可编辑 | |
| VOICE | FILTER | P_FTYPE P_E4 P_E5 P_E7 | 锁 | 仅引擎 = ANALOG 且非鼓轨时出现（`page_shown`） |
| FX | FX | P_DIST P_CHOR P_DLY P_REV P_TFLT P_LEVEL P_PAN P_ATK | TFLT/LEVEL/PAN 可编辑，其余锁 | 沿用现 FX 页 8 槽 |
| FX | SLICER | P_SLCR P_SLPAT P_SLRATE P_SLDEPTH | 可编辑 | |
| FX | INSERT | P_ITYPE P_IA P_IB P_IC | 可编辑 | 标签随 ITYPE（Task 3） |
| FX | INSERT 2 | P_IMIX | 可编辑 | 1 槽 |
| GLOBAL | GLO | G_BPM G_SWING G_CLOCK G_TUNE G_DTIME G_DFDBK G_DCOLOR G_DMIX | 可编辑 | = 现 GLOBAL 页 |
| GLOBAL | REV | G_RSIZE G_RDAMP G_CRATE G_CDEPTH G_DUST G_DUCK G_FILT G_ROLL | 可编辑 | REV/CHO + MASTER |
| GLOBAL | DRUMS | G_DRCH G_DRLVL G_DRREV G_DRDLY | 可编辑 | DRCH 为 MIDI 通道，联动阶段生效 |
| GLOBAL | TEXTURE | G_TXSRC G_TXLVL G_TXTONE G_TXMOVE G_TXRATE G_TXWHEN G_TXDUCK | 可编辑 | 7 槽 |

## 4. TOOLS 语义（Task 6；`ui_input.c:568–612`）

| 动作 | 上游行为 | Tab5 要求 |
|---|---|---|
| CLEAR SEQ（`G_CLRSEQ`） | `undo_mark(TSEL, (undo_sess+=4)\|3)` → `track_defaults_steps(TSEL)`，提示 `PATTERN CLEARED`（**只清选中轨**的步表，可 undo） | `sloop_core_tools(0)`：同序（先 undo_mark 再清步表，引擎锁内）；效果经 TRANSPORT UNDO 可撤销 |
| INIT SOUND（`G_INITSND`） | `set_engine(TSEL->eng_req)`（引擎默认 + 预置 0），提示 `SOUND INIT` | `sloop_core_tools(1)`：等价 `sloop_core_set_engine(选中轨, 当前引擎)`；鼓轨上游不可用（`page_desc` 排除）⇒ 拒绝 |
| NEW（`G_NEWPRJ`） | 唯一守卫：SONG 播放中且 arrangement_enabled ⇒ `STOP THE SONG FIRST`；否则 `project_new()`（`ui_input.c:462-490`，**static、不在 sloop_core TU**）：停走带、panic、每轨 undo_mark、全部轨参数回默认并套 TRK_DEF 默认声音、鼓 kit/tune/decay/mute 复位、除 MIDI 类外全部全局回默认（BPM/DLY/REV/TEXTURE 都不留）、清 solo/octave；A–D 段与 settings 对象保留，提示 `NEW LOOP, A-D KEPT` | `sloop_core_tools(2)`：同一条守卫；`project_new` **逐行镜像**进适配层（引用 `ui_input.c:462-490`，同 `SONG_PANEL_DEF` 先例） |
| 预备 | 第一次拧到 = `ui.arm = id`（~1.5s，`ui.arm_t = 90`），提示 `AGAIN: <label>`；窗口内第二次才执行 | 页面：第一次点 chip = 预备（1.5s 窗口），再点才执行；超时取消 |

## 5. 缺口处置（○/◐ 汇总）

| 处置 | 项 |
|---|---|
| P4-F Task 4（已落地，✅） | ENV/ENV DEST/LFO×3、SEL×2、ARP×2、VOICE×2、PATTERN、FILTER(ANALOG) 的参数页（MOD/SCALE/VOICE chip 页） |
| P4-F Task 5（已落地，✅） | SOUND（12 引擎 + 页眉 ENGINE/PRESET 选择）、鼓轨 kit 选择行、SLICER/INSERT×2、REV/MASTER、DRUMS、TEXTURE |
| P4-F Task 6（已落地，✅） | TOOLS 三动作；TRANSPORT REC 配置 chip（QUANT/MODE/START）；键盘参数锁 lock/unlock 与 fill 条件 |
| P4-F Task 7（证据） | 既有 ✅ 行的设备证据补齐：GLO mute/solo、SEL 选轨、EDIT 层擦除、ABOUT 版本行；导航与有序对全矩阵 |
| 非目标（阶段 5） | SYSTEM 页的 MIDI/SYNC/ROUTE（清单 §2.2） |
| 文档化（显式试听等效） | SEQ > DRUM PREVIEW |
| DI-44 | PRESETS 收藏（fav 16） |
| DI-53 | 八度移位（OCT−/+） |
| DI-54 | 按名浏览全部工厂预置的列表 |
| DI-45 | EDIT 层实时擦除（走带中 MPC 式） |
| DI-46 | 鼓轨 MUTE mode（`drum_mute` 位掩码，`ui_layers.c:787`） |
| DI-47 | ARP roll（note repeat 层） |
| DI-48 | tap tempo |
| DI-49 | SEL 层键盘行为（CHORD/KEYS SNAP/WHITE 在 Tab5 键盘上的走音阶） |
| DI-50 | 开机画面 / 开机音 |
| DI-51 | VIS 可视化 |
| DI-52 | Punch-in effects 层 |
| 既有 DI | DI-33（warm reset）、DI-34（采样导入）、DI-36、DI-37、DI-38、DI-39（路由）、DI-43 |

## 6. 审计裁决（对 spec 的修订/确认）

1. **确认**：spec §3/§4 的六张族页与分组与上游页表一致；页数 18（spec 已更正）。
2. **PATTERN 页是 4 槽**（含 `P_SLEN`），不是 builder check 假设的 3 槽；VOICE 页 PATTERN chip 用 4 槽。
3. **FILTER chip 仅 ANALOG 出现**（`page_shown`，`params.c:460`）：VOICE 页在非 ANALOG 引擎下只有 3 个 chip——实现须支持「条件 chip」。
4. **鼓轨逐槽规则**以本表为准（FX chip 内混有可编辑与锁定槽，必须逐槽而非逐 chip 判定）——与 spec A8 的「逐 chip」措辞相比更细，以本表为准。
5. **FX 页的扩展 8 槽保留**（含 TFLT/LEVEL/PAN/ATK）：与上游 FX 页不同但已机读验证，兼容优先。
6. **hold REC 清轨**：Tab5 为即时 `CLEAR T<n>`（无 2s 环），记为口径差异而非缺口。
7. ~~不修订 spec 的容量/导航数字；est 上限不变。~~ **已被实施结果取代**（Task 10a 按裁决改写 spec）：
   chip 页 est 定为 **3,584**（R7；spec 的 5,120 是上限，chip 行改单对象自绘后实测
   2,936–3,104B）、SOUND 6,144、MIX Σ **10,752**、EDIT Σ **16,896**；spec §3 表已按此修订。
   est 上限的性质不变——实测超限时先改对象设计，不抬 est（ADR-008 §8.12）。
8. **审计复查修订（评审）**：工厂预置选择/换引擎、REC 配置、鼓轨 kit 选择、参数锁与 fill 条件、八度此前被误标 ✅——它们在 Tab5 上只有 console 入口，违反 A2。处置：前三项进 Task 5/6，参数锁/fill 进 Task 6（键盘层，零新增 LVGL 对象，TRANSPORT est 不变），八度 DI-53、预置列表浏览 DI-54。
9. **ENTER = 焦点槽 +1**（裁决 R8）：全部参数页（SOUND/MOD/SCALE/VOICE/GLOBAL/FX）生效，
   与 DSYN/FM6P 页一致；此前参数页的 ENTER 无动作。锁定槽无动作、不发命令。
10. **FX 页在鼓轨上逐槽锁定**（裁决 R8）：以本表 §3 为准（TFLT/LEVEL/PAN 可编辑，其余锁）。
   Task 4 之前 Tab5 的 FX 页在鼓轨上整页可编辑，是对上游 `page_for_drum` 的偏离，已纠正；
   console `PARAM` 作为诊断面不受页面锁定约束。
11. **锁 / fill 条件的作用范围 = 聚焦步在本轨 LEN 之内**（裁决 R12）：不要求该步有音，空步与
   只有 TIE 的步同样可锁（上游 `ui_layers.c` 的 held-step 循环 `idx < trk_len`）；LEN 之外回
   `NO STEP`。Task 6 首版自定的「必须有音」已撤销。
12. **「✅」的证据时点**：§2 各行的设备证据以 Task 7 同会话（终版固件 `e481469`：
   `params` 713 / `nav` 583 / `tracks` 84 / `step_edit` 314 / `sys` 354，均 0 FAIL）为准；
   Task 2–6 的首轮读数见 `docs/dev/verify/4-pf-2026-11-24/task-N-*`。阶段收口的完整设备矩阵
   （十场景 + 182 例 + selftest-heap + 截屏矩阵 + 长时 soak）由 Task 10b 在同一固件上重跑。
