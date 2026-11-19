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

## 2. M5Stack Tab5 Keyboard（A164）— ⏳ 待固件验证

- 经 Tab5 **Ext.Port1** 以 I2C 连接（SDA=G0 / SCL=G1，地址 **0x6D**，INT=G50），**不是 USB 设备**——PC 侧 `lsusb` 无法枚举，连接与否只能物理确认排线。
- 程序化验证：固件 bring-up 时在启动自检中扫描 I2C 0x6D（计划项，TODO 见 `firmware/main/main.c`）。

## 3. M-VAVE FM-1（刷 SLOOP）— ❌ 未连接

- 当前 `lsusb` 无 `1209:0001` 设备；接入后应出现 MIDI 端口 "Felucca"。
- 联动模式开发期接入时在本文登记批次/固件版本（SLOOP 版本号可经编辑器协议 `INFO` 读取）。

## 4. 历史记录

| 日期 | 事件 |
|---|---|
| 2026-11-19 | 首次烧录 `sloop_fm1_tab5.bin`（骨架，git 3394356）成功；启动日志正常（v1.3/16MB/32MB PSRAM 确认） |
| 2026-11-19 | 显示点亮（git 211bee5）：ST7121 探测正常，横屏 1280×720 测试图绘制成功 |
