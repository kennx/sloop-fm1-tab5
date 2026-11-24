# ADR-008: 页面懒建与 LVGL 池预算（阶段 4 参数页的前置框架）

> 状态：已接受（阶段 4，P4-0 Task 4；Task 9 补 §7 参数写/读接口与状态层投影；
> **Task 10 定稿**：§8 参数页框架（knob/布局表/格式化/编辑路径/预算）；**Task 11 收口**：
> 五参数页（+FM6/SAMPLE）、格式化截断口径、`UI KNOB` locked 令牌、九页预算表；**P4-C
> Task 11**：SONG 页 est 12,288B 与六组页签（§3 的 SONG 行、§5 的 12 页有序对））｜
> 日期：2026-11-21（P4-C 更新 2026-11-23）
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

- **注册顺序 = 组内子签顺序**（P4-B Task 11 起：页签是**两行**——上行组签
  （P4-D Task 8 起 7 个：`PLAY/TRACKS/EDIT/SYNTH/SEQ/SONG/FILE`），下行当前组子签；组模型
  在 `ui.c` 的 `k_groups[]`，页签文本/数量由「组 + 组内已注册页」派生，不再由平铺注册表
  派生。组签宽度按 7 组算（**142px**：7×142+6×8=1,042，右端 16+1,042=**1,058** ≤
  1,280−216 reserve；P4-C 的 6 组口径是 168px/右端 1,064），子签复用下行 5 个槽
  （176px，右端 928）；右侧 216px 留给 SLOOP 标题/层提示，见 §8.1 与 ADR-005 §2/§5.1）。
  **注册表只增不删**：外部组件可在
  `sloop_ui_start()` 之前调 `ui_page_register()` 预注册页面，内置页注册幂等
  （`static done` 标志 + 按 id 去重）；无已注册页的组不显示。输入层的 `TAB` 轮转
  用 `sloop_ui_page_order_next()`（组顺序的**扁平**轮转：组内子页→下一组第一页），
  不按 `sloop_ui_page_count()`，也不按 `APP_STATE_PAGES`。
- **9 页内置**在 `sloop_ui_start()` 内注册（`k_builtin_pages`：PLAY/TRACKS/STEP/TRANSPORT +
  SONG + FILE 组 PROJECTS/PRESETS + P4-D Task 8 的 SYNTH 占位页 DSYN/FM6P）；参数页
  （GLOBAL/FX/ANALOG/FM6/SAMPLE）由 `page_layout_register()` 注册进同一注册表——交付态
  共 **14 页 / 7 组**（ADR-005 §5.1）。`ui_page_register()` 供后续页加入。`id` 受 `APP_STATE_PAGES` 约束（状态层
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
| TRANSPORT | 3,788 B（Task 10 前）→ **8,248–8,716 B**（P4-C Task 10，21 次首建读数） | 4,608 B → **9,216 B** | +22% → **+5.7%**（对最大 8,716；见 §3 注） | 旧：4 按钮（各带子 label）+ 4 标签（12 对象）；Task 10：4 个走带按钮收敛为**单对象** chip + 4 标签 + 18 个工具 chip（3 录音/fill + 6 聚焦步字段 + 9 pattern）= 26 对象；工具动作结果复用走带状态 label |
| GLOBAL | 2,856 B | 3,584 B | +25% | 8 旋钮（单对象自绘）+ 页眉 + 2 个 ± 按钮（Task 10） |
| FX | 2,856 B | 3,584 B | +25% | 同上（Task 10；实测 2,816–2,876 随池历史漂移） |
| ANALOG | 2,868 B | 3,584 B | +25% | 同上（Task 10；实测 2,848–2,868） |
| FM6 | 2,864 B（Task 11）→ **4,956–5,100 B**（Task 13，含 bank 行） | 3,584 B → **6,144 B** | +25% → **+20.5%**（对 5,100；区间 4,956 处 +24.0%，见 §3 注） | 8 旋钮（ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH）+ 底部 bank 存取行（BANK 标签/±/槽号/SAVE/ERASE/状态行 = 7 个单对象 label，Task 13） |
| SAMPLE | 2,432 B（P4-D Task 8 前）→ **4,360–4,508 B**（Task 11，含槽管理行） | 3,072 B → **6,144 B**（Task 8，spec §4.4 固定上限） | **+36.3%–40.9%**（**高于 +23% 规则上沿**：est 是 spec 锁定值，见下条 SYNTH 表注） | 6 旋钮（空槽 edit[5]/edit[7] 不建）+ 槽管理行（`U1..U4` / `ERASE` / 状态行 = 6 个单对象 label，P4-D Task 8） |
| DSYN | **6,436 B**（P4-D Task 9 设备实测；初版 26 对象 7,724B 超 est 556B → 压缩到 22 对象） | **7,168 B**（spec §4.4 固定上限） | **+11.4%** | 22 对象：8 旋钮（常驻、当前字段页空槽隐藏）+ 4 kit chip + LANE 三区对象（标签/数值/± 合一）+ 4 字段页 chip + AUDITION/STORE + 底部 ± 两 chip + 状态行（§8.7） |
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
| PRESETS | 12,008–12,276 B（多次运行；Task 10 归档读数 12,008/12,044/12,276，见 `inject-nav-20261122-234244.log` 与 `task-10fix-transport-green.log`） | 12,800 B | +4.4% | 32 格（单对象可点击 label，两行文本）+ 3 按钮 + 状态行 = 36 对象 |
| **FILE 组 Σ** | **19.8–20.0 KB** | **20,992 B** | — | 两页同时已建（组内切页只改显隐）；过渡峰值见 ADR-005 §5（free 7,308–7,840B） |
| SONG | 11,136–11,564 B（P4-C Task 11，`song` 场景多次首建；`UI MEM` 读数 11,136/11,228/11,384/11,548/11,564） | 12,288 B | +6.3%–10.3% | 35 个单对象可点击 label（状态 1 + A–D 卡 4 + 动作 chip 8（CANCEL 常建、按态隐藏）+ 16 条目 + 配置 chip 6）+ 页容器；组预算下 FILE→STEP 不受影响，est 保持 spec 的计划上限 |

**SYNTH 组三页（P4-D Task 8 落位、Task 9/10 单页实测、Task 11 组口径收口）**：

| 页 | 实测 delta（首次构建，`UI MEM`） | est_bytes | 余量 | 说明 |
|---|---|---|---|---|
| SAMPLE | **4,360–4,508 B**（`UI MEM` delta，跨快照 min–max；`页面构建` 池增量 4,148–4,508） | 6,144 B | +36.3%–40.9% | 6 旋钮 + 槽管理行（`U1..U4`+`ERASE`+状态行）。**est 是 spec §4.4 的固定上限**：实测只用到 71–73%，预留不回收（收 est 会改 SYNTH Σ 与有序对，spec 锁定） |
| DSYN | **6,344–6,452 B**（`UI MEM` delta，跨快照 min–max；构建日志 6,100–6,452，最小值为 soak 预热；Task 9 首测 6,436 在区间内） | 7,168 B | +11.1%–13.0% | 22 对象（§8.7）；初版 26 对象 7,724B 超 est 556B → 压缩 |
| FM6P | **6,980–7,124 B**（`UI MEM` delta；构建日志 6,832–7,124；Task 10 fix 的 7,124B 为最大值） | 7,168 B | +0.6%–2.7% | 24 对象（§8.8）；余量最薄 **44B**（7,168−7,124），**刻意保留为 tripwire**：加控件前先减对象 |
| **SYNTH 组 Σ** | **17,696–17,936 B**（`UI MEM sum_delta` 的 `built=3` 读数；与逐页区间不同源，不相加） | **20,480 B** | — | 组内三页共驻留（同组切页只改显隐）；`build_ms` 12–13ms；跨组峰值见 ADR-005 §5.1（SYNTH→STEP 实测 `pool_free 11,244B`） |

- **SAMPLE 槽 ERASE 的 UI 停顿**（P4-D 评审 M5）：`ERASE` chip 在 LVGL 回调里同步擦
  20 个 4KB 扇区（整槽 80KiB），最坏阻塞一次整槽擦写。**未单独测时**；量级同 spec
  §4.3 的 ≤150ms 级 UI 停顿。`STORE STATS` 的 `last_save_ms` 不覆盖采样擦除
  （`sloop_store_sample_erase` 不走 `store_note_write`）。走带中由存储层拒绝，故不会
  在播放中触发。

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
  它们参与「FILE 全建 → 首建 STEP」这个新的最坏有序对，余量从 STEP 的 4,776B 压到 1,192B（P4-B 时点；P4-C Task 11 页签单对象化后
  base 降到 11,624B，该对余量回到 3,368B）；当时的账面（`13,800 + 22,528 + 23,040 +
  4,096 = 63,464 > 63,120`）是这条 +4.4% 的理由。判据
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
`base + max(源组 Σ est + 目标组首建页 est) + 4KB ≤ 63,120B`（base **11,624B**，
P4-C Task 11 最终固件：boot `UI MEM` free 49,016 + PLAY delta 2,480；P4-B 的
`lv_button + 子 label` 两对象页签同法为 13,800B，Task 11 单对象化省 2,176B；旧值
7,016B 是 `pool_used` 口径的低估，见 ADR-005 §5）。**最坏有序对 = SEQ 两页全建 →
首建 PRESETS**（P4-C Task 10 起）：`11,624 + (23,040+9,216) + 12,800 + 4,096 = 60,776
≤ 63,120`（余 2,344B；保守 base 11,936 时余 2,032B）；实测过渡峰值 `pool_free 8,324B`
（`transport` ⑩j 断言 ≥4,096B），仍是弧线 mask 峰值（≈2.6KB）的 3.2 倍。**按当前
est 表的有序对排名**（P4-C Task 10 起 SEQ Σest 由 27,648 抬到 32,256；Task 11 加入
SONG 后重排）：
① SEQ→PRESETS `45,056B`（账面 `11,624 + 45,056 + 4,096 = 60,776`、余 **2,344B**；
TRANSPORT est 9,216 是**计划锁定的上限**，实测 delta 必须 ≤ est）。
**已按设备实测（P4-C Task 11 最终固件）**：SEQ 全建后首建 PRESETS 的构建日志为
`池增量=12,068B 池剩余=8,324B`（`task-11-transport-green4.log`）⇒ 过渡峰值
`pool_free=8,324B ≥ 4,096`；PRESETS delta 的归档区间 12,008–12,276B（池历史漂移；
Task 10 读数 12,276B → 峰值 6,240B，Task 11 页签单对象化后 8,324B）。
② SEQ→SONG `44,544B`（Task 11 新增：32,256 + SONG 12,288；账面 `11,624 + 44,544 +
4,096 = 60,264`、余 **2,856B**；保守 base 11,936 时余 2,544B）。
③ FILE→STEP `44,032B`（账面 59,752、余 3,368B；保守 base 11,936 时余 3,056B）；
④ EDIT→STEP `43,008B`（Task 13 的 FM6 bank 行使 EDIT Σ = 19,968B；账面
`11,624 + 43,008 + 4,096 = 58,728`、余 **4,392B**；保守 base 余 4,080B）。每页 `free ≥ est` 前置检查不变。切组顺序固定为「建目标页 → 切到
目标页 → 销毁源组**全部**已建页（含原当前页）」——源组页面在**切换之后**才释放（先销毁
会出现空白帧、输入路径在跨组切换期间失去活页），故峰值是瞬态且必须被上式覆盖；离开组后
源组 `built=0`。组内首次访问兄弟页时构建、同组后续切换只改显隐。`UI MEM` 增加
`GROUP <name> built=<n> sum_est=<B> sum_delta=<B> build_ms=<ms>` 分组行。上表九页实测
数据保留为 P4-A 终审时点的平铺历史口径；新页（FILE 组 PROJECTS/PRESETS、SONG 等）按组预算
纳入，不再要求「全部页面同时已建」。

**TRANSPORT 落地的预算例外（P4-C Task 10，2026-11-22 实测）**：18 个工具 chip 全用
单对象可点击 label、动作结果复用走带状态 label 后，首次构建 `池增量` 在 21 次读数里
为 8,248–8,716B（预算 9,216B、池剩余 38,172B 起、耗时 12–14ms；`GROUP SEQ
sum_est=32,256`）——跨度来自池历史（同页源码、不同分配序的文本缓冲位置）。其中
**归档可见**的读数是 8,296 / 8,312B（`task-10fix-transport-green.log` /
`task-10-transport-green.log`）；其余读数来自 `build/verify` 的临时日志（已被
`--clean` 清除）。est 保持计划上限
9,216 而不是按 +10–23% 收紧到 9,728+：**上限是由有序对不变量锁定的**（SEQ→PRESETS 只剩
168B），再把 est 抬到实测 +10% 会顶破 63,120B。est 仍然 ≥ delta（对最大 8,716 是 +5.7%），真门禁
（逐页 `free ≥ est` 与过渡后 `pool_free ≥ 4,096B`）不变；这与 FILE 两页（+5.5%/+4.4%）
是同一类例外。若后续再加工具，先改对象设计（合并 chip/复用绘制），不抬 est。

**P4-C 12 页预算（Task 11 已落地）**：SONG 使用独立
顶层组，组顺序 `PLAY/TRACKS/EDIT/SEQ/SONG/FILE`，不加入 SEQ 稳态；TRANSPORT est
上限由 4,608 调整为 **9,216B**（落地实测 delta 8,248–8,716B），SONG est **12,288B**
（Task 11 实测 delta 11,136–11,564B，保持计划上限——它不参与最坏对，抬 est 没有收益）。
页签从 `lv_button + 子 label` 收敛为**单对象可点击 label**（11 个对象），基础占用
13,800 → **11,624B**（boot `UI MEM` free 49,016 + PLAY 2,480）。按 base 11,624B 与
4,096B 切换余量：最坏 = SEQ→PRESETS `60,776B`（余 2,344B）；SEQ→SONG `60,264B`
（余 2,856B）；FILE→STEP `59,752B`（余 3,368B）；EDIT→STEP `58,728B`；SONG→PRESETS
`40,808B`。均 ≤63,120B。落地面测：`transport` ⑩j 的 SEQ→PRESETS 峰值
`pool_free 8,324B`、`song` ⑩ 的 FILE→STEP 峰值 `9,744–9,860B`（均 ≥4,096B）；逐页
`free ≥ est` 不变；超预算先改对象设计，不抬 est、不扩池。

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
     `base + max(源组 Σ est + 目标组首建页 est) + 4KB ≤ 63,120B`（P4-C Task 11
     实测 base 11,624B：账面最坏对 = SEQ 全建 → 首建 PRESETS
     `11,624 + 45,056 + 4,096 = 60,776`、余 2,344B；次坏 = SEQ→SONG 60,264、余
     2,856B；FILE→STEP 59,752、余 3,368B；实测 SEQ→PRESETS 峰值 `pool_free 8,324B`、
     FILE→STEP `9,744–9,860B`）。源组页面在切换后才释放，
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
`reserve` 只是展示值，门禁见 §5）。P4-B 交付态（11 页注册 + 组作用域；P4-C Task 11 后 12 页，SONG 行见 §3；
`store`/`nav`/`song` 会话实测）`UI MEM` 形如：

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
  行）、SAMPLE 6,144B（P4-D Task 8 的槽管理行）、DSYN 7,168B（P4-D Task 9，实测
  6,344–6,452B）、FM6P 7,168B（Task 10 首测 → Task 11/12 归档 6,980–7,124B））；P4-B 起按组核算：P4-D Task 8 起
  SAMPLE 随 **SYNTH** 组，EDIT Σ **16,896B**、SYNTH Σ **20,480B**（ADR-005 §5.1；
  全建后 `pool_free` 实测 26–34KB；九页平铺时点的 2,816B 仅作历史对照）。
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

### 8.7 DSYN 鼓合成编辑页（P4-D Task 9）

- **对象与几何**（页面区相对坐标，y 绝对 = +72）：页眉一行 y∈[4,36)——
  `SYN1..SYN4` 四 kit chip（x=16+70k，64×32）、LANE **三区对象**
  `-   LANE nn   +`（298..478，180×32；左右各 42px 是 ∓1 区、中段是标签，点击靠指针
  相对 x 判区）、字段页 chip `SND1/SND2/SND3/KIT`（x=526+74i，68×32）、
  `AUDITION`（828，116×32）、`STORE`（952，88×32）、状态行（1048..1264，16pt 右对齐）；
  8 旋钮沿用参数页网格（cell 300×290，x=16+c·316、y=52+r·298，盒 160×160）；
  底部 `−`/`+`（16/104，80×44，y=552..596）作用于焦点槽 ±1（TAP 不能拖拽，这是
  TAP 能触发的唯一字段写入口）。
- **字段页**（页内 UI 局部态，不新增 app page）：SND1 = WAVE/SRC/CLAP/PITCH/FINE/BEND/
  BTIME/HOLD；SND2 = DECAY/TLEV/T2/T2LEV/CLICK/NLEV/NHOLD/NDEC；SND3 = FLT_MODE/
  FLT_RES/FCUT/FENV/HPF/CHIP/DRIVE/LEVEL；KIT = CRUSH/KIT SRC。8 个旋钮对象**常驻**，
  当前页没有字段的槽隐藏（隐藏 = 不绘制/不命中，池峰值按 8 个算，组内切页不重新分配）。
- **值/写路径**：读 `app_state_t.dsyn`（`sloop_dsyn_field_from_sound` 纯解码；kit 级
  crush/src 直取投影）；写 `CMD_DSYN_SET`（a=kit b=lane n=field）、KIT SRC =
  `CMD_DSYN_LOAD_KIT`、KIT 页 `CRUSH` = `CMD_DSYN_CRUSH`（a=kit，v=0..255；状态层校验、
  core 钳位 + `dsu_fix_kit` + `dsu_dirty`）、`AUDITION` = `CMD_DSYN_AUDITION`（力度固定
  100）、`STORE` = `CMD_DSYN_STORE`（走带中 → `STOP BEFORE STORE`）。命令成功后投影同拍刷新，本页
  立即重绘（不做本地值推测）。kit/lane 选择走 `state_set_dsyn_select`（与
  `UI DSYN KIT/LANE` 共用，`STATE SNAP` 立即反映）；DSYN 与选中轨无关，**无锁定态**。
- **数字文本**：`knob_t` 增加可选 `text_cb`（默认 NULL → `knob_format_value`）；DSYN
  的 24 个字段由 `sloop_core_dsyn_text` 给出 ms（`DECAY_K` 反解）/Hz（`CUTOFF_HZ`）/
  0.25dB/FLT 名/音名，UI 不引入上游生成表。LEVEL 文本按上游注释 `128 + 4 × dB` 精确到
  百分之一 dB（`0.0dB` / `+6.0dB` / `+0.25dB` / `+31.75dB` / `-32.0dB`，正数带 `+`）。
  ms 是**显示近似**：`10254/(65536-DECAY_K[i])` 一阶反解钳 5..4000ms（长衰减端与真实
  −60dB 时间差十几%），只影响文本、不参与声音。
- **KIT 页**：`CRUSH` 0..255 可编辑（P4-D Task 9 fix 补 `sloop_core_dsyn_set_crush` +
  `CMD_DSYN_CRUSH`）：与 24 个声音字段**同一条** `dsyn_write` 路径（拖拽/↑↓/± 都写值、
  写后从投影回读），不另写动作函数。`KIT SRC` 值变化（拖拽/↑↓/±）
  **立即** `CMD_DSYN_LOAD_KIT`（上游 DSYN_PUT part 17 同义：套用即覆盖该 kit 的 16 lane
  与 name/crush/src）；点击旋钮只设焦点（TAP 不能拖拽，避免误触清空编辑）。名称用状态行
  显示（无动作结果时 `KIT <name>`），来源数值就是 KIT SRC 旋钮读数。
- **状态行**（单一映射 `theme.h` 的 `ui_dsyn_status_text`）：act（SET/STORE/KIT/
  AUDITION/NONE）× `synth_status` × `dsyn.dirty` × `sloop_store_ready()` →
  `EDIT OK` / `STORE OK` / `KIT LOADED` / `AUDITION` / `INVALID` / `STOP BEFORE STORE` /
  `FROZEN` / `NO STORAGE` / `STORE ERROR`，**所有**非空文本后按 dirty 追加 ` DIRTY`
  （失败分支也不静默吞脏位）。换 kit/lane/字段页清掉「上一次动作」的结果（新上下文不显示
  旧 lane 的 EDIT OK）。
- **TAP kind**：`SLOOP_TAP_DSYN_SEL`（a=kit b=lane d=page v=1；chip 与旋钮焦点）、
  `SLOOP_TAP_DSYN_FIELD`（a=字段码、v=写后回读、err=0 成功/2 命令失败；CRUSH 也走这条）、
  `SLOOP_TAP_DSYN_STORE`、`SLOOP_TAP_DSYN_AUDITION`（a=kit b=lane）、
  `SLOOP_TAP_DSYN_KIT`（a=kit b=新 src、v=写后回读）。回显
  `-> DSYN kit=.. lane=.. page=.. select` / `-> DSYN field=<name> value=<v> status="…"` /
  `-> DSYN STORE status="…"` / `-> DSYN AUDITION lane=n` / `-> DSYN KIT src=n`；LANE 中段
  （标签）按 `TAP miss`。字段名由 `sloop_ui_dsyn_field_name()`（0x80/0x81 = CRUSH/KIT SRC）。
- **键盘**：←/→ 焦点（跳过当前页空槽、环绕）、↑/↓ 调焦点槽 ±1；输入任务上下文只写状态层
  （`render=false`，控件由 30ms 定时器刷新，与参数页 `sloop_ui_param_adjust` 同纪律）。
  kit/lane/字段页 chip 只经触摸/console。
- **预算（est 7,168B 固定）**：初版 26 个对象实测 **7,724B**（超 est 556B）→ 压缩：
  `LANE` 标签/数值/± 合成一个三区对象（−2）、KIT 名称改由状态行承载（−1）、± 保留在
  底部；22 个对象、实测 **6,436B**（+11.4%，512 对齐下一条是 6,656/7,168，符合 +10–23%
  规则）。SYNTH Σest 仍 **20,480B**（SAMPLE 6,144 + DSYN 7,168 + FM6P 7,168），组不变量
  不变。设备证据：`UI MEM page id=12 est=7168 delta=6436`（Task 9 报告）。
- **验证入口**：host `scripts/synth-test.sh`（dsyn text 全字段）；设备 `UI DSYN KIT/LANE/
  PAGE` + TAP（选择器/字段 ±/CRUSH 写/STORE/AUDITION/KIT 中段 miss）+ `DSYN CRUSH` + 键盘 →/↑ +
  `UI MEM` + 四字段页截屏与像素抽检（Task 9 探针；任务 11 的 `synth` 场景固化）。

### 8.8 FM6P 深度 patch 编辑页（P4-D Task 10）

- **对象与几何**（页面区相对坐标，y 绝对 = +72；页面区 x 到 1,264）：页眉一行
  y∈[4,36)——`OP1..OP6` 六算子 chip（x=16+60k，56×32）、字段页 chip
  `EG/LEVEL/PITCH/PEG/VOICE/LFO`（x=380+68p，64×32）、BANK **三区对象**
  `-  Bnn  +`（792..916，124×32；左右各 40px 是 ∓1 区、中段是槽号标签，点击靠指针
  相对 x 判区）、`SAVE`（924，72×32）、状态行（1004..1264，16pt 右对齐）；8 旋钮沿用
  参数页网格（cell 300×290，x=16+c·316、y=52+r·298，盒 160×160）；底部**一个三区
  对象** `−        +`（16..184，168×44，y=552..596）作用于焦点槽 ±1（TAP 不能拖拽，
  这是 TAP 能触发的唯一字段写入口）。左右各 **72px** 是 ± 区、中段 **24px（72..96）是
  死区**（`TAP miss`；把 zone 设成半个宽度会让整条都能写值，中点误触即改音色）。
  24 个对象（8 旋钮 + 12 chip + BANK + SAVE + 状态行 + 细调）。
- **字段页**（页内 UI 局部态，不新增 app page；算子 chip 也只影响前三页）：
  EG = R1/R2/R3/R4/L1/L2/L3/L4（op 字段 0..7）；LEVEL = BP/LD/RD/LC/RC/RS/KVS/OL
  （8..13,15,16；AMS=14 在 PITCH 页）；PITCH = MODE/FC/FF/DET/AMS（17..20,14）+ 3 空；
  PEG = PR1..PR4/PL1..PL4（全局 0..7）；VOICE = ALG/FB/OKS/TRNSP（全局 8/9/10/18）+
  4 空；LFO = LFS/LFD/LPMD/LAMD/LKS/LFW/LPMS（全局 11..17）+ 1 空。字段编号 = Task 3
  冻结的编号（op 0..20 = `FP_R1..FP_DET`；全局 0..18 = `FP_PR1..FP_TRNSP`），label/
  范围/枚举全部来自 `sloop_core_fm6_desc`（`fm6_max()` 同一张表），全局 19..28 是名称
  字节（只读）不进任何页。8 个旋钮对象**常驻**，当前页没有字段的槽隐藏（池峰值按 8 个
  算，隐藏 = 不绘制/不命中）。
- **值/写路径**：读 `app_state_t.fm6p`（选中轨的 155B live patch 投影，纯解码
  `sloop_fm6_field_from_patch`）；写 `CMD_FM6_SET`（a=track b=op n=field，op 6 = 全局
  块）、`SAVE` = `CMD_BANK_PUT`（目标槽是页内 0..26，与 FM6 宏页的 bank 行同一套 27
  槽）。数字文本走 `knob_format_value`（ONOFF / ALG "1".."32" / LFW / LPMS 枚举都由
  desc 给出，无需 core 的索引→文本表）。
- **ALG 宏覆盖**（spec §4.1）：ALG = 全局字段 **8**（`SLOOP_FM6_ALG_FIELD` =
  `FP_ALG - FP_PR1` = 134−126；VOICE 页首槽；state/console/page 共用这一公共常量）。
  写 ALG 时适配层在同一次引擎锁内清 `trk[track].p[P_E0]`（宏 → PAT），否则上游
  `fm6_sync` 会用宏覆盖 patch 的算法、编辑听不到；状态层 `cmd_fm6_set` 因此在 ALG
  分支**同拍重取 `p[]`**（否则 UI 的 `ALG MACRO n` 提示与 TAP 回显会多留一拍）。
  `P_E0 != 0` 时状态行显示 `ALG MACRO n`（1..32）：**优先级 = 失败 > 提示 > 成功结果**
  ——覆盖警告是持续事实、`EDIT OK` 是瞬时的；编辑 ALG 成功后宏已被清，提示自然消失。
  其余宏（FB/MLVL/MRAT/MEG/VMOD/DTUN）是叠加语义，写 patch 字段不动它们。
- **锁定态**（`app_state_t.fm6p.engine_ok == 0`：选中轨不是 FM6，含鼓轨）：8 个旋钮一律
  `--`（`knob_show_locked`），`OP/PAGE/BANK` chip、`SAVE`、底部 ± 全部**拒绝**（不下发
  命令、不改选择），状态行 `NOT FM6`（红字，页内状态，不写 `synth_status`/`store_status`）；
  解锁时清掉上一次动作结果。`UI FM6P OP/PAGE` 是 console 直写的页内量，不受锁定影响
  （脚本据此在锁定态断言语义不变）。
- **状态行**（单一映射 `theme.h` 的 `ui_fm6p_status_text`/`ui_fm6p_status_color`）：
  act（SAVE 走 `ui_file_status_text` 的 act 0 文案 / FIELD 走 `synth_status`：
  `EDIT OK`、`INVALID`、`FULL`、`BLOCKED`、`FROZEN`、`LOCKED`、`EDIT ERROR`、未就绪
  `NO STORAGE`）× locked × `P_E0` 宏 → 文本；颜色 成功绿 / 锁定与失败红 / ALG 提示
  强调黄 / 其余灰。换页/换算子/锁定态变化清掉上一次动作结果。
- **TAP kind**：`SLOOP_TAP_FM6P_SEL`（a=op b=page v=1；chip 与旋钮焦点）、
  `SLOOP_TAP_FM6P_FIELD`（a=op b=field、v=写后回读、err=0 成功/1 锁定页拒绝（未下发
  命令）/2 命令失败）、`SLOOP_TAP_FM6P_BANK`（a=目标槽 d=±1）、`SLOOP_TAP_FM6P_SAVE`
  （a=目标槽 v=0 已下发/1 锁定页拒绝）。回显 `-> FM6P op=n page=p select` /
  `-> FM6P <field> value=<v> status="…"`（拒绝另带 `ERR locked|error`）/
  `-> FM6P BANK slot=k select` / `-> FM6P SAVE status="…"`；字段名由
  `sloop_ui_fm6p_field_name(op, field)`（描述符 label）。BANK 槽号中段与底部细调
  中段按 `TAP miss`。
- **键盘**：←/→ 焦点（跳过当前页空槽、环绕）、↑/↓ 调焦点槽 ±1（锁定页空操作）；输入
  任务上下文只写状态层（`render=false`，控件由 30ms 定时器刷新）。chip 只经触摸/console。
- **预算（est 7,168B 固定）**：24 个对象实测 **6,980–7,124B**（Task 10 fix 7,124B 为最大；≤ est；区间取决于
  构建时的池历史）。**最薄余量 44B 是刻意保留的 tripwire**：不改 est（改到 7,680 会让
  SYNTH Σ 21,088、SYNTH→STEP 账面 59,752B，仍在 60,776B 最坏有序对之内，但没有必要），
  任何新增控件都必须先减对象或走一次预算评审。SYNTH Σest 仍 **20,480B**（SAMPLE 6,144 + DSYN 7,168 + FM6P 7,168），
  ADR-005 §5 的组不变量不变。设备证据见任务 10 报告与 `docs/dev/verify/4-pd-2026-11-23/`。
- **验证入口**：设备 `UI FM6P OP/PAGE` + `TAP`（六页 × 8 槽的字段名/写后回读、锁定页
  `NOT FM6`、ALG 宏提示、BANK SAVE→`STORE BANK GET` 往返）+ 键盘 ←/→/↑ + `UI MEM` +
  三张截屏与像素抽检（Task 10 探针；任务 11/12 的 `synth` 场景固化）。

### 8.9 参数页 chip 框架（P4-F Task 4：MOD / SCALE / VOICE）

- **布局表升为 chip 结构**（`page_layout.{h,c}`，**不含 LVGL**、host 单测
  `scripts/knob-test.sh` 同源编译）：`param_page_t{page_id,title,est_bytes,chip_count,
  chips[5]}`，`param_chip_t{name,when,slot_count,slots[8]}`，`param_slot_t{scope,engine,id,
  drum_ok}`。页 id/title/est/chip/槽来自同一张 X-macro 表；`ui_page_t` 注册条目在
  `page_params.c` 注册时由表填出（`page_layout_register` 迁到那里——它需要 LVGL）。
  **权威槽表 = `docs/design/012-feature-parity-checklist.md` §3**；host 测试用上游
  `core.h:48-75` 的枚举顺序独立复刻 `tP_*`，逐槽比对 MOD/SCALE/VOICE/FX（布局表写的是
  `L_P_*` 数字，抄错即 FAIL）。
- **两类页**：`chip_count == 0` 的旧式整页（GLOBAL/FX/ANALOG/FM6/SAMPLE：一个无名单 chip、
  不画 chip 行、对象与行为不变）；`chip_count ≥ 2` 的 chip 页（MOD 5 chip、SCALE 4、
  VOICE 4）——8 个旋钮对象常驻，切 chip **零分配**重配：只换 `desc`（label/fmt/范围）、
  显隐与焦点，不新建/删除对象、不改样式。选中 chip 是**页内 UI 局部态**（跨组页面重建
  保留），chip 行、TAP、键盘与 console `UI <page> CHIP <n>` 共用。
- **chip 行 = 一个自绘可点击对象**（页眉行 y=4..36；chip 宽 `11×字符数+16`、间距 8、
  x0=16；`LV_EVENT_DRAW_MAIN` 画圆角底 + 居中 16pt 名字，`CLICKED` 按指针 x 选 chip；
  标题「`<页> · TRACK n`」挪到行右侧）。取舍：首版每 chip 一个可点击 label 实测
  **336B/chip**（MOD 5 chip ⇒ delta 4,544B，est 须 5,120，EDIT→STEP 账面 63,848 >
  63,120）；单对象后 **3,012–3,088B**，**est 3,584**（+16..19%、512 对齐，与旧式参数页
  同值）。EDIT Σ 20,480、MIX Σ 7,168（ADR-005 §5.3）。
- **描述符来源**：TRACK scope 一律 `sloop_core_desc_track_live(选中轨, id)`（Task 3；
  ENV SYNC 的 ATK/DEC/REL 音符名、INSERT A/B/C、引擎钩子的动态标签，范围同静态表）——
  FX 页原来的 `ATK/ESYNC` 手工标注因此删除；GLOBAL/ENGINE scope 仍取静态
  `sloop_core_desc`。页面上下文键加入 `P_ESYNC`/`P_ITYPE`，两者翻转即重解析已建槽的
  描述符并重绘。
- **鼓轨规则逐槽**（布局表 `drum_ok`，012 §3「鼓」列）：选中鼓轨时未标 `drum_ok` 的
  TRACK/ENGINE 槽显示 `--` 且拒绝编辑（`TAP ±` 回 `-> PARAM slot=n locked (page or slot
  N/A)`、`UI KNOB` 回 `ignored (locked)`、键盘无动作，**不发命令**）；标了的照常
  （FX：TFLT/LEVEL/PAN；VOICE：PAN；PATTERN 四槽）；全局参数不受影响。旧式引擎页
  （ANALOG/FM6/SAMPLE）的整页锁不变；逐槽规则页在鼓轨上的页眉是
  `… · TRACK 4 (DRUM)`（不是红字 N/A）。
- **条件 chip**：`when` 谓词按选中轨上下文求值——`CHIP_WHEN_ANALOG`（VOICE·FILTER =
  `P_FTYPE` + ANALOG 的 E4/E5/E7，上游 `page_shown`：仅 ANALOG 合成轨出现）。隐藏的
  chip 那一格留空（下标不塌陷、可见 chip 位置恒定），TAP miss、`UI VOICE CHIP 3` 回
  `UI ERR chip (hidden)`；选中的 chip 变隐藏时生效 chip 落到首个可见并回写选择。
- **键盘**：←/→ 在当前 chip 的存在槽间环绕（跳过空/隐藏槽）、↑/↓ 调焦点槽 ±1、
  **ENTER = 焦点槽 +1**（全部参数页生效；锁定槽无动作）。chip 只经触摸与 console。
- **console**：`UI <参数页名> CHIP [<n>]`——写选中 chip（LVGL 锁内立即重配；越界/
  非法 `UI ERR chip`、隐藏 `UI ERR chip (hidden)`、旧式整页不可写）；回显
  `UI <page> chip=<n>` + 生效 chip 的逐槽行 `UI <page> slot=<k> id=<绝对 id>
  label="<旋钮正在显示的名称>" locked=<0|1>`（旧式整页也可读——鼓轨逐槽规则的机读口；
  脚本把 label 与 `ENG DESC live <track> <id>` 逐槽对照）。TAP 回显
  `-> PARAMS <page> chip=<n> select`（`SLOOP_TAP_PARAM_CHIP`）。
- **验证入口**：`scripts/knob-test.sh`（表完整性 + 槽位 = 012 §3 + 可见性/锁定规则 +
  九组导航）；`scripts/inject/params.sh` ⑩（三页 est/构建、每 chip 逐槽 label ==
  `ENG DESC live`、首槽 ± 回读、chip 行 TAP、键盘、20 次切 chip 零分配、鼓轨逐槽、
  FILTER 条件 chip；六页全面矩阵留给 Task 7）。FX/GLOBAL 的 chip 化（SLICER/INSERT/
  REV/DRUMS/TEXTURE）是 Task 5，同一框架加表行即可。

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
  - 现状（P4-A；**P4-F Task 3/4 起已改**：TRACK scope 一律走
    `sloop_core_desc_track_live`，ATK/DEC/REL 直接显示上游音符名，下面的手工标注与
    route a 复核项都已落地，见 §8.9）：FX 页在选中轨 `P_ESYNC != 0` 时把 ATK 槽标注为
    `ATK/ESYNC` 并按 `F_INT` 原始值 0..127 显示（无 ms 后缀，`page_params.c` 的 `slot_desc`），
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
