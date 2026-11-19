# ADR-001: ESP-IDF 版本选择 — v5.5.5（本机已装）

> 状态：已接受 ｜ 日期：2026-11-19 ｜ 决策人：会话协商
> 关联：AGENTS.md §4 开发环境

## 背景

初始化 `firmware/` ESP-IDF 工程（target `esp32p4`），需确定 IDF 版本基线。本机已装 **v5.5.5**（`/home/kenn/esp/esp-idf`）。

## 选项

| 选项 | 评估 |
|---|---|
| **A. v5.5.5（本机）** | 5.5 为当前稳定线；P4 支持经 5.4 之后多轮成熟（USB OTG / MIPI-DSI / PSRAM）；关键依赖全部兼容（见下） |
| B. v5.4.x（BSP 声明的最低线） | 无收益：5.4 是 P4 首个稳定版，5.5 修复更多；本机亦未安装 |
| C. 升级 v6.x / master | 非稳定线，BSP 未声明支持，不引入 |

## 依赖兼容性核验（2026-11-19，components.espressif.com）

| 组件 | 版本 | 约束 | 结论 |
|---|---|---|---|
| espressif/m5stack_tab5（BSP） | 1.3.3 | IDF ≥5.4，target esp32p4 | ✅ 5.5.5 满足 |
| espressif/usb_host_uac（FM-1 音频采集，联动模式） | 1.5.0 | 支持 esp32p4 | ✅ |
| esp_lvgl_port ^2 / esp_codec_dev ~1.5 / esp_video ~2.6（BSP 传递依赖） | — | 随 BSP 解析 | ✅ 由 BSP 保证 |
| 上游 sloop 核心 | 2.8.5 | 纯 C，无 IDF 约束 | ✅ 无反向限制 |

## 决策

**采用本机 v5.5.5**，`firmware/CMakeLists.txt` 注释声明最低线 5.4，`idf_component.yml` 锁 BSP `^1.3.3`。

## 后果

- 已知缺口：`usb_host_uac` 仅 UAC1 音频、**不含 USB MIDI host 类**——FM-1 的编辑器协议（USB-MIDI SysEx）需要另找组件（如 esp_midi 系）或基于 esp-usb host 栈自写 MIDI 类驱动（bulk 端点 + 4 字节事件包，协议简单，工作量可控）。作为独立设计课题（未来 ADR）处理，不阻塞当前版本决策。
- 若后续某依赖明确要求 >5.5 或仅支持 5.4，回到本 ADR 修订。
- 升级 IDF 前必须重跑 §5.5 门禁（构建 + selftest + 真机烧录冒烟）。
