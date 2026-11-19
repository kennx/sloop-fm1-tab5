# SLOOP 固件调研（移植主体）

> 用途：sloop-fm1-tab5 项目上游固件参考
> 调研日期：2026-11-19 ｜ 上游版本：SLOOP 2.8.5（commit bac9c05）
> 仓库：<https://github.com/isod89/sloop-fm1> ｜ 许可：**GPL-3.0**（衍生移植必须同样 GPL-3.0 开源）
> 上游文档：[README.md](https://github.com/isod89/sloop-fm1/blob/main/README.md)（功能/操作）、[SLOOP.md]（手册，98KB）、[GUIDE.md]（指南，132KB）、[BUILDING.md]（构建）、[DUALCORE.md]（双核）、[RAM.md]（内存）

---

## 1. 定位与血统

SLOOP 把 M-VAVE FM-1 变成一台**四轨 live groovebox**：3 个合成器轨 + 1 个鼓机轨。基于 **Felucca**（Leo Kuroshita / Hügelton Instruments，同为 GPL-3.0）深度扩展而来，编辑器协议帧保留 Felucca 的 "FL" 头。

核心数字（2.8.x）：

| 项 | 值 |
|---|---|
| 轨道 | 4（1–3 合成器，4 鼓机），轨色蓝/绿/黄/橙 |
| 合成引擎 | 12：ANALOG、FM4、**FM6**（6-op，吃 DX7 patch）、PHASE（相位失真）、TRIO（三振荡）、DRAWBAR（音轮风琴）、VOICE（共振峰）、GRAIN（颗粒）、LOFI（芯片）、SAMPLE（采样）、**PHYS**（物理建模，DaisySP/Mutable Rings 系）、**NOISE** |
| 声音 | 153 工厂预置 + 32 用户槽 |
| 鼓 | 37 kits（1 采样声学 + 32 合成 + USR1–4 自采样 + SYN1–4 自合成），16 lanes/kit |
| 音序 | 每轨 64 步、独立 LEN/DIV（1/32–2 小节，多律同相）、chord≤4 音、TIE/SIDE、MPC swing 0–100、parameter locks（24/轨）、micro timing（±1/64 步）、fill 条件 |
| 歌曲 | 4 段 A–D + 链式播放 + SONG REC |
| FX | 每轨 INSERT + per-track 滤波（LP/BP/HP + 共振）+ 发送（合唱/延时/混响）；27 个 punch-in 效果；母带 DUST/DUCK/FILT/限制器；TEXTURE 底噪层 |
| MIDI | USB MIDI in/out + TRS MIDI IN；每轨可配通道；MIDI CC / LEARN / Bank Select / 时钟同步（USB 或 TRS）/ OUT=SEQ |
| USB 音频 | UAC1 输入 44.1/48kHz 16bit 立体声（母带输出） |
| 采样 | USR1–4 槽（各 80KB，约 7.4s），16 zone/槽，IMA ADPCM 4bit，支持 loop |

## 2. 硬件操作模型（移植 UI/按键映射的依据）

FM-1 面板：**27 键**（白 16 / 黑 11，带键灯）、**4 旋钮**、**MASTER / SELECT 双编码器**、顶排功能键 **FX · SEL · ENV · LFO · EDIT · GLO**、**HOME · SAVE · ARP · SEQ**、**PLAY · REC**、**OCT− / OCT+**、**ALGORITHM**（选轨）、**PRESETS**。

交互范式（SLOOP 的核心设计，Tab5 版须对等复刻）：

1. **层（layer）机制**：每个功能键是一层——**按住**它，16 个白键 + 4 旋钮换功能，屏幕显示映射；**单击**开它的页面；按住层键再点 HOME **锁定**层。
2. 层表（摘要）：
   - **FX**：27 punch-in 效果（白键）+ FILTER/DUST/DUCK 旋钮
   - **EDIT**：擦除音符/声音；黑键 = 鼓实时静音；EDIT+OCT± = undo/redo
   - **ARP**：roll（note repeat）
   - **SEQ**：16 步进写作；按住某步：LEVEL/VELOCITY、RATCHET、NUDGE、参数锁（PRESETS/ALGORITHM）、fill 条件（OCT+）
   - **SEL**：调性/音阶/和弦
   - **GLO**：1–4 静音、5–8 独奏、9 fill（按住）、10 fill 下小节、16 tap tempo；F#3 = MIDI LEARN
   - **SAVE**：1–4 播放段 A–D、5–8 存段、13 loop/song、14 SONG REC、16 链
3. **REC**：free 模式（无网格录音，按 REC 闭环）或 tempo 模式；overdub 叠录；按住 ~2s 清轨。
4. 屏幕是 240×240，字体 Terminus Bold 全大写；TRACKS 主屏 + 12 种可视化。
5. 菜单（按住 HOME）：SCREEN / LIGHTS / AUDIO / SYSTEM 四节。

> Tab5 版输入映射初步设想（开发时细化）：Tab5 Keyboard 70 键复刻 27 键 + 层功能键；触摸屏承担层页/钢琴卷帘/鼓网格（参照 Sloop Go 的页面模型）。

## 3. 源码架构（`firmware/`）

```
firmware/
├── app.ld, crt0.S        # JieLi 平台链接/启动（不移植，重写）
├── src/                  # 核心：纯 C，平台无关（除少量 HAL 调用）
├── hal/                  # FM-1 硬件抽象层接口（fm1_*.h，全部 inline → 移植点）
├── host/preview.c        # 宿主（PC/WASM）适配层 → 移植可行性证明
└── loader/               # OTA 更新加载器（Tab5 不需要）
```

### 3.1 `src/` 模块图（66 文件，按职责分组）

| 分组 | 文件 | 职责 |
|---|---|---|
| 内核/主循环 | `core.h`（27KB 公共头：参数表 P_*/G_*、fmt 枚举、数据结构）、`main.c`、`felucca.c`（编译开关）、`audio.c`（音频主循环）、`dsp.c`、`libc.c` | |
| 引擎 | `engines.c` + `eng_analog/digital(=FM4)/drawbar/fm6/formant/grain/lofi/noise/phase/phys/sample/slice/trio.c`、`fm6_core.c`（Dexed msfa 移植）、`phys_dsp.c`/`phys_symp.c`（物理建模） | 12 引擎，每引擎一个 `voice_render` 型接口 |
| 鼓 | `drums.c`（LANE_NOTE 表、kits）、`drum_synth.c`（dsnd_t 22B 合成鼓声）、 | |
| 声部/FX | `voice.c`（复音管理/窃取）、`fx.c`（70KB：INSERT/发送/母带/punch-in）、`punch.c`（27 效果） | |
| 音序 | `seq.c`（**160KB**，步进/录音/锁/nudge/fill/roll/erase）、`arranger.c`+`arranger_scene.c`（A–D 段与链）、`slicer.c` | |
| 参数/预置 | `params.c`（参数描述/范围）、`upreset.c`（用户预设银行）、`preset_trim.h` | |
| 项目/存储 | `project.c`（55KB，格式 FUN1–FUN5）、`storage.c`（A/B 扇区提交抽象） | |
| UI | `ui.c`/`ui_draw.c`/`ui_input.c`/`ui_layers.c`/`ui_menu.c`/`ui_song.c`/`ui_studio.c`/`ui_vis.c`、`gfx.c`、`lcd.c`、`icons.c`、`panel.c`（面板输入）、`splash.c` | **Tab5 版需替换为 LVGL/自绘 UI** |
| 编辑器协议 | `editor.c`（49KB 协议主实现）+ `editor_dsyn.c`/`editor_fm6.c`/`editor_live.c`、`usb.c`（52KB：USB MIDI/UAC/CDC 组合设备） | **Tab5 联动模式的协议对端；独立模式可内嵌复用为本地 IPC** |
| 平台杂项 | `midi_uart.c`（TRS MIDI IN）、`console.c`、`cpu1.c`（第二核）、`ota.c`/`recovery.c`/`bootguard.h`、`uac_fir.h`、`scratch.h` | |

### 3.2 HAL 接口（`hal/fm1_*.h`，Tab5 移植的对接面）

| 头文件 | 职责 | Tab5 对应 |
|---|---|---|
| `fm1_audio.h` | 音频缓冲输出、采样率、DMA 节拍 | I2S → ES8388（esp_codec_dev） |
| `fm1_input.h`（16.5KB） | 27 键 + 4 旋钮 + 功能键面板事件 | Tab5 Keyboard I2C + 触摸事件注入 |
| `fm1_lcd_hw.h` | 240×240 帧缓冲推送 | MIPI-DSI 帧缓冲 / LVGL |
| `fm1_flash.h` + `fm1_flash_id.h` | flash 读写/擦、A/B 对象、芯片识别 | NVS / SD 卡 / 分区表重设计 |
| `fm1_usb.h` | USB MIDI/UAC/CDC 组合设备 | **角色反转**：usb_host MIDI + UAC host |
| `fm1_uart.h` | TRS MIDI IN UART | 可选：Tab5 无 TRS MIDI（暂不移植） |
| `fm1_cpu1.h` + `fm1_cpu1.S` | 第二核启动/通信 | ESP-IDF 双核 FreeRTOS 任务 |
| `fm1_time/timer/irq/gpio/adc/sys/xip/guard/isr/vec/cc` | 平台细节 | ESP-IDF 原生替代 |

### 3.3 host build（移植可行性证明）

`firmware/host/preview.c` 把固件核心编到 PC（测试）和 WASM（网页音频预览，Emscripten）：
- `tools/build_preview.py` → `sloop-preview.wasm`，12 引擎逐样本对拍测试；
- `tests/run_tests.sh`：flash 存储/用户预置/MIDI 解析/DSP 渲染/4 轨混音/项目格式/SPLICER/回归套件（golden.txt 哈希 + CPU 预算）；
- **含义**：核心 DSP/音序代码与平台解耦良好，第三个平台目标（ESP-IDF）工程上是「再写一个 host」+ 替换 UI。

### 3.4 原平台构建（对照）

- 工具链：JieLi clang 4.0.1（pi32v2），AC79 SDK（wl82）取 3 个文件；
- 编译开关（`felucca.c`）：FELUCCA_FLASH / OTA / CDC / UAC / UART；
- 产物：`felucca.bin` + `loader/ota.bin` → `felucca.fwsc`。

## 4. 移植到 Tab5（ESP-IDF）要点清单

1. **许可**：GPL-3.0 传染——本项目固件源码须 GPL-3.0，并在仓库保留上游版权与 LICENSES/ 声明（上游另有 DaisySP/Mutable Rings/Dexed msfa 等子许可，见上游 LICENSING.md 与 LICENSES/）。
2. **取源策略**：以 git submodule / subtree 固定 isod89/sloop-fm1 上游 commit，`src/` 尽量不改或以补丁层维护，便于跟随上游演进（2.x 迭代很快）。
3. **新 HAL 后端**：按 3.2 表实现 `tab5_*.h` 等价物；音频块大小/采样率对齐上游假设（44.1kHz，块大小见 `audio.c`/`fm1_audio.h`，移植时确认）。
4. **UI 重写**：上游 UI（240×240、层模型）→ Tab5 1280×720 LVGL；保留「层 + 16 键」心智模型，触摸与实体键盘双通道输入。
5. **编辑器协议复用**：`editor.c` 系列在独立模式可作为本地状态服务（UI ↔ 引擎走同协议，方便与联动模式共用上层代码）——待设计阶段定案。
6. **存储重映射**：上游 flash 布局（0xA0000..0xEFFFF，A/B 对象）→ Tab5 分区/NVS/SD；备份对象模型（BK_LIST/BK_GET/BK_PUT 的 11 类对象）保持兼容以互通备份文件。
7. **算力**：pi32v2 双核 120MHz → P4 双核 360MHz，富余；注意上游有定点/查表优化与第二核分工（DUALCORE.md），先单核跑通再并行化。
8. **测试搬运**：host 回归套件（golden 哈希）可直接用于验证 Tab5 构建的引擎逐样本一致性。

## 5. 信息来源

1. isod89/sloop-fm1 仓库根 README.md / BUILDING.md（2026-11-19 抓取，bac9c05）
2. firmware/ 目录列表与 hal/ 头文件清单（GitHub API）
3. web/EDITOR_PROTOCOL.md（全文，见 `editor-protocol.md`）
4. SLOOP.md / GUIDE.md / DUALCORE.md / RAM.md 未逐字阅读，按需再查（上游仓库内）

> ⚠️ 模块职责由文件名/大小/协议文档交叉推断，未逐文件通读；进入移植开发前应精读 `core.h`、`main.c`、`audio.c`、`editor.c` 与目标引擎源码。
