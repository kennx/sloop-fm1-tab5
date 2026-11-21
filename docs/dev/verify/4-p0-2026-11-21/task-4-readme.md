# P4-0 Task 4（LVGL 页面懒建与预算框架，ADR-008）证据说明

日期：2026-11-21｜分支：`feat/phase4`
提交：`e00bd6c`（feat 代码）、`f4773fd`（评审修复轮代码）；文档/证据提交见 `verification-log.md`
受测固件：`f4773fd` 源码（clean 构建 ×2 后在设备重烧重采；构建时 HEAD = `1a8c8a6`、工作树含
未提交的修复源码，启动日志 `App version: 1a8c8a6-dirty`）

## 1. 变更与语义

- `sloop_ui` 引入页面注册表（`ui_page_t` + `ui_page_register()`）：页签由注册表派生；
  PLAY 在 `sloop_ui_start()` 内立即构建（开机首帧非空白），TRACKS/STEP/TRANSPORT
  首次访问时构建、离页不销毁；30ms 定时器只刷新已建页。
- **注册表只增不删**：外部组件可在 start 之前预注册页面；内置注册幂等（`static done`），
  不会清空预注册项（评审修复；验证见 §2.1）。
- 每页 `est_bytes` 预算 + 构建前 `lv_mem_monitor()` 检查（池剩余不足即拒绝）；构建成功
  记录池 free 减少量（delta，最近一次成功构建）并立即刷新首帧。
- 失败可见（不静默空白页）：`build()` 返回 NULL → 删除半成品容器 + ERROR 日志
  （含 `UI ERR page_build <id>`）+ 页面区错误状态条（`UI_ERROR` 色）；
  同步切页返回 `ESP_ERR_NO_MEM`，console `UI step` 打印 `UI ERR page_build 2`。
- console 诊断面：`UI MEM`（池水位 + 每页 state/est/delta）、`UI FORCEFAIL <page> [on|off]`
  （强制构建失败，回显注入后的 `state=`）；`UI` 的错误令牌新增
  `UI ERR not_registered <id>`（页 id 合法但未注册）与既有 `page_build`/`lock_timeout` 分道。
- 输入层 tab 轮转按 `sloop_ui_page_count()`（注册表计数），与页签数量同源。
- ADR：`docs/design/008-param-pages.md`；ADR-005「阶段 4 容量决策」已写回决策（选 A）。

## 2. 构建 / 烧录 / 启动门禁

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/build.sh --clean` | `task-4-build-clean.log` | 退出码 0；编译警告 **0**；首遍仅 §5.5 豁免的 kconfig 提示对；镜像 `0x172090`（分区余量 82%） |
| `scripts/build.sh`（增量第二遍） | `task-4-build-incremental.log` | 退出码 0；编译警告 **0**；CMake 提示 **0** |
| `scripts/flash.sh` | `task-4-flash.log` | esptool 三段 `Hash of data verified` |
| 启动日志（RTS 复位后原始回显） | `task-4-boot-log.txt` | `bootstrap (IDF v5.5.5)`、`stream started`、`页面构建 page=PLAY(0) 池增量=2492B 预算=3072B`、`UI 就绪: 4 页注册/1 已建 @1280x720, LVGL 池已用 12724/63120 字节`、`InputManager 55 条映射，轮询 10ms`、`触摸反馈 OFF`；无 fatal/abort/backtrace/task_wdt/panic |
| 开机首帧（复位后未发任何 UI 命令即截屏） | `task-4-boot-frame.png` | `bde48cc8…` = 阶段 3 PLAY 基线，**非空白**；PLAY 在启动内建好、首帧即完整页面 |

## 2.1 评审修复轮验证

| 修项 | 证据 | 结果 |
|---|---|---|
| **Important 1**：预注册页不被内置注册清空 | `task-4-fix-regtest-boot.log`、`task-4-fix-regtest-tabs.png` | 临时构型（已还原）：`APP_STATE_PAGES=5`，`main.c` 在 `sloop_ui_start()` **之前**从组件外部 `ui_page_register({id=4,"REGTEST"})` → 启动日志 `REGTEST: 预注册外部页 -> ESP_OK`、`UI 就绪: **5 页注册**/1 已建`、`UI MEM ... registered=5` 且 `page id=4 title=REGTEST state=unbuilt`；截屏页签条为 5 个（REGTEST 在首位＝注册顺序，PLAY 选中，accent bbox x=200–375 即第 2 个页签）。临时补丁已 `git checkout` 还原，最终固件注册=4 |
| **Minor 7**：tab 轮转按注册表计数 | `task-4-fix-tab-rotation.log` | `KEY 0x20 down/up` ×2：`UI page=play (0)` → `tracks (1)` → `step (2)`（`KEY` 注入与物理同入口） |
| **Minor 4**：FORCEFAIL 回显与实际一致 | `task-4-forcefail.log`、`task-4-forcefail-rebuild.log`、`task-4-fix-forcefail-off.log` | 未建页：`UI FORCEFAIL page=step (2) on state=unbuilt`（挂起）；已建页：`… on state=failed`（注入当场消费、立即重建到失败态）；`on` 后 `off` 再访问 → 正常构建（`state=built`），即 off 只撤销未消费注入 |
| **Minor 8**：未注册页令牌 | 代码核对（`console.c` 分支） | `sloop_ui_switch_page()` 返回 `ESP_ERR_INVALID_ARG` → `UI ERR not_registered <id>`（与 `not_started` 分道）。正常构型不可达（四页都注册），故仅静态核对 |
| **Minor 6**：start 失败路径悬空指针 | 代码核对（`ui.c` 失败分支） | 清树后同时复位 `s_page_error`/`s_layer_hint`/`s_page_obj`/`s_page_state` |
| **Minor 3**：delta 语义 | ADR-008 §3、`sloop_ui.h` | 注释改为「最近一次成功构建」并注明采样于首帧 `update()` 之前（不含首帧文本分配） |

**Important 2**（登记措辞）：`verification-log.md` 的 STEP 断言已改正，并补 `task-4-ab-5bbb325-*.png`
为 A/B 证据（见 §4）；父提交固件截图由临时 `git stash` 构建烧录采集，随后还原重建重烧。

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
故 delta 取 `free_size` 减少量；ADR-008 §3 已记录该方法与采样点。

## 4. 四页截屏回归（像素）

默认状态的页面（`UI <page>` 同步切页后 SCR），括号内为 SHA256 前缀：

| 页 | 本任务 | 对照 | 结论 |
|---|---|---|---|
| PLAY | `task-4-play.png` bde48cc8 | `3-mvp-2026-11-21/task-8-play.png` bde48cc8 | **逐字节相同** |
| TRACKS | `task-4-tracks.png` 9c20f58d | `3-mvp-2026-11-20/task-6-tracks.png` 9c20f58d | **逐字节相同** |
| STEP | `task-4-step.png` eaf94f73 | `task-4-ab-5bbb325-step.png` eaf94f73（父提交同状态 A/B） | **逐字节相同** |
| TRANSPORT | `task-4-transport.png` 92efd67c | `3-mvp-2026-11-20/task-4-transport.png` 92efd67c | **逐字节相同** |

- 阶段 3 没有与当前提示文案同状态的默认 STEP 归档：仓库中最新的默认 STEP 证据
  `3-mvp-2026-11-20/task-5-step-empty.png`（70383618）早于 `a2043cf` 的底部提示文案更新，
  与本任务帧的差异为 **4,187 px、bbox (242,681)–(805,696)**，全部落在提示行
  （`… DIM = BEYOND LEN …` 为新增文字），页面其余像素完全一致；不是本任务引入。
- **同状态 A/B**：把固件源码临时回到父提交 `5bbb325` 构建烧录，在同样状态截图，再恢复
  本任务源码重建重烧。四张父提交固件截图已入库：`task-4-ab-5bbb325-{play,tracks,step,transport}.png`
  （bde48cc8 / 9c20f58d / eaf94f73 / 92efd67c，与本任务逐字节相同）。
- 说明：复位后**立刻**截 PLAY 会得到 `a166395c`（阶段 3 `task-3-forced-retry.png` 同值），
  差异只在电平条（引擎 level 尚未非零的时序态）；稳定态同帧 `bde48cc8`，故 A/B 采用稳定态帧。

注入场景（`scripts/inject/`，退出码均 0；原始回显 + 脚本 stdout）：

| 场景 | 产物 | 与阶段 3 基线 |
|---|---|---|
| transport | `task-4-inject-transport.{log,png}` + `-run.txt` | png 9608ad10 == `task-8-transport.png` **逐字节相同**；24 项断言 PASS |
| step_edit | `task-4-inject-step_edit.{log,png}` + `-run.txt` | png c717fa8c == `task-8-fix2-inject-step-two-on.png` **逐字节相同**（编辑态，不是默认 STEP 帧）；16 项 PASS |
| tracks | `task-4-inject-tracks.{log,png}` + `-run.txt` | png b0a188cf == `task-8-tracks-sel3.png` **逐字节相同**；11 项 PASS |

## 5. 失败路径（强制注入）

`task-4-forcefail.log`（原始回显，复位后 STEP 未建时）：

```
sloop>  UI FORCEFAIL step
UI FORCEFAIL page=step (2) on state=unbuilt
sloop>  UI step
E (7278) sloop_ui: 页面构建失败 page=STEP(2) 原因=forced 池剩余=50628B 预算=24576B（UI ERR page_build 2）
UI ERR page_build 2
sloop>  UI MEM
UI MEM page id=2 title=STEP state=failed est=24576 delta=0
sloop>  TAP 172 200
TAP miss 172,200 page=2
```

- `task-4-forcefail-step.png`（1325305d）：错误状态条可见——`UI_ERROR` 色像素 3,465 个，
  bbox (440,361)–(839,415)（两行居中文本），页签条与正常帧完全相同（无残留脏区）；
  页面区无 STEP 控件（半成品已回收）。两次独立构建截屏 hash 相同（确定性）。
- 失败页 TAP 不派发（`TAP miss`，不崩溃）；`UI MEM` 显示 `state=failed`。
- `task-4-forcefail-step-recovered.png`（eaf94f73）：注入消费后再次 `UI step` 构建成功，
  恢复帧与正常 STEP 帧**逐字节相同**；`UI MEM` 显示 `state=built delta=21776`。
- `task-4-forcefail-rebuild.log`：对**已建**页 `UI FORCEFAIL step` → 回显
  `on state=failed`（删除控件、立即重建到失败态）；再 `UI step` 重建成功（delta 21,784），
  验证重建路径与页内脏检查缓存复位。
- `task-4-fix-forcefail-off.log`：未建页注入后 `off` → `state=unbuilt`，随后 `UI step`
  正常构建（无 ERROR），验证 off 只撤销未消费注入。

## 6. 回归与工具门禁

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/host-test.sh` | `task-4-host-test.log` | `ALL HOST TESTS PASSED (subset)`、182 golden 0 changed/0 gone、0 health/voice/CPU 超预算/crash |
| `scripts/state-test.sh` | `task-4-state-test.log` | `8 passed, 0 failed` / `STATE TEST PASSED` |
| `python3 scripts/screenshot.py --selftest` | `task-4-screenshot-selftest.log` | 全部断言通过（含重试/坏帧/SCR ERR 用例） |
| `third_party` 完整性 | `task-4-manifest.txt` | `sha256sum -c MANIFEST.sha256` 233/233 OK |
| 设备 SELFTEST 单例 | `task-4-device-selftest.log` | `preset/ANALOG/00_808_BOOM` → `identical=1 mismatch=0 result=PASS`（本任务未改 DSP/引擎，全量 182 例口径沿用阶段 2 post-fix 与 P4-0 Task 2 transcript） |

## 7. 备注

- `UI MEM` 的 `pool_total` 三个不同值（63,120 → 59,772）是 LVGL TLSF 监视口径（块负载
  之和，块头不计），不是池容量变化；池仍是 `CONFIG_LV_MEM_SIZE=65536`。
- ADR-008 背景的 40,532/59,632 是**阶段 3 收口归档**启动行（`3-mvp-2026-11-20/task-7-boot-log.txt`）；
  40,712/59,612 是父提交（`4-p0-2026-11-21/task-2-boot-log.txt`）；40,124/59,772 是本任务
  懒建全建后重测——三组数不是同一固件/时点，ADR 已分别标注。
- 已知边界（ADR-008 §4，登记 DI-13）：`build()` 只能判定对象分配失败；标签文本缓冲等
  二级分配在 LVGL 内部无返回值，靠预算余量 + 共享 `lv_style_t` 纪律兜底；错误状态条自身
  文本分配失败时只剩 ERROR 日志 + console 令牌（非静默成功）。
- 失败状态条只在失败路径出现；隐藏时零像素开销，四页正常帧与阶段 3 逐字节一致即证据。
