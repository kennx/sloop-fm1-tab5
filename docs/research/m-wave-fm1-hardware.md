# M-VAVE FM-1 硬件调研（目标联动设备）

> 用途：sloop-fm1-tab5 项目上游硬件参考
> 调研日期：2026-11-19
> 说明：本项目中「m-wave-fm1」指 **M-VAVE FM-1**（深圳 M-VAVE 品牌的口袋 FM 合成器）。硬件本身不是本项目的运行平台，而是 **USB 联动对端**：刷入 SLOOP 固件后通过 USB 与 Tab5 互连。
> 信息来源见文末。

---

## 1. 设备概述

M-VAVE FM-1 是一台低价口袋 FM 合成器（约 ¥300 档），2026 年中上市。官方固件为 6 算子 FM 合成器；社区已为其逆向出完整自制固件生态（SLOOP、Felucca、Clipper、Melodee 等），是本项目存在的先决条件。

| 项目 | 参数 |
|---|---|
| 主控 SoC | 杰里科 JieLi **AC791N**（封装 AC7911B8），双核 32 位 **pi32v2** RISC 自有内核（非 ARM、非 Xtensa、无公开指令集手册），实测主频约 120 MHz |
| 屏幕 | 240×240 TFT 彩屏，SPI 接口 |
| 键盘 | 27 键硅胶键盘（约两个八度，标准钢琴布局，白键 16 / 黑键 11），LED 键灯 |
| 编码器 | 共 6 个：MASTER、SELECT 双编码器 + KNOB 1–4（布局详见 SLOOP 固件文档 §2） |
| DAC | CS4344 |
| 功放 | SL5316（内建扬声器，插耳机自动切换） |
| USB | USB Type-C：USB MIDI + USB 音频（UAC1）+ 充电，**class-compliant 免驱** |
| MIDI | USB MIDI / BLE MIDI（原厂固件；SLOOP 从不开蓝牙）/ 3.5mm TRS MIDI IN，三者原厂可同时用 |
| 音频输出 | 3.5mm 立体声耳机孔 + 内建扬声器 |
| 电池 | 2000mAh，约 12 小时，5V/1A 充电 |
| 尺寸 | 161.5 × 96.5 × 28.6 mm |
| 官方固件规格 | 6 算子 FM、32 算法、128 预设、12 复音、琶音器（7 模式）、16 步音序器（16 个独立 sequence） |

## 2. 与本项目相关的关键事实

### 2.1 USB 身份（Tab5 侧识别 FM-1 的依据）

刷 SLOOP/Felucca 固件后（原厂固件身份不同）：

| 项 | 值 |
|---|---|
| USB VID:PID | **0x1209:0x0001**（pid.codes 社区 VID） |
| MIDI 端口名 | **"Felucca"**（SLOOP 刻意保留，供编辑器/安装器识别） |
| USB 音频 | UAC1 输入（立体声 16-bit，44.1/48 kHz 由固件 AUDIO > USB RATE 决定），bcdDevice 3.11；音频输入名同为 "Felucca" |
| 组合设备 | 同一 USB 设备上同时挂 MIDI + UAC1，录音时 MIDI 协议照常工作 |

> ⚠️ 原厂固件 / M-UPGRADE 更新模式下 USB 身份不同（更新帧走 `F0 22 24 35 …` 键帧和 `00 59 …` 帧，编辑器协议中明确禁止发送）。本项目只做编辑器协议，不碰更新协议。

### 2.2 USB 连线拓扑

- FM-1 是 **USB device**；Tab5 必须做 **USB host**（Tab5 有 Type-A Host 口 + Type-C OTG 口，均可用，见 `docs/tab5-hardware.md`）。
- 先例：[andrerezemini/fm1-cartridge](https://github.com/andrerezemini/fm1-cartridge) 用 **ESP32-S3 做 USB host** 连 FM-1：接收 USB-MIDI（音符/时钟/走带），回传立体声 PCM（USB 音频），由 FM-1 混音输出——证明 ESP32 系芯片作 FM-1 的 USB host 收发 MIDI + 音频完全可行。本项目方向相反（Tab5 控制 FM-1、并可用 FM-1 的 USB 音频输入取回声音），但链路层同类。

### 2.3 刷机与恢复（开发期需要）

- SLOOP 安装：浏览器 Web Installer（WebMIDI，Chrome/Edge）或 `python tools/fm1_install.py xxx.fwsc`（mido + python-rtmidi）。
- 固件包格式 `.fwsc`（app + OTA loader，带 SHA-256/CRC 校验）。
- 回官方：M-VAVE 官方 M-UPGRADE 工具 + FM-1 V16 官方包；或 Clipper 安装页的 "Return to the official firmware"。
- 变砖恢复：[kurogedelic/FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter)。
- SLOOP 救援模式：按住 OCT− 开机进入 USB RESCUE。
- ⚠️ 2.8.5 才支持新版 UC25HQ80 flash 芯片的机型；开发用 FM-1 若为该批次需刷 2.8.5+。

### 2.4 算力对比（移植可行性）

| | FM-1 (AC791N) | Tab5 (ESP32-P4) |
|---|---|---|
| 内核 | 双核 pi32v2 @ ~120MHz | 双核 RV32 @ 360MHz + LP 核 |
| RAM | 片内有限（SLOOP 有 RAM.md 专文记录分配） | 32MB Octal PSRAM + 片内 SRAM |
| DSP | JieLi HW DSP/EQ/SRC（SLOOP 未依赖，纯软件合成） | 无专用音频 DSP，软件浮点/定点 |
| Flash | 外挂 SPI（SLOOP 用 ~1MB 布局，见下） | 16MB |

SLOOP 在 120MHz 双核 pi32v2 上跑满 4 轨 12 引擎并留有余量（2.7 起用第二核扩容），Tab5 的 360MHz 双核 RISC-V 算力显著富余；主要差异在浮点能力与内存模型，移植时需按 sloop 的 host build 先行验证。

## 3. SLOOP 使用的 Flash 布局（来自 EDITOR_PROTOCOL.md / upreset.c）

编辑器协议暴露的存储对象（Tab5 联动模式下做备份/恢复时直接相关）：

| 地址 | 内容 |
|---|---|
| 0xA0000 / 0xB4000 / 0xC8000 | 用户采样槽 USR1–USR3（各 80 KiB） |
| 0xDC000..0xDFFFF | 用户预设银行（OBJ_UPRESET0/1，A/B 扇区对，2×16 记录×192B） |
| 0xE5000 / 0xE6000 | FM6 银行（27×128B packed DX7 patch，A/B 扇区对） |
| 0xE7000 | 用户采样槽 USR4（SLOOP 2.4 起） |

## 4. 社区生态（可参考的周边项目）

| 项目 | 说明 | 对本项目的价值 |
|---|---|---|
| [isod89/sloop-fm1](https://github.com/isod89/sloop-fm1) | SLOOP 固件（Felucca 衍生，GPL-3.0） | 移植主体，见 `sloop-fm1-firmware.md` |
| [hugelton/Felucca](https://github.com/hugelton/Felucca) | SLOOP 的上游（Leo Kuroshita） | 协议 "FL" 头与命令 1–15 的来源 |
| [jahlib/sloop-fm1-go](https://github.com/jahlib/sloop-fm1-go) | Android 原生 companion app | 联动 UX 与协议实现的参考蓝本，见 `sloop-fm1-go.md` |
| [joakimeriksson/fm1-emu](https://github.com/joakimeriksson/fm1-emu) | AC791N 全系统模拟器（pi32v2 指令集逆向） | 交叉验证固件行为；证明 240×240 屏与外设模型 |
| [andrerezemini/fm1-cartridge](https://github.com/andrerezemini/fm1-cartridge) | ESP32-S3 USB host 伴生音源 | ESP32↔FM-1 USB host 链路先例 |
| shcherbakov7/m-wave-fm1-browser-emulator | 浏览器模拟器 | 参考 |
| Frostskade-OS / InfeKt-FM1 / spiralfm1 等 | 其他第三方固件 | 仅参考 |

## 5. 信息来源

1. sloop-fm1 README.md / BUILDING.md / web/EDITOR_PROTOCOL.md（2026-11-19 抓取，commit bac9c05）
2. jahlib/sloop-fm1-go README.md 与 android/README.md（2026-11-19 抓取，commit ed941b5）
3. Reddit r/synth FM-1 拆机帖（AC7911B8 / SL5316 / CS4344 / ACC3610S）
4. Elektronauts 论坛规格表、Amazon 商品页规格（尺寸/电池/MIDI）
5. joakimeriksson/fm1-emu README（AC791N 双核 pi32v2、240×240 屏、外设模型）
6. andrerezemini/fm1-cartridge README（ESP32 USB host 先例）

> ⚠️ 本文为整理稿；主频（~120MHz）来自模拟器文档推断，未见于官方资料。动手前以实测与上游源码为准。
