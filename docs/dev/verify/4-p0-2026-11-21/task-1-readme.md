# 阶段 4 P4-0 Task 1 证据：DI-9 mute/solo/selected 引擎写路径

> 受测固件：代码提交 **68d2a07**（启动日志 `App version: bbbc82e`——版本串取 clean
> 构建时的 HEAD，即本次证据提交；其代码与 68d2a07 相同）。
> 采集机时：2026-11-21 05:50–06:03 CST；Tab5 USB-C → `/dev/ttyACM0`。
> 登记见 `docs/dev/verification-log.md`；本目录 `SHA256SUMS` 覆盖全部文件（除自身）。

## 1. 门禁结果

| 门禁（AGENTS §5.5） | 结果 | 证据 |
|---|---|---|
| 状态层单测（TDD RED） | ✅ 6 passed / 2 failed（断言失败，退出码 1） | `task-1-state-test-red.log` |
| 状态层单测（GREEN ×2） | ✅ 8 passed / 0 failed，两次独立运行退出码 0 | `task-1-state-test-green1.log`、`task-1-state-test-green2.log` |
| 固件代码（构建 ×2，clean + 增量） | ✅ 退出码 0、编译警告 0；clean 首遍仅 §5.5 豁免的 kconfig 提示对，增量遍 0 提示 | `task-1-build-clean.log`、`task-1-build-incremental.log` |
| 烧录验证 | ✅ `Hash of data verified` ×3 | `task-1-flash.log` |
| 启动日志 | ✅ 无 fatal/abort/Backtrace/task_wdt；`stream started`、`UI 就绪 4 页`、`InputManager 55 条映射`、触摸反馈 OFF | `task-1-boot-log.txt` |
| 引擎/DSP 回归 | ✅ `ALL HOST TESTS PASSED (subset)`；182 golden 0 changed/0 gone | `task-1-host-test.log` |
| 设备 SELFTEST 单例 | ✅ `preset/ANALOG/00_808_BOOM 0b447c66765e1780 identical=1 result=PASS` | `task-1-device-selftest.log` |
| 行为验证（真机注入） | ✅ 17 项断言全 PASS（见 §2） | `task-1-mute-solo-transcript.log` + `task-1-mute-solo-run.txt`（断言 stdout） |

## 2. 行为验证：GLO/SEL 层 → 引擎真值

原始回显见 `task-1-mute-solo-transcript.log`（`# [di9]` 前缀为工具注记，其余为串口原样）。
注入走 `KEY <code> <down|up>`，与物理键盘同入口（ADR-006）。

| 动作 | 断言 | STATE SNAP（AppState） | ENG SNAP（引擎真值） |
|---|---|---|---|
| 基线 | mute/solo/sel 全 0 | `mute=0 solo=0` | `sel=0`、`mute=0 solo=0` |
| Alt+1（0x41/0x01）= 静音轨 0 | mute k0=1，k1 不变 | `trk k=0 mute=1` | `trk k=0 … mute=1` |
| Ctrl+2（0x40/0x02）= 选轨 1 | selected/sel=1 | `selected_track=1 focus_track=1` | `sel=1` |
| Alt+5（0x41/0x05）= 独奏轨 0 | solo k0=1 且 mute k0 仍 1 | `trk k=0 … solo=1` | `trk k=0 … mute=1 … solo=1` |
| 还原（Alt+1 / Alt+5 / Ctrl+1） | mute/solo/sel 全回 0 | `mute=0 solo=0 selected_track=0` | `sel=0`、`mute=0 solo=0` |

要点：`STATE SNAP` 与 `ENG SNAP` 对这三类值**同源**（state.c 的同步例外已删除）。
两次 `STATE SNAP` 之间隔着 30–60ms 的 `state_sync_from_engine`，值仍保持，说明
引擎写入被快照读回，而非状态层自持。

**未覆盖（人测项，非门禁）**：静音/独奏的**听感**确认（扬声器/耳机实际静音与独奏混音）
需人耳；机器侧只证到上游 `trk_silent()` 口径的输入状态（`P_MUTE`、`song.solo`）已落引擎。

> ⚠️ **人测顺序警告**：上表的 Alt+1（mute 轨 0）→ Alt+5（solo 轨 0）组合在听感上是
> **全静音**——`trk_silent()` = `P_MUTE` 置位 **或**「有独奏且本位未置」，mute 优先。
> 判断独奏混音时必须先解除 mute（Alt+1）再独奏，或对另一条轨独奏。听音项已登记
> `docs/deferred-issues.md` DI-7。

## 3. 备注

- 设备证据（flash/boot/transcript/selftest）均在最终 `build.sh --clean` + 增量构建之后
  重烧重采（构建时工作树干净），镜像即受测提交源码所构建；`Hash of data verified` ×3
  与启动日志 `App version` 自证。
- `task-1-boot-log.txt` 末尾的 `# [di9] boot capture end` 为采集工具注记。
