# ADR-010: 音序与编排接入 — 单 TU arranger、PER3 子集与 SONG 容量预算

> 状态：已接受（阶段 4 P4-C Task 2 编译骨架；后续任务实现公共编辑、持久化与 UI）｜ 日期：2026-11-22
> 关联：P4-C spec `docs/superpowers/specs/2026-11-22-p4c-sequencer-arranger-design.md` §2/§3.1/§3.3/§4；
> ADR-004（vendored 只读）、ADR-005（分组导航）、ADR-007（音频生命周期与锁）、ADR-008（LVGL 池）、ADR-009（存储后端）。

## 背景

`seq.c` 已随 `regress.c → hostsim.c` 编入 `sloop_core`，但固件此前没有定义
`FELUCCA_ARRANGER`，所以 arranger 启动、live sections、quick chain、SONG 与 SONG REC
都不在固件中。上游把这些职责拆在三处：`arranger.c` 负责启动，`arranger.h` 负责纯顺序
时钟，`arranger_scene.c` 负责 RAM 场景应用与工作 loop 暂存，`seq.c` 负责走带状态机。

P4-B 已验证的存储适配使用 `project.c(PROJ_HOST)` 保留 FUN1–FUN6 格式层，并自行实现
flash 存取和 autosave。若为了歌曲直接切到完整 `!PROJ_HOST project.c`，会同时引入
panel/theme/lights/MIDI LEARN 和旧 flash 启动路径，与现有适配重叠并提前实施 P4-E。

## 决策

### 1. 保留 PROJ_HOST 子集，显式控制单 TU 顺序

采用 P4-C spec §3.1 的方案 A：保留 `PROJ_HOST project.c`，只显式补入 arranger 所需源码，
不修改 vendored 文件。顺序固定为：

```text
selftest.c
  FELUCCA_ARRANGER=1
  regress.c → hostsim.c → arranger.c → seq.c
  FELUCCA_ARRANGER undef
  store_tab5.c
    storage.c
    PROJ_HOST=1 → project.c → PROJ_HOST undef
    arranger_scene.c
    upreset.c / fm6_bank.c / Tab5 adapter
```

- `FELUCCA_ARRANGER` 只包住 `regress.c`，在 `store_tab5.c` 之前撤销。这样 `seq.c` 能看到
  arranger，而 `project.c(PROJ_HOST)` 不会误入 `!PROJ_HOST` 的 settings/sections/旧 flash
  路径。
- `arranger.c` 必须先于 `seq.c`：后者消费前者定义的
  `arrangement/arrangement_clock/arrangement_enabled` 与 `arrangement_start()`。
- `arranger_scene.c` 必须在 `project.c` 之后：它消费 `proj_slot/proj_apply/proj_capture`，
  并向先编入的 `seq.c` 提供 `arrangement_ready/arrangement_apply/section_bars/
  song_backup/song_restore` 的单 TU 内部定义。
- `arrangement_start()` 在 include 前声明为 `noinline`。O3 会把这个单次调用的 static
  函数内联进 `seq_start()`，使编译骨架失去 map 判别口；该属性只保留真实函数体，不改
  状态机或全局优化选项。固件 map 必须各有一份 `arrangement_start` 与
  `arrangement_apply`，重复或缺失都视为接入错误。
- host `seq2_test` 自身以 `FELUCCA_ARRANGER=1` 编译，`t_chain()` 的四条 quick-chain
  断言继续作为 host 门禁；`arranger_test` 与 `song_audio_test` 覆盖纯时钟和真实场景切换。

本任务只建立运行骨架。`arrangement_enabled` 保持 static 零初始化，默认 **0**；普通
loop 的 `seq_start()` 路径不变。本任务不新增 UI、公共 SONG 命令或 settings 写入。

### 2. 锁、音频块边界与时钟

- Tab5 不以 hostsim 空实现的 `fm1_irq_off/on` 充当并发保护。后续公共 arranger 写入口与
  引擎/渲染共用 `s_engine_lock`；存储操作仍遵守 ADR-009 的「存储锁 → 引擎锁，擦写时
  不持引擎锁」顺序，互斥锁不可递归获取。
- `arrangement_apply()` 只在音频块边界调用：释放旧话音、ARP/roll 后以
  `proj_apply(scene, all=0)` 替换轨道内容；BPM 和普通全局 FX 不随 scene 改变。该路径
  不擦写 flash、不分配内存、不操作 LVGL。
- quick chain 用 `section_bars()` 从 RAM 槽的最长 pattern 计算时长；SONG 播放由
  `arrangement_clock` 在渲染块前后推进。停止或自然结束通过 `song_restore()` 恢复进入 SONG
  前的工作 loop。
- sections 与歌曲 dirty writer 使用 `esp_timer_get_time()` 的毫秒时钟，不依赖本固件没有
  周期推进的上游 `fm1_ms`。停止、无发声、输入 idle 与存储锁门限由后续存储任务实现。
- `ENG SELFTEST` 会恢复本 TU 的 `.data/.bss`。歌曲配置与 dirty/运行态的显式恢复属于
  P4-C 持久化任务；本任务先保证 `song_keep` 指针不被回滚，不允许自检后形成半旧半新的
  存储视图。

### 3. `song_keep` 只留指针在内部 RAM

上游 `arranger_scene.c` 的 `song_keep` 是 3,816B `.pool` 实体。Tab5 按 P4-B 的声明宏
重定向方法把它改成 `(*s_song_keep_ps)`：

- 实体由 `sloop_store_init()` 使用 `heap_caps_malloc(sizeof *s_song_keep_ps,
  MALLOC_CAP_SPIRAM)` 分配，并纳入 `sloop_store_ready()` 的就绪判据；渲染路径没有
  malloc/free。
- `s_song_keep_ps` 是 4B `.noinit` 指针。原声明的 `.pool` 属性只在包住
  `arranger_scene.c` 的宏作用域内去掉，再由后置声明把指针定位到 `.noinit`；作用域结束
  立即恢复 `__attribute__` 并撤销 `song_keep` 宏。
- 因此内部 DRAM 不承载 3,816B 工作 loop 副本，自检 restore 也不会把指针清成 NULL。
  `linker.lf` 无需改动，现有 `.pool/.lowram` 与 SELFTEST 窗口布局保持原样。

### 4. PER3 是歌曲顺序的唯一持久化格式

P4-C 后续设置任务在 ADR-009 的 `OBJ_SETTINGS` 上实现最小 PER3 读写，不新建 SONG 对象，
不把歌曲顺序塞入 FUN6/autosave：

- `arr_config_t` 为 36B、16 项、4 scene；主 payload 偏移 48，entries 从 52 开始。A–D
  内容仍使用 FUN6 工程对象。quick chain、SONG REC 临时态和 `arrangement_enabled` 不持久化。
- 无 settings 时，首次歌曲保存构造上游 canonical PER3：88B `persist_t` + 1,464B 默认
  DSU bank = 1,552B，并生成 80B 默认 side record；合法但尚无 DSU 的 PER3 只补默认 DSU，
  已有尾段不重写。
- 合法 PER3（含历史长度与 MIDI 尾段）保留整个 payload，只替换 arrangement 36B；side
  record 随新 A/B 副本原样提交。PER1/PER2 按上游 `persist_boot` 口径迁移；未知 magic
  或损坏对象拒绝保存，不当作空设置覆盖。
- 写失败保留 `song_dirty`，后续停止/安静/idle 窗口重试。这是可靠性修正，不改变成功
  路径格式。
- 在第一次真实 settings 写入之前，`STORE TEST` 必须扩为两个完整 4KB 扇区的快照、逐字节
  恢复与校验，覆盖 payload、未知尾段和 side record。P4-B「只保存 payload、settings
  没有真实写入者」的豁免在 P4-C 到此终止。

本任务不实现上述写入；它只固定格式与后续任务边界。

### 5. SONG 独立顶层组，按 12 页有序对核算池预算

`APP_STATE_PAGES` 在 UI 任务中由 11 增至 12，新增 `APP_PAGE_SONG`。SONG **不加入 SEQ**，
而使用独立组，组顺序为 `PLAY/TRACKS/EDIT/SEQ/SONG/FILE`；这样不增加 SEQ 稳态占用，
也不改变 FILE→STEP 的既有最坏有序对。六个组签仍复用 72px 两行页签条，16pt 字体和
64KB LVGL 池不变。

P4-C 规划上限：TRANSPORT 9,216B，SONG 12,288B；SONG 页落地后必须以 `UI MEM` 实测收紧，
不能直接抬 est 或扩池。按 base 13,800B、切换余量 4,096B、TLSF 可用 63,120B 计算：

| 有序对（源组全建 → 目标组首建） | 页面 est 对值 | 总账面 | 余量 |
|---|---:|---:|---:|
| FILE → STEP | `20,992 + 23,040 = 44,032B` | **61,928B** | 1,192B |
| SEQ（STEP 23,040 + TRANSPORT 9,216）→ PRESETS | `32,256 + 12,800 = 45,056B` | **62,952B** | 168B |
| SONG → PRESETS | `12,288 + 12,800 = 25,088B` | **42,984B** | 20,136B |

三组均满足 `base + 对值 + 4,096 ≤ 63,120`。第二个对值成为新的账面最坏情况，实施时
必须先量六个组签对 base 的影响，再验证 `free ≥ est`、过渡后 `pool_free ≥ 4,096` 和组
往返无泄漏；任何超预算先减少对象，不扩大池。

`TAB` 的规划顺序为：
`PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT→SONG→PROJECTS→PRESETS→PLAY`；
`UI TAB song` 与物理组签必须共用同一动作函数。本任务不注册页面，不改变当前 11 页行为。

## 后果

- 固件获得上游 arranger/seq 的真实状态机与 RAM 场景切换能力，后续 adapter 可在同一 TU
  内安全封装，而不复制调度器或修改 vendored 源码。
- 默认 arrangement mode 仍关闭，普通 loop 与 host golden 不变；map 符号和 quick-chain
  host 用例共同防止「只编了启动文件、漏掉 seq/场景定义」或重复 include。
- PSRAM 增加 3,816B，内部 RAM 只增加一个 `.noinit` 指针及 arranger 小型运行状态；现有
  内部堆与音频门禁不放宽。
- PER3、SELFTEST 后歌曲恢复、完整扇区测试快照、公共 SONG 命令与 SONG 页面分别由后续
  P4-C 任务完成；在这些入口落地前，本 ADR 的 UI 数字是硬预算，不是已完成的实测声明。
