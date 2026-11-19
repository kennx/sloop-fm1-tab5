# AGENTS.md — sloop-fm1-tab5 项目协作规范

> 本文件是本仓库所有 AI 代理与协作者的第一入口。**每次会话开始先读本文件**；项目演进时按「文档维护协议」同步更新本文件与 `docs/`。

---

## 1. 项目目标

把 M-VAVE FM-1 的自制固件 **SLOOP**（isod89/sloop-fm1，GPL-3.0）移植到 **M5Stack Tab5**（ESP32-P4，ESP-IDF 开发），并接入 **Tab5 Keyboard**（70 键 I2C 键盘）。产品形态为**双模式**：

1. **独立模式（Standalone）**：Tab5 自身成为一台 SLOOP groovebox——移植 SLOOP 的 DSP 引擎、音序器与层式操作模型，经触摸屏（1280×720 MIPI-DSI）与 Tab5 Keyboard 实体键盘操作，音频经 ES8388 编解码从扬声器/耳机输出。
2. **联动模式（Companion）**：当刷有 SLOOP 固件的 FM-1 经 USB 接入 Tab5（Tab5 为 USB host）时，达到与 **sloop-fm1-go**（Android 伴生 App）相同的体验——**两台设备同步操作**：Tab5 上的编辑经 SLOOP 编辑器协议（USB-MIDI SysEx）即时下发 FM-1；FM-1 上的操作经 push 事件实时反映到 Tab5 UI；可遥控面板（LIVE）、可采集 FM-1 的 USB 音频（UAC1）。

两条模式共享同一 UI 与状态模型（设计原则：状态快照 + 编辑命令的抽象在本地引擎与远程 FM-1 之间归一）。

### 非目标（当前）

- 不做 FM-1 固件更新/刷机功能（明确禁止发送更新协议帧）。
- 不做 BLE MIDI（上游 SLOOP 从不开蓝牙，Tab5 也无经典蓝牙）。
- 不改动上游 sloop-fm1 的 FM-1 侧行为；联动完全基于公开的编辑器协议。

## 2. 上游与关键参考

| 对象 | 位置 | 本仓库参考文档 |
|---|---|---|
| SLOOP 固件（移植主体，GPL-3.0） | https://github.com/isod89/sloop-fm1 | `docs/research/sloop-fm1-firmware.md` |
| 编辑器协议（联动核心，权威=上游 `web/EDITOR_PROTOCOL.md`） | 同上仓库 | `docs/research/editor-protocol.md` |
| Sloop Go（联动行为蓝本） | https://github.com/jahlib/sloop-fm1-go | `docs/research/sloop-fm1-go.md` |
| M-VAVE FM-1 硬件 | — | `docs/research/m-wave-fm1-hardware.md` |
| Tab5 / Tab5 Keyboard 硬件 | M5Stack 官方 | `docs/tab5-hardware.md`、`docs/tab5-keyboard-hardware.md` |

上游版本基线（首次调研，2026-11-19）：SLOOP 2.8.5（bac9c05）、Sloop Go 1.8.0（ed941b5）、编辑器协议 v12。**上游迭代快，动工前先核对上游最新版本并更新调研文档。**

## 3. 目录规划

```
sloop-fm1-tab5/
├── AGENTS.md                  # 本文件：目标/规范/协议（随项目演进维护）
├── LICENSE                    # GPL-3.0 全文（与上游 sloop-fm1/sloop-fm1-go 同一 blob）
├── docs/
│   ├── tab5-hardware.md           # Tab5 硬件参考（已有）
│   ├── tab5-keyboard-hardware.md  # Tab5 Keyboard 参考（已有）
│   ├── research/                  # 上游调研（只读参考，随上游版本更新）
│   │   ├── m-wave-fm1-hardware.md
│   │   ├── sloop-fm1-firmware.md
│   │   ├── sloop-fm1-go.md
│   │   └── editor-protocol.md
│   ├── design/                    # 架构与设计决策（ADR；已有 001-idf-version.md）
│   └── dev/                       # 开发日志/环境/设备档案（已有 device-notes.md）
├── firmware/                      # ESP-IDF 工程（v5.5.5，target esp32p4，骨架已初始化并真机验证）
│   ├── main/                      #    应用入口（当前: bootstrap；待: 双模式调度）
│   ├── components/                #    【规划】sloop_core / sloop_hal_tab5 / editor_proto / usb_fm1
│   ├── sdkconfig.defaults         #    含本机批次适配（P4 v1.3 样片，见 docs/dev/device-notes.md）
│   ├── dependencies.lock          #    组件版本锁（入库，保证 BSP 屏幕批次探测等行为可复现）
│   └── partitions.csv             #    【规划】自定义分区（大 app + 资产 FATFS + 预留 OTA；LVGL/资产合入前必须落地——默认单 app 仅 1MB）
├── scripts/                       # 构建/烧录/截屏验证脚本（用法见 §5）
│   ├── env.sh                     #    ESP-IDF 环境探测（被各脚本 source）
│   ├── build.sh                   #    构建（可 --clean）
│   ├── flash.sh                   #    烧录（可 --monitor；端口自动探测）
│   ├── screenshot.py              #    串口请求帧缓冲 → PNG（协议见 §5.3）
│   ├── verify.sh                  #    一键验证：构建→烧录→等启动→截屏
│   └── requirements.txt           #    Python 依赖（pyserial/Pillow/numpy 可选）
└── tools/                         # 【规划】其他辅助脚本（抓取上游、生成、测试）
```

规划中的目录在建立时于本节同步更新；`【规划】`标记表示尚未落地。

## 4. 开发环境

- **ESP-IDF ≥ 5.4**（P4 支持），本机已安装；target `esp32p4`。
- Tab5 BSP：`espressif/m5stack_tab5`（MIPI-DSI/触摸/音频 ES8388/SD）；键盘、BMI270 等未封装外设自写驱动（I2C 地址与接线见 `docs/tab5-hardware.md` §4、`docs/tab5-keyboard-hardware.md`）。
- Tab5 Keyboard：独立 I2C 总线 G0(SDA)/G1(SCL)，地址 0x6D，INT=G50；参考 M5Tab5-UserDemo 出厂固件与 I2C 协议 PDF。
- USB host（联动模式）：Tab5 Type-A Host 口或 Type-C OTG；FM-1 身份 VID 0x1209:PID 0x0001，MIDI 端口名 "Felucca"。
- 构建/烧录/截屏验证统一走 `scripts/`（标准流程与门禁见 §5），不直接手敲 `idf.py`。
- 引擎正确性：复用上游 host 回归套件思路（golden 渲染哈希），Tab5 构建须与上游逐样本一致后才能动性能优化。

## 5. 构建、烧录与验证（标准流程）

所有构建/烧录/截屏操作统一走 `scripts/`，不手敲 `idf.py` 长命令；脚本失败时读其输出定位，不得绕过。

### 5.1 脚本总览与环境

| 脚本 | 用途 |
|---|---|
| `scripts/build.sh [--clean] [idf.py 参数…]` | 构建 `firmware/`；自动探测 ESP-IDF 环境（已在 PATH 则直接用，否则尝试 `$ESP_IDF_EXPORT`、`~/esp/esp-idf/export.sh` 等常见位置） |
| `scripts/flash.sh [PORT] [--monitor]` | 烧录；PORT 缺省自动探测（`/dev/ttyACM*` 优先，其次 `/dev/ttyUSB*`）；`--monitor` 烧录后进入串口监视 |
| `scripts/screenshot.py PORT [OUT.png]` | 经串口发送 `SCR` 命令取帧缓冲，存为 PNG（默认输出 `$SLOOP_TAB5_PROJECT/build/verify/`；`--selftest` 离线自测，无需设备） |
| `scripts/verify.sh [PORT]` | 一键验证：构建 → 烧录 → 等待启动 → 截屏到 `firmware/build/verify/` |

- Python 依赖（screenshot.py）：`pip install -r scripts/requirements.txt`（pyserial + Pillow；numpy 可选，加速 RGB565 解码）。
- `flash.sh` 接受 `--clean` 但**忽略**之（构建清理统一走 `build.sh --clean`）；`verify.sh` 不接受额外参数。
- 工程目录可用 `SLOOP_TAB5_PROJECT` 环境变量覆盖（默认 `<repo>/firmware`）；工程未初始化时脚本会明确报错退出。

### 5.2 标准流程（每次固件迭代）

```bash
scripts/build.sh                 # 1. 构建（改了 Kconfig/sdkconfig 后可 --clean）
scripts/flash.sh                 # 2. 烧录（Tab5 USB-C 直连，esptool 自动复位进入下载）
scripts/verify.sh                # 3. 或全流程一键：构建+烧录+截屏
scripts/flash.sh --monitor       #    需要看启动日志/运行日志时
```

### 5.3 截屏协议（固件侧必须实现的接口约定）

Tab5 无操作系统级截屏能力，约定由固件 console 提供帧缓冲导出命令（USB Serial/JTAG，CDC 速率下 1280×720 RGB565 约 1.8MB，数秒内完成）：

1. PC 发送行命令：`SCR\n`
2. 固件响应头：`SCR <width> <height> <fmt> <len>\n`（fmt ∈ `rgb565` | `rgb888`，len 为字节数）
3. 固件随后发送 **len 字节裸二进制**（帧缓冲原样：rgb565 为小端，rgb888 为 RGB 字节序）
4. 尾标记：`SCR OK\n`

固件实现要求：console 组件注册 `SCR` 命令；从显示控制器的活动帧缓冲取**完整稳定帧**（双缓冲下取非活动帧或暂停刷新），避免撕裂。协议字段后续如扩展（如 `SCR <page>` 指定页面），须同步更新本节与 `scripts/screenshot.py`。

### 5.4 UI 视觉验证标准（涉及 UI 的改动必做）

1. 烧录后执行 `scripts/screenshot.py`（或 `verify.sh`），截图统一存 `firmware/build/verify/screenshot-<时间戳>.png`。
2. **必须用视觉能力读取 PNG 检查**（不是看日志推断）：
   - 无花屏、撕裂、错位、缺色、残留脏区；
   - 布局与该屏设计一致（如 TRACKS 屏四轨着色 蓝/绿/黄/橙，文本可读）；
   - 与改动预期状态一致（选中态、层级页面、弹窗等）。
3. 结论写入任务报告；**失败的截图也保留**（文件名加 `-fail`）供对比修复。
4. 多页面/多状态验证：待 UI 页面路由落地后，扩展 `SCR <page>` 或串口按键注入参数化截屏（届时更新本节）。

### 5.5 验证门禁（任务完成的定义）

按任务类别满足对应门禁，全部通过才可提交：

| 类别 | 门禁 |
|---|---|
| 固件代码 | `scripts/build.sh` 退出码 0，且**不新增**编译警告/错误 |
| 固件代码（烧录验证） | esptool 输出 `Hash of data verified`；启动日志无 fatal/abort/backtrace |
| UI 相关 | §5.4 全流程：截屏成功 + 视觉检查通过 |
| 引擎/DSP 相关 | host 回归测试通过（上游 golden 套件搬运后生效，届时补 `scripts/host-test.sh`） |
| 联动协议 | 对真机 FM-1 的协议冒烟（连接 → INFO 版本协商 → WATCH 推送可达；测试脚本待落地） |
| scripts/ 工具改动 | `python3 scripts/screenshot.py --selftest` 退出码 0（解码/协议路径离线自测） |

> **已知豁免**：kconfig 重新生成后的首个 configure pass 会出现一对 CMake 提示（`build.cmake:631` 警告 + `Missing kconfig option. Re-run the build process...`），紧接的第二次构建即消失——这是 IDF 已知行为，门禁按第二次构建的输出判定。

## 6. 文档维护协议（自动维护）

每次产生代码/结构变更的任务，**同任务内**完成对应文档更新，视为完成条件的一部分：

| 触发 | 必须更新 |
|---|---|
| 新建/删除/移动目录、模块、组件 | 本文件 §3 目录规划 |
| 新增接口/驱动/任务边界变化 | `docs/design/` 对应设计文档（含 ADR：背景/选项/决策/后果） |
| 上游 sloop-fm1 / sloop-fm1-go 版本跟进 | `docs/research/` 对应文档的「版本基线」与差异 |
| 编辑器协议理解修正（以原文为准） | `docs/research/editor-protocol.md` |
| 环境搭建踩坑、构建配置变更 | `docs/dev/` |
| 调研文档失实被发现 | 就地修正并注明来源 |

规则：
- 文档与代码同一提交组内提交（纯文档变更用 `docs` 类型独立提交）。
- 调研文档标注「整理日期」与「信息来源」，不确定的信息保留 ⚠️ 标记，不得删除。
- 本文件（AGENTS.md）是唯一的项目状态权威；任何会话结束前自查 §3 与实际目录一致。

## 7. 提交协议

### 7.1 自动分组

- 提交前按**逻辑变更单元**拆分：不同 `<type>`、不同模块路径、互不依赖的改动**不得**混入同一提交。
- 代码与其直接依赖的文档/测试同组；无关的文档、格式化、配置改动独立成提交。
- 一次任务产出多个逻辑单元时依次分别提交；先提交被依赖方。
- 禁止「杂项」式提交；无法归类的改动说明原因后再决定归入或拆分。

### 7.2 提交格式

**Header**（一行）：`<type>(<模块路径>): <语义摘要>`

- `<type>` 严格限定为 `feat` / `fix` / `refactor` / `chore` / `docs` / `test`，**不允许任何变体**（no `feature`/`bugfix`/`style`/`perf`…）。
- `<模块路径>` 为仓库内受影响的主要模块路径（如 `components/editor_proto`、`docs/research`、`firmware/main`；跨模块取共同祖先）。
- `<语义摘要>` 用中文短语，祈使语气，不加句号。

**Body**：信息只能按以下标签分组，**禁止无结构段落**：

```
<Motivation>
[Change_Set]
[Impact_AST]
[Infra_State]
```

- `[Motivation]` — 触发条件或架构必要性
- `[Change_Set]` — 列出受影响的文件/资源及变更方式（新建/编辑/删除/移动）
- `[Impact_AST]` — 明确列出改动的接口、函数及受影响的依赖链
- `[Infra_State]` — 声明环境副作用（构建产物、依赖安装、分区/flash 布局、生成文件等；无则写「无」）

仅当 `<type>` 为 `fix` 时**必填**以下两组（其余类型不得出现）：

- `[Root_Cause]` — 缺陷根因
- `[Blast_Radius]` — 影响面（受波及的调用方、数据、设备状态）

### 7.3 示例

```
feat(components/editor_proto): 实现 v14/pack7 编解码与 INFO 版本协商

[Motivation]
联动模式需要与 FM-1 上的 SLOOP 固件建立编辑器协议会话，帧编解码是一切命令的前置。

[Change_Set]
- 新建 components/editor_proto/include/editor_proto.h（公共接口）
- 新建 components/editor_proto/src/frame.c（帧收发与校验）
- 新建 components/editor_proto/src/codec.c（v14/pack7/u35 编解码）

[Impact_AST]
- 新增 API：editor_proto_info()/editor_proto_get()/editor_codec_v14_{enc,dec}()
- 依赖链：usb_fm1（后续）→ editor_proto → 无外部依赖

[Infra_State]
无（纯新增组件，未接入构建目标）
```

```
fix(components/sloop_hal_tab5): 修正 ES8388 采样率配置导致爆音

[Root_Cause]
I2S MCLK 分频按 48kHz 计算，与 44.1kHz 固件假设不符。

[Blast_Radius]
独立模式全部引擎输出；联动模式不受影响（未走本地音频路径）。

[Motivation]
… [Change_Set] … [Impact_AST] … [Infra_State] …
```

### 7.4 其他 Git 规则

- 提交信息与代码同为交付物：格式不符的提交必须 `git commit --amend` 修正后再继续。
- 不提交构建产物、`build/`、本地环境文件；`.gitignore` 保持同步。
- 分支策略（feature 分支命名、合并方式）在首次开分支时补充到本节。

## 8. 工程约定（摘要）

- 许可合规：本项目固件衍生自 GPL-3.0 代码，整体 GPL-3.0；保留上游版权声明与子模块许可（Dexed msfa / DaisySP / Mutable 等）。
- 上游跟进：sloop_core 以固定上游 commit 引入，改动以补丁/适配层隔离，禁直接大改（便于 diff 跟随上游）。
- 协议实现以上游 `web/EDITOR_PROTOCOL.md` 为唯一权威；发现本文档系摘要有误时修文档不改协议理解。
- 硬件操作先查 `docs/`（I2C 地址、复位时序坑、屏幕批次差异）；屏幕驱动批次（ST7123/ST7121 vs ILI9881C+GT911）问题见 `docs/tab5-hardware.md` §6。
