# Task 14 证据索引（P4-B 收口：门禁矩阵、soak 新判据、文档闭环）

受测固件：`feat/phase4b` @ `6167039` + Task 14 工作区源码（`App version: 6167039-dirty`；
源码改动 = `page_presets.c` 注释 + `scripts/soak.py` + `scripts/inject/inject.py`，无固件逻辑改动）；
镜像 `0x184210`（1,589,776B，分区余量 81%），SHA256 `d28f930dea38394cd694b5f82161be5a5cee97cceeaceb34e7ce35d16333df8f`
（`task-14-bin-sha.txt`）。设备：Tab5 /dev/ttyACM0（ESP32-P4，ST7121 屏批次）。

## 门禁矩阵（命令 → 结果 → 证据）

| 门禁 | 命令 | 结果 | 证据 |
|---|---|---|---|
| 构建（clean） | `scripts/build.sh --clean` | rc=0、**0 警告**、0 kconfig 提示 | `task-14-build-clean.log` |
| 构建（增量 ×2） | `scripts/build.sh` ×2 | rc=0、**0 警告**、0 kconfig 提示（§5.5 的 kconfig 豁免对未出现） | `task-14-build2.log`、`task-14-build3.log` |
| 烧录 | `scripts/flash.sh` | rc=0、三段 `Hash of data verified` | `task-14-flash.log` |
| 启动 | esptool 硬复位 + 12s 串口抓取 | `App version: 6167039-dirty`；`sloop_store: ready … base=0x97000 span=0x68000`、`autosave 恢复`；`UI 就绪: 11 页注册/1 已建`（池 16,232/62,760B，内部堆 104,383B）；`InputManager 56 条映射`；无 fatal/panic/abort/backtrace | `task-14-boot-log.txt` |
| host 回归 | `scripts/host-test.sh` | rc=0、`ALL HOST TESTS PASSED (subset)`（含 storage/project/upreset/fm6 与 fixture `--host-check`） | `task-14-host-test.log` |
| 状态层 | `scripts/state-test.sh` | `19 passed, 0 failed`、`STATE TEST PASSED` | `task-14-state-test.log` |
| 旋钮纯函数 | `scripts/knob-test.sh` | `5 passed, 0 failed`、`KNOB TEST PASSED` | `task-14-knob-test.log` |
| 截屏工具 | `python3 scripts/screenshot.py --selftest` | rc=0、`selftest OK`（解码/协议/重试/坏尾/像素抽检全过） | `task-14-screenshot-selftest.log` |
| 设备等价 | `python3 scripts/device-selftest.py /dev/ttyACM0` | rc=0、**182/182 identical**、`checks=7 check_fail=0 deterministic=1 result=PASS` | `task-14-selftest.log` |
| 稳定性 soak | `python3 scripts/soak.py /dev/ttyACM0 --duration 300 --save-window 60` | `VERDICT: PASS`：内部堆 99,211→99,263B（min 91,908 ≥60KB）、xrun=0（窗口增量 0 ≤16）、窗口后 streaming=1、duty_mean 最大 11.77% ≤18.88%、音频栈 3,128B、无 panic/WDT | `task-14-soak.txt`、`task-14-soak-summary.txt`、`task-14-soak-transcript.log` |

六条注入场景（**同一会话、脚本之间不重启**；`store` 仅执行其设计内的硬复位），全部退出码 0：

| 场景 | 命令 | 结果（PASS/FAIL） | 证据 |
|---|---|---|---|
| nav | `bash scripts/inject/nav.sh /dev/ttyACM0` | 128 / 0（组作用域、TAB 11 页轮转、⑧ 往返 Δ pool_free −56B ≤128B） | `task-14-nav.txt`、`task-14-nav-tab-cycle.png` |
| params | `bash scripts/inject/params.sh /dev/ttyACM0` | 147 / 0（五参数页、`UI KNOB` 触摸路径、EDIT Σ est 19,968） | `task-14-params.txt`、`task-14-page-{global,fx,analog,fm6,sample}.png` |
| tracks | `bash scripts/inject/tracks.sh /dev/ttyACM0` | 11 / 0 | `task-14-tracks.txt`、`task-14-tracks-sel3.png` |
| transport | `bash scripts/inject/transport.sh /dev/ttyACM0` | 43 / 0 | `task-14-transport.txt`、`task-14-transport.png` |
| step_edit | `bash scripts/inject/step_edit.sh /dev/ttyACM0` | 89 / 0（计数 = 归档文件的 `[PASS]` 行数） | `task-14-step_edit.txt`、`task-14-step-bank1.png` |
| store | `bash scripts/inject/store.sh /dev/ttyACM0` | **903 / 0**（工程/预置/FM6/autosave/torn-write/旧格式/FILE 两页/FM6 bank 行） | `task-14-store.txt`、`task-14-store-transcript.log`、`task-14-projects-used.png`、`task-14-presets.png`、`task-14-fm6-bank{,-locked}.png` |

## soak 新判据（Task 14 落地）

`scripts/soak.py` 增两项，默认行为不变（不带 `--save-window` 时 `xrun` 必须为 0）：

1. **`--save-window N`**：窗口内停走带做一次 `STORE PROJ SAVE 0` + `STORE PROJ LOAD 0`
   （音频流不停），断言窗口内 `xrun` 增量 ≤16、窗口后 `streaming=1`；`xrun` 是自启动累计值，
   窗口后冻结基线，**窗口外不得新增**（基线可为之前会话留下的非 0 计数）。
   本轮实测：窗口 60s，xrun 0 → 0（增量 0）。
2. **跨组池检查**：采样前 SEQ(TRANSPORT)↔PLAY ×5 往返，`UI MEM` 的 `pool_free`
   首末差与单次下降都 ≤128B（R34 实测噪声 84B）。本轮实测：44,100 → 44,104
   （Δ=+4B，单次最小 −44B）。

本轮 `--duration 300 --interval 300` ⇒ 2 次采样（t=62s 窗口结束、t=301s 收尾）；
堆/jitter 判定按这两个采样与 `internal_min` 计算，xrun/streaming/duty 逐采样核验。

## 场景假设修正（原 DI-23，已在本次收口删除）

同会话跑六条场景时，`store` 的 ⑩/⑪ 暴露了「起点来自 autosave 恢复的历史状态」的假设问题，
按原 DI-23 的 (a)+(b) 收口（`scripts/inject/inject.py`）：

- ⑩ 显式 `P_MUTE=0`；轨 1 先清步 5、再保证一个非 5 的步，foreign 的步数 = 同轮 `fmt1`
  捕获的轨 1 步数 **+1**（diagnostic 自己放的步 5），不再写死 `steps=1`。
- ⑪ 显式归一 `P_MUTE`/`P_LEVEL`/`P_AMODE`/`P_AHOLD`：ARP（AMODE>0）在停走带时不发声；
  KBD HOLD（`!AMODE && AHOLD`）让松开后的 voice 永不释放 ⇒ `audio_quiet` 恒 false、
  ⑪e/⑪f 的写入断言假红。实测该状态来自旧格式 fixture 漂进 autosave 的
  `mute=1/AMODE=14/AHOLD=1`。

失败轨迹按轮次归档（最终轮 903 PASS 为门禁证据，前几轮保留作诊断）：

| 文件 | 失败点 |
|---|---|
| `task-14-store-fail-di23.txt` / `-transcript.log` | ⑩f `steps=1` 与恢复状态（3 步）不符（DI-23 原始形态） |
| `task-14-store-fail-di23b.txt` / `-transcript.log` | ⑪e `voices=0`（mute=1/level=1）+ 写入断言级联 |
| `task-14-store-fail-di23c.txt` / `-transcript.log` | ⑩f fmt1 捕获 0 步、foreign 1 步（相对比较需 +1） |
| `task-14-store-fail-di23d.txt` / `-transcript.log` | ⑪e 仍 `voices=0`（AMODE=14 的 ARP 持有） |
| `task-14-store-fail-di23e.txt` / `-transcript.log` | ⑪e voices 正常但松开后 voice 不释放（AHOLD=1 的 KBD HOLD） |

## 已知发现：`ENG SELFTEST` 内部堆泄漏（DI-26，门禁矩阵的附加约束）

收口矩阵按「六场景 → selftest → soak 同一会话」连跑时，soak 的 HEAP_FLOOR 失败：
`internal_free 59,355 / internal_min 59,296`（<60KB）。定位为 `ENG SELFTEST` 自身：
每次全量自检净减 ~40KB 内部堆；两次后 19.3KB 且 `ENG AUDIO ON` 超时（需重启）。
音频 OFF/ON、普通命令（含浮点打印）无此现象；泄漏按执行的 regress job 计（~230B/job）。
测量与失败 soak 归档：`task-14-selftest-heap-leak.txt`、`task-14-soak-fail-after-selftest{,-summary}.txt`、
`task-14-soak-fail-after-selftest-transcript.log`。**复位后同一 soak 命令 PASS**（本目录
`task-14-soak*`）。条目登记 `docs/deferred-issues.md` 的 DI-26；AGENTS §5.1/§5.5 已注明
「selftest 之后复位再 soak」。本任务未改 selftest（避免在收口任务里盲改堆/快照），列为待修。

## 文件清单

- 构建/烧录/启动：`task-14-build-clean.log`、`task-14-build2.log`、`task-14-build3.log`、
  `task-14-bin-sha.txt`、`task-14-flash.log`、`task-14-boot-log.txt`
- 主机门禁：`task-14-host-test.log`、`task-14-state-test.log`、`task-14-knob-test.log`、
  `task-14-screenshot-selftest.log`、`task-14-selftest.log`
- 注入场景：`task-14-{nav,params,tracks,transport,step_edit,store}.txt` + 截屏（见上表）
  + `task-14-store-transcript.log`
- soak：`task-14-soak.txt`、`task-14-soak-summary.txt`、`task-14-soak-transcript.log`
- 失败/诊断：`task-14-store-fail-di23{,b,c,d,e}.txt` + `-transcript.log`、
  `task-14-soak-fail-after-selftest{,-summary}.txt`、`-transcript.log`、`task-14-selftest-heap-leak.txt`
- 全值 SHA256：`SHA256SUMS`（本目录）
