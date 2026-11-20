# 阶段 3：独立模式 MVP 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 最小可玩闭环——触摸/键盘输入 → AppState/EditCmd → sloop_core 引擎 → ES8388 出声，LVGL 呈现 PLAY/TRACKS/STEP/TRANSPORT 四屏，串口注入使 UI 可脚本复现（roadmap §5）。

**Architecture:** 三层——`sloop_core`（引擎，补只读快照与全量步读写）、`sloop_state`（AppState+EditCmd，纯 C 可 host 测，唯一写者=input 任务）、`sloop_ui`（LVGL 四屏 + 页签 + InputManager）；注入 `KEY/TAP/UI` 与物理路径在 EditCmd 层等价。DI-8 用 `sloop_hal_audio_stop()` + `ENG AUDIO ON|OFF` 解决。

**Tech Stack:** ESP-IDF v5.5.5 / esp32p4 / LVGL 9.6 + esp_lvgl_port 2.9.0（BSP 管理）/ tab5_keyboard（0x6D）/ sloop_core（阶段 2）。

**Spec:** `docs/superpowers/specs/2026-11-20-phase3-mvp-ui-design.md`（含 builder-check 裁决回写）+ `docs/design/005-ui-architecture.md` + `docs/design/006-keyboard-mapping.md`。执行前必读。

## Global Constraints

- 构建/烧录走 `scripts/`；`scripts/build.sh` ×2 第二遍零新增警告；烧录 `Hash of data verified`、启动无 fatal/abort/backtrace。
- **third_party/sloop-fm1 只读**（manifest 233/233；任何改动=停）。
- 内存门禁：idle 内部堆（`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`）**≥60KB**；LVGL 缓冲保持 PSRAM（阶段 1 配置），不得改回内部 RAM。
- 音频：44.1kHz/2ch/16bit；音频任务优先于 UI/input；`ENG SELFTEST`/`AUD TONE` 在流式时拒绝并提示 `ENG AUDIO OFF`（DI-8 语义，spec §4）。
- 输入延迟目标：键按下→出声 **≤20ms**（input 任务优先级 > UI；EditCmd 短平快，不在 UI 回调里做长操作）。
- UI 改动门禁（AGENTS §5.4）：截屏 + `screenshot.py --check` 像素抽检 + 视觉检查；证据晋升 `docs/dev/verify/3-mvp-<日期>/` 并登记 verification-log。
- 所有 LVGL 调用在 `bsp_display_lock()` 纪律下；UI 定时器 30–60ms 脏检查重绘。
- 引擎回归：`scripts/host-test.sh` 保持全绿；`ENG SELFTEST`（`ENG AUDIO OFF` 后）182/182 保持（UI 任务不触碰 DSP 状态）。
- 状态层单测：`scripts/state-test.sh`（gcc，stub 引擎）退出码 0。
- 提交 AGENTS §7；feature 分支 `feat/mvp-ui` 自 dev 拉出；**每任务 review clean 后 push origin**（沿用阶段 2 用户工作流）；收尾合 dev。

## Review Focus（测试已钉进对应任务）

1. **修饰键+键组合的事件竞态**（键盘逐事件上报；层状态在 input 任务与 UI 读副本之间可能读到中间态）→ Task 6 Step 1 真机组合核对 + Task 6 Step 5 注入层序列（down 修饰→down 键→up 键→up 修饰）断言层行为。
2. **步编辑误清字段**（read-modify-write 若漏读 lvl/rat/micro/fill 会静默删参数锁/连击）→ Task 5 Step 2 单测：写一步后断言未改字段逐字节保留（含 TIE time 与 fill）。
3. **输入延迟不达标**（UI 线程做长操作或锁竞争阻塞引擎）→ Task 6 Step 6 延迟测量（注入 100 次 note-on 统计到 `sloop_core_note` 返回的时间）+ Task 8 PERF 不劣化。
4. **内部 DRAM 跌破 60KB**（LVGL 对象/主题/字体）→ Task 4 Step 4 与 Task 8 Step 1 门禁（idle + 四屏切换后水位）。
5. **注入与物理不等价**（`TAP` 绕过真实回调路径导致脚本绿而手测红）→ Task 7 Step 3 同一 EditCmd 断言：hit-test 注入与模拟控件回调产生相同 cmd 序列。

---

## 文件结构

```
firmware/components/
├── sloop_core/            # 修改：include/sloop_core.h（快照/步读写/全局）+ src/selftest.c（实现）
├── sloop_hal_tab5/        # 修改：audio stop（audio_stream.c/h）
├── sloop_state/           # 新建：include/sloop_state.h + src/{state.c,cmd.c}
└── sloop_ui/              # 新建：include/sloop_ui.h + src/{ui.c,page_play.c,page_tracks.c,page_step.c,page_transport.c,input.c,theme.h}
firmware/main/           # 修改：main.c（UI 启动/音频 ON）、console.c（ENG AUDIO、KEY/TAP/UI）
scripts/state-test.sh    # 新建
scripts/inject/          # 新建：3 条注入脚本（.sh）
docs/dev/verify/3-mvp-<日期>/  # 证据
```

---

### Task 0: 基线与分支

**Files:** 无

- [ ] `git checkout dev && git checkout -b feat/mvp-ui`
- [ ] `scripts/build.sh` 基线绿；`scripts/host-test.sh` 绿（记录终端行）。

### Task 1: 引擎只读/步读写接口（sloop_core.h 扩展）

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_core.h`, `firmware/components/sloop_core/src/selftest.c`

**Interfaces:**
- Produces（spec §3.1 逐字）：
```c
typedef struct { bool playing; uint16_t bpm; uint8_t selected_track;
    struct { uint8_t engine, preset, level, mute, armed, solo; uint8_t len, div; } trk[4];
    struct { uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags; } step[4][16];
    struct { uint8_t on; uint8_t lvl; uint8_t rat; } dstep[16];
    uint8_t step_pos[4]; } sloop_core_snapshot_t;
void sloop_core_read(sloop_core_snapshot_t *out);
typedef struct { uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
    uint8_t lvl; uint8_t rat; int8_t micro; uint8_t fill; } sloop_step_t;
void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out);
void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in);
void sloop_core_set_global(uint32_t id, int16_t value);
```

- [ ] **Step 1:** 在 `selftest.c` API 区实现 `sloop_core_read`（加 `s_engine_lock`；`bpm` 取 `song.g[G_TEMPO]`；`trk[k].len/div` 取 `P_SLEN/P_SDIV`；`step_pos[k]` 取 `trk[k].seq_idx` 或 0；`solo` 取 `song.solo` 位；dstep 聚合 `on = dstep[i].on != 0`）、`read_step/write_step`（映射上游 `step_t`：`lvl/rat` 为每音符 2bit 打包保持原样，`micro`/`fill` 透传）、`set_global`（走 `GP[]` 范围钳位后写 `song.g[]`）。
- [ ] **Step 2:** 设备验证：`scripts/build.sh`×2 → flash → 串口 `ENG INFO` 后新增临时校验（在 `ENG SMOKE` 或临时命令中打印 `sloop_core_read` 的 bpm/selected/step0 字段）——或直接写一个最小 `ENG SNAP` 命令打印快照关键字段（保留为诊断命令）。
- [ ] **Step 3:** `ENG AUDIO OFF` 后 `ENG SELFTEST 00_808_BOOM`（用 golden 全名 `preset/ANALOG/00_808_BOOM`）仍 PASS；`scripts/host-test.sh` 绿。
- [ ] **Step 4:** Commit `feat(components/sloop_core): 引擎只读快照与全量步读写接口`。

### Task 2: DI-8 音频启停 + ENG AUDIO

**Files:**
- Modify: `firmware/components/sloop_hal_tab5/{include/sloop_hal.h,src/audio_stream.c}`, `firmware/main/{main.c,console.c}`

**Interfaces:**
- Produces: `esp_err_t sloop_hal_audio_stop(void);`（挂起→`vTaskDelete(audio_task)`→置 NULL；codec 保持打开；`sloop_hal_audio_streaming()` 变 false）；console `ENG AUDIO ON|OFF`。
- Consumes: 阶段 2 `sloop_hal_audio_start()`。

- [ ] **Step 1:** 实现 `audio_stop`（无活任务返回 ESP_OK 幂等）；`sloop_core_selftest` 的 busy 分支提示追加 `hint=ENG AUDIO OFF`（不改拒绝语义）。
- [ ] **Step 2:** main.c：启动序列末尾 `ENG`-style 直接调 `sloop_hal_audio_start(sloop_core_render)`（UI 前先开音频；失败 ERROR 不阻断）。
- [ ] **Step 3:** 真机验证：boot → `ENG SELFTEST` 被拒（hint 出现）→ `ENG AUDIO OFF` → SELFTEST 单例 PASS → `ENG AUDIO ON` → `ENG TRIG synth 0 60` 出声路径 OK（日志）；`AUD TONE` 在 ON 时拒绝、OFF 时可用。
- [ ] **Step 4:** Commit `feat(components/sloop_hal_tab5): 音频流启停与 ENG AUDIO（DI-8）`，并在 `docs/deferred-issues.md` 删除 DI-8 条目（已解决），`docs/dev/device-notes.md` 记录行为。

### Task 3: sloop_state 组件（TDD，host 可测）

**Files:**
- Create: `firmware/components/sloop_state/{CMakeLists.txt,include/sloop_state.h,src/state.c,src/cmd.c}`
- Create: `scripts/state-test.sh`, `firmware/components/sloop_state/test/state_test.c`

**Interfaces:**
- Produces（spec §3.2）：
```c
typedef enum { CMD_NOTE_ON, CMD_NOTE_OFF, CMD_PLAY, CMD_STOP, CMD_SELECT_TRACK,
               CMD_TOGGLE_STEP, CMD_SET_BPM, CMD_SET_TRACK, CMD_MUTE, CMD_SOLO } edit_cmd_type_t;
typedef struct { edit_cmd_type_t type; uint32_t a, b; uint16_t v; } edit_cmd_t;
void state_init(void);
void state_apply(const edit_cmd_t *cmd);
void state_sync_from_engine(void);
void state_get_copy(app_state_t *out);
```
- `app_state_t` 字段：`sloop_core_snapshot_t core;` + `uint8_t page; uint8_t focus_step; uint8_t focus_track; uint8_t last_note; uint8_t layer_held;`（层键位掩码）。

- [ ] **Step 1 (RED):** 写 `test/state_test.c` 断言（stub 引擎用弱符号/编译期替换，`state-test.sh` 用 `gcc -DHOST_TEST` 编 stub）：① `CMD_SET_BPM` 越界钳位（20..300）并落 engine 调用；② `CMD_TOGGLE_STEP` 空步→写入 `last_note`（n=1/vel=100/time=NOTE/flags=0）；再切→n=0/time=REST；③ read-modify-write：引擎预置 lvl=0x0A/rat=0x05/micro=-3/fill=2，切换后四字段不变；④ `CMD_MUTE/CMD_SOLO` 更新对应轨位；⑤ `state_get_copy` 反映最近 cmd 后的状态。`scripts/state-test.sh` 打印 PASS/FAIL。
- [ ] **Step 2:** 跑 `scripts/state-test.sh` → RED（未实现）。
- [ ] **Step 3:** 实现 `state.c`（唯一写者语义、互斥 `state_mutex`、`state_get_copy` 复制）；`cmd.c` 为 cmd→engine/stub 分发（HOST_TEST 下调用 stub，设备下调用 sloop_core）。
- [ ] **Step 4:** GREEN；`scripts/state-test.sh` 退出码 0。
- [ ] **Step 5:** Commit `feat(components/sloop_state): AppState/EditCmd 状态层（host 单测）`。

### Task 4: sloop_ui 骨架 + PLAY/TRANSPORT 两屏

**Files:**
- Create: `firmware/components/sloop_ui/{CMakeLists.txt,include/sloop_ui.h,src/ui.c,src/theme.h,src/page_play.c,src/page_transport.c}`
- Modify: `firmware/main/{main.c,CMakeLists.txt}, firmware/main/console.c`（`UI` 命令）

**Interfaces:**
- Produces: `esp_err_t sloop_ui_start(void);`（创建页签+四屏对象、注册 UI 定时器；LVGL 对象在调用方已持锁或内部加锁）；`void sloop_ui_switch_page(uint8_t page);`（0=PLAY,1=TRACKS,2=STEP,3=TRANSPORT）；console `UI <play|tracks|step|transport>`。
- Consumes: `state_get_copy`、`sloop_core_read`（经 state 同步）。

- [ ] **Step 1:** `ui.c`：单容器 + 顶部页签（4 按钮，主题色高亮选中）；30ms 定时器 `state_sync_from_engine()` + 各页 `page_update(const app_state_t*)` 脏检查。
- [ ] **Step 2:** PLAY 页：当前轨 engine/preset 名（用快照数字即可，MVP 不做名称表）、音符触发指示（`sloop_ui_flash_note()` 由 input 调用，点亮 120ms）、电平条（`trk.level`）。
- [ ] **Step 3:** TRANSPORT 页：PLAY/STOP 大按钮（发 `CMD_PLAY/STOP`）、BPM −/+（长按连发；`CMD_SET_BPM`）、步位置指示。
- [ ] **Step 4:** 验证：build×2 → flash → `UI play`/`UI transport` → `python3 scripts/screenshot.py /dev/ttyACM0 --check …`（页面元素锚点色）+ 截屏四要素（页签/按钮/文本）视觉检查；idle 内部堆 ≥60KB（新增 `ENG MEM` 命令或复用 PERF 输出——若需新增，读 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 打印）。证据入 `docs/dev/verify/3-mvp-<日期>/`。
- [ ] **Step 5:** Commit `feat(components/sloop_ui): LVGL 四屏框架与 PLAY/TRANSPORT 页`。

### Task 5: TRACKS + STEP 两屏（步编辑闭环）

**Files:**
- Create: `firmware/components/sloop_ui/src/page_tracks.c`, `src/page_step.c`
- Modify: `firmware/main/console.c`（如需注入辅助）

**Interfaces:**
- Produces: 页更新函数 `page_tracks_update/page_step_update(const app_state_t*)`；STEP 网格点击 → `CMD_TOGGLE_STEP{a=track,b=step}`；轨道卡片点击 → `CMD_SELECT_TRACK`；点击网格时同步 `focus_track/focus_step`。
- Consumes: Task 3 的 `state_apply`、Task 4 的页框架。

- [ ] **Step 1:** TRACKS：四卡横排（主题色描边），选中态高亮；卡内 engine/preset/level/mute/solo 文本；点击发 `CMD_SELECT_TRACK`。
- [ ] **Step 2:** STEP：4×16 网格；行=轨色，列=步；格子亮=有音（`n>0`）；播放头高亮（`step_pos[focus_track]`）；点击格子发 `CMD_TOGGLE_STEP`（spec §3.2 语义：空→`last_note`，开→REST，read-modify-write）。
- [ ] **Step 3 (test):** 注入序列：`UI step` → 选定轨/步（`TAP` 在 Task 7 前先用 `state_test` 覆盖；本任务用 console 临时命令 `UI FOCUS <track> <step>`）→ 断言引擎 `read_step` 变化（`ENG SNAP` 或新增 `ENG STEP <track> <idx>` 打印）。真实网格点击验证用 `TAP` 或临时命令；像素抽检锚定格子颜色。
- [ ] **Step 4:** 截屏矩阵：TRACKS（选中 1/4 轨）、STEP（步开/关/播放头）。§5.4 全流程 + 证据。
- [ ] **Step 5:** Commit `feat(components/sloop_ui): TRACKS 与 STEP 页（步格编辑闭环）`。

### Task 6: InputManager（键盘 → 层模型 → EditCmd）+ DI-4 子集核对

**Files:**
- Create: `firmware/components/sloop_ui/src/input.c`, `include/sloop_ui.h`（追加注入 API）
- Modify: `firmware/main/console.c`（`KEY <code> <down|up>`）

**Interfaces:**
- Produces: `void sloop_ui_input_key(uint8_t code, bool down);`（物理与注入共用）；`void sloop_ui_input_tap(int16_t x, int16_t y);`（Task 7 实现）；input 任务（优先级 4，低于音频 5、高于 UI 默认）。
- Consumes: `tab5_keyboard` 的 `tab5_kb_poll()`、`tab5_kb_key_name()`；`state_apply`。

- [ ] **Step 1（硬件前置）:** 真机核对修饰键组合：`scripts/flash.sh --monitor` → 按 Sym+Aa+字母/Arrow 等 → 记录事件序列（是否独立 down/up）；结果写 `docs/design/006-keyboard-mapping.md`（若组合不报→回退为层键锁定，更新映射表并继续）。
- [ ] **Step 2:** 映射表实现（表驱动，`{code, layer, action}` 数组；演奏键→`CMD_NOTE_ON/OFF` with `last_note` 更新；层键→`layer_held` 位；EDIT 层键→`CMD_TOGGLE_STEP` 删除语义走 read-modify-write；GLO 层 1–8→`CMD_MUTE/SOLO`；space→`CMD_PLAY` toggle；esc/tab/arrows→页/焦点命令）。
- [ ] **Step 3:** input 任务：`tab5_kb_poll` 事件 → `sloop_ui_input_key`；PLAY 页 `sloop_ui_flash_note` 指示。
- [ ] **Step 4:** 注入命令 `KEY` 接到 `sloop_ui_input_key`（与物理同入口）。
- [ ] **Step 5 (test):** 注入序列：`KEY <Sym down>` → `KEY <a down/up>`（EDIT 层擦除断言 `read_step` 变化）→ `KEY <Sym up>`；GLO 层 1–4 静音断言；演奏键 note-on/off 断言 `ENG SNAP`/电平变化。`KEY` 的 code 用驱动键名表反查（报告列出使用的 code 值）。
- [ ] **Step 6 (延迟):** 注入 100 次 note-on，记录 `state_apply` 前后 esp_timer 差 → 均值/最大 ≤20ms；`ENG PERF` 音频占空比不劣化（vs 阶段 2 9.44%）。证据入档。
- [ ] **Step 7:** Commit `feat(components/sloop_ui): InputManager 70 键映射与层模型` + 映射表文档更新（`docs/design/006-keyboard-mapping.md` 按键名落表）。

### Task 7: TAP 注入 + 注入脚本 + 协议文档同步

**Files:**
- Create: `scripts/inject/{play_note.sh,step_edit.sh,transport.sh}`（pyserial 序列，复用 screenshot.py 或自包含）
- Modify: `firmware/components/sloop_ui/src/{ui.c,page_step.c,page_tracks.c,page_transport.c,page_play.c}`（页注册 hit-test 表）、`firmware/main/console.c`（`TAP`）
- Modify: `AGENTS.md` §5.3/§5.4（注入约定）

**Interfaces:**
- Produces: `void sloop_ui_input_tap(int16_t x, int16_t y)`：按当前页 hit-test 表（矩形→EditCmd）派发；`TAP <x> <y>` console 命令。
- Hit-test 表：每页 static 数组 `{x,y,w,h,cmd}`；UI 布局常量与表同源（避免漂移）。

- [ ] **Step 1:** 各页注册 hit-test 表（PLAY 页无编辑；TRANSPORT 两按钮+BPM；TRACKS 四卡；STEP 4×16 格）。
- [ ] **Step 2:** `TAP` 命令 → `sloop_ui_input_tap`；落在表外打印 miss（不崩溃）。
- [ ] **Step 3 (等价性):** 断言 `TAP` 与模拟控件回调产生相同 `edit_cmd_t` 序列（页 hit-test 单测或日志对比）。
- [ ] **Step 4:** 三条注入脚本稳定复现（每脚本：`UI`→操作→`SCR` 存图）：①TRANSPORT 播放→STEP 截图（播放头）；②STEP 点两格→截图；③TRACKS 选轨→截图。运行退出码 0，图存 `firmware/build/verify/` 后晋升。
- [ ] **Step 5:** AGENTS §5.3（`SCR` 说明补充「先 `UI` 切页」）与 §5.4（注入脚本约定）更新；Commit `feat(main): 串口注入 KEY/TAP/UI 与脚本化截屏序列`。

### Task 8: 稳定性 soak + 收口

**Files:**
- Create: `docs/dev/verify/3-mvp-<日期>/`（soak 摘要、截屏矩阵、延迟/内存数据）
- Modify: `docs/dev/{verification-log.md,device-notes.md}`、`docs/design/002-port-roadmap.md`（§1 阶段 3 状态）、`AGENTS.md`（§3 sloop_state/sloop_ui 落地）

- [ ] **Step 1:** 全门禁：`scripts/build.sh`×2、`scripts/host-test.sh`、`scripts/state-test.sh`、`ENG AUDIO OFF` 后 `ENG SELFTEST`（单例+抽若干）、四屏截屏矩阵 + `--check`、idle 内部堆 ≥60KB。
- [ ] **Step 2:** 30min soak：`ENG AUDIO ON` + 注入脚本循环（演奏/步编辑/切页），每 5min 记录 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 与 `ENG PERF`；判定：非单调下降、无 WDT/panic、占空比不劣化 ≥2×。
- [ ] **Step 3:** 证据晋升 + verification-log 登记（含 3 条注入脚本 transcript、soak 数据、截屏）。
- [ ] **Step 4:** 文档同步：roadmap §1 阶段 3 → 🔶 机器侧完成（人测项列清单）；AGENTS §3 组件落地；device-notes 历史行；删除 DI-8（Task 2 已完成，若未删则此处删）。
- [ ] **Step 5:** `git checkout dev && git merge --no-ff feat/mvp-ui`；push（若沿用阶段 2 工作流）。
- [ ] **Step 6:** Commit `docs(.): 阶段 3 MVP 收口与验证证据入档`。

---

## 完成判据对照（roadmap §5）

1. 端到端（机器侧）：注入演奏 → 引擎出声路径 + UI 指示联动；人测试玩入清单（键盘弹奏/四屏/听感）；
2. 四屏截屏全部过 §5.4（视觉人测项标 ⏳）；
3. ≥3 条注入脚本稳定复现（Task 7）；
4. host-test + state-test 全绿；30min soak 量化通过（Task 8）。

## 遗留与后续

- 阶段 4：FX/SEQ/EDIT/GLO 完整层、鼓步编辑（DRUM_STEP）、LEN/DIV 编辑、参数页、预置名表。
- 人测清单：真实键盘弹奏手感、四屏视觉复看、听感；DI-4 全矩阵（含修饰组合）随 Task 6 硬件核对收窄，剩余逐键项仍挂账。
