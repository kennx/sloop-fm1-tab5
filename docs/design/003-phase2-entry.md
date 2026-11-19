# 003 — 阶段 2 入口：sloop_core 引擎移植的目标与开工清单

> 状态：生效中 ｜ 制定日期：2026-11-19
> 关联：`docs/design/002-port-roadmap.md` §4（阶段 2 任务表与完成判据，权威）；`docs/superpowers/specs/2026-11-19-sloop-tab5-port-design.md`（设计规格：§5 架构、§6 决策表 D1/D2/D8、§7 阶段 2 要点）
> 性质：阶段 2 的**入口目标文档**——把 roadmap §4 的任务展开为开工前必须完成的目标/决策/核查清单。bite-sized 实施计划在开工时按 `docs/superpowers/plans/` 粒度另行制定。

## 1. 阶段目标（一句话）

上游 SLOOP 引擎（DSP + 音序器 + 状态）作为 `sloop_core` 组件在 Tab5 上**正确**运行，且与 PC host 构建逐样本/逐帧一致（golden 锚定）——**本阶段不做 UI**，设备侧用 console 命令驱动与验证。

## 2. 交付物目标（对应 roadmap P2-1…P2-6）

| # | 交付物 | 完成判据（摘） |
|---|---|---|
| P2-1 | 上游代码引入（固定 commit）+ 引入机制 ADR（D1）+ 许可核查记录 | 引入物清单与许可结论入 docs；机制落 ADR |
| P2-2 | PC host 基线：`scripts/host-test.sh` + golden 基线值入库 | host-test.sh 退出码 0；工具链/编译 flags 随基线入库 |
| P2-3 | `sloop_core` + `sloop_hal_tab5` 组件；本地编辑入口决策（D2）落 ADR | 交叉编译零新增警告；I2S 44.1kHz/块大小与上游一致；依赖边界入 ADR |
| P2-4 | 设备自检命令：`ENG SELFTEST`（golden 子集 + max\|err\|/SNR 容差通道）、`ENG TRIG <kit> <note>`、`ENG PERF` | 设备哈希 == host 哈希；不等时容差内通过并留痕；至少一合成 kit + 一采样 kit 出声 |
| P2-5 | 样本资产：来源核实 + **再分发许可核查** + loader 移植 + kit 枚举；flash-vs-SD 微 ADR（D4） | `ENG TRIG` 采样 kit 出声正确；许可结论入档（不可再分发则 fetch 脚本，不入库） |
| P2-6 | 性能基线：每音频块 CPU 周期/内存水位 vs FM-1（pi32v2 120MHz）→ P4 预算 | 数据入 docs/dev + verification-log 登记 |

**里程碑候选**：「sloop_core 首版可听」（roadmap §7.4，dev→main 候选点）。

## 3. 开工前核查清单（进入阶段 2 的第一周内完成）

1. **上游版本复核**（roadmap §0.5）：核对 isod89/sloop-fm1 / sloop-fm1-go / `EDITOR_PROTOCOL.md` 最新版本，更新 `docs/research/` 基线（当前基线 2026-11-19：SLOOP 2.8.5 bac9c05 / Go 1.8.0 ed941b5 / 协议 v12）。
2. **精读上游源码**（research §5 ⚠️ 项）：`core.h`、`main.c`、`audio.c`、`editor.c`、目标引擎源码；确认音频块大小与 `fm1_audio.h` 节拍语义。
3. **D1 ADR（引入机制）**：vendoring（脚本化固定 commit 抓取，spec §6 建议）vs submodule——论证后落 `docs/design/`。
4. **D2 ADR（本地编辑入口）**：自定精简 EditCmd 词表（spec §6 建议）vs 内嵌上游 editor 命令全集。
5. **数值约定固化**：host/device 双侧 `-ffp-contract=off`、禁 `-ffast-math`；工具链版本与 flags 随 golden 基线入库（roadmap §4 浮点等价）。
6. **host 环境就绪**：PC 上跑通上游 `host/preview.c` 构建与 `tests/run_tests.sh`（golden 的搬运基础）。

## 4. 阶段 1 遗留输入（本阶段须消化）

| 项 | 内容 | 处理时机 |
|---|---|---|
| DI-2 | `sdkconfig.defaults` 被 `firmware/sdkconfig` 掩盖的构建陷阱 | 阶段 2 首次改 defaults 前顺手修进 `build.sh`（或继续手动 rm） |
| DI-6 | ES8388 决定性输出寄存器未二分；音量全开为保守实证态 | P2-3 sloop_hal_tab5 音频落地时二分并定音量策略；复核 44.1kHz 音高（人耳） |
| DI-4 | 键盘 70 键矩阵人测 | 随时可在硬件旁执行（清单在 `docs/dev/verify/1-bringup-2026-11-19/`）；不阻塞阶段 2 |
| 触摸校准/测试图目检 | 阶段 1 剩余人测 | 同上 |
| console 排障命令 | `SCR/KB/AUD(REG/PA/L2/MAX)` 已就位 | `ENG SELFTEST/TRIG/PERF` 按同一模式扩展（console.c） |

## 5. 边界（本阶段不做）

- 不做 UI（阶段 3）；不做 USB host / 编辑器协议（阶段 5，但 D2 词表为其预留兼容性）；
- 不做双核分派优化（D8：单核跑通 + P2-6 数据之后再说）；
- 不改上游 `src/` 内部逻辑（补丁/适配层隔离，AGENTS §8）。

## 6. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-11-19 | 首版：阶段 1 收尾时制定，展开 roadmap §4 为开工目标与核查清单 |
