# 阶段 3 / Task 7 证据（TAP 注入 + 注入脚本 + 协议文档同步）

日期：2026-11-20/21 ｜ 受测固件提交：`9395250`（`feat(main): 串口注入 KEY/TAP/UI 与脚本化截屏序列`）。
构建时工作树含未提交文档 → 启动日志 `App version: 9395250-dirty`，固件代码即 `9395250`。

## A. 设计口径（与 ADR-005「TAP hit-test 注入」一致）

- `TAP <x> <y>`：屏幕**绝对**坐标（含 72px 页签条），console → `sloop_ui_input_tap()` →
  当前页 hit-test 表 → **与控件回调相同的动作函数**。等价性在 **EditCmd 层**
  （同一函数、同一 `state_apply` 序列），**不声称**原始指针事件等价。
- 命中矩形不另存坐标：STEP/TRACKS 由建控件的布局宏现场推导；TRANSPORT 用 `{x,y,w,h,kind,v}`
  静态表，几何取自同一组 `TRANSPORT_*` 宏。
- 未命中：打印 `TAP miss x,y page=P`，返回非零，不改状态、不崩溃。
- 切页确定性：`UI`（同步）在 LVGL 锁内作废异步切页邮箱；`KEY PANIC` 用同一专用清空；
  异步 tab「一拍一页」（同 tick 多次 tab 只前进一页）。

## B. 证据文件

| 文件 | 内容 |
|---|---|
| `task-7-build-gates.txt` | `build.sh --clean` + 第二遍均 exit 0、编译警告 0（首遍仅 §5.5 豁免的 kconfig 提示对）、镜像 0x16ff70、烧录三段 `Hash of data verified`、启动自证（9395250 / UI 就绪 / InputManager / 触摸反馈 OFF，无 fatal/backtrace）、host-test 182 golden 0 changed/0 gone、state-test 7 passed、`screenshot.py --selftest` 7 组、manifest 233/233 |
| `task-7-boot-log.txt` | RTS 复位后的完整启动日志（自证用节选见上） |
| `task-7-tap-transcript.log` | 手工覆盖会话的**原始回显**（PLAY/STEP/TRACKS/TRANSPORT 命中与 miss、鼓轨只读、切页确定性四组）；`# [inject]` 行是工具注记 |
| `task-7-tap-transcript.txt` | 同一会话的脚本断言输出（35 项 PASS / 0 FAIL） |
| `task-7-equivalence.txt` | 结构核对：`CMD_*` 在每页仅一处发放点，且回调与 `page_*_tap` 调同一动作函数 |
| `task-7-transport.log` / `-transport.png` | `transport.sh` 原始回显 + TRANSPORT 截屏（PLAYING、BPM 90） |
| `task-7-step-playhead-a.png` / `-b.png` | 播放中 STEP 两帧：播放头 4 行各 1 列，且列号与抓屏前 `ENG SNAP pos` 一致（A: pos 1/列 1，B: pos 0/列 0 —— 16 列环绕按模计，两次落点由引擎自证而非依赖间隔时长） |
| `task-7-step_edit.log` / `task-7-step-two-on.png` | `step_edit.sh`：TAP 点两格（轨 0 步 2 / 轨 1 步 5）→ `ENG STEP n=1/time=0`、焦点 (1,5)、表外 miss → 截屏（蓝格 + 绿格 + 白焦点框）；重复运行先点灭再点亮；**三次独立运行（000225 / 000756 / 000923）截屏 SHA256 全同 `fd14f09c…`** |
| `task-7-tracks.log` / `task-7-tracks-sel3.png` | `tracks.sh`：TAP 卡 2 → `selected_track=2 focus_track=2`、卡缝 miss → 截屏（6px 黄描边 + 亮底） |
| `task-7-screenshot-checks.txt` | 三条脚本的 stdout：全部回显断言 + 像素抽检（页签/按钮底/格色/焦点框/选中卡底/卡缝/播放头区域计数） |
| `task-7-crosshair.txt` + `task-7-crosshair-play.png` | 准星关闭：启动日志两行（就绪→OFF），TAP 之后 PLAY 截屏与 Task 6 证据**逐字节相同**（`bde48cc8…`），准星特征色像素 0 |
| `task-7-crosshair-session.log` | 准星核验会话的原始回显 |

## C. 现场限制

- **人眼复看**：截屏判读全部由像素抽检 + 区域计数完成（AI 会话无图像工具），观感/可读性人眼项按惯例挂账。
- **物理触摸**：未做手指按压（无人手）；等价性以「同一动作函数 + 运行期 TAP 断言」取证，
  物理触摸仍走 LVGL 回调路径（未改动）。
- **DI-10**：本会话未观察到幽灵按压干扰（`step_edit.sh` 三次独立运行截屏 SHA256 全同），根因未定性，条目保留；
  准星关闭只消除可视噪声，不修 indev 事件。
