# ADR-008: 页面懒建与 LVGL 池预算（阶段 4 参数页的前置框架）

> 状态：已接受（阶段 4，P4-0 Task 4）｜ 日期：2026-11-21
> 关联：ADR-005「阶段 4 容量决策」；AGENTS §5.4.1（console 注入约定）、§5.5（门禁）；
> 代码：`firmware/components/sloop_ui/src/ui.c`、`include/sloop_ui.h`、`src/theme.h`、
> `src/page_*.c`、`firmware/main/console.c`；证据：`docs/dev/verify/4-p0-2026-11-21/task-4-*`
> 与 `docs/dev/verification-log.md`（阶段 4 / P4-0 Task 4）。

## 背景

阶段 3 的四页（PLAY/TRACKS/STEP/TRANSPORT）在 `sloop_ui_start()` 里一次性建完。
**阶段 3 收口归档**的启动行（`docs/dev/verify/3-mvp-2026-11-20/task-7-boot-log.txt`）为
池已用 **40,532 / 59,632** 字节（≈68%）；本任务父提交（`4-p0-2026-11-21/task-2-boot-log.txt`，
阶段 4 起点）为 40,712 / 59,612。**本任务在懒建下四页全建后重测**为
40,124 / 59,772 字节（≈67%，`task-4-ui-mem-transcript.log`；free 19,648），三组数不是同一
固件/时点的同一口径，引用时须区分。池剩余约 19.6KB，阶段 4 要把 192 个参数摊到多个参数页
（GLOBAL/MASTER/引擎组…），若沿用「启动即建全部页」，第一个参数页落地就会撞池顶。
ADR-005 已登记该容量决策必须在第一个参数页之前完成。

池本身不能扩：`CONFIG_LV_USE_STDLIB_MALLOC=0`，池是静态 `.bss`（`CONFIG_LV_MEM_SIZE=65536`，
TLSF 可用 63,120 字节）；阶段 3 实测扩到 96KB 会撑爆内部 RAM（链接期 `.bss` 溢出）。

## 选项

| 选项 | 评估 |
|---|---|
| **A. 注册表 + 首次访问懒建 + 每页预算（采纳）** | 启动只建 PLAY（开机非空白），参数页首次进入才分配；每页有实测预算表与「池剩余 < 预算即拒绝」的前置检查；失败可见（错误状态条 + ERROR + console 令牌）；不改 LVGL 配置、不动现有页布局 |
| B. 池扩容到 96KB+ | 阶段 3 已实测链接溢出；须先找到内部 RAM 减项（字体/缓冲），代价与风险都不在本任务可控范围 |
| C. 跨页共享控件/样式池 | 参数页结构差异大、抽象成本高；且收益（同构控件）要等参数页成型后才可评估 |
| D. 离页销毁已建页 | 能省池，但切页要重建、焦点/滚动状态需随页保存，且每次切页都有分配失败面；阶段 4 参数页数量已知（可预算），暂不需要驱逐策略 |

## 决策

A. 落地形态：

### 1. 页面注册表（`ui_page_t`）

```c
typedef struct {
    uint8_t id;                              /* = app_page_t，< APP_STATE_PAGES */
    const char *title;                       /* 页签名 */
    lv_obj_t *(*build)(lv_obj_t *parent);    /* 成功返回 parent；任一控件分配失败返回 NULL */
    void (*update)(const app_state_t *st);   /* 只对该页已建控件做脏检查刷新 */
    uint32_t est_bytes;                      /* 构建前预算门槛（池剩余低于此值拒绝构建） */
} ui_page_t;

esp_err_t ui_page_register(const ui_page_t *page);  /* start 之前注册；id 唯一 */
```

- 注册顺序 = 页签顺序；页签文本/数量由注册表派生（4 页时宽度仍是 176px，像素不变；
  注册页更多时按可用宽度收窄，不溢出到 SLOOP 标题/层提示区）。**注册表只增不删**：
  外部组件可在 `sloop_ui_start()` 之前调 `ui_page_register()` 预注册页面，内置页注册
  幂等（`static done` 标志 + 按 id 去重），不会重置/清空已注册项——预注册页保持原位
  （页签顺序在 PLAY 之前）。输入层的 tab 轮转按 `sloop_ui_page_count()`（注册表计数），
  不按 `APP_STATE_PAGES`，与页签数量同源。
- 现有四页在 `sloop_ui_start()` 内注册（`k_builtin_pages`）；`ui_page_register()` 供阶段 4
  的参数页加入同一注册表。`id` 受 `APP_STATE_PAGES` 约束（状态层 `state_set_page`、input
  的 tab 轮转、console 页名表同源）；加页时一起扩 `APP_STATE_PAGES`。

### 2. 懒建策略

- 页对象（透明容器 + 页内控件）在**首次进入**时创建，之后只改 `LV_OBJ_FLAG_HIDDEN`，
  **不销毁**（切页零分配、焦点/滚动/状态行都保留）。
- PLAY 在 `sloop_ui_start()` 内立即构建：开机首帧非空白（实测首帧与阶段 3 PLAY 基线
  逐字节相同）。其余三页首次访问时构建。
- 构建成功后立即按当前 AppState 刷新一次，首帧不等 30ms 定时器（切页/截屏时序确定）。
- 30ms 定时器只刷新**已建**页；未建页不参与脏检查。
- 构建函数必须复位本页脏检查缓存（重建路径要求）：`page_*_build()` 开头复位
  cache/`s_valid`/`s_header_dirty`，否则重试后控件是全新的、缓存是旧的，首帧会停在硬编码文案。
- 单页构建失败**不**回退测试图：框架（根容器/页签/定时器）可用时 UI 继续服务，
  错误在页面区可见；只有框架级分配失败才 `sloop_ui_start()` 返回 `ESP_ERR_NO_MEM`。

### 3. 预算表（实测）

测量方法：`ensure_page_built()` 在构建前后各取一次 `lv_mem_monitor()`，delta = 池
`free_size` 的减少量（含 TLSF 块头元数据）。不用 `total_size - free_size` 的差：
`lv_mem_monitor` 的 `total_size` 只累计块负载、不含块头，用它算 delta 会低估每次分配。
**采样点**在 `build()` 返回之后、首帧 `update()` 之前：不含首帧文本分配（量级为文案
长度）。`delta_bytes` 的语义是「**最近一次**成功构建」（重建成功会覆盖旧值）。

| 页 | 实测 delta（`UI MEM`） | est_bytes（预算） | 余量 | 说明 |
|---|---|---|---|---|
| PLAY | 2,492 B | 3,072 B | +23% | 6 标签 + 1 电平条 + 音符指示盒（7 对象） |
| TRACKS | 6,080 B | 7,168 B | +18% | 4 卡 × 4 子对象 + 4 电平条 |
| STEP | 21,760 B | 24,576 B | +13% | 75 对象（64 格 + 标签）+ 12 个共享 `lv_style_t` 的实例引用 |
| TRANSPORT | 3,744 B | 4,608 B | +23% | 6 按钮（含标签）+ 4 标签 |
| **合计** | **34,076 B** | **39,424 B** | — | 四页全建后池 free=19,648 B / total=59,772 B |

> 2026-11-21 P4-0 Task 5（步窗）后复测：STEP 首次构建 `池增量=21832B 预算=24576B`
> （列号改为单行绝对步号后 TLSF 分配序变化，对象数不变；delta ≤ est 仍成立），
> 其余三页未改。预算表与规则不变。数据出处：`4-p0-2026-11-21/task-5-inject-step_edit-run1.log`
> （首跑归档件；同名主日志已被修复轮第二跑覆盖）。

预算规则：

- `est_bytes` = 实测 delta × 1.13–1.23、512 对齐；预算是**构建前门槛**，不是精确账本。
- **Σ est_bytes < 池可用总量**（现 39,424 < 59,772，留 ≈20KB 实际余量）。这是「任意一页
  都能在其它页已建之后仍被允许构建」的**充分条件**（其它页实际占用 ≤ Σ其它 est ≤ Σ其它
  预算），不是必要条件：实际占用小于 est 的页即使 Σ est ≥ 池也可能构建成功——预算门槛
  只是保守拒绝，不承诺「Σ est ≥ 池即必然失败」。
- 构建前检查 `free_size < est_bytes` → 直接拒绝（`原因=budget`），不做半途分配：
  池碎片化时中途失败的代价（半成品容器 + 后续分配面更小）比拒绝更高。
- 参数页落地后增表行；`UI MEM` 的 `sum_est`/`reserve` 行是现场校验入口。
- 超出预算的页面（或 Σ est 逼近池总量）**必须先做设计决策**（拆页/共享样式/对象复用/
  驱逐策略），不得直接调大 est 或调大池——后者已实测把内部 RAM 撑爆。

### 4. 失败可见性（不静默空白页）

- `build()` 返回 NULL（页内任一控件分配失败，`UI_BUILD_CHECK` 逐个检查）→
  删除半成品容器（回收已分配对象）→ 标记 FAILED → ERROR 日志：
  `页面构建失败 page=STEP(2) 原因=build|budget|forced 池剩余=… 预算=…（UI ERR page_build 2）`。
- 页面区显示错误状态条（`UI_ERROR` 色、两行居中）：`STEP PAGE BUILD FAILED` +
  `UI ERR page_build 2 (build|budget|forced)`。页签仍可用、其它页不受影响。
- 同步切页返回 `ESP_ERR_NO_MEM`；console `UI step` 打印 `UI ERR page_build 2` 并返回非零
  （与 `UI ERR lock_timeout`/`UI ERR not_started` 分开，脚本不得互相误判）。
- 失败页下次访问重试（池可能已因其它页恢复/变化而有余量）；TAP 在未建/失败页一律
  `TAP miss`（不碰页内静态控件指针）。
- **错误状态条自身也可能分配不出文本**（DI-13 家族）：`create_page_error_label()` 建不出
  标签时按框架级失败回退 bring-up 测试图（非空白）；标签存在但 `lv_label_set_text_fmt`
  的文本缓冲分配失败时，只剩 ERROR 日志 + console 令牌——画面是无控件的页面区，不是
  「静默成功」。此路径无法从 API 判定，登记 DI-13 跟踪。
- `UI FORCEFAIL` 语义（调试）：未建页挂起到下一次构建（回显 `state=unbuilt`，注入在
  构建尝试时消费）；已建**当前页**删除控件并立即重建（注入当场消费，回显 `state=failed`）；
  已建**非当前页**只删除控件、回到未建态（回显 `state=unbuilt`），注入保持待命，下次访问
  时构建失败。
  `off` 只撤销尚未消费的注入，**不会**重建已失败页（失败页在下次访问时重试）。
- **已知边界**：`build()` 的失败判定覆盖「对象分配」（`lv_*_create` 返回 NULL）；
  LVGL 标签文本缓冲等二级分配发生在 API 内部、无返回值，不在判定内（登记 DI-13）。
  预算前置检查（est = 实测 + 余量）与共享 `lv_style_t` 纪律是这一层的防线；若后续发现
  文本分配失败（池顶症状），按「超预算页面」走设计决策，不加静默重试。
- 验证入口：`UI FORCEFAIL <page> [on|off]`（console，调试命令）——让该页**下一次**构建
  强制失败（一次性，构建尝试后自动解除）；对已建**当前页**则删除并立即重建到失败态
  （已建**非当前页**只删除控件回到未建态、注入待命），
  从而同时验证「重建路径 + 缓存复位」。实测：失败态 ERROR/console/截屏三处一致，
  `UI FORCEFAIL off` 或下一次访问即恢复，恢复帧与正常帧逐字节相同。

### 5. 池上限与不变量

- 池配置保持 `CONFIG_LV_USE_STDLIB_MALLOC=0` + `CONFIG_LV_MEM_SIZE=65536`（TLSF 可用
  63,120B）。不改配置：96KB 实测链接期 `.bss` 溢出。
- 不变量（`UI MEM` 可复核）：
  1. `pool_total - sum_est > 0`（当前 20,348 B）；
  2. 每页 `delta <= est`（当前全部成立，余量 13–23%）；
  3. 四页全建后 `pool_free > 0`（当前 19,648 B）；
  4. 参数页落地后重新核对以上三条并把新行写入本表。
- 阶段 4 参数页的硬性规则：
  1. 每页一个注册表条目，`est_bytes` 必须来自本机 `UI MEM` 实测（同方法），不得拍脑袋；
  2. `build()` 内每个 `lv_*_create` 用 `UI_BUILD_CHECK()` 检查，并复位本页脏检查缓存；
  3. 共享 `lv_style_t`（状态变化用 `lv_obj_replace_style` 换指针）是既有纪律，参数页沿用；
  4. 超出预算或 Σ est 逼近池总量 → 先写设计决策（可新增 ADR 或修订本 ADR），不得只改数字；
  5. 禁用「离页销毁」做为省池手段（会引入切页分配面与状态保存问题）；若参数页规模超出
     懒建能覆盖的范围，另立 ADR 讨论驱逐/分页。

### 6. 统计接口

`sloop_ui_page_pool_stats(&stats)`（`sloop_ui.h`）：池 used/free/total、注册/已建/失败页数、
按注册顺序的每页 `state`/`est`/`delta`。console `UI MEM` 输出：

```
UI MEM pool_used=40124 pool_free=19648 pool_total=59772 registered=4 built=4 failed=0 sum_est=39424 reserve=20348
UI MEM page id=2 title=STEP state=built est=24576 delta=21760
```

## 迁移与回归（不改渲染）

- 四页从 `page_*_create()` 迁到 `page_*_build()`：布局常量、控件属性、绘制顺序全部未动，
  仅在构建路径加分配检查与缓存复位。
- 逐字节回归（`docs/dev/verify/4-p0-2026-11-21/`）：
  - PLAY / TRACKS / STEP / TRANSPORT 四页与阶段 3 证据逐字节相同（见 task-4-readme 表格）；
  - 另在改动前后固件（`5bbb325` vs 本任务）上做同状态 A/B：PLAY（稳定态）、TRACKS、STEP、
    TRANSPORT 四页 PNG 逐字节相同（STEP 同状态 A/B 证明「懒建不影响像素」；
    `3-mvp-2026-11-20/task-5-step-empty.png` 的差异只在底部提示行，因为该归档早于
    `a2043cf` 的提示文案更新，不是本任务引入）。
  - 三条注入脚本 `scripts/inject/{transport,step_edit,tracks}.sh` 退出码 0，
    截图与阶段 3 基线（`task-8-transport.png` 9608ad10、`task-8-fix2-inject-step-two-on.png`
    c717fa8c、`task-8-tracks-sel3.png` b0a188cf）逐字节相同。

## 后果

- 阶段 4 参数页可以「按页付池费」：未访问的参数页不占池；每页有明确预算与失败出口。
- 切页首次进入有一次构建延迟（毫秒级；PLAY 例外）。当前没有页面销毁/驱逐机制；
  若参数页总预算超出池（Σ est 逼近 63KB），必须回到本 ADR 的设计决策，而不是调大池。
- 池水位从「启动常量」变成「随访问增长」，soak 门禁（AGENTS §5.5）仍需在参数页落地后
  重跑：`UI MEM` 的 built 数会随 soak 的切页动作增长到全页。
- `UI MEM`/`UI FORCEFAIL` 是 console 诊断面：前者只读，后者一次性注入、默认关闭，
  不影响产品行为。
