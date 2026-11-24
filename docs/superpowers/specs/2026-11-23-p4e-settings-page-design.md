# P4-E 设置页 spec（SYS 组 / SYSTEM 页 + 设置持久化）

> 状态：builder check 完成（5 条发现已并入正文：NVS 延迟提交、ZOOM 全局单对象、SELFTEST
> chip 停流序 + console 路径不变、palette 精确映射表、BRIGHT 断言改读回）→ 待用户确认。
> 上位文档：`docs/design/002-port-roadmap.md` 阶段 4 表 P4-E 行、
> `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md` §3 P4-E 行。
> 本文裁决记录（Approaches §5）按用户授权（auto 权限模式、「按你推荐的方式取舍，目标是最大程度移植」）由本会话代为决定。

## 1. 意图与背景

P4-B 落地了 settings 对象的字节层（PER3 组装/保留规则、torn-write 快照），P4-C/P4-D 把
arrangement/DSU 挂了上去，但 **settings 仍是「上游首启快照」的硬编码**（palette/lowcut/zoom=0、
panel=PANEL_DEFAULT、lights=0），没有任何设置 UI。P4-E 补上：

1. 把上游 HOME 菜单的设置面（SCREEN/LIGHTS/AUDIO/SEQ/SYSTEM 五节）移植成 Tab5 的
   **SYS 组 / SYSTEM 单页**；
2. 把 P4-B 的「快照默认值」换成**真实状态源**（ADR-009 §6 明文要求的 P4-E 义务，含
   `song_persist.h` 的 `SONG_PANEL_DEF` 同步）；
3. 接上 Tab5 原生的平台控制面：**屏幕亮度、输出音量、输出路由、键盘 RGB**；
4. 顺手收 DI-32（`STORE RAW` 不能写 settings 对象）——settings 成为一等对象后触发条件已满足。

退出判据（roadmap）：设置持久化（`OBJ_SETTINGS` 上游格式；NVS 不承载 SLOOP 状态）、
重启保持。

## 2. 上游事实（2.8.5，侦察结论）

### 2.1 settings 对象布局（ADR-009 §6 已有 + 本次补全）

- `persist_t` 88B：magic@0 / palette@4 / lowcut@8 / zoom@12 / panel@16（32B 硬件校准表）/
  arrangement@48（36B）/ **lights@84（4B `lights_word`，2.3 追加）**。
- DSU 1,464B@88；MIDI 尾段 40B（`2*ML_N+8`，ML_N=16）：`midi_set[10]` + ML 映射 32B + …；
  **`flt_reso`（2.7.2 FILTER RESO，0..10）= 尾段 byte `2*ML_N+6`=38 的 bits 2..5**（project.c
  938 行读、893 行写）。
- side record 80B：theme_t 40B（THM2 magic + name[8] + c[5] + trk[4] + bg + hi + rsv + crc，
  gfx.c `theme_check`）+ FAVS magic + fav[16] + CRC。
- 上游写路径：`settings_save()` = quiet 时随 autosave 一起写（「settings_save when quiet,
  with the song」，project.c:1209）；播放中的修改置 `settings_later`，停止后补写。

### 2.2 上游菜单面（ui_menu.c，HOME 长按）

五节：**SCREEN**（COLOR=palette+theme、ZOOM、KNOB ACCEL）、**LIGHTS**（LIGHTS、KEYS、NOTES）、
**AUDIO**（SPEAKER LOWCUT、USB AUDIO、USB SERIAL、USB RATE、FILTER RESO）、**SEQ**（STEP
ADVANCE、DRUM PREVIEW）、**SYSTEM**（2ND CORE、HARDWARE CALIBRATION、LEARN CLEAR、ABOUT）。

`lights_word` 位域（panel.c 104..135）：lights_lvl 0..3 / lights_keys 4..7 / lights_notes 8 /
**rec_tempo 9 / rec_count 10** / usb_full 11 / lights_sync 12..13 / lights_mout 14 /
lights_min 15 / usb_serial 16 / vis_style 17..20 / lights_min-CH 21..25 / cpu1_off 26 /
knob_accel_off 27 / usb_rate 28..29 / **theme_on 30** / **rec_quant_off 31**。

`settings.zoom` = 「被触摸参数的大读数」（ui_draw.c:808），不是 NOTES 页的 `ui.zoom`（运行期）。

### 2.3 golden 确定性风险（本次侦察新发现）

regress 的 `blk()` → `mix_block()`：输出级过 `master_pre`（**fx_lowcut** 两节高通）与
`djf_process`（**flt_reso**）。这两项非 0 会改变 golden 哈希。今天设备自检 182/182 成立只是
因为 lowcut/reso 从未被置位——**P4-E 一旦可设，自检必须先归零再跑**。

### 2.4 Tab5 硬件面（已核实）

- **背光**：BSP `bsp_display_brightness_set(0..100)`（LEDC 10bit，bsp_display.c:95）；
  `bsp_display_backlight_on()` = 100%（display.c:313 现状）。
- **音量**：`esp_codec_dev_set_out_vol(handle, 0..100)`，启动设 70（audio_stream.c:17,81），
  `sloop_hal_audio_codec()` 已暴露活句柄。
- **输出通路**：官方原理图第 3 页显示，NS4150B 扬声器功放与耳机左声道同接 ES8388
  **LOUT1**，ROUT1 只到耳机右声道，**LOUT2/ROUT2 悬空**；PI4IOE5V6408 的 SPK_EN 是另一
  条可能的控制路径。仅靠 LOUT1/ROUT1 与 LOUT2/ROUT2 音量寄存器无法分离扬声器和耳机，
  故 D10 已按 checkpoint 降级为 **speaker-only**（DI-39；docs/tab5-hardware.md §3.2）。
- **键盘 RGB**（0x6D，M5Tab5-Keyboard-Internal-FW 的 `user_i2c_reg.h` /
  `user_i2c_callback.c` / `user_rgb.c`）：`0x03` 全局亮度 0..100（默认 20）、独立 `0x11`
  RGB 模式（0 绑定/1 自定义）、`0x60` 起每颗 LED 4 字节（B/G/R/保留），RGB1=`0x60..62`、
  RGB2=`0x64..66`；键盘工作模式在另一个 `0x10`，与 RGB 模式无关。
- LVGL 字体：Montserrat 14/16/20/28 已启用（28 为错误状态条在用）——ZOOM 大读数直接可用。
- NVS：分区在（0x9000/0x6000），firmware 目前零使用。

2026-11-23 修正（来源：官方原理图 + Internal-FW 源码，DI-39）。

## 3. 目标 / 非目标 / 反目标

**目标**：

1. SYS 组 / SYSTEM 页（15 页 / 8 组），四节映射上游五节；键盘 + 触摸 + console 三入口。
2. 设置持久化：上游可映射项写 `OBJ_SETTINGS`（字节级上游格式）；Tab5 原生项（亮度/音量/
   路由）写 NVS；重启（含硬复位）双路保持。
3. 设置项真实生效：palette/theme 重着色 UI、lowcut/reso 进 DSP、REC 三项（MODE/START/QUANT）
   开机恢复、LIGHTS 驱动键盘 RGB、亮度/音量/路由即时生效。
4. `ENG SELFTEST` 家族的 golden 确定性：运行期强制 lowcut/reso=0，结束后按 settings 重载。
5. `STORE RAW END settings`（DI-32 收口）+ `STORE SETTINGS INFO|SAVE` 诊断面。
6. 全套机器门禁：host（persist/state）、注入场景 `sys.sh`、截屏、soak、有序对预算。

**非目标**（本轮明确不做）：

- 主题**编辑器**（40B theme 的编写是 web 编辑器的事；Tab5 只读取/显示/应用 side 里已有的
  theme；名字文本输入见 DI-37）。
- MIDI LEARN / LEARN CLEAR、MIDI 尾段其余字段的 UI（midi_set/ML 映射逐字节保留）。
- USB AUDIO/SERIAL/RATE、2ND CORE、HARDWARE CALIBRATION（panel 表）、STEP ADVANCE、DRUM
  PREVIEW、VISUALISER STYLE 的 UI——均无 Tab5 对应语义，位/字节按上游默认保留（详见 §4 表）。
- 收藏（fav[16]）：上游 PRESETS 页 KNOB 4 的功能，属 PRESETS 页交互而非设置页；side 逐字节
  保留，功能延后。
- 双模式（companion）调度、编辑器协议——阶段 5。

**反目标**：

- 不得为亮度/音量等 Tab5 原生项改上游 settings 字节布局（尾段/新字段/位域挪用都是破坏
  ADR-009 §6 的字节兼容）。**特别裁决：屏幕亮度不挪用 lights_lvl**（那是 FM-1 按键背光，
  挪用会在 FM-1 备份互通时错误迁移）；键盘 LED 亮度用 lights_lvl 是同义映射（同一菜单行、
  同一枚举、同为「灯的亮度」），不算挪用。
- 不新建持久化对象/分区；NVS 只放平台级三键（bright/vol/out），不承载任何 SLOOP 状态；键盘 RGB 由 lights_lvl + accent 派生，不另存 kbd 键。
- 不因设置页超 LVGL 预算而扩池（超了先改对象设计）。

## 4. 字段映射总表（上游项 → Tab5 落点 → 持久化）

| 上游菜单项 | 上游存储 | Tab5 落点 | 持久化 |
|---|---|---|---|
| COLOR | persist.palette + side theme + bit30 theme_on | UI 重着色（§6）；THEME 仅当 side 有效时作为第 6 选项显示 | settings |
| ZOOM | persist.zoom | 参数页聚焦旋钮的大读数条（Montserrat 28） | settings |
| KNOB ACCEL | bit27 | **不移植**：环带粗/细双区已覆盖「快转快调」语义（文档化适配） | 位保留 0 |
| LIGHTS | bits0..3（lights_lvl） | 键盘 2×WS2812：亮度 0/20/60/100 + 自定义模式 + 颜色=当前 accent | settings + I2C |
| KEYS / NOTES | bits4..7 / bit8 | 无对应（非每键灯） | 位保留 0 |
| SPEAKER LOWCUT | persist.lowcut | `fx_lowcut` 实时 | settings |
| FILTER RESO | MIDI 尾段 byte38 bits2..5 | `flt_reso` 0..10 实时 | settings（尾段 RMW） |
| USB AUDIO / SERIAL / RATE | bits11/16/28..29 | 无对应 | 位保留 |
| SYNC / MIDI OUT / IN | bits12..15/21..25 | 无对应（联动阶段另议） | 位保留 |
| STEP ADVANCE / DRUM PREVIEW | MIDI 尾段 | 无对应 | 字节保留 |
| 2ND CORE | bit26 | 无对应（P4 性能固定双核） | 位保留 |
| HARDWARE CALIBRATION | persist.panel（32B） | 无对应（70 键固定映射） | 字节保留 PANEL_DEFAULT |
| LEARN CLEAR | MIDI 尾段 ML 映射 | 非目标 | 字节保留 |
| ABOUT | —（页面） | SYSTEM 节版本行 | — |
| REC MODE/START/QUANT | bits9/10/31 | TRANSPORT 页 REC 配置改为**开机恢复 + 变更即置 settings dirty**（上游语义） | settings |
| —（Tab5 原生） | — | 屏幕亮度 10..100（步进 10） | **NVS** |
| —（Tab5 原生） | — | 音量 0..100（步进 5） | **NVS** |
| —（Tab5 原生） | — | 输出 SPEAKER（D10 已降级；HEADPHONE 回 `N/A`，DI-39） | **NVS** |
| —（Tab5 原生） | — | KBD RGB 颜色不单独存（= 当前 accent 派生） | — |

## 5. Approaches（取舍记录）

**D1 导航：SYS 第 8 组、单页 SYSTEM**（取）。组签 142→**122px**（8×122+7×8=1,032，右端
1,048 ≤ 1,064 reserve；`UI_GROUP_W` 与 theme.h 断言同步）。`APP_PAGE_SYSTEM=14`、
`APP_STATE_PAGES` 14→15，TAB 轮转 …→PRESETS→SYSTEM→PLAY。四节行直接映射上游菜单五节（SEQ
节的三项 REC 设置散在 TRANSPORT 的 REC chip，持久化在 §7 状态层做，不放 SYS 页）。否决：
四页方案（SYS Σest ~20K 使 SYS→STEP 账面逼近 47.4K 上限、子签利用率低）、并入 FILE 组
（语义错位且抬高 FILE 稳态）。
**D2 Tab5 原生项持久化 = NVS**（取）。三键 `tab5.bright/vol/out`（kbd 不落 NVS，由
lights_lvl 派生）；main 初始化 `nvs_flash_init`。依据 ADR-009 §6「NVS 只留平台级」。否决：
扩 settings 尾段（破坏上游字节布局）、新 flash 对象（破坏 ADR-009 分区语义）、挪用 lights 位
（反目标）。**NVS 提交时机 = 延迟**（builder check 修订）：音量/亮度恰恰是播放中最常改的项，
而 NVS 页擦写同样会 stall 音频路径（ADR-009 §3 的擦写静音纪律适用一切 flash 写）——副作用
（LEDC/I2C）即时生效，NVS 落盘并入 D4 的同一静默窗口；`STORE SETTINGS SAVE` 一并 flush。
**D3 palette/theme 应用 = 运行时色表 + 强制重建**（取）。theme.h 的颜色 #define 收敛为运行时
`ui_theme_t`（默认=现行色板），`ui_theme_apply()` 供样式创建消费；切换时更新色表 → 销毁全部
已建页 → 重建当前页（页签条样式一并刷新），兄弟页下次懒建自然取新色。**内置 palette 的精确
映射**（builder check 修订，写死避免实施漂移）：

| theme.h 常量 | 内置 palette 时 | theme（side 有效且选中）时 |
|---|---|---|
| UI_BG | 固定 0x101418（上游 pal_bg 无 theme 即黑，同义） | theme.bg |
| UI_PANEL / UI_PANEL_HI / UI_PANEL_SEL | pal[0]/pal[1]/pal[1]+½ 档的**中性灰混合**（保留明度阶梯、染 palette 色相） | pal[0]/pal[1]/pal[1]+½（theme.c[] 同法） |
| UI_TEXT / UI_TEXT_DIM | 近白固定 / pal[2] 染灰 | theme.hi 派生 / pal[2] |
| UI_ACCENT | **pal[4]**（C_HI） | theme.hi |
| UI_ERROR | 固定 | 固定 |
| 轨道色 `ui_track_color` | 固定蓝/绿/黄/橙 | theme.trk[] |

MONO(4) 映射现行 Tab5 视觉（文档化：MONO=默认，其 pal 恰为灰阶、混合结果=现行色）。
否决：LVGL 主题切换器（不覆盖既有静态样式体系）、编译期多套（不能运行时切）。
**D4 设置写路径 = 并入 1s sections tick**（取）。`settings_dirty` 与 section/song 同门限
（停止+无发声+idle≥1.5s+gap≥5s）、同一次 `song_write_locked` A/B 提交（= 上游「quiet 时随
song 写」）；`STORE SETTINGS SAVE` / UI 动作在停止态可立即写（播放中拒绝并保留 dirty）。
**NVS 提交同窗口**（builder check 修订）：settings flash 写与 NVS flush 都在该 tick 的静默
窗口内完成，dirty 状态可观测（§9 的 `set_dirty=`/`set_nvs=`）。
**D5 字节层 = 逐字段 RMW、owned-bits 隔离**（取）。§8.1。ADR-009 §6 要求的「来源换真实
状态 + SONG_PANEL_DEF 同步」一并落（panel 恒 PANEL_DEFAULT 不变）。
**D6 SELFTEST 入口 = 一次性任务 + 每 boot 一次**（取）。chip 不在 LVGL 回调里跑（自检要拿
LVGL 锁 quiesce，会死锁）：置请求标志，由独立一次性任务（栈对齐 console 任务）执行。**完整
序**（builder check 修订，补停流）：`STORE AUTO OFF` → 停音频流（与 `ENG AUDIO OFF` 同一
内部入口；流式时自检会被拒）→ `sloop_core_selftest(NULL)` → 起流（`ENG AUDIO ON` 同义）→
autosave resume 路径恢复工作工程（比脚本口径更强，见 DI-20）→ `AUTO ON`。第二次进入回
`RESET FIRST`（DI-26 排雷）。**console `ENG SELFTEST` 行为完全不变**（脚本管理、无自动恢复
——`device-selftest.py`/`store`/`sequence`/`soak` 的既有门禁与 DI-20 口径不受影响）。
结果行显示 `SELFTEST <ran-failed>/<ran> PASS|FAIL`（计数来自 core 运行器，不写死全量 182）。
**D7 golden 确定性 = 运行期归零 + 结束重载**（取）。自检 setup 强制 `fx_lowcut=0/flt_reso=0`
（golden 基线口径），收尾 reload 按 settings 重放（与 song/DSU 同批）。这是今天不可达、
P4-E 起可达的正确性修复，不修则 lowcut ON 后 182 例必红。
**D8 ZOOM = ui.c 全局单对象大读数条**（取；builder check 修订归属）。zoom ON 且当前页有聚焦
旋钮时，屏幕层一条**全局共享** label（Montserrat 28，ui.c 持有、**首次 zoom ON 才懒建**，
页面区顶部）显示 `LABEL  VALUE`；无聚焦/zoom OFF 隐藏。覆盖全部 `knob_t` 页（参数五页 +
DSYN + FM6P）。否决：逐页 label（FM6P 只剩 44B tripwire，7 页 est 抬升连锁改 ADR-008/
inject 断言）；改旋钮本体字号（重建成本高、抖动大）。
**D9 LIGHTS → 键盘 RGB 直映射**（取）。lights_lvl 四档 → `0x03` 亮度 0/20/60/100 +
独立 `0x11` RGB=自定义 + `0x60` 起步长 4 的 B/G/R 颜色缓冲 = accent 降额（可视门禁人测）。
`tab5_keyboard` 组件新增 `tab5_keyboard_rgb_set()`（复用键盘总线既有互斥，避免与读键任务竞争）。
**D10 输出路由 = speaker-only（checkpoint 已降级）**。官方原理图证明扬声器功放与耳机 L
共用 LOUT1、LOUT2/ROUT2 悬空，原「LOUT1/R1 vs LOUT2/R2」切换假设不成立。实现只接受
speaker；`AUD OUT headphone` 回 `ESP_ERR_NOT_SUPPORTED` 且状态不变，SYSTEM 页显示 `N/A`。
后续若实现耳机路由，须改走 SPK_EN 功放使能并另测副作用（DI-39；VOLUME/LOWCUT 不受影响）。

## 6. UI 设计（SYSTEM 页）

页面区 1280×648（y 相对页区），单列节式布局（对象全部为**单对象可点击 label**，沿用
FILE/DSYN 页惯例；三区行 = `−  值  +` 一对象，左右 42px 为 ∓1 区）：

```
SCREEN ──────────────────────── (节标题，16 行高)
  COLOR   −  MONO          +     (GREEN/AMBER/CYAN/RED/MONO[/THEME <name>])
  ZOOM       OFF  (chip：点=切换)
  BRIGHT  −  100%          +     (10..100 步进 10；NVS)
LIGHTS ─────────────────────────
  LIGHTS  −  OFF           +     (OFF/LOW/MID/HIGH；键盘 RGB)
AUDIO ──────────────────────────
  VOLUME  −  70            +     (0..100 步进 5；NVS)
  OUTPUT  −  SPEAKER       +     (speaker-only；HEADPHONE 请求回 N/A，D10/DI-39)
  SPK LOWCUT  OFF (chip)
  FILTER RESO − OFF        +     (OFF/1..10)
SYSTEM ─────────────────────────
  APP <git describe>   SLOOP 2.8.5   (两行只读)
  [ SELFTEST ]  (chip)
  <状态行>
```

- est **10,240B**（≈35 个单对象 label；ZOOM 大读数条是 ui.c 全局对象，不计入本页），`UI MEM` 走既有 `delta ≤ est` 门禁。
- 键盘：↑/↓ 焦点环（跳过只读行）、←/→ 调值、ENTER=+1/切换；TAP 断言
  `-> SYS <row> value=<写后读读> status="<状态行>"`，chip 类 `-> SYS ZOOM value=ON|OFF`。
- 状态行单一映射 `theme.h` 的 `ui_sys_status_text()`（`OK`/`SAVED`/`RESET FIRST`/
  `SELFTEST 182/182 PASS|FAIL <例>`/`NO STORAGE`/`SAVE ERROR`）。
- 页面锁定态：无（设置页不依赖选中轨/引擎）。

## 7. 状态层与命令

- `CMD_SYS_BRIGHT{v}` / `CMD_SYS_VOLUME{v}` / `CMD_SYS_OUTPUT{v}`：平台副作用即时
  （brightness_set / set_out_vol / 路由寄存器，不取引擎锁、不受走带限制）+ 置 `nvs_dirty`
  （提交延迟到 §8.4 的静默窗口），投影 `app_state_t.sys`；返回码并入既有 status 体系
  （平台调用失败 → error）。
- `CMD_SETTINGS_SET{a=field, v}`：palette/lowcut/zoom/lights_lvl/reso 单字段写（引擎锁内改
  上游 static + 置 settings dirty；palette/theme 走 UI 重着色回调），投影
  `app_state_t.settings`（palette/lowcut/zoom/lights/reso/theme_on/theme_name/dirty）。
- `CMD_REC_CONFIG` 成功后追加置 settings dirty（bits9/10/31 随下次保存落盘；开机 boot 序列
  用 `rec_config` 重放）。
- `CMD_SELFTEST_RUN{}`：置请求标志（实际执行见 D6），状态行轮询结果。
- 每条产品写路径经 `state_apply`（console 是诊断执行器）——与既有纪律一致。

## 8. 字节层设计

### 8.1 `song_persist.h`（纯函数，host 同源）

- `sloop_settings_fields_t { palette, lowcut, zoom, lights_lvl, theme_on, rec_tempo,
  rec_count, rec_quant_off, flt_reso }` + side theme 读结果
  `{ valid, magic_ok, name[9], trk[4], c[5], bg, hi }`。
- `song_per_put_settings(payload, len, f)`：palette/lowcut/zoom 直写字段；lights word
  **RMW 只动 owned bits**（0..3/9/10/30/31），其余位原样；尾段存在（len ≥ 1,552+40）⇒ byte38
  bits2..5 RMW；尾段不存在且 reso≠0 ⇒ 追加 **40B 默认尾段**（设备侧由同 TU 的上游
  `midi_part_fill()` 生成——与上游 `settings_write`「非默认才写尾段」同语义；host 测试用
  fixture 传入）再打点；reso=0 不追加；panel/DSU/其余尾段字节/side 逐字节保留。
  `SONG_PANEL_DEF` 保持 PANEL_DEFAULT（panel 无 UI，§4 表）。
- `song_per_get_settings(payload, len, f)` + `song_per_side_theme(side, len, out)`（THM2 +
  CRC 校验；THM1 beta 36B 与坏 CRC 均 invalid）。
- canonical（无对象）与历史长度（84/88/1,552/1,584/1,588/1,592）的组装规则不变，只是来源
  从硬编码改为 §8.2 的 RAM 状态。

### 8.2 `store_tab5.c`（flash/锁/生效）

- RAM 生效副本 `s_set`（settings 字段 + side theme 缓存）：`sloop_store_init` 在
  song/DSU 装载之后读 settings → 填 `s_set` → **应用副作用**（palette/theme→UI 色表、
  lowcut/reso→fx statics、REC 三项→`rec_config`、lights_lvl→键盘 RGB）。
- `sloop_store_settings_set(field, v)`：停止/播放皆可改内存 + dirty（UI 色表即时重着色走
  sloop_ui 的重建入口）；落盘只走 §5 D4。
- `sloop_store_settings_save()`：停止态存储锁内 `song_write_locked()`（与 song/DSU 同一 A/B
  提交；上游 `settings_save` 同义）；`sloop_store_settings_reload()`：boot 与 `ENG SELFTEST`
  收尾（与 `song_reload`/`dsu_reload`/sample reload 同批——`fx_lowcut`/`flt_reso`/
  `rec_tempo`/`rec_count` 都在上游 `.bss`，restore 会回滚）。
- `sloop_core_selftest` 的 setup 增补：golden 运行期强制 `fx_lowcut=0/flt_reso=0`（D7）。

### 8.3 `STORE RAW END settings`（DI-32 收口）

上传暂存复用 `s_raw_stage`；END 时：读现有 side（st_side）→ `st_save3` 提交 payload + 原
side（没有则默认 80B）→ `settings_reload` 立即应用。不设 magic 防线（RAW 语义就是任意字节；
坏对象的后果= 上游一致：boot 落默认，reload/INFO 报 invalid）。配套 `STORE RAW SIDE <160 hex>`
单独写 80B side record（theme 注入的测试入口，写后同样 reload）。其余 STORE 子命令的
abort-before-act 约定不变。

### 8.4 NVS 与平台应用

- main 初始化 `nvs_flash_init()`；`sys_nvs_{get,set}` 三键（bright 默认 100 / vol 默认 70 /
  out 默认 speaker；kbd 由 lights_lvl 派生不落 NVS；`tab5.out` 保留平台输出状态，D10 降级后只接受 speaker）。
- **提交时机**（builder check 修订）：`set` = RAM 值 + 平台副作用 + `nvs_dirty`，**不写
  flash**；`nvs_dirty` 由 1s tick 的同一静默窗口（停止+无发声+idle+gap）flush，`STORE
  SETTINGS SAVE` 一并 flush。播放中改音量/亮度零 flash 写（xrun 口径不受影响）。
- boot 序列：display init 后 `brightness_set(nvs)`（替换现 `backlight_on()`）；audio open 后
  `set_out_vol(nvs)`；路由按 D10。硬复位前未等到窗口的改动会丢（与上游 `settings_later`
  同语义，可接受）。

## 9. console 诊断面（§5.4.1 注册表同步）

| 命令 | 语义 |
|---|---|
| `STORE SETTINGS [INFO\|SAVE]` | INFO：kind/len/side + `f=` 字段表 + theme 名/valid + dirty；SAVE：立即写（播放中 `playing`） |
| `DISP BRIGHT [0..100]` | 亮度读写（与 UI 同路径，NVS） |
| `AUD VOL [0..100]` / `AUD OUT [speaker\|headphone]` | 音量/路由读写（同路径） |
| `KBD RGB [off\|low\|mid\|high]` | 键盘灯档位（同路径，回显 applied 档位） |
| `UI system` | 第 15 页切页 |
| STATE SNAP 追加 | `sys_bright= sys_vol= sys_out= set_palette= set_lowcut= set_zoom= set_lights= set_theme= set_reso= set_dirty= set_nvs=` |

## 10. 测试策略

- **host（`seq-test.sh` persist 段扩展）**：`put/get_settings` 全字段往返；owned-bits 隔离
  （构造外部位=1 的 word，改 owned 后外部位逐位不变）；尾段 RMW（有/无尾段两分支按实现定案）
  与 byte38 位隔离；side theme 读（THM2 有效/THM1/坏 CRC）；SONG_PANEL_DEF 逐字节 =
  PANEL_DEFAULT；真实 `st_save3` 往返后 get 一致；canonical 构造的字段来自 RAM 状态（非 0
  值可往返）。
- **host（`state-test.sh` 扩展）**：`CMD_SYS_*` NVS 替身 + 越界钳位；`CMD_SETTINGS_SET` 字段
  路由（不越权改别的字段）、`CMD_REC_CONFIG` 后 set_dirty 置位；status 映射。
- **设备（新注入场景 `scripts/inject/sys.sh`，破坏性自归一）**：页构建/est/`UI MEM` 组行
  （SYS 单页组）；122px 组签截屏 + 像素抽检；COLOR 循环（截屏 accent 像素变化 + `STORE
  SETTINGS INFO` 一致 + 兄弟页重着色）；**BRIGHT/VOLUME 读回断言**（builder check 修订：
  `DISP BRIGHT`/`STATE SNAP sys_bright=` 读回、`set_nvs=1` → 停止过静默窗口 → `set_nvs=0`、
  硬复位后保持；**截屏像素前后不变是预期**——SCR 读帧缓冲、背光是 LEDC PWM，亮度视觉归
  人测）；LIGHTS→`KBD RGB` 回显；LOWCUT/RESO 写读（`STORE SETTINGS INFO`）+ **自检归零回归**
  （lowcut ON 时 `ENG SELFTEST` 仍 182/182，结束后 INFO 恢复 ON）；REC 三项改 → 硬复位保持；
  settings dirty 延迟写落盘（改值 → 过门限 → INFO saved、A/B 副本 hash 变化）；NVS 硬复位
  保持（bright/vol）；SEQ→SYS 有序对（构建日志 `池剩余` ≥4,096）；SELFTEST chip（182/182 +
  工作工程恢复 + 第二次 `RESET FIRST`）；`STORE RAW` settings 往返（注入构造的合法 PER3 +
  side theme，reload 后 INFO/COLOR 显示 THEME 名）。
- **soak**：`soak.py` 页面访问表加 SYSTEM（15 页/8 组），口径不变。
- **人测（外部门禁，列明）**：音量/路由听感（含耳机插入检测范围外的手动切换）、键盘 RGB
  目视、亮度视觉、palette 目检（截屏 + 人眼双保险）。

## 11. 容量账（ADR-005 §5 口径）

- 组签：8 组 → `min(176,(1280−32−216−7×8)/8)=122px`，8×122+7×8=1,032，右端 1,048 ≤ 1,064；
  子行不动（SYS 单页）。
- SYS est 10,240：**SEQ→SYS** `32,256+10,240=42,496` ⇒ 账面 58,016（余 5,104）；
  **SYS→STEP** `10,240+23,040=33,280` ⇒ 账面 49,000。**最坏有序对不变**（SEQ→PRESETS
  60,776、余 2,344）。FM6P 44B tripwire 等既有口径不动。
- `free ≥ est` 构建前检查、`build_ms ≤100ms`、组作用域往返 ≤128B 照旧。
- **ZOOM 大读数条**（D8，ui.c 全局单对象、zoom ON 首用懒建）：+≈768B 计入 zoom ON 时的
  瞬时占用；最坏对账面 60,776+768=**61,544B**（余 1,576B ≥0）。zoom OFF（默认）无此开销，
  页 est 与 FM6P 44B tripwire 不动。

## 12. 风险与开放问题

1. **D10 输出路由已降级为 speaker-only**——原理图排除原寄存器切换方案；后续耳机路由须走 SPK_EN，见 DI-39。
2. flt_reso 无尾段时的构造分支——**已定案**：追加上游 `midi_part_fill()` 的默认尾段（§8.1），host 测试固定
  「RMW / 追加 / NULL 不追加」三语义。
3. palette 变更的「销毁全部已建页」路径复用组切换的销毁序（先建后毁不变式）——需要在
  SYS 页内触发全局重建时走 `sloop_ui` 既有入口，不能绕过（防 DI-19 类回归；nav 场景复跑）。
4. 键盘 RGB 写与读键任务的并发——组件内互斥覆盖；`KBD RGB` 回显是机读断言上限，颜色正确
   性靠人测。
5. SELFTEST 一次性任务的栈/优先级取 console 任务同款（实现时对齐 main.c 注册值）。
6. DI-26（自检堆泄漏）不在本阶段修（P4-F 前置），`RESET FIRST` 守卫是缓解不是修复。
7. **全量自检后的音频重启异常（原 DI-40，2026-11-24 修复轮已关闭）**：根因不在 D6 的收尾序（停流 → 自检
   → 起流）或 D7 的归零/重载，而是 DI-26 的堆泄漏之后 `xTaskCreate` 把音频任务的栈落到 P4 的 LP SRAM
   （渲染慢 ~24 倍、每块 xrun）；修复在 HAL——首次启流用 `INTERNAL|DMA` 显式分配常驻 L2MEM 栈
   （ADR-007 新节）。**D6 / D7 决策不变、仍然有效**；D6 的「第二次进入回 `RESET FIRST`」是产品行为，
   取证用的 `SYS SELFTEST ARM`（R25）只是清每 boot 标记的测试注入，不改变它。

## 13. 交付物清单（供计划引用）

- 固件：`song_persist.h`（settings 段）、`store_tab5.c`（settings 存储/生效/自检钩）、
  `theme.h`/`ui.c`（运行时色表 + 122px 组签 + SYSTEM 页注册）、`page_system.c`（新）、
  `sloop_state`（命令/投影）、`tab5_keyboard`（RGB API）、`audio_stream.c`/`display.c`
  （NVS 应用点）、`console.c`（诊断命令）、`selftest.c`（D7 归零）。
- 脚本：`scripts/inject/sys.sh`、`soak.py` 页表、（host 测试扩展进既有 `seq/state-test.sh`）。
- 文档：AGENTS.md §3/§5、ADR-005（组表/预算 5.2 节）、ADR-009（§6 落地注记）、
  deferred-issues（DI-32 删除）、roadmap 状态行、verification-log + 证据包
  `docs/dev/verify/4-pe-<日期>/`。
