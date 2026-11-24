# P4-F 阶段 4 收口 spec（DI-26 前置 / 功能对齐清单 / 参数族覆盖 / 截屏矩阵 / 长 soak / dev→main）

> 状态：builder check 完成（5 条发现已并入：DI-26 兜底规则、表外缺口处置规则、状态相关标签的钩子/镜像、
> soak 无人值守、SOUND 底行条件化；页数更正为 18）→ 待用户确认。
> 上位：`docs/design/002-port-roadmap.md` §6（阶段 4 完成判据）、
> `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md` §3 P4-F 行。
> 取舍按用户长期授权（auto 权限模式、「最大程度移植 sloop-fm1 到 Tab5」）由本会话代为决定；
> **dev→main 合并与全部人测项不由本 spec 自动执行**（§7）。

## 1. 意图与背景

阶段 4 的完成判据：功能对齐清单全绿；全页面截屏套件通过且证据入库；soak 量化判据通过；
强制 dev→main 合并。侦察结论（2026-11-24）：

1. **功能对齐清单从未落成文档**（parity spec §100 要求从上游 `SLOOP.md`/`GUIDE.md` 整理）。
2. **参数覆盖是最大缺口**：上游 `params.c` 的 `PAGES[]` 约 40 张 4 旋钮页（含每轨族与全局族）；
   Tab5 现有参数页只有 GLOBAL（8 个 G 参数：BPM/SWING/CLOCK/TUNE + DLY×4）、FX（8 个 P 参数）、
   ANALOG（仅引擎 0 的 edit[0..7]）、FM6、SAMPLE——**12 个引擎里 9 个没有任何引擎编辑页**；
   ENV/ENV DEST/LFO×2/LFO DEST、SEL×2、ARP×2、VOICE×2、SLICER、INSERT×2、REV/CHO、MASTER、
   DRUMS、TEXTURE×2、TOOLS 在 UI 上完全缺失（数据路径已通：`sloop_core_desc` 覆盖全部 `TP[]`/`GP[]`/
   `ENGINES[]->edit[]`，console `PARAM GLOBAL` 可写）。P4-A 的「框架证明口径」明确把全页覆盖推迟到本阶段。
3. **DI-26**（全量自检每次泄漏 ~40KB 内部堆，≈230B/执行 job）是收口前置：定位方向见 DI 条目
   （job 路径的库调用 vs `selftest_restore()` 回滚了堆相关全局）。泄漏点未定位；本 spec 不预设结论。
4. **DI-43**：内部堆余量仅 4KB，SELFTEST worker 16KB 栈常驻。
5. INA226（0x41，G31/G32）与芯片温度无驱动；`soak.py` 无功耗/温度采样。

## 2. 目标 / 非目标 / 反目标

**目标（按序）**：
1. DI-26 定位并修复；门禁：同一 boot 连续两次全量自检后 `internal_free` 下降 ≤1KB；DI-26 删除。
   **兜底规则（builder check 裁决）**：调查限时两个任务轮；若泄漏点在 IDF/newlib 内部且无干净修法，
   **不得掩盖**——`selftest-heap.py` 降为记录模式（只断言两次全量自检能完成且不 OOM，记录差值），
   DI-26 保持开放并附定位结论，目标 2–6 照常推进，是否带此项合并由用户在 §7 决定。
   「隔离」的合法含义 = 改变诊断结论的非掩盖性缓解（例如把一处分配挪出快照窗口），不含自检后重建堆。
2. 功能对齐清单落成 `docs/design/012-feature-parity-checklist.md`，每行有证据或有处置（P4-F 任务/DI/非目标）；
   「全绿」定义 = **无未处置行**，已知缺口以 DI 形式在收口报告里明示。
3. 参数族覆盖：全部 12 引擎的 edit[0..7] 可编辑（标签经引擎 `desc` 钩子取模式正确的文本）；每轨族（ENV/LFO/SEL/ARP/VOICE）与全局族
   （REV/CHO、MASTER、DRUMS、TEXTURE）与 FX 族（SLICER/INSERT）在 UI 可编辑；TOOLS 三动作
   （CLEAR SEQ / INIT SOUND / NEW PROJECT）可达。
4. 全页面截屏矩阵（18 页 × 关键状态）入库 + 联系表（contact sheet）供人眼复核。
5. 数小时 soak（默认 3h）+ INA226 电压/电流 + 芯片温度采样；量化判据沿用 §5.5。
6. DI-43(1)：worker 栈按实测高水位收缩。
7. 证据晋升、文档/ADR 对齐、全矩阵、收口报告；**不合并 main**（用户 2026-11-24 明示：dev→main 不在本阶段执行；roadmap 的「强制里程碑」顺延，收口报告里标为待办）。

**非目标**：MIDI LEARN / MIDI 通道 / SYNC / ROUTE 设置页（阶段 5 联动，`G_MIDI*`）；可视化 VIS 页、
punch-in 现场 FX 层、LIVE 页（清单里登记为 DI，写明触发条件）；采样 WAV/SD 导入（DI-34）、
屏上命名（DI-37）、耳机路由（DI-39）；性能优化（roadmap §0.1：本阶段之后）；上游版本跟进。

**反目标**：不为凑页数复制上游 40 页的逐页布局（ADR-008 策展布局是既定决策）；不扩 LVGL 池、
不突破 ADR-005 §5 有序对不变量（超预算先改对象设计）；不碰 `third_party/`；不为了「全绿」
把缺口标成已对齐——缺口的唯一合法出口是 DI + 触发条件；dev→main 不得在机器门禁红或未获用户确认时执行。

## 3. 容量与导航（ADR-005 §5 口径）

现状：15 页 / 8 组，组签 122px；最坏有序对 SEQ→PRESETS 60,776（余 2,344）。

方案（见 §5 A1）：**9 组 / 18 页**，组签 `min(176,(1280−32−216−8×8)/9)=107.5 → 104px`
（9×104+8×8=1,000，右端 1,016 ≤ 1,064；最长标签 TRACKS ≈ 66px 可容纳）。

| 组 | 子页（≤5） | est（B） | Σ est |
|---|---|---|---|
| PLAY | PLAY | 3,072 | 3,072 |
| TRACKS | TRACKS | 7,168 | 7,168 |
| EDIT | SOUND / FM6 / MOD / SCALE / VOICE | 3,584 + 6,144 + 3×7,168 | 31,232 |
| MIX | GLOBAL / FX / TOOLS | 3×7,168 | 21,504 |
| SYNTH | SAMPLE / DSYN / FM6P | 6,144 + 7,168 + 7,168 | 20,480 |
| SEQ | STEP / TRANSPORT | 23,040 + 9,216 | 32,256 |
| SONG | SONG | 12,288 | 12,288 |
| FILE | PROJECTS / PRESETS | 8,192 + 12,800 | 20,992 |
| SYS | SYSTEM | 10,240 | 10,240 |

**EDIT Σ 31,232 超限**（EDIT→STEP = 31,232 + 23,040 = 54,272 ⇒ 账面 69,996 > 63,120）。
裁决：MOD/SCALE/VOICE 三页用**单页多 chip 复用 8 旋钮**（DSYN/FM6P 模式），est 压到 **5,120**
（比 DSYN 少一半 chip 行/无状态行；实测后按 ADR-008 定稿）⇒ EDIT Σ = 3,584 + 6,144 + 15,360 =
**25,088**，EDIT→STEP = 48,128 ⇒ 账面 **63,908 > 63,120 仍超 788B**。
再裁决：FM6 页（edit[0..7] + bank 行）的编辑功能被 SOUND（通用引擎页）完全覆盖，**FM6 页只保留
bank 存取行**（SAVE/ERASE/槽号，旋钮区显示 `SOUND` 页同款 8 槽只读？否——）：
**FM6 并入 SOUND 页的底部 bank 行**（选中轨为 FM6 引擎时出现，同 SAMPLE 槽行几何），FM6 页删除
⇒ EDIT = SOUND / MOD / SCALE / VOICE，est 6,144 + 3×5,120 = **21,504**；EDIT→STEP = 44,544
⇒ 账面 60,264（余 2,856，与 SONG→? 同量级）。最坏有序对仍 SEQ→PRESETS 60,776（余 2,344，不变）。
新增对：MIX→STEP 21,504+23,040 = 44,544（同上 60,264）；SEQ→MIX 32,256+7,168 = 39,424 ⇒ 55,184。
FM6P 44B tripwire、ZOOM +768B（zoom ON 时 61,544）口径不变。**est 是上限**：实测超限先改对象设计。
（FM6 页删除的连带：`UI fm6` 页名→重定向到 SOUND 并显示 bank 行；`params`/`store`/`synth` 场景的
FM6 用例改指向 SOUND；TAB 轮转 19 页。）

## 4. 字段/页面映射（上游 → Tab5）

| Tab5 页 | 内容（数据全部来自 `sloop_core_desc`，不引入新描述符表） | 交互 |
|---|---|---|
| **SOUND**（取代 ANALOG+FM6） | 选中轨引擎的 `edit[0..7]`（标题显示引擎名，标签经引擎 `desc` 钩子；鼓轨 ⇒ 锁定 `NO SOUND`，同现 ANALOG 锁定语义）；底部：引擎为 FM6 ⇒ bank 行（B1..B27/SAVE/ERASE，A10）；SAMPLE 引擎的槽行只在 SAMPLE 页 | 同现 ANALOG/FM6 页 |
| **MOD**（chip 页） | chip：ENV（ATK/DEC/SUS/REL + 其余 ENV 族 TP 参数按上游 ENV 页序）/ ENV DEST / LFO / LFO 2 / LFO DEST；每 chip ≤8 槽，`desc` 缺失的槽不建 | DSYN 页 chip 模式；键盘 ←→ 焦点、↑↓ 调值、chip 经 `UI MOD PAGE` |
| **SCALE**（chip 页） | SEL / SEL 2 / ARP / ARP 2 | 同上 |
| **VOICE**（chip 页） | VOICE / VOICE 2 / PATTERN（`P_SDIV/P_SSWING/P_SGATE`，A8）；ANALOG 的 FILTER TYPE 页仅当引擎为 ANALOG 时追加为第 4 chip | 同上 |
| **FX**（chip 页，从 EDIT 移入 MIX） | FX / SLICER / INSERT / INSERT 2（FX chip = 现 FX 页 8 槽，保持兼容） | 同上 |
| **GLOBAL**（chip 页） | GLO / DLY / REV / MASTER / DRUMS / TEXTURE / TEXTURE 2（GLO+DLY = 现 8 槽） | 同上 |
| **TOOLS** | 三动作：CLEAR SEQ / INIT SOUND / NEW PROJECT（上游 TOOLS 页 `G_CLRSEQ/G_INITSND/G_NEWPRJ` 的语义经适配层移植，动作型全局在引擎里如何落地以清单审计为准） | 触摸 chip + 确认态（二次点击确认，防误触）；走状态层 `CMD_TOOLS_*` |
| MIDI / MIDI 2 / MIDI 3 / SYSTEM | 阶段 5（非目标） | — |
| NOTES（上游音序编辑器） | STEP 页（P4-C） | — |
| REC / STUDIO(DSYN) / SONG / PRESETS / USER / SECTION / MENU | 已对齐（TRANSPORT / DSYN / SONG / PRESETS / PROJECTS / SYSTEM） | — |
| VIS / PUNCH / LIVE / LEARN / HARDWARE CALIBRATION | 清单登记 DI（触发条件写明） | — |

审计先行（Task 1）：清单落定后若上游事实与本表冲突（例如某族的参数在 `desc` 不可达、
TOOLS 动作在上游不是「参数」而是 UI 动作），**以清单与上游源码为准**，回头修订本表并记录裁决。

## 5. Approaches（取舍记录）

**A1 参数覆盖的 UI 形态**（取 B）：
- A 逐页复刻：~30 张新页 ⇒ 导航/容量不可行（子签每组 ≤5，Σ 超限）。
- **B 族页 + chip 子页（取）**：SOUND/MOD/SCALE/VOICE/FX/GLOBAL 六页覆盖全部族，复用 ADR-008 §8.8
  的 chip 模式（DSYN/FM6P 已验证）；新增 MIX 组承载 GLOBAL/FX/TOOLS；FM6 并入 SOUND。
  代价：与上游「层键选族」的心智不同（Tab5 用组签/chip），但与既有 DSYN/FM6P 一致。
- C 通用参数浏览器（desc 全量列表 + 搜索）：工作量最小，但偏离上游信息架构、触屏上难用。

**A2 对齐清单口径**（取特性级）：以「上游特性（页面/层键/菜单项/动作）在 Tab5 是否有可达入口并生效」
为行，而不是逐页像素对齐（ADR-005/008 策展布局是既定决策）。来源：上游 `params.c PAGES[]`、
`ui.c` 屏与层键、`ui_menu.c`、`ui_studio.c`/`ui_song.c`/`ui_vis.c`/`ui_input.c`，加上游
`SLOOP.md`/`GUIDE.md`（固定 commit bac9c05 的只读抓取到 `/tmp`，不入库）。

**A3 DI-26 定位法**（取二分优先）：先用现固件做单用例 ×N 的堆差测量 + `heap_caps_get_info`
按 cap 分类（INTERNAL / DMA / SPIRAM），把 230B/job 收敛到具体 job 名单与 cap；再用「恢复窗口内外的
对照」（在 `selftest_restore()` 前后各读一次堆相关全局，或临时把可疑全局挪出快照窗口）验证
「restore 回滚堆相关全局」假设；仍不收敛再上 `esp_heap_trace` 诊断构建。不预设修法；
**禁止**用「自检后手动重建堆」之类掩盖。DI-26 条目自带的四个方向是候选不是结论。

**A4 截屏矩阵**（取独立脚本）：`scripts/screenshot_matrix.py`（复用 `inject.py` 的 `Session` 与
`screenshot.py` 的读取/重试），不塞进 8.5k 行的 `inject.py` 场景。矩阵 = 19 页 × 2–4 个关键状态
（默认 / 编辑中 / 播放中 / 锁定或错误），产物 `matrix/<page>-<state>.png` + `matrix.csv`
（页/状态/命令/锚点像素）+ 每组一张联系表（Pillow 拼图，≤4×5 格）。像素锚点只做抽检；
**人眼复核是外部门禁**（联系表就是给用户的复核材料）。

**A5 长 soak**（取 3h 默认 + 可延长）：`soak.py --duration 10800`，新增 `ENG POWER` 读数列
（INA226 总线电压 mV / 电流 mA、芯片温度 ℃）。INA226：I2C 0x41，先读总线电压（无需校准）；
电流需要分流电阻值——**若原理图/手册查不到，电流列记 ⚠️ 并以分流电压原始读数代替**，不得臆造校准值。
芯片温度用 IDF `temperature_sensor`。供电：开发机由主机 USB-C 供电即「外接电源」。**无人值守**：收口矩阵里由执行者在后台起跑并继续做文档，
断连/超时判失败并重试一次；用户只需在收口前让设备保持连接（§7）。判定沿用 §5.5（heap floor/duty/xrun/池往返/音频栈），另加
「温度不超过芯片规格的降频阈值」的记录项（不设新门禁，只记录）。

**A6 worker 栈（DI-43(1)）**（取高水位收缩）：chip 跑完打印 `uxTaskGetStackHighWaterMark`，
按实测下调 `SYS_SELFTEST_STACK`（预期 4–6KB，回收 ~10KB 内部堆）。与 A3 同任务量测
（全量自检正好跑一次）。

**A8 表外缺口的处置规则**（builder check 裁决）：审计发现 §4 未列的缺口时——(i) 数据已可达且能作为
一个 chip 进现有页 ⇒ P4-F 内做；(ii) 需要新页、新适配语义或超过一个任务 ⇒ 登记 DI + 触发条件；
(iii) **仅 console `PARAM` 可写不算用户可达入口**（对上游有页/旋钮的参数而言）。已知三项落位：
上游 PATTERN 页（`P_SDIV/P_SSWING/P_SGATE`）⇒ VOICE 页第 3 个 chip `PATTERN`；鼓轨 ⇒ SOUND 锁定
（引擎 edit 无意义），但 MOD/SCALE/VOICE/FX 各 chip 按上游 `page_for_drum` 决定可编辑还是锁定，
审计列出逐 chip 结论；LEN（STEP 页工具）/PAN（FX 页）已可达；`P_ED_FX` 上游亦无页，不算缺口。

**A9 状态相关标签**（builder check 裁决）：上游 `track_desc()` 有三处按状态换标签/名/格式而范围不变
（INSERT A/B/C 随 `P_ITYPE`、ATK/DEC/REL 在 ENV SYNC 时显示音名、LOFI/NOISE/PHYS 随引擎模式）。
SOUND 页：`sloop_core_desc` 的 ENGINE scope 追加调用 `ENGINES[e]->desc` 钩子（上游函数指针，TU 内可达），
不复制表。INSERT 与 ENV SYNC 的表是 vendored `params.c` 的 `static`、外部不可达 ⇒ 允许适配侧**小镜像**
（同 `SONG_PANEL_DEF`/`settings_tail_fill` 先例：注释指向上游行号、host 测试钉住），**只镜像标签/名/格式，
不镜像范围**。§4「不引入新描述符表」据此解读为「不引入新的范围表」。

**A10 SOUND 底行**（builder check 裁决）：底行随选中轨引擎条件出现——FM6 ⇒ bank 行（B1..B27/SAVE/ERASE，
搬自被删的 FM6 页）、其它引擎 ⇒ 无底行；SAMPLE 引擎的槽行**只在 SAMPLE 页**（SOUND 不重复）。
`UI fm6` 保留为 SOUND 的别名。原 R36「锁定页 `NOT FM6`」因行不再出现而不可达：`store` ⓰ 与 `params`
的两个锁定用例改为「底行缺席」断言（TAP bank 行坐标 ⇒ miss），鼓轨 SAVE 的状态层拒绝不变。

**A7 dev→main**：机器门禁全绿 + 清单无未处置行 + 证据入库后，**停下来把收口报告交用户**；
产出收口报告后**停止**（用户明示本阶段不合并 main）。人测项（目检联系表、键盘全矩阵 DI-4、旋钮手感 DI-16、
听感 DI-7/39、LED 颜色、亮度/音量）在报告里列为「main 之前待办/已知开放」，由用户决定
作为阶段 4 的开放项随报告交付；dev→main 的时机由用户另行决定。

## 6. 测试策略

- **DI-26**：新 `scripts/selftest-heap.py PORT`（硬复位 → `ENG MEM` → 全量自检 → `ENG MEM` → 再全量
  → `ENG MEM`，断言第二次前后 `internal_free` 差 ≤1,024B 且 ≥60KB）；放进阶段收口矩阵。
- **host**：`state-test.sh`（`CMD_TOOLS_*`、chip 页不涉状态层新命令除 TOOLS）、`knob-test.sh`
  （chip 页纯函数：族→槽表）、`seq/persist/synth` 不变。
- **注入**：`params.sh` 扩成六页（SOUND 的 12 引擎轮换：`ENG TRACK` 换 12 个引擎后 8 槽 label
  与 `ENG DESC` 一致、鼓轨锁定、FM6/SAMPLE 底行条件出现；MOD/SCALE/VOICE/FX/GLOBAL 各 chip
  的字段名逐一对照 `desc`、写后回读；TOOLS 三动作的确认态与效果）；`nav.sh` 19 页轮转 / 9 组 / 104px；
  `store`/`synth` 场景的 FM6 用例改指向 SOUND；新有序对（EDIT→STEP / MIX→STEP / SEQ→MIX）实测 `pool_free ≥ 4,096`。
- **截屏矩阵**：`screenshot_matrix.py` 退出码 0 + PNG 解码校验 + 联系表生成。
- **soak**：3h + `ENG POWER` 列；soak-selftest.py 的 host 单测同步。
- **全矩阵**：build ×2 / host 全部 / MANIFEST / 烧录 ×3 / 182 / 十一条场景（含复跑）/ 复位 → 3h soak。

## 7. 人测项与需要用户配合的事（不自动执行）

1. 长 soak 3h 期间 Tab5 保持 USB 连接（屏幕常亮）。
2. 联系表目检（19 页 × 状态）。
3. DI-4 键盘 70 键全矩阵、DI-16 旋钮手感、DI-7/39 听感、键盘 RGB 颜色（DI-43(4)）、亮度/音量。
4. 人测结论回填到 verification-log（可选）；dev→main **不在本阶段**，无需决定。

## 8. 风险

1. 审计可能发现本 spec 未预见的缺口（如某引擎 edit 槽数 ≠8、TOOLS 动作语义）——以清单为准修订。
2. 容量：MIX/EDIT 的 est 是上限，实测超限须改对象设计（chip 页比 DSYN 少状态行，预期可守住）。
3. DI-26 若落在 newlib/IDF 库内部（非我方代码），修法受限——允许的最坏结果是「定位 + 隔离 +
   记录」，并在收口报告明示；不得为通过门禁而放宽断言。
4. INA226 分流电阻未知 ⇒ 电流列降级为原始读数。
5. 9 组签 104px：`TRACKS` 文本宽度需实测（预计 ~66px）。
