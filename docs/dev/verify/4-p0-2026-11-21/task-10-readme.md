# Task 10 — 参数页框架（旋钮页 + 布局表）

日期：2026-11-21 ｜ 阶段：P4-A ｜ 提交：`17149c1`（fmt 镜像）+ `aaed100`（参数页框架）｜
设备 app version：`aaed100-dirty`（构建时 docs 未提交；源码 = aaed100）（`task-10-boot-log.txt`）

## 1. 结论

- 三个参数页 **GLOBAL / FX / ANALOG** 落地为**描述符驱动的旋钮页**（8 槽/页，懒建）：
  页眉上下文行 + 8 旋钮（自绘弧线 + 名称/数值）+ 页眉右侧 `−`/`+` 细调。
- 编辑闭环：TAP `−`/`+`、键盘 ←/→（焦点）+ ↑/↓（调值）、物理触摸按角度粗调；
  全部经状态层 `CMD_SET_PARAM`（TRACK/ENGINE）或新增的 `CMD_SET_GLOBAL`（GLOBAL）
  到引擎，写后立即回读并刷新。切页往返后值保持（引擎真值重读）。
- 预算：每页实测池增量 **2,848–2,876 B**（est **3,584 B**）；七页全建 free **9,284 B**
  （`UI MEM`），`sum_est=50,176`。为此旋钮做成**单对象自绘**（`lv_arc`+`lv_label`
  实测 662B/旋钮会撑爆池，见 ADR-008 §8.2）。
- 页签 7 页收窄到 150px（右侧留 150px），注入脚本的页签几何/探针同步。
- 状态层新增 `CMD_SET_GLOBAL`（RED→GREEN：k=14 先 6 项失败，实现后 15 passed）。

## 2. 实现摘要

| 层 | 文件 | 内容 |
|---|---|---|
| 控件 | `components/sloop_ui/src/knob.{h,c}` | 单对象旋钮：`LV_EVENT_DRAW_MAIN` 自绘弧线（135°..405°，底部死区）+ 两行文本；`text_local=1`；触摸按角度设值（内圈 50px 不响应） |
| 布局表 | `src/page_layout.{h,c}` | GLOBAL/FX/ANALOG，8 槽 `{scope,engine,id}`；X-macro 一处生成页定义 + `ui_page_t` 注册条目（页签顺序 = 声明顺序） |
| 页 | `src/page_params.{h,c}` | 构建/刷新/TAP；值路径与焦点（临界区）；键盘路由 `sloop_ui_param_*`；引擎页不适用时锁定（`--`） |
| 框架 | `src/ui.c`、`include/sloop_ui.h`、`src/theme.h`、`src/input.c` | 注册参数页；页签收窄 + 右侧留位；TAP 派发；参数页 ←/→/↑/↓ 路由 |
| 状态 | `components/sloop_state/include/sloop_state.h`、`src/cmd.c` | `APP_STATE_PAGES=7`、`app_page_t` 增 GLOBAL/FX/ANALOG；`CMD_SET_GLOBAL` |
| 描述符 | `components/sloop_core/include/sloop_core.h`、`src/selftest.c` | `sloop_desc_fmt_t` 公开镜像 + 与上游 `F_*` 的静态断言 |
| console | `firmware/main/console.c` | 页名表 7 项；TAP `PARAM` 回显；`PARAM <id>` 只读；`PARAM GLOBAL <id> [<value>]` |
| 工具 | `scripts/inject/inject.py`、`scripts/inject/params.sh` | 页签几何/探针同步；params 场景（TAP ±/键盘/切页/池/截屏） |

页内容（策展取舍见 ADR-008 §8.1）：

- GLOBAL：BPM / SWING / CLICK / TUNE / TIME / FDBK / COLR / MIX
- FX：DST / CHO / DLY / REV / FILT / LVL / PAN / ATK
- ANALOG：WAVE / DTN / MIX / NOIS / CUT / RES / DRV / KTR（引擎 edit[0..7]）

格式化（`knob_format_value`）覆盖 F_ENUM/F_PCT/F_BIPCT/F_DB/F_TIME/F_CUTOFF/F_LFOHZ/
F_SEMI/F_BPM/F_NOTE/F_ONOFF/F_STEPS/F_SWING/F_FILT/F_INT(unit)，未知 fmt 回落原始
整数（表见 ADR-008 §8.3）。

## 3. 池预算（`UI MEM`）

| 页 | est | 实测 delta | 备注 |
|---|---|---|---|
| GLOBAL | 3,584 | 2,848 / 2,852 / 2,876 | `task-10-ui-mem-transcript.log`、params 注入日志 |
| FX | 3,584 | 2,816–2,860 | 同上（池历史不同，delta 轻微漂移） |
| ANALOG | 3,584 | 2,848–2,864 | 同上 |

- 未访问页 `state=unbuilt`、`delta=0`；七页全建 `pool_used≈49.3KB / free 9,284 B`。
- STEP（est 24,576）在参数页已默认建完之后仍可构建（free 30.5KB > est）——
  这是本任务把单页成本压到 ~2.85KB 的直接收益。

## 4. 验证矩阵

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 ×2、零新增警告 | clean rc=0 / 增量 rc=0，`warning:` 0 / 0；镜像 `0x175cd0`（82% 余量） | `task-10-build-clean.log`、`task-10-build-incremental.log` |
| 烧录 + 启动 | esptool 三段 `Hash of data verified`；`App version: aaed100-dirty`（构建时 docs 未提交；源码 = aaed100）；`UI 就绪: 7 页注册/1 已建`（池 14,404/62,940）、`InputManager 55 条映射`、内部堆 116,715B；无 fatal/abort/backtrace/task_wdt | `task-10-flash.log`、`task-10-boot-log.txt` |
| 懒建/池水位 | 逐页首次构建的 `池增量` 与 `UI MEM delta` 一致；est ≥ delta | `task-10-ui-mem-transcript.log` |
| 三页截屏 + 像素抽检（§5.4） | 每页 `screenshot.py --check` **5/5**：选中页签 accent、PLAY 页签未选中、旋钮环顶、行 1 环底缺口（页面背景）、`−` 按钮底 | `task-10-{global,fx,analog}.png`、`task-10-shot-*.log` |
| 文本渲染（视觉代理） | PNG 区域二值化可读：GLOBAL 槽 0 = `BPM` / `90BPM`；ANALOG 槽 0 = `WAVE`/`SAW`、槽 4 = `CUT`/`2.6kHz`（无图像查看工具，按像素还原字形；人眼目检为外部门禁） | 同上 PNG |
| TAP ± 编辑 | GLOBAL BPM 90→91→90（`PARAM GLOBAL 0` + `STATE SNAP bpm`）；FX LEVEL 104→105→104（`STATE SNAP param level`）；ANALOG WAVE 0→1→0（`PARAM 63`） | `task-10-inject-params.log`、`task-10-inject-params-run.txt` |
| 键盘编辑 | ←/→ 焦点（accent 像素随焦点移动：槽 0→槽 3）、↑/↓ 调值：GLOBAL SWING ±2、FX ATK ±1、ANALOG CUT ±1 | `task-10-focus-{a,b}.png`（焦点 accent 像素：槽 0→槽 3） |
| 切页保值 | ANALOG CUT=1 → PLAY → ANALOG 后 `PARAM 67` 仍 1 | `task-10-inject-params.log` |
| 编辑钳位与 desc 交叉 | 旋钮/± 走 desc 的 min/max 映射；`PARAM 0 500` → `applied=127 min=0 max=127`（P_LEVEL 与 `ENG DESC` 同源） | `task-10-probe.log`；描述符字段见 `task-9-*` DESC 证据 |
| 引擎页不适用（N/A） | `ENG TRACK 1 1 0` → 选轨 1 → ANALOG 页：页眉红字（1,104 个 UI_ERROR 像素）、旋钮 `--`、`TAP +` 回 `PARAM slot=4 locked (engine page N/A)` 且 `PARAM 67` 不变 | `task-10-locked-probe.log`、`task-10-locked-analog.png` |
| 失败可见性（Task 4 框架） | `UI FORCEFAIL global` → `state=failed` + ERROR 日志 + 错误状态条；再 `UI global` 重建成功且 BPM 值保持 91（引擎真值不变） | `task-10-probe.log` |
| 状态层单测 | RED（回退 dispatch）k=14 六项失败；GREEN×2 `15 passed, 0 failed` 且逐字节相同 | `task-10-state-test-{red,green1,green2}.log` |
| host 回归 | `ALL HOST TESTS PASSED (subset)`；182 golden 0 changed/0 gone | `task-10-host-test.log` |
| 设备 SELFTEST 单例 | `preset/ANALOG/00_808_BOOM` `identical=1 mismatch=0 result=PASS`（未改 DSP；全量 182 沿用 P4-0 Task 2） | `task-10-device-selftest.log` |
| 工具门禁 | `screenshot.py --selftest` 全过 | `task-10-screenshot-selftest.log` |
| 上游完整性 | `sha256sum -c MANIFEST.sha256` 233/233 | `task-10-manifest.txt` |
| 注入脚本（五条） | tracks 11 / transport 28 / step_edit 71 / nav 40 / params 56 PASS，0 FAIL，退出码均 0 | `task-10-inject-*.log`、`task-10-inject-*-run.txt`、PNG |

## 5. 未覆盖 / 限制

- **物理触摸拖拽**不能在注入环境复现（`TAP` 是 hit-test 注入，不是指针拖拽）。
  角度设值逻辑（`angle_to_value`）与 TAP 焦点/± 编辑共享同一状态层路径；物理触摸
  需人测（列入人测清单）。
- **人眼目检**：本会话无图像查看工具，截屏检查用像素统计/字形还原完成；最终视觉
  判断仍以人眼为准（同前序任务口径）。
- 引擎页 N/A 路径需 `ENG TRACK` 改引擎（产品 UI 暂无换引擎入口），属一次性探针证据；
  探针后已复位设备（默认 ANALOG/engine 0）。
- GLOBAL 页合并了上游 GLO>GLOBAL 与 GLO>DLY 两页；FX 页含 LEVEL/PAN/ATK（输出条）
  以便覆盖 F_DB/F_TIME 显示路径——取舍记录在 ADR-008 §8.1，非逐像素复刻上游。
- `PARAM GLOBAL` 的 id 空间是 `GP[]`，与 `PARAM <id>`（TP/引擎槽）不同；脚本按
  回显关键字区分。
