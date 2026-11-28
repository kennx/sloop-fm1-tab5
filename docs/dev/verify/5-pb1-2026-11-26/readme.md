# 5B-1 Task 2 证据包：Sound 仪表盘骨架与真机原型测量（2026-11-26）

> 阶段 5 / 5B-1「Sound 单轨仪表盘」Task 2（5B-0）：SOUND 参数页换成滚动容器 + 13 张自绘卡片的骨架，挂到参数页框架
> （`PARAM_FEAT_DASHBOARD`），**只判命中与滚动、不写值**；在真机上测池增量、构建耗时、SEQ→SOUND 过渡峰值与内部堆。
> 分支 `feat/phase5b`。设备 Tab5 `/dev/ttyACM0`，ESP-IDF 5.5.5，target `esp32p4`；门禁口径 AGENTS §5.5。
> **固件 = `3628871`**（`task-2-proto-boot.log`：`App version:      3628871`，无 `-dirty`）；评审前修正后 = **`562c63f`**
> （SOUND est 按实测校准为 8,704；`task-2-fix-boot.log`，见 §5）。
> **状态：骨架的读数门禁通过；预算裁决 = 不需要任何退路**（池 delta 6,808B；est 于评审前按实测校准为 **8,704**，§5）。
> 主控已裁决（§5）：① `knob-test.sh` 曾为红的 EDIT→STEP 账面断言——不放宽、不提前退役页面，把 SOUND est 按实测从 12,288 校准到
> 8,704，现 17/17；② 往返判据 = 预热一轮后 5 轮 `pool_free` 首末差 ≤128B，冷启动首轮是框架的一次性缓存。
> **仍未闭合**：PNG 没有人眼读过（本会话无图像查看能力），只有像素核对；按旧 SOUND 页写的 `inject.py` / `soak.py` /
> `screenshot_matrix.py` 断言在 Task 3/7/8 改写前失配、本包未跑注入场景。

## 1. 门禁矩阵（门禁 → 命令 → 证据 → 结论）

除第 10、12 行外都在**最终固件 `3628871`** 上；自检堆与往返对照跑在提交前的同源构建上（第 11 行两组都有）——
`git diff 3628871 -- firmware scripts third_party tools` 的「提交前」工作树与本提交只差 `page_layout.c` /
`page_params.c` 的文件头注释（编译进调试信息、不进代码），读数在最终固件上复现（第 6、8 行）。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-2-proto-build-1.log`、`task-2-proto-build-2.log` | ✅ 两遍 rc=0、编译 0 警告；第一遍只有 §5.5 豁免的那一对 CMake 提示（`build.cmake:631` + `Missing kconfig option. Re-run the build process...`，第 41 / 56 行），第二遍没有任何提示 |
| 2 | 固件代码（首构建，保留作对照） | CMake 变更后的第一遍 | `task-2-proto-build-first.log` | ⚠️ rc=0，但有 **3 条弃用警告**（`page_sound.c` 的 `lv_obj_remove_flag` / `lv_obj_add_flag`，第 160 / 170 / 176 行）——烧录前已改为专用 setter（`lv_obj_set_scrollable` / `_scroll_chain` / `_gesture_bubble` / `_clickable`），第 1 行的构建里没有 |
| 3 | host：Sound 布局纯函数 | `scripts/sound-test.sh` | `task-2-proto-sound-test.log` | ✅ `4 passed, 0 failed`、`SOUND LAYOUT TEST PASSED` |
| 4 | host：参数页布局表 | `scripts/knob-test.sh` | `task-2-proto-knob-test.log` | ❌ **16 passed, 1 failed**：`case_layout_pages:576  UI_BASE_BYTES + edit_est + UI_STEP_EST + 4096 <= UI_POOL_AVAIL`（EDIT→STEP 账面 64,176 > 63,120）。其余已按本任务改钉（SOUND est ≤ 12,768、`EDIT Σ est = 23,040`、features 位掩码含 DASHBOARD、仪表盘几何 9 条）——评审前修正后 17/17，见 §5（第 18 行） |
| 5 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-2-proto-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 6 | 启动日志 | esptool 硬复位，抓 10s 串口（测量收尾后那次开机；同一固件） | `task-2-proto-boot.log` | ✅ `App version:      3628871`、`ELF file SHA256:  bb7efb1ca…`；`UI 就绪: 18 页注册/1 已建 … 池已用 14964/62856 字节`；fatal/abort()/backtrace/Guru Meditation/panic 计数 0 |
| 7 | 页面构建：池 delta / build_ms | `task-2-proto-measure.py /dev/ttyACM0 base …`（`STORE AUTO OFF` → `ENG MEM` → `UI sound` → `UI MEM` → `ENG MEM`） | `task-2-proto-final-base.log` | ✅ `页面构建 page=SOUND(6) 池增量=6808B 预算=12288B 池剩余=41484B 耗时=11ms`；`UI MEM page id=6 title=SOUND state=built est=12288 delta=6808 build_ms=11`；`UI MEM pool_used=18320 pool_free=44008 pool_total=62328 … built=1 failed=0`；`切页 page=SOUND(6) 组=EDIT 跨组耗时=14ms`；`lvgl_stack_free=6024` |
| 8 | SEQ→SOUND 过渡峰值 | `… seq …`（`UI step` → `UI transport` → `UI sound`） | `task-2-proto-final-seq.log` | ✅ `页面构建 page=SOUND(6) 池增量=6668B 预算=12288B 池剩余=13456B 耗时=11ms`（≥4,096）；切完 `pool_used=18960 pool_free=43096`；`切页 … 跨组耗时=18ms` |
| 9 | 内部堆 | 同第 7 行的两个 `ENG MEM` | `task-2-proto-final-base.log` | ✅ `internal_free` 73019 → 72963（差 **56B** ≤256）；`internal_min=71972`（≥61,440）。页面静态状态在 PSRAM：`nm` 里 `s_pos`/`s_cards`/`s_scroll*`/`s_gesture_*`/`s_label*` 全在 `0x4800xxxx`（`.ext_ram.bss`） |
| 10 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0`（提交前构建，22:29:51 → 22:35:17） | `task-2-proto-selftest-heap.log`（stdout）、`task-2-proto-selftest-heap-transcript.log`（串口回显） | ✅ rc=0（326s）：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72675B`；两次 `cases=182 identical=182 … result=PASS`；`# [selftest-heap] PASS` |
| 11 | 往返：`UI step` ↔ `UI sound` ×5 | `… roundtrip …`（硬复位后连跑） | 最终固件：`task-2-proto-final-roundtrip-{cold,warm,warm2}.log`；提交前：`task-2-proto-precommit-roundtrip-{cold,warm,warm2..warm6}.log` | ⚠️ 冷启动首轮超限、**预热后达标 = 判据口径**（§4 ②，主控裁决）。最终固件冷启动首 5 轮 `pool_free 43076 -> 42780（差 -296B）`、delta 6688 → 6392；第 2 组 −52B、第 3 组 −76B。提交前那一次开机的 7 组：−384 / −144 / −36 / −20 / +4 / −8 / +36B |
| 12 | 往返对照（其它页，同一 `UI step` ↔ 页 路径） | `… drift <页> …` | `task-2-proto-precommit-drift-{mod,global,sample}.log`（热机后各 5 轮）、`task-2-proto-precommit-drift2-sample-1..6.log`（硬复位后 SAMPLE 连跑 6 组） | MOD −60B、GLOBAL +28B、SAMPLE −84B；**硬复位后的 SAMPLE**：−200 / −36 / −48 / −8 / −32 / +32B——与 SOUND 同形态（首组大、之后收到噪声） |
| 13 | EDIT 四页全建 → 首建 STEP（为 §4 ① 取实测） | `… edit2step …` | `task-2-proto-final-edit2step.log` | `UI MEM pool_used=27128 pool_free=34008 … built=4`（`GROUP EDIT built=4 sum_est=23040`）→ `页面构建 page=STEP(2) 池增量=21796B 预算=23040B 池剩余=12212B 耗时=31ms`（≥4,096） |
| 14 | 银行行位置（裁决 R8） | `… fm6 …` → 截屏 → `… fm6-restore …` | `task-2-proto-final-fm6.log`、`task-2-proto-final-fm6-restore.log`、`task-2-proto-sound-fm6-20261126-225150.png`、`task-2-proto-pngcheck-fm6.txt` | ✅ 经页眉 ENGINE 行 `+` ×9 到 `value=9 name="FM6"`：`TAP 122,170 page=6 -> BANK slot=26 delta=-1`（行在页面区 y=76）；旧位置 `TAP 378,674 page=6 -> SOUND focus card=2 slot=6 flat=15`（落到卡片，视口顶已是 128）；`TAP 100,248 … -> SOUND focus card=0 slot=0 flat=0`。显隐翻转前后 `pool_used=19068` 不变。还原：`value=0 name="ANALOG"`，四轨 `0/0,0/0,0/0,0/0` |
| 15 | TAP 命中 / 不适用入口 | `base` 相的 6 条 `TAP` + `UI KNOB` + `UI sound CHIP` | `task-2-proto-final-base.log` | ✅ `TAP 100,200 … -> SOUND focus card=0 slot=0 flat=0`；`TAP 22,160 … -> SOUND card=0`；`TAP miss 636,272`（列间空隙）；`TAP miss 900,122`（视口之上）；`TAP 1106,122 page=6 -> PARAM slot=0 locked (page or slot N/A)`（`−` 在页面区 y=40，不发命令）；`TAP miss 1272,52`（旧按钮位置）；`UI KNOB 0 press 0 -40` ⇒ `UI ERR knob_slot 0`；`UI sound CHIP` ⇒ `UI sound chip=255`（无逐槽行） |
| 16 | UI 截屏（默认） | `python3 scripts/screenshot.py /dev/ttyACM0 <out>.png` | `task-2-proto-sound-20261126-225131.png` | ✅ 截屏成功（1280×720 rgb565）。像素核对 `task-2-proto-pngcheck-default.txt` = `PNG CHECK PASS`：可见 4 张卡（Global / Master / Envelope / LFO）26 个槽各有环 / 名称 / 值占位，卡片体外无背景色、两侧与视口底边以下是背景，页眉两行与 `−/+` 面板在位。**人眼读图未做**（§4 ③） |
| 17 | UI 截屏（FM6，银行行） | 同上 | `task-2-proto-sound-fm6-20261126-225150.png`、`task-2-proto-pngcheck-fm6.txt` | ✅ `PNG CHECK PASS`（`--bank`：视口顶 128，21 个槽露出；银行行 y=76..120 有 BANK 标签墨迹、`−/+` 面板、SAVE/ERASE 彩色底，行的上下都是背景） |

## 2. 读数汇总

- **池 delta**：最终固件首建 **6,808B**；本包 66 次构建 6,044–6,816B（随池历史浮动，同一次开机里也是；STEP 页自己也在
  21,508–22,304B 之间浮动）。est 12,288 ⇒ 占 49–55%，**两级退路（银行行并成单对象 / 细调并入提示行）都不需要**。
- **build_ms**：9–13ms（≤100ms 预算）。
- **SEQ→SOUND 过渡**：`池剩余=13,456B`（提交前那次开机 13,452B），≥4,096。
- **内部堆**：`internal_free` 差 56B（另一次开机 60B）；`internal_min` 71,884–72,124B。
- **LVGL 任务栈余量** `lvgl_stack_free`：5,628–6,024B（门线 2,048B）。

## 3. 文件说明与复现

- 设备读数脚本 `task-2-proto-measure.py`（pyserial；相位 `base` / `seq` / `roundtrip` / `edit2step` / `fm6` /
  `fm6-restore` / `drift <页>`），串口字节逐字节写 transcript，工具注记一律 `# [proto] ...`。例：
  `python3 docs/dev/verify/5-pb1-2026-11-26/task-2-proto-measure.py /dev/ttyACM0 base /tmp/x.log`。
  每个相位前后各读一次四轨 `engine/preset`，证明读数之外的状态没被改动（`fm6` 相是唯一有意改状态的，`fm6-restore` 还原，
  收尾硬复位重新载入 autosave 对象）。
- 像素核对脚本 `task-2-proto-pngcheck.py <png> [--bank]`：按 `sound_layout_place` 的同一规则复算 13 张卡片的位置后逐张核对
  （退出码 0 = `PNG CHECK PASS`）。
- `task-2-proto-final-*.log`：最终固件上的串口原始回显。`task-2-proto-precommit-*.log`：提交前同源构建上的往返漂移对照
  （含一次**探针坐标写错**的记录，见 §4 ④）。
- `SHA256SUMS`：本目录全部文件的 SHA256。

## 4. 偏差、事故与裁决

1. **`knob-test.sh` 的 EDIT→STEP 账面断言为红。** SOUND est 6,144 → 12,288 是本任务的既定值，但 EDIT 组里 MOD/SCALE/VOICE
   三张页要到 5B-1 Task 7 才退役；在那之前账面 = 14,000 + 23,040 + 23,040 + 4,096 = **64,176 > 63,120**（超 1,056B）。
   实测（第 13 行）`池剩余=12,092–12,212B`，真实余量充足。我没有放宽这条断言（DI-56 的「登记超额」先例需要主控裁决）；
   其余四条被本任务直接改掉的钉值已更新。处置（主控裁决）：不放宽断言、不提前退役页面，按 ADR-008 §5 把 SOUND est 重校准为 8,704——见 §5。
2. **往返判据按字面不达标。** 计划写「往返 5 次后 delta 不变、`pool_free` 往返差 ≤128B」；冷启动后的头 5 轮是
   −296B / −384B（两次开机；同源构建上另有 −204B、−316B），之后 −144 → −52 → −36 → −20 → +4 → −8 → +36B。对照：未改动的 SAMPLE 页在硬复位后同样
   −200 → −36 → −48 → −8 → −32 → +32B（MOD −60B、GLOBAL +28B）。`synth.sh` ⑤ 的既有写法就是「预热两轮后」再量。
   「delta 不变」在任何页上都做不到（TLSF 块头随池历史浮动）。处置（主控裁决）：判据 = **预热一轮后 5 轮 `pool_free` 首末差 ≤128B**；冷启动首轮的下降是框架的一次性缓存，
   读数保留、照此标注（上表第 11 行的 warm / warm2 即判据口径）。
3. **PNG 没有人眼读过。** 本会话的代理没有图像输入能力，只做了像素核对（几何、墨迹、裁剪、背景）和一张 140×132 单元格的
   字符画抽查（名称在弧上方 y≈3–12、环 135°→45°、`--` 在弧内）。请用图像查看器过目两张 PNG。
4. **探针坐标写错一次（已恢复）。** 首版脚本把「卡片标题行」写成屏幕 `TAP 22 80`——那是页面区 y=8，即页眉 ENGINE 行的 `−` 区，
   把**选中轨（轨 2）的引擎从 ANALOG 拨到 NOISE**（`task-2-proto-precommit-base-miscoord.log:79`）。当时 `STORE AUTO OFF`，
   esptool 硬复位后四轨回到 `0/0`（`ENG SNAP` 核对），autosave 对象未被改写（`autosave_writes=0`）。脚本已改并加了相位前后的
  引擎快照。另一处同类错误：首版 `fm6` 相用 `ENG TRACK 0 9 0`——它直接写 core、不改 UI 的选中轨，页面上什么都没发生；
  改为经页眉 ENGINE 行走与手指相同的动作。
5. **注入场景未跑。** `scripts/inject/params.sh` / `nav.sh` / `store.sh`（bank 行坐标）/ `soak.py` / `screenshot_matrix.py`
   里按旧 SOUND 页写的断言与常量（8 槽 label 轮换、`SOUND_EST = 6144`、`PARAM_EDIT_SUM_EST = 6144 + 3 * 3584`、
   bank 行 y=552、`UI sound CHIP` 逐槽行）在 Task 3/7/8 改写前必然失配，本任务按计划没有跑；其余七条场景不经过 SOUND 页，
   也未重跑（留给 5B-1 收口矩阵）。
6. **`UI KNOB` 在 SOUND 页的令牌**：固件回 `ESP_ERR_NOT_SUPPORTED`，console 落到通用分支打 `UI ERR knob_slot <n>`（返回 1）。
   简报只让加两条 TAP 回显，专用令牌（`ignored (dashboard)` 之类）留给 Task 3。
7. **提交前自查修掉一处坐标换算错误。** `gesture_start` 把 indev 的**屏幕**坐标直接喂给 `content_point()`（它收页面区
   坐标，差 72px 页签条），手指按下时的环带判定会整体偏 72px。`TAP` 注入走的是另一条入口（ui.c 已减 72），所以设备读数
   发现不了；自查发现后修掉并并入 feat 提交，**全部读数与截屏在 `3628871` 上重取**（与修前同源构建的数字相同：唯一的代码差别是
   触摸回调里的一行算术）。手指拖动（滚动 vs 环带归属、死区、移动后不算点击）**没有任何真机验证**，TAP 不能拖。
8. **观察（未改）**：列间 16px 空隙与卡片间 8px 间隙没有可点对象，从那里起手的拖动不滚动（容器按简报没设 CLICKABLE）；
   卡片标题的 `▾` 用 `LV_SYMBOL_DOWN`（内置 Montserrat 字体没有 U+25BE）。

## 5. 评审前修正（主控裁决，2026-11-26；固件 `562c63f`）

裁决：`knob-test.sh` 的 EDIT→STEP 账面断言不放宽、页面不提前退役，改按 ADR-008 §5 的规则（实测 + 10–26%、512 对齐）把 SOUND 的
est 从开工估算 12,288 校准到 **8,704**（6,816 + Task 4 预留的两个对象约 640B = 7,456，+10% → 512 对齐）。改的只有布局表那一行
与 `knob_test.c` 的两处钉值；页面对象、绘制、手势、TAP 不变，所以第 7–17 行的读数与截屏仍然成立，下面只重跑了会受 est 影响的项。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 18 | host：参数页布局表 | `scripts/knob-test.sh` | `task-2-fix-knob-test.log` | ✅ **17 passed, 0 failed**。SOUND est 钉成 8,704、`EDIT Σ est = 19,456`；EDIT→STEP 账面 = 14,000 + 19,456 + 23,040 + 4,096 = **60,592 ≤ 63,120**（余 2,528B） |
| 19 | host：Sound 布局纯函数 | `scripts/sound-test.sh` | `task-2-fix-sound-test.log` | ✅ `4 passed, 0 failed` |
| 20 | 固件代码（构建） | `scripts/build.sh` ×2 | `task-2-fix-build-1.log`、`task-2-fix-build-2.log` | ✅ rc=0 / rc=0，编译 0 警告；第一遍只有豁免的 CMake 提示对（第 41 / 56 行），第二遍无 |
| 21 | 固件代码（烧录验证）+ 启动日志 | `scripts/flash.sh`；硬复位抓 10s 串口 | `task-2-fix-flash.log`、`task-2-fix-boot.log` | ✅ `Hash of data verified` ×3；`App version:      562c63f`、`ELF file SHA256:  c3d23c6e6…`；fatal/abort/backtrace/panic 0 行 |
| 22 | `UI MEM` 的 SOUND 行 | `… base …` | `task-2-fix-base.log` | ✅ `UI MEM page id=6 title=SOUND state=built est=8704 delta=6808 build_ms=9`；`页面构建 page=SOUND(6) 池增量=6808B 预算=8704B 池剩余=41484B 耗时=9ms`；`GROUP EDIT built=1 sum_est=19456 sum_delta=6808`；`internal_free` 差 60B、`internal_min=72064` |
| 23 | SEQ→SOUND 过渡峰值 | `… seq …` | `task-2-fix-seq.log` | ✅ `页面构建 page=SOUND(6) 池增量=6668B 预算=8704B 池剩余=13456B 耗时=10ms`（≥4,096）；切完 `pool_used=18960 pool_free=43096` |
| 24 | EDIT 四页全建 → 首建 STEP | `… edit2step …` | `task-2-fix-edit2step.log` | `GROUP EDIT built=4 sum_est=19456 sum_delta=15872` → `页面构建 page=STEP(2) 池增量=21796B 预算=23040B 池剩余=12212B 耗时=30ms` |

## 6. 5B-1 Task 3：槽值、锁定、拖动改值与注入（2026-11-26/27；固件 `f60c790`）

> Task 3 把 §1 的骨架接上真槽：成员表按上下文（选中轨 / 引擎 / `P_ITYPE` / `P_ESYNC` / `p[P_E0..P_E7]`）解析成存在位图，
> 存在的槽显示值、鼓轨上按 `drum_ok` 锁定、环带内拖动经状态层命令改值；新增 console/注入入口
> `UI SOUND CARD|SCROLL|STATS|KNOB` 与两条 TAP 回显。设备 Tab5 `/dev/ttyACM0`。**固件 = `f60c790`**（本节全部读数在它上面取；
> boot 自报 `App version: f60c790-dirty`——`-dirty` 来自工作树里**尚未提交的 docs 改动**，`git diff f60c790 -- firmware scripts third_party tools` 为空）。
> 读数脚本 `task-3-check.py`（硬复位 → 开机日志 → 12 个相位，断言 53 条），像素核对 `task-3-pngcheck.py`。
> **状态：机器侧全部通过；PNG 人眼未读；手指拖动没有任何真机验证**（TAP 与 `UI SOUND KNOB` 都不能产生连续指针事件）。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-3-commit-build-1.log`、`task-3-commit-build-2.log`（提交前同源构建：`task-3-build-1.log`、`task-3-build-2.log`） | ✅ 两遍 rc=0、编译 0 警告 0 错误（无 CMake/kconfig 变更，两遍都没有 §5.5 豁免的那对提示） |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-3-knob-test.log`、`task-3-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed`（两套测试的源与被测纯数据层本任务未改） |
| 3 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-3-commit-flash.log`（提交前：`task-3-flash.log`） | ✅ rc=0，`Hash of data verified` ×3 |
| 4 | 启动日志 | `task-3-check.py` 开头的 esptool 硬复位（输出进 transcript） | `task-3-commit-check.log` 第 29–31 行 | ✅ `App version: f60c790-dirty`（见上）、`ELF file SHA256: aa1484f89…`；fatal / abort() / backtrace / Guru Meditation 计数 0（全文与收尾那次复位） |
| 5 | 存在性与描述符 | `UI SOUND CARD 4 SLOT 8`、`UI SOUND CARD 1 SLOT 3`、13 卡全部 109 槽逐个对照 `ENG DESC live 0 <id>` / `ENG DESC global 0 <id>` | `task-3-commit-check.log` | ✅ `UI SOUND SLOT card=4 slot=8 flat=37 present=1 scope=0 id=58 label="TYPE" locked=0 value=0 x=28 y=1092 w=140 h=132`；`… card=1 slot=3 flat=7 present=1 scope=1 id=30 label="ROLL" locked=0 value=1 x=1080 y=188 …`；**109/109 label == 描述符、不一致 0**；首槽依次 BPM / DUST / ATK / RATE / WAVE / LVL / DST / ROOT / MODE / LEN / CH4 / CH1 / SRC |
| 6 | 池与内部堆 | `UI sound` → `UI MEM`；进入页面前后 `ENG MEM` | 同上 | ✅ `页面构建 page=SOUND(6) 池增量=6844B 预算=8704B 池剩余=41452B 耗时=10ms`；`UI MEM page id=6 title=SOUND state=built est=8704 delta=6844 build_ms=10`（≤12,288；Task 2 实测 6,808B，本任务不新增对象）；`internal_free` 72947 → 72843（差 **104B** ≤256）；`internal_min=71824` |
| 7 | 拖动改值（Voice·LVL，flat 38） | `UI SOUND KNOB 38 …` + `ENG SNAP` + `STATE SNAP` + 回读 | 同上 | ✅ `press 0 -40` ⇒ `UI SOUND KNOB flat=38 press dx=0 dy=-40 value=104`（不变）；`drag 40 0` ⇒ `… value=127`；`ENG SNAP trk k=0 engine=0 preset=0 level=127 …`、`STATE SNAP param trk=0 level=127 …`、`CARD 5 SLOT 0 … value=127` 相符；`drag -40 0` ⇒ `value=62`（见 §7 ①）；`drag 0 -40`（锚点）⇒ `value=104`；`press 0 0` ⇒ `… ignored (ring band)`；其后 `UI SOUND STATS cards=13 slots=109 scroll_max=1932 collapsed=0x0000` |
| 8 | 上下文键（Task 2 评审遗留 1） | `PARAM 53 1` / `PARAM 53 0`（`P_ITYPE`）、`PARAM 59 1` / `0`（`P_ESYNC`） | 同上 | ✅ `card 6 slot 18`：`label="INS A"` → `PARAM 53 1` → `label="DRIVE"` → `PARAM 53 0` → `"INS A"`（不用换轨/重进页）。⚠️ ESYNC 在 console 上**看不出**：上游 `ESYNC_DESC` 只换 fmt/names、label 仍是 `ATK`（`task-3-check.log` 里记为注记，不计 FAIL）；引擎槽值进键的部分与 ITYPE 同一条路径，本包未单独构造「换引擎钩子档位」的用例 |
| 9 | 锁定（Ctrl+4 选鼓轨） | `KEY 0x40 down` → `KEY 0x04 down` / `up` → `KEY 0x40 up` | 同上 | ✅ `selected_track=3`；`UI SOUND SLOT card=4 slot=0 flat=29 present=1 scope=0 id=63 label="KIT" locked=1 value=0 …`；`UI SOUND KNOB flat=29 drag dx=40 dy=0 ignored (locked)`、`… press dx=0 dy=-40 ignored (locked)`；四轨 `ENG SNAP` 行前后相同、槽值不变；`UI SOUND SLOT card=9 slot=0 flat=84 … label="LEN" locked=0 value=16 …` 且鼓轨上可拖（16 → 37 → 回 16）；`card=0 slot=0 … label="BPM" locked=0`（GLOBAL 不锁）；`TAP 98,442 page=6 -> SOUND focus card=2 slot=0 flat=9 CMD_SET_PARAM locked=1` |
| 10 | 不存在的槽 | 鼓轨上 `UI SOUND CARD 4 SLOT 1`、`UI SOUND KNOB 30 …`、TAP | 同上 | ✅ `UI SOUND STATS cards=13 slots=102 …`（109 − `P_E1..P_E7`）；`UI SOUND SLOT card=4 slot=1 flat=30 present=0 scope=- id=- label="-" locked=0 value=0 x=0 y=0 w=0 h=0`；`UI SOUND KNOB 30 drag 40 0` ⇒ `UI ERR sound (absent)`；同一滚动位置 `TAP miss 238,366 page=6`（它左边的 KIT 槽命中，见上一行末） |
| 11 | 冻结 | `ENG FREEZE ON` → press/drag → `ENG SNAP` → `ENG FREEZE OFF` | 同上 | ✅ 冻结期 `… press dx=0 dy=-40 value=0`、`… drag dx=40 dy=0 value=0`、四轨 `ENG SNAP` 行不变（`level=104`）；解冻后 `CARD 5 SLOT 0 … value=104`。值显示 0 是状态层在冻结时读全零的既有行为（见 §7 ④） |
| 12 | 滚动与 TAP | `UI SOUND SCROLL` / `400` / `99999` / `-50`，滚动后 TAP 槽与标题行 | 同上 | ✅ `UI SOUND SCROLL y=0 max=1932` → `y=400 max=1932`；`CARD 5 SLOT 0` 的矩形 `y 960 -> 560`（上移 400）；`TAP 730,626 page=6 -> SOUND focus card=5 slot=0 flat=38 CMD_SET_PARAM`；`TAP 688,542 page=6 -> SOUND card=5 collapsed=0`；`y=1932`（越界钳到底）、`y=0`（负值）；拖动/滚动之后 `collapsed=0x0000` |
| 13 | 非仪表盘页守卫 | `UI play` 后四条 `UI SOUND …`，再 `UI sound` 回来 | 同上 | ✅ 四条都回 `UI ERR sound (not_dashboard)`；回到 SOUND 后读数照常（`池增量=6700B`，同一页第二次构建） |
| 14 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0`（`f60c790` 同源构建） | `task-3-commit-selftest-heap.log`（stdout）、`task-3-commit-selftest-heap-transcript.log`（串口） | ✅ rc=0（326s）：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72675B`（≥61,440）；两次 `cases=182/182 identical=182 … result=PASS`，`# [selftest-heap] PASS`；transcript 里 fatal/abort/backtrace 计数 0。提交前同源构建那次（`task-3-selftest-heap.log`、`task-3-selftest-heap-transcript.log`）读数相同 |
| 15 | UI 截屏 | `python3 scripts/screenshot.py /dev/ttyACM0 …`（默认位置 + `UI SOUND SCROLL 1900`） | `task-3-sound-20261126-235855.png`、`task-3-sound-scrolled-20261126-235855.png`、`task-3-screenshot-{default,scrolled}.log`、`task-3-pngcheck-{default,scrolled}.txt` | ✅ 两张都截到（1280×720 rgb565）。`task-3-pngcheck.py` = `PNG CHECK PASS`：默认位置完整露出 21 个槽、滚动后 23 个，环在位 / 扫角缺口是面板色 / 值墨迹不少于占位；`BPM`（120）的 135° 与 180° 是次级文本色（值弧）、`SWING`（0）是面板亮色（只有底环）；Task 2 的 `task-2-proto-pngcheck.py` 在默认位置那张上仍 `PNG CHECK PASS`。**人眼读图未做** |

## 7. Task 3 的偏差、事故与未做项

1. **简报 Step 4 的「`drag -40 0` ⇒ 回到原值」与它自己 Step 2 规定的锚点语义矛盾。** 值 = 锚点值 + 角度差比例（与 `knob.c` / `UI KNOB` 同）：
   锚点在 `press 0 -40`（−90°），`drag -40 0` 是 180°，离锚点 −90° ⇒ 锚点值 − 值域/3 = **62**，不是 104；回原值要 `drag 0 -40`。
   我没有改语义，照实记录（第 7 行）并把「回到原值」的断言放在锚点方向上。
2. **简报的 `lv_obj_invalidate_area(卡片, 槽矩形)`「矩形用卡片内坐标」与 LVGL 9.6 不符。** `lv_obj_invalidate_area` →
   `lv_obj_area_is_visible` 收**屏幕绝对坐标**，再按对象与各级父对象的裁剪求交（`lv_obj_pos.c`）。实现传
   「卡片的屏幕坐标 + 槽内容矩形 − 卡片内容原点」，效果同简报的意图（只失效该卡片上的那个槽）。
3. **公共入口比简报多两个无锁小函数**：`sloop_ui_sound_slot_index(card, slot)`（`UI SOUND CARD n SLOT m` 对**不存在**的槽也要回
   `flat=`，而 `slot_info` 对它回 `NOT_FOUND`）与 `sloop_ui_sound_collapsed()`（`UI SOUND STATS` 的 `collapsed=`）。另：**下标越界**回
   `ESP_ERR_INVALID_ARG`（简报写 `NOT_FOUND`）——console 要用 `UI ERR sound (invalid)` 与 `(absent)` 把「参数写错」和「槽不存在」分开。
4. **冻结期的值是 0。** `ENG FREEZE ON` 后状态层每拍的 `sloop_core_read_params` 回全零（`sloop_core.h` 的既有契约，参数页旋钮同样），
   所以仪表盘的 `value=` 与屏幕上的值在冻结期显示 0；判据是 `ENG SNAP` 不变 + 解冻后回原值。**注入的锚点跨命令存活**（同 `UI KNOB`）：
   冻结期 `press` 建的锚点值是 0，解冻后不重新 `press` 直接 `drag` 会从 0 起算——开发探针里就这样把轨 0 的 LEVEL 写成了 0（见第 6 条）。
5. **不存在的槽的语义是我定的**（简报没写）：不画、TAP miss、环带内按下归滚动。Task 2 是「命中即回显 / 归槽」；按「present 取代旋钮页的槽存在性」
   推出，鼓轨上的 `P_E1..P_E7` 七个格子因此是空白。锁定槽仍画名称 + `--`、TAP 设焦点并回 `locked=1`。
6. **事故：开发探针改写了 autosave 对象，已恢复。** 调试拖动/冻结时我没有先 `STORE AUTO OFF`，轨 0 的 LEVEL 被写成 0、选中轨变成 0 并被 autosave
   落盘（下一次硬复位开机即 `level=0`）。恢复：`PARAM 0 104` + Ctrl+2 选回轨 1，等 autosave 写回后硬复位核对——四轨 `level=104`、`sel=1`、
   `bpm=120`、轨 1 `atk=10 dec=70 sus=90 rel=60`，与任务开始后第一次开机的读数逐项相同；autosave 对象因此多写了两次
   （`autosave_hash` C748D62F → FBC5F2C7 → 02C79E5A，最后一次之后两次开机读数不变）。**只能证明快照字段一致，不能逐字节证明对象与任务前相同。**
   证据脚本 `task-3-check.py` 从第一次正式运行起就是先 `STORE AUTO OFF`、收尾硬复位。
7. **未跑**：十条注入场景与截屏矩阵（§5.5「TAP 相关固件改动」那一类）——按旧 SOUND 页写的断言在 Task 7/8 改写 `inject.py` 前必然失配
   （§4 ⑤ 同），简报的设备核对清单也没有列；手指拖动（环带归属、死区、空白处滚动、长按抑制）没有任何真机验证，只有代码路径与
   `UI SOUND KNOB` 共用同一对 `sound_handle_pressed/pressing` 这一点；PNG 人眼读图。
8. **观察（未改）**：鼓轨上 Engine edit 卡首槽是 `KIT`（`P_E0` → `DRUM_KIT_DESC`）却按 `TS` 的 `drum_ok == 0` 锁定——与简报预期
   （`locked=1`）和 012 §3「旋钮区锁；KIT 行写 `P_E0`」一致，但仪表盘上这个旋钮永远是 `--`，主控可考虑在 Task 4+ 改成可编辑。
9. **读数脚本自身的三处错**（首版第一次运行 47/52 就是它们；那份 transcript 已被重跑覆盖，现存两份都是修正后的 53/53）：label 对照把 `scope` 当 `id` 用、
   INSERT A 的槽下标写成 14（应为 18）、锁定/不存在槽的 TAP 点在视口之外（`y=828 > 644`）。修正后的两次运行（提交前与 `f60c790`）都是 53/53。

## 8. Task 3 修复轮 1（评审「Needs fixes」；固件 `be06161`，2026-11-27）

> 两处 Important：① 鼓轨的 ANALOG 引擎门槛（`P_FTYPE` 在鼓轨上「存在但锁定」，`slots=102`）；② 上下文键一变就整页重绘
> （拖引擎槽 / ITYPE / ESYNC 每次移动都改键）。五处 Minor：`console.c` 两条语句误连一行、换轨后拖动锚点不失效、值文本每帧
> `lv_strndup`、长按抑制不显式、AGENTS.md 索引缺 `UI SOUND …` 一行。**本节读数都在 `be06161` 上取**（boot 自报
> `App version: be06161-dirty`，`-dirty` = 工作树里未提交的 docs 改动；`git diff be06161 -- firmware scripts third_party tools` 为空）。
> 测量用了一行**临时**固件日志 `TEMP-FIX1 重解析 失效卡片=N`（`task-3-fix1-measure.py` 靠它读数），测完即从源码删除：
> 提交里 `grep -rn TEMP-FIX1 firmware/` 为空。§6 第 10 行的「鼓轨 `slots=102`」自此作废，以本节为准。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | `scripts/build.sh` ×2（提交前同源）、提交后再 ×1 | `task-3-fix1-build-1.log`、`task-3-fix1-build-2.log`、`task-3-fix1-commit-build.log` | ✅ rc=0、编译 0 警告 0 错误 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-3-fix1-knob-test.log`、`task-3-fix1-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed` |
| 3 | 烧录 + 启动日志 | `scripts/flash.sh`；`task-3-check.py` 开头的硬复位 | `task-3-fix1-commit-flash.log`、`task-3-fix1-check.log`（第 29 行起） | ✅ `Hash of data verified` ×3；`App version: be06161-dirty`；fatal / abort / backtrace 计数 0 |
| 4 | 鼓轨引擎门槛（Important ①） | 轨 0 ANALOG：`UI SOUND STATS`、`UI SOUND CARD 4 SLOT 8`；Ctrl+4 后同样两条 | `task-3-fix1-check.log` | ✅ 轨 0：`UI SOUND STATS cards=13 slots=109 scroll_max=1932 collapsed=0x0000`、`UI SOUND SLOT card=4 slot=8 flat=37 present=1 … label="TYPE" locked=0 …`；鼓轨：`UI SOUND STATS cards=13 slots=101 …`、`UI SOUND SLOT card=4 slot=8 flat=37 present=0 scope=- id=- label="-" locked=0 value=0 x=0 y=0 w=0 h=0` |
| 5 | 重绘范围（Important ②）——**修复前** | `python3 docs/dev/verify/5-pb1-2026-11-26/task-3-fix1-measure.py /dev/ttyACM0 …`（固件 `c3ea99c` + 只有那行临时日志） | `task-3-fix1-baseline.log` | ❌ 待修行为：Engine edit·WAVE（`P_E0`，flat 29）4 次改值移动每次 `失效卡片=13`；ITYPE（flat 64）5 次改值移动每次 `失效卡片=13`（其余移动值没变、不触发重解析） |
| 6 | 重绘范围——**修复后** | 同一脚本（修复后固件 + 同一行临时日志） | `task-3-fix1-after.log` | ✅ WAVE：8 次移动的失效卡片数 `[0, 0, 0, 0, 0, 0, 0, 0]`——4 次改值移动**每次 0 张卡**（`失效卡片=0 all=0`）；ITYPE：5 次改值移动 `[1, 0, 0, 1, 1]`——只在 INSERT A 的标签真的换了的移动（`INS A → DRIVE`、`DRIVE → BITS`、`BITS → INS A`）失效 1 张（FX 卡），标签没换的移动 0 张 |
| 7 | 拖动后值仍刷新 | 同上，每次移动后 `UI SOUND CARD n SLOT m` 读回 | 同上 | ✅ 两个槽 16 次移动全部「读回 value == KNOB 回显」（脚本逐条核对）；ITYPE 换档时 `card 6 slot 18` 的标签跟着换（`INS A` / `DRIVE` / `BITS`） |
| 8 | 换轨打断拖动（Minor b） | 轨 0 上 `UI SOUND KNOB 38 press 0 -40` → Ctrl+4 → `drag 40 0` → Ctrl+1 → `drag 40 0` | `task-3-fix1-check.log` | ✅ 两次都 `UI SOUND KNOB flat=38 drag dx=40 dy=0 ignored (ring band)`；鼓轨 `ENG SNAP trk k=3 … level=104` 未被改写 |
| 9 | 回归：拖动 / 冻结 / 锁定 / 不存在 / 滚动 / TAP / 守卫 | `python3 docs/dev/verify/5-pb1-2026-11-26/task-3-check.py /dev/ttyACM0 …`（新增第 4、8 行的断言） | `task-3-fix1-check.log`、`task-3-check.py`（已更新） | ✅ **57/57**：`press 0 -40` 值不变 → `drag 40 0` ⇒ 127 → `drag 0 -40` ⇒ 104 → `press 0 0` ⇒ `ignored (ring band)`；`card=4 slot=0 … label="KIT" locked=1` 且 `ignored (locked)`；`card=4 slot=1 … present=0`、`UI ERR sound (absent)`；`ENG FREEZE on` 期间 `… drag dx=40 dy=0 value=0`、`ENG SNAP` 不变、解冻后回 104；`UI SOUND SCROLL y=400`、滚动后 TAP 命中 |
| 10 | 池与内部堆 | `UI sound` → `UI MEM`；进入前后 `ENG MEM` | `task-3-fix1-check.log` | ✅ `页面构建 page=SOUND(6) 池增量=6844B 预算=8704B 池剩余=41452B 耗时=11ms`；`UI MEM page id=6 title=SOUND state=built est=8704 delta=6844 build_ms=11`（≤8,704）；`internal_free` 差 128B；`internal_min=71824` |
| 11 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-3-fix1-selftest-heap.log`、`task-3-fix1-selftest-heap-transcript.log` | ✅ rc=0（324s）：run 1 `delta=260`、run 2 `delta=36`（门限 ≤1,024B），收尾 `internal_free=72683B`；两次 `cases=182/182 identical=182 … result=PASS`（跑在与 `be06161` 只差一处注释换行的同源构建上） |
| 12 | UI 截屏（值文本共用静态缓冲后） | `python3 scripts/screenshot.py …` + `task-3-pngcheck.py` | `task-3-fix1-sound-20261127-003838.png`、`task-3-fix1-screenshot.log`、`task-3-fix1-pngcheck.txt` | ✅ `PNG CHECK PASS`（21 个槽环在位、值墨迹 ≥ 占位、BPM 值弧 / SWING 底环）；Global 卡 4 个槽的值墨迹 `791 / 412 / 550 / 424` 各不相同（共用的 `s_value_text` 没有把上一个槽的文本留给下一个槽）。**人眼读图未做** |

说明：第 5 行的「修复前」读数是在 `c3ea99c` 上只加那一行临时日志测的，为的是给第 6 行一个同口径的对照；§6 的全部读数来自
修复前的固件，其中**只有「鼓轨 `slots=102` / `P_FTYPE` 在鼓轨上锁定」一条被本轮推翻**，其余不受这两处修复影响。

---

## 9. 5B-1 Task 4：键盘焦点、页级 ±、选轨条、折叠与读数行（2026-11-27；固件 `74cde92`）

> Task 4 在 Task 3 的卡片区上接上：←/→ 焦点（portMUX 保护的 PSRAM 状态，不取 LVGL 锁）、↑/↓/ENTER 与页级 `−/+`
> 对焦点槽 ±1、焦点槽滚进视口、右上角 4 格选轨条、卡片折叠（标题行点击 / `UI SOUND CARDTOGGLE`）、页眉下方的
> 提示/读数行（ZOOM 开 = 焦点槽的 28pt 读数），以及 console `UI SOUND FOCUS [<n>]`。设备 Tab5 `/dev/ttyACM0`。
> **固件 = `74cde92`**（`task-4-check.log` 第 29 行 `App version:      74cde92`，无 `-dirty`：代码先提交、再构建烧录取证）。
> 读数脚本 `task-4-check.py`（硬复位 → 10 个相位 → 收尾硬复位核对，断言 72 条）。
> **状态：机器侧全部通过；PNG 人眼未读；手指拖动/点击没有任何真机验证**（TAP 与 `UI SOUND KNOB` 都不能产生连续指针事件）。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-4-build-1.log`、`task-4-build-2.log` | ✅ 两遍 rc=0、编译 0 警告 0 错误；第一遍只有 §5.5 豁免的那对 CMake 提示（提交改了版本串 ⇒ 重新 configure），第二遍没有 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-4-knob-test.log`、`task-4-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed`（两套测试的源与被测纯数据层本任务未改） |
| 3 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-4-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 4 | 启动日志 | `task-4-check.py` 开头的硬复位（输出进 transcript） | `task-4-check.log` 第 29–31 行与第 884 行起 | ✅ `App version:      74cde92`、`ELF file SHA256:  5a29f370f…`；fatal / abort() / backtrace / Guru Meditation 计数 0（两次开机） |
| 5 | 池与内部堆 | `UI sound` → `UI MEM`；进入页面前后 `ENG MEM` | `task-4-check.log`、`task-4-check.out` | ✅ `页面构建 page=SOUND(6) 池增量=7312B 预算=8704B 池剩余=40984B 耗时=11ms`；`UI MEM page id=6 title=SOUND state=built est=8704 delta=7312 build_ms=11`（**est 未抬**；Task 3 实测 6,844B，本任务两个对象 +468B；同一会话里重建两次读 7,176 / 7,116B——首帧文本差，同其它页）；`internal_free` 72827 → 72759（差 **68B**）；`internal_min=71808` |
| 6 | 键盘焦点 | `UI SOUND FOCUS`、`KEY 0x4C`（→）、`KEY 0x4A`（←） | 同上 | ✅ 首个存在槽 `flat=0 card=0 slot=0 label="BPM"`；→ ⇒ `flat=1 … "SWING"`；← ⇒ 回 0；再 ← ⇒ 环绕到 `flat=108 card=12 slot=6 label="DUCK"`；末槽再 → ⇒ 回 0 |
| 7 | 焦点滚进视口 | 同上 + `UI SOUND SCROLL`、`UI SOUND CARD 12 SLOT 6` | 同上 | ✅ 焦点到末槽 ⇒ `UI SOUND SCROLL y=1872 max=1932`，末槽矩形 `y=576`（视口 152..716 内）；→ ×4（108 → 0 → 1 → 2 → 3）⇒ `flat=3 "TUNE"`、`y=28`（从底部滚回顶部）；`SCROLL 0` 后 `UI SOUND FOCUS 108` ⇒ `y=1872`；再按 → 环绕回 0 ⇒ `y=28`，首槽矩形 `x=28 y=160` |
| 8 | 调值（轨 0 · Voice·LVL，flat 38，原值 104） | `KEY 0x3B`（↑）×3、`KEY 0x3D`（ENTER）、`KEY 0x4B`（↓）×4、`TAP 1224 132`（+）、`TAP 1136 132`（−） | 同上 | ✅ `ENG SNAP trk k=0 … level=105` → `107` → ENTER `108` → ↓×4 `104`；`TAP 1224,132 page=6 -> PARAM slot=38 id=0 delta=+1 value=105`；`TAP 1136,132 … delta=-1 value=104`；其余三轨行不变、往返后四轨行与基线逐字相同 |
| 9 | 锁定槽 | Ctrl+4 鼓轨，焦点 9（ATK `locked=1`）↑/↓、`TAP 1224 132` | 同上 | ✅ 值不变、四轨行不变；`TAP 1224,132 page=6 -> PARAM slot=9 locked (page or slot N/A)`；锁定槽可持有焦点 `flat=9 … "ATK"` |
| 10 | 换轨后 30ms 窗口内的拖动（Task 3 评审遗留） | 一次批写 `UI SOUND KNOB 38 press 0 -40` → `TAP 1150 92` → `UI SOUND KNOB 38 drag 40 0` → `TAP 998 92`，×5 | 同上 | ✅ ×5 都是 `UI SOUND KNOB flat=38 drag dx=40 dy=0 ignored (ring band)`，轨 2 的 LEVEL 未被改写。⚠️ 主机侧批往返最长 111ms，**不能证明** drag 落在那 30ms 窗口里——见 §10 第 4 条 |
| 11 | 选轨条 | `TAP 998 92`（第 1 格）、`TAP 1150 92`（第 3 格）、`STATE SNAP`、`UI` | 同上 | ✅ `TAP 998,92 page=6 -> TRACKS select track=0 CMD_SELECT_TRACK`；`TAP 1150,92 … track=2 …`；`selected_track=2`；`UI page=sound (6)`；换轨后焦点仍是存在的槽；还原 `track=0` |
| 12 | 折叠 | `UI SOUND CARDTOGGLE 6`、`UI SOUND STATS`、`UI SOUND SCROLL 99999`、`UI SOUND FOCUS 60`、`UI SOUND CARD 6 SLOT 13` | 同上 | ✅ `UI SOUND CARDTOGGLE card=6 collapsed=1`；`UI SOUND STATS cards=13 slots=109 scroll_max=1524 collapsed=0x0040`（1932 → 1524）；`y=1524 max=1524`；折叠卡的槽 `present=1 … x=0 y=0 w=0 h=0`；`UI SOUND FOCUS 60` ⇒ `y=1084`（折叠卡标题行 y=1092 可见）；还原 `collapsed=0`、`scroll_max=1932 collapsed=0x0000` |
| 13 | 标题行 TAP | `TAP 76 358`（Envelope 卡标题行）×2 | 同上 | ✅ `-> SOUND card=2 collapsed=1`，`collapsed=0x0004`、`scroll_max=1712`、`y=0`（焦点在首槽，列表不动）；再 TAP ⇒ `collapsed=0`、`0x0000`、`1932`；`CARDTOGGLE 13` ⇒ `UI ERR sound (invalid)` |
| 14 | 消失槽 | `UI SOUND FOCUS 37`（P_FTYPE）→ `TAP 283 92`（ENGINE +）→ `UI SOUND FOCUS` → `UI SOUND FOCUS 37` → `KEY 0x4C` → `TAP 37 92` | 同上 | ✅ `-> SOUND ENGINE value=1 name="DIGITAL" status="OK"`；`card=4 slot=8 flat=37 present=0`；焦点重指 `flat=38 card=5 slot=0 label="LVL"`（扁平序下一个存在的槽）；`UI SOUND FOCUS 37` ⇒ `UI ERR sound (absent)`；→ ⇒ `flat=39 … "VCE"`；`-> SOUND ENGINE value=0 name="ANALOG" status="OK"`，`present=1 label="TYPE"`，`slots=109 scroll_max=1932`；无崩溃 |
| 15 | ZOOM 读数行 | `UI system` → `TAP 374 163`（ZOOM chip）→ `UI sound` → 截屏 → 再 `TAP 374 163` → `STORE SETTINGS SAVE` | `task-4-sound-hint.png`、`task-4-sound-zoom.png`、`task-4-sound-zoom-lvl.png`、`task-4-check.out`（⚠️ 三个文件名自修复轮 1 起已改：`task-4-sound-hint-placeholder.png`（提示行含 4 个占位空框，见 §11）、`task-4-fix1-sound-zoom.png`、`task-4-fix1-sound-zoom-lvl.png`；哈希见提交 `a7dc839` 的 `SHA256SUMS`） | ✅ `-> SYS ZOOM value=ON status="OK"` / `value=OFF status="OK"`，`STORE SETTINGS INFO … zoom=0 … dirty=0`。像素核对（脚本内，MONO 色取 `theme_derive.h`）：ZOOM 关的提示行墨迹 3,866 像素、亮色（`UI_TEXT`）0；选轨条第 1 格选中底色 1,860、其余三格面板底色 1,844 / 1,842 / 1,841；焦点描边（flat 0）强调色 1,004 像素且内区 0、非焦点槽（flat 1）0；ZOOM 开读数行亮色 968 像素（x 19..208、y 117..136，未被 32px 行高裁掉）；焦点换槽后两帧读数行差异 2,068 像素。**人眼读图未做** |
| 16 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-4-selftest-heap.log`（stdout）、`task-4-selftest-heap-transcript.log`（串口） | ✅ rc=0（325s）：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72659B`（≥61,440）；两次 `cases=182/182 identical=182 … result=PASS`；transcript 里 fatal/abort/backtrace 计数 0 |
| 17 | 还原核对 | `task-4-check.py` 收尾硬复位后的快照 vs 开机基线 | `task-4-check.log`、`task-4-check.out` | ✅ `sel=1 bpm=120`、四轨 `engine=0 preset=0 level=104`、`STATE SNAP param trk=1 level=104 atk=10 dec=70 sus=90 rel=60` 与基线逐字相同；`zoom=0 dirty=0`；`autosave_hash=02C79E5A autosave_writes=0`（`autosave` 开关复位后回 1）。自检堆之后再硬复位一次读数相同 |


## 10. Task 4 的偏差、事故与未做项

1. **提示文案是 ASCII，不是简报给的中文。** 简报/规格 §5.2 的提示行是 `拖动旋钮调值 · 拖动空白滚动 · ←→ 焦点 ↑↓ 调值 · 点页眉名字选择`；
   固件只带 Montserrat 14/16/20/28（`sdkconfig` 的 `CONFIG_LV_FONT_MONTSERRAT_*`），没有 CJK 与箭头字形，`CONFIG_LV_USE_FONT_PLACEHOLDER=y`
   下中文会画成一排占位框。实际文案：`Drag a knob to edit · Drag empty space to scroll · Left/Right focus · Up/Down value · Tap header name to choose`
   （`page_sound.c` 的 `SOUND_HINT_TEXT`，一个宏；中文版要等 5F 的字体）。**需要主控裁决**：接受 ASCII，还是 5F 再换。
2. **简报 Step 4 的「消失槽 ⇒ `card=4 slot≤7`」与派发说明的遗留 2 不一致。** 遗留 2 规定「扁平序里下一个存在的槽，否则首个」，
   焦点在 `P_FTYPE`（37）上换引擎后落到 **38（Voice·LVL）**，不是 Engine edit 卡的其它槽。按遗留 2 实现。
3. **简报 Step 4 的「`KEY 0x4C` ×4 后 `y` > 0」只有在接在「← 环绕到 108」之后才成立**（108 → 0 → 1 → 2 → 3，焦点槽从视口底部回到顶部，`y=28`）；
   若从 `flat=0` 起按 ×4，焦点槽 4 在视口顶部、无需滚动（`y=0`）。脚本按前一种顺序断言。滚进视口的规则用派发说明的「上下留 8px」，不是简报的 `y − 80` / `y + h − 644`。
4. **换轨窗口内的拖动（遗留 3）只有代码路径与一次尽力而为的批写测试。** 实现：`sound_handle_pressed` 记下锚点所属的 `s_ctx.track/engine`，
   `sound_handle_pressing` 与**此刻**的 `state_selected_track()/state_selected_engine()` 比对，不符就 `s_drag_flat = -1` 并返回 false。
   这同时盖住「按下时上下文已换、下一拍还没重解析」（记的是旧上下文）与「按下之后才换」两种情形。批写测试的主机往返最长 111ms，
   设备侧三行是否背靠背落在 30ms 内没有证据。
5. **TAP 命中卡片标题行现在翻折叠位**（`collapsed=` 是翻转后的值）。Task 3 时只回显（`task-3-check.log` 里 `TAP 688,542 … card=5 collapsed=0`）；
   按 AGENTS §5.4.1「TAP 执行与控件回调相同的动作函数」改。**`task-3-check.py` 的那条断言（期望 `collapsed=0`）自此失配**，脚本未改。
6. **事故：ZOOM 被写进了 flash，已恢复。** 第一次取证运行（固件 `74cde92`，`task-4-zoom-persist-check.{log,out}`，71/72）在收尾复位后读到
   `settings: flash … zoom=1`：ZOOM 开着截两张屏的 ~35s 里，1s 任务的延迟写把 settings 对象落了盘——settings 的 dirty **不受 `STORE AUTO OFF` 约束**
   （`store_tab5.c` 的 `sloop_store_sections_tick`：停走带 + 安静 + 输入空闲 ≥1.5s + 距上次尝试 ≥5s；P4-E Task 2 的既定行为），日志里没有一行提示。
   之前的开发运行恰好没撞上 5s 间隔，复位后是 `zoom=0`，让脚本错误地以为「硬复位会丢弃」。恢复：`STORE SETTINGS SET zoom 0` →
   `STORE SETTINGS SAVE` → `STORE SETTINGS INFO … zoom=0 … dirty=0` → 硬复位后 `zoom=0`、`autosave_hash=02C79E5A`。脚本照 `inject.py` 的
   `run_sys ⑦` 改成关掉 ZOOM 后显式 SAVE，第二次运行 72/72。**settings 对象因此多写了两次（内容与基线相同）。**
7. **未跑**：十条注入场景与截屏矩阵（§5.5「TAP 相关固件改动」那一类；旧 SOUND 页断言在 Task 7/8 前必然失配，同 Task 3 §7 第 7 条）；
   手指点击标题行/选轨条/± 按钮、手指拖动（只有 TAP 与 `UI SOUND KNOB` 走同一动作函数这一点）；PNG 人眼读图；
   LVGL 回调里 `LV_EVENT_CLICKED` 对选轨条的指针落点分支（TAP 走的是 `page_sound_tap` 的矩形表，不是回调）。
8. **观察（未改）**：页眉上下文行（`SOUND · TRACK n`，`page_params.c` 的 `header` label）矩形是 x 720..1080，与选轨条 960..1264 在对象层重叠 120px；
   文字实际止于 ~900/720（20pt），不与选轨条相撞，所以没动它。折叠卡里的槽仍可持有焦点（`UI SOUND FOCUS <n>` 接受、读数行与 ± 照常）。

---

## 11. Task 4 修复轮 1（评审「Needs fixes」；固件 `3b3bdf7`，2026-11-27）

> 评审：Critical 1（提示行的 `·` 渲染成占位框）、Important 2–4（焦点/折叠态随页面重建丢失；键盘与 ± 路径的过期上下文写入；
> R12 折叠卡里的焦点按悬空处理）、Minor a–g。**本节读数都在 `3b3bdf7` 上取**（boot 自报 `App version:      3b3bdf7-dirty`：
> `-dirty` = 工作树里未提交的 docs/证据改动，`git diff 3b3bdf7 -- firmware scripts third_party tools` 为空）。

### A. 改了什么

| 评审项 | 改动 |
|---|---|
| Critical 1 提示占位框 | `SOUND_HINT_TEXT` 的分隔符 `·`（U+00B7）→ `\|`；文案每个字符都在 ASCII 0x20..0x7E（`Drag a knob to edit \| Drag empty space to scroll \| Left/Right focus \| Up/Down value \| Tap header name to choose`） |
| Important 2 重建保留 | `page_sound_forget` 不再复位 `s_focus_flat`、`page_sound_build` 不再清零 `s_collapsed`；`focus_resolve` 在 `!s_ctx_valid`（重建后首拍重解析之前）只回显、不重指 |
| Important 3 过期上下文 | `sound_slot_cmd`（唯一写路径）比对 `s_ctx.track/engine` 与此刻的 `state_selected_track()/state_selected_engine()`，不符即拒——拖动、键盘、`−/+`、TAP 同一条纪律 |
| Important 4 / R12 | `focus_ok` = 存在 ∧ 所在卡片没折叠；`focus_resolve` 每拍保证「焦点 ∈ 可聚焦」：悬空（消失或被折叠）⇒ 扁平序下一个可聚焦的槽（环绕）；全部折叠 ⇒ 保持原焦点，`sound_slot_cmd` 拒写、读数行 `--` |
| Minor a 滚动方向 | `gesture_move` 的 dy 由 `from − p.y` 改为 `p.y − from`（内容跟手指），证据见 B |
| Minor b | `s_collapsed` 经 `collapsed_get/set/bit` 在同一把 `s_focus_mux` 临界区读写 |
| Minor c | 读数行只在 ZOOM 开、或从读数切回提示时重写/失效；ZOOM 关的静态文案画过一次不再失效 |
| Minor d/e/f/g | `console-and-injection.md` 删掉过时的「焦点槽动作在 Task 4 接入，不发命令」；AGENTS §5.4.1 的 `UI SOUND` 行加 `FOCUS [<flat>]` / `CARDTOGGLE <n>`（31,317B）；`task-3-check.py` 的标题行 TAP 断言改为 `collapsed=1` 并用 `CARDTOGGLE 5` 展开；`param_tap_button` 用 `page_sound_adjust_value` 的 `applied` 回显、`0xFFFFu` → `SOUND_SLOT_NONE` |

### B. 滚动方向的证据（Minor a）

LVGL 9.6 自己的手势滚动：`lv_indev.c:1395` `indev->pointer.vect.y = act_point.y - last_point.y`（指针位移，新 − 旧）→
`lv_indev_scroll.c:107,119` `diff_y = elastic_diff(…, vect.y, …)`、`lv_obj_scroll_by_raw(scroll_obj, diff_x, diff_y)` →
`lv_obj_scroll.c:472-475` `scroll.y += y; lv_obj_move_children_by(obj, x, y, true)`（子对象按 dy 平移 = 内容跟手指）。
`lv_obj_scroll_by`（非动画）直接转 `lv_obj_scroll_by_raw(obj, dx, dy)`，`lv_obj_scroll_by_bounded` 在它外面做钳位。
本页原先传 `from - p.y`（= −vect.y）⇒ 内容逆着手指走；现传 `p.y - from`。**手指滚动仍未在真机上验证**（console 不能产生连续指针事件）。

### C. 提示行字形检查（Critical 1 的证明）

方法（`task-4-check.py` 的 `hint_glyphs`）：截屏的提示行（屏幕 y 112..144、x 16..1088）取墨迹（与背景色差 > 40），
8 连通域；**占位框 = 高 ≥ 15px 且内部无墨迹的连通域**。依据：LVGL 缺字形时画的是 `line_height` 高的空框
（`lv_font.c`：`box_w = line_height / 2`、`box_h = line_height`；14pt ⇒ 10×16），而真字形的连通域 ≤ 14px。
同一函数跑在修复前后的两张截屏上：

| 截屏 | 连通域 | 最高连通域 | 占位框 |
|---|---|---|---|
| 修复前 `task-4-sound-hint-placeholder.png`（`74cde92`，`·` 分隔） | 88 | 16px | **4**（x 162 / 375 / 509 / 640 起，各 10×16 空心框） |
| 修复后 `task-4-sound-hint.png`（`3b3bdf7`，`\|` 分隔） | 88 | 14px（`\|`） | **0** |

另：脚本从 `page_sound.c` 抠出 `SOUND_HINT_TEXT` 的字面量，断言每个字符在 0x20..0x7E 内。

### D. 命令与输出

```
scripts/knob-test.sh        → 17 passed, 0 failed            scripts/sound-test.sh → 4 passed, 0 failed
scripts/build.sh ×2         → rc=0 / rc=0，编译 0 警告 0 错误（第一遍只有 §5.5 豁免的那对 CMake 提示）
scripts/flash.sh            → rc=0，Hash of data verified ×3
boot: App version:      3b3bdf7-dirty     ← -dirty = 未提交的 docs；fatal/abort/backtrace 计数 0
I (7766) sloop_ui: 页面构建 page=SOUND(6) 池增量=7312B 预算=8704B 池剩余=40984B 耗时=11ms
UI MEM page id=6 title=SOUND state=built est=8704 delta=7312 build_ms=11
ENG MEM internal_free=72843 internal_min=72032 …   ← 进页面前；72783 / internal_min=71972 ← 进页面后（差 60B）

—— Important 3：Ctrl+2 与 ↑ 一次批写（6 行一写），×5
# [check] PASS 批写 Ctrl+2 → ↑ ⇒ selected_track=1，四轨 ENG SNAP 行不变（轨 1 LEVEL 未被写）   ×5
（若 ↑ 落在下一拍之后，它会正常给轨 1 的 LEVEL +1：这条通过 = 5 次都落在窗口里且被拒）

—— Important 4 / R12
UI SOUND FOCUS 60（card 6 已折叠）⇒ UI SOUND FOCUS flat=69 card=7 slot=0 label="ROOT"；UI SOUND SCROLL y=1164 max=1524
UI SOUND SLOT card=7 slot=0 flat=69 present=1 scope=0 id=25 label="ROOT" locked=0 value=0 x=28 y=160 w=140 h=132
焦点 40（card 5）时 CARDTOGGLE 5 ⇒ UI SOUND CARDTOGGLE card=5 collapsed=1；UI SOUND FOCUS flat=47 card=6 slot=0 label="DST"
全部折叠（CARDTOGGLE 0..12）⇒ UI SOUND STATS cards=13 slots=109 scroll_max=0 collapsed=0x1FFF；UI SOUND FOCUS flat=0 card=0 slot=0 label="BPM"
全折叠时 ↑/↓ ⇒ 四轨行不变；TAP 1224,132 page=6 -> PARAM slot=0 locked (page or slot N/A)
全部展开 ⇒ UI SOUND STATS cards=13 slots=109 scroll_max=1932 collapsed=0x0000；焦点不变 flat=0

—— Important 2：重建保留
UI step → UI sound ⇒ UI SOUND STATS cards=13 slots=109 scroll_max=1524 collapsed=0x0040；UI SOUND FOCUS flat=69 card=7 slot=0 label="ROOT"（与重建前相同）

—— Critical 1
# [check] PASS 提示行字形：连通域 88 个、最高 14px、占位框 0 个（期望 0）
# [check] PASS SOUND_HINT_TEXT 全 ASCII 0x20..0x7E：Drag a knob to edit | Drag empty space to scroll | Left/Right focus | Up/Down value | Tap header name to choose
# [check] PASS 同一检查在修复前的截屏 task-4-sound-hint-placeholder.png 上报出占位框 4 个（最高 16px）

—— 回归（首轮的 72 条全部保留，其中「折叠卡里的 FOCUS 60」一条按 R12 改写）
# [check] PASS 86/86
python3 scripts/selftest-heap.py /dev/ttyACM0 → rc=0（323s）
# [selftest-heap] run1: before=72987 after=72719 delta=268 result=PASS cases=182/182 identical=182
# [selftest-heap] run2: before=72719 after=72683 delta=36 result=PASS cases=182/182 identical=182
```

设备核对脚本仍是 `task-4-check.py`（已更新；可选第 4 个参数 = 修复前截屏，用来证明字形检查分得清占位框）。
池 delta 7,312B（est 8,704 未抬；本轮没有新增对象）；同一会话重建两次读 7,112 / 7,092B。
收尾硬复位后：`sel=1`、四轨 `engine=0 preset=0 level=104`、`STATE SNAP param trk=1 level=104 atk=10 dec=70 sus=90 rel=60`、
`zoom=0 dirty=0`、`autosave_hash=02C79E5A autosave_writes=0`；自检堆之后再硬复位一次读数相同。

### E. 仍然有效的保留项

手指滚动/点击未在真机验证（滚动方向只有 LVGL 源码证据）；PNG 人眼未读；十条注入场景与截屏矩阵未跑（同 §10 第 7 条）；
换轨窗口的批写测试：`↑` 那条有区分力（窗外会写、窗内不写），拖动那条仍只有「被拒 + 未改写」。

---

## 12. Task 4 修复轮 2（复审：R12 的全折叠条款；固件 `fc59cb7`，2026-11-27）

> 复审确认修复轮 1 的各项都已处理，只剩 R12 的一个条款没实现、且测试被放宽成了「某个存在的槽」。本节读数都在
> `fc59cb7` 上取（boot 自报 `App version:      fc59cb7-dirty`：`-dirty` = 工作树里未提交的 docs，
> `git diff fc59cb7 -- firmware scripts third_party tools` 为空）。

| 评审项 | 改动 |
|---|---|
| R12 全折叠条款 | `focus_resolve`：找不到任何可聚焦的槽时，焦点槽仍存在 ⇒ **保持**（`next` 维持 `f`）；只有 `!present_bit(f)` 才 `sound_layout_first_present`。修复轮 1 的版本在这里无条件落到 flat 0 |
| 精确断言 | `task-4-check.py`：焦点 47（card 6）→ 先折其它 12 张（card 6 仍开 ⇒ 焦点不动）→ 最后折 card 6 ⇒ `UI SOUND FOCUS` **仍是 47**；`↑/↓` 不写；`TAP +` ⇒ `PARAM slot=47 locked`。另加一条「全折叠且焦点槽已不存在 ⇒ 首个存在槽」（焦点设到 37 后换引擎让它消失 ⇒ `flat=0`） |
| 文档 | `console-and-injection.md`：去掉「或所在卡片被折叠）由 UI 拍」的多余右括号；`TAP` 的 `−/+` 说明补「`PARAM slot=<n> locked` 也是『所在卡片折叠』『上下文已变』两种拒绝的回显」；`firmware-map.md` 同步措辞 |
| `hint_text_is_ascii` | 字面量含反斜杠（`\xNN` / `\uNNNN` 转义在源码里是 ASCII、展开后不是）一律判失败 |

```
scripts/knob-test.sh / sound-test.sh   → 17 passed, 0 failed / 4 passed, 0 failed
scripts/build.sh ×2                    → rc=0 / rc=0，编译 0 警告 0 错误（第一遍只有 §5.5 豁免的那对 CMake 提示）
scripts/flash.sh                       → rc=0，Hash of data verified ×3
I (7756) sloop_ui: 页面构建 page=SOUND(6) 池增量=7312B 预算=8704B 池剩余=40984B 耗时=11ms
UI MEM page id=6 title=SOUND state=built est=8704 delta=7312 build_ms=11
ENG MEM internal_free=72827 internal_min=71836 …  → 72759 / internal_min=71808（差 68B）

其余 12 张折叠、card 6 仍开 ⇒ 焦点不动：UI SOUND FOCUS flat=47 card=6 slot=0 label="DST"
全部折叠 ⇒ UI SOUND STATS cards=13 slots=109 scroll_max=0 collapsed=0x1FFF；焦点保持不变：UI SOUND FOCUS flat=47 card=6 slot=0 label="DST"（折叠前 47）
全折叠时 ↑/↓ 不写（四轨行不变）；TAP 1224,132 page=6 -> PARAM slot=47 locked (page or slot N/A)
全折叠时把焦点设进 card 4 ⇒ 保持：UI SOUND FOCUS flat=37 card=4 slot=8 label="TYPE"
焦点槽随引擎消失（全折叠）⇒ 落到首个存在槽：UI SOUND FOCUS flat=0 card=0 slot=0 label="BPM"
全部展开 ⇒ UI SOUND STATS cards=13 slots=109 scroll_max=1932 collapsed=0x0000；焦点不变 flat=0
提示行字形：连通域 88 个、最高 14px、占位框 0 个；同一检查在修复前截屏上报出占位框 4 个（最高 16px）
# [check] PASS 90/90
python3 scripts/selftest-heap.py /dev/ttyACM0 → rc=0（325s）
# [selftest-heap] run1: before=72963 after=72703 delta=260 result=PASS cases=182/182 identical=182
# [selftest-heap] run2: before=72703 after=72659 delta=44 result=PASS cases=182/182 identical=182
```

还原：收尾硬复位后 `sel=1`、四轨 `engine=0 preset=0 level=104`、`STATE SNAP param trk=1 level=104 atk=10 dec=70 sus=90 rel=60`、
`zoom=0 dirty=0`、`autosave_hash=02C79E5A autosave_writes=0`；自检堆之后再硬复位一次读数相同。池 delta 7,312B（est 8,704 未抬）。
证据：`task-4-fix2-{build-1,build-2,flash,check,knob-test,sound-test,selftest-heap,selftest-heap-transcript}.log`、`task-4-fix2-check.out`、`task-4-fix2-sound-hint.png`。

## 13. 5B-1 Task 6：引擎 / 预置 / 鼓 kit 选择器（2026-11-27；固件 `792a5a0`）

> 点 SOUND 页眉行的**名字区**（中段；左右 42px 仍是 ∓1）打开的面板：合成轨 row 0 = 引擎 chip 行 + 12 个引擎的列表、row 1 = chip 行 +
> **当前引擎**的预置列表；鼓轨 row 0 = 46 个 kit 的列表（没有 chip 行）。chip 只高亮/预览，再点一次或 ENTER 才换引擎；列表行选择各发一条
> `CMD_SET_ENGINE` / `CMD_SET_PRESET` / `CMD_SET_PARAM`（kit）。逻辑在 `page_sound_picker.{h,c}`，`page_sound.c` 只路由。
> 设备 Tab5 `/dev/ttyACM0`。**固件 = `792a5a0`**（`task-6-check.log` 第 29 行 `App version:      792a5a0`，无 `-dirty`：代码先提交、再构建烧录取证）。
> 读数脚本 `task-6-check.py`（硬复位 → 6 个相位 → 收尾硬复位核对，断言 109 条）。
> **状态：机器侧全部通过；PNG 人眼未读（只有像素核对）；手指点击/拖动 chip 行的横向滚动没有任何真机验证**（TAP 不能产生连续指针事件）。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-6-build-1.log`、`task-6-build-2.log` | ✅ 两遍 rc=0、编译 0 警告 0 错误；第一遍只有 §5.5 豁免的那对 CMake 提示（提交改了版本串 ⇒ 重新 configure），第二遍没有 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-6-knob-test.log`、`task-6-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed`（两套测试的源与被测纯数据层本任务未改） |
| 3 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-6-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 4 | 启动日志 | `task-6-check.py` 开头/收尾的两次硬复位 | `task-6-check.log` 第 29–31 行与第 1290 行起 | ✅ `App version:      792a5a0`、`ELF file SHA256:  b2553dcba…`；E / abort() / backtrace / Guru Meditation 计数 0（两次开机） |
| 5 | 池与内部堆 | `UI sound` → `UI MEM`；进入页面前后 `ENG MEM` | `task-6-check.out` | ✅ `页面构建 page=SOUND(6) 池增量=7308B 预算=8704B 池剩余=40984B 耗时=10ms`（est 未抬；选择器的对象开时才建）；`internal_free` 72859 → 72755（差 **104B**）；`internal_min=71896` |
| 6 | 打开 row 0（引擎） | `TAP 160 92`、`UI SOUND PICK` | 同上 | ✅ `TAP 160,92 page=6 -> SOUND pick open row=0`；`UI SOUND PICK open=1 row=0 engine=0 highlight=0 rows=12`；`UI SOUND PICK 0` ⇒ `text="ANALOG" current=1 x=176 y=248 w=928 h=32`；`PICK 8` ⇒ `"GRAIN"`、`PICK 9` ⇒ `"FM6"`、`PICK 11` ⇒ `"NOISE"`（`current=0`）；`PICK 12` ⇒ `UI ERR sound (absent)`、`PICK x` ⇒ `(invalid)`；`UI` 仍 `page=sound (6)`；`UI SOUND FOCUS` / `UI SOUND STATS`（109 槽、`collapsed=0x0000`）不受影响 |
| 7 | 池：打开时 | 打开前后 `UI MEM` | 同上 | ✅ `pool_free` 43504 → 42860（差 **644B**，≤ 4,096） |
| 8 | 矩形查询滚入视口 | `UI SOUND PICK 11` → `PICK 0` | 同上 | ✅ 第 11 行（第 12 个，一屏 11 行之外）⇒ `y=568`（列表上移一行）；`PICK 0` ⇒ 列表滚回顶部 `y=248` |
| 9 | 键盘：引擎 | `KEY 0x4C`（→）×9、`KEY 0x3D` | 同上 | ✅ `→ ×9` ⇒ `highlight=9`（轨 0 引擎仍 0）；ENTER ⇒ `ENG SNAP trk k=0 engine=9 preset=0`；`UI SOUND PICK open=0` |
| 10 | TAP 行选引擎 | 再开 row 0（`engine=9 highlight=9 rows=12`）→ `PICK 8` 的矩形中心 `TAP` | 同上 | ✅ `TAP 640,520 page=6 -> SOUND pick engine=8 CMD_SET_ENGINE`；轨 0 `engine=8`；选择器关闭 |
| 11 | chip：先高亮、再确认 | `TAP 324 220`（DIGITAL chip 中心）×2 | 同上 | ✅ 第一次 `-> SOUND pick engine=1 highlight`、`open=1 row=0 engine=8 highlight=1`、轨 0 引擎仍 GRAIN；第二次 `-> SOUND pick engine=1 CMD_SET_ENGINE`、轨 0 `engine=1` |
| 12 | 确认当前项不下发 | `KEY 0x4A`（←）+ ENTER 回 ANALOG；再开 row 0，`PICK 0` 的行 TAP | 同上 | ✅ `← + ENTER` ⇒ `engine=0`；`TAP 640,264 page=6 -> SOUND pick engine=0 unchanged`；轨 0 引擎仍 ANALOG；选择器关闭 |
| 13 | 打开 row 1（预置） | `TAP 512 92`、`PICK 0`、`PICK 3` 的行 TAP | 同上 | ✅ `-> SOUND pick open row=1`；`open=1 row=1 engine=0 highlight=0 rows=22`；`text=" 1  808 BOOM" current=1`；`TAP 640,360 page=6 -> SOUND pick preset=3 CMD_SET_PRESET`；`ENG SNAP trk k=0 … preset=3`；关闭 |
| 14 | 键盘：预置 | 再开 row 1（`PICK 3` ⇒ `current=1`）→ `KEY 0x4B`（↓）×2 → ENTER | 同上 | ✅ 轨 0 `preset=5`；关闭 |
| 15 | ESC | 开 row 1 → `KEY 0x00` | 同上 | ✅ `UI SOUND PICK open=0`；`UI page=sound (6)`（不切页） |
| 16 | 预览规则 | `TAP 1009 220`（FM6 chip）→ `PICK 0` 的行 TAP | 同上 | ✅ `-> SOUND pick engine=9 highlight`；`open=1 row=1 engine=0 highlight=9 rows=8`（FM6 的 8 个预置：`" 1  TINE EP"`，`current=0`）；行 TAP ⇒ `-> SOUND pick preset=0 ignored (preview)`；轨 0 仍 `engine=0 preset=5`；面板保持打开 |
| 17 | chip 环绕与页级 ± | `KEY 0x4A` ×10、`KEY 0x4C`、`TAP 1224 132` | 同上 | ✅ `← ×9` ⇒ `highlight=0 rows=22`；再 ← ⇒ `highlight=11 rows=11`（NOISE）；→ ⇒ 回 `highlight=0 rows=22`；页级 `+` ⇒ `-> SOUND pick preset=21 highlight`（列表高亮上一行，同 ↑） |
| 18 | 上下文变化关闭 | 开 row 0 → Ctrl+2；轨 1 上开 → `TAP 1150 92`；开 → `UI tracks` → `UI sound` | 同上 | ✅ Ctrl+2 ⇒ `open=0`、`selected_track=1`；选轨条 ⇒ `-> TRACKS select track=2`、`open=0`；TRACKS 页 `UI SOUND PICK` ⇒ `(not_dashboard)`，切回 SOUND ⇒ `open=0`（不跨页存活） |
| 19 | 鼓轨 kit 列表 | Ctrl+4 → `TAP 512 92`、`TAP 160 92`、`PICK 0`、`PICK 1` 的行 TAP、`PARAM 63` | 同上 | ✅ `TAP miss 512,92`（没有 PRESET 行）；`-> SOUND pick open row=0`；`open=1 row=0 engine=0 highlight=0 rows=46`；`PICK 0` ⇒ `text="ACOUSTIC" current=1 … y=216`；基线 `PARAM track=3 id=63 value=0 min=0 max=45`；`TAP 640,264 page=6 -> SOUND pick kit=1 CMD_SET_PARAM` ⇒ `value=1`；关闭 |
| 20 | 鼓轨键盘 | 再开（`PICK 1` ⇒ `"DEEP" current=1`）→ ↓ ×2 → ENTER | 同上 | ✅ `PARAM track=3 id=63 value=3`；KIT 行 `−` ×3 还原 `value=0` |
| 21 | 池往返 | 开 / `UI MEM` / ESC / `UI MEM` ×5（row 0 与 row 1 交替） | 同上 | ✅ 五轮都是 `before=43064 open=42448 after=43064`（打开差 **616B**；关闭后差 **0B**，判据 ≤ 128） |
| 22 | 未打开时的读 | ESC 后 `UI SOUND PICK 0` | 同上 | ✅ `UI ERR sound (not_built)` |
| 23 | UI 截屏 ×4 | `scripts/screenshot.py`（经 `task-6-check.py`） | `task-6-pick-engine.png`（row 0，ANALOG 高亮）、`task-6-pick-engine-chip-hl.png`（→ ×9 后 FM6 chip 高亮）、`task-6-pick-preset.png`（row 1，预置 3 高亮）、`task-6-pick-kit.png`（鼓轨 kit 列表） | ✅ 像素核对（脚本内，逐点精确比对）：四边 2px 描边同色 `(32,40,48)` 且 ≠ 面板底 `(24,32,32)` / 背景；高亮行有 `UI_PANEL_HI` 条、非高亮行没有；合成轨 chip 行强调色像素 3125（一个高亮 chip），鼓轨面板顶带 0；列表区当前项强调色文字 186 / 259 像素；提示行墨迹右缘 711 / 882 / 582 ≤ 1096（未被面板裁掉）。**人眼读图未做** |
| 24 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-6-selftest-heap.log`（stdout）、`task-6-selftest-heap-transcript.log`（串口） | ✅ rc=0（324s）：run 1 `delta=268`、run 2 `delta=36`（门限 ≤1,024B），收尾 `internal_free=72683B`（≥61,440）；两次 `cases=182/182 identical=182 … result=PASS`。transcript 里的 `task_wdt … IDLE1` 与 Task 4 的 transcript 同形（自检期间 console_repl 占住 CPU 1，既有现象） |
| 25 | 还原核对 | `task-6-check.py` 收尾硬复位 + 自检堆之后再硬复位一次 | `task-6-check.out`、`task-6-restore.log` | ✅ `sel=0 bpm=120`、四轨 `engine=0 preset=0 level=104` 与基线逐字相同；`autosave=1 autosave_hash=FBC5F2C7 autosave_writes=0`；`zoom=0 dirty=0` |

### 偏差与说明

1. **row 0 的列表是 12 个引擎，不是「高亮引擎的预置」**（`rows=12`、`PICK 8` ⇒ `GRAIN`，与派发说明的更正期望一致；主控 R13 的文字写的是预置列表）。
   两种读法互斥，按可核对的设备期望实现；row 1 才是预置列表（chip 预览别的引擎时换成该引擎的预置）。换成 R13 的字面读法只需改
   `page_sound_picker_open()` 里 row 0 的 `rows`/高亮与 `list_select()` 的一条判断。
2. **确认当前项不下发**（`unchanged`）：引擎 = 默认值 + 预置 0、预置 = 整份声音，打开面板后误触 ENTER 不该复位当前声音。R13 未规定这一条。
3. `UI SOUND PICK <n>` 对不在一屏 11 行里的行会**先把列表滚到该行**再报矩形（否则报出的矩形在面板外，TAP 够不着）；高亮再变时列表才回去跟着高亮走。
4. 12 个引擎 chip（11px/字符 + 24、间距 8）合计约 1,041px，比面板内宽 928 长：chip 行横向滚动，高亮 chip 始终留在行内（`→ ×9` 后 FM6 可见）。
   真机的手指横向拖动 chip 行**没有验证**——chip 行不接受拖动，只能靠高亮带动。
5. 池差 608–644B 随开的是 row 0 / row 1 与池历史浮动（三个对象 + 样式/事件簿记）；关后逐轮回到原值。

## 14. Task 6 修复轮 1（评审：R16 预览语义 + 两项 Important + 四项 Minor；固件 `9e537bd`，2026-11-27）

> R16（取代 R13 的措辞）：row 0 = 引擎列表 + chip 行；row 1 = chip 行 + **高亮引擎**的预置列表；chip 高亮 ≠ 当前引擎时列表**只读**
> （点行不执行、回 `-> SOUND pick preview engine=<高亮>`，`current` 恒 0；ENTER 不论最后移的是哪一行都是确认 chip ⇒ `CMD_SET_ENGINE`）。
> 评审 Important：① 预览态同下标的预置被画成/回报为当前项；② 载入用户预置后 `t->preset == 0`，工厂预置 0 被 `unchanged` 守卫拒绝。
> Minor：面板空白处 TAP 回 `(unknown)`；CLICKED 没有「手指移动过不算点击」的门；`PICK_ENGINES` 重复核心常量；`open()` 的快照写在临界区外。
> 本节读数都在 `9e537bd` 上取（boot 自报 `App version:      9e537bd`，无 `-dirty`：代码与 `task-6-check.py` 同一提交）。读数脚本仍是
> `task-6-check.py`（断言 142 条：首轮 109 条全部保留 + 本轮 33 条）。**PNG 人眼未读；手指拖动/点击仍无真机验证。**

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-6-fix1-build-1.log`、`task-6-fix1-build-2.log` | ✅ 两遍 rc=0、编译 0 警告 0 错误；第一遍只有 §5.5 豁免的那对 CMake 提示，第二遍没有 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-6-fix1-knob-test.log`、`task-6-fix1-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed` |
| 3 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-6-fix1-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 4 | 启动日志 | `task-6-check.py` 的两次硬复位 | `task-6-fix1-check.log` 第 29–31 行、第 1511 行起 | ✅ `App version:      9e537bd`；E / abort / backtrace / Guru Meditation 计数 0 |
| 5 | 首轮 109 条回归 | `task-6-check.py` | `task-6-fix1-check.out` | ✅ 全部仍通过（池 43504 → 42860（644B）；往返 ×5 `43064 → 42448 → 43064`；`UI MEM page id=6 … est=8704 delta=7308`；`internal_min=71864`） |
| 6 | 面板空白处 = miss（Minor a） | 开 row 1 → `TAP 640 616`（提示行）、`TAP 269 220`（chip 间隙）→ `UI SOUND FOCUS` | 同上 | ✅ `TAP miss 640,616 page=6`、`TAP miss 269,220 page=6`（返回非零）；焦点槽不变、面板仍打开 |
| 7 | 滑过的松手不算点击（Minor b） | `UI SOUND PICK press 640 360` → `UI SOUND PICK release 700 360`；对照：`press 640 424` → `release 640 424`（当前预置行） | 同上 | ✅ `UI SOUND PICK press x=640 y=360 hit` → `UI SOUND PICK release x=700 y=360 moved`，`ENG SNAP trk k=0 engine=0 preset=5` 不变、面板仍打开；对照 `release x=640 y=424 hit` ⇒ 命中当前行（`unchanged`，关闭） |
| 8 | R16 预览态 | 开 row 1（ANALOG，预置 5）→ `TAP 324 220`（DIGITAL chip）→ `UI SOUND PICK 5` → 行 TAP → `KEY 0x4B`（↓）→ `KEY 0x3D` | 同上、`task-6-fix1-pick-preview.png` | ✅ `-> SOUND pick engine=1 highlight`；`open=1 row=1 engine=0 highlight=1 rows=22`；`n=5 text=" 6  AFRO KEYS" current=0`（与当前预置同下标，**不再** current=1）；像素：列表区强调色文字 0 像素、chip 行 3125；行 TAP ⇒ `TAP 640,424 page=6 -> SOUND pick preview engine=1`，`engine=0 preset=5` 不变、面板仍打开；ENTER ⇒ `engine=1 preset=0`、关闭 |
| 9 | 预览态 chip 环绕 / 页级 ± / 当前项 | 开 row 1（DIGITAL）→ ←、←、→ ×2 → `TAP 1224 132` → 行 0 TAP | 同上 | ✅ `highlight=0 rows=22` → `highlight=11 rows=11` → `highlight=1 rows=22`；`-> SOUND pick preset=21 highlight`；`TAP 640,264 … preset=0 unchanged` |
| 10 | 用户预置后工厂预置 0 可选（Important ②） | `STORE PRESET LIST` → `STORE PRESET SAVE 0` → `STORE PRESET LOAD 0 0` → 开 row 1 → `PICK 0` → 行 TAP → 再开 `PICK 0` → `STORE PRESET ERASE 0` | 同上、`task-6-fix1-user-preset.png`、`task-6-fix1-factory-preset.png` | ✅ `PRESET 0 SAVED DIGITAL DIGITAL 01`、`PRESET 0 LOADED track=0`；`n=0 text=" 1  RHODES" current=0`（用户预置在位）；`TAP 640,264 page=6 -> SOUND pick preset=0 CMD_SET_PRESET`（修复前是 `unchanged`）；再开 `current=1`；页眉 PRESET 名的强调色墨迹 174 → 195 像素、不同 271（`USER 01` → 工厂名）；`PRESET 0 ERASED`、`PRESET 0 EMPTY` |
| 11 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-6-fix1-selftest-heap.log`、`task-6-fix1-selftest-heap-transcript.log` | ✅ rc=0（325s）：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72659B`；两次 `cases=182/182 identical=182 … result=PASS`。transcript 里的 `task_wdt … IDLE1` 与 Task 4 的同形（既有现象） |
| 12 | 还原核对 | `task-6-check.py` 收尾硬复位 + 自检堆之后再硬复位 | `task-6-fix1-check.out`、`task-6-fix1-restore.log` | ✅ `sel=0 bpm=120`、四轨 `0/0/0/104` 与基线逐字相同；`autosave=1 autosave_hash=FBC5F2C7 autosave_writes=0`；用户预置槽 0 回到 EMPTY；自检后复位读数相同 |

说明：`UI SOUND PICK press|release <x> <y>` 是本轮新增的触摸注入（评审 Minor b 的验证路径）：没有它，TAP 只能注入一次松手，
造不出「按下后移动再松手」。它与 CLICKED 回调走同一个 `picker_release()` / 同一个 4px 死区，但**不是**原始指针事件——
真机手指在列表上滑动的手感仍未验证。用户预置检查用了空的槽 0（`STORE PRESET LIST` 的首个 EMPTY），事后擦除。
`ENG SNAP`/`STATE SNAP` 没有 `user` 字段：用户预置在位的证据是 `UI SOUND PICK 0` 的 `current=0` 与页眉名墨迹，`user` 清掉的证据是
其后的 `current=1` 与页眉名墨迹变化。

## 15. Task 6 修复轮 2（复审：R16 的 ENTER 条款 + 两项 Minor；固件 `5eee751`，2026-11-27）

> R16 ENTER 条款的最终规则：ENTER 按**焦点所在的那一行**确认（焦点 = 最后一次 ←/→ 或 ↑/↓ 移过的那一行）。焦点在列表 ⇒ 选中
> 列表高亮行（高亮 = 当前引擎时装载该预置；**预览态下不动作**，面板保持打开）；焦点在 chip 行 ⇒ 确认高亮的 chip ⇒ `CMD_SET_ENGINE`。
> 修复轮 1 把预览态的 ENTER 做成了「不论焦点都确认 chip」，与裁决相反，本轮改回。Minor：`UI SOUND PICK` 只认四种形式（其余用法错误、
> rc 1）；裸 `release` 不拿陈旧按下点比对（`ignored (no press)`）。顺带修了预览态提示行从不重绘（高亮变化只失效 chip 行与列表，
> 提示行画在面板上）。读数脚本 `task-6-check.py`（断言 153 条）。**PNG 人眼未读；手指操作仍无真机验证。**

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-6-fix2-build-1.log`、`task-6-fix2-build-2.log` | ✅ 两遍 rc=0、编译 0 警告 0 错误；第一遍只有 §5.5 豁免的那对 CMake 提示 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-6-fix2-knob-test.log`、`task-6-fix2-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed` |
| 3 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-6-fix2-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 4 | 启动日志 | `task-6-check.py` 的两次硬复位 | `task-6-fix2-check.log` 第 29–31 行、第 1619 行起 | ✅ `App version:      5eee751`；E / abort / backtrace 计数 0 |
| 5 | 之前两轮全部检查回归 | `task-6-check.py` | `task-6-fix2-check.out` | ✅ 仍通过（池 43504 → 42860（644B）；往返 ×5 `43064 → 42448 → 43064`；`est=8704 delta=7308`；`internal_min=71856`） |
| 6 | R16 ENTER（焦点在列表） | 开 row 1 → DIGITAL chip 高亮（预览）→ ↓ → ENTER | 同上 | ✅ `UI SOUND PICK open=1 row=1 engine=0 highlight=1 rows=22`、`ENG SNAP trk k=0 engine=0 preset=5`（不动作、面板保持打开） |
| 7 | R16 ENTER（焦点在 chip 行） | → 一次 → ENTER | 同上 | ✅ `open=1 row=1 engine=0 highlight=2 rows=8`（焦点回 chip 行，高亮 LOFI）→ `trk k=0 engine=2 preset=0`、关闭 |
| 8 | 预览态提示行 | `task-6-fix1-pick-preview.png`（本轮重拍） | 同上 | ✅ 墨迹右缘 1057（≠ 普通 row 1 的 882，≤ 1096 未被裁）：文案换成只读预览 |
| 9 | Minor a：参数形式 | `UI SOUND PICK press 640` / `a b c` / `0 0` / `0 x` | 同上 | ✅ 四种都打印 `用法: UI SOUND …`、不回显行、不动选择器 |
| 10 | Minor b：裸 release | 开面板后直接 `UI SOUND PICK release 640 424`；`press`+`release` 之后再 `release` | 同上 | ✅ `UI SOUND PICK release x=640 y=424 ignored (no press)`（四轨行不变、面板仍打开）；用过即清：`release x=640 y=360 ignored (no press)` |
| 11 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-6-fix2-selftest-heap.log`、`task-6-fix2-selftest-heap-transcript.log` | ✅ rc=0（325s）：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72659B`；两次 `cases=182/182 identical=182 … result=PASS` |
| 12 | 还原核对 | 收尾硬复位 + 自检后再硬复位 | `task-6-fix2-check.out`、`task-6-fix2-restore.log` | ✅ `sel=0`、四轨 `0/0/0/104` 与基线逐字相同；`autosave=1 autosave_hash=FBC5F2C7 autosave_writes=0`；用户预置槽 0 `EMPTY` |

## 16. Task 6 修复轮 3（复审：row 0 回归；固件 `2ac1f3a`，2026-11-27）

> 修复轮 2 把 R16 的预览门写成 `!drum && chip_hl != engine`，没限定 row：row 0 的 ↓/↑/页级 ± 把焦点放到列表并把 `list_hl` 抄进
> `chip_hl`，其后的 ENTER 被当成「预览态 + 列表焦点」拒绝。本轮给预览分支加 `row == 1`；row 0 / 鼓轨的列表焦点 ENTER = 用高亮的那一项。
> **固件 = `2ac1f3a`**（`task-6-fix3-check.log` 第 29 行自报 `App version:      2ac1f3a-dirty`：`-dirty` = 上一轮核对重拍的 3 张 PNG
> （docs），`git diff 2ac1f3a -- firmware scripts third_party tools` 为空）。读数脚本 `task-6-check.py`（断言 161 条）。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-6-fix3-build-1.log`、`task-6-fix3-build-2.log` | ✅ 两遍 rc=0、编译 0 警告 0 错误；第一遍只有 §5.5 豁免的那对 CMake 提示 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-6-fix3-knob-test.log`、`task-6-fix3-sound-test.log` | ✅ `17 passed, 0 failed` / `4 passed, 0 failed` |
| 3 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-6-fix3-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 4 | 启动日志 | `task-6-check.py` 的两次硬复位 | `task-6-fix3-check.log` 第 29–31 行、第 1743 行起 | ✅ E / abort / backtrace 计数 0 |
| 5 | 之前三轮全部检查回归 | `task-6-check.py` | `task-6-fix3-check.out` | ✅ 仍通过（池 43504 → 42860（644B）；往返 ×5 `43064 → 42448 → 43064`；`est=8704 delta=7308`；`internal_min=71808`） |
| 6 | row 0：列表焦点 + ENTER（本轮回归） | 开 row 0 → `KEY 0x4B`（↓）→ `KEY 0x3D`；还原后再开 → `TAP 1136 132`（页级 −）→ ENTER | 同上 | ✅ `open=1 row=0 engine=0 highlight=1 rows=12` → ENTER ⇒ `ENG SNAP trk k=0 engine=1 preset=0`、关闭；还原 `-> SOUND pick engine=0 CMD_SET_ENGINE`；页级 − ⇒ `-> SOUND pick engine=1 highlight` → ENTER ⇒ `engine=1`；还原 |
| 7 | 鼓轨：列表焦点 + ENTER | 再开 → ↓ ×2 → ENTER | 同上 | ✅ `PARAM track=3 id=63 value=3 min=0 max=45`、关闭（上一轮已通过，本轮加关闭断言） |
| 8 | 自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-6-fix3-selftest-heap.log`、`task-6-fix3-selftest-heap-transcript.log` | ✅ rc=0：run 1 `delta=260`、run 2 `delta=44`（门限 ≤1,024B），收尾 `internal_free=72659B`；两次 `cases=182/182 identical=182 … result=PASS` |
| 9 | 还原核对 | 收尾硬复位 + 自检后再硬复位 | `task-6-fix3-check.out`、`task-6-fix3-restore.log` | ✅ `sel=0`、四轨 `0/0/0/104` 与基线逐字相同；`autosave=1 autosave_hash=FBC5F2C7 autosave_writes=0` |

## 17. 5B-1 Task 7：退役五张参数页并同步导航与页表（2026-11-27；固件 `5de0404` + 脚本 `c8305e4`）

> 布局表只剩 SOUND + SAMPLE，EDIT = SOUND、MIX = TOOLS（13 页 / 9 组）。**固件 = `5de0404`、脚本 = `c8305e4`**（提交后构建烧录取证；
> `task-7-boot.log` 第 29 行自报 `App version:      c8305e4-dirty`：`-dirty` = 未提交的 docs，`git diff c8305e4 -- firmware scripts third_party tools`
> 为空）。提交前另做过一遍 `--clean` 全量构建（`task-7-build-clean.log`：1,996 个目标、0 警告；源码与 `5de0404` 相同）。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | 固件代码（构建） | 提交后 `scripts/build.sh` ×2 | `task-7-build-1.log`、`task-7-build-2.log`（全量：`task-7-build-clean.log`） | ✅ 两遍 rc=0、编译 0 警告 0 错误；第一遍只有 §5.5 豁免的那对 CMake 提示 |
| 2 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-7-knob-test.log`、`task-7-sound-test.log` | ✅ `16 passed, 0 failed`（「MOD/SCALE/VOICE/FX/GLOBAL 槽位 = 012 §3」一例随页删除）/ `4 passed, 0 failed` |
| 3 | 工具自测 | `python3 scripts/soak-selftest.py`、`python3 scripts/screenshot.py --selftest`、`python3 scripts/screenshot_matrix.py --selftest` | `task-7-soak-selftest.log`、`task-7-screenshot-selftest.log`、`task-7-matrix-selftest.log` | ✅ rc=0：`结果: PASS` / `selftest OK` / `screenshot_matrix selftest OK`（MATRIX 20 条目 / 13 页） |
| 4 | 固件代码（烧录验证） | `scripts/flash.sh` | `task-7-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 5 | 启动日志 | esptool 硬复位后抓 12s | `task-7-boot.log` 第 28–31 行 | ✅ E / abort / backtrace 计数 0 |
| 6 | 页表（设备） | `UI MEM`、`UI mod` / `scale` / `voice` / `global` / `fx` | `task-7-ui-mem.txt` | ✅ `registered=13`、`sum_est=118784`、`GROUP EDIT sum_est=8704`、`GROUP MIX sum_est=3584`；五个退役页名各回 `UI ERR not_registered <id>`（15 / 16 / 17 / 4 / 5） |
| 7 | 注入场景 | `bash scripts/inject/{nav,params,store,song,sys}.sh` | `task-7-<场景>-stdout.log`；nav / params 另有 `task-7-nav-transcript.log`、`task-7-params-transcript.log` | ✅ 退出码均 0：`nav` **527 / 0**、`params` **356 / 0**、`store` **901 / 0**、`song` **353 / 0**、`sys` **353 / 0**；末行均为 `[PASS] <场景>: 全部断言与像素抽检通过` |
| 8 | 有序对与过渡 | `nav` ⑩ | `task-7-nav-stdout.log`、`task-7-nav-transcript.log` | ✅ 72 组对 / 104 目标页全部 ≥4,096：最紧 SEQ→FILE 6,412B；SEQ→EDIT 11,428、EDIT→SEQ 19,192、MIX→SEQ 23,904、SEQ→MIX 16,076B；`lvgl_stack_free=5116B`；SEQ（STEP + TRANSPORT 全建）→ `UI sound`：`页面构建 page=SOUND(6) 池增量=7160B 预算=8704B 池剩余=11428B 耗时=13ms` |
| 9 | 还原核对 | 场景后 `ENG SNAP` → 还原 → 再读 → 硬复位后再读 | `task-7-after.txt`、`task-7-restore-final.txt`、`task-7-restore.log` | ⚠️ 场景后四轨有残留（见下第 2 条）；按烧录前基线还原后 `sel=0`、四轨 `engine=0 preset=0 level=104 mute=0 len=16 div=2`、轨 0 包络 `20/40/60/70`、`bpm=120` 与基线逐字相同；**鼓轨步位图未能还原** |

偏差与说明：

1. 提交前的工作运行（未提交树）里 `params` 红过两次、`store` 红过一次——证据 `task-7-params-fail1.log`、`task-7-params-fail2.log`、
   `task-7-store-fail.log`。原因与处置（修复都在 `c8305e4`）：
   - 「bank 行缺席 ⇒ bank 带 TAP miss」在仪表盘上不成立：行隐藏时页面区 y=76..120 的带被卡片视口盖住，TAP 落在卡片标题行上
     （回显 `-> SOUND card=0 collapsed=1`，并把 card 0 的折叠位留在了设备上）⇒ 判据改为「带内没有 SAVE / ERASE 按钮色」
     （`bank_row_absent_probes`：params 的 ANALOG / 鼓轨 / GRAIN 与 store ⓰ 同）；
   - FM6 段漏了 `select_track(1)` 之后的 `UI sound`（页签停在 TRACKS，页眉 TAP 全部 miss，一次 TAP 还落在 TRACKS 卡上改了选中轨）⇒ 补回；
   - 参数锁段的前置改经仪表盘时取了 Global 卡的 BPM（全局作用域：`PARAM 0` 读到的是轨道 LEVEL；且 card 0 当时被折叠 ⇒
     `UI SOUND FOCUS 0` 被重指）⇒ 改为先展开全部折叠卡，再取 Envelope 卡的 ATK 槽（轨道作用域，与旧 MOD·ENV chip 的槽 0 同一个参数）；
   - store ⓰ 的 `UI SOUND CHIP` 期望 `chip=0`——仪表盘自 Task 3 起回 `chip=255` 且无逐槽行（console-and-injection.md 已写明）⇒ 对齐。
   fail1 的中止运行里 `STORE BANK ERASE 0` 已执行（FM6 bank 槽 0 被擦；擦前内容未记录——设备当时四轨都是 ANALOG 预置 0 的默认态，
   槽 0 大概率本就为空，但这是推测）。
2. 五条场景跑完后 `ENG SNAP trk`（`task-7-after.txt`）与烧录前不同：轨 0 `level=1 mute=1 len=64 div=8 atk=0 dec=112 sus=0 rel=52`、
   轨 2 `engine=4 preset=5`（基线均为默认值），工程槽 A–D 全 EMPTY，鼓轨步（烧录后、场景前 `on=1` 在 i=0/3/5/6）全部清空。
   哪条场景留下轨 0 / 轨 2 的改动未定位（store 或 sys 的 fixture 段）；与本任务的页表改动无关。已按基线还原四轨的引擎·预置·LEVEL·
   mute·LEN·DIV 与轨 0 的包络四值；**鼓轨步的原始位图没有基线，无法还原**；工程槽擦除前的状态也没有基线。autosave 为 ON，
   还原后的状态由自动存档落盘。
3. 未做：`screenshot_matrix.py` 的设备全量（只跑 `--selftest`；`sound` 的两条目是占位，等 Task 8）、其余五条场景
   （transport / step_edit / tracks / sequence / synth）、`device-selftest.py` / `selftest-heap.py` / `desc-check.py`（简报未列）。

## 18. 5B-1 Task 8：SOUND 仪表盘注入场景与六态矩阵（2026-11-28；脚本 `a19a2f9`）

> 固件未改（仍为 Task 7 的 `5de0404`），因此本任务没有重建或重烧。下表的 PASS/FAIL 计数按 stdout 里的 `[PASS]` / `[FAIL]` 行计，含场景末行总 PASS。所有真机场景都同时保留 tee 的 stdout 与原始串口 transcript。

| # | 门禁项 | 命令 | 证据文件 | 结论 / 读数 |
|---|---|---|---|---|
| 1 | host 单测 | `scripts/knob-test.sh`、`scripts/sound-test.sh` | `task-8-knob-test.log`、`task-8-sound-test.log` | ✅ `16 passed, 0 failed` / `4 passed, 0 failed` |
| 2 | 工具自测 | `python3 scripts/soak-selftest.py`、`python3 scripts/screenshot.py --selftest`、`python3 scripts/screenshot_matrix.py --selftest` | `task-8-soak-selftest.log`、`task-8-screenshot-selftest.log`、`task-8-matrix-selftest.log` | ✅ 均 rc=0；矩阵自测为 **24 条目 / 13 页**，SOUND 六态均为必需状态，`sound/editing` 占位已删 |
| 3 | SOUND 注入场景 | `bash scripts/inject/sound.sh /dev/ttyACM0` | `task-8-sound-stdout.log`、`task-8-sound-transcript.log`、`task-8-sound-{default,scrolled,collapsed,picker-engine,picker-preset,drum}.png` | ✅ **407 PASS / 0 FAIL**；est=8,704，② delta=6,484B；SEQ→SOUND 池增量=7,164B、池剩余=12,852B、build=12ms；最终 `UI MEM` delta=6,576B / build_ms=9ms；10 次 SOUND↔STEP `pool_free 42060→41972`（Δ=−88B，单次最大下降 80B） |
| 4 | 逐槽结构 / 动态标签 | `sound` ③、⑨、⑩、③b | 同上 stdout/transcript；定点 `task-8-affected-probe-{stdout,transcript}.log` | ✅ ANALOG 109、鼓轨 101；12 引擎存在槽数 `109/107/107/108/106/108/108/108/108/108/108/108`；鼓轨 Engine edit 只有 KIT present+locked，16 个 TD 槽可编辑；ITYPE = FX 卡槽 17 / flat 64，改值后 `INS A/B/C/MIX` 正确重解析 |
| 5 | SOUND 六态设备矩阵 | `python3 scripts/screenshot_matrix.py /dev/ttyACM0 --pages sound --out docs/dev/verify/5-pb1-2026-11-26/task-8-sound-matrix` | `task-8-matrix-stdout.log`、`task-8-sound-matrix/matrix-20261128-085657/`（6 PNG、matrix.csv、matrix.log、联系表） | ✅ **6/6 PNG、31/31 锚点、6/6 条目后 LIVEHASH 不变、0 次重试**，起点还原；stdout 共 134 PASS / 0 FAIL |
| 6 | 共享 inject.py 十条回归 | 顺序 `nav → params → store → song → sys → tracks → sequence → synth → step_edit → transport` | `task-8-<场景>-stdout.log`、`task-8-<场景>-transcript.log` | ✅ nav **527/0**、params **363/0**、store **901/0**、song **353/0**、sys **353/0**、tracks **84/0**、sequence **562/0**、synth **794/0**、step_edit **321/0**、transport **288/0** |
| 7 | 全量自检堆泄漏 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-8-selftest-heap.log`、`task-8-selftest-heap-transcript.log` | ✅ 两次均 182/182；run 1 Δ=260B、run 2 Δ=44B（≤1,024B）；`internal_min=75,136B`（≥61,440B），收尾 free=75,987B |
| 8 | 基线还原 + 硬复位 | `task-8-baseline.py verify … --reset` | `task-8-baseline.txt`、`task-8-restore-live-stdout.log`、`task-8-restore.txt`、`task-8-restore-transcript.log` | ⚠️ **294/295 项相同，唯一差异是 LIVEHASH `30117351 → 351646DB`**；详见下方开放项，未记为 RESTORE OK |

### 失败运行与断言出处

- `task-8-sound-import-fail.log`：继承代码在 import 时先使用 `SOUND_SCOPE_*` 后定义，且卡槽数断言对 int 调 `len()`；分别改为先定义常量、直接比较槽数列表。`_unused_tail` 已删除。
- `task-8-sound-fail1-*`：① 150° 环点落在数值弧上。`page_sound.c:415-418` 先画整圈 UI_PANEL_HI、再从 135° 叠值弧，故探针改到 **135° 左下起点 `(cx−31,cy+31)`**，不是改期望色；fail PNG 离线复判两点容差内通过。② `ENG TRACK` 直写 core（`console.c:830`），立刻重开选择器会拍到旧 sound 投影，下一拍关闭（`page_sound_picker.c:646-650`），ESC 随后按 `input.c:294-300` 回 PLAY；场景在该诊断命令后等 0.1s。隔离 RED 见 `task-8-esc-engine-probe-*`，GREEN 见 `task-8-esc-engine-fixed-*`。
- `task-8-sound-fail2-*`：成员镜像漏 `TD(L_P_ITYPE)`（`sound_layout.c:46`），补齐后鼓轨可编辑槽恰 16；Engine edit 在鼓轨只有 P_E0/KIT 的描述符，E1..E7 与 FTYPE 均 absent；ITYPE 是 FX 槽 17（base 47 + 17 = **flat 64**），不是继承稿的 65。定点复核 `task-8-affected-probe-*` 全过。池判据未放宽：先预热并以相邻两轮 ≤128B 证明起点稳定，再量同样 10 轮；独立 `task-8-pool-probe-*` / `task-8-pool-stable-probe-*` 分别为 −56B / −12B。
- `task-8-sound-fail3-*`：预热起点离群造成首末 −140B（仅超 12B），所以加上述起点稳定条件；最终 clean run 为 −88B，阈值仍 128B。
- `task-8-matrix-fail1-stdout.log`：`ensure_engine/select_track` 会落 TRACKS，`sound_reset` 在错误页发 `UI SOUND`；在 reset 入口显式 `UI sound` 后六态全过。
- `task-8-step_edit-fail{1,2,3}-*`：发声探针的音频工作可见性是异步的。隔离按键 `task-8-voice-probe-*` 首次读数 voices=2；发声观察窗改为 1.0s。ENTER no-op 的 21 行引擎快照仍逐字比较，只排除异步 `voices=` 字段（`page_step.c:1118` 已证明合成轨直接返回），其它引擎/预置/步/lock 字段不放宽。
- 首轮六图与 `sheet-edit.png` 的视觉检查曾发现页眉占位框：`page_params.c` 的 `·`/`—` 不在 Montserrat 字形表，显示为 `SOUND □ TRACK`、`□ NO SOUND`；因此「全部视觉干净」不成立。fix round 1 已改为 ASCII `-`，并以 `task-8-fix1-sound-matrix/matrix-20261128-161533/` 重新抓取六图逐张复核：占位框消失，卡片、滚动、折叠、选择器与鼓轨锁定态正常。

### 开放项（不计为已闭合）

基线工具只记录四轨引擎/参数、全局、kit、settings、槽清单与整份 LIVEHASH，没有记录步表/锁/section/arrangement 的可恢复内容。旧十条场景按自身设计含有诊断性工程操作；全部可恢复字段已逐项写回并落盘，但整份工程的 hash 未回到入口。入口四个工程槽又全为 EMPTY，没有可加载的入口备份。按本任务「不再清 pattern/lane、不再做 STORE 写/擦除」约束，无法从现有数据重建那部分工程，故硬复位后仍只有上述一项 `RESTORE MISMATCH`。没有再用 NEW/CLEAR/ERASE 掩盖差异；`task-8-restore-pre-reset.txt` 与 `task-8-restore.txt` 都保留完整 295 项对照。后续必须从包含入口工程的备份恢复，或扩充基线为完整可重放的工程数据后重做还原。

### 18a. LIVEHASH 漂移根因与 sequence 修复（2026-11-28；`4c1fcf8`）

`LIVEHASH` 的输入不是 arrangement/section 对象本身。`sloop_store_live_hash()`（`store_tab5.c:517-526`）调用上游 `proj_capture()`（`third_party/sloop-fm1/firmware/src/project.c:460-483`），覆盖：

- 32 个项目全局 `song.g[0..31]`、选中轨 `song.sel`，以及打包进保留字段的 TEXTURE SRC/WHEN/DUCK/RATE/LVL/TONE/MOVE；
- 四轨全部 `P_COUNT` 参数、引擎请求、预置、合成步或 16-lane 鼓步、每步 micro/fill、参数锁；
- 每 lane 的 drum TUNE/DECAY（`drum_tw_put`）。FNV-1a 覆盖 `project_t` 除末尾 `sum` 外的全部字节。

逐场景入口/出口测量（stdout + transcript 均以 `task-8-hash-*` 入库）：

| 场景 | 入口 | 出口 | 结论 |
|---|---|---|---|
| `sound` | `351646DB` | `351646DB` | ✅ 稳定；本轮 404 PASS / 0 FAIL |
| `sequence`（修复前） | `351646DB` | `21124915` | ❌ 可复现漂移；562 PASS / 0 FAIL 但工程被改写 |

`task-8-hash-sequence-fields.txt` 的 295 项对照进一步给出 8 项差异：`p0.0 127→104`、`p0.17 14→0`、`p0.23 1→0`、`p0.37 2→0`、`rec_tempo 0→1`（另反映到 `state.param`、`trk0` 与 hash）。直接原因是 `run_sequence` ⓪/⑨ 的 `SEQ PATTERN CLEAR 0..3` 清空四轨 steps/locks/micro/fill，以及 `seq_set_track_param` 的归一写，均没有外层工程还原括号。修复前的原始步/锁/micro/fill 因此不可由参数表恢复。

修复 `4c1fcf8`：⓪ 先用既有 `session_once` 保存歌曲/REC，再用 `proj_snapshot_open` 把整份工作工程存入诊断槽 D；内部只用 A–C，D 的 ready 位明确视为入口快照；⑨ `proj_snapshot_close` 执行 LOAD 并断言 `LIVEHASH == 入口`，再还原歌曲顺序和 REC 配置。连续两轮（`task-8-hash-sequence-fix{1,2}-*`）均为：

- 入口 `21124915` → 出口 `21124915`；每轮 **567 PASS / 0 FAIL**；
- `⑨ 收口后 LIVEHASH 21124915 == 入口 21124915（工作工程逐位还原）`；
- 歌曲顺序与 REC 配置也逐项回入口；入口 D 为空时擦快照，非空则保留。

随后重新写回全部可恢复基线字段并等 autosave/settings 落盘（`task-8-final-restore-live-*`），再硬复位（`task-8-final-restore-{stdout,transcript}.log`）：**294/295 项仍相同，唯一差异仍是 `30117351 → 351646DB`**。`task-8-restore.txt` 已用这次最终对照更新。现在可以确定的永久差异是修复前 `sequence` 清掉的原始 pattern 内容（steps / locks / micro / fill）；没有入口备份，不能伪造或清空更多内容来制造一致。新修复保证后续 `sequence` 不再扩大这类损失。

### 18b. Task 8 fix round 1：快照安全、独立 SOUND 判据与全场景 hash 门禁（2026-11-28）

代码提交：`f5b4353`（脚本快照/SOUND）、`dae4b75`（固件页眉）、`197bdaf`（params 入口恢复）、`7223a5c`（tracks 选轨）、`ad34c63`（银行读取重试）。

- **Critical #1**：`proj_snapshot_close` 在 LOAD 后 hash 不等时记 FAIL 并抛 `InjectError`，不再擦 D；消息明确「工作工程已改变、良好快照保留、`STORE PROJ LOAD <slot>` 可恢复」。`run_sequence` 包上异常清理：先停走带，D 仍是入口快照才 LOAD 并核 hash，歌曲/REC/autosave 各自尽力恢复；未验证步骤记 FAIL，原异常继续上抛；`ScenarioRestart` 明确跳过清理。`task-8-fix1-snapshot-selftest.log` 用 fake Session 证明「不符不擦/保留快照」与「原异常上抛 + restart 不清理」。
- **Important #2**：12 引擎存在槽数钉完整元组 `(109,107,107,108,106,108,108,108,108,108,108,108)`；`task-8-fix1-sound-stdout.log` 实得完全一致。
- **Important #3**：R12 在折叠最后一张展开卡之前读 flat，折叠后与跨拍读数都须等于该值；实得 flat=102（卡 12 槽 0）一致，锁定/值不变断言仍通过。
- **Important #4**：SOUND 的 `try/finally` 前移到第一次选轨/设置写入之前；记录并恢复入口 collapsed mask，ZOOM 与选轨在早退也恢复。clean run 回显 `collapsed=0x0000（入口 0x0000）`。
- **Minor #5**：`sound.sh` / scripts.md 改为「无破坏性工程/银行 STORE 操作」，不再把 ZOOM 的 `STORE SETTINGS SET` 说成零写；sequence docstring 与 console-and-injection 的十一场景清单同步。
- **Minor #6**：`page_params.c:521-535` 的 `·`/`—` 全部改 ASCII `-`；第二遍构建 0 警告、3× Hash verified、启动无 fatal/abort/backtrace。新矩阵 6/6、31/31 锚点全中，像素坐标没有因文字变化而调整；新图目检无占位框。

全场景结果（PASS 含末行总 PASS；原始 transcript 与 stdout 同包；每个 stdout 末尾有入口/出口 hash）：

| 场景 | PASS / FAIL | 首次入口 → 出口 | 修复/复跑后的稳定证明 |
|---|---:|---|---|
| sound | 404 / 0 | 351646DB → 351646DB | 不变；新增四项判据均过 |
| nav | 527 / 0 | 351646DB → 351646DB | 不变 |
| params | 369 / 0 | 修复前 351646DB → 4E898759 | 修复后 **4E898759 → 4E898759**；轨 0 全 71 参数/引擎/预置/选轨回入口 |
| store | 902 / 0 | 4E898759 → B17A591C（既有归一） | 复跑 `store-2`：**B17A591C → B17A591C** |
| song | 353 / 0 | B17A591C → B17A591C | 不变 |
| sys | 353 / 0 | B17A591C → B17A591C | 不变 |
| tracks | 87 / 0 | 修复前 B17A591C → 216B5ABA | 修复后 **216B5ABA → 216B5ABA**；选轨回最外层入口 |
| sequence ×2 | 各 567 / 0 | 两轮均 **216B5ABA → 216B5ABA** | 快照 LOAD + hash、歌曲/REC 均回入口 |
| synth | 794 / 0 | 216B5ABA → 769D9D1C（既有归一） | 银行重试后 `synth-2`：**769D9D1C → 769D9D1C**；两次失败证据保留（USB 日志交错），第三次完整 PASS |
| step_edit | 319 / 0 | 769D9D1C → 80F8C6F0（既有归一） | 复跑 `step_edit-2`：**80F8C6F0 → 80F8C6F0** |
| transport | 288 / 0 | 80F8C6F0 → 4114C0AC（既有归一） | 复跑 `transport-2`：**4114C0AC → 4114C0AC** |

补充修复的根因证据：`task-8-fix1-params-fields.txt` 显示 ATK/DEC/SUS/REL 残留；`tracks` 的局部入口在点卡 2 之后才拍，选轨因此残留；`synth-2-fail{1,2}-*` 记录 USB host 日志插入 256-hex 行，银行读取改为有界三次重试，不放宽长度/字节断言。归一化场景首次改变工程的情况按上表逐项保留，没有伪装成首次也不变。

矩阵 `task-8-fix1-sound-matrix/matrix-20261128-161533/` 六条目的 LIVEHASH 均为 **4114C0AC**。`selftest-heap` 两次均 182/182，run 1/2 Δ260/44B，`internal_min=75,136B`，收尾 75,987B。最终 `task-8-restore.txt` 是**不修改当前工程来迎合旧 hash**的硬复位对照：当前 `4114C0AC`，与原基线有 38 项字段差异（原 `30117351`；其中原始 pattern 已永久丢失）。本轮证明的是修复/归一后各场景的入口/出口稳定，而不是恢复原工程。

其它仍无异常清理的快照使用者已登记 **DI-66**：`transport.pattern`、`step_edit.erase`、`params.tools`。按评审要求未在本轮统一重构；它们正常路径的 LIVEHASH 断言不受影响。

### 18c. Task 8 fix round 2：params 非零入口选轨与完整恢复（2026-11-28；脚本 `a2dd6c7`）

根因是 `sound_read_vector` 通过 `PARAM <id>` 读取**当前选中轨**，原入口却未先选轨 0；`entry_sel != 0` 时会把入口轨的向量误记为轨 0，并在收口写到错误轨道。修复在每次读取前显式选择目标轨，逐轨拍下三条合成轨的引擎/预置/71 参数及 50 个全局；快照结束后重选轨 0 再做页眉归一，收尾恢复全部快照和入口选轨。`ENG SNAP` 的轨号查询不改变选中轨，不能替代这一步。

| 回归 | PASS / FAIL | LIVEHASH | 轨 0 参数向量 | 证据 |
|---|---:|---|---|---|
| track-0 clean run | 389 / 0 | `0DB3A0B8 → 0DB3A0B8` | 三轨与全局均逐项恢复 | `task-8-fix2-params-track0-4-{stdout,transcript}.log`、`task-8-fix2-params-track0-4-{entry,exit}-transcript.log` |
| track-1 run 1 | 389 / 0 | `F07F2945 → F07F2945` | 入口/出口 71 项完全相等 | `task-8-fix2-params-track1-1-{before,after}.json`、`task-8-fix2-params-track1-1-{stdout,transcript}.log` 及 before/after 串口日志 |
| track-1 run 2 | 389 / 0 | `F07F2945 → F07F2945` | 入口/出口 71 项完全相等 | 同上，前缀为 `task-8-fix2-params-track1-2-` |

track-1 的判别性前置值：轨 0 的 LEVEL/ATK/DEC/SUS/REL = `55/11/22/33/44`，轨 1 = `66/21/32/43/54`，入口 `selected_track=1`。每轮读轨 0 后都重选轨 1，保证第二轮入口一致。两轮后按 `task-8-fix2-baseline.txt` 写回这十项并回选轨 0；`task-8-fix2-restore.txt` 为 **RESTORE OK（295/295，差异 0）**，LIVEHASH 回到 `0DB3A0B8`。三张通过运行的 SOUND PNG 均已视觉复看，无占位框、错位或脏区。

开发失败证据未覆盖：`task-8-fix2-params-track0-stdout.log` 是仅轨 0 恢复时的 0 断言失败但 hash 漂移（`4114C0AC → 58DFFDEF`）；`-2` / `-3` 保留三轨快照后误停轨 2 造成的七项页眉失败，对应原始 transcript 同包。最终修复来自这些记录，不是放宽断言。

离线复核：`python3 -m py_compile scripts/inject/inject.py scripts/inject/snapshot-selftest.py` 退出码 0；`task-8-fix2-{snapshot,soak,matrix}-selftest.log` 均 PASS。保留的 `scripts/inject/snapshot-selftest.py` 无设备可重跑，明确覆盖 hash-mismatch/no-erase/recovery-message、`ScenarioRestart` 清理排除，以及普通 `InjectError` 清理后原样上抛。固件文件未改，本轮不重建、不重烧。


## 19. Task 9 leg A：最终源码的主机侧门禁（2026-11-28；基线 `51d687a`）

本段只覆盖 Task 9 Step 1 的 1–3 项；固件、脚本与第三方源码均为 `51d687a`，未烧录、未访问设备串口。文档清理与这些日志在同一证据提交中；设备段与阶段收口结论留待后续 leg。

| 门禁 | 命令 | 日志 | 结果 |
|---|---|---|---|
| 构建 ×3 | `scripts/build.sh --clean && scripts/build.sh && scripts/build.sh` | `task-9-build.log` | ✅ rc=0；首次 clean configure 只有 AGENTS §5.5 豁免的 CMake/kconfig 提示对；第二、三遍均 **0 warnings** |
| DSP / golden | `scripts/host-test.sh` | `task-9-host-test.log` | ✅ rc=0；182 golden renders（0 changed / 0 gone）、0 health failures、0 voice/routing failures、0 CPU entries over budget；`ALL HOST TESTS PASSED (subset)` |
| 状态层 | `scripts/state-test.sh` | `task-9-state-test.log` | ✅ rc=0；46 passed / 0 failed |
| UI 纯函数 / 页表 | `scripts/knob-test.sh` | `task-9-knob-test.log` | ✅ rc=0；16 passed / 0 failed |
| Sound 布局 | `scripts/sound-test.sh` | `task-9-sound-test.log` | ✅ rc=0；4 passed / 0 failed |
| 音序 / persist | `scripts/seq-test.sh` | `task-9-seq-test.log` | ✅ rc=0；617 条 `ok`，`SEQ HOST TEST PASSED (0 failures)`、`persist-host: PASS (0 failures)` |
| DSYN / FM6 / 采样 | `scripts/synth-test.sh` | `task-9-synth-test.log` | ✅ rc=0；246 条 `ok`，`SYNTH HOST TEST PASSED` |
| 编辑器协议 | `scripts/editor-proto-test.sh` | `task-9-editor-proto-test.log` | ✅ rc=0；6/6 |
| USB-MIDI 流 | `scripts/usb-fm1-test.sh` | `task-9-usb-fm1-test.log` | ✅ rc=0；5/5 |
| soak 工具 | `python3 scripts/soak-selftest.py` | `task-9-soak-selftest.log` | ✅ rc=0；`结果: PASS` |
| 截屏工具 | `python3 scripts/screenshot.py --selftest` | `task-9-screenshot-selftest.log` | ✅ rc=0；`selftest OK` |
| 截屏矩阵 | `python3 scripts/screenshot_matrix.py --selftest` | `task-9-matrix-selftest.log` | ✅ rc=0；24 条目 / 13 页，`screenshot_matrix selftest OK` |
| FM-1 冒烟判定 | `python3 scripts/fm1-smoke.py --selftest` | `task-9-fm1-smoke-selftest.log` | ✅ rc=0；14 组样例，`FM1 SMOKE SELFTEST OK`（不是设备冒烟） |
| 快照异常路径 | `python3 scripts/inject/snapshot-selftest.py` | `task-9-snapshot-selftest.log` | ✅ rc=0；3/3，`snapshot selftest OK` |

文档遗留同步：DI-54 的现行引用指向 Task 6 的 `page_sound_picker.c`，012 的按名浏览行补 Task 8 `sound.sh` 设备证据；ADR-008 的预览 ENTER 描述核对为 R16 的按焦点分流语义；门禁参考补 settings 显式还原要求；DI-66 核对保留；新增 DI-67 记录 SOUND 静态文本缓冲的同步软件绘制前提。`scripts/inject/snapshot-selftest.py` 已在 scripts 参考中，无需重复添加。


## 20. Task 9 leg B1：设备矩阵（2026-11-28；固件 `65f3e65`，中止于 `store` 漂移）

FM-1 全程未接；boot 后 `FM1 STATUS link=none`。构建、烧录、启动与两项自检门禁通过，但 `store` 场景虽然自身 rc=0 / 902 PASS / 0 FAIL，外层入口/出口 `STORE LIVEHASH` 探针发现漂移。按任务停止规则，在 `store` 完成后立即停止；未重跑，后续场景、截屏矩阵与 soak 均未开始。串口已关闭。设备留在 `store` 出口工程态 `B17A591C`（该场景收尾已恢复 AUTO ON）；未再做写入或掩盖漂移。

| 门禁 | 命令 | 证据文件 | 结果 / 读数 |
|---|---|---|---|
| 当前提交构建 | `scripts/build.sh` | `task-9-device-build.log` | ✅ rc=0，`App "sloop_fm1_tab5" version: 65f3e65`；仅有版本变化触发 configure 时的 §5.5 豁免提示对 |
| 烧录 | `scripts/flash.sh /dev/ttyACM0` | `task-9-flash.log` | ✅ rc=0，`Hash of data verified` ×3 |
| 启动 / FM-1 未接 | esptool 硬复位 + console `FM1 STATUS` | `task-9-boot.log`、`task-9-boot-stdout.log` | ✅ `App version: 65f3e65`；fatal/abort/backtrace 0；`link=none` |
| 全量设备自检 | `python3 scripts/device-selftest.py /dev/ttyACM0 --output …task-9-device-selftest-transcript.log` | `task-9-device-selftest.log`、`task-9-device-selftest-transcript.log` | ✅ rc=0，182/182 identical、mismatch=0、health_fail=0、deterministic=1、result=PASS；一次既有 task-watchdog 报告（DI-64），未重启 |
| 连续两次自检堆门禁 | `python3 scripts/selftest-heap.py /dev/ttyACM0` | `task-9-selftest-heap.log`、`task-9-selftest-heap-transcript.log` | ✅ rc=0；两次均 182/182；run 1/2 Δ=260/36B；`internal_min=75,400B`（≥61,440），收尾 internal_free=76,551B |
| 场景 1 `sound` | `bash scripts/inject/sound.sh /dev/ttyACM0` + 外层 LIVEHASH | `task-9-sound-{stdout,transcript,entry-transcript,exit-transcript}.log` | ✅ rc=0，403 PASS / 0 FAIL；hash 不变 |
| 场景 2 `nav` | `bash scripts/inject/nav.sh /dev/ttyACM0` + 外层 LIVEHASH | `task-9-nav-{stdout,transcript,entry-transcript,exit-transcript}.log` | ✅ rc=0，527 PASS / 0 FAIL；hash 不变 |
| 场景 3 `params` | `bash scripts/inject/params.sh /dev/ttyACM0` + 外层 LIVEHASH | `task-9-params-{stdout,transcript,entry-transcript,exit-transcript}.log` | ✅ rc=0，389 PASS / 0 FAIL；hash 不变 |
| 场景 4 `store` | `bash scripts/inject/store.sh /dev/ttyACM0` + 外层 LIVEHASH | `task-9-store-{stdout,transcript,entry-transcript,exit-transcript}.log`、`task-9-livehashes.log` | ❌ **leg 门禁失败**：场景 rc=0、902 PASS / 0 FAIL，但 LIVEHASH `5FB1EDD1 → B17A591C`；随即停止 |
| 其余七场景 | 未运行 | — | NOT RUN：`song` / `sys` / `tracks` / `sequence` / `synth` / `step_edit` / `transport` |
| 全量截屏矩阵 | 未运行 | — | NOT RUN：没有 24 条目、锚点或联系表结论，也未晋升本 leg 的矩阵 PNG |
| soak | 未运行 | — | NOT RUN（本 leg 本就排除） |

### 场景入口 / 出口 LIVEHASH

外层探针在每个场景前后各发一次只读 `STORE LIVEHASH`，同时核对后一场景入口等于前一场景出口；原始回显在各 `*-entry-transcript.log` / `*-exit-transcript.log`，汇总与停止注记在 `task-9-livehashes.log`。

| 场景 | PASS / FAIL | 入口 LIVEHASH | 出口 LIVEHASH | 结论 |
|---|---:|---|---|---|
| sound | 403 / 0 | `5FB1EDD1` | `5FB1EDD1` | ✅ 不变 |
| nav | 527 / 0 | `5FB1EDD1` | `5FB1EDD1` | ✅ 不变 |
| params | 389 / 0 | `5FB1EDD1` | `5FB1EDD1` | ✅ 不变 |
| store | 902 / 0 | `5FB1EDD1` | `B17A591C` | ❌ 漂移；停止，不重跑 |

本段不构成 Task 9 通过结论。后续须先裁定/修复 `store` 的入口还原缺口，再从未完成的设备矩阵继续；不得把本次部分证据记为全矩阵通过。


## 21. Task 9 leg B1 完成段（2026-11-29；固件 `65f3e65`，FM-1 未接）

§20 的首次停止与后续诊断均保留。本段是修复后的最终场景与截屏矩阵结果。**所有十一场景最终均 0 FAIL，但 nav/store 仍有本节末尾披露的状态相关 LIVEHASH 漂移；因此不声称「入口工程逐位不变」全绿。** 烧录、启动、182 自检与堆门禁沿用 §20 的有效读数；最终段另在 `task-9-final-reset.log` 证明硬复位后 `App version=65f3e65` 且 LIVEHASH 保持 `505A56B0`。

| 门禁 | 命令 / 范围 | 证据 | 结果 / 读数 |
|---|---|---|---|
| 十一注入场景 | 顺序 `sound → nav → params → store → song → sys → tracks → sequence → synth → step_edit → transport`；每条前后 `STORE LIVEHASH` | `task-9-<场景>-stdout.log` / `-transcript.log`、`-entry-transcript.log` / `-exit-transcript.log`、`task-9-livehashes.log` | 全部 rc=0（transport 为定点修复后复跑）、**0 FAIL**；逐条计数/hash 见下表；nav/store 有残余漂移 |
| store 当前态复核 | 场景运行前后各拍 295 字段 + 192 合成步 + 16 鼓 lane + 锁表 | `task-9-store-current-{before,after}.txt`、`task-9-store-steps-{before,after}.json`、`task-9-store-current-run2-*` | 连续两次均 931/0、`F31869BB → F31869BB`；其中一轮全字段/步/锁差异均 0。未为当前无差异的状态再改代码 |
| 全量截屏矩阵 | `python3 scripts/screenshot_matrix.py /dev/ttyACM0` | `task-9-matrix-fix1-stdout.log`、`task-9-matrix-fix1-transcript.log`、`task-9-matrix-fix1.csv`、十张 `task-9-sheet-*.png`、六张 `task-9-sound-*.png` | ✅ rc=0；24/24 条目 / 13 页、**123/123 锚点**、10 联系表、0 重试、0 FAIL；起点/终点 LIVEHASH `F31869BB`、AUTO ON |
| 矩阵首轮失败 | 同命令，未修脚本时 | `task-9-matrix-stdout.log`、`task-9-matrix-transcript.log`、`task-9-{dsyn-default,fm6p-engine,transport-default}-fail.png` | ❌ 24 图齐全但 4 锚点失败（DSYN ×1 / FM6P ×2 / TRANSPORT ×1）；保留为修复前证据，未静默重跑 |
| 视觉复核 | 读取全部六张 Sound PNG、sheet-all / synth / seq 与修复后三张页面 | 本节列出的 PNG | 无花屏、错位、缺色、占位框或残留脏区；选择器/折叠/鼓轨状态与预期一致。首轮三张 `-fail` 图也保留供对照 |

### 最终场景表

| 场景 | PASS / FAIL | 入口 LIVEHASH | 出口 LIVEHASH | 备注 |
|---|---:|---|---|---|
| sound | 416 / 0 | `F31869BB` | `F31869BB` | 不变 |
| nav | 527 / 0 | `F31869BB` | `1CF836D9` | **残余漂移** |
| params | 396 / 0 | `1CF836D9` | `1CF836D9` | 不变；包含 71a93f1 的前段步/选轨修复 |
| store | 931 / 0 | `1CF836D9` | `F31869BB` | **残余漂移**；见下方范围说明 |
| song | 353 / 0 | `F31869BB` | `F31869BB` | 不变 |
| sys | 354 / 0 | `F31869BB` | `F31869BB` | 不变；5278e85 恢复入口 BPM |
| tracks | 87 / 0 | `F31869BB` | `F31869BB` | 不变 |
| sequence | 567 / 0 | `F31869BB` | `F31869BB` | 不变；Task 8 的快照恢复仍有效 |
| synth | 814 / 0 | `F31869BB` | `F31869BB` | 不变；184a7ac 恢复轨 0/1 |
| step_edit | 330 / 0 | `F31869BB` | `F31869BB` | 不变；213fe11 的全场景快照覆盖②a–②g |
| transport | 317 / 0 | `F31869BB` | `F31869BB` | 定点修复后复跑；首次全pass的 rc=1 证据保留在 fix4/fix5 日志 |

### 本段修复与双跑证明

| 提交 | 根因 → 修复 | 证明 |
|---|---|---|
| `7f2a4b8` | store 复位/归一化在入口捕获之外 → 入口拍三轨引擎/预置、参数、全局、选轨，末尾恢复 | 非归一化 `693042AB` 两轮均 931/0、入口=出口；run 1 的 295 字段差异 0 |
| `5278e85` | sys 自检段留下 BPM=120 → AUTO ON 前恢复入口 BPM | `693042AB` 两轮 355/0、354/0，均入口=出口；run 1 字段差异 0 |
| `184a7ac` | synth 固定留下 ANALOG/轨 0 → 恢复入口轨 0/1 与选轨 | 两轮均 813/0、`693042AB` 不变；run 1 字段差异 0 |
| `213fe11` | step_edit 的②g 快照漏掉②a–②f → 同一快照扩到全场景，并恢复轨 0 字段/选轨 | 最终 `B0FCDCEB` 两轮均 332/0、入口=出口；字段-only 失败证据保留 |
| `71a93f1` | params 前段步编辑在 param_tools 快照之外；param_tools 快照与 D 的选轨还可能不同 → 外层入口括号 + 内部快照前选轨 0 | 非归一化 `505A56B0` 两轮均 396/0、入口=出口；控制运行另含全部步/lane/锁对照 |
| `90d303a` + `d0f0eb5` | transport 快照太窄；中途恢复还漏测试步与采样相位 → 全场景快照 + 中途恢复测试步 + 条件等待播放头 | 最终 `F31869BB` 两轮均 317/0、入口=出口；没有放宽播放头或编辑断言 |
| `c698906` | sound ⑧ 未建立轨 1=ANALOG 前提 → 非 ANALOG 时归一，并在既有 finally 恢复轨 1 | `505A56B0` 两次 0 FAIL、入口=出口；修复后 ⑧ 为 109 槽 |
| `673d496` | 矩阵继承前场景的 UI 局部态 → 首次写入前 guarded reset，复位前后 LIVEHASH 不同即拒绝 | selftest PASS；设备复跑 24 条目、123 锚点全过、0 重试 |

### 残余漂移与范围

- `nav`：本轮 `F31869BB → 1CF836D9`，rc=0 / 527/0；`store` 随后 `1CF836D9 → F31869BB`，rc=0 / 931/0。它们体现的是**当前工作工程内容**的残余状态依赖，不只是存储槽清单。没有凭本轮 hash 推断具体字段；各场景的 stdout 与原始回显均保留。
- store 的当前态双跑（`F31869BB` 稳定）与本轮从 `1CF836D9` 的漂移必须同时阅读，不能只引用前者。295 字段工具不含全部步/锁内容，后续修复须用全内容快照/对照；store ① 擦四个工程槽本身仍是诊断面的既有破坏范围，不计入 LIVEHASH 字段归因。
- 没有为上述残余再扩大本轮修复范围，也没有通过重跑隐藏它们。soak、FM-1 已接条件的冒烟/自检，以及最终收口报告留后续 leg。
