# P4-C 音序与编排 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Tab5 上交付 SLOOP 的完整 64 步 pattern、参数锁、实时录音、A–D sections、quick chain 与 SONG REC，并以 PER3/FUN6 原格式持久化。

**Architecture:** 保留单 TU 与 P4-B `PROJ_HOST project.c`，在 `seq_tab5.c/store_tab5.c` 适配上游 static 动作；arranger 在音频锁内做 RAM scene 切换，停止后经共享存储锁写 PER3。AppState/EditCmd 是唯一产品写路径；STEP/TRANSPORT/SONG 三页提供原生 UI，SONG 独立组以守住 LVGL 峰值预算。

**Tech Stack:** ESP-IDF 5.5.5 / ESP32-P4、FreeRTOS、LVGL 9.6、SLOOP 2.8.5 `bac9c05`、gcc host tests、Python/pyserial 注入。

**Spec:** `docs/superpowers/specs/2026-11-22-p4c-sequencer-arranger-design.md`

## Global Constraints

- `third_party/sloop-fm1/**` 零修改；每次涉及 include/引擎的任务跑 `sha256sum -c third_party/MANIFEST.sha256`（工作目录 `third_party/`）。
- 每次固件任务：`scripts/build.sh && scripts/build.sh` 第二遍零新增警告；引擎等价改动跑 `scripts/host-test.sh`，设备定点/收口跑全量 182/182。
- 所有新写动作经 `state_apply`；console `SEQ` 是与 STORE 同类的诊断执行器，调用同一公共 API。UI 不直接写引擎或 flash。
- 一条编辑命令对应一个完整上游动作和一次引擎锁；渲染内部调用 locked 版本，绝不递归取锁，音频路径不擦写/分配/操作 LVGL。
- PER3 canonical = 88B persist + 1,464B DSU = 1,552B，默认 MIDI 尾段省略；默认 side 80B。合法旧 payload/尾段/side 必须保留，未知 magic 拒绝覆盖。
- LVGL 可用池 63,120B、base 13,800B（保守 14,112B）；STEP ≤23,040B、TRANSPORT ≤9,216B、SONG ≤12,288B。不得扩池；每页构建前 `free ≥ est`。
- 每任务由新 implementer 实施、新 reviewer 审查；修复轮 resume 原 implementer。review clean 后才 push `origin feat/phase4c`，不使用 worktree。
- 提交遵 AGENTS §7：中文 header + `[Motivation]/[Change_Set]/[Impact_AST]/[Infra_State]`；fix 另含 `[Root_Cause]/[Blast_Radius]`。

## Review Focus

1. **空/损坏/旧版 settings**：合法 PER1/2 可迁移，未知 magic 不被清空，PER3 尾段和 side 逐字节保留（Task 7）。
2. **播放中 scene/section 操作**：无全工程 LOAD、无 flash 写、无递归锁；切换只发生在小节边界（Task 2/8/12）。
3. **锁满、不可锁参数、复杂 CONT 跨度**：第 25 锁返回 full，非法操作不写任何字段，note 删除不留下残音（Task 3/4/5）。
4. **free take 与物理/API PLAY 分叉**：物理 PLAY drop，API PLAY close，STOP 不意外提交 take（Task 6/12）。
5. **SELFTEST、切组与诊断槽 3 后的状态**：arrangement 不回默认、页面不泄漏、D 被擦除后歌曲引用正确清理（Task 7/11/12/13）。

---

### Task 0: 规格提交、分支与基线

**Files:**
- Create: `docs/dev/verify/4-pc-2026-11-22/task-0-baseline.txt`
- Commit: 本 spec 与本计划

**Interfaces:**
- Produces: `feat/phase4c`，基线提交号与门禁输出。

- [ ] **Step 1: 提交规格到 dev 并建分支**

```bash
git checkout dev && git pull --ff-only origin dev
git add docs/superpowers/specs/2026-11-22-p4c-sequencer-arranger-design.md docs/superpowers/plans/2026-11-22-p4c-sequencer-arranger.md
git commit -m "docs(superpowers): 新增 P4-C 音序与编排规格及实施计划"
git push origin dev
git checkout -b feat/phase4c
```

- [ ] **Step 2: 跑基线并记录**

Run: `scripts/build.sh && scripts/build.sh && scripts/host-test.sh && scripts/state-test.sh && python3 scripts/screenshot.py --selftest`；记录 dev 提交、零警告、PASS 与 manifest 233/233。
Expected: 全绿；失败则停止，不把既有红灯带入 P4-C。

- [ ] **Step 3: 提交基线并在 review clean 后 push**

Commit `chore(dev/verify): 记录 P4-C 开工基线`；随后 `git push -u origin feat/phase4c`。

---

### Task 1: 扩上游 host 音序/编排回归

**Files:**
- Modify: `scripts/host-test.sh`
- Modify: `docs/design/002-port-roadmap.md`（P4-C 开工行）
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（P4-C 状态）

**Interfaces:**
- Produces: 后续任务必须保持通过的 `hold_rec/len/studio_drums/arranger/song_audio` 门禁。

- [ ] **Step 1: 先单独编译运行五个未接入测试**

Run: 用 host-test 相同 CFLAGS 编 `hold_rec_test.c`、`len_test.c`、`studio_drums_test.c`、`arranger_test.c`、`song_audio_test.c`（需要 `-lm`）。
Expected: 全过；若上游测试需要生成资产，复用脚本已生成头，不另造 fixture。

- [ ] **Step 2: 接入 host-test.sh**

在 `seq2_test` 后按 `len → hold_rec → studio_drums → arranger → song_audio` 编译运行；输出标题说明各自覆盖。不启用 golden 更新模式。

- [ ] **Step 3: 验证并更新开工文档**

Run: `scripts/host-test.sh`；Expected `ALL HOST TESTS PASSED` 且五项输出可见。roadmap 标 P4-C 开工，关联 ADR-010（Task 2 创建）。

- [ ] **Step 4: 提交，review clean 后 push**

Commit `test(scripts): 扩音序录音与编排 host 回归`。

---

### Task 2: arranger 编译骨架与 ADR-010

**Files:**
- Create: `docs/design/010-sequencer-arranger.md`
- Modify: `firmware/components/sloop_core/src/selftest.c`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/components/sloop_core/linker.lf`（仅在 PSRAM 重定向需要时）
- Modify: `docs/design/{005-ui-architecture,008-param-pages,009-storage-backend}.md`

**Interfaces:**
- Consumes: 现有 `proj_slot/proj_apply/sync_reload/ui.force`。
- Produces: `arrangement/arrangement_clock/arrangement_enabled`、`arrangement_apply/song_backup/song_restore` 与 `section_bars()` 可供后续单 TU adapter 使用；默认 arrangement mode 仍 off。

- [ ] **Step 1: 写编译判别测试**

在 host 源级检查/构建断言中证明 `FELUCCA_ARRANGER=1` 时 `seq2_test` quick-chain 段仍执行；固件构建map中 `arrangement_start/arrangement_apply` 各只有一份。
Expected 未改前固件无这两个符号。

- [ ] **Step 2: 调整单 TU 顺序**

在 include `regress.c` 前定义 `FELUCCA_ARRANGER=1`，使 hostsim 顺序为 `arranger.c → seq.c`；`project.c(PROJ_HOST)` 后显式 include `arranger_scene.c`。`song_keep` 用声明宏重定向到 PSRAM，实体指针放 `.noinit` 并在 `sloop_store_init` 分配；不得在渲染路径分配。

- [ ] **Step 3: 写 ADR-010 并更新预算**

记录 PROJ_HOST 子集方案、include 顺序、锁/时钟、PER3 决策、SONG 独立组与 12 页有序对预算；ADR-009 删除「P4-C 不写 settings」旧口径并指向本 ADR。

- [ ] **Step 4: 构建与回归**

Run: 两次 build、`scripts/host-test.sh`、manifest。
Expected: 0 新警告、全测试 PASS；默认 loop golden 完全不变。

- [ ] **Step 5: 提交，review clean 后 push**

Commit `feat(components/sloop_core): 接入 arranger 运行骨架与场景切换`。

---

### Task 3: 原始步、鼓 lane、fill/micro 与参数锁 API

**Files:**
- Create: `firmware/components/sloop_core/include/sloop_seq.h`
- Create: `firmware/components/sloop_core/src/seq_tab5.c`
- Modify: `firmware/components/sloop_core/src/selftest.c`（末尾 include）
- Modify: `firmware/components/sloop_core/src/selftest.c` 窗口快照投影
- Modify: `firmware/main/console.c`
- Create: `scripts/seq-host-test.c` 与 `scripts/seq-test.sh`

**Interfaces:**
- Produces:

```c
typedef struct { uint8_t on[2], lvl[4], rat[4]; } sloop_dstep_t;
typedef struct { uint8_t step, param; int16_t value; } sloop_lock_t;
void sloop_seq_read_dstep(uint32_t step, sloop_dstep_t *out);
esp_err_t sloop_seq_write_dstep(uint32_t step, const sloop_dstep_t *in);
esp_err_t sloop_seq_set_micro(uint32_t track, uint32_t step, int8_t value);
esp_err_t sloop_seq_set_fill(uint32_t track, uint32_t step, uint8_t value);
esp_err_t sloop_seq_lock_set(uint32_t track, uint32_t step, uint32_t param, int16_t value);
esp_err_t sloop_seq_lock_del(uint32_t track, uint32_t step, uint32_t param); /* param==P_COUNT 清整步 */
uint32_t sloop_seq_lock_list(uint32_t track, sloop_lock_t *out, uint32_t cap);
```

快照的合成窗口步增加 `lvl/rat/micro/fill`；读接口冻结语义与现有 core 一致。

- [ ] **Step 1: 写失败 host 测试**

`seq-host-test.c` 通过同源单 TU/stub 断言：micro -32/31 接受、-33/32 拒绝；fill 0..2 接受、3 按上游存储语义处理；锁 `(3,E0)` 与 `(3,DIST)` 共存，同键覆写不耗槽，第 25 项返回 `ESP_ERR_NO_MEM`，P_SLEN 返回 invalid；drum 16 lane 的 on/lvl/rat 原样往返。

- [ ] **Step 2: 跑测试确认 RED**

Run: `scripts/seq-test.sh`；Expected 编译失败或 API 缺失，不以跳过代替 RED。

- [ ] **Step 3: 实现薄 wrapper 与 console**

wrapper 外持引擎锁，直接调用上游 `lock_set/lock_del` 和打包访问器；不在状态层重打包。console 加 `SEQ DSTEP [step]`、`SEQ MICRO <track> <step> <v>`、`SEQ FILL <track> <step> <v>`、`SEQ LOCK LIST|SET|DEL …`，full/invalid 令牌分开。

- [ ] **Step 4: 跑 GREEN 与完整回归**

Run: `scripts/seq-test.sh && scripts/host-test.sh && scripts/build.sh && scripts/build.sh`。
Expected: 全 PASS、0 新警告。

- [ ] **Step 5: 提交，review clean 后 push**

Commit `feat(components/sloop_core): 开放完整步字段与参数锁编辑`。

---

### Task 4: 上游 span/note/pattern 工具与 undo

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_seq.h`
- Modify: `firmware/components/sloop_core/src/seq_tab5.c`
- Modify: `scripts/seq-host-test.c`
- Modify: `firmware/main/console.c`

**Interfaces:**
- Produces:

```c
typedef enum { SEQ_STEP_SPAN, SEQ_STEP_NOTE_DEL, SEQ_STEP_NOTE_MOVE, SEQ_STEP_NOTE_COPY,
               SEQ_STEP_LVL, SEQ_STEP_RAT, SEQ_STEP_SLIDE, SEQ_STEP_CLEAR } sloop_step_op_t;
typedef struct { uint8_t track, step, index, target; int16_t value; } sloop_step_edit_t;
esp_err_t sloop_seq_step_edit(const sloop_step_edit_t *cmd);
typedef enum { SEQ_PATTERN_ROTATE, SEQ_PATTERN_LENGTH, SEQ_PATTERN_TRANSPOSE,
               SEQ_PATTERN_CLEAR, SEQ_PATTERN_UNDO, SEQ_PATTERN_REDO } sloop_pattern_op_t;
esp_err_t sloop_seq_pattern(uint32_t track, sloop_pattern_op_t op, int32_t arg);
```

- [ ] **Step 1: 写复杂跨度与工具判别测试**

固定用例：C1 以 CONT 跨 C2/D2/E2，删 C1 后其它音不变且无残留；move/copy 带 micro/fill/lock，复杂 CONT 未先缩 span 时拒绝；rotate 首尾回绕但 lock/micro/fill 锚定；LEN 16→32 复制前半、3→1；transpose 0/127 饱和且鼓轨拒绝；clear 清 lock/micro/fill 不改 LEN；undo 只恢复 steps/micro/LEN，不恢复 fill/lock。

- [ ] **Step 2: RED 后移植纯动作**

从 `ui.c/ui_layers.c` 提取 `note_span/note_span_set/note_del/note_shift/pattern_*` 与 undo session 到 adapter，保持上游语句和边界；用 adapter 自有 `step_sess` 替代 240×240 `ui.step_sess`。公共入口一次持锁。

- [ ] **Step 3: 加 console 与 GREEN**

`SEQ EDIT ...`、`SEQ PATTERN <rotate|length|transpose|clear|undo|redo> …` 返回明确结果。Run seq-test + host-test + 两次 build，全 PASS。

- [ ] **Step 4: 提交，review clean 后 push**

Commit `feat(components/sloop_core): 移植音符跨度与 pattern 工具`。

---

### Task 5: 状态层命令与完整投影

**Files:**
- Modify: `firmware/components/sloop_state/include/sloop_state.h`
- Modify: `firmware/components/sloop_state/src/{cmd,state}.c`
- Modify: `firmware/components/sloop_state/test/*`（沿用现有测试文件）
- Modify: `scripts/state-test.sh`
- Modify: `firmware/main/console.c`（`STATE SNAP`/`ENG STEP` 完整字段）

**Interfaces:**
- Consumes: Task 3/4 API。
- Produces: `CMD_STEP_EDIT{p=sloop_step_edit_t}`、`CMD_PATTERN{a=track,b=op,v=(uint16_t)arg}`、`CMD_SET_MICRO/FILL`、`CMD_LOCK_SET/DEL`、`CMD_DRUM_STEP_WRITE{p=sloop_dstep_t,b=step}`；`app_state_t.seq_status` 与详细窗口步。

- [ ] **Step 1: 写 stub 失败测试**

断言：非法 track/step/param 不调引擎；完整 POD 字段透传；ESP_ERR_NO_MEM→`SEQ_STATUS_FULL`；成功命令同拍刷新详细窗口；LOAD 等外部 sel 改变后 `focus_track=selected_track`；drum 写不改合成步。

- [ ] **Step 2: RED 后实现命令分发**

沿用 `CMD_SET_PARAM_BATCH` 的同步 `p` 生命周期；`state_apply` 返回前指针有效。状态层不重复实现上游钳位/锁算法。

- [ ] **Step 3: 替换 MVP 删除语义并更新旧测试**

toggle/erase 走 Task 4 完整上游动作；清步同时清 lock/micro/fill。修改旧测试期望是因为语义升级，不得删除 CONT 判别断言。

- [ ] **Step 4: 全回归并提交**

Run: `scripts/state-test.sh && scripts/seq-test.sh && scripts/host-test.sh && scripts/build.sh && scripts/build.sh`。Commit `feat(components/sloop_state): 接入完整音序编辑命令`，review clean 后 push。

---

### Task 6: 实时录音、鼓键盘输入与 REC 状态

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_seq.h`
- Modify: `firmware/components/sloop_core/src/seq_tab5.c`
- Modify: `firmware/components/sloop_state/{include/sloop_state.h,src/cmd.c}`
- Modify: `firmware/components/sloop_ui/src/input.c`
- Modify: `docs/design/006-keyboard-mapping.md`
- Modify: `firmware/main/console.c`
- Modify: `scripts/{seq-host-test.c,state-test.sh}` 涉及测试

**Interfaces:**
- Produces:

```c
typedef enum { SEQ_REC_OFF, SEQ_REC_ARMED, SEQ_REC_RECORDING, SEQ_REC_COUNT_IN, SEQ_REC_FREE_TAKE } sloop_rec_state_t;
typedef struct { uint8_t state, track, quant_off, tempo_mode, count_mode, take_bars; } sloop_rec_snapshot_t;
void sloop_seq_rec_read(sloop_rec_snapshot_t *out);
esp_err_t sloop_seq_rec_toggle(void);
esp_err_t sloop_seq_rec_config(uint8_t quant_off, uint8_t tempo_mode, uint8_t count_mode);
esp_err_t sloop_seq_fill_control(bool held, bool next_bar);
```

状态层新增 `CMD_REC_TOGGLE/CONFIG`、`CMD_FILL_CONTROL`、`CMD_DRUM_HIT{a=lane,v=velocity}`。

- [ ] **Step 1: 写失败测试**

固定：非空工程停止态 arm 后首音启动并写 step0；REC OFF 不停播放；选轨移动 rec 位；count-in 四拍；POLY 独立释放；free take 有效关闭与 <0.5s 丢弃；物理 PLAY drop 与 API PLAY close 分开；16 鼓 lane 映射不回落 track0。

- [ ] **Step 2: 移植 REC 动作与状态投影**

复用 `rec_toggle/rec_begin/rec_follow` 上游语义；`sloop_core_select` 补 `rec_follow()`。一次性 `rec_go/ft_bars` 由快照消费，不在绘制函数里清。quant/count/tempo 仅运行期配置，本任务不写 settings。

- [ ] **Step 3: 接 InputManager**

鼓轨 `z x c v b n m .`→lane0..7，`q w e r t y u i`→lane8..15；合成轨原 24 音不变。space 走带使用上游物理按钮路径，保证 free take drop；其它/API play 保持 close。

- [ ] **Step 4: 验证与文档**

Run seq/state/host/build；ADR-006 写新键表与 12 页 TAB 顺序预留。Expected 全 PASS。

- [ ] **Step 5: 提交，review clean 后 push**

Commit `feat(firmware): 接通实时录音与十六 lane 鼓演奏`。

---

### Task 7: PER3 歌曲设置与整扇区安全测试

**Files:**
- Create: `scripts/persist-host-test.c`（或并入 seq-host-test.c 的独立 section）
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/main/console.c`
- Modify: `docs/design/009-storage-backend.md`

**Interfaces:**
- Produces:

```c
typedef struct { uint8_t scene, bars; } sloop_song_entry_t;
typedef struct { uint8_t count, loop; sloop_song_entry_t entry[16]; } sloop_song_config_t;
esp_err_t sloop_store_song_get(sloop_song_config_t *out);
esp_err_t sloop_store_song_set(const sloop_song_config_t *cfg);
void sloop_store_sections_tick(uint32_t now_ms);
```

内部提供 `arrangement_save()` 同义 adapter、PER1/2 迁移、sec/song dirty flush；selftest 结束后重载 arrangement。

- [ ] **Step 1: 写字节级失败 fixture**

断言：空设置→1,552B canonical + 80B side；arrangement offset48/entries52；PER3 DSU/MIDI 尾段与 side 保存前后逐字节相同；PER1/2 迁移字段正确；未知 magic 返回 invalid 且原扇区不变；非法 arrangement 回默认；失败保存保留 dirty。

- [ ] **Step 2: RED 后实现 PER3 子集**

使用上游结构布局、默认 DSU/theme/favourites 与 `st_save3`；完整读出现有 payload，仅改 36B arrangement。禁止只写 36B/88B 覆盖已有对象。

- [ ] **Step 3: 修 STORE TEST 快照**

快照 settings 两个完整 4KB 扇区，结束恢复并逐字节校验；新增测试证明 arrangement/尾段/side 不被六场景破坏。失败响亮报 `restore-settings`。

- [ ] **Step 4: sections flush 与 SELFTEST 恢复**

复用 1s 存储任务和 esp_timer 毫秒；停止+quiet+idle1.5s，失败重试5s。SELFTEST 后重载歌曲与必要钩子；不在渲染中写。

- [ ] **Step 5: 验证并提交**

Run persist/seq/host/state/两次 build。Commit `feat(components/sloop_core): 以 PER3 持久化歌曲并保护设置副本`，review clean 后 push。

---

### Task 8: sections、chain、SONG 公共命令与状态层

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_seq.h`
- Modify: `firmware/components/sloop_core/src/seq_tab5.c`
- Modify: `firmware/components/sloop_state/{include/sloop_state.h,src/cmd.c}`
- Modify: `firmware/main/console.c`
- Modify: `scripts/seq-host-test.c` 与状态层测试

**Interfaces:**
- Produces:

```c
typedef struct {
  uint8_t enabled, running, index, bar, ready, live, dirty, chain_count, srec;
  sloop_song_config_t config;
} sloop_song_snapshot_t;
void sloop_seq_song_read(sloop_song_snapshot_t *out);
esp_err_t sloop_seq_section(uint32_t slot, bool store, bool erase);
esp_err_t sloop_seq_section_request(uint32_t slot);
esp_err_t sloop_seq_chain_begin(uint32_t first);
esp_err_t sloop_seq_chain_commit(const uint8_t *scenes, uint32_t count);
esp_err_t sloop_seq_song_mode(bool enabled);
esp_err_t sloop_seq_song_rec(void);
```

状态层对应 `CMD_SECTION_*`、`CMD_CHAIN_*`、`CMD_SONG_MODE/REC/SET`；project erase 同步 `arr_drop_scene/live_sec/song_dirty`。

- [ ] **Step 1: 写失败行为测试**

A2+B3 loop=0 自然停止并恢复原 loop hash/sel；loop=1 STOP 恢复；后续缺 scene 整首拒播；live STORE 播放中立即可用且不写 flash；chain A/B/A 自动 bars、8 项上限、单 section cancel；SONG REC 从下一小节、64 bars 拆分、16 项完成、空录制 NO SONG。

- [ ] **Step 2: 实现 wrapper 与 dirty writer 接线**

begin(first) 立即 `live_req`，commit 原子安装 1..8 项；scene 切换只走上游渲染路径。section capture 引擎锁内，flash 由 Task 7 tick 锁外执行。

- [ ] **Step 3: 接 console 与状态投影**

`SEQ SECTION/CHAIN/SONG/SREC` 输出 ready/live/dirty/index/bar/config；状态层成功/失败更新 `seq_status`，UI 不需要读上游全局。

- [ ] **Step 4: 全回归并提交**

Run seq/state/host/两次 build。Commit `feat(firmware): 接通 sections、quick chain 与 SONG REC`，review clean 后 push。

---

### Task 9: STEP 深度呈现与鼓 lane 编辑

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_step.c`
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`
- Modify: `firmware/components/sloop_ui/src/input.c`
- Modify: `scripts/inject/inject.py`（坐标 helper 与既有 step_edit 期望）
- Modify: `docs/design/005-ui-architecture.md`

**Interfaces:**
- Consumes: 详细窗口步、`CMD_STEP_EDIT/DRUM_STEP_WRITE`。
- Produces: STEP 格内 TIE/CONT/SLIDE/lvl/rat/micro/fill 视觉状态与聚焦步操作；鼓 lane 选择/编辑。

- [ ] **Step 1: 更新注入断言使其先失败**

断言 TIE 与 NOTE 不同、SLIDE/fill/micro 可见、鼓格可选 lane 并写原始位图；列隐藏/绝对步号/播放头旧断言全部保留。

- [ ] **Step 2: 实现复用对象的绘制**

64 格对象不增加每格 label；用自绘或共享样式表达状态，文本分配有界。鼓编辑经同一动作函数发状态命令，不再只写 PHASE 4 提示。

- [ ] **Step 3: 设备定点验证**

Run build/flash、`scripts/inject/step_edit.sh`；截屏到暂存并用视觉工具读取。STEP delta ≤23,040B，超预算则减少对象/文本而不是抬 est。

- [ ] **Step 4: 提交，review clean 后 push**

Commit `feat(components/sloop_ui): 呈现完整步状态并开放鼓步编辑`。

---

### Task 10: TRANSPORT 录音、fill 与 pattern 工具区

**Files:**
- Modify: `firmware/components/sloop_ui/src/page_transport.c`
- Modify: `firmware/components/sloop_ui/{include/sloop_ui.h,src/input.c}`
- Modify: `firmware/components/sloop_ui/src/theme.h`（仅共享几何/状态文案）
- Modify: `docs/design/{005-ui-architecture,008-param-pages}.md`

**Interfaces:**
- Consumes: REC/fill/pattern/聚焦步命令。
- Produces: 控件回调与 TAP 共用动作函数；录音五态、工具错误和锁满状态在屏幕可见。

- [ ] **Step 1: 写控件动作/注入失败用例**

REC arm/off、fill held/next-bar、ROT±、LEN×2/÷2、TRANS±、CLEAR、UNDO/REDO 与聚焦步 lvl/rat/micro/span/slide 各产生一条对应 EditCmd；非法动作不改状态。

- [ ] **Step 2: 实现单对象工具区**

保留 PLAY/STOP/BPM；新增工具用可点击 label，不用 `lv_button+child`。几何宏同时供建控件与 TAP；TRANSPORT est 设 9,216B，实测 delta 必须更低。

- [ ] **Step 3: 验证截图和内存**

Run build/flash + transport/step_edit 注入；视觉检查 REC/工具/错误态；`UI MEM GROUP SEQ sum_est=32256`。按新 est 重算并记录 SEQ→PRESETS 62,952B。

- [ ] **Step 4: 提交，review clean 后 push**

Commit `feat(components/sloop_ui): 新增录音与 pattern 操作区`。

---

### Task 11: SONG 页与六组导航

**Files:**
- Create: `firmware/components/sloop_ui/src/page_song.c`
- Modify: `firmware/components/sloop_ui/src/ui.c`
- Modify: `firmware/components/sloop_ui/{include/sloop_ui.h,src/input.c,src/theme.h}`
- Modify: `firmware/components/sloop_state/include/sloop_state.h`
- Modify: `firmware/components/sloop_ui/CMakeLists.txt`
- Modify: `firmware/main/console.c`
- Modify: `docs/design/{005-ui-architecture,008-param-pages}.md`

**Interfaces:**
- Consumes: Task 8 song API/命令。
- Produces: `APP_PAGE_SONG=11`、`APP_STATE_PAGES=12`；组 `PLAY/TRACKS/EDIT/SEQ/SONG/FILE`；SONG 页 section/config/chain/rec 控件。

- [ ] **Step 1: 先写导航与页面动作失败断言**

12 页扁平 TAB 顺序含 SONG；`UI song`、`UI TAB song` 成功；A–D 卡、16 项、bars/loop、STORE/ERASE/CHAIN/SREC 与状态层命令一致；空 scene 播放拒绝可见。

- [ ] **Step 2: 实现 SONG 单对象页面**

初建 est 12,288B；配置编辑不直接写全局，统一发命令。chain SAVE 层 begin/commit 与 spec 一致；project erase 后歌曲项同步消失。

- [ ] **Step 3: 六组页签与预算实测**

组签仍 16pt、72px；SONG 独立组。`UI MEM` 每页 delta≤est，SONG↔PLAY×10 与 SEQ↔SONG×10 的 pool_free 差≤128B；最坏 FILE→STEP free≥4,096B。

- [ ] **Step 4: 设备截图视觉检查**

归档 SONG 空态、A2+B3 编辑态、播放 scene B、chain 与 NO SONG/invalid 状态；视觉读取无裁切/残留/不可读文本。

- [ ] **Step 5: 提交，review clean 后 push**

Commit `feat(components/sloop_ui): 新增 SONG 编排页与六组导航`。

---

### Task 12: sequence 端到端注入矩阵

**Files:**
- Create: `scripts/inject/sequence.sh`
- Modify: `scripts/inject/inject.py`
- Modify: `scripts/soak.py`（访问 SONG、六组往返与 sequence 动作）
- Modify: `AGENTS.md` §3/§5.1/§5.4.1/§5.5
- Create: `docs/dev/verify/4-pc-2026-11-22/task-12-*`

**Interfaces:**
- Consumes: 全部公共 API 与三页 UI。
- Produces: 第七条可重复设备场景；soak 覆盖新页面和录音/编排。

- [ ] **Step 1: 写矩阵并先对最终固件跑通归一逻辑**

场景依次：详细步/锁/工具 → REC/free take/鼓16lane → live sections/dirty flush → chain → SONG A2+B3/loop restore → SONG REC → STORE TEST/SELFTEST 后歌曲保持 → 硬复位 PER3 恢复。每段显式归一 AUTO、走带、选中轨、窗口与诊断槽3。

- [ ] **Step 2: 加入判别性反例**

第25锁 full、非法 lock 不变、缺 scene PLAY 拒绝、chain 9 项拒绝、空 SONG REC 不写配置、物理/API PLAY free-take 分叉；不得只测 happy path。

- [ ] **Step 3: 跑七场景同会话**

Run: 原六场景 + `scripts/inject/sequence.sh /dev/ttyACM0`；Expected 全退出0。sequence 失败先修实现/脚本后从头重跑，不删断言。

- [ ] **Step 4: 视觉与证据归档**

复制 sequence 全部门禁截图/transcript 到证据目录；ReadMediaFile 逐张检查，失败图加 `-fail` 并保留。

- [ ] **Step 5: 提交，review clean 后 push**

Commit `test(scripts): 新增音序录音与编排设备矩阵`。

---

### Task 13: 全门禁、终审修复与阶段收口

**Files:**
- Create: `docs/dev/verify/4-pc-2026-11-22/{readme.md,SHA256SUMS}`
- Modify: `docs/dev/verification-log.md`
- Modify: `docs/design/002-port-roadmap.md`
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`
- Modify: `docs/superpowers/specs/2026-11-22-p4c-sequencer-arranger-design.md`（状态/实测）
- Modify: `docs/deferred-issues.md`（删除已关闭 DI-24；新增仅真实遗留）

**Interfaces:**
- Produces: 可合并 dev 的证据包与阶段状态。

- [ ] **Step 1: 整分支终审与修复波**

新 reviewer 对照 spec 全量审 diff；implementer 修复所有 critical/important 与可复现 minor，重跑受影响门禁。review clean 前不收口。

- [ ] **Step 2: 标准机器门禁**

Run: 两次 build、flash（Hash verified）、host/state/seq/knob/screenshot selftest、manifest、device-selftest 182/182、七注入场景。保存原始日志。

- [ ] **Step 3: 复位后长时 soak**

Run: `scripts/soak.py /dev/ttyACM0 --duration 1800 --interval 60 --save-window 900`。
Expected: PASS；若 selftest 已跑必须先复位，遵守 DI-26，不把已知泄漏伪造成新回归。

- [ ] **Step 4: 证据哈希与文档收口**

readme 建「门禁→命令→日志→结论」表；`sha256sum` 覆盖全部证据并校验；verification-log 登记提交与 SHA。roadmap/phase4 spec 标 P4-C 机器侧完成，人测项保留。

- [ ] **Step 5: 提交、push、合并 dev**

Commit `docs(dev/verify): 登记 P4-C 全量门禁与阶段证据` 并 push feature；随后：

```bash
git checkout dev && git pull --ff-only origin dev
git merge --no-ff feat/phase4c -m "merge: P4-C 音序与编排（完整步编辑 / 录音 / sections / SONG）"
scripts/build.sh && scripts/build.sh && scripts/host-test.sh && scripts/state-test.sh
git push origin dev
```

Expected: 合并后全绿；最终报告列机器已证、人测遗留和 P4-D 入口。
