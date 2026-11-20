# 阶段 4 P4-0 Task 2 证据：DI-11 全量 SELFTEST 与 UI 共存（quiesce 流程）

> 受测固件：代码提交 **fb24ecc**（启动日志 `App version: fb24ecc-dirty`；`-dirty`
> 只来自构建时尚在暂存区外的 docs 变更——AGENTS §5、ADR-007、deferred-issues 与
> 本目录证据，固件源文件与 fb24ecc 一致）。
> 采集机时：2026-11-21 06:35–06:52 CST；Tab5 USB-C → `/dev/ttyACM0`。
> 登记见 `docs/dev/verification-log.md`；本目录 `SHA256SUMS` 覆盖全部文件（除自身）。

## 1. 结论

全量 `ENG SELFTEST`（182 例）在 **UI live 的开机态**下通过，诊断期 UI/LVGL 定时器
暂停、KEY/TAP 注入被拒，结束后 UI 恢复、走带/触发/内存全部正常。

```
ENG SELFTEST cases=182 identical=182 mismatch=0 health_fail=0 checks=7 check_fail=0 deterministic=1 expected=182 expected_checks=7 main_rc=1 result=PASS
```

- 实测时长 **156.5s**（修复前 input 任务空转吃满一核时约 316s）。
- 诊断期日志：`诊断期静默 ON：UI 定时器暂停、LVGL 定时器停用、输入丢弃生效` →
  `ENG SELFTEST BEGIN` → … → `诊断期静默 OFF：UI 定时器恢复、输入处理恢复（丢弃 0 条键盘事件）`。

## 2. 门禁结果

| 门禁（AGENTS §5.5） | 结果 | 证据 |
|---|---|---|
| 固件代码（构建 ×2，clean + 增量） | ✅ 退出码 0、编译警告 0；clean 首遍仅 §5.5 豁免的 kconfig 提示对，增量遍 0 提示 | `task-2-build-clean.log`、`task-2-build-incremental.log` |
| 烧录验证 | ✅ `Hash of data verified` ×3 | `task-2-flash.log` |
| 启动日志 | ✅ 无 fatal/abort/Backtrace/task_wdt；`App version: fb24ecc-dirty`、`stream started`、`UI 就绪 4 页`、`InputManager 55 条映射，轮询 5ms（1 tick）` | `task-2-boot-log.txt` |
| 引擎/DSP：全量 SELFTEST + UI live（quiesce 流程） | ✅ 182/182、checks=7、deterministic=1、result=PASS（156.5s） | `task-2-selftest-full-uiquiesce.log`（原始回显，`# [inject]` 前缀为工具注记） |
| 门禁脚本 `device-selftest.py`（默认全量，fresh boot） | ✅ 退出码 0：`identical=182 missing=0 extra=0 result=PASS`；脚本自动 `ENG AUDIO OFF/ON` | `task-2-device-selftest.log`、`task-2-device-selftest-transcript.log` |
| 诊断期注入拒绝 + 恢复 | ✅ `UI QUIESCE on` → `KEY ERR quiesced`（`KEY STATS quiesce_dropped=1`）、`TAP ERR quiesced`；`UI QUIESCE off` → KEY 正常、`UI page=play` | 同上 transcript 尾部 |
| 回归与工具门禁 | ✅ host `ALL HOST TESTS PASSED (subset)`、182 golden 0 changed/0 gone；state-test 8 passed；`screenshot.py --selftest` 7 组；third_party manifest **233/233 OK** | `task-2-host-test.log`、`task-2-state-test.log`、`task-2-screenshot-selftest.log`、`task-2-manifest.txt` |

## 3. 运行中/运行后现场

全量运行期间从同一串口队列里排队的两条无害命令（`KEY STATS`、`UI`）在诊断结束后
按序执行——console 会话未受影响、无丢字节：

```
KEY STATS count=2 mean_us=61 max_us=108 input_stack_free=3784 quiesce_dropped=0
UI page=play (0)
```

运行后（同一 transcript）：

| 检查 | 结果 |
|---|---|
| `STATE SNAP` | page=0、focus 0/0、layer 0、selected_track=0、bpm=90、playing=0，四轨 mute/solo 全 0 |
| `ENG SNAP` | sel=0、playing=0、四轨 engine=0 preset=0 len=16、dstep 全 0（自检结束 `selftest_restore()` 回到 init 快照） |
| `ENG TRIG synth 0 60` | `duration_ms=500 xrun=0 OK`（音频流可重启，无 stuck note：随后 `KEY PANIC` 释放 0 个音符） |
| `ENG MEM` | `internal_free=73507 internal_min=73500`（≥60KB 门限，余量 ≈11.8KB；含为 TRIG 启流后的占用） |
| UI 响应 | `UI page=play (0)`；`UI QUIESCE off` 后 KEY 注入立即生效（`elapsed_us=81`） |

## 4. 已知现象：诊断期一次 `task_wdt`

transcript 第 142 行有一条 `task_wdt`（`- IDLE0`、`CPU 0: console_repl`），发生在
运行末尾单个 golden job 连续占用 console 任务 >5s 时（运行仍在 1.3s 后给出 PASS
汇总）。它与 DI-11 的卡死形态不同：当前任务就是 console 本身、任务在推进、summary
随后出现。登记为 DI-12（诊断期单 job 超 WDT 窗口，非卡死）。

## 5. DI-11 归档（历史失败证据）

- `task-2-quiesce-only-wedge-fail.log`：本轮**只加 quiesce（暂停 UI 定时器/输入丢弃）
  但未修零 tick 自旋**时的全量运行失败原始回显——`ENG SELFTEST BEGIN` 后 4.7s 起
  `task_wdt` 刷屏，`CPU 1: sloop_input` / `PC 停在 vPortYield`，与 DI-11 记录一致。
  该证据推翻了「长时持锁 + 高优先级等待者」的旧归因。
- `task-2-di11-wedge-history.log`：阶段 3 收口期的长时卡死原始回显（`/tmp` 留存件，
  234 次 WDT 转储；固件版本串未记录 = 证据缺口）。阶段 3 目录
  `3-mvp-2026-11-21/task-8-fix2-selftest-full-fail.log` 是同一缺陷的另一份（含
  `ENG AUDIO OFF` / `BEGIN` 上下文）。
- 基线固件（c3f6b6a）复现日志：`/tmp` 中**没有**独立留存件；该次复现只在 DI-11
  条目与阶段 3 验证记录中有文字记录（证据缺口，随 DI-11 删除一并归档于此）。

## 6. 定位过程（供后续参考）

1. 只加 quiesce → 仍卡死（`task-2-quiesce-only-wedge-fail.log`）。
2. 只加冻结（无 quiesce）/ quiesce + 暂停 LVGL 定时器 + input 自挂起 → 仍卡死。
3. 加临时探针（job 进度 / quiesce 各阶段）缩小到「cleanup 阶段或任意 yield」；
   寄存器现场反复落在 `vPortYield` 的 `crosscore_int_ll_get_state()` busy-wait 与
   `xPortEnterCriticalTimeout(reason_spinlock)`。
4. 读 `input.c`：`vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS))`，`INPUT_POLL_MS=5`，
   `CONFIG_FREERTOS_HZ=100` → **`pdMS_TO_TICKS(5)==0`** → input 任务全速自旋，
   每圈自发跨核 IPI。改为 tick 下限 1（10ms）后：单例压力连跑 **80/80 无 WDT**，
   全量 182 例 156.5s PASS。
5. 结论：DI-11 的根因是零 tick 延时（轮询周期），冻结/quiesce/短锁的价值是
   「诊断期快照一致 + UI 明确暂停」，不是回避持锁缺陷。
