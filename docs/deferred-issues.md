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

## DI-3: DPI 帧缓冲内容错位（LVGL 局部渲染 flush 管线缺陷，屏显自 Task 2 起即错）

| 项 | 内容 |
|---|---|
| **背景** | Task 3 SCR 截屏探针发现 DPI 帧缓冲内容与逻辑画面不符（屏幕自 Task 2 点亮起就是错的，此前仅日志验证未看屏）：顶带 y<~128 基本正确、其下逐行水平错动（DMA2D 路径实测剪切 shift(y)=(-4·y) mod 128px）、底带四轨条整体错位（rotation 0 实测竖向 2:1 压缩：内容行 c 落在 py≈c/2）、py∈[644,1206) 为非图案噪声、py≥1206 未写（黑）。帧缓冲间距 1440B（720px）与导出旋转几何均已排除（raw 帧转储证实 FB 行 py=0 = 逻辑列 x=1279，与源码推导的 ROTATION_90 映射一致）。 |
| **影响面** | **屏显内容本身**：SCR 截屏忠实反映了坏帧（截图管线无责），但 Task 4 像素探针门禁无法通过、后续一切 UI 视觉验证失效。SCR 协议/传输/旋转不受影响（已独立验证）。 |
| **为何可延迟** | Task 3 核心交付（SCR 证据通道）已完成并可用；缺陷属显示组装（Task 2 域），与截屏管线可分离修复；不影响键盘/音频 bring-up 开发。 |
| **解决方向** | 已排除：DMA2D 帧拷贝（CPU memcpy 路径同样错）、LVGL 9.6 vs 9.5（pin 9.5.0 症状逐探针点相同）、PPA（未启用）、stride 对齐（CONFIG_LV_DRAW_BUF_STRIDE_ALIGN=1 无填充）、旋转几何（rotation 0 同样错）、并行 draw unit（CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1 已是 1）、TWDT/传输损坏（cmp 逐字节一致）。嫌疑收敛于 **LVGL 9.5/9.6 局部渲染切片（slice/tile）与 esp_lvgl_port 2.9.0 flush 假设的交互**（首切片正确、后续切片目的偏移漂移）。下一步：对 esp-bsp `display_rotation` 官方例程在同等 LVGL 版本下复现对照；或上报 espressif/esp-bsp issue 附 SCR 证据；或临时绕过（整帧重绘/DIRECT 模式与 sw_rotate 互斥，需改用 LVGL 自带矩阵旋转或 PPA 路线）。 |
| **触发条件** | Task 4（像素探针门禁）开工前必须解决；任何需要「屏幕内容正确」的任务同理。 |
| **登记** | 2026-11-19（阶段 1 Task 3 SCR 首次看屏发现；证据与实验矩阵见 task-3-report.md） |
