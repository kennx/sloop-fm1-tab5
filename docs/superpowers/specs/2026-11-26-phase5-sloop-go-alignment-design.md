# 阶段 5 设计：对齐 Sloop Go（界面重构 + 联动模式）

> 状态：**已接受**（2026-11-26，用户指示按推荐方案执行）｜ 日期：2026-11-26
> 实施者复查（builder check）：已完成，8 条意见的处置见 §9 后半张表；没有产生需要用户先回答的新问题。
> 关联：`docs/design/002-port-roadmap.md` §7（阶段 5）、ADR-005（UI 架构）、ADR-008（参数页/池预算）、
> `docs/design/012-feature-parity-checklist.md`（阶段 4 对齐清单）、`docs/deferred-issues.md` DI-1、
> `docs/research/sloop-fm1-go.md`、`docs/research/editor-protocol.md`
> 上游基线复核（2026-11-26）：sloop-fm1 HEAD = `bac9c05`（v2.8.5，与阶段 4 基线一致，未变）；
> sloop-fm1-go HEAD = `ed941b5`（1.8.0，未变）。

## 1. 意图与缘起

用户的两条输入：

1. **界面设计以 Sloop Go 为参考**（`jahlib/sloop-fm1-go` 的 `screenshots/`，0–11.png）。「目前单轨编辑这些似乎都没有。」
2. **FM-1 真机连不上。**

第 2 条不是故障，是**尚未实现**（§2.1）。第 1 条点中了一个结构性差距：阶段 4 做到了「上游每个特性在 Tab5
上有 UI 入口」（清单 012：✅42），但界面是 **FM-1 旋钮模型的直译**——9 个组签、18 个页面、每页 1–8 个旋钮槽，
选中轨的参数分散在 4 个页面的 15 个 chip 里。Sloop Go 的「单轨编辑」是一张**仪表盘**：选中轨的全部参数组
（Global / Master / Envelope / LFO / Engine edit / Voice / FX / Scale / Arp / Pattern / Drums …）以双列卡片
排在同一张可滚动的页面上，每行最多 4 个旋钮。

两件事必须一起规划，原因是 AGENTS §1 的产品定义：**独立模式与联动模式共享同一 UI 与状态模型**。Sloop Go 的
界面本来就是「对着一台 FM-1 工作的编辑器」；新界面只写一份，执行端在本地引擎（sloop_core）与远程 FM-1 之间
切换。先定界面、再接联动，界面就不必返工。

**成功的样子**：

- Tab5 开机后像 Sloop Go：任何页面左上角固定「播放/停止 + 菜单」，每页一条固定页眉，右侧是链路/队列状态。
- Sound 页一屏滚动看尽选中轨的全部参数组；Mixer 是四条带推子的通道条；Sequencer 是 16 lane 鼓格 + 钢琴卷帘。
- 刷 SLOOP 的 FM-1 插入 USB-A → Tab5 提示进入联动模式 → **同一套界面**直驱 FM-1；在 FM-1 上拧旋钮，
  Tab5 上的对应控件实时跟着变。

## 2. 现状核对（证据）

### 2.1 连接：固件里没有这条通路

| 核对项 | 结果 |
|---|---|
| 固件源码（`firmware/main`、`firmware/components`）里有没有 USB host / USB-MIDI / Felucca / 0x1209 的连接代码 | **没有**。`usb_host|usbh_|tinyusb|midi|Felucca|0x1209|VBUS|USB5V` 只命中无关项：`midi` 是引擎的 MIDI 通道参数名，`VBUS` 是电源采样的电池侧节点 |
| `firmware/sdkconfig` | 没有 `CONFIG_USB_HOST_ENABLED`；`CONFIG_USB_*` 只有 IDF 默认项。控制台走 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`（USB-C） |
| USB-A 口供电 | PI4IO 0x44 E2.P3 `USB5V_EN`（`docs/tab5-hardware.md` §3.11）从未被驱动 → 插上设备也**没有电** |
| 文档记录 | `docs/dev/device-notes.md` §3「M-VAVE FM-1（刷 SLOOP）— ❌ 未连接」；`docs/deferred-issues.md` DI-1「USB-MIDI host 类驱动缺失」 |
| 路线图 | 阶段 5（P5-0…P5-5）状态 ⬜，未开工 |

结论：「插上 FM-1 没有任何反应」是当前固件的预期行为。要做的事就是本文的 5A/5C/5E。

### 2.2 界面：Sloop Go 的 11 个页面 vs. Tab5 现状

Sloop Go 页面构成取自上游 Compose 源码（`android/app/src/main/kotlin/com/sloop/go/ui/`）与
`android/README.md`「Current scope」；截图做了像素/亮度结构核对（本会话的模型**读不了图像**，见 §4 的诚实边界）。

| Sloop Go 页（截图） | 上游构成 | Tab5 现状（9 组 18 页） | 差距 | 归入 |
|---|---|---|---|---|
| 导航壳（每页） | 左上固定「▶/■ + ⋮」；⋮ = 下拉页菜单（11 页 + undo/redo）；每页一条固定页眉：左侧页面控件横向排布，右侧链路状态/队列/错误 | 顶部两行页签：9 个组签（104px）+ 组内子签（176px），占 72px；右侧 `SLOOP` 标题与层键提示 | 信息架构不同：页签条平铺 18 页，没有「每页页眉」这一层 | 5B |
| **Sound**（1.png） | 页眉：轨选择、预置名下拉、音色选择、4 个 LFO 示波按钮；主体：**双列卡片瀑布流**，13 个参数组（Global/Master/Envelope/LFO/Engine edit/Voice/FX/Scale/Arp/Pattern/Drums/MIDI/Texture），卡片可折叠、可长按拖动排序，页内每行 ≤4 旋钮；鼓轨追加 DRUM SOUND 卡（16 lane 的 TUNE/DECAY） | EDIT 组：SOUND（8 槽 + 页眉 ENGINE/PRESET ±）、MOD（5 chip）、SCALE（4 chip）、VOICE（4 chip）；MIX 组：GLOBAL、FX、TOOLS | 参数都有入口，但分散在 4 页 15 个 chip；**没有一屏仪表盘**；无预置名列表选择器（DI-54）；无收藏（DI-44）；鼓轨逐 lane TUNE/DECAY 无入口（待 5B Task 0 核对）；`G_FXFADE`（SLOOP 2.8 的 50 号全局）是否在页待核对 | **5B** |
| Mixer（9.png） | 4 条竖向通道条：序号/引擎名、电平数、竖向推子、Mute、Edit（= 选中该轨去编辑） | TRACKS 页 4 张卡（LEVEL 文本 + 电平条 + MUTE/SOLO）；电平值在 FX 页的 LEVEL 槽里改 | 没有推子；卡片不承担「进入编辑」 | **5B** |
| Sequencer（2/3.png） | 顶部控制条（预置、Now/Late、SEND、Clear、Fit、Q、TIE、LEN…）；**鼓格 16 lane × 整轨 LEN**（缩放、点按、长按拖画、lane 拖动、力度拖动、播放头）；**钢琴卷帘**（加/删音符、拖右端改时值、长按移动、框选、量化、±12 变调）；步编辑对话框（音符/力度/acc/slide/tie/nudge/fill/参数锁） | STEP 页：4 轨 × 16 步窗网格 + 鼓 lane 4×4；TRANSPORT 页工具区；参数锁/fill 走键盘层 | **没有钢琴卷帘**（多音/时值/TIE 不可视）；鼓格不是 16 lane 全宽；没有拖画/框选/缩放手势；步编辑没有对话框 | 5D |
| Live | 细工具条（播放/菜单/BPM/母带/CPU）+ 混音卡（轨 mute/solo/send、FILL、THROW）+ **27 个 punch-in 效果垫** | 无（punch-in 引擎钩子在编、UI 无：DI-52；mute/solo 走键盘 GLO 层；FILL 在 TRANSPORT） | 整页缺 | 5D |
| Song（7.png） | A–D 四张段落卡（含 pattern 缩略图）+ 编排时间条 | SONG 页（功能完整：段落/链/循环/SONG REC） | 已有，版式不同 | 5D |
| Samples（5.png） | USR1–4 槽卡 + 波形显示 + Chop 切片面板 | SAMPLE 页 + `scripts/sample_upload.py`（主机侧编码上传） | 没有波形/切片 UI（导入 UI 是 DI-34） | 5E |
| Drum synth（6.png） | SYN1–4 每 kit 16 lane × 22 参数，卡片 + 旋钮 | DSYN 页（功能完整） | 已有，版式不同 | 5E |
| FM6（4.png） | 槽库条 + 音色卡（算法链图）+ 6 张算子卡（密排旋钮）+ LFO/PEG | FM6P 页 + bank 行（功能完整） | 已有，版式不同 | 5E |
| MIDI patterns（8.png） | App 内 pattern 库：存/取/改名/删除、缩略图、长按拖入卷帘 | 无 | 整页缺 | 5E |
| Recorder（10.png） | FM-1 的 UAC1 音频 → WAV/FLAC；压限器；频谱/波形/声像/电平 | 无 | 整页缺（依赖音频采集 = 路线图 P5-5） | 5E |
| Device（11.png） | 选/连/断 MIDI 设备、固件信息、主题 | SYSTEM 页只有 INFO 版本行 + 显示/音频/颜色设置 | 没有连接管理 | **5B**（连接状态与固件信息，读 5A 的能力）→ 5E（主题） |

### 2.3 对可行性起决定作用的实测/既有数字

- **LVGL 池**：64KB 内置 TLSF（`CONFIG_LV_MEM_SIZE=65536`，可用 63,120B）。ADR-005 只评估过「扩到 96KB 会撑爆
  内部 RAM（链接期 `.bss` 溢出）」，**从未评估过把池放到 32MB PSRAM**。页面按组作用域存活，峰值 = 基础占用
  （约 9KB：阶段 3 四页 ≈43KB/68% 反推）+ 当前组 Σ est；现最大组是 SEQ（Σ est 32,256B，切页时要求池余 ≥4,096B）。
- **旋钮单价**：`sloop_ui/src/knob.h`——一个旋钮 = **1 个自绘 LVGL 对象，≈300B**（不用 `lv_arc`+`lv_label`
  的 662B 方案）。Sloop Go Sound 页在 SLOOP 2.8.5（`P_E0=63`）下的参数数：Global 4、Master 4、Envelope 9、
  LFO 11、Engine edit 9、Voice 9、FX 22、Scale 7、Arp 8、Pattern 4、Drums 4、MIDI 10、Texture 7 = **108 个**
  （上游 `ParamsScreen.kt` 的 `layout(pe0)` 逐页相加）。≈100 个控件 × 300B ≈ 30KB，加 13 张卡片框
  → **est 约 35–40KB**：与现有最大组同量级，**不动池架构即可容纳**，但它必须是单页作用域。
- **屏幕换算**：Tab5 5″ 1280×720（293ppi）≈ **698×392 dp**；Sloop Go 的目标手机横屏 2412×1080 @3x =
  **804×360 dp**。两者 dp 画布几乎相同（Tab5 宽 11% 小、高 9% 大）→ 布局可按 dp 近 1:1 搬，
  **1dp ≈ 1.83px**（5B Task 0 实测校准）。
- **字体**：现在只有 Montserrat 14/16/20/28。Sloop Go 全程 JetBrains Mono（OFL-1.1，上游仓库 `fonts/` 自带）。
  所需字号 ≈ 20/22/26/29/44px，位图化后约 200–250KB；app 分区 8MB、现用 1.77MB，余量充足。
- **内存分配策略**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`——≤16KB 的分配默认落内部 RAM；
  USB host 库任务栈按默认方式创建会落内部 RAM（官方 BSP 是 4,096B 的 `usb_lib` 任务；同板先例
  `M5MusicTab5` 连 HID 后台任务在内注明约 8KB）。
- **内部堆门线**：`scripts/soak.py` 与 `scripts/selftest-heap.py` 的 `HEAP_FLOOR` = 60KB
  （61,440B，AGENTS §5.5 内存门禁）；本机开机 `internal_min` 实测就是 **61,440B**
  （`docs/dev/device-notes.md` 的踩坑记录）——余量只有约 1KB。USB host 一启用，这条门禁**必然破线**，
  所以 5A 必须同时给出「怎么把内部 RAM 省回来」与「门线怎么重定基线」两件事的答案（§6 5A 的 Task 0 ③）。
- **Flash 分区**：`storage`（FAT，7.5MB）与 `sloopstore` 已就位（ADR-009）。

### 2.4 同板先例：`~/projects/M5MusicTab5`（本机，MIT）

`components/service_usb_host/service_usb_host.c`（584 行）+ `usb_host_midi_guide.md`：**在同一块 Tab5 上用
ESP-IDF 原生 `usb_host` 跑通了 USB-A 口的 USB-MIDI 键盘接入**——识别 Audio 类 0x01 / MIDI Streaming 子类 0x03，
claim 接口，持续提交 Bulk IN，解析 4 字节事件包。它**没有**：Bulk OUT、SysEx 重组、编辑器协议。
它是「路线可行」的证据；本项目按 IDF 公开接口自写 `usb_fm1`，把它当参考而不是复制来源。

## 3. 目标、非目标、反目标

**目标**

1. Tab5 的信息架构与每页构成对齐 Sloop Go：导航壳 + Sound / Mixer / Sequencer / Live / Song / Samples /
   Drum synth / FM6 / MIDI patterns / Recorder / Device。
2. 接入刷 SLOOP 的 FM-1：连接 → INFO 版本协商 → push 可达 → 双向同步 → LIVE 遥控 → UAC1 采集，达到
   Sloop Go 级的体验（路线图阶段 5 的完成判据不变）。
3. 新界面从第一天起就只依赖 AppState 快照 + EditCmd：独立模式执行端 = sloop_core，联动模式 = FM-1 会话。

**非目标**

- 不改上游 sloop-fm1 的 FM-1 侧行为；不做固件更新/刷机；不做 BLE MIDI（AGENTS §1）。
- 不做像素级复刻 Android Material 控件；目标是**信息架构、每页分区、控件类型与交互语义**一致，
  渲染用 LVGL 原生风格（扁平、深色、等宽字体）。
- Sloop Go 里属于「手机 App 自身」的功能默认不搬：App 侧 4 个 LFO（见 5C 的评估项）、应用内更新对话框、
  录音权限流程、SAF 文件选择器。
- 不合并 `main`（用户 2026-11-24 的决定继续有效，时机由用户定）。

**反目标（做到了也算失败）**

- 为了版式好看而绕过 `sloop_state`/EditCmd，让页面直接调引擎或直接拼 SysEx——双模式归一被破坏。
- 推倒重写 UI、丢掉 TAP/KEY 注入取证体系、est 预算门禁、十条注入场景脚本。
- 新页面还没过门禁就把旧页面删掉，导致某项功能在这段时间不可达。
- 靠扩池/抬 est 上限来硬顶仪表盘（ADR-008 §8.12：实测超限先改对象设计）。
- 联动模式用「Tab5 自己的引擎状态去猜 FM-1」而不是以 FM-1 的回复为真值。

## 4. 约束与诚实边界

- **截图**：本会话模型没有图像输入，无法直接看图。页面构成依据上游源码与 README，截图只做了像素/亮度结构
  核对（双列卡片、通道条、鼓格列结构等）。**人眼对照上游截图是外部门禁**，每个界面子阶段收口时需要用户目检。
  `screenshots/0.png`（README 标注「Sloop Go」）内容未能从源码对应，留待目检。
- **触摸**：拖动、长按可用（现有旋钮已是拖动改值）；**捏合缩放**依赖多点触摸，且本机是 ILI9881C+GT911 批次
  （`docs/tab5-hardware.md` §6 有批次差异），5D Task 0 先实测，不行则以工具条按钮缩放替代（Sloop Go 工具条本就有 Fit/缩放入口）。
- **输入面**：触摸 + Tab5 Keyboard 70 键 + 1 个旋钮。键盘层键（ADR-006）与「焦点槽」模型（ADR-008）保留，
  新页面都要能被键盘/旋钮/`TAP`/`KEY` 注入操作。
- **真机**：联动门禁需要一台刷了 SLOOP 的 FM-1。目前**未知**：它刷的版本、用户上次插的是哪个口、用的什么线。
- **协议权威**：上游 `web/EDITOR_PROTOCOL.md`；本项目摘要在 `docs/research/editor-protocol.md`，冲突以原文为准。
- **上游未变**：两个上游仓库 HEAD 与阶段 4 基线相同，无需更新调研文档。

## 5. 方案

### 5.1 总路线：先连接还是先界面

| 方案 | 内容 | 代价 |
|---|---|---|
| **A. 连接先行（采纳）** | 5A 连接打通 → 5B 壳 + 单轨编辑 + Mixer → 5C 同步层 + 双模式 → 5D 音序/Live/Song → 5E 声音设计页/Pattern 库/Recorder | 用户最关心的界面要等一个小子阶段之后才出现 |
| B. 界面先行 | 5B → 5A → 5C → 5D → 5E | 硬件通路的不确定性（USB-A host、控制台共存、用户 FM-1 的固件版本）晚一个大子阶段才暴露；5B 做完才有机会对着真机验证「同一套界面两个执行端」 |
| C. 两线并行 | 5A 与 5B 同时推进 | 两者都要改 `main/` 的初始化、console、页面注册表与设备验证流程，串行化本项目的子代理执行方式下冲突不断，门禁证据也会互相覆盖 |

选 A 的理由：5A 体量中等（M），却是唯一带**外部未知数**的一段；它的产出（FM-1 的真实协议版本、DESC 实况、
`P_E0`/`P_COUNT`/`G_COUNT`）正是 Sound 页「按设备能力显示控件」的输入；并且 A 让 5B 起的每个新页面都能在
两种执行端上验证，省掉一轮「先独立、后联动」的返工。5A 与 5B 彼此独立，若用户更想先看界面，5A 结束前后都可调换。

### 5.2 界面改造路径

| 方案 | 内容 | 评估 |
|---|---|---|
| U1. 原地换肤 | 保留 9 组 18 页与页签条，只改配色/字体，另加一个仪表盘页 | 便宜，但达不到 Sloop Go 的信息架构（每页页眉、菜单导航、一屏看尽），用户点名的问题没解决 |
| **U2. 新壳 + 逐页重建（采纳）** | 用 Sloop Go 的壳（左上固定簇 + 下拉页菜单 + 每页页眉）替换页签条；页面逐个按 Sloop Go 的版式重建，**复用**现有页面注册表、生命周期、est 预算门禁、`TAP`/`KEY` 注入、`knob.c`、主题色表、键盘焦点模型；鼓格/卷帘/时间条这类网格用「单对象自绘层」（P4-C Task 9 先例） | 新旧页可并存、逐页替换、逐页过门禁；代价是壳一换，注入脚本的导航部分必须同步改写（§7.3） |
| U3. 推倒重写 | 不受现有注册表/注入体系约束，从头写 | 丢掉十条场景脚本、截屏矩阵、est 门禁与 TAP 等价性声明，阶段 3–4 的证据链作废，否决 |

### 5.3 连接技术路线（定案写入 ADR-002，在 5A Task 0）

| 方案 | 评估 |
|---|---|
| **C1. ESP-IDF 原生 `usb_host` + 自写 MIDI Streaming 客户端（倾向）** | Bulk IN/OUT + 4 字节事件包 + SysEx 重组/分段；与音频采集要用的 `espressif/usb_host_uac` 同栈；同板已有跑通的先例（§2.4）；IDF 5.5.5 自带 `usb_host_lib`/`cdc`/`hid`/`msc`/`uvc` host 例程 |
| C2. TinyUSB host | 要把 OTG 栈整体换成 TinyUSB；与 `usb_host_uac` 不同栈，音频采集得另起炉灶 |
| C3. CherryUSB | 整栈替换；MIDI host 类在该版本里的成熟度待查证；对只需要一个类客户端的本项目过重 |
| C4. 现成组件 | Espressif 组件库 `espressif/` 命名空间下没有 `usb_host_midi`（有 `usb_host_uac`、`usb_host_hid`）；其它命名空间在 ADR-002 时再查一轮 |

### 5.4 双模式调度（概要；定案写入 ADR-011，在 5C）

- 两个模式是**互斥的编辑会话**：同一时刻执行端只有一个。FM-1 接入时弹提示「进入联动模式？」，不自动切换、
  不打断正在播放的独立会话。
- 切到联动时本地会话**冻结保留在 RAM**（不是丢弃），切回独立模式原样恢复；联动模式的编辑历史断线即丢（同 Sloop Go）。
- 联动模式下本地引擎不出声；FM-1 的声音经 5E 的 UAC1 采集进 ES8388（采集落地前，联动模式只遥控、Tab5 自己不响）。
- 5A 期间的临时处置：检测到 FM-1 只在 console `FM1 STATUS` 与日志里可见，不改变任何行为
  （状态栏提示与 Device 页随 5B 的壳出现）。

## 6. 子阶段划分

体量只给相对量级（S / M / L / XL），不给工期。每个子阶段开工时各写一份 spec + 实施计划（沿用 P4-B…P4-F 的做法）。

### 5A 连接打通（体量 M；= 路线图 P5-0 + P5-1 + P5-2 的编解码/INFO 子集）

**目标**：FM-1 插入 USB-A → Tab5 识别设备 → 完成 INFO 版本协商 → WATCH/PING 保活、设备上的旋钮改动以 CHANGED
推送到 Tab5；全部状态经 console 命令可读、可脚本断言。即 §5.5「联动协议」门禁的脚本化：
连接 → INFO 版本协商 → WATCH push 可达。

**不做**：请求队列与瞬时 SET 合并、LIVE 遥控、音频采集、让现有页面直驱 FM-1（这些是 5C/5E）；
**也不做任何 UI 页面**——Device 页连同新壳一起在 5B 落地（5A 若往旧页签条里加页，导航脚本与截屏矩阵要改两轮，
Device 页也要建两次）。5A 的用户可见面就是控制台回显与日志。

**版本政策**：只支持与本项目基线同版的 SLOOP（2.8.x，协议 v12）。FM-1 上是更旧的 SLOOP 时，INFO 读出的版本
经 `FM1 STATUS` 如实显示并标注「请升级 FM-1 固件」；不做旧版参数 id 映射表，也不为旧版把 AppState 的定长数组
改成按设备计数。

**内容**

0. **前置首验（Task 0，任何驱动代码之前）**，四件事都有明确的通过判据：
   ① **控制台与 host 共存**：USB-C 的 USB-Serial/JTAG 控制台与 USB-A host 同时工作（芯片有两个 OTG 控制器、
   HS/FS 各一个 PHY：`SOC_USB_OTG_PERIPH_NUM=2`、`SOC_USB_FSLS_PHY_NUM=1`；USB-A 实际接在哪个 PHY 上以原理图
   为准）。判据：host 库装上后，console 命令与烧录照常。
   ② **USB-A 供电**：`bsp_usb_host_start()`（本仓库锁定的 BSP 版本）是否给 USB-A 供上电；否则直接置
   PI4IO 0x44 E2.P3 `USB5V_EN`。判据：万用表/接入设备枚举成功二者之一。
   ③ **内部 RAM**：判据是「门线之上留得住」。顺序：先把能挪到 PSRAM 的挪走（USB 库任务等不在中断上下文的
   任务用 `xTaskCreateWithCaps` + `MALLOC_CAP_SPIRAM`；复核现有任务栈里有没有同样能挪的），再量固定占用。
   实在压不回 60KB 之上的部分，**按实测的固定占用下调 `HEAP_FLOOR`**，作为门禁变更登记到
   `docs/dev/reference/verification-gates.md` 与 `device-notes.md`（附改动前后的 `ENG MEM` 读数）。
   这会动一条全阶段门禁的数字，所以在 §9 单列为需用户知情的裁决。
   ④ **参考材料入库**：`third_party/sloop-fm1-go` @ `ed941b5` 带 `MANIFEST.sha256` 入库（与
   `third_party/sloop-fm1` 同一做法，GPL-3.0，只读参考、不进构建；含 `screenshots/`），供 5B 起的布局依据、
   像素抽检参照图与协议实现对照离线可用。
1. **ADR-002**（USB-MIDI host 选型，关闭 DI-1 的选型部分；§5.3 的 C1，附 C4 的补查结果）。
2. **`usb_fm1` 组件**：host 库任务、client、枚举（优先 VID:PID `1209:0001`；端口名 "Felucca" 需读字符串描述符）、
   找 MIDI Streaming 接口并 claim、Bulk IN/OUT 环、USB-MIDI 事件包 ⇄ MIDI 字节流、**SysEx 重组**（CIN 4/5/6/7）
   与**分段发送**、断开（`DEV_GONE`）清理与重插恢复。
3. **`editor_proto` 组件**：帧格式 `F0 7D 46 4C <cmd> … F7`、v14/pack7/u35 编解码、请求构造与回复匹配
   （按 cmd；23/24/26 永不作回复号）、INFO 解析（版本串/NENGINES/P_COUNT/G_COUNT/NSTEP/P_E0/协议版本）、
   push 解析（CHANGED/RELOAD/STEP_CHANGED/TRACK_CHANGED）。全部 host 可单测。
4. **最小事务层**：同步「一问一答 + 超时」（one-in-flight 队列的雏形）；`WATCH 3` + `PING` 1s 定时器。
5. **常驻 host**：USB host 库开机即装（插拔检测依赖它），不做成惰性启动。
6. console：`FM1 INFO` / `FM1 WATCH <0|3>` / `FM1 GET <scope> <id>` / `FM1 SET <scope> <id> <v>` /
   `FM1 PING` / `FM1 STATUS` / `FM1 EVENTS`（收到的 push 计数与最近一条），全部回显可断言。
7. `device-notes.md` §3 登记；`deferred-issues.md` 的 DI-1 按解决程度删除或收窄。

**验证**：host（codec round-trip；SysEx 重组的分段/粘包/坏帧/超长；回复匹配；INFO/push 解析，golden 取自上游
`editor.c` 与协议原文）；设备（枚举到 1209:0001 → INFO 版本串 → WATCH 3 回 3 → 在 FM-1 上拧旋钮后 Tab5 收到
CHANGED → 拔线清理、重插恢复 → 全程 USB-C 控制台不掉）；门禁（构建零警告、host 全过、`selftest-heap.py`
与 soak 按重定后的门线）。

**门禁脚本形态（操作员在环）**：`scripts/fm1-smoke.py` 在需要人手的地方打印提示并轮询等待——
「请把 FM-1 插到 Tab5 的 USB-A 口」等到 `FM1 STATUS` 报已连接；「请在 FM-1 上拧任意一个旋钮」等到
`FM1 EVENTS` 出现 CHANGED（带超时）。编辑器自己的 SET 不产生 push（协议规定），所以这一步只能由人来做。
5A 之后的设备回归分两种条件跑：host 常开、FM-1 未接（独立模式全矩阵）与 FM-1 已接（联动场景）。

**风险**：①控制台与 host 的共存（Task 0 第一件事，不通则需改控制台通道，影响面大，立即上报）；
②内部 RAM 压不回门线之上（走 §内容 0 ③ 的门禁变更）；③用户 FM-1 的固件版本偏旧（按版本政策提示升级，
设备门禁等用户升级后才能关闭）；④线材/接口（必须 USB-A 口 + 能传数据的 A 转 C 线）。

### 5B 新壳 + Sound（单轨编辑）+ Mixer + Device 页（体量 L）

- **壳**：左上固定「▶/■」「⋮」（下拉：页面列表 + undo/redo）；每页固定页眉（页面控件 + 右侧链路/队列状态）；
  页签条退役；生命周期从「组作用域」改为「单页作用域」（建目标 → 切换 → 销毁来源页）。
- **过渡期的壳高固定为 72px**（`UI_TAB_BAR_H` 不变，页面区原点不动）：固定簇 + 页眉 + 右侧状态都排在这一条带里。
  这样 10 张尚未重建的旧页**零改动**，`scripts/inject/inject.py` 里几十处 `72 + 页内 y` 的注入坐标与
  `screenshot_matrix.py` 的 `TABCAT` 也继续有效——换壳的脚本改写量只剩导航部分。等旧页全部退役（5E 收口）
  再评估是否收到 Sloop Go 的 48dp。
- **Device 页**（新壳上的第一张页，版式按 Sloop Go Device 页）：未连接/连接中/已连接三态；固件信息（版本串、
  NENGINES、P_COUNT、G_COUNT、NSTEP、P_E0、协议版本）；断开；版本过旧时的升级提示。它把 5A 的 `FM1 STATUS`
  所见搬到屏幕上，不新增协议能力。
- **退役判据 = 功能全覆盖 + 新页过门禁**，两者缺一不退；任何功能在任何时刻都保持可达。
  5B 结束时的退役清单：SOUND / MOD / SCALE / VOICE / GLOBAL / FX（全部参数进 Sound 页，含 MIDI 组卡片——
  这些全局参数独立引擎里同样存在，不必等 5C）与 **TRACKS**（Mixer 的通道条在 Mute 旁多一个 **Solo** 键：
  Sloop Go 的 Mixer 没有 Solo，因为它的 Solo 在 Live 页，而 Live 要到 5D 才有；这一键是 Tab5 的必要补充）。
  **TOOLS 不退**（CLEAR SEQ / INIT SOUND / NEW 在 Sloop Go 里分散在音序页控制条与菜单，5D 接手）。
- **过渡期规则**：被替换之前，旧页面以原名留在菜单里（STEP、TRANSPORT、TOOLS、SONG、DSYN、FM6P、SAMPLE、
  PROJECTS、PRESETS、SYSTEM、PLAY）。
- **主题与字体**：默认「SLOOP GO」外观（深色：背景 #000、卡片 #0E0E0E、次级 #1A1A1A、文字 #F2F2F2/#BDBDBD、
  描边 #3A3A3A、主色绿 #1ECC70、点缀蓝 #287CFF/黄 #FFC618/橙 #FF621A；浅色按上游 Light 方案）；现有 FM-1
  五套色板与主题开关保留为可选项（上游 COLOR 特性）。JetBrains Mono 位图字体（OFL-1.1，登记到
  `docs/dev/upstream-licenses.md`）。
- **Sound 页**：页眉（轨选择、预置名下拉 → **预置选择器列表**（吸收 DI-54）、引擎/音色选择、鼓轨 kit）；
  双列卡片：Global、Master、Envelope、LFO、Engine edit、Voice、FX、Scale、Arp、Pattern、Drums、MIDI、Texture；
  卡片折叠与排序（持久化）；鼓轨的 DRUM SOUND 卡（16 lane 的 TUNE/DECAY）；
  枚举 → chip 行、开关 → 切换控件、BPM → 文本控件；收藏面板（DI-44）列为可选。
- **卡片成员的权威来源 = 一份纯数据表**（做法同 `ui_nav.h` 的 `UI_GROUP_LIST`）：内容照 Sloop Go
  `ParamsScreen.kt` 的 `layout(pe0)` 移植——每个成员是「作用域 + 参数 id」，带版本门槛（按 INFO 的
  `pe0` / 协议版本）；外加 Sloop Go 的 `shouldShow` 过滤（空标签、`SKIP` 名单、`min==max` 且无枚举）。
  **描述符只提供值的形态**（fmt / min / max / def / label / unit / 枚举名），不提供分组与可见性——
  USB 协议的 DESC 回复里本来就没有这些字段，Sloop Go 也是把分组写在 App 一侧。所以「描述符提供者」这道缝
  （5C）只需要换掉描述符与 INFO 数值的来源，卡片结构不随执行端变化。
- **Mixer 页**：四条通道条（序号/引擎名、电平数、竖向推子、Mute、Solo、Edit）。
- **内存**：Sound 页 est 预计 35–40KB，Task 0 用原型页实测；超限预案按序：只建可见卡片（滚动虚拟化）→
  行级自绘 → 池移 PSRAM（需新 ADR，此前从未评估）。
- **键盘/旋钮**：焦点模型沿用（←/→/↑/↓ 在卡片内移动、ENTER = 焦点槽 +1、旋钮改焦点槽）；层键不变。

**验证**：新增 `sound.sh`、`mixer.sh`、`device.sh`（FM-1 未接 / 已接两态）注入场景；`nav.sh`、`soak.py`
的导航序改写；截屏矩阵重做；对照上游 1.png / 9.png / 11.png 做区域像素抽检（参照图取
`third_party/sloop-fm1-go/screenshots/`）；人眼目检为外部门禁。

### 5C 同步层 + 双模式（体量 L；= 路线图 P5-3 + P5-4）

- 把 Sloop Go 的 `DeviceController`（单在飞 FIFO 上限 16384、瞬时 SET 合并、超时、断连丢弃）与 `DeviceState`
  （不可变快照）的语义用 C 移植；AppState 增加可替换的执行端；DESC/GET/DUMP 初载、TRACK_MIX、选轨；
  push → 快照；LIVE 轮询（~8Hz，播放态/播放头）；Now/Late 两模式与 SEND（随 5D 的音序页落地）。
- **ADR-011 双模式调度**（§5.4）。Sound / Mixer / Device / Song 页直驱 FM-1；断线恢复。
- 评估项：Sloop Go 的 App 侧 4 个 LFO（手机算好经参数队列写下去）是否在 Tab5 上移植——联动模式下有意义，
  独立模式用引擎自带的 LFO 参数。
- **验证**：同步场景清单（编辑下发 / push 上行 / 超时重试 / 断连恢复 / 切换模式）；前后截屏 diff 证明双向同步
  （Tab5 操作 → FM-1 屏幕人工确认；FM-1 操作 → Tab5 `SCR` 变化）。

### 5D Sequencer + Live + Song（体量 XL；吸收阶段 4 的大部分 DI）

- **Sequencer**：顶部控制条；鼓格 16 lane × 整轨 LEN（缩放、点按、长按拖画、lane 拖动、力度拖动、播放头）；
  钢琴卷帘（加/删音符、拖右端改 TIE 时值、长按移动、框选、量化、±12 变调；MONO/LEGATO/UNISON 下不重叠、
  POLY 下允许重叠的上游语义）；步编辑对话框；键盘 = 演奏录入 + 八度移位（DI-53）。自绘层实现。
- **Live**：工具条（播放/BPM/母带/CPU，tap tempo 吸收 DI-48）+ 混音卡 + 27 个 punch-in 垫（DI-52）、
  实时擦除（DI-45）、ARP roll（DI-47）、鼓 MUTE mode（DI-46）、可视化（DI-51）。
- **Song**：A–D 段落卡（含 pattern 缩略图）+ 编排时间条（重排现有 SONG 页的功能）。
- 旧 STEP / TRANSPORT / PLAY / TOOLS 页退役（TRACKS 已在 5B 由 Mixer 接替）；TOOLS 的三个动作分流到
  Sequencer 控制条（Clear）与 ⋮ 菜单（INIT SOUND / NEW），两段式确认语义（ADR-008 §8.11）不变。

### 5E 声音设计页 + Pattern 库 + Recorder（体量 XL；含路线图 P5-5）

- Drum synth、FM6（库条 + 算法图 + 6 张算子卡）、Samples（槽卡 + 波形 + **Chop 切片**，吸收 DI-34）、
  MIDI patterns（pattern 库：存/取/改名/缩略图/拖入卷帘）。
- **Recorder**：UAC1 采集（`usb_host_uac`）→ WAV（FLAC 可选）→ SD/FAT；压限器；频谱/波形/声像。
  FM-1 的 UAC1 速率（44.1/48k）与本地 44.1k 的双时钟/SRC 方案届时出 ADR。
- Device 页补主题；System 设置（亮度/音量/LOWCUT/RESO/INFO/颜色）并入 Device 或保留为 Settings。
- **Tab5 独有、予以保留的页面**：Projects、Presets（本地工程槽与预置银行；Sloop Go 未移植这两项）。

**阶段 5 完成判据**（路线图 §7 不变，补充一条）：联动协议门禁全绿；双向同步场景全过；音频采集可听；
DI-1 删除；**新界面的人眼对照目检通过**。dev → main 合并仍按用户指示顺延。

## 7. 跨子阶段规则

1. **页面只认 AppState + EditCmd**。新页面不 include 引擎私有头、不直接发 SysEx。联动模式下一切显示以 FM-1
   回复为真值。
2. **值的形态来自描述符，结构来自纯数据表**。参数页每个控件的 fmt / 范围 / 标签 / 枚举名取自描述符
   （独立模式由 sloop_core 供给，联动模式由 DESC 回复供给）；控件的分组、成员、顺序与版本门槛取自 UI 侧的
   纯数据表（同 `ui_nav.h` 的做法），与执行端无关。页面不硬编码取值范围。
3. **旧页退役 = 功能全覆盖 + 新页过门禁 + 其证据同步替换**。三者缺一不退；每退役一张旧页，必须同时交付
   新页的注入场景脚本、`nav.sh`/`soak.py` 导航序更新、截屏矩阵重做；旧脚本不删到新脚本过门禁之后。
   console `UI <page>` 页名表随页面表更新。
4. **预算与门禁沿用**：每页一个注册表条目 + 实测 est + 构建前分配检查；Σ 逼近池总量先做设计决策。
5. **上游跟进**：开工各子阶段前复核两个上游仓库 HEAD；有变则先更新 `docs/research/` 与 `third_party/` 清单。
6. **文档协议**：ADR-002（5A）、ADR-011（5C）、必要的 ADR-013（池迁移，仅当触发）；路线图 §7 按本文 §6 改写
   （用户确认后作为 5A Task 0 的一部分）；AGENTS.md 索引与 `docs/dev/reference/` 按 §6 的规则同步。

## 8. 留给后续 spec / 实施者决定的事

- 5A：`usb_fm1`/`editor_proto` 的头文件接口、USB 事件任务与事务层的任务/队列划分、超时与重试参数、
  字符串描述符读取时机、console 命令的精确输出格式、host 测试框架的落点（沿用 `scripts/*-test.sh` 先例）。
- 5B：菜单条目顺序与图标/文字、页眉各页控件清单、卡片默认排序与折叠默认值、浅色主题是否首版就做、
  「SLOOP GO」外观进上游色表体系的方式、排序/折叠状态的存储位置（NVS 键名）。
- 5D：自绘层的坐标/缩放模型、手势判定阈值、框选与拖画的互斥规则。
- 5E：Chop 的起音检测算法移植范围、录音文件命名、FLAC 是否首版实现。

## 9. 本次由我代为裁决的事项（auto 模式）与「若错代价」

| 裁决 | 理由 | 若错的代价 |
|---|---|---|
| 先 5A 连接、后 5B 界面（§5.1 A） | 5A 小且带外部未知数，其产出是界面的输入 | 界面晚一个中等子阶段出现；两者独立，随时可调换 |
| U2 新壳 + 逐页重建，不做 U1 | U1 解决不了「单轨编辑没有」 | 一次性换壳的脚本改写成本已计入 5B |
| 目标是信息架构与每页构成一致，不追求 Material 控件外观一致 | 触摸屏 + 实体键盘 + 旋钮的操作面不同；LVGL 原生风格更稳 | 之后可单做一轮视觉打磨，结构不变 |
| 界面分 5B/5D/5E 三批落地 | 一批做完太久且风险集中；每批都可独立收口 | 用户若想先看某页（如钢琴卷帘），可在 5B 后调序 |
| 保留 Projects / Presets 两页 | Tab5 的本地存储是独立模式的核心能力，Sloop Go 没有 | 去掉很容易 |
| 双模式为互斥会话 + 接入提示（§5.4） | 最简单且不破坏正在进行的独立会话 | 5C 出 ADR 时仍可改，届时未写代码 |
| 不动 LVGL 池架构，超限才评估 PSRAM | 估算 35–40KB 在现有峰值量级内 | 若 Task 0 实测超限，预案已列，损失一个任务 |
| **只支持与基线同版的 SLOOP（2.8.x / 协议 v12），不做旧版 id 映射**（5A「版本政策」） | 旧版映射会让 AppState 定长数组、Sound 卡片表、5C 体量翻倍；FM-1 固件由用户掌控，升级即可 | 用户的 FM-1 若是旧版，需先升级才能过设备门禁 |
| **USB host 开机常驻；内部堆门线按实测固定占用重定基线**（5A Task 0 ③）——**需用户知情** | 插拔检测依赖 host 常驻；开机 `internal_min` 已压在 60KB 门线上，不让出几 KB 就装不下。先尽量把任务栈挪去 PSRAM，再把压不掉的部分登记为门禁变更 | 这动的是一条全阶段安全余量的数字。若不接受，退路是把 host 做成惰性启动（进 Device 页才装），代价是「插上即提示」做不了 |
| 5A 不做任何 UI 页面，Device 页随 5B 的壳落地 | 免得往旧页签条加页、导航脚本改两轮、Device 页建两次 | 联动门禁在 5A 只能从控制台看，用户要到 5B 才在屏幕上看到连接状态 |
| 过渡期的壳高固定 72px | 10 张旧页与几十处注入坐标零改动 | 壳比 Sloop Go 的 48dp 高一截，观感打折，直到旧页全部退役 |
| 卡片成员/顺序/版本门槛 = UI 侧纯数据表（移植自 Sloop Go `layout(pe0)`）；描述符只给值的形态 | 协议的 DESC 本就不含分组与可见性，Sloop Go 也是这么做的 | 无——这是上游的实际结构 |
| 旧页退役判据 = 功能全覆盖 + 过门禁；Mixer 加 Solo 键以便在 5B 退役 TRACKS；TOOLS 留到 5D | 不让任何功能出现「临时不可达」 | Mixer 比 Sloop Go 多一个键 |

## 10. 需要用户提供/确认的事

1. FM-1 上刷的是 SLOOP 吗？哪个版本（FM-1 的 SYSTEM → ABOUT 可见；5A 做完后 `FM1 STATUS` 也能读出来）？
2. 上次「连不上」时，FM-1 插的是 Tab5 的 **USB-A 口**还是 Type-C 口？（当前固件两个口都不会有反应，
   但这决定 5A 的验证步骤怎么写。）
3. **设备门禁需要人手**：5A 的冒烟脚本在两处会停下来等操作员——把 FM-1 插到 Tab5 的 USB-A 口（需要一根
   能传数据的 A 转 C 线），以及在 FM-1 上拧一个旋钮。之后每个联动场景的设备验证同理。
4. 每个界面子阶段收口时，需要用户对着上游截图做一次人眼目检（§4）。

## 11. 信息来源

- `jahlib/sloop-fm1-go` @ `ed941b5`：`README.md`、`android/README.md`（Current scope）、
  `android/app/src/main/kotlin/com/sloop/go/ui/{App,ParamsScreen,PageHeader,Theme,MixerScreen}.kt`（全文），
  其余 UI 文件的函数级提纲，`screenshots/0–11.png`（像素/亮度结构分析）。
- `isod89/sloop-fm1`：`git ls-remote`（HEAD 与 tag 核对）；`web/EDITOR_PROTOCOL.md` 的本项目摘要
  `docs/research/editor-protocol.md`。
- 本仓库：`docs/design/005-ui-architecture.md`（§落地状态、§容量决策、§导航信息架构）、
  `docs/design/012-feature-parity-checklist.md`、`docs/deferred-issues.md`（DI-1）、`docs/dev/device-notes.md` §3、
  `docs/tab5-hardware.md` §3.11、`firmware/sdkconfig`、`firmware/partitions.csv`、
  `firmware/components/sloop_ui/src/knob.h`。
- 本机：`~/projects/M5MusicTab5/components/service_usb_host/`（同板先例，MIT）、
  `~/projects/M5Tab5-UserDemo/platforms/tab5/components/m5stack_tab5/`（官方 BSP 的 `bsp_usb_host_start`）、
  `~/esp/esp-idf` v5.5.5 的 `components/usb/` 与 `soc_caps.h`。
