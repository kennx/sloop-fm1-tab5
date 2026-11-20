# 阶段 3 / Task 5 证据（TRACKS + STEP 两屏，步编辑闭环）

日期：2026-11-20 ｜ 受测固件提交：`1e2d5bc`（评审修复版：`lv_mem_monitor` 移入锁内 + 文案/焦点清理；首版为 `094c270` = `feat(components/sloop_ui): TRACKS 与 STEP 页（步格编辑闭环）`）
三轮采集的 A 组 PNG 逐字节相同（渲染与锁修复无关）；scratch 组中 `step-drum-msg`/`playhead-a`/`playhead-b` 因「鼓轨点击不再改焦点」而重采，见 `task-5-scratch-note.md`。

## A. 终版构型（1e2d5bc，scratch 注入已撤除，clean 树构建 + 烧录）

| 文件 | 内容 |
|---|---|
| `task-5-tracks-sel1.png` | TRACKS 页，选中轨 1（默认态）：四卡横排、轨色 outline 3px、选中卡 6px + `UI_PANEL_SEL` 底、卡内 `TRACK n / ENG k PRE p / LEVEL v / MUTE x SOLO y`、电平条 |
| `task-5-step-empty.png` | STEP 页空网格：4×16 格（行=轨色）、焦点格（0,0）白描边、顶栏 `TRACK 1 STEP 1 LEN 16 DIV 2`、列号 4/8/12/16、底部提示、鼓轨行全暗 |
| `task-5-play-regress.png` | PLAY 页回归截屏（新增 75+20 个对象后无回归）：与 Task 4 证据 `task-4-play.png` **逐字节相同**（SHA256 `bde48cc8…`） |
| `task-5-final-transcript.txt` | 串口原始 transcript：`UI`/`UI tracks`/`UI step`/`STATE SNAP`/`ENG MEM` 与三次 `--check` 抽检输出 |
| `task-5-gates.txt` | 构建 ×2（第二遍 0 警告 0 CMake 提示）、烧录三段 `Hash of data verified`、host-test / state-test / `--selftest` / manifest 233/233 |
| `task-5-boot-log.txt` | RTS 复位后 9s 启动日志全文（无 fatal/abort/Backtrace/task_wdt） |

## B. scratch 构型（点击/走带/鼓步注入临时命令，**提交前已完全撤除**）

Task 5 时 `TAP`（Task 7）与 `KEY`（Task 6）注入尚未落地，触摸点击无法机器触发。为验证
**真实控件回调接线**与截屏矩阵中的交互态，临时加了四条只用于验证的命令（均已撤除，
`grep SCRATCH(Task 5) firmware/` 为空）：

| 临时命令 | 作用 |
|---|---|
| `UI TAP card <0..3>` / `UI TAP step <r> <c>` | 向**真实卡片/格子对象**派发 `lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL)`（触摸松手时 LVGL 派发的事件，与物理点击同一条回调路径） |
| `STATE PLAY` / `STATE STOP` | 走 TRANSPORT 按钮同一条 `state_apply(CMD_PLAY/STOP)` 路径 |
| `ENG STEPW <k> <i> <n> <time>` | 直接改步的 n/time，用来构造 TIE 步（n=1,time=1）验证网格「亮」判定 |
| `ENG DRUMSTEP <idx> <lane>` + `UI TOUCH ON|OFF` | 构造鼓步聚合（MVP 无鼓步编辑入口）；采集时暂时关闭触摸 indev（见下「现场噪声」） |

| 文件 | 内容 |
|---|---|
| `task-5-tracks-sel4.png` | TRACKS 页选中轨 4（`UI TAP card 3`）：橙 outline 6px + 亮底，轨 1 回到 3px 暗底 |
| `task-5-step-two-on.png` | STEP 页两个相邻亮格（轨 1 步 2/3，蓝）+ 焦点格（步 3）白描边 + 状态行 `T1 S3` |
| `task-5-step-tie.png` | **判定口径证据**：`ENG STEPW 1 4 1 1` 造出 n=1/time=TIE 的步，网格仍判「空」（该格保持暗色），证明渲染与状态层共用 `state_step_is_on()` |
| `task-5-step-drum.png` | 鼓轨第 4 行聚合：`ENG DRUMSTEP` 在步 4/6 触发，两个橙格点亮，其余暗 |
| `task-5-step-drum-msg.png` | 点鼓轨格（`UI TAP step 3 7`）→ 不发 CMD_TOGGLE_STEP（`ENG SNAP` dstep 未变）、状态行 `DRUM EDIT = PHASE 4`、状态行像素 1054（对比空格页 0） |
| `task-5-playhead-a.png` / `-b.png` | 播放中两次截屏（间隔 0.6s）：播放头描边从第 4 列移到第 9 列（四行同步），亮格/鼓格/焦点格保持不变 |
| `task-5-scratch-transcript.txt` | scratch 构型完整 transcript：点击后的 `ENG STEP`（n/note/vel/time 前后值）、`STATE SNAP` 焦点、`ENG SNAP` dstep、48 步全量 dump（仅 (0,1)(0,2) 为 n=1，其余 n=0/time=REST） |

## 现场噪声（影响采集方式，不影响固件结论）

采集期间触摸面板间歇性上报**幽灵按压**（`sloop_display: touch (690,22)` 落在页签条上），
导致页面/焦点被意外切换、网格格被意外翻转。scratch 构型因此在采集时用 `UI TOUCH OFF`
（`lv_indev_enable(indev,false)`）关掉触摸 indev，并在结束用 48 步 dump 复核「只有预期格在响」。
终版固件不含该开关，触摸真值验证留 Task 7（`TAP` 注入 + 人测）。

## 未覆盖项

- **人眼视觉复看**（可读性、观感、选中态是否够醒目）：本会话无图像工具，机器侧用像素抽检 +
  PIL 区域统计覆盖布局/配色/文字存在性，人眼项挂账 verification-log。
- **物理触摸命中**（卡片/格子真实点击、滚动误触）：Task 7。
- **步字段保留（lvl/rat/micro/fill）的设备断言**：MVP 无这些字段的注入入口，只能靠
  `scripts/state-test.sh` 的 `case_toggle_step_preserves`（host，含 TIE 步与字段逐字节比对）。
