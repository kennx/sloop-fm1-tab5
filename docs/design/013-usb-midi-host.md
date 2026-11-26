# ADR-002: USB-MIDI host — ESP-IDF 原生 usb_host + 自写 MIDI Streaming 客户端

> 状态：已接受（阶段 5 5A Task 0）｜ 日期：2026-11-26
> 关联：spec `docs/superpowers/specs/2026-11-26-phase5-sloop-go-alignment-design.md` §2.1/§2.3/§2.4/§5.3/§6（5A）/§9；
> `docs/deferred-issues.md` DI-1（本 ADR 关闭其选型部分）；
> ADR-001（依赖核验：`usb_host_uac` 1.5.0）、ADR-004（vendored 只读）、ADR-007（任务栈与内部 RAM）、ADR-009 §7（内部 SRAM 边界）；
> AGENTS.md §4（USB host 身份）、§5.5（门禁）；`docs/tab5-hardware.md` §3.11；`docs/dev/device-notes.md` §3/§9；
> 实现对照：`third_party/sloop-fm1/firmware/src/usb.c`（设备侧）、
> `third_party/sloop-fm1-go/android/app/src/main/kotlin/com/sloop/go/midi/MidiEngine.kt`（host 侧）

## 背景

联动模式的全部控制面走 USB-MIDI：编辑器协议的请求下发与 CHANGED/RELOAD/STEP_CHANGED/TRACK_CHANGED 推送上行
都是 USB-MIDI SysEx。DI-1（ADR-001 依赖核验时发现）：`espressif/usb_host_uac` 1.5.0 只覆盖 UAC1 音频采集，
**不含 USB-MIDI host 类**；而 FM-1 是 **MIDI + UAC1 的组合设备**（VID:PID `1209:0001`，MIDI 端口名 "Felucca"），
Tab5 做 host 时两类接口各需一个客户端。当前固件没有任何 USB host 代码：未使能 `CONFIG_USB_HOST_ENABLED`，
USB-A 口的 VBUS（PI4IO 0x44 E2.P3 `USB5V_EN`）从未驱动——「插上 FM-1 没有任何反应」是现状的预期行为（spec §2.1）。

本 ADR 只回答一件事：**MIDI Streaming 接口的 host 客户端用哪条栈实现**。协议层（帧编解码、INFO、push）是纯 C、
与栈无关，另由 `editor_proto` 承担。

与选型相关的平台事实（2026-11-26 核对，本机）：

- ESP-IDF v5.5.5 的 `components/usb` 只有 Host Library 核心（`usb_host.c` / `usbh.c` / `hcd_dwc.c` / `hub.c`），
  **不自带 MIDI 类驱动**（全目录无 "midi"）；自带 host 例程为 `cdc` / `hid` / `msc` / `uvc` / `usb_host_lib` /
  `cherryusb_host`。
- BSP `espressif/m5stack_tab5` 1.3.3 提供 `bsp_usb_host_start(mode, limit_500mA)` 与 `BSP_FEATURE_USB`。
- **同板先例** `~/projects/M5MusicTab5`（本机，MIT）：
  `components/service_usb_host/service_usb_host.c`（584 行）+ `usb_host_midi_guide.md` 用原生 `usb_host` 在
  **同一块 Tab5 的 USB-A 口**上跑通了 USB-MIDI 键盘接入——识别 Audio 类 `0x01` / MIDI Streaming 子类 `0x03`、
  claim 接口、持续提交 Bulk IN、解析 4 字节事件包。它**没有** Bulk OUT、SysEx 重组、编辑器协议。
- 内部 RAM 余量很小：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`（≤16KB 的分配默认落内部 RAM，USB host 库任务按
  默认方式创建会落在内部 RAM）；内部堆门线 61,440B（`scripts/soak.py` / `scripts/selftest-heap.py` 的
  `HEAP_FLOOR`），而把任务栈/TCB 直接放 `.bss` 曾让开机 `internal_min` 恒减约 4.4KB（65,888 → 61,440B，
  正好压线；device-notes §9、ADR-007）。

## 选项

| 选项 | 评估 |
|---|---|
| **C1. ESP-IDF 原生 `usb_host` + 自写 MIDI Streaming 客户端（采纳）** | 要自己写一个类客户端：Bulk IN/OUT + 4 字节事件包 + SysEx 重组/分段。换来的是：与音频采集要用的 `espressif/usb_host_uac` **同栈**（两类接口共用一个 Host Library、各 claim 各的接口）；同板已有跑通的先例（背景）；IDF 自带多个 host 例程可对照；host 侧与设备侧各有现成实现可作测试对照（`MidiEngine.kt`、上游 `usb.c`） |
| C2. TinyUSB host | 要把 OTG 栈整体换成 TinyUSB；与 `usb_host_uac` 不同栈，音频采集得另起炉灶。TinyUSB 上游本身有 host MIDI 类（`src/class/midi/midi_host.c`），否决理由与它的成熟度无关 |
| C3. CherryUSB | 整栈替换；按其目录约定（`class/<类>/usbh_<类>.c`）**没有 MIDI 的 host 类驱动**（`class/midi/` 只有共用头 `usb_midi.h`），仍然要自己写；对只需要一个类客户端的本项目过重 |
| C4. 现成组件 | `espressif/` 下没有 `usb_host_midi`；其它命名空间也没有可直接用的 USB-MIDI host 类客户端（补查见下） |

**C4 补查（2026-11-26，ESP Component Registry API + IDF 5.5.5 本机树）**：

- `espressif/` 现有 host 类驱动：`usb_host_cdc_acm` / `usb_host_hid` / `usb_host_msc` / `usb_host_uac` /
  `usb_host_uvc`（另有 `*_vcp` 系列）；`espressif/usb_host_midi` 不存在（API 404）。检索 `usb host midi` /
  `usb_host_midi` / `midi host` 均返回 No component found。
- 名字或描述含 MIDI 的条目只有四类，都不是 USB host 类客户端：`espressif/esp_midi` 1.1.0（MIDI 文件解析 +
  SoundFont 合成，预编译库，不含 USB 传输）、`espressif/ble_midi` 0.1.0~2（BLE-MIDI，AGENTS §1 非目标）、
  `sauloverissimo/midi2` 0.10.2 与 `midi2cpp`（MIDI 2.0 / UMP 消息层，自述 "stays at the message layer;
  pair with your USB stack"）、`chegewara/midi-parser`（Registry 无已发布版本）。
- TinyUSB / CherryUSB 在 Registry 有打包（`espressif/tinyusb`、`espressif/esp_tinyusb`、
  `cherry-embedded/cherryusb` 等），IDF 5.5.5 另带 `cherryusb_host` 例程——即 C2/C3 取得方便，但不改变上表的评估。

## 决策

**C1**。`usb_fm1` 组件基于 ESP-IDF 原生 Host Library 自写 MIDI Streaming 客户端，不引入 TinyUSB / CherryUSB
等第三方 USB 栈。理由：

1. **与 `usb_host_uac` 同栈**。FM-1 的 UAC1 采集（5E）要用的 `espressif/usb_host_uac` 建立在 IDF Host Library
   之上；MIDI 客户端走同一栈，两类接口才是「一个 Host Library、两个客户端」。选 C2/C3 则音频采集要另起炉灶。
2. **同板先例证明路线可行**。`~/projects/M5MusicTab5`（MIT）在同一块 Tab5 的 USB-A 口上用原生 `usb_host`
   跑通了 MIDI 键盘接入。它是独立项目，**只作可行性证据与接口用法的参考，不复制其代码**；本项目按 IDF 公开
   接口自写。
3. **要自己写的部分有限且可测**。缺的只有「一个类」：枚举后找 MIDI Streaming 接口、claim、Bulk IN/OUT 环、
   USB-MIDI 事件包 ⇄ MIDI 字节流 ⇄ SysEx 帧。其中事件包与 SysEx 的转换不依赖 IDF，可以在 host 上单测。
4. **C4 确认没有现成组件可直接用**（见补查）。

## 后果

- **任务划分**（5A）：
  - `editor_proto`（纯 C，无 IDF 依赖）：帧编解码、v14/pack7/u35、INFO 解析、push 解析；host 单测（Task 2）。
  - `usb_fm1/midi_stream.c`（纯 C，无 IDF 依赖）：USB-MIDI 事件包 ⇄ MIDI 字节流 ⇄ SysEx 帧的重组与分段；
    host 单测（Task 3），测试对照取上游 `usb.c` 与 `MidiEngine.kt`。
  - `usb_fm1` 的 IDF 胶水：Host Library 开机常驻安装、client、枚举与 claim、Bulk IN/OUT、断开清理
    （Task 1 立骨架并实测内部堆；Task 4 完成客户端）。FM-1 的 UAC 接口在 5A **不 claim**（音频采集属 5E）。
  - 最小事务层与 console `FM1 …` 命令族（Task 5）；冒烟脚本与文档收口（Task 6/7）。
  - 5A **不做**：请求队列与瞬时 SET 合并、LIVE 遥控、音频采集、任何 UI 页面（归 5B/5C/5E）。
- **栈放 PSRAM**：本决策新建的任务（USB host 库任务等不在中断上下文的任务）栈一律用
  `xTaskCreateWithCaps(…, MALLOC_CAP_SPIRAM)` 创建，不占内部 RAM；音频等实时任务的栈不在此列，仍按 ADR-007
  留在内部 RAM。
- **内部堆门线不变**：`internal_min ≥ 61,440B`、`scripts/selftest-heap.py` 退出码 0。本 ADR 不下调
  `HEAP_FLOOR`；Task 1 先挪栈、再量固定占用，压不回门线之上时如实上报读数——是否按实测重定门线属门禁变更，
  按 spec §6（5A 内容 0 ③）另行登记并由用户知情，不在本 ADR 内决定。
- **代价与风险**：
  - 自己维护一个类客户端：USB-MIDI 事件包格式（含 SysEx 的 CIN 4/5/6/7 与末组规则）由本项目实现并对着上游
    实现做测试，上游变更时要跟。
  - 控制台与 host 的共存（芯片有两个 OTG 控制器；USB-C 的 USB-Serial/JTAG 控制台与 USB-A host 同时工作）是
    5A 的第一道关口，不通则影响面大、须立即上报（spec §6 5A 风险 ①）。
  - USB-A 口须先使能 VBUS，否则表现为静默无设备。
  - 内部 RAM 余量小，host 常驻的固定占用是已知风险（见上条）。
- **被否决方案**：TinyUSB host（整栈替换、与 `usb_host_uac` 不同栈）、CherryUSB（整栈替换且无 MIDI host 类）、
  现成组件（不存在）。
- **验证**：host 单测覆盖事件包 ⇄ SysEx 的分段/粘包/坏帧/超长与帧编解码；设备侧以「枚举到 `1209:0001` →
  INFO 版本协商 → `WATCH 3` → 在 FM-1 上拧旋钮后收到 CHANGED → 拔线清理、重插恢复 → 全程 USB-C 控制台不掉」
  为门禁（AGENTS §5.5「联动协议」）。

## 实测登记

- ✅ host 安装后 internal_min / internal_free（Task 1，2026-11-26 真机）：`usb_fm1_start()` 内部读数 `internal_free 83099 -> 80559`（安装本体 **−2,540B**，含 `usb_lib`/`fm1_cli` 两个落内部 RAM 的 TCB；此时还没有设备被枚举）；开机稳态连读三次 `ENG MEM` 均为 `internal_free=64695 internal_min=63476`（改动前 ≈69,7xx / 66,208 ⇒ 空闲 −5.0KB、开机最低点 −2.7KB），满足 `internal_min ≥ 61,440`、`internal_free ≥ 63,488`（`internal_free` 余量 1,207B）；`scripts/selftest-heap.py` 退出码 0（两次全量 182/182）。**读数口径：USB-A 口当时接着一台 FM-1 且已被枚举**（与计划「不接设备」的口径不同），按「稳态差值 − 安装本体」估算其中约 2.5KB 是设备对象与描述符缓存，不插设备的稳态读数未采到；无设备时的固定占用以安装本体 2,540B 为准。未改任何 sdkconfig：IDF v5.5.5 的 `usb` Kconfig 没有 `USB_HOST_ENABLED` 符号，P4 上 host 库恒编译（上文背景「未使能 `CONFIG_USB_HOST_ENABLED`」不成立）
- ✅ 控制台与 host 共存（Task 1，2026-11-26 真机）：`usb_host_install(skip_phy_setup=false)` 之后，USB-C 的 USB-Serial/JTAG 控制台全程可用——启动日志无 fatal/abort/backtrace，复位后 `ENG MEM`/`FM1 STATUS` 照常应答，`SCR` 截屏成功（1280×720 rgb565），两次全量 `ENG SELFTEST`（182/182）与 `selftest-heap.py` 均在 host 常驻下跑完；上文「代价与风险」里的共存风险关闭
- ⚠️ FM-1 枚举读数（Task 4，2026-11-26 真机；⚠️ 只因操作员步骤未做，内部堆门线已回线）
  - **枚举**：`VID=0x1209 PID=0x0001`、`class=0x00`（按接口分类的组合设备）、product=`Felucca`；配置里的接口：
    - 接口 0：Audio Control（`class=0x01 subclass=0x01`，eps=0）——UAC，不 claim
    - 接口 1：MIDI Streaming（`class=0x01 subclass=0x03`，eps=2）——**claim 这个**（alt 0）：`ep_in=0x81 mps=64`、`ep_out=0x01 mps=64`、IN 缓冲 `mps*4=256` 字节
    - 接口 2：Audio Streaming（`class=0x01 subclass=0x02`，alt 0 eps=0 / alt 1 eps=1）——UAC，不 claim（音频采集属 5E）
  - `FM1 STATUS link=ready vid=0x1209 pid=0x0001 product=Felucca rx=0 tx=0 drops=0 open_fail=0 midi_if=1 ep_in=0x81 ep_out=0x01`
  - **收发路径尚未被数据走过**：`rx=0 tx=0`——FM-1 不主动发 SysEx，5A 到本任务为止也没有 `on_frame`/`on_link` 的注册方和 `usb_fm1_send_frame` 的调用方，Bulk IN 环只是空转、Bulk OUT transfer 只分配未提交；第一批真实帧由 Task 5 的事务层发出。
  - **IN 环规则**（Task 4 评审修正）：`COMPLETED` 重提交并清零连续错误；瞬时错误（`ERROR`/`TIMED_OUT`/`OVERFLOW`/`SKIPPED`）先清 halt 再重提交（库出错后只 flush，pipe 停在 HALTED，直接提交会被拒）并累计连续错误，连续满 3 次才 link down；`STALL`/`NO_DEVICE`/`CANCELED` 不重提交、立即 link down。出错分支只经代码核对，真机未触发过——下文那次 `status=1` 发生在修正之前、走的是旧的立即停环路径——留待操作员拔插步骤与 Task 5 覆盖。
  - **共存与稳定**：控制台可用、`UI` 照常应答、`SCR` 截屏 1280×720 成功（PLAY 页，与 Task 1 固件同页截图 SHA256 相同）；`ENG PERF` xrun 计数 claim 前（Task 1 固件、FM-1 已枚举未打开）`xrun=0`，claim 后各次读数均 `xrun=0`。
  - **拔出/重插的一次旁证**（探针期间的偶然事件，不是受控操作员步骤；同时段有物理按键 `KB 0x4D down space`，物理原因未确认）：`W usb_fm1: Bulk IN 出错 status=1` → `I usb_fm1: 设备已清理（rx/tx 计数保留）` → 约 2.5s 后 `新设备 addr=2 VID=0x1209 PID=0x0001` → `MIDI Streaming 接口 1 就绪`，再次 `link=ready`，重 claim 前 `internal_free` 回到与首次 claim 前相同的值（清理无泄漏）。只此一例，是否与 claim 有关无定论，Task 5 留意复发。
  - **内部堆**（门线 `internal_min ≥ 61,440`、`selftest-heap.py`，均未改）：
    - ① 未开任何选项时 claim 一次的固定占用 `internal_free 64575 -> 60459`（−4,116B），分段实测——`usb_host_interface_claim` 本身 −2,444B（库的接口对象、两个端点 wrapper、两条 HCD pipe 的描述符表）、Bulk IN transfer −260B、Bulk OUT transfer −1,412B（1,373B 对齐到 1,408B）；稳态 `internal_free=60351 internal_min=59704`，比门线低 1,736B，`selftest-heap.py` 退出码 1（两次自检 182/182、无泄漏，唯一不过的是收尾 `internal_free=60035 < 61440`）。
    - ② 开 `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`（`sdkconfig.defaults`；重生成的 `sdkconfig` 与旧文件只差这一行）：claim 一次降到 −380B（`65931 -> 65551`），稳态 `internal_free=65467 internal_min=64232`，`selftest-heap.py` 退出码 0（两次 182/182、收尾 65,119B）；但余量只有 2,792B，低于要求的 4,096B。
    - ③ 再把 `sloop_input` 任务（8KB 栈，非音频、不写 flash）的栈挪到 PSRAM（`xTaskCreateWithCaps`，`components/sloop_ui/src/input.c`）：claim 一次 `74127 -> 73747`（−380B），稳态 `internal_free=73643 internal_min=72408`（余量 10,968B）；`scripts/inject/tracks.sh` 0 FAIL（84 PASS，`KEY` 注入经 input 任务派发），`KEY STATS input_stack_free=7884`（与内部栈时相同），xrun 仍 0；`selftest-heap.py` 退出码 0（两次 182/182、收尾 73,335B）。
  - **操作员步骤**：拔掉 FM-1（受控复跑：`FM1 STATUS` 回 `link=none`、`rx/tx` 保留、再插回 `link=ready`）未做（待操作员）；普通 USB 设备（U 盘/鼠标）未做（待操作员）
