# 阶段 5B 设计：单轨编辑仪表盘（Sound）+ Mixer + Device 页

> 状态：**已接受**（2026-11-26，用户确认）｜ 日期：2026-11-26
> 关联：总设计 `docs/superpowers/specs/2026-11-26-phase5-sloop-go-alignment-design.md`（§2.2 差距表、§5.2、§6 5B、§7）、
> ADR-005（UI 架构/导航/池预算）、ADR-008（参数页与旋钮）、ADR-006（键盘映射）、
> `docs/design/012-feature-parity-checklist.md` §3（槽位权威表与鼓轨逐槽规则）、
> 收口报告 `docs/dev/closeout-phase5a.md`、侦察地图 `.superpowers/5b-recon.md`（会话内工作文件，不入库）。
> 上游基线：sloop-fm1 `bac9c05`（2.8.5）、sloop-fm1-go `ed941b5`（1.8.0），均未变。

## 1. 意图与缘起

用户指定 Sloop Go 的截图为界面设计参考，并点出「目前单轨编辑这些似乎都没有」。5A 已把 FM-1 连接打通
（`dev` @ `aba1499`）。5B 要交付的是用户最先问到的那个东西：**在 Tab5 上一页滚动看尽并编辑选中轨的全部参数**，
外加 Sloop Go 的 Mixer（四条带推子的通道条）与 Device（连接状态）两页。

5A 收口后用户明确选择「从 5B 开始」，而不是先做联动（5C）。5B 因此不涉及 FM-1：三张新页都只驱动本地引擎，
Device 页只「显示」5A 已有的连接状态。

## 2. 现状核对（证据，均来自代码与设备实测）

| 事实 | 出处 | 对 5B 的含义 |
|---|---|---|
| LVGL 池可用 **60,564B**（设备 `UI MEM pool_total`）；基础占用（无页）今天设备读数 ≈ 9.5KB（STEP 建成时 used 31,788 / delta 22,224），DI-56 重测为 ≈ 14,000B——预算一律按 14,000B 保守计 | 设备读数 2026-11-26；DI-56 | 留给「一页」的余量有限 |
| 切页顺序固定「建目标页 → 切换 → 销毁源组」；不变量 `base + max(源组 Σest + 目标首建页 est) + 4,096 ≤ 63,120` | ADR-008 §5；`ui.c show_page()` | 最大源组是 SEQ（Σest 32,256）⇒ **Sound 页 est ≤ 12,768B** |
| 旋钮 = 1 个自绘 LVGL 对象 ≈ 300B，尺寸写死 160px | `knob.h:36-38`、`knob.c` | 108 个旋钮各建对象 ≈ 32KB，**装不下**；且 160px 在半宽卡片里一行放不下 4 个 |
| 参数页槽表是纯数据（`param_slot_t/param_chip_t/param_page_t`，X 宏 `PARAM_PAGES`） | `page_layout.h:38-68`、`page_layout.c:164-218` | 卡片成员可以沿用同一做法（纯数据表） |
| 描述符：`sloop_core_desc_track_live(track,id,&d)` / `sloop_core_desc(scope,engine,id,&d)`；值 `st->p[]`（仅选中轨）/`st->g[]`；写 `CMD_SET_PARAM{a=track,b=id,v}` / `CMD_SET_GLOBAL{a=id,v}` | `page_params.c:283-300/388-470` | 仪表盘的每个槽沿用同一条读写路径 |
| 鼓轨逐槽规则（哪些槽可编辑、哪些锁） | 012 §3 | 仪表盘逐槽判定，锁定显示 `--` |
| 引擎名/预置名**没有按索引取名的 API**，只有当前声音的 `sloop_core_sound_read` | `sloop_core.h:259` | 预设选择器列表需要新增只读 API |
| 页面结构体只有 `build/update/est_bytes`；TAP 走 `ui.c page_tap()` 的 switch，键盘走 `input.c dispatch()` 的分支链 | `sloop_ui.h:42-48`、`ui.c:955-975`、`input.c:306-370` | 每张新页要在两处登记并提供无锁入口函数 |
| 键盘语义（参数页）：←/→ 移焦点、↑/↓ 调值、ENTER +1；TAB 翻页 | ADR-008；`input.c:306-346` | 新页沿用同一语义 |
| 页面区原点 `UI_TAB_BAR_H 72`，注入脚本里几十处 `72 + 页内 y` | `theme.h:28`、`inject.py` | **不能动** |
| 平台 NVS 只有 3 个固定 u8 键 | `sys_settings.c:33,103-105` | 没有「卡片折叠/排序」这类 UI 偏好的持久化位置 |
| 字体只有 Montserrat 14/16/20/28 | `sdkconfig` | Sloop Go 的等宽字体要换字体资产（见 §5 的方案取舍） |
| 脚本对页签条依赖重：`run_nav` 40 处 `UI TAB` + 42 处 `UI SUB`、约 60 个页签像素探针、`screenshot_matrix` 给每张截图加 2 个页签锚点 | `inject.py`、`screenshot_matrix.py:226-236` | 「换壳」会一次性作废这些证据 |
| 本会话的模型不能读图 | — | 界面的人眼目检是外部门禁 |

## 3. 目标、非目标、反目标

**目标**

1. **Sound 页（单轨编辑仪表盘）**：页眉选轨/换引擎/选预置（或鼓轨选 kit）；双列卡片，一张可滚动的页面呈现选中轨的
   全部参数组（Global、Master、Envelope、LFO、Engine edit、Voice、FX、Scale、Arp、Pattern、Drums、MIDI、Texture，
   ANALOG 引擎下共 109 个槽，其它引擎 106–108 个）；触摸拖动、键盘、`TAP`/`KEY` 注入三种方式都能改到每一个参数。
2. **预设选择器**：按名字浏览并选择引擎与预置（关闭 DI-54）。
3. **Mixer 页**：四条通道条——轨号与引擎名、电平数、竖向推子、Mute、Solo、Edit（选轨并进入 Sound）。
4. **Device 页**：显示 FM-1 连接状态与 5A 读到的固件信息。
5. 旧参数页 SOUND/MOD/SCALE/VOICE/GLOBAL/FX 与 TRACKS 页退役，其功能由新页**全覆盖**（逐项核对见 §5.8）；
  旧 SOUND 页上的 FM6 银行行迁到新 Sound 页。SAMPLE 页（采样槽行）**保留不动**，由 5E 重做为采样编辑页。
6. 证据同步：新页各有注入场景；被退役页的场景被替换；导航、池预算表、截屏矩阵同步。

**非目标**

- 界面外壳（左上固定播放/菜单、每页页眉、右侧状态栏）、Sloop Go 主题、等宽字体——挪到新的最后一个子阶段 5F（见 §5.1）。
- 卡片拖动排序、折叠状态持久化（没有通用 NVS 接口）；收藏面板（DI-44）。
- 鼓轨逐 lane 的 TUNE/DECAY 卡（需要新增引擎写接口；与 5D 的鼓格页一起做更顺）。
- 枚举/开关控件用 chip/开关样式渲染（5B 一律用旋钮，值文本已能显示枚举名与 ON/OFF）。
- 与 FM-1 的任何联动（5C）；Sloop Go 的 App 侧 4 个 LFO。

**反目标（做到了也算失败）**

- 页面绕过 `sloop_state` 直接调引擎写入口，或直接拼协议帧。
- 每个旋钮一个 LVGL 对象（池装不下），或靠抬 est/扩池硬顶（ADR-008 §8.12）。
- 改动页面区原点 72，或改动 `knob.c` 现有 160px 旋钮的几何（旧页与全部脚本依赖它）。
- 新页只能用触摸操作、键盘或注入够不到某个参数。
- 退役的旧页上某项功能在新页找不到入口。

## 4. 方案

### 4.1 总路线：先换壳还是先做页面

| 方案 | 内容 | 评估 |
|---|---|---|
| A. 按原规划「5B 先换壳」 | 先做外壳（菜单/页眉/状态栏）+ 主题 + 字体，再做三张页 | 换壳会一次性作废 `nav.sh` 全套、约 60 个页签像素探针和每张截图的页签锚点；而 5D/5E 替换其余旧页时这套证据还要再改一轮；旧页在过渡期挂在菜单里，外观不伦不类 |
| **B. 页面先行、外壳收尾（采纳）** | 5B 只做三张新页，挂进现有页签条；外壳 + 主题 + 字体挪到全部旧页退役之后，成为最后一个子阶段 **5F**，那时脚本只围绕最终导航模型改写一次 | 换壳的风险与工作量在「旧页最多」的时候最大，在「旧页为零」的时候最小；用户要的「单轨编辑」实质是页面而不是外壳；现有页签条的组位足够容纳（EDIT→SOUND、MIX→TOOLS、TRACKS→MIXER、SYS→SYSTEM+DEVICE），几何常量（16/104/8/176/5、九组）全部不变，`soak-selftest` 对 `ui_nav.h` 的钉死不用动 |
| C. 一次性重写整个 UI | 不分子阶段 | 无法逐页验证，评审粒度过大，否决 |

B 的代价：用户在 5F 之前看到的仍是旧页签条。若用户更想先看到外壳，可在 5B 之后立刻插入 5F，二者互不依赖。

### 4.2 Sound 页怎么装进池里

| 方案 | 评估 |
|---|---|
| **每张卡片一个自绘对象（采纳）** | 13 张卡片 = 13 个 LVGL 对象（标题与旋钮都在卡片的 `DRAW_MAIN` 里画），旋钮的状态放 **PSRAM** 里的静态数组；池开销 ≈ 5KB，加页眉与滚动容器后 est 12,288，在上限 12,768 之内（§5.6）。先例：STEP 页「一个自绘层，不是 64 个新对象」（ADR-005）。 |
| 每个旋钮一个对象（沿用 `knob.c`） | ≈32KB，切页峰值超池；且旋钮 160px 不可参数化 |
| 只建可见卡片（滚动虚拟化） | 焦点、键盘、注入都要处理「对象尚不存在」的槽，复杂度高，收益不如上一行 |

### 4.3 滚动与拖动的手势归属

现有页面全部关闭滚动；仪表盘是第一个可滚动页。风险：在旋钮上拖动会被 LVGL 当成滚动。

**决策**：卡片对象接管自己的全部手势，滚动由页面手动驱动——按下点落在某个旋钮的环带内 ⇒ 这是拖动旋钮；
落在卡片其它位置 ⇒ 这是滚动，每次移动事件里调用 `lv_obj_scroll_by()`（LVGL 9.6 提供）。没有惯性/回弹，行为确定、可注入。
这是一个需要在实施第一步用真机原型验证的假设（§7 Task 0）：若手动滚动在 13 张自绘卡片上掉帧或与 `TAP` 注入冲突，退路是
让滚动容器自己处理滚动、旋钮只在环带内响应（需验证 LVGL 的手势归属规则）。

## 5. 详细设计

### 5.1 子阶段调整（对总设计 §6 的修订）

- 原 5B「新壳 + Sound + Mixer + Device 页」拆为：**5B = Sound + Mixer + Device**（本文件）；**5F = 新壳 + 主题 + 字体**（排在 5E 之后）。
- 5C/5D/5E 内容不变。5F 的入口条件：5D、5E 的页面全部过门禁，旧页清零。

### 5.2 Sound 页

**布局**（页面区 1280×648，页内坐标，绝对 y = 页内 y + 72）

- 页眉（y 4..72，沿用现有 SOUND 页眉几何 `row0 (16,4) 288×32`、`row1 (320,4) 384×32`）：
  - row0：`ENGINE − 名 +`（鼓轨为 `KIT − 名 +`）；row1：`PRESET − 名 +`。左右 42px 区是 ±，沿用现有行为与 TAP 回显。
  - 每行右端新增 42px 的 `▾` 区：打开**选择器**（§5.3）。
  - **选轨条**：页眉行右侧 `(960,4) 304×32`，一个自绘对象分 4 格（`1 2 3 4`，每格 76px），当前轨用轨色高亮；
    点某格 ⇒ `CMD_SELECT_TRACK{k}`，留在本页。
  - FM6 引擎时的银行行：**从旧 SOUND 页迁入**，固定在卡片区上方 y 76..120，只在 FM6 引擎时出现（动作与 TAP 回显不变；
    实现可沿用现有的 7 个对象，池吃紧时改为单个自绘对象——见 §5.6）。
  - 页眉下方一行（y 40..72）：左侧提示（14pt，暗色，x 16 起、宽 1080 以内）：拖动旋钮调值 · 拖动空白滚动 · 方向键移焦点与调值；
    右侧是**细调按钮** `−/+`（沿用参数页的页级按钮：`BTN_DEC_X 1096` / `BTN_INC_X 1184`，80×40，同样式），作用于当前焦点槽。
    **ZOOM 设置打开时，提示文本改为显示焦点槽的大号读数**（标签 + 值，28pt）；页眉保持可操作。
    不调用全局的 `sloop_ui_zoom_knob`（它固定画在页面区顶部 40px，会盖住本页可操作的页眉）。
- 卡片区：一个竖向滚动容器，视口 x 16..1264、y 从页眉/固定行之下（默认 80；FM6 引擎时 128）到 644；两列，列宽 616，列间距 16。
  卡片按 Sloop Go 的 staggered 规则放置：依次放到「当前累计高度较小」的列（纯函数 `sound_layout_place`，host 可测）。
- 卡片：圆角面板（`UI_PANEL`），内边距 12；标题行高 36（16pt，`UI_TEXT`），右端画 `▾/▸` 折叠标记；点标题行折叠/展开（会话内有效）。
  内容每行 4 个槽，槽 140×132；卡片高 = 36 + 行数×132 + 12（折叠时 36）。
- 槽（自绘旋钮，直径 96，弧线宽 8）：底弧 `UI_PANEL_HI`，指示弧「焦点 = `UI_ACCENT`，否则 `UI_TEXT_DIM`」；
  名称 14pt 在弧上方，数值 16pt 在弧内。锁定槽不画弧、数值 `--`、名称 45% 暗。格式化沿用 `knob_format_value`。

**卡片成员**（纯数据头 `sound_layout.h`，X 宏；每个成员 = 作用域 + 参数 id；顺序与分组照 Sloop Go `layout(pe0)`）

| 卡片 | 成员数 | 备注 |
|---|---|---|
| Global | 4 | G_BPM G_SWING G_CLOCK G_TUNE |
| Master | 5 | DUST DUCK FILT **ROLL** + TFLT（轨参数）。`G_ROLL` 在 Sloop Go 的 Master 页里没有，但上游 MASTER 页有（`params.c:187`）、Tab5 现有 GLOBAL·REV chip 也有——为了让退役 GLOBAL 页后它仍有入口，收进本卡（与 Sloop Go 的一处有意偏离） |
| Envelope | 9 | ENV 4 / ENV DEST 4 / ENV SYNC 1（标签经 `sloop_core_desc_track_live` 取 ESYNC 名） |
| LFO | 11 | LFO 4 / LFO 2 3 / LFO DEST 4 |
| Engine edit | 9 | `P_E0..P_E7`（标签经引擎 `desc` 钩子）+ **`P_FTYPE`**（上游 `core.h:70`，标签 TYPE，仅 ANALOG 引擎出现，其它引擎不建槽）；鼓轨上整卡锁定（kit 由页眉选） |
| Voice | 9 | LEVEL 1 / VOICE 4 / VOICE 2 4 |
| FX | 22 | FX 4 / SLICER 4 / FX FADE 1 / DELAY 4 / REVERB·CHORUS 4 / INSERT 4 / INSERT 2 1 |
| Scale | 7 | SEL 4 / SEL 2 3 |
| Arp | 8 | ARP 4 / ARP 2 4 |
| Pattern | 4 | SLEN SDIV SWING GATE |
| Drums | 4 | DRCH DRLVL DRREV DRDLY |
| MIDI | 10 | MIDI 4 / MIDI 2 2 / MIDI 3 4（联动阶段才有可闻效果，参数本身可编辑） |
| Texture | 7 | TEXTURE 4 / TEXTURE 2 3 |

  **槽数**：ANALOG 引擎 **109**（Engine edit 9 + 其余 100）；其它合成引擎 **108**；SAMPLE 引擎 **106**（其 `edit[5]/edit[7]` 是占位，
  `min==max` 且无枚举）。这是槽数不是参数数：`G_DRCH` 同时出现在 Drums 与 MIDI 两张卡里（Sloop Go 同）。
  过滤规则同 Sloop Go `shouldShow`：标签为空、或 `min==max` 且无枚举的参数不建槽；仅 ANALOG 出现的 `P_FTYPE`
  （上游 `core.h:70`）由成员表的引擎门槛控制。host 测试按引擎钉住上面三个数。

**读写**：与参数页同一条路径——TRACK 作用域 `CMD_SET_PARAM{a=选中轨, b=id, v}`，GLOBAL 作用域 `CMD_SET_GLOBAL{a=id, v}`；
值读 `st->p[id]` / `st->g[id]`（30ms 拍同步）。拖动中每次变化即发命令（与现有旋钮一致）。

**锁定**：鼓轨逐槽规则（012 §3）：只有 PATTERN、SLICER、INSERT×2、TFLT、LEVEL、PAN 与全部 GLOBAL 卡可编辑，其余 `--`。
描述符取不到的槽不建。

**触摸**：卡片对象处理 PRESSED / PRESSING / RELEASED：按下点距某槽中心 30..52px ⇒ 该槽获得焦点并开始拖动
（沿用 `knob_math` 的角度差→值差，270° 扫角）；否则 ⇒ 页面滚动（§4.3）。
实施约束（已对照 LVGL 9.6 源码核实，见 `lv_indev_scroll.c` / `lv_obj_scroll.c`）：
- 滚动容器与卡片对象都要清掉 `LV_OBJ_FLAG_SCROLLABLE`，否则手指位移超过 10px 后 LVGL 会再滚一次（内容走两倍速）；
- 手动滚动用 `lv_obj_scroll_by_bounded()`（`lv_obj_scroll_by()` 不钳位）；
- `PRESS_LOCK` 默认开着，卡片在整个手势期间保持为按压对象；
- 滚动手势结束时的 `CLICKED`/`SHORT_CLICKED` 与长按都必须被抑制，否则「拖完滚动」会误触发卡片标题的折叠。

**键盘**（沿用 ADR-008 语义，在 `input.c dispatch()` 加分支）：←/→ 焦点在「卡片顺序 × 槽顺序」的扁平序上 ±1（环绕）；
↑/↓ 当前焦点槽 ∓1/+1；ENTER +1。焦点变化后自动滚动使其可见。折叠的卡片跳过。

**TAP 注入**：`page_sound_tap(x,y)`：页眉 ± 区与 `▾` 区、银行行、采样槽行、卡片标题行（折叠）、槽（只设焦点，不改值）。
新增 console 子命令供脚本定位与滚动：`UI SOUND CARD <n> SLOT <m>` ⇒ 打印该槽当前的屏幕矩形；
`UI SOUND SCROLL <y>` ⇒ 设置滚动偏移并回显；`UI SOUND FOCUS [<n>]` ⇒ 读/写焦点槽的扁平序号；
**`UI SOUND KNOB <n> <press|drag> <dx> <dy>`** ⇒ 用合成指针事件走卡片对象的**同一**手势处理路径
（与现有 `UI KNOB` 对参数页的作用相同，DI-16 的拖动门禁由此延续到新环带 30..52px）。

### 5.3 选择器（引擎与预置）

点页眉的 `▾` 打开一个覆盖在卡片区之上的面板（按需创建、关闭即销毁；池开销临时 ≤4KB）：

- 顶行：引擎 chips（12 个，单对象自绘行，当前引擎高亮）；点选 ⇒ `CMD_SET_ENGINE{a=track, v=engine}`（鼓轨没有这一行，改为 kit 列表）。
- 下方：当前引擎的预置名列表（`序号  名字`，当前预置高亮）；点选 ⇒ `CMD_SET_PRESET{a=track, v=preset}`（鼓轨：`CMD_SET_PARAM{a=3,b=63,v=kit}`）。
- 键盘：↑/↓ 移高亮、←/→ 换引擎、ENTER 选中并关闭、ESC 关闭。`TAP`：列表第 n 行的矩形由 `UI SOUND PICK <n>` 给出。
- 新增只读 core API（`sloop_core.h`，实现在 `sound_tab5.c`，不建镜像表）：
  `uint8_t sloop_core_nengines(void); bool sloop_core_engine_name(uint32_t engine, char *buf, uint32_t n);`
  `uint8_t sloop_core_npresets(uint32_t engine); bool sloop_core_preset_name(uint32_t engine, uint32_t preset, char *buf, uint32_t n);`
  以及鼓 kit 的同名两函数。

### 5.4 Mixer 页

- 四条通道条：x = 24 + k×312，宽 296，y 40..640（占满页面区高度；x 几何与现有 TRACKS 卡相同，便于脚本沿用命中点）。
- 每条：标题（20pt：`1 DRUM` / `2 ANALOG`…，选中轨用轨色）；电平数（16pt）；竖向推子（自绘：轨道 24×高约 300，
  滑块 88×24；点或拖到某高度即设为该电平）；三个按钮（高 44，单对象 label）：`MUTE`（开：`UI_ERROR` 底）、`SOLO`（开：`UI_ACCENT` 底）、
  `EDIT`（选中轨显示 `EDITING` 且不可点；否则点了 = `CMD_SELECT_TRACK{k}` 并切到 Sound 页）。
- **标题的触摸/TAP = 只选轨、留在本页**（与现有 TRACKS 卡一致）：沿用 TAP 种类 `SLOOP_TAP_TRACK_CARD` 与回显
  `-> TRACKS select track=`，这样 `inject.py` 的 `select_track()` 只需把页名从 `tracks` 换成 `mixer`。
- 读写：电平 `CMD_SET_PARAM{a=k, b=0(P_LEVEL), v}`，读 `st->core.trk[k].level`；Mute/Solo 是翻转命令 `CMD_MUTE{a=k}` / `CMD_SOLO{a=k}`，
  读 `st->core.trk[k].mute/solo`；选轨 `CMD_SELECT_TRACK{a=k}`。
- 键盘：←/→ 选轨；↑/↓ 选中轨电平 ±1；ENTER = 进入 Sound 页（Edit）。`TAP`：推子、三个按钮、标题。
  console：`UI MIXER LEVEL <k>`（读）。
- 对象数：每条 6 个，共 24 个 ⇒ est 10,240。

### 5.5 Device 页

一屏一张表（单对象自绘，行高 36，16pt；无焦点操作；`TAP` 全部 miss），每 30ms 拍比较签名后重画：

| 行 | 来源 |
|---|---|
| 连接状态（● 已连接 / ○ 未连接） | 链路投影（见下） |
| 设备（产品名、VID:PID） | 同上 |
| 接口 / 端点（MIDI 接口号、bulk IN/OUT） | 同上 |
| SLOOP 版本串 | 同上（INFO） |
| 协议版本、引擎数、P_COUNT、G_COUNT、NSTEP、P_E0、NTRK | 同上 |
| 升级提示（`need_upgrade=1` 时显示「请升级 FM-1 固件到 SLOOP 2.8.x」） | 同上 |
| 收发统计（rx / tx / drops / timeouts / stray） | 同上 |
| Tab5 固件版本（`esp_app_desc_t.version`）与 SLOOP 核心版本 | esp_app / `sloop_core` |

**链路投影**：`app_state_t` 新增 `link` 子结构（上述字段），由状态层 30ms 同步时通过一个**提供者回调**填充
（`state_set_link_provider(fn)`，由 `main.c` 注册一个读 `usb_fm1_get_status()` / `fm1_session_info_cached()` / `fm1_session_stats()`
的函数）。这样 `sloop_state` 不依赖 USB 组件（host 单测不受影响），`sloop_ui` 只读 AppState；5C 把执行端换成 FM-1 时沿这条缝扩展。
est 3,072。

### 5.6 页表、导航与退役

- 页 id：Sound 沿用 `APP_PAGE_ANALOG`（id 6，页名 `sound`）；Mixer 沿用 `APP_PAGE_TRACKS`（id 1，页名改为 `mixer`，`tracks` 别名删除）；
  Device 新增 `APP_PAGE_DEVICE`（追加在 `app_page_t` 末尾，`APP_STATE_PAGES` 19→20）。
- 组表：`X(MIXER, APP_PAGE_TRACKS)`、`X(EDIT, APP_PAGE_ANALOG)`、`X(MIX, APP_PAGE_TOOLS)`、`X(SYS, APP_PAGE_SYSTEM, APP_PAGE_DEVICE)`；
  其余组不变。TAB 扁平序变为 14 页：
  `PLAY → MIXER → SOUND → TOOLS → SAMPLE → DSYN → FM6P → STEP → TRANSPORT → SONG → PROJECTS → PRESETS → SYSTEM → DEVICE`。
- 退役：参数页布局表只保留 SAMPLE（`idx 3`）；MOD/SCALE/VOICE/GLOBAL/FX 与旧式引擎页不再注册；
  `page_tracks.c` 删除。`UI <page> CHIP`、`UI KNOB` 只对仍存在的参数页有效。
- 预算表：Sound est **12,288**、Mixer est **10,240**、Device est **3,072**（512 对齐，实测 delta + 10–26% 校准）。
  **Sound 的 est 没有上抬空间**：按 DI-56 重测的基础占用 ≈14,000B 计，SEQ 两页全建 → 首建 Sound 的过渡峰值
  = 14,000 + 32,256 + est + 4,096 ≤ 63,120 ⇒ est ≤ 12,768。Sound 页的非卡片对象约 20 个（页眉两行、选轨条、提示/读数行、
  细调按钮 ×2、滚动容器、FM6 银行行最多 7 个），加 13 张卡片。**Task 0 实测若超 est，第一退路是把银行行的 7 个对象并成 1 个自绘对象；
  第二退路是把细调按钮并入提示行对象（自绘两个按钮区）。不得抬 est、不得扩池。**
  其余最坏对不变（SEQ → PRESETS 60,776；SEQ → Mixer 60,216；SEQ → Sound 62,640，余 480B——Task 0 必须实测这一对的过渡 `pool_free`）。

### 5.7 证据同步

- 新增场景：`scripts/inject/sound.sh`、`mixer.sh`、`device.sh`（逻辑各在 `inject.py` 的 `run_sound/run_mixer/run_device`）。
  sound.sh 覆盖：卡片与槽数（按引擎 109/108/106）、逐槽标签 = 描述符、首槽写后回读、鼓轨逐槽锁定、折叠、键盘焦点与调值、
  `TAP` 设焦点、**拖动（`UI SOUND KNOB`：按下不改值、+90° 改值、拖回回原值、环带外不响应、锁定槽忽略）**、滚动后重画无残留、
  滚动手势后的释放不触发折叠、选轨条、细调按钮、选择器选引擎/选预置（`STATE SNAP` 为真值）、银行行、
  池 delta/est、与 SEQ 的有序对 `pool_free ≥ 4,096`、截屏。
  mixer.sh 覆盖：推子点/拖设电平（`ENG SNAP trk` 为真值）、Mute/Solo 翻转、标题选轨留在本页、EDIT 切到 Sound 且选中轨正确、键盘。
  device.sh 覆盖：页面各行与 `FM1 STATUS`/`FM1 INFO` 一致（FM-1 未接/已接两种状态都按当前真值断言，并记录是哪一种）。
- 退役并替换：`tracks.sh` → `mixer.sh`；`params.sh` 拆解——SOUND/族页段由 `sound.sh` 接管，SAMPLE/DSYN/FM6P 段并入 `synth.sh`，
  TOOLS / REC 配置 / 参数锁段并入 `transport.sh` 或 `step_edit.sh`（计划阶段定）。十条场景的总断言数不得下降为名、只降为实：
  被退役页的断言必须有新页上的对应断言。
- 同步更新：`nav.sh`（14 页序、组成员、Σest、MIXER 组签像素）、`soak.py`（PAGES / GROUP_PAGES / POOL_RT_REP）、
  `screenshot_matrix.py`（条目表：sound×6 / mixer×3 / device×2，替换 18 个退役条目）、`scripts/knob-test.sh` 的页表完整性检查、
  `inject.py` 里依赖旧页几何的常量（如 `BANK_ROW_Y = 72 + 552`，银行行挪到 y 76 之后 store 的银行场景要改）、
  `docs/dev/reference/{firmware-map,scripts,verification-gates,console-and-injection}.md`、AGENTS.md §3/§5.1。
- 人眼目检（外部门禁）：Sound / Mixer / Device 三页的截屏对照 `third_party/sloop-fm1-go/screenshots/{1,9,11}.png`，由用户执行。

### 5.8 功能覆盖核对（退役页 → 新落点）

| 退役页上的功能 | 新落点 |
|---|---|
| GLOBAL 页 8 槽（BPM SWING CLOCK TUNE / DTIME DFDBK DCOLOR DMIX） | Global 卡 4 + FX 卡的 DELAY 4 |
| FX 页 8 槽（DIST CHOR DLY REV / TFLT / LEVEL / ATK） | FX 卡的 FX 4、Master 卡（TFLT）、Voice 卡（LEVEL）、Envelope 卡（ATK） |
| GLOBAL·REV chip 8 槽（RSIZE RDAMP CRATE CDEPTH / DUST DUCK FILT ROLL） | FX 卡的 REVERB / CHORUS 4、Master 卡（含 ROLL） |
| GLOBAL·DRUMS / TEXTURE chip | Drums 卡 / Texture 卡 |
| MOD / SCALE / VOICE 的全部 chip | Envelope / LFO / Scale / Arp / Voice / Pattern 卡 |
| 旧 SOUND 页：ENGINE/PRESET 页眉、鼓轨 KIT 行、8 个引擎旋钮、FM6 银行行、页级 ± | 页眉两行 + 选择器、Engine edit 卡、迁移的银行行、细调按钮 |
| ZOOM 大读数 | 页眉下方的读数行（§5.2） |
| TRACKS 页：点卡选轨、电平条、MUTE/SOLO 状态 | Mixer：标题选轨、推子、Mute/Solo 按钮 |
| console `UI <page> CHIP` / `UI KNOB`（对退役页） | `UI SOUND CARD/SLOT`、`UI SOUND KNOB` |

## 6. 约束与诚实边界

- 池预算、切页顺序、页面区原点 72、`knob.c` 几何：全部不动（§2）。
- 新增静态数组（旋钮状态 ≈ 108×~60B、卡片布局缓存）必须落 PSRAM（`EXT_RAM_BSS_ATTR` 或 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`）；
  内部堆门线不变（`internal_min ≥ 61,440`）。
- Sound 页构建延迟预算 ≤100ms（ADR-005）；滚动时的重画性能以真机为准。
- 字体仍为 Montserrat：新页的外观因此与 Sloop Go 不完全一致（等宽字体在 5F）。
- 截图只能做像素核对；人眼读图由用户完成。
- 手势归属（§4.3）是待验证假设，Task 0 的真机原型是第一道关。

## 7. 实施切片（每片各出一份计划；体量只给相对量级）

| 切片 | 内容 | 体量 |
|---|---|---|
| **5B-0 原型验证** | 真机原型：一张滚动容器 + 13 张自绘卡片 + 109 个槽；测池 delta、构建耗时、滚动重画、手动滚动与旋钮拖动是否互不干扰、`TAP`/`UI SOUND KNOB` 注入是否可达、SEQ → 原型页的过渡 `pool_free`。不保留原型代码 | S |
| **5B-1 Sound 页** | core 只读 API；`sound_layout.h`；`page_sound.c`；选择器；键盘/TAP/console；退役 6 张参数页；`sound.sh`；host 测试 `sound-test.sh` | L |
| **5B-2 Mixer 页** | `page_mixer.c`；退役 TRACKS；`mixer.sh` | M |
| **5B-3 Device 页** | 链路投影与提供者；`page_device.c`；`device.sh` | S |
| **5B-4 收口** | nav/soak/screenshot_matrix/knob-test 同步；全矩阵（两种 FM-1 条件）；人眼目检；证据与收口报告 | M |

5B 完成判据：三张页各自的注入场景 0 FAIL；十条场景全绿；截屏矩阵全过；有序对全矩阵 `pool_free ≥ 4,096`；
`selftest-heap.py` 与 30 分钟 soak 通过；用户目检三页截屏通过；功能对齐清单里 SOUND/MOD/SCALE/VOICE/GLOBAL/FX/TRACKS 各行改指向新页。

## 8. 留给后续计划/实施者决定的事

- 精确像素：卡片圆角、行距、文本基线、折叠标记样式；选择器面板的尺寸与行高。
- `sound_layout_place` 的具体算法与两列高度相同时的归侧规则。
- 选择器列表用单个自绘对象还是逐行 label（以池实测为准，上限 4KB）。
- Mixer 推子拖动的坐标→电平映射细节与滑块绘制。
- Device 页行顺序与文案。
- `params.sh` 各段的具体去向。
- host 测试用例清单与 `scripts/sound-test.sh` 的编译方式（照 `knob-test.sh`）。

## 9. 本次由我代为裁决的事项（auto 模式）与「若错代价」

| 裁决 | 理由 | 若错的代价 |
|---|---|---|
| **页面先行、外壳收尾**：把壳 + 主题 + 字体从 5B 挪到新的 5F | 换壳的证据代 价在旧页最多时最大；用户要的是单轨编辑本身 | 用户在 5F 之前看到旧页签条；可在 5B 后立刻插入 5F |
| 每张卡片一个自绘对象，旋钮状态放 PSRAM | 池装不下 108 个对象；STEP 页已有先例 | 卡片自绘代码比复用 `knob.c` 多约 300 行 |
| 手动驱动滚动（卡片接管手势） | LVGL 的手势归属规则不易按位置区分；手动方案确定、可注入 | 没有惯性滚动；若真机原型证明不行，改为容器滚动 + 仅环带响应 |
| 5B 一律用旋钮，不做 chip/开关样式 | 复用现有焦点/注入模型，零新控件类型 | 枚举/开关的外观与 Sloop Go 不同 |
| 卡片顺序固定、折叠状态只在本会话有效 | 没有通用 NVS 接口；拖动排序是复杂手势 | 与 Sloop Go 的「可排序、可持久」不同 |
| 鼓轨 TUNE/DECAY 卡延后到 5D | 需要新增引擎写接口；与鼓格页同做更顺 | 鼓轨的逐 lane 调音在 5D 之前仍不可达 |
| 页眉不单设 VOICE 下拉 | VOICE 模式已是 Voice 卡里的一个旋钮 | 多点一次才能改 VOICE 模式 |
| 键盘语义沿用 ADR-008（←→ 焦点、↑↓ 调值） | 与现有页面和全部键盘测试一致 | 108 个槽线性移动较慢 |
| 页名：Sound 沿用 `sound`、Mixer 取代 `tracks`、Device 新增 `device` | 注入脚本的 `UI <page>` 是稳定接口 | 旧脚本里的 `UI tracks` 必须改 |
| 链路状态经「提供者回调」进 AppState | 保持 `sloop_state` 可 host 单测、不依赖 USB 组件；也是 5C 的接入点 | 多一层间接 |
| Master 卡收进 `G_ROLL`（Sloop Go 没有） | 否则退役 GLOBAL 页后它无入口，违反反目标 | 与 Sloop Go 多一个旋钮 |
| SAMPLE 页保留不动（只迁 FM6 银行行） | 采样槽行是 `page_params.c` 里的单例状态、按页门控，搬到 Sound 页要重构；它是 5E 采样编辑页的现成起点 | SAMPLE 引擎的 8 个旋钮在新旧两页各有一份 |
| Sound 页眉加 4 格选轨条 | 目标里写了「页眉选轨」；TRACKS 退役后纯触摸用户要能换轨 | 多一个对象（约 300B） |
| Mixer 标题点选只选轨、留在本页，沿用旧 TAP 回显 | 与旧 TRACKS 行为一致，脚本只改页名 | 想从 Mixer 进 Sound 要再点 EDIT |
| Sound 页保留页级 ± 细调按钮；ZOOM 改在提示行显示大读数 | 96px 旋钮靠触摸做 ±1 基本不可能；全局 ZOOM 标签会盖住可操作的页眉 | 多两个对象（est 更紧） |
| 新增 `UI SOUND KNOB` 注入 | 新环带 30..52px 是全新的拖动代码，不能只靠人眼 | console 与文档多一项 |
| Sound est 上限 12,768，不得上抬 | 基础占用按 14,000B 保守计，SEQ → Sound 的余量只有约 480B | 若 Task 0 实测超限，按两级退路改对象设计 |

## 10. 需要用户提供/确认的事

1. 确认 §4.1 的路线调整（页面先行、外壳收尾）。
2. 5B 收口时对照 Sloop Go 的 1/9/11 号截图做三页的人眼目检。

## 11. 信息来源

- 代码：`firmware/components/sloop_ui/`（`ui.c`、`ui_nav.h`、`theme.h`、`theme_derive.h`、`page_layout.{h,c}`、`page_params.c`、
  `knob.{h,c}`、`knob_math.{h,c}`、`input.c`、`page_tracks.c`、`page_transport.c`、`page_system.c`）、`sloop_state.h`、`sloop_core.h`、
  `sloop_hal.h`、`main/console.c`；设备 `UI MEM`/`ENG PERF` 读数（2026-11-26）。
- 文档：ADR-005、ADR-008、ADR-006、012 §3、`docs/dev/reference/*`。
- 脚本：`scripts/inject/inject.py`、`soak.py`、`soak-selftest.py`、`screenshot_matrix.py`、`knob-test.sh`。
- 上游：`third_party/sloop-fm1-go/android/app/src/main/kotlin/com/sloop/go/ui/{ParamsScreen,SequencerScreen(TrackSelect/VoiceSelect/PresetPicker),MixerScreen,ConnectScreen,UiScale,Controls,Theme}.kt`；
  `third_party/sloop-fm1/firmware/src/{params.c,editor.c,editor_live.c}`。
- LVGL 9.6.0：`managed_components/lvgl__lvgl/include/lvgl/core/{lv_obj.h,lv_obj_scroll.h}`。
