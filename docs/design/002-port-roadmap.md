# 002 — sloop-fm1 → Tab5 移植路线图（分阶段计划）

> 状态：生效中（阶段 0 已完成）｜ 制定日期：2026-11-19
> 关联：AGENTS.md §1（产品目标）、§5（构建/烧录/验证门禁）、§7.4（分支与里程碑）；`docs/research/`（上游调研）；设计规格 `docs/superpowers/specs/2026-11-19-sloop-tab5-port-design.md` 与阶段 1 实施计划 `docs/superpowers/plans/2026-11-19-phase1-bringup.md`
> 性质：移植主计划。各阶段开工/完成时更新本文状态列；阶段内产生的新决策按 AGENTS §6 沉淀为 ADR 并在本文登记。

## 0. 总原则

1. **正确性优先于性能**（AGENTS §4）：引擎逐样本对齐上游之后才允许性能优化。
2. **host 先行、golden 锚定**：SLOOP 核心本就支持 PC/WASM 构建（上游 `host/preview.c`）。先把引擎在 PC 上跑起来并固化 golden 基线（渲染/音频哈希），再交叉编译上 Tab5；设备侧只需证明「搬运无损」。
3. **截屏驱动 UI 验证**（AGENTS §5.3/§5.4）：阶段 1 落地 `SCR` 后，一切 UI 改动以「截图 + 视觉检查」为完成条件；阶段 3 增加串口输入注入，使 UI 状态可脚本化复现、可前后对比；**门禁证据按 §8 晋升入库**，合并主张以登记证据为准。
4. **每阶段收尾即里程碑候选**：阶段完成 = §5.5 对应门禁全绿，即可作为 dev→main 合并点（§7.4 的示例即「bring-up 完成」「sloop_core 首版可听」）；阶段 4（独立模式完整）与阶段 5（联动可用）为强制合并点。
5. **开工先核对上游**：上游迭代快，每阶段开工先核对 sloop-fm1 / sloop-fm1-go / `EDITOR_PROTOCOL.md` 最新版本，更新 `docs/research/` 版本基线后再写代码。
6. **双模式共享状态抽象**（AGENTS §1）：阶段 3 起建立「状态快照 + 编辑命令」归一模型，阶段 5 的联动模式直接复用，避免两套 UI 逻辑。

## 1. 阶段总览

| # | 主题 | 一句话目标 | 核心验证手段 | 状态 |
|---|---|---|---|---|
| 0 | 工程骨架 | 仓库/文档/脚本/构建烧录链路就绪 | 构建 + 烧录 + 启动日志 | ✅ 完成（基线 7f7543d） |
| 1 | 平台 bring-up | Tab5 成为**可观测、可验证**的目标机：屏/触摸/键盘/音频/截屏/分区 | SCR 截屏视觉检查；按键触摸日志；听音 | 🔶 机器侧完成 + 听音✅；余 3 项人测（见下注） |
| 2 | sloop_core 引擎移植 | 上游引擎在 Tab5 上**正确**运行（golden 对齐） | `host-test.sh`；设备哈希/误差度量比对；可听触发 | 🔶 机器侧完成（设备/host 182/182 同源哈希 + PERF；人耳待办，见下注） |
| 3 | 独立模式 MVP | 可玩闭环：选 kit → 键盘演奏 → 步进编排 → 循环出声 | 截屏矩阵 + 注入式交互 + 端到端试玩 | 🔶 机器侧完成（2026-11-21，`feat/mvp-ui` → dev）；人测 4 项（见下注） |
| 4 | 独立模式功能对齐 | 完整 SLOOP 体验（全页面/全特性/预置存取） | 全页面截屏套件 + 稳定性 soak | 🔶 P4-0/P4-A 完成（2026-11-21，`feat/phase4`）；P4-B 完成（2026-11-22，`feat/phase4b`）；P4-C **机器侧完成**（2026-11-23，`feat/phase4c`；ADR-010）；P4-D **机器侧完成**（2026-11-23，`feat/phase4d`；ADR-011）；P4-E–F 待开工 |
| 5 | 联动模式 | FM-1 双机同步（编辑器协议双向 + UAC1 采集） | 真机协议冒烟 + 前后截屏 diff | ⬜ |

> 排序与门禁优先于日历估时（不设假精度的工期）：阶段 2 与阶段 4 体量最大（引擎对齐 / 功能对齐），阶段 1、3 次之，阶段 5 含 DI-1 驱动自研的不确定面。
>
> **阶段 1 状态**：🔶 机器侧完成 + 听音人测通过（2026-11-19，es8388 无声缺陷已修 4278509）；余 3 项人测：DI-4 键盘 70 键矩阵、触摸五点校准、测试图目检（清单 `docs/dev/verify/1-bringup-2026-11-19/human-checklist.md`）。阶段 2 运行于 `feat/sloop-core`（2026-11-20）。
>
> **阶段 2 状态**：🔶 机器侧完成（2026-11-20，`feat/sloop-core`）：设备与 host 同源 regress **182/182 哈希全等**（同源编译上游 `tests/regress.c`，golden 机械嵌入）；PERF 静默渲染均值 9.44%/最大 11.73%、xrun=0（口径见 `docs/dev/verify/2-engine-2026-11-20/`）；CC0 资产随树 283KB/8 集；人耳待办：合成轨/鼓/SAMPLE 预置听音（收尾清单）。
>
> **阶段 3 状态**：🔶 机器侧完成（2026-11-21，`feat/mvp-ui` → dev）：四屏 PLAY/TRACKS/STEP/TRANSPORT 截屏矩阵 + 像素抽检、`KEY`/`TAP`/`UI` 注入与三条场景脚本（`scripts/inject/`）、EditCmd 层等价性（ADR-005）与切页确定性（ADR-006）全绿；`host-test` / `state-test` / `screenshot.py --selftest` 全绿；30min soak（`scripts/soak.py`）内部堆非单调下降、xrun=0、无 WDT/panic（`docs/dev/verify/3-mvp-2026-11-21/`）。**人测待办**：① 真实键盘弹奏手感与 70 键矩阵（DI-4）；② 物理触摸（四角校准 + DI-10 幽灵按压复核）；③ 四屏目检（§5.4(2) 人眼复看可读性/观感）；④ 听感（合成/鼓/采样，DI-7 收尾）。
>
> **阶段 4 状态**：🔶 P4-0 + P4-A 完成（2026-11-21，`feat/phase4`，Task 11 收口）。P4-0：全量 `ENG SELFTEST` 182/182 在 **UI live** 下通过（诊断期 quiesce，157s，DI-11 删除）、懒建/每页预算框架落地（9 页全建池 free 2,816B）、步窗抽象与 `nav.sh` 四导航用例绿、SCR 重试硬化、DI-9/DI-5 闭环。P4-A：描述符驱动参数页框架证明——**3 个引擎页（ANALOG/FM6/SAMPLE）+ GLOBAL + 家族页 FX** 可编辑生效（TAP ±/键盘/触摸相对拖拽，`PARAM`/`STATE SNAP` 断言），五页截屏 + 像素抽检 + 九页池水位证据入库（`docs/dev/verify/4-p0-2026-11-21/task-10-*`、`task-11-*`）；`knob-test`/`state-test`/`host-test`/`desc-check`/五条注入脚本/soak 120s 全绿。**9 页曾是 LVGL 池的容量天花板**（原延迟条目，已随 P4-B 导航决策关闭）。人测项延续阶段 3 清单（键盘手感/物理触摸/目检/听感）+ 参数页旋钮物理手感（DI-16）。**P4-B 开工**（2026-11-21，`feat/phase4b`）：导航/容量决策落 ADR-005（分组页签 + 组作用域，5 组/11 页；不变量 `base + max(源组 Σest + 目标组首建页 est) + 4KB ≤ 63,120`，按有序对取最坏，见 ADR-005 §5），存储后端落 ADR-009（原始分区 `sloopstore` + mmap/XIP），原 9 页池容量天花板条目关闭。**P4-B Task 0–12 已落地**（2026-11-22）：存储全链路（工程/预置/FM6 银行/自动存档/torn-write/旧格式）+ 状态层存储命令 + 分组导航 + **全部 11 页注册**（FILE 组 PROJECTS/PRESETS 接 TAP/键盘，`TAB` 走最终 11 页顺序）；最坏过渡对变为 FILE 全建 → 首建 STEP（账面 61,928 ≤ 63,120，实测 free 7,308–7,840B）；跨组往返（PLAY↔TRACKS ×10）pool_free 变化 ≤84B（评审 R34 修复了样式重初始化的 268B/往返泄漏）。

> **P4-B 完成（2026-11-22，`feat/phase4b` Task 14 收口）**：存储全链路 + 状态层命令 + 分组导航 + 11 页的机器门禁全绿，证据包 `docs/dev/verify/4-pb-2026-11-21/`（`readme.md` 门禁→命令→结论、`SHA256SUMS`）+ verification-log 登记。关键结论：① **工程 A–D 往返 hash 一致**（RAM 槽 + flash 双层；重启后 `STORE PROJ HASH` 仍等于保存值，即开机 fetch 的证据）；② **torn-write 设备套件 `STORE TEST PASS 6/6`**（擦除失败 / payload 半写 / 头写失败三种掉电窗口下旧副本完整可读、写后破坏 payload 报 CRC 错而非坏数据、空提交、两次提交取 seq 大者；套件可重复跑）；③ **旧格式兼容 3 种**（FUN5/FUN4/FUN1 fixture 经 `STORE RAW` 上传，`PROJ FIELDS` 62 项逐字段与独立 Python 实现 + vendored `project.c` 交叉校验一致；截断对象回 `invalid` 且不动引擎）；④ **自动存档/上电恢复**（idle 2.5s + gap 20s + 停走带 + 无发声四门，硬复位后 hash/bpm/步表恢复且第一拍不重写）；⑤ **换预置/引擎真正换声音**（`ENG PRESET` 期望向量 vs `ENG TRACK` 应用 p[] 71/71，含独立上游表 oracle 防同源假绿；发声时换预置 voices 归 0）；⑥ **FILE 组预算**（PROJECTS delta ≤ 8,192 / PRESETS ≤ 12,800；FILE 全建 → 首建 STEP 的过渡余量实测 ≥4KB）；⑦ **组作用域**收口 DI-19（注入脚本之间无需重启）与 DI-25（跨组往返 pool_free Δ ≤84B）、两行页签 5 组/11 页；⑧ soak 新判据落地：`--save-window`（窗口内 SAVE/LOAD 的 xrun 增量 ≤16、窗口后 streaming=1、窗口外不增长）+ 跨组池往返检查（≤128B）。人测项延续（键盘/触摸/目检/听感/旋钮手感：DI-4/6/7/10/16）。

> **P4-C 完成（2026-11-23，`feat/phase4c` Task 13 收口；机器侧）**：音序与编排全链路落地——64 步/四 bank 分窗编辑、micro/fill、每轨 24 个参数锁、span/note/pattern 工具与有限 undo、实时录音（arm/overdub/count-in/free take）、16 lane 鼓输入、live sections A–D + dirty 延迟写、quick chain、SONG 播放/loop/REC 与工作 loop 恢复、PER3 歌曲顺序持久化（硬复位恢复、旧 PER1/2 迁移）。所有入口经公共 API + 状态层（`CMD_STEP_EDIT`/`CMD_PATTERN`/`CMD_LOCK_*`/`CMD_REC_*`/`CMD_FILL_CONTROL`/`CMD_DRUM_*`/`CMD_SECTION_*`/`CMD_CHAIN_*`/`CMD_SONG_*`），UI 为 STEP 深度步/鼓 lane、TRANSPORT 工具区、SONG 页与六组导航（12 页）。证据包 `docs/dev/verify/4-pc-2026-11-22/`（`readme.md` 门禁→命令→结论、`SHA256SUMS` 127 项）：构建 clean+增量 **0 警告**、烧录三段 `Hash of data verified`、设备 `ENG SELFTEST` **182/182**、八条注入场景同会话 **2,580 PASS / 0 FAIL**（288/196/11/140/149/903/331/562）、复位后 `soak.py --duration 1800 --interval 60 --save-window 900` **PASS**（内部堆 −20B、xrun 0、占空比 ≤12.54%、池往返 −52B）。人测项延续（键盘手感 DI-4、物理触摸 DI-10/16、目检、听感 DI-7/39）；DI-26（自检堆泄漏，~40KB/次）仍须在 P4-F 前处理；DI-32/33 为本阶段新增的真实遗留。**P4-D 入口**：USR 采样槽上传/编辑、DSYN 22 参数面、FM6 patch 深度编辑（ADR-010 §9 的诊断面已就位）。

> **P4-D 完成（2026-11-23，`feat/phase4d` Task 12 收口；机器侧）**：声音设计面落地——USR1–4 采样槽（flash 原始槽 + PSRAM 镜像、串口 `SMP` 上传、CRC/zone 校验、开机/上传收尾/自检收尾同步）、DSYN 4 kit × 16 lane × 22B 编辑面（24 虚拟字段、工厂 kit、试听、DSU 随 PER3 持久化）、FM6 155B 深度 patch（6×21+29 字段、ALG 同锁清宏、bank 27 槽持久化），新增 **SYNTH 组**（SAMPLE/DSYN/FM6P，14 页/7 组）。证据包 `docs/dev/verify/4-pd-2026-11-23/`（`readme.md` 门禁→命令→结论、`SHA256SUMS`）：构建 clean+增量 **0 警告**、烧录三段 `Hash of data verified`、设备 `ENG SELFTEST` **182/182**、九条注入场景同会话 **3,455 PASS / 0 FAIL**（transport 286 / step_edit 196 / tracks 11 / nav 156 / params 197 / store 903 / song 354 / sequence 562 / synth 790）、复位后 `soak.py --duration 1800 --interval 60 --save-window 900` **PASS**（详细数值见证据包 readme）。SYNTH 组 Σ est 20,480B（实测 `sum_delta` 17,696–17,936B），SYNTH→STEP 峰值 `pool_free 11,244B`，最终矩阵最低跨组余量 `song` ⑩ FILE→STEP **8,924B**（≥4,096B）。人测项延续（键盘 DI-4、触摸 DI-10/16、目检、听感 DI-7/39）；新增遗留 DI-34（WAV/SD 导入）、DI-38（DSYN-only 延迟写）、DI-36（FM6 live patch 掉电语义）、DI-37（名称文本输入）；DI-26（自检堆泄漏）仍须 P4-F 前处理。**P4-E 入口**：设置页（SCREEN/AUDIO/SYSTEM/LIGHTS，`OBJ_SETTINGS` 上游格式）。

## 2. 阶段 0 — 工程骨架（✅ 已完成，2026-11-19）

仓库与协作文档（AGENTS.md、docs/ 全套）、`scripts/` 构建烧录截屏链路、`firmware/` ESP-IDF 骨架（BSP 1.3.3；P4 v1.3 样片与 ST7121 批次适配）、LICENSE、分支策略。真机烧录验证通过。

## 3. 阶段 1 — 平台 bring-up

**目标**：把 Tab5 变成一台「处处可观测」的目标机——屏幕点亮并确认 ST7121 批次探测、SCR 截屏闭环打通（§5.4 验证体系上线）、Tab5 Keyboard 真机验证、ES8388 出声、分区表为引擎与资产腾好空间。**本阶段不含任何 SLOOP 代码。**

| 任务 | 内容 | 验证 |
|---|---|---|
| P1-1 分区表 | `partitions.csv`：**OTA 姿态定为单 factory app（无 A/B 槽）**——开发期 USB 常在，回滚路径 = esptool 重烧；未来确需 OTA 时再重划分区（资产重烧成本已知，可接受）。示例布局：factory app 8MB + FATFS 资产 ~7.9MB + NVS（实现时定稿并回写本文）；FATFS 取大值，使 P2-5 无论资产主力在 flash 还是 SD 均容纳（SD 为独立硬件，后加无损现有分区）。**P4-B 按 ADR-009 落定**：`sloopstore`（data/0x99）`0x810000/0x68000` 承载上游 flash map，FAT 让位到 `0x878000/0x788000`（不再承载 SLOOP 数据） | 烧录后分区日志正常；app 摆脱 1MB 默认上限 |
| P1-2 显示点亮 | `bsp_display_start()`（MIPI-DSI + esp_lvgl_port）；确认日志 `Discovered board version 3 (LCD ST7121…)`（批次探测，见 device-notes §1）；画测试图（彩条 + 几何图形 + 文本标签） | SCR 截屏视觉检查：彩条颜色顺序正确、无花屏撕裂、文本可读 |
| P1-3 SCR 命令 | console 注册 `SCR`（§5.3 协议）：从显示控制器取完整稳定帧，rgb565 小端输出；LVGL 双缓冲下取非活动帧避免撕裂；扩展 `scripts/screenshot.py` 支持像素抽检（如 `--check <x>,<y>,<r>,<g>,<b>`），已知色块可机检 | `screenshot.py` 出图且像素抽检通过（程序化 + 视觉检查；scripts 门禁 selftest 仍须过） |
| P1-4 触摸 | BSP 触摸 indev；触摸处画十字准星 + 坐标日志 | 点四角/中心 → 截屏中准星位置与手指一致（坐标映射无旋转/镜像错误） |
| P1-5 Tab5 Keyboard | 独立 I2C 总线 G0/G1 探测 0x6D；INT=G50；按键解码（参考 M5Tab5-UserDemo 与协议 PDF）；启动自检；**全 70 键逐键 + 修饰键/多键同按典型组合**解码核对 | 全键矩阵逐键日志核对；探测成功后 device-notes §2 更新为 ✅ |
| P1-6 音频 | BSP 音频（esp_codec_dev，ES8388）：console `AUD TONE` 播 440Hz 正弦；I2S/codec 实际采样率确认为 **44.1kHz**（上游假设，见 research；48k 时钟配 44.1k 数据会整体音高偏高约 8%，440Hz 试音兼作人工校验） | 扬声器/耳机可听、无爆音；codec 配置回读 44.1kHz；听感音高正确 |
| P1-7 验证链路收口 | 本阶段所有验证走 `scripts/verify.sh`；截图先落暂存区 `firmware/build/verify/`，**门禁证据按 §8 晋升入库并登记** | verify.sh 一键全绿 + verification-log 登记 |

**完成判据（阶段门禁）**：
1. §5.5「固件代码」全部行绿（第二次构建零新增警告、烧录 Hash verified、启动无 fatal/abort/backtrace）；
2. 测试图截屏通过 §5.4 视觉检查——**项目第一次真实 UI 截图验证**；
3. 键盘自检通过（0x6D 应答 + 70 键全矩阵逐键 + 典型组合解码正确）；
4. 触摸四角校准截图通过；
5. `AUD TONE` 听音通过（人工确认，音高正确）；
6. 验证证据按 §8 晋升入库并在 verification-log 登记（第一次完整闭环）；
7. 里程碑候选：bring-up 完成（可合 main，§7.4）。

**风险与对策**：
- ST7121 批次差异 → BSP 1.3.3 运行时自动分流已核验（device-notes §1），日志确认即闭环；
- 键盘 I2C 协议细节（寄存器/中断语义）未知面 → 以 M5Tab5-UserDemo 出厂固件源码为权威参考；
- 屏幕复位时序坑（tab5-hardware §6.1）→ 全程走 BSP 初始化，不自写面板序列。

## 4. 阶段 2 — sloop_core 引擎移植（正确性优先）

**目标**：上游 SLOOP 引擎（DSP + 音序器 + 状态）作为 `sloop_core` 组件在 Tab5 上运行，且与 PC host 构建逐样本/逐帧一致。**本阶段不做 UI**——设备侧用 console 命令驱动与验证。

| 任务 | 内容 | 验证 |
|---|---|---|
| P2-1 上游引入 | 开工先核对上游最新版本（§0.5）并更新 research 基线；以固定 commit 引入上游代码（机制小 ADR：submodule vs vendoring+补丁，遵 §8「补丁/适配层隔离」）；GPL 声明与子模块许可（Dexed msfa / DaisySP / Mutable 等）完整保留 | 引入物清单 + 许可核查记录入 docs |
| P2-2 host 基线 | PC 构建 host/preview；复用/补齐 golden 回归（渲染哈希、音频块哈希）；落 `scripts/host-test.sh`（§5.5 该门禁自此生效）；golden 基线值入库。**数值编译约定（ADR）**：host/device 双侧 `-ffp-contract=off`、禁 `-ffast-math`，工具链版本与编译 flags 随 golden 基线一并入库（保证基线换机可复现） | host-test.sh 退出码 0 |
| P2-3 组件拆分 | `sloop_core`（引擎，尽量不改上游文件）+ `sloop_hal_tab5`（hal 落地：I2S/ES8388 音频、FATFS 存储、定时器、日志）。决策点（ADR）：本地是否复用上游 editor 命令语义作为统一编辑入口（为双模式归一铺路） | 组件交叉编译零新增警告；I2S 实测输出采样率 44.1kHz、音频块大小与上游一致；依赖边界入 ADR |
| P2-4 设备侧自检命令 | console：`ENG SELFTEST`（跑 golden 子集，回传**每用例哈希 + 对内嵌参考向量的 max\|err\|/SNR**，使「容差内通过」可计算可记录）、`ENG TRIG <kit> <note>`（出声）、`ENG PERF`（每音频块 CPU 周期） | 设备哈希 == host 哈希；不等时按误差度量走容差通道（见下「浮点等价」） |
| P2-5 样本资产 | 工厂 kit 样本来源与格式核实（⚠️ 上游 release 资产 / loader 打包 / FM-1 flash 镜像，开工时定）；**资产再分发许可核查**——结论入 docs，不可再分发则资产不入库，改为 fetch-from-upstream 脚本 + 本地烧录流程；烧入 FATFS 分区或 SD 卡（flash-vs-SD 微型 ADR）；loader 移植、kit 枚举 | `ENG TRIG` 采样 kit 出声正确；许可结论与资产入库方式入档 |
| P2-6 性能基线 | 记录每块 CPU 周期/内存水位，对照 FM-1（pi32v2 ~120MHz）→ P4（360MHz 双核）预算 | 数据记 docs/dev 并登记 verification-log；确认余量充足 |

**完成判据**：
1. `scripts/host-test.sh` 全绿（PC 基线固化，工具链/flags 随基线入库）；
2. `ENG SELFTEST` 设备哈希与 host 一致；不一致时在文档化容差内（max\|err\|/SNR 度量）通过并记录成因与差异源；
3. `ENG TRIG` 至少一个合成 kit + 一个采样 kit 听音正确；
4. 性能基线数据入档；构建零新增警告；
5. 里程碑候选：「sloop_core 首版可听」（可合 main，§7.4）。

**浮点等价风险**：除 libm 超越函数的 ULP 差异外，**FP contraction 是更早的分叉源**——GCC 默认 `-ffp-contract=fast`，RV32F 会把 `a*b+c` 收缩为 `fmadd` 指令，而 x86-64 基线（SSE2，未开 `-mfma`）不会，普通 DSP 乘加行即分叉。对策（P2-2 ADR）：双侧 `-ffp-contract=off` + 禁 `-ffast-math`，工具链与 flags 随基线入库；残余差异（libm 等）用 `ENG SELFTEST` 的误差度量判定——**哈希不等 ≠ 失败，走可计算的容差通道并留痕**。

## 5. 阶段 3 — 独立模式 MVP（可玩闭环）

**目标**：最小但完整的一圈：触摸/键盘输入 → 状态模型 → 引擎 → 音频出声，UI 呈现 MVP 四屏。

| 任务 | 内容 | 验证 |
|---|---|---|
| P3-1 输入抽象与映射 | InputManager：触摸控件 / Tab5 Keyboard /（未来）遥控事件 → SLOOP 层式操作模型动作；70 键 → 演奏区 + 功能层映射表（等价 FM-1 的 27 键 + 6 编码器操作），映射 ADR + 映射表文档 | 映射表评审 + 按键注入测试 |
| P3-2 UI 架构 ADR | UI 用 LVGL（BSP esp_lvgl_port）；决策：1280×720 原生重排版（推荐，参考 Sloop Go 屏设）vs 240×240 放大兼容；页面路由对应 SLOOP 层级；视觉规范（四轨主题色 蓝/绿/黄/橙 等） | ADR 落档 |
| P3-3 MVP 四屏 | PLAY（kit 信息 + 演奏触发指示）、TRACKS（四轨选择着色）、STEP（步进网格编辑）、TRANSPORT（播放/停/BPM） | 每屏截屏过 §5.4 |
| P3-4 状态模型 | AppState 快照 + 编辑命令归一抽象（本地引擎执行；阶段 5 同一抽象直发 FM-1） | 状态层单测（host 可跑）+ 设备行为 |
| P3-5 串口输入注入 | console `KEY <code>` / `TAP <x> <y>` 与 `SCR` 组合，UI 状态可脚本复现；同步更新 AGENTS §5.3/§5.4 与 screenshot.py（含 §5.4 预留的 `SCR <page>` 参数化一并评估） | 注入脚本驱动的截屏序列。`SCR <page>` 已评估**未采纳**：`UI <page>` 同步切页（回显即落地页）+ 无参 `SCR` 已覆盖全部验证场景，新增参数只会让截屏协议多一条路径（Task 7/8 实证） |

**完成判据**：
1. 端到端人工试玩：选 kit → Tab5 Keyboard 演奏出声 → 步进写入 → 循环播放（听感正确、无爆音卡顿）；
2. MVP 四屏截屏全部过 §5.4 视觉检查（布局/着色/可读性）；
3. 注入式测试：至少 3 条脚本化交互流（如「选轨→改步→截屏」）稳定复现；
4. host-test.sh 仍全绿；短时 soak（≥30min）：最小剩余堆水位稳定（不随时间单调下降）、日志无 task watchdog/backtrace/重启。

## 6. 阶段 4 — 独立模式功能对齐

**目标**：独立模式达到与 FM-1 上 SLOOP 等价的完整体验。

任务（先从上游手册/界面整理**功能对齐清单**，逐项编 **P4-x** 任务号，再逐项打勾）：
- 全部页面：kit/参数编辑器（旋钮页）、pattern/编曲、采样槽管理（USR/SYN）、设置页等；
- **入口条件（阶段 3 终审登记）**：动第一个参数页之前先完成 ADR-005「阶段 4 容量决策」
  （LVGL 池 68% 已用：懒建拆页/扩容/共享池化三选一 + 每页预算表）；扩 STEP 网格之前先在
  状态层/快照引入 `{bank, window_start, window_len}` 视窗抽象（页面、hit-test、注入脚本同源，
  列 ≥ LEN 的格沿用阶段 3 的 dim 判定）；
- 引擎特性补齐：TIE/SLIDE/SLICER、FM6/DSYN 参数面、用户预置存取（原始分区 + mmap，ADR-009）；
- 视觉：全页面截屏套件（页面 × 关键状态矩阵），证据按 §8 晋升入库；
- 稳定性：长时 soak（数小时，量化口径同阶段 3）+ 堆/PSRAM 水位记录；顺带记录 INA226 电流与芯片温度（板载即有，tab5-hardware §3.6）；供电姿态：标准版无 NP-F550 电池，长 soak 须外接电源；
- 仅在此之后做性能优化（§0.1）。

**完成判据**：功能对齐清单全绿；全页面截屏套件通过且证据入库；soak 量化判据通过；**强制里程碑：dev → main 合并（§5.5 全门禁重跑）**。

## 7. 阶段 5 — 联动模式（Companion）

**目标**：刷 SLOOP 的 FM-1 经 USB 接入后，达到 Sloop Go 级别的双机同步体验（AGENTS §1.2）。

| 任务 | 内容 | 验证 |
|---|---|---|
| P5-0 前置 | FM-1 真机接入（device-notes 登记，SLOOP 版本经 INFO 回读）；使能 USB-A VBUS（PI4IO 0x44 E2.P3 `USB5V_EN`，不开则总线无电、表现为静默无设备，tab5-hardware §3.11）；**双模式调度与共存策略 ADR**（FM-1 接入时本地会话处置：切换/并行/双音频源；对应 AGENTS §3 `main/`「待：双模式调度」预留课题）；**解决 DI-1**：ADR-002 选型并实现 USB-MIDI host 类驱动 | 设备枚举 1209:0001，MIDI 端口 "Felucca"；调度 ADR 落档 |
| P5-1 usb_fm1 组件 | MIDI bulk IN/OUT + 4 字节事件包 + SysEx 重组（参考 fm1-cartridge 先例） | 环回 + 真机收发 |
| P5-2 editor_proto 组件 | 帧编解码（以上游 `EDITOR_PROTOCOL.md` 为权威，⚠️ 版本开工复核）、INFO 协商、GET/SET、WATCH/PING、push 解析、one-in-flight 队列、LIVE 遥控 | codec round-trip 单测 + mock 对端冒烟 |
| P5-3 同步层 | DeviceController（FIFO + 瞬时 SET 合并）/ DeviceState（不可变快照）语义移植自 Sloop Go；断连丢弃队列/草稿 | 同步场景测试 |
| P5-4 Companion UI | 连接状态页、编辑页（直驱 FM-1）、LIVE 遥控面板；复用 P3-4 状态抽象 | 截屏 + **前后截图 diff 证明双向同步**（FM-1 操作 → Tab5 SCR 变化；Tab5 操作 → FM-1 屏幕人工确认） |
| P5-5 音频采集 | usb_host_uac 采集 FM-1 UAC1 → 混音/监听出 ES8388。⚠️ FM-1 的 UAC1 速率由其固件设置决定（44.1/48k），与本地 44.1k 混音存在双时钟/SRC 问题（重采样或时钟主从方案届时定） | 听音（无可闻漂移/爆音）+ 采集统计日志 |

**完成判据**：
1. §5.5「联动协议」门禁生效并全绿（连接 → INFO 版本协商 → WATCH push 可达，冒烟脚本化）；
2. 双向同步场景清单全过（编辑下发 / 推送上行 / LIVE 遥控 / 断连恢复）；
3. 音频采集可听；
4. DI-1 条目删除（ADR-002 已沉淀）；**强制里程碑：dev → main 合并**。

## 8. 横切事项（全阶段）

- **验证证据入库**：`firmware/build/verify/` 仅为**抓取暂存**（被 .gitignore 覆盖，`build.sh --clean` 会整目录删除）；门禁判定所用证据（通过代表图、全部失败图、soak 摘要、`ENG PERF` 数据、协议冒烟 transcript）晋升至 `docs/dev/verify/<阶段>-<日期>/`，并在 `docs/dev/verification-log.md` 登记（阶段/门禁/日期/提交号/证据 SHA256/结论）。阶段 4/5 强制合并点的完成主张**以登记证据为准**。
- **分支**（§7.4）：阶段内工作在 `feat/<主题>` 分支推进、合回 dev；每阶段收尾重跑 §5.5 对应门禁，均可作为合并 main 的候选点。
- **门禁激活时点**：UI 截屏门禁自阶段 1 第一次截图起生效；`host-test.sh` 门禁自阶段 2 生效；「联动协议」门禁自阶段 5 生效。
- **文档同步**（§6）：新组件/新 ADR/环境坑随任务同提交；上游版本跟进先改 research 基线。
- **deferred issues**：阶段推进中发现的可延迟缺口随时登记 `docs/deferred-issues.md`，解决即删。
- **永久排除**（AGENTS §1 非目标）：FM-1 刷机/更新帧、BLE MIDI。

## 9. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-11-19 | 首版：阶段 0–5 划分、任务编号（P1-x…P5-x）与各阶段完成判据 |
| 2026-11-19 | 评审修订（1 Critical + 3 Important + 7 Minor）：证据入库策略（§8/P1-7/阶段 4，配套 AGENTS §5.4 与 verification-log）；浮点等价可执行化（FP contraction 对策 + 误差度量容差通道）；分区 OTA 姿态定稿（单 factory app，算术自洽）；资产再分发许可核查；44.1kHz 对齐（P1-6/P2-3）；USB-A VBUS 使能（P5-0）；双模式调度 ADR（P5-0）；soak 判据量化；70 键全矩阵核对；阶段 4 补 P4-x 编号约定 |
| 2026-11-21 | 阶段 3 收口：§1 阶段 3 → 🔶 机器侧完成（四屏截屏矩阵 + 三条注入脚本 + 30min soak），列人测 4 项；P3-5 记录 `SCR <page>` 已评估未采纳；soak 工具落地为 `scripts/soak.py`（AGENTS §5.1/§5.5） |
