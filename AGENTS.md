# AGENTS.md — sloop-fm1-tab5 项目协作规范

> 本文件是本仓库所有 AI 代理与协作者的第一入口。**每次会话开始先读本文件**；项目演进时按「文档维护协议」同步更新本文件与 `docs/`。

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
│   ├── design/                    # 架构决策与移植路线图（001-idf-version、002-port-roadmap、003-phase2-entry、004-upstream-import、005-ui-architecture、006-keyboard-mapping、007-audio-lifecycle、008-param-pages、009-storage-backend）
│   ├── superpowers/               # AI 协作工作流产物：specs/（移植设计规格）与 plans/（分阶段详细实施计划）
│   └── dev/                       # 开发日志/环境/设备档案（device-notes.md、verification-log.md；门禁证据入 verify/）
├── firmware/                      # ESP-IDF 工程（v5.5.5，target esp32p4，骨架已初始化并真机验证）
│   ├── main/                      #    应用入口（显示/console/键盘/音频 bring-up + 开机启流 + sloop_state 初始化 + sloop_ui 启动（失败回退测试图）；待: 双模式调度）
│   ├── components/                #    tab5_keyboard（键盘 I2C 驱动，已落地）；sloop_core / sloop_hal_tab5（引擎、同源 golden 自测、RAM 存储与 ES8388 音频流已落地；`sloop_core_read_window` 按步窗基取 16 列快照；`sloop_core_desc` 投影上游参数描述符（`ENG DESC` 抽样，`scripts/desc-check.py`）；`sloop_core_set_params` 一次锁批量写参（与 desc 同源钳位）+ `sloop_core_read_params` 参数快照）；**换引擎/预置换声音**（P4-B Task 5：上游 `ui.c` 的 `apply_preset_to/set_engine_of/TRK_DEF/param_kept/arp_param/arp_is_sounds` 原语义移植进 `store_tab5.c`，`sloop_core_track` 改为「引擎变 → 默认值 + 预置 0，预置变 → 整份声音写 p[]」，不再只写索引；公共入口 `sloop_core_apply_preset/set_engine/preset_preview`，纯写值抽成 `preset_values` 供应用与预览共用；`ENG PRESET` 打印 71 项期望向量、`ENG SNAP` 的 `voices=` 是音符释放的断言口；`project_load` 补上游 `preset==0xFF` 默认声音 + `proj_foreign_parts`）；`sloop_store`（P4-B：上游 `storage.c` 的 A/B + CRC + 回读校验、`project.c` 的格式层与 `upreset.c` 的用户预置原样编入同一 TU（`src/store_tab5.c` 由 `selftest.c` 末尾 include）：工程 A–D 的「RAM 槽（PSRAM 4×3,816B）+ flash」双层存读/擦、开机四槽 fetch，`proj_slot/proj_v3_tmp/proj_tmp` 经 include 前宏重定向进 PSRAM、`main_scratch`（3,840B）留内部 SRAM，指针/锁/shim 的 `flash_ok` 放 `.noinit`（自检 restore 不回滚）；**用户预置 32 槽**（`#define FELUCCA_FLASH 1` 包住 `upreset.c` 的 include，否则 `up_bank_find` 读不到 XIP 窗口）：`sloop_store_preset_{count,used,name,engine,store,load,erase}`，SAVE = 引擎锁内捕获 → 锁外 `up_put` 擦写（擦写期间绝不持引擎锁），LOAD = 引擎锁内 `up_load_to`（任意合成轨，缺省选中轨），`up_boot` 在 init 与每个预置入口重扫银行索引（`up_bank_xip` 是 vendored `.bss`，落在 SELFTEST 快照窗口内），ADR-009 §3/§8；**自动存档/上电恢复**：上游 `autosave_tick/autosave_resume` 按原语义移植——停走带 + 无输入 2.5s + 距上次写 20s + 无发声（`audio_quiet`）+ hash 变化才写 `OBJ_AUTOSAVE`（捕获/擦写分离：引擎锁内捕获、锁外擦写；拿不到存储锁跳过本拍），开机在工厂态快照与四槽 fetch 之后 `st_load`→`proj_import`→`proj_apply`→`sel` 钳位→`proj_foreign_parts`→`engine=eng_req`，并记住 hash（第一拍不重写）；1s 周期任务（栈 3072/优先级 1，`sloop_store_autosave_start()` 在 **resume 之后**才创建）；活动戳由键盘/TAP/切页/焦点变更/`state_apply` 打点；`STORE AUTO ON|OFF` + `STORE STATS` 的 autosave 三项是诊断面）；sloop_state（AppState/EditCmd 状态层 + `step_window_t` 步窗（`CMD_SET_WINDOW`/`step_window()`，窗口列投影，引擎步号绝对）+ 参数命令与投影（`CMD_SET_PARAM`/`CMD_SET_PARAM_BATCH`，`app_state_t.p[]/g[]`），纯 C，host 单测）；sloop_ui（LVGL 页面框架 + 页面注册表/懒建（`ui_page_t`，PLAY 启动即建、其余首次访问建，每页 est 预算与失败可见，ADR-008），**9 页**：PLAY/TRANSPORT 只读呈现与走带、TRACKS 卡选轨、STEP 4×步窗（1..16 列，bank1..3 可编辑，绝对步号列/播放头映射）编辑闭环；参数页 GLOBAL/FX/ANALOG/FM6/SAMPLE（`page_layout.c` 策展布局表 8 槽/页（SAMPLE 空槽 5/7 不建） + `page_params.c` 通用页 + `knob.c` 单对象自绘旋钮：弧线/两行文本/按下=焦点+锚点、拖动=相对角度差粗调（55..95px 环带）/± 细调/键盘 ←→↑↓；纯数学与格式化在 `knob_math.c`（host 单测 `scripts/knob-test.sh`）、触摸接线由 `UI KNOB` 注入验证，ADR-008 §8）；InputManager 70 键映射/层模型/`KEY` 注入见 ADR-006；`TAP` hit-test 注入见 ADR-005；`UI MEM`/`UI FORCEFAIL`/`UI WINDOW`/`UI LOCKHOLD`/`PARAM` 见 §5.4.1）；【规划】editor_proto / usb_fm1
│   ├── sdkconfig.defaults         #    含本机批次适配（P4 v1.3 样片，见 docs/dev/device-notes.md）
│   ├── dependencies.lock          #    组件版本锁（入库，保证 BSP 屏幕批次探测等行为可复现）
│   └── partitions.csv             #    已落地（单 factory app 8MB + FATFS storage 0x7F0000 + NVS，无 OTA A/B，回滚=重烧；见 docs/design/002 §3 P1-1）
├── scripts/                       # 构建/烧录/截屏验证脚本（用法见 §5）
│   ├── env.sh                     #    ESP-IDF 环境探测（被各脚本 source）
│   ├── build.sh                   #    构建（可 --clean）
│   ├── flash.sh                   #    烧录（可 --monitor；端口自动探测）
│   ├── screenshot.py              #    串口请求帧缓冲 → PNG（协议见 §5.3）
│   ├── host-test.sh               #    上游 host 生成头 + golden/DSP 回归门禁
│   ├── device-selftest.py         #    串口 ENG SELFTEST（默认全量 182 例）→ PC 对 golden.txt 严格比对
│   ├── verify.sh                  #    一键验证：构建→烧录→等启动→截屏
│   ├── inject/                    #    注入脚本（transport.sh/step_edit.sh/tracks.sh/nav.sh/params.sh + inject.py）：UI 切页→TAP/KEY→STATE/ENG 断言→SCR 截屏（§5.4.1）
│   ├── desc-check.py              #    `ENG DESC` 描述符抽样对照 params.c（10 项逐字段 + 4 条越界拒绝）
│   ├── soak.py                    #    稳定性能 soak：播放/步编辑/切页循环 + 每 5min ENG MEM/PERF 采样与判定（§5.5）
│   └── requirements.txt           #    Python 依赖（pyserial/Pillow/numpy 可选）
├── third_party/                   # 第三方源码与完整性清单
│   ├── sloop-fm1/                 #    vendored 上游 SLOOP 2.8.5（bac9c05）精简子树，只读
│   └── MANIFEST.sha256            #    全部 vendored 文件的 SHA256（在 third_party/ 下校验）
└── tools/                         # 其他辅助脚本
    ├── fetch_upstream.sh          #    上游固定 commit 抓取、许可文件保留与 MANIFEST 生成/校验
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

| 脚本 | 用途 |
|---|---|
| `scripts/build.sh [--clean] [idf.py 参数…]` | 构建 `firmware/`；自动探测 ESP-IDF 环境（已在 PATH 则直接用，否则尝试 `$ESP_IDF_EXPORT`、`~/esp/esp-idf/export.sh` 等常见位置） |
| `scripts/flash.sh [PORT] [--monitor]` | 烧录；PORT 缺省自动探测（`/dev/ttyACM*` 优先，其次 `/dev/ttyUSB*`）；`--monitor` 烧录后进入串口监视 |
| `scripts/screenshot.py PORT [OUT.png] [--retry N]` | 经串口发送 `SCR` 命令取帧缓冲，存为 PNG（默认输出 `$SLOOP_TAB5_PROJECT/build/verify/`；`--selftest` 离线自测，无需设备）。复位动作拆成 `esptool_reset()`（只做 `flash_id --after hard_reset`、**不等提示符**，供注入场景「复位后立刻发命令」的早探复用，见 `store.sh` ⑦）与 `reset_device()`（= esptool_reset + `wait_boot`）；传输失败（超时/短帧/坏尾/长度不符）时经 esptool 硬复位设备后重试，默认 `--retry 3`（`0`=单次尝试）；耗尽后非零退出并落失败证据（`<out>-fail.bin`，零字节时 `<out>-fail.txt`）——传输失败自动复位重试，见 `docs/dev/device-notes.md` §8.1 |
| `scripts/host-test.sh` | 强制刷新 4 个生成头并运行上游 host golden/DSP 回归子集；禁止基线更新模式 |
| `scripts/state-test.sh` | `sloop_state` 状态层 host 单测（gcc + stub 引擎，`-DHOST_TEST`）；打印 PASS/FAIL，失败退出码非 0 |
| `scripts/knob-test.sh` | `sloop_ui` 旋钮纯函数 host 单测（gcc，无 LVGL）：弧线↔值往返、环带（55..95px）、角度差归一化、相对拖拽换算/钳位、格式化表（上游生成表口径）；依赖 `third_party/sloop-fm1/build/gen/felucca_tables.h`（先跑 `host-test.sh`）；打印 PASS/FAIL，失败退出码非 0 |
| `scripts/device-selftest.py PORT [--case NAME] [--output LOG]` | 运行设备 `ENG SELFTEST`，逐用例与 host `golden.txt` 比对。**默认全量 182 例**（诊断期固件自动 quiesce UI：暂停 30ms 定时器、输入丢弃、引擎读写冻结，跑完恢复——UI live 开机即可跑全量门禁）；`--case NAME` 走旧单例口径。脚本先自动 `ENG AUDIO OFF`（固件开机即流式，否则自检被拒）、结束后恢复 `ENG AUDIO ON`；也支持 `--compare LOG` 离线复核（默认按全量 182 例校验，旧单例日志需配 `--case NAME`） |
| `scripts/desc-check.py [PORT]` | 设备 `ENG DESC <scope> <engine> <id>` 抽样：10 项逐字段对照 `params.c` 硬编码期望表（F_DB/F_TIME/F_BIPCT/F_ENUM/F_BPM、鼓轨 kit 哨兵、ANALOG·FM6·SAMPLE edit[]，含 unit 与 F_INT+names 表）+ 4 条越界拒绝；原始回显存 `firmware/build/verify/desc-<时间戳>.log`，退出码 0 = 全过 |
| `scripts/inject/{transport,step_edit,tracks,nav,params,store}.sh [PORT]` | 注入场景脚本（pyserial）：`UI` 切页（步窗场景另用 `UI WINDOW`）→ `TAP`/`KEY` 注入 → `STATE`/`ENG` 断言 → `SCR` 截屏；产物 `firmware/build/verify/inject-<场景>-<时间戳>.{log,png}`（原始回显日志 + 截图）。`step_edit` 覆盖窗口列编辑 + bank1 绝对步号（`ENG STEP 0 16`）+ `window_len=8` 隐藏列/命中 + 播放头窗口映射；STEP 坐标由页面布局宏派生（`step_cell_xy`），列语义对齐 `STATE SNAP` 的窗口。`nav` 覆盖四条导航行为：页签缝 TAP miss（状态/引擎不变）、STEP 鼓轨行只读（焦点不变、无引擎写、鼓格像素不变）、同拍 `KEY tab`+`KEY PANIC`（撤销待切页、无回跳）、同拍 3×`KEY tab`（只前进一页，ADR-006）；「同拍」用一次性批写多行 + 批内 `UI` 自证，见 §5.4.1。`params` 覆盖五个参数页：TAP `−`/`+` 与键盘 ←→↑↓ 编辑（`PARAM`/`PARAM GLOBAL`/`STATE SNAP` 断言）、`UI KNOB` 触摸路径、切页保值、`UI MEM` 九页池水位（每页 delta ≤ est）、五页截屏 + 像素抽检（旋钮环/数值文本/页签）；FM6/SAMPLE 页另用 `ENG TRACK` 切引擎 + `ENG DESC`（ALG 33 名 / SET 12 名）并在锁定态断言 `ignored (locked)`；坐标由 `param_knob_xy()` 按 cell/旋钮盒宏派生。`store` 覆盖工程 A–D 双层往返（`STORE PROJ …`/`STORE LIVEHASH`）：四槽 ERASE 归一 → BPM 100 捕获 h0 → SAVE 0 → HASH 0==h0 → BPM 120 ⇒ h1≠h0 → LOAD 0 ⇒ LIVEHASH==h0 且 `STATE SNAP bpm` 回到 100 → ERASE 0 ⇒ LIST EMPTY 且 LIVEHASH 不变 → 再 SAVE（h2==h0）→ esptool 硬复位（不写 flash）⇒ **早探**（`esptool_reset` 不等提示符，见到 `sloop>` 立即发 `STORE PROJ LOAD 0`，必须回 `STORE ERR no_storage`——`.noinit` 哨兵跨热复位存活而 HAL ready 在 .bss，故 init 之前必须拒绝且绝不挂死）→ LIST USED、**HASH 0==h2**（HASH 只读 RAM 槽 ⇒ 开机 fetch 的证据）、LOAD 0 ⇒ LIVEHASH==h2 → 单例 `ENG SELFTEST` 之后 HASH 0 仍==h2（`.noinit` 指针不被自检 restore 回滚）；**用户预置**（`STORE PRESET …`）：归一基线（选轨 0 + `ENG TRACK 0 0 0` + `PARAM BATCH` 写 8 个已知参数）→ ERASE 0/1 → SAVE 0（默认名 `ANALOG 01`）→ LIST `PRESET 0 USED` + `PRESET COUNT used=1 total=32` → 改值 → LOAD 0 0 ⇒ 8 参数逐项回到保存时 → LOAD 0（缺省选中轨）→ 换引擎（`ENG TRACK 0 4 0`）后 LOAD ⇒ 引擎回到预置里的 0 → **跨轨** `LOAD 0 1` ⇒ 选中轨 p[] 不变、轨 1 取到预置 → **银行 1 槽 16** 往返（ERASE 16 不碰槽 0）→ ERASE 0 ⇒ EMPTY → 鼓轨（TAP 卡 3）SAVE 1 / LOAD 0 3 均回 `STORE ERR drum` 且出 `DRUM TRACK: NO SOUND`（上游 `up_ui` 顺序：鼓轨先于播放守卫） → 单例 `ENG SELFTEST` 后 LIST/LOAD 仍可用 → 第二次 selftest 后**紧跟** SAVE 1（不先读预置）⇒ 槽 0 仍 USED（写路径重扫）→ 硬复位后 LIST 仍 USED 且 LOAD 还原 → **播放中** SAVE/ERASE 回 `STORE ERR playing`、播放中鼓轨 SAVE 仍回 `drum`；场景开头 esptool 硬复位（Task 6 起复位还会恢复上一轮的 autosave，故「起点」= 上一轮 ⑪ 结束时的状态，从第 2 轮起逐位相同）并**显式归一到轨 0**（选中轨是工程的一部分、随 autosave 恢复），h2==h0；`STATE SNAP bpm` 在 LOAD 之后轮询（≤1s），不假定一拍已过；`ENG AUDIO ON` + 停止归一（走带请求只由渲染循环消费，停流时上一轮残留请求会让写 API 回 playing）。场景最先跑**换音**用例（旧固件没有 `ENG PRESET` ⇒ RED 快速失败）：`ENG PRESET 0 2 1` 取期望向量 → `ENG TRACK 0 2 1` ⇒ `PARAM` 逐项回读 **71/71** 与期望一致且 P_LEVEL/P_SLEN（param_kept）保留 → `ENG TRACK 0 1 0` 换引擎 ⇒ 71/71 与「引擎 1 预置 0」一致（P_E0..P_E7 是预置写值，不是 edit[] def）+ **独立上游表 oracle**（R17：DIGITAL RHODES / ANALOG 808 BOOM 的 e[]/env/fenv/mono/FX/XP 与 `PRESET_TRIM` 硬编码自 `eng_digital.c:117`、`eng_analog.c:128`、`preset_trim.h`，刻意不经过 `preset_values`/`ENG PRESET`，否则写值实现内部的错会在「预览 vs 应用」两边同时复制）→ 按住 `KEY 0x42`（`ENG SNAP voices≥1`）时换预置 ⇒ voices 归 0（panic 路径）且 `STATE SNAP playing` 不变 → `STORE PROJ SYNTH fmt1|foreign`（R18：`project_load` 的两条 fallback——格式 1 的 `preset==0xFF` 轨取默认声音 + 步清空、未知引擎（≥ NENGINES）走 `proj_foreign_parts` 且图案保留；**破坏诊断槽 3**，槽 0–2 不碰，收口 ERASE 3）；收口还原 P_LEVEL/P_SLEN；**自动存档**（Task 6，场景最后一段；`STORE AUTO OFF` 在 ⓪ 与**每次硬复位之后**重新下发——它不跨复位）：归一（选轨 0/停止/窗口 0..16/轨 0 绝对步 5 有音，幂等 TAP）→ `STORE AUTO ON` → 改 BPM ⇒ 1.0s 不写（idle 2.5s 未到）→ 轮询到首次写（距 boot 不足 20s 时 boot gap 也参与）且 `autosave_hash == LIVEHASH` → **紧跟上一次写**再改值 4s 不写（若忽略 gap，idle 一到 2.5s+一拍就会写 ⇒ 4s 的读数能分辨）、实测「改值→写」≥6s → 状态不变等 25s（>gap+idle，只有 hash 门能拦）不写 → **PLAY 中改 BPM**（`proj_capture` 真捕获的值）+ 断言 `autosave_hash != LIVEHASH`（hash 门不成立）⇒ 10s+3s 两读都不写（idle/gap 都满足，只有 `song.playing` 能拦；顺带记一次 `ENG SNAP voices` 说明发声门此刻是否也成立）→ 按住 C（`voices≥1`）改值、**先睡过 idle（3.5s）** 再观察 3s 不写（整个窗口 idle/gap 都满足，只有 `audio_quiet` 能拦）、松开后 2.5–6s 写（纯 idle）→ `STORE AUTO OFF` 后改值（BPM 140）、等过 26s 的 gap 再观察 5s 不写，然后 ON ⇒ 开闸即写（≤5s，写的就是 OFF 期间的现状，`autosave_hash==LIVEHASH`）——开关在拦、不是 gap → esptool 硬复位 ⇒ `LIVEHASH`==该 hash、`STATE SNAP bpm=140`、`ENG STEP 0 5` 逐字段相同、`autosave_writes=0`，再等过 boot 的 20s gap 计数仍 0（resume 记住 hash，第一拍不重写）。截屏传输失败时与 `screenshot.py` 同源复位重试（复位 → 重开串口 → 场景从头重跑，默认 3 次）。场景逻辑在 `scripts/inject/inject.py`，见 §5.4.1 |
| `scripts/soak.py PORT [--duration 1800] [--interval 300] [--cycle 12]` | 稳定性 soak：先访问全部 9 页（九页全建后再采样），保持 TRANSPORT 页播放中，循环「步编辑 → 选轨/切页 → 键盘音符」，每 `--interval` 秒采样 `ENG MEM`/`ENG PERF`/`KEY STATS`/`UI MEM`（`pool_free` 入采样表）并核验切页确定性；产物 `firmware/build/verify/soak-<时间戳>.log`（原始回显）+ `soak-summary-<时间戳>.txt`（采样表 + 判定）。判定：internal_free 与 internal_min ≥60KB、采样首末差 ≥-8KB、连续 3 个采样非增累计下滑 ≤2KB、streaming=1 且 xrun=0、流式占空比 duty_mean ≤ 2× 阶段 2 基线 9.44%（=18.88%）、音频栈余量 ≥2KB、KEY 事件计数增量 == 期望、日志无 panic/WDT、实测时长 ≥--duration；退出码 0 = 通过；**复位后不要立刻起跑**：t=1s 的首个采样含启动瞬态（render_blocks 仅数百、duty_mean 偏高，实测 21.07% 会被判失败），等流稳定 ~30s 再跑（P4-B Task 6 现场）|
| `scripts/verify.sh [PORT]` | 一键验证：构建 → 烧录 → 等待启动 → 截屏到 `firmware/build/verify/` |

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

固件实现要求：console 组件注册 `SCR` 命令；从显示控制器的活动帧缓冲取**完整稳定帧**（双缓冲下取非活动帧或暂停刷新），避免撕裂。**截屏前的页面选择用 console `UI <play|tracks|step|transport|global|fx|analog|fm6|sample>`**（同步切页，成功后会作废待执行的异步切页），`SCR` 不再需要页面参数；接入触摸/键盘注入见 §5.4.1。协议字段后续如扩展，须同步更新本节与 `scripts/screenshot.py`。

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

| 命令 | 语义 |
|---|---|
| `UI <play\|tracks\|step\|transport\|global\|fx\|analog\|fm6\|sample>` | **同步**切页（内部取 LVGL 锁；成功后作废待执行的异步切页邮箱，回显页即落地页）；无参打印当前页。9 页时页签收窄为 107px（注册顺序 = 页签顺序；右侧留给标题/层提示 220px）。`UI QUIESCE <on\|off>` 手动复验诊断期静默（暂停/恢复 30ms UI 定时器、LVGL 定时器、触摸 indev 与输入派发），与 `ENG SELFTEST` 内部同入口。注意手动 `on` 不是粘滞的：下一次 `ENG SELFTEST` 会自行 on→off 并在结束时恢复（等价于一次 `off`），自检结束后需重新手动 `on` 才能复验拒绝路径。**失败令牌**：取 LVGL 锁超时回 `UI QUIESCE <on\|off> ERR lock_timeout quiesced=<0\|1>` 并返回非零（on 失败 = 输入静默已回滚、UI 仍在跑；off 失败 = 保持静默态，可重试）；`ENG SELFTEST` 在 on 失败时回 `cases=0 result=FAIL reason=quiesce` 且**不冻结引擎** |
| `UI KNOB <slot> <press\|drag> <dx> <dy>` | 参数页旋钮的**触摸路径**调试注入（ADR-008 §8.2）：用相对旋钮中心（±1000px）的合成坐标走与物理触摸**同一**处理路径——`press` = 设焦点 + 记录锚点（不改值）；`drag` = 相对锚点的角度差 → 值差（270° = 整个值域，环带 55..95px）。回显 `UI KNOB page=P slot=n <op> dx=.. dy=.. value=V`；环带外/无锚点回 `… ignored (ring band)`、引擎页不适用（选中轨引擎与页声明不符/鼓轨，旋钮显示 `--`）回 `… ignored (locked)`，两者都返回非零且值不变（令牌分开：前者是命中问题，后者是页面锁定）；非参数页 `UI ERR not_params`、页未建 `UI ERR page_build <id>`、静默期 `UI ERR quiesced`、锁超时 `UI ERR lock_timeout`。用于无手指环境验证触摸回调接线，物理手感仍需人测 |
| `TAP <x> <y>` | 触摸注入，坐标为**屏幕绝对坐标**（含 72px 页签条，与触摸 indev 同口径）。按当前页 hit-test 表命中矩形后执行与控件回调**相同的动作函数**——等价性在 EditCmd 层（同一 `state_apply` 命令序列），**不声称**原始指针事件等价（见 ADR-005）。未命中打印 `TAP miss x,y page=P` 并返回非零（不改状态；UI 未启动时打印 `page=n/a`）；LVGL 锁超时打印 `TAP ERR lvgl_lock_timeout`、诊断期静默打印 `TAP ERR quiesced`（均**未派发**，与 miss 区分）。`UI` 的锁超时打印 `UI ERR lock_timeout`、目标页构建失败打印 `UI ERR page_build <id>`、页 id 合法但未注册打印 `UI ERR not_registered <id>`（三者与 miss/not_started 分开）。参数页命中：旋钮 → `PARAM slot=<n> focus`（只设焦点，注入不能拖拽）；`−`/`+` → `PARAM slot=<n> id=<参数 id> delta=±1 value=<写后回读值>`；引擎页不可编辑 → `PARAM slot=<n> locked`（均与 miss 区分） |
| `KEY <code> <down\|up>` / `KEY PANIC` / `KEY STATS` | 键盘注入，与物理键盘同入口（ADR-006）；诊断期静默时 `KEY <code>` 打印 `KEY ERR quiesced`（丢弃并计数，`KEY STATS` 的 `quiesce_dropped` 可回读）。**`KEY PANIC` 是刻意的例外**：它是安全停走带命令（释放已按下音符、清层键/待切页），诊断期仍执行（不受 quiesce 门控），便于脚本在自检/异常后收口；`KEY STATS` 始终可读 |
| `UI MEM` / `UI FORCEFAIL <page> [on\|off]` | 页面注册表与懒建的池水位（ADR-008）：`UI MEM` 打印池 used/free/total、注册/已建/失败页数、`sum_est`/`reserve` 与每页 `state`/`est`/`delta`（delta = 最近一次成功构建前后池 free 减少量，含块头开销，采样于首帧 update 前）；`UI FORCEFAIL` 强制该页构建失败：未建页挂起到下一次构建（回显 `state=unbuilt`），已建**当前页**删除并立即重建到失败态（注入当场消费，回显 `state=failed`），已建**非当前页**只删除控件回到未建态（回显 `state=unbuilt`，注入保持待命、下次访问时构建失败），用于验证「错误状态条 + ERROR 日志 + `UI ERR page_build <id>`」路径；`off` 只撤销尚未消费的注入，不重建已失败页（失败页在下次访问时重试） |
| `UI LOCKHOLD <ms>` | 调试注入（验证 quiesce 失败路径）：辅助任务持 LVGL 锁 `ms`（1100..30000）毫秒，命令等持锁确认后回 `UI LOCKHOLD ms=<n> held=1`；紧随的 `ENG SELFTEST` 应在 quiesce 阶段等锁 1s 超时并回 `cases=0 result=FAIL reason=quiesce`（UI 继续运行、引擎不冻结）。取锁确认超时回 `UI LOCKHOLD ERR not_acquired` |
| `UI WINDOW [<start> <len>]` | 写步窗（`sloop_state` 的 `step_window_t`，UI 局部态）：start 向下 16 对齐、0..48（bank = start/16），len 1..16；回显**落地值** `UI WINDOW bank=… start=… len=…`（无参打印当前窗口）。STEP 页列 c = 引擎绝对步 `start+c`，列数 = len，窗口外的格/列号隐藏且 TAP 命不中；注入脚本据此派生几何（`step_cell_xy`），不写死中心点。窗口是**持久状态**：脚本进入 STEP 前须归一到 `UI WINDOW 0 16`（step_edit/transport/nav/soak 均做） |
| `PARAM <id> [<value>]` / `PARAM GLOBAL <id> [<value>]` / `PARAM BATCH <id> <value> …` / `PARAM RAW <id> <value>` | 参数读写诊断注入（选中轨，经状态层 `CMD_SET_PARAM`/`CMD_SET_PARAM_BATCH`/`CMD_SET_GLOBAL` → 引擎入口；与 UI 编辑同一路径）：`PARAM <id>` 无 value 时只读回显（`value`/`min`/`max`，参数页编辑生效的断言入口）；`PARAM GLOBAL` 读/写 `song.g[]`（GLOBAL 参数页同一路径，回显 `applied`）；value 接受任意 int16（钳位在引擎，回显 `applied` 为钳位结果 + `min`/`max`）；`PARAM BATCH` 每个 (id,value) 一对，一次命令一次引擎调用（pair 数受 esp_console `max_cmdline_args` 默认 32 约束 → ≤15 对；状态层上限 128 只有 API 路径可达）。id 在该轨上无描述符（`sloop_core_desc` 判定：鼓轨仅 `P_E0`=kit）回 `PARAM ERR track=… id=… reason=invalid_id` 且不写。`PARAM RAW` **跳过 desc 预判**直接下发，用于复验引擎「无描述符 id 逐条忽略」的路径：回显的 `applied` 是回读的现有值（与 `value` 不同即未写；id ≥ P_COUNT 打印 `applied=n/a`）。写后立即回读，`STATE SNAP` 的 `param`/`global` 行即投影真值 |
| `STORE STATS` / `STORE PROJ LIST\|SAVE n\|LOAD n\|ERASE n\|HASH n` / `STORE PRESET LIST\|SAVE n\|LOAD n [track]\|ERASE n` / `STORE LIVEHASH` / `STORE RAWTEST` / `STORE FAULT <nth> [half]\|CLEAR` | 存储/工程诊断（P4-B，ADR-009 §9）：console 是**诊断执行器**——与 UI 命令共用同一套 `sloop_store_*` + 存储锁 + 引擎锁，行为相同，只是能把返回码变成脚本可断言的回显。`PROJ LIST` 每槽一行 `PROJ n EMPTY` / `PROJ n USED hash=XXXXXXXX`；`PROJ SAVE\|LOAD n` 回 `PROJ n SAVED\|LOADED hash=<捕获 hash>`、`PROJ ERASE n` 回 `PROJ n ERASED`；`PROJ HASH n` 只读 **RAM 槽**（不走 flash 回落，故是开机 fetch 的判别口）、空槽回 `STORE ERR empty`；`LIVEHASH` 是当前引擎状态的捕获 hash（工程往返门禁）。`PRESET LIST` 32 槽各一行 `PRESET n USED <ENGINE> <NAME>` / `PRESET n EMPTY` + 末尾 `PRESET COUNT used=n total=32`；`PRESET SAVE n`（= 选中轨的声音，默认名 `<ENGINE> <槽号>`）回 `PRESET n SAVED <ENGINE> <NAME>`、`PRESET LOAD n [track]`（缺省 track = 选中轨）回 `PRESET n LOADED track=t`、`PRESET ERASE n` 回 `PRESET n ERASED`；鼓轨存/载回 `STORE ERR drum`（与槽/轨号越界的 `invalid` 分开），两者都先打上游消息 `DRUM TRACK: NO SOUND`；`SAVE` 的鼓轨判定先于播放守卫（上游 `up_ui` 顺序，播放中选中鼓轨回 `drum` 而非 `playing`）。错误统一 `STORE ERR <no_storage\|playing\|empty\|invalid\|save\|drum>`（未就绪/走带中/空槽/槽号越界/擦写或回读校验失败/鼓轨无声音）。`STORE PROJ SYNTH <fmt1\|foreign>` 是**测试专用**诊断（**破坏性：覆盖诊断槽 3**，槽 0–2 不碰）：以当前引擎状态捕获内存工程 → 改写出 `project_load` 的 fallback 边界（格式 1 的 `preset==0xFF` / 引擎号 ≥ NENGINES）→ 存槽 3 → 清 RAM 槽强制 flash 回落（`proj_import`）→ 公共 LOAD → 每轨一行 `PROJ 3 SYNTH case=<c> trk k=n engine=.. preset=.. steps=.. atk=.. e0=..`；用于覆盖真机上原本不可达的两条 fallback。`STORE STATS` 打印 ready/base/span/map/objects/faults/armed + autosave=0|1/autosave_hash/autosave_writes；`STORE AUTO <on\|off>` 是自动存档开关（默认 on，测试脚本用：关掉后 tick 不再写 flash，开机恢复照常） |

- 命中矩形与控件布局**同源**：页内用同一组布局宏建控件、算 hit-test，禁止另抄一份坐标数字。
- 状态断言：`STATE SNAP`（页面/焦点/层键/选轨/bpm/playing/步窗 `window_bank/window_start/window_len`）、`ENG SNAP`（引擎真值；`trk k=… voices=<n>` 是该轨 active 话音数，note-on 后 ≥1、panic/淡出结束后归 0）、`ENG STEP <track> <idx>`（引擎真值，idx 为绝对步号 0..63）、`ENG PRESET <track> <engine> <preset>`（同一「当前 p[] + 引擎 + 预置」的纯写值向量：71 行 `P<id>=<v>`，无副作用；`ENG TRACK` 应用后 `PARAM <id>` 逐项回读必须与它 71/71 相同——「换预置 = 换声音」的机器判据；诊断冻结时不打印零向量而是 `ENG PRESET ERR frozen`（返回非零），越界参数同令牌；冻结由 `ENG FREEZE <on|off>` 手动复验（与 `ENG SELFTEST` 内部同一入口，**必须成对**；`UI QUIESCE` 只停 UI 定时器/输入、不冻结引擎读写，测不到这条路径））。
- 场景脚本：`scripts/inject/{transport,step_edit,tracks,nav,params,store}.sh [PORT]`（场景逻辑集中在 `scripts/inject/inject.py`，pyserial + screenshot.py 的读取/解码路径）。脚本须可重复（幂等，必要时先归一基线），退出码 0 才算通过。
- **同拍语义（nav 场景）**：「同一 30ms UI tick」用一次性批写多行实现（`Session.cmd_batch`，行间无主机延迟；单条命令慢路径往返 ~50ms，逐条发送必然跨拍），并用批内追加的 `UI` 在设备侧自证前提——`KEY tab` 与 `KEY PANIC` 之间若被一拍插入，页面已前进，批内 `UI` 读数就会暴露；3 组 `KEY tab` 按下/释放（down+up 配对，防「重复按下沿去抖」假通过）同理：批内 `UI` 仍是起点页 ⇒ 按下之间无拍执行。前提不成立记为工具侧「重新对齐」（日志注记，不判固件失败），最多 5 次；只有前提成立后的页面断言才计入 PASS/FAIL。锚点方法（单按 tab 并快速轮询到页翻转）见 `inject.py` 的 `nav_anchor`。
- **transcript 必须是原始回显**：串口字节逐字节写日志，不裁剪、不手改、不摘述；只允许 `# [inject] ...` 前缀的工具注记（非固件输出），SCR 的二进制帧体不入日志。日志与截图落 `firmware/build/verify/`，门禁证据按 §5.4(1) 晋升。

### 5.5 验证门禁（任务完成的定义）

按任务类别满足对应门禁，全部通过才可提交：

| 类别 | 门禁 |
|---|---|
| 固件代码 | `scripts/build.sh` 退出码 0，且**不新增**编译警告/错误 |
| 固件代码（烧录验证） | esptool 输出 `Hash of data verified`；启动日志无 fatal/abort/backtrace |
| UI 相关 | §5.4 全流程：截屏成功 + 视觉检查通过 |
| 引擎/DSP 相关 | `scripts/host-test.sh` 通过；设备等价改动另跑 `scripts/device-selftest.py` **全量 182 例**严格哈希比对（脚本自动停流/恢复；手动时先 `ENG AUDIO OFF`，跑完 `ENG AUDIO ON` 恢复——固件开机即流式，流式时 SELFTEST 会被拒）。自检内部成对执行诊断期 quiesce（暂停 30ms UI 定时器与 LVGL 定时器、禁用触摸 indev、输入事件丢弃并计数、TAP/KEY 注入被拒）+ 引擎读写冻结，跑完全量后恢复，故 **UI live 的开机态即可跑**，无需停 UI；`--case` 单例仅用于定点排障，不作门禁口径。见 sloop_core.h、sloop_ui.h 与 ADR-007 |
| 状态层/EditCmd 相关 | `scripts/state-test.sh` 退出码 0（gcc + stub 引擎，覆盖钳位/步切换字段清除与 TIE 连缀折叠/显式步号擦除/MUTE-SOLO/快照同步/参数命令与批量投影） |
| 参数页旋钮逻辑/格式化（`knob_math.c`/`knob.c` 纯函数与触摸接线） | `scripts/knob-test.sh` 退出码 0（弧线往返/环带/角度差/相对拖拽/格式化表，定点小数按上游 `fmt_fix` **截断**：TIME 104→`1.88s`、CUTOFF 90→`2.5kHz`、LFOHZ 105→`12.5Hz`；次单位负值补符号：F_DB raw 111→`-0.5dB`）；触摸接线另由 `scripts/inject/params.sh` 的 `UI KNOB press/drag` 用例覆盖（五页：按下不改值、拖拽改引擎真值、环带外 `ignored (ring band)`、引擎页锁定 `ignored (locked)`）；物理手感仍为人测项（DI-16） |
| 描述符投影（`sloop_core_desc` / `ENG DESC`） | `scripts/desc-check.py [PORT]` 退出码 0：10 项正例 label/fmt/min/max/def/unit/enums/first/last 逐字段一致 + 4 条越界路径回 `ENG DESC ERR` |
| 阶段收口 soak | `scripts/soak.py PORT --duration 1800`（阶段 3 口径 30min）退出码 0：internal_free/internal_min ≥60KB、首末差 ≥-8KB、连续 3 采样非增下滑 ≤2KB（HEAP_JITTER）、streaming=1 且 xrun=0、duty_mean ≤2× 阶段 2 静默渲染基线 9.44%（=18.88%，`ENG PERF` 流式分支口径）、音频任务栈余量 ≥2KB、KEY 事件计数增量 == 期望、日志无 panic/WDT；soak 摘要晋升并在 verification-log 登记（阶段 4 长时 soak 沿用同一口径，时长按阶段判据） |
| 联动协议 | 对真机 FM-1 的协议冒烟（连接 → INFO 版本协商 → WATCH 推送可达；测试脚本待落地） |
| scripts/ 工具改动 | `python3 scripts/screenshot.py --selftest` 退出码 0（解码/协议路径离线自测） |
| 注入脚本（`scripts/inject/`）/ TAP 相关固件改动 | 六条场景脚本对真机各跑一遍，退出码均 0（`UI`/`TAP`/`KEY` 回显断言 + `STATE`/`ENG` 快照断言 + 截屏像素抽检）；未命中路径打印 `TAP miss ...` 且不改状态；`nav` 的同拍用例要求批内 `UI` 自证前提成立（否则重对齐，不放宽断言）；`store` 另含**换音**用例（`ENG PRESET` 期望向量 vs `ENG TRACK` 后的 p[] 71/71、**独立上游表 oracle**（硬编码 `eng_digital.c`/`eng_analog.c`/`preset_trim.h` 字面量，防「预览与实现同源」的假绿）、`param_kept` 保留、发声时换预置的 `voices` 归零、`STORE PROJ SYNTH fmt1\|foreign` 覆盖 `project_load` 的两条 fallback；**自动存档**用例段：`STORE AUTO ON/OFF` 开关、`STORE STATS` 的 autosave 三项、idle 2.5s / gap 20s / 播放与发声抑制、硬复位后 `LIVEHASH`+bpm+步表恢复且开机不重写；设备上留有 autosave 对象时 `device-selftest.py` 仍须 182/182——自检基线是 resume **之前**拍的工厂态快照）。**在组作用域页面生命周期（DI-19）落地前，注入脚本之间必须重启设备**：同一会话连续跑 `store` + `params`（都建满 9 页）会把 LVGL 池余量压到弧线 mask 分配之下，触发空指针解引用 panic（见 `docs/deferred-issues.md` DI-19） |

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
| 发现可延迟的已知缺口/风险（不阻塞当前任务但跨会话须记住） | `docs/deferred-issues.md` 新增条目（必填：背景/影响面/为何可延迟/解决方向/触发条件） |
| deferred 条目被解决 | **删除该条目**；若涉及方案选型，先沉淀 `docs/design/` ADR 再删 |

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
