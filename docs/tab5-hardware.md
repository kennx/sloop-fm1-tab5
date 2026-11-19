# M5Stack Tab5 硬件开发文档

> 用途：Dictation-Tab 项目硬件参考（ESP-IDF 开发向）
> 整理日期：2026-10-22 ｜ 整理人：小智（OpenClaw）
> 信息来源见文末「信息来源」章节，所有数据均来自 M5Stack 官方文档与官方资源

---

## 1. 概述

Tab5 是一款双芯片架构的便携式智能物联网终端开发设备：

- **主控**：ESP32-P4NRW32（RISC-V 32 位双核 @360MHz + LP 单核 @40MHz），负责计算与外设
- **无线**：ESP32-C6-MINI-1U（Wi-Fi 6），通过 SDIO 与 P4 通信；天线可在内置 3D 天线与外部 MMCX 接口间切换
- **显示**：5″ IPS TFT，1280×720（720P），MIPI-DSI 接口，显示触控一体化驱动 IC（ST7123 / ST7121）

⚠️ **无经典蓝牙**：ESP32-C6 仅支持 Wi-Fi 6 + BLE 5，**不支持蓝牙经典（BR/EDR）**，无法连接普通 A2DP 蓝牙音箱（Espressif 芯片规格）。

## 2. 规格总表

| 项目 | 参数 |
|---|---|
| 主控 SoC | ESP32-P4NRW32 @RISC-V 32 位双核 360MHz + LP 核 40MHz |
| 无线模组 | ESP32-C6-MINI-1U（Wi-Fi 6，SDIO 通信） |
| Flash | 16MB |
| PSRAM | 32MB（Hex 16 线接口；esp32p4 在 IDF 中唯一 PSRAM 模式为 `SPIRAM_MODE_HEX`，M5 规格页记作 Octal 指颗粒封装） |
| 显示 | 5″ IPS TFT 1280×720，MIPI-DSI，ST7123/ST7121 触控一体（早期版为 ILI9881C+GT911） |
| 摄像头 | SC2356 2MP（1600×1200），MIPI-CSI |
| 音频编解码 | ES8388 |
| 回声消除前端 | ES7210（AEC） |
| 麦克风 | 双麦克风阵列 |
| 扬声器 | 1W@8Ω（NS4150B 功放） |
| 耳机 | 3.5mm 耳机孔 |
| IMU | BMI270 六轴（加速度+陀螺仪，支持中断唤醒） |
| RTC | RX8130CE（支持定时中断唤醒） |
| 电源监测 | INA226 实时电压电流监测 |
| 充放电管理 | IP2326，升降压 MP4560 |
| 电池 | NP-F550 可拆卸锂电池 2000mAh（标准版不含，Kit 版含） |
| USB | Type-A（Host）+ Type-C（USB 2.0 OTG） |
| 工业总线 | RS-485（SIT3088，板载 120Ω 终端电阻拨码开关） |
| 扩展接口 | HY2.0-4P、M5-Bus（30pin）、GPIO_EXT 排母、microSD 卡槽、STAMP 焊盘（预留 Cat.M / NB-IoT / LoRaWAN 模块） |
| 按键 | Reset / Boot |

## 3. 管脚映射（ESP32-P4 视角）

### 3.1 摄像头（MIPI-CSI）

| ESP32-P4 | CAM |
|---|---|
| G32 | CAM_SCL |
| G31 | CAM_SDA |
| G36 | CAM_MCLK |
| CSI_DATAP1（专用） | CAM_D1P |
| CSI_DATAN1（专用） | CAM_D1N |
| CSI_CLKP（专用） | CAM_CSI_CKP |
| CSI_CLKN（专用） | CAM_CSI_CKN |
| CSI_DATAP0（专用） | CSI_DOP |
| CSI_DATAN0（专用） | CSI_DON |

### 3.2 音频：ES8388（编解码，I2C 地址 0x10）

| ESP32-P4 | G30 | G27 | G26 | G29 | G32 | G31 |
|---|---|---|---|---|---|---|
| ES8388 (0x10) | MCLK | SCLK | DSDIN | LRCK | SCL | SDA |

### 3.3 音频：ES7210（AEC 前端，I2C 地址 0x40）

| ESP32-P4 | G30 | G27 | G28 | G29 | G32 | G31 |
|---|---|---|---|---|---|---|
| ES7210 (0x40) | MCLK | SCLK | ASDOUT | LRCK | SCL | SDA |

### 3.4 显示（MIPI-DSI）

| ESP32-P4 | ILI9881C / ST7123 / ST7121 |
|---|---|
| G22 | LEDA（背光） |
| DSI_CLKN（专用） | DSI_CK_N |
| DSI_CLKP（专用） | DSI_CK_P |
| DSI_DATAN1（专用） | DSI_D1_N |
| DSI_DATAP1（专用） | DSI_D1_P |
| DSI_DATAN0（专用） | DSI_D0_N |
| DSI_DATAP0（专用） | DSI_D0_P |

### 3.5 触摸（I2C）

| ESP32-P4 | G31 | G32 | G23 |
|---|---|---|---|
| GT911 (0x14) / ST7123 (0x55) / ST7121 (0x55) | SDA | SCL | TP_INT |

### 3.6 传感器与电源监测（I2C，共用总线）

| ESP32-P4 | G32 | G31 |
|---|---|---|
| BMI270 (0x68) | SCL | SDA |
| RX8130CE RTC (0x32) | SCL | SDA |
| INA226 (0x41) | SCL | SDA |

中断唤醒链路（PMS150G-U06 协处理器）：

| PMS150G-U06 | PA6/CIN- |
|---|---|
| BMI270 (0x68) | INT(E_TRG) |
| RX8130CE | INT(E_TRG) |

### 3.7 ESP32-C6 无线模组（SDIO2）

| ESP32-P4 | G11 | G10 | G9 | G8 | G13 | G12 | G15 | G14 |
|---|---|---|---|---|---|---|---|---|
| ESP32-C6 | SDIO2_D0 | SDIO2_D1 | SDIO2_D2 | SDIO2_D3 | SDIO2_CMD | SDIO2_CK | RESET | IO2 |

### 3.8 microSD（双模式）

| ESP32-P4 | G39 | G40 | G41 | G42 | G43 | G44 |
|---|---|---|---|---|---|---|
| SPI 模式 | MISO | — | — | CS | SCK | MOSI |
| SDIO 模式 | DAT0 | DAT1 | DAT2 | DAT3 | CLK | CMD |

**容量说明**（官方文档/商店页均未标注上限，2026-10-23 核实）：
- 理论上限 = 2TB（SDXC 规范上限，P4 的 SDMMC 控制器支持 SD/SDHC/SDXC）
- 实际由软件栈决定：FAT32（≤32GB）通吃所有框架；64GB+ 需 ESP-IDF 开 `CONFIG_FATFS_SUPPORT_EXFAT=y`（Arduino 路线建议止步 32GB）
- 实用推荐：32GB 最省心；录音场景绰绰有余（16kHz/16bit 单声道 PCM ≈ 115MB/小时）

### 3.9 RS-485

| ESP32-P4 | G21 | G20 | G34 |
|---|---|---|---|
| SIT3088 | RX | TX | DIR |

### 3.10 HY2.0-4P（PORT.A）

| HY2.0-4P | Black | Red | Yellow | White |
|---|---|---|---|---|
| PORT.A | GND | 5V | G53 | G54 |

### 3.11 IO 扩展器：PI4IOE5V6408 ×2（I2C）

两颗扩展器共用主 I2C 总线（SCL=G32，SDA=G31）：

- **PI4IOE5V6408-1（0x43）** 控制位：

| 功能组 | E1.P0 | E1.P1 | E1.P2 | E1.P4 | E1.P5 | E1.P6 | E1.P7 |
|---|---|---|---|---|---|---|---|
| 天线切换 | RF_PTH_L_INT_H_EXT | RF_INT_EXT_SWITCH | | | | | |
| NS4150B 功放 | | SPK_EN | | | | | |
| 扩展 5V | | | EXT5V_EN | | | | |
| LCD | | | | LCD_RST | | | |
| 触摸 | | | | | TP_RST | | |
| 摄像头 | | | | | | CAM_RST | |
| 耳机检测 | | | | | | | HP_DET |

- **PI4IOE5V6408-2（0x44）** 控制位：

| 功能组 | E2.P0 | E2.P3 | E2.P4 | E2.P5 | E2.P6 | E2.P7 |
|---|---|---|---|---|---|---|
| ESP32-C6 | WLAN_PWR_EN | | | | | |
| USB-A | | USB5V_EN | | | | |
| 设备电源 | | | PWROFF_PLUSE | | | |
| IP2326 充电 | | | | nCHG_QC_EN | CHG_STAT_LED | CHG_EN |

要点：
- `RF_PTH_L_INT_H_EXT`：低电平用内置天线，高电平用外置天线
- `EXT5V_EN`：控制 M5-Bus、2.54-10P 侧边拓展、HY2.0-4P 的 5V 输出
- `WLAN_PWR_EN`：ESP32-C6 供电开关

### 3.12 M5-Bus（30pin，背面金手指）

| FUNC | PIN | LEFT | RIGHT | PIN | FUNC |
|---|---|---|---|---|---|
| | GND | 1 | 2 | G16 | GPIO |
| | GND | 3 | 4 | G17 | PB_IN |
| | GND | 5 | 6 | RST | EN |
| MOSI | G18 | 7 | 8 | G45 | GPIO |
| MISO | G19 | 9 | 10 | G52 | PB_OUT |
| SCK | G5 | 11 | 12 | 3V3 | |
| RXD0 | G38 | 13 | 14 | G37 | TXD0 |
| PC_RX | G7 | 15 | 16 | G6 | PC_TX |
| Int SDA | G31 | 17 | 18 | G32 | Int SCL |
| GPIO | G3 | 19 | 20 | G4 | GPIO |
| GPIO | G2 | 21 | 22 | G48 | GPIO |
| GPIO | G47 | 23 | 24 | G35 | GPIO |
| | HVIN | 25 | 26 | G51 | GPIO |
| | HVIN | 27 | 28 | 5V | |
| | HVIN | 29 | 30 | BAT | |

## 4. I2C 设备地址速查

| 设备 | 地址 | 总线 |
|---|---|---|
| ES8388 音频编解码 | 0x10 | G31/G32 |
| ES7210 AEC 前端 | 0x40 | G31/G32 |
| PI4IOE5V6408-1 | 0x43 | G31/G32 |
| PI4IOE5V6408-2 | 0x44 | G31/G32 |
| 触摸 GT911（早期批次） | 0x14 | G31/G32 |
| 触摸 ST7123/ST7121（一体驱动） | 0x55 | G31/G32 |
| BMI270 IMU | 0x68 | G31/G32 |
| RX8130CE RTC | 0x32 | G31/G32 |
| INA226 电源监测 | 0x41 | G31/G32 |
| Tab5 Keyboard | 0x6D | 独立总线 G0(SDA)/G1(SCL)，INT=G50（经 Ext.Port1） |

## 5. 开发环境（ESP-IDF 优先）

### 5.1 ESP-IDF 官方资源

| 资源 | 链接 | 说明 |
|---|---|---|
| **官方 BSP**（Espressif 组件库） | https://components.espressif.com/components/espressif/m5stack_tab5 | `idf.py add-dependency "espressif/m5stack_tab5"`，最新稳定版 1.3.3（2026-11-19 复核，本仓库锁定 ^1.3.3），Apache-2.0，target: esp32p4 |
| BSP 源码 | https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_tab5 | esp-bsp 仓库 m5stack_tab5 目录 |
| **出厂固件源码** | https://github.com/m5stack/M5Tab5-UserDemo | 官方出厂固件，即 ESP-IDF 工程，含全部板载外设初始化参考（含键盘） |
| 出厂固件编译教程 | https://docs.m5stack.com/zh_CN/esp_idf/m5tab5/userdemo | 官方 ESP-IDF 编译/烧录教程 |

**BSP 能力矩阵**（v1.0.0 时点，以 Registry 最新版为准）：
- ✅ 显示（MIPI-DSI → esp_lvgl_port ^2 / LVGL）、✅ 触摸（GT911；2025-10-14 后批次为 ST7123/ST7121 一体驱动，用最新版 BSP）
- ✅ 音频（esp_codec_dev ~1.5：ES8388 扬声器输出 + ES7210 麦克风输入）——**Dictation-Tab 语音输入/回放直接可用**
- ✅ SD 卡（≥5.4）、✅ 摄像头（MIPI-CSI）
- ❌ 未封装：按键/旋钮/LED/电池/IMU——BMI270、RX8130CE、INA226、PI4IO 扩展器、Tab5 Keyboard 均需自行驱动（I2C 地址见第 4 节）

官方 BSP 示例（esp-bsp/examples）：display、display_audio_photo、display_camera_csi、display_lvgl_demos、display_rotation、display_sdcard、display_usb_hid

**ESP-IDF 版本**：P4 支持需 **IDF ≥5.4**（BSP 依赖声明）；建议跟最新稳定版。

### 5.2 ESP-IDF 硬件驱动要点

- **I2C 总线**：所有板载 I2C 设备挂 G31(SDA)/G32(SCL)，用 `i2c_master` API 建总线后按地址挂设备（地址表见第 4 节）
- **PSRAM**：32MB，项目开 `SPIRAM`，模式用 `SPIRAM_MODE_HEX`（P4 唯一选项，无 OCT 符号；误写 `SPIRAM_MODE_OCT` 会报 unknown kconfig symbol）
- **烧录**：USB-C（OTG）+ esptool：`idf.py flash monitor`
- **屏幕复位时序**：见第 6 节坑点；ESP-IDF 下参考出厂固件/BSP 的面板初始化实现，勿自己从零写

### 5.3 其他框架（备选）

- Arduino：M5Unified ≥0.2.23 / M5GFX ≥0.2.30，https://docs.m5stack.com/zh_CN/arduino/m5tab5/program
- PlatformIO（官方参考配置）：

```ini
[env:esp32p4_pioarduino]
platform = https://github.com/pioarduino/platform-espressif32.git#54.03.21
upload_speed = 1500000
monitor_speed = 115200
build_type = debug
framework = arduino
board = esp32-p4-evboard
board_build.mcu = esp32p4
board_build.flash_mode = qio

build_flags =
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=5
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1
lib_deps =
    https://github.com/M5Stack/M5Unified.git
    https://github.com/M5Stack/M5GFX.git
```

- UiFlow2：https://docs.m5stack.com/zh_CN/uiflow2/Tab5/program

### 5.4 社区 ESP-IDF 固件参考

| 仓库 | 说明 |
|---|---|
| [cloudspores/tab5](https://github.com/cloudspores/tab5) | ESP-IDF + LVGL 应用（网络电台/FM 合成/启动器） |
| [hey-its-brian/tab5-cyberdeck](https://github.com/hey-its-brian/tab5-cyberdeck) | Tab5 + Keyboard 赛博键盘固件（31★） |
| [ImLunchtime/Tab5_Launcher](https://github.com/ImLunchtime/Tab5_Launcher) | Launcher 移植（22★） |
| [DevanMetz/Tab5OS](https://github.com/DevanMetz/Tab5OS) | 开源触控环境与应用商店 |
| [tobozo/LGFX_PPA](https://github.com/tobozo/LGFX_PPA) | P4 PPA 硬件加速工具（LovyanGFX/M5GFX 配套） |
| [CarbonNeuron/m5stack-tab5-bsp](https://github.com/CarbonNeuron/m5stack-tab5-bsp) | 社区 BSP（6★，自动检测） |

## 6. 已知坑（务必注意）

1. **屏幕复位时序**：屏幕复位时 PI4IO 的 G4（LCD_RST）拉低阶段需配置为推挽或开漏模式，**拉高阶段需切换为输入上拉模式**，否则会干扰 I2C 总线稳定性。使用 M5Unified ≥0.2.23、M5GFX ≥0.2.30 可规避。
2. **屏幕驱动批次变更**：2025-10-14 起，Tab5 由 ILI9881C+GT911（分立）变更为 **ST7123/ST7121 显示触控一体驱动**；其后 2026-04-28 起 TDDI 料号由 ST7123 变更为 **ST7121**（本机设备即为该批次，见 `docs/dev/device-notes.md`）。早期编译固件可能无法运行，需用新版库重编译。可通过设备背部贴纸确认驱动型号。
3. **无经典蓝牙**（见第 1 节），音频输出走内置扬声器 / 3.5mm / Wi-Fi 推流。
4. 标准版不含 NP-F550 电池，需另购或外接电源。

## 7. 数据手册与官方资源

| 资源 | 链接 |
|---|---|
| Tab5 结构文件 | https://github.com/m5stack/M5_Hardware/tree/master/Products/C145_Tab5/Structures |
| ESP32-P4 数据手册 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/esp32-p4_datasheet_cn.pdf |
| ESP32-C6 数据手册 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/505/esp32-c6_datasheet_cn.pdf |
| ES8388 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1141/ES8388.pdf |
| ES7210 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/K128%20CoreS3/ES7210.PDF |
| NS4150B | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/atom/Atomic%20Echo%20Base/NS4150B.pdf |
| BMI270 | https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/app/Stamp%20Fly/BMI270.PDF |
| INA226 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/INA226.pdf |
| IP2326 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/IP2326.pdf |
| RX8130CE 技术手册 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/RX8130CE_cn.pdf |
| RX8130CE 寄存器手册 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/RX8130CE_cn-Register-Datasheet.pdf |
| ST7123 屏幕手册 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/ST7123_SPEC_Preliminary_V0.5.pdf |
| ST7123 触控协议 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/ST7123-TDDI-Interface-Protocol-V01.11.pdf.pdf |
| NP-F550 电池报告 | https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/K145_Battery_Product_Report.zip |

## 8. 信息来源

1. **M5Stack Tab5 官方文档**（主来源，规格/管脚映射/开发环境/坑）：https://docs.m5stack.com/zh_CN/core/Tab5 （2026-10-22 抓取）
2. **Espressif 官方 BSP espressif/m5stack_tab5**（能力矩阵/依赖/示例）：https://components.espressif.com/components/espressif/m5stack_tab5 （2026-10-22 抓取，含 v1.0.0 README）
3. **M5Stack Tab5 Keyboard 官方文档**（I2C 地址）：https://docs.m5stack.com/zh_CN/tab5/Tab5_Keyboard （2026-10-22 抓取）
4. **Espressif ESP32-C6 芯片规格**（蓝牙能力结论）：https://www.espressif.com/en/products/socs/esp32-c6
5. 社区固件仓库信息来自 GitHub 搜索（2026-10-22），星数为当日数据

> ⚠️ 本文为整理稿，动手前建议核对官方文档最新版本；管脚数据以官方文档与原理图为准。
