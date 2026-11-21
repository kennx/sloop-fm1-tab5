# P4-B Task 6 证据索引（自动存档与上电恢复）

日期：2026-11-21 ｜ 受测固件：`560b588-dirty`（工作区＝Task 6 源码，提交前的现场；`App version: 560b588-dirty`）
详细报告：`.superpowers/sdd/2026-11-21-p4b-storage-presets/task-6-report.md`

| 文件 | 内容 |
|---|---|
| `task-6-red-run.txt` | **RED**：同一 `store.sh`（含新 ⑪ 段与 ⓪ 的 `STORE AUTO OFF`）在 Task 5 固件上跑 ⇒ 第一个新断言就炸：`回显缺少 /^STORE AUTO off$/`（回用法行），exit=1 |
| `task-6-red.log` | RED 的串口原始回显（逐字节） |
| `task-6-red-transcript.txt` | RED 的手工复核：`STORE STATS` 无 autosave 三项、`STORE AUTO ON/OFF` 均回用法行 |
| `task-6-store-green{1,2,3}.txt` | **GREEN ×3**：`bash scripts/inject/store.sh` 退出码 0、**287 PASS / 0 FAIL**。三次运行 h0=`D160C9B7`、h1=`07F7D713`、h2=`D160C9B7`、最终 autosave hash=`8AA92C9B` 全同；stdout 只差日志路径/主机计时/起始 BPM（上一轮 ⑪ 结束时的 111 或 137）与 ⑪c/⑪e 的实测秒数 |
| `task-6-store-transcript{1,2,3}.log` | GREEN ×3 的串口原始回显（固件断言行逐字节相同） |
| `task-6-store-newfw-firstattempt-2fail.txt` | 新固件的**第一次**运行：2 项失败（⑪a 的首次写 6s 超时）——暴露「`autosave_ms` 从 0 起算 ⇒ 开机首个写只能落在 uptime ≥20s 之后」的上游语义；断言据此改为「首次写宽松 + ⑪e 单独量纯 idle」，其余断言未放宽（见报告 §2） |
| `task-6-build1.log` / `task-6-build2.log` | 构建 ×2：rc=0、**编译警告 0**（两遍均 0 提示；镜像 `0x17dff0`，81% 余量） |
| `task-6-flash.log` | `scripts/flash.sh`：esptool 三段 `Hash of data verified` |
| `task-6-boot-log.txt` | 复位后的启动日志（`App version: 560b588-dirty`、`sloop_store: ready … auto=0x48349af4(3816B)`、`autosave 恢复：hash=F1AFECF4 sel=1 bpm=90`、`UI 就绪 9 页`）+ `ENG MEM internal_free=100,835` + `STORE STATS … autosave_writes=0`；无 fatal/abort/backtrace/task_wdt |
| `task-6-memory.txt` | 段归属：`s_auto/s_auto_hash/s_auto_ms/s_auto_checked/s_auto_writes/s_last_input_ms/s_auto_on/s_busy` 全在 `.noinit`（0x4ff1b130…，早于 `_sloop_selftest_bss_start`=0x4ff50bf4）⇒ `ENG SELFTEST` 的 restore 不会清零自动存档状态；`s_auto` 实体在 PSRAM；1s 任务栈 3072B/优先级 1 |
| `task-6-selftest.log` | `scripts/device-selftest.py`：全量 **182/182**（`identical=182 mismatch=0 checks=7 deterministic=1`）——**设备上有 autosave 对象且开机已 resume 的情况下**跑；基线仍是 init 时拍的工厂态 |
| `task-6-nav.txt` / `task-6-nav.log` | `bash scripts/inject/nav.sh`：40 PASS / 0 FAIL |
| `task-6-params.txt` / `task-6-params.log` | `bash scripts/inject/params.sh`：134 PASS / 0 FAIL |
| `task-6-soak-summary.txt` / `task-6-soak.log` | `python3 scripts/soak.py --duration 120`：**VERDICT PASS**（内部堆 100,583 → 100,355B、min 92,884B、xrun=0、duty_mean 10.30→11.10% ≤18.88%、音频栈 3,124B） |
| `task-6-soak-coldstart-fail.txt` | 同一 soak 在**复位后立即**启动的采样（t=1s duty_mean 21.07%）——首个采样的启动瞬态（render_blocks 仅 709），等流稳定 30s 后重跑即 PASS；登记为采样口径注意项，不是固件问题 |
| `task-6-host-test.log` | `bash scripts/host-test.sh`：`ALL HOST TESTS PASSED (subset)`（未改 DSP/格式层） |
| `task-6-state-test-green{1,2}.log` | `bash scripts/state-test.sh`：15 passed ×2（逐字节相同；`state_apply` 新增活动戳调用的 host stub） |

## 评审修复轮（R20–R22 + minors 1–6）追加证据

| 文件 | 内容 |
|---|---|
| `task-6fix-store-green{1,2}.txt` / `task-6fix-store-transcript{1,2}.log` | **GREEN（修复轮）×2**：`store.sh` 退出码 0、**run1 292 / run2 293 PASS（0 FAIL）**；⑪ 三个窗口改成强判别（见 `task-6-report.md` §8 的表）；两轮 h0/h1/h2 全同（`D160C9B7`/`07F7D713`/`D160C9B7`），autosave writes 均 1→2→3→4；第 2 轮的 autosave 绝对 hash 不同（`CBED18AE`→`84793974`），因为入参状态是上一轮自检后的工厂态（每轮内 `hash==LIVEHASH` 都成立） |
| `task-6fix-store-run1-selbug.txt` | 修复轮第 1 次运行：⑩ 段 11 项失败——**场景自身的隐含假设**（复位后 `selected_track` 随 autosave 恢复成 1，而 ⑩ 的 `PARAM` 作用于选中轨）。修法：⓪ 显式 `select_track(0)`。固件无关 |
| `task-6fix-store-run2-race.txt` | 修复轮第 2 次运行：⑪g 5 项失败——⑪f 在 `AUTO ON` 后又改了一次值，恰好落在 tick 之后，使「刚写下的 hash」与「当前 LIVEHASH」错拍。修法：ON 后不再改值，写的就是 OFF 期间改出来的现状（开闸即写 0.1s） |
| `task-6fix-selftest-transcript.txt` | **同一份 transcript** 内的配对证据：`App version: 2e3d66a-dirty` → `sloop_store: autosave 恢复：hash=5FB1EDD1 sel=0 bpm=90` → `STORE STATS … autosave_hash=5FB1EDD1 autosave_writes=0` → `ENG SELFTEST cases=182 … result=PASS` → `ENG AUDIO streaming=1`；`device-selftest.py --compare` 该文件 = `identical=182 mismatch=0` PASS |
| `task-6fix-build{1,2}.log` | 修复轮构建 ×2：首遍含 §5.5 豁免的 kconfig 提示对、第二遍 0 警告 0 提示（镜像 `0x17e170`） |
| `task-6fix-flash.log` | 修复轮烧录：三段 `Hash of data verified` |
| `task-6fix-memory.txt` | 段归属（含新增 `s_auto_task`）+ **启动顺序的代码位置**（`sloop_store_init` → `resume` → `autosave_start`）+ 失败路径的栈余量调用 |
| `task-6fix-nav.txt` / `.log` | `nav.sh`：40 PASS / 0 FAIL |
| `task-6fix-params.txt` / `.log` | `params.sh`：134 PASS / 0 FAIL |
| `task-6fix-soak-summary.txt` / `task-6fix-soak.log` | soak 120s **VERDICT PASS**（内部堆 100,555→100,323B、min 92,884、xrun=0、duty ≤11.15%） |
| `task-6fix-host-test.log` / `task-6fix-state-test-green{1,2}.log` | host-test 全绿；state-test 15 passed ×2（逐字节相同，含 `state_set_focus` 的新活动戳） |

## 二轮评审（R23：⑪d 的播放门）追加证据

| 文件 | 内容 |
|---|---|
| `task-6fix2-store-green{1,2}.txt` / `task-6fix2-store-transcript{1,2}.log` | **GREEN ×2**：`store.sh` 退出码 0、**run1 335 / run2 335 PASS（0 FAIL）**，两轮逐行相同；`h0/h1/h2=D160C9B7/07F7D713/D160C9B7`；⑪d 现在是「先 `ensure_silent_pattern()`（三轨 `P_SLEN=1` + 步 0 空）→ PLAY 中改 BPM → 前提 `autosave_hash != LIVEHASH`（两轮都是 `CD3CBFDE != A8204011`）→ 10s+3s 不写 → 窗口前后 `voices=[0,0,0,0]`」（四道门都成立，只有 `song.playing` 能拦）；窗口后 `P_SLEN` 归默认 16（否则副作用随 autosave 漂进下一轮 ② h0——本轮实测踩到 `h0=D160C9B7`→`7F23D623` 并已修） |
| `task-6fix2-selftest-transcript.txt` | 同一 transcript 内：`App version: 3e19ce8-dirty` → `autosave 恢复：hash=84793974 sel=0 bpm=140` → `STORE STATS … autosave_hash=84793974 autosave_writes=0` → `ENG SELFTEST cases=182 … result=PASS`；`--compare` PASS |
| `task-6fix2-build{1,2}.log` / `task-6fix2-flash.log` | 构建 ×2（第二遍 0 警告，镜像 `0x17e170`）、烧录三段 `Hash of data verified` |
