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
| 低八度演奏 | z 0x42、s 0x33、x 0x43、d 0x34、c 0x44、v 0x45、g 0x36、b 0x46、h 0x37、n 0x47、j 0x38、m 0x48、`.`(句点，键名表印 `"."`) 0x49 | C3..C4（基音 48，note-on/off 发 `CMD_NOTE_ON/OFF`，更新 `last_note`；走选中合成轨，选中鼓轨时落回 track 0） |
| 高八度演奏 | q 0x21、2 0x02、w 0x22、3 0x03、e 0x23、r 0x24、5 0x05、t 0x25、6 0x06、y 0x26、7 0x07、u 0x27 | C4..B4（基音 +12） |
| 层（按住） | Sym 0x30=FX；Aa 0x31=EDIT；Ctrl 0x40=SEL；Alt 0x41=GLO | 层键 down/up 置/清 `layer_held` 位（`APP_LAYER_*`） |
| FX 层 | Sym + 任意键 | MVP 无功能（只置位 + 页签条提示），阶段 4 接 27 垫 |
| EDIT 层 | Aa + a 0x32 / b 0x46 / c 0x44 / d 0x34 / e 0x23 / f 0x35 / g 0x36 | 音名 A/B/C/D/E/F/G：按**音级**擦除聚焦步里的同名音（跨八度；命令 `CMD_ERASE_NOTE` 按精确音符，逐音符发） |
| SEL 层 | Ctrl + 1..4（0x01..0x04） | 选轨 0..3（`CMD_SELECT_TRACK`，同时设焦点轨） |
| GLO 层 | Alt + 1..4 / 5..8（0x01..0x04 / 0x05..0x08） | 静音 / 独奏轨 0..3（`CMD_MUTE`/`CMD_SOLO`；状态层持有，断言用 `STATE SNAP`，DI-9） |
| 走带 | space 0x4D | 播放/停止切换（`CMD_PLAY`/`CMD_STOP`）；enter 0x3D 未映射（原计划的 REC 占位在 MVP 移除） |
| 页面 | esc 0x00=PLAY；tab 0x20=下一屏 | `sloop_ui_request_page()`：只记目标页，30ms UI 定时器在 LVGL 上下文切页（输入路径不等锁，派发 22–72µs） |
| 焦点 | ← 0x4A / → 0x4C 移焦点步（0..15 环绕）；↑ 0x3B / ↓ 0x4B 移焦点轨（三条合成轨环绕） | `state_set_focus()` |

八度切换（原计划的 OCT±）**未实现**：键表里 `-` 0x0B / `+` 0x0C（以及 0 0x0A 等）仍空闲，
本可分配；真实原因是 MVP 裁定未包含八度切换，留给阶段 4 与完整层键位子集一起定
（MVP 演奏音域 C3..B4 共 24 音）。

### DI-4 子集核对（修饰键组合是否独立上报）

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

### 注入与延迟

`KEY <code> <down|up>`（console）与物理键盘同入口 `sloop_ui_input_key()`；code 十进制或
`0x` 十六进制，取值 0..0x7F（tab5_keyboard 事件码）。`KEY` 每条打印 `elapsed_us`（命令入口
→ 派发返回，含 state_apply 与 PLAY 指示），`KEY STATS [RESET]` 打印累计均值/最大。
InputManager 任务：优先级 4（低于音频 5），栈 4KB，`tab5_kb_poll()` 5ms 轮询，每批 8 条。

### 层提示

页签条右侧（`SLOOP` 标题左侧）显示 `LAYER FX|EDIT|SEL|GLO`（多层层键叠加时并列），
由 30ms UI 定时器按 `state_get_copy().layer_held` 脏检查刷新；无层时为空（不改变既有像素）。

## 后果

- 映射表调整不影响架构（InputManager 表驱动）；注入 `KEY` 测试即回归。
- `sloop_ui_flash_note()` 由输入层在 `state_apply` 返回后调用（PLAY 页音符指示）：
  只写一个「待显示音符」邮箱、**不取任何锁**，由 30ms UI 定时器点亮。Task 6 实测：
  同步取 LVGL 锁的旧实现让每条 note-on 的派发被帧等待拖到 ≈21ms（state_apply 本身
  仅 183–187µs）；邮箱化后 100 次 note-on 派发均值 ≈0.51ms、p95 ≈0.68ms、max ≈1.37ms（note-off max 1.32ms）。
  层提示与键盘操作共用状态层，联动模式（阶段 5）可复用。
- 阶段 4 需在层内补齐完整键位子集（FX 27 垫、SEQ 步进、八度切换等）。
