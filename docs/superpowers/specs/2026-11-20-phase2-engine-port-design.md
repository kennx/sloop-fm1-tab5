# 阶段 2：sloop_core 引擎移植设计规格（Phase-2 Spec）

> 日期：2026-11-20 ｜ 状态：草案（待项目负责人评审）
> 上游基线：**SLOOP 2.8.5（bac9c05ea7627449405aa10e48da21d75b6c8c56）**——2026-11-20 经 `git ls-remote` SSH 复核 main HEAD 未漂移，research 基线保持有效。
> 上位文档：`docs/superpowers/specs/2026-11-19-sloop-tab5-port-design.md`（总体设计，§5 架构/§6 决策表）；`docs/design/003-phase2-entry.md`（入口目标与核查清单）；`docs/design/002-port-roadmap.md` §4（任务与完成判据，权威）。
> 本文所有「实证」结论来自 2026-11-20 对上游 bac9c05 源码的精读（经 GitHub MCP 取源），非调研摘要转述。

## 1. 目标与非目标

**目标**（roadmap §4）：上游引擎（DSP + 音序器 + 状态）作为 `sloop_core` 组件在 Tab5 上运行，与 PC host 构建逐样本一致（golden 锚定）；console 驱动验证，**不做 UI**。

**非目标**：不做 UI（阶段 3）；不做 USB host/编辑器协议（阶段 5）；不做双核分派（D8：单核先行）；不做性能优化（P2-6 只测基线）；不改上游 `src/` 内部逻辑。

## 2. 上游架构事实（本次精读实证）

### 2.1 核心是「单 TU 组合 + 平台 stub 穿插」结构

上游 `firmware/host/preview.c`（浏览器 WASM 适配层）展示了标准嵌入方式——**一个翻译单元按序 #include 核心 .c 文件，在包含点之间插入平台 stub**：

```c
#include "../src/libc.c"            /* memset/memcpy/memcmp 宏替换为 felucca_* */
#include "felucca_tables.h"         /* 生成头（见 2.3） */
static uint8_t user_flash[4][0x14000];         /* 用户采样槽 4×80KB，RAM 或后端存储 */
#define SMP_USER_XIP(k) (user_flash[(k) & 3u])
static struct { volatile uint32_t notes, buttons; } fm1_in;   /* 面板输入（Tab5 由输入层喂） */
static void fm1_irq_off(void) {}  static void fm1_irq_on(void) {}
#include "../src/core.h"
#include "../src/engines.c"
#include "../src/drums.c"
#include "../src/params.c"
#include "../src/voice.c"
#include "../src/slicer.c"
#include "../src/cpu1.h"           /* 第二核接口；preview 全部 stub 为单核内联 */
static int cpu1_run(cpu1_job_t fn, uint32_t arg) { ... } 等 4 个 stub
#include "../src/fx.c"
static void fm1_delay_ms(uint32_t ms) { ... }
#include "../src/usb.c"
#include "../src/midi_uart.c"
#include "../src/seq.c"
```

Tab5 的 `sloop_core` 复刻此模式：一个 `sloop_core.c`（组合 TU）+ `tab5_*.h/c`（stub 后端）。**上游文件零修改**；所有平台差异在 stub 与包含顺序中表达。

### 2.2 host 原生构建零障碍

`tests/run_tests.sh` 实证：核心测试（hostsim/regress/seq2/project 等）只用
`cc -O2 -w -Ibuild/gen -Ifirmware/src [-Ifirmware/hal] <单文件测试.c> -lm` 即可在 Linux 原生编译运行——**无 JieLi SDK 依赖**。`tests/hostsim.c` 直接把整个核心编成原生渲染器（`hostsim 0 0 1 out.wav` 出 WAV）。

注意：完整 `run_tests.sh` 还含依赖 FM-1 打包产物（`build/felucca.fwsc`/`.dis`）的用例（ota/ldr/installer/site/pwa/target_budget）。**Tab5 的 `scripts/host-test.sh` 只搬运 DSP/音序/存储/协议编解码子集**（清单见 §6），不搬 FM-1 打包链路。

### 2.3 生成头是构建前置（4 个，纯 Python）

| 生成脚本 | 产物（build/gen/） | 外部依赖 |
|---|---|---|
| `tools/gen_tables.py` | `felucca_tables.h` | 无（纯 Python） |
| `tools/gen_samples.py` | `felucca_samples.h` | 可选 `assets/samples-cc0/`（缺省仅用 `gen_waves.py` 合成鼓生成，**不下载也能构建**）；CC0 由 `tools/fetch_cc0.py` 单独抓取 |
| `tools/gen_drumkits.py` | `felucca_drumkits.h` | 纯 Python |
| `tools/gen_fm6_patches.py` | `felucca_fm6.h` | 纯 Python |

**资产许可结论（P2-5 前置解除）**：内置采样资产源为 **CC0**（公有领域，可再分发，`assets/samples-cc0/CREDITS.txt` 记出处），Felucca 自有合成鼓随 GPL 源码——**不存在「不可再分发」阻断项**；但 CC0 WAV 体积与获取通道需 Task 时核实（是否入库或 fetch 脚本，见 §5 D4）。

### 2.4 音频节拍与渲染 API

- 渲染入口：`mix_block(int32_t *block, frames)`——一次渲染 `frames` 帧 **int32 交错 L/R**；有效幅度在 int16 范围（preview.c `clamp(block[i], -32768, 32767)` 实证）。
- 核心常量（host/device 共用，不存在转换；枚举值经宿主 C 编译求值）：

| 常量 | 值 | 含义 | 上游来源 |
|---|---:|---|---|
| `FS` | 44100 | 采样率（Hz） | `firmware/src/core.h:351` |
| `CTL` | 32 | 控制/DSP 块长（帧） | `tools/gen_tables.py:14`（生成 `felucca_tables.h`） |
| `HALF_FRAMES` | 256 | I2S 半缓冲帧数（44.1kHz 下约 5.8ms） | `firmware/src/core.h:16` |
| `NSTEP` | 64 | 每轨步数 | `firmware/src/core.h:15` |
| `NENGINES` | 12 | 默认引擎数（`FELUCCA_SLICE=0`） | `firmware/src/core.h:17,23` |
| `P_COUNT` | 71 | 每轨参数数 | `firmware/src/core.h:48-75` |
| `G_COUNT` | 50 | 全局参数数 | `firmware/src/core.h:77-109` |
| `SMP_USER_SIZE` | 0x14000（81920 字节 / 80KiB） | 单个用户采样槽大小 | `firmware/src/eng_sample.c:46` |

- FM-1 硬件节拍（`hal/fm1_audio.h`）：应用自有双半缓冲（int32 L/R 24-bit 左对齐），ISR 中 `fm1_audio_pending/ack_aux/free_half/ack_half` 填空闲半缓冲。
- **Tab5 映射**：`sloop_hal_tab5` 持同形双半缓冲；I2S DMA 半完成回调（或音频任务等待可写）→ `mix_block` 填空闲半 → 格式转换（int32 → int16 交错，直接取值）→ `esp_codec_dev_write`（阶段 1 已实证 44.1kHz/2ch/16bit 通路）。

### 2.5 golden 机制（tests/regress.c）

- `tests/golden.txt`：每用例一行 `<用例名> <hash>`，hash 为渲染 int32 缓冲的 **FNV-1a 64**；含 `mix_4`（四轨混音）等；每引擎×预置、鼓 kit、声部模式、FX 发送全覆盖。
- `tests/cpu_baseline.txt`：CPU 成本基线（+25% 容差）；`target_budget.txt` 文件随 tests 树入库，但其依赖 FM-1 反汇编的 **target_budget 用例/检查不在 `host-test.sh` 搬运范围**。
- `ENG SELFTEST` 设计：设备侧复用 regress.c 的用例驱动与哈希生成（同一份 C 代码编进设备），逐用例输出 `name hash`，与 host 跑出的 `golden.txt` 对比；不一致时用例转误差通道（max|err|/SNR 对内嵌参考向量）。

### 2.6 初始化序列（preview_init 实证，设备复刻）

G/P 参数默认值 → `steps_clear` ×4 轨 → `dsu_defaults()` → `fm6_init()` → `song.master_q12=4096`、`G_SYNC=0`。触发/传输语义：`input_on/off(&trk[k], note, vel)`、鼓 `drum_on(LANE_NOTE[lane], vel)`（先切 `TDRUM->p[P_E0]` 选 kit）、`transport_req=1/2`、`mix_block` 内驱动音序。

## 3. 方案：D1 上游引入机制（定稿，详见 ADR-004）

**采用 vendoring**：`third_party/sloop-fm1/`（精简上游子树 @bac9c05，含 LICENSE/LICENSES/CREDITS/ATTRIBUTION），由 `tools/fetch_upstream.sh` 按固定 commit 抓取/校验（SHA 锁定 + 全部 vendored 文件 SHA256 清单）；`firmware/components/sloop_core/` 只放组合 TU 与 Tab5 shim，经相对路径引用 third_party。

- 不选 submodule：构建不依赖 submodule init 状态与网络；GPL 对应源完整随仓；diff 跟随上游 = 重跑 fetch 脚本换 commit + git diff。
- **取源通道（2026-11-20 复核）**：本机 HTTPS git/curl 直连 GitHub 失败（TLS 中断），SSH `git ls-remote` 与 clone 可用。fetch 脚本默认 SSH，并支持 `--mirror` 备用前缀；源码入库后的构建不依赖网络。

## 4. 组件设计

```
third_party/sloop-fm1/            # vendored 上游（只读，禁改）
firmware/components/
├── sloop_core/
│   ├── CMakeLists.txt            # 单 TU 编译；CFLAGS 对齐 §5 数值约定
│   ├── include/sloop_core.h      # 对外 API（§4.1）
│   └── src/sloop_core.c          # 组合 TU（复刻 preview.c 包含序，Tab5 stub）
└── sloop_hal_tab5/
    ├── include/sloop_hal.h       # 音频流/存储/时间/日志后端接口
    └── src/{audio_stream.c, storage_fatfs.c, ...}
```

### 4.1 sloop_core 对外 API（console 与阶段 3 状态层的唯一入口）

```c
esp_err_t sloop_core_init(void);                       /* 复刻 preview_init + 后端挂接 */
void sloop_core_render(int32_t *block, uint32_t frames); /* = mix_block，音频任务调用 */
void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset); /* = preview_track：选轨引擎/预置 */
void sloop_core_note(uint32_t track, uint32_t note, bool on);
void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity);
void sloop_core_play(bool on);
uint32_t sloop_core_rate(void);   /* FS */
uint32_t sloop_core_frames(void); /* Tab5 音频半缓冲粒度：HALF_FRAMES（256）；核心控制量子仍为 CTL=32 */
```

设备骨架另暴露 `sloop_core_status()` 供 `ENG INFO` 读取启动初始化结果；公开计数宏
`SLOOP_CORE_NENGINES/P_COUNT/G_COUNT` 在组合 TU 内以 `_Static_assert` 与上游枚举核对，
避免 console 直接包含上游 `core.h`。

Tab5 链接适配由 `sloop_core/linker.lf` 把上游 `.pool`/`.lowram` 零初始化 DSP 缓冲映射到
`dram0_bss`；4×80KiB 用户采样槽由 `sloop_hal_tab5` 在 PSRAM 堆分配（内部 DRAM 不足以同时
容纳约 260KiB `.pool`、其余核心状态与 320KiB 用户槽），`SMP_USER_XIP` 直接指向该 RAM。

`ENG SELFTEST` 不走此 API——它把 regress.c 用例驱动作为独立 TU 编入（与 host 同源），保证 host/device 跑的是同一份判定代码。

### 4.2 D2（本地编辑入口）记录

采用**精简 EditCmd 词表**（spec §6 建议）：本阶段 `sloop_core_*` 即词表雏形（note/hit/play/param/global/step/select…语义与 preview_* 对齐，字段与编辑器协议兼容）；阶段 3 状态层正式定义并落 ADR。本阶段不内嵌上游 `editor.c` 全集。

### 4.3 单核先行（D8）

`cpu1_run/wait/on/ticks` 全部 stub 为单核内联（同 preview.c）；P2-6 性能基线后再评估双核。

## 5. 关键决策表（本阶段）

| # | 决策 | 结论 | 依据 |
|---|---|---|---|
| D1 | 引入机制 | **vendoring + fetch 脚本**（§3，ADR-004） | 构建自洽/GPL 完整/diff 可跟随 |
| D2 | 本地编辑入口 | 精简 EditCmd 词表（§4.2），阶段 3 形式化 | 避免引入协议全集复杂度 |
| D4 | 资产位置 | **生成头内嵌 flash（默认）**：CC0 缺席时也能构建；完整 CC0 集经 fetch_cc0.py 获取后重新生成，数据进 rodata（SMP_DATA）；**不引入 FATFS 资产依赖**——仅当 rodata 超预算才转 FATFS（届时微 ADR） | gen_samples 实证可无资产构建；减少活动部件 |
| D8 | 核分派 | 单核 stub | preview.c 同构 |

## 6. 验证策略

1. **host 基线（P2-2）**：`scripts/host-test.sh` = 生成 4 头 → 编译并运行子集：`regress`（golden 全量）、`hostsim`（渲染 WAV）、`seq2_test`、`project_test`、`storage_test`、`upreset_test`、`dsyn_test`、`fm6_test`、`drumkit_test`、`punch_test`、`soak_test`（短参数）。工具链版本与 CFLAGS 随 golden 基线入库（`docs/dev/verify/2-engine-<日期>/host-baseline.txt`）。
2. **设备等价（P2-4）**：`ENG SELFTEST` 逐用例 hash 对 host `golden.txt`；不等 → max|err|/SNR 容差通道（阈值随基线入库）+ 成因留痕。
3. **可听（P2-4/P2-5）**：`ENG TRIG` 合成 kit + 采样 kit 人测。
4. **性能（P2-6）**：`ENG PERF` 每块 CPU 周期与内存水位入档。
5. 数值约定：host/device 双侧 `-ffp-contract=off`、禁 `-ffast-math`，`-fwrapv -fno-strict-aliasing` 与上游构建对齐（build_preview.py 实证上游即用这两旗标）。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 取源通道不通（本机 HTTPS 已实证失败） | 默认 SSH 抓取（已实证可用）+ `--mirror` 备用前缀；源码入库后构建完全离线 |
| 浮点分叉（RV32F fmadd 收缩） | §6.5 双侧 FP 约定 + 容差通道 |
| 单 TU 体积（seq.c 160KB + fx.c 70KB + 全部引擎）编译慢/内存大 | 可接受（一次性成本）；`-O2` 与上游一致，不为编译速度降优化 |
| SMP_DATA rodata 体积（含 CC0 全集时） | D4 已定义降级路径（FATFS）；P2-5 实测体积后定 |
| `mix_block` 实时性未知 | P2-6 基线测量先行；单核 360MHz vs FM-1 120MHz 有 3× 余量预期（不预支结论） |
| 上游回归套件含 FM-1 专属用例 | §6.1 只搬子集，清单即范围 |

## 8. 留给实施者

1. `tests/regress.c` 的用例清单与哈希调用约定在 Task 2 精读该文件后固化进 `ENG SELFTEST`（机制已定为同源复用，无需预写细节）。
2. CC0 资产体积实测与获取（P2-5）；D2 形式 ADR（阶段 3）。
