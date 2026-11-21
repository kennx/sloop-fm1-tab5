# Task 10 — 参数页框架（旋钮页 + 布局表）

日期：2026-11-21 ｜ 阶段：P4-A ｜
提交：`17149c1`（fmt 镜像）+ `aaed100`（参数页框架）+ `23bfbd8`（评审修复：旋钮接线/相对拖拽）
设备 app version：`23bfbd8`（`task-10-boot-log.txt`）

> 本目录的日志/PNG 在评审修复轮（`23bfbd8`）整体重跑；`task-10-state-test-red.log`
> 与 `task-10-manifest.txt` 沿用前一轮（前者是 `CMD_SET_GLOBAL` 的 RED 证据，state
> 源码在修复轮未变；后者是 `third_party` 完整性校验，未改动）。

## 1. 结论

- 三个参数页 **GLOBAL / FX / ANALOG** 落地为**描述符驱动的旋钮页**（8 槽/页，懒建）：
  页眉上下文行 + 8 旋钮（自绘弧线 + 名称/数值）+ 页眉右侧 `−`/`+` 细调。
- 编辑闭环：TAP `−`/`+`、键盘 ←/→（焦点）+ ↑/↓（调值），以及**物理触摸**：
  - `PRESSED`：只设焦点 + 记录锚点角度/值，**不改值**；
  - `PRESSING`：相对锚点的角度差 → 值差（270° = 整个值域，四舍五入/钳位），
    只响应半径 55..95px 环带（中心/盒角不跳值）。
  全部经状态层 `CMD_SET_PARAM`（TRACK/ENGINE）或 `CMD_SET_GLOBAL`（GLOBAL）到引擎，
  写后立即回读；切页往返后值保持（引擎真值重读）。
- **评审修复**（`23bfbd8`）：`knob_create()` 的 memset 曾把调用方设的 `user/on_change/
  on_focus` 清掉（物理触摸完全失效）、`KNOB_CTX(0,0)` 编码成 NULL；已改为显式参数
  传入 + 上下文 +1 偏移，并用 console `UI KNOB` 合成事件在设备上证明回调接线。
  交互从「按下即按绝对角度设值」改为「按下=焦点/锚点、拖动=相对角度差」。F_TIME 秒档
  换算修正（ms10/1000 → ms10/10000），格式化改用上游生成表。
- 预算：每页实测池增量 **2,848–2,876 B**（est **3,584 B**）；七页全建 free **9,284 B**
  （`UI MEM`），`sum_est=50,176`。为此旋钮做成**单对象自绘**（`lv_arc`+`lv_label`
  实测 662B/旋钮会撑爆池，见 ADR-008 §8.2）。
- 页签 7 页收窄到 150px（右侧留 150px），注入脚本的页签几何/探针同步。
- 状态层新增 `CMD_SET_GLOBAL`（RED→GREEN：k=14 先 6 项失败，实现后 15 passed）。

## 2. 实现摘要

| 层 | 文件 | 内容 |
|---|---|---|
| 控件 | `components/sloop_ui/src/knob.{h,c}` | 单对象旋钮：`LV_EVENT_DRAW_MAIN` 自绘弧线 + 两行文本（`text_local=1`）；按下=焦点+锚点、拖动=相对角度差（环带 55..95px）；`knob_test_press/drag` 合成事件走同一路径 |
| 纯逻辑 | `src/knob_math.{h,c}` | 无 LVGL：弧线↔值、角度差归一化、相对拖拽换算、环带、格式化（上游生成表）；host 单测 `scripts/knob-test.sh` |
| 布局表 | `src/page_layout.{h,c}` | GLOBAL/FX/ANALOG，8 槽 `{scope,engine,id}`；X-macro 一处生成页定义 + `ui_page_t` 注册条目（页签顺序 = 声明顺序） |
| 页 | `src/page_params.{h,c}` | 构建/刷新/TAP/`UI KNOB` 注入；值路径与焦点（临界区）；键盘路由 `sloop_ui_param_*`；引擎页不适用时锁定（`--`） |
| 框架 | `src/ui.c`、`include/sloop_ui.h`、`src/theme.h`、`src/input.c` | 注册参数页；页签收窄 + 右侧留位；TAP/`UI KNOB` 派发；参数页 ←/→/↑/↓ 路由 |
| 状态 | `components/sloop_state/include/sloop_state.h`、`src/cmd.c` | `APP_STATE_PAGES=7`、`app_page_t` 增 GLOBAL/FX/ANALOG；`CMD_SET_GLOBAL` |
| 描述符 | `components/sloop_core/include/sloop_core.h`、`src/selftest.c` | `sloop_desc_fmt_t` 公开镜像 + 与上游 `F_*` 的静态断言 |
| console | `firmware/main/console.c` | 页名表 7 项；TAP `PARAM` 回显；`PARAM <id>` 只读；`PARAM GLOBAL <id> [<value>]`；`UI KNOB <slot> press\|drag <dx> <dy>` |
| 工具 | `scripts/inject/inject.py`、`scripts/inject/params.sh`、`scripts/knob-test.sh` | 页签几何/探针同步；params 场景（TAP ±/键盘/UI KNOB/切页/池/截屏）；旋钮纯函数 host 单测 |

页内容（策展取舍见 ADR-008 §8.1）：

- GLOBAL：BPM / SWING / CLICK / TUNE / TIME / FDBK / COLR / MIX
- FX：DST / CHO / DLY / REV / FILT / LVL / PAN / ATK
- ANALOG：WAVE / DTN / MIX / NOIS / CUT / RES / DRV / KTR（引擎 edit[0..7]）

格式化（`knob_format_value`）覆盖 F_ENUM/F_PCT/F_BIPCT/F_DB/F_TIME/F_CUTOFF/F_LFOHZ/
F_SEMI/F_BPM/F_NOTE/F_ONOFF/F_STEPS/F_SWING/F_FILT/F_INT(unit)，未知 fmt 回落原始
整数（表见 ADR-008 §8.3）。F_TIME/F_CUTOFF/F_LFOHZ/F_DB 直接用上游生成表
（`felucca_tables.h`），F_BIPCT/F_SEMI 仅 v>0 加 `+`。

## 3. 池预算（`UI MEM`）

| 页 | est | 实测 delta | 备注 |
|---|---|---|---|
| GLOBAL | 3,584 | 2,848 | `task-10-ui-mem-transcript.log`、params 注入日志 |
| FX | 3,584 | 2,816–2,860 | 同上（池历史不同，delta 轻微漂移） |
| ANALOG | 3,584 | 2,848–2,864 | 同上 |

- 未访问页 `state=unbuilt`、`delta=0`；七页全建 `pool_used≈49.3KB / free 9,284 B`。
- STEP（est 24,576）在参数页已默认建完之后仍可构建（free 30.5KB > est）——
  这是本任务把单页成本压到 ~2.85KB 的直接收益。

## 4. 验证矩阵（修复轮 `23bfbd8` 上重跑）

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 ×2、零新增警告 | clean rc=0 / 增量 rc=0，`warning:` 0 / 0；镜像 `0x176a00`（82% 余量） | `task-10-build-clean.log`、`task-10-build-incremental.log` |
| 旋钮纯函数 host 单测 | `knob-test.sh` ×2 均 `5 passed, 0 failed` 且日志逐字节相同（弧线往返/环带/角度差/拖拽换算/格式化表） | `task-10-knob-test-{1,2}.log` |
| 烧录 + 启动 | esptool 三段 `Hash of data verified`；`App version: 23bfbd8`；`UI 就绪: 7 页注册/1 已建`（池 14,404/62,940）、`InputManager 55 条映射`、内部堆 116,483B；无 fatal/abort/backtrace/task_wdt | `task-10-flash.log`、`task-10-boot-log.txt` |
| 懒建/池水位 | 逐页首次构建的 `池增量` 与 `UI MEM delta` 一致；est ≥ delta | `task-10-ui-mem-transcript.log` |
| 三页截屏 + 像素抽检（§5.4） | 每页 `screenshot.py --check` **5/5**：选中页签 accent、PLAY 页签未选中、旋钮环顶、行 1 环底缺口（页面背景）、`−` 按钮底 | `task-10-{global,fx,analog}.png`、`task-10-shot-*.log` |
| 文本渲染（视觉代理） | PNG 区域二值化可读：GLOBAL `BPM`/`90BPM`；ANALOG `WAVE`/`SAW`、`CUT`/`2.6kHz`（无图像查看工具，按像素还原字形；人眼目检为外部门禁） | 同上 PNG |
| **触摸回调接线 + 相对拖拽**（`UI KNOB`） | GLOBAL BPM 90：`press (0,-73)` → value=90（按下不改值）；`drag (73,0)` → value=157（+90°→+67）且 `PARAM GLOBAL 0`/`STATE SNAP bpm`=157；`drag (0,-73)` → value=90（回锚点）；中心 `press (0,0)`/环带外 `drag (0,-40)` → `ignored (ring band)` 且值不变 | `task-10-inject-params.log`、`-params-run.txt` |
| TAP ± 编辑 | GLOBAL BPM 90→91→90；FX LEVEL 104→105→104；ANALOG WAVE 0→1→0 | 同上 |
| 键盘编辑 | ←/→ 焦点（槽 0→槽 3）、↑/↓ 调值：SWING ±2、FX ATK ±1、ANALOG CUT ±1 | `task-10-focus-probe.txt`、`task-10-focus-{a,b}.png`（槽 0 100px accent → 槽 3 85px） |
| 切页保值 | ANALOG CUT=1 → PLAY → ANALOG 后 `PARAM 67` 仍 1 | `task-10-inject-params.log` |
| 编辑钳位与 desc 交叉 | `PARAM 0 500` → `applied=127 min=0 max=127`（P_LEVEL 与 `ENG DESC` 同源） | `task-10-probe.log`；描述符字段见 `task-9-*` DESC 证据 |
| 引擎页不适用（N/A） | `ENG TRACK 1 1 0` → 选轨 1 → ANALOG 页：页眉红字（1,104 个 UI_ERROR 像素）、旋钮 `--`、`TAP +` 回 `PARAM slot=3 locked (engine page N/A)` 且 `PARAM 67` 不变 | `task-10-locked-probe.log`、`task-10-locked-analog.png` |
| 失败可见性（Task 4 框架） | `UI FORCEFAIL global` → `state=failed` + ERROR 日志 + 错误状态条；再 `UI global` 重建成功且 BPM 值保持 91（引擎真值不变） | `task-10-probe.log` |
| 状态层单测 | GREEN×2 `15 passed, 0 failed` 且逐字节相同；RED 证据沿用（回退 `CMD_SET_GLOBAL` dispatch 时 k=14 六项失败） | `task-10-state-test-{green1,green2,red}.log` |
| host 回归 | `ALL HOST TESTS PASSED (subset)`；182 golden 0 changed/0 gone | `task-10-host-test.log` |
| 设备 SELFTEST 单例 | `preset/ANALOG/00_808_BOOM` `identical=1 mismatch=0 result=PASS`（未改 DSP；全量 182 沿用 P4-0 Task 2） | `task-10-device-selftest.log` |
| 工具门禁 | `screenshot.py --selftest` 全过 | `task-10-screenshot-selftest.log` |
| 上游完整性 | `sha256sum -c MANIFEST.sha256` 233/233 | `task-10-manifest.txt` |
| 注入脚本（五条） | tracks 11 / transport 28 / step_edit 71 / nav 40 / **params 70** PASS，0 FAIL，退出码均 0 | `task-10-inject-*.log`、`task-10-inject-*-run.txt`、PNG |

## 5. 未覆盖 / 限制

- **物理触摸手感**仍未人测（DI-16）：接线与相对拖拽换算已由 `UI KNOB` + `knob-test.sh`
  机器覆盖；真人手指在真机屏上的拖拽手感/命中率（indev 坐标映射、按压抖动）需人测。
- **人眼目检**：本会话无图像查看工具，截屏检查用像素统计/字形还原完成；最终视觉
  判断仍以人眼为准（同前序任务口径）。
- 引擎页 N/A 路径需 `ENG TRACK` 改引擎（产品 UI 暂无换引擎入口），属一次性探针证据；
  探针后已复位设备（默认 ANALOG/engine 0）。
- GLOBAL 页合并了上游 GLO>GLOBAL 与 GLO>DLY 两页；FX 页含 LEVEL/PAN/ATK（输出条）
  以便覆盖 F_DB/F_TIME 显示路径——取舍记录在 ADR-008 §8.1，非逐像素复刻上游。
- `PARAM GLOBAL` 的 id 空间是 `GP[]`，与 `PARAM <id>`（TP/引擎槽）不同；脚本按
  回显关键字区分。
- `UI KNOB` 的 drag 需要先 press（锚点）；单发 drag 回 `ignored (ring band)`（无锚点）。
