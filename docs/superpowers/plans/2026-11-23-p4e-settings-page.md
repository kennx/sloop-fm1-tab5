# P4-E 设置页（SYS 组 / SYSTEM 页 + 设置持久化）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把上游 HOME 菜单的设置面移植成 Tab5 的 SYS 组 / SYSTEM 单页，设置按上游 `OBJ_SETTINGS` 字节格式持久化（Tab5 原生项走 NVS），重启保持，并修掉自检对 `fx_lowcut`/`flt_reso` 的 golden 确定性缺陷。

**Architecture:** 字节层仍是 `song_persist.h` 纯函数（host 同源）；生效副本与 RMW 编排在 `settings_tab5.c`（与 `store_tab5.c` 同 TU，直接触碰上游 statics）；平台控制（亮度/音量/路由/NVS/键盘 RGB）放 `sloop_hal_tab5` + `tab5_keyboard`；每条产品写路径经 `sloop_state` 命令；UI 新增第 8 组单页，颜色 #define 收敛为运行时色表。

**Tech Stack:** ESP-IDF 5.5.5 / LVGL / FreeRTOS / esp_codec_dev(ES8388) / NVS / pyserial 注入脚本 / gcc host 单测。

**Spec:** `docs/superpowers/specs/2026-11-23-p4e-settings-page-design.md`

## Global Constraints

- `third_party/sloop-fm1/**` 只读，MANIFEST 233/233；适配代码只放 `firmware/components/sloop_core/src/*_tab5.c`（在 `selftest.c` 末尾 include）。
- 上游 settings 字节布局不改：只 RMW owned 字段（palette/lowcut/zoom、lights word bits 0..3/9/10/30/31、MIDI 尾段 byte38 bits2..5）；panel/其余位/尾段其余字节/side 逐字节保留；`SONG_PANEL_DEF` 恒 = PANEL_DEFAULT。
- NVS 只放平台级三键（bright 默认 100 / vol 默认 70 / out 默认 speaker）；**NVS 提交延迟**到 1s tick 的静默窗口（停止+无发声+idle≥1.5s+gap≥5s），播放中改音量/亮度零 flash 写。
- 不扩 LVGL 池（64KB）；每页构建前 `free ≥ est`；SYSTEM est 10,240B；组签 `UI_GROUP_W` 142→122（8 组）；FM6P 44B tripwire 不动。
- console `ENG SELFTEST` 行为不变；自检运行期强制 `fx_lowcut=0/flt_reso=0`，收尾 `sloop_store_settings_reload()`。
- 走带中拒绝一切 flash 擦写类动作（settings save / RAW END）；擦写不持引擎锁、不进音频路径。
- 提交格式（AGENTS.md §7.2）：`<type>(<模块>): 中文摘要` + Body 四标签 `[Motivation]/[Change_Set]/[Impact_AST]/[Infra_State]`；fix 另加 `[Root_Cause]/[Blast_Radius]`。每任务 review clean 后 push。
- 门禁（AGENTS.md §5.5）：`scripts/build.sh && scripts/build.sh` 第二遍 0 警告；host：`seq-test.sh`（含 persist 段）/`state-test.sh`/`knob-test.sh`/`synth-test.sh`/`host-test.sh`；设备：`ENG SELFTEST` 182/182、注入场景、截屏 + 像素抽检（无图像查看工具，人眼读图外置）。

## Review Focus

1. **palette 切换发生在走带中 / 与 `KEY tab` 同拍**：重建全部已建页期间输入路径必须始终有活页、无 fatal、不泄漏。→ Task 5 Step 7（`nav` 场景复跑 + 走带中切色 pool_free 往返）。
2. **NVS 键缺失/损坏（首次启动、NVS 被擦）**：必须落默认 100/70/speaker 且不崩。→ Task 3 Step 6（`SYS NVS ERASE` + 硬复位后读回默认）。
3. **流式播放中连续快速改亮度/音量**：零 xrun、零 flash 写（`set_nvs` 保持 1 直到窗口）。→ Task 6 Step 5（走带中 20 次 ± + `ENG PERF xrun` 不变）。
4. **无 MIDI 尾段的对象上设 `flt_reso`，再被上游 `midi_part_read` 读回**：尾段必须按上游 `midi_part_fill` 默认布局生成且合法（CH 不为 0）。→ Task 1 Step 3 用例 (c)。
5. **SELFTEST chip 在走带中 / REC ARMED / dirty 窗口内被按**：页面守卫拒绝（`STOP FIRST`），不发请求、不停流。→ Task 7 Step 4。

---

### Task 0: 基线与分支

**Files:** 无代码改动。

- [ ] **Step 1: 从 dev 拉 `feat/phase4e`**

Run: `git checkout dev && git pull --ff-only origin dev && git checkout -b feat/phase4e`
Expected: HEAD = dev 顶端，工作树干净。

- [ ] **Step 2: 基线门禁**

Run: `scripts/build.sh && scripts/build.sh && scripts/seq-test.sh && scripts/state-test.sh && scripts/knob-test.sh && scripts/synth-test.sh`
Expected: 全部退出码 0；第二遍构建 0 警告。记录输出到 `/tmp/p4e_baseline_*.log`。

### Task 1: 字节层 — settings 字段 RMW（`song_persist.h`）

**Files:**
- Modify: `firmware/components/sloop_core/src/song_persist.h`（文件尾新增「settings 字段」节）
- Test: `scripts/persist-host-test.c`（由 `scripts/seq-test.sh` 编译运行）

**Interfaces:**
- Consumes: 既有 `song_per_build()`/`song_per_put_dsu()`/`song_side_*()`、`st_crc32`（storage.c，host 测试已链接）。
- Produces:
  ```c
  typedef struct { uint32_t palette, lowcut, zoom, lights_lvl, theme_on, rec_tempo, rec_count, rec_quant_off, flt_reso; } song_settings_t;
  typedef struct { uint8_t c[5][3], trk[4][3], bg[3], hi[3]; char name[9]; } song_theme_view_t; /* RGB888 各 3 字节 */
  /* 在 song_per_build/put_dsu 之后调用；返回新 len（可能 +40 追加尾段）。tail_def = 40B 上游 midi_part_fill 默认尾段（NULL ⇒ 永不追加） */
  uint32_t song_per_put_settings(uint8_t *payload, uint32_t len, uint32_t cap, const song_settings_t *f, const uint8_t *tail_def);
  bool song_per_get_settings(const uint8_t *payload, uint32_t len, song_settings_t *out);
  bool song_per_side_theme(const uint8_t *side, song_theme_view_t *out); /* THM2 magic + CRC36；THM1/坏 CRC ⇒ false */
  ```

- [ ] **Step 1: 写失败测试（`persist-host-test.c` 追加 6 个用例）**

  - (a) `settings_canonical`：无对象 → `song_per_build` → `put_dsu` → `put_settings(f={palette 2, lowcut 1, zoom 1, lights_lvl 3, theme_on 1, rec_tempo 1, rec_count 0, rec_quant_off 1, flt_reso 5}, tail_def=40B fixture)`；断言 len==1,592、`word(payload+4)==2`、`+8==1`、`+12==1`、word@84 的 bits 0..3==3 / 9==1 / 10==0 / 30==1 / 31==1、其余位==0、尾段 byte38 bits2..5==5、尾段其余字节==fixture。
  - (b) `settings_foreign_bits_kept`：构造合法 PER3（len 1,592，word@84 = 0xFFFFFFFF，尾段 byte38 = 0xFF，palette@4 = 9，arrangement@48 任意非零）→ `put_settings(f={palette 4, lights_lvl 0, theme_on 0, rec_* 0, reso 0})`；断言 palette@4==4、word@84 == 0xFFFFF0F0 & ~(1<<9|1<<10|1<<30|1<<31) 的精确值（外部位 4..8/11..29 全保持）、byte38 == 0xC3（bits2..5 清零、其余保持）、48..83 arrangement 逐字节不变、84..87 不变。
  - (c) `settings_reso_appends_tail`：合法 PER3 len 1,552（无尾段）→ reso=0 ⇒ len 仍 1,552；reso=7 ⇒ len==1,592、尾段==tail_def 且 byte38 bits2..5==7；tail_def==NULL 且 reso=7 ⇒ len 仍 1,552（不追加、不报错）。
  - (d) `settings_roundtrip`：(a)(b)(c) 各用 `song_per_get_settings` 读回，字段逐项相等（reso 读回 ≤10 钳位）。
  - (e) `side_theme_read`：构造 80B side：magic `0x324D4854`("THM2")、name "MYTHEME\0"、c/trk/bg/hi 任意、crc=`st_crc32(rec, 36)` → `song_per_side_theme` 返回 true、name/c/trk/bg/hi 逐项相等；magic "THM1" ⇒ false；改 1 字节不更新 crc ⇒ false；全 0xFF ⇒ false。
  - (f) `settings_real_st_save3`：(b) 的 payload 经真实 `st_save3` 往返（含 side）后 `get_settings` 与写前一致、SONG_PANEL_DEF 16..47 逐字节不变。

- [ ] **Step 2: 跑测试确认失败**

Run: `scripts/seq-test.sh`
Expected: persist 段 FAIL（`song_per_put_settings` 未定义 → 编译错）。

- [ ] **Step 3: 实现三个函数**

  `song_per_put_settings`：直写 palette@4/lowcut@8/zoom@12；word@84 = `(old & ~(mask)) | (owned)`，mask = bits 0..3|9|10|30|31；`len >= 1,552+38+1` ⇒ byte[len-40+38] = `(b & ~0x3C) | (clamp(reso,10) << 2)`；`len == 1,552 && reso != 0 && tail_def && cap >= 1,592` ⇒ memcpy 40B 后打点、len += 40。`song_per_get_settings` 按 kind（`song_per_kind`）取字段；PER1/PER2（40/48B）只读 palette（其余 0）；len 不足 88 时 word=0。`song_per_side_theme`：magic@0 == THM2、crc@36 == `st_crc32(side, 36)`，RGB565 → 3 字节展开（`(v>>11)&31 → *255/31`，绿 6 位、蓝 5 位同法）。

- [ ] **Step 4: 跑测试确认通过**

Run: `scripts/seq-test.sh`
Expected: persist-host PASS，其余段不变。

- [ ] **Step 5: 提交**

```bash
git add firmware/components/sloop_core/src/song_persist.h scripts/persist-host-test.c
git commit  # feat(components/sloop_core): 增加 settings 字段的 RMW 字节层与 side theme 读取
```

### Task 2: 存储层 — 生效副本 / 延迟写 / 自检重载 + `STORE SETTINGS`

**Files:**
- Create: `firmware/components/sloop_core/src/settings_tab5.c`
- Modify: `firmware/components/sloop_core/src/selftest.c`（末尾 include；自检 setup 归零 + 收尾 reload）
- Modify: `firmware/components/sloop_core/src/store_tab5.c`（`song_write_locked` 调 put_settings；`sections_tick` 成功后清 settings dirty；`sloop_store_raw_end` 增 settings 分支；`sloop_store_init` 末尾 reload）
- Modify: `firmware/components/sloop_core/include/sloop_store.h`（公共 API）
- Modify: `firmware/main/console.c`（`STORE SETTINGS SET <field> <v>|INFO|SAVE`；`STORE RAW END settings`）

**Interfaces:**
- Consumes: Task 1 的三个纯函数；同 TU 的上游 statics `fx_lowcut`、`flt_reso`、`rec_tempo`、`rec_count`、`rec_quant_off`、`midi_part_fill()`、`midi_part_default()`。
- Produces（`sloop_store.h`）:
  ```c
  typedef enum { SLOOP_SET_PALETTE, SLOOP_SET_LOWCUT, SLOOP_SET_ZOOM, SLOOP_SET_LIGHTS, SLOOP_SET_RESO, SLOOP_SET_THEME_ON, SLOOP_SET_FIELD_COUNT } sloop_set_field_t;
  #define SLOOP_SETTINGS_PALETTES 5u
  typedef struct { song_settings_t f; bool theme_valid; char theme_name[9]; bool dirty; } sloop_settings_t;
  const sloop_settings_t *sloop_store_settings(void);            /* 生效副本（存储未就绪 ⇒ 全零 + theme_valid=false） */
  esp_err_t sloop_store_settings_set(sloop_set_field_t field, uint32_t v); /* 改内存 + apply + dirty；冻结回 INVALID_STATE；palette 钳 0..4、reso 钳 0..10、lights 钳 0..3、bool 钳 0/1 */
  esp_err_t sloop_store_settings_save(void);                      /* 停止态立即写；播放中 INVALID_STATE；成功清 dirty */
  esp_err_t sloop_store_settings_reload(void);                    /* 以 flash 为准重读 + apply；boot/自检收尾调用 */
  bool sloop_store_settings_dirty(void);
  void sloop_store_settings_dirty_mark(void);                     /* REC 配置变更用 */
  ```

- [ ] **Step 1: 设备侧可测的失败断言（先写场景草稿，在 `inject.py` 的 `run_sys` 里分段落地；本任务只建骨架 ①）**

  `scripts/inject/inject.py` 新增 `run_sys(s)`：① 归一（`STORE AUTO OFF`、停走带、`ENG AUDIO ON`）→ `STORE SETTINGS INFO` 解析 `palette= lowcut= zoom= lights= theme_on= reso= rec_tempo= rec_count= rec_quant= dirty= theme=<name|none>` → `STORE SETTINGS SET palette 2` ⇒ INFO `palette=2 dirty=1` → `STORE SETTINGS SAVE` ⇒ `STORE SETTINGS SAVED len=<n> side=<0|1>`、`dirty=0` → esptool 硬复位 → INFO 仍 `palette=2`。`scripts/inject/sys.sh` 入口（复制 `synth.sh` 骨架，`--scenario sys`），`SCENARIOS` 表加 `"sys": run_sys`。

- [ ] **Step 2: 跑 `sys.sh` 确认失败**

Run: `bash scripts/inject/sys.sh`
Expected: ① 在 `STORE SETTINGS INFO` 处 `STORE ERR`/未知命令 ⇒ FAIL。

- [ ] **Step 3: 实现 `settings_tab5.c` + 接线**

  - `.noinit` 的 `static sloop_settings_t s_set`（自检快照窗口外）+ `static uint8_t s_midi_def[40]`。`apply_locked()`：`fx_lowcut = f.lowcut != 0; flt_reso = f.flt_reso; rec_tempo = f.rec_tempo; rec_count = f.rec_count; rec_quant_off = f.rec_quant_off;`（引擎锁内）。
  - `reload`：存储锁内 `st_load(OBJ_SETTINGS)` + `st_side_load` → `song_per_get_settings`/`song_per_side_theme` → palette ≥ 5 归 4（上游 `settings_init` 同法）→ `apply_locked`；失败/empty 落全零 + 默认；返回 ESP_OK。
  - `set`：`s_frozen` ⇒ INVALID_STATE；`!sloop_store_ready()` ⇒ INVALID_STATE；钳位后写 `s_set.f[field]`、`apply_locked`、`dirty=true`、`ESP_OK`。
  - `song_write_locked`：`song_per_build` 后在**同一引擎锁内** `len = song_per_put_settings(raw, len, cap, &s_set.f, s_midi_def)`；`s_midi_def` 在 init/reload 时 `midi_part_fill(s_midi_def)` 一次（只取默认布局）。
  - `sections_tick`：门限内 `song_write_locked` 成功 ⇒ `s_set.dirty=false`；失败保留。
  - `sloop_store_init`：`dsu_reload` 之后、READY 置位之前调 `settings_reload()`。
  - `selftest.c`：`selftest_snapshot_init()` 成功后立刻 `fx_lowcut = 0; flt_reso = 0;`（D7）；收尾 `sloop_store_dsu_reload()` 之后调 `sloop_store_settings_reload()`（失败只告警）。
  - `sloop_store_raw_end`：`strcmp(obj,"settings")==0` ⇒ 存储/走带/冻结守卫同 project → `st_side_load` 读现有 side（无/全 FF ⇒ `song_side_default`）→ `st_side=side; st_side_len=80; st_save3(OBJ_SETTINGS, s_raw_stage, len, 0,0,0,0)` → `settings_reload()`；回显 `STORE RAW END settings 0 saved len=<n>`（console 侧）。
  - console：`STORE SETTINGS` 无参/INFO 打印上述字段行；`SET <field> <v>` 直调 `sloop_store_settings_set`（诊断执行器，回显写后值 + `dirty=`）；`SAVE` 回 `STORE SETTINGS SAVED len=<n> side=<0|1>` / `STORE ERR playing|no_storage|invalid|save`。

- [ ] **Step 4: 构建 + 设备验证**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh && bash scripts/inject/sys.sh`
Expected: 第二遍 0 警告；① 全 PASS（含硬复位后 palette=2）。

- [ ] **Step 5: 自检 golden 回归（D7）**

Run: 串口 `STORE SETTINGS SET lowcut 1` → `STORE SETTINGS SET reso 6` → `STORE SETTINGS SAVE` → `python3 scripts/device-selftest.py` → `STORE SETTINGS INFO`
Expected: 182/182；INFO `lowcut=1 reso=6`（reload 恢复）。把回显存 `firmware/build/verify/p4e-task2-selftest-lowcut.log`。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_core/src/settings_tab5.c firmware/components/sloop_core/src/selftest.c firmware/components/sloop_core/src/store_tab5.c firmware/components/sloop_core/include/sloop_store.h firmware/main/console.c scripts/inject/inject.py scripts/inject/sys.sh
git commit  # feat(components/sloop_core): 增加 settings 生效副本、延迟写与自检后重载
```
（console 与 inject 可按 §7.1 拆成 `feat(firmware/main)` / `test(scripts/inject)` 两提交。）

### Task 3: HAL 平台控制 — 亮度 / 音量 / 路由 / NVS + 键盘 RGB + console

**Files:**
- Create: `firmware/components/sloop_hal_tab5/src/sys_settings.c`
- Modify: `firmware/components/sloop_hal_tab5/include/sloop_hal.h`、`CMakeLists.txt`
- Modify: `firmware/components/sloop_hal_tab5/src/audio_stream.c:17,81`（音量取 settings）
- Modify: `firmware/components/tab5_keyboard/include/tab5_keyboard.h`、`src/tab5_keyboard.c`（`kb_write_reg` 复用）
- Modify: `firmware/main/display.c:313`、`firmware/main/main.c`（初始化顺序）、`firmware/main/console.c`（`DISP BRIGHT`/`AUD VOL`/`AUD OUT`/`KBD RGB`/`SYS NVS ERASE`）
- Modify: `firmware/components/sloop_core/src/store_tab5.c`（静默窗口末尾调 `sloop_hal_settings_flush()`）

**Interfaces:**
- Produces（`sloop_hal.h`）:
  ```c
  esp_err_t sloop_hal_settings_init(void);          /* nvs_flash_init + 读三键（缺失⇒默认 100/70/0）+ 返回；不应用 */
  uint8_t  sloop_hal_settings_bright(void);         /* 10..100 */
  uint8_t  sloop_hal_settings_volume(void);         /* 0..100 */
  uint8_t  sloop_hal_settings_output(void);         /* 0 speaker / 1 headphone */
  bool     sloop_hal_settings_nvs_dirty(void);
  esp_err_t sloop_hal_settings_set_bright(uint8_t v);  /* 钳位 + LEDC 立即 + nvs_dirty */
  esp_err_t sloop_hal_settings_set_volume(uint8_t v);  /* 钳位 + esp_codec_dev_set_out_vol(活句柄，未开流时只存) + nvs_dirty */
  esp_err_t sloop_hal_settings_set_output(uint8_t v);  /* 路由寄存器（D10；不可实现 ⇒ ESP_ERR_NOT_SUPPORTED 且不改状态） */
  void     sloop_hal_settings_flush(void);             /* nvs_dirty ⇒ nvs_set+commit（不持任何锁；由 1s tick 静默窗口调用） */
  void     sloop_hal_settings_erase(void);             /* 诊断：删三键（下一次 boot 落默认） */
  ```
  `tab5_keyboard.h`: `esp_err_t tab5_keyboard_rgb_set(uint8_t level, uint8_t r, uint8_t g, uint8_t b); /* level 0..3 ⇒ 0x03 亮度 0/20/60/100；独立 0x11 RGB=自定义；0x60 起步长 4 的 B/G/R/保留颜色缓冲（2026-11-23 按 Internal-FW 修正） */`

- [ ] **Step 1: console 失败断言**

Run（烧录当前固件后串口）: `DISP BRIGHT` / `AUD VOL` / `KBD RGB low`
Expected: 未知命令。

- [ ] **Step 2: 实现 HAL 模块 + 接线**

  - NVS namespace `tab5`，键 `bright`(u8)/`vol`(u8)/`out`(u8)；`nvs_flash_init` 失败 `ESP_ERR_NVS_NO_FREE_PAGES/_NEW_VERSION_FOUND` ⇒ erase+init 重试一次。
  - `display.c`：`bsp_display_backlight_on()` → `bsp_display_brightness_set(sloop_hal_settings_bright())`；`main.c` 把 `sloop_hal_settings_init()` 放在 display init **之前**。
  - `audio_stream.c`：`AUDIO_OUT_VOL` → `sloop_hal_settings_volume()`；`sloop_hal_settings_set_volume` 在 `codec != NULL` 时立即 `esp_codec_dev_set_out_vol`。
  - 路由（D10，2026-11-23 按官方原理图修正并降级）：NS4150B 扬声器功放与耳机左声道共用 LOUT1，LOUT2/ROUT2 未连接，不能按原计划用 `0x2E..0x31` 分离两路。保持 speaker-only；`headphone` 回 `ESP_ERR_NOT_SUPPORTED` 且状态不变，DI-39 记录后续 SPK_EN 方向。HAL 仍用 `i2c_master_transmit` 裸读 `0x2E..0x31` 作 `AUD OUT applied` 的回读校验；不实现原 speaker/headphone 寄存器互斥写。
  - `tab5_keyboard_rgb_set`：与 `tab5_kb_poll` 同一执行上下文/互斥（若 poll 在另一任务且无锁，加 `s_bus_mutex` 包住 `kb_write_reg` 的所有调用者）。
  - console：`DISP BRIGHT [v]`、`AUD VOL [v]`、`AUD OUT [speaker|headphone]`、`KBD RGB [off|low|mid|high]`（无参读回，有参经 `sloop_hal_settings_set_*`/`tab5_keyboard_rgb_set`，回显 `applied=<v>`）；`SYS NVS ERASE`（诊断，删键，回 `SYS NVS ERASED`）。
  - `store_tab5.c` 的 `sloop_store_sections_tick`：每次静默窗口（无论是否写 settings）末尾调 `sloop_hal_settings_flush()`。

- [ ] **Step 3: 构建烧录 + 默认值读回**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh && scripts/flash.sh --monitor`（看启动日志后 Ctrl-] 退出）→ 串口 `DISP BRIGHT` / `AUD VOL` / `AUD OUT`
Expected: 0 警告；`DISP BRIGHT applied=100`、`AUD VOL applied=70`、`AUD OUT applied=speaker`；启动日志无 fatal；`ENG AUDIO ON` 后 `AUD TONE 440 500` 可出声（人耳）。

- [ ] **Step 4: NVS 缺失路径**

Run: 串口 `DISP BRIGHT 30` → `AUD VOL 20` → 等停止态 ≥6s（flush）→ 硬复位 → `DISP BRIGHT`/`AUD VOL` → `SYS NVS ERASE` → 硬复位 → 再读
Expected: 复位后 `30`/`20`；擦除后回 `100`/`70`。存 `firmware/build/verify/p4e-task3-nvs.log`。

- [ ] **Step 5: 键盘 RGB**

Run: `KBD RGB high` → `KBD RGB off`
Expected: 回显 `applied=3`/`applied=0`；键盘两颗 LED 亮/灭（人眼，外部门禁）；`KEY STATS` 事件计数在 RGB 写入期间按 5 键仍正常递增。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_hal_tab5 firmware/components/tab5_keyboard firmware/main/display.c firmware/main/main.c firmware/main/audio_tone.c firmware/main/console.c firmware/components/sloop_core/src/store_tab5.c
git commit  # feat(components/sloop_hal_tab5): 增加亮度/音量/输出路由/NVS 平台设置与键盘 RGB
```
（按模块拆 2–3 个提交：hal+keyboard / main 接线 / console。）

### Task 4: 状态层 — `CMD_SETTINGS_SET` / `CMD_SYS_*` / REC 持久化

**Files:**
- Modify: `firmware/components/sloop_state/include/sloop_state.h`、`src/sloop_state.c`
- Test: `firmware/components/sloop_state/test/`（stub 引擎 + 用例，`scripts/state-test.sh`）
- Modify: `firmware/main/console.c:688`（`STATE SNAP` 追加字段）

**Interfaces:**
- Consumes: Task 2 `sloop_store_settings_set/dirty_mark`、Task 3 `sloop_hal_settings_*`。
- Produces:
  ```c
  CMD_SETTINGS_SET,   /* a=sloop_set_field_t v=值 */
  CMD_SYS_BRIGHT, CMD_SYS_VOLUME, CMD_SYS_OUTPUT,  /* v=值 */
  typedef enum { APP_SETTINGS_IDLE, APP_SETTINGS_OK, APP_SETTINGS_INVALID, APP_SETTINGS_BLOCKED, APP_SETTINGS_ERROR } app_settings_status_t;
  /* app_state_t 追加 */ struct { uint8_t palette, lowcut, zoom, lights, theme_on, theme_valid, reso, dirty; } settings; struct { uint8_t bright, vol, out, nvs_dirty; } sys; app_settings_status_t settings_status;
  ```
  `STATE SNAP` 行尾追加 `sys_bright=%u sys_vol=%u sys_out=%u set_palette=%u set_lowcut=%u set_zoom=%u set_lights=%u set_theme=%u set_reso=%u set_dirty=%u set_nvs=%u`。

- [ ] **Step 1: 写失败测试（state-test 用例）**

  - `settings_set_routes_each_field`：6 个字段各发一次 `CMD_SETTINGS_SET{a,v}`，stub 记录 `(field, v)` 与调用次数 == 1；`state_get_copy().settings.palette == v`。
  - `settings_set_clamps_and_invalid`：palette 9 ⇒ stub 收到 4；reso 99 ⇒ 10；lights 7 ⇒ 3；theme_on 2 ⇒ 1；`a >= SLOOP_SET_FIELD_COUNT` ⇒ `settings_status=INVALID` 且 stub 未被调用。
  - `settings_set_blocked_frozen`：stub 返回 `ESP_ERR_INVALID_STATE` ⇒ BLOCKED；`ESP_ERR_INVALID_ARG` ⇒ INVALID。
  - `sys_bright_volume_output`：bright 0 ⇒ 10、200 ⇒ 100；vol 0/100 边界；out 2 ⇒ INVALID（不入 HAL）；HAL stub 失败 ⇒ ERROR；`sys.nvs_dirty==1`（stub 置位）。
  - `rec_config_marks_settings_dirty`：`CMD_REC_CONFIG` 成功 ⇒ `sloop_store_settings_dirty_mark` stub 计数 +1；失败不变。

- [ ] **Step 2: 跑测试确认失败**

Run: `scripts/state-test.sh` Expected: FAIL（枚举/字段未定义）。

- [ ] **Step 3: 实现**

  `state_apply` 分支：`CMD_SETTINGS_SET` ⇒ 钳位 → `sloop_store_settings_set` → 映射状态 → 同拍刷 `settings` 投影；`CMD_SYS_*` ⇒ 钳位 → HAL → 刷 `sys`（`nvs_dirty` 取 HAL）；`CMD_REC_CONFIG` 成功路径末尾 `sloop_store_settings_dirty_mark()`；`state_sync_from_engine`/`state_refresh_locked` 各读一次 `sloop_store_settings()` 与 HAL 三值。console `STATE SNAP` 追加字段。

- [ ] **Step 4: 跑测试确认通过 + 构建**

Run: `scripts/state-test.sh && scripts/build.sh && scripts/build.sh`
Expected: PASS；0 警告。

- [ ] **Step 5: 设备**

Run: `scripts/flash.sh` → 串口 `STATE SNAP` → `STORE SETTINGS SET palette 1`（直调路径）→ `STATE SNAP`
Expected: `set_palette=1` 在下一拍同步出现；`DISP BRIGHT 50` ⇒ `sys_bright=50 set_nvs=1`，停止态 ≥6s 后 `set_nvs=0`。

- [ ] **Step 6: 提交**

```bash
git add firmware/components/sloop_state firmware/main/console.c
git commit  # feat(components/sloop_state): 增加设置与平台命令及投影
```

### Task 5: 运行时色表 + ZOOM 大读数条

**Files:**
- Create: `firmware/components/sloop_ui/src/theme_derive.h`（纯函数，host 可编）
- Modify: `firmware/components/sloop_ui/src/theme.h`（`ui_theme_t` + 宏改运行时）、`src/ui.c`（色表实例、`sloop_ui_rebuild_all`、ZOOM overlay、30ms 拍检测 palette 变化）、`src/knob.c`/`page_params.c`/`page_dsyn.c`/`page_fm6p.c`（焦点变化时喂 ZOOM）
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`
- Test: `scripts/knob-test.sh` + `scripts/knob-host-test.c`（或同目录既有纯函数测试文件）

**Interfaces:**
- Produces:
  ```c
  /* theme_derive.h */ void ui_theme_derive_mono(ui_theme_t *out);                                   /* 恰等于现行常量 */
                           void ui_theme_derive_palette(const uint16_t pal5[5], ui_theme_t *out);       /* spec D3 表 */
                           void ui_theme_derive_theme(const song_theme_view_t *th, ui_theme_t *out);
  /* theme.h */ typedef struct { uint32_t bg, panel, panel_hi, panel_sel, text, text_dim, accent, error, track[4]; } ui_theme_t;
               const ui_theme_t *ui_theme(void);   /* UI_BG 等宏改为 (ui_theme()->xxx) */
  /* sloop_ui.h */ void sloop_ui_theme_apply(const sloop_settings_t *s);  /* 更新色表 + sloop_ui_rebuild_all() */
                    void sloop_ui_rebuild_all(void);                   /* 销毁全部已建页 → 重建当前页（组切换同序） */
                    void sloop_ui_zoom_knob(const struct knob_t *k);    /* zoom ON 且 k 有焦点 ⇒ 显示 LABEL  VALUE；否则隐藏 */
  ```

- [ ] **Step 1: 写失败测试（knob-host-test）**

  `theme_mono_equals_constants`：`ui_theme_derive_mono` 的 bg==0x101418、panel==0x1B2026、panel_hi==0x242B33、panel_sel==0x2C3540、text==0xF2F5F8、text_dim==0x8A949E、accent==0xE0B32A、error==0xE0542A、track=={0x3B7BD8,0x3FA34D,0xE0B32A,0xE06A1B}。`theme_green_accent`：GREEN ramp ⇒ accent==RGB565(120,255,146) 的 RGB888 == 0x78FF92、bg 不变、track 不变。`theme_custom_overrides`：theme view（bg/hi/trk 任意）⇒ 全套替换。

- [ ] **Step 2: 跑确认失败**

Run: `scripts/knob-test.sh` Expected: 编译错（函数未定义）。

- [ ] **Step 3: 实现**

  - 审计 `grep -n "UI_\(BG\|PANEL\|PANEL_HI\|PANEL_SEL\|TEXT\|TEXT_DIM\|ACCENT\|ERROR\)" firmware/components/sloop_ui/src` 的所有静态初始化器（`static const … = {UI_…}`）改为运行时读 `ui_theme()`；`ui_track_color` 改读色表。
  - `ui.c`：`static ui_theme_t s_theme`，`sloop_ui_start` 建页前 `sloop_ui_theme_apply(sloop_store_settings())`；30ms 拍比较 `app_state.settings.palette/theme_on/theme_valid` 与上次 ⇒ 变化即 `sloop_ui_theme_apply`（内部先 `lv_obj_invalidate` 根对象）。
  - `sloop_ui_rebuild_all`：复用组切换销毁序（先建当前页新实例 → 切 → 销毁其余）——实现为：记当前页 id → 对当前组走既有「切组」路径（临时切到 PLAY 再切回，若当前就是 PLAY 则切到 TRACKS 再回）或在 ui.c 内直接按同序重建；两者都须保持「切换瞬间有活页」。
  - ZOOM overlay：`static lv_obj_t *s_zoom`，首次需要时创建（屏幕层、y=72 顶部、Montserrat 28、半透明底）；`sloop_ui_zoom_knob(k)`：`k && settings.zoom` ⇒ `lv_label_set_text_fmt("%s  %s", k->label, k->text_cb(k))` + `SHOW`，否则 `HIDDEN`；knob 焦点变化处（`knob.c` 的 focus setter）与三处页面的 update 调用它。

- [ ] **Step 4: 跑测试 + 构建**

Run: `scripts/knob-test.sh && scripts/build.sh && scripts/build.sh` Expected: PASS；0 警告。

- [ ] **Step 5: 设备 — 默认视觉零变化**

Run: `scripts/flash.sh` → `python3 scripts/screenshot.py` 对 PLAY/TRACKS/STEP/SONG 四页截屏 → 与 P4-D 证据包同页截屏做逐像素 diff（Pillow）
Expected: diff == 0（MONO 恒等现行色板）。

- [ ] **Step 6: 设备 — 换色与重建**

Run: 串口 `STORE SETTINGS SET palette 2` → 等 1 拍 → 截屏 CYAN 页签/accent 像素（抽检点取 `UI_ACCENT` 原锚点，如选中页签底色）→ `UI MEM` → 往返 SEQ/SONG/FILE/PLAY 10 轮 → `bash scripts/inject/nav.sh`
Expected: accent 像素 == 0x8CDEFF；无 fatal；`pool_free` 首末差 ≤128B；`nav` 全 PASS。存 `firmware/build/verify/p4e-task5-*.png`。

- [ ] **Step 7: 走带中切色**

Run: `KEY 0x42 down` + `ENG PLAY`（走带）→ `STORE SETTINGS SET palette 1` → 3 拍后 `cmd_batch`：`KEY tab`/`KEY tab`/`KEY tab` → `ENG SNAP`/`UI` → `STOP`
Expected: 无 fatal/abort；页面前进 1 页（ADR-006 同拍语义）；播放不中断（`STATE SNAP playing=1`）。

- [ ] **Step 8: ZOOM**

Run: `STORE SETTINGS SET zoom 1` → `UI global` → `UI KNOB 0 press 0 0` → 截屏 → `UI step` → 截屏
Expected: GLOBAL 页顶出现大读数 `LABEL  VALUE`（像素抽检：y∈[76,110) 有前景像素）；STEP 页隐藏；`UI MEM` 无 SYSTEM/其它页 est 变化；zoom ON 时 SEQ→PRESETS 峰值 `pool_free` 比默认少 ≤768B。

- [ ] **Step 9: 提交**

```bash
git add firmware/components/sloop_ui scripts/knob-test.sh scripts/knob-host-test.c
git commit  # feat(components/sloop_ui): 运行时色表、palette 重建与 ZOOM 大读数
```

### Task 6: SYS 组 / SYSTEM 页 + 导航更新 + `sys.sh` 全矩阵

**Files:**
- Create: `firmware/components/sloop_ui/src/page_system.c`、`page_system.h`
- Modify: `firmware/components/sloop_state/include/sloop_state.h:38,320`（`APP_PAGE_SYSTEM=14`、`APP_STATE_PAGES 15`）、`firmware/components/sloop_ui/include/sloop_ui.h:28`（`SLOOP_UI_GROUPS 8`、`sloop_ui_system_status_text()`）、`src/theme.h`（`UI_GROUP_W 122`、布局宏）、`src/ui.c`（`k_groups` 加 `{"SYS",1,{APP_PAGE_SYSTEM}}`、注册）、`firmware/main/console.c:905`（页名表 `"system"`、TAP 回显）
- Modify: `scripts/inject/inject.py`（`run_sys` ②–⑦、`nav` TAB 轮转 15 页）、`scripts/soak.py:65-68`（`"system": 14`，`POOL_RT_GROUPS` 加 `"system"`）、`scripts/inject/params.sh` 相关的 `inject.py` 断言不变
- Test: `scripts/inject/sys.sh`、`nav.sh`、`params.sh`、`synth.sh`（回归）

**Interfaces:**
- Consumes: Task 4 命令与投影、Task 5 色表、Task 3 `KBD RGB` 路径（`sloop_hal_keyboard_rgb(level)`——在 Task 6 抽成 HAL 入口供页面与 LIGHTS apply 调用，颜色 = `ui_theme()->accent` 降额）。
- Produces: `lv_obj_t *page_system_build(lv_obj_t *)`、`void page_system_update(const app_state_t *)`、`esp_err_t page_system_key(uint32_t code)`、`int page_system_tap(int x, int y)`（回 `-> SYS …`）、`const char *sloop_ui_system_status_text(void)`、`void sloop_ui_system_clear_action(void)`。

- [ ] **Step 1: `run_sys` ② 写失败断言**

  ② `UI system` ⇒ `UI page=system (14)`；`UI MEM` ⇒ `GROUP SYS built=1 sum_est=10,240` 与 `page=SYSTEM est=10240 delta<=10240`；`UI TAB sys`/`UI SUB 0` 回显；截屏 `inject-sys-*.png`（组签 8 个、SYSTEM 高亮、四节标题、各值行）；像素抽检：第 8 个组签矩形 x∈[16+7×130, +122) 有高亮底色。

- [ ] **Step 2: 跑确认失败**

Run: `bash scripts/inject/sys.sh` Expected: ② `UI ERR` 失败。

- [ ] **Step 3: 实现页面与注册**

  - 布局（页区相对坐标）：节标题 16pt `SCREEN`(y16)/`LIGHTS`(y150)/`AUDIO`(y230)/`SYSTEM`(y430)；行高 44，三区对象宽 420（x=24）、chip 宽 140；SYSTEM 节两行版本（`APP <esp_app_get_description()->version>`、`SLOOP 2.8.5 (bac9c05)`）+ `[ SELFTEST ]` chip（Task 7 接线，本任务先显示 `SELFTEST` 但按下回 `status="RESET FIRST"`? 不——本任务 chip 回 `status="SELFTEST: N/A"`，Task 7 换成真实入口）；状态行 (24,600) 20pt。
  - 行与命令：COLOR ± ⇒ `CMD_SETTINGS_SET{PALETTE, cur±1 环 0..4[+THEME]}`；ZOOM chip ⇒ `{ZOOM, !cur}`；BRIGHT ± ⇒ `CMD_SYS_BRIGHT{cur±10 钳 10..100}`；LIGHTS ± ⇒ `{LIGHTS, cur±1 环 0..3}`；VOLUME ± ⇒ `CMD_SYS_VOLUME{cur±5}`；OUTPUT ± ⇒ `CMD_SYS_OUTPUT{!cur}`（HAL NOT_SUPPORTED ⇒ 状态行 `N/A`）；SPK LOWCUT chip；FILTER RESO ±（OFF..10）；全部经 `state_apply`。
  - THEME 选项：`settings.theme_valid` 时 COLOR 环多一档，显示 `theme_name`，选中 = `CMD_SETTINGS_SET{THEME_ON,1}`；选回内置 palette 时 `{THEME_ON,0}` 再 `{PALETTE,n}`（两条命令，顺序固定）。
  - 状态行 `ui_sys_status_text`（theme.h 单一映射）：`OK` / `SAVED` / `N/A` / `NO STORAGE` / `RESET FIRST` / `SELFTEST <pass>/<total> PASS|FAIL`；失败红字。
  - 键盘：↑/↓ 焦点环（COLOR/ZOOM/BRIGHT/LIGHTS/VOLUME/OUTPUT/LOWCUT/RESO）、←/→ = ±、ENTER = +1/切换。
  - TAP：三区命中 ⇒ 同动作；回显 `-> SYS <ROW> value=<写后读回> status="<状态行>"`；chip ⇒ `-> SYS <ROW> value=<ON|OFF>`；`TAP miss` 命名与其它页一致。
  - 导航：`SLOOP_UI_GROUPS 8`、`UI_GROUP_W 122`、`_Static_assert` 更新、`k_groups` 加 SYS（排在 FILE 后）、TAB 轮转 …→PRESETS→SYSTEM→PLAY；console 页名表、`soak.py`、`inject.py` 的 `PAGES`/轮转期望同步。

- [ ] **Step 4: 构建 + 截屏 + 像素抽检**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh && bash scripts/inject/sys.sh`
Expected: ② PASS；组签右端 1,048 ≤ 1,064；`build_ms ≤100`。

- [ ] **Step 5: `run_sys` ③–⑦ 落地并跑绿**

  ③ TAP/键盘：COLOR 循环 5 档（每档 `STORE SETTINGS INFO palette=` 一致 + accent 像素变化 + 兄弟页 `UI tracks` 重着色）；BRIGHT `TAP −`×2 ⇒ `DISP BRIGHT applied=80`、`sys_bright=80 set_nvs=1`；VOLUME `TAP +` ⇒ `AUD VOL applied=75`；LIGHTS `TAP +` ⇒ `KBD RGB` 读回 `low`；LOWCUT chip ⇒ INFO `lowcut=1`；RESO `TAP +`×3 ⇒ `reso=3`；走带中改值（Review Focus 3）：`ENG PLAY` + 20 次 `DISP BRIGHT` 交替 ⇒ `ENG PERF xrun` 增量 0、`set_nvs=1` 持续到 STOP 后窗口 ⇒ `set_nvs=0`。
  ④ 持久化：dirty 延迟写（改 palette 后 ≤1s `STORE SETTINGS INFO dirty=1`；停止+过窗口 ⇒ `dirty=0` 且 `STORE SONG INFO side=` 不变）；硬复位 ⇒ INFO 全字段保持、`DISP BRIGHT`/`AUD VOL` 保持；REC 三项：`SEQ REC CONFIG 1 1 1` → 过窗口 → 硬复位 → `SEQ REC` 的 `quant=off tempo=tempo count=count`。
  ⑤ 自检 golden 回归：`lowcut=1 reso=6` 下 `device-selftest.py` 182/182（Task 2 已覆盖，此处只断言 INFO 保持）。
  ⑥ 有序对：`UI step`+`UI transport` 全建 → `UI system`，构建日志 `池剩余` ≥4,096；SYS→STEP 同法；`GROUP SEQ sum_est=32,256` 不变。
  ⑦ 收口：palette 4 / zoom 0 / lights 0 / lowcut 0 / reso 0 / bright 100 / vol 70 + `SAVE` + `STORE AUTO ON`。

- [ ] **Step 6: 回归导航与池**

Run: `bash scripts/inject/nav.sh && bash scripts/inject/params.sh && bash scripts/inject/synth.sh`
Expected: 三场景 PASS（TAB 轮转 15 页、组作用域往返 ≤128B、EDIT/SYNTH Σ 不变）。

- [ ] **Step 7: 提交**

```bash
git add firmware/components/sloop_ui firmware/components/sloop_state/include/sloop_state.h firmware/main/console.c firmware/components/sloop_hal_tab5 scripts/inject scripts/soak.py
git commit  # feat(components/sloop_ui): 增加 SYS 组与 SYSTEM 设置页
git commit  # test(scripts/inject): 增加 sys 设置页注入场景与 15 页导航期望
```

### Task 7: SELFTEST chip（一次性任务）+ `STORE RAW END settings`

**Files:**
- Modify: `firmware/components/sloop_state`（`CMD_SELFTEST_RUN`）、`firmware/main/main.c`（创建请求队列/任务）、新建 `firmware/main/sys_selftest.c/.h`（任务体：AUTO OFF → 停流 → `sloop_core_selftest(NULL)` → 起流 → `sloop_store_autosave_resume()` → AUTO ON → 结果写回状态层 `state_set_selftest_result(pass,total)`）、`page_system.c`（chip 接线、守卫）、`store_tab5.c`/`console.c`（RAW settings 回显——Task 2 已做则只补场景）
- Test: `state-test`（透传/守卫）+ `inject.py` `run_sys` ⑧⑨

**Interfaces:**
- Produces: `CMD_SELFTEST_RUN`（走带中/REC armed ⇒ 页面守卫 `STOP FIRST` 不发；状态层置 `selftest_req=1` 投影，由 main 任务消费 `state_take_selftest_request()`）；`void state_set_selftest_result(uint8_t pass, uint8_t total, bool fail)`；状态行 `SELFTEST 182/182 PASS`；`RESET FIRST` 守卫（`.noinit` 的 `s_ran` 跨复位保留；冷启动显式清零）。

- [ ] **Step 1: state-test 失败用例**

  `selftest_run_sets_request`：命令 ⇒ `state_get_copy().selftest_req==1`；`state_take_selftest_request` 取走后回 0；`state_set_selftest_result(182,182,false)` ⇒ `settings_status==OK` 且状态文本 `SELFTEST 182/182 PASS`（文本在 UI 层，state 层只存数值）。

- [ ] **Step 2: 实现**

  任务栈 = console 任务同款（`main.c` 查 `usb_serial_jtag` console 任务栈配置值并复用常量），优先级同 console；一次性创建、跑完自删；`.noinit s_ran` 第二次置位后页面回 `RESET FIRST`。RAW settings 场景：`gen_project_fixtures.py` 风格的 `scripts/gen_settings_fixture.py --theme NAME` 生成合法 PER3（palette 3 + word 0 + 40B 默认尾段）+ 用 `STORE RAW BEGIN/…/END settings` 写入（side theme 经第二步 `STORE RAW END settings side`？——**决定**：RAW END 不带 side；side theme 用 `STORE RAW SIDE <hex80>` 新子命令单独写并 `settings_reload`，这样 fixture 可测 theme 读路径）。

- [ ] **Step 3: 构建烧录**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh` Expected: 0 警告。

- [ ] **Step 4: `run_sys` ⑧⑨**

  ⑧ SELFTEST chip：走带中 `TAP` ⇒ `status="STOP FIRST"` 且 `ENG MEM` 不变、`ENG AUDIO` 仍 ON；停止后 `TAP` ⇒ 状态行轮询到 `SELFTEST 182/182 PASS`（≤120s）、`STORE AUTO` 回 on、`STORE LIVEHASH` == 自检前 hash（工作工程恢复）、流式恢复 `streaming=1`；再 `TAP` ⇒ `status="RESET FIRST"`。
  ⑨ RAW settings：`STORE RAW BEGIN <len>` → hex → `STORE RAW END settings` ⇒ `saved len=<n>`；`STORE RAW SIDE <160 hex>` ⇒ `STORE RAW SIDE saved`；`STORE SETTINGS INFO` ⇒ `palette=3 theme=<NAME> theme_valid=1`；COLOR 环出现 THEME 档；选它 ⇒ accent 像素 == theme.hi；硬复位后仍保持；`STORE TEST` 仍 PASS 6/6。

- [ ] **Step 5: 提交**

```bash
git add firmware/main firmware/components/sloop_state firmware/components/sloop_ui/src/page_system.c firmware/components/sloop_core/src/store_tab5.c firmware/main/console.c scripts/gen_settings_fixture.py scripts/store_raw.py scripts/inject/inject.py
git commit  # feat(firmware/main): 增加 SYSTEM 页自检入口任务与 RAW settings/side 注入
```

### Task 8: 文档、ADR、证据与全门禁、合并

**Files:**
- Modify: `AGENTS.md`（§3 目录/组件描述、§5.1 新脚本与命令、§5.4.1 页名/TAP 回显/`STORE SETTINGS`/`DISP`/`AUD VOL|OUT`/`KBD RGB`/`SYS NVS`、§5.5 新门禁行）、`docs/design/005-ui-architecture.md`（§1 组表 +8 组、§2 组签 122px、§5.2 P4-E 预算与实测）、`docs/design/009-storage-backend.md`（§6.1 P4-E 落地注记：真实状态源、RMW、NVS 三键延迟提交、RAW settings）、`docs/deferred-issues.md`（删 DI-32；D10 降级时新增条目）、`docs/design/002-port-roadmap.md`（P4-E 状态行）、`docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（P4-E 行 ✅）、`docs/dev/verification-log.md`
- Create: `docs/dev/verify/4-pe-<日期>/`（`readme.md` 门禁→命令→结论、`SHA256SUMS`、日志/截屏）

- [ ] **Step 1: 全矩阵**

Run: `scripts/build.sh --clean && scripts/build.sh && scripts/build.sh && scripts/host-test.sh && scripts/seq-test.sh && scripts/state-test.sh && scripts/knob-test.sh && scripts/synth-test.sh && python3 scripts/screenshot.py --selftest && python3 scripts/soak-selftest.py && python3 scripts/sample_upload.py --selftest` + `cd third_party && sha256sum -c sloop-fm1/../MANIFEST.sha256`（按既有校验命令）
Expected: 全 0；0 警告；MANIFEST 233/233。

- [ ] **Step 2: 设备矩阵（同一会话）**

Run: `scripts/flash.sh` ×3（看 `Hash of data verified`）→ `python3 scripts/device-selftest.py` → 十条注入场景 `transport/step_edit/tracks/nav/params/store/song/sequence/synth/sys`（`sequence`/`synth`/`sys` 各复跑一次）→ 复位后 `python3 scripts/soak.py PORT --duration 1800 --interval 60 --save-window 900`
Expected: 182/182；十场景 0 FAIL；soak PASS（口径同 §5.5）。

- [ ] **Step 3: 文档更新 + 证据晋升**

  截图/日志复制到 `docs/dev/verify/4-pe-<日期>/`，`sha256sum * > SHA256SUMS`；verification-log 登记；AGENTS §3 自查与目录一致。

- [ ] **Step 4: 提交 + 整分支终审 + 合并**

```bash
git add AGENTS.md docs && git commit  # docs(design): 登记 P4-E 设置页决策与阶段证据
git push origin feat/phase4e
```
整分支 reviewer（superpowers:requesting-code-review 口径）→ 修复 → 复验 → `git checkout dev && git merge --no-ff feat/phase4e` → 合并后重跑 Step 1 + `device-selftest.py` → `git push origin dev`。

---

## 自审记录

- Spec 覆盖：§4 表每行→Task（palette/theme 1/2/5，zoom 1/2/5，lights 1/2/3/6，lowcut/reso 1/2/4，REC bits 1/2/4/6，亮度/音量/路由 3/4/6，KBD RGB 3/6）；D6→Task 7；D7→Task 2 Step 5 + Task 6 ⑤；§8.3 RAW→Task 2（END）+Task 7（SIDE）；§9 console→Task 2/3/4；§10 测试→各任务 Step；§11 容量→Task 5/6 设备步骤。
- 类型一致：`sloop_set_field_t`/`sloop_settings_t`/`song_settings_t`/`song_theme_view_t`/`ui_theme_t` 在 Task 1/2/4/5/6 命名一致；`sloop_hal_settings_*` 在 Task 3/4 一致。
- 比例：计划 ≈ spec 的 1.1 倍，代码块仅签名。
