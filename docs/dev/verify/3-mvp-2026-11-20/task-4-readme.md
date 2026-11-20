# 阶段 3 / Task 4 证据（sloop_ui 骨架 + PLAY/TRANSPORT）

日期：2026-11-20 ｜ 受测固件提交：见 `docs/dev/verification-log.md` 同批登记行

| 文件 | 内容 |
|---|---|
| `task-4-play.png` | PLAY 页截屏（`SCR`，1280×720）：页签条、TRACK/ENGINE/PRESET、电平条、NOTE 指示盒 |
| `task-4-transport.png` | TRANSPORT 页截屏：PLAY/STOP 大按钮、状态文本、步位置、BPM 显示与 −/+ 按钮 |
| `task-4-tracks-placeholder.png` / `task-4-step-placeholder.png` | TRACKS/STEP 占位页（`UI tracks`/`UI step` 切页无崩溃，选中页签高亮随页移动） |
| `task-4-screenshot-checks.txt` | 四页 `screenshot.py --check` 像素抽检（PLAY 5/5、TRANSPORT 6/6、占位页各 3/3）+ 区域分析（文字/按钮实心色/电平条 indicator/无测试图残余） |
| `task-4-ui-mem-transcript.txt` | 串口原始 transcript：`UI` 查询/切页、`UI bogus` 用法、逐页 `ENG MEM` 内部堆水位 |
| `task-4-click-transcript.txt` | **scratch 构型**的按钮回调 transcript（见下方说明）：BPM −/+ 改值、PLAY/STOP 置走带 |
| `task-4-boot-log.txt` | 终版固件 RTS 复位后 7s 启动日志节选（无 fatal/abort/backtrace/WDT） |
| `task-4-gates.txt` | 构建 ×2（零警告）、烧录三段 Hash verified、host-test/state-test/screenshot --selftest/manifest 摘录 |

## scratch 构型说明（`task-4-click-transcript.txt`）

本任务尚无 `TAP`/`KEY` 注入（Task 6/7），触摸点击无法机器触发。为验证 TRANSPORT 按钮回调
接线，临时加了一条仅用于验证的 `UI CLICK <play|stop|minus|plus>` 命令（向同一按钮对象
`lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL)`，即触摸松手时 LVGL 派发的事件），
烧录后采集 transcript，随后**完全撤除**该临时命令（源文件从备份恢复，`grep TEMP` 为空），
重新构建 ×2 并烧录终版固件——终版启动日志、截屏与逐页堆水位均来自撤除后的构型。
物理触摸命中真值与 BPM 长按连发留待 Task 7 `TAP` 注入/人测。
