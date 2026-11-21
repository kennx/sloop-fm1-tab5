# 阶段 4：独立模式功能对齐设计规格（Phase-4 Spec）

> 日期：2026-11-21 ｜ 状态：草案（待项目负责人评审）
> 上位文档：`docs/design/002-port-roadmap.md` §6（任务与完成判据，权威）；阶段 2/3 spec；ADR-005/006/007。
> 本阶段体量最大。按「多个独立子项目」处理：本文定义**子里程碑拆分、顺序、共享框架**；每个子里程碑开工时按既定粒度另立详细计划（阶段 1–3 同款协议）。

## 1. 目标

独立模式达到与 FM-1 上 SLOOP **等价**的完整体验：全页面（参数编辑、音序/编排、采样/鼓合成、FM6、设置、歌曲）、引擎特性补齐（TIE/SLIDE/SLICER、参数锁、微时序、fill）、预置/工程存取（FATFS/NVS）、全页面截屏套件、数小时级 soak + 电源/温度记录；**性能优化只在此之后**（roadmap §0.1）。

**完成判据（roadmap §6）**：功能对齐清单全绿；全页面截屏套件通过且证据入库；soak 量化判据通过；**强制里程碑：dev → main 合并**。

## 2. 现状与差距（本次侦察实证）

| 领域 | 现状 | 差距 |
|---|---|---|
| 参数描述符 | `params.c` 已有完整 `param_desc_t`（label/fmt/min/max/def/enums；`TP[]`/`GP[]`/`ENGINES[].edit[]`） | **无对外读取 API**；UI 无参数页 |
| 组件编译集 | `libc/core/engines/drums/params/voice/slicer/fx/usb/midi_uart/seq`（+regress） | `project.c`、`storage.c`、`upreset.c`、`arranger.c(+scene)`、`fm6_bank.c` **未编译**；无存储后端 |
| 音序深度 | STEP 网格 16 列；toggle/erase 对齐上游 | LEN/DIV 编辑、TIE/SLIDE、per-note lvl/rat、nudge、fill、参数锁、64 步分窗 |
| 引擎特性 | 12 引擎/鼓/DSYN 引擎侧就绪（导出 API 有限） | FM6 patch 编辑面、DSYN 编辑面、SMP 槽管理、预置/工程加载的 golden 覆盖 |
| 存储 | 无（全部 RAM） | FATFS/NVS 后端；A/B 提交语义；11 类备份对象兼容（阶段 5 互通） |
| UI 容量 | LVGL 池 68%（64KB）；四页 eager 创建 | 参数页等重页面必须**懒建/预算**（ADR-005 已列决策，未实现） |
| 设备门禁 | 单例 SELFTEST（DI-11：全量+UI live 楔死） | DI-11 修复后方可依赖全量门禁 |
| soak | 30min 通过 | 数小时 + INA226 电流/温度（BSP 无驱动，需自写最小驱动） |

## 3. 子里程碑拆分与顺序

| 子里程碑 | 内容 | 退出判据 | 依赖 | 状态 |
|---|---|---|---|
| **P4-0 前置清账** | ① DI-11：**不升级 IDF**（工具链迁移会使 golden 基线与全部证据失效、ADR-001 改版——代价不可接受），修复限定在 5.5.5 内、firmware 侧：selftest 运行期 **quiesce**（挂起 input 任务 + 暂停 UI 定时器 + 冻结写），跑完全量 182 后恢复——「UI 共存」口径=UI 存在且运行，诊断期间被暂停，不要求渲染与 selftest 并发；② DI-9：引擎三写路径（`song.sel`/`P_MUTE`/`song.solo`，`trk_silent()` 口径），机器侧断言=写后 `ENG SNAP` 反映值（可听人测列为外部门禁）；③ LVGL 懒建/预算框架落地；④ `{bank,window_start,window_len}` 步窗抽象并被 STEP 页消费；⑤ `nav.sh`：四用例=tab 栏 miss、鼓轨只读、tab+PANIC 同 tick、单 tick 三次 tab（防路由重构回归）；⑥ DI-5 SCR 重试硬化（脚本级重试 + RTS 复位；P4-F 无人值守截屏矩阵的前置）；⑦ 阶段 3 机器侧残留小项（文档/证据口径） | DI-11 删除（全量 182 + quiesce 流程 PASS）；mute/solo 写入可见；步窗抽象被消费；nav.sh 四用例绿；state-test/host-test 绿 | 阶段 3 | ✅ 完成（2026-11-21；全量 182 + quiesce，DI-11 删除） |
| **P4-A 参数页框架** | 引擎 DESC 读取 API（`sloop_core_desc`）+ **批量设置 API**（`sloop_core_set_params`，一次锁内 N 对）；描述符驱动的旋钮页；页面懒建 + 池预算；键盘/触摸值编辑（粗调/细调）；值回读脏同步；`sloop_ui` 自带**策展布局表**（映射上游 page 划分，不入引擎） | **框架证明口径**：3 个引擎页（ANALOG/FM6/SAMPLE）+ GLOBAL 页 + 一个家族页（MASTER/FX 二选一）可编辑且生效；全 12 引擎×全页覆盖列为 P4-C/D/F 的功能对齐清单条目，逐条打勾；截屏+探针；池占用记录 | P4-0 | ✅ 完成（2026-11-21；ANALOG/FM6/SAMPLE + GLOBAL + FX，Task 11 收口） |
| **P4-B 存储与预置** | `storage.c`/`project.c`/`upreset.c`/`fm6_bank.c` 编入；FATFS/NVS 后端（A/B 提交、CRC）；工程 A–D 存读（FUN 格式兼容）；用户预置 32 槽；FM6 银行；工厂预置/kit 浏览 | 存/读往返 hash 一致；掉电中断不破坏旧副本（torn-write 测试沿用上游 storage_test）；预置加载改变声音 | P4-A（UI 入口）/ P4-0 | ⬜ 未开工（入口：ADR-009） |
| **P4-C 音序/编排深化** | 64 步分窗编辑；LEN/DIV；TIE/SLIDE；per-note lvl/rat；nudge ±1/64；fill 条件；参数锁（24/轨）；pattern shift/len/transpose 工具；arranger A–D + chain + SONG REC（`arranger.c` 编入） | 上游 seq2/arranger 测试概念在设备侧 smoke；注入脚本矩阵；编排播放截屏 | P4-A、P4-B | ⬜ 未开工 |
| **P4-D 采样/鼓合成/FM6** | SMP 槽（USR1–4）状态/擦除/上传（串口脚本，镜像 SMP_BEGIN/WRITE/END）；DSYN 22 参数编辑面；FM6 patch 编辑面（算法/算子宏） | 采样上传后 TRIG 出声；DSYN/FM6 编辑生效；许可与体积复核 | P4-B | ⬜ 未开工 |
| **P4-E 设置页** | SCREEN（亮度/主题）、AUDIO（音量/输出）、SYSTEM（版本/自检入口）、LIGHTS（键盘 RGB 可选） | 设置持久化（NVS）；重启保持 | P4-B | ⬜ 未开工 |
| **P4-F 收口** | 功能对齐清单全绿；全页面截屏套件（页面×关键状态矩阵）；数小时 soak（外接电源）+ INA226 电流/温度；证据晋升；**dev→main 强制合并** | roadmap §6 完成判据 | 全部 | ⬜ 未开工（dev→main 强制合并点；先决 DI-17 容量决策） |

**顺序理由**：P4-0 先清阶段 3 欠账（否则后续页面建在失效门禁与容量悬崖上）；A 的 DESC 框架是 B–E 全部页面的地基；B 的存储是 C（编排保存）与 D（采样槽）的前置；E 最小；F 收口。性能优化不在本阶段（roadmap §0.1）。

## 4. 共享框架设计

### 4.1 描述符读取 API（P4-A 核心）

```c
typedef enum { SLOOP_DESC_TRACK, SLOOP_DESC_GLOBAL, SLOOP_DESC_ENGINE } sloop_desc_scope_t;
typedef struct {
    const char *label; const char *unit;
    uint8_t fmt; int16_t min, max, def;          /* 与 params.c param_desc_t 对齐（fmt 见 core.h param_format） */
    const char *const *enums; uint8_t enum_count; /* F_ENUM 时有效 */
} sloop_desc_t;
bool sloop_core_desc(sloop_desc_scope_t scope, uint32_t engine, uint32_t id, sloop_desc_t *out);
```
直接投影上游 `param_desc_t`；UI 按 fmt 做显示格式化（与编辑器协议 DESC 同源，阶段 5 复用）。**不引入新描述符表**（single source）。

**批量设置**（阶段 3 遗留，P4-A 落地）：

```c
void sloop_core_set_params(uint32_t track, const uint8_t *ids, const int16_t *values, uint32_t n);
```

一次 `s_engine_lock` 内应用 N 对（预置加载 71 参数不再 71 次往返）；状态层 `CMD_SET_PARAM`（单值）与 `CMD_SET_PARAM_BATCH{p=数组,n}` 在写者任务内同步执行，数组生命周期由调用方保证（apply 返回前有效）。

**页面布局归属**：页面分组/标题/槽位序是 UI 关注点——由 `sloop_ui` 的策展布局表持有（映射上游 `PAGES[]` 的语义与观感，含鼓轨/ANALOG 特例），**引擎 API 只给描述符**（不暴露 page 表）。目标为「每个参数可达、范围与语义等价」，非逐像素复刻上游页布局。

### 4.2 参数页框架（ADR-008 草案）

- 页 = 参数组（上游 `ui_menu`/`panel` 的页划分映射：ENV/LFO/EDIT/VOICE/INS/FX/GLOBAL/MASTER…）；每页 N 个旋钮槽（触摸拖拽 + 键盘上下/左右）；
- 懒建/预算：页面对象**首次进入时创建、离开时保持但按预算池化**；首次创建失败 → 明确错误（不静默）；每页预算表入 ADR-008；
- 值编辑：拖动/按键改值 → `CMD_SET_PARAM_BATCH`（状态层已有扩展点）→ 引擎；回读快照脏检查。
- FM6/DSYN 等特殊面单独页，不复用旋钮框架的形状约束（可嵌）。

### 4.3 存储后端（ADR-009 草案）

`storage.c` 的 `st_read/st_erase/st_prog` 由 `sloop_hal_tab5` 存储后端实现：FATFS 分区文件内 A/B 双槽 + CRC（保持上游语义与 torn-write 测试）；工程/预置/FM6 银行对象映射同上游 11 类备份对象 id（阶段 5 互通）。NVS 用于小设置（P4-E）。

## 5. 约束

- `CMD_SET_PARAM_BATCH` 为真批量（N 对）——阶段 5 的单在飞 SET 合并直接复用语义；state-test 覆盖批量/边界。
- 引擎回归：每次接入上游 TU（project/storage/upreset/arranger/fm6_bank）后必须重跑 `scripts/host-test.sh` + 设备 SELFTEST；golden 基线若因新 TU 链接顺序变化而变，按容差通道处理并留痕（阶段 2 规则）。
- 存储安全：写 flash 类操作在播放中拒绝（对齐协议 rc=3 语义）；A/B 提交 + CRC；测试沿用上游 storage_test 的 torn-write 用例。
- 内存：LVGL 池按 ADR-008 预算；内部堆门禁维持 ≥60KB；PSRAM 用于大表/采样槽。
- UI 门禁：每页 §5.4 截屏 + 探针；P4-F 全页面矩阵。
- soak：数小时（≥3h，外接电源）；每 10min 记录内部堆/PSRAM/INA226 电流/温度；判据沿用阶段 3 + 温度稳定。
- 人测项：手感/视觉/听感照旧入清单。
- **DI-11 修复前，设备等价门禁以「单例 + 阶段 2 全量 transcript」双轨表述**，AGENTS §5.5 同步。

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| LVGL 池在参数页下溢出 | P4-0 先落懒建/预算；每页进入前后记录池水位；池改大不可行（内部 RAM） |
| 存储代码的 A/B 语义移植错 | 沿用 `storage_test` torn-write 用例作设备侧 smoke；FATFS 文件模拟扇区边界 |
| 参数页数量爆炸（12 引擎 × 参数组） | 描述符驱动生成，不手写每页；枚举格式化复用 fmt |
| 接入上游 TU 改变 golden | 用容差通道 + 逐用例留痕；新 TU 若影响 DSP 路径需先 host 对拍 |
| 数小时 soak 需要外接电源 | 计划内标注；无电源时降级 ≥1h 并记录 |
| DI-11 未修好则全量门禁不可用 | P4-0 列为硬前置；单例门禁不足以支撑本阶段主张 |

## 7. 留给实施者

- 功能对齐清单：从上游 `SLOOP.md`/`GUIDE.md` 整理页面/特性表，逐项编 P4-x 并在各子里程碑开工时细化（roadmap §6 约定）。
- ADR-008（参数页框架/池预算）与 ADR-009（存储后端）在 P4-A/P4-B 入口定稿。
- 每个子里程碑的详细计划在开工时按 `docs/superpowers/plans/` 粒度另立；**本次交付的计划覆盖 P4-0 + P4-A 的启动部分**。
