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
  `state_set_focus()` + `CMD_TOGGLE_STEP`（绝对步号；鼓轨第 4 行只读，展示 `dstep[c].on`
  窗口聚合，点击写状态行提示不发命令）。
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
| STEP | 建格同一组宏 `GRID_X0/Y0、CELL_W/H、CELL_STEP_X、ROW_PITCH` 现场推导（行/列各一次区间判定） | `step_activate(r,c)`（`cell_clicked_cb` 的唯一调用体） | `state_set_focus()` + `CMD_TOGGLE_STEP`；鼓轨行只写状态行、不发命令 |
| TRACKS | `CARD_X0/Y/W/H/PITCH` | `tracks_select(k)`（`card_clicked_cb` 的唯一调用体） | `CMD_SELECT_TRACK` |
| TRANSPORT | 静态表 `{x,y,w,h,kind,v}`，几何取自建按钮的 `TRANSPORT_*` 宏 | `transport_play()` / `transport_stop()` / `transport_bpm_step(delta)` | `CMD_PLAY` / `CMD_STOP` / `CMD_SET_BPM` |
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

顶层 5 组（`SYS` 待 P4-E 有设置页时再加入）：

| 组 | 子页 |
|---|---|
| PLAY | PLAY |
| TRACKS | TRACKS |
| EDIT | GLOBAL / FX / ANALOG / FM6 / SAMPLE（后续引擎页） |
| SEQ | STEP / TRANSPORT |
| FILE | PROJECTS / PRESETS |

- `app_page_t` 追加 `APP_PAGE_PROJECTS, APP_PAGE_PRESETS`，`APP_STATE_PAGES` 9→11；
  console `UI` 页名表、`UI MEM` 的组字段、注册表顺序同源。
- 页签 = 组签（上行）+ 组内子签（下行）；注册顺序 = 组顺序，组内顺序 = 子页顺序。

#### 2. 两行页签复用现有 72px

- 组行约 34px、子行约 30px（16pt 标签约 20px 高）；**页面区起点仍是 y=72，现有九页布局
  零改动**（参数页 `52+2×298=648` = `UI_H−72`、STEP 底行等像素级布局不重排）。
- 页签命中几何：`TAP` 仍按 y<72 判页签区；组签/子签各自的矩形由同一组布局宏派生。
- 右侧 220px 继续留给 `SLOOP` 标题（上行）与层键提示（下行）；子行只有 1 页的组留空。
- 组行 5 个页签：`min(176, (1280 − 32 − 220 − 4×8)/5) = 176px`（最长标签 `TRACKS` 远小于
  页签宽）；子行按组内页数等分。**不再受 N=10 时 95px < `TRANSPORT` 101.6px 的宽度墙
  约束**——组签只有 5 个。

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
PLAY → TRACKS → GLOBAL → FX → ANALOG → FM6 → SAMPLE → STEP → TRANSPORT → PROJECTS → PRESETS → PLAY
```

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

- `base` = 非页面基础占用（根/两行页签条/错误状态条/字体/LVGL 内部等），**P4-B Task 11
  两行页签落地后实测 13,800B**（＝ 63,120 − boot `UI MEM` 的 `pool_free 46,840` − PLAY
  delta 2,480，最终固件）。九页平铺页签的 boot 基线同法为 13,196B，故两行页签的代价
  ≈ **+604B**（第一版子行建 8 个复用槽时 15,648B；收敛为 5 槽后回落到 13,800B）。
  「UI 就绪」那一拍的瞬时值 14,112B（含启动期临时分配，差 0.3KB）可作保守上界。
  本节的旧值 7,016B（= `pool_used 55,064 − Σdelta 48,048`）用 `pool_used` 会漏计
  TLSF 块头，是低估，已作废；两行页签后已按本值重测回写。
- 组 est：PLAY 3,072 / TRACKS 7,168 / EDIT `3×3,584 + 6,144 + 3,072 = 19,968`
  （FM6 自 P4-B Task 13 起含 bank 存取行，est 3,584 → 6,144，实测 delta 4,956–5,100B）/ SEQ
  `STEP 23,040 + TRANSPORT 4,608 = 27,648` / FILE `PROJECTS 8,192 + PRESETS 12,800
  = 20,992`（P4-B Task 12 按实测定稿：首次构建 delta 7,764B / 12,260B，est = +5.5% /
  +4.4%、512 对齐；初版 4,608/5,120 是**下限估算**，实测对象数（26/36 个）远超，见
  ADR-008 §3 的 FILE 表。构建前实测 est 以 ADR-008 表为准）。
- **有序对按当前 est 表重算**（每条 = 源组全建 Σest + 目标组首建页 est；账面 =
  `base 13,800 + 对值 + 4,096`，不变量 ≤ 63,120）：
  1. **FILE 两页全建 → SEQ 首建 STEP（最坏）**：`20,992 + 23,040 = 44,032B` ⇒ 账面
     **61,928**、余 **1,192B**（base 取保守上界 14,112B 时 62,240、余 880B）；
  2. **EDIT 全建 → SEQ 首建 STEP**：`19,968 + 23,040 = 43,008B` ⇒ 账面 **60,904**、
     余 **2,216B**（保守 base：61,216、余 1,904B）——Task 13 的 FM6 bank 行把 EDIT 从
     17,408 抬到 19,968，它从旧「次坏并列（40,448）」升为单独次坏；
  3. **SEQ 全建 → FILE 首建 PRESETS**：`27,648 + 12,800 = 40,448B` ⇒ 账面 **58,344**、
     余 4,776B；
  4. 反方向 SEQ（全建）→ EDIT 首建 GLOBAL：`27,648 + 3,584 = 31,232B` ⇒ 账面 49,128。
  （STEP 属 SEQ，所以它不可能是「SEQ 常驻时」的跨组目标；但可以是 EDIT→SEQ /
  FILE→SEQ 的目标——最坏对必须按有序对枚举，旧式「任意单页上限 + 任意最大组」是
  病态上界。）
- **实测过渡峰值**（构建日志的「池剩余」= 销毁源组之前的 free）：
  2026-11-22 Task 11 最终固件：EDIT 五页全建 `pool_free 34,212B` → 跨组建 STEP
  （delta 21,856B、耗时 30ms）后 **`pool_free 12,356B`**；SEQ 两页全建
  `pool_free 23,376B` → 跨组建 GLOBAL（delta 2,848B）后 `pool_free 20,528B`。
  Task 12（FILE 组落地）：FILE 两页全建 `pool_free 28,424–29,112B` → 跨组建 STEP
  （delta 21,716–21,732B、31ms）后 **`pool_free 7,308–7,840B`**（四次运行）——当前最坏**实测**峰值
  （`store` 场景 ⓯ 断言 ≥4,096B），仍是 DI-19 弧线 mask 峰值（≈2.6KB，取 4KB 余量）
  的 2.8 倍。
- **为什么必须按峰值核算**：切组顺序是「建目标页 → 切到目标页 → 销毁源组页面」，源组的
  池占用在**切换之后**才释放；若在切换前释放，会出现空白帧、输入路径在跨组切换期间
  没有活页。因此峰值 = 源组已建 Σ + 目标页实际构建量，是瞬态但必须被上式覆盖；切换
  完成后回落为稳态 `base + max_group Σ est = 41,448B ≤ 63,120B`。
- **P4-C 12 页预算（规划，ADR-010 §5）**：SONG 使用**独立顶层组**而不是加入 SEQ，组
  顺序变为 `PLAY/TRACKS/EDIT/SEQ/SONG/FILE`；TRANSPORT est 上限调为 **9,216B**、SONG
  初估 **12,288B**，落地后均按 `UI MEM` 实测收紧。有序对重算：FILE→STEP 仍
  `20,992+23,040=44,032B`、账面 **61,928B**；SEQ（23,040+9,216）→PRESETS 为
  `32,256+12,800=45,056B`、账面 **62,952B**、余 **168B**，成为新的账面最坏对；
  SONG→PRESETS 为 `12,288+12,800=25,088B`、账面 **42,984B**。六个组签仍在 72px
  页签条内，实施时须重测 base 并保留 `free ≥ est`、过渡后 `pool_free ≥4,096B` 门禁；
  超预算先改对象设计，不扩池。本任务只确定预算，不注册 SONG 页、不改变当前 11 页。
  P4-C 的 `TAB` 规划顺序是在 TRANSPORT 后插入 SONG，其余顺序不变。
- **每页构建前 `free ≥ est` 的检查继续保留**（真门禁不变）；组作用域只是把「同时已建
  页集合」限制在一组内。
- `UI MEM` 增加分组行：`GROUP <name> built=<n> sum_est=<B> sum_delta=<B> build_ms=<ms>`，
  与既有 `page` 行并存（`sum_est` = 组内已注册成员 est，`sum_delta` = 已建成员池增量）；
  九页平铺的实测预算表保留在 ADR-008 §3。
- 池仍为 64KB（`CONFIG_LV_MEM_SIZE=65536`）；扩池路线不重启。
- 构建延迟（`build_ms`）：跨组进入 SEQ（STEP 首建）最终 nav 证据 **33ms**（早期会话
  29–35ms）、EDIT 首建 GLOBAL 7–11ms、PLAY/TRACKS 首入 ≤10ms；同组首建
  FX/ANALOG/FM6/SAMPLE/TRANSPORT 4–9ms——全部 `0 < x ≤100ms`。

#### 6. 验证入口

- `UI MEM` 的分组 Σ + 每页 `delta ≤ est`；跨组切换后源组 `built=0`（含原当前页）。
- `nav.sh` 按新顺序与新组语义更新并通过（含跨组销毁时同拍输入安全）。
- `scripts/inject/store.sh` 覆盖 FILE 组：`UI projects/presets` 截屏 + `TAP` 动作 +
  `UI MEM` 组水位断言（FILE Σ ≤ 预算、离开 FILE 后两页回 `UNBUILT`，含原当前页）。
- 九页既有截屏基线只允许页签区差异（页面区像素不变）；差异证据按 AGENTS §5.4 晋升。

#### 7. 落地（P4-B Task 11，2026-11-22）

- 组表 `k_groups[]`（ui.c）：PLAY{PLAY} / TRACKS{TRACKS} / EDIT{GLOBAL,FX,ANALOG,FM6,
  SAMPLE} / SEQ{STEP,TRANSPORT} / FILE{PROJECTS,PRESETS}。组签固定 5 槽（176×32 @ y=4），
  无已注册页的组隐藏并压缩排布（Task 12 起 FILE 已注册，5 个组签常显）；子签固定 5 槽
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
- 验证：`nav.sh` ①–⑦（⑤ 组作用域：EDIT→SEQ 后 EDIT built=0、同组只改显隐、
  build_ms `0 < x ≤100ms`；⑥ 手指导航：点组进上次子页且记忆跨往返、跨组点组销毁
  源组并建目标首个子页、子签同组切换不重建、同拍 `KEY tab` + `UI TAB` 点击胜出；
  ⑦ TAB 全 11 页轮转 `…→TRANSPORT→PROJECTS→PRESETS→PLAY`，异步切页用
  `STATE SNAP` 轮询等落地）；`params.sh` 的池口径改为组作用域（同时只常驻一个组，
  `GROUP EDIT sum_delta` + `pool_free ≥20KB`）；DI-19 由「store→params 不重启」现场收口。

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
