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
  - `sloop_core_selftest()` 在首尾成对执行 `sloop_core_diag_freeze(true/false)`：
    冻结时持引擎锁取一份快照，之后 `sloop_core_read` 返回该冻结快照、`read_step` 返回
    全零、所有改变引擎状态的 API 直接丢弃（**不取锁**）。UI 刷新内容停住、键盘编辑
    不发声、不落账；
  - 冻结让快照恢复与用例运行期间没有其它读者/写者取引擎锁，因此自检**不再整段持锁**：
    restore 只在首尾各取一次短锁（长时间持锁 + 每次 job 后的 `vTaskDelay` 没有收益，
    只增加跨核调度交错的面）；
  - 冻结标志与快照放在 `.noinit`：`selftest_restore()` 会回滚组件 data/bss，普通静态
    标志会被恢复动作本身清掉（首版实现即因此失效）。
  音频按设计已在自检前停流，故不受影响。
- **诊断期 quiesce：全量 SELFTEST 与 UI live 共存**（阶段 4 P4-0 Task 2，DI-11 收口）。
  自检现成对执行 `sloop_ui_quiesce(true)` → `sloop_core_diag_freeze(true)` → 跑用例 →
  freeze off → `sloop_ui_quiesce(false)`，且只有一条出口：
  - quiesce(on)：先置输入丢弃标志，等 input 任务回到「批次之间」（≤50ms，超时记 WARN；
    置标志时它可能正派发一批事件），随后 input 任务阻塞等恢复通知；再在 LVGL 锁内
    `lv_timer_pause` 30ms 定时器、作废待执行切页，并 `lvgl_port_stop()` 停掉 LVGL 的
    全部定时器（刷新/indev）——LVGL 任务转入 500ms 级休眠。此后物理键盘事件只计数，
    TAP/KEY 注入回 `TAP ERR quiesced` / `KEY ERR quiesced`（不派发）。
  - quiesce(off)：`lvgl_port_resume()` + `lv_timer_resume`（下一拍重新 `state_sync` 并
    刷新四页），通知 input 任务恢复，清零并打印诊断期丢弃的事件数。
  - 构型：`sloop_core` 不依赖 `sloop_ui`（依赖方向相反），只提供
    `sloop_core_diag_quiesce_hook()`；`sloop_ui_start()` 成功后注册 `sloop_ui_quiesce`。
    钩子指针与冻结标志同放 `.noinit`（注册发生在 init 快照之后，普通静态会被自检开头的
    快照回滚抹成 NULL，结束时 quiesce(false) 就永远不会执行）。UI 未启动 = 空操作，
    门禁脚本（`device-selftest.py` 默认全量）无需感知 UI 状态。
  - 人工复验入口：console `UI QUIESCE <on|off>`（与自检内部同一函数）。

### DI-11 真因（2026-11-21 阶段 4 定位）

DI-11 原先归因于「长时持引擎锁 + 高优先级等待者」触发 FreeRTOS SMP 端口的跨核
yield 风暴。**该归因不成立**：阶段 4 实测显示只加 quiesce（暂停 UI 定时器/输入派发）
与只加冻结都仍会在 BEGIN 后数秒卡死；加临时探针后定位到：

- `firmware/components/sloop_ui/src/input.c` 的轮询周期写作
  `vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS))`，`INPUT_POLL_MS = 5`；本工程
  `CONFIG_FREERTOS_HZ=100` → `pdMS_TO_TICKS(5) == 0` → **`vTaskDelay(0)` 只 yield
  不阻塞**。input 任务因此退化为全速自旋：每圈都 `vPortYield()` →
  `esp_crosscore_int_send_yield()` 抢全局 `reason_spinlock` 并自发跨核 IPI。
- 该 IPI 风暴与 UI/DSP 并发时，某个核的 IPI 得不到服务（`vPortYield` 的
  `crosscore_int_ll_get_state()` busy-wait 不退出 / 另一核在 `xPortEnterCriticalTimeout`
  里等同一把 `reason_spinlock`），IDLE 饿死、`task_wdt` 每 5s 刷屏、console 无响应。
  症状与 DI-11 的原始记录一致。
- 修复：轮询周期取 tick 下限 1（`INPUT_POLL_TICKS`，100Hz 下即 10ms）。修复后
  80 轮单例自检连跑 0 次 WDT；全量 182 例与 UI live 共存通过。
- 结论：quiesce / 冻结 / 短锁仍是诊断期的正确形态（保证快照一致性、UI 明确暂停），
  但**卡死的根因是 input 任务的零 tick 延时**，与持锁策略无关。
- **超时强删返回错误并锁死启流**（2026-11-21 修复）：`sloop_hal_audio_stop()` 走强制删除路径
  （任务 1s 内未退出写循环）时置 `broken` 并返回 `ESP_ERR_INVALID_STATE`（不再假装成功）；
  此后 `sloop_hal_audio_start()` 记 ERROR 并拒绝启流——I2S 驱动锁可能随被删任务丢失，
  继续启流只会产生 `codec write failed` 刷屏。恢复手段唯一：复位设备。

实现要点（真机教训）：不可直接 `vTaskDelete` 阻塞在 `esp_codec_dev_write` 里的任务——`i2s_channel_write()` 持有驱动 `binary` 信号量，任务被删后信号量不归还，后续写报 `The channel is not enabled`（实测 AUD TONE 失败）。故必须让任务自己走出写路径再删；超时兜底（任务卡死 DMA）保留强删并显式告警 I2S 状态可能损坏。

## 后果

- 成品固件的诊断门（SELFTEST/AUD TONE/离线 PERF/SMOKE）保留，但都需要先停流；脚本化路径已自动化。
- 全量 SELFTEST（182 例）回到设备门禁口径：UI live 的开机态即可跑，诊断期间 UI 冻结/
  输入被丢弃、结束后自动恢复（下一拍刷新四页）；单例 `--case` 仅用于定点排障。
- 诊断期间 UI 显示停在最后一帧、键盘编辑不落账（既有冻结语义），这是「UI 共存」的
  定义——UI 存在且状态一致，但不要求渲染与自检并发。
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
