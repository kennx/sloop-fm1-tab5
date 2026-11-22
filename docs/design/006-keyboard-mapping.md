# ADR-006: Tab5 Keyboard 70 键映射 — DAW 式半音矩阵 + 修饰层键

> 状态：已接受并落地（阶段 3，P3-1；实现 `components/sloop_ui/src/input.c`）｜ 日期：2026-11-20
> 关联：spec `2026-11-20-phase3-mvp-ui-design.md` §3.4；注入入口 `console` 的 `KEY` 命令

## 背景

Tab5 Keyboard 是 70 键电脑键盘（14×5 矩阵，无 F 排/旋钮）。SLOOP 的操作模型基于「27 键 + 按住层键 + 4 旋钮」。需等价的键盘映射。

## 选项

| 选项 | 评估 |
|---|---|
| **A. DAW 式半音矩阵 + 修饰键作层键（采纳）** | Z 排低八度/Q 排高八度是 DAW 通用心智；Sym/Aa/Ctrl/Alt 天然对应「按住层键」；无需新增硬件 |
| B. 逐键复刻 27 键位置 | 键盘布局与 FM-1 面板不同形，强行对应反直觉 |
| C. 键盘只做走带、演奏靠触摸 | 违反「用实体键盘演奏」的产品目标 |

## 决策

A。InputManager 为表驱动（`{code, layer, action, param}`），查表两遍：先匹配所持层
（同码多条按表层序，**GLO > SEL > EDIT**），没有再落基础层；层内未定义的键（方向/走带/页面）
在任何层下都可用。EDIT/SEL/GLO 按住时演奏键**不发声**（避免 `Aa+c` 擦除与 `Aa+z` 弹音并存
的歧义）；FX 层是 MVP 无功能层，演奏保持透明。

### 键位表（键名/码取自 `tab5_keyboard.c` 的 `k_key_names`；code = row<<4|col）

| 区域 | 键（code） | 功能 |
|---|---|---|
| 低八度演奏 | z 0x42、s 0x33、x 0x43、d 0x34、c 0x44、v 0x45、g 0x36、b 0x46、h 0x37、n 0x47、j 0x38、m 0x48、`.`(句点，键名表印 `"."`) 0x49 | C3..C4（基音 48，note-on/off 发 `CMD_NOTE_ON/OFF`，更新 `last_note`）。合成轨不变；**选中鼓轨时改走 16 lane 鼓表**（见下），不回退 track 0 |
| 高八度演奏 | q 0x21、2 0x02、w 0x22、3 0x03、e 0x23、r 0x24、5 0x05、t 0x25、6 0x06、y 0x26、7 0x07、u 0x27、i 0x28 | C4..C5（基音 +12，i 为半音 24）；鼓轨时 q w e r t y u i 是 lane 8..15 的鼓键，2/3/5/6/7 忽略。**i 0x28 的绑定是 Task 12 补的**：`input_map.h` 的 16 lane 表一直含 0x28，但 `input.c` 的绑定表此前止于 u ⇒ 物理按 i 既不发声也不触发 lane 15（只经 `SEQ HIT 15` 可达） |
| **鼓 lane（选中鼓轨，P4-C Task 6）** | 低行白键 z 0x42、x 0x43、c 0x44、v 0x45、b 0x46、n 0x47、m 0x48、`.` 0x49 → lane 0..7；高行白键 q 0x21、w 0x22、e 0x23、r 0x24、t 0x25、y 0x26、u 0x27、i 0x28 → lane 8..15 | down = `CMD_DRUM_HIT{a=lane,v=100}`（上游 `drum_input()/rec_hit()` 同一路径，实时录音写入鼓步）；up = `CMD_DRUM_HIT{a=lane,v=0}`（velocity 0 = 松开，结束自由录音的鼓音符段，上游 `KS_DRUM` key-up 口径）。其它演奏键在鼓轨**忽略**（不回落合成轨 0）；键表见 `src/input_map.h`，host 单测覆盖 16 lane |
| 层（按住） | Sym 0x30=FX；Aa 0x31=EDIT；Ctrl 0x40=SEL；Alt 0x41=GLO | 层键 down/up 置/清 `layer_held` 位（`APP_LAYER_*`） |
| FX 层 | Sym + 任意键 | MVP 无功能（只置位 + 页签条提示），阶段 4 接 27 垫 |
| EDIT 层 | Aa + a 0x32 / b 0x46 / c 0x44 / d 0x34 / e 0x23 / f 0x35 / g 0x36 | 音名 A/B/C/D/E/F/G：按**音级**擦除聚焦步里的同名音（跨八度；命令 `CMD_ERASE_NOTE` 按精确音符，逐音符发；音高→index 的查找+删除现在由 seq adapter 在**同一把引擎锁**内完成，实时录音并发改步不会让 index 失效） |
| SEL 层 | Ctrl + 1..4（0x01..0x04） | 选轨 0..3（`CMD_SELECT_TRACK`，同时设焦点轨；armed/recording 的 rec 位跟随选中轨，上游 `rec_follow`） |
| GLO 层 | Alt + 1..4 / 5..8（0x01..0x04 / 0x05..0x08） | 静音 / 独奏轨 0..3（`CMD_MUTE`/`CMD_SOLO`；引擎写入口 `sloop_core_set_mute/set_solo`，`STATE SNAP`/`ENG SNAP` 同源） |
| 走带 | space 0x4D | **物理 PLAY 键**（P4-C Task 6）：down/up 两沿发 `CMD_PLAY_BUTTON`，与上游面板按钮同一路径——自由录音（free take）进行中按下 = **drop**（丢弃，工程不变），其它情况请求走带（playing 时 stop）。API PLAY（`CMD_PLAY`/console `SEQ REC BUTTON` 之外的 `ENG`/LIVE 入口）仍走 `sloop_core_play`，对自由录音是 **close**（收成 loop）。两个入口不得混用 |
| REC | `0` 0x0A | `CMD_REC_TOGGLE`（上游 `rec_toggle`）：停止态 arm（首个音符或 PLAY 启动）；走带中立即录选中轨；再按一次关录音**不停播放**；自由录音进行中 = 关闭它（收成 loop，与上游 editor 的 REC 同一 `transport_req=1` 路径；PLAY 才是 drop） |
| FILL 保持 | `-` 0x0B（按住） | down/up 发 `CMD_FILL_CONTROL{a=1/0,b=0}` → 上游 `fill_held`：按住期间 fill 步条件成立（fill-only 步演奏、no-fill 步静音） |
| FILL NEXT | `+` 0x0C（点按） | down 发 `CMD_FILL_CONTROL{a=0,b=1}` → **脉冲**：只翻转上游 `fill_arm`（下一小节 fill），**不读不写 `fill_held`**（按住 `-` 时点 `+` 不会清掉保持电平）；再按一次取消。STOP 清 held/armed |
| ENTER 动作 | enter 0x3D | **STEP 页 = 切换聚焦步的选中 drum lane 位**（`sloop_ui_step_drum_toggle()` → `CMD_DRUM_STEP_WRITE`，P4-C Task 9）；**FILE 页（PROJECTS/PRESETS）= LOAD 选中槽**（`sloop_ui_file_load()` → `CMD_PROJECT_LOAD`/`CMD_PRESET_LOAD`）；**TRANSPORT 页 = 焦点工具字段 +1/切换**（`sloop_ui_transport_adjust(1)`，P4-C Task 10：LVL/RAT 循环、MIC/SPAN ±1、SLIDE 切换、ROT/LEN/TRANS 前进）；**SONG 页 = LOAD 选中 section**（`sloop_ui_song_load()` → `CMD_SECTION_LOAD`，P4-C Task 11）；其它页无动作（不抢演奏键）。FILE 的 SAVE/ERASE 只走触摸按钮与 console（spec §4.5，避免与演奏键冲突） |
| 页面 | esc 0x00=PLAY；tab 0x20=组顺序扁平轮转的下一张 | `sloop_ui_request_page()`：只记目标页，30ms UI 定时器在 LVGL 上下文切页（输入路径不等锁，派发 22–72µs）；顺序 = **最终 12 页**（P4-C Task 11 起 SONG 已注册）PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT→SONG→PROJECTS→PRESETS→PLAY（ADR-005 §4；跳过未注册页） |
| 焦点 | ← 0x4A / → 0x4C：**STEP 等普通页**移焦点步（0..15 环绕）；**FILE 页（PROJECTS/PRESETS）选槽**（`sloop_ui_file_slot_move()`，无选中时 → 落 A、← 落 D，之后环绕）；**TRANSPORT 页**在 8 个工具字段环上移动（`sloop_ui_transport_focus_move()`：LVL,RAT,MIC,SPAN,SLIDE,ROT,LEN,TRANS 环绕）；**SONG 页**在 count 内的 16 个歌曲条目间移动（`sloop_ui_song_entry_move()`，环绕）；↑ 0x3B / ↓ 0x4B：普通页移焦点轨（三条合成轨环绕）；**STEP 页**走焦点环 `T1,T2,T3,DRM L1..L16`（`sloop_ui_step_focus_vertical()`：T3↓→L1、T1↑→L16、L1↑→T3、L16↓→T1，鼓轨上逐 lane）；**TRANSPORT 页**调焦点工具字段（`sloop_ui_transport_adjust(±1)`）；**SONG 页**在 A–D section 卡间移动（`sloop_ui_song_section_move()`，环绕） | `state_set_focus()` / `sloop_ui_step_focus_vertical()` / `sloop_ui_file_slot_move()` / `sloop_ui_transport_focus_move()` / `sloop_ui_transport_adjust()`（参数页另有 ←/→ 选槽、↑/↓ 调值，见 ADR-008 §8） |

八度切换（原计划的 OCT±）**未实现**：`-` 0x0B / `+` 0x0C 已在 P4-C 分配给 FILL 保持 /
FILL NEXT、`0` 0x0A 分配给 REC（见键表）；八度切换仍留给阶段 4 的完整层键位子集
（可换用 `del`/`backspace` 等空闲键）。MVP 演奏音域 C3..B4 共 24 音。

### 录音 / fill / 鼓演奏（P4-C Task 6）

- **REC（`0`）** 只做一次 `CMD_REC_TOGGLE`（= 上游 `rec_toggle`）。上行编辑器/UI 的 REC
  也走同一 API；自由录音（空工程、停止态 arm 后的首个音符）进行中按 REC = 收成 loop。
- **物理 PLAY（space）与应用内 PLAY 分叉**：space 发 `CMD_PLAY_BUTTON`（down/up 两沿），
  与上游面板按钮同一条路径——自由录音中被 `seq.c ft_block()` 的 PLAY 按钮掩码**丢弃**
  （take 不上屏、工程不变）；编辑命令/console/UI 的 PLAY 仍走 `sloop_core_play()`
  （`transport_req=1`），对自由录音是 **close**（量化成 loop 并播放）。两者刻意不共用
  入口，脚本断言也因此能区分。
- **鼓轨不回落**：选中鼓轨时演奏键只按上表的 16 lane 表发 `CMD_DRUM_HIT`，其它演奏键
  忽略；合成轨的 24 音矩阵不变。**释放永远按按下时记录的轨号收尾**（选中鼓轨后松开
  合成键仍发 `CMD_NOTE_OFF` 到原轨，不会挂音）。键盘松开鼓键发 `v=0`（= 上游 key-up，
  用于自由录音的鼓音符段）。注入覆盖 `KEY` 与 console `SEQ HIT`（同一 adapter 函数）。
- **fill**：`-` 按住/松开只写 `fill_held`；`+` 点按只翻转 `fill_arm`（上游两个**独立**
  变量，NEXT 脉冲不碰 held）。下一小节 fill 只持续一小节，随后自动落回 normal；
  STOP 由上游清两者。这些是运行期表演状态，不持久化（REC 的 quant/tempo/count 配置
  同样不写 settings，留给 P4-E 的完整 panel 设置）。



Task 6 Step 1 要求真机按 `Sym+字母`/`Ctrl+Alt+字母`/`Sym+Aa+字母` 记录 `KB` 日志。
**本次会话无人工按压条件**（AI 会话无人手，且 DI-4 全键人测仍挂账），真机 `KB` 计数
`down=0 up=0` 只证明键盘在位（0x6D/FW 0x01）与 INT 空闲。改用**权威源码核对**：

- 键盘内部固件 `user_scan_handle.c`（M5Tab5-Keyboard-Internal-FW，MIT）：`keyboard_process()`
  对 70 个键位**逐键**比较采样/消抖，每次状态变化调 `keyboard_enqueue(i, sample)`，
  事件字节 = bit7 按压 + row/col —— 不存在「修饰键吞事件/合成组合」的逻辑。
- `user_i2c_callback.c`：普通（KEY）模式的 `KEY_EVENT` 寄存器直接从 `event_fifo` 弹出原始事件；
  `special_key` 过滤只存在于 HID/字符模式的下游转换。
- 本项目驱动（阶段 1 已机器验证）逐事件解码 bit7/row/col，不区分修饰键。

**结论/裁决**：采用**按住（hold）语义**，不回退层键锁定。Sym/Aa/Ctrl/Alt 与普通键一样各自
独立 down/up，层状态 = 当前按住的层键位掩码。物理按压的最终确认仍属 DI-4 全键人测
（不阻塞本任务：若实测发现组合缺事件，只改 `input.c` 的层键处理为「点按锁定/再按解锁」，
映射表与架构不变）。注：按住 Aa 在本键盘内部还会改大小写状态（`aa_flag`，只影响 HID/字符
模式），普通模式下对事件流无影响。

### SONG 页（P4-C Task 11）

- ←/→ 在 1..count 个歌曲条目间移动选中项（`sloop_ui_song_entry_move()`，count 内环绕）、
  ↑/↓ 在 A–D section 卡间移动（`sloop_ui_song_section_move()`）、ENTER = LOAD 选中
  section（`sloop_ui_song_load()` → `CMD_SECTION_LOAD`）。选中项是 UI 局部态
  （`page_song.c` 静态字节），控件描边由 30ms UI 定时器落地；SAVE/ERASE/CHAIN/REC
  与 16 项/bars/loop 配置只走触摸 + console（避免为 16 个歌曲项各设专用键，spec §4.4）。

### TRANSPORT 工具区（P4-C Task 10）

- **焦点环**：TRANSPORT 页 ←/→ 在 8 个字段上移动（LVL,RAT,MIC,SPAN,SLIDE,ROT,LEN,TRANS），
  ↑/↓ 调焦点字段，ENTER = +1/切换。字段焦点是 UI 局部态（`page_transport.c` 静态字节，
  跨组重建保留），画成该字段 chip 的 3px 白描边；console `UI TOOL [<0..7>]` 读写同一状态
  （脚本的键盘断言口：`UI TOOL field=<n> name=<lvl|rat|mic|span|slide|rot|len|trans>`）。
- **↑/↓ 的字段语义**：LVL 循环（GHOST<SOFT<NORM<HARD）、RAT 循环（x1..x4）、MIC ±1（-32..31）、
  SPAN ±1（1..LEN，越界回另一端）、SLIDE 切换（两向同义）、ROT ±1、LEN ×2/÷2、TRANS ±1；
  每条都是**一条 EditCmd**（`page_transport.c` 的 `tr_action`，与 TAP/触摸同一个函数）。
- **与 STEP 页的关系**：焦点步（轨×窗口列）在 STEP 页设、在 TRANSPORT 页改字段；鼓轨时
  LVL/RAT 作用于 `UI LANE` 选中 lane 的原始 2-bit 字段（`CMD_DRUM_STEP_WRITE` 整份 POD），
  SPAN/SLIDE 不适用（不发命令）。LVL/RAT 只改聚焦步第 1 个音符（无音符光标，DI-30）。

### 注入与延迟

`KEY <code> <down|up>`（console）与物理键盘同入口 `sloop_ui_input_key()`；code 十进制或
`0x` 十六进制，取值 0..0x7F（tab5_keyboard 事件码）。`KEY` 每条打印 `elapsed_us`（命令入口
→ 派发返回，含 state_apply 与 PLAY 指示），`KEY STATS [RESET]` 打印累计均值/最大。
InputManager 任务：优先级 4（低于音频 5），栈 4KB，`tab5_kb_poll()` 每批 8 条。

**轮询周期 = tick 地板**（阶段 4 P4-0 Task 2）：周期写成 5ms 意图（`INPUT_POLL_MS`），
实际取 `INPUT_POLL_TICKS = max(1 tick, pdMS_TO_TICKS(5))`。本工程 `CONFIG_FREERTOS_HZ=100`
（`firmware/sdkconfig`）→ `pdMS_TO_TICKS(5) == 0`，因此**生效周期 = 1 tick = 10ms**；
启动日志按实际值打印 `轮询 10ms（1 tick @100Hz）`。零 tick 的 `vTaskDelay(0)` 只 yield
不阻塞，会让 input 任务退化为全速自旋并制造跨核 yield IPI 风暴（DI-11 的真因，见 ADR-007）。

**端到端延迟预算**（人测口径）：≤10ms 轮询 + 派发 ≈0.5ms（`KEY STATS` 均值；note-on 最大
≈1.4ms）+ 音频块 5.8ms（`HALF_FRAMES` 256 @44.1kHz）≈ **≤16.3ms 最坏情况**，仍在
「键→发声/动作 ≤20ms」的机器门禁（console 入口口径）之内。听感上的最终裁决仍是人工项
（DI-4 全键人测 + 演奏手感），覆盖端到端延迟而不只是 console 入口耗时。

### 切页的确定性（Task 7 评审裁定；P4-B Task 11 组顺序）

输入路径不取 LVGL 锁，切页走 `sloop_ui_request_page()` 邮箱，由 30ms UI 定时器执行：

- **一拍一页**：tab 的目标页按「当前页在组顺序扁平轮转里的下一张」计算
  （`sloop_ui_page_order_next()`，最终 12 页 PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→
  SAMPLE→STEP→TRANSPORT→SONG→PROJECTS→PRESETS→PLAY，跳过未注册页），邮箱只保留最后一次请求——同一 30ms
  周期内的 N 次 tab 只前进一页（不是 N 页）。需要一次跨多页请用 console `UI`（同步切页）。
- **esc = PLAY**：组与子页随页面复位（PLAY 组只有一页）；不改变「点组签记住组内
  上次子页」的记忆（那是组签/子签的触摸语义，不是 esc 的）。
- **同步 `UI` 作废异步邮箱**：`sloop_ui_switch_page()` 在 LVGL 锁内先
  `sloop_ui_cancel_pending_page()` 再切页，避免「tab 刚记下目标页 → console 同步切到 X →
  下一拍被旧请求切走」；回显页即落地页。
- **`KEY PANIC` 用专用清空**：撤销待切页走同一个 `sloop_ui_cancel_pending_page()`，
  不再「读当前页再请求同页」——后者在切页进行中会读到旧页而漏撤销。
- 定时器已消费的请求无法回滚（页面已经切了）；PANIC 只撤销**尚未执行**的请求。
- **组作用域**：跨组切换的销毁在 30ms 定时器/LVGL 锁内完成，输入路径只碰邮箱与
  状态层，不访问 LVGL 对象；同拍 `tab`+`PANIC` 的语义不变（PANIC 撤销尚未执行的
  切页请求，页面停在原组，源组页面不被销毁）。

### 层提示

页签条**下行右侧**（`SLOOP` 标题在下行上方、上行右侧）显示 `LAYER FX|EDIT|SEL|GLO`
（多层层键叠加时并列），由 30ms UI 定时器按 `state_get_copy().layer_held` 脏检查刷新；
无层时为空（不改变既有像素）。

## 后果

- 映射表调整不影响架构（InputManager 表驱动）；注入 `KEY` 测试即回归。
- `sloop_ui_flash_note()` 由输入层在 `state_apply` 返回后调用（PLAY 页音符指示）：
  只写一个「待显示音符」邮箱、**不取任何锁**，由 30ms UI 定时器点亮。Task 6 实测：
  同步取 LVGL 锁的旧实现让每条 note-on 的派发被帧等待拖到 ≈21ms（state_apply 本身
  仅 183–187µs）；邮箱化后 100 次 note-on 派发均值 ≈0.51ms、p95 ≈0.68ms、max ≈1.37ms（note-off max 1.32ms）。
  层提示与键盘操作共用状态层，联动模式（阶段 5）可复用。
- 阶段 4 需在层内补齐完整键位子集（FX 27 垫、SEQ 步进、八度切换等）。
- 轮询周期随 tick 频率变化（`INPUT_POLL_TICKS`）：若将来把 `CONFIG_FREERTOS_HZ` 提到
  1000，生效周期自动回到 5ms；启动日志与本节口径需按实测值复核。
- 演奏手感/延迟的人测项覆盖**端到端**（键→轮询→派发→音频块），不只 console 入口的
  `elapsed_us`；`KEY STATS` 的均值/最大值只是机器侧代理指标。
