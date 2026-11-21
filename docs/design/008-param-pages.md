# ADR-008: 页面懒建与 LVGL 池预算（阶段 4 参数页的前置框架）

> 状态：已接受（阶段 4，P4-0 Task 4；Task 9 补 §7 参数写/读接口与状态层投影；
> **Task 10 定稿**：§8 参数页框架（knob/布局表/格式化/编辑路径/预算）；**Task 11 收口**：
> 五参数页（+FM6/SAMPLE）、格式化截断口径、`UI KNOB` locked 令牌、九页预算表）｜
> 日期：2026-11-21
> 关联：ADR-005「阶段 4 容量决策」；AGENTS §5.4.1（console 注入约定）、§5.5（门禁）；
> 代码：`firmware/components/sloop_ui/src/{ui.c,knob.c,page_layout.c,page_params.c,page_*.c}`、
> `include/sloop_ui.h`、`src/theme.h`、`firmware/main/console.c`；证据：
> `docs/dev/verify/4-p0-2026-11-21/task-{4,9,10}-*` 与 `docs/dev/verification-log.md`
> （阶段 4 / P4-0 Task 4、9、10）。

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
  注册页更多时按可用宽度收窄，并给右侧 SLOOP 标题/层提示留 `UI_TAB_RIGHT_RESERVE`
  =220px——7 页时 140px、9 页时 107px，见 §8.1）。**注册表只增不删**：
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
  逐字节相同）。其余页（TRACKS/STEP/TRANSPORT + 参数页）首次访问时构建。
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
| PLAY | 2,508 B | 3,072 B | +22% | 6 标签 + 1 电平条 + 音符指示盒（7 对象） |
| TRACKS | 6,040 B | 7,168 B | +19% | 4 卡 × 4 子对象 + 4 电平条 |
| STEP | 21,836 B | 23,040 B | +5.4% | 75 对象（64 格 + 标签）+ 12 个共享 `lv_style_t` 的实例引用（est 见下方注；9 页末页自由量只够 5.4% 余量） |
| TRANSPORT | 3,788 B | 4,608 B | +22% | 6 按钮（含标签）+ 4 标签 |
| GLOBAL | 2,856 B | 3,584 B | +25% | 8 旋钮（单对象自绘）+ 页眉 + 2 个 ± 按钮（Task 10） |
| FX | 2,856 B | 3,584 B | +25% | 同上（Task 10；实测 2,816–2,876 随池历史漂移） |
| ANALOG | 2,868 B | 3,584 B | +25% | 同上（Task 10；实测 2,848–2,868） |
| FM6 | 2,864 B | 3,584 B | +25% | 8 旋钮（ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH，Task 11） |
| SAMPLE | 2,432 B | 3,072 B | +26% | 6 旋钮（空槽 edit[5]/edit[7] 不建，Task 11） |
| **合计** | **48,048 B** | **55,296 B** | — | 九页全建后 `UI MEM` 同次输出（Task 11）：pool_used 55,064 / pool_free **2,816** / pool_total 57,880。**reserve（total − sum_est）不是可用余量**（见 §5 invariant 1）；P4-A 终审轮 STEP est 再下调到 23,040 后，九页全建 `pool_free` 实测 2.2–2.7KB 量级（终审轮 2,628–2,644B）。各行 delta 是同一次 `UI MEM` 的快照（每页最后一次成功构建的池增量），不是跨次拼表 |

> **STEP 的 est 在 9 页口径两次下调：24,576 → 24,064（Task 11）→ 23,040（P4-A 终审
> 修复轮）**。**其它八页建完、STEP 尚未建时** free 实测 **24,044B**（终审轮 params
> 场景顺序：`池剩余=24044B 预算=24064B` 被拒 20B）到 24,272B（Task 11 STEP-last
> transcript）。24,064 已经高于「delta 21,864 + 10% = 24,050B」，在 free 下沿
> 根本放不下——**STEP 在 9 页末页无法保留 10% 余量**。取 23,040（= 最大实测 delta
> 21,864B + 1,176B / +5.4%，512 对齐；实测 delta 21,484–21,864B）：对 free 下沿
> 24,044B 余 1,004B（冷启动 STEP-last：free 24,112B、delta 21,484B、余 1,072B），
> STEP 建完瞬间 free 2.2–2.6KB。这是经验可行的下沿，不是不变量允许的裕量；**9 页是当前池的容量天花板**，
> 再加页/扩控件前必须按 §5 rule 4 做设计决策（DI-17）。

> 2026-11-21 P4-0 Task 5（步窗）后复测：STEP 首次构建 `池增量=21832B 预算=24576B`
> （列号改为单行绝对步号后 TLSF 分配序变化，对象数不变；delta ≤ est 仍成立），
> 其余三页页面代码未改（池历史不同，实测 delta 可轻微漂移——同日志 TRANSPORT 复测
> 3,792B vs 表内 3,744B）。预算表与规则不变。数据出处：`4-p0-2026-11-21/task-5-inject-step_edit-run1.log`
> （首跑归档件；同名主日志已被修复轮重跑覆盖——09:37:30，早于 run2 09:39:00）。

预算规则：

- `est_bytes` = 实测 delta × 1.10–1.23、512 对齐；预算是**构建前门槛**，不是精确账本
  （STEP 在 9 页口径取 23,040 = 最大实测 delta + 5.4%——末页自由量放不下 10%，见 §3 注）。
- **真门禁是逐页的 `free_size ≥ est_bytes`（构建前检查）**，不是「Σ est < 池总量」。
  后者算错了基数与固定开销：TLSF 池可用总量是 63,120B（不是 `CONFIG_LV_MEM_SIZE`
  的 65,536B），且非页面的基础占用（根/页签条/错误状态条/字体等）不计入 Σ est——
  同一次 `UI MEM` 实测 **7,016B = pool_used 55,064 − Σdelta 48,048**。按 Task 11 的
  est 之和（56,320B），真实账面 **63,120 − 7,016 − 56,320 = −216B**——当时「九页
  全建」在静态账面上是负的，端到端能落地（实测 `pool_free 2,816B`）只因**真实
  delta（48,048B）小于 est 之和**：est 每页留了余量，Σ est 不是占用预测。P4-A 终审
  轮把 STEP est 下调到 23,040 后，保守账面变为 **63,120 − 7,016 − 55,296 = +808B**
  （基础占用漂移到 7,112B 时 +712B；勉强为正），但 ~800B 不是可用余量：逐页构建
  时的池历史才是。实测下沿 = 其它八页建完时 free 24,044B，STEP（est 23,040）门槛
  余量 **1,004B**（冷启动 STEP-last 1,072B）；STEP 建完瞬间 free 2.2–2.6KB。因此「9 页可用」仍是**经验结论**（每页 delta ≤ est 且最坏页序过门槛），
  不是任何一条静态不等式推出的；保守替代（Σ est + 基础占用 ≤ 63,120）在 Task 11 的
  est 下就失败，现在也只余 808B，都不能替代构建前逐页检查。
- 构建前检查 `free_size < est_bytes` → 直接拒绝（`原因=budget`），不做半途分配：
  池碎片化时中途失败的代价（半成品容器 + 后续分配面更小）比拒绝更高。
- 参数页落地后增表行；`UI MEM` 的 `pool_free`/`sum_est` 是现场校验入口（`reserve`
  仅是 total − sum_est 的展示值，**不**代表可用余量）。
- 超出预算的页面（或九页全建后 free 逼近 2KB）**必须先做设计决策**（拆页/共享样式/
  对象复用/驱逐策略），不得直接调大 est 或调大池——后者已实测把内部 RAM 撑爆（DI-17）。

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
- 不变量（`UI MEM` 可复核；Task 11 后为**九页**口径）：
  1. **构建前门槛 `free_size ≥ est_bytes`**（逐页、按实际构建顺序）——这才是真门禁。
     旧注「`pool_total − Σ est > 0`」**不是**不变量：它用 TLSF 块负载口径的
     `total_size`（59K 上下浮动，非池容量 63,120B）并忽略 7,016B 基础占用；按 Task 11
     的 est 之和真实账面 63,120 − 7,016 − 56,320 = **−216B**（终审轮 est 下调后为
     +712–808B，仍不是可用余量）。九页端到端可用是经验结论：实测 Σdelta 48.0–48.1KB、
     其它八页建完时 free 下沿 24,044B（冷启动 STEP-last 实测 24,112B）、STEP 门槛
     余量 **1,004–1,072B**（est 23,040；STEP delta 实测 21,484–21,864B）。
     更保守的替代（Σ est + 基础占用 ≤ 63,120）在 Task 11 的 est 下就失败，不能
     作为门禁；
  2. 每页 `delta <= est`（九页全部成立，余量 5.4–26%；STEP 例外见 §3 注）；
  3. 九页全建后 `pool_free > 0`（Task 11 实测 2,816 B）；
  4. 再加页/改控件前重新核对以上三条并把新行写入本表。**9 页已到容量天花板**
     （STEP 门槛余量 1,004B 量级，且页签宽度在 10 页时已不足——见 ADR-005「导航信息架构」）：
     再加页必须先做设计决策（拆页/共享样式/对象复用/驱逐），不得只调 est 或池；
     登记 DI-17（该条只门禁内存/池，不覆盖页签信息架构）。
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
UI MEM pool_used=55064 pool_free=2816 pool_total=57880 registered=9 built=9 failed=0 sum_est=56320 reserve=1560
UI MEM page id=4 title=GLOBAL state=built est=3584 delta=2856
UI MEM page id=5 title=FX state=built est=3584 delta=2856
UI MEM page id=6 title=ANALOG state=built est=3584 delta=2868
UI MEM page id=7 title=FM6 state=built est=3584 delta=2864
UI MEM page id=8 title=SAMPLE state=built est=3072 delta=2432
```

（示例为 Task 11 现场（STEP est 24,064）；P4-A 终审轮 STEP est 23,040 后 `sum_est` 55,296、`reserve` 相应变大——`reserve` 只是展示值，门禁见 §5。
Task 11 之后注册表 9 页：四基础页 + GLOBAL/FX/ANALOG/FM6/SAMPLE；未访问的页
`state=unbuilt` 且 `delta=0`。上面是九页全建后的现场输出。）

### 7. 参数写/读 API 与状态层投影（P4-0 Task 9 落地）

参数页（Task 10）与后续预置加载的前置接口，逐字签名见 spec §4.1 与
`sloop_core.h`/`sloop_state.h`：

| 层 | 接口 | 语义 |
|---|---|---|
| 引擎 | `sloop_core_set_params(track, ids, values, n)` | **一次** `s_engine_lock` 应用 N 对；逐条钳位源与 `sloop_core_desc` 同源（`id < P_E0 → TP[id]`；`P_E0..P_E7`：合成轨 `ENGINES[eng_req]->edit[id-P_E0]`、鼓轨仅 `P_E0 → DRUM_KIT_DESC`）；无描述符的 id 逐条忽略**不中止整批**；越界 track/NULL/n=0/冻结态整批丢弃 |
| 引擎 | `sloop_core_read_params(track, p, g)` | 一次锁拷出 `trk[track].p[P_COUNT]` + `song.g[G_COUNT]`（原始真值）；track 越界 p 全零；冻结态两者全零（与 `read_step` 同口径） |
| 状态 | `CMD_SET_PARAM{a=track,b=id,v}` | 以 N=1 走批量入口；`v` 为 16 位补码（双极参数用 `(uint16_t)(int16_t)x`）；写后立即回读投影 |
| 状态 | `CMD_SET_PARAM_BATCH{a=track,p,n}` | `p` = `sloop_param_pair_t{uint8_t id; int16_t v;}`（sizeof==4，静态断言）数组，**同步 apply**：数组只需在 `state_apply` 返回前有效；状态层解包到栈数组（上限 `APP_STATE_PARAM_BATCH_MAX`=128，超出截断并 `ESP_LOGW`）后**一次**引擎调用 |
| 状态 | `CMD_SET_GLOBAL{a=id,v}`（Task 10） | 全局参数（`song.g[]`）：经 `sloop_core_set_global` 按 `GP[id]` 钳位后写引擎，立即回读 `g[]`；`id == G_BPM` 同拍更新 `core.bpm`。只用于值参数（动作类全局不得经此下发） |
| 状态 | `app_state_t.p[]/g[]` | p = **选中轨**参数投影、g = 全局；`state_sync_from_engine` 与窗口快照同拍用一次 `read_params` 整份覆盖；非选中轨写入不改 p |

- 钳位只在引擎一处（状态层不重复实现 desc 范围）；`PARAM` 的 id 合法性预判走
  `sloop_core_desc`（只判存在性，不钳位）。
- 投影跟着**选中轨**：`CMD_SET_TRACK`/`CMD_SELECT_TRACK` 在引擎选轨落地后**立即**
  回读（`cmd_select_track`，同拍，不等下一拍 sync）——参数页的标题（`core.selected_track`）
  与值（`p[]`）不会出现「标题=新轨、参数=旧轨」的帧。
- 设备诊断入口：console `PARAM <id> <value>` / `PARAM BATCH <id> <value> …`（选中轨；
  回显 `applied` 即引擎钳位结果），与 UI 编辑同一状态层路径（AGENTS §5.4.1）；
  `PARAM RAW <id> <value>` 跳过 desc 预判，用于复验引擎对无描述符 id 的忽略路径。

### 8. 参数页框架（P4-A Task 10 落地）

#### 8.1 页与布局表

- **布局表**（`sloop_ui/src/page_layout.c`）：GLOBAL / FX / ANALOG / FM6 / SAMPLE
  五页，每页 8 槽 `{scope, engine, id}`（`0xFF` = 空槽）。页元数据（id/title/est）
  与 `ui_page_t` 注册条目由**同一张 X-macro 表**生成：页签顺序 = 声明顺序，接在
  四基础页之后；`PARAM_PAGE_COUNT == APP_STATE_PAGES - 4` 与 `app_page_t` 有静态断言。
- 槽位语义：GLOBAL → `GP[]`（`song.g[]`）；TRACK → `TP[]`（`trk[].p[]`）；
  ENGINE → `ENGINES[engine]->edit[0..7]`（= P_E0..P_E7）。
- 页内容（策展取舍，映射上游 `PAGES[]` 语义而非逐像素复刻）：
  - GLOBAL = BPM / SWING / CLICK / TUNE + DLY(TIME/FDBK/COLR/MIX)（上游 GLO>GLOBAL
    与 GLO>DLY 合成一页）；
  - FX = DST / CHO / DLY / REV + FILT(P_TFLT) + LVL / PAN / ATK（FX 四件 + 轨道滤波
    + 输出条；覆盖 F_PCT/F_FILT/F_DB/F_TIME/F_BIPCT 的显示路径）；
  - ANALOG = 引擎 edit[0..7]：WAVE(F_ENUM)/DTN(F_INT unit=ct)/MIX/NOIS/CUT(F_CUTOFF)/
    RES/DRV/KTR（覆盖 F_ENUM/F_INT/F_PCT/F_CUTOFF）。
  - FM6（`ENGINES[9]->edit[0..7]`，Task 11）= ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH：
    上游 `eng_fm6.c` 的 8 个宏一个不落（EDIT 1+2 合并）。ALG 是 **F_INT + 33 名**
    （`PAT`/1..32），PTCH 是 F_INT + 35 名（F1..F8/B1..B27），两者都走 `F_INT` 的
    0 终止名字表路径（`name_for_int`，值域均分即 1:1）。取舍：不拆成「OPS/PATCH」
    两个上游页，因为 8 槽正好一页，且编辑宏时通常同时看算法与调制量。
  - SAMPLE（`ENGINES[4]->edit[0..7]`，Task 11）= SET/TUNE/BITS/LOOP/CUT/DRV 六个
    真实槽；上游 edit[5]/edit[7] 是占位（label `-`、min=max=0），布局表用 `0xFF`
    空槽**不建旋钮**（点击无效、键盘焦点跳过、`TAP` 命不中）。SET 是 F_ENUM 12 名
    （PIANO..PERC + USR1..USR4，用户槽在无采样时与工厂槽显示同一表）。
  - 家族页取 **GLOBAL + FX**（而非 MASTER）：上游 MASTER 的 8 个宏是全局/轨道混排，
    GLOBAL 与 FX 已覆盖其大部分语义（FX 页含输出条 LVL/PAN/ATK，主电平/声像在
    此），故 P4-A 的「一个家族页」口径由 GLOBAL+FX 两页共同满足（spec §3）。
- 页签：9 页时 `tab_w = min(176, (1280 - 2·16 - 220 - 8·8)/9) = 107`；右侧 220px 留给
  SLOOP 标题/层提示（`theme.h` 的 `UI_TAB_RIGHT_RESERVE`）。层提示 16pt + 宽度封顶 +
  省略号截断，四层键同按也不压页签（ADR-005「导航信息架构」节）。注入脚本的页签
  几何/探针已同步（`scripts/inject/inject.py`，探针取页签左缘内 8px 的填充色；9 页
  y=36 行的 x 依次为 24/139/254/369/484/599/714/829/944）。

#### 8.2 旋钮控件（`knob.c`）

- 一个旋钮 = **一个 LVGL 对象**：普通 `lv_obj` + `LV_EVENT_DRAW_MAIN`，
  `lv_draw_arc` 两次（底弧 UI_PANEL_HI / 指示弧 UI_TEXT_DIM|UI_ACCENT）、
  `lv_draw_label` 两次（名称 16pt / 数值 20pt，`text_local=1` 让 LVGL 复制文本——
  绘制任务可能晚于回调执行）。
- **预算决策**：不用 `lv_arc`+`lv_label`。实测 662B/旋钮/页（lv_arc 的 bg/indicator
  绘图描述符 + 3 个样式部件），8 槽页 6.6KB；七页全建的中间态就会把 STEP 的预算
  拒绝（Task 10 实测 free 23,304B < 24,576B）。单对象自绘约 300B/旋钮，页 delta
  2.4–2.9KB。
- **触摸语义（相对拖拽，不是 lv_arc 的绝对角度）**：
  - `PRESSED`：设焦点 + 记录**锚点角度与锚点值**，**不改值**（轻按不会跳值）；
  - `PRESSING`：相对锚点的角度差 → 值差（整圈扫角 270° = 整个值域，
    `knob_drag_value` 四舍五入并钳位）。拖回锚点角度即回原值；跨越底部死区时
    角度差取短路径（`knob_angle_delta`，(-180,180]）；
  - 只响应半径 **55..95px** 的环带（环线中心 73、线宽 14）：中心与盒角不产生跳变；
  - `RELEASED`/`PRESS_LOST` 清锚点。焦点态 = 指示弧/名称用 UI_ACCENT。
- 细调：页眉右侧 `−`/`+` 按钮（80×40，作用于**焦点槽**，±1 原始单位）；键盘 ↑/↓
  同一动作。焦点槽是 UI 局部态（`s_focus[页]`，临界区保护），按页保持。
- TAP 注入（`page_params_tap`）：旋钮命中只设焦点 → `-> PARAM slot=n focus`；
  `−`/`+` 命中走与控件回调相同的动作函数 → `-> PARAM slot=n id=.. delta=±1 value=..`
  （value = 写后回读的钳位结果）；不可编辑页 → `-> PARAM slot=n locked`。
- 调试注入（console `UI KNOB <slot> press|drag <dx> <dy>`）：用相对旋钮中心的
  合成坐标走**与物理触摸相同**的 `handle_pressed`/`handle_pressing` 路径（press =
  焦点+锚点；drag = 相对拖拽），回显 `value` 或 `ignored (ring band)`。用于在无
  手指/无触摸注入的环境里机器验证「回调接线 + 相对拖拽」；物理手感仍需人测（DI-16）。
  实现约束：`knob_create()` 显式接收 `user/on_change/on_focus`（早期版本内部 memset
  会把调用方先设的字段清掉，物理触摸因此完全失效——这类接线错误 TAP 注入看不见，
  所以必须有直连控件事件的调试入口）。
  令牌：环带外/无锚点回 `ignored (ring band)`；**引擎页不适用**（选中轨引擎与页
  声明不符/鼓轨）回 `ignored (locked)`（Task 11；`ESP_ERR_NOT_ALLOWED`，与环带外
  的 `ESP_ERR_NOT_FOUND` 分开）——两种「值不变」的原因不同，脚本不得混判。
- 键盘路由（`input.c` + `sloop_ui_param_*`）：参数页上 ←/→ 移动焦点（跳过空槽、
  环绕）、↑/↓ 调值 ±1；只写状态层，不取 LVGL 锁。
- **刷新时延口径**：键盘/± 路径只写状态层，控件重绘在**下一个 30ms UI tick**
  （≤30ms + 渲染一帧）；拖拽路径在事件回调内直接下发并立即刷新本页（同一帧）。
  计划文档里的「≤16ms/步」指输入轮询/事件派发预算，不是参数页重绘时延。
- 纯数学/格式化拆到 `knob_math.{h,c}`（无 LVGL）：弧线映射、角度差、相对拖拽、
  环带、格式化；host 单测 `scripts/knob-test.sh`（table-driven，含格式化表）——
  这是旋钮逻辑的机读门禁，UI 接线错误另由 `UI KNOB` 直连事件验证。

#### 8.3 值路径与格式化

- 值路径（唯一）：槽位值 → 状态层命令 → 引擎钳位 → 立即回读投影并刷新本页。
  GLOBAL → `CMD_SET_GLOBAL`；TRACK/ENGINE → `CMD_SET_PARAM`（a=选中轨，
  b=TP id 或 `P_E0 + i`）。
- 格式化（`knob_format_value`，显示层）：

| fmt | 显示 | 例 |
|---|---|---|
| F_ENUM | `enums[v-min]` + desc.unit | WAVE `SAW`；CLICK `OFF` |
| F_PCT | `(v*100 + max/2)/max` + `%` | SUS 90 → `71%` |
| F_BIPCT | `v*100/64` + `%`（仅 v>0 加 `+`） | PAN 0 → `0%`；+32 → `+50%` |
| F_DB | 0 = `OFF`；其余 `LEVEL_DB_X10[v]`（上游表） | LVL 104 → `-4.0dB`；111 → `-0.5dB` |
| F_TIME | `TIME_MS_X10[v]`（上游表）：<10ms 1 位小数 / <1s 整数 ms / <10s 2 位小数 s / 其余 1 位小数 s | ATK 10 → `2.1ms`；127 → `10.0s` |
| F_CUTOFF | `CUTOFF_HZ[v]`（上游表）→ Hz/kHz | CUT 90 → `2.5kHz`（截断） |
| F_LFOHZ | `LFO_HZ_X100[v]`（上游表）→ Hz | RATE 60 → `1.18Hz` |
| F_SEMI / F_BPM / F_NOTE / F_ONOFF / F_STEPS / F_SWING / F_FILT | 上游 param_format 同口径（SEMI 仅 v>0 加 `+`） | `0st`/`+2st` / `90BPM` / `C` / `ON` / `16STEP` / `50` / `LP50%` |
| F_INT（含 0 终止 names）与未知 fmt | 原始整数（+ desc.unit） | DTN 10 → `10ct` |

- F_TIME/F_CUTOFF/F_LFOHZ/F_DB **直接用上游生成表**（`third_party/sloop-fm1/build/gen/
  felucca_tables.h` 的 TIME_MS_X10/CUTOFF_HZ/LFO_HZ_X100/LEVEL_DB_X10，由
  tools/gen_tables.py 生成），不做指数近似；`knob_math.c` 通过 `-I` 引用该表头
  （sloop_ui 的 CMake 在表头缺失时 FATAL_ERROR，与 sloop_core 同口径）。host 单测
  `scripts/knob-test.sh` 用同一表头，格式化表逐项断言（含秒档 1.88s/10.0s——早期
  实现把 ms10/1000 当秒，≥1s 的值大 10 倍，已由该测试钉住）。
- **定点小数按上游 `libc.c` 的 `fmt_fix` 截断**（Task 11 修正）：先整数分频再拼
  小数位，不是 printf 的 `%.Nf` 四舍五入。可辨例：TIME 104（18,862）→ `1.88s`
  （不是 1.89）、CUTOFF 90（2,568）→ `2.5kHz`（不是 2.6）、LFOHZ 105（1,257）→
  `12.5Hz`（不是 12.6）。`knob-test.sh` 用这三个值钉住截断口径；<10Hz 的 LFOHZ
  （h/100 恰两位）与 <10ms 的 TIME（ms10/10 恰一位）两档在截断/舍入下同值。
- **负数且 |v| < 一个显示单位**（整数部分为 0）时符号单独补：`format_fixed` 在
  `v < 0 && ip == 0` 时输出 `-0.5`（上游 `fmt_fix` 输出 `-.5`，同值、可读）。
  F_DB 的 `LEVEL_DB_X10[111] = -5` 是设备上唯一的次单位负值（TIME/CUTOFF/LFOHZ
  生成表全为非负），由 `knob-test` 的 `DB 111 → -0.5dB` 与 `DB 110 → -1.0dB`
  边界用例钉住。
- `fmt` 的上游编码由 `sloop_core.h` 的 `sloop_desc_fmt_t` 镜像，`selftest.c` 静态
  断言与 `F_*` 逐项一致（换上游版本编译期暴露）。单位只由 fmt 决定（desc.unit 只
  用于 F_INT/F_ENUM），不额外加 fmt 不隐含的后缀。

#### 8.4 引擎页的适用性

- ENGINE 槽位的描述符取**页声明的引擎**（布局表），值取 `trk[选中轨].p[P_E0+i]`
  （选中轨的引擎槽就是这 8 个 id）。选中轨的引擎与页声明不符、或选中鼓轨（P_E0 是
  kit 不是引擎槽）时：页眉红字 `… USES ENG k — N/A`（鼓轨 `(DRUM) — N/A`），旋钮显示
  `--`，TAP ± 回 `PARAM slot=n locked`，键盘调值忽略——避免拿 ANALOG 的 label 写另一
  个引擎的槽位。
- 当前固件所有轨默认 ANALOG（engine=0），该路径需 `ENG TRACK <k> <engine> <preset>`
  触发；一次性探针（`ENG TRACK 1 1 0` → 选轨 1 → ANALOG 页）证据见
  `docs/dev/verify/4-p0-2026-11-21/task-10-*`。

#### 8.5 预算与验证

- 九页预算见 §3 表（GLOBAL/FX/ANALOG/FM6 各 est 3,584B、SAMPLE 3,072B；九页全建
  free 2,816B）。
- 验证入口：`scripts/inject/params.sh`（五页：TAP ± 与键盘编辑经 `PARAM`/`PARAM GLOBAL`/
  `STATE SNAP` 断言；**`UI KNOB` press/drag 证明触摸回调接线与相对拖拽**——按下不改值、
  +90° 改 +67（BPM range 200）、拖回锚点回原值、中心/环带外被忽略；FM6/SAMPLE 页
  另用 `ENG TRACK` 切引擎 + `ENG DESC` 断言 ALG 33 名 / SET 12 名，`UI KNOB` 在锁定
  页回 `ignored (locked)`；切页保值、九页池水位、五页截屏 + 像素抽检）；
  `scripts/knob-test.sh`（旋钮纯函数 + 格式化表 host 单测）；
  `UI FORCEFAIL global` 复验参数页的失败可见性与重建（重建后引擎真值不变）。

### 9. 描述符契约的定稿决定（Task 8/10）

- **不做新描述符表**：`sloop_core_desc` 直接投影上游 `param_desc_t`（`label/unit/fmt/
  min/max/def/names`），指针指向上游静态数据，调用方不得释放/改写。
- **鼓轨哨兵**：`SLOOP_DESC_ENGINE` + `engine == SLOOP_CORE_NENGINES (12)` + `id == 0`
  返回 `DRUM_KIT_DESC`（上游 `ed_eng` 对鼓轨报「无引擎」的同一取值）；哨兵的
  `id != 0` 返回 false（鼓轨没有 P_E1..P_E7）。
- **TRACK scope 拒绝 id ≥ P_E0**（63）：引擎槽没有轨道上下文，必须走 ENGINE scope。
- **动态描述符钩子不投影**：上游 `params.c` 的 `track_desc()` 会在 `ins_desc()`
  （INSERT A/B/C 随 P_ITYPE）、`ESYNC_DESC[]`（ENV SYNC 的 ATK/DEC/REL）与
  `engine->desc()`（引擎 mode 依赖 label/names）处替换静态表；本投影只给静态
  `TP[]`/`GP[]`/`edit[]`。**动态描述符不仅能换 label/names/unit，也会换 fmt/unit
  语义**：`ESYNC_DESC[P_ATK]` 是 `F_INT` + 音符名表（单位 1/384 小节），而静态
  `TP[P_ATK]` 是 `F_TIME`（ms）——min/max（0..127）相同，但「这个数字是什么意思」
  完全不同。因此本投影的前提只是 **min/max 相同**（钳位安全）；fmt/unit 的差异
  必须由使用方处理：
  - 现状（P4-A）：FX 页在选中轨 `P_ESYNC != 0` 时把 ATK 槽标注为 `ATK/ESYNC` 并按
    `F_INT` 原始值 0..127 显示（无 ms 后缀，`page_params.c` 的 `slot_desc`），
    绝不把音符值显示成毫秒。**运行中翻转**（console `PARAM 59` 注入或后续 FM-1
    同步）：`page_params_update` 把 `P_ESYNC` 纳入页面上下文键，位变化时重新解析
    已建槽的描述符并按 label/fmt/unit 差异重绘，不要求重进页面（P4-A 无 ENV 编辑页，
    但注入路径今天就能走到）。
  - **P4-B 复核项（route a）**：把动态钩子纳入投影需要 track 上下文（desc API 增
    track 参数或新增 `sloop_core_desc_track(track, id, out)`），届时 ATK/DEC/REL
    可直接显示上游音符名（`ESYNC_NAMES`）；同时 `sloop_core_set_params` 的钳位
    也应换用同一动态表。任何情况下 `sloop_core_set_params` 的 min/max 前提若被
    上游改变，必须立即切到动态表并重跑设备参数断言。

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
- Task 11 后：参数页每页 2.4–2.9KB（est 3,072–3,584B），九页全建 free 2,816B；
  页签 9 页时收窄到 107px（右侧留 220px 给 SLOOP/层提示；10 页放不下，见 ADR-005「导航信息架构」）。参数页的控件选择见 §8.2 的预算决策
  （单对象自绘旋钮），**不要**退回 `lv_arc`+`lv_label`（会撑爆池）。新增参数页的
  步骤：布局表加一行 X 宏（id/title/est + 8 槽）→ `PARAM_PAGE_COUNT` 与
  `app_page_t`/`APP_STATE_PAGES` 同步 → console 页名表 → 实测 est。
