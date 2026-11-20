# ADR-007: 音频流生命周期 — 可停启流、开机即流、自检前置停流

> 状态：已接受（阶段 3，Task 2）｜ 日期：2026-11-20
> 关联：spec `docs/superpowers/specs/2026-11-20-phase3-mvp-ui-design.md`（DI-8 行、§4）；DI-8（`docs/deferred-issues.md`，已解决并删除）；行为记录 `docs/dev/device-notes.md` §6；脚本门禁 AGENTS §5.1/§5.5

## 背景

阶段 2 的 `sloop_hal_tab5/src/audio_stream.c` 只有 `sloop_hal_audio_start()`、无 stop 路径：`audio_task` 句柄一旦置位就永不复位，`sloop_hal_audio_streaming()` 恒为真。任何一次 `ENG TRIG`（以及阶段 3 UI 的开机启流）之后，`ENG SELFTEST`（182 例同源哈希，最强正确性门禁）、`AUD TONE`、离线 `ENG PERF`、`ENG SMOKE` 全部不可用，直到复位——「音频流单向门」（DI-8）。

阶段 3 UI 需要开机即持有音频（首屏即发声），单向门等于在出货固件里放弃设备侧自检能力。

## 选项

| 选项 | 评估 |
|---|---|
| **A. 实现 stop/start + 开机启流 + 自检前先停流（采纳）** | 诊断门在成品固件中保留；代价是新增停流状态机与脚本前置停流 |
| B. 保持单向门，SELFTEST 仅开发构建 | 出货固件失去最强门禁；违背「成品可诊断」目标 |
| C. 开机先自动跑 SELFTEST 再启流 | 全量 182 例约 316s 开机时间，不可接受 |

## 决策

A。落地形态：

- `esp_err_t sloop_hal_audio_stop(void)`：协作停流（置停止请求 → 任务在写块收尾后跳出循环、自挂起 → `vTaskDelete` → 句柄置 NULL），codec 保持打开；无任务时幂等返回 `ESP_OK`。
- `main.c` 在 `sloop_core_init()` 成功后开机启流；失败只记 ERROR 不阻断（`ENG AUDIO ON` 可重试）。
- console `ENG AUDIO [ON|OFF]`；流式时 `ENG SELFTEST` 拒绝行带 `hint=ENG AUDIO OFF`（拒绝语义不变）。
- `scripts/device-selftest.py` 自动 `ENG AUDIO OFF` → 跑 SELFTEST → 恢复 `ENG AUDIO ON`；手动路径同样先 OFF 后 ON（AGENTS §5.1/§5.5）。

实现要点（真机教训）：不可直接 `vTaskDelete` 阻塞在 `esp_codec_dev_write` 里的任务——`i2s_channel_write()` 持有驱动 `binary` 信号量，任务被删后信号量不归还，后续写报 `The channel is not enabled`（实测 AUD TONE 失败）。故必须让任务自己走出写路径再删；超时兜底（任务卡死 DMA）保留强删并显式告警 I2S 状态可能损坏。

## 后果

- 成品固件的诊断门（SELFTEST/AUD TONE/离线 PERF/SMOKE）保留，但都需要先停流；脚本化路径已自动化。
- 停流**不复位引擎运行期编辑**（无「恢复 init 快照」入口）：`ENG AUDIO OFF` 后 SNAP/注音仍是停流前状态；`ENG SELFTEST` 因自身开始/结束都 `selftest_restore()`，会抹掉运行期编辑。
- 启停调用上下文：console REPL 为常规上下文；`main.c` 开机另调一次 start，与 console 首条命令间的数十 ms 启动窗口未加锁（正常时序下 start 已返回）。
- `xrun` 为累计计数，停/启不清零；超时强删路径后需复位设备才能恢复 AUD TONE。
- 后续 UI 若接管音频所有权，应保持「同一上下文启停」并复用本 ADR 的 stop 语义。

## 未决

- `AUD TONE` 的并发拒绝日志未带 `ENG AUDIO OFF` 提示（spec §4 提及；跟踪于阶段 3 收尾清单）。
- 超时强删路径未做真机演练（仅 DMA 停摆时触发）。
