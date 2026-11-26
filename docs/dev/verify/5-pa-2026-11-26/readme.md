# 5A 连接打通阶段证据包（2026-11-26）

> 阶段 5 / 5A「连接打通」：Tab5 作为 USB-MIDI host 接入刷 SLOOP 的 M-VAVE FM-1——枚举 → INFO 版本协商 →
> WATCH/PING 保活 → 设备侧旋钮改动以 CHANGED 推送到 Tab5；全部状态经 console `FM1` 命令族可读、可脚本断言。
> 分支 `feat/phase5-sloop-go`。设备 Tab5 `/dev/ttyACM0`，ESP-IDF 5.5.5，target `esp32p4`；门禁口径 AGENTS §5.5。
> **固件 = `715fdae`**（设备自报 `App version: 715fdae`，见 `31-boot.log`）。
> **FM-1 未接条件跑全矩阵**：构建、host、烧录、无链路检查、182、堆、十条场景、截屏矩阵、soak（§1 第 1–14 行）。
> **FM-1 已接证据（最终固件，19:05–19:18）= 冒烟 + U 盘监听 + 182 + selftest-heap**（§1 第 16–20 行）。
> 首轮（12:59 / 13:04）的冒烟与监听转录已丢失，只作历史保留（第 21 行、§4）。
> **状态：两种条件的门禁全部通过**——FM-1 未接：全矩阵（27 步里 26 步 rc=0；首跑 soak 的 rc=1 是驱动脚本没按「硬复位 + 30s 后起跑」
> 的口径，复位后重跑 PASS，首跑转录保留）；FM-1 已接：`FM1 SMOKE PASS 7/7`、182/182、堆 PASS。
> **已合回 `dev`**：合并提交 `aba1499`（父 `f4b2026` + `00379c0`）；合并前离线全量测试与合并后门禁见 §1 第 22–23 行。
> 合并后的树在 `firmware/`、`scripts/`、`tools/` 上与全矩阵固件 `715fdae` 逐字节相同（`git diff --stat 715fdae dev -- firmware scripts tools`
> 为空），**设备矩阵没有在合并后重跑**。

## 1. 门禁矩阵（门禁 → 命令 → 证据 → 结论）

运行器 = `.superpowers/sdd/2026-11-26-5a-fm1-connect/task7-matrix.sh`（gitignored，不入库），逐步的起止时间、
rc 与耗时在 `status.txt`（13:13:09 → 15:05:36；`HEAD=715fdae branch=feat/phase5-sloop-go`）。全部在 FM-1 未接
（USB-A 口留空）的条件下、同一次开机会话里自上而下跑——**除了第 14 行的 soak，它是硬复位之后单独起跑的**。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | `scripts/build.sh --clean` → `scripts/build.sh` ×2 | `01-build-clean.log`、`02-build.log`、`03-build.log`、`status.txt` | ✅ 三遍 rc=0（186s / 2s / 3s）。后两遍 0 警告（`status.txt`：`warnings_02=0`、`warnings_03=0`）；首遍只有 §5.5 豁免的那一对 CMake 提示（`build.cmake:631` + `Missing kconfig option. Re-run the build process...`，`01-build-clean.log:56,70-71`） |
| 2 | host：golden / DSP 回归 | `scripts/host-test.sh` | `10-host-test.log` | ✅ `182 golden renders (0 changed, 0 gone), 0 health failures, 0 voice / routing checks failed, 0 CPU entries over budget`；`ALL HOST TESTS PASSED (subset)` |
| 3 | host：状态层 / UI 纯函数 / 音序 / 声音设计面 | `scripts/state-test.sh`、`knob-test.sh`、`seq-test.sh`、`synth-test.sh` | `11-state-test.log`、`12-knob-test.log`、`13-seq-test.log`、`14-synth-test.log` | ✅ state `46 passed, 0 failed`；knob `17 passed, 0 failed`；seq `persist-host: PASS (0 failures)`；synth `SYNTH HOST TEST PASSED` |
| 4 | host：编辑器协议帧 / MIDI 流 | `scripts/editor-proto-test.sh`、`usb-fm1-test.sh` | `15-editor-proto-test.log`、`16-usb-fm1-test.log` | ✅ `EDITOR PROTO TEST PASSED (6/6)`；`USB FM1 TEST PASSED (5/5)` |
| 5 | 工具离线自测 | `python3 scripts/soak-selftest.py`、`screenshot.py --selftest`、`fm1-smoke.py --selftest` | `20-soak-selftest.log`、`21-screenshot-selftest.log`、`22-fm1-smoke-selftest.log` | ✅ 三个 rc=0：soak `结果: PASS`；screenshot `selftest OK`；`FM1 SMOKE SELFTEST OK`（14 组样例：3 组全通过 7/7；9 组各恰好一条失败；另 2 组：SET 无回复、操作员中止） |
| 6 | 固件代码（烧录验证） | `scripts/flash.sh` | `30-flash.log`、`status.txt` | ✅ `Hash of data verified` ×3（`hash_verified=3`） |
| 7 | 启动日志 | 烧录后开机 | `31-boot.log`、`status.txt` | ✅ `App version:      715fdae`；`boot_fatal=0`（fatal/abort/backtrace 计数 0） |
| 8 | 无链路开机检查（FM-1 未接） | `FM1 STATUS` / `FM1 INFO` / `FM1 GET` / 非法参数 ×4 / `FM1 EVENTS` | `32-nolink.log`、`status.txt` | ✅ `FM1 INFO ERR no_link`、`FM1 GET ERR no_link`；`FM1 GET 2 1` / `FM1 SET 0 1 9000` / `FM1 WATCH 2` / `FM1 FOO` 四条均 `FM1 ERR invalid`（`invalid=4`）；`FM1 EVENTS total=0 changed=0 … last=none`；`FM1 STATUS link=none … watch=0 info=no proto=- version=- need_upgrade=0`；`ENG MEM internal_free=73467 internal_min=72288` |
| 9 | 设备全量自检 | `python3 scripts/device-selftest.py /dev/ttyACM0` | `40-device-selftest.log`（stdout）、`40-device-selftest-transcript.log`（串口回显） | ✅ rc=0（163s）：`cases=182 identical=182 mismatch=0 health_fail=0 checks=7 check_fail=0 deterministic=1 result=PASS`；`SELFTEST COMPARE identical=182 mismatch=0 result=PASS` |
| 10 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `41-selftest-heap.log`（stdout）、`selftest-heap-20261126-132250.log`（串口回显） | ✅ rc=0（327s）：run 1 `delta=260`、run 2 `delta=36`（上限 1,024B），收尾 `internal_free=73239B`（门线 61,440B）；`# [selftest-heap] PASS` |
| 11 | 十条注入场景 | `scripts/inject/{transport,step_edit,tracks,nav,params,store,song,sequence,synth,sys}.sh` | `50..59-inject-*.log`（stdout）+ `inject-*-20261126-*.log`（串口回显 ×10） | ✅ 十条全部「全部断言与像素抽检通过」、rc=0；合计 **5,014 PASS / 0 FAIL**（计数口径与逐场景读数见 §2） |
| 12 | 截屏矩阵 | `python3 scripts/screenshot_matrix.py /dev/ttyACM0` | `60-screenshot-matrix.log`（stdout）、`matrix-20261126-142157/` | ✅ rc=0（781s）：`PNG 33/33 张；锚点 166 个；截屏复位重试 0 次`；逐条目后 `LIVEHASH 6132542B == 起点 6132542B`、`STORE AUTO on`；stdout `[PASS]` 492 行 / `[FAIL]` 0 行 |
| 13 | 阶段收口 soak——**首跑 FAIL**（同一次开机、接在截屏矩阵之后；保留作对照） | `python3 scripts/soak.py /dev/ttyACM0 --duration 1800`（14:34:58 起跑） | `70-soak-fail-sameboot.log`（stdout；原文件名 `70-soak.log`）、`soak-20261126-143458.log`（串口回显）、`soak-summary-20261126-143458.txt` | ❌ rc=1：`0 项断言失败，41 项判定问题` = `xrun=1` ×31 个采样（t=1s 即为 1，全程序列不变）+ `duty_mean` 超 18.88% ×10 个采样（t=1s 29.31% → t=541s 18.92%）。两条判据都是**自开机累计量**：`soak.py` 不复位设备，这一跑继承了此前一小时自检 / 注入的计数与均值（定性见 §4） |
| 14 | 阶段收口 soak——**复位后重跑 PASS** | 硬复位 + 30s → `python3 scripts/soak.py /dev/ttyACM0 --duration 1800`（15:08:34 → 15:39:43） | `71-soak-fresh.log`（stdout）、`soak-20261126-150834.log`（串口回显）、`soak-summary-20261126-150834.txt`、`70b-soak-fresh-summary.txt`（与前者逐字节相同） | ✅ rc=0，`VERDICT: PASS`：实测 1801s / 31 采样 / 断言失败 0 / 判定问题无；内部堆 73,119 → 73,099B（Δ−20B）、`internal_min=71,876B`；streaming=1、`xrun=0`；`duty_mean` 最大 **18.31%**（上限 18.88%）；音频栈余量 3,120B、输入栈 7,848B；池往返 `pool_free 45824 -> 45836`（Δ+12B，单次最小 −20B，容差 ±128B）；`ina=ok` 31/31、温度 31.1℃；无 panic/WDT |
| 15 | 渲染占空比 A/B（对照，不计门禁） | `task7-ab.sh`（首轮）+ `task7-ab-b.sh`（补跑 B）；每轮硬复位后 → `STORE PROJ SAVE 0`（仅 A）/ `LOAD 0` → `soak.py --duration 600 --interval 60` | `ab-status.txt`、`ab-A-5a-{prep,soak}.log` + `ab-A-5a-summary.txt`、`ab-C-5a-{prep,soak}.log` + `ab-C-5a-summary.txt`、`ab-B-dev-{prep,soak}.log` + `ab-B-dev-summary.txt`、`ab-flash-{dev,5a}.log`、`ab-B-boot.log`（空）、`ab-back-boot.log`（空） | 稳态 `render_mean_us` @601s：5A 之前 `f4b2026` = **1037µs（17.87%）**；5A `715fdae` = **1071 / 1071µs（18.45%）**（A 与 C 两次）。⇒ 5A 使渲染均值 +34µs（+3.4%，约 +0.6 个百分点占空比）；历史 9–13% 与今天 ~18% 的差距来自 soak 起点工程内容（dev 固件同样 17.9%），非 5A 回归。**这四次 600s 的 `VERDICT` 都是 FAIL**（断言失败 0；判定问题全是占空比——见 §4） |
| 16 | 联动协议冒烟（FM-1 已接；**最终固件上的门禁证据**；用户执行，19:05） | `python3 scripts/fm1-smoke.py /dev/ttyACM0` | `fm1-smoke-20261126-190513.log`（串口原始回显；头 `firmware App version:      715fdae`，无 `-dirty`） | ✅ 末行 **`FM1 SMOKE PASS 7/7`**：① `link=ready … watch=3 info=yes proto=12 version="FELUCCA SLOOP 2.8.5" need_upgrade=0`；② `FM1 INFO version="FELUCCA SLOOP 2.8.5" nengines=12 p_count=71 g_count=50 nstep=64 pe0=63 ntrk=4 proto=12`；③ PING ×10，中位数 1ms（1–2）；④ ATK `FM1 GET 0 1` 0 → `SET` 1 → 读回 1 → 还原 0 → 读回 0；⑤ 拧旋钮后先到 `reload=2`，随后 `changed=1 … last=cmd=23 scope=1 id=0 value=81`；⑥ 静置 10s 后再拧：`changed=2 … last=cmd=23 scope=1 id=0 value=80`（全程 `total=6 changed=2 reload=4`）；⑦ 拔线 `link=none watch=0 info=no`、`FM1 INFO ERR no_link`、连发 5 次 `FM1 PING ERR no_link` 每次 1s 内有回复；重插 `link=ready watch=3 info=yes`、`FM1 INFO` 恢复、`FM1 PING ms=1`。接口 1 MIDI Streaming，bulk IN 0x81 / OUT 0x01；重插时日志 `MIDI Streaming 接口 1 就绪 … in_buf=256 字节, internal_free 73155 -> 72775` |
| 17 | 非 MIDI 设备插入（U 盘；操作员在场；**重做**，19:07:49） | 主控的监听 `fm1-nonmidi-watch.py /dev/ttyACM0` | `fm1-nonmidi-watch-2.log` | ✅ 无 abort，`# RESULT open_fail>=1 seen; exit 0`：FM-1 拔出 ⇒ `Bulk IN 重提交失败: ESP_ERR_INVALID_STATE`、`link=none`；U 盘（VID 0x0930 PID 0x6545，接口 `class=0x08 subclass=0x06 protocol=0x50`）枚举 ⇒ `W usb_fm1: 设备 addr=3 无 MIDI 接口`、`FM1 STATUS link=none … open_fail=1` |
| 18 | 设备自检（**FM-1 已接**） | `python3 scripts/device-selftest.py /dev/ttyACM0`（19:09:57 → 19:12:41） | `att-182.log`（stdout）、`att-182-transcript.log`（串口回显）、`att-status.txt`（运行记录） | ✅ rc=0：`DEVICE SUMMARY cases=182 identical=182 mismatch=0 health_fail=0 checks=7 check_fail=0 deterministic=1 … result=PASS`；`SELFTEST COMPARE identical=182 mismatch=0 result=PASS` |
| 19 | 自检堆泄漏（**FM-1 已接**） | `python3 scripts/selftest-heap.py /dev/ttyACM0`（→ 19:18:07） | `att-heap.log`（stdout）、`att-heap-transcript.log`（串口回显） | ✅ rc=0：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72675B`（门线 61,440B）；`# [selftest-heap] PASS` |
| 20 | FM-1 已接两项检查之后的链路状态 | `FM1 STATUS` / `FM1 INFO` / `FM1 PING` / `FM1 EVENTS` / `ENG MEM`（19:18:21） | `att-fm1.log` | ✅ `FM1 STATUS link=ready … rx=322 tx=322 drops=0 open_fail=0 … watch=3 info=yes proto=12 version="FELUCCA SLOOP 2.8.5" need_upgrade=0`；`FM1 INFO` 与基线一致；`FM1 PING ms=1`；`FM1 EVENTS total=0 … last=none`；`ENG MEM internal_free=72559 internal_min=71824 psram_free=30065824 psram_min=29344924` |
| 21 | 首轮冒烟与 U 盘监听（12:59 / 13:04；**历史记录，不作为门禁证据**） | `python3 scripts/fm1-smoke.py /dev/ttyACM0`；`fm1-nonmidi-watch.py` | 无文件——两份原始转录已被全矩阵的 `build.sh --clean` 删除（§4） | 首轮结论：`FM1 SMOKE PASS 7/7`；U 盘插入 `link=none open_fail=1`、无 abort。读数出自主控当时核实的清单，没有原始回显可核；当时设备固件是 `715fdae` 的同源构建（版本串含 `-dirty`）。门禁证据以第 16–17 行为准 |
| 22 | 合并前离线全量测试（`feat/phase5-sloop-go` @ `00379c0`） | `scripts/host-test.sh`、`state-test.sh`、`knob-test.sh`、`seq-test.sh`、`synth-test.sh`、`editor-proto-test.sh`、`usb-fm1-test.sh`；`python3 scripts/soak-selftest.py`、`screenshot.py --selftest`、`fm1-smoke.py --selftest` | `finish-selftest.log` | ✅ 日志里十项全部 PASS：`182 golden renders (0 changed, 0 gone), 0 health failures, 0 voice / routing checks failed, 0 CPU entries over budget` + `ALL HOST TESTS PASSED (subset)`；state `46 passed, 0 failed`；knob `17 passed, 0 failed`；seq `SEQ HOST TEST PASSED (0 failures)`、`persist-host: PASS (0 failures)`；synth `SYNTH HOST TEST PASSED`；editor-proto `6/6`；usb-fm1 `5/5`；soak-selftest `结果: PASS`；screenshot `selftest OK`；`FM1 SMOKE SELFTEST OK`（14 组样例）。MANIFEST 校验的输出不在这份日志里（§4） |
| 23 | 合并 `dev` 后门禁（`dev` @ `aba1499`） | `scripts/build.sh` ×2 + 第 22 行的离线套件 | `postmerge-gate.log` | ✅ 两遍构建都到 `==> 构建完成`，固件编译 0 警告：第一遍只有 §5.5 豁免的一对 CMake 提示（`build.cmake:631` + `Missing kconfig option. Re-run the build process...`，第 41 / 55 行），第二遍没有任何提示。离线套件十项全部 PASS，结论行与第 22 行相同。MANIFEST 同样不在日志里（§4）；设备矩阵未重跑（依据见本节开头） |

## 2. 文件说明与计数口径

- `status.txt`：全矩阵运行器的逐步记录（27 步；`DONE failed=1: 70-soak`）。
- 构建 / host / 工具自测：`01..03-build*.log`、`10..16-*-test.log`、`20..22-*-selftest.log`（均为命令的 stdout+stderr）。
- 设备：`30-flash.log`、`31-boot.log`、`32-nolink.log`（第 8 行那组命令的串口回显）；
  `40-device-selftest.log` / `41-selftest-heap.log` 是脚本 stdout，`40-device-selftest-transcript.log` /
  `selftest-heap-20261126-132250.log` 是同一次运行的串口原始回显。
- 注入：`50..59-inject-*.log` 是场景 stdout（逐条 PASS/FAIL 与末尾汇总）；`inject-<场景>-20261126-<时间戳>.log`
  是串口原始回显。计数口径 = stdout 里 `[PASS]` / `[FAIL]` 的行数（含末尾未缩进的场景汇总行）。

| 场景 | stdout | 串口回显 | rc | 耗时 | PASS / FAIL |
|---|---|---|---|---|---|
| transport | `50-inject-transport.log` | `inject-transport-20261126-132818.log` | 0 | 160s | 288 / 0 |
| step_edit | `51-inject-step_edit.log` | `inject-step_edit-20261126-133057.log` | 0 | 247s | 321 / 0 |
| tracks | `52-inject-tracks.log` | `inject-tracks-20261126-133504.log` | 0 | 92s | 84 / 0 |
| nav | `53-inject-nav.log` | `inject-nav-20261126-133636.log` | 0 | 516s | 583 / 0 |
| params | `54-inject-params.log` | `inject-params-20261126-134512.log` | 0 | 648s | 768 / 0 |
| store | `55-inject-store.log` | `inject-store-20261126-135600.log` | 0 | 382s | 907 / 0 |
| song | `56-inject-song.log` | `inject-song-20261126-140222.log` | 0 | 193s | 354 / 0 |
| sequence | `57-inject-sequence.log` | `inject-sequence-20261126-140535.log` | 0 | 164s | 562 / 0 |
| synth | `58-inject-synth.log` | `inject-synth-20261126-140819.log` | 0 | 175s | 794 / 0 |
| sys | `59-inject-sys.log` | `inject-sys-20261126-141114.log` | 0 | 643s | 353 / 0 |
| **合计** | | | | | **5,014 / 0** |

与阶段 4 最终回归（`4-pf-2026-11-24/` 的 10c3，5,017）相比：八条场景行数相同；`step_edit` 321 = 323 − 2（少一对入口相关的
条件断言——10c3 的入口多一行 `TAP 点灭 track=1 step=5` 及其 `ENG STEP` 复核）；`sys` 353 = 354 − 1（`[PASS] STORE SETTINGS INFO`
回显行 58 对 59）。把两边的 `[PASS]` 行做数值归一后逐行比对，其余差异都只是读数值（版本串、PER3 对象长度、哈希、池水位）。
`nav` / `params` 的 `lvgl_stack_free` 最小 5,116B（门限 2,048）。

- soak：`70-soak-fail-sameboot.log` 是首跑 stdout（入库时由 `70-soak.log` 改名，内容未动），`71-soak-fresh.log` 是复位后
  重跑的 stdout；`soak-20261126-143458.log` / `soak-20261126-150834.log` 是两次的串口回显，`soak-summary-*.txt` 是
  `soak.py` 的摘要。`70b-soak-fresh-summary.txt` 与 `soak-summary-20261126-150834.txt` SHA256 相同（同一份摘要的两份拷贝）。
- A/B：`ab-status.txt` 是两轮运行器的状态流水（含每轮摘要的首 / 301s / 601s 采样行与判定问题）。首轮 A / B / C 的
  `flash rc=2` ⇒ B 实为 5A 固件，那三轮只证明可复现（B 轮稳态 1072µs / 18.47%）；有效对照是补跑的 B-dev：
  `ab-flash-dev.log` 烧录 rc 后 `hash_verified=3`、app 镜像 1,767,104B（5A 为 1,843,824B，`ab-flash-5a.log`），
  t=1s `internal_free=69751`（5A 各轮为 73,119）——dev 固件的内部堆签名。`ab-B-boot.log` 与 `ab-back-boot.log`
  是 0 字节文件（运行器没抓到开机日志），原样入库。入库的三份准备日志（A / B-dev / C）里 `STORE PROJ` 的哈希都是
  `02C79E5A`（工程槽 0）。
- FM-1 已接（最终固件，19:05–19:18）：`fm1-smoke-20261126-190513.log`（冒烟串口回显，`# [fm1-smoke] …` 是脚本注记）、
  `fm1-nonmidi-watch-2.log`（U 盘插入监听，行首 `<秒>s` 是监听脚本的相对时间）、`att-status.txt`（已接检查的运行记录：
  `link=ready` 之后依次 `device-selftest` / `selftest-heap`，rc 均为 0）、`att-182.log` / `att-heap.log`（脚本 stdout）与
  `att-182-transcript.log` / `att-heap-transcript.log`（串口回显）、`att-fm1.log`（两项检查之后的 `FM1` 与 `ENG MEM` 读数）。
  受测固件 = `715fdae`：冒烟、监听与已接检查起跑是同一次开机（`att-status.txt` 起跑时 `rx=198 tx=192`，与监听日志末尾一致），
  冒烟转录头自报 `App version: 715fdae`；`att-*` 自身没有开机日志，`att-status.txt` 里没有烧录步骤。
- 合并相关：`finish-selftest.log`（合并前在 `00379c0` 上跑的离线套件输出，从 `host-test.sh` 的生成步骤开始）、
  `postmerge-gate.log`（合并后在 `aba1499` 上的 `build.sh` ×2 + 同一套离线套件）。两份日志都没有记录 HEAD / 分支，
  提交号与执行时点来自主控。
- `matrix-20261126-142157/`：截屏矩阵的联系表 `sheet-*.png` ×10 + 每页一张代表图 `<page>-default.png` ×18
  （入库规则见 §3）。

## 3. 截屏矩阵目录

矩阵共 33 个条目（18 页 × 状态）。为了控制体积，只入库**联系表**（`sheet-*.png` ×10）与**每页一张代表图**
（该页的 `<page>-default.png` ×18），共 28 张。其余 15 张 PNG 留在 gitignored 暂存区
`firmware/build/verify/matrix-20261126-142157/`，文件名与 SHA256 如下（不入 `sha256sum -c` 清单）：

| 文件（未入库） | SHA256 |
|---|---|
| fm6p-engine.png | f1acb6787fd2830e21200cf45582f47412a76072bef4a9d0a8c475784b5887f5 |
| fx-editing.png | f24c83b0b9960613456ee00f8b682db8b672fa71d8e1d0cc296a59b4117b430e |
| global-editing.png | c3d584671f77da4cf221d56b9461eac0d63d419696d677151bac632957568717 |
| mod-editing.png | 6c1f0c113ace88d5ba8b60fb364f5dfe9bcf136ac5d6894671c01dac9af58b66 |
| mod-locked.png | 492a2d2401ea0e2cf1fafad3bc2624d7652381b53e55529b3e584245eddab9a6 |
| sample-usr.png | f832659fe5c3459d92adcdd2fb6e28d6f1ea1d1c0dae486b6a0276e5d4aa7ece |
| scale-editing.png | a7a246b534037f1190c47e5567a5b8d7d9cec1a7991b931c1b31c323d341c654 |
| sound-drum.png | 3e0ef9517dea824a28eab3eb4957785e76fa239e0c715924f03876e8221918e2 |
| sound-editing.png | 8762683fed6985f363320904338431e6f3dc68b0807b569ada820d422a458e03 |
| sound-fm6.png | 657517b47c22556da050ab63e437f5af72d6dae5610cf41f4544bd0876a28c3f |
| step-playing.png | a2a452261cf1ea8794bfdb69ef61341d2a14e31be89cf47c0a78ae149103a110 |
| system-selftest.png | 6a7eb735497d3ebbb3529f79fa226f21e822013f7409bf4febd06c6d5a44aaa7 |
| tools-armed.png | a892d8f0de6906795c9e8355a6c3a0af01a2a3c3dc5e6c17e4f3df101766488e |
| transport-playing.png | 5cedc017694ff89d7c283cc92289a9a0aaf5218f738373b9db1d4721beee336a |
| voice-editing.png | 958a6baac08578b4b6b42880116833fdb3e31f7e0f9eb2ce25e7fe7d84b00d69 |

矩阵目录里的 `matrix.csv`（逐条目置态命令与锚点读数）与 `matrix.log`（串口回显）同样没有入库；条目通过与否以
`60-screenshot-matrix.log` 为准。`system-selftest.png` 与已入库的 `system-default.png` SHA256 相同（两帧逐字节一样）。

## 4. 诚实边界

- **首轮（12:59 / 13:04）的两份转录已丢失**：两份文件都写在 `firmware/build/verify/`，而全矩阵的第一步 `build.sh --clean`
  （13:13:09 起，`status.txt`）会清空 `firmware/build/`；入库时已不存在，全盘检索没有副本。**已由 19:05 之后在最终固件上的
  重跑补证**（第 16–20 行）；第 21 行保留首轮的读数作为历史，来源是主控当时核实的清单，没有原始回显可核。
- **首跑 soak 的 FAIL 怎么定性**：主控裁定为驱动脚本缺陷——`soak.py` 不复位设备，既定口径是「硬复位 + 30s 后起跑」，
  而驱动把它接在截屏矩阵后面同一次开机上。两条判据都是自开机累计量，不是 soak 期间新发生的：`xrun` 在 t=1s 就已是 1
  且 31 个采样全程不变；`duty_mean` 是开机以来的累计均值，被此前一小时的自检 / 注入抬高后单调回落。复位后重跑以
  重跑读数为准（第 14 行）。首跑转录保留，不改名掩盖（文件名带 `-fail`）。
- **A/B 四次 600s 的 `VERDICT` 都是 FAIL**（A 6 项、首轮 B 6 项、C 6 项、B-dev 3 项判定问题；断言失败均为 0）：全是
  `duty_mean` 超 18.88%，t=1s 在 21.8–22.9%——同一「自开机累计」口径，准备步骤的 `SAVE/LOAD` 工程槽 0 写 flash 抬高了
  起点（主控的归因）。A/B 是同条件下的对照，只比较 601s 的稳态 `render_mean_us`，不作为门禁结论。另外
  `ab-status.txt` 里 B-dev 那一行记的是 `soak rc=0`，与它的摘要 / stdout（`VERDICT: FAIL`、`[FAIL] soak: 0 项断言失败，
  3 项判定问题`）不一致：这是 `task7-ab-b.sh` 的记录缺陷（同一个 `echo` 里 `$(date +%T)` 先于 `$?` 展开，`$?` 取到的是
  `date` 的返回值；`soak.py` 只要有判定问题就以 1 退出）。以摘要与 stdout 为准。
- **B-dev 轮没有开机日志**（`ab-B-boot.log` 为空）：「受测的是 dev 固件 `f4b2026`」的依据是烧录日志、app 镜像大小与内部堆
  签名（§2），不是设备自报的 `App version`。
- **没有做视觉检查**：全部截图只有像素探针 / 锚点结论。入库时只核对了文件与 SHA256，没有人看过这 28 张 PNG；
  人眼复核的材料是 `matrix-20261126-142157/sheet-*.png`。
- **FM-1 已接条件只跑了冒烟与 182 / selftest-heap**（按 spec，第 16–20 行），没有 soak、十条场景与截屏矩阵。
  `need_upgrade=1` 与协商失败退避分支仍只有读证（真机是 2.8.5）。IN 环的「连续 3 次才断链」上限分支没有在硬件上演练过：
  两次拔线观察到的都是设备已不在 ⇒ `Bulk IN 出错 status=1（连续第 1/3 次）` → 重提交失败 `ESP_ERR_INVALID_STATE` →
  设备已清理 → `link=none`（冒烟转录第 181–185 行；监听日志的 FM-1 拔出段只有其后的 `重提交失败`）。
- **`ENG SELFTEST` 期间 `psram_min` 掉到约 29.3MB**：未接 `psram_min=29350520`（`psram_free` 30,071,420）、已接 `29344924`
  （30,065,824）——P4-F 10c 的堆转录同款（`29373824`），自检期的既有瞬态，非回归；自检之后 `psram_free` 回到开机值。
- 注入场景自己截的 PNG（`firmware/build/verify/inject-*-20261126-*.png`）与 A/B 四次 600s 的串口回显
  （`soak-20261126-{154139,155304,160428,161903}.log`）没有入库；首轮 B 轮的完整摘要也不在包里（只有 `ab-status.txt`
  摘录的三行采样）。
- 全量自检期间每次各有一条 task-watchdog 报告：未接（`41-selftest-heap.log`）是 `IDLE0 (CPU 0)`、`console_repl` 在 CPU 0；
  已接（`att-heap.log`）是 `IDLE1 (CPU 1)`、`console_repl` 在 CPU 1——没喂上狗的是自检所在核的 IDLE。既有现象，与
  `verification-log.md` 里登记通过的转录同款（如 `4-p0-2026-11-21/task-11-selftest-full-transcript.log`；DI-64）；
  各次自检均 PASS、未重启。
- **两份合并相关日志不含 MANIFEST 校验的输出**（第 22–23 行）：主控的清单把 MANIFEST 列为第 11 项，日志里只有其余十项的
  结论行。入库时在 `dev` @ `aba1499` 上只读复核了一次：`cd third_party && sha256sum -c MANIFEST.sha256` ⇒ 320 行全 OK——
  这是入库者的复核，不是那两次运行的输出。
- host-test 里 state 测试的编译有一条 `state_test.c:2700:1: warning: '/*' within comment [-Wcomment]`（两份日志都有）：
  host 侧 gcc 警告，13:18 的 `11-state-test.log` 同款，既有；固件构建本身是 0 警告（第 23 行）。
- 设备态：全矩阵与 A/B 按设计改写过工程槽 0（A/B 的准备步骤 `SAVE` / `LOAD`）；autosave 的工作工程是复位后那次 soak
  之后的状态。

## 5. SHA256 清单

本目录内除 `readme.md` 外的全部文件（101 项），在证据目录内生成；校验：
`cd docs/dev/verify/5-pa-2026-11-26 && sha256sum -c <(grep -E '^[0-9a-f]{64}' readme.md)`。

43a39b8f6b80736b0bd8d8ede54466ed28f07a890a21d0bebeee455e5cb07c51  01-build-clean.log
1c8302f32281ce4bac8cd62b2a44d522b6ea820659c18d95dc705c984d607b28  02-build.log
1c8302f32281ce4bac8cd62b2a44d522b6ea820659c18d95dc705c984d607b28  03-build.log
6c4ab765d05b3a1a8b97c42be1fd46db3cc688ba9a9b6a7d2ce099fa342e2f15  10-host-test.log
cc1b91ee6cf6501b7ea7612679da97654f60c2315436844a092e8546703fc748  11-state-test.log
2f7de5e7e68d10748c4b7f11bdc16a837e248a60821cbfcbcec72887f6ebadba  12-knob-test.log
f87b25ccdd0220429f5700691483105ece094f0136d7b426c3b7940ca0a92141  13-seq-test.log
01ad8216f09601581f4e7a128034ea9e6a139923aacce1c1a8bb9ec970cadbec  14-synth-test.log
450bea249fe9d5113dd8a3aadaceee016679ce95dbecafa13c0d36eca9a44eac  15-editor-proto-test.log
0e6677c5d92224f6156774ec19cacd4c62ab985c3bf7559152719e77504f988f  16-usb-fm1-test.log
a246f7e621fd81277f9bdb0c617cf76554d8d63bfd60013e3ef11b4b9b4a2d4a  20-soak-selftest.log
71e9fe2326b54804979d6d97b4ab6ec1acde02d237752ad08d28c31807d5d8f9  21-screenshot-selftest.log
b8b937e77e59e35f9e04a412a2a96388caaad6916fa037906856dfac76fe27de  22-fm1-smoke-selftest.log
6283f33b713ca439a3503e5ead239f4997a2bb883140b957d5ba1000fd10f924  30-flash.log
127eae79c50141e2e72136058ee759a418a0551c507762155fde6c809da4153c  31-boot.log
5932f1ce703d44f5e5bb1186221c772d1f0077b5e75f8564a6452646ab05da20  32-nolink.log
7358d87dfeb866a414067c55bd90e213523e6d45dc269256c9fbb8603c2d35ea  40-device-selftest-transcript.log
d54fe3a2d33ed6a6ec29a4524a5f2d1bd923c80cac3e03ec63662253c611656e  40-device-selftest.log
90501a2251716f8b4bdc98b971a8708684ef8b88b65dcc950ea4f39c2e829c16  41-selftest-heap.log
f49a312211af7c5bce402dd4fdb44092cbcb77d7053daedbefc0a37f72960dbc  50-inject-transport.log
68347ae736d891ad2b16492e912258828318007cdf4fd5cbb2f16227e0c9a6cf  51-inject-step_edit.log
d76a5c0b17729bbd81d99c41fce52cc04c93c738055560fc8a5326ac46dc7ed2  52-inject-tracks.log
6b5be0443b97a0bde9f20858c2c6fdb687f4174ab1e3816630f373d78d4beb8e  53-inject-nav.log
a5886171868617d22984d10091af5760e20b9388f80bf06fe5a75b8c29d9d57f  54-inject-params.log
7201ec717a3a8403d430a9375eab9fbd7f0902285e54787b58cb3c52b86d71aa  55-inject-store.log
3d1d94fbdc7ae4cb66974eff8a0f3272a68cba9cfaa17c069ade4090b8a8c8b9  56-inject-song.log
6a661f7c5192c6155b19815030705d07e0bb5ff5accaa1e4c212384547182a5e  57-inject-sequence.log
1d67366482241f55041e01bae61c653ffff36d0ae3008d29aaf88f9f50e6cddb  58-inject-synth.log
c1d77a475495072ca5c52ab9114166b35ff0be460edced324f194b26aadc375c  59-inject-sys.log
6c390fd80675043f3170447d427b25ca62494084e4291dfc8c52e9137915fe16  60-screenshot-matrix.log
479737d7a198fd8d7ed05b75c8c66aed99a18888aa857754d05da45afcc6eb9f  70-soak-fail-sameboot.log
a36735b54a5c4d4693c01d86445d44a453c5a56e62111fd2a5deabbeb1c636de  70b-soak-fresh-summary.txt
529110752adc24fa795ac503d4ec5b1ae7fa8d6a89bf0079476def61a5b0ba69  71-soak-fresh.log
d93b670b87810d27efd17a6463ca3ff0cfcdcd4e43f79d9c29e40443d22bbce2  ab-A-5a-prep.log
0c8c381f1f0f93f30f8ce37aac974f2f3fe155496fcac0c7655cb4004d75adae  ab-A-5a-soak.log
14fbde44f42355dfbb12be0b3dcc7eb41ebdec867395b67f2957f64036322cfd  ab-A-5a-summary.txt
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  ab-B-boot.log
eeb02fd69d82e8053e05ae7807a5d7eaa21253acbb6e571156b8de815777b830  ab-B-dev-prep.log
406a985df062dd15ae08fcab0ba7b4db63ce4741ddfa8cefcbff5198134b2006  ab-B-dev-soak.log
4df8acc9827004ab15703ffac979608803d2f104d79ff4cdfafda58bb5ec58e2  ab-B-dev-summary.txt
dbdbb05c412df7ad2f661d093c6303b92bc2fc0f31d9c13dea05cb47c2ac8745  ab-C-5a-prep.log
2648a87bbcf8262d773c27ac475b958238e3ce9397dfeb3c1c35b61560d40d82  ab-C-5a-soak.log
c96873321e7b7e211f2fbc2fa660405aea145507cb5574734b49df72d17e3321  ab-C-5a-summary.txt
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  ab-back-boot.log
6fb0354d920fa45233eeb18daec7a60fed668b9edab2b72fb1d62ef25901c305  ab-flash-5a.log
1181aba47026de5102801113007ad35dee7e7ab57858b7c1f82012d000af8449  ab-flash-dev.log
52a16a8bad2a7ccb8beaaf9ca33e2572d8caec6a5bcad384eedb6b5d74be4413  ab-status.txt
627bc33dcc7c096e6f8845eae1974a6ebbdafb864ea84c130b6211863658d1a0  att-182-transcript.log
9fcdb9d8996dce196906d54321e2d94a0aa73998eaca3f2e406fcd53b1c223e0  att-182.log
23fa101b7dfee4f8fb4cf74bcf3bdd8152c505fe9b5746ec8dabc4dd61671b36  att-fm1.log
c73bd42e800b2039662710ae38349048228ccdbb41c611c261474e168d063622  att-heap-transcript.log
fa65fd377c9ba02b6a815e009af908c64df98ba1b5fa895e5679279b74e373b5  att-heap.log
02b5c794937227ec413e4baf4932d565c484f7739f5ed80ccba7667b91673322  att-status.txt
0f51b99e8a019b22591113cf97ffb9e91966183d574ed3c1b7a66e046b6925b2  finish-selftest.log
f15ebdb88ffd15ad943a36a4127ab038a2cdca479cc312b25d9648613e11b7ba  fm1-nonmidi-watch-2.log
e1e8d18cea94ced9ad36a1bcd0d3a0255ba708d550a65fa1104f769515ac3fa5  fm1-smoke-20261126-190513.log
98868449aa25005a2315c8d6681cdaa303c8ff448be6e191ce0441258895a30c  inject-nav-20261126-133636.log
39c8116835e0ec31f7b06fcf9a49678f45162af7f24e980c91d8f03415207076  inject-params-20261126-134512.log
5d347b011739dd2963fddc774d150a48cf86346a0641b696cedae7bad9a0770d  inject-sequence-20261126-140535.log
84c2e4730644958fbd9ef48f3beb2ceb73161f2cbbbc4c6a2d2edec95a4a01fb  inject-song-20261126-140222.log
08f4866723781db8dd7b2e6023ff917961ca10a3b9f360a361ea74868f4400f3  inject-step_edit-20261126-133057.log
e124617c520f3d1570c09b801f0e808c96516efd554b0c13254c2586a2e04d1e  inject-store-20261126-135600.log
2bf56d03033420f80827e0237f7ffca1c00c535ef25834b10e94152a006e9b1d  inject-synth-20261126-140819.log
b467544b5f2e0c1c5669cffe1546487901ed7f289cef313ff146c03f725f7675  inject-sys-20261126-141114.log
8087e0eb4afc81314b64e36c410b5a60df5fbf535b615f5fe5339b7847ca8220  inject-tracks-20261126-133504.log
b7b40cf68b22ffa37f245e777b301669891be9630208afb50ec1600f46e5163e  inject-transport-20261126-132818.log
cadf5d084f55ee79f32003354087e4e17c1d3e5c2138298c68e4fcfdd4ab2242  matrix-20261126-142157/dsyn-default.png
f85a40750e16f3ee6610a1383debd7e5e4a807504e772a132a7a8930d73e4cf9  matrix-20261126-142157/fm6p-default.png
5dfdec9c1715628aee314039b29d6b61ab99b68fd7efe55b756770a4cea36d72  matrix-20261126-142157/fx-default.png
642da72c1f0c958142b56c55582ff1814b736bdbe68ad6f98f5c793728217219  matrix-20261126-142157/global-default.png
a908d33474d5c41e6016c618c8417ffcb7cab5f64e62c5f5d7ef1b1704b77d8d  matrix-20261126-142157/mod-default.png
8fedf5a749657d8a1b8a9c202cc6e232db9457b3d3377e28507fe5ab93d62991  matrix-20261126-142157/play-default.png
31a800a6297a42220ef4ea65076118963701e02c6aba201f8778787da5d5eccb  matrix-20261126-142157/presets-default.png
71eae07ed2ba45efbfcd6318295852447295e3909b664a98c76a94f2e7aea894  matrix-20261126-142157/projects-default.png
76d6a3faf0746b280efa6e48ef372edfcd5e29f5ca24e1f470cbf9f4123a3401  matrix-20261126-142157/sample-default.png
f08c7431e941520441911b3ddd89459fe0c0aa31244bcfca861f79913ea193b4  matrix-20261126-142157/scale-default.png
bb85ed41d938e952ac98b81881ee2c1ad0b934b5b24fb94e985ea1f125f5fa41  matrix-20261126-142157/sheet-all.png
cb0e8700eb1ce9cf4c979a7b4ea2155622f77ecb70d276b4810d7fd850420511  matrix-20261126-142157/sheet-edit.png
4040e10b3283c4048144e1567cf43e81a7865671679a8a942091f69029bf3ccb  matrix-20261126-142157/sheet-file.png
d3e982a3ab5f92784a756c5720ea2371f122ff222a73c63ebee1342dd2fc8509  matrix-20261126-142157/sheet-mix.png
57cacc7905b86a45c7510ee12ac12ffd540062b753ddac924bf30bca0fab684e  matrix-20261126-142157/sheet-play.png
e38bd276dfbad902cd7c6efcd93d1960b48d0d5d9107643438ef22cdd47e1f55  matrix-20261126-142157/sheet-seq.png
6e3bd9c82973ac6c8acfd0bf9fb5888910aac8650f9992362ab526e1856a53d1  matrix-20261126-142157/sheet-song.png
04f7a66c6ec7c8de6e25adea7bc6146979032acd3fd2abaf9353d2d6148ac95e  matrix-20261126-142157/sheet-synth.png
57ed96e57c34901687f25d19f97323597658685b3fb6d5ed3e728dccf132bee7  matrix-20261126-142157/sheet-sys.png
9cf46b6c0f1fa250713d40986a9104588c51aab2cfa6b35f3e4681d21a6ed0e4  matrix-20261126-142157/sheet-tracks.png
aee3f17bf75074115e57064fd75f559d11013cb8ffcbd0221ddd97011ee7dca7  matrix-20261126-142157/song-default.png
eb9aaf48ac557c9a7c77ba5331e4d0ee9d4a290c51ddec35c38f6166a6b27595  matrix-20261126-142157/sound-default.png
b9002435595275f23a5e7d98556620df00f96ee8bd8855811fa9e71c390c976b  matrix-20261126-142157/step-default.png
6a7eb735497d3ebbb3529f79fa226f21e822013f7409bf4febd06c6d5a44aaa7  matrix-20261126-142157/system-default.png
c0e21b67bff85bfd77623687d797b1247ec008ef32c272d851ac7d52acd64f7b  matrix-20261126-142157/tools-default.png
005af4a19815c1d0e8e239569433f8e190e4ad2a0279fa36ce6d461048de436e  matrix-20261126-142157/tracks-default.png
da1425e8e83dad422535cbb6383dfde83fc970a7317327a5697f41c2305d49dd  matrix-20261126-142157/transport-default.png
956b6e3ea3e57cb0862a3fa857cb6d3751b7b13474fdc2f5d7bd0c1a7230ded7  matrix-20261126-142157/voice-default.png
fb9beedd435f726fcc15c5c3b22992e593d1a1e709e15225dbe27bdc6f71a8c6  postmerge-gate.log
621a51f64d4c5087759d8046f19d442b5a0eddecf2c1d81c500a507a68693950  selftest-heap-20261126-132250.log
c2dc61103cb9d4b6443d84ab9ad66bd860ffa73127d4fe3054fd5a66221d0c14  soak-20261126-143458.log
23d179da8ebab9b31652a72100910a3a6f5cd5e70d208e20ad5c5c0c717e0124  soak-20261126-150834.log
32b33d920bcd89db3e2b9639178b59e4aa95a09658595ec05066a6c5a268fafc  soak-summary-20261126-143458.txt
a36735b54a5c4d4693c01d86445d44a453c5a56e62111fd2a5deabbeb1c636de  soak-summary-20261126-150834.txt
2c2b14669cb53dae14640bf4a4e6319f48d28c60dcba461a573bd1f1971ed362  status.txt
