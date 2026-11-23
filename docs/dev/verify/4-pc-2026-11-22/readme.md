# Task 13 证据索引（P4-C 全量门禁与阶段收口）

受测固件：`feat/phase4c` @ `2f370d8`（clean 工作树，构建时 HEAD=`2f370d8`）；
`App version: 2f370d8`；镜像 `0x192a70`（1,649,264B，分区余量 80%），SHA256
`e3de21abff95fe442c1764f2460390e26913c688e65ce2421c035063fda1f083`
（`task-13-bin-sha.txt`）。设备：Tab5 /dev/ttyACM0（ESP32-P4，ST7121 屏批次）。

所有 transcript 均为串口原始回显（`inject.py` 口径：逐字节写盘，唯一非固件字节是
`# [inject] ...` 工具注记）；`*-stdout.log` 是断言摘要，其中另以 `[fw]` 前缀镜像串口
回显。PNG 由场景的像素抽检在运行时校验，**人眼读图/物理触摸/听感仍是外部门禁**
（本会话无图像查看工具）。

## 机器门禁矩阵（命令 → 结果 → 证据）

| 门禁 | 命令 | 结果 | 证据 |
|---|---|---|---|
| 构建（clean） | `scripts/build.sh --clean` | rc=0、**0 编译警告**（首遍仅 §5.5 豁免的 kconfig 提示对）；镜像 `0x192a70` | `task-13-build1-clean.log` |
| 构建（增量） | `scripts/build.sh` | rc=0、**0 警告、0 kconfig 提示** | `task-13-build2.log` |
| 烧录 | `scripts/flash.sh /dev/ttyACM0` | rc=0、三段 `Hash of data verified` | `task-13-flash.log` |
| 启动 | esptool 硬复位 + 串口抓取 | `App version: 2f370d8`；`sloop_store: ready … base=0x97000 span=0x68000`、autosave 恢复；`UI 就绪: 12 页注册/1 已建`（池 13,992/62,960B，内部堆 98,179B）；`InputManager 60 条映射`；无 fatal/panic/abort/backtrace/task_wdt | `task-13-boot-log.txt` |
| host 回归 | `scripts/host-test.sh` | rc=0、`ALL HOST TESTS PASSED (subset)`（含 `len`/`hold_rec`/`studio_drums`/`arranger`/`song_audio`/fixture `--host-check`） | `task-13-host-test.log` |
| 状态层 | `scripts/state-test.sh` | rc=0、**28 passed, 0 failed**、`STATE TEST PASSED` | `task-13-state-test.log` |
| 音序 host | `scripts/seq-test.sh` | rc=0、`SEQ HOST TEST PASSED (0 failures)` + persist 段 `PASS (0 failures)` | `task-13-seq-test.log` |
| 旋钮/输入表 | `scripts/knob-test.sh` | rc=0、**7 passed, 0 failed** | `task-13-knob-test.log` |
| 截屏工具 | `python3 scripts/screenshot.py --selftest` | rc=0、`selftest OK` | `task-13-screenshot-selftest.log` |
| 完整性 | `cd third_party && sha256sum -c MANIFEST.sha256` | **233/233 OK**、树未改动 | `task-13-manifest.log` |
| 设备等价 | `python3 scripts/device-selftest.py /dev/ttyACM0 --output …` | rc=0、**182/182 identical**、`checks=7 check_fail=0 deterministic=1 result=PASS` | `task-13-selftest.log`、`task-13-selftest-transcript.log` |
| 八条注入场景（同会话） | 见下表 | 全部 rc=0、**2,580 PASS / 0 FAIL** | `task-13-eight-matrix.txt`、各 `task-13-<场景>-{transcript,stdout}.log` |
| 稳定性 soak | `python3 scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 60 --save-window 900` | **PASS**：31 次采样 / 1801s；内部堆 93,011→92,991B、`internal_min` 86,148B ≥60KB；`xrun` 0（窗口增量 0 ≤16）；`duty_mean` 最大 12.54% ≤18.88%；音频栈 3,120–3,168B；池往返 Δ−52B（单次 −40B ≤128B）；无 panic/WDT | `task-13-soak.log`、`task-13-soak-summary.txt`、`task-13-soak-transcript.log` |

## 八条注入场景（同一会话，脚本之间不重启）

顺序 transport → step_edit → tracks → nav → params → store → song → sequence，04:47:41 起
到 05:10:12 结束（≈22.5 分钟），脚本之间**不重启**。`store` 按设计在其内部执行 **5 次**
esptool 硬复位（⓪ 归一、⑦ 热复位早探、⑨g 预置重启、⑪g autosave 恢复、⑫f 银行重启），
`sequence` 在 ⑧ 执行 1 次（PER3/section 恢复），每次复位后都在**同一场景内继续**执行后续断言。

| 场景 | 断言重点 | PASS / FAIL | 证据（transcript / PNG 数） |
|---|---|---|---|
| transport | 走带、BPM、工具区 18 个 chip（REC 五态/FILL 保持+NEXT/LVL/RAT/MIC/SPAN/SLIDE/ROT/LEN/TRANS/UNDO/REDO/CLEAR）、空步与冻结拒绝、bank1 窗口、焦点轨 ≠ 选中轨、键盘字段环 | 288 / 0 | `task-13-transport-transcript.log` / 5 |
| step_edit | 窗口列编辑、绝对步号、隐藏列/播放头映射、深度步态（TIE/CONT/micro/fill/颜色）、鼓 lane 编辑（TAP/ENTER/冻结回读） | 196 / 0 | `task-13-step_edit-transcript.log` / 8 |
| tracks | 四轨卡选择、选中态与像素抽检 | 11 / 0 | `task-13-tracks-transcript.log` / 1 |
| nav | 页签缝 miss、鼓格 lane 编辑、同拍 tab+PANIC / 三连 tab、组作用域、`UI TAB`/`UI SUB` 手指导航、12 页 TAB 轮转、跨组池往返 ≤128B | 140 / 0 | `task-13-nav-transcript.log` / 7 |
| params | 五参数页 TAP/键盘/`UI KNOB` 拖拽、切页保值、组作用域池水位、锁定页 `ignored (locked)`、FM6/SAMPLE 编辑 | 149 / 0 | `task-13-params-transcript.log` / 8 |
| store | 工程 A–D 双层往返与硬复位早探、预置 32 槽、换音 71/71、自动存档四门、FM6 银行、torn-write `STORE TEST PASS 6/6`、旧格式 FUN5/4/1、FILE 两页、FM6 bank 行（锁定页 `NOT FM6`、槽环绕、`STOP BEFORE SAVE`） | 903 / 0 | `task-13-store-transcript.log` / 8 |
| song | SONG 独立组与 16 项配置、section 卡 STORE/LOAD/ERASE/JUMP、chain begin/commit/cancel、空 scene 可见拒绝、SONG REC NO SONG、池往返、页面守卫 `STOP FIRST` | 331 / 0 | `task-13-song-transcript.log` / 9 |
| sequence | ① 深度步/24+25 锁/非法锁/工具/undo、② free take（API close vs 物理 drop）/16 lane 鼓/FILL、③ live sections + dirty 延迟写、④ chain、⑤ SONG A2+B3 与工作 loop 恢复（判别哈希 A=3B04E62E B=6EBBEF0A h0=07D9E27D）、⑥ SONG REC、⑦ `STORE TEST` 与 SELFTEST 后保持、⑧ 硬复位 PER3/section 恢复、⑨ 收口 | 562 / 0 | `task-13-sequence-transcript.log` / 3 |

## soak 判据与执行顺序（DI-26）

`soak.py` 在**同一次开机**内先跑 182 例 `ENG SELFTEST` 会泄漏 ~40KB 内部堆（DI-26），
故矩阵顺序固定为「八场景 → `device-selftest` → **esptool 硬复位** → soak」；复位后内部堆
回到 ~98KB，soak 的 60KB 底线不再被已知泄漏弄成假红。

soak 参数按 brief：`--duration 1800 --interval 60 --save-window 900`。**默认不带
`--section-capture`**（该开关每 3 轮在播放中覆盖工程槽 A，是破坏性压测，Task 12 已单独
跑过并归档）；`--save-window` 只在窗口内做一次 `STORE PROJ SAVE 0` + `LOAD 0`（覆盖槽 0，
属判据设计）。窗口前会 `STORE AUTO OFF` 并在窗口内保持关闭，收尾恢复 `STORE AUTO ON`；
窗口前的顺序是「停走带 → 等 section/song 延迟写排空 → 取基线」（Task 12 终审修复）。

本轮实测（`task-13-soak-summary.txt`）：**VERDICT: PASS**——31 次采样、实测 1801s；
`internal_free` 93,011→92,991B（首末差 −20B，`internal_min` 86,148B ≥60KB）；每次采样
`streaming=1 xrun=0`，save 窗口 900s 内 `xrun` 0→0（增量 0 ≤16），窗口后 `streaming=1`；
`duty_mean` 9.44%→12.54%（上限 18.88%）；音频任务栈 3,120–3,168B（≥2KB）；KEY 计数
0→300；页面恒 transport；跨组池往返（transport→song→projects→play ×10）`pool_free`
47,372→47,320B（Δ=−52B，单次最小 −40B ≤128B）；日志无 panic/WDT/abort。

采样窗口说明：`--save-window` 先于采样循环执行（Task 12 起的设计），所以首个采样在
t≈902s（窗口占前 900s，期间走带停止、窗口后恢复播放）；窗口结束后 `interval=60` 小于
窗口长度，前 ~40s 出现 16 个密集追赶采样（902–943s），随后恢复 60s 节奏——不影响
xrun/堆/占空比判据（密集段之间没有足够时间发生堆变化）。

## deferred 审计（Task 13）

- **DI-24 已在 P4-C Task 5 关闭并从 `docs/deferred-issues.md` 删除**（`010-sequencer-arranger.md`
  §8 与 P4-C spec §4.3 保留「按选中轨变化同步 focus」的收口叙述）；本任务确认清单中无
  残留条目、无悬空引用。
- 更新登记：DI-27（`sequence` 按持久效果断言，one-shot take 令牌仍不可稳定观测）、
  DI-31（脚本按「开一步再清」自愈，CLEAR 语义未改）。
- 新增真实遗留：DI-32（`STORE RAW` 不能上传 settings 对象，PER1/2 迁移只有 host 证据）、
  DI-33（warm reset 后 live section 不保留，上游保留）。
- 保留的已知项：DI-1（USB-MIDI host，P5）、DI-2（sdkconfig 重建）、DI-4/10/16（人测）、
  DI-7/39（音频/听感；DI-6 已关闭并入 DI-39）、DI-12（自检单 job 的 task_wdt 噪声，本轮 182 例仍出现 6 行，非失败）、
  DI-13/15（LVGL 文本/枚举边界）、DI-20（自检回滚与 autosave）、DI-21/22（加密与故障注入）、
  DI-26（自检堆泄漏，P4-F 前必须处理）、DI-28/29/30。

## 人测项（外部门禁，未在本任务声称通过）

- 键盘：70 键逐键与修饰键组合（DI-4）；真实弹奏手感与延迟手感。
- 触摸：四角校准、幽灵按压（DI-10）、参数页旋钮物理手感（DI-16）。
- 视觉：49 张 PNG 的人眼可读性/观感复看（机器只做像素/区域抽检）。
- 听感：合成/鼓/采样/SONG 播放与 mute/solo（DI-7/39）；SOUND/SONG 的时序听感。

## 证据清单

本目录 `SHA256SUMS` 覆盖目录内除自身外的全部文件（`sha256sum -c` 校验）。文件命名：
`task-13-<门禁>.*`（构建/烧录/启动/host/设备/soak）、`task-13-<场景>-transcript.log`
（原始回显）、`task-13-<场景>-stdout.log`（断言摘要）、`task-13-<场景>-<截图名>.png`。
