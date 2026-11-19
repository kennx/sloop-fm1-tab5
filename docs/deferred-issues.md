# 延迟处理问题清单（deferred issues）

> **性质**：已知但可延迟的缺口/风险收件箱。**条目解决后即从本文档删除**（若涉及方案选型，先沉淀为 `docs/design/` 的 ADR 再删条目）。
> **新增时机**：调研或开发中发现"现在不修、但不该被遗忘"的问题时立即登记（一行命令的事，换的是跨会话不丢失）。
> **每条必填**：背景 / 影响面 / 为何可延迟 / 解决方向 / **触发条件**（何时必须处理）。
> 维护规则已纳入 AGENTS.md §6 文档维护协议。

---

## DI-1: USB-MIDI host 类驱动缺失（联动模式核心通道）

| 项 | 内容 |
|---|---|
| **背景** | ADR-001 依赖核验发现：`usb_host_uac` 1.5.0 仅覆盖 UAC1 音频采集，**不含 USB-MIDI host 类**。FM-1 是组合 USB 设备（MIDI + UAC1），Tab5 做 host 需各自驱动。 |
| **影响面** | **联动模式全部控制面**：编辑器协议 77 命令下发、CHANGED/RELOAD/STEP_CHANGED/TRACK_CHANGED push 上行（双向同步骨架）、LIVE 遥控、LIVE 页普通音符（按轨通道）——全部走 USB-MIDI，缺驱动则一个字节都发不出。音频采集（usb_host_uac）不受影响；**独立模式零影响**。 |
| **为何可延迟** | 开发顺序独立模式先行（显示/键盘 bring-up → sloop_core → UI），不碰 USB host；且技术可行性与工作量已知可控（见下）。 |
| **解决方向** | 新建 `usb_fm1` 组件时先写 **ADR-002** 选型：(a) 基于 ESP-IDF 原生 `usb_host` 栈自写 MIDI 1.0 类驱动——bulk IN/OUT + 4 字节事件包，SysEx 重组参考 Sloop Go `MidiEngine.kt` 与 `docs/research/editor-protocol.md` §8；(b) tinyusb host 路线（MIDI host 成熟度**待查证**）；(c) 社区组件（registry 再查一轮）。**先例**：[fm1-cartridge](https://github.com/andrerezemini/fm1-cartridge)（ESP32-S3 对 FM-1 做 USB-MIDI host + 回传音频）证明链路可行，驱动实现可参考。 |
| **触发条件** | 进入联动模式开发（`usb_fm1` 组件立项）之前必须解决；在此之前仅按需更新本条目认知。 |
| **参考** | `docs/design/001-idf-version.md`（缺口首次记录）、`docs/research/editor-protocol.md` §8、`docs/research/sloop-fm1-go.md` §3、`docs/dev/device-notes.md` §3（FM-1 接入状态） |
| **登记** | 2026-11-19（代码评审会话） |

## DI-2: sdkconfig.defaults 变更不生效（旧生成物 sdkconfig 掩盖）

| 项 | 内容 |
|---|---|
| **背景** | `firmware/sdkconfig`（gitignored 生成物）存在时，改 `sdkconfig.defaults` 后重新构建**不会**应用新默认值——Task 1 实测：分区表自定义项被静默忽略，首次构建仍用旧 1MB 表。须删除 `firmware/sdkconfig` 触发重建。 |
| **影响面** | 所有改 `sdkconfig.defaults` 的任务（阶段 1 已知 Task 3 console 通道切换；后续阶段 Kconfig 变更同理）。症状是「构建成功但配置未生效」，易误判为配置写错。 |
| **为何可延迟** | 手动规避只需一行 `rm firmware/sdkconfig`；无功能缺陷，纯开发环境效率/正确性陷阱。 |
| **解决方向** | `scripts/build.sh` 在 defaults 比 sdkconfig 新时自动删除重建（或至少显著警告）；实现时注意保留用户在 sdkconfig 中的有意覆盖（本项目策略=defaults 为唯一权威，覆盖不入库，直接重建即可）。 |
| **触发条件** | 下一次任何 `sdkconfig.defaults` 变更任务（Task 3）前不修则必须手动规避；顺手修进 build.sh 更佳。 |
| **登记** | 2026-11-19（阶段 1 Task 1 实测） |
