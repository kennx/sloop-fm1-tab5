# SLOOP 编辑器协议摘要（USB-MIDI SysEx）

> 用途：sloop-fm1-tab5 联动模式（Tab5 ↔ FM-1）的协议速查
> 整理日期：2026-11-19 ｜ 权威来源：[isod89/sloop-fm1 `web/EDITOR_PROTOCOL.md`](https://github.com/isod89/sloop-fm1/blob/main/web/EDITOR_PROTOCOL.md)（41KB，本文为其结构化摘要；实现时以原文为准）
> 固件端实现：`firmware/src/editor.c`（+ editor_dsyn/editor_fm6/editor_live.c）；参考实现：web 编辑器（editor.html）、Sloop Go（Kotlin）、tools/fm1_sample_upload.py

---

## 1. 帧格式与基本编码

- 请求/回复：`F0 7D 46 4C <cmd> <args...> F7`
  - `7D` 非商用 SysEx ID，`46 4C` = "FL"（Felucca 血统）
- **每个请求恰好一个回复**（同 cmd）；不认识的命令**不回复**（版本探测靠这一点）
- 所有数据字节 **7-bit**
- 设备同一时刻只缓存**一个入站帧** → 客户端必须**一请求一回复**（10–50ms）串行

| 编码 | 规则 |
|---|---|
| **v14**（数值） | 2 字节 LSB first，`value = (lo \| hi<<7) − 8192`，范围 −8192..8191 |
| string | ASCII，`00` 结尾 |
| scope | 0 = 选中轨参数（`P_*`，0..P_COUNT−1）；1 = 全局（`G_*`，0..G_COUNT−1） |
| track | 0..3（0–2 合成器轨，3 = 鼓轨） |
| engine | 0..NENGINES−1；鼓轨的 engine 字节 = NENGINES（无引擎无预置） |
| **pack7** | ≤7 字节一组，前缀 1 字节放各组员 bit7 |
| u35 | 5×7bit LSB first（备份长度/CRC 用） |

- 引擎参数 = `P_E0..P_E7`（P_COUNT−8..P_COUNT−1），含义随引擎变——**换引擎后重读 DESC**
- `INFO` 返回 NENGINES / P_COUNT / G_COUNT / NSTEP / P_E0，编辑器一切布局以此为准

## 2. 命令总表（按协议版本分组）

### v1 基础（Felucca 期）

| cmd | 名 | 请求 → 回复（要点） |
|---|---|---|
| 1 | INFO | — → 版本串、NENGINES、P_COUNT、G_COUNT、NSTEP、P_E0、引擎名×N、NTRK(4)、**协议版本号** |
| 2 | GET | scope,id → scope,id,v14 |
| 3 | SET | scope,id,v14 → 钳位后的值。SET 全局 G_ENGSEL = 换引擎（带默认值） |
| 4 | DUMP | — → engine,preset,P_COUNT×v14（选中轨）,G_COUNT×v14 |
| 5 | DESC | scope,id → fmt,min,max,def,label,unit[,枚举名×n]。fmt 表见 §5 |
| 6 | STEP_GET | idx → idx,n(0..4),note0..3,time(0 NOTE/1 TIE/2 REST),flags(1 accent/2 slide),vel[,lvl,hi,rat(v5)] |
| 7 | STEP_SET | 同上写入 → 写后读回。无 v5 字节则 lvl/rat 清 0 |
| 8 | PRESET | engine,preset → 应用预置（不动混音/图案/调性） |
| 9 | PROJECT | op(0 load/1 save/2 query),slot 0..3 → 写 flash 约 2s |
| 10 | NAMES | engine → 预置名列表 + 两个编辑页标题 |
| 11–15 | SMP_* | 采样槽写：SMP_BEGIN(slot 0..2→清头) / SMP_WRITE(offset≥512 且 256 倍数递增,pack7≤256B,4KiB 边界自动擦) / SMP_END(480B pack7 头) / SMP_ERASE(~1s) / SMP_INFO |

### v2 用户预置 + live 同步

| cmd | 名 | 要点 |
|---|---|---|
| 16 | UP_LIST | start,count(≤16) → 总槽 32、逐槽 used/engine/name |
| 17 | UP_GET | slot → used,engine,name,P_COUNT×v14,16×(note,flags) |
| 18 | UP_PUT | 同上写入 → rc(0/1 参数/2 flash/3 播放中)。写 flash ~1s |
| 19 | UP_STORE | slot,name（""=自动名 "ANALOG 07"）→ 存当前声 |
| 20/21 | UP_LOAD / UP_ERASE | 应用 / 擦除（rc 3 播放中） |
| 22 | **WATCH** | on=1 开推送；**on=3（v4）附加 TRACK_CHANGED**。回复同值 |
| 25 | PING | → 0（保活用，~1s 周期） |

### v3 轨道（四轨模型）

| cmd | 名 | 要点 |
|---|---|---|
| 27 | TRACK | 查询/选轨 → 选中轨,NTRK,逐轨(engine,preset,level,mute,armed)（v5 尾接 solo 掩码） |
| 28 | TRACK_MIX | 轨道 level(0..127)/mute（鼓轨 level = 全局 G_DRLVL） |
| 29 | TRACK_DUMP | 任意轨 P_COUNT×v14（无全局） |
| 30 | TRACK_STEP | 任意轨步读写（同 STEP_SET 形参） |

### v4 任意轨参数

| cmd | 名 | 要点 |
|---|---|---|
| 31 | TRACK_PARAM | track,id[,v14]：不改选中地读/写任意轨参数 |
| 32 | TRACK_CHANGED (push) | 非选中轨的 P_LEVEL/P_PAN/P_MUTE 变化（WATCH bit1 时） |

### v5 鼓 lanes / 步级别与栅（SLOOP 2.0）

| cmd | 名 | 要点 |
|---|---|---|
| 33 | DRUM_STEP | idx → on(16bit=lane 掩码,3×7bit),lvl(32bit=每 lane 2bit:0 NORM/1 GHOST/2 SOFT/3 HARD,5×7bit),rat(32bit=每 lane 2bit x1..x4,5×7bit)。set 整步替换 |

鼓 lane 音高表（LANE_NOTE，F3 起）：36 kick,35 kick2,38 snare,39 clap,42 hat,46 open hat,44 pedal,37 rim,40 snare2,43 low tom,48 hi tom,49 crash,51 ride,70 shaker,63 conga,56 cowbell。合成步的 lvl/rat 为每音符 2bit 打包进 8bit（高位进 hi）。

### v6 备份/恢复（SLOOP 2.3）

| cmd | 要点 |
|---|---|
| 34 BK_LIST | → 对象表 {id,len(u35),CRC32(u35)}：0 工作项目,1 设置,2..5 项目 A–D,6..7 预置银行,8 FM6 银行,9 SYN kits,10 MIDI 设置,11 主题,32..35 采样槽 USR1–4 |
| 35 BK_GET | id,offset u35,count(≤256) → pack7 数据 |
| 36 BK_PUT | 三段式 begin(id,len,crc)/data(offset 顺序)/commit，校验失败 rc2，播放中 rc3 |

### v7/v8 步微调与锁（SLOOP 2.4）

| cmd | 名 | 要点 |
|---|---|---|
| 37/38 | LOCK_GET / LOCK_SET | 轨道参数锁（≤24/轨， Elektron 式）；可锁参数见原文清单 |
| 39/40 | MICRO_GET / MICRO_SET | 每步 nudge −32..31（1/64 步），字节 +64 编码 |
| 41/42 | FILL_GET / FILL_SET | 每步 fill 条件 2bit（0 常规/1 仅 fill/2 无 fill），16 字节 pack7=19B |

### v9 FM6（SLOOP 2.4，沿用 Felucca 命令号 68–71）

| cmd | 要点 |
|---|---|
| 68/69 FM6_GET/PUT | target(0 轨/1 银行槽 0..26/2 工厂 0..7 只读),index,128B packed DX7 patch（本身全 7-bit，无 pack7） |
| 70/71 FM6_LIST / FM6_ERASE | 银行列出 / 擦除 |

### v10 SYN 鼓组合成（SLOOP 2.5）

| cmd | 要点 |
|---|---|
| 72–76 DSYN_LIST/GET/PUT/STORE/PLAY | SYN1–4 每音色 22 字节 `dsnd_t`（wave/src/pitch/fine/bend/…/level），part 0..15 单音色、16 名字、17 整 kit 自工厂派生；PLAY 试听 |

### v11 主题（SLOOP 2.7）：备份对象 11（36B `theme_t`，RGB565 五屏色+四轨色+背景色+CRC32）。

### v12 LIVE（SLOOP 2.7）——**遥控面板，本项目的核心命令**

命令 **77 LIVE**：`op [args]` → `op` + 50 字节实时状态快照。

| op | args | 动作（= FM-1 面板操作） |
|---|---|---|
| 0 | — | 只取状态（页轮询 ~8Hz） |
| 1 | 0/1/2 | STOP / PLAY / 按一下 PLAY |
| 2 | layer,key(0..15),down/up | 层键：6=GLO(1..4 mute,5..8 solo,9 fill 按住,10 fill 下小节,16 tap)、7=SAVE(1..4 播段,5..8 存段,13 loop/song,14 SONG REC)、5=SEL |
| 3 | fx 0..26,on/off | punch-in 效果 |
| 4 | — | REC |
| 5 | 0/1 | undo / redo |
| 6 | track,note|lane,level,down/up | ROLL（ARP+键；页用 note 0x80+ 不被 ARP 松开影响） |
| 7 | track,note,down/up | ERASE（EDIT+键） |
| 8 | track,tool 0/1/2,up/down | 图案 shift / 长度 ×2½ / 移调 |
| 9 | n,sections(≤8) | 链（SAVE+多键） |
| 10 | bits(1 rev/2 echo) | THROW |
| 11 | 0..126 或 127 | 母带电平；127 = 交还旋钮 |
| 12 | lane,TUNE+8,DECAY+4 | 鼓声 TUNE/DECAY |

状态快照（50B）：flags(播放/song/REC/fill…)、flags2、BPM v14、播放段/请求段、链、mutes/solos/REC 轨、选中轨、效果号、CPU%、song 位置、每轨 step/LEN/电平(8log2|peak|)、母带、THROW、16 鼓声 TUNE|DECAY。
**音符不走 SysEx**：LIVE 页音符用普通 MIDI 音符按轨通道发（鼓走 DR MAP）。

## 3. push 事件（设备 → 编辑器，WATCH 期间随时可来）

| cmd | 载荷 | 语义 |
|---|---|---|
| 23 CHANGED | scope,id,v14 | 设备上参数被改（旋钮/菜单/音序编辑）；20ms 每 (scope,id) 合并取最新；**编辑器自己的 SET 不产生** |
| 24 RELOAD | engine,preset[,track] | 引擎/预置/用户预置/项目被加载（含编辑器自己请求的加载）→ 重读 DESC/DUMP/步 |
| 26 STEP_CHANGED | index[,track] | 设备上步被录/清/编（编辑器自己的写不推）→ 重读该步（鼓轨含 DRUM_STEP） |
| 32 TRACK_CHANGED | track,id,v14 | 非选中轨 level/pan/mute 变（需 WATCH=3） |

推送规则：WATCH 后 **3s 无任何请求自动停**（PING ~1s 续期）；USB reset / host 消失即停；推送不阻塞回复（仅发送队列有空间时发）。回复匹配按 cmd（23/24/26 永不做回复号）。

## 4. 版本探测与兼容

- `INFO` 尾字节 = 协议版本（v5..v12；旧固件无字节）。不支持的新命令**无回复**——用超时+INFO 判别。
- `WATCH 3` → 回复 3 判 v4+。
- 参数计数随版本膨胀（P_COUNT 53→58→61→71；G_COUNT 32→33→45→50），**一切以 INFO 为准**，跨版本按计数映射（尾部 8 个是 P_E0..E7）。
- 项目格式 FUN1–FUN5 向下兼容转换；备份 JSON `{format:"sloop-backup", version:1, …}`。

## 5. fmt 枚举（DESC 返回）

| 值 | 名 | 值 | 名 | 值 | 名 |
|---|---|---|---|---|---|
| 0 INT | | 5 CUTOFF | | 10 NOTE | |
| 1 PCT | | 6 DB | | 11 ONOFF | |
| 2 BIPCT | | 7 SEMI | | 12 OCT | |
| 3 TIME | | 8 **ENUM**（DESC 附枚举名） | | 13 STEPS | |
| 4 LFOHZ | | 9 BPM | | 14 SWING（0..100→MPC 50%+v/4） | 15 FILT（−64..63：0 关/负 LP/正 HP） |

## 6. 采样槽二进制格式（SMP_* 上传的目标）

槽 = 80KB：头 512B + IMA ADPCM 数据。头（全部小端）：
`0x00 magic "FSMP"(0x504D5346) + u16 ver 1 + u8 zone 数 1..16`；`0x08 名字 8B`；`0x10 u32 数据长 + u32 CRC32(zlib)`；`0x20 起 16 zone × 28B`：u32 off/n/loop_start/loop_end/rate(Hz/44100×65536)、i16 root×16、ADPCM predictor@loop、u8 step_index@loop/lo/hi/looped(0 单发/1 循环/2 按住循环松开放尾)。
数据 IMA ADPCM 4bit 低半字节先，从 predictor 0 / step 0 起。参考上传器 `tools/fm1_sample_upload.py`。

## 7. 编辑器实现注意事项（原文 "Notes for the editor"）

1. **一请求一回复**，等 10–50ms 再发下一帧。
2. 跟随设备：WATCH+PING；不支持 push 的旧固件退化为 300–500ms 轮询 DUMP。
3. 端口：MIDI 名 **"Felucca"**（USB **1209:0001**）；同一 USB 设备另有 UAC1 音频输入（44.1/48kHz 立体声），录音与 MIDI 并行不冲突。
4. 安全：写 flash 的命令（PROJECT save / SMP_* / UP_PUT/STORE/ERASE / BK_PUT 银行）在播放中回 rc 3（flash 擦除会静音 ~50ms）。
5. **绝不发送**更新协议帧（`F0 22 24 35 …` / `00 59 …`）。

## 8. Tab5 集成视角备注

- Tab5 为 USB host：枚举匹配 VID 1209:PID 0001 或 MIDI 端口名 "Felucca"；USB-MIDI 事件包（每 4 字节，cin 0x4 SysEx 起始/继续 0x5/结束 0x6/0x7）需重组为完整 SysEx 帧。
- ESP-IDF 组件：esp-usb host 栈（usb_host MIDI 类目前需自行处理或用 tinyusb host MIDI；UAC 下行采集用 esp-usb 的 UAC host 组件）——开发时以当期组件 registry 为准。
- 时序适配：单在飞请求模型与 push/轮询三路并发，建议独立协议任务 + 状态快照（参照 Sloop Go 的 DeviceController/DeviceState）。
- 独立模式下若把 `editor.c` 内嵌为本地服务，Tab5 UI 与本地引擎可复用同一协议栈（设计阶段定案）。
