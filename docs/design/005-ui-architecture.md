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

## 后果

- 阶段 4 全页面在此架构上扩展（页面路由+控件复用）；阶段 5 将 `state_apply` 的执行端换成 FM-1 会话即可复用 UI。
- 引擎保持单例+互斥；UI 输入路径增加 input 任务（≤20ms 延迟目标）。
