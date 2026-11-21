# P4-0 Task 6（nav.sh 四导航回归用例）证据说明

日期：2026-11-21｜分支：`feat/phase4`
受测固件：`a9a2380`（Task 5 评审修复后的 HEAD，本任务**未改固件**，构建/烧录沿用该版本）
本任务只改脚本与文档：新增 `scripts/inject/nav.sh` + `inject.py` 的 `nav` 场景，
`AGENTS.md` §3/§5.1/§5.4.1/§5.5 同步登记。

## 1. 用例设计（四条导航行为）

来源：阶段 3 终审点名的四条行为——它们是阶段 4 导期重构（router）最容易静默破坏的点。

| # | 用例 | 断言（全 PASS 才算通过） |
|---|---|---|
| ① | 页签条内、页签矩形外的 TAP | `TAP miss 196,36 page=2`；`STATE SNAP` 11 个字段前后逐字段一致；`ENG SNAP` 21 行全量一致 |
| ② | STEP 鼓轨行 TAP（行 3 = DRM） | 回显 `-> STEP drum step=3 read-only` 且回显无 `CMD_TOGGLE_STEP`；`focus_track=2`/`focus_step=2` 不变、`STATE SNAP` 全字段不变；`ENG SNAP` 21 行全量不变（无引擎写）；鼓格像素区（70×108，格 67×104 外扩 2px）点击前后**逐字节相同**；状态行橙字 0→396（`DRUM EDIT = PHASE 4`）、蓝字 78→0 |
| ③ | 同一 30ms 拍内 `KEY tab` + `KEY PANIC` | 锚点 tab 前进一页（PLAY→TRACKS）；批次 [tab, PANIC, UI] 的**批内 UI 仍 TRACKS**（PANIC 执行时请求尚未被拍消费）；6 拍后仍 TRACKS；再 6 拍仍 TRACKS（无回跳）；截屏 TRACKS 页签选中、PLAY 未选中 |
| ④ | 同一 30ms 拍内 3 次 `KEY tab` | 锚点 tab 前进一页；批次 [tab×3, UI] 的**批内 UI 仍 TRACKS**（3 次按下之间无拍执行）；6 拍后 STEP（只前进一页）；再 6 拍仍 STEP（无第二次前进）；截屏 STEP 页签选中、TRANSPORT 未选中 |

几何来源：页签缝坐标由 `theme.h` 的 `UI_TAB_X0/W/GAP/Y/H` 派生（`tab_gap_xy()`，
页签 i 与 i+1 之间 8px 缝中心 = 196,36）；STEP 格坐标由 `step_cell_xy()` 派生。
两处都与页面布局同源，页面改布局时脚本按同名宏同步（AGENTS §5.4.1）。

### 1.1 「同一拍」怎么测（本任务的方法与实测依据）

单条 console 命令的慢路径往返 ~50ms（`read_response` 的 `read(256)` 在无数据时
阻塞到 timeout），逐条发送必然跨拍；因此：

1. **批写**：`Session.cmd_batch()` 把多行一次性写入串口（行间无主机延迟），
   `in_waiting` 快速读取（~1ms 往返）数满 N 个 `sloop>` 提示符。实测批内 3 行
   ~16–20ms（`# [inject] ③/④ 对齐 …` 注记）。
2. **锚点**：`nav_anchor()` 单按 tab 并快速轮询 `UI` 到页翻转，把「刚过一拍」作为
   批次起点（拍下界 = 最后一次读到旧页的主机时刻）。
3. **批内自证**（关键）：用例③/④ 在批次末尾追加 `UI`。若 tab 与 PANIC / 3 次 tab
   之间被一拍插入，页面会立即前进，批内 `UI` 读到的就不是起点页——前提不成立，
   工具侧**重新对齐**（日志注记 `[NOTE]`，最多 5 次），不判固件失败；只有前提成立
   后的页面断言才计入 PASS/FAIL。**断言不放宽**：前提成立后页面不符即 FAIL。
4. **观察窗**：6 拍（`0.5s`）后判定一次，再 6 拍复判（覆盖实测 ~270ms 的拍迟滞）。

拍迟滞实测（对齐注记 `tab 写入→翻转 …`）：30ms 定时器的执行间隔多数 <40ms，
但**偶有 170–270ms**（LVGL 任务被渲染/刷新拖住；`lv_timer` 只会晚不会早，故锚点
下界仍成立）。这也是引入「批内自证 + 重对齐」而不是纯时序判断的原因。

### 1.2 自证器的灵敏度（反证：它确实会开火）

`task-6-detector-probe.txt`：**不做锚点**（随机相位）跑 40 次
`[tab, PANIC, UI]` 与 40 次 `[tab×3, UI]`，统计批内 UI 的火警率与误放率
（同法共跑两轮，合计 80+80 组；下表为两轮合并）：

| 批次 | 批内 UI 报警（前提不成立） | 前提成立但页面错 |
|---|---|---|
| tab+PANIC | 1/80 | 0 |
| tab×3 | 6/80 | 0 |

即随机相位下真实跨拍率 ~1–7%（拍周期 30ms、批长 ~16–20ms，且批内首行在写入后
~1–2ms 才被派发；单轮 40 组的小样本波动见 `task-6-detector-probe.txt`：pair 1/40 +
0/40，3tab 2/40 + 4/40）。改为「批内自证 + 重对齐」之后，9 次真机运行（含本任务
3 次入库）**0 次报警、0 FAIL**；160 组探针在前提成立时页面结果全部正确。
（迭代中发现的过程事实：最早的版本用纯时序条件判前提——「批次结束 < 拍下界 + 30ms」，
该下界在拍迟滞 ~100ms 时过于保守，曾把 1 次真机运行误判为「前提不成立」；
改为批内 UI 自证后不再依赖该时序判断，仅保留其数值作为日志注记。）

## 2. 真机运行结果（v 1.2 节方法，`nav.sh`）

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/inject/nav.sh /dev/ttyACM0`（第 1 次） | `task-6-inject-nav.log`（原始回显）+ `task-6-inject-nav-run.txt` | 退出码 **0**：39 PASS / 0 FAIL / 0 NOTE |
| 第 2 次（可重复性） | `task-6-inject-nav-run2.log` + `task-6-inject-nav-run2.txt` | 退出码 **0**：39 PASS / 0 FAIL / 0 NOTE |
| 第 3 次（可重复性） | `task-6-inject-nav-run3.log` + `task-6-inject-nav-run3.txt` | 退出码 **0**：39 PASS / 0 FAIL / 0 NOTE |
| 截屏（第 1 次） | `task-6-inject-nav-0{1..5}-*.png` | 5 张；三次运行**逐字节相同**（SHA256 见 §4） |

截屏语义：`nav-01-tabbar`（用例①后，STEP 页 + 页签条）、`nav-02-drum-before`
（用例②点击前，焦点已归一到轨 2/列 2，状态行蓝字 `T1 S3`）、`nav-03-drum-readonly`
（鼓轨点击后：状态行橙字、鼓格与 `nav-02` 的鼓格区逐字节相同）、`nav-04-panic-final`
（用例③终态 TRACKS）、`nav-05-tab3-final`（用例④终态 STEP）。`nav-05` 与 `nav-03`
逐字节相同是**预期**（同一页/同一焦点/同一状态行），用例④的判据是 `UI` 页断言与
页签像素探针。

**可重复性归一（脚本可重复的关键）**：状态行是 STEP 页的局部控件、跨运行残留，
因此脚本开头先对合成轨格 TAP 两次（落回原值、净零引擎写），把状态行文本从上一轮
必然残留的 `DRUM EDIT = PHASE 4` 换成 `T1 S3`；否则第二次运行起「状态行变橙」不可判、
PNG 也不确定。用例②的焦点也由方向键归一到 (2,2)（方向键只改 UI 局部焦点、不发引擎写）。

## 3. 回归（既有三条脚本 + 工具门禁）

| 项 | 文件 | 结果 |
|---|---|---|
| `scripts/inject/tracks.sh` | `task-6-inject-tracks.log` + `-run.txt` | 退出码 0，11 PASS / 0 FAIL |
| `scripts/inject/transport.sh` | `task-6-inject-transport.log` + `-run.txt` | 退出码 0，28 PASS / 0 FAIL |
| `scripts/inject/step_edit.sh` | `task-6-inject-step_edit.log` + `-run.txt` | 退出码 0，75 PASS / 0 FAIL |
| `scripts/screenshot.py --selftest` | `task-6-screenshot-selftest.log` | 退出码 0，`selftest OK` |

本任务未改固件（无 build/flash/selftest 门禁）；`inject.py` 的改动是**新增**快速
读取/批写方法与 nav 场景，既有场景走的仍是原 `cmd()` 路径，上表三脚本即为回归证据。

## 4. 视觉复核口径

本会话无图像查看工具，PNG 的人工目检按 AGENTS §5.4(2) 仍为外部人测项。机器侧证据：
- 页签选中/未选中：`(104/288/472/656, 36)` 四个页签中心的 RGB565 像素（选中 = UI_ACCENT
  (224,176,40)，未选中 = UI_PANEL_HI (32,40,48)）。
- 状态行 `DRUM EDIT = PHASE 4`：右上状态行区域 `(1040,82)-(1258,112)` 的橙字像素
  0→396、蓝字 78→0（数值判定，不依赖字形识别）。
- 鼓轨只读：鼓格矩形外扩 2px 的区域点击前后**逐字节相同**（不点亮、不换焦点描边）。

## 5. 文件与哈希

变更：
- 新建 `scripts/inject/nav.sh`（入口，source `scripts/env.sh`、自动探测端口）
- 编辑 `scripts/inject/inject.py`（`LoggedSerial.in_waiting`；`Session._read_prompts/
  cmd_fast/cmd_batch/page_fast`；`tab_gap_xy`；`count_region`/`nav_state_fields`/
  `nav_eng_snapshot`/`nav_same`/`nav_set_focus`/`nav_anchor`/`nav_last_page`/`run_nav`；
  场景表加 `nav`；模块 docstring「三条 → 四条」）
- 编辑 `AGENTS.md`：§3 目录树、§5.1 脚本表（nav 行）、§5.4.1 场景脚本 + 「同拍语义」
  条目、§5.5 注入脚本门禁（三条 → 四条）

证据文件与 SHA256 见同目录 `SHA256SUMS`（`task-6-*` 行）与本任务报告。
