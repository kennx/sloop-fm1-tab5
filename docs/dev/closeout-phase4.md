# 阶段 4（独立模式功能对齐）收口报告

> 分支 `feat/phase4f`，受测固件 **`d317a72`**（boot `App version: d317a72`，无 `-dirty`）。`d317a72` 之后的提交**只改 `scripts/` 与文档**：
> `git diff d317a72..HEAD --stat -- firmware third_party` 为空（2026-11-26 复核），HEAD 上的固件**源码**与受测固件逐字节同一（二进制内嵌构建时间，不逐字节比较）。
> **状态：机器侧完成（2026-11-26）**——§2 的门禁全部通过，含最终固件上的 3h soak PASS 与 10c3 最终回归。
> 这**不是**「阶段 4 完成」的声明：人测项一个都没做（§5 是待办清单），dev → main 不在本阶段（§6）。
> 机器侧门禁全部通过；**没有任何一项经过人眼/人耳/人手验证**（§5 是待办清单）。
> **dev → main 不在本阶段**（§6）。没有 push、没有合并、没有切分支。
> 证据：`docs/dev/verify/4-pf-2026-11-24/`（`readme.md` §7、`SHA256SUMS`）与 `docs/dev/verification-log.md`。

## 1. 交付范围

| 子阶段 | 内容（一句话） | 证据包 |
|---|---|---|
| P4-0 | 全量 `ENG SELFTEST` 182/182 在 UI live 下通过（诊断期 quiesce）；懒建/每页预算框架；步窗抽象；注入框架 | `4-p0-2026-11-21/` |
| P4-A | 描述符驱动的参数页框架（引擎页 + GLOBAL + FX；TAP/键盘/触摸相对拖拽） | `4-p0-2026-11-21/` |
| P4-B | 存储全链路（工程 A–D、用户预置 32、FM6 银行 27、autosave、torn-write 套件、旧格式兼容）+ 状态层存储命令 + 分组导航 + FILE 组 | `4-pb-2026-11-21/` |
| P4-C | 64 步分窗编辑、micro/fill、每轨 24 参数锁、pattern 工具与 undo、实时录音、16 lane 鼓、live sections / quick chain / SONG / SONG REC、PER3 持久化 | `4-pc-2026-11-22/` |
| P4-D | USR1–4 采样槽、DSYN 4 kit × 16 lane、FM6 155B patch、SYNTH 组 | `4-pd-2026-11-23/` |
| P4-E | SYS 组 / SYSTEM 页、settings 生效副本与 NVS、运行时 palette/theme、键盘 RGB、SELFTEST chip | `4-pe-2026-11-23/` |
| P4-F | DI-26 堆泄漏修复、功能对齐清单、chip 框架 + MOD/SCALE/VOICE、SOUND 通用引擎页、GLOBAL/FX chip 页、TOOLS、REC 配置行、键盘参数锁/fill、18 页九组导航、全页面截屏矩阵、`ENG POWER` | `4-pf-2026-11-24/` |

阶段 2/3（引擎移植、MVP 四屏）的证据在 `2-engine-2026-11-20/`、`3-mvp-2026-11-21/`。

## 2. 门禁结果（最终固件 `d317a72`）

完整表（命令 → 结果 → 证据文件）在 `4-pf-2026-11-24/task-10c-matrix-summary.txt`（10c-1/10c-2）与 `task-10c3-summary.txt`
（10c-3 = 本节最后两行）。`d317a72` 之后的提交只改 `scripts/inject/inject.py`（R20/R23 两处归一化）与文档。

| 门禁 | 结果 |
|---|---|
| 构建 `build.sh --clean` → `build.sh` ×2 | exit 0；第二、三遍 0 warning |
| host：host-test / seq / state / knob / synth、四个 `--selftest`、MANIFEST | 全部 exit 0（state 46/46、knob 17/17、MANIFEST 233/233） |
| 烧录 ×3 + 启动日志 | 每次 3× `Hash of data verified`；0 fatal/abort/backtrace |
| `device-selftest.py` | 182/182 |
| `selftest-heap.py` | PASS：run 1 −268B、run 2 −36B、收尾 69,887B |
| `desc-check.py` | 10/10 + live 2/2 + 越界 7/7 |
| 十条注入场景首轮 | **5,007 / 0**（分两批读数：params 768 来自另一轮；单一会话口径见下行 10c3 的 **5,017 / 0**。params 768、step_edit 312、tracks 84、nav 583、transport 288、store 908、song 354、sequence 562、synth 794、sys 354） |
| 复跑 | sequence 562/0、synth 794/0、nav 583/0、**sys 354/0**（10c-2）；**step_edit**：默认入口（MONO）rc=0，**321 / 0**（LIVEHASH FCE9B26C 前后不变）；轨 0 静音的入口 rc=0，**321 / 0**——同一入口态在修复前是 `voices 峰值 0` 的红；场景之后轨 0 仍是 `mute=1 level=1`（入口值被还原）（R20 修复的判别用例） |
| `screenshot_matrix.py` | **481 / 0**：33 条目、166 锚点、逐条目后 `LIVEHASH` 33/33 不变；播放头存在性锚点首次在设备上命中 |
| 终审修复波的欠账验证 a–f | a（A1 用户预置页眉）、b（B-I1 槽标签重解析）、c（播放头锚点）、d（重启路径 pos/neg）、e（歌曲还原）、f（隐藏 chip / CHIP quiesced / TOOLS 过期）——全部通过，边界见 `readme.md` §7 |
| **最终 3h soak**（固件 `d317a72`） | **PASS**：`soak.py /dev/ttyACM0 --duration 10800 --interval 60 --save-window 3600`，2026-11-25 23:24:03 → 2026-11-26 02:24；实测 10801s / 181 采样 / 断言失败 0 / 判定问题无。内部堆 69,747→69,731B（Δ−16B）、`internal_min` 66,208B（门限 61,440B）；streaming=1、xrun 0；`duty_mean` ≤12.78%；音频栈 3,120B、输入栈 7,852B；PLAY 稳态池往返 45,868→45,856B（Δ−12B，单次最小 −48B）；save 窗口 xrun 0→0、窗口后 streaming=1。功耗/温度仅记录（裁决 R15）：`ina=ok` 181/181、芯片温度 27.1..29.1 ℃。修复前固件 `1e8587b` 的同口径信号 soak 也 PASS（只作信号，裁决 R17）；R11/R14 的降栈备用方案没有用上。证据 `task-10c-soak-final-{summary,transcript}.*` |
| **10c3 最终回归**（2026-11-26；脚本 = HEAD `03798f1`，固件源码与 `d317a72` 逐字节同一） | **R23 的设备验证**：CYAN 入口（硬复位后 `STORE SETTINGS SET palette 2` + `SAVE`）跑 `step_edit` ⇒ rc=0、**326 / 0**（R23 前同一入口 306/14；多出的 5 条是括号自己的断言），场景之后 `palette=2` 被还原；CYAN 入口的 `store`（开头就硬复位）⇒ **911 / 0**、同样还原。**十场景同一会话**（transport → step_edit → tracks → nav → params → store → song → sequence → synth → sys）⇒ **5,017 PASS / 0 FAIL**（transport 288 / step_edit 323 / tracks 84 / nav 583 / params 768 / store 907 / song 354 / sequence 562 / synth 794 / sys 354；与 10c-1 的 5,007 相比差 +10，逐行 diff 后全是入口相关的条件断言或已登记的基线变化，没有丢检查）；`screenshot_matrix.py` rc=0：33 条目 / 166 锚点 / 481 PASS 行 / 0 FAIL 行；`device-selftest.py` 182/182；nav 有序对 72/72、最紧 SEQ→FILE 6,564B。证据 `task-10c3-*`（`readme.md` §8） |

## 3. 功能对齐清单状态

`docs/design/012-feature-parity-checklist.md`：64 行，✅ 42 / ◐ 5 / ○ 9 / 非目标 8，无未处置行。

◐ 5：EDIT 层擦除（聚焦步、非走带中逐拍 → DI-45）；SEL 层参数已有、键盘行为无（DI-49）；输出路由 speaker-only（DI-39，硬件）；SYSTEM 页的 MIDI/SYNC/ROUTE（阶段 5）；DRUM PREVIEW（显式试听等效，文档化）。

○ 9 与 P4-F 期间的其它开放项（`docs/deferred-issues.md`）：

- **DI-44** PRESETS 收藏（16 个）无 UI。
- **DI-45** EDIT 层走带中的 MPC 式实时擦除未移植（现在是擦聚焦步）。
- **DI-46** 鼓轨 MUTE mode（静音某个鼓声）未移植。
- **DI-47** ARP roll（note repeat）未移植。
- **DI-48** tap tempo 未移植。
- **DI-49** SEL 层键盘行为（CHORD / KEYS SNAP·WHITE）未映射。
- **DI-50** 开机画面 / 开机音未移植。
- **DI-51** VIS 可视化（12 风格）未移植。
- **DI-52** Punch-in effects 层未移植。
- **DI-53** 八度移位（OCT−/+）未移植。
- **DI-54** 工厂预置无按名浏览列表。（已于 5B-1 Task 6 关闭：SOUND 页眉的选择器）
- **DI-55** 诊断期静默每次转换泄漏约 40B 内部堆（只在自检/注入路径，全量自检两次 −268/−36B）。
- **DI-56** 池的有序对纸面不变量在重测 base（≈14.0KB）下对最坏对略超上限；实测 72/72 对 ≥4,096B（最终固件最紧 6,620B）。
- **DI-57** SOUND 页眉对用户预置只显示 `USER nn`，不显示槽名。

更早的开放项（DI-1…43）原样保留在清单里，其中与本阶段用户可见面相关的：DI-4（键盘 70 键人测）、DI-7/39（听音 / 输出路由）、DI-13、DI-16（旋钮手感）、DI-20/21/22/27…38（诊断与存储边界）、DI-43。

## 4. 已知限制与开放项

**合并前无需处理（已定性、有裁决或只在诊断路径）**

- INA226 的测量点在电池侧（`SYS_BAT_PRE`）：USB-C 供电且未装电池时读 ≈0.9V / 0mA，**USB 输入功耗不可观测**；soak 的功耗/温度列只记录（裁决 R15）。装电池后读数才有意义。
- DI-56 的账面贴限：门禁是实测的 `池剩余 ≥ 4,096`，不是纸面账。
- DI-55 的 ≈40B/次只在 `ENG SELFTEST` / `UI QUIESCE` 路径上发生；用户操作不经过它。
- ADR-008 §8.12 记录的已知边界：同批次 console `TAP` 落在 30ms 同步窗口内时，可以对**刚被换引擎隐藏**的 chip 返回 `select`（只选 chip、不写参数，一拍后 `TAP miss`）；物理触摸走 LVGL indev，碰不到这个窗口（裁决 R21）。
- 截屏矩阵不是「全程不写 flash」：非 MONO 设备上会写一次 settings，SELFTEST 条目的 worker 落盘一次（`readme.md` §6）。
- B-I1（SOUND 槽标签随引擎槽值重解析）只做过 GREEN 验证，没有在修复前固件上做 RED；A1 的 `USER 01` 字面没有 OCR，机读证据是 `UI SOUND CHIP`/`TAP` 回显与页眉区像素差（裁决 R22）。
- `params` ⑪ 与 `transport` ⑩g 的「截屏复位重跑」路径没有在设备上演练（`step_edit` ②g 的 pos/neg 两条已演练）。
- 探针与重启包装器（`task-10c-probes.py`、`task-10c-restart_wrap.py`）是一次性的，没有固化进 `scripts/inject/`。
- `step_edit` 的像素常量假定 MONO 主题，场景曾不像 `sys` 那样自归一 palette：设备停在别的 palette 时会有十几条像素 FAIL（10c-2 实测：306 PASS / 14 FAIL，`task-10c-step_edit-rerun-cyan-stdout.log`）。**已按裁决 R23 修（`93f8f96`，只改脚本）并做了设备验证（10c3，`readme.md` §8）**：十条场景共用入口括号——非 MONO/主题开时归一 palette 4 + 主题关并落盘，收尾（含异常路径）还原入口调色板/主题开关；默认 MONO 入口零写命令。CYAN 入口的 `step_edit` ⇒ rc=0、326/0（= MONO 入口的 321 + 括号自己的 5 条），场景之后 `palette=2` 被还原；CYAN 入口的 `store`（开头就硬复位）同样通过并还原。主题开着（`theme_on=1`）的入口只有离线演练，没有上设备。
- 载入工程/预置时 `P_LEVEL`/`P_MUTE`/ARP/HOLD 是保留参数（`param_kept`，上游语义）——换声音不会把静音轨放开。

**后续跟进（需要动代码或等外部条件）**

- 两条任务栈的余量**没有被机器验证**：输入任务（8,192B）只在 `phys>0` 时断言，全部注入运行都是 `phys=0`；LVGL 任务（10,240B）的 `lvgl_stack_free` ≥5,036B 覆盖的是定时器拍与 `TAP` 路径，物理点击链路（chip/页眉/±/bank/SONG 配置）只由静态调用图和一次临时诊断固件覆盖。
- SONG 页配置 chip 的回调路径在 LVGL 任务栈上叠**三份** `app_state_t`（`song_action → song_action_cfg → song_apply_cfg`），靠余量兜着；再加一层嵌套前应压平。
- `selftest-heap.py` 不断言 `cases=182`（由 `device-selftest.py` 覆盖）；matrix 的 `--inject-drift` 可被 argparse 缩写触发（已加 `allow_abbrev=False`，未再在设备上复验）。
- `power_sensor.c` 的头注释仍写与键盘 RGB 共总线（错；固件文件，留给下一次构建）。
- 文档措辞类 minor：ADR-005 §5.5 SYNTH 作目标列的 est 低估 1,024B（不影响最坏五对）；roadmap 里「阶段 4/5 强制合并点」的旧措辞与「不合并 main」并存。
- 上面 §3 的 ○/◐ 各项（功能缺口）按各自 DI 的触发条件处理。

## 5. 人测待办清单

机器侧没有覆盖、需要人在设备上做的：

1. **联系表目检**：`4-pf-2026-11-24/task-10c-matrix/sheet-*.png`（10 张；18 页 × 33 个状态）。全部截图只有像素探针/锚点结论，**没有人看过**；重点看是否有花屏、错位、残留脏区、文字截断。
2. **DI-4**：Tab5 Keyboard 70 键全矩阵（含修饰层 Alt/Ctrl/Aa 与 16 lane 鼓键位）。
3. **DI-16**：参数页旋钮的触摸手感（按下聚焦、环带拖动粗调、中心细调）。
4. **DI-7 / DI-39**：合成轨 / 鼓 / SAMPLE 预置的听音；扬声器与耳机共用 LOUT1 的实际表现。
5. 键盘 RGB：`KBD RGB off|low|mid|high` 四档与 `lights` 联动（颜色 = 当前主题 accent）。
6. 亮度（10..100）与音量（0..100）的实际范围；`AUD OUT headphone` 应回 `not_supported`。
7. **物理点击一轮后读栈余量**：点过 chip 行、页眉 ±、bank 行、TOOLS、SONG 配置之后 `UI MEM` 的 `lvgl_stack_free`（门限 2,048）；按过 Aa+k / Aa+l / Aa+9 / 擦除 / 演奏键之后 `KEY STATS` 的 `input_stack_free`（门限 2,048，`phys>0` 才有意义）。
8. Aa+k / Aa+l / Aa+;（fill 条件）的实键手感与 `LOCKED`/`NO PARAM` 提示。
9. TOOLS 的两段式确认（1.5s 窗口）与 SYSTEM 页 SELFTEST chip 的全路径（约 165–180s，期间屏幕显示 RUNNING）。
10. 装电池后看一眼 `ENG POWER`（唯一能让功耗列有信息的姿态）。

## 6. dev → main：不在本阶段

用户 2026-11-24 明示本阶段不合并 main。当前状态：`feat/phase4f` 经整分支终审与收口回归后合回 `dev`（**不合并 main**）；何时合 `main` 由用户另行决定。合 `main` 前建议至少完成 §5 的第 1、7 两项，（最终 soak 已 PASS，§2）。

## 7. 开发机状态提醒

- **autosave 的工作工程是场景测试态**：最终固件的矩阵与复跑之后，设备 RAM/autosave 副本里的工程由注入场景留下（`sys` 复跑的归一 SAVE 之后是 `5270B666` 一类，不是用户的工程）。`sequence`/`store` 等场景按设计会擦写工程槽——**开发机上的工程槽 A–D 与用户预置槽自 P4-C 起就是场景测试态**，不要把它当成存着音乐作品的设备。
- 更早的会话曾把工作工程留在 `16FC5CB6`（轨 0 `len=64 / mute=1 / level=1`）；10c-1 的矩阵批从 TOOLS NEWPRJ 的默认工程起跑后，autosave 副本已换成默认工程，这个残留**不在了**。
- 设备主题被 10c-1 一次误起又中止的 `sys` 复跑留在 CYAN；10c-2 的 `sys` 复跑按设计把它归一回 MONO 并落盘。10c-3 的 CYAN 判别运行（`step_edit`、`store` 各一次）之后已手动回 palette 4（MONO）并落盘（`task-10c3-mono-restore{,_2}.log`）。若用户原本用的是别的 palette，需要自己改回。
- `STORE AUTO` 在 soak 起跑前是 on（`autosave=1`、`autosave_hash=FCE9B26C`、`playing=0`、`rec=off`）；soak 结束与 10c3 收尾之后也是 on（`task-10c3-final-state.log`：`autosave=1`；硬复位 `App version: d317a72`、fatal 0）。
- 最终 3h soak 已在 3,600s 处停走带做了一次 `STORE PROJ SAVE 0 + LOAD 0`（写工程槽 0），并把 `STORE AUTO` 关掉、跑完再打开——工程槽 0 里是 soak 入口的工作工程快照（场景测试态，不是用户作品）。10c3 的 `store` 场景又按设计擦写过工程槽。

## 8. 我替用户做的裁决一览

按时间序；每条带「若错代价」。R11/R14 是备用方案，没有触发。

| # | 裁决 | 若错的代价 |
|---|---|---|
| R1 | 导航重组（9 组 / 104px / MIX 组）提前到 Task 4，而不是等 Task 7 | 任务间门禁红 |
| R2 | 清单复查把 6 行误标 ✅ 改回缺口并重新处置（工厂预置/换引擎 → SOUND 页眉；鼓轨 kit → KIT 行；REC 配置 → TRANSPORT chip；参数锁/fill → 键盘 EDIT 层；八度 → DI-53；预置列表当时登记为 DI-54，后由 5B-1 Task 6 的 SOUND 选择器关闭） | 范围估计偏差 |
| R3 | mute/solo 行保持 ✅（设备证据由 Task 7 的 KEY 注入补） | 仅证据时点 |
| R4 | TOOLS 的走带守卫按上游（CLRSEQ/INITSOUND 走带中允许；NEW 仅 SONG 播放时拒绝） | 走带中清轨/初始化声音的预期行为不同 |
| R5 | `project_new` 逐行镜像并引用上游行号 | NEW 的副作用清单漏项 |
| R6 | 描述符不做标签镜像，直接转调上游 `track_desc()` / 引擎 desc 钩子 | 无（比镜像更忠实） |
| R7 | MOD/SCALE/VOICE est 取 3,584（spec 的 5,120 是上限） | 无（更省） |
| R8 | 接受三处范围外行为变化：ENTER 在所有参数页 = +1；FX 页在鼓轨上逐槽锁定；`UI <page> CHIP` 对旧页打印槽信息 | 鼓轨上以前能改失真/合唱的用法消失（上游本就锁定） |
| R9 | `nav` ② 的 dstep 断言换形式（聚合行；原始位图另有精确断言） | 漏掉「第 3 行必须变化」（位图断言已覆盖） |
| R10 | LVGL 任务栈 7,168 → 10,240B；点击/输入路径不整份拷贝 `app_state_t`；`UI MEM` 增 `lvgl_stack_free` | 多占 3KB 内部堆 |
| R11（备用） | soak 余量不足则 LVGL 栈降回 8,192 | 收口 soak 红一次 |
| R12 | 锁的作用范围按上游 = 聚焦步在轨 LEN 之内（含 TIE-only/空步） | 无（更贴近上游） |
| R13 | 输入任务栈 4,096 → 8,192B + 去掉 `sloop_ui_step_tool` 的第二份状态拷贝 | 多占 4KB 内部堆 |
| R14（备用） | soak min <63KB 则执行 R11 并把输入栈降到 6,144 | 收口 soak 红一次后调 |
| R15 | INA226 测量点在电池侧，USB 供电下功耗读数无信息，只记录 | 功耗数据无意义但不影响门禁 |
| R16 | 播放态的像素锚点移出网格（随后评审补了播放头存在性锚点） | 漏掉「网格在播放态渲染异常」的像素证据（其它条目仍覆盖网格） |
| R17 | 修复前固件的 3h soak 只作信号；最终固件上必须重跑 | 多花 3 小时设备时间 |
| R18 | 接受对两条栈读数 minor 的反驳（内核返回值单位、`phys=` 计数位置）；其中 HWM 单位那半随后在 `58bd40d` 更正（本端口 `StackType_t`=`uint8_t`，内核除以 `sizeof(StackType_t)` 后返回的即字节） | 栈门禁读数差 4 倍（多处的 ≈5KB 实测已排除） |
| R19 | SOUND 页眉 PRESET ± 是 Tab5 的**按引擎**步进（上游是单一列表跨引擎环绕） | 与上游浏览心智不同，但功能可达 |
| R20 | `step_edit` ②g 的发声探针在入口归一轨 0 的 `P_MUTE=0`/`P_LEVEL=104` 并在收尾还原（脚本缺口，不是固件缺陷） | 无 |
| R21 | 接受「同批 console TAP 可对刚隐藏的 chip 返回 select」为投影模型的固有延迟（≤1 拍） | 极小概率的误触（人手 30ms 内完成换引擎 + 点 chip） |
| R22 | A1 的 `USER 01` 以 `UI SOUND CHIP`/`TAP` 回显为机读证据，页眉字面留给目检 | 无 |
| R23 | 带像素探针的场景在入口归一 MONO（palette 4 + 主题关）并在收尾还原入口调色板/主题开关（脚本缺口，不是固件缺陷；`93f8f96`） | 无 |
