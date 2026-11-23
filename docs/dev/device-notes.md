# 本机设备档案（device-notes）

> 记录本机实际连接的开发设备与批次差异。新设备接入或发现批次差异时更新本文。
> 建档日期：2026-11-19

## 1. M5Stack Tab5（主机）— ✅ 已连接

| 项 | 值 | 备注 |
|---|---|---|
| USB 连接 | USB-C → `/dev/ttyACM0`（`303a:1001` Espressif USB JTAG/serial debug unit） | 用户在 dialout 组，可直接读写/烧录 |
| 主控修订版 | **ESP32-P4 rev v1.3（早期样片，非量产 v3.x）** | esptool 报告；启动日志 `efuse_init: Chip rev: v1.3` |
| 屏幕 TDDI | **ST7121 批次**（2026-04-28 起料号由 ST7123 变更为 ST7121；本机为该批次，用户确认） | 见下「v1.3 样片的配置影响」与「ST7121 批次的软件影响」 |
| Flash / PSRAM | 16MB 外挂 / 32MB（启动日志确认 32768K pool） | |

### v1.3 样片的配置影响（已固化在 `firmware/sdkconfig.defaults`）

IDF 默认面向量产 v3.1+，且 P4 的 `<3.0` 与 `>=3.0` 支持**互斥**，必须：

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y   # 总开关（先开这个，REV_MIN_1xx 才可见）
CONFIG_ESP32P4_REV_MIN_100=y           # 最低修订 v1.0，覆盖本机 v1.3
```

未配置时症状：esptool `bootloader.bin requires chip revision in range v3.1 - v3.99 (this chip is revision v1.3)`。

⚠️ 以该配置构建的产物**仅适用于 rev 1.0–1.99 样片**（IDF 中两条支持线互斥）；在量产 v3.x Tab5 上运行需改回默认（`ESP32P4_REV_MIN_301`）另建配置。

### ST7121 批次的软件影响

BSP 1.3.3 的 `bsp_get_board_version()`（`src/bsp_display.c`）运行时读 TDDI 固件版本自动分流：fw 1 → ST7121（`disp_init_data_st7121.h`），fw 3 → ST7123，否则 ILI9881C+GT911。批次分流本身**无需任何配置**，bring-up 后从日志 `Discovered board version 3 (LCD ST7121...)` 可确认探测结果。

⚠️ 例外：应用侧若绕过 `bsp_display_start()` 改用 `bsp_display_new_with_handles()` 组装（为保留 panel 句柄供 SCR 取帧，见 `firmware/main/display.c`），DSI lane 速率须自行固定 **965Mbps**——BSP 对 ST7121 批次的降速适配（`BSP_LCD_ST712X_MIPI_DSI_LANE_BITRATE_MBPS`）发生在其内部「先探测后建总线」流程里，而 `bsp_get_board_version()` 是 BSP 静态接口，应用无法先探测再传参。该固定值仅适用于 ST7121 批次本机。

## 2. M5Stack Tab5 Keyboard（A164）— ✅ 已连接（固件自检通过）

- 经 Tab5 **Ext.Port1** 以 I2C 连接（SDA=G0 / SCL=G1，地址 **0x6D**，INT=G50），**不是 USB 设备**——PC 侧 `lsusb` 无法枚举，连接与否只能物理确认排线。
- 2026-11-19 bring-up 验证（git 8f8e01b）：启动自检 `keyboard: OK (0x6D)`；键盘内部固件 **FW 0x01**（寄存器 0xFE 读回，与 Internal-FW V1.0 预期一致）；console `KB` 可查 probe/事件计数，INT 空闲电平正确（证据 `docs/dev/verify/1-bringup-2026-11-19/console-transcript.txt`）。
- ⏳ 人测待办（DI-4）：70 键全矩阵逐键 + Shift/Ctrl/Alt/Sym 组合核对。

## 3. M-VAVE FM-1（刷 SLOOP）— ❌ 未连接

- 当前 `lsusb` 无 `1209:0001` 设备；接入后应出现 MIDI 端口 "Felucca"。
- 联动模式开发期接入时在本文登记批次/固件版本（SLOOP 版本号可经编辑器协议 `INFO` 读取）。

## 4. 历史记录

| 日期 | 事件 |
|---|---|
| 2026-11-19 | 首次烧录 `sloop_fm1_tab5.bin`（骨架，git 3394356）成功；启动日志正常（v1.3/16MB/32MB PSRAM 确认） |
| 2026-11-19 | 显示点亮（git 211bee5）：ST7121 探测正常，横屏 1280×720 测试图绘制成功 |
| 2026-11-19 | 阶段 1 bring-up 机器侧收口（git 0f6fbf5）：分区表直读核对、键盘 0x6D+FW 0x01、ES8388 44.1kHz 出声自检、SCR 截屏+像素抽检闭环全绿；人测项（DI-4 键矩阵/触摸校准/听音）待办 |
| 2026-11-19 | 阶段 1 听音人测无声 → 定位 es8388 输出音量初始化缺口（见 §5）→ 修复（4278509）→ 人测出声 |
| 2026-11-20 | 阶段 2 引擎上机：设备/host 182/182 同源 golden 哈希全等；PERF 静默均值 9.44%/最大 11.73%、xrun=0；CC0 资产 283KB/8 集；ENG SELFTEST/TRACK/TRIG/PERF/INFO/SMOKE 命令就位（feat/sloop-core） |
| 2026-11-20 | 阶段 3 Task 2（DI-8）：音频流可停启（`ENG AUDIO ON/OFF`），开机即启流；SELFTEST/AUD TONE/离线 PERF 需先停流（见 §6） |
| 2026-11-21 | 阶段 3 收口（`feat/mvp-ui` → dev）：全门禁复跑（构建/烧录/启动/host-test/state-test/三条注入脚本）+ 30min soak；SCR 传输在连续串口开合会话后出现一次降速停滞（DI-5 复发，经 esptool 硬复位恢复，见 §8；复位走 esptool 的 bootloader/USB-JTAG 复位序列，不是裸 RTS 翻转——实测无效，见 §8.1） |

## 5. ES8388 无声排障取证（2026-11-19，阶段 1 听音人测）

- 现象：默认态 `AUD TONE` 全链路 rc=0 但扬声器无声（人测）。
- 取证链（排障命令 `AUD REG/PA/L2/MAX`，fix 4278509 引入）：
  - PI4IO 扩展器 0x43：SPK_EN（E1.P1）输出锁存=1，功放已使能；
  - ES8388 寄存器默认态：CHIPPOWER/DACPOWER 正常、DACCONTROL3 未静音，但 **DACCONTROL26/27（LOUT2/ROUT2）=0x00**（esp_codec_dev es8388 初始化显式写 0，-33.5dB 近静音），DAC 数字音量 0x16（=set_out_vol(70)→-11dB）；
  - 实验：仅 L2/R2→0x1E 仍无声；DAC 0dB + 四路输出（0x2E..0x31）→0x21 **出声**。
- 结论：板级功放输入路径需要显式初始化输出音量；开机态固化为四路 0x21（`audio_tone_init` 板级修正）。
- 未决：决定性寄存器（L1 还是 L2、0x21 是否必要）未二分（DI-6；2026-11-23 按官方原理图确定功放取自 LOUT1、LOUT2/ROUT2 未连接，条目关闭，余项见 DI-39 与 `docs/tab5-hardware.md` §3.2）；耳机通路未单独人测。
- 附录：`esp_codec_dev_read_reg` 对 es8388 不透传（恒失败），读寄存器须走裸 I2C（BSP 总线句柄 + 0x10 从地址）。

## 6. 音频流所有权与启停（DI-8，2026-11-20 解决）

- 开机序列末尾（`main.c`，`sloop_core_init` 成功后）直接 `sloop_hal_audio_start(sloop_core_render)` 启流；失败只记 ERROR 不阻断（`ENG AUDIO ON` 可重试）。
- `ENG AUDIO OFF` → `sloop_hal_audio_stop()`：置停止请求，任务在写块收尾后跳出循环、自挂起，stop 再 `vTaskDelete` 并置句柄 NULL；**codec 保持打开**（`AUD TONE` 立即可用）。不可直接删阻塞在 codec 写里的任务——会丢失 I2S 驱动锁（实测 AUD TONE 报 `channel is not enabled`）。无参数打印 `streaming=`/`xrun=`；无任务时 stop 幂等返回 ESP_OK。
- 流式期间 `ENG SELFTEST` 拒绝行带 `hint=ENG AUDIO OFF`（拒绝语义不变）；停流后 `ENG SELFTEST`（含单例）、`AUD TONE`、离线 `ENG PERF` 恢复正常。
- 流式期间 `ENG SMOKE` 同样被拒（`ENG SMOKE FAIL busy: audio streaming`，无 OFF 提示）；`ENG PERF` 在流式时只打印 xrun/栈余量分支，离线 100 块基线也需 `ENG AUDIO OFF` 后才可测。
- 停流不复位引擎运行期编辑（`sloop_core` 无「恢复 init 快照」入口）：普通 `ENG AUDIO OFF` 后 `ENG SNAP`/注音仍看到停流前的状态；而 `ENG SELFTEST` 自身在开始时 `selftest_restore()` 到初始快照、结束时再恢复一次，故自检会抹掉运行期编辑（自检语义，非 stop 副作用）。
- `xrun` 为累计计数，停/启不清零。
- 脚本化自检：`scripts/device-selftest.py` 自动 `ENG AUDIO OFF` → 跑 SELFTEST → 恢复 `ENG AUDIO ON`（见 AGENTS §5.1/§5.5）。

## 7. 触摸幽灵按压（2026-11-20，阶段 3 Task 5 采集现场发现）

- 现象：空闲采集期间触摸面板间歇上报「幽灵按压」，日志形如 `sloop_display: touch (690,22)`（落在页签条），
  后果是页面被意外切换、STEP 网格格被意外翻转（`ENG STEP` 可复现到非预期格变 n=1）。同期 Tab5 Keyboard
  也出现 `INT=1`、`pending=32`（队列灌满）的异常读数，两者是否有共同电源/接地原因未查。
- 影响：任何「注入 + 截屏」类证据采集（Task 7 的 `TAP` 注入、后续 soak）都可能被幽灵输入污染，**断言必须复核
  终态**（例如 Task 5 用 48 步全量 dump 复核只有预期格在响）。
- 规避（仅验证时）：scratch 构型用 `lv_indev_enable(touch_indev,false)` 关掉触摸 indev 再采集（Task 5 已用，开关已撤除）。
  真值排查方向：读触摸控制器原始坐标/INT 波形（`bsp_touch_new` 句柄 + 裸 I2C），以及检查排线/接地。
- 状态：未定性为固件缺陷（触摸驱动 Task 1 已验证四角坐标）；Task 7 触摸注入 + 人测时一并复核。

## 8. SCR 帧传输降速/停滞（DI-5 复发观测，2026-11-21 阶段 3 收口）

- 现象：Task 8 门禁采集时，在连续多次「串口开→关」的会话（`ENG MEM` 采样 → `device-selftest.py` → `inject/transport.sh`）之后，transport 脚本的首张截屏在 **208800/1843200** 字节处超时；随后直接读串口可见设备**仍在发帧**，速率约 **8.4KB/s**（5s 连续读 41,941B；正常会话约 1MB/s，全帧 ~2s），无 `ESP-ROM` 重启痕迹、无 panic/WDT；console 任务被 SCR 写循环占住（新命令无响应）。
- 处置（当次）：手工 esptool 硬复位（`--after hard_reset`；实际经 esptool 的 bootloader 接入 + USB-JTAG 复位序列，不是裸 RTS 引脚翻转——纯 pyserial 翻转 RTS/DTR 实测无效，见 §8.1）后同一脚本 48.8s 跑完（含 3 张 1.8MB 截屏，像素抽检全过），随后 step_edit/tracks 连续通过。与 DI-5「USB 状态相关、硬复位自愈」一致。（2026-11-21 起该复位已自动化，见 §8.1）
- 证据：`docs/dev/verify/3-mvp-2026-11-21/task-8-transport-scr-stall-fail.log`（原始串口回显；同目录 `task-8-transport-run.txt` 是复位后复跑通过的 stdout 断言摘要，不是失败证据）；速率测量命令见 Task 8 报告。
- 未定性（截至 2026-11-21 阶段 3 收口）：降速根因（疑似 USB-Serial-JTAG TX 侧或主机 IN 调度退化）。当时为避免掩盖固件问题未加自动重试、DI-5 保持在册——**该状态已由 §8.1 取代**（脚本级自动复位重试落地，DI-5 删除）。

### 8.1 处置：脚本级复位重试（2026-11-21 阶段 4 P4-0 Task 3）

- 复发升级：P4-0 Task 2 取证后又连续 3 次命中（全量 SELFTEST 后首张截屏 1.66–1.68MB/1.84MB 超时），且本任务实施时设备正处于停滞态（首次探测在 1,687,552/1,843,200 字节超时）——升级条件「无人值守批量截屏纳入常规流程」已触发。
- 落地：`scripts/screenshot.py --retry N`（默认 3；`0` = 旧行为）。传输层失败（帧头/帧数据/尾标记超时、短帧、坏尾、长度与分辨率不符＝响应头损坏、读 OSError）→
  `esptool --chip esp32p4 -p PORT -b 460800 --after hard_reset flash_id`（只读，不写 flash）→ 轮询 console 提示符
  `sloop>`（实测启动 ~4.1–4.6s）→ 重开串口重试；耗尽后非零退出：已收字节写 `<out>-fail.bin`，一字节未收写
  `<out>-fail.txt`（记录错误与尝试次数），随后 best-effort 复位一次（失败只打印）让批量继续。
  固件明确报错（`SCR ERR`、未知像素格式、响应头数值非法）与端口打开失败**不重试**（快速失败语义不变，避免掩盖固件问题）。
  `--timeout` 默认 30s（实测单帧 16.4–17.8s，原 20s 余量仅约 12%）。
  `scripts/inject/inject.py` 复用同一 `read_frame`/复位路径：截屏失败 → 复位 → 重开串口 → 场景从头重跑
  （复位清空页面/走带状态，只有重跑才能让断言与截图同源）；耗尽时同样落 `-fail.bin` 并做一次 best-effort 复位。
- 复位手段实测结论：**纯 pyserial 翻转 RTS/DTR 不能复位 USB-Serial/JTAG**（esptool 的
  `HardReset`/`USBJTAGSerialReset` 序列逐字节复刻后仍无启动日志；健康态与停滞态各试一次）。
  必须让 esptool 走 bootloader 接入（默认 `--before default_reset` 即 USB-JTAG 复位序列）再 `--after hard_reset`。
  esptool 定位顺序：`$SLOOP_ESPTOOL` > PATH（`esptool`/`esptool.py`）> `$IDF_PYTHON_ENV_PATH` > `~/.espressif/python_env/idf5.5*` > 其他 `~/.espressif/python_env/*/bin/esptool`（避免硬编码单一 IDF 版本）。
- 验证：离线 `--selftest` 覆盖「首轮短帧→复位→次轮成功」与「重试耗尽→失败证据」；真机 20 连拍全过（见
  `docs/dev/verify/4-p0-2026-11-21/task-3-scr-batch20.log`）；两次强制中断（直连 harness + inject）均由复位重试恢复。
- 残余：**降速根因仍未定位**（USB-Serial-JTAG TX 或主机 IN 调度退化的嫌疑不变）。DI-5 已删除（无人值守
  可靠性问题由自动重试解决）；若后续出现「重试后仍失败」「停滞波及非截屏串口命令」或「需要固件侧修 USB-JTAG
  TX」，按新问题重新登记，并以 `-fail.bin`/`-fail.txt` 与当时的 `dmesg`/USB 拓扑为起点取证。

