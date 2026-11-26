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
    刷新已建页），通知 input 任务恢复，清零并打印诊断期丢弃的事件数。
  - **返回值与失败路径**（P4-A 终审修复）：`sloop_ui_quiesce` 返回 bool。on 的
    `bsp_display_lock(1000)` 超时 → 回滚输入静默、不置 `s_quiesced`、返回 false，
    `sloop_core_selftest` 据此回 `cases=0 result=FAIL reason=quiesce` 并**不冻结引擎**
    （定时器仍在跑，冻结后它会读 base-0 步窗快照 → STEP 列错位）。off 取锁失败会重试
    3 次，仍失败则保持静默态（返回 false，可再次 `UI QUIESCE off`），绝不留下「定时器
    暂停但标志已清」的半恢复态。复验入口：`UI LOCKHOLD <ms>` 让辅助任务持 LVGL 锁，
    紧随 `ENG SELFTEST` 断言拒绝路径。
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

### 音频任务栈必须整块在内部 L2MEM（2026-11-24，P4-E 收口时定位，原 DI-40）

现象：全量 `ENG SELFTEST` 之后 `ENG AUDIO ON`，console 被饿死 5–400s，`ENG PERF` 显示
`xrun == render_blocks`、`render_mean_us ≈ 13,000`（预算 5,804）、`duty_mean ≈ 220%`，
`voices=0`，停流再启流不恢复，硬复位才好；单例自检不触发。P4-D 固件同序列 xrun 0。

定位链（每步都有设备读数）：

- **不是引擎状态**：自检后立刻在 console 任务里跑离线 `ENG PERF`，`us_mean=527`（正常 9%）；
  同一份 `sloop_core_render` 在音频任务里 13ms。
- **不是时间**：只停流 165s 不自检，再启流 xrun 0。
- **是任务上下文**：坏态下 `task_wdt` 的寄存器转储里音频任务的 SP 全在 `0x5010_9xxx`
  （`lp_ram_seg`，32KB LP SRAM），console 任务的 SP 在 `0x4ff3_xxxx`（L2MEM）；PC 落在
  `fx_buses`/`track_render`/`mix_units` 的 DSP 热循环（`-Og` 下局部变量都在栈上）。
- **机制**：P4 上 LP SRAM 以 `SOC_MEMORY_TYPE_RTCRAM` 注册为 `MALLOC_CAP_INTERNAL` 的
  **低优先级兜底堆**（IDF `heap/port/esp32p4/memory_layout.c`）；全量自检的堆泄漏（DI-26，
  P4-E 开机 68.9KB → 自检后 28.7KB；（DI-26 已于 2026-11-24 P4-F Task 2 修复：host 替身 `cpu1_host` 的懒初始化落在自检快照窗口内、每 job 回滚重建 mutex/cond，现改为快照前预初始化；`scripts/selftest-heap.py` 守住））之后 L2MEM 拿不出连续的 4KB，`xTaskCreate` 把音频
  任务的栈落到 LP SRAM——该落点下栈密集的 DSP 慢 ~24 倍（推断原因是 HP 核访问 LP SRAM 不经
  L1 cache；机制本身未单独验证，落点→变慢的因果由下面的干预实验证实）。P4-D 开机
  内部堆多 ~20KB，同样的泄漏后仍放得下，故未暴露。
- **干预验证**：把栈固定到 L2MEM 之后同一流程 `ENG AUDIO ON` 100ms 返回、xrun 0、
  `render_mean_us=526`，`stream started … stack=0x4ff2____`。

决策：`sloop_hal_audio_start()` 不再用 `xTaskCreate` 的默认内部堆，而是首次启流时
`heap_caps_malloc(AUDIO_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)`（LP SRAM 没有
DMA 能力，必然落在 L2MEM）、复位前常驻复用，配 `xTaskCreateStatic` + 静态 TCB。为什么不把
栈/TCB 直接放 `.bss`：那会让开机 `internal_min` 恒减 ~4.4KB（实测 65,888 → 61,440，正好压在
soak 的 60KB 门限上），而稳态（流式）占用两种做法相同。静态 TCB 的代价是 `stop()` 必须等
IDLE 把已删任务从待清理表摘掉（任务数减一，上限 1s）才能复用，否则下一次 start 重新初始化
TCB 后 IDLE 的 `uxListRemove` 会摘掉**新任务**的状态表项；等不到即置 `broken` 并返回
`ESP_ERR_INVALID_STATE`（与超时强删同一出口）。堆泄漏本身当时仍是 DI-26（已于 2026-11-24 P4-F Task 2 修复，回归门禁 `scripts/selftest-heap.py`）。

**由此定下的规则**：selftest TU（`selftest.c` 及其 include 的上游/host 替身）里任何带堆的懒初始化状态
（RTOS 对象、libc 缓存、「已初始化」标志 + 句柄）都必须在 `selftest_snapshot_init()` **之前**完成——
落在快照窗口里的「未初始化」标志会被 `selftest_restore()` 逐 job 回滚，变成每 job 一次的泄漏。

回归：`scripts/inject/sys.sh` ⑤（console 全量自检之后）与 ⑧（chip worker 起流之后）都断言
`ENG PERF` 的 `duty_mean ≤ 18.88%` 且 3s 窗口 xrun 增量 0；启流日志行带 `stack=<地址>`，
可直接看出落点。

### 诊断/HAL 附注：只读电源读数与任务栈（P4-F，2026-11-25）

**`power_sensor` 的只读契约**（`firmware/main/power_sensor.{c,h}`，spec A5；行为记录在
`docs/dev/device-notes.md` §10）：

- INA226（从地址 0x41）挂在 BSP 板载 I2C 总线（`bsp_i2c_get_handle()`，与 ES8388 / 触摸同
  一条；**键盘及其 RGB 不在此总线上**——Tab5 Keyboard 走自己的 G0/G1 独立总线），只在该
  总线上**懒挂一个**设备句柄（400kHz），不建第二条总线、不另加锁
  （总线自身的锁串行化各设备）。
- 驱动**只读、不写 INA226 任何寄存器**：每次读三笔、每笔一个寄存器 2 字节——0xFE 厂商 ID
  （须为 0x5449）、0x01 分流电压（LSB 2.5µV，int16）、0x02 总线电压（LSB 1.25mV）。指针
  **不自动递增**（数据手册 7.5.5.3；首版一笔 4 字节读得到 `5449 FFFF` ⇒ `ina=err`）。上电
  默认配置 0x4127 即「分流 + 总线连续转换」；校准寄存器复位为 0 ⇒ 电流寄存器 0x04 恒 0，
  故不读它。`cur_ma = shunt_uv / 5`（整数 mA），5mΩ 是官方原理图第 5 页的 R39（R005/1%），
  没有查证来源就不得换算。
- 不碰 ES8388、不持引擎锁；流式下 20 次连读 `xrun 0 → 0`。失败形态：INA226 无应答或 ID
  不符 ⇒ `ESP_ERR_INVALID_STATE` 且三字段为 `INT32_MIN` 哨兵（console 回显 `na`、
  `ina=err`）；温度读失败只把 `temp_c` 置 `na`。芯片温度走 IDF `temperature_sensor`，
  量程 −10..80℃。调用契约是**单调用者**（console 任务）；若以后让别的任务并发调用，需要给
  `ina226_ensure()` 加初始化保护。
- **测量点在电池侧（裁决 R15）**：VBUS 引脚接的不是 USB-C VBUS，而是电池侧节点
  `SYS_BAT_PRE`（`R39` 的另一端是 `SYS_BAT_2S`）。USB-C 供电且未装电池时实测
  **≈915–933mV / −1..0mA**——**USB 输入功率不可由这颗 INA226 观测**。只有装上电池后
  `vbus_mv`/`cur_ma` 才有信息（届时 `cur_ma` 为正 = 放电是按图推断、未实测，应先核对
  符号）。因此 soak 的功耗/温度两列**仅记录、不设门禁**；原理图上整机只有这一颗电流计，
  整机输入功耗需要硬件侧的其它手段。温度实测 27–28℃（轻载 + 室温），低于 spec 预期的
  35–70℃，是正常读数。

**任务栈（P4-F 终态）**：

| 任务 | 栈 | 依据 |
|---|---|---|
| audio | 4,096B，`MALLOC_CAP_INTERNAL\|MALLOC_CAP_DMA` 分配 + 静态 TCB | 本节上文（必须整块在 L2MEM） |
| LVGL（定时器拍/页面构建/全部控件回调） | **10,240B** | 裁决 R10：原 7,168B 被叠放的 `app_state_t` 副本压穿；余量经 `UI MEM lvgl_stack_free` 暴露，门禁 ≥2,048 |
| input（键盘轮询与派发） | **8,192B**（5A 起栈在 PSRAM：`xTaskCreateWithCaps`，见 ADR-002） | 裁决 R13：原 4,096B，键盘路径同样会叠副本；`KEY STATS input_stack_free` 仅在 `phys>0` 时有意义 |
| sys_selftest worker（SYSTEM 页 SELFTEST chip） | 5,632B | Task 2 由 16,384 收缩；高水位 1,484 字节（峰值 ≈4.1KB），栈落在 L2MEM |
| console REPL | 16,384B | 既有；`TAP`/`KEY` 注入在这个任务上执行，**量不到** LVGL/input 任务的栈 |

**规则**：点击路径（LVGL 控件回调）与输入路径（键盘派发）在栈上**至多持一份
`app_state_t`**（约 1.7KB；`refresh`/`sloop_ui_step_tool` 的那一份原位刷新），需要单字段时
用状态层的窄读访问器；页面**构建**函数不持整份副本。**已知例外**：SONG 页配置类 chip 的
LVGL 回调路径 `song_action → song_action_cfg → song_apply_cfg` 在栈上叠**三份**（Task 6 复审
的静态调用图 `task-6-dispatch-copypaths-{before,after}.txt`；只在 LVGL 任务、10,240B 栈上
执行，键盘 ENTER=LOAD 不走这条链）——未改，靠 `lvgl_stack_free` 的余量（实测最小 5,036B）
兜着；再加一层嵌套前先把它压平。物理点击只能进 LVGL 任务，所以
`TAP` 注入测不到这条栈——加页或加副本前先看 `lvgl_stack_free`，并留意仍未被机器验证的
输入路径（ADR-008 §8.12、ADR-006）。

## 后果

- 成品固件的诊断门（SELFTEST/AUD TONE/离线 PERF/SMOKE）保留，但都需要先停流；脚本化路径已自动化。
- 全量 SELFTEST（182 例）回到设备门禁口径：UI live 的开机态即可跑，诊断期间 UI 冻结/
  输入被丢弃、结束后自动恢复（下一拍刷新已建页；懒建见 ADR-008）；单例 `--case` 仅用于定点排障。
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
