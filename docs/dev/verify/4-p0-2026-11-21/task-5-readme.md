# P4-0 Task 5（步窗抽象 `{bank, window_start, window_len}`）证据说明

日期：2026-11-21｜分支：`feat/phase4`
受测固件：本任务源码 clean 构建 ×2 后重烧重采（构建时 HEAD = `be38fd0`、工作树含
未提交的本任务源码，启动日志 `App version: be38fd0-dirty`）
门禁结果：**全部通过**（state-test RED→GREEN×2、build×2、flash/boot、host-test、
设备 SELFTEST 单例、manifest 233/233、三条注入脚本）

## 1. 变更与语义

- 状态层新增 `step_window_t { bank, window_start, window_len }`（`sloop_state.h`）：
  **UI 局部态**，引擎步号恒为绝对索引 0..63。默认 `{0,0,16}`；`CMD_SET_WINDOW
  {a=bank(冗余), b=start, v=len}` 落地，`step_window()` 读副本。钳位：start 向下
  16 对齐、上限 48（bank = start/16）、len 1..16、focus_step 夹到窗口内。
- 绝对步号映射只此一处：`step_window_abs(win, col) = window_start + col`。STEP 页
  渲染、TAP 命中、输入层 EDIT 擦除都调它（不各自算偏移）。
- `app_state_t.core.step[k][c]` / `dstep[c]` 的语义改为**窗口列投影**（= 引擎绝对步
  `window_start+c`）。引擎快照只带 16 列，故新增 `sloop_core_read_window(start, out)`：
  一次引擎锁内按窗口基填 16 列（`sloop_core_read` = 基 0 的同一条路径）。每拍 sync
  仍只取一次引擎锁（不逐列 `read_step`，避免 4×5.8ms 的锁等待）。
- STEP 页（`page_step.c`）：列数 = `window_len`，窗口外的格/列号标签隐藏（TAP 命不中）；
  列号改为**单行绝对步号**（旧实现每行重复建同一位置的标签，本次改为 16 列各一个，
  对象数不变）；顶栏加 `WIN a..b`、`STEP`/焦点显示绝对步号；播放头把绝对
  `step_pos` 映射进窗口，落在窗口外（别的 bank 在播）不显示残留描边；格仍按
  「绝对步 ≥ 本轨 LEN」暗显。
- console：`UI WINDOW [<start> <len>]`（无参打印当前窗口；回显落地值供脚本派生几何）；
  `STATE SNAP` 增加 `window_bank/window_start/window_len`。
- 注入（`scripts/inject/inject.py`）：STEP 几何由布局宏派生（`step_cell_xy`），列语义
  对齐 `STATE SNAP` 的窗口；`step_edit` 场景扩为「默认窗口两格 → bank1 绝对步 16 →
  window_len=8 隐藏列/命中 → 播放头窗口映射」，脚本可重复（先归一窗口、清理临时步）。
  `scripts/soak.py` 前置归一步窗到 0/16（窗口是持久 UI 状态）。

## 2. state-test RED→GREEN

| 步骤 | 文件 | 结果 |
|---|---|---|
| RED（新用例对旧实现） | `task-5-state-test-red.log` | gcc 退出码 1：`unknown type name 'step_window_t'`、`implicit declaration of function 'step_window'`（用 HEAD 版 state 源码/头 + 本任务新 `state_test.c` 复现） |
| GREEN #1 | `task-5-state-test-green1.log` | 9 passed, 0 failed；新用例 k=7「步窗抽象：默认/映射/投影/钳位」PASS |
| GREEN #2 | `task-5-state-test-green2.log` | 同上（与 #1 逐字节相同） |

用例覆盖：默认 {0,0,16}；列→绝对步映射（0→0、15→15、bank1 0→16、15→31）；
`CMD_SET_WINDOW{b=16,v=16}` 后窗口投影取引擎步 16..31（含鼓轨 `dstep`）；命令写绝对
步（列 5 → 绝对 21，bank0 同列不动）；窗口外绝对步可写但不进投影；`sync` 用**一次**
窗口快照并把鼓轨/步表拉进窗口列、窗口本身不被 sync 改；钳位（start 17→16、64→48、
len 0→1、99→16、focus 夹到窗口内）。

## 3. 构建 / 烧录 / 启动门禁

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/build.sh --clean` | `task-5-build-clean.log` | 退出码 0；编译警告 **0**；首遍仅 §5.5 豁免的 kconfig 提示对；镜像 `0x172690`（分区余量 82%） |
| `scripts/build.sh`（增量第二遍） | `task-5-build-incremental.log` | 退出码 0；警告/CMake 提示均为 **0** |
| `scripts/flash.sh` | `task-5-flash.log` | esptool 三段 `Hash of data verified` |
| 启动日志（RTS 复位后原始回显） | `task-5-boot-log.txt` | `App version: be38fd0-dirty`、`UI 就绪: 4 页注册/1 已建`（池 12,724/63,120 B）、`InputManager 55 条映射`、stream started；无 fatal/abort/backtrace/task_wdt/panic |

## 4. 设备注入与视觉证据（`task-5-inject-step_edit.log` 为串口原始回显）

> 文件沿革：主日志 `task-5-inject-step_edit.log` 的原始内容（本任务首跑，含 STEP 首次构建
> `池增量=21832B`）在修复轮被第二跑覆盖，首跑已按原字节归档为
> `task-5-inject-step_edit-run1.log`（8e7dd62f…，与 `f313d2c` 清单一致）；主日志现为修复轮
> 重跑（见 verification-log 的两行登记）。

`scripts/inject/step_edit.sh` 退出码 0：**71 项 PASS / 0 FAIL**。三条注入脚本均通过
（`task-5-inject-{transport,tracks}-summary.log` 退出码 0、0 FAIL）。

| 断言/抽检 | 证据 | 结果 |
|---|---|---|
| 默认窗口归一 | 回显 `UI WINDOW bank=0 start=0 len=16`；`STATE SNAP ... window_start=0 window_len=16` | PASS |
| bank1 绝对步号 | `UI WINDOW 16 16` → TAP 列 0 → `-> STEP track=0 step=16`；`ENG STEP 0 16 n=1 time=0`；`focus_step=0`、`window_start=16` | PASS |
| bank1 列号 17..32 + 顶栏 `WIN 17..32` | `task-5-inject-step_edit-step-bank1.png` | 列号标签逐列像素阈值 ASCII 复原：列 0=`17`、列 1=`18`、列 15=`32`；顶栏 ASCII 复原 `TRACK 1   STEP 17   LEN 16   DIV 2   WIN 17..32`；bank0 的步 2/5 格在 bank1 窗口中隐藏（像素=页面背景） |
| bank1 亮格（DIM） | 同上 | 列 0 亮格像素 `(24,52,96)` ≈ 蓝 45%（绝对步 16 ≥ 默认 LEN=16）；列 1 空位 `(16,16,16)` ≈ `UI_PANEL_HI` 45% |
| window_len=8 | `UI WINDOW 0 8`；列 8 TAP → `TAP miss 705,208 page=2`；列 7 TAP → 绝对步 7，`ENG STEP 1 7 n=1`；`task-5-inject-step_edit-step-len8.png` | PASS；列 8..15 隐藏（列号文本像素=0、格=页面背景），可见列标签 1..8 |
| 播放头映射 | `task-5-inject-step_edit-step-bank0-playhead.png`（播放中 bank0）→ 4 行播放头列均 `[4]`；`task-5-inject-step_edit-step-bank1-no-playhead.png`（播放中切 bank1）→ 4 行均无强调色描边 | PASS；窗口外播放头不显示残留 |
| 可重复性 | `task-5-fix-step_edit-run2.log`（第二次运行，原始回显） | 退出码 0（71 PASS）；两次运行 5 张 PNG 逐字节相同（确定性），见 §7 |

> 视觉复核口径：本会话无图像查看工具，PNG 的人工目检按 AGENTS §5.4(2) 仍为外部
> 人测项。本次用像素探针 + 列号区域二值化 ASCII 复原（可读出 17..32/1..8）+ 顶栏
> 文本复原作为机器侧证据；布局/配色/无脏区的整体目检须人眼复看已入库 PNG。

## 5. 回归与工具门禁

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/host-test.sh` | `task-5-host-test.log` | `ALL HOST TESTS PASSED (subset)`；182 golden 0 changed/0 gone |
| 设备 `ENG SELFTEST sends/ANALOG/dry` | `task-5-device-selftest.log` | `cases=1 identical=1 mismatch=0 result=PASS`（本任务未改 DSP；全量 182 口径沿用 P4-0 Task 2 transcript） |
| `scripts/screenshot.py --selftest` | `task-5-screenshot-selftest.log` | 全部断言通过 |
| manifest | `task-5-manifest.txt` | `sha256sum -c` 233/233 OK（third_party 未改） |
| 池预算 | STEP 首次构建日志 `池增量=21832B 预算=24576B`（`task-5-inject-step_edit-run1.log` 8e7dd62f…，首跑归档件） | delta ≤ est 成立；ADR-008 表已补注 |

## 6. 已知限制 / 后续

- 默认 LEN=16 时 bank1 的绝对步 16..31 在「LEN 之外」，页面按既有 dim 口径显示暗色
  （编辑与 ENG 真值不受影响）。LEN 编辑属 P4-C，届时 bank1 可显示亮格。
- `sloop_core_read_window` 在诊断冻结期与 `read` 同口径返回冻结快照（基 0）；UI 本就在
  quiesce 中暂停，不影响门禁。
- 步窗是持久 UI 状态：注入/soak 脚本进入 STEP 前须归一到 0/16（step_edit/transport/soak
  均做；transport 在评审后续修复中补上）。

## 7. 评审后续修复（2026-11-21，Task 5 修复轮）

| 项 | 改动 | 证据 |
|---|---|---|
| Important 1：transport 未归窗 | `run_transport()` 在 `UI step` 前 `wlen = ensure_window(s, 0, 16)`（落窗 + STATE SNAP 断言）；播放头扫描/期望列/探针区域全部按 `wlen` 几何（`playhead_columns(img, wlen)`） | 先把窗口弄脏：`task-5-fix-transport-dirty-window-pre.txt`（`UI WINDOW 16 16` → `window_bank=1 window_start=16 window_len=16`）；随后 `task-5-fix-transport-dirty-window.log` 原始回显含 `UI WINDOW 0 16` 归一，播放头 4 行断言 PASS，脚本退出码 0。AGENTS §5.4.1 措辞改为「step_edit/transport/soak 均做」 |
| Important 2：soak.py 无法启动（DI-14） | `inject.Session(args.port, log_path, out_dir, stamp, "soak", timeout=3.0)`（Session 自建 LoggedSerial）；删除本地 `LoggedSerial` 构造与 `import serial`；日志注记改 `s.ser.note()`；保留 Task 5 的 `UI WINDOW 0 16` 前置 | `python3 scripts/soak.py /dev/ttyACM0 --duration 120 --interval 60 --cycle 12` → `VERDICT: PASS`：`task-5-fix-soak.log`、`task-5-fix-soak-summary.txt`（3 采样/120s，内部堆 113,283→113,279B、xrun=0、duty_mean 最大 11.10% ≤ 18.88%、无 panic/WDT）。DI-14 已删除 |
| Minor 3：readme 顶栏文本 | 实际像素为 `TRACK 1`（焦点轨 0），readme §4 已改正；`inject.py` 锚点注释写明**主题 hex 经 RGB565 量化**（`UI_PANEL_HI 0x242B33=(36,43,51) → (32,40,48)`），容差 10–16 覆盖 ≤4/通道量化差，锚点值不按 hex 改 | 本文 §4；`inject.py` 锚点块注释 |
| Minor 4：`playhead_columns` 参数未用 | transport 传入真实 `wlen`；期望列 `% wlen`、探针区域 `88 + wlen*73`。step_edit 的播放头扫描在归一到 0/16（以及 bank1 的 16/16）后按 16 列几何扫描（`playhead_columns` 默认参数；`window_len` 同为 16，故未显式传参） | `task-5-fix-transport-dirty-window.log` |
| Minor 5：sync 与 SET_WINDOW 交错 | `state_sync_from_engine` 取快照后**复核窗口起点**，变了按新窗口重取一次（最多 2 次尝试），不再把旧基投影写回（EDIT 擦除读该投影） | host 新用例 k=8 `case_window_sync_race`（stub 钩子在第一次 `read_window` 内落 `CMD_SET_WINDOW 16/16`）：`read_window` 共 3 次、写回列 0=绝对 16、列 4=绝对 20、旧基未混入；`task-5-fix-state-test.log` 10 passed（`task-5-fix-state-test-green2.log` 与其逐字节相同） |
| Minor 6：`state_set_focus` 文档 | 头文件注明 step 是窗口列、须 `< window_len`，窗口外被忽略 | `sloop_state.h` |
| Minor 7：ADR-005 STEP 描述 | 改为「4 行 × 步窗网格（列数=window_len，列 c=绝对步 window_start+c）」，列号/顶栏绝对、窗口外隐藏、播放头映射、dim 口径 | `docs/design/005-ui-architecture.md` |
| Minor 8：step_edit 可重复性claim | 第二次运行原始回显入库 `task-5-fix-step_edit-run2.log`；两次运行 5 张 PNG 逐字节相同（确定性） | `task-5-fix-step_edit-run2.log` + `task-5-fix-step_edit-run2-summary.log` |

修复轮回归：`task-5-fix-state-test.log`（10 passed）、`task-5-fix-host-test.log`
（ALL HOST TESTS PASSED）、`task-5-fix-build-clean.log` / `task-5-fix-build-incremental.log`
（exit 0，编译警告 0；clean 首遍仅豁免的 kconfig 提示对）、`task-5-fix-flash.log`
（三段 Hash verified）、`task-5-fix-boot-log.txt`（`App version: 28774c0-dirty`，无 fatal）。
`task-5-manifest.txt` 重采 233/233。
