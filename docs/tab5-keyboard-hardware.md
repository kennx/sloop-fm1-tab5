# M5Stack Tab5 Keyboard 硬件开发文档

> 用途：Dictation-Tab 项目硬件参考（Tab5 专用 70 键物理键盘，ESP-IDF 开发向）
> 整理日期：2026-10-22 ｜ 整理人：小智（OpenClaw）
> 信息来源见文末「信息来源」章节，所有数据均来自 M5Stack 官方文档与官方资源

---

## 1. 概述

Tab5 Keyboard（SKU: A164）是专为 Tab5 打造的 70 键物理键盘输入扩展模块：

- 通过 **Tab5 Ext.Port1** 直接连接主机，即插即用
- 内置 **STM32F030C8T6** 主控，负责 14×5 键盘矩阵扫描与通信处理
- **I2C 通信**（默认地址 **0x6D**），使用 Tab5 侧独立 I2C 引脚（SDA=G0 / SCL=G1，与板载 G31/G32 总线不同）
- 提供**独立中断引脚（INT）**，按键事件低延迟实时上报（具体引脚定义见原理图与 I2C 协议文档）
- 板载 2 颗 RGB LED（WS2812E-1313）可做状态指示

## 2. 规格参数

| 规格 | 参数 |
|---|---|
| SKU | A164 |
| MCU | STM32F030C8T6 |
| 按键 | 70 键 |
| 矩阵 | 14 × 5（支持多键同时按下） |
| I2C 地址 | 0x6D（默认） |
| RGB LED | 2 × WS2812E-1313 |
| 接口 | 2 × 5 PIN 排针，间距 2.54mm |
| 待机功耗 | DC 3.3V @ 14.5mA |
| 工作功耗 | RGB（白色最大亮度）3.3V @ 21.5mA；输入（RGB 关闭）3.3V @ 14.5mA |
| 产品尺寸 | 128.0 × 59.4 × 13.1 mm |
| 产品重量 | 65.2g |
| 包装尺寸 / 毛重 | 147.0 × 82.0 × 16.5 mm / 97.4g |

## 3. 按键布局

70 键提供：完整字母、数字、符号，以及常用功能键：

- 修饰键：`Aa`（大小写/上档）、`Ctrl`、`Alt`、`Sym`（符号）
- 功能键：`Tab`、`Esc`、方向键（↑↓←→）
- 支持多键同时按下（14×5 矩阵扫描）

## 4. 工作模式（预置固件三模式）

| 模式 | 说明 |
|---|---|
| 普通（Normal） | 默认模式，按键经 I2C 上报给 Tab5 主机，由主机固件解析 |
| HID | 键盘模拟 USB HID 设备工作 |
| 字符（Char） | 直接输出字符 |

模式细节与切换方法见官方用户手册（链接见下）。**Dictation-Tab 场景默认用「普通」模式**：Tab5 主机从 I2C 0x6D 读键值 + INT 中断触发。

## 5. 驱动与开发资源（ESP-IDF 视角）

ESP-IDF 下没有官方键盘驱动组件，需按 I2C 协议自写驱动（或直接参考出厂固件）。按优先级：

| 优先级 | 资源 | 链接 | 用途 |
|---|---|---|---|
| ⭐⭐⭐ | **Tab5 出厂固件源码**（ESP-IDF 工程） | https://github.com/m5stack/M5Tab5-UserDemo | 官方出厂固件，含键盘初始化/读键实现，可直接搬 |
| ⭐⭐⭐ | **I2C 协议文档（CN）** | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1241/Tab5_Keyboard-I2C-Protocol-CN-V1.0.pdf | 寄存器/协议定义，自写驱动的第一手资料 |
| ⭐⭐ | **键盘内部 STM32 固件源码** | https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW | 键盘 MCU 源码，协议不确定时直接看 |
| ⭐⭐ | **原理图 PDF** | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1241/SCH_Tab5_Keyboard_SCH_V1.0_20251109_2026_05_06_16_24_34.pdf | 引脚/电路（含 INT 定义） |
| ⭐ | **用户手册（CN）** | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1241/Tab5_Keyboard_User_Manual_CN.pdf | 模式切换、操作说明 |
| ⭐ | **硬件评估示例 UserDemo** | https://github.com/m5stack/M5Tab5-Keyboard-UserDemo | 官方 Demo（Arduino，可看协议用法） |
| 参考 | M5Unit-KEYBOARD 库（Arduino） | https://github.com/m5stack/M5Unit-KEYBOARD | Arduino 库，ESP-IDF 下仅作协议参考 |
| 参考 | Arduino 项目教程 | https://docs.m5stack.com/zh_CN/arduino/projects/tab5/tab5_keyboard | 官方上手教程（Arduino） |
| 其他 | 模型尺寸 PDF / 结构文件 / STM32 数据手册 | 见第 2 节与官方文档 | 结构设计与 MCU 规格 |

## 6. 集成要点（Dictation-Tab，ESP-IDF）

1. **接线**：经 Tab5 Ext.Port1 连接，主机侧为独立 I2C 引脚 **SDA=G0 / SCL=G1，中断 INT=G50**（官方文档管脚映射；与板载 G31/G32 总线相互独立，可用独立 `i2c_master` 总线或单独的 I2C 外设驱动，无地址冲突）
2. **取键方式**（普通模式）：GPIO 中断（INT 引脚，定义见原理图）→ `i2c_master_transmit_receive` 读键值；寄存器定义见 I2C 协议 PDF；**推荐直接移植出厂固件 M5Tab5-UserDemo 的键盘代码**（同为 ESP-IDF）
3. **RGB 指示**：2 颗 WS2812 可用 `led_strip` 组件驱动，做打字状态/呼吸灯反馈
4. **功耗**：输入态 14.5mA @3.3V，电池场景可接受；长期不用可断 EXT5V（见主机文档 IO 扩展器章节）
5. **拿不准协议时**：直接翻 STM32 内部固件源码（开源），这是最权威的参考

## 7. 信息来源

1. **M5Stack Tab5 Keyboard 官方文档**（主来源，规格/模式/链接）：https://docs.m5stack.com/zh_CN/tab5/Tab5_Keyboard （2026-10-22 抓取）
2. **M5Stack Tab5 官方文档**（Ext.Port/I2C 总线/G31/G32/出厂固件链接）：https://docs.m5stack.com/zh_CN/core/Tab5 （2026-10-22 抓取）
3. **Tab5 出厂固件源码 M5Tab5-UserDemo**（ESP-IDF 键盘驱动参考）：https://github.com/m5stack/M5Tab5-UserDemo
4. GitHub 官方仓库：M5Tab5-Keyboard-UserDemo、M5Tab5-Keyboard-Internal-FW、M5Unit-KEYBOARD（星数为 2026-10-22 数据）

> ⚠️ 本文为整理稿；INT 引脚编号与寄存器细节未在官方网页明示，动手前请核对原理图 PDF 与 I2C 协议文档。
