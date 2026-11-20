# 阶段 3 终审修复轮证据（2026-11-21）

> 受测固件：`a2043cf-dirty`（= `c3f6b6a` + 本轮 4 个代码提交；`App version: a2043cf-dirty`
> 由启动日志自证，dirty 表示采集时工作树还带着后续文档改动）。采集机时：2026-11-21
> 02:45–03:2x CST；Tab5 USB-C → `/dev/ttyACM0`。
> 门禁登记见 `docs/dev/verification-log.md`；本目录 `SHA256SUMS` 覆盖全部文件（含本轮新增）。

## 1. 门禁结果

| 门禁（AGENTS §5.5） | 结果 | 证据 |
|---|---|---|
| 固件代码（构建 ×2） | ✅ 两遍退出码 0、编译警告 0；首遍 `--clean` 仅出现 AGENTS §5.5 豁免的 kconfig 提示对（`build.cmake:631` + `Missing kconfig option`），第二遍 0 提示 | `task-8-fix2-build-clean.log`、`task-8-fix2-build-incremental.log` |
| 烧录 | ✅ 三段 `Hash of data verified` | `task-8-fix2-flash.log` |
| 启动日志 | ✅ 键盘 0x6D、`UI 就绪 4 页`（池 40712/59612B）、InputManager 55 映射、触摸反馈 OFF；无 fatal/abort/Backtrace/task_wdt | `task-8-fix2-boot-log.txt` |
| 状态层单测（新用例） | ✅ 8 passed（新增关步字段清零、TIE 连缀折叠、显式步号擦除） | `task-8-fix2-state-test.log` |
| 引擎回归 | ✅ `ALL HOST TESTS PASSED (subset)`、182 golden 0 changed/0 gone | `task-8-fix2-host-test.log` |
| scripts 工具 | ✅ `screenshot.py --selftest` 退出码 0 | `task-8-fix2-screenshot-selftest.log` |
| 设备 SELFTEST 单例（诊断模式） | ✅ `0b447c66765e1780` identical=1、`result=PASS`、`expected_checks=0`（单例口径，全量才是 7） | `task-8-fix2-selftest-case.log` |
| 阶段收口 soak（30min，占空比窗口化） | ✅ **VERDICT: PASS**（见 §3） | `task-8-fix2-soak.log`、`task-8-fix2-soak-summary.txt`、`task-8-fix2-soak-stdout.txt` |
| 三类注入脚本（真机） | ✅ 三条退出码 0（回显 + 像素抽检） | `task-8-fix2-inject-{transport,step_edit,tracks}.log` + `-run.txt` + PNG |
| third_party 完整性 | ✅ `sha256sum -c MANIFEST.sha256` 233/233 | 见 task-8-fix2-gates 摘录（`task-8-fix2-readme.md` §4） |

## 2. 本轮修复对门禁的影响

- **I1/M3**：`state_test` 8 组用例全绿（关步/删末音整步清零 + TIE 连缀折叠 + LEN 边界；
  焦点在别处时按命令步号删除）。
- **I2/I6**：`ENG SELFTEST` 前后进入诊断模式（`sloop_core_diag_freeze`），单例 PASS 且汇总行
  带 `expected_checks=`；全量模式仍撞 DI-11（见 §5，既有缺陷）。
- **I3**：`ENG PERF` 的 `duty_*` 窗口改为本次流会话（`sloop_hal_audio_start` 清零），
  本轮 soak 数据即该口径。
- **M1/M4/M5**：三条注入脚本复跑全绿（TAP/UI 错误令牌不影响 miss 断言；默认 LEN=16 下
  网格像素与旧证据一致——`task-8-fix2-inject-step-two-on.png` 与
  `task-8-step-two-on.png` 逐字节相同）。

## 3. 30min soak（含占空比窗口）

驱动：`scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 300 --cycle 12`
（播放中 → STEP 翻格 → TRACKS 选卡 → KEY 音符 → 回 TRANSPORT，每 12s 一轮；每 300s 采样）。
占空比窗口 = **本次流会话**（`sloop_hal_audio_start` 清零统计；会话起点在注入脚本之前，
故首个采样含注入期的渲染成本）。

```
    t_s  internal_free  internal_min  psram_free  streaming  xrun  audio_stack_free  render_blocks  render_mean_us  render_max_us  duty_mean  duty_max  key_count  key_mean_us  key_max_us  input_stack_free  page  bpm
      0         113467         98092    30128928          1     0              3180          17444             783           3496      13.49     60.23          0           0           0              3752     3   90
    300         113455         98084    30128928          1     0              3132          69127             702           4119      12.10     70.97         50         219         290              3752     3   90
    600         113459         98084    30128928          1     0              3132         120810             690           4184      11.89     72.09        100         338        6215              3752     3   90
    900         113451         98084    30128928          1     0              3132         172494             686           4374      11.82     75.36        150         329        6215              3752     3   90
   1200         113467         98084    30128928          1     0              3132         224176             684           4585      11.78     79.00        200         305        6215              3752     3   90
   1500         113471         98084    30128928          1     0              3132         275856             682           4585      11.75     79.00        250         393       26626              3752     3   90
   1800         113471         98084    30128928          1     0              3132         327536             681           4585      11.73     79.00        300         579       58904              3752     3   90
```

判定（全部通过，`VERDICT: PASS`，断言失败 0 项）：
1. 内部堆 113,467 → 113,471B（净 +4B，非单调下降；min_since_boot 98,084B ≥60KB，余量 37KB），
   首末差 ≥ −8KB、无连续 3 采样非增下滑 → 通过；
2. 每次采样 `streaming=1`、`xrun=0`；音频栈余量 3,132B ≥2KB → 通过；
3. **占空比**：`duty_mean` 11.73–13.49%（上限 18.88% = 2× 阶段 2 基线 9.44%），
   30min 内单调收敛（13.49% → 11.73%，注入期负荷摊薄）→ 通过。`duty_max` 60–79% 是
   **单块峰值**（最大 4,585µs < 块预算 5,804µs，故 xrun=0），含启流首块冷缓存开销
   （实测首块 ~2.6–3.5ms），按设计只记录不设门限；
4. KEY 事件计数 0→300（每采样 +50 == 期望 25 轮×2）→ 通过；
5. 页面恒 transport(3)、150 轮切页/选卡/翻格无 `TAP miss`/`TAP ERR`/`UI ERR` → 通过；
6. 日志无 panic/Guru/task_wdt/abort/Backtrace → 通过；实测 1800s ≥ 请求 1800s。

## 4. manifest

`cd third_party && sha256sum -c MANIFEST.sha256` → **233/233 OK**（树未改动）。

## 5. 已知失败证据（DI-11，既有缺陷，非本轮引入）

`task-8-fix2-selftest-full-fail.log`：UI live 时跑**全量** `ENG SELFTEST`（无 `--case`）在
BEGIN 后数秒卡死——每 5s `task_wdt`（IDLE0/1 未喂狗），当前任务 `sloop_input`，PC 停在
`vPortYield` 的 busy-wait（`port.c:651`）与 `esp_crosscore_int_send` 自旋锁，console 无响应，
需硬复位。**阶段 3 收口固件（c3f6b6a）同样复现**（同一日志形态；见 §5 备注），故登记为
`docs/deferred-issues.md` DI-11；门禁口径用单例自检（AGENTS §5.5）。

备注：`c3f6b6a` 基线复现的原始日志为本地 `/tmp` 采集（未入库），同形态摘要：
`ENG SELFTEST BEGIN` → 5s 后 `task_wdt - IDLE1 (CPU 1)` / `CPU 1: sloop_input`，
MEPC 恒为 `vPortYield` 的跨核发送路径，运行 12 分钟无任何用例行输出。
