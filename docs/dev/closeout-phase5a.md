# 阶段 5A（连接打通）收口报告

> 分支 `feat/phase5-sloop-go`，受测固件 **`715fdae`**（boot `App version: 715fdae`，无 `-dirty`）。
> **状态：机器侧完成（2026-11-26）**——两种条件的门禁全部通过：FM-1 未接的全矩阵（含复位后的 30min soak PASS），
> 与 FM-1 已接的冒烟 `FM1 SMOKE PASS 7/7` + 182 + selftest-heap（最终固件 `715fdae`，19:05–19:18）。
> 首轮（12:59 / 13:04）的冒烟与 U 盘监听原始转录曾丢失，已由上述重跑补证；首轮读数作为历史保留（§6）。
> **除冒烟脚本里由人拧旋钮 / 拔插的步骤外，没有人工验证项**；全部截图只有像素探针 / 锚点结论，没有人做过视觉检查。
> 5A 本身没有 UI 页面，用户可见面是控制台回显与日志。
> 分支**已合回 `dev`**（合并提交 `aba1499`，§9）；`main` 未合并，dev → main 顺延，时机由用户决定。
> 证据：`docs/dev/verify/5-pa-2026-11-26/`（`readme.md`、SHA256 清单）与 `docs/dev/verification-log.md`。

## 1. 做成了什么

5A 是阶段 5 的第一个子阶段：把 Tab5 变成 FM-1 的 USB-MIDI host，并把「连接 → INFO 版本协商 → WATCH push 可达」
做成可脚本断言的门禁。**不做** UI 页面、请求队列、LIVE 遥控、音频采集（这些是 5B/5C/5E）。`1e36a4d..715fdae` 共 40 个提交。

| 任务 | 产出 |
|---|---|
| Task 0 | 文档与参考前置：路线图阶段 5 改写为 5A–5E；**ADR-002** USB-MIDI host 选型（`docs/design/013-usb-midi-host.md`，关闭 DI-1）；`third_party/sloop-fm1-go` 参考副本入库（`MANIFEST.sha256` 共 320 行 = `sloop-fm1/` 233 + `sloop-fm1-go/` 87） |
| Task 1 | `usb_fm1` 骨架：USB host 开机常驻（`usb_fm1_start()` 由 `main.c` 在音频启动之后、UI 启动之前调用，失败不阻断独立模式）；内部堆实测 |
| Task 2 | `editor_proto`：编辑器协议 v12 的帧编解码（v14 / pack7 / u35、INFO 解析、push 识别），纯 C、host 可单测（`scripts/editor-proto-test.sh`，6 例） |
| Task 3 | `midi_stream`：USB-MIDI 事件包 ⇄ SysEx 重组与分段发送（`firmware/components/usb_fm1/src/midi_stream.c`） |
| Task 4 | USB-MIDI 客户端：枚举 → 找 MIDI Streaming 接口 → claim → Bulk IN/OUT 环 → 断开清理与重插恢复（`usb_fm1.c`） |
| Task 5 | `fm1_session` 最小事务层（一问一答 + 超时、`WATCH 3` + `PING` 保活）与 console `FM1` 命令族：`FM1 STATUS \| INFO \| GET \| SET \| WATCH \| PING \| EVENTS` |
| Task 6 | 门禁脚本 `scripts/fm1-smoke.py`（操作员在环，离线自测 14 组样例）与文档收口 |
| 终审修复波 + 微修 | `FM1 STATUS` 增 `version=` / `need_upgrade=`、协商失败指数退避、冒烟加拔线后 5×PING 与重插 INFO 断言；修复波自己引入的 `s_ready` 置位次序回归由微修 `ab66a16` 关闭 |
| Task 7 | 最终固件上的全矩阵 + 证据入库 + 本报告 |

## 2. 证据汇总（固件 `715fdae`，2026-11-26）

完整表（命令 → 证据文件 → 读数）在 `5-pa-2026-11-26/readme.md` §1；逐门禁登记与 SHA256 在 `verification-log.md`。

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 `build.sh --clean` → `build.sh` ×2 | ✅ 全部 rc=0；后两遍 0 警告（首遍仅 §5.5 豁免的一对 CMake 提示） | `01..03-build*.log` |
| host：golden / state / knob / seq / synth / editor-proto / usb-fm1 + 三个 `--selftest` | ✅ 全部 rc=0：`182 golden renders (0 changed, 0 gone), 0 health failures, 0 voice / routing checks failed, 0 CPU entries over budget`；editor-proto 6/6、usb-fm1 5/5；`fm1-smoke.py --selftest` 14 组样例 | `10..16-*-test.log`、`20..22-*-selftest.log` |
| 烧录 + 启动日志 | ✅ `Hash of data verified` ×3；`boot_fatal=0`；`App version: 715fdae` | `30-flash.log`、`31-boot.log` |
| 无链路开机检查（FM-1 未接） | ✅ `FM1 INFO ERR no_link`、`FM1 GET ERR no_link`、四条非法参数 `FM1 ERR invalid`、`FM1 EVENTS` 全 0 | `32-nolink.log` |
| `device-selftest.py`（FM-1 未接） | ✅ `cases=182 identical=182 mismatch=0 … result=PASS` | `40-device-selftest{,-transcript}.log` |
| `selftest-heap.py`（FM-1 未接） | ✅ run 1 Δ260B / run 2 Δ36B（上限 1,024B），收尾 `internal_free=72,239B`（门线 61,440B） | `41-selftest-heap.log`、`selftest-heap-20261126-132250.log` |
| 十条注入场景 | ✅ 全部「全部断言与像素抽检通过」、rc=0；合计 5,014 PASS / 0 FAIL | `50..59-inject-*.log` + 十条串口回显 |
| 截屏矩阵 | ✅ `PNG 33/33 张；锚点 166 个；截屏复位重试 0 次`；起点 `LIVEHASH 6132542B` 还原、autosave 回 on | `60-screenshot-matrix.log`、`matrix-20261126-142157/` |
| soak 首跑（同一次开机，接在截屏矩阵之后） | ❌ FAIL（保留作对照）：0 项断言失败、41 项判定问题 = `xrun=1` + `duty_mean` 29.31%→18.16%——两条都是自开机累计量；原因是驱动脚本没按「硬复位 + 30s 后起跑」 | `70-soak-fail-sameboot.log`、`soak-20261126-143458.log` |
| soak 复位后重跑（15:08:34 → 15:39:43） | ✅ **PASS**：31 采样 / 1801s、断言失败 0、判定问题无；内部堆 73,119→73,099B（Δ−20B）、`internal_min=71,876B`、`xrun=0`、`duty_mean` 最大 18.31%（上限 18.88%）、池往返 +12B、温度 31.1℃、无 panic/WDT | `71-soak-fresh.log`、`soak-20261126-150834.log`、`soak-summary-20261126-150834.txt` |
| 渲染占空比 A/B（对照，不计门禁） | dev `f4b2026` 稳态 `render_mean_us` 1037µs（17.87%）vs 5A `715fdae` 1071 / 1071µs（18.45%）：差距来自 soak 起点工程内容，非 5A 回归；5A 使渲染均值 +34µs（+3.4%，约 +0.6 个百分点） | `ab-status.txt`、`ab-*-summary.txt`、`ab-*-soak.log`、`ab-flash-{dev,5a}.log` |
| 联动协议冒烟 `fm1-smoke.py`（FM-1 已接，最终固件，用户执行，19:05） | ✅ **`FM1 SMOKE PASS 7/7`**：`link=ready … watch=3 info=yes need_upgrade=0`；`FM1 INFO version="FELUCCA SLOOP 2.8.5" … proto=12`；PING ×10 中位数 1ms；ATK 0 → 1 → 0 并读回；拧旋钮后 `changed=1 … last=cmd=23 scope=1 id=0 value=81`，静置 10s 再拧 `changed=2 … value=80`；拔线 `link=none watch=0 info=no` + `FM1 INFO ERR no_link` + 连发 5 次 PING 均 1s 内有回复；重插 `watch=3 info=yes`、`FM1 INFO` / `PING` 恢复 | `fm1-smoke-20261126-190513.log`（头自报 `App version: 715fdae`） |
| 非 MIDI 设备插入（U 盘；重做，19:07） | ✅ 日志 `设备 addr=3 无 MIDI 接口`、`FM1 STATUS link=none … open_fail=1`、无 abort | `fm1-nonmidi-watch-2.log` |
| `device-selftest.py`（**FM-1 已接**，19:09:57 → 19:12:41） | ✅ `cases=182 identical=182 mismatch=0 … result=PASS`，rc=0 | `att-182.log`、`att-182-transcript.log`、`att-status.txt` |
| `selftest-heap.py`（**FM-1 已接**，→ 19:18:07） | ✅ run 1 Δ260B、run 2 Δ44B（上限 1,024B），收尾 `internal_free=72,675B`；之后 `FM1 STATUS link=ready … watch=3 info=yes`、`FM1 PING ms=1` | `att-heap.log`、`att-heap-transcript.log`、`att-fm1.log` |
| 首轮冒烟与 U 盘监听（12:59 / 13:04；**历史记录**） | 首轮结论 `FM1 SMOKE PASS 7/7`、`link=none open_fail=1` 无 abort——原始转录已丢失，读数出自主控当时核实的清单 | 无文件 |

## 3. 与 spec 的偏差

对照 `docs/superpowers/specs/2026-11-26-phase5-sloop-go-alignment-design.md` §6 的 5A 节与实施计划：

1. **`sysex_feed` 改为流式「返回消费字节数」接口**：`size_t sysex_feed(sysex_asm_t *a, const uint8_t *bytes, size_t n, sysex_result_t *result)`，FRAME / DROPPED 时返回值停在该事件之后，调用方循环把剩余字节喂完。计划里的单缓冲 + 单返回码接口在「同一包里 F7 紧跟 F0」时无法同时交帧与保留尾部字节。
2. **`FM1 STATUS` 比计划多字段**：`midi_if` / `ep_in` / `ep_out`、`watch=<0|1|3>`（计划只写 `<0|3>`）、`version="…"` / `need_upgrade=<0|1>`（版本政策的落地）。
3. **参数错误统一回 `FM1 ERR invalid`**（不带子命令名；id 128..199 也判 invalid），发送 / 解析失败另有 `FM1 <SUB> ERR bad_reply`。
4. **IN 环错误处理**：设备仍在时先清 halt 再重提交，**连续 3 次才断链**（中间一次 COMPLETED 即清零）；STALL / NO_DEVICE / CANCELLED 或设备已不在 ⇒ 立即 `link_down`。espressif/usb 1.6.1 的 `hcd_urb_enqueue` 拒绝 HALTED 管道，重提交前必须 clear-halt。
5. **内部堆用两步保住门线，门线没有下调**：`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`（`sdkconfig.defaults`）+ `sloop_input` 任务栈进 PSRAM（`fd0ffaf`）；spec 预留的「按实测固定占用下调 `HEAP_FLOOR`」没有用上。
6. **协商失败指数退避 1s → 30s**（spec §8 把超时与重试参数留给实施者决定；计划里没有这个退避）。

其余（帧格式、编解码规则、命令族、WATCH 3 + PING 保活、host 常驻、不做 UI）与 spec 一致。

## 4. 实测读数

- **真机 INFO**：`version="FELUCCA SLOOP 2.8.5" nengines=12 p_count=71 g_count=50 nstep=64 pe0=63 ntrk=4 proto=12`；接口 1 MIDI Streaming，bulk IN 0x81 / OUT 0x01；PING ×10 中位数 1ms（1–2）。（`fm1-smoke-20261126-190513.log`。）
- **FM-1 已接时的内部堆**：selftest-heap 收尾 `internal_free=72675B`；两项检查之后 `ENG MEM internal_free=72559 internal_min=71824`。重插后 claim MIDI 接口使 `internal_free 73155 -> 72775`。
- **推送计数（19:05 冒烟）**：步骤 5–6 结束时 `FM1 EVENTS total=6 changed=2 reload=4`，两条 CHANGED 都是 `cmd=23 scope=1 id=0`（value 81、80）。
- **内部堆（FM-1 未接）**：无链路开机 `internal_free=73467 internal_min=72288`；复位后 30min soak 全程 `internal_min=71,876B`，门线 61,440B 不变。
- **渲染占空比**：30min soak `duty_mean` 最大 18.31%（上限 18.88%，余量 0.57 个百分点）；A/B 稳态 dev 1037µs / 5A 1071µs。历史各阶段 soak 的占空比在 9–13%，今天的 ~18% 不是 5A 带来的（dev 固件在同一工程上同样 17.9%）。
- **首跑 soak 的两条累计量**：`xrun` 在 t=1s 已为 1 且 31 个采样不变；`duty_mean` 从 29.31% 单调回落到 18.92% 仍高于上限。

## 5. 裁决清单

按 SDD ledger（`.superpowers/sdd/2026-11-26-5a-fm1-connect/progress.md`）的记录顺序；每条带理由与「若错的代价」。
R9 / R16 带随后的更正。R20 的 ② 与 R21 在实施中真的触发，其余没有用到退路。

| # | 出处 | 裁决 | 理由 | 若错的代价 |
|---|---|---|---|---|
| R1 | 预检 | T4 的发送缓冲用静态区而非栈（1,373B） | console 与会话任务都会调，栈上放不下且计划表述含糊 | 改成调用方传入缓冲，改动局部 |
| R2 | 预检 | `FM1 EVENTS` 的 `last_*`：CHANGED / TRACK_CHANGED 取 scope/id/v14；RELOAD 取 engine/preset、value=0；STEP_CHANGED 取 index、其余 0 | 计划只定义了 CHANGED | 只影响一行诊断输出 |
| R3 | 预检 | Agent 工具没有 model 参数，子代理模型由 harness 决定；按任务复杂度选 `subagent_type` | 工具限制 | 无 |
| R4 | 预检 | [操作员] 步骤拿不到操作员时，实施者做完其余步骤并在报告列 WAITING_OPERATOR；主控在能做的都做完后统一请用户插拔，设备证据补齐前对应任务不标「设备验证通过」 | 计划全局约束 | 设备缺陷晚一个任务才暴露 |
| R5 | 预检 | T0 里 `tools/fetch_upstream.sh` 的改动与 vendored 副本同提交（`chore(third_party)`） | 同一逻辑单元 | 无 |
| R6 | Task 0 | 路线图 §0 第 4 条与 §7 完成判据 4 的「阶段 5 强制合并 dev→main」改为「合并顺延、时机由用户决定」 | 已接受的 spec §3 非目标明文规定不合并 main；计划里「保留现有措辞」是主控的疏漏 | 用户想要强制合并时改回两句话 |
| R7 | Task 0 | ADR-009:464「MANIFEST 校验保持 233/233」加注（sloop-fm1-go 并入同一清单，共 320 行；sloop-fm1/ 的 233 行不变） | 陈述已失实 | 无 |
| R8 | Task 0 | spec 里 5C 的「ADR-011」与既有声音设计 ADR 撞号，改为不带编号的「双模式调度 ADR」 | 编号在 5C 落档时再分配 | 无 |
| R9 | Task 0 | AGENTS.md §3 的 design/ 列表不为 013 加行；`fetch_upstream.sh` 清单的整体排序与计数口径改动接受。**随后更正**：列表实际到 012，Task 1 的文档提交已补 `013-usb-midi-host.md` | 主控原以为列表只点名 001/002（有误）；脚本改动无害 | 无 |
| R10 | Task 1 | `console-and-injection.md` 的 FM1 行按计划在 Task 5 添加 | 计划 §Task 5 Files 明列 | 文档晚一个任务同步 |
| R11 | Task 2 | `edp_u35_enc(0xFFFFFFFF)` 的断言改为 `u[0]==0x7F && u[4]==0x0F`（5×7 位 LSB first，字节 4 只装 bit 28..31） | 与上游 `editor.c` 的 `ed_bk_u32` 逐字复算一致，计划里的向量是主控算错 | 无（上游语义为准）；计划文件同步改 |
| R12 | Task 2 | `edp_pack7_unpack` 在 cap 不足时返回 0（与 pack 一致，不截断） | 截断会让调用方拿到看似合法的短数据 | 改成上游的截断语义，改动局部 |
| R13 | Task 2 | `edp_frame_parse` 增加一条校验：除首 F0 与末 F7 外的内部字节 ≥0x80 ⇒ 无效帧 | 计划的校验清单漏了这条，Review Focus 3（畸形帧）要求丢弃 | 放宽一条判断，改动局部 |
| R14 | Task 2 | `test_info` 里 `f.cmd = EDP_CMD_PING` 那条断言前先把 argc 恢复，使它真正测到命令号检查 | 原断言因 argc=8 先被长度检查挡掉，等于没测 | 无 |
| R15 | Task 2 | `EDP_ARGS_MAX` 维持 384（5A 用到的 INFO/GET/SET/WATCH/PING 都远小于）；`DSYN_GET`（约 416）超限记为 5E 的入口条件 | 5A 不发该命令 | 5E 开工时改成 512 |
| R16 | Task 2 | AGENTS.md §5.1 / §5.5 不单列 `editor-proto-test.sh`。**其后已列入**：§3 脚本树与 §5.5 host 单测行都补了 `editor-proto-test.sh` / `usb-fm1-test.sh` | 与 knob-test / seq-test 的既有处理一致 | 无 |
| R17 | Task 3 | `sysex_feed` 改为流式「返回消费字节数」形态；FRAME / DROPPED 时返回值停在该事件之后，调用方循环喂完剩余字节 | 单缓冲 + 单返回码的接口是设计错了；消费字节数是流式解析器的标准解法，对任意设备无损 | 接口多一个出参，Task 4 调用点改一行循环 |
| R18 | Task 3 | 5A 里只有 CIN 4..7（SysEx）事件包进入 `sysex_feed`；通道声音消息（CIN 8..E）不进汇编器 | 5A 只关心编辑器协议帧；FM-1 自身对 SysEx 内实时字节 / 其它状态字节的处理不在 5A 范围 | 5C 需要时补「SysEx 内状态字节中止」规则 |
| R19 | Task 3 | 超长帧返回 `SYSEX_DROPPED`、`drops++`、回到等待 F0 | 简报原文 | 无 |
| R20 | Task 4 | 按顺序处置内部堆：① 启用 `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`；② 若 ① 后 `internal_min − 61,440 < 4,096` 则把 `sloop_input` 任务栈（8192B）改 `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`；③ 仍不过才重定门线（须主控 / 用户裁决） | spec 5A 内容 0③ 的顺序；标准开关优先于改别的子系统 | ① 在数据通路上若出问题可回退（只影响缓冲位置）；② 若输入任务异常可回退一行 |
| R21 | Task 4 | 设备仍在时对 ERROR / OVERFLOW 等错误重提交，但设「连续错误上限 3 次」（中间一次 COMPLETED 即清零）；STALL / NO_DEVICE / CANCELLED 或设备已不在 ⇒ 立即 `link_down` | 兼顾计划要求与实施者对 STALL 打转的顾虑；一次瞬时总线错误不应毁掉整个会话 | 阈值 3 可调 |
| R22 | Task 5 | 接受 `fm1_session.h` 的三个额外声明（`fm1_session_on_frame` / `on_link` 供 main 接线、`fm1_session_info_cached` 供 STATUS 读缓存） | 否则 main 无法把回调接到会话、STATUS 得发帧 | 无 |
| R23 | Task 5 | 参数错误统一打印 `FM1 ERR invalid`（不带子命令名）；发送 / 解析失败打印 `FM1 <SUB> ERR bad_reply`；id 128..199 也判 invalid（id 是 7 位字节） | 与主控派发时的说明一致 | 只影响诊断文案 |
| R24 | Task 5 | 修复轮一并处理：等互斥超时也计入 timeouts；`send_frame` 传「剩余超时」而非整份超时（控制台阻塞上限回到一个超时周期）；文档更正（栈水位数字、`fm1_session` 归属 `usb_fm1`、WATCH 1 时 STATUS 可显示 watch=1） | 都是同一段代码 / 同一份文档里的一行改动 | 无 |
| R25 | Task 6 | 接受简报之外的增量（还原后再 GET 一次、STATUS 的 5s 窗口、`watch=<0\|1\|3>` 在 firmware-map 同步、scripts.md 目录树行、AGENTS §5.5 的「联动协议」行更新） | 都是为让门禁可执行 / 文档一致 | 无 |
| R26 | Task 6 | 冒烟脚本追加两条断言：第 5 步首个推送 `last` 以 `cmd=23` 开头；第 7 步拔线后 STATUS 为 `watch=0 info=no` | 这是关闭 Task 5「拔线后 watch 归 0」的唯一自动化证据来源 | 放宽两条断言 |
| R27 | Task 6 | 修复轮一并处理：Ctrl-C ⇒ 记录 error 并落结论行（退出码非 0）；`cmd=23` 后须跟空白（不接受 cmd=230）；第 7 步读到 `link=none` 但 watch / info 未归零时先等 200ms 重读一次再判失败 | 操作员在环门禁必须对每种退出方式都有结论；门禁不应被理论竞态误判（置 link=false 与清 watch 在两个临界区） | 无 |
| R28 | Task 7 | 全矩阵只在「最终提交」上跑一次——顺序改为：操作员冒烟 → 全分支终审 + 修复波 → 最终提交上的全矩阵 + 证据入库 + 收口报告 | 终审若触发固件修复，先跑的矩阵要重来（P4-F 的教训） | 多等一个终审的时间 |
| R29 | Task 7 | 按 spec 全矩阵分两种条件：FM-1 未接（独立模式全矩阵）与 FM-1 已接（`fm1-smoke` + 182 + selftest-heap）；计划 Task 7 Step 1 的「FM-1 插着跑全矩阵」与 spec 冲突，以 spec 为准 | spec 5A 验证节 | 多跑一遍矩阵 |
| R30 | 终审 | I-1 取最小实现：STATUS 追加 `version="…"` / `need_upgrade=<0\|1>`、协商失败指数退避（1s→30s）、非 SLOOP 设备不再刷屏 | spec 版本政策是已接受设计的一部分；真机是 2.8.5，无法实测旧固件分支，只能读证 | 多两个字段与一个退避 |
| R31 | 终审 | I-2：冒烟第 7 步加「`link=none` 后连发 5×PING 且每次 1s 内有回复」与「重插后 FM1 INFO 断言」；非 MIDI 设备步骤由操作员会话必做 | 计划 Review Focus 明列的两条不能只有散文 | 冒烟多约 10 秒 |
| R32 | 终审 | 修复波范围 = I-1 / I-2 / I-3 全部 + 便宜的 Minor（M-1/2/3/5/6/8/10）；M-4/7/9 与冒烟结论行归因小项保持延后 | 一次修复波、一次范围内复审 | 无 |
| R33 | 终审 | 修复波自己引入的「`s_ready` 在信号量创建前置位」是真实回归（开机微秒级窗口内 FM1 请求会取空互斥量），连同另两处小项做一次微修 | 复审规定没有第二波修复，但这三条是修复波的自身破损而非新发现，且全矩阵尚未开跑 | 多一个提交 |
| R34 | Task 7 | soak 首跑的失败是驱动脚本的缺陷（没按「硬复位 + 30s 后起跑」）；按标准口径硬复位后重跑 30 分钟 soak，以重跑读数为准；首跑转录保留并在本报告里如实说明 | 两条判据都是「自开机累计」量，非 soak 期间新发生（xrun 在 t=1s 已为 1） | 重跑仍失败则说明是真回归，需排查 |
| R35 | Task 7 | 合并 dev 前做受控 A/B：同一已保存工程（工程槽 0）、同一 600s soak，A = 5A `715fdae`、B = 5A 之前的 dev `f4b2026`、C = 5A 复测；C 结束设备回到最终固件 | 渲染占空比涨四成若为真会影响后续所有界面工作的余量，不能带着未知合并 | 多花约 40 分钟 |
| R36 | Task 7 | +0.6 个百分点的渲染开销可接受，合并照常；登记 DI（soak 起点工程归一为固定参照工程；5A 的 +3.4% 渲染开销待在 5B/5C 定位）；并把 soak 口径「硬复位 + 30s 后起跑」写进 verification-gates | 门禁通过（30min soak 最大 18.31% < 18.88%），余量 0.57 个百分点偏紧需显式跟踪 | 余量被 5B 之后的改动吃光时再回头查 |

## 6. 已知边界与延后项

**设计固有限制**

- **协议无序号**：超时后迟到的回复可能被下一次同 cmd 的请求匹配（Sloop Go 同样）；设备静默停推时 `watch=3` 仍显示 Tab5 上次设的值。
- **只读证、未实测的分支**：`need_upgrade=1`（真机是 2.8.5）与协商失败的指数退避；IN 环错误分支在 19:05 冒烟的拔线里走到了：`Bulk IN 出错 status=1（连续第 1/3 次）` → 重提交失败 `ESP_ERR_INVALID_STATE` → 设备已清理 → `link=none`；19:07 监听的拔线段只留下其后的 `重提交失败`。这是「设备已不在」时的直接清理，不是「连续 3 次错误」触发的断链——后者没有在硬件上演练过。
- **`fm1_cli` / `usb_lib` 两个任务的栈水位没有实测**（栈各 4096、在 PSRAM）。

**证据缺口**

- **首轮两份 FM-1 已接转录丢失（已补证）**：12:59 的冒烟与 13:04 的 U 盘监听都写在 `firmware/build/verify/`，被全矩阵第一步的
  `build.sh --clean` 清掉；读数只有主控的清单，而且那次冒烟跑在 `-dirty` 的同源构建上。19:05 起在最终固件 `715fdae` 上重跑
  并入库（§2），首轮读数只作历史。
- **FM-1 已接检查没有开机日志**：`att-*` 的固件身份靠同一次开机的冒烟转录头（`App version: 715fdae`）与 `att-status.txt`
  （没有烧录步骤），不是设备自报。
- **`ENG SELFTEST` 期间 `psram_min` 掉到约 29.3MB**：未接 `psram_min=29350520`、已接 `29344924`（`psram_free` 约 30.07MB）；
  P4-F 10c 的堆转录同款（`29373824`）——自检期的既有瞬态，非回归；自检之后 `psram_free` 回到开机值。
- A/B 四次 600s 的 `VERDICT` 都是 FAIL（累计 `duty_mean` 超限），只作对照；B-dev 轮没有开机日志，受测固件的依据是烧录日志、镜像大小与内部堆签名。
- 截屏矩阵与十条场景的截图都没有做过视觉检查；`system-selftest.png` 与 `system-default.png` 逐字节相同。
- `sequence` / `synth` / `sys` 的复跑本轮未做（5A 没有改注入脚本与页面 TAP 路径）。

**已登记的 deferred issues**（`docs/deferred-issues.md`）

- **DI-63**（本次新增）：soak 渲染占空比余量偏紧——起点工程内容未归一 + 5A 渲染均值 +3.4% 待定位。
- **DI-64**（本次新增）：`ENG SELFTEST` 期间自检所在核的 IDLE 的 task-watchdog 报告（未接那次是 `IDLE0 (CPU 0)`，已接那次是 `IDLE1 (CPU 1)`；既有现象）。
- **DI-58** 开机偶发 BSP `pi4ioe5v6408` "Chip reset failed" abort（与本任务无关，自动重启后正常）。
- **DI-59** `EDP_ARGS_MAX = 384` 小于上游 `DSYN_GET` 回复（5E 的入口条件）。
- **DI-60** `usb_fm1` 的两处异常路径（OUT transfer 永不完成 / `NEW_DEV` 时旧句柄尚在）。
- **DI-61** `fm1_session` 的 `request()` 在「永远等待」超时下无法被链路断开唤醒（5C 的入口条件）。
- **DI-62** `scripts/selftest-heap.py` 不捕获 `KeyboardInterrupt`。
- **DI-65**（补证时新增）：`scripts/fm1-smoke.py` 的结论行对「无回复」类 error 的步骤归因不准。

## 7. 需要用户知情的事与开发机状态

- **FM-1 的当前引擎 / 预置被冒烟期间的拧动改过**：冒烟第 5–6 步要人在 FM-1 上拧旋钮（19:05 那次期间设备推来 `reload=4 changed=2`）。第 4 步 `SET` 过的 ATK 已还原并读回确认（0 → 1 → 0，`fm1-smoke-20261126-190513.log`）。
- **开发机的工程槽 0 已被今天的 A/B 覆盖**（准备步骤 `STORE PROJ SAVE 0`）；autosave 的工作工程是复位后那次 soak 之后的状态。注入场景按设计是破坏性、自归一的（会擦写工程槽），截屏矩阵会把工作工程还原到起点；要找回之前的工程只能靠工程槽 A–D 里的存档。
- 设备固件：A/B 结束时已刷回 5A `715fdae`（`ab-flash-5a.log`：hash 3×）。
- USB-A 口留空时开机即 §2 的「无链路检查」状态：`FM1 STATUS link=none`，独立模式照常。

## 8. 下一步：5B 的入口条件

1. **合并 `dev` 的证据前提已齐**：最终固件上的 `fm1-smoke.py` 转录、U 盘插入、FM-1 已接的 182 + selftest-heap 都已入库（§2）。合并 `dev` 已执行（§9）；dev → main 不在本阶段。
2. **回归口径**：自 5A 起设备回归分「FM-1 未接」（独立模式全矩阵）与「FM-1 已接」（`fm1-smoke.py` + 182 + selftest-heap）两种条件，已写入 `docs/dev/reference/verification-gates.md`；soak 须硬复位 + 30s 后起跑。
3. **5B 只读 5A 已有的能力**（spec §6 5B）：Device 页把 `FM1 STATUS` 的 `link` / `version` / `need_upgrade` 与 `FM1 INFO` 字段搬到屏幕上，不新增协议能力。5A 与 5B 彼此独立。
4. **带入 5B 的约束**：渲染占空比余量只有 0.57 个百分点（DI-63）——新页面的 est / 池预算照既有框架逐页实测，5A 的 +3.4% 渲染开销在 5B/5C 定位；输入任务栈已在 PSRAM（`fd0ffaf`），`KEY STATS` 的 `input_stack_free` 断言仍只在 `phys>0` 时有意义。
5. **留给后续子阶段的 DI**：DI-61（5C 开工必须处理）、DI-59（5E 入口条件）、DI-60 / DI-62（各自触发条件）。

## 9. 集成状态

- `feat/phase5-sloop-go` 已合回 `dev`：合并提交 **`aba1499`**（父 `f4b2026` + `00379c0`，保留了合并提交）。
- 合并后门禁通过：在 `aba1499` 上 `build.sh` ×2（第一遍只有 §5.5 豁免的一对 CMake 提示，第二遍没有任何提示，固件编译 0 警告）+ 离线套件十项全部 PASS（`5-pa-2026-11-26/postmerge-gate.log`）；合并前在 `00379c0` 上跑过同一套离线套件，同样全过（`finish-selftest.log`）。两份日志都不含 MANIFEST 校验的输出——入库时在 `aba1499` 上只读复核 `third_party` 的 MANIFEST 为 320/320 OK。
- **设备矩阵没有在合并后重跑**：`git diff --stat 715fdae dev -- firmware scripts tools` 为空，合并后的树在固件、脚本与工具上与跑过全矩阵的 `715fdae` 逐字节相同，§2 的设备读数沿用。
- `main` 未合并（用户 2026-11-24 的决定：dev → main 顺延，时机由用户决定）。
- 特性分支 `feat/phase5-sloop-go` 保留，本地与 origin 都指向 `00379c0`。
