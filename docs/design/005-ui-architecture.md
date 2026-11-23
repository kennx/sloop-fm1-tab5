# ADR-005: 独立模式 UI 架构 — LVGL 原生 1280×720 多屏 + AppState 状态层

> 状态：已接受（阶段 3，D3/D2 形式化）｜ 日期：2026-11-20
> 关联：`docs/superpowers/specs/2026-11-20-phase3-mvp-ui-design.md`

## 背景

独立模式需在 5″ 1280×720 触屏上呈现 SLOOP 的层式操作模型。上游 UI 为 240×240 单色（ui.c 等 11 文件），无法直接复用；引擎（sloop_core）目前只写不读。

## 选项

| 选项 | 评估 |
|---|---|
| **A. LVGL 原生 1280×720 多屏 + AppState 状态层（采纳）** | LVGL/esp_lvgl_port 已在（阶段 1）；原生分辨率清晰、触控目标尺寸合理；状态层纯 C 可 host 单测，阶段 5 远程会话可替换执行端 |
| B. 单 canvas 自绘 | 控件/主题/滚动全自写，成本高，无收益 |
| C. 240×240 放大兼容 | 模糊、浪费 5″ 屏、触控目标过小；且仍需重排层页 |

## 决策

A。UI 组件 `sloop_ui`（LVGL 四屏 + 页签 + InputManager）；状态组件 `sloop_state`（AppState + EditCmd，无 LVGL 依赖）；引擎补只读快照与全量步读写接口（spec §3.1）。渲染 = AppState 副本的纯函数式刷新（脏检查）。物理触摸经 LVGL 控件回调发 EditCmd；注入 `TAP` 走各屏 hit-test 表 → 同一 EditCmd（EditCmd 层等价，非原始指针事件）。

## 落地状态（2026-11-20，阶段 3 Task 4 / Task 5）

`sloop_ui` 组件已落地骨架：单容器屏 + 顶部页签 `PLAY|TRACKS|STEP|TRANSPORT`（页面注册表
`ui_page_t`，页对象首次访问时懒建、切页只改显隐，PLAY 启动即建——ADR-008）、30ms 定时器（`state_sync_from_engine` + 逐页脏检查）、`sloop_ui_switch_page()` 与
console `UI <page>`。全部 LVGL 调用走 `bsp_display_lock`（esp_lvgl_port 递归锁，页签回调/定时器已在
LVGL 任务上下文）；UI 启动失败回退 bring-up 测试图。idle 内部堆门禁实测 ≈120.0KB（≥60KB）。

四页均已落地（Task 4：PLAY/TRANSPORT；Task 5：TRACKS/STEP）：

- **TRACKS**：四轨卡片横排，轨主题色 outline 描边（选中 6px + 亮底，未选中 3px），卡内
  `TRACK n / ENG k PRE p / LEVEL v / MUTE x SOLO y` + 电平条；点击卡发 `CMD_SELECT_TRACK`
  （`cmd_select_track` 同时写 `selected_track` 与 `focus_track`）。
- **STEP**：4 行 × 步窗网格（列数 = `window_len` 1..16，列 c = 引擎绝对步 `window_start+c`；
  P4-0 Task 5），行=轨色；格「亮」=`state_step_is_on()`（n>0 且 time==NOTE，与状态层
  切换共用同一判定）；列号/顶栏 `STEP`/`WIN a..b` 用绝对步号，窗口外的格/列号隐藏（TAP 命不中）；
  播放头把绝对 `step_pos` 映射进窗口（窗口外不显示）；绝对步 ≥ 本轨 LEN 的格暗显。点格
  `state_set_focus()` + `CMD_TOGGLE_STEP`（绝对步号；**阶段 3 时点**鼓轨第 4 行只读，
  展示 `dstep[c].on` 窗口聚合，点击写状态行提示不发命令——P4-C Task 9 起鼓轨逐 lane
  可编辑，见本文末节）。
- **LVGL 内存纪律**：格/标签/卡片共用静态 `lv_style_t`（状态变化走 `lv_obj_replace_style` 换指针），
  卡片内文本合并；仍保留局部样式的只有四张卡的 `pad_all`、四条电平条（main/indicator）与顶栏/状态行
  两个单例标签，量级固定。这样把 64KB 内置 TLSF 池占用从首版的 ≈97%（文本重分配即可能失败）压到
  ≈68%；池扩容到 96KB 会撑爆内部 RAM（实测链接期 `.bss` 溢出），故不走改配置路线。`sloop_ui_start()`
  就绪行固定打印池占用，且该采样（`lv_mem_monitor`）在 `bsp_display_lock` 内执行——30ms 定时器此时
  已创建，无锁遍历 TLSF 池会读到撕裂的块结构。

## TAP hit-test 注入与等价性边界（2026-11-20，阶段 3 Task 7）

console `TAP <x> <y>` 走 `sloop_ui_input_tap()`：取当前页，按页内 hit-test 命中矩形，
执行**与控件事件回调相同的动作函数**。设计要点：

| 页 | 命中矩形来源 | 共同动作函数 | 等价 EditCmd |
|---|---|---|---|
| STEP | 建格同一组宏 `GRID_X0/Y0、CELL_W/H、CELL_STEP_X、ROW_PITCH` 现场推导（行/列各一次区间判定）；鼓行再加 4×4 lane 网格命中（`DRUM_GRID_X0/Y0`、`DRUM_LANE_PITCH`） | 合成轨：`step_activate(r,c)`（`cell_clicked_cb` 的唯一调用体）；鼓轨：`drum_toggle()`（控件回调经 `step_activate` 鼓分支进入，`TAP` 命中 lane 后直接调用同一函数） | 合成轨：`state_set_focus()` + `CMD_TOGGLE_STEP`；鼓轨：`state_set_focus(DRM)` + `CMD_DRUM_STEP_WRITE`（P4-C Task 9 起；不再是「只写状态行」） |
| TRACKS | `CARD_X0/Y/W/H/PITCH` | `tracks_select(k)`（`card_clicked_cb` 的唯一调用体） | `CMD_SELECT_TRACK` |
| TRANSPORT | 走带 4 项：静态表 `{x,y,w,h,kind,v}`（`TRANSPORT_*` 宏）；工具 18 项：`k_chips[]` 同一张表既建控件又做命中（几何/动作/初始文本的唯一来源） | `transport_play()` / `transport_stop()` / `transport_bpm_step(delta)` / `tr_action(act)`（P4-C Task 10；工具 chip 的所有动作） | `CMD_PLAY` / `CMD_STOP` / `CMD_SET_BPM`；工具：`CMD_REC_TOGGLE` / `CMD_FILL_CONTROL` / `CMD_STEP_EDIT` / `CMD_SET_MICRO` / `CMD_DRUM_STEP_WRITE`（鼓 lane LVL/RAT）/ `CMD_PATTERN` |
| PLAY | 无编辑控件（纯呈现 + 音符指示） | — | 恒 `TAP miss` |

**等价性声明（诚实边界）**：物理触摸 = LVGL hit-test → `LV_EVENT_CLICKED` → 回调；
`TAP` = 矩形表 → 同一动作函数。二者在 **EditCmd 层等价**（同一函数、同一 `state_apply`
命令序列、同一状态层/引擎落点），且"每页只有一个 EditCmd 发放点"可由源码核对（`CMD_*`
在各页源码中各出现一次，均在共同动作函数内）。**不声称原始指针事件等价**：按下/长按/
滑动、控件状态的视觉反馈（如按钮 pressed 态）与 LVGL 的事件派发时序不在 `TAP` 覆盖范围。
命中/未命中都可在 console 回显断言（命中打印动作与参数，未命中打印 `TAP miss x,y page=P`
并返回非零）；`sloop_ui_input_tap()` 另有独立的锁超时状态，console 打印
`TAP ERR lvgl_lock_timeout` 并返回非零——锁超时表示**未派发**，脚本不得把它当正常 miss。
`UI <page>` 同理：`sloop_ui_switch_page()` 返回 `ESP_ERR_TIMEOUT` 时 console 打印
`UI ERR lock_timeout`，页断言不会因「回显缺失」而假通过。`scripts/inject/` 三条场景脚本即以此对真机取证。

TAP 在 console 任务里取 `bsp_display_lock`（命中后会写状态行并立即刷新本页）；锁序
（LVGL → state → engine）与控件回调一致，无反转。落表外不写任何状态、不崩溃。

## 触摸准星的角色收敛（Task 7）

bring-up 的触摸十字准星画在 `lv_layer_top()`，会盖住生产页面并出现在截图里。新增
`app_display_touch_feedback_enable(bool)`：`sloop_ui_start()` 成功后由 `main.c` 关闭
（准星 + 坐标日志），UI 启动失败（回退测试图）时保持开启。DI-10 的幽灵按压属 indev
事件问题，不在本处修复——关闭的只是可视反馈。

## 阶段 4 容量决策（LVGL 池预算，2026-11-21 终审登记）

阶段 3 收口实测：四页静态创建一次后，64KB 内置 TLSF 池已用 **≈68%**（`sloop_ui_start()`
就绪行打印；对象/样式按上面「LVGL 内存纪律」共享）。池扩容到 96KB 会撑爆内部 RAM
（实测链接期 `.bss` 溢出），所以阶段 4 的页面增长必须先在下列方案里**作出决策**，
且必须在**第一个参数页（旋钮页）落地之前**完成——不能边加页面边试探池水位：

| 方案 | 要点 | 代价/风险 |
|---|---|---|
| A. 懒建 + 拆页（每页预算） | 页面对象按需创建、离页销毁；每页分配前检查 `lv_mem_monitor()` 剩余，超预算拒绝并记 ERROR | 切页有构建延迟（首帧）；焦点/滚动状态需随页保存 |
| B. 池扩容 | 把 `CONFIG_LV_MEM_SIZE_KILOBYTES` 提到 96KB+ | 阶段 3 已实测内部 RAM 链接溢出，须先找到内部 RAM 的其他减项 |
| C. 共享池化 | 控件/样式模板跨页复用（同构控件共用对象池） | 抽象成本高，页面差异大时收益低 |

决策产出物：一张「页面 × 对象/字节预算」表 + 选型理由（写回本节）。在此之前若临时加页，
须在 `sloop_ui_start()` 就绪行附池占用证据，并确认仍有余量。

**决策（2026-11-21，阶段 4 P4-0 Task 4）：选 A（懒建 + 每页预算），落地为 ADR-008。**

- 页面注册表 `ui_page_t` + `ui_page_register()`：页签由注册表派生；PLAY 启动即建
  （开机非空白），其余页首次进入时建、离页不销毁。
- 实测预算表（`UI MEM` 口径，池 free 减少量）：PLAY 2,492 / TRACKS 6,080 / STEP 21,760 /
  TRANSPORT 3,744 B（STEP 在 P4-0 Task 5 步窗后复测为 21,832 B，见 ADR-008 §3）；est_bytes 分别为 3,072 / 7,168 / 23,040 / 4,608（合计 37,888 B，
  < 池可用 63,120 B；STEP 在九页口径由 24,576→24,064→**23,040** 两次下调，见 ADR-008 §3/§5）。构建前 `free < est` 即拒绝。
- 失败可见：构建失败 → 删除半成品 + ERROR 日志 + 页面区错误状态条 +
  `UI ERR page_build <id>`（console），失败页下次访问重试；`UI FORCEFAIL <page>` 为验证注入。
- 池仍为 64KB（`CONFIG_LV_MEM_SIZE=65536`）；96KB 的 `.bss` 溢出结论不变，扩池路线不重启。
- 阶段 4 参数页沿用同一套：每页一个注册表条目 + 实测 est + `UI_BUILD_CHECK` 分配检查；
  Σ est 逼近池总量时必须先做设计决策（拆页/共享/驱逐），详见 ADR-008。


## 阶段 4 入口条件：16 步视窗抽象（2026-11-21 终审登记；同日 P4-0 Task 5 落地）

此前的 STEP 网格固定 16 列（`APP_STATE_STEPS`），快照只带前 16 列。阶段 4 一旦支持
bank/滚动或 LEN > 16 的编辑，必须**先**在状态层与快照引入一次 `{bank, window_start,
window_len}`，页面布局/hit-test/注入脚本都从它推导，再扩网格列数；禁止在页面里
各自再加一套列偏移。网格渲染须同时把列 ≥ 每轨 `len` 的格暗显（阶段 3 已落地该 dim
位，阶段 4 沿用同一判定）。

**落地（2026-11-21，P4-0 Task 5）**：此前「STEP 网格固定 16 列、快照只带前 16 列」的
状态已由步窗取代——快照仍是一次取 16 列，但基址由 `window_start` 给出（见下）。

- `step_window_t { uint8_t bank, window_start, window_len; }` 由 `sloop_state` 持有
  （UI 局部态，非引擎真值），默认 `{0,0,16}`；`CMD_SET_WINDOW{a=bank(冗余), b=start,
  v=len}` 写、`step_window()` 读。start 16 对齐、0..48（bank = start/16），len 1..16，
  focus_step 夹在窗口内。引擎步号**恒为绝对索引**（命令携带绝对值；`step_window_abs()`
  是「窗口列→绝对步」的唯一映射函数）。
- 快照的 `step[k][c]` / `dstep[c]` 是**窗口列投影**：`state_sync_from_engine` 调
  `sloop_core_read_window(window_start, &snap)`，一次引擎锁内取 16 列（每拍取锁次数
  仍为 1，不逐列 `read_step`）；`CMD_SET_WINDOW` 落窗时立即重取一份投影，避免下一拍
  之前显示旧 bank 的格。
- STEP 页列数 = `window_len`，窗口外的格与列号标签隐藏（TAP 命不中）；列号/顶栏
  `STEP`/`WIN a..b` 均用绝对步号；播放头把绝对 `step_pos` 映射进窗口，窗口外不显示
  （不留残留描边）；dim 仍按「绝对步 ≥ 本轨 LEN」判定。
- 注入几何由 `page_step.c` 的布局宏派生（`step_cell_xy`），列语义对齐 `STATE SNAP`
  的 `window_start/window_len`；console `UI WINDOW [<start> <len>]` 为落窗入口。
  `step_edit.sh`、`transport.sh` 与 `soak.py` 在进入 STEP 前归一到默认窗口（窗口是持久
  UI 状态；transport 的归窗在 P4-0 Task 5 修复轮补上）。

## 后果

- 阶段 4 全页面在此架构上扩展（页面路由+控件复用）；阶段 5 将 `state_apply` 的执行端换成 FM-1 会话即可复用 UI。
- 引擎保持单例+互斥；UI 输入路径的 input 任务已落地（`sloop_ui/src/input.c`：按 tick
  地板轮询 `tab5_kb_poll`——100Hz tick 下 10ms，见 ADR-006；优先级 4）；串口 `KEY <code> <down|up>` 与物理同入口，
  实测派发延迟（含 state_apply 与 PLAY 指示入队）均值 ≈0.51ms、note-on 最大 ≈1.37ms（note-off 最大 1.32ms；Task 6，见 verification-log）。端到端「键→发声」还含轮询（1 tick=10ms）与音频块（5.8ms），听感/手感的人测项按端到端口径复核（ADR-006）。切页走 `sloop_ui_request_page()` 邮箱（派发 22–72µs），输入路径不再等 LVGL 锁。
- 阶段 3 遗留的「选轨与逐轨 mute/solo 由状态层持有」特例已在阶段 4 P4-0 Task 1 删除：引擎落地 `sloop_core_select/set_mute/set_solo`（`song.sel`/`P_MUTE`/`song.solo`），`state_sync_from_engine` 整份取引擎真值、无例外；`STATE SNAP` 与 `ENG SNAP` 对这三类值同源，音频效果即上游 `trk_silent()` 口径（听感人测仍为外部门禁）。

## 导航信息架构（2026-11-21 P4-A 终审登记；P4-B 决策门）

P4-A 把页签从 4 张扩到 9 张（PLAY/TRACKS/STEP/TRANSPORT + GLOBAL/FX/ANALOG/FM6/
SAMPLE），页签条已接近物理极限。**P4-B 在注册第 10 页之前必须先做一次导航信息
架构决策**——当时的池容量条目（已关闭，见下节）只门禁 LVGL 池容量，不覆盖页签宽度/
可发现性。

现状（2026-11-21，P4-A，实测/as-built）：

- 页签宽度 = `min(176, (1280 − 2×16 − 220 − 8×8) / N)`：N=9 时 **107px**，最后一张页签
  右端 x=1043；`TRANSPORT` 的 16pt 标签宽 101.6px（页签新建时 `pad_all=0`，剩 ~5px）。
- 右侧留给 `SLOOP` 标题（右对齐 −24）与层键提示（右对齐 −110）：提示标签盒
  x∈[1051,1170]（16pt，宽度 = 1170 − 页签右端 − 8），**宽度封顶**
  （`LV_LABEL_LONG_DOT`，文本右对齐）：单/双层键完整显示，四层键同按时以省略号
  收尾，绝不压到页签上。P4-A 修复轮之前是 20pt + 无宽度约束（与最后一张页签约
  30px 重叠），见该轮证据（`docs/dev/verify/4-p0-2026-11-21/task-11-reviewfix-*`）。
- **N=10 不可行**：`(1280−32−220−64)/10 = 95px < 101.6px`——`TRANSPORT` 标签已放不下，
  更不用说新增页名。因此第 10 页不是「改个数字」的事。

P4-B 的候选方向（决策时写入本节，选一个再动代码）：

1. **分组导航**：页签只保留组（如 PLAY/EDIT/FX/SYS），组内页用二次页签或页内切换；
2. **两行页签**（占用页面区高度，需重排所有页面布局）；
3. **溢出菜单**：前 N 页常驻 + 其余进 MENU 页（需要新的页面/焦点语义）；
4. **按上下文隐藏页签**（独立/联动模式不同页集）——会破坏「页签 = 全部页面」的
   可发现性，慎用。

在此之前：不得注册第 10 页、不得加宽页签、不得缩小页签字体（16pt 已是可读下限）。

> 上述禁令随 2026-11-21 的决策解除；本节保留为 P4-A 时点的历史记录（9 页实测数据见上），
> 新决策见下节。

### 导航决策（2026-11-21，P4-B Task 1 终审登记）

**决策：A（分组页签 + 组内子页签 + 组作用域页面生命周期）。** 依据 P4-B spec §3.3；
本决策同时关闭原「9 页参数页框架已达 LVGL 池容量天花板」延迟条目（条目要求「解决后删除；
方案选型先沉淀 ADR」——即本节）。选择理由：不改现有九页布局、不动页签条高度、不给
「第 10 页」加宽度墙，同时把池内存从「九页平铺 Σ」变成「基础占用 + 单组 Σ」
的确定预算；原条目记录的 LRU 驱逐/拆页/扩池三条方向均被否决（驱逐时机不可预测、拆页
牺牲同页信息密度、扩池已实测链接溢出）。

#### 1. 组与子页

顶层 7 组（P4-C Task 11 起含 SONG、P4-D Task 8 起含 SYNTH；`SYS` 待 P4-E 有设置页时
再加入）：

| 组 | 子页 |
|---|---|
| PLAY | PLAY |
| TRACKS | TRACKS |
| EDIT | GLOBAL / FX / ANALOG / FM6 |
| SYNTH | SAMPLE / DSYN / FM6P（P4-D Task 8：SAMPLE 从 EDIT 移入；DSYN 实体页在 Task 9、FM6P 在 Task 10） |
| SEQ | STEP / TRANSPORT |
| SONG | SONG（P4-C Task 11：独立组，不占 SEQ 稳态） |
| FILE | PROJECTS / PRESETS |

- `app_page_t` 追加 `APP_PAGE_PROJECTS, APP_PAGE_PRESETS`（P4-B），P4-C Task 11 再追加
  `APP_PAGE_SONG`，P4-D Task 8 追加 `APP_PAGE_DSYN, APP_PAGE_FM6P`；`APP_STATE_PAGES`
  9→11→12→**14**。SONG/SYNTH 的 id 排在最后（既有页 id 不变），页签顺序由组表决定
  （SYNTH 在 EDIT 与 SEQ 之间）。console `UI` 页名表、`UI MEM` 的组字段、注册表顺序同源。
- 页签 = 组签（上行）+ 组内子签（下行）；注册顺序 = 组顺序，组内顺序 = 子页顺序。

#### 2. 两行页签复用现有 72px

- 组行约 34px、子行约 30px（16pt 标签约 20px 高）；**页面区起点仍是 y=72，现有九页布局
  零改动**（参数页 `52+2×298=648` = `UI_H−72`、STEP 底行等像素级布局不重排）。
- 页签命中几何：`TAP` 仍按 y<72 判页签区；组签/子签各自的矩形由同一组布局宏派生。
- 右侧 reserve 继续留给 `SLOOP` 标题（上行）与层键提示（下行）；子行只有 1 页的组留空。
- 组行 P4-B 时 5 个页签：`min(176, (1280 − 32 − 220 − 4×8)/5) = 176px`（最长标签
  `TRACKS` 远小于页签宽）；P4-C Task 11 增至 6 组后取 `min(176, (1280 − 32 − 216 −
  5×8)/6) = 171.5 → **168px**（6×168+5×8=1,048，右端 1,064 = 1,280−**216** reserve）；
  P4-D Task 8 增至 7 组后取 **142px**（7×142+6×8=1,042，右端 16+1,042=**1,058** ≤
  1,064，theme.h 有静态断言；SLOOP 标题起点 ~1,191 仍有 133px）。子行仍是 176px（5 个
  复用槽，右端 928），**不受组数影响**。**不再受 N=10 时 95px < `TRANSPORT` 101.6px 的
  宽度墙约束**。

#### 3. 组作用域页面生命周期

- 切组顺序固定：**建目标页 → 切到目标页 → 销毁源组的全部已建页**（含刚离开的当前页；
  `lv_obj_del` + `s_page_obj=NULL`、state 回 `UNBUILT`、delta=0）。因此离开组后源组
  `built=0`。顺序不可交换：先销毁会在切页时出现**空白帧**，且跨组切换期间输入路径必须
  始终有可派发的活页；代价是切换瞬间的峰值 = 源组已建 Σ + 目标页 est（由 §5 的峰值
  不变量覆盖，源组页面在切换后才释放，峰值是瞬态）。
- 组内首次访问兄弟页时构建它；同组后续切换只改显隐（零重建）。PLAY 单页仍在启动时构建。
- 销毁在 LVGL 锁内执行，先作废待执行的异步切页；同拍输入（`KEY tab` + `KEY PANIC` 等）
  不得访问已销毁对象（`nav.sh` 四用例按新语义覆盖）。
- 构建延迟预算 ≤100ms 适用于**跨组切换**（目标组首建）与**组内首次构建兄弟页**；同组
  后续切换零重建。若实测超限，退化为「每组保留最近 1 页」（不回到无边界 LRU）。
- 焦点/滚动状态由状态层在重建时恢复（与既有懒建路径一致）。

#### 4. `TAB` 扁平轮转新顺序

`TAB` = 按组顺序的扁平轮转，组内最后一页的下一张 = 下一组第一页：

```
PLAY → TRACKS → GLOBAL → FX → ANALOG → FM6 → SAMPLE → DSYN → FM6P → STEP → TRANSPORT
     → SONG → PROJECTS → PRESETS → PLAY
```
（P4-D Task 8 起 14 页 / 7 组；SAMPLE 属 SYNTH 组，DSYN/FM6P 紧随其后。）

- 保留现有「tab 轮转」心智与 `nav.sh` 用例形态，只更新期望序列；未注册的页（实施中途）
  轮转到最后一个已注册页即回 PLAY。
- 点顶层页签进入该组并**记住组内上次子页**；点子页签直接切换。
- `ESC` = 回 PLAY（组与子页同时复位）；弹奏/走带键语义不变。FILE 页的槽选择与 LOAD 见
  P4-B spec §4.5（`←/→` 选槽、`ENTER` = LOAD；SAVE/ERASE 仅触摸按钮 + console）。

#### 5. 容量不变量（取代「九页平铺」口径）

`UI MEM` 口径（TLSF 可用 63,120B），**跨组切换的过渡峰值**（按有序对取最坏）：

```
base + max(源组 Σ est + 目标组首建页 est) + 4KB ≤ 63,120B
```

- `base` = 非页面基础占用（根/两行页签条/错误状态条/字体/LVGL 内部等），**P4-C
  Task 11 最终固件实测 11,624B**（＝ 63,120 − boot `UI MEM` 的 `pool_free 49,016` −
  PLAY delta 2,480）。P4-B 的两行页签（组签/子签都是 `lv_button + 子 label` 两对象）
  同法为 **13,800B**（63,120 − 46,840 − 2,480）；Task 11 把两行 11 个签收敛成
  **单对象可点击 label**（§7.2），基础占用降 **2,176B**。九页平铺页签同法为 13,196B。
  「UI 就绪」那一拍的瞬时值比稳态高 0.3KB 量级，保守上界取 **11,936B**（11,624 +
  312）。本节的旧值 7,016B（= `pool_used 55,064 − Σdelta 48,048`）用 `pool_used` 会
  漏计 TLSF 块头，是低估，已作废。
- 组 est（P4-C 12 页口径；P4-D Task 8 起 SAMPLE 移入 SYNTH，新表见 §5.1）：PLAY 3,072 /
  TRACKS 7,168 / EDIT `3×3,584 + 6,144 + 3,072 = 19,968`
  （FM6 自 P4-B Task 13 起含 bank 存取行，est 3,584 → 6,144，实测 delta 4,956–5,100B）/ SEQ
  `STEP 23,040 + TRANSPORT 9,216 = 32,256`（TRANSPORT 自 P4-C Task 10 起含录音/fill/
  聚焦步字段/pattern 工具；实测 delta 8,248–8,716B，见 ADR-008 §3 注）/ SONG `12,288`
  （P4-C Task 11：独立组单页；实测 delta 11,136–11,564B，见 ADR-008 §3 的 SONG 行）/
  FILE `PROJECTS 8,192 + PRESETS 12,800 = 20,992`（P4-B Task 12 按实测定稿：首次构建
  delta 7,764B / 12,260B，est = +5.5% / +4.4%、512 对齐；初版 4,608/5,120 是**下限估算**，
  实测对象数（26/36 个）远超，见 ADR-008 §3 的 FILE 表。构建前实测 est 以 ADR-008 表为准）。
- **有序对按当前 est 表重算**（每条 = 源组全建 Σest + 目标组首建页 est；账面 =
  `base 11,624 + 对值 + 4,096`，不变量 ≤ 63,120；括号内为保守 base 11,936 时的值）：
  1. **SEQ 两页全建 → FILE 首建 PRESETS（最坏）**：`32,256 + 12,800 = 45,056B` ⇒
     账面 **60,776**、余 **2,344B**（61,088、余 2,032B）——TRANSPORT 的 est 9,216 与
     Task 11 的页签单对象化共同留下的余量；实测 peak `pool_free 8,324B`（下条）；
  2. **SEQ 两页全建 → SONG 首建**（P4-C Task 11 新增）：`32,256 + 12,288 = 44,544B` ⇒
     账面 **60,264**、余 **2,856B**（60,576、余 2,544B）——第二坏；
  3. **FILE 两页全建 → SEQ 首建 STEP**：`20,992 + 23,040 = 44,032B` ⇒ 账面
     **59,752**、余 **3,368B**（60,064、余 3,056B）；
  4. **EDIT 全建 → SEQ 首建 STEP**：`19,968 + 23,040 = 43,008B` ⇒ 账面 **58,728**、
     余 **4,392B**（59,040、余 4,080B）——FM6 bank 行把 EDIT 抬到 19,968；
  5. **SEQ 全建 → EDIT 首建 GLOBAL**：`32,256 + 3,584 = 35,840B` ⇒ 账面 51,560。
  6. SONG 出发的最坏对是 SONG→STEP `12,288 + 23,040 = 35,328B` ⇒ 账面 51,048；
     SONG→PRESETS `12,288 + 12,800 = 25,088B` ⇒ 账面 40,808。
  页签单对象化把旧账面最坏对（62,952 / 余 168B）的余量恢复到 **2,344B**；若未来
  再加组/页，仍按有序对与实测 peak 重算。
  （STEP 属 SEQ，所以它不可能是「SEQ 常驻时」的跨组目标；但可以是 EDIT→SEQ /
  FILE→SEQ 的目标——最坏对必须按有序对枚举，旧式「任意单页上限 + 任意最大组」是
  病态上界。）
- **有序对 1 的设备实测（P4-C Task 11 最终固件，`transport` 场景 ⑩j）**：建满
  SEQ（STEP+TRANSPORT，`GROUP SEQ built=2 sum_est=32,256`）后从 SEQ 直接 `UI presets`
  （跨组：目标页构建期间源组仍存活）——构建日志 `页面构建 page=PRESETS(10)
  池增量=12,068B 预算=12,800B 池剩余=8,324B`（`task-11-transport-green4.log`），
  即 **过渡峰值点 `pool_free=8,324B`**（≥4,096B 门禁通过）；过渡后
  `SEQ built=0 / FILE built=1`。PRESETS delta 的归档区间 12,008–12,276B
  （池历史漂移；Task 10 读数 12,276B → 峰值 6,240B）。
- 同批证据另覆盖：`TAP` 回显的 `status="…"` 与屏幕同源、焦点轨 ≠ 选中轨的目标、
  冻结/空步/bank1 窗口的拒绝路径（见本文末节与 `task-10-report.md`）。
- **实测过渡峰值**（构建日志的「池剩余」= 销毁源组之前的 free）：
  2026-11-22 Task 11 最终固件：EDIT 五页全建 `pool_free 34,212B` → 跨组建 STEP
  （delta 21,856B、耗时 30ms）后 **`pool_free 12,356B`**；SEQ 两页全建
  `pool_free 23,376B` → 跨组建 GLOBAL（delta 2,848B）后 `pool_free 20,528B`。
  Task 12（FILE 组落地）：FILE 两页全建 `pool_free 28,424–29,112B` → 跨组建 STEP
  （delta 21,716–21,732B、31ms）后 **`pool_free 7,308–7,840B`**（四次运行）——P4-B 时点
  最坏**实测**峰值（`store` 场景 ⓯ 断言 ≥4,096B），仍是 DI-19 弧线 mask 峰值
  （≈2.6KB，取 4KB 余量）的 2.8 倍。
  **P4-C Task 11（12 页/6 组、页签单对象化）**：`transport` ⑩j 的 SEQ→PRESETS
  构建后 **`pool_free 8,324B`**（PRESETS delta 12,068B）；`song` ⑩ 的 FILE→STEP
  构建后 **`pool_free 9,744–9,860B`**（STEP delta 21,608–21,664B、预算 23,040B）；两处均 ≥4,096B。
  基础占用从 13,800 降到 11,624 后，两次最坏实测峰值从 7.3–7.8KB 抬到 8.3–9.3KB
  （离 4KB 底线更远）。
- **为什么必须按峰值核算**：切组顺序是「建目标页 → 切到目标页 → 销毁源组页面」，源组的
  池占用在**切换之后**才释放；若在切换前释放，会出现空白帧、输入路径在跨组切换期间
  没有活页。因此峰值 = 源组已建 Σ + 目标页实际构建量，是瞬态但必须被上式覆盖；切换
  完成后回落为稳态 `base + max_group Σ est = 11,624 + 32,256 = 43,880B ≤ 63,120B`。
- **P4-C 12 页预算（Task 11 已落地）**：SONG 使用 **独立顶层组** 而不是加入 SEQ，
  组顺序 `PLAY/TRACKS/EDIT/SEQ/SONG/FILE`；TRANSPORT est 上限 **9,216B**（Task 10
  实测 delta 8,248–8,716B，est 保持上限——见 ADR-008 §3 注）、SONG est **12,288B**
  （Task 11 实测 delta 11,136–11,564B，保持初估上限——它不参与最坏有序对，抬 est
  没有收益，收紧也没有必要）。基础占用 11,624B（页签单对象化）下：SEQ→PRESETS
  `45,056B`、账面 **60,776B**、余 **2,344B**（最坏）；SEQ→SONG `44,544B`、账面
  **60,264B**、余 2,856B；FILE→STEP `44,032B`、账面 **59,752B**、余 3,368B；
  SONG→PRESETS `25,088B`、账面 **40,808B**。六个组签仍在 72px 页签条内（168px 宽，
  见 §2），`free ≥ est` 与过渡后 `pool_free ≥4,096B` 门禁不变；超预算先改对象设计，
  不扩池。
- **每页构建前 `free ≥ est` 的检查继续保留**（真门禁不变）；组作用域只是把「同时已建
  页集合」限制在一组内。
- `UI MEM` 增加分组行：`GROUP <name> built=<n> sum_est=<B> sum_delta=<B> build_ms=<ms>`，
  与既有 `page` 行并存（`sum_est` = 组内已注册成员 est，`sum_delta` = 已建成员池增量）；
  九页平铺的实测预算表保留在 ADR-008 §3。
- 池仍为 64KB（`CONFIG_LV_MEM_SIZE=65536`）；扩池路线不重启。
- 构建延迟（`build_ms`）：跨组进入 SEQ（STEP 首建）最终 nav 证据 **33ms**（早期会话
  29–35ms）、EDIT 首建 GLOBAL 7–11ms、PLAY/TRACKS 首入 ≤10ms；同组首建
  FX/ANALOG/FM6/SAMPLE/TRANSPORT 4–9ms——全部 `0 < x ≤100ms`。

#### 5.1 P4-D 14 页/7 组预算（2026-11-23 Task 8 落位；Task 11 实测收口）

- 组顺序 `PLAY/TRACKS/EDIT/SYNTH/SEQ/SONG/FILE`（7 组）；SAMPLE 参数页从 EDIT 移入新的
  SYNTH 组（SAMPLE/DSYN/FM6P），EDIT 缩为 4 页。组签宽 168→**142px**（见 §2），子行
  仍 176×5。`APP_STATE_PAGES` 12→**14**（`APP_PAGE_DSYN=12`、`APP_PAGE_FM6P=13`，既有
  页 id 不变），`TAB` 变 14 页扁平轮转；`UI TAB synth` / `UI SUB 0..2` 与物理组签同路径。
- 组 est：EDIT `3×3,584 + 6,144 = 16,896`（FM6 的 bank 行不变）/ SYNTH
  `SAMPLE 6,144 + DSYN 7,168 + FM6P 7,168 = **20,480**`（SAMPLE 因槽管理行 3,072→6,144；
  DSYN/FM6P 按 spec §4.4 的计划上限落位）。
- **实测（P4-D Task 11，`params` + `synth`，`UI MEM` 池增量 / `页面构建` 日志）**：
  SAMPLE **4,360–4,508B**（est 6,144）、DSYN **6,168–6,476B**（est 7,168；Task 9 首测
  6,436）、FM6P **6,892–7,124B**（est 7,168；Task 10 首测 6,892–7,124，本轮 7,004–7,068）；
  SYNTH `sum_delta` **17,824–17,976B**（`sum_est` 20,480，两场景读数），`build_ms` 11–14ms（≤100ms）。
  三页 `delta ≤ est` 全部成立；SAMPLE 的 est 是 spec §4.4 的**固定上限**（实测只用到
  71–73%，属预留，不参与最坏有序对的调节）。
- 有序对（§5 的 base 11,624 + 对值 + 4,096 ≤ 63,120）：**SEQ→PRESETS 仍最坏 60,776
  （余 2,344B，不变）**；新增 **SYNTH→STEP** `20,480 + 23,040 = 43,520` ⇒ 账面
  **59,240**、余 3,880B；**EDIT→STEP** `16,896 + 23,040 = 39,936` ⇒ 账面 **55,656**、
  余 7,464B（SAMPLE 移出后 EDIT 不再接近上限）。
- **实测过渡峰值**（`synth` ⑤：SYNTH 三页全建后直接 `UI step`，构建日志
  `页面构建 page=STEP(2) 池增量=22,188B 预算=23,040B 池剩余=11,244B 耗时=31ms`）⇒
  `pool_free=11,244B ≥ 4,096B`；过渡后稳态 `pool_free 28,800B`（`UI MEM` 实测）。
  同批 `synth` ⑤ 的 10 轮 SYNTH/FILE/SONG/SEQ 往返 `pool_free 47,072 → 47,040`
  （Δ−32B、单次最大下降 44B ≤128B）；`nav` ⑧ 的 PLAY↔TRACKS ×10 为 Δ−44B
  （单次 72B）。跨组池不泄漏口径不变。
- 采样槽管理行（SAMPLE 页底部 `U1..U4` + `ERASE` + 状态行，y=552..596）与 FM6 bank 行
  同几何；状态行单一映射在 `theme.h`（选中槽 `SET U<n>` / `ERASE OK` / `NOT USR` /
  `STOP BEFORE ERASE` / `NO STORAGE` / `ERASE ERROR`）。`sloop_store_sample_info` 是
  文档化只读例外：建页/动作/键变化时立即读，30ms 拍的非强制路径**最短 250ms**、
  key 稳定时 1s（单次最坏 4×80KiB CRC，绝不每 30ms 轮询）。
- 设备证据（P4-D Task 11）：`UI TAB synth` / `UI SUB 0..2`、SYNTH 三页全建
  `built=3 sum_est=20,480`、离开组 `built=0`（三页 unbuilt）；`params` 的
  `EDIT sum_est=16,896`、组作用域后 `pool_free 35,972B`（≥20KB）。

#### 6. 验证入口

- `UI MEM` 的分组 Σ + 每页 `delta ≤ est`；跨组切换后源组 `built=0`（含原当前页）。
- `nav.sh` 按新顺序与新组语义更新并通过（含跨组销毁时同拍输入安全）。
- `scripts/inject/store.sh` 覆盖 FILE 组：`UI projects/presets` 截屏 + `TAP` 动作 +
  `UI MEM` 组水位断言（FILE Σ ≤ 预算、离开 FILE 后两页回 `UNBUILT`，含原当前页）。
- 九页既有截屏基线只允许页签区差异（页面区像素不变）；差异证据按 AGENTS §5.4 晋升。

#### 7. 落地（P4-B Task 11，2026-11-22）

- 组表 `k_groups[]`（ui.c）：PLAY{PLAY} / TRACKS{TRACKS} / EDIT{GLOBAL,FX,ANALOG,FM6,
  SAMPLE} / SEQ{STEP,TRANSPORT} / FILE{PROJECTS,PRESETS}。组签固定槽位（P4-B 时 5 组 176×32 @ y=4；
  P4-C Task 11 起 6 组、168×32），无已注册页的组隐藏并压缩排布（Task 12 起 FILE 已注册、
  Task 11 起 SONG 已注册，6 个组签常显）；子签固定 5 槽
  复用（176×30 @ y=38），**单页组（PLAY/TRACKS）子行留空**；层键提示在下行右侧
  （x∈[936,1170]，LONG_DOT），SLOOP 标题在上行右侧（−24,6）。页面区仍是 y=72、1280×720。
- `TAP` 对 y<72 一律 miss（页签切换走 LVGL 回调 / `UI` / `KEY tab`）；面板命中几何
  由 `theme.h` 的 UI_GROUP_*/UI_SUB_* 宏与注入脚本 `group_tab_probe/sub_tab_probe` 同源。
- `show_page()` 的组作用域顺序与 `destroy_group_pages()` 落在 ui.c；目标页构建失败也
  销毁源组（否则两个组的页面在错误态下并存）。`s_group_build_ms[]` 记录跨组进入/组内
  首建的耗时（`UI MEM GROUP … build_ms`），每页另有 `build_ms`。
- `sloop_ui_page_order_next()` 是 `TAB` 的唯一顺序来源；`sloop_ui_page_count()` 保留为
  注册表计数（不再用于轮转）。
- 手指路径（R32）：console `UI TAB <group>` / `UI SUB <index>` 与物理组签/子签**共用**
  同一动作函数（`group_select_locked`/`sub_select_locked`）——TAP 对 y<72 一律 miss，
  这是唯一能机器覆盖手指导航的入口；`UI SUB` 在单页组（子行为空）与越界时回
  `UI ERR sub <n>`，`UI TAB` 对未注册页的组回 `UI ERR group <NAME>`。
- 切页作废旧请求（R30）：`show_page()` 顶部 `sloop_ui_cancel_pending_page()`——同一
  30ms 拍内 `KEY tab` 排队后紧跟手指点击时，点击胜出、待切页被丢弃（定时器路径
  邮箱已消费，为 no-op；console 同步切页另有一次幂等清空）。
- 验证：`nav.sh` ①–⑧（⑤ 组作用域：EDIT→SEQ 后 EDIT built=0、同组只改显隐、
  build_ms `0 < x ≤100ms`；⑥ 手指导航：点组进上次子页且记忆跨往返、跨组点组销毁
  源组并建目标首个子页、子签同组切换不重建、同拍 `KEY tab` + `UI TAB` 点击胜出、
  SONG 单页组 built=0；⑦ TAB 全 12 页轮转 `…→TRANSPORT→SONG→PROJECTS→PRESETS→PLAY`，
  异步切页用 `STATE SNAP` 轮询等落地；⑧ PLAY↔TRACKS ×10 的池往返）；`params.sh` 的
  池口径改为组作用域（同时只常驻一个组，`GROUP EDIT sum_delta` + `pool_free ≥20KB`）；
  DI-19 由「store→params 不重启」现场收口；`song.sh` 覆盖 SONG 组（见 §7.2）。

#### 7.1 FILE 组两页落地（P4-B Task 12，2026-11-22）

- `k_builtin_pages[]` 追加 `{APP_PAGE_PROJECTS,"PROJECTS",…,8192}` 与
  `{APP_PAGE_PRESETS,"PRESETS",…,12800}`；FILE 组签从此显示、`TAB` 轮转含两页、
  `UI MEM GROUP FILE` 给 built/sum_est/sum_delta/build_ms。
- 页内只留状态行（页名由下行子签承担）、按钮/格子用**单对象可点击 label**（不带
  `lv_button` 的子 label）：36 个对象（32 格 + 状态 + 3 按钮）压到 12.3KB。样式
  `lv_style_t` 只初始化一次（`static bool done`）——组作用域会反复销毁/重建页面，
  重跑 `lv_style_init` 会漏掉已分配的 props 数组。该纪律现在**全部页面**遵守
  （评审 R34 补齐 TRACKS/STEP；10 次 PLAY↔TRACKS 往返 `pool_free` 变化 0，见 ADR-008 §3 注）。
- `TAP` kind：`SLOOP_TAP_FILE_SLOT`（a=页 b=槽）与 `SLOOP_TAP_FILE_ACTION`
  （a=页 b=槽 v=0 save/1 load/2 erase）；PRESETS 的版面顺序（LOAD/SAVE/ERASE）与
  动作语义（0/1/2）由 `k_btn_act[]` 显式映射（首轮设备实测踩到：按版面当语义 ⇒
  SAVE 按钮发出了 LOAD）。console 回显 `-> FILE <page> slot=k select|action=… status="…"`。
- 状态行 `theme.h` 的 `ui_file_status_text` 单点映射（屏幕文本 + TAP 回显同源）；
  键盘 ←/→ 选槽、`ENTER` = LOAD（`sloop_ui_file_{slot_move,load}`，输入路径不碰控件）。
- 预算与峰值见 §5；实测：`store` 场景 ⓯ 的 FILE sum_delta 19,936B ≤ sum_est 20,992B，
  两页 delta 7,572–7,764 / 12,228–12,276 ≤ 8,192/12,800，过渡峰值 free 7,308–7,840B；
  PLAY↔TRACKS ×10 的 `pool_free` 变化 ≤84B（`styles_init` 每页只跑一次，评审 R34）。

#### 7.2 SONG 页与六组导航（P4-C Task 11，2026-11-23）

- **组表**：`k_groups[]` 插入 `{"SONG", 1, {APP_PAGE_SONG}}`（SEQ 与 FILE 之间）；
  `SLOOP_UI_GROUPS` 5→6，组签宽 176→**168px**（6×168+5×8=1,048，右端 1,064 =
  1,280−216 reserve；子行 5 槽仍是 176px/928）。`APP_STATE_PAGES` 11→12，`APP_PAGE_SONG=11`（id 排在最后，
  既有页 id 不变），console 页名表追加 `song`；`UI TAB song` / `UI SUB` 的令牌不变
  （单页组 `UI SUB 0` 回 `UI ERR sub 0`）。`TAB` = 组顺序扁平轮转
  （…TRANSPORT→SONG→PROJECTS→PRESETS→PLAY）。
- **页面**（`page_song.c`，35 个单对象可点击 label，无 `lv_button` 子对象）：
  - 状态行 (24,20) 20pt 两行：`SONG <SONG|LOOP> <STOP|PLAY> BAR n LIVE <A-D|-> READY
    <ABCD 位掩码>` + `REC <OFF|ARM|REC> CHAIN <n/i|-> LAST <none|NO SONG|N> | <动作结果>`；
    存储未就绪 → `SONG  NO STORAGE`，所有动作只回状态、不发命令。
  - A–D 卡 (24,96) 288×104：`A\nUSED[\nLIVE]`/`A\nEMPTY`，选中 = 6px UI_ACCENT 描边 +
    UI_PANEL_SEL 底；点卡设选中 section（链采集中同一 tap = 追加进临时列表）。
  - 动作行 y=216（140×48 ×4，x=24+i·148）：`STORE`/`LOAD`/`ERASE`/`JUMP`（JUMP =
    live loop 播放中的下一小节 section 请求，**绝不用 project LOAD**）；右侧
    x=640 起：`SONG`（模式切换，SONG 模式黄底）/`CHAIN`（链采集中变 `COMMIT` 并黄底）/
    `CANCEL`（仅采集中显示）/`REC`（SONG REC，armed/recording 黄底）。
  - 16 项歌曲顺序（2×8，148×60，x=24+c·153、y=300+r·70）：`01 A 4`；count 之外
    `01 --` 暗色；选中 = 4px UI_ACCENT 描边。
  - 配置行 y=456（140×52 ×6）：`SCENE`（循环 A→D）/`BARS-`/`BARS+`/`COUNT-`/`COUNT+`/
    `LOOP`（文本随状态 `LOOP ON|OFF`，黄底 = ON）。
- **动作 = 一条状态层命令**：STORE/LOAD/ERASE/JUMP = `CMD_SECTION_*`；SONG =
  `CMD_SONG_MODE`；REC = `CMD_SONG_REC`；CHAIN/CANCEL = `CMD_CHAIN_*`；六个配置 chip
  各一条 `CMD_SONG_SET`（整份 cfg 随命令下发，count 1..16/bars 1..64 由状态层/引擎
  钳位；页面不直接写上游全局）。值一律写后回读，失败显示 `EMPTY/INVALID/BLOCKED/
  FROZEN/ERROR` 并置红字。**COUNT+ 的 seed 规则**：只把结构非法的槽（`scene>3` /
  `bars==0` / `bars>64`）写成 `A4`；合法 tail 原样保留——`COUNT-` 不抹 arrangement 里的
  项，`COUNT+` 放回时不得覆盖已持久化的编辑（设备断言：8 项、项 5=`C9` → COUNT-4 →
  COUNT+5 后项 5–8 仍是原值、只有从未初始化的项 9 才 seed 为 `A4`）。保留 `A4` 的理由：它是上游
  `arr_defaults` 的条目口径（A/4 bars），只用于从未初始化或损坏的槽，不是对合法项的默认覆盖。
- **chain = SAVE 层 begin/commit**：第一次按 CHAIN = `CMD_CHAIN_BEGIN(选中 section)`
  （立即作为普通 `live_req` 排队、清旧 chain）；链中 tap A–D 卡只进 ≤8 项 UI 临时
  列表（空 scene 也在列表里）；再按 = `CMD_CHAIN_COMMIT`（整份校验/原子安装）；
  `CANCEL` = `CMD_CHAIN_CANCEL`。未播放时 begin/commit 由适配层回 blocked（可见
  `CHAIN: BLOCKED`），空 scene 回 `CHAIN: EMPTY`（err not_found）且不装半条 chain。
- **键盘**：←/→ 在 count 内的条目间移动、↑/↓ 在 A–D 卡间移动、ENTER = LOAD 选中
  section（与页面 LOAD chip 同一 `CMD_SECTION_LOAD`）；SAVE/ERASE/CHAIN 只走触摸 +
  console，避免与演奏键冲突（spec §4.5 的同一条取舍）。
- **回显**：`-> SONG card=B select` / `-> SONG entry=1 select scene=0 bars=4` /
  `-> SONG <ACT> [section=A] value=<写后回读> status="<屏幕状态行>"`；失败另带
  `ERR <not_found|blocked|frozen|…>`。`sloop_ui_song_status_text()` 与屏幕同源。
- **预算与实测**（`song` 场景）：SONG delta **11,136–11,564B ≤ est 12,288B**（+6.3%–10.3%
  余量；构建 15–21ms）；SONG↔PLAY ×10 的 `pool_free` 首末差 **+16B**、单次最大下降 28B；
  SEQ↔SONG ×10（SEQ 两页全建 → SONG → 回 PLAY 稳态测量）首末差 **+84B**、单次最大
  下降 28B（无逐次增长）；修复轮复跑为 +68B / −16B（同一脚本的池历史差异，均 ≤128B；终审轮 +36/−8B；各有一轮预热以排除一次性
  文本/字形缓存）；FILE 两页全建 → 首建 STEP 的过渡峰值
  **free 9,744–9,860B ≥4,096B**。
  §5 的有序对按新表重排（SEQ→SONG 账面 60,264B 为次坏，SEQ→PRESETS 60,776B 仍最坏）。
- 文档同时把「本任务只确定预算、不注册 SONG」改为落地口径；`AGENTS` §3/§5.1/§5.4.1/§5.5
  与 ADR-008 §1/§3/§5 同步。
- **未覆盖**：人眼视觉读图（本会话无图像查看工具，用像素区扫描 + 布局几何检查代替，
  见 `task-11-visual-check.txt`）；物理触摸手感/长按不在 TAP 覆盖范围。

## STEP 深度呈现与鼓 lane 编辑（P4-C Task 9，2026-11-22）

spec §4.4 要求 STEP 格呈现 TIE/SLIDE/lvl/rat/micro/fill 并开放鼓轨 lane 编辑，同时
**不增加每格 label、不抬 STEP est（23,040B）**。落地取舍如下。

### 1. 一个自绘层，不是 64 个新对象

- 64 个格对象保持原样（bg/border 共享样式继续表达「有音/暗/焦点/播放头」）。
- 新增**唯一**一个透明、不可点的 overlay 对象覆盖 16 列网格，在 `LV_EVENT_DRAW_MAIN`
  里按每格的 `cell_view_t` 缓存自绘：文字（`lv_draw_label`）与小方块/内框（`lv_draw_rect`）。
  这样没有每格 label / 每格事件回调（64 个 `lv_event_dsc_t` 约 2KB 池，会顶破 est 余量），
  池增量只有 1 个对象量级。
- 文字使用**静态缓冲**（`s_note_txt/s_mic_txt/s_rat_txt`，`text_local=0`），绘制任务只
  引用它们，不在每帧做池分配；文本只在视图签名变化时重写。
- 脏检查：每格保留 1 字节样式状态（原有）+ 1 个 32-bit 视图签名；签名变化才
  `lv_obj_invalidate_area(overlay, 格矩形)`——只重画那一格，不整屏重画。样式变化
  （亮/暗/隐藏/描边）本身会失效该格区域，overlay 在该区域的重入绘制因此同步。
- 样式 `lv_style_t` 仍然只初始化一次（R34）：本任务没有新增共享样式。

### 2. 格内视觉编码（图例写在页面底部两行提示里）

| 状态 | 呈现 |
|---|---|
| NOTE | 轨色实心格 + 音符名（最多 4 个，2×2 槽）；名字颜色 = 每音符 lvl：白 NORM / 黄 HARD / 灰 SOFT / 深灰 GHOST |
| TIE | 空心：格底是面板色 + 轨色内框（inset 3px）+ `TIE` 文本（暗格用 45% 轨色）——与实心 NOTE 明确不同 |
| CONT | 该音符（`SF_CONT(k)`）名字**正下方** 12×2 白色短横线（长音从前一步延续；每音符独立，k=1/3 不会画到 k=0/2 的名字旁）；状态行在音符名后加 `*` |
| SLIDE / ACCENT | 顶条白色 `S` / `A`（`SF_SLIDE` / `SF_ACCENT`） |
| fill | 顶条 `F`（fill-only，黄）/ `N`（no-fill，灰）；normal 不画 |
| micro | 底条 `m±n`（`m+5` / `m-31`，灰） |
| ratchet | 底条 `xN`（N = 该步最大击打数 = rat+1，白） |
| DRM | 4×4 lane 网格：亮 dot 按 lane lvl 着色（橙 NORM / 白 HARD / 灰 SOFT / 深橙 GHOST），空格是暗色小方块（`UI_PANEL_SEL`），选中 lane 有白色 ring；顶/底条同上（鼓步的 fill/micro 由快照 `dstep[].fill/micro` 投影）。鼓格底色**保持空格样式**（不用聚合 `on` 填满鼓轨橙——同色的 NORM dot 会不可见），活动完全由 dot 表达 |

状态行（右上 600px）由**聚焦步派生**（不再是「最后一次动作」）：合成轨
`T1 S3 NOTE C4* E4 LVL H RAT x2 MIC +5 FILL F SLD ACC`（轨色），鼓轨
`DRM S6 LANE 6 OPEN ON LVL N RAT x1 MIC +0 FILL -`（橙）。焦点/选 lane 变化立即可见，
TAP 与键盘走同一投影。

### 3. 鼓 lane 选择与原始位图编辑

- **数据**：`sloop_core_snapshot_t` 追加 `dstep_raw[16]`（窗口列的原始 16 lane
  on/lvl/rat，`sloop_dstep_t` 同型；定义移入 sloop_core.h）与 `dstep[].micro/fill`；
  聚合 `dstep[].on/lvl/rat` 保持原义（`ENG SNAP`/`STATE SNAP FULL` 口径不变）。
- **动作**：触摸点 4×4 网格里的 lane dot = 选中该 lane 并切换该步该位；点格内其它位置
  = 切换当前选中 lane 的位。控件回调经 `step_activate()` 的鼓轨分支进入 `drum_toggle()`；
  `TAP` 命中 lane 后**直接**调 `drum_toggle()`（不经过合成轨分支）——两者最终是同一个
  函数、同一 `CMD_DRUM_STEP_WRITE`（整份 POD，lvl/rat 原样带回）。`drum_toggle()` 复用
  调用方的**一份** AppState 缓冲（读前快照 → 写后回读刷新），输入任务栈上不叠第二份
  ~600B 副本（`KEY STATS` 的 `input_stack_free` 是证据口）。不再只写 "PHASE 4" 提示。
- **键盘**：焦点环 = `T1,T2,T3,DRM L1..L16`（↑/↓；从 T3 下进入 L1、从 T1 上进入 L16、
  L1 上回 T3、L16 下回 T1），`ENTER` = 切换聚焦步的选中 lane 位。lane 是 UI 局部态
  （`page_step.c` 静态字节，跨组重建保留），console `UI LANE [<0..15>]` 读写同一状态
  供脚本断言；非 STEP 页 ENTER/↑/↓ 语义不变（FILE LOAD / 焦点轨）。
- **TAP 回显**：`-> STEP drum step=k lane=n bit=b CMD_DRUM_STEP_WRITE mask=0xNNNN`
  （`sloop_tap_result_t` 的 a=lane b=绝对步 v=mask d=bit err=0）。mask/bit 是**写后
  回读**的状态层真值；写入被拒（诊断冻结等）改回
  `-> STEP drum step=k lane=n ERR <frozen|invalid|blocked|full|error> mask=0xNNNN`，
  其中 mask 是未变的真值——不按本地翻转结果伪造成功（设备用例：`ENG FREEZE ON` 后
  TAP 回 `ERR frozen mask=<写前值>`，`SEQ DSTEP` 不变）。
- **选中 ring 的范围**：白 ring 画在**每个**鼓格的同一 lane 上，不只焦点格——扫一行
  就能看到「下一位会落在哪个 lane」（点格内空白正按选中 lane 写位）；代价是 16 个白框。
- **鼓行观感取舍**：鼓格底色固定「空格」样式（活动只由 lane dot 表达），换取 NORM
  橙色 dot 与格底的对比；失去的是旧版「整格填满鼓轨色 = 该步有 hit」的远看提示。
  逐 lane 可读性优先（spec §4.4 的深度编辑目标）。
- **触摸几何**：`DRUM_GRID_X0/Y0=8/30`、`DRUM_LANE_PITCH=14`、dot 8×8（格内坐标），
  注入脚本 `drum_lane_xy()` 与建/画用同一组宏。

### 4. 容量与验证

- STEP est 保持 **23,040B**；实测 delta 应只比旧值（21,788B）多 1 个 overlay 对象量级。
  若超 est：先减字形/对象，不调大 est 或池（ADR-008 规则）。
- 设备证据：`step_edit` 场景 ②e/②f（TIE≠NOTE 空心、S/F/m±/xN/HARD 像素；2 音符和弦
  上 CONT 在 k=1 的标记必须落在 k=1 名字下方（x+38 起）且 k=0 名下无横线、也无旧实现的格左缘竖条，引擎真值
  `flags==0x10 note=62,60`；鼓 dot 选中+位图往返、键盘 lane 环与 ENTER；`ENG FREEZE ON`
  的 TAP 回 `ERR frozen` 且位图不变；窗口 len=8 的隐藏鼓列 TAP miss；合成轨 ENTER 无
  op；`UI LANE` 越界回 `UI ERR lane`）与 `nav` ②（鼓轨 TAP 写位图、焦点落鼓轨、ENG 只有
  该 dstep 行变化）；PNG 逐像素区扫描 + 人眼视觉读图（像素扫描不能代替人眼），证据按
  §5.4 晋升。
- 未覆盖：CONT 的**演奏时序**（是否真的持续发声）属 host `seq` 门禁与听感项，本页只
  呈现标记；鼓 lane 的 vel（lvl/rat 编辑）留给 TRANSPORT 聚焦步工具（Task 10）。

## TRANSPORT 录音、fill 与 pattern 工具区（P4-C Task 10，2026-11-22）

走带三件套（PLAY/STOP/BPM±）保持；**4 个走带按钮也收敛为单对象可点击 label**，新增区全部是**单对象可点击 label**
（18 个 chip：3 录音/fill + 6 聚焦步字段 + 9 pattern 工具），不引入
`lv_button + 子 label`（对象数翻倍，预算装不下）。

### 1. 布局（页面区相对坐标；绝对 y = 页内 y + 72）

| 区 | 几何 | 内容 |
|---|---|---|
| 走带状态 | (48,264) 28pt 单行 | `PLAYING`/`STOPPED`；有工具动作后追加 `   <动作>: <结果>`（失败红字、N/A 黄字、其余主文本色）——**不新增状态行对象**；`status="…"` 的 console 回显与屏幕**同源**（`sloop_ui_transport_status_text`） |
| 聚焦步摘要 | (48,320) 20pt 两行 | 第一行 `TRACK n STEP pos / len`（**选中轨**的播放头/LEN）；第二行 `T<ft+1> S<abs+1> LVL N RAT x1 MIC +0 SPAN 1 SLD -`（鼓轨 `T4 S<n> DRM L<lane> …`）——`T<ft+1>` = **焦点轨**、`S<n>` = 引擎绝对步（**1 起**，与 STEP 页列号同口径；第一行的 `STEP pos / len` 是 step_pos 原值 = 0 起）（工具的目标；与第一行的选中轨可以不同）；label 颜色 = **焦点轨色**（工具写的那条轨） |
| 录音/fill | y=384，56 高 | REC (48,140)：五态文本 `REC OFF`/`REC ARM`/`REC ON T<n>`/`REC CNT T<n>`/`FREE TAKE` + 底色（黄 arm / 红 record / 橙 count-in / 绿 free take）；take 结果（`TAKE n BAR` / `TAKE DROP`）latch 在**同一 chip**（仅 OFF 时显示，下一次 REC 动作清除）——不占用工具状态行，避免与同一拍的动作结果互相覆盖；FILL (200,120) `FILL`→`FILL ON`（黄底）；NEXT (328,120) `NEXT`→`NEXT ARM`（黄底） |
| 聚焦步字段 | y=464，88×56、间距 8（x=24+i·96） | `LVL` `RAT` `MIC-` `MIC+` `SPAN` `SLIDE`；SLIDE 打开时黄底 |
| pattern | x=792 起 | y=292 `ROT-`/`ROT+`/`LEN x2`/`LEN /2`（100 宽）；y=360 `TRANS-`/`TRANS+`（120）+`UNDO`/`REDO`（84）；y=428 `CLEAR T<n>`（432，破坏性动作单独一行，文本显示目标轨=选中轨） |
| BPM | 既有 (900,40)/(880,72)/(860|1000,140) | 不变 |

### 2. 动作（一个 chip 一条 EditCmd；控件回调/TAP/键盘共用一个 `tr_action`）

| chip | 动作 | EditCmd |
|---|---|---|
| REC | 切换 | `CMD_REC_TOGGLE`（状态机在上游：arm/off/record close） |
| FILL | 保持电平切换 | `CMD_FILL_CONTROL a=1-held` |
| NEXT | 脉冲 | `CMD_FILL_CONTROL b=1`（只翻转 `fill_arm`，不读写 held） |
| LVL | 响度循环前进（GHOST<SOFT<NORM<HARD） | `CMD_STEP_EDIT op=lvl`（鼓轨 = `CMD_DRUM_STEP_WRITE`，写选中 lane 的 2-bit 字段） |
| RAT | x1→x2→x3→x4→x1 | `CMD_STEP_EDIT op=rat`（鼓轨同上） |
| MIC-/+ | nudge ±1（-32..31 UI 钳位） | `CMD_SET_MICRO` |
| SPAN | 目标长度 +1（到本轨 LEN 回 1） | `CMD_STEP_EDIT op=span` |
| SLIDE | 切换 | `CMD_STEP_EDIT op=slide` |
| ROT± / TRANS± | ±1 | `CMD_PATTERN rotate/transpose`（选中轨） |
| LEN x2 / LEN /2 | ×2 或 ÷2 | `CMD_PATTERN length arg=±1` |
| CLEAR / UNDO / REDO | — | `CMD_PATTERN clear/undo/redo`（选中轨） |

- **值一律写后回读**（TAP 回显 `value=` 与屏幕状态行同源）：失败时报写前真值 + seq_status，
  不按本地计算结果伪造。`CMD_FILL_CONTROL` 是唯一例外：状态层不重取 rec 投影（`sloop_seq_rec_read`
  会消费一次性 take 结果），回显用命令写入的电平/翻转后的待命。
- **LVL/RAT 作用于聚焦步的第 1 个音符**（`index=0`）：STEP 页没有音符光标；**SPAN 以可见
  TIE 链长度为基准**（POLY 长音在后续和弦下方的 CONT 延伸不在快照里）。两条都记入
  `docs/deferred-issues.md` DI-30，属已知近似而非协议偏差。
- 鼓轨：LVL/RAT 改**选中 lane**（STEP 页 `UI LANE`/↑↓/点 dot 的同一字节）；SPAN/SLIDE
  不适用——不发命令、回 `ignored (not_synth)`、芯片灰字；TRANSPOSE 由上游拒绝（`ERR invalid`）。
- **触摸只保留常用方向**：LVL/RAT 是循环按钮、SPAN 只 +1（到 LEN 回 1）；反向由键盘
  ↑/↓ 提供（见 §3）。MIC 保留 ± 两个 chip（nudge 双向是常用操作）。

### 3. 键盘（ADR-006）

TRANSPORT 页：`←/→` 在 8 个工具字段环上移动（LVL,RAT,MIC,SPAN,SLIDE,ROT,LEN,TRANS），
`↑/↓` 调焦点字段（LVL/RAT 双向循环、MIC ±1、SPAN ±1、SLIDE 切换、ROT ±1、LEN ×2/÷2、
TRANS ±1），`ENTER` = +1/切换——与参数页「焦点 + ↑/↓ 调值」同一模型。焦点画成该字段
chip 的 3px 白描边，是 UI 局部态（跨组重建保留）；console `UI TOOL [<0..7>]` 读写同一状态
供脚本断言。触摸点任一字段 chip 也把焦点落过去。

### 4. 回显（console `TAP`；脚本断言口）

`-> TRANSPORT <CMD> <op> value=<写后回读>`，例如
`-> TRANSPORT CMD_STEP_EDIT lvl value=3` / `-> TRANSPORT CMD_DRUM_STEP_WRITE rat value=1` /
`-> TRANSPORT CMD_PATTERN rotate value=1` / `-> TRANSPORT CMD_FILL_CONTROL hold value=1`；
失败回 `-> TRANSPORT <CMD> <op> ERR <not_found|invalid|full|blocked|frozen|error> value=<写前真值>`，
鼓轨上的 span/slide 回 `-> TRANSPORT CMD_STEP_EDIT span ignored (not_synth) value=0`。
令牌与 STEP 鼓格共用 `seq_status_token()`（同一 `app_seq_status_t` 词表）。

### 5. 容量与验证

- **est 9,216B（计划上限），实测 delta 8,248–8,716B**（21 次首建读数）（`UI MEM`：`页面构建 page=TRANSPORT(3)
  池增量=8668B 预算=9216B 池剩余=38172B 耗时=12ms`）。18 个新对象 + 既有 8 个对象
  = 26 个对象（旧的 4 个 `lv_button` + 4 个子 label 也收敛成 4 个单对象大 chip）。
  余量 +5.7%（对最大 8,716）低于 ADR-008 常态 +10–23%——这是**计划锁定的上限**（SEQ→PRESETS 有序对
  的账面余量只剩 168B），与 FILE 两页（+4.4%/+5.5%）同一处理方式：不抬 est/池，靠
  `free ≥ est` 与实测峰值门禁兜底。
- 设备证据：`transport` 场景 ⑩a–⑩i（REC 五态、fill 两变量、LVL/RAT 循环、MIC ± 与 31
  钳位、SPAN TIE 往返、SLIDE、空步 `ERR not_found` 且逐字段不变、鼓 lane 读改写与
  `not_synth`、pattern 六动作、鼓轨 TRANSPOSE 拒绝、`LIVEHASH` 快照还原、键盘焦点环与
  ↑/↓/ENTER、`UI MEM` 的 est/delta 与 `GROUP SEQ sum_est=32,256`、截屏像素抽检）。
- 未覆盖：人眼视觉读图（本会话无图像查看工具，用 §5.4 的像素区扫描代替，见
  `task-10-visual-check.txt`）；物理触摸手感与长按/连发不在 TAP 覆盖范围。
