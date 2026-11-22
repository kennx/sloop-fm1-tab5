# ADR-010: 音序与编排接入 — 单 TU arranger、PER3 子集与 SONG 容量预算

> 状态：已接受（阶段 4 P4-C Task 2 编译骨架 + Task 3 原始步/参数锁 API + Task 4 span/note/pattern 工具与 undo + Task 5 状态命令/详细投影 + Task 6 实时录音/16 lane 鼓输入/物理 PLAY 与原子音高删除 + Task 7 PER3 歌曲持久化 + Task 8 sections/quick chain/SONG 公共命令与状态层；后续任务实现 SONG 页面）｜ 日期：2026-11-22
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
  seq_tab5.c（原始步 / micro / fill / lock 公共 adapter）
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
  没有真实写入者」的豁免在 P4-C 到此终止——**Task 7 已落地**（ADR-009 §3）。

**落地（Task 7）**：字节层在 `firmware/components/sloop_core/src/song_persist.h`（纯函数，由
`scripts/persist-host-test.c` 同源编译：真实 `storage.c` + 8KB 假 flash），flash/锁/时间戳在
`store_tab5.c` 的「PER3 歌曲与延迟写」小节；公共入口 `sloop_store_song_{get,set,save,reload,info,dirty}`
与 `sloop_store_sections_tick(now_ms)`。ADR-009 §6.1 是落地表（canonical 1,552B、默认 80B side、
合法长度集合、延迟写门限、dirty 记账、SELFTEST 重载）。`STORE SONG` 是设备诊断面。

**落地（Task 8）**：sections A–D 仍走 FUN6 对象（`OBJ_PROJECT0 + slot`），但写路径由本节 §10 的
`section_store`/延迟写接管：捕获在 `sloop_seq_section(store)` 的引擎锁内，flash 由**同一** 1s 任务
（`sloop_store_sections_tick`）在停止 + 无发声 + 输入空闲 ≥1.5s + 间隔 ≥5s 时写；section 与歌曲在同一拍
里 flush，失败保留 dirty 按 5s 窗口重试。`project_erase` 同步 `arr_drop_scene/live_sec/song_dirty`。

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

### 6. 原始步与参数锁只经同 TU 薄 adapter 公开

Task 3 新增 `sloop_seq.h` 与 `seq_tab5.c`，后者在 `store_tab5.c` 之后编入同一 TU。公共边界只暴露
`sloop_dstep_t`、`sloop_lock_t`、POD 参数和 `esp_err_t`，不暴露 `track_t` 或上游 static 函数：

- `sloop_seq_read_dstep/write_dstep` 原样读写鼓步的 16 lane `on/lvl/rat` 打包字节；
  `sloop_seq_set_micro` 接受 -32..31；`sloop_seq_set_fill` 接受 0..3，其中 3 由上游
  `step_plays()` 按 normal 处理。
- `sloop_seq_lock_set/del/list` 直接调用上游 `lock_set/lock_del` 或枚举其 `plock_t` 存储。
  每轨 24 槽，同键覆写不占新槽，第 25 项返回 `ESP_ERR_NO_MEM`；不可锁参数（含 P_SLEN）和
  越界输入返回 `ESP_ERR_INVALID_ARG`；`lock_del(param=P_COUNT)` 清除该步全部锁。
- 所有公共写入口先检查 `s_frozen`，再在 `s_engine_lock` 内复核并写入；host 的空
  `fm1_irq_off/on` 不是并发保护。void 读接口在越界或冻结时返回全零，`lock_list` 返回 0。
- `sloop_core_snapshot_t.step[4][16]` 只追加 `lvl/rat/micro/fill` 字段；既有 UI/state 的字段名和
  访问方式不变，窗口填充在既有引擎锁内完成，不增加取锁次数。
- console 新增 `SEQ DSTEP [step]`、`SEQ MICRO/FILL <track> <step> <v>` 与
  `SEQ LOCK LIST|SET|DEL …`。`ESP_ERR_NO_MEM` 映射为 `SEQ ERR full`，非法/不可锁参数映射为
  `SEQ ERR invalid`，冻结映射为 `SEQ ERR frozen`。
- `scripts/seq-test.sh` 编译同源 `hostsim.c + seq_tab5.c`，只以计数桩替代 FreeRTOS 互斥；
  测试必须调用真实上游打包和锁函数，不维护另一份锁/打包实现。

### 7. 上游 span/note/pattern 动作与有限 undo

Task 4 继续扩展同一 `seq_tab5.c`，不编入上游 UI，也不建立第二份步/锁引擎。`ui.c` 的
`note_len/note_set_len/note_span/note_span_set/note_del/note_shift` 与 `ui_layers.c` 的
`pattern_rotate/length/transpose` 只移植纯数据动作；IRQ/UI 消息由既有引擎锁和后续状态投影替代。

- 新公共入口为 `sloop_seq_step_edit()` 与 `sloop_seq_pattern()`。两者都先查诊断冻结，取得一次
  `s_engine_lock` 后复核，并在同一把锁内完成动作。adapter 自有 `step_sess` 在每个公共动作前后归零，
  因而一次 API/console 命令对应一个 undo session，不依赖 240×240 `ui.step_sess` 的生命周期。
- `SPAN.value` 是目标长度；`NOTE_MOVE/COPY.target` 是目标起点。move 通过重复上游单步 shift 到达目标；
  copy 先落在原音跨度之后，再按同一 shift 语义移动副本。复杂 CONT 跨度未先缩回普通 TIE 跨度或目标被
  占用时，move/copy 返回 `ESP_ERR_NOT_ALLOWED`（console `SEQ ERR blocked`）。note move/copy 携带
  micro/fill，并重定位/复制锁；copy 在任何 step/micro/fill/lock 写入前计算「目标已有同键锁除外」的
  需求与空槽数，容量不足返回 `ESP_ERR_NO_MEM` 且整轨不变。`ESP_ERR_INVALID_STATE` 只表示冻结。
- `LVL/RAT` 写选中音符的原始 2-bit 字段，`SLIDE` 接受 0/1；`STEP_CLEAR` 删除该步音符（合成轨经
  `note_del` 清净 CONT）并清 lock/micro/fill。`PATTERN_CLEAR` 调上游 `steps_clear()`，不改变 LEN。
- rotate 和 length 只移动/复制 `step[]`，micro/fill/lock 锚定绝对步；transpose 对全部 64 步逐音饱和到
  0/127，鼓轨返回 `ESP_ERR_INVALID_ARG`。undo/redo 直接交换上游 `undo` 中的 64 步、micro 和 LEN，
  不恢复 fill/lock；没有有效方向时返回 `ESP_ERR_NOT_FOUND`。
- brief 的 `sloop_step_edit_t` 原有字段无法承载 `sloop_step_op_t`，因此实际 POD 在原五字段末尾追加
  `op`。这是让既定函数签名可分发动作所必需的接口修正，后续 `CMD_STEP_EDIT` 必须透传该字段。
  两个公共入口都以有符号转换显式拒绝负 op，再检查上界，不依赖编译器选择 enum 的底层类型。
- console 新增 `SEQ EDIT <SPAN|DEL|MOVE|COPY|LVL|RAT|SLIDE|CLEAR> …` 与
  `SEQ PATTERN <ROTATE|LENGTH|TRANSPOSE|CLEAR|UNDO|REDO> …`；与公共 API 共用错误返回。

`scripts/seq-host-test.c` 继续使用真实 hostsim/seq.c/drums.c，增加复杂 CONT、move/copy 元数据、工具锚定、
0/127 饱和、clear 与 undo 范围、一次取锁及取锁后冻结判别用例。

### 8. 状态层命令、详细投影与焦点同步

Task 5 将 spec §4.3 的命令面接入 `sloop_state`：`CMD_STEP_EDIT` 同步透传包含 `op` 的完整
`sloop_step_edit_t`，`CMD_PATTERN{a=track,b=op,v=(uint16_t)arg}` 还原有符号 arg；
`CMD_SET_MICRO/FILL` 携带 `a=track,b=step,v=value`；`CMD_LOCK_SET/DEL` 使用
`a=track,b=step,n=param,v=value`；`CMD_DRUM_STEP_WRITE{b=step,p=sloop_dstep_t}` 只写原始鼓轨。
POD 指针与既有参数批量命令一样，只需在 `state_apply()` 返回前有效。状态层只校验公共边界和分发，
不复制上游 span、CONT、锁容量或钳位算法。

`app_state_t.seq_status` 分别投影 `IDLE/OK/NOT_FOUND/INVALID/FULL/BLOCKED/FROZEN/ERROR`。
其中 `ESP_ERR_NO_MEM → FULL`、`ESP_ERR_NOT_ALLOWED → BLOCKED`、`ESP_ERR_INVALID_STATE → FROZEN`，
UI 无需把三种失败混成同一个错误。成功的音序写命令在同一 `state_apply` 内重取窗口；合成步的
`lvl/rat/micro/fill` 与鼓步聚合随之更新，失败不改投影。旧 `CMD_TOGGLE_STEP` 的关路径与
`CMD_ERASE_NOTE` 分别改走 `SEQ_STEP_CLEAR` 和 `SEQ_STEP_NOTE_DEL`，因此清步同时清 lock/micro/fill，
复杂 CONT 删除不再由状态层的 MVP 循环模拟。

DI-24 按「选中轨变化才同步」收口：`state_init` 以引擎选中轨初始化焦点，`state_sync_from_engine`、
存储成功后的 `state_refresh_locked`、`CMD_SET_WINDOW` 与音序成功刷新在 `selected_track` 相对上一份投影发生变化时同步
`focus_track`（包括窗口刷新消费外部选轨变化的情形）。显式 `CMD_SELECT_TRACK/SET_TRACK` 已同时设置两者；选中轨不变时，键盘独立移动焦点不被
周期投影覆盖。失败 LOAD 不刷新也不改焦点。`STATE SNAP` 在原有行尾追加 `seq_status`；详细 `synth step/dstep` 行只在 `STATE SNAP FULL` 打印，默认保持既有响应体量。`ENG STEP` 原有完整字段与所有既有 token 保持不变。`sloop_seq_frozen()` 让 toggle/erase 在被冻结的 void 读写入口之前先给出 FROZEN，而不是 OK/NOT_FOUND。

### 9. 实时录音、16 lane 鼓输入与物理 PLAY 分叉

Task 6 把 seq.c 已有的录音/鼓演奏状态机接到 Tab5，不复制其算法：

- **录音快照**（`sloop_rec_snapshot_t`）只投影上游运行态：`ft_on → FREE_TAKE`、
  `ci_on → COUNT_IN`、`rec_wait → ARMED`、`song.rec → RECORDING`（轨道取最低置位，
  与 `rec_follow` 的单 arm 不变量一致）。`quant_off/tempo_mode/count_mode` 是运行期
  配置，本阶段不写 settings（P4-E 完整面板设置）。
- **一次性结果**：`rec_go` 与 `ft_bars` 由快照读取消费（读后清零），不在绘制函数里清。
  `take_bars` 语义：0 无、0xFF 丢弃、其余为关闭的小节数。`rec_go` 没有独立投影字段
  （ARMED→RECORDING 的迁移已表达「刚开始录音」），消费它只是避免把一次性标志留给
  之后无关的读者。状态层只在 `state_sync_from_engine` 写回成功的那一拍读一次（重试
  循环里读会把 take 结果丢在第一次 attempt），命令 `CMD_REC_TOGGLE/CONFIG` 之后同拍
  补读一次。副作用：console/注入通常晚于下一次 30ms sync，读不到 `take=` 令牌；
  设备断言应看 take 的持久效果（LEN/BPM/步内容），见 `docs/deferred-issues.md` DI-27。
- **动作**：`CMD_REC_TOGGLE` = 上游 `ui_studio.c rec_toggle` 的状态转移（停止态
  arm；走带中 `rec_begin`；再按关录音不停播放；自由录音中 `transport_req=1` 关闭）。
  与上游相同地先吞掉 `ft_owns_press()` 为真的按键：自由录音关闭/丢弃后的 **300ms**
  内 REC 不 arm、不改 rec 位（`fm1_ms` 由渲染推进，见下）。`rec_follow` 由
  `sloop_core_select` 在已持引擎锁时调用 `seq_rec_follow_locked`（互斥锁不可递归
  获取；适配函数因此不取锁）。`CMD_REC_CONFIG` 只写 `rec_*`。
- **fill 两个独立变量**：`CMD_FILL_CONTROL{a=held,b=next_bar}` 镜像上游
  `fill_held`/`fill_arm`——`b=0` 只写 held（FILL 保持键的 down/up），`b=1` 是 NEXT BAR
  的**脉冲**，只翻转 arm、不读不写 held。否则按住 FILL 时点 NEXT 会把保持电平锁到
  STOP（Task 6 评审 Critical 1）。下一小节 fill 只持续一小节，随后自动落回 normal。
- **物理 PLAY 与 API PLAY 分叉**：space 是面板按钮路径——`CMD_PLAY_BUTTON` →
  `sloop_seq_play_button(down)`；自由录音中按下 = 置上游 PLAY 按钮位，`ft_block()`
  上升沿 **drop**（工程不变，`ft_bars=0xFF`）。API/编辑命令的 PLAY 仍是
  `sloop_core_play`（`transport_req=1`），对自由录音是 **close**。松开沿只清
  `fm1_in.buttons` 的单 volatile 位、**不取引擎锁**（诊断冻结时也能清干净）。该位是
  Tab5 自定的 bit 0：本固件不编上游 panel/main，seq.c 只用按钮掩码与全零的
  `ly_bit/dyn_bit` 读 buttons，故不与层键别名；接入面板驱动时须改用
  `panel.btn[B_PLAY]`。上游 `fm1_ms` 由 FM-1 的 1ms ISR 推进；Tab5 没有该定时器，
  `sloop_core_render` 在每次渲染回调开头用 `esp_timer_get_time()` 校正 `fm1_ms`，
  否则 `ft_owns_press()` 的 300ms 窗口永不失效。
- **16 lane 鼓**：`CMD_DRUM_HIT{a=lane,v=velocity}` → `sloop_seq_drum_hit` →
  上游 `drum_input()/rec_hit()`（实时录音与自由录音同一路径）；velocity 0 = 松开
  （上游 `KS_DRUM` key-up，结束自由录音的鼓音符段）。键位表在
  `sloop_ui/src/input_map.h`（host 单测），选中鼓轨时其它演奏键忽略、**不回退
  track 0**；**按键释放永远走按下时记录的轨号**（`input_map_key_route` 对 up 一律回
  NOTE），切换选轨不会把 note-off 吞进鼓分支。成功不改 `seq_status`（演奏不是一次
  编辑），越界 → invalid 且不调引擎。
- **音高删除原子化**（Task 5 评审 deferral）：`CMD_ERASE_NOTE` 不再「状态层预读步 →
  另取锁删」，改由 `sloop_seq_note_del` 在**同一把引擎锁**内按音高找 index 并调上游
  `note_del`。实时录音并发改步因此不会让预读的 index 失效。测试以引擎锁计数 = 1
  与该步唯一被删音为判据（`seq-host-test`），状态层以「不读步、只调一次 adapter」
  为判据（`state-test`）。
- **arrangement 交互**已由 Task 8 接上（§10）：SONG 开始时 `seq_start` 清普通 REC（上游
  `song.rec = 0`）；`rec_toggle`/物理 PLAY 的「STOP THE SONG FIRST」「EMPTY SECTION」守卫在
  SONG 入口（`sloop_seq_song_rec`/`sloop_seq_section_request`/`sloop_seq_chain_*`）给出，
  返回 `ESP_ERR_NOT_ALLOWED`（console/状态层 = blocked）。

### 10. sections / quick chain / SONG 公共命令与状态层（Task 8）

Task 8 把 `seq.c`（FELUCCA_ARRANGER 段）的 live sections、quick chain、SONG 模式与 SONG REC
运行态包成公共 POD/`esp_err_t` 入口；`sloop_store_*` 只承担 RAM 槽/歌曲的 flash 与延迟写。
两条纪律不变：**播放中的场景切换绝不用 `sloop_store_project_load`**（只排队 `live_req`，
由渲染块边界的 `arrangement_apply` 生效）；**捕获在引擎锁内、擦写在引擎锁外**。

- **公共 API**（`include/sloop_seq.h`）：`sloop_seq_song_read`（只读快照）、
  `sloop_seq_section(slot, store, erase)`、`sloop_seq_section_request(slot)`、
  `sloop_seq_chain_begin/commit`、`sloop_seq_song_mode/rec`。快照含
  enabled/running/index/bar/ready(bitmask)/live/dirty/chain(count/index/bars/scenes)/
  srec/srec_done + 16 项 `sloop_song_config_t`；`ready` 与 `arrangement_ready()` 同源
  （`proj_ok` 位掩码），UI 不再读上游全局。`index` 在快照里恒夹到 `0..count-1`
  （上游 loop=0 自然结束时停在 count；count==16 的消费者不能拿它索引 `entry[]`）。
  **存储未就绪时快照全零且不取引擎锁**：此时 `proj_slot` 是 NULL/垃圾 PSRAM 指针，
  `arrangement_ready()` 会解引用它。
- **section**：`store=true` 在引擎锁内 `proj_capture(&proj_slot[s])` + `live_sec = s`，
  **播放中也允许**（上游 `section_store` 的 IRQ off 段），随后只置 section dirty；
  `erase=true` / 两者皆 false（load）只在停止态（adapter 回 `ESP_ERR_NOT_ALLOWED`，
  console/状态层 = blocked）。load 把 **RAM 槽**应用到引擎（不回落 flash、不写存储）；
  erase 写空对象、清 RAM 槽/dirty 位/`live_sec`，并 `arr_drop_scene`（有项被删 →
  `song_pending_mark`；一段不剩回默认歌曲）。空槽 = `ESP_ERR_NOT_FOUND` 且不写 flash。
  **存储未就绪**时 store/load/erase 都回 `ESP_ERR_INVALID_STATE`（console `no_storage`），
  绝不 touch `proj_slot`；erase 若在擦写期间遇到新的 capture（section gen 变化）则保留
  新捕获与 dirty 位，由下一个 tick 重写 flash（不静默丢并发写入）。
- **request**：只接受「live loop 播放中且 SONG 未播放」；空槽 = not_found；存储未就绪
  = invalid state（不碰 `proj_slot`）；排队
  `live_req` 并 `chain_n = 0`（上游第一次 section tap 同时取消 chain）。下一个小节
  由 `live_block` 调 `arrangement_apply` + `seq_reset_tracks`。
- **quick chain**：`begin(first)` 立即排队 `live_req=first` 并清旧 chain；`commit(scenes,
  1..8)` 先整份校验（scene < 4 且 ready 且存储就绪），再原子写
  `chain_sec/chain_i/chain_bars/chain_n`（`chain_bars = section_bars(first)`）。**begin 不是
  commit 的前提**：首项既没在播放、也没有相同的 pending 请求时，commit 自己请求
  `scenes[0]`（standalone COMMIT 不会从 entry 1 开始）；首项已在播放则不重复请求。
  `count 0` = **cancel**（清 chain、保留已排队请求）；`count==1` 是合法的单 section
  循环 chain（console 的单 scene 参数只做 begin = 上游单 tap 语义）。`SEQ CHAIN <s>` 只做
  begin = 单 tap 取消；第 9 项/空 scene 整份拒绝，不安装半条 chain。
- **SONG**：`song_mode` 只改 `arrangement_enabled`；SONG REC 进行中拒绝（REC IS ON）；
  存储未就绪时**启**用拒绝（invalid state：PLAY 的 `arrangement_start` 会读 `proj_slot`）。
  `song_rec` 未录音时强制 loop 模式、`srec=1`（下一个小节起录；`live_sec<0` 则等选定
  section），录音中/armed 时 = `srec_stop()` 收尾（空录制 `srec_done=0xFF`，不覆盖原歌）。
  SONG 正在播放（`arrangement_clock.running`）时拒绝（STOP FIRST）。
- **SONG REC 结果 → dirty**：`srec_done` 在适配层按代次对账（`s_srec_marked`）——一次 take
  结束（含 STOP 收尾）后置 `song_dirty`，延迟写才把新顺序落盘；每 30ms 的 sync 不会重复
  mark 造成 dirty 永远清不掉。启动新 take 时**先对账上一轮结果**再清 `srec_done`
  （`SEQ SREC last=none`）——否则两次命令间没跑 30ms sync 时会丢掉上一轮的歌曲顺序。
- **延迟写（ADR-009 §6.1）**：`sloop_store_sections_tick(now_ms)` 现在同时处理 section
  与歌曲 dirty：同一停止/无发声/idle/gap 门限。section 先写（上游 `sections_write` 顺序）：
  持存储锁 + 引擎锁把 `proj_slot[i]` 拷进暂存 → **松开引擎锁** `st_save` → 成功清 dirty、
  失败复位 dirty（下个 5s 窗口重试）。随后 `song_write_locked()` 走 Task 7 的 PER3 组装与
  代次清 dirty。`project_save` **不**清 section dirty 位（capture 本身不清 dirty；只有
  一次成功 flush 才清——擦写期间的新捕获不能被旧 SAVE 清掉）；`project_erase` 清位并
  同步 `live_sec`/`arr_drop_scene`/`song_dirty`（同样受 section gen 保护）。
- **状态层**：`app_state_t.song`（快照）+ `CMD_SECTION_STORE/LOAD/ERASE/REQUEST`、
  `CMD_CHAIN_BEGIN/COMMIT/CANCEL`、`CMD_SONG_MODE/REC/SET`。`CMD_SONG_SET` 只把
  `sloop_song_config_t` 透传给 `sloop_store_song_set`（改内存 + dirty；播放中禁用属于
  SONG 页的输入守卫，Task 11）。返回码写 `seq_status`：`NOT_ALLOWED → BLOCKED`；
  `INVALID_STATE` 且 `seq_frozen` → FROZEN，其余（存储未就绪）→ ERROR；成功同拍刷新
  song 快照（section LOAD/ERASE 另整份刷新引擎投影）。
- **console**：`SEQ SECTION [<A-D|0..3> STORE|LOAD|ERASE|REQUEST]`、`SEQ CHAIN [<A-D|0..3>…]`、
  `SEQ SONG [MODE song|loop|REC]`、`SEQ SREC [TOGGLE]`；无参分别打印
  ready/live/dirty/index/bar、chain(count/index/bars + 逐项 scene)、SONG 全量配置、
  srec 与最近结果（`last=none|no_song|N`）。错误令牌：blocked/frozen/invalid/not_found；
  存储未就绪的 INVALID_STATE 回 `no_storage`。`SEQ REC` 行尾追加
  `fill_held=/fill_arm=`（Task 6 deferral 的设备观测口）。
- **host 门禁**：`scripts/seq-host-test.c` 与上游 `song_audio_test` 同序编入
  `FELUCCA_ARRANGER` + `project.c(PROJ_HOST)` + `arranger_scene.c`，断言 A2+B3 loop=0
  自然结束与 loop=1 STOP 都恢复原 loop hash/sel、后续缺 scene 整首拒播、live STORE
  播放中立即生效且只置 dirty、load/erase/arr_drop、request 下一小节、chain A/B/A 的
  `section_bars` 时长与 8 项上限/单 tap 取消、SONG REC 从下一小节/64 bars 拆分/16 项完成/
  空录制 NO SONG；另覆盖 **存储未就绪（proj_slot 置 NULL，SIGSEGV 守卫）** 的快照全零与
  store/load/erase/request/chain/song_mode 拒绝、**standalone COMMIT 自行请求首项**、
  **index 夹到 count-1**、**新 take 前对账旧结果**。`scripts/state-test.sh` 覆盖命令透传、
  边界与 `seq_status` 映射。
- **设备 smoke**：A/B STORE → dirty 在门限后落盘（hash 非 0）→ 播放中 STORE C 立即
  live=C 且 dirty=1 → request B 下一小节生效 → chain A/B/A 的 index 按 1/2 小节推进 →
  单 tap 取消；SONG A2+B3 在 240 BPM 自然结束且 `LIVEHASH` 回到进入前；缺 D 的歌曲
  拒播；SONG REC 得到 `last=2` 并落盘；硬复位后歌曲 count/entries 与 A/B/C 三个
  section hash 逐字节不变；`SECTION C ERASE` 把歌曲项同步移除；最终镜像复验 live STORE
  与 dirty 落盘。证据：`docs/dev/verify/4-pc-2026-11-22/task-8-device-smoke.log`
  （阶段收口时按 AGENTS §5.4 入库；当前在任务目录暂存）。

## 后果

- 固件获得上游 arranger/seq 的真实状态机与 RAM 场景切换能力，后续 adapter 可在同一 TU
  内安全封装，而不复制调度器或修改 vendored 源码。
- 默认 arrangement mode 仍关闭，普通 loop 与 host golden 不变；map 符号和 quick-chain
  host 用例共同防止「只编了启动文件、漏掉 seq/场景定义」或重复 include。
- PSRAM 增加 3,816B，内部 RAM 只增加一个 `.noinit` 指针及 arranger 小型运行状态；现有
  内部堆与音频门禁不放宽。
- pattern 工具沿用上游单槽 undo；它不是完整工程历史，fill/lock 的撤销缺失是刻意保留的上游边界。
- PER3、SELFTEST 后歌曲恢复与完整扇区测试快照已由 Task 7 落地（ADR-009 §6.1/§3）；公共
  SONG/sections/chain 命令与状态层已由 Task 8 落地（§10），section 与歌曲共用同一 1s
  dirty writer。SONG 页面（计划 Task 11）仍待完成；在此之前，本 ADR 的 UI 数字
  是硬预算，不是已完成的实测声明。
