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
- **SELFTEST 是诊断模式：冻结引擎读、丢弃引擎写**（2026-11-21 修复）。终审要求「快照窗口
  对活写者安全」——UI 定时器每 30ms `sloop_core_read`、input 任务会
  `sloop_core_note/write_step`，而自检要恢复快照并跑用例。落地形态：
  - console 的 `ENG SELFTEST` 在调用前后执行 `sloop_core_diag_freeze(true/false)`：
    冻结时持引擎锁取一份快照，之后 `sloop_core_read` 返回该冻结快照、`read_step` 返回
    全零、所有改变引擎状态的 API 直接丢弃（**不取锁**）。UI 照常刷新但内容停住
    （屏幕看起来冻结），键盘编辑不发声、不落账；
  - `sloop_core_selftest()` 因此可以整段持引擎锁（restore → jobs → 结束 restore），
    而锁上**没有等待者**——这正是关键：本 FreeRTOS SMP 端口在「长时持锁 + 高优先级任务
    阻塞在该锁上」时会触发跨核 yield 风暴（`vPortYield` 的 busy-wait，IDLE 饿死、
    `task_wdt` 每 5s 刷屏、自检停在 BEGIN）。冻结让读/写都不再碰锁，既满足「快照窗口
    无写者」也避开该缺陷；
  - 冻结标志与快照放在 `.noinit`：`selftest_restore()` 会回滚组件 data/bss，普通静态
    标志会被恢复动作本身清掉（首版实现即因此失效）。
  音频按设计已在自检前停流，故不受影响。**已知残留风险**：UI live 时跑*全量* SELFTEST
  仍可能撞上同一端口的调度缺陷（阶段 3 收口固件同样复现，与本修复无关），见
  `docs/deferred-issues.md` DI-11；门禁口径用单例自检（`--case`）。
- **超时强删返回错误并锁死启流**（2026-11-21 修复）：`sloop_hal_audio_stop()` 走强制删除路径
  （任务 1s 内未退出写循环）时置 `broken` 并返回 `ESP_ERR_INVALID_STATE`（不再假装成功）；
  此后 `sloop_hal_audio_start()` 记 ERROR 并拒绝启流——I2S 驱动锁可能随被删任务丢失，
  继续启流只会产生 `codec write failed` 刷屏。恢复手段唯一：复位设备。

实现要点（真机教训）：不可直接 `vTaskDelete` 阻塞在 `esp_codec_dev_write` 里的任务——`i2s_channel_write()` 持有驱动 `binary` 信号量，任务被删后信号量不归还，后续写报 `The channel is not enabled`（实测 AUD TONE 失败）。故必须让任务自己走出写路径再删；超时兜底（任务卡死 DMA）保留强删并显式告警 I2S 状态可能损坏。

## 后果

- 成品固件的诊断门（SELFTEST/AUD TONE/离线 PERF/SMOKE）保留，但都需要先停流；脚本化路径已自动化。
- 停流**不复位引擎运行期编辑**（无「恢复 init 快照」入口）：`ENG AUDIO OFF` 后 SNAP/注音仍是停流前状态；`ENG SELFTEST` 因自身开始/结束都 `selftest_restore()`，会抹掉运行期编辑。
- 启停调用上下文：console REPL 为常规上下文；`main.c` 开机另调一次 start，与 console 首条命令间的数十 ms 启动窗口未加锁（正常时序下 start 已返回）。
- `xrun` 为累计计数，停/启不清零；渲染耗时统计（`duty_mean/duty_max`）窗口 = 本次流会话
  （`sloop_hal_audio_start()` 清零），首块含冷缓存/首次渲染开销（实测 ~4.3ms）计入峰值。
- 超时强删路径后 `sloop_hal_audio_stop()` 返回 `ESP_ERR_INVALID_STATE`、`start()` 拒绝启流，
  需复位设备；正常路径 start/stop 幂等语义不变。
- 后续 UI 若接管音频所有权，应保持「同一上下文启停」并复用本 ADR 的 stop 语义。

## 未决

- `AUD TONE` 的并发拒绝日志未带 `ENG AUDIO OFF` 提示（spec §4 提及；跟踪于阶段 3 收尾清单）。
- 超时强删路径未做真机演练（仅 DMA 停摆时触发；返回码/启流拒绝已按上文落地，但强制路径的
  真机行为——含 `broken` 置位后的 `ENG AUDIO ON` 拒绝回显——仍未实测）。
