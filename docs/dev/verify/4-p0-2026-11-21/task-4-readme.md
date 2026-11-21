# P4-0 Task 4（LVGL 页面懒建与预算框架，ADR-008）证据说明

日期：2026-11-21｜分支：`feat/phase4`｜代码提交：`e00bd6c`（`feat(components/sloop_ui): LVGL 页面懒建与预算框架（ADR-008）`）
受测固件：本任务代码（clean 构建 ×2 后在设备重烧重采；构建时 HEAD = `e00bd6c`、工作树含未提交文档，
启动日志 `App version: e00bd6c-dirty`）

## 1. 变更与语义

- `sloop_ui` 引入页面注册表（`ui_page_t` + `ui_page_register()`）：页签由注册表派生；
  PLAY 在 `sloop_ui_start()` 内立即构建（开机首帧非空白），TRACKS/STEP/TRANSPORT
  首次访问时构建、离页不销毁；30ms 定时器只刷新已建页。
- 每页 `est_bytes` 预算 + 构建前 `lv_mem_monitor()` 检查（池剩余不足即拒绝）；构建成功
  记录池 free 减少量（delta）并立即刷新首帧。
- 失败可见（不静默空白页）：`build()` 返回 NULL → 删除半成品容器 + ERROR 日志
  （含 `UI ERR page_build <id>`）+ 页面区错误状态条（`UI_ERROR` 色）；
  同步切页返回 `ESP_ERR_NO_MEM`，console `UI step` 打印 `UI ERR page_build 2`。
- 新 console 诊断面：`UI MEM`（池水位 + 每页 state/est/delta）、
  `UI FORCEFAIL <page> [on|off]`（一次性强制构建失败，验证用）。
- ADR：`docs/design/008-param-pages.md`；ADR-005「阶段 4 容量决策」已写回决策（选 A）。

## 2. 构建 / 烧录 / 启动门禁

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/build.sh --clean` | `task-4-build-clean.log` | 退出码 0；编译警告 **0**；首遍仅 §5.5 豁免的 kconfig 提示对（`build.cmake:631` + `Missing kconfig option`）；镜像 `0x171ec0`（分区余量 82%） |
| `scripts/build.sh`（增量第二遍） | `task-4-build-incremental.log` | 退出码 0；编译警告 **0**；CMake 提示 **0** |
| `scripts/flash.sh` | `task-4-flash.log` | esptool 三段 `Hash of data verified`（bootloader/partition/app） |
| 启动日志（RTS 复位后原始回显） | `task-4-boot-log.txt` | `bootstrap (IDF v5.5.5)`、`stream started`、`页面构建 page=PLAY(0) 池增量=2492B 预算=3072B 池剩余=50436B`、`UI 就绪: 4 页注册/1 已建 @1280x720, LVGL 池已用 12724/63120 字节, 内部堆剩余 118487 字节`、`InputManager 55 条映射，轮询 10ms`、`触摸反馈 OFF`；无 fatal/abort/backtrace/task_wdt/panic |
| 开机首帧（复位后未发任何 UI 命令即截屏） | `task-4-boot-frame.png` | `bde48cc8…` = 阶段 3 PLAY 基线，**非空白**；证明 PLAY 在启动内建好、首帧即完整页面 |

## 3. 懒建池水位（`UI MEM`，`task-4-ui-mem-transcript.log`）

| 时刻 | built | pool_used / pool_free / pool_total | sum_est | reserve |
|---|---|---|---|---|
| 开机（仅 PLAY） | 1 | 12,492 / 50,628 / 63,120 | 39,424 | 23,696 |
| 访问 TRACKS 后 | 2 | 17,876 / 44,648 / 62,524 | 39,424 | 23,100 |
| 访问 STEP 后 | 3 | 36,792 / 23,260 / 60,052 | 39,424 | 20,628 |
| 访问 TRANSPORT 后 | 4 | 40,124 / 19,648 / 59,772 | 39,424 | 20,348 |

每页首次构建日志（同文件）与 `UI MEM` delta：

| 页 | 日志 `池增量` | `UI MEM delta` | est | 余量 |
|---|---|---|---|---|
| PLAY | 2,492 B | 2,492 B | 3,072 B | +23% |
| TRACKS | 6,080 B | 6,080 B | 7,168 B | +18% |
| STEP | 21,760 B | 21,760 B | 24,576 B | +13% |
| TRANSPORT | 3,744 B | 3,744 B | 4,608 B | +23% |

`pool_total` 随分配略降是 `lv_mem_monitor` 口径（`total_size` 只累计块负载、不含块头），
故 delta 取 `free_size` 减少量；ADR-008 §3 已记录该方法。

## 4. 四页截屏回归（像素）

默认状态的页面（`UI <page>` 同步切页后 SCR），括号内为 SHA256 前缀：

| 页 | 本任务 | 阶段 3 对应证据 | 结论 |
|---|---|---|---|
| PLAY | `task-4-play.png` bde48cc8 | `3-mvp-2026-11-21/task-8-play.png` bde48cc8 | **逐字节相同** |
| TRACKS | `task-4-tracks.png` 9c20f58d | `3-mvp-2026-11-20/task-6-tracks.png` 9c20f58d | **逐字节相同** |
| STEP | `task-4-step.png` eaf94f73 | 无同状态的阶段 3 归档（见下） | 见 A/B |
| TRANSPORT | `task-4-transport.png` 92efd67c | `3-mvp-2026-11-20/task-4-transport.png` 92efd67c | **逐字节相同** |

STEP 说明：仓库里最新的默认 STEP 归档是 `3-mvp-2026-11-20/task-5-step-empty.png`（70383618），
早于 `a2043cf` 的底部提示文案更新；与本任务帧的差异为 **4,187 px、bbox (242,681)–(805,696)**，
全部落在提示行（`… DIM = BEYOND LEN …` 是新增文字），页面其余像素完全一致。

**同状态 A/B（改动前后固件）**：把固件源码临时回到 `5bbb325`（本任务父提交）构建烧录，
在同样状态截图，再恢复本任务源码重建重烧：

- PLAY（稳定态）`bde48cc8` == 本任务 `bde48cc8`；TRACKS `9c20f58d` == 本任务；TRANSPORT
  `92efd67c` == 本任务；**STEP `eaf94f73` == 本任务 `eaf94f73`（逐字节相同）**。
- 说明：复位后立刻截 PLAY 会得到 `a166395c`（阶段 3 `task-3-forced-retry.png` 同值），
  差异只在电平条（引擎 level 尚未非零的时序态），与本任务代码无关；稳定态同帧。

注入场景（`scripts/inject/`，退出码均 0；原始回显 + 脚本 stdout）：

| 场景 | 产物 | 与阶段 3 基线 |
|---|---|---|
| transport | `task-4-inject-transport.{log,png}` + `-run.txt` | `task-4-inject-transport.png` 9608ad10 == `3-mvp-2026-11-21/task-8-transport.png` **逐字节相同**；24 项断言 PASS |
| step_edit | `task-4-inject-step_edit.{log,png}` + `-run.txt` | `task-4-inject-step_edit.png` c717fa8c == `3-mvp-2026-11-21/task-8-fix2-inject-step-two-on.png` **逐字节相同**；16 项 PASS |
| tracks | `task-4-inject-tracks.{log,png}` + `-run.txt` | `task-4-inject-tracks.png` b0a188cf == `3-mvp-2026-11-21/task-8-tracks-sel3.png` **逐字节相同**；11 项 PASS |

## 5. 失败路径（强制注入）

`task-4-forcefail.log`（原始回显，复位后 STEP 未建时）：

```
sloop>  UI FORCEFAIL step
UI FORCEFAIL page=step (2) armed=1
sloop>  UI step
E (7267) sloop_ui: 页面构建失败 page=STEP(2) 原因=forced 池剩余=50628B 预算=24576B（UI ERR page_build 2）
UI ERR page_build 2
sloop>  UI MEM
UI MEM page id=2 title=STEP state=failed est=24576 delta=0
sloop>  TAP 172 200
TAP miss 172,200 page=2
```

- `task-4-forcefail-step.png`（1325305d）：错误状态条可见——`UI_ERROR` 色像素 3,465 个，
  bbox (440,361)–(839,415)（两行居中文本），页签条与正常帧完全相同（未残留脏区）；
  页面区无 STEP 控件（半成品已回收）。
- 失败页 TAP 不派发（`TAP miss`，不崩溃）；`UI MEM` 显示 `state=failed`。
- `task-4-forcefail-step-recovered.png`（eaf94f73）：注入一次性消费后再次 `UI step`
  构建成功，恢复帧与正常 STEP 帧**逐字节相同**；`UI MEM` 显示 `state=built delta=21776`。
- `task-4-forcefail-rebuild.log`：对**已建**页 `UI FORCEFAIL step` → 删控件、立即重建到
  失败态（同 ERROR 行）；再 `UI step` 重建成功（delta 21,784）。验证了重建路径与
  页内脏检查缓存复位（重建后首帧文案/格子状态正确，与正常帧一致）。

## 6. 回归与工具门禁

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/host-test.sh` | `task-4-host-test.log` | `ALL HOST TESTS PASSED (subset)`、182 golden 0 changed/0 gone、0 health/voice/CPU 超预算/crash |
| `scripts/state-test.sh` | `task-4-state-test.log` | `8 passed, 0 failed` / `STATE TEST PASSED` |
| `python3 scripts/screenshot.py --selftest` | `task-4-screenshot-selftest.log` | 全部断言通过（含重试/坏帧/SCR ERR 用例） |
| `third_party` 完整性 | `task-4-manifest.txt` | `sha256sum -c MANIFEST.sha256` 233/233 OK |
| 设备 SELFTEST 单例 | `task-4-device-selftest.log` | `preset/ANALOG/00_808_BOOM` → `identical=1 mismatch=0 result=PASS`（脚本自动停流/恢复；本任务未改 DSP/引擎代码，全量 182 例口径沿用阶段 2 post-fix 与 P4-0 Task 2 的原始 transcript） |

## 7. 备注

- `UI MEM` 的 `pool_total` 三个不同值（63,120 → 59,772）是 LVGL TLSF 监视口径（块负载
  之和，块头不计），不是池容量变化；池配置仍是 `CONFIG_LV_MEM_SIZE=65536`。
- 已知边界（ADR-008 §4，登记 DI-13）：`build()` 只能判定对象分配失败；标签文本缓冲等
  二级分配在 LVGL 内部无返回值，靠预算余量 + 共享 `lv_style_t` 纪律兜底。
- 失败状态条只在失败路径出现；隐藏时零像素开销，四页正常帧与阶段 3 逐字节一致即证据。
