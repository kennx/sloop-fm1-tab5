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

`sloop_ui` 组件已落地骨架：单容器屏 + 顶部页签 `PLAY|TRACKS|STEP|TRANSPORT`（页对象创建一次、
切页只改显隐）、30ms 定时器（`state_sync_from_engine` + 逐页脏检查）、`sloop_ui_switch_page()` 与
console `UI <page>`。全部 LVGL 调用走 `bsp_display_lock`（esp_lvgl_port 递归锁，页签回调/定时器已在
LVGL 任务上下文）；UI 启动失败回退 bring-up 测试图。idle 内部堆门禁实测 ≈120.0KB（≥60KB）。

四页均已落地（Task 4：PLAY/TRANSPORT；Task 5：TRACKS/STEP）：

- **TRACKS**：四轨卡片横排，轨主题色 outline 描边（选中 6px + 亮底，未选中 3px），卡内
  `TRACK n / ENG k PRE p / LEVEL v / MUTE x SOLO y` + 电平条；点击卡发 `CMD_SELECT_TRACK`
  （`cmd_select_track` 同时写 `selected_track` 与 `focus_track`）。
- **STEP**：4 行 × 16 步网格，行=轨色；格「亮」=`state_step_is_on()`（n>0 且 time==NOTE，与状态层
  切换共用同一判定）；播放中当前步描 `UI_ACCENT`、焦点格描 `UI_TEXT`；点格 `state_set_focus()` +
  `CMD_TOGGLE_STEP`（鼓轨第 4 行只读，展示 `dstep[i].on` 聚合，点击写状态行提示不发命令）。
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
并返回非零）；`scripts/inject/` 三条场景脚本即以此对真机取证。

TAP 在 console 任务里取 `bsp_display_lock`（命中后会写状态行并立即刷新本页）；锁序
（LVGL → state → engine）与控件回调一致，无反转。落表外不写任何状态、不崩溃。

## 触摸准星的角色收敛（Task 7）

bring-up 的触摸十字准星画在 `lv_layer_top()`，会盖住生产页面并出现在截图里。新增
`app_display_touch_feedback_enable(bool)`：`sloop_ui_start()` 成功后由 `main.c` 关闭
（准星 + 坐标日志），UI 启动失败（回退测试图）时保持开启。DI-10 的幽灵按压属 indev
事件问题，不在本处修复——关闭的只是可视反馈。


- 阶段 4 全页面在此架构上扩展（页面路由+控件复用）；阶段 5 将 `state_apply` 的执行端换成 FM-1 会话即可复用 UI。
- 引擎保持单例+互斥；UI 输入路径的 input 任务已落地（`sloop_ui/src/input.c`：5ms 轮询
  `tab5_kb_poll`，优先级 4，层模型见 ADR-006）；串口 `KEY <code> <down|up>` 与物理同入口，
  实测派发延迟（含 state_apply 与 PLAY 指示入队）均值 ≈0.51ms、note-on 最大 ≈1.37ms（note-off 最大 1.32ms；Task 6，见 verification-log）。切页走 `sloop_ui_request_page()` 邮箱（派发 22–72µs），输入路径不再等 LVGL 锁。
- 阶段 3 MVP 的选轨与逐轨 mute/solo 暂由状态层持有（引擎暂无写入口）：音频效果与串口断言局限见 `docs/deferred-issues.md` DI-9（串口用 `STATE SNAP`，不能用 `ENG SNAP`）；引擎写入落地后删除该特例。
