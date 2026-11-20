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
- **LVGL 内存纪律**：对象共用静态 `lv_style_t`（不给对象加局部样式）+ 卡片内文本合并，把 64KB 内置
  TLSF 池占用从首版的 ≈97%（文本重分配即可能失败）压到 ≈68%；池扩容到 96KB 会撑爆内部 RAM（实测
  链接期 `.bss` 溢出），故不走改配置路线。`sloop_ui_start()` 就绪行固定打印池占用，且该采样（`lv_mem_monitor`）在 `bsp_display_lock` 内执行——30ms 定时器此时已创建，无锁遍历 TLSF 池会读到撕裂的块结构。

## 后果

- 阶段 4 全页面在此架构上扩展（页面路由+控件复用）；阶段 5 将 `state_apply` 的执行端换成 FM-1 会话即可复用 UI。
- 引擎保持单例+互斥；UI 输入路径增加 input 任务（≤20ms 延迟目标）。
- 阶段 3 MVP 的选轨与逐轨 mute/solo 暂由状态层持有（引擎暂无写入口）：音频效果与串口断言局限见 `docs/deferred-issues.md` DI-9（串口用 `STATE SNAP`，不能用 `ENG SNAP`）；引擎写入落地后删除该特例。
