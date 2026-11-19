# sloop-fm1 → Tab5 移植设计规格（Design Spec）

> 日期：2026-11-19 ｜ 状态：草案（待项目负责人评审）
> 上游基线：SLOOP 2.8.5（bac9c05）· Sloop Go 1.8.0（ed941b5）· 编辑器协议 v12（基线详情见 `docs/research/`，各阶段开工时复核）
> 关系：本文是**设计基线**（架构 + 方案选择 + 决策状态）；`docs/design/002-port-roadmap.md` 是**阶段主计划**（任务编号、排序、门禁）；两者互补，冲突时以先更新者为准并同步另一份。

---

## 1. 意图与动机

把 M-VAVE FM-1 上的自制固件 SLOOP（isod89/sloop-fm1，GPL-3.0）移植到 M5Stack Tab5（ESP32-P4 / ESP-IDF v5.5.5），产品形态为双模式（AGENTS.md §1）：

1. **独立模式**：Tab5 自身成为 SLOOP groovebox——同一 DSP 引擎、音序器与层式操作模型，经触摸屏（1280×720 MIPI-DSI）与 Tab5 Keyboard（70 键 I2C）操作，音频经 ES8388 输出。
2. **联动模式**：刷 SLOOP 的 FM-1 经 USB 接入 Tab5（Tab5 为 host）时，达到 Sloop Go 级别的双机同步体验——编辑器协议（USB-MIDI SysEx）双向同步 + UAC1 音频采集。

**为什么可行**：上游核心（`firmware/src/`，纯 C）与平台解耦良好，本就有 PC/WASM 宿主构建（`host/preview.c`）；Tab5 移植在工程上是「再写一个宿主 + 替换 UI + 角色反转 USB」。

**为什么这样做**（设计动机）：SLOOP 的价值在引擎与操作模型，不在 FM-1 的 240×240 UI；Tab5 的大屏与全键盘能把同一操作模型放大成更好的独立设备，同时其 USB host 能力又能让它成为 FM-1 的编辑器——两条产品线共享同一状态抽象，投入一次。

## 2. 目标 / 非目标 / 反目标

**目标**
- 独立模式：与 FM-1 上 SLOOP **等价**的完整体验（12 引擎、四轨 64 步音序、FX、预置、采样、歌曲）——正确性以与上游 host 构建逐样本一致为锚（golden）。
- 联动模式：Sloop Go 的核心体验——双向同步编辑、WATCH 推送、LIVE 遥控、UAC1 采集。
- 全程可验证：每阶段有机器可判的门禁（构建/哈希/截屏/协议冒烟），证据入库可追溯。

**非目标**（当前，AGENTS §1）
- 不做 FM-1 固件更新/刷机（**禁止发送更新协议帧** `F0 22 24 35…` / `00 59…`）。
- 不做 BLE MIDI（上游不开蓝牙，Tab5 无经典蓝牙）。
- 不改动上游 sloop-fm1 的 FM-1 侧行为。

**反目标**（技术上成功也算失败）
- 引擎输出与上游**不可证一致**就直接做性能优化。
- 为追赶上游而大改引入的 `sloop_core` 内部代码，导致无法 diff 跟随上游演进。
- 独立模式与联动模式各写一套 UI 状态逻辑。
- 验证证据只存在于 `build/`（会被 `--clean` 删除）而未晋升入库。

## 3. 约束

| 类别 | 约束 | 出处 |
|---|---|---|
| 许可 | 衍生固件整体 GPL-3.0；保留上游版权与子模块许可（Dexed msfa / DaisySP / Mutable 等）；样本资产须单独核查再分发许可 | research §4.1、AGENTS §8 |
| 上游策略 | sloop_core 以固定上游 commit 引入，改动以补丁/适配层隔离，禁直接大改 | AGENTS §8 |
| 协议权威 | 编辑器协议以上游 `web/EDITOR_PROTOCOL.md` 为唯一权威 | AGENTS §8 |
| 音频 | 采样率对齐上游假设 **44.1kHz**；音频块大小移植时从 `audio.c`/`fm1_audio.h` 确认 | research §4.3 |
| 数值 | host/device 双侧 `-ffp-contract=off`、禁 `-ffast-math`；golden 基线随工具链/flags 入库 | roadmap §4 |
| 硬件 | 本机 ESP32-P4 **rev v1.3 样片**（`<3.0` 与 `>=3.0` 支持互斥）；屏幕 **ST7121 批次**（BSP 1.3.3 运行时探测分流）；键盘独立 I2C G0/G1 @0x6D、INT=G50；flash 16MB / PSRAM 32MB | device-notes §1 |
| 构建 | ESP-IDF v5.5.5（ADR-001）；BSP `espressif/m5stack_tab5 ^1.3.3`；构建/烧录/验证统一走 `scripts/` | AGENTS §5 |
| 验证 | UI 改动以 SCR 截屏 + 视觉检查为完成条件；门禁证据晋升 `docs/dev/verify/` 并登记 verification-log | AGENTS §5.4/§5.5、roadmap §8 |

## 4. 总体方案（Approaches）

三个真实可选的总体路线：

| 路线 | 内容 | 取舍 | 结论 |
|---|---|---|---|
| **A. 引擎先行、分阶段**（已选，即路线图） | host golden 锚定 → 平台 bring-up → 引擎上机 → 独立 UI → 联动 | 每步可验证、风险前置化解（引擎正确性最先锁定）；UI/协议等大块工作建立在已验证的地基上 | ✅ **采纳**：与「正确性优先于性能」「host 先行」原则同构 |
| B. 大爆炸一次性移植 | 一次性把 src/ + UI + USB 全部搬上 Tab5 再调 | 省去阶段管理开销；但引擎/UI/USB 三类风险纠缠，出问题时无法定位是移植错还是原始理解错 | 否决：不可验证的中间态违反门禁体系 |
| C. 联动先行（薄客户端优先） | 先做 Tab5 作 FM-1 编辑器（Sloop Go 移植），独立模式后置 | 联动不依赖引擎移植，理论上更快见效 | 否决：核心不确定性（DI-1 USB-MIDI host 驱动自研）在最前面；且「Tab5 自成 groovebox」是第一产品目标 |

**路线 A 内部的关键分决策**见 §6 决策表——已定项直接生效；标「阶段入口 ADR」的项在对应阶段开工时以微型 ADR 定稿（避免今天的推测性决策污染后续阶段）。

## 5. 架构设计

### 5.1 组件划分（目标形态）

```
firmware/
├── main/                     # 应用入口：启动序列 + 双模式调度（AGENTS §3 预留课题）
├── components/
│   ├── sloop_core/           # 上游引擎（固定 commit 引入，见 D1）+ 最小适配
│   │   └── upstream/…        #   上游 src/ 原样（或经微补丁）
│   ├── sloop_hal_tab5/       # 上游 hal/fm1_*.h 接口的 Tab5 后端：
│   │                         #   音频(I2S/ES8388)、存储(FATFS/NVS)、时间/定时器、日志、双核
│   ├── tab5_keyboard/        # Tab5 Keyboard I2C 驱动（0x6D，阶段 1 落地，阶段 3 被 InputManager 消费）
│   ├── editor_proto/         # 编辑器协议帧编解码 + 会话（INFO/GET/SET/WATCH/LIVE…；联动模式）
│   ├── usb_fm1/              # USB host：FM-1 枚举 + MIDI bulk + SysEx 重组 + UAC1（ADR-002）
│   └── sloop_ui/             # LVGL UI + InputManager + 状态模型（阶段 3 立项时定名与边界）
```

依赖方向（单向）：`main → sloop_ui → (状态模型) → sloop_core / editor_proto`；`sloop_core → sloop_hal_tab5`；`editor_proto → usb_fm1`。UI 组件的具体拆分（是否再分 input/ui 两个组件）在 P3-2 UI 架构 ADR 时定，**此处只锁定「UI 与引擎/协议经状态模型解耦」**。

### 5.2 双模式状态抽象（本设计的核心结构决策）

两种模式共享同一上层结构（参照 Sloop Go 的 DeviceController/DeviceState，research-go §6.3）：

```
        ┌───────────── 同一套 UI（页面/控件/视觉） ─────────────┐
        │                    AppState（不可变快照）              │
        │                        ▲ 渲染                          │
        │            ┌───────────┴───────────┐                  │
        │        编辑命令（EditCmd）                       │
        │            │                           │                  │
        │   LocalEngineSession            RemoteFm1Session       │
        │   （sloop_core 直驱）      （editor_proto over usb_fm1）│
        └────────────────────────────────────────────────────────┘
```

- **AppState**：单一不可变状态结构体（ESP-IDF 下：新快照入队 + 事件通知，UI 任务渲染完即弃）。字段以编辑器协议的模型为词表（参数 P_*/G_*、轨/步/预置），因为它天然覆盖 SLOOP 全部可编辑面且两种模式共用同一词表。
- **EditCmd**：编辑命令的归一表示。离散编辑保序、连续编辑（旋钮/拖动）可合并只留最新——该语义在独立模式下抑制引擎冗余重算，在联动模式下正是协议单在飞队列的合并策略（Sloop Go 同款，research-go §3.1）。
- **会话接口**：`open/close/push(EditCmd)/snapshot()/事件回调`。LocalEngineSession 在进程内直调引擎；RemoteFm1Session 走协议。UI 对会话类型无感知。
- 独立模式下**是否直接复用上游 `editor.c` 的命令语义**作为 EditCmd 词表（内嵌为本地 IPC），还是自定精简词表：**P2-3 ADR 定稿**（见 D2）。词表无论如何向协议靠拢，保证阶段 5 复用。

### 5.3 引擎与音频路径

- 音频链：`sloop_core 渲染线程 → sloop_hal_tab5 音频缓冲 → I2S → ES8388`，44.1kHz、块大小对齐上游（移植时确认）。上游 `fm1_audio.h` 的 DMA 节拍语义映射到 I2S DMA 回调。
- 任务/核分派：先单核跑通（research §4.7），性能基线（P2-6）之后才考虑双核分派（音频渲染与 UI/LVGL 分核是首选拆法，届时有数据再定）。
- PSRAM 32MB 足够容纳样本资产工作集；帧缓冲与 LVGL 已由 BSP 占用部分 PSRAM，内存水位进 soak 判据。

### 5.4 存储布局

- `partitions.csv`：**单 factory app 8MB + FATFS 资产 ~7.9MB + NVS**（回滚 = esptool 重烧，路线图 P1-1 已定稿姿态）。
- 上游 flash 对象模型（A/B 扇区 + 11 类备份对象，BK_LIST/BK_GET/BK_PUT）重映射到 FATFS/NVS；**备份对象字节格式保持兼容**以互通 FM-1 备份文件（research §4.6）。
- 样本资产主力在 flash FATFS 还是 SD 卡：P2-5 微型 ADR（分区已按两种选择都能容纳设计）。

### 5.5 验证架构（每层的正确性证明）

| 层 | 证明手段 |
|---|---|
| 引擎 | `scripts/host-test.sh`（上游 golden 哈希，PC 基线）→ `ENG SELFTEST` 设备侧同套用例（哈希相等；不等时按 max\|err\|/SNR 容差通道判定并留痕） |
| UI | SCR 截屏 + 视觉检查（§5.4 门禁）+ 阶段 3 起串口注入（`KEY`/`TAP`）使状态可脚本复现 |
| 协议 | codec round-trip 单测（host 可跑）→ mock 对端冒烟 → 真机 FM-1 冒烟脚本 |
| 系统 | soak：剩余堆水位不随时间单调下降、无 watchdog/backtrace/重启 |
| 证据 | 一切门禁证据晋升 `docs/dev/verify/<阶段>-<日期>/` + verification-log 登记（含 SHA256） |

## 6. 关键决策表

| # | 决策点 | 选项 | 状态 / 建议 |
|---|---|---|---|
| D1 | 上游引入机制（P2-1） | (a) git submodule 固定 commit；(b) vendoring：脚本化按 commit 抓取入库 + 补丁层 | **建议 (b)**：构建不依赖网络/submodule init 状态，GPL 对应源完整随仓；diff 跟随靠 pin commit + 适配层隔离。**阶段入口 ADR 定稿** |
| D2 | 本地编辑入口（P2-3） | (a) 复用上游 editor 命令语义作本地 IPC；(b) 自定精简 EditCmd 词表 | **建议先 (b)**：EditCmd 只需覆盖 UI 所需编辑面，协议命令全集（备份/FM6 银行等）留给阶段 5 直译；词表对齐协议字段。**阶段入口 ADR 定稿** |
| D3 | UI 布局（P3-2） | (a) 1280×720 原生重排版（参考 Sloop Go 屏设）；(b) 240×240 放大兼容 | **建议 (a)**：上游 UI 为 240×240 单色风，放大到 5″ 既浪费屏幕也不适合触摸目标尺寸；层式操作模型保留、视觉重排。**阶段入口 ADR 定稿** |
| D4 | 样本资产位置（P2-5） | flash FATFS（~7.9MB） vs SD 卡 | **建议 flash 先行**：容量对 37 kit 的 CC0 采样鼓组待核实，不足再上 SD（SD 为后加硬件，不破坏分区）。**微型 ADR 定稿** |
| D5 | 分区姿态 | OTA A/B vs 单 factory | **已定**：单 factory app 8MB（路线图 P1-1） |
| D6 | USB-MIDI host 驱动（DI-1/ADR-002） | (a) esp-usb host 栈自写 MIDI 1.0 类驱动；(b) tinyusb host；(c) 社区组件 | **建议 (a)**：bulk 双端点 + 4 字节事件包，协议面小，有 fm1-cartridge 先例；先例代码与 Sloop Go 重组逻辑可参考。**P5-0 前 ADR 定稿** |
| D7 | 双模式调度（P5-0） | FM-1 接入时本地会话：切换 / 并行 / 双音频源 | **阶段入口 ADR 定稿**（含音频混音路径决策） |
| D8 | 引擎双核分派 | 单核先行 vs 立即双核 | **已定**：单核跑通 + 性能基线后按数据决定（research §4.7） |
| D9 | Tab5 的 USB device 侧能力（独立模式） | (a) OTG 始终 host-only：不暴露 USB-MIDI/MSC gadget，文件经 microSD 进出；(b) 独立模式切 gadget（MIDI/MSC），联动模式切 host | **建议 (a)**：P4 仅一个 OTG 控制器，角色切换复杂度远超收益；SD 卡槽独立于 OTG 可承担样本/备份/FM6 SysEx 的进出（与 D4 回退路径一致）；代价是独立模式对 PC 的 USB-MIDI/MIDI 时钟同步能力缺失，在阶段 4 对齐清单中单列「上游有 / 本移植无（硬件形态不同）」类。**阶段 3/4 入口定稿** |

> 决策表状态为「建议」的条目由本 spec 提出（我的判断 + 理由），最终以对应阶段 ADR 落档为准；ADR 落档后回写本表状态。

## 7. 各阶段设计要点（补充路线图未尽的设计细节）

### 阶段 1 — 平台 bring-up（不含 SLOOP 代码）
- **测试图**（P1-2）：8 色彩条（验证 RGB565 通道序）+ 圆/方/线几何（验证坐标系与旋转）+ 等宽文本（验证可读性与字形缓存）+ 四角标记（验证无裁切）。布局固定，供 `--check` 像素抽检锚定已知色块。
- **SCR 实现**（P1-3）：esp_console 注册命令；取帧策略——`bsp_display_lock()` 暂停 LVGL 刷新后，经 `esp_lcd_dpi_panel_get_frame_buffer_by_index()` memcpy 帧缓冲（BSP 双缓冲下取非扫描帧），解锁后经 CDC 原样输出（8-bit 干净通道，无 LF 转换）。1.8MB @ CDC 数秒内完成（§5.3 协议既有约定）。
- **键盘驱动**（P1-5）：落 `components/tab5_keyboard`；协议以 M5Tab5-UserDemo 出厂固件（同为 ESP-IDF）+ I2C 协议 PDF 为权威，拿不准看键盘 STM32 内部固件源码；INT=G50 电平/边沿语义以原理图核实。
- **音频自检**（P1-6）：`AUD TONE [freq]` 出正弦；44.1kHz 配置回读 + 440Hz 听感双重校验（48k/44.1k 错配会整体偏高约 8%，人耳可辨）。

### 阶段 2 — sloop_core 引擎移植
- 引入机制 D1 定稿后，适配层只实现 `hal/fm1_*.h` 中引擎实际触达的子集（以编译期缺符号清单为准，不预先全实现）。
- golden 容差通道：哈希不等时用内嵌参考向量计算 max|err|/SNR，阈值随基线入库；成因（libm ULP、FPU 语义）记录在案。
- 资产许可核查（P2-5）结论三选一：可再分发（入库）/ 不可（fetch-from-upstream 脚本）/ 部分可（分开处理）——**不可再分发则一律不入库**。

### 阶段 3 — 独立模式 MVP
- InputManager 是唯一输入汇点：触摸控件 / 键盘 / 注入事件 → 统一动作流 → 状态模型。70 键映射表（等价 FM-1 的 27 键 + 4 旋钮 + 功能键层面板）以独立文档维护（映射 ADR 配套）。
- MVP 四屏（PLAY/TRACKS/STEP/TRANSPORT）只做状态呈现与最小编辑，视觉规范（四轨蓝/绿/黄/橙）自此固化。
- 串口注入（`KEY <code>` / `TAP <x> <y>`）与 `SCR` 组合 = 脚本化 UI 回归，同步更新 AGENTS §5.3/§5.4。

### 阶段 4 — 功能对齐
- 功能对齐清单从上游手册（SLOOP.md/GUIDE.md）整理，逐项编 P4-x；全页面截屏套件（页面 × 关键状态矩阵）+ 长时 soak（外接电源）收口。**强制 dev→main 合并点**。

### 阶段 5 — 联动模式
- **FM-1 接入端口定为 USB-A Host 口**（PI4IO 0x44 E2.P3 `USB5V_EN` 使能 VBUS）：Type-C 必须保留为 USB-Serial-JTAG console——`SCR` 截屏、`KEY`/`TAP` 注入、日志与重烧全部走它，是阶段 5 全部证据的采集通道；Type-C 切 OTG host 会恰好拿走验证通道（对 AGENTS §4「Type-A 或 Type-C OTG」二选一的裁决）。
- `editor_proto` 与 `usb_fm1` 分层：协议组件不依赖 USB 实现（可对接 mock 对端跑单测）。
- 同步层移植 Sloop Go 语义：单在飞 FIFO + 瞬时 SET 合并 + WATCH/PING 续期 + push 分发 + 断连清队列/草稿。
- UAC1 采集的双时钟问题（FM-1 44.1/48k 由其固件决定）在 P5-5 以重采样或时钟主从方案解决，届时 ADR。
- **强制 dev→main 合并点**。

## 8. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| 浮点分叉（FP contraction / libm ULP）使 golden 哈希不可复现 | 高 | 双侧 `-ffp-contract=off` + 禁 `-ffast-math`；残余差异走容差通道（可计算、留痕） |
| 样本资产不可再分发 | 中 | P2-5 开工即核查；不可分发则 fetch 脚本 + 本地烧录，不入库 |
| 键盘 I2C 协议细节不明（寄存器/INT 语义） | 中 | UserDemo 出厂固件为权威参考 + STM32 内部固件源码兜底 |
| USB-MIDI host 驱动自研工作量超预期（DI-1） | 中 | ADR-002 先评估 tinyusb host 成熟度；fm1-cartridge 先例证明链路可行；只影响联动模式，独立模式无此风险 |
| 上游快速迭代导致基线漂移 | 中 | 固定 commit 引入；每阶段开工复核上游版本并更新 research 基线 |
| v1.3 样片与量产 v3.x 互斥（构建产物不可跨批次） | 低 | sdkconfig.defaults 已固化；量产批次切换时另建配置并重跑门禁 |
| 8MB app 分区不足（LVGL + 引擎 + 协议栈） | 低 | 16MB flash 内重划分区是纯本地操作；P2 起监控 app 体积 |

## 9. 留给实施者（Builder）的事项

1. D1/D2/D3/D4/D6/D7/D9 的阶段入口 ADR——按 §6 建议展开论证后定稿。
2. 上游源码的精读清单（research §5 ⚠️）：`core.h`、`main.c`、`audio.c`、`editor.c`、目标引擎源码——进入阶段 2 后第一步。
3. 各阶段的详细实施计划（bite-sized）：**进入该阶段时**按 `docs/superpowers/plans/` 的粒度另行制定（阶段 1 计划见 `2026-11-19-phase1-bringup.md`），不预写远期阶段的伪细节。
4. UI 组件边界（sloop_ui 拆分）与页面路由——P3-2 ADR。

## 10. 假设（待项目负责人确认；错误时影响标注）

| # | 假设 | 影响 |
|---|---|---|
| A1 | Tab5 Keyboard 已物理接在 Ext.Port1 且处于「普通」I2C 上报模式 | 若否：P1-5 降级为「探测失败登记 + 延后」，阶段 1 门禁 3 摘出 |
| A2 | FM-1 真机由用户侧刷好 SLOOP（版本经 INFO 回读，协议面以实测版本为准） | 若迟迟不可得：阶段 5 以 mock 对端推进开发、真机门禁保持开放 |
| A3 | microSD 卡与外接电源（长 soak 用）按需可得 | 仅影响 D4 回退路径与阶段 4 soak 安排 |
| A4 | 上游基线（2.8.5 / 协议 v12）在阶段开工时仍有效或差异已按 §0.5 复核 | 各阶段入口任务已内置复核 |
