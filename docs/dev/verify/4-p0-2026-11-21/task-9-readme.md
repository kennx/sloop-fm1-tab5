# Task 9 — `sloop_core_set_params` + 状态层参数命令与投影

日期：2026-11-21｜分支：`feat/phase4`
代码提交：`feat(components/sloop_core): 批量参数写与参数快照读取`、
`feat(components/sloop_state): 参数命令与批量设置投影`、
`feat(firmware/main): PARAM 参数写诊断命令与 STATE SNAP 参数投影`；证据/登记见本目录与
`docs/dev/verification-log.md`（docs 提交）。
受测固件：上述源码 clean 构建 ×2 后重烧重采（构建时 HEAD = `4506b9a`，工作树含未提交
新源码，启动日志 `App version: 4506b9a-dirty`）。

## 0. 结论

- **完成**。引擎批量参数写/读 API 按 spec §4.1 逐字落地；状态层两条参数命令
  （`CMD_SET_PARAM` / `CMD_SET_PARAM_BATCH`）与 `app_state_t.p[]/g[]` 投影落地，
  RED→GREEN ×2。
- 设备 22 项断言全过：单 SET（含补码）、上下限钳位、非法 id 拒绝、引擎槽 id、
  BATCH 三对、鼓轨 kit 哨兵（`P_E0` 0..45、`P_E1` 拒绝）、`STATE SNAP` 立即反映、
  `ENG SNAP`/`ENG STEP` 不受非结构参数影响、收尾还原默认。
- 构建 ×2 警告 0；三段 `Hash of data verified`；启动干净；host-test 绿；设备
  SELFTEST 单例 PASS（未改 DSP）；state-test 13 passed；manifest 233/233。
- 未改 UI 渲染/布局，§5.4 截屏视觉门禁不适用（`STATE SNAP`/`PARAM` 是 console 面）。

## 1. API 契约

| 项 | 决策 | 依据 |
|---|---|---|
| 引擎签名 | `sloop_core_set_params(uint32_t track, const uint8_t *ids, const int16_t *values, uint32_t n)` | spec §4.1 逐字（控制方裁定保留双数组形式） |
| 锁粒度 | **一次** `s_engine_lock` 覆盖整批 | 预置加载 71 参数不再 71 次往返（spec §4.1） |
| 钳位源 | 与 `sloop_core_desc` 同源：`id < P_E0` → `TP[id]`；`P_E0..P_E7` → 合成轨 `ENGINES[trk[track].eng_req]->edit[id-P_E0]`、鼓轨仅 `id == P_E0` → `DRUM_KIT_DESC`（kit） | 上游 `params.c` 的 `lock_desc()`/`track_desc()` 同一张表；`eng_req` 与 desc/UI 投影一致（淡入淡出期间 `engine` 可能短暂落后，见 `sloop_core.h` 注释） |
| 无效 id | 逐条忽略、**不中止整批**（有效项照常应用；回读 `read_params` 取真值） | 控制方裁定；预置/参数页混合下发时不应因一条坏 id 丢整批 |
| 整批丢弃 | 越界 track、`ids/values == NULL`、`n == 0`、冻结态 | 与其它引擎写 API 同口径（诊断期不取锁） |
| 读取 | `sloop_core_read_params(track, p, g)`：一次锁拷 `trk[track].p[P_COUNT]` + `song.g[G_COUNT]`；track 越界 p 全零、g 照常；冻结态两者全零 | 状态层一次调用即可刷新 71+50 个值（不逐项读）；冻结态对齐 `read_step` 口径 |
| `CMD_SET_PARAM` | `a=track b=id v=value`，以**批量入口 N=1** 下发；`v` 为 16 位补码（双极参数用 `(uint16_t)(int16_t)x`） | 钳位/非法判定只在引擎一处，状态层不复制 desc 范围 |
| `CMD_SET_PARAM_BATCH` | `a=track p=sloop_param_pair_t[] n=count`；POD `{uint8_t id; int16_t v;}`，`sizeof == 4`（`_Static_assert` 保证 host/xtensa 一致） | 控制方裁定；避免给引擎加第二个变体 |
| 批量生命周期 | **同步 apply**：`p` 只需在 `state_apply()` 返回前有效；状态层当次解包到栈数组（`APP_STATE_PARAM_BATCH_MAX`=128 → 384B）后一次下发，不保存指针；超过 128 对截断并 `ESP_LOGW` | spec §4.1 的同步语义；POD 不是字节流格式（不得按协议 pack7 解读） |
| 投影 | `app_state_t.p[P_COUNT]` = **选中轨**参数、`g[G_COUNT]` = 全局；`state_sync_from_engine` 与窗口快照同拍一次 `read_params` 整份覆盖；参数命令落地后立即回读（含引擎钳位结果） | 控制方裁定；参数页按 `core.selected_track` 显示 p[]，选轨变化下一拍对齐 |
| 非选中轨写入 | 引擎落地，但 p[] 仍读选中轨，不被覆盖 | p 的定义即「选中轨投影」 |

`STATE SNAP` 新增两行（抽样字段）：

```
STATE SNAP param trk=0 level=104 atk=10 dec=70 sus=90 rel=60 slen=16 mute=0
STATE SNAP global bpm=90 swing=0
```

`PARAM` 回显（钳位在引擎，`applied` 即钳位结果）：

```
PARAM track=0 id=1 value=500 applied=127 min=0 max=127
PARAM BATCH track=0 n=3 id=0 value=100 applied=100 id=1 value=20 applied=20 id=29 value=8 applied=8
PARAM ERR track=0 id=999 reason=invalid_id
```

## 2. RED → GREEN

| 阶段 | 证据 | 结果 |
|---|---|---|
| RED | `task-9-state-test-red.log` | 新用例编译/链接通过（头文件+stub 先落地），实现缺失 → `10 passed, 3 failed`：k=9（单 SET 无引擎调用/投影全零）、k=10（批量无调用/截断不对）、k=11（init 投影 g 全零、sync 不读参数） |
| GREEN ×2 | `task-9-state-test-green1.log`、`task-9-state-test-green2.log` | 均 `13 passed, 0 failed`，两日志逐字节相同（`cmp` 相等） |

RED 的生成方式：`git stash push -- src/cmd.c src/state.c` 后跑 `state-test.sh`，再
`git stash pop`（测试与 stub 保持最终版本，确保 RED 对的是最终断言）。

用例覆盖：单 SET 一次调用（轨号/id/值原样，钳位由引擎）、投影采纳引擎钳位
（500→127）、补码 `-1` 原样下发、越界 id 引擎忽略且投影不动、`id > 255` 不调引擎、
越界轨丢弃、鼓轨 P_E0 落引擎；批量一次调用 + 入参保序 + 越界 id 逐条忽略 + 非选中轨
写入不动投影 + 129 对截断到 128 + NULL/n=0/越界轨整条丢弃；sync 一次 `read_params`
采纳引擎外部改动、p 跟随选中轨切换。

## 3. 设备证据（`task-9-device-transcript.log`，22/22 PASS）

会话脚本 `task-9-device-check.py`（本目录，ad-hoc；transcript 为串口原始回显，仅 `# [param-check]` 工具注记）：

| # | 命令 | 断言 |
|---|---|---|
| 1-2 | `STATE SNAP`（复位后） | `param trk=0 level=104 atk=10 … slen=16`、`global bpm=90 swing=0`（默认值） |
| 3 | `PARAM 1 50` | `applied=50 min=0 max=127` |
| 4 | `STATE SNAP` | 立即反映 `atk=50`（参数命令写后回读） |
| 5-6 | `ENG SNAP` / `ENG STEP 0 0` | 与基线**逐行相同**（写 P_ATK 不改快照结构字段/步表） |
| 7 | `PARAM 1 500` | 上限钳位 `applied=127` |
| 8 | `PARAM 1 -50` | 下限钳位 `applied=0`（负数解析） |
| 9 | `PARAM 999 1` | `PARAM ERR track=0 id=999 reason=invalid_id`，不写 |
| 10 | `PARAM 63 5` | 引擎槽（ANALOG WAVE）`applied=4 min=0 max=4`（TRACK/ENGINE 两段 desc 源） |
| 11-12 | `PARAM BATCH 0 100 1 20 29 8` + `STATE SNAP` | 三对一次命令落地；投影 `level=100 atk=20 slen=8` |
| 13 | `ENG SNAP` | `trk k=0 level=100 len=8`，其余字段不变 |
| 14-15 | `UI tracks` + `TAP 1108 347` | 选中鼓轨（`-> TRACKS select track=3`） |
| 16 | `STATE SNAP` | `param trk=3` |
| 17 | `PARAM 63 99`（鼓轨 P_E0=kit） | `applied=45 min=0 max=45`（DRUM_KIT_DESC） |
| 18 | `PARAM 64 5`（鼓轨 P_E1） | `PARAM ERR … reason=invalid_id`（鼓轨只有 P_E0） |
| 19 | `PARAM 1 30`（鼓轨 TP 参数） | `applied=30`（鼓轨同时有 TP 参数，与上游 `track_desc` 一致） |
| 20-22 | `TAP 172 347` + `PARAM BATCH 0 104 1 10 29 16 63 0` + `STATE SNAP` | 收尾还原选轨 0 与默认值 |

设备侧 BATCH 的“一次引擎调用”无法从串口观测，由 state-test 的 stub 计数断言
（`set_params_calls` 增量 == 1）覆盖；设备侧证明的是批量路径端到端生效。

## 4. 门禁

| 门禁 | 证据 | 结果 |
|---|---|---|
| §5.5 构建 ×2 | `task-9-build-clean.log`、`task-9-build-incremental.log` | 退出码 0；编译警告 **0**（clean 首遍仅 §5.5 豁免的 kconfig 提示对，增量遍 0 提示）；镜像 `0x1737f0`（分区余量 82%） |
| §5.5 烧录 | `task-9-flash.log` | 3× `Hash of data verified` |
| §5.5 启动 | `task-9-boot-log.txt` | `App version: 4506b9a-dirty`、`stream started`、`UI 就绪: 4 页注册/1 已建`（池 12,724/63,120）、`InputManager 就绪：55 条映射`、`触摸反馈 OFF`；无 fatal/abort/backtrace/panic/task_wdt |
| 状态层 | `task-9-state-test-{red,green1,green2}.log` | RED 3 failed → GREEN ×2 13 passed（逐字节相同） |
| 引擎/DSP 回归 | `task-9-host-test.log` | `ALL HOST TESTS PASSED (subset)`（182 golden 0 changed/0 gone） |
| 设备 SELFTEST | `task-9-device-selftest.log` | `preset/ANALOG/00_808_BOOM`：`cases=1 identical=1 mismatch=0 result=PASS`；未改 DSP，全量 182 口径沿用 P4-0 Task 2 |
| 设备参数断言 | `task-9-device-transcript.log` | 22/22 PASS |
| scripts 工具 | `task-9-screenshot-selftest.log` | `screenshot.py --selftest` 全过（scripts 未改，回归） |
| 第三方完整性 | `task-9-manifest.txt` | `sha256sum -c MANIFEST.sha256` 233/233 OK |
| §5.4 UI 视觉 | — | 不适用：未改页面/布局/渲染，未新增屏幕状态 |

## 5. 复现

```bash
scripts/state-test.sh                                        # 13 passed
scripts/build.sh --clean && scripts/build.sh                 # 警告 0 ×2
scripts/flash.sh                                             # 3× Hash of data verified
# 启动日志：复位后开串口读 9s（本目录 readme 的 device-check 前一步）
python3 docs/dev/verify/4-p0-2026-11-21/task-9-device-check.py \
        docs/dev/verify/4-p0-2026-11-21 /dev/ttyACM0          # 22/22 PASS
scripts/host-test.sh
python3 scripts/device-selftest.py /dev/ttyACM0 --case preset/ANALOG/00_808_BOOM
python3 scripts/screenshot.py --selftest
(cd third_party && sha256sum -c MANIFEST.sha256)
```

## 6. 备注 / 后续

1. `PARAM` 是**诊断命令**（阶段 4 P4-A 的设备断言入口），不是产品面；参数页
   （Task 10）走状态层命令，不经 console。
2. 参数投影 `p[]` 是选中轨的一份拷贝（约 142B）+ `g[]` 100B；sync 每拍多一次短
   引擎锁（`read_params`），与窗口快照分两次取锁——两者之间若发生参数写，最多一拍
   不一致（下一拍自愈），与既有 `core` 快照语义一致。
3. `ENG DESC` 仍不投影动态 `desc` 钩子（INSERT A/B/C、ENV SYNC、engine mode 依赖
   描述符，Task 8 已登记）；参数页若需动态 label/names 须另加带 track 的查询。
4. Task 8 carry 的 console `first`/`last` 枚举名末元素解引用兜底未在本任务处理（未触
   该路径；建议随 Task 10/11 的 desc 扩展一并修）。
5. 鼓轨 P_E1..P_E7 在设备上被拒绝（`invalid_id`）是保守选择（上游鼓轨无引擎槽）。

## 7. 评审修复轮（1 Important + minors）

**Important：选轨后参数投影滞后一拍。** 旧实现 `cmd_select_track` 只写
`core.selected_track`，`p[]` 要等下一拍 30ms sync 才换成新轨——参数页/`STATE SNAP`
可能落在「标题=新轨、参数=旧轨」的一帧。修复：选轨落地后在同一 state 锁内立即
`sloop_core_read_params(track, p, g)`（与其它编辑写路径相同的 state→engine 锁序）。
state-test 新增 k=12：轨 0/1/2 各设不同参数，`CMD_SET_TRACK 2` 后**不调 sync** 即断言
`p[0]/p[1]` 为轨 2 的值，`CMD_SELECT_TRACK 1` 同理。

| 证据 | 结果 |
|---|---|
| `task-9-fix-state-test-red.log` | 仅回退 `cmd_select_track` 的刷新调用 → `13 passed, 1 failed`（k=12 三条断言 `got 11 want 33/44/22`），其余用例保持绿 |
| `task-9-fix-state-test-green1.log` / `green2.log` | 均 `14 passed, 0 failed`，两日志逐字节相同 |
| `task-9-fix-build-clean.log` / `task-9-fix-build-incremental.log` | 均 rc=0，警告 **0**；镜像 `0x173980`（82% 余量） |
| `task-9-fix-flash.log` / `task-9-fix-boot-log.txt` | 3× `Hash of data verified`；`App version: 2b455bf-dirty`、`stream started`、`UI 就绪 4 页/1 已建`、`InputManager 55 条映射`、`触摸反馈 OFF`；无 fatal/abort/backtrace/panic/task_wdt |
| `task-9-fix-device-check.py` / `-transcript.log` / `-device-check.txt` | **41/41 PASS**：每轨不同 ATK（11/22/33/44）→ TAP 选轨后**紧接** `STATE SNAP` 显示 `param trk=N atk=对应值`；`PARAM RAW 200 7` → `applied=n/a` 且参数不变；鼓轨 `PARAM RAW 64 5` → `applied=0`（未写）；`PARAM RAW 63 99` → 45；原有钳位/非法 id/批量断言与收尾还原全过 |
| `task-9-fix-host-test.log` / `-device-selftest.log` / `-screenshot-selftest.log` / `-manifest.txt` | host-test `ALL HOST TESTS PASSED (subset)`；SELFTEST 单例 `identical=1 mismatch=0 result=PASS`；screenshot 自测全过；manifest 233/233 |

**Minors 处理**

1. `sloop_core_read_params`：取锁后**复核** `s_frozen`（与 `sloop_core_read` 同口径），
   冻结在持锁期间置位时返回已清零缓冲、不读引擎。
2. 批量用例首项 id 由 0 改为 **2**（丢首项/错序必失败），相应投影/落值断言同步改为 id 2。
3. 引擎「无描述符 id 逐条忽略」路径：新增 console `PARAM RAW <id> <value>`（跳过 desc
   预判直下状态层命令），设备上实际走到引擎 NULL 分支（`200` 越界、鼓轨 `64`），
   `applied` 与 `value` 不同即证明未写；`sloop_core_set_params` 的 NULL 分支注释指向该证据。
4. stub 的 kit 钳位改为 **0..45**（与上游 `DRUM_KIT_DESC` 一致）；state_test 用
   `STUB_ENGINE_P_E0` 常量替代字面量 63。
5. 文档 nit：`stub_engine.h` 头部函数计数改为 **12**；本 readme/Task 9 报告中的
   「REPL 栈 4096B」更正为 **16,384B**（`console.c` 的 `repl_cfg.task_stack_size = 16384`）；
   `PARAM BATCH` 的 pair 上限按 esp_console 默认 `max_cmdline_args=32` 记为 ≤15 对
   （不是先前推断的 4 对）。
6. `docs/deferred-issues.md` 新增 **DI-15**：`ENG DESC` 枚举名首/末元素回显缺越界兜底
   （Task 8 carry 项，触发条件=上游新增带 names 的 F_ENUM 参数或 Task 11 desc 收口前）。
7. `param_desc_at` 注释点名上游动态描述符来源（`ins_desc()` / `ESYNC_DESC[]` /
   `engine->desc()`），并写明「钳位等价」前提（动态描述符目前只换 label/names/unit，
   min/max 与静态表相同）；上游一旦改变范围须纳入动态钩子。
8. `state_sync_from_engine` 注释显式写明「窗口快照 + 参数快照是两次取锁」的 ≤1 拍
   陈旧窗口与自愈语义，并注明选轨路径已即时刷新、不会出现跨轨帧。

修复轮提交：`cb2b960` `fix(components/sloop_state): 选轨即刷参数投影与覆盖补强`
（见 `docs/dev/verification-log.md` 的 Task 9 修复行）。
