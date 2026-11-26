# AGENTS.md — sloop-fm1-tab5 项目协作规范

> 本文件是本仓库所有 AI 代理与协作者的第一入口。**每次会话开始先读本文件**；项目演进时按「文档维护协议」同步更新本文件与 `docs/`。
> **本文件是索引**（保持 ≤32KB）：组件现状、脚本逐条说明、console/注入命令全义、门禁完整判据已拆到 `docs/dev/reference/`（firmware-map / scripts / console-and-injection / verification-gates）——改动涉及哪一块，先读对应参考文件；写回规则见 §6。

---

## 1. 项目目标

把 M-VAVE FM-1 的自制固件 **SLOOP**（isod89/sloop-fm1，GPL-3.0）移植到 **M5Stack Tab5**（ESP32-P4，ESP-IDF 开发），并接入 **Tab5 Keyboard**（70 键 I2C 键盘）。产品形态为**双模式**：

1. **独立模式（Standalone）**：Tab5 自身成为一台 SLOOP groovebox——移植 SLOOP 的 DSP 引擎、音序器与层式操作模型，经触摸屏（1280×720 MIPI-DSI）与 Tab5 Keyboard 实体键盘操作，音频经 ES8388 编解码从扬声器/耳机输出。
2. **联动模式（Companion）**：当刷有 SLOOP 固件的 FM-1 经 USB 接入 Tab5（Tab5 为 USB host）时，达到与 **sloop-fm1-go**（Android 伴生 App）相同的体验——**两台设备同步操作**：Tab5 上的编辑经 SLOOP 编辑器协议（USB-MIDI SysEx）即时下发 FM-1；FM-1 上的操作经 push 事件实时反映到 Tab5 UI；可遥控面板（LIVE）、可采集 FM-1 的 USB 音频（UAC1）。

两条模式共享同一 UI 与状态模型（设计原则：状态快照 + 编辑命令的抽象在本地引擎与远程 FM-1 之间归一）。

移植的**阶段划分、各阶段目标与完成判据**见 `docs/design/002-port-roadmap.md`（主计划，随进度维护状态列）。

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
│   ├── deferred-issues.md         # 延迟处理问题清单（活文档：条目解决即删，见 §6）
│   ├── design/                    # 架构决策与移植路线图（001-idf-version、002-port-roadmap、003-phase2-entry、004-upstream-import、005-ui-architecture、006-keyboard-mapping、007-audio-lifecycle、008-param-pages、009-storage-backend、010-sequencer-arranger、011-sound-design、012-feature-parity-checklist、013-usb-midi-host.md）
│   ├── superpowers/               # AI 协作工作流产物：specs/（移植设计规格）与 plans/（分阶段详细实施计划）
│   └── dev/                       #    开发日志/环境/设备档案（device-notes.md、verification-log.md、closeout-phase4.md）
│       └── reference/             #    AGENTS.md 拆出的全文参考（本文件只留索引）：firmware-map.md 组件现状 · scripts.md 脚本逐条 ·
│                                     console-and-injection.md 命令与注入 · verification-gates.md 门禁判据；门禁证据入 dev/verify/
├── firmware/                      # ESP-IDF 工程（v5.5.5，target esp32p4，骨架已初始化并真机验证）
│   ├── main/                      #    应用入口（display / console / keyboard / audio bring-up + sloop_state / sloop_ui 启动；详见 `docs/dev/reference/firmware-map.md`）
│   ├── components/                #    tab5_keyboard / sloop_core / sloop_hal_tab5 / sloop_state / sloop_ui / usb_fm1 / editor_proto（逐组件现状见 `docs/dev/reference/firmware-map.md`）
│   ├── sdkconfig.defaults         #    含本机批次适配（P4 v1.3 样片，见 docs/dev/device-notes.md）
│   ├── dependencies.lock          #    组件版本锁（入库，保证 BSP 屏幕批次探测等行为可复现）
│   └── partitions.csv             #    已落地（单 factory app 8MB + sloopstore 0x810000/0x68000 + FATFS storage 0x878000 + NVS，无 OTA A/B，回滚=重烧；ADR-009 §1）
├── scripts/                       # 构建/烧录/截屏验证脚本（用法见 §5）
│   ├── env.sh                     #    ESP-IDF 环境探测（被各脚本 source）
│   ├── build.sh                   #    构建（可 --clean）
│   ├── inject/                    #    十条注入场景 .sh + inject.py（场景逻辑都在 inject.py；逐脚本见 §5.1）
│   ├── {knob,state,seq,synth,editor-proto,usb-fm1}-test.sh / *-host-test.c   #    host 门禁与其用例源（§5.1）
│   ├── (其余脚本)                  #    build/flash/screenshot/verify、device-selftest.py、selftest-heap.py、desc-check.py、
│                                     soak.py、soak-selftest.py、screenshot_matrix.py、gen_*_fixtures.py、fixture_host_check.c、
│                                     store_raw.py、sample_upload.py——逐脚本摘要见 §5.1，全文见 reference/scripts.md
│   └── requirements.txt           #    Python 依赖（pyserial/Pillow/numpy 可选）
├── third_party/                   # 第三方源码与完整性清单
│   ├── sloop-fm1/                 #    vendored 上游 SLOOP 2.8.5（bac9c05）精简子树，只读
│   ├── sloop-fm1-go/              #    Sloop Go 参考副本（UI 源码 + screenshots，只读）
│   └── MANIFEST.sha256            #    全部 vendored 文件的 SHA256（在 third_party/ 下校验）
└── tools/                         # 其他辅助脚本
    ├── fetch_upstream.sh          #    上游固定 commit 抓取、许可文件保留与 MANIFEST 生成/校验
    ├── fetch_sloop_go.sh         #    Sloop Go 参考副本抓取（固定 commit；清单保留 sloop-fm1/ 行）
    └── move_regress_tables.py     #    构建后把 regress 簿记表（512 项 job/golden/CPU）从内部 RAM 移到 PSRAM
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

> 每个脚本一行摘要；**逐脚本的用途、参数、覆盖段与产物见 `docs/dev/reference/scripts.md`**（表格全文）。

| 脚本 | 用途（摘要） |
|---|---|
| `scripts/build.sh [--clean] [idf.py 参数…]` | 构建 `firmware/`（自动探测 ESP-IDF 环境；`--clean` 清理后全量构建） |
| `scripts/flash.sh [PORT] [--monitor]` | 烧录（端口自动探测，`/dev/ttyACM*` 优先；`--monitor` 烧录后进串口监视） |
| `scripts/screenshot.py PORT [OUT.png] [--retry N]` | `SCR` 取帧缓冲 → PNG（默认落 `build/verify/`；`--selftest` 离线自测；含复位/重试 helper 供场景复用） |
| `scripts/host-test.sh` | 上游 golden/DSP 回归门禁（强制刷新生成头；禁止基线更新） |
| `scripts/seq-test.sh` | host 单测：音序步/锁/pattern 工具 + PER3/DSU 字节层（persist 段）+ sections/chain/SONG（含 `persist-host-test.c`） |
| `scripts/state-test.sh` | 状态层 host 单测（gcc + stub 引擎，46 例） |
| `scripts/knob-test.sh` | `sloop_ui` 纯函数 host 单测（旋钮数学/格式化、`input_map` 键表、布局表、导航表、色表） |
| `scripts/synth-test.sh` | host 单测：DSYN 24 字段 / FM6 155B patch / 采样槽适配 |
| `scripts/device-selftest.py PORT [--case NAME] [--output LOG]` | 设备全量 `ENG SELFTEST` 182 例，与 golden 严格比对（自动 AUTO OFF / AUDIO OFF→ON） |
| `scripts/selftest-heap.py PORT [--record-only]` | 连续两次全量自检的内部堆泄漏门禁（原 DI-26 回归；`--record-only` 只记录） |
| `scripts/desc-check.py [PORT]` | 设备 `ENG DESC` 抽样对照上游描述符表（10 项 + 2 项 live + 越界拒绝） |
| `scripts/fm1-smoke.py PORT [--selftest]` | FM-1 联动协议冒烟门禁（操作员在环 7 步：接入 → INFO → PING → GET/SET 还原 → 拧旋钮推送 → 保活 → 拔插；`--selftest` 离线） |
| `scripts/inject/{transport,step_edit,tracks,nav,params,store,song,sequence,synth,sys}.sh [PORT]` | 十条注入场景（`UI`→`TAP`/`KEY`→`STATE`/`ENG` 断言→`SCR`；破坏性、自归一；**逐场景覆盖见参考**） |
| `scripts/inject/song.sh [PORT]` | SONG 编排页注入矩阵（破坏性、自归一） |
| `scripts/inject/sequence.sh [PORT]` | sequence 端到端矩阵：深度步/锁/录音/sections/chain/SONG/SONG REC（破坏性、自归一） |
| `scripts/inject/synth.sh [PORT]` | SYNTH 组矩阵：采样上传 / DSYN / FM6P（破坏性、自归一） |
| `scripts/inject/sys.sh [PORT]` | SYSTEM 设置页矩阵：settings/RAW 往返、持久化、SELFTEST chip（破坏性、自归一） |
| `scripts/soak.py PORT [--duration 1800] [--interval 300] [--cycle 12] [--save-window N] [--section-capture]` | 稳定性 soak（播放中循环编辑/切页/音符 + 周期采样与判定；`--save-window`、`--section-capture`） |
| `scripts/soak-selftest.py` | soak 自身的 host 单测（无设备） |
| `scripts/verify.sh [PORT]` | 一键：构建 → 烧录 → 等启动 → 截屏 |
| `scripts/gen_project_fixtures.py --out DIR [--host-check]` | 旧格式工程 fixture（FUN5/FUN4/FUN1）+ `expected.json`；`--host-check` 交叉校验 |
| `scripts/gen_settings_fixture.py --theme NAME [--out DIR] [--selftest]` | settings（PER3）/ side（THM2 主题）fixture；`--selftest` 离线校验 |
| `scripts/sample_upload.py [--port PORT] info\|erase SLOT\|load SLOT NAME file.wav[:ROOT[:LO-HI]]…` | `SMP` 采样槽上传（`--sine` fixture、`--selftest` 离线） |
| `scripts/store_raw.py PORT FILE [--obj project\|settings] [--slot 3] [--side FILE]` | `STORE RAW` 分块上传 payload（默认工程槽 3；可 `--obj settings` / `--side`） |
| `scripts/screenshot_matrix.py PORT [--out DIR] [--pages a,b,…] [--retry N]` / `--selftest`（另有**仅测试用**的隐藏参数 `--inject-drift`） | 全页面截屏矩阵 + 联系表（18 页 × 状态，锚点抽检，`LIVEHASH` 守卫；`--selftest` 离线） |

- Python 依赖（screenshot.py / inject.py / soak.py）：`pip install -r scripts/requirements.txt`（pyserial + Pillow；numpy 可选，加速 RGB565 解码）。
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
5. 失败令牌：固件侧任一步失败时改回 `SCR ERR <原因>\n`（如 `display_panel_not_ready`、`spiram_malloc_failed`、`lvgl_lock_timeout`）；PC 工具收到须**快速失败**（`scripts/screenshot.py` 即时报错退出，不等超时）

导出帧为**旋转后的横屏逻辑帧**（1280×720）：面板物理为竖扫 720×1280，固件发送前已按 `firmware/main/display.c` 的旋转几何旋出横屏帧——PC 侧拿到的即屏幕所见帧，无需再旋转。

固件实现要求：console 组件注册 `SCR` 命令；从显示控制器的活动帧缓冲取**完整稳定帧**（双缓冲下取非活动帧或暂停刷新），避免撕裂。**截屏前的页面选择用 console `UI <play|tracks|step|transport|global|fx|analog|fm6|sample|projects|presets|system>`**（同步切页，成功后会作废待执行的异步切页），`SCR` 不再需要页面参数；接入触摸/键盘注入见 §5.4.1。协议字段后续如扩展，须同步更新本节与 `scripts/screenshot.py`。

### 5.4 UI 视觉验证标准（涉及 UI 的改动必做）

1. 烧录后执行 `scripts/screenshot.py`（或 `verify.sh`），截图先落**抓取暂存区** `firmware/build/verify/screenshot-<时间戳>.png`（该目录不入库，`build.sh --clean` 会整目录删除）。**门禁判定所用证据须晋升入库**：复制到 `docs/dev/verify/<阶段>-<日期>/`，并在 `docs/dev/verification-log.md` 登记（阶段/门禁/日期/提交号/证据 SHA256/结论；策略详见 `docs/design/002-port-roadmap.md` §8）。
2. **必须用视觉能力读取 PNG 检查**（不是看日志推断）：
   - 无花屏、撕裂、错位、缺色、残留脏区；
   - 布局与该屏设计一致（如 TRACKS 屏四轨着色 蓝/绿/黄/橙，文本可读）；
   - 与改动预期状态一致（选中态、层级页面、弹窗等）。
3. 结论写入任务报告与 verification-log；**失败截图同样晋升入库**（文件名加 `-fail`）供对比修复。
4. 多页面/多状态验证：用 `UI <page>` 切页 + `TAP`/`KEY` 注入（§5.4.1），不必扩展 `SCR` 参数。

#### 5.4.1 注入约定（console → 屏幕状态 → 断言 → 截屏）

注入以「console 命令 → 与物理输入同层的动作函数 → 状态层 → 渲染」为准，脚本化验证一律走这条链路：

> 命令一览（每行一句）；**完整语义、回显格式、令牌与场景规则见 `docs/dev/reference/console-and-injection.md`**。

| 命令 | 语义（摘要） |
|---|---|
| `UI <play\|tracks\|step\|transport\|global\|fx\|sound\|sample\|dsyn\|fm6p\|projects\|presets\|song\|system\|mod\|scale\|voice\|tools>` | 同步切页（回显页 = 落地页）；无参打印当前页。页签两行 + 组作用域语义见参考 |
| `UI KNOB <slot> <press\|drag> <dx> <dy>` | 参数页旋钮的触摸路径注入（press / drag；环带外 `ignored (ring band)`、锁定页 `ignored (locked)`） |
| `TAP <x> <y>` | 触摸注入：屏幕绝对坐标，`y < 72` 一律 miss；命中后执行与控件回调**同一动作函数**（各页回显格式见参考） |
| `UI DSYN [KIT <0..3> \| LANE <0..15> \| PAGE <0..3>]` | DSYN 页选择器（KIT / LANE / PAGE） |
| `UI <参数页名> CHIP [<n>]` | 参数页 chip 框架的选择器 + 逐槽诊断 |
| `UI FM6P [OP <0..5> \| PAGE <0..5>]` | FM6P 页选择器（OP / PAGE） |
| `KEY <code> <down\|up>` / `KEY PANIC` / `KEY STATS` | 键盘注入（与物理键盘同入口）；`KEY PANIC` / `KEY STATS` |
| `UI TAB <group>` / `UI SUB <index>` | 手指导航注入（与物理组签/子签同一动作函数） |
| `UI MEM` / `UI FORCEFAIL <page> [on\|off]` | 页面注册表与池水位（含分组行）；`UI FORCEFAIL` 强制页面构建失败 |
| `UI LOCKHOLD <ms>` | 调试注入：验证 quiesce 失败路径 |
| `UI TOOL [<0..7>]` | TRANSPORT 页的键盘工具焦点 |
| `UI WINDOW [<start> <len>]` | 步窗（`step_window_t`，UI 局部态） |
| `UI LANE [<0..15>]` | STEP 页选中的鼓 lane（UI 局部态） |
| `PARAM <id> [<value>]` / `PARAM GLOBAL <id> [<value>]` / `PARAM BATCH <id> <value> …` / `PARAM RAW <id> <value>` | 参数读写诊断注入（经状态层；`PARAM` / `PARAM GLOBAL` / `BATCH` / `RAW`） |
| `SEQ DSTEP [step]` / `SEQ MICRO <track> <step> <v>` / `SEQ FILL <track> <step> <v>` / `SEQ LOCK LIST <track>` / `SEQ LOCK SET <track> <step> <param> <value>` / `SEQ LOCK DEL <track> <step> <param>` / `SEQ EDIT <op> …` / `SEQ PATTERN <op> …` / `SEQ REC [TOGGLE\|CONFIG q t c\|FILL HOLD h\|FILL NEXT\|BUTTON d]` / `SEQ HIT <lane> [velocity]` / `SEQ SECTION [<A-D\|0..3> STORE\|LOAD\|ERASE\|REQUEST]` / `SEQ CHAIN [<A-D\|0..3>…]` / `SEQ SONG [MODE song\|loop\|REC]` / `SEQ SREC [TOGGLE]` | 音序诊断：原始步字段 / 参数锁 / 编辑工具 / 录音 / 鼓 / section / chain / SONG |
| `STORE STATS` / `STORE PROJ LIST\|SAVE n\|LOAD n\|ERASE n\|HASH n\|FIELDS n` / `STORE PRESET LIST\|SAVE n\|LOAD n [track]\|ERASE n` / `STORE BANK LIST\|PUT n\|GET n\|LIVE\|ERASE n` / `STORE SONG [SET c l s:b…\|SAVE\|LOAD\|INFO]` / `STORE LIVEHASH` / `STORE RAW BEGIN\|hex\|END\|SIDE\|ABORT` / `STORE RAWTEST` / `STORE TEST` / `STORE FAULT <nth> [half]\|CLEAR` | 存储诊断：工程 / 预置 / FM6 银行 / 歌曲设置 / RAW / 故障注入 / torn-write 套件 |
| `SMP INFO` / `SMP BEGIN \| DATA \| HDR \| END \| ERASE` | 采样槽上传与诊断（`SMP`） |
| `DSYN INFO \| GET \| SET \| KIT \| CRUSH \| STORE \| AUDITION` | DSYN 鼓音色诊断 |
| `FM6P INFO \| GET \| SET \| ALG` | FM6 155B patch 诊断 |
| `STORE SETTINGS [INFO\|SET <field> <v>\|SAVE]` | settings 对象诊断（INFO / SET / SAVE） |
| `DISP BRIGHT [v]` / `AUD VOL [v]` / `AUD OUT [speaker\|headphone]` / `KBD RGB [off\|low\|mid\|high]` / `SYS NVS ERASE` / `SYS SELFTEST ARM` | 平台与声音设置诊断（亮度 / 音量 / 输出 / 键盘 RGB / NVS / SELFTEST ARM） |

- 命中矩形与控件布局**同源**：页内用同一组布局宏建控件、算 hit-test，禁止另抄一份坐标数字。
- 状态断言用 `STATE SNAP` / `ENG SNAP` / `ENG STEP` / `ENG PRESET`（逐字段格式见参考）；`STATE SNAP FULL` 追加步列明细，默认不打印。
- 场景逻辑集中在 `scripts/inject/inject.py`（pyserial）；脚本须可重复（必要时先归一基线），退出码 0 才算通过。
- **同拍语义（nav 场景）**：「同一 30ms UI tick」用一次性批写多行实现（`Session.cmd_batch`，行间无主机延迟；单条命令慢路径往返 ~50ms，逐条发送必然跨拍），并用批内追加的 `UI` 在设备侧自证前提——`KEY tab` 与 `KEY PANIC` 之间若被一拍插入，页面已前进，批内 `UI` 读数就会暴露；3 组 `KEY tab` 按下/释放（down+up 配对，防「重复按下沿去抖」假通过）同理：批内 `UI` 仍是起点页 ⇒ 按下之间无拍执行。前提不成立记为工具侧「重新对齐」（日志注记，不判固件失败），最多 5 次；只有前提成立后的页面断言才计入 PASS/FAIL。锚点方法（单按 tab 并快速轮询到页翻转）见 `inject.py` 的 `nav_anchor`。
- **transcript 必须是原始回显**：串口字节逐字节写日志，不裁剪、不手改、不摘述；只允许 `# [inject] ...` 前缀的工具注记（非固件输出），SCR 的二进制帧体不入日志。日志与截图落 `firmware/build/verify/`，门禁证据按 §5.4(1) 晋升。

### 5.5 验证门禁（任务完成的定义）

按任务类别满足对应门禁，全部通过才可提交：

> 每类一行；**各类别的完整判据（命令、覆盖项、证据口径、豁免）见 `docs/dev/reference/verification-gates.md`**。

| 类别 | 门禁（摘要） |
|---|---|
| 固件代码 | `scripts/build.sh` 退出码 0，且**不新增**编译警告/错误 |
| 固件代码（烧录验证） | esptool 输出 `Hash of data verified`；启动日志无 fatal/abort/backtrace |
| UI 相关 | §5.4 全流程：截屏成功 + 视觉检查通过 |
| host 单测（引擎 / 音序 / 状态层 / UI 纯函数 / DSYN·FM6·采样 / 工具自测） | `scripts/host-test.sh`、`seq-test.sh`、`state-test.sh`、`knob-test.sh`、`synth-test.sh` 与 `*.py --selftest` 退出码均 0；逐项判据见参考 |
| 设备自检与堆 | `python3 scripts/device-selftest.py` **182/182**；`python3 scripts/selftest-heap.py` PASS；`python3 scripts/desc-check.py` PASS |
| 注入脚本（`scripts/inject/`）/ TAP 相关固件改动 | 十条场景 + 截屏矩阵对真机各跑一遍（`sequence`/`synth`/`sys` 各复跑），退出码均 0；断言不放宽；逐场景判据见参考 |
| 阶段收口 soak | `python3 scripts/soak.py PORT --duration 1800 --interval 60 --save-window 900`（阶段收口 3h）退出码 0；判据与口径见参考 |
| 阶段收口 | 全矩阵 + 证据晋升 `docs/dev/verify/<阶段>-<日期>/` + `verification-log.md` 登记（门禁→命令→结论见各包 readme）；dev→main 不在阶段 4 范围（用户 2026-11-24 决定） |
| 联动协议 | `python3 scripts/fm1-smoke.py --selftest` 退出码 0；接有 FM-1 且操作员在场时 `python3 scripts/fm1-smoke.py PORT` 退出码 0（末行 `FM1 SMOKE PASS 7/7`；未跑列为 WAITING_OPERATOR，不等于通过） |
| scripts/ 工具改动 | `python3 scripts/screenshot.py --selftest` 退出码 0（解码/协议路径离线自测）；`python3 scripts/sample_upload.py --selftest` 退出码 0（采样 header/CRC/zone 与上传序列离线自测）；`python3 scripts/gen_settings_fixture.py --selftest` 退出码 0（PER3/THM2/FAVS fixture 与 CRC 离线自测）；`python3 scripts/screenshot_matrix.py --selftest` 退出码 0（截屏矩阵的离线自测：`MATRIX` 完整性、主题色派生、锚点判定、联系表拼装、CSV 往返） |

> **已知豁免**：kconfig 重新生成后的首个 configure pass 会出现一对 CMake 提示（`build.cmake:631` 警告 + `Missing kconfig option. Re-run the build process...`），紧接的第二次构建即消失——这是 IDF 已知行为，门禁按第二次构建的输出判定。

## 6. 文档维护协议（自动维护）

每次产生代码/结构变更的任务，**同任务内**完成对应文档更新，视为完成条件的一部分：

| 触发 | 必须更新 |
|---|---|
| 新建/删除/移动目录、模块、组件 | 本文件 §3 目录规划 |
| 新增接口/驱动/任务边界变化 | `docs/design/` 对应设计文档（含 ADR：背景/选项/决策/后果） |
| 组件行为/接口的现状说明（超过一行） | `docs/dev/reference/firmware-map.md` 对应小节；AGENTS.md §3 的 `components/` 行只留索引 |
| 新增/修改脚本、参数、场景覆盖段 | `docs/dev/reference/scripts.md` 对应行；AGENTS.md §5.1 只在「用途」变化时改摘要 |
| 新增 console 命令 / 改 TAP·KEY 回显 / 改场景规则 | `docs/dev/reference/console-and-injection.md`；AGENTS.md §5.4.1 只留一行摘要 |
| 新增门禁类别 / 改判据 | `docs/dev/reference/verification-gates.md`；AGENTS.md §5.5 只留一行摘要 |
| 上游 sloop-fm1 / sloop-fm1-go 版本跟进 | `docs/research/` 对应文档的「版本基线」与差异 |
| 编辑器协议理解修正（以原文为准） | `docs/research/editor-protocol.md` |
| 环境搭建踩坑、构建配置变更 | `docs/dev/` |
| 调研文档失实被发现 | 就地修正并注明来源 |
| 发现可延迟的已知缺口/风险（不阻塞当前任务但跨会话须记住） | `docs/deferred-issues.md` 新增条目（必填：背景/影响面/为何可延迟/解决方向/触发条件） |
| deferred 条目被解决 | **删除该条目**；若涉及方案选型，先沉淀 `docs/design/` ADR 再删 |

规则：
- 文档与代码同一提交组内提交（纯文档变更用 `docs` 类型独立提交）。
- 调研文档标注「整理日期」与「信息来源」，不确定的信息保留 ⚠️ 标记，不得删除。
- 本文件（AGENTS.md）是唯一的项目状态权威；任何会话结束前自查 §3 与实际目录一致。
- **AGENTS.md 是索引，保持 ≤32KB**：任何单项说明超过约 300 字节时写进 `docs/dev/reference/` 对应文件，本文件只留一行摘要 + 链接（2026-11-26 拆分；拆分前 211KB）。

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

**分支策略**（2026-11-19 首次开分支时确立）：

- `main`：稳定线，始终满足 §5.5 门禁（可构建、可烧录、自测通过）；只接受 `dev` 的周期性合并与紧急 hotfix。
- `dev`：日常开发集成分支；所有新功能在 dev 上推进。
- feature 分支：`feat/<组件或主题>`（如 `feat/usb-fm1`、`feat/display-bringup`），从 `dev` 拉出，完成后合回 `dev`；存活期短。
- `dev` 在里程碑（如 bring-up 完成、sloop_core 首版可听）时合回 `main`，合并前重跑 §5.5 门禁。

## 8. 工程约定（摘要）

- 许可合规：本项目固件衍生自 GPL-3.0 代码，整体 GPL-3.0；保留上游版权声明与子模块许可（Dexed msfa / DaisySP / Mutable 等）。
- 上游跟进：sloop_core 以固定上游 commit 引入，改动以补丁/适配层隔离，禁直接大改（便于 diff 跟随上游）。
- 协议实现以上游 `web/EDITOR_PROTOCOL.md` 为唯一权威；发现本文档系摘要有误时修文档不改协议理解。
- 硬件操作先查 `docs/`（I2C 地址、复位时序坑、屏幕批次差异）；屏幕驱动批次（ST7123/ST7121 vs ILI9881C+GT911）问题见 `docs/tab5-hardware.md` §6。
- 声音设计面（采样原始槽 / DSYN 22B / FM6 155B）的数据布局、钳位与持久化对象以上游为准，取舍见 ADR-011；**走带中拒绝采样上传**是记录在案的 Tab5 安全差异（上游允许），不得为「对齐上游」而放开。
