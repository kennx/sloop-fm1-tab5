# 阶段 3：独立模式 MVP 设计规格（Phase-3 Spec）

> 日期：2026-11-20 ｜ 状态：草案（待项目负责人评审）
> 上位文档：`docs/superpowers/specs/2026-11-19-sloop-tab5-port-design.md`（总体设计 §5.2 双模式状态抽象）；`docs/design/002-port-roadmap.md` §5（任务与完成判据，权威）；阶段 2 spec（引擎 API/DI-8 现状）。
> 前置输入：阶段 2 终审结论——引擎 API 只写不读、音频流单向门（DI-8）、内部 DRAM 余量（min_free_internal≈86KB）。

## 1. 目标与非目标

**目标**（roadmap §5）：最小但完整的一圈——触摸/键盘输入 → 状态模型 → 引擎 → 出声，UI 呈现 MVP 四屏（PLAY/TRACKS/STEP/TRANSPORT），串口注入使 UI 状态可脚本复现。

**非目标**：全部 SLOOP 页面与特性（阶段 4）；联动模式（阶段 5）；性能优化；FM6/SLICER 深度编辑页。

**反目标**：UI 直接读写引擎全局（绕过状态层 → 撕裂/不可测）；四屏各自维护状态副本；内嵌上游 `ui.c`（240×240 模型不适配触屏）。

## 2. 方案（Approaches）

| 决策 | 选项 | 结论 |
|---|---|---|
| UI 架构（D3） | (a) LVGL 原生 1280×720 多屏 + 顶部页签；(b) 单 canvas 自绘；(c) 240×240 放大 | **（a）**：LVGL 已有（esp_lvgl_port 2.9.0 + LVGL 9.6），触屏控件/主题成熟；缩放版模糊且浪费 5″ 屏 |
| 状态层（D2 形式化） | (a) AppState 影子快照 + EditCmd 派发 + 引擎快照同步；(b) UI 每帧加锁直读引擎；(c) 事件溯源全量重渲染 | **（a）**：UI 渲染纯函数化、host 可单测、阶段 5 远程会话可替换执行端 |
| 键盘映射 | (a) DAW 式半音矩阵（Z 排低八度 + Q 排高八度）+ 修饰键作层键；(b) 逐键复刻 FM-1 27 键布局；(c) 键盘只做走带 | **（a）**：电脑键盘无 F 排/音乐键位，半音矩阵是 DAW 通用心智；修饰键天然映射 SLOOP「按住层键」模型 |
| DI-8 音频门 | (a) 实现 `audio_stop` + 开机启流 + SELFTEST 要求先停流；(b) 保持单向门、SELFTEST 仅开发构建；(c) 开机自动跑 SELFTEST 再启流 | **（a）**：成品固件保住诊断门；(b) 让最强正确性门禁在成品中失效；(c) 开机 +316s 不可接受 |

## 3. 组件与接口设计

```
firmware/components/
├── sloop_core/     # 阶段 2 已有；本阶段扩展只读/批量接口
├── sloop_state/    # 新建：AppState + EditCmd（纯 C，无 LVGL，host 可测）
├── sloop_ui/       # 新建：LVGL 四屏 + 页签 + InputManager（消费 tab5_keyboard 与触摸）
└── tab5_keyboard/  # 已有
```

### 3.1 引擎层新增（sloop_core.h 扩展）

```c
/* 只读快照：MVP 屏所需状态一次性拷出（内部加锁）。字段与编辑器协议词表对齐。 */
typedef struct {
    bool playing; uint16_t bpm; uint8_t selected_track;      /* 走带（selected= song.sel） */
    struct { uint8_t engine, preset, level, mute, armed, solo; uint8_t len, div; } trk[4];
    struct {                                                 /* 16 步 MVP 视窗（前 16 列） */
        uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
    } step[4][16];
    struct { uint8_t on; uint8_t lvl; uint8_t rat; } dstep[16];  /* 鼓轨第 4 行聚合 */
    uint8_t step_pos[4];                                     /* 播放头（模 len） */
} sloop_core_snapshot_t;
void sloop_core_read(sloop_core_snapshot_t *out);

/* 步写：读写完整上游 step_t 语义（time/flags/vel 全保留；清步= n=0/time=REST），
 * 状态层做 read-modify-write，绝不把未改字段拍平 */
typedef struct {
    uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
    uint8_t lvl; uint8_t rat;                                /* 每音符 2bit 打包（同协议） */
    int8_t micro; uint8_t fill;                              /* 保留字段（MVP 只回写） */
} sloop_step_t;
void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out);
void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in);

void sloop_core_set_global(uint32_t id, int16_t value);   /* BPM(G_TEMPO) 等 */
```
注：`sloop_core_note()` 维持 `vel=100`；MVP 不做速度层。批量参数接口推迟到阶段 4（MVP 四屏只需 track/preset 切换，走已有 `sloop_core_track`）。

### 3.2 sloop_state（D2 词表）

```c
typedef struct { ... } app_state_t;          /* AppState：引擎投影 + UI 局部态（当前页/焦点/层键集/last_note） */
typedef enum { CMD_NOTE_ON, CMD_NOTE_OFF, CMD_PLAY, CMD_STOP, CMD_SELECT_TRACK,
               CMD_TOGGLE_STEP, CMD_SET_BPM, CMD_SET_TRACK, CMD_MUTE, CMD_SOLO,
               /* …阶段 4 扩展 */ } edit_cmd_type_t;
typedef struct { edit_cmd_type_t type; uint32_t a, b; uint16_t v; } edit_cmd_t;
void state_init(void);
void state_apply(const edit_cmd_t *cmd);     /* 执行端=引擎（阶段 5 换成 FM-1 会话） */
void state_sync_from_engine(void);           /* 拉取快照合并（UI 定时器调用） */
void state_get_copy(app_state_t *out);       /* 互斥复制；UI 只读副本 */
```
**并发规则**：唯一写者=input 任务（物理/注入事件 → `state_apply`）；UI 任务经 `state_get_copy` 读副本渲染；`state_apply` 与 `state_sync_from_engine` 互斥。延迟目标：键按下→出声 ≤20ms（input 任务优先级低于音频、高于 UI）。
**MVP 层语义**（AppState 实现，不依赖上游 ui_layers/panel）：
- EDIT 层：按住时按演奏键 → 从当前聚焦步删除该音（`read_step`→改→`write_step`）。
- GLO 层：按住时键 1–4 → 轨静音切换；5–8 → 独奏切换（`solo` 位）。
- 步网格：空→开 写入 `last_note`（无则 60），n=1/vel=100/time=NOTE/flags=0；开→关 写 n=0/time=REST；均走 read-modify-write 保留 lvl/rat/micro/fill。
- 鼓轨第 4 行：MVP **只读展示**（聚合 on/off），不做编辑（阶段 4 接 DRUM_STEP）。
- 步进写入口径：**网格点按**（不含实时录音 pen；阶段 4 再定）。LEN/DIV：MVP 不改写，快照携带；播放头按 `step % len` 显示，超出 16 列的部分不显示（文档化限制）。

### 3.3 sloop_ui 四屏（视觉规范）

- 主题色：四轨 蓝 `0x3B7BD8` / 绿 `0x3FA34D` / 黄 `0xE0B32A` / 橙 `0xE06A1B`；背景 `0x101418`；顶栏页签 `PLAY | TRACKS | STEP | TRANSPORT` 常驻可点。
- **PLAY**：当前选轨 kit/引擎名 + 大号音符触发指示（按键/触摸弹奏时点亮）+ 音量条。
- **TRACKS**：四轨卡片横排，主题色描边；点选切轨（选中态高亮）；每卡显示 engine/preset/level/mute。
- **STEP**：4 行 ×16 步网格，行=轨（着色），列=步；点格切换有/无音（Cmd_TOGGLE_STEP）；播放头高亮（`step_pos`）；顶栏显示当前轨步长。
- **TRANSPORT**：PLAY/STOP 大按钮（Cmd_PLAY/STOP）、BPM 步进器（Cmd_SET_BPM ±1，长按连发）、当前小节/步位置指示。
- 路由：单 `lv_screen` 容器 + 页签切换页面对象（不销毁重建）；`UI <page>` console 命令切页（注入用）。
- 线程：所有 LVGL 调用在 `bsp_display_lock` 纪律下；UI 定时器（30–60ms）`state_sync_from_engine()` + 按需重绘（脏检查）。

### 3.4 InputManager（P3-1）

统一事件：`{src: KEY|TOUCH|INJECT, code/xy, pressed}` → 层模型（按住层键期间按键走层表）→ EditCmd。
**70 键映射（DAW 式，映射表入 `docs/design/006-keyboard-mapping.md`）**：

| 区域 | 键 | 功能 |
|---|---|---|
| 演奏低八度 | z s x d c v g b h n j m `,` | 白/黑键半音序（DAW 标准） |
| 演奏高八度 | q 2 w 3 e r 5 t 6 y 7 u | 同上 +12 半音 |
| 八度 | `[` `]`（实际键位以驱动键名为准：`(` `)` 或 `<` `>` 行） | OCT− / OCT+ |
| 层修饰（按住） | Sym=FX、Aa=EDIT、Ctrl=SEL、Alt=GLO | 按住期间演奏键映射到层功能（MVP 先实现 EDIT 层=擦除、GLO 层=1–4 静音/5–8 独奏的键位子集） |
| 走带 | space=播放/停止、enter=REC（占位） | Cmd_PLAY/STOP |
| 导航 | ←→↑↓ | 步/轨移动（STEP 页焦点） |
| 页面 | esc=HOME（回 PLAY）、tab=下一页签 | UI 局部命令 |

层键按下集写入 `app_state_t`（UI 显示层提示）；映射表须可注入复现（`KEY <code> <down|up>`）。
**前置硬件核对（DI-4 子集）**：Task 6 第一步在真机验证「修饰键按住 + 字母键」是否作为两个独立事件上报；若键盘固件不报组合，回退方案=层键改为「点按锁定/再按解锁」（映射表相应调整，架构不变）。

### 3.5 串口注入（P3-5）

console 新增：`KEY <code> <down|up>`（等价物理键盘事件，走 InputManager 同一入口）、`TAP <x> <y>`（注入到各屏注册的 hit-test 表 → 同一 EditCmd；诚实边界：物理触摸由 LVGL 控件回调发 EditCmd，注入在 EditCmd 层等价，不是原始指针事件）、`UI <play|tracks|step|transport>`（切页）、`SCR` 保持当前页截图（`SCR <page>` 扩展评估后不做：`UI`+`SCR` 已可脚本化，screenshot.py 不变）。

## 4. 约束

- 内存：内部 DRAM 余量 ≈86KB —— LVGL 缓冲已在 PSRAM（阶段 1 配置），新增 UI 对象/状态层须把 **idle 内部堆 ≥60KB** 作为门禁；soak 期间不得单调下降。
- 时序：UI 定时器与音频任务分核/分优先级（UI 低优先级，不得阻塞 5.8ms 音频块；`UI` 与引擎交互只走短 EditCmd）。
- 引擎只读接口内部加锁；批量参数一次锁内完成。
- DI-8 落地：`sloop_hal_audio_stop()`（挂起并删除任务、codec 保持打开）、`ENG AUDIO ON|OFF` 命令；UI 开机 `ON`；`ENG SELFTEST` 在流式时给出 `hint=ENG AUDIO OFF`（拒绝语义不变）。
- 每 UI 改动过 §5.4：截屏 + `--check` 像素抽检 + 视觉检查（人眼项入清单）；证据晋升 `docs/dev/verify/3-mvp-<日期>/`。

## 5. 验证策略

1. **状态层单测（host）**：`scripts/state-test.sh`（gcc，stub 引擎）覆盖 EditCmd→AppState 转移、批量参数、BPM 边界、步切换、层键集合。
2. **UI 截屏矩阵**：四屏 × 关键状态（选中轨 1/4、步开/关、播放中播放头）→ §5.4；注入脚本序列（`UI`→`KEY`/`TAP`→`SCR`）≥3 条可复现。
3. **端到端**：机器侧——注入演奏音符经引擎出声（`ENG` 路径已有）+ UI 指示联动日志；人测——真实键盘弹奏、触摸四屏、听感（收尾清单）。
4. **稳定性**：30min soak（播放中 + UI 定时器运行）：内部堆水位非单调下降、无 WDT/panic、`ENG PERF` 音频占空比不劣化。
5. 引擎回归：`scripts/host-test.sh` + `ENG SELFTEST`（UI 改不触碰引擎 DSP；selftest 需 `ENG AUDIO OFF`）。

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| LVGL 9.6 与 esp_lvgl_port 的 API 细节（阶段 1 已踩过） | 以 vendored 组件源码为准；UI 任务全走 bsp_display_lock；复用阶段 1 显示初始化 |
| 内部 DRAM 吃紧（LVGL 对象/中文字体） | 不加载 CJK 字体（UI 英文）；缓冲在 PSRAM；门禁 60KB |
| 键盘映射争议 | 映射表文档 + 注入测试先固化行为；可后续按手感调（不影响架构） |
| 注入与物理事件不等价 | 同一 InputManager 入口；注入测试即回归 |
| 引擎快照频率导致的锁竞争 | 快照 O(轨×16 步) ≈ 数百字节拷贝；30–60ms 一次；音频块 5.8ms 内锁窗口微秒级 |

## 7. 留给实施者

- 映射表具体键名以 `tab5_keyboard.c` 的 `k_key_names` 实际键名为准（如 `"` `<` `>` 行），计划 Task 6 落地时逐键核对并写表。
- 字体：用 LVGL 内置 Montserrat 序号（英文）；如需等宽数字步进器，用既有内置字体，不新增资产。
- `SCR <page>` 扩展评估：`UI` 命令已使脚本化可行，本阶段不扩协议（阶段 4 全页面矩阵时再定）。
