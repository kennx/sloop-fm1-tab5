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

- **注册顺序 = 组内子签顺序**（P4-B Task 11 起：页签是**两行**——上行 5 个组签
  `PLAY/TRACKS/EDIT/SEQ/FILE`，下行当前组子签；组模型在 `ui.c` 的 `k_groups[]`，页签
  文本/数量由「组 + 组内已注册页」派生，不再由平铺注册表派生。组签宽度按 5 组算
  （`min(176, (1280−32−220−4×8)/5)` = 176px），子签复用下行 5 个槽；右侧 220px 留给
  SLOOP 标题/层提示，见 §8.1 与 ADR-005 §3）。**注册表只增不删**：外部组件可在
  `sloop_ui_start()` 之前调 `ui_page_register()` 预注册页面，内置页注册幂等
  （`static done` 标志 + 按 id 去重）；无已注册页的组不显示。输入层的 `TAB` 轮转
  用 `sloop_ui_page_order_next()`（组顺序的**扁平**轮转：组内子页→下一组第一页），
  不按 `sloop_ui_page_count()`，也不按 `APP_STATE_PAGES`。
- **6 页内置**在 `sloop_ui_start()` 内注册（`k_builtin_pages`：PLAY/TRACKS/STEP/TRANSPORT +
  FILE 组 PROJECTS/PRESETS）；参数页（GLOBAL/FX/ANALOG/FM6/SAMPLE）由
  `page_layout_register()` 注册进同一注册表——交付态共 **11 页 / 5 组**（ADR-005「导航
  决策」）。`ui_page_register()` 供后续页加入。`id` 受 `APP_STATE_PAGES` 约束（状态层
  `state_set_page`、input 的组/子签、console 页名表同源）；加页时一起扩 `APP_STATE_PAGES`。

### 2. 懒建策略

- 页对象（透明容器 + 页内控件）在**首次进入**时创建。**组作用域生命周期**（P4-B
  Task 11，ADR-005 §3）：**同组内**切页只改 `LV_OBJ_FLAG_HIDDEN`（零分配、焦点/滚动/
  状态行保留）；**跨组**切换固定顺序「建目标页 → 切到目标页 → 销毁源组**全部**已建页
  （含原当前页）」——离开组后源组 `built=0`，再进入时懒建。代价是切换瞬间的峰值 =
  源组已建 Σ + 目标页 est（瞬态，不变量见 ADR-005 §5）；收益是稳态只驻留一个组，
  解得开 9 页平铺的池天花板。
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
| FM6 | 2,864 B（Task 11）→ **4,956–5,100 B**（Task 13，含 bank 行） | 3,584 B → **6,144 B** | +25% → **+20.5%**（对 5,100；区间 4,956 处 +24.0%，见 §3 注） | 8 旋钮（ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH）+ 底部 bank 存取行（BANK 标签/±/槽号/SAVE/ERASE/状态行 = 7 个单对象 label，Task 13） |
| SAMPLE | 2,432 B | 3,072 B | +26% | 6 旋钮（空槽 edit[5]/edit[7] 不建，Task 11） |
| **合计** | **48,048 B**（Task 11 时点；Task 13 后 FM6 +2,268B） | **55,296 B**（Task 11；现 57,856B） | — | 九页全建后 `UI MEM` 同次输出（Task 11）：pool_used 55,064 / pool_free **2,816** / pool_total 57,880。**reserve（total − sum_est）不是可用余量**（见 §5 invariant 1）；P4-A 终审轮 STEP est 再下调到 23,040 后，九页全建 `pool_free` 实测 2.2–2.7KB 量级（终审轮 2,628–2,644B）。各行 delta 是同一次 `UI MEM` 的快照（每页最后一次成功构建的池增量），不是跨次拼表 |

> **STEP 的 est 在 9 页口径两次下调：24,576 → 24,064（Task 11）→ 23,040（P4-A 终审
> 修复轮）**。**其它八页建完、STEP 尚未建时** free 实测 **24,044B**（终审轮 params
> 场景顺序：`池剩余=24044B 预算=24064B` 被拒 20B）到 24,272B（Task 11 STEP-last
> transcript）。24,064 已经高于「delta 21,864 + 10% = 24,050B」，在 free 下沿
> 根本放不下——**STEP 在 9 页末页无法保留 10% 余量**。取 23,040（= 最大实测 delta
> 21,864B + 1,176B / +5.4%，512 对齐；实测 delta 21,484–21,864B）：对 free 下沿
> 24,044B 余 1,004B（冷启动 STEP-last：free 24,112B、delta 21,484B、余 1,072B），
> STEP 建完瞬间 free 2.2–2.6KB。这是经验可行的下沿，不是不变量允许的裕量；**9 页是当前池的容量天花板**，
> 再加页/扩控件前必须按 §5 rule 4 做设计决策（原「9 页参数页框架已达 LVGL 池容量
> 天花板」条目已随 ADR-005 的 P4-B 导航决策关闭）。

**FILE 组两页（P4-B Task 12，2026-11-22 实测；组作用域口径，不是九页平铺）**：

| 页 | 实测 delta（首次构建，`UI MEM`） | est_bytes | 余量 | 说明 |
|---|---|---|---|---|
| PROJECTS | 7,572–7,764 B（四次运行） | 8,192 B | +5.5% | 4 卡（字母/详情 2 label + 3 个单对象按钮）+ 状态行 = 26 对象；静态样式只 init 一次 |
| PRESETS | 12,228–12,276 B（四次运行） | 12,800 B | +4.4% | 32 格（单对象可点击 label，两行文本）+ 3 按钮 + 状态行 = 36 对象 |
| **FILE 组 Σ** | **19.8–20.0 KB** | **20,992 B** | — | 两页同时已建（组内切页只改显隐）；过渡峰值见 ADR-005 §5（free 7,308–7,840B） |

> **R34 备注（2026-11-22 评审修复）**：`styles_init()` 必须**每次开机只跑一次**（`static bool`
> 守卫，PROJECTS/PRESETS 原本如此，TRACKS/STEP 本轮补齐）——组作用域会反复销毁/重建页面，
> 重跑 `lv_style_init` 会丢掉静态样式已分配的 props 数组（实测 PLAY↔TRACKS 每往返漏 ~300B）；
> 修复后 10 次往返 `UI MEM` 的 `pool_free` 变化 = 0（证据 `4-pb-2026-11-21/task-12-di25-leak.txt`）。
> 初版 est `PROJECTS 4,608 / PRESETS 5,120`（spec §5 的下限估算）在对象数实测后不可达：
> 一个 lv 对象 ≈ 300B，32 格网格任何「object + label」两层写法都会翻倍到 ~24KB。
> 两页的优化路径是**单对象**（可点击 label 当按钮/格子）与**去掉页眉/提示 label**
> （页名由下行子签承担）——这不是放宽门禁，而是把 est 按实测重订并把组不变量重算
> （ADR-005 §5：最坏有序对变成 FILE 全建 → SEQ 首建 STEP，账面余 1,192B）。

> 2026-11-21 P4-0 Task 5（步窗）后复测：STEP 首次构建 `池增量=21832B 预算=24576B`
> （列号改为单行绝对步号后 TLSF 分配序变化，对象数不变；delta ≤ est 仍成立），
> 其余三页页面代码未改（池历史不同，实测 delta 可轻微漂移——同日志 TRANSPORT 复测
> 3,792B vs 表内 3,744B）。预算表与规则不变。数据出处：`4-p0-2026-11-21/task-5-inject-step_edit-run1.log`
> （首跑归档件；同名主日志已被修复轮重跑覆盖——09:37:30，早于 run2 09:39:00）。

预算规则：

- `est_bytes` = 实测 delta × 1.10–1.23、512 对齐；预算是**构建前门槛**，不是精确账本
  （STEP 在 9 页口径取 23,040 = 最大实测 delta + 5.4%——末页自由量放不下 10%，见 §3 注）。
  **组作用域口径的例外（P4-B Task 12）**：FILE 两页取 +5.5% / +4.4%（8,192 / 12,800）——
  它们参与「FILE 全建 → 首建 STEP」这个新的最坏有序对，余量从 STEP 的 4,776B 压到 1,192B；
  再加 10% 会把不变量顶破（`13,800 + 22,528 + 23,040 + 4,096 = 63,464 > 63,120`）。判据
  仍是「delta ≤ est 且逐页 `free ≥ est`」，余量不足用**实证峰值**（free 7,308–7,840B ≥ 4KB）
  兜底，而不是放宽门禁。
  **delta 有池历史漂移的页（P4-B Task 13，FM6）**：该页实测 **4,956–5,100B**（同一源码、
  不同分配历史），est 6,144 = 12×512。余量按**最大**实测 delta（门槛的约束方向，
  `free ≥ est` 与 `est ≥ delta` 都只受上沿约束）算：`6,144 / 5,100 = +20.5%`，在
  +10–23% 内；对区间下沿 4,956 是 +24.0%——**比规则上沿（+23%）高 1 个百分点**：这不是
  est 过大，而是该次 delta 读数偏小（est 的可测约束是 `est ≥ delta` 与 `free ≥ est`，
  两者的余量都以**最大** delta 为准）。若某页的 delta 区间宽到**上沿**也超出 +23%，
  则按组不变量与实测峰值做设计决策（拆页/共享样式），不得直接调大 est。
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
  对象复用/驱逐策略），不得直接调大 est 或调大池——后者已实测把内部 RAM 撑爆（该容量
  条目已随 ADR-005「导航决策」关闭）。

**组作用域生命周期（P4-B，决策见 ADR-005「导航决策」）**：预算从「九页平铺 Σ」改为
**按组核算**——跨组切换的**过渡峰值**不变量
`base + max(源组 Σ est + 目标组首建页 est) + 4KB ≤ 63,120B`（base **两行页签实测
13,800B**，2026-11-22 Task 11 最终固件；旧值 7,016B 是 `pool_used` 口径的低估，见
ADR-005 §5）。**最坏有序对 = FILE 两页全建 → 首建 STEP**（P4-B Task 12 起）：
`13,800 + (8,192+12,800) + 23,040 + 4,096 = 61,928 ≤ 63,120`（余 1,192B；base 取
保守上界 14,112B 时余 880B）；实测过渡峰值 `pool_free 7,308–7,840B`（`store` 场景 ⓯ 断言
≥4,096B），仍是弧线 mask 峰值（≈2.6KB）的 2.8 倍。**按当前 est 表的有序对排名**：
① FILE→STEP `44,032B`（账面 61,928、余 1,192B；保守 base 14,112 时余 880B）；
② EDIT→STEP `43,008B`（Task 13 的 FM6 bank 行使 EDIT Σ = 19,968B；账面
`13,800 + 43,008 + 4,096 = 60,904`、余 **2,216B**；保守 base 余 1,904B）；
③ SEQ→PRESETS `40,448B`（账面 58,344、余 4,776B）。每页 `free ≥ est` 前置检查不变。切组顺序固定为「建目标页 → 切到
目标页 → 销毁源组**全部**已建页（含原当前页）」——源组页面在**切换之后**才释放（先销毁
会出现空白帧、输入路径在跨组切换期间失去活页），故峰值是瞬态且必须被上式覆盖；离开组后
源组 `built=0`。组内首次访问兄弟页时构建、同组后续切换只改显隐。`UI MEM` 增加
`GROUP <name> built=<n> sum_est=<B> sum_delta=<B> build_ms=<ms>` 分组行。上表九页实测
数据保留为 P4-A 终审时点的平铺历史口径；新页（FILE 组 PROJECTS/PRESETS 等）按组预算
纳入，不再要求「全部页面同时已建」。

**P4-C 12 页预算（规划，ADR-010 §5）**：SONG 使用独立顶层组，组顺序变为
`PLAY/TRACKS/EDIT/SEQ/SONG/FILE`，不加入 SEQ 稳态；TRANSPORT est 上限由 4,608 调整为
**9,216B**，SONG 初估上限 **12,288B**（两者都须在落地后按 `UI MEM` 实测收紧）。按 base
13,800B 与 4,096B 切换余量，FILE→STEP 仍为 **61,928B**；SEQ（23,040+9,216）→PRESETS
为 **62,952B**、余 168B，成为新的账面最坏有序对；SONG→PRESETS 为 **42,984B**。三者均
≤63,120B。六个组签落地时须重测 base、逐页 `free ≥ est` 与过渡后 `pool_free ≥ 4,096`；
超预算先改对象设计，不抬 est、不扩池。此处是 P4-C 的预算门槛，不表示本任务已注册第
12 页或已有实测 delta。

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
  3. **当前组全建后 `pool_free ≥20KB`**（P4-B 组作用域口径；`params` 场景门禁实测
     26–34KB）。「九页全建后 free > 0」（Task 11 实测 2,816B）只是 P4-A 平铺时点的
     历史口径，组作用域下九页不会同时常驻；
  4. 再加页/改控件前重新核对以上三条并把新行写入本表。**9 页平铺曾是容量天花板**
     （STEP 门槛余量 1,004B 量级，且页签宽度在 10 页时已不足——见 ADR-005「导航信息
     架构」）：P4-B 起由组作用域生命周期解除（见下条），但单组内的加页仍须按 rule 4
     做设计决策，不得只调 est 或池；
  5. **组作用域（P4-B 起）**：跨组切换的**过渡峰值**不变量
     `base + max(源组 Σ est + 目标组首建页 est) + 4KB ≤ 63,120B`（两行页签实测
     `13,800 + (8,192+12,800) + 23,040 + 4,096 = 61,928 ≤ 63,120`，余 1,192B；
     最坏对 = FILE 全建 → 首建 STEP，实测峰值 `pool_free 7,308–7,840B`；Task 12 前是
     EDIT 全建 → 首建 STEP，58,344 / 实测 12,356B）。源组页面在切换后才释放，
     峰值是瞬态但必须被覆盖；稳态回落为 `base + max_group Σ est = 41,448B`。且离开组后
     该组 `built=0`（含原当前页；`UI MEM` 分组行可复核）——这是把「九页全建」的经验
     下沿换成可复核预算（ADR-005「导航决策」§5）。
- 阶段 4 参数页的硬性规则：
  1. 每页一个注册表条目，`est_bytes` 必须来自本机 `UI MEM` 实测（同方法），不得拍脑袋；
  2. `build()` 内每个 `lv_*_create` 用 `UI_BUILD_CHECK()` 检查，并复位本页脏检查缓存；
  3. 共享 `lv_style_t`（状态变化用 `lv_obj_replace_style` 换指针）是既有纪律，参数页沿用；
  4. 超出预算或 Σ est 逼近池总量 → 先写设计决策（可新增 ADR 或修订本 ADR），不得只改数字；
  5. 禁止**无组边界**的「离页销毁/LRU 驱逐」做为省池手段（会引入不可预测的切页分配面
     与状态丢失）。P4-B 采纳的**组作用域销毁**（目标页建/切后销毁源组全部页面，含原
     当前页；焦点由状态层恢复，见 ADR-005「导航决策」）是受控例外；其过渡峰值由 §5
     invariant 5 覆盖（源组页面在切换后释放）。若仍需组内驱逐，另立 ADR 讨论。

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

（上面示例的数值是 P4-A 九页平铺时点（`pool_total`/`pool_used` 随池块拆分漂移，
`reserve` 只是展示值，门禁见 §5）。P4-B 交付态（11 页注册 + 组作用域；`store`/`nav`
会话实测）`UI MEM` 形如：

```
UI MEM pool_used=33568 pool_free=26688 pool_total=60256 registered=11 built=1 failed=0 sum_est=78848 reserve=-18592 group=SEQ
UI MEM GROUP PLAY built=0 sum_est=3072 sum_delta=0 build_ms=8
UI MEM GROUP TRACKS built=0 sum_est=7168 sum_delta=0 build_ms=11
UI MEM GROUP EDIT built=0 sum_est=19968 sum_delta=0 build_ms=8
UI MEM GROUP SEQ built=1 sum_est=27648 sum_delta=21316 build_ms=29
UI MEM GROUP FILE built=0 sum_est=20992 sum_delta=0 build_ms=33
UI MEM page id=2 title=STEP state=built est=23040 delta=21316 build_ms=26
…（其余页 state=unbuilt delta=0 build_ms=0）
```

（`group=<name>` = 当前页所属组；`sum_est` = 组内已注册成员 est，`sum_delta` = 已建
成员池增量；离开组后该组 `built=0`、`sum_delta=0`。`sum_est=78,848` 是**全部**已注册
页的 est 之和，`reserve` 因此在组作用域下常为负——它是历史展示值，不是门禁（容量
口径是 ADR-005 §5 的「base + 单组 Σest + 4KB」不变量，见 §5）。FILE 两页已注册 ⇒
该组 `sum_est = 8,192 + 12,800 = 20,992`、上行 FILE 组签常显、`TAB` 轮转含两页。）

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
- 页签（P4-B Task 11 起两行，ADR-005「导航决策」§2）：上行组签 5×176px @ y=4..36
  （PLAY/TRACKS/EDIT/SEQ/FILE，无已注册页的组不显示），下行当前组子签 176×30 @
  y=38..68（单页组留空）；右侧 220px 留给上行 SLOOP 标题 / 下行层键提示
  （`theme.h` 的 `UI_TAB_RIGHT_RESERVE`）。层提示 16pt + 宽度封顶 + 省略号截断，
  四层键同按也不压子签。注入脚本的页签几何/探针已同步（`scripts/inject/inject.py`
  的 `group_tab_probe(i)` / `sub_tab_probe(i)`：组签探针 y=8、子签 y=42；GLOBAL/FX/
  ANALOG/FM6/SAMPLE 依次是 EDIT 子行的第 0..4 张）。

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

- 九页预算见 §3 表（GLOBAL/FX/ANALOG 各 est 3,584B、FM6 6,144B（Task 13 的 bank
  行）、SAMPLE 3,072B）；P4-B 起按组核算（EDIT 组 Σ 19,968B，全建后 `pool_free`
  实测 26–34KB；九页平铺时点的 2,816B 仅作历史对照）。
- 验证入口：`scripts/inject/params.sh`（五页：TAP ± 与键盘编辑经 `PARAM`/`PARAM GLOBAL`/
  `STATE SNAP` 断言；**`UI KNOB` press/drag 证明触摸回调接线与相对拖拽**——按下不改值、
  +90° 改 +67（BPM range 200）、拖回锚点回原值、中心/环带外被忽略；FM6/SAMPLE 页
  另用 `ENG TRACK` 切引擎 + `ENG DESC` 断言 ALG 33 名 / SET 12 名，`UI KNOB` 在锁定
  页回 `ignored (locked)`；切页保值、九页池水位、五页截屏 + 像素抽检）；
  `scripts/knob-test.sh`（旋钮纯函数 + 格式化表 host 单测）；
  `UI FORCEFAIL global` 复验参数页的失败可见性与重建（重建后引擎真值不变）。

#### 8.6 FM6 页 bank 存取行（P4-B Task 13）

- **几何**：页面区相对 `y=552..596`（高 44）。旋钮盒底边 = `52 + 298 + 26 + 160 =
  536`，行与旋钮之间留 16px，页面区高 648 ⇒ **不压缩旋钮区**，也不需要改网格宏；
  `TAP` 命中矩形（`BANK_*` 宏）与建控件同源。行内容：`BANK`(16..104) / `-`(112..176) /
  槽号（`B<n+1>`，例 `B4`，184..280，UI_ACCENT 居中） / `+`(288..352) / `SAVE`(368..468，绿) /
  `ERASE`(484..584，橙) / 状态行(600..1264)。
- **语义**：槽号 = **目标槽** 0..26（显示 `B<槽+1>`），与 PTCH（音源，PTCH = 8 + 槽号）
  独立；`SAVE` 发 `CMD_BANK_PUT`（选中轨的 live patch = `fm6_patch[track]` 经
  `fm6_pack`，与 console `STORE BANK PUT` 同一入口），`ERASE` 发 `CMD_BANK_ERASE`。
  注意 FM6 的 8 个 EDIT 值是**宏**（上游 `fm6_sync` 叠加在 patch 上）——调 ALG 等宏
  **不改** `fm6_patch`，故 SAVE 存的是 patch 本体（与上游 "Store in bank" 同义）。
- **锁定页拒绝（R36）**：选中轨的引擎不是 FM6（含鼓轨——没有引擎）时，SAVE **和**
  ERASE 都**直接拒绝**：不发状态层命令、不动存储，状态行与 `TAP` 回显显示
  `NOT FM6`（红字；页内局部状态，不写 `app_state_t.store_status`）。理由：每个合成轨
  都有 `fm6_patch`（上电 INIT VOICE、之后是该轨最后一次载入的 patch），旧行为会在锁定
  页把这份「别的引擎轨上残留的 patch」写进用户银行槽；页眉此时已显示
  `… USES ENG k — N/A` / `(DRUM) — N/A`。
- **状态行**：非锁定页复用 `theme.h` 的 `ui_file_status_text`（act 0 = SAVE / 2 =
  ERASE），bank 行可达的文案只有 `SAVE OK` / `ERASE OK` / `STOP BEFORE SAVE` /
  `NO STORAGE` / `<ACT> ERROR`——`DRUM TRACK: NO SOUND` 在 bank 行**不可达**（鼓轨恒为
  锁定页 ⇒ `NOT FM6` 先于存储层；该令牌仍用于 console `STORE BANK PUT/LIVE` 的鼓轨
  守卫）。播放中由存储层拒绝（`store_status = 3`）；锁定页的 `NOT FM6` 是页内优先级
  更高的一档（先于存储结果）。
- **阻塞**：擦写在 LVGL 回调里发生（TAP/按钮 → `state_apply` → 存储层），一次
  SAVE/ERASE 最坏阻塞一次整银行擦写窗口（4KB 扇区重写；spec §4.3 接受的 ≤150ms 级
  UI 停顿，与工程/预置保存同一条「擦写期间不持引擎锁」纪律，ADR-009 §3）。
- **TAP kind**：`SLOOP_TAP_BANK_SLOT`（`b`=目标槽、`d`=±1）、`SLOOP_TAP_BANK_STORE`/
  `SLOOP_TAP_BANK_ERASE`（`b`=目标槽、`v`=0/2）；console 回显
  `-> BANK slot=k delta=±1` 与 `-> BANK slot=k action=save|erase status="…"`
  （状态串来自 `sloop_ui_bank_status_text()`，与屏幕同源）。
- **键盘**：`←/→` 仍只在旋钮间移动焦点（不改既有旋钮模型），bank 行**只经触摸/
  console**——与 FILE 页「SAVE/ERASE 只触摸 + console」同一取舍。
- **预算**：7 个单对象 label（BANK/槽号/±/SAVE/ERASE/状态行）+ 3 个静态样式 ⇒
  实测 delta **4,956–5,100B**（旧 2,864B），est `3,584 → 6,144`（512 对齐；对**最大**
  实测 delta 5,100 是 **+20.5%**，在预算规则 +10–23% 内；区间另一端 4,956 的 +24.0%
  （高过 +23% 上沿 1 pp）只是池历史让 delta 偏小的读数，不是余量不足——见 §3 注）；
  EDIT 组 Σ est `17,408 → 19,968`。组不变量见 §3（最坏对 FILE→STEP 不变；
  EDIT→STEP 成单独次坏，账面 60,904、余 2,216B）。

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
- 切页首次进入有一次构建延迟（实测 4–35ms，≤100ms 预算；PLAY 例外）。P4-B Task 11
  起有**组作用域销毁**（跨组切页时销毁源组全部已建页），组内不重建、无 LRU；
  若单组 Σ est 或跨组峰值超出池，必须回到本 ADR 的设计决策，而不是调大池。
- 池水位从「启动常量」变成「随访问增长」，soak 门禁（AGENTS §5.5）仍需在参数页落地后
  重跑：组作用域下采样时只驻留当前组（soak 停 TRANSPORT ⇒ SEQ），`UI MEM` 分组行
  给出该组 built/sum_delta。
- `UI MEM`/`UI FORCEFAIL` 是 console 诊断面：前者只读，后者一次性注入、默认关闭，
  不影响产品行为。
- P4-A Task 11 后：参数页每页 2.4–2.9KB（est 3,072–3,584B），九页平铺全建 free 2,816B
  （P4-B 组作用域下只驻留一组，EDIT 全建 free 26–34KB）；
  页签为两行（组签 + 子签，P4-B Task 11），右侧 220px 给上行 SLOOP / 下行层提示
  （ADR-005「导航决策」§2；九页平铺时 107px 的宽度墙已解除）。参数页的控件选择见 §8.2 的预算决策
  （单对象自绘旋钮），**不要**退回 `lv_arc`+`lv_label`（会撑爆池）。新增参数页的
  步骤：布局表加一行 X 宏（id/title/est + 8 槽）→ `PARAM_PAGE_COUNT` 与
  `app_page_t`/`APP_STATE_PAGES` 同步 → console 页名表 → 实测 est。
