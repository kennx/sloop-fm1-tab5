# Sloop Go 调研（联动模式的行为蓝本）

> 用途：sloop-fm1-tab5 项目参考——「两台设备同步操作」的目标体验即本 App 的体验
> 调研日期：2026-11-19 ｜ 上游版本：1.8.0 / versionCode 51（commit ed941b5；versionName 两处 README 不一致——根 README 记 1.8.0、android/README 记 1.7.25，以仓库代码为准）
> 仓库：<https://github.com/jahlib/sloop-fm1-go> ｜ 许可：GPL-3.0
> 注意：名字带 "go" 但**不是 Go 语言**——是 Kotlin + Jetpack Compose 的 Android 原生 App（"go" = 便携伴生）。

---

## 1. 定位

Sloop Go 是 FM-1/SLOOP 的原生 Android 编辑器：通过 Android `MidiManager` 以 **USB-MIDI SysEx 直连** FM-1，逐字节实现 SLOOP 编辑器协议，**编辑即时到达设备、设备操作实时反映到 App**——无 WebView、无 JS 桥。本项目 Tab5 的「联动模式」要对齐的就是这种双向同步体验。

## 2. 架构（android/app，包 com.sloop.go）

```
com.sloop.go
├─ proto/            协议层：从 sloop-fm1 仓库 web/EDITOR_PROTOCOL.md 逐字节移植
│  ├─ Protocol.kt    CMD 号、帧格式（F0 7D 46 4C … F7）、v14/字符串编解码、Reader
│  ├─ Requests.kt    请求构造 + replyMatches（回复匹配）
│  ├─ Replies.kt     回复/push 解析 + 数据类
│  └─ Fm6.kt         FM6(DX7) patch 模型、SysEx 导入导出
├─ midi/MidiEngine.kt  MidiManager 封装：枚举/开端口/SysEx 重组/USB 权限
├─ device/
│  ├─ DeviceController.kt  ★ 同步核心：一请求在飞队列、瞬时 SET 合并、WATCH/PING、
│  │                        push 分发（CHANGED/RELOAD/STEP_CHANGED/TRACK_CHANGED）
│  └─ DeviceState.kt       不可变 StateFlow 快照，UI 直接渲染
├─ audio/            采样页解码/麦克风采集、USB 录音机（WAV/FLAC）、压缩/限制器、
│                    频谱（Hann FFT→192 对数点）、波形缓存
└─ ui/               Compose 页面：Sound / Sequencer / Mixer / Device 等
```

## 3. 同步机制（本项目联动模式直接对标）

### 3.1 请求侧（App → FM-1）

- **单在飞请求**（协议硬性要求：设备只缓存一个入站 SysEx 帧）：FIFO 上限 16384 条，每条等回复（10–50ms）再发下一条。
- **合并策略**：音符/步进等离散编辑保序；同一旋钮/推子的中间值只留最新（coalesced instant SET）。
- 失败暂停队列 → Device → Retry 恢复；断开时未发编辑明确报告并丢弃；队列纯 RAM 不持久化。
- **Now / Late（Store 草稿）两模式**：Now 每次改动立即入队；Late 按轨存 RAM 草稿，SEND 时把整轨 64 步按序入队（含休止）。
- **LIVE 命令（77）驱动实时页**：PLAY/STOP、层键（mute/solo/fill/tap）、27 punch-in 效果垫、REC、undo/redo、ROLL、ERASE、图案工具、链、THROW、母带音量、鼓 TUNE/DECAY——**即 FM-1 面板按钮的完整遥控**（op 表见 `editor-protocol.md` §2「v12 LIVE」）。切换型请求只发一次，超时不重发。
- 音符不走 SysEx：LIVE 页用普通 MIDI 音符按轨通道发出（GLO > MIDI 配置；鼓走 DR MAP）。

### 3.2 状态侧（FM-1 → App）

- `WATCH 3` 开推送 + `PING` ~1s 保活（3s 无任何请求推送自动停）。
- push 事件：`CHANGED`（参数在设备上被旋钮/菜单改，20ms 合并取最新）、`RELOAD`（引擎/预置/项目加载后需重读 DESC/DUMP/步）、`STEP_CHANGED`（设备上步被录/清/编）、`TRACK_CHANGED`（非选中轨 level/pan/mute 变化，WATCH bit1）。
- 编辑器自己的写不会产生对应 push（固件区分来源），无需回环去重。
- `LIVE` 状态轮询 ~8Hz：50 字节快照（播放/REC/段落/链/静音独奏/CPU/每轨步位+电平表/母带/THROW/鼓 TUNE-DECAY）→ playhead 和 Play/Stop 按钮跟随真机。
- 断线：队列与草稿丢弃、编辑历史丢弃。

### 3.3 UI 状态模型

- `DeviceState` 不可变快照 + StateFlow 单向数据流；UI 渲染纯函数化——**值得 Tab5 沿用**（ESP-IDF 下可用事件组/消息队列 + 单一状态结构体）。
- 每页有 undo/redo RAM 历史（值级），断线丢弃；flash 写（Store/采样上传）不可撤销。

## 4. 功能页与联动能力清单（对 Tab5 联动模式的功能范围参考）

| 页 | 能力 | 协议命令 |
|---|---|---|
| Sound | 全部声音参数（ENV/LFO/EDIT/VOICE/FX/SCALE/ARP/PATTERN/GLOBAL/MASTER/DRUMS 分组旋钮）、引擎/kit/预置选择、INSERT、TEXTURE、MIDI 3 | DESC/GET/SET/DUMP/PRESET/NAMES |
| Live | 轨静音/独奏+电平表、FILL、THROW、27 效果垫、TAP、真机播放态 | LIVE(77) + TRACK_MIX |
| Sequencer | 鼓网格 16 lanes + 钢琴卷帘（缩放/框选/拖动/拉伸/克隆/量化 ±12 变调）、nudge/fill/参数锁、MIDI 文件导入导出、Now/Late | STEP_*/TRACK_STEP/DRUM_STEP/LOCK_*/MICRO_*/FILL_* |
| FM6 | 槽库、DX7 SysEx 导入导出、6 算子编辑器（算法链图） | FM6_GET/PUT/LIST/ERASE |
| Drum synth | SYN1–4 每音色 22 参数旋钮、试听 | DSYN_LIST/GET/PUT/STORE/PLAY |
| Song | 段落卡编排 A–D、循环、读/发 | BK_GET/PUT（settings 对象 48..83 字节） |
| Samples | USR1–4：文件/麦克风/FM-1 自身 USB 音频采入、Chop 切片（自动找 hit、吸附、适配槽）、归一化→22050Hz→IMA ADPCM→带进度上传 | SMP_BEGIN/WRITE/END/ERASE/INFO |
| Mixer | 轨条/推子/静音 | TRACK_MIX/SET |
| Recorder | FM-1 USB 音频录 WAV/FLAC（16/24/32f，压限器、频谱/波形/声像表） | UAC1 音频流（非 SysEx） |
| Device | 连接管理、固件信息、主题 | INFO/PING |

未移植（上游协议有而 App 未做）：projects、用户预设置银行、sample upload（⚠️ 上游两处 README 矛盾：根 README 把采样上传述为功能、android/README 列入未移植；固件侧 SMP_* 命令确实存在，App 实现状态待核）、备份/恢复、live playhead（部分）。

## 5. USB 接入细节

- 经系统 MidiManager 拿 USB-MIDI（一般无需裸 USB 权限）；另对 FM-1（VID 0x1209/PID 0x0001）best-effort 申请裸 USB 权限；USB 插入可自动拉起。
- MIDI 端口名 "Felucca" 自动优先，任意 MIDI 设备可选。
- 录音：`AudioRecord` 绑定 USB 输入设备（float 采集、UNPROCESSED 源）。

## 6. 对 Tab5 版的启示

1. **协议层与设备控制层可整体照搬设计**（ proto/ + device/ 的职责划分），用 C 在 ESP-IDF 重写；帧编解码在 `editor.c` 上游本就有 C 参考实现（Tab5 移植可直接对照）。
2. **单在飞请求 + 合并 + push 分发**是同步体验的骨架；ESP-IDF 下用消息队列 + 定时器实现 PING/WATCH 续期、LIVE 轮询。
3. **双模式归一**：Tab5 独立模式（本地引擎）与联动模式（远程 FM-1）如果共享同一「状态快照 + 编辑命令」抽象，UI 只写一份——上游协议天然适合（DESC 驱动的动态参数 UI）。
4. Tab5 做 USB host 需自实现的等价物：MIDI 设备枚举/匹配（1209:0001 或端口名）、SysEx 重组（USB-MIDI 事件包 → 字节流）、UAC1 音频下行采集（esp-usb host 组件）。
5. 断线/重连的清理策略（队列、草稿、undo 历史）需明确设计。

## 7. 信息来源

1. jahlib/sloop-fm1-go 根 README.md（2026-11-19 抓取，ed941b5）
2. jahlib/sloop-fm1-go android/README.md（架构与实现细节，同 commit）
3. isod89/sloop-fm1 web/EDITOR_PROTOCOL.md（协议权威来源）

> ⚠️ 本文基于上游两个 README；Kotlin 源码未逐行核对。实现 Tab5 版时如需细节（如 replyMatches 匹配规则、pack7 边界处理）应同时对照上游 Kotlin 源码与固件 `editor.c`。
