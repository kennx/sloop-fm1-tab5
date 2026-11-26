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
