# 阶段 2：sloop_core 引擎移植实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 上游 SLOOP 引擎（DSP+音序器+状态）作为 `sloop_core` 组件在 Tab5 上正确运行，与 PC host 构建 golden 逐用例一致；console `ENG` 命令驱动验证，**不做 UI**（roadmap §4）。

**Architecture:** vendored 上游树（third_party/sloop-fm1 @bac9c05，只读）+ 单 TU 组合（复刻 preview.c 包含序，Tab5 stub 穿插）+ sloop_hal_tab5 音频/存储后端；host 基线用上游 tests/ 原生编译（cc+libm），设备侧 ENG SELFTEST 与 host 跑**同一份** regress 用例驱动对哈希。

**Tech Stack:** ESP-IDF v5.5.5 / esp32p4 / 上游 SLOOP 2.8.5（bac9c05，GPL-3.0）/ Python 3（生成头与脚本）/ 本机 cc（host 基线）。

**Spec:** `docs/superpowers/specs/2026-11-20-phase2-engine-port-design.md`（阶段 2 spec，含上游精读实证）+ `docs/design/004-upstream-import.md`（D1 ADR）+ `docs/design/003-phase2-entry.md`。执行前两份 spec/ADR 必读。

## Global Constraints

- 构建/烧录只走 `scripts/`（build.sh / flash.sh / verify.sh）；host 侧门禁走 `scripts/host-test.sh`（Task 2 落地后生效，AGENTS §5.5 引擎/DSP 行）。
- 固件门禁：`scripts/build.sh` 退出码 0 且第二遍零新增警告（kconfig 首 pass 豁免对，AGENTS §5.5）；烧录 `Hash of data verified`、启动无 fatal/abort/backtrace。
- **third_party/sloop-fm1/ 只读禁改**（AGENTS §8；适配全部在组件 shim 与包含序）；GPL 许可文件（LICENSE/LICENSES/CREDITS）必须随树入库。
- 数值约定：host/device 双侧 `-ffp-contract=off`、禁 `-ffast-math`，核心编译对齐上游 `-O2 -fwrapv -fno-strict-aliasing`；工具链版本与 flags 随 golden 基线入库。
- 音频：FS=44100；`mix_block` 输出 int32 交错 L/R、有效幅度 int16 范围；ES8388 通路 44.1kHz/2ch/16bit（阶段 1 已实证，输出音量板级初始化见 fix 4278509，勿回退）。
- DI-2：凡改 `sdkconfig.defaults` 先 `rm firmware/sdkconfig`。
- 证据晋升：`docs/dev/verify/2-engine-<日期>/` + verification-log 登记（含 SHA256）。
- 提交格式 AGENTS §7；feature 分支 `feat/sloop-core` 从 dev 拉出，收尾合回。
- 人测项（听音）按阶段 1 Ruling 1 惯例：机器侧全做，人耳项列入收尾清单。

## Review Focus（测试已钉进对应任务）

1. **vendored 树不完整或被改动**（缺文件/混入修改 → 后续全部结论失真）→ Task 1 Step 4 MANIFEST.sha256 校验 + 许可文件存在性检查；后续任何任务发现 third_party 有改动立即停。
2. **浮点分叉**（RV32F fmadd 收缩 vs x86 SSE2 → 普通乘加即分叉）→ Task 2 Step 3 与 Task 3 Step 2 双侧 flags 核验 + Task 4 容差通道。
3. **生成头缺失/陈旧**（build/gen 不入 git，换机/清理后静默用旧头）→ Task 2 Step 2 host-test.sh 强制先跑 4 个生成脚本 + Task 3 Step 2 固件构建依赖同一批生成头。
4. **ENG SELFTEST 与 host 判定代码不同源**（手抄用例 → 假绿）→ Task 4 Step 1 设备侧直接编译 third_party 的 tests/regress.c 同一文件，禁复制改写用例逻辑。
5. **实时性/内存**（mix_block 超块周期 → 爆音；大静态区挤占内存）→ Task 4 Step 4 ENG PERF 占空比与 xrun 计数 + Task 6 水位入档。

---

## 文件结构（本计划落地后）

```
third_party/sloop-fm1/                 # 新建（Task 1）：vendored 上游子树 + LICENSE 族，只读
third_party/MANIFEST.sha256            # 新建（Task 1）：完整性清单
tools/fetch_upstream.sh                # 新建（Task 1）：固定 commit 抓取 + 校验（默认 SSH，支持镜像参数）
scripts/host-test.sh                   # 新建（Task 2）：host 基线门禁（生成头 + 子集编译运行）
firmware/components/
├── sloop_core/                        # 新建（Task 3）
│   ├── CMakeLists.txt
│   ├── include/sloop_core.h           # 对外 API（spec §4.1）
│   └── src/sloop_core.c               # 组合 TU（preview.c 包含序 + Tab5 stub）
└── sloop_hal_tab5/                    # 新建（Task 3 骨架 / Task 4 音频流）
    ├── CMakeLists.txt
    ├── include/sloop_hal.h
    └── src/{audio_stream.c, storage_ram.c}
firmware/main/{main.c,console.c,CMakeLists.txt}   # 修改：ENG 命令（Task 3 骨架 / Task 4 实现）
docs/dev/verify/2-engine-<日期>/        # 新建（Task 2 起）：host 基线/golden/哈希对比/性能证据
docs/{design,research,dev}…            # 各任务按 AGENTS §6 同步
```

---

### Task 0: 基线与分支

**Files:** 无

- [ ] **Step 1:** `git checkout dev && git checkout -b feat/sloop-core`
- [ ] **Step 2:** `scripts/build.sh` → 退出码 0、无 warning（比较基线）。

### Task 1: P2-1 上游引入（vendoring + 完整性 + 常量提取）

**Files:**
- Create: `tools/fetch_upstream.sh`, `third_party/sloop-fm1/`（子树）, `third_party/MANIFEST.sha256`
- Modify: `AGENTS.md`（§3 目录规划 third_party/tools 落地）, `docs/research/sloop-fm1-firmware.md`（复核记录行）, `docs/superpowers/specs/2026-11-20-phase2-engine-port-design.md`（§2.4 常量表回写）

**Interfaces:**
- Produces: `third_party/sloop-fm1/`（至少含 `firmware/src/`、`firmware/hal/`、`firmware/host/`、`tests/`、`tools/`（gen_*/sampleio/gen_waves/fetch_cc0）、`web/EDITOR_PROTOCOL.md`、`LICENSE`+`LICENSES/`+许可/CREDITS 文件）；pin `bac9c05ea7627449405aa10e48da21d75b6c8c56`。
- Produces（常量，Task 3/4 消费）：`FS=44100`、`CTL=32`、`HALF_FRAMES=256`、`NSTEP=64`、`NENGINES=12`（默认 `FELUCCA_SLICE=0`）、`P_COUNT=71`、`G_COUNT=50`、`SMP_USER_SIZE=0x14000`（81920 字节）。除 `CTL` 来自 `tools/gen_tables.py`、`SMP_USER_SIZE` 来自 `eng_sample.c` 外，其余定义均在 vendored `core.h`。

- [ ] **Step 1: 确立取源通道**

上游取源默认使用已实证可用的 SSH：`git ls-remote git@github.com:isod89/sloop-fm1.git HEAD` 返回 bac9c05；HTTPS git/curl 不通的问题由 SSH 规避。fetch 脚本以 SSH 为默认通道，保留 `--mirror`/`--ref` 作为备选通道与升级参数。

- [ ] **Step 2: 实现 `tools/fetch_upstream.sh`**

参数：`[--ref <sha>] [--mirror <prefix>]`；默认 ref=上述 pin。行为：抓取 → 检出到 `third_party/sloop-fm1/`（若已存在则先校验再覆盖）→ 生成/校验 `third_party/MANIFEST.sha256`（对子树全部文件）。脚本幂等；校验失败非零退出。

- [ ] **Step 3: 引入并核对许可**

Run: `tools/fetch_upstream.sh`
Expected: 子树就位；`LICENSE`、`LICENSES/`（Dexed msfa / DaisySP / Mutable 等子许可）、`assets/samples-cc0/CREDITS.txt`（若含 assets）存在。许可核查记录写入 `docs/dev/`（一段即可：GPL-3.0 主体 + 子许可清单 + CC0 资产结论）。

- [ ] **Step 4: 完整性校验 + 只读确认**

Run: `cd third_party && sha256sum -c MANIFEST.sha256` → 全 OK。
Run: `git status --short third_party/` → 仅新增，无后续改动（此后任何任务 third_party 出现 M 立即停，Review Focus 1）。

- [ ] **Step 5: 常量提取与文档回写**

从 vendored 树提取 §Produces 常量值，回写 spec §2.4 常量表与本计划 Task 1 Produces（本计划无字面 `__CTL__`/`__HALF_FRAMES__` 占位）。`docs/research/sloop-fm1-firmware.md` 头部加一行：2026-11-20 复核 main HEAD=bac9c05 无漂移（git ls-remote SSH）。

- [ ] **Step 6: Commit**

`feat(third_party): vendored 上游 SLOOP 2.8.5（bac9c05）与抓取校验脚本`（AGENTS §7 标签体；AGENTS §3 同步随此提交）。

### Task 2: P2-2 host 基线（生成头 + host-test.sh + golden 入库）

**Files:**
- Create: `scripts/host-test.sh`
- Create: `docs/dev/verify/2-engine-<日期>/host-baseline.txt`（工具链/flags/golden 摘要记录）

**Interfaces:**
- Consumes: Task 1 的 third_party 树与 4 个生成脚本。
- Produces: `scripts/host-test.sh`（后续一切引擎改动的回归门禁）；`third_party/sloop-fm1/build/gen/` 4 个生成头（`felucca_tables.h`/`felucca_samples.h`/`felucca_drumkits.h`/`felucca_fm6.h`，不入 git——确认 .gitignore 覆盖 `third_party/sloop-fm1/build/`）；host 侧可执行 `regress`/`hostsim` 与 golden 基线记录。

- [ ] **Step 1: 精读 `third_party/sloop-fm1/tests/regress.c` 头部与 main**

提取：用例驱动入口（main 参数）、golden.txt 读写方式、hash 调用位置、健康检查开关——Task 4 设备侧同源复用要按此对接。结论记入报告（不写推测）。

- [ ] **Step 2: 写 `scripts/host-test.sh`（生成头 + 编译 + 运行子集）**

行为（在 third_party 树根执行，输出到其 `build/host/`）：
1. 强制运行 4 个生成脚本（`python3 tools/gen_*.py build/gen/<对应头>`；gen_samples 无 CC0 时打印 generated-only 属正常）；
2. `CC="${CC:-cc} -O2 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -Ibuild/gen -Ifirmware/src"`（含 `-Ifirmware/hal` 的用例按 run_tests.sh 原样补）；
3. 编译并运行：`regress`（`tests/regress.c -lm`，跑 `tests/golden.txt tests/cpu_baseline.txt`）、`hostsim`（`tests/hostsim.c -lm`，渲染一次 WAV 验证退出码）、`seq2_test`、`project_test`、`storage_test`、`upreset_test`、`dsyn_test`、`drumkit_test`、`punch_test`、`soak_test`（`SOAK_MIN=1` 短参数）。
全部通过打印 `ALL HOST TESTS PASSED (subset)`，任一失败非零退出。**不搬** ota/ldr/installer/site/pwa/target_budget/web 用例（FM-1 打包链路）。

- [ ] **Step 3: 跑通并固化基线**

Run: `scripts/host-test.sh`
Expected: 末行 `ALL HOST TESTS PASSED (subset)`，退出码 0。若 regress 报 golden 差异：先怀疑本机 flags（确认 `-ffp-contract=off` 已加、未开 fast-math），仍不过则记录差异用例与数值，**不得 GOLDEN_UPDATE**——停下来报控制器。
记录 `cc --version` 首行、完整 CFLAGS、`tests/golden.txt` 的 SHA256 到 `docs/dev/verify/2-engine-<日期>/host-baseline.txt`，并在 verification-log 登记「host-test.sh 门禁自此生效」。

- [ ] **Step 4: Commit**

`test(scripts): host 基线门禁 host-test.sh（golden 子集 + 生成头强制刷新）`

### Task 3: P2-3 sloop_core + sloop_hal_tab5 组件（交叉编译 + 骨架 ENG）

**Files:**
- Create: `firmware/components/sloop_core/{CMakeLists.txt,include/sloop_core.h,src/sloop_core.c}`
- Create: `firmware/components/sloop_hal_tab5/{CMakeLists.txt,include/sloop_hal.h,src/storage_ram.c}`（音频流在 Task 4）
- Modify: `firmware/main/{main.c,console.c,CMakeLists.txt}`、`firmware/main/idf_component.yml`（如需）

**Interfaces:**
- Consumes: third_party 树（Task 1）、Task 2 生成头、Task 1 常量值。
- Produces（spec §4.1，逐字）：
```c
esp_err_t sloop_core_init(void);
void sloop_core_render(int32_t *block, uint32_t frames);
void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset);
void sloop_core_note(uint32_t track, uint32_t note, bool on);
void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity);
void sloop_core_play(bool on);
uint32_t sloop_core_rate(void);
uint32_t sloop_core_frames(void);
```
- Produces（HAL）：`esp_err_t sloop_hal_init(void);`（本任务先挂 RAM 存储后端；音频在 Task 4）。

- [ ] **Step 1: 写 `sloop_core.c` 组合 TU**

复刻 spec §2.1 的 preview.c 包含序与 stub 清单（libc.c 宏替换 → 生成头 → SMP_USER_XIP（先指 RAM 槽，同 preview 的 4×0x14000）→ fm1_in/irq_off/on（空或临界区）→ core.h → engines/drums/params/voice/slicer → cpu1.h 四个单核 stub → fx.c → fm1_delay_ms（vTaskDelay）→ usb.c/midi_uart.c/seq.c）。包含路径经 CMake 指向 third_party（`../../../../third_party/sloop-fm1/firmware/src` 等，用 CMake 变量集中定义）。若编译暴露 preview.c 未覆盖的 FM-1 专属依赖（如 `fm1_lcd_hw`/`fm1_flash` 引用），在 shim 中以最小 stub 补齐并记录清单——**不改上游文件**。
`sloop_core_init()` 复刻 preview_init 序列（spec §2.6）。

- [ ] **Step 2: CMake 集成与 flags**

`sloop_core/CMakeLists.txt`：`idf_component_register(SRCS src/sloop_core.c INCLUDE_DIRS include PRIV_REQUIRES sloop_hal_tab5)`；`target_compile_options(... PRIVATE -O2 -fwrapv -fno-strict-aliasing -ffp-contract=off -Wno-unused-function)`；包含 third_party src/hal 与生成头目录。**生成头来源**：Task 2 已在 third_party/build/gen 生成——固件构建直接引用该目录，并在组件 CMake 加注释「生成头由 scripts/host-test.sh / tools 生成，缺失时先跑 Task 2 Step 2 命令」（不引入 CMake 自定义命令跑 Python 的复杂度；缺失时报错信息指名解决路径）。

- [ ] **Step 3: 骨架 ENG 命令 + 启动挂接**

console.c 注册 `ENG`（本任务仅 `ENG INFO`：打印 rate/frames/NENGINES/P_COUNT/G_COUNT 常量与 init 结果）；main.c 启动调 `sloop_core_init()`（失败记 ERROR 不阻断其他自检）。

- [ ] **Step 4: 构建烧录验证**

Run: `scripts/build.sh && scripts/build.sh` → 第二遍零新增警告；`idf.py size` 类输出记录 app/rodata 增量（供 Task 5 资产预算）。
Run: `scripts/flash.sh` + 串口 `ENG INFO` → 常量打印正确（与 Task 1 提取值一致）、启动无 fatal/abort/backtrace。

- [ ] **Step 5: Commit**

`feat(components/sloop_core): 单 TU 组合上游核心与 Tab5 shim（交叉编译通过）`

### Task 4: P2-4 ENG SELFTEST / TRIG / PERF + 音频流（设备等价与可听）

**Files:**
- Create: `firmware/components/sloop_core/src/selftest.c`（设备侧 regress 同源驱动的控制台适配）
- Create: `firmware/components/sloop_hal_tab5/src/audio_stream.c`
- Modify: `firmware/main/console.c`、`firmware/components/sloop_core/{CMakeLists.txt,include/sloop_core.h}`

**Interfaces:**
- Consumes: Task 3 全部 API；Task 2 的 regress.c 精读结论；阶段 1 `audio_tone` 的 codec 初始化模式（`bsp_audio_codec_speaker_init` + 44.1kHz/2ch/16bit + 板级输出音量初始化——复用而非重写，注意最终审查建议：codec 所有权需单一，audio_tone 与 audio_stream 共用一个 handle，audio_tone 保留为诊断）。
- Produces: console `ENG SELFTEST`（逐用例 `name hash` 输出 + 汇总 PASS/FAIL）、`ENG TRACK <k> <engine> <preset>`（= `sloop_core_track`，选轨引擎/预置）、`ENG TRIG <synth|drum> <a> <b>`（合成轨 note / 鼓 kit+lane）、`ENG PERF`（每块周期数、块周期预算、占空比%、xrun 计数、最小剩余堆）。

- [ ] **Step 1: 设备侧 selftest 同源接入**

把 third_party `tests/regress.c` 以独立 TU 编入 sloop_core（`selftest.c` 提供其需要的 host 依赖 shim：printf→ESP_LOG/控制台、文件读 golden→内嵌 `golden.txt` 字符串常量（由 third_party 原文件生成，CMake 或一次性 xxd 均可，注明生成方式）、任何 host-only 分支用与 host 相同的宏路径）。**禁止手抄/改写用例**（Review Focus 4）。哈希生成代码路径与 host 完全一致。

- [ ] **Step 2: `ENG SELFTEST` 命令**

运行用例集（可分子集参数 `ENG SELFTEST [case]`），逐用例打印 `name hash`；与 host `golden.txt` 比对在 PC 侧做（脚本或人工 diff）：全等 → PASS；不等 → 对该用例输出 max|err|/SNR（内嵌参考向量来自 host 渲染 WAV/原始块，随基线入库），按容差通道判定并记录成因。比对结果与容差阈值入 `docs/dev/verify/2-engine-<日期>/`。

- [ ] **Step 3: 音频流 + `ENG TRIG`**

`audio_stream.c`：双半缓冲 int32（`HALF_FRAMES` 帧，Task 1 常量）+ 音频任务（或 I2S 半完成回调）：空闲半 `sloop_core_render()` 填充 → int32→int16 转换写 codec。启动 `ENG TRIG` 时才开流（默认静音，避免与 AUD TONE 争用——同一 codec handle 共享）。
`ENG TRACK <k> <engine> <preset>` = `sloop_core_track`（引擎号枚举见 third_party engines.c，SAMPLE 等索引在 Task 5 使用时查得）。`ENG TRIG synth <track> <note>` = `sloop_core_note(track,note,on/off)` 各一次（间隔 ~500ms）；`ENG TRIG drum <kit> <lane>` = `sloop_core_hit`。各出声一次。

- [ ] **Step 4: `ENG PERF` + 验证**

`ENG PERF`：连续渲染 N=100 块，测每块 esp_timer 周期 → 均值/最大、占块周期预算（`HALF_FRAMES/44100` 秒）的占空比、xrun 计数、最小剩余堆（Review Focus 5）。
机器门禁：build×2 零警告、烧录、启动无 fatal；SELFTEST 哈希比对结果入档；TRIG 日志无错误。
人测项：合成轨 + 鼓各听一次（收尾清单）。

- [ ] **Step 5: Commit**

`feat(components/sloop_core): ENG SELFTEST/TRIG/PERF 与设备等价验证音频流`

### Task 5: P2-5 样本资产（CC0 获取 + 体积实测 + 采样 kit 可听）

**Files:**
- Modify: `third_party/sloop-fm1/build/gen/`（重新生成含 CC0 的 `felucca_samples.h`，产物不入库）
- Modify: `docs/dev/`（资产许可与体积记录）
- 视体积结果可能新增微 ADR（D4 降级路径）

- [ ] **Step 1: 获取 CC0 资产**

Run: `cd third_party/sloop-fm1 && python3 tools/fetch_cc0.py`（通道同 Task 1；失败则记录并以 generated-only 继续，不阻塞阶段完成判据中「合成 kit」项，采样 kit 项降级为待办）。
核对 `assets/samples-cc0/CREDITS.txt` 与 CC0 许可结论入 docs。

- [ ] **Step 2: 重新生成 + 体积实测**

重跑 gen_samples → 重编固件；记录 `SMP_DATA` 字节数与 app/rodata 增量。若 rodata 超出 factory 分区余量 → 启动 D4 降级微 ADR（SMP_DATA 转 FATFS storage 分区 + XIP 映射改读取），否则保持 rodata 内嵌。

- [ ] **Step 3: 采样 kit/引擎可听验证**

先 `ENG TRACK 0 <SAMPLE引擎号> 0`（引擎号从 third_party engines.c 查得）再 `ENG TRIG synth 0 60` 触发 SAMPLE 引擎预置（如 PIANO/GRAND PNO），并以 `ENG TRIG drum <kit> <lane>` 触发 GM 鼓 kit；机器侧日志无错；人耳确认入收尾清单。SELFTEST 哈希比对复跑（资产变化可能改变 golden——若 host/device 同步重新生成仍一致即过；host 基线记录同步更新）。

- [ ] **Step 4: Commit**

`feat(components/sloop_core): CC0 采样资产接入与体积核验`（若走降级路径则含微 ADR 与 FATFS 后端）。

### Task 6: P2-6 性能基线与阶段收口

**Files:**
- Create: `docs/dev/verify/2-engine-<日期>/`（性能数据、SELFTEST 比对汇总、host 基线、听音记录）
- Modify: `docs/dev/{verification-log.md,device-notes.md}`、`docs/design/002-port-roadmap.md`（§1 阶段 2 状态）、`AGENTS.md`（§3 组件行落地）、`docs/design/003-phase2-entry.md`（完成标注）

- [ ] **Step 1: 全门禁复跑**

`scripts/host-test.sh`（0）、`scripts/build.sh`×2（零新增警告）、`scripts/verify.sh`（绿）、`ENG SELFTEST` 比对结论、`ENG PERF` 数据（vs FM-1 pi32v2 120MHz 预算说明）。

- [ ] **Step 2: 证据晋升 + 文档同步**

证据（host-baseline、golden 比对、PERF、许可记录）晋升 `docs/dev/verify/2-engine-<日期>/` 并登记 verification-log；AGENTS §3 组件行去掉 sloop_core/sloop_hal_tab5 的【规划】；roadmap §1 阶段 2 状态按实际标注（机器侧完成/听音待办分列）。

- [ ] **Step 3: 合并**

`git checkout dev && git merge --no-ff feat/sloop-core`；里程碑候选「sloop_core 首版可听」——dev→main 由负责人在人测项清完后决定。

- [ ] **Step 4: Commit**

`docs(.): 阶段 2 引擎移植收口与性能基线入档`

---

## 完成判据对照（roadmap §4）

1. host-test.sh 全绿、工具链/flags 随基线入库（Task 2）；
2. ENG SELFTEST 设备哈希与 host 一致或容差内通过并留痕（Task 4）；
3. ENG TRIG 合成 + 采样 kit 听音正确（Task 4/5，人耳项入收尾清单）；
4. 性能基线入档、构建零新增警告（Task 6）；
5. 里程碑候选 dev→main（Task 6）。

## 遗留与后续

- 阶段 1 残余人测（DI-4 键盘矩阵/触摸校准/目检）与 DI-6（输出寄存器二分、音量策略、音高复核）→ DI-6 已在 Task 4 音频落地时处理为优。
- D2 形式 ADR 与状态层：阶段 3。
- 阶段 3 入口：InputManager 70 键映射、UI ADR（D3）、MVP 四屏、串口注入（roadmap §5）。
