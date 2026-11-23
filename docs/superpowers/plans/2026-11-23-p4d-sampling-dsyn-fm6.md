# P4-D 采样、DSYN 鼓合成与 FM6 深度 patch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Tab5 上交付 USR1–4 采样槽、SYN1–4 DSYN 鼓合成编辑与 FM6 155B patch 深度编辑，并保持 PER3/FUN6/原始 flash 槽的上游字节兼容。

**Architecture:** 采样槽用上游原始 flash 区（0xA0000/0xB4000/0xC8000/0xE7000）+ 既有 4×80KiB PSRAM 镜像；DSYN 直接编辑 `dsu` 并写回 PER3 settings 的 88..1551；FM6 直接编辑 `fm6_patch` 并经既有 bank 27 槽持久化。新增 `SYNTH` 顶层组（SAMPLE/DSYN/FM6P），页面仍是 8 旋钮 + 页内选择器；编辑只经状态层命令。

**Tech Stack:** ESP-IDF 5.5.5 / ESP32-P4、FreeRTOS、LVGL 9.6、SLOOP 2.8.5 `bac9c05`、gcc host tests、Python/pyserial 注入。

**Spec:** `docs/superpowers/specs/2026-11-23-p4d-sampling-dsyn-fm6-design.md`

## Global Constraints

- `third_party/sloop-fm1/**` 零修改；涉及 include/引擎的任务跑 `sha256sum -c third_party/MANIFEST.sha256`（工作目录 `third_party/`）。
- 每个固件任务：`scripts/build.sh && scripts/build.sh` 第二遍零新增警告；引擎等价改动跑 `scripts/host-test.sh`；设备收口跑全量 182/182。
- 所有产品写动作经 `state_apply`；console `SMP/DSYN/FM6P` 是诊断执行器，调用同一公共 API；UI 不直接写引擎或 flash。
- 一条编辑命令 = 一个完整动作 = 一次引擎锁；渲染路径不取写锁、不擦写、不分配、不操作 LVGL。
- 采样 SET 索引 = `SMP_NSETS + k` = **8..11**；8 是 USR1，不是 32。
- DSYN 24 字段编号冻结（§4.1 顺序）；`FLT` 拆 MODE/RES，`CLAP` 独立，写回仍是原始 22 字节。
- PER3 canonical = 88B persist + 1,464B DSU = 1,552B；任何 settings 保存都写当前 `dsu`；未知 magic 拒绝覆盖，尾段/side 逐字节保留。
- LVGL 池 63,120B、base 11,624B；新页 est：SAMPLE 6,144 / DSYN 7,168 / FM6P 7,168；SYNTH Σ 20,480；不得扩池，构建前 `free ≥ est`。
- 走带中采样上传/擦除、DSYN STORE、FM6 bank 写一律拒绝（`playing`）。
- 子代理驱动、**不用 worktree**；每任务一个新 implementer + 新 reviewer，修复轮 resume 原 implementer；review clean 后 push `origin feat/phase4d`。
- 提交遵 AGENTS §7：`<type>(<模块>): 中文摘要` + `[Motivation]/[Change_Set]/[Impact_AST]/[Infra_State]`；fix 另含 `[Root_Cause]/[Blast_Radius]`。

## Review Focus

1. **采样上传中断/坏 header**：`SMP BEGIN` 后掉电或 CRC/zone 错时槽必须为空且不发声，不能留下半覆盖槽被 `SAMPLE` 播到（Task 4/6/12）。
2. **DSYN 保存碰撞歌曲/尾段/side**：DSYN STORE 与歌曲延迟写共用 settings 时，48..83 之外、MIDI 尾段、side record 不得被改写；未知 magic 不得覆盖（Task 5/12）。
3. **自检/复位后的恢复**：`ENG SELFTEST` 回滚 `.bss` 后 DSU 与四槽采样镜像必须重载/重扫，硬复位后 DSU 与采样槽字节不变（Task 4/5/12）。
4. **FM6 编辑被宏/PTCH 覆盖**：深度页写 ALG 后必须清 `P_E0` 宏、写任意字段后 `fm6_slot` 不得让 `fm6_poll` 回滚 patch；非 FM6 轨拒绝（Task 3/6/10/12）。
5. **新组容量**：7 组/14 页的有序对、SYNTH 全建→STEP 首建峰值 ≥4,096B、离开组 built=0、跨组往返 pool_free 差 ≤128B（Task 8/11/12）。

---

### Task 0: 规格提交、分支与基线

**Files:**
- Create: `docs/dev/verify/4-pd-2026-11-23/task-0-baseline.txt`
- Commit: 本 spec 与本计划

**Interfaces:**
- Produces: `feat/phase4d`，基线提交号与门禁输出。

- [ ] **Step 1: 提交规格到 dev 并建分支**

```bash
git checkout dev && git pull --ff-only origin dev
git add docs/superpowers/specs/2026-11-23-p4d-sampling-dsyn-fm6-design.md docs/superpowers/plans/2026-11-23-p4d-sampling-dsyn-fm6.md
git commit -m "docs(superpowers): 新增 P4-D 采样/DSYN/FM6 patch 规格及实施计划"
git push origin dev
git checkout -b feat/phase4d
```

- [ ] **Step 2: 跑基线并记录**

Run: `scripts/build.sh && scripts/build.sh && scripts/host-test.sh && scripts/state-test.sh && scripts/seq-test.sh && python3 scripts/screenshot.py --selftest`
Expected: 全绿；记录 dev 提交、第二遍零警告、`ALL HOST TESTS PASSED`、manifest 233/233。失败则停止，不带红灯进入 P4-D。

- [ ] **Step 3: 提交基线并在 review clean 后 push**

Commit `chore(dev/verify): 记录 P4-D 开工基线`；`git push -u origin feat/phase4d`。

---

### Task 1: 上游 SAMPLE 回归与 synth 门禁骨架

**Files:**
- Modify: `scripts/host-test.sh`
- Create: `scripts/synth-test.sh`
- Create: `scripts/synth-host-test.c`
- Modify: `docs/design/002-port-roadmap.md`（P4-D 开工行）
- Modify: `docs/superpowers/specs/2026-11-21-phase4-feature-parity-design.md`（P4-D 状态）

**Interfaces:**
- Produces: `scripts/synth-test.sh`（后续任务的 host 门禁）；`synth-host-test.c` 的 hostsim include 骨架。

- [ ] **Step 1: 接入上游 `sample_pitch_test`**

在 `scripts/host-test.sh` 的 `fm6_test` 之后按同 CFLAGS 加 `compile sample_pitch_test tests/sample_pitch_test.c -lm` 与 `run "SAMPLE pitch/loop" …`。Run `scripts/host-test.sh`；Expected 新测试可见且全过。

- [ ] **Step 2: 建立可运行的 synth host 骨架**

`scripts/synth-host-test.c`：与 `seq-host-test.c` 同法 `#define main hostsim_main` + include `tests/hostsim.c`，随后 `#define PROJ_HOST 1` include `project.c`；本任务先不 include `sound_tab5.c`，`main()` 打印 `SYNTH HOST TEST PENDING` 并返回 0。
`scripts/synth-test.sh`：复制 `seq-test.sh` 的 CFLAGS/include，编译到 `$PROJECT_DIR/build/synth-test/synth_host_test` 并运行。

- [ ] **Step 3: 运行骨架并通过**

Run: `scripts/synth-test.sh`
Expected: `SYNTH HOST TEST PENDING`，exit 0（真正的失败测试在 Task 2 引入 `sound_tab5.c` 后写）。

- [ ] **Step 4: 更新开工文档并提交**

roadmap 阶段表 P4-D 行标“开工（2026-11-23，`feat/phase4d`）”；parity spec 的 P4-D 状态同步。Commit `test(scripts): 接入 SAMPLE 回归并建立 synth host 门禁骨架`。

---

### Task 2: DSYN 公共 API、24 字段描述符与工厂 kit

**Files:**
- Create: `firmware/components/sloop_core/src/sound_tab5.c`
- Modify: `firmware/components/sloop_core/src/selftest.c`（末尾 include `sound_tab5.c`）
- Modify: `firmware/components/sloop_core/include/sloop_core.h`
- Modify: `scripts/synth-host-test.c`
- Test: `scripts/synth-test.sh`

**Interfaces:**
- Consumes: 上游 `dsu`/`dsu_kit`/`dsu_from_factory`/`dsu_fix_sound`/`dsu_fix_kit`/`dsu_dirty`/`dsu_aud_*`（同一 TU）。
- Produces: `sloop_dsyn_field_t`、`sloop_dsyn_sound_t`、`sloop_core_dsyn_{desc,read,field,set,load_factory,audition,dirty}`、`sloop_dsyn_field_from_sound`、`sloop_core_dsyn_text`。

- [ ] **Step 1: 写失败测试（24 字段读写/钳位/FLT 组合/工厂 kit）**

在 `synth-host-test.c` 加 `test_dsyn_fields()`：断言字段数 24；`SET` WAVE=1 / PITCH=60 / FINE=15 / BEND=96 / T2=31 / LEVEL=255 后 `GET` 原始 22B 的对应字节；`FLT_MODE=LP(1)` 后 `FLT_RES=31` 再读 MODE 仍 1，原始 `flt==(31<<3)|1`；`CLAP=1` 后 `SRC` 低 4 位不变、bit4 置位；`HOLD=200` 回读 127；`load_factory(0, 0)` 后 `name=="808"` 且 22B 等于 `DS_KITS[0]` 对应 lane。

- [ ] **Step 2: 运行确认失败**

Run: `scripts/synth-test.sh`；Expected 编译/链接失败（符号未定义）。

- [ ] **Step 3: 实现 API**

`sound_tab5.c` 内含：24 字段枚举/描述符表（范围与枚举名按 spec §4.1）；原始字节偏移映射（`dsnd_t` 的 22 字节顺序）；`FLT_MODE/RES`、`CLAP/SRC` 的组合读写；`sloop_core_dsyn_set` 在引擎锁内 `dsu_fix_sound` 后回读；`load_factory` 调 `dsu_from_factory` + `dsu_dirty=1`；`audition` 写 `dsu_aud_k/lane/vel`；`dirty` 读 `dsu_dirty`。`sloop_core_dsyn_read` 只读、引擎锁内 memcpy。`sloop_core_dsyn_text` 用 `DECAY_K`/`CUTOFF_HZ` 给 ms/Hz 文本（`BTIME/DECAY/NDEC/FCUT/HPF/CHIP`），其余字段返回通用整数文本。`selftest.c` 末尾在 `seq_tab5.c` 后加 `#include "sound_tab5.c"`。

- [ ] **Step 4: 运行测试**

Run: `scripts/synth-test.sh`；Expected `SYNTH HOST TEST PASSED`。

- [ ] **Step 5: 构建并提交**

Run: `scripts/build.sh && scripts/build.sh`；Expected 0 警告。Commit `feat(components/sloop_core): 新增 DSYN 24 字段编辑与工厂 kit API`。

---

### Task 3: FM6 155B patch 描述符与字段写入口

**Files:**
- Modify: `firmware/components/sloop_core/src/sound_tab5.c`
- Modify: `firmware/components/sloop_core/include/sloop_core.h`
- Modify: `scripts/synth-host-test.c`

**Interfaces:**
- Consumes: `fm6_patch/fm6_slot/fm6_set_patch/fm6_max/fm6_sanitize/fm6_pack/fm6_unpack`（同一 TU）。
- Produces: `sloop_fm6_patch_snapshot_t` 与 `sloop_core_fm6_{desc,patch,field,set}`、`sloop_fm6_field_from_patch`。

- [ ] **Step 1: 写失败测试**

`test_fm6_fields()`：`desc` 覆盖 6×21 + 29 项；`set(track,0,R1,99)` / `L4=99` / `OL=99` / `DET=14` / `FC=31` 后 `patch()` 对应字节；`set(track,6,ALG,31)` 后 `patch[FP_ALG]==31` 且 `trk[track].p[P_E0]==0`（宏清 PAT）；`set(track,0,OL,200)` 回读 99；非 FM6 轨（ANALOG）回 `ESP_ERR_NOT_ALLOWED`；`fm6_pack`→`fm6_unpack` 往返与 `patch()` 逐字节一致。

- [ ] **Step 2: 运行确认失败**

Run: `scripts/synth-test.sh`；Expected 链接失败。

- [ ] **Step 3: 实现**

`sloop_core_fm6_desc(op,field)` 返回静态描述符（算子字段 0..20 映射 `FP_*`，全局 0..28 映射 `FP_PR1..FP_NAME`；`ALG` 0..31，UI 文案用 0=PAT 的显示由页面定）；`sloop_core_fm6_patch(track,...)` 在引擎锁内 memcpy `fm6_patch[track]`，`engine_ok = trk[track].eng_req==ENGI_FM6`，`slot=trk[track].p[P_E7]`；`sloop_core_fm6_set` 校验 track/op/field、非 FM6 返回 `NOT_ALLOWED`、冻结返回 `INVALID_STATE`，写 `v` 后 `fm6_sanitize` + `fm6_set_patch` + `fm6_slot[track]=trk[track].p[P_E7]`，ALG 字段额外 `trk[track].p[P_E0]=0`。

- [ ] **Step 4: 运行测试并通过**

Run: `scripts/synth-test.sh`；Expected PASS。

- [ ] **Step 5: 提交**

Run `scripts/build.sh && scripts/build.sh`，然后 Commit `feat(components/sloop_core): 新增 FM6 155B patch 字段编辑 API`。

---

### Task 4: 采样槽 flash+镜像存储 API 与开机装载

**Files:**
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/sound_tab5.c`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`（init 装载）
- Modify: `scripts/synth-host-test.c`

**Interfaces:**
- Produces: `sloop_store_sample_{info,begin,hdr_append,write,end,erase,reload}` 与 `sloop_store_sample_info_t`。
- Consumes: `smp_user_scan`、`SMP_USER_OFF/SIZE/DATA`、`smp_zone_ok`、`st_crc32`、`sloop_hal_storage_{erase,prog,read}`、`sloop_hal_user_samples()`。

- [ ] **Step 1: 写失败测试（header/CRC/zone/中断）**

`test_sample_slot()`：用 `sampleio` 等价的小 header（1 zone，data 1KB，CRC 正确）走 `begin→hdr_append(480B)→write(off=512)→end`，断言 `info.used=1`、name、zones、KB；`begin` 后不 `end` 时 `info.used=0` 且镜像 header 是 0xFF；CRC 改 1 字节 → `end` 回 `ESP_ERR_INVALID_CRC` 且 `used=0`；zone `n=0`/`off` 越界 → `ESP_ERR_INVALID_ARG`；`off<512`、`off` 非 256 对齐、`off+n>0x14000` → `ESP_ERR_INVALID_SIZE`；走带中 `begin/erase` → `ESP_ERR_INVALID_STATE`。

- [ ] **Step 2: 运行确认失败**

Run: `scripts/synth-test.sh`；Expected 链接失败。

- [ ] **Step 3: 实现**

`sound_tab5.c` 增加 `s_smp_open` 与 480B header 暂存；BEGIN：清 open/header、`usr_nz[k]=0`、`usr_zone[k][*].n=0`、镜像 header 0xFF、`sloop_hal_storage_erase(SMP_USER_OFF(k))`；HDR：累计 ≤480；WRITE：校验并 `prog`，同步 `memcpy` 镜像；END：`smp_zone_ok` 逐 zone + `st_crc32(镜像+512,data_len)` 校验，`prog` header，整槽 flash→镜像，`smp_user_scan(k)`；ERASE：16 扇区 erase + 镜像 0xFF + 失效；INFO 读镜像 header（magic/version/nz/zone 合法才算 used）。所有入口持 `s_store_lock`（通过 `store_tab5.c` 暴露的内部锁函数或在 sound_tab5.c 内调用公共包装），检查 ready/冻结/走带。`store_tab5.c` 的 `sloop_store_init` 在 HAL ready 后逐槽 `memcpy` flash→`sloop_hal_user_samples()` 并 `smp_user_scan(k)`。

- [ ] **Step 4: 运行 host 与构建**

Run: `scripts/synth-test.sh && scripts/build.sh && scripts/build.sh`；Expected PASS、0 警告。若 `s_store_lock` 在 sound_tab5.c 不可见，改为调用 `store_tab5.c` 新增的 `sloop_store_sample_lock()/unlock()` 内部入口，禁止复制锁。

- [ ] **Step 5: 提交**

Commit `feat(components/sloop_core): 落地采样槽 flash+镜像存储与开机装载`。

---

### Task 5: DSU 随 PER3 持久化与自检后重载

**Files:**
- Modify: `firmware/components/sloop_core/src/song_persist.h`
- Modify: `firmware/components/sloop_core/src/store_tab5.c`
- Modify: `firmware/components/sloop_core/include/sloop_store.h`
- Modify: `firmware/components/sloop_core/src/selftest.c`
- Modify: `scripts/synth-host-test.c`（或 `scripts/persist-host-test.c`）

**Interfaces:**
- Produces: `song_per_put_dsu()`、`sloop_store_dsu_reload/store/dirty`。
- Consumes: `dsu`/`dsu_valid`/`dsu_defaults`/`dsu_fix_kit`、`st_load/st_save3`、`song_write_locked`。

- [ ] **Step 1: 写失败测试**

在 persist 段：canonical 1,552B 的 88..1551 等于当前 `dsu`；把 `dsu.k[0].s[0].pitch` 改 42 后 `song_per_put_dsu` 写回，后续 `song_per_read`/`dsu_boot` 等价函数读回 42；84/88B 旧 PER3 保存后 `len==1552` 且 48..83 仍替换、尾段（1,584/1,588/1,592）逐字节保留；未知 magic 仍返回 BAD；保存成功 `dsu_dirty==0`。

- [ ] **Step 2: 运行确认失败**

Run: `scripts/synth-test.sh` 或 `scripts/seq-test.sh`（persist 段）；Expected 新断言失败。

- [ ] **Step 3: 实现**

`song_persist.h` 增 `static void song_per_put_dsu(uint8_t *payload, uint32_t len)`：`len >= SONG_PER_CANON` 时 `memcpy(payload+88,&dsu,1464)`；`song_per_build` 在确定 `kind` 与最终 `len`（≥canonical）后调用它。`store_tab5.c`：`song_write_locked` 成功后 `dsu_dirty=0`；新增 `sloop_store_dsu_reload()`（`st_load(OBJ_SETTINGS)` → 88 起 `dsu_valid` 则 memcpy + 逐 kit `dsu_fix_kit`，否则 `dsu_defaults()`，清 dirty）；`sloop_store_dsu_store()` = ready/非冻结/非走带 → 存储锁内 `song_write_locked()`；`sloop_store_dsu_dirty()` 返回 `dsu_dirty`。`sloop_core_selftest` 收尾在 `song_reload` 后调 `dsu_reload` 与四槽 `sample_reload`。开机 `sloop_store_init` 在工程 fetch 后调 `dsu_reload`。

- [ ] **Step 4: 运行并提交**

Run: `scripts/synth-test.sh && scripts/seq-test.sh && scripts/build.sh && scripts/build.sh`；Expected 全绿、0 警告。Commit `feat(components/sloop_core): DSU 随 PER3 持久化并在自检后重载`。

---

### Task 6: 状态层 DSYN/FM6/采样命令与投影

**Files:**
- Modify: `firmware/components/sloop_state/include/sloop_state.h`
- Modify: `firmware/components/sloop_state/src/cmd.c`
- Modify: `firmware/components/sloop_state/src/state.c`
- Modify: `scripts/state-test.sh`
- Test: `firmware/components/sloop_state/test/state_test.c`

**Interfaces:**
- Consumes: 上述 core/store API。
- Produces: `CMD_DSYN_SET/LOAD_KIT/STORE/AUDITION`、`CMD_FM6_SET`、`CMD_SAMPLE_ERASE`；`app_state_t.dsyn/fm6p/synth_status`；`state_set_dsyn_select(kit,lane)`。

- [ ] **Step 1: 写失败 state 测试**

断言：`CMD_DSYN_SET` 越界 kit/lane/field 不调引擎且 `synth_status=INVALID`；合法写后 `dsyn.sound` 同拍回读；`CMD_DSYN_LOAD_KIT` 置 dirty；`CMD_DSYN_STORE` 走带中 → `BLOCKED`；`CMD_DSYN_AUDITION` 不改数据；`CMD_FM6_SET` 非 FM6 轨 → `LOCKED`，合法写后 `fm6p.patch` 回读；`CMD_SAMPLE_ERASE` 走带中 → `BLOCKED`、槽越界不调存储；`state_set_dsyn_select(5,20)` 被钳到 3/15 且 `STATE SNAP` 可读。

- [ ] **Step 2: 运行确认失败**

Run: `scripts/state-test.sh`；Expected 新用例失败。

- [ ] **Step 3: 实现命令与投影**

`edit_cmd_type_t` 追加枚举；`app_state_t` 追加三字段；`cmd.c` 分发并映射返回码（`ESP_ERR_NOT_ALLOWED`→BLOCKED、`INVALID_STATE`→FROZEN/ERROR、`INVALID_ARG`→INVALID/LOCKED）；成功同拍调用 `sloop_core_dsyn_read`/`sloop_core_fm6_patch`/`sloop_store_dsu_dirty`。`state_set_dsyn_select` 在状态锁内钳位并立即重取 `dsyn`。`state_sync_from_engine` 每拍读一次 `dsyn`（按当前选择）与 `fm6p`（选中合成轨）。`fmt`/label 不在此层。

- [ ] **Step 4: 运行并提交**

Run: `scripts/state-test.sh && scripts/build.sh && scripts/build.sh`；Expected PASS、0 警告。Commit `feat(components/sloop_state): 新增 DSYN/FM6/采样命令与投影`。

---

### Task 7: console `SMP`/`DSYN`/`FM6P` 与 PC 上传脚本

**Files:**
- Modify: `firmware/main/console.c`
- Create: `scripts/sample_upload.py`
- Create: `scripts/sample_fixture.py`（或并入 `sample_upload.py --sine`）
- Modify: `scripts/requirements.txt`（如需）
- Test: `python3 scripts/sample_upload.py --selftest`

**Interfaces:**
- Consumes: Task 4/5/6 公共 API 与状态命令。
- Produces: 诊断命令与可复用上传脚本；`--sine` 生成 1 zone/1,000 样本 fixture。

- [ ] **Step 1: 写脚本离线自测（先红）**

`sample_upload.py` 的 `build_sine_slot(name, n=1000, hz=440)` 用 vendored `sampleio.user_slot` 生成 header/data；`--sine NAME OUT` 写 `OUT.hdr`/`OUT.bin`；`--selftest` 校验 header magic/version/nz/data_len/CRC、data 长度、zones 合法，并打印 `SAMPLE SELFTEST OK`（失败非零）。先只写自测，运行确认因未实现而失败。

- [ ] **Step 2: 实现脚本上传**

实现 `info` / `load SLOT NAME file.wav[:root[:lo-hi]]…` / `erase SLOT`：经 pyserial 发 `SMP INFO` 解析；`SMP BEGIN k` → 每 120B `SMP DATA k off <hex>`（off 从 512 起）→ 4 行 `SMP HDR <hex>`（480B）→ `SMP END k`；逐行断言回显；任一步失败非零退出。复用 `screenshot.py` 的串口打开/复位重试风格。

- [ ] **Step 3: 实现 console 命令**

`console.c` 注册 `SMP INFO|BEGIN|DATA|HDR|END|ERASE`，解析与回显按 spec §4.5；错误令牌 `SMP ERR <no_storage|playing|frozen|size|header|crc|zones|flash|state>`。`DSYN INFO|GET|SET|KIT|STORE|AUDITION` 与 `FM6P INFO|GET|SET|ALG` 走状态层命令或只读快照；`STATE SNAP` 追加 `dsyn kit= lane= dirty=` 与 `fm6p slot= engine_ok=`。`UI DSYN …`/`UI FM6P …` 的选择器令牌**不在本任务**注册（依赖 Task 9/10 的页面静态量），本任务只保证上述命令可编译运行。

- [ ] **Step 4: 运行离线与构建**

Run: `python3 scripts/sample_upload.py --selftest && scripts/build.sh && scripts/build.sh`；Expected `SAMPLE SELFTEST OK`、0 警告。

- [ ] **Step 5: 提交**

Commit `feat(firmware/main): 新增 SMP/DSYN/FM6P 诊断面与采样上传脚本`。

---

### Task 8: SYNTH 组、SAMPLE 槽管理行与 14 页导航

**Files:**
- Modify: `firmware/components/sloop_ui/include/sloop_ui.h`
- Modify: `firmware/components/sloop_ui/src/ui.c`
- Modify: `firmware/components/sloop_ui/src/theme.h`
- Modify: `firmware/components/sloop_ui/src/page_params.c`
- Modify: `firmware/components/sloop_state/include/sloop_state.h`（`APP_STATE_PAGES 14`、新 page id）
- Modify: `firmware/main/console.c`（页名表、`UI` 语法）
- Test: `scripts/inject/nav.sh`（Task 11 设备跑；本任务先 host 构建）

**Interfaces:**
- Produces: `APP_PAGE_DSYN=12`、`APP_PAGE_FM6P=13`；7 组（PLAY/TRACKS/EDIT/SYNTH/SEQ/SONG/FILE）；`UI_GROUP_W=142`；SAMPLE 页槽管理行与 hit-test。

- [ ] **Step 1: 加页 id 与组表**

`app_page_t` 追加两 id，`APP_STATE_PAGES 14`，并把静态断言改为 `APP_PAGE_FM6P + 1 == APP_STATE_PAGES`；`SLOOP_UI_GROUPS 7`；`k_groups[]` 把 SAMPLE 移入 `{"SYNTH",3,{APP_PAGE_SAMPLE,APP_PAGE_DSYN,APP_PAGE_FM6P}}`，EDIT 改 4 页；`UI_GROUP_W 168→142` 并更新静态断言（右端 ≤1,064）。console 页名表加 `dsyn`/`fm6p`，`UI TAB synth` 可用。

- [ ] **Step 2: SAMPLE 槽管理行**

`page_params.c` 在 SAMPLE 页（`page_layout_get` 的 SAMPLE 索引）底部加 `U1..U4` 四个单对象 label + `ERASE` chip + 状态行（y=552..596，复用 BANK 行几何）。槽 label 只读 `sloop_store_sample_info`（文档化只读例外）；点槽发 `CMD_SET_PARAM`（id=`P_E0`=63，value=`8+k`）；ERASE 发 `CMD_SAMPLE_ERASE`（仅当前 SET∈8..11，否则状态 `NOT USR`）。`TAP` 新 kind `SLOOP_TAP_SAMPLE_SLOT/ACTION`，回显 `-> SAMPLE slot=k select` / `-> SAMPLE slot=k action=erase status="…"`。est 改 6,144。

- [ ] **Step 3: 页面注册与导航**

`ui.c` 注册两占位页（Task 9/10 建实体；本任务先建空页返回页容器并 `est` 已定）；`sloop_ui_page_order_next` 自动来自组表；`UI MEM` 组行出现 SYNTH。跑 `scripts/build.sh && scripts/build.sh`，Expected 0 警告；`scripts/state-test.sh` 仍全绿（页 id 静态断言更新）。

- [ ] **Step 4: 提交**

Commit `feat(components/sloop_ui): 新增 SYNTH 组与 SAMPLE 槽管理行`。

---

### Task 9: DSYN 编辑页

**Files:**
- Create: `firmware/components/sloop_ui/src/page_dsyn.c`
- Create/Modify: `firmware/components/sloop_ui/src/page_dsyn.h`（如需）
- Modify: `firmware/components/sloop_ui/src/ui.c`（注册实体页）
- Modify: `firmware/components/sloop_ui/src/theme.h`（DSYN tap kinds/状态文本）
- Modify: `firmware/main/console.c`（`UI DSYN` 选择器、`TAP` 回显、`STATE SNAP`）

**Interfaces:**
- Consumes: `app_state_t.dsyn`、`CMD_DSYN_*`、`state_set_dsyn_select`、`sloop_core_dsyn_desc/text`。
- Produces: `page_dsyn_build/update`；`SLOOP_TAP_DSYN_SEL`；`UI DSYN KIT/LANE/PAGE`。

- [ ] **Step 1: 写失败测试（先红）**

在 `synth-host-test.c`（不是 knob-test）加 DSYN 文本断言：`sloop_core_dsyn_text(DECAY,0/64/127)` 的 ms 单调且与 `DECAY_K` 公式一致；`FCUT=127` 输出 `CUTOFF_HZ[127]`；`LEVEL=128` → `0.0dB`、`255` → `+31.75dB`；`FLT_MODE` 7 个名字完整。运行 `scripts/synth-test.sh` 确认失败。

- [ ] **Step 2: 实现格式化并通过**

在 `sound_tab5.c` 完成 `sloop_core_dsyn_text`（Task 2 已给骨架，本任务补齐所有索引字段）；`scripts/synth-test.sh` 通过。UI 只调用该函数，不把上游表引入 `sloop_ui`。

- [ ] **Step 3: 建页面（8 旋钮 + 选择器）**

`page_dsyn.c`：页眉 `SYN1..4` 四 chip、`LANE` `−`/`+`+`Knn`、字段页 chip `SND1/SND2/SND3/KIT`、`AUDITION`、`STORE`、状态行；8 旋钮按 24 字段当前页建（KIT 页建 CRUSH/SRC 两槽）；值= `sloop_dsyn_field_from_sound(st->dsyn.sound, field)`；写=`CMD_DSYN_SET`；选 kit/lane=`state_set_dsyn_select`；AUDITION=`CMD_DSYN_AUDITION`；STORE=`CMD_DSYN_STORE`。锁定态不适用（DSYN 与选中轨无关）。状态行读 `synth_status` + `dirty`。

- [ ] **Step 4: 分页/回显/键盘**

`UI DSYN KIT/LANE/PAGE` 与页面 chip 共用静态量；`TAP` 回显 `-> DSYN kit=k lane=n page=p select` / `-> DSYN field=<name> value=<写后回读> status="…"` / `-> DSYN STORE status="…"`；键盘 ←/→ 焦点、↑/↓ 调值沿用参数页。跑 `scripts/build.sh && scripts/build.sh`（0 警告）与 `scripts/knob-test.sh`。

- [ ] **Step 5: 提交**

Commit `feat(components/sloop_ui): 新增 DSYN 鼓合成编辑页`。

---

### Task 10: FM6P 深度 patch 页

**Files:**
- Create: `firmware/components/sloop_ui/src/page_fm6p.c`
- Modify: `firmware/components/sloop_ui/src/ui.c`
- Modify: `firmware/components/sloop_ui/src/theme.h`
- Modify: `firmware/main/console.c`

**Interfaces:**
- Consumes: `app_state_t.fm6p`、`CMD_FM6_SET`、`sloop_core_fm6_desc`、既有 `CMD_BANK_PUT`。
- Produces: `page_fm6p_build/update`；`SLOOP_TAP_FM6P_SEL`；`UI FM6P OP/PAGE`。

- [ ] **Step 1: 页面骨架与锁定态**

`page_fm6p.c`：页眉 `OP1..OP6` chip 环、`PAGE EG/LEVEL/PITCH/PEG/VOICE/LFO`、`BANK −/B<n>/+` + `SAVE` + 状态行；8 旋钮由 `sloop_fm6_field_from_patch(st->fm6p.patch, op, field)` 驱动。`engine_ok==0` 时全部旋钮显示 `--`、chip/SAVE 拒绝、状态 `NOT FM6`；`slot` 显示当前 PTCH。

- [ ] **Step 2: 字段映射与动作**

六页映射：EG=R1..R4/L1..L4；LEVEL=BP/LD/RD/LC/RC/RS/KVS/OL；PITCH=MODE/FC/FF/DET/AMS/（空 3 槽）；PEG=PR1..PR4/PL1..PL4；VOICE=ALG/FB/OKS/TRNSP/（空 4 槽）；LFO=LFS/LFD/LPMD/LAMD/LKS/LFW/LPMS/（空 1 槽）。写=`CMD_FM6_SET`；ALG 经同一命令清 `P_E0`（Task 3 已实现）。`SAVE` 发 `CMD_BANK_PUT`（目标槽 UI 局部态 0..26）；状态行复用 `ui_file_status_text` + `synth_status`。`TAP` 回显 `-> FM6P op=n page=p select` / `-> FM6P <field> value=<回读> status="…"`。

- [ ] **Step 3: 构建与截屏准备**

跑 `scripts/build.sh && scripts/build.sh`（0 警告）；`UI fm6p` + `SCR` 手测页面可建（设备在 Task 12 跑）。`UI MEM` 记录 est 7,168。

- [ ] **Step 4: 提交**

Commit `feat(components/sloop_ui): 新增 FM6 155B 深度 patch 编辑页`。

---

### Task 11: 注入场景 `synth.sh` 与 nav/params/soak 更新

**Files:**
- Create: `scripts/inject/synth.sh`
- Modify: `scripts/inject/inject.py`（`run_synth`）
- Modify: `scripts/inject/nav.sh` / `nav` 段（7 组/14 页/TAB 顺序）
- Modify: `scripts/inject/params.sh`（EDIT Σ 16,896、SYNTH 三页）
- Modify: `scripts/soak.py`（页表/组往返）
- Modify: `docs/design/005-ui-architecture.md`（7 组容量）
- Modify: `docs/design/008-param-pages.md` / `009-storage-backend.md`

**Interfaces:**
- Consumes: 全部公共命令与三页。
- Produces: `scripts/inject/synth.sh` 可重复场景；更新后的导航/参数/soak 门禁。

- [ ] **Step 1: nav/params/soak 更新**

`nav` 的 TAB 期望改为 `PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→DSYN→FM6P→STEP→TRANSPORT→SONG→PROJECTS→PRESETS→PLAY`，组签 7 个、`UI TAB synth`、`UI SUB 0..2`、离开 SYNTH `built=0`。`params` 的 EDIT Σ 改 16,896，新增 SAMPLE/DSYN/FM6P 的 `delta ≤ est`、SAMPLE 槽行像素、DSYN/FM6P 锁定态。`soak` 页表加三页，跨组往返加 SYNTH。

- [ ] **Step 2: 写 `synth.sh` 场景**

归一（AUTO OFF/停走带/选轨 0/擦 USR1/DSYN 工厂 kit/选轨 FM6 的 bank 槽空）→ 采样上传（`sample_upload.py --sine`）→ `SMP INFO`/`PARAM 63 8`/`ENG TRIG` 使 voices>0 → BEGIN 中断/CRC 负例 → ERASE 静音 → DSYN 字段/试听/STORE/硬复位恢复/`ENG SELFTEST` 后恢复 → FM6P 编辑/ALG 清宏/锁定拒绝/`STORE BANK PUT`+`BANK GET` → 三页截屏与像素抽检 → `UI MEM` SYNTH Σ/delta + SYNTH→STEP 峰值 ≥4,096B → 收口。场景逻辑集中在 `inject.py` 的 `run_synth`，`synth.sh` 只做入口。

- [ ] **Step 3: 设备跑并落证据**

Run: `scripts/build.sh && scripts/flash.sh && scripts/inject/synth.sh`；Expected exit 0、截图生成、`UI MEM GROUP SYNTH sum_est=20,480`。失败截图保存 `-fail`。证据先落 `firmware/build/verify/`。

- [ ] **Step 4: 更新文档并提交**

ADR-005 §5 重算有序对（SEQ→PRESETS 60,776 最坏；SYNTH→STEP 59,240；EDIT→STEP 55,656）；ADR-008 §3 加三页表；ADR-009 §6 加 DSU 写回与采样原始槽。Commit `test(scripts): 新增 synth 注入矩阵并更新导航/参数/soak 门禁`。

---

### Task 12: 全门禁、证据晋升与阶段收口

**Files:**
- Create: `docs/design/011-sound-design.md`（ADR-011）
- Modify: `AGENTS.md` §3/§5/§8
- Modify: `docs/deferred-issues.md`（新增遗留，删除已解决项）
- Create: `docs/dev/verify/4-pd-2026-11-23/`（readme + SHA256SUMS + logs/png）
- Modify: `docs/dev/verification-log.md`

**Interfaces:**
- Consumes: 全部前置任务。
- Produces: 阶段证据包、ADR-011、dev 合并。

- [ ] **Step 1: 写 ADR-011**

记录：SYNTH 组与容量取舍、24 字段 UI 抽象与原始字节映射、DSU 随 settings 写回、采样 BEGIN 失效镜像、走带中上传拒绝的差异、FM6 ALG 宏清除、selftest 后 DSU/采样重载。

- [ ] **Step 2: 跑完整门禁**

顺序：`scripts/build.sh`（clean）→ `scripts/build.sh`（零警告）→ `scripts/host-test.sh` → `scripts/synth-test.sh` → `scripts/state-test.sh` → `scripts/seq-test.sh` → `scripts/knob-test.sh` → `python3 scripts/screenshot.py --selftest` → `sha256sum -c third_party/MANIFEST.sha256`（233/233）→ `scripts/flash.sh` → `scripts/device-selftest.py`（182/182）→ 九条注入场景同会话（含 `synth`）→ 复位后 `python3 scripts/soak.py --duration 1800 --interval 60 --save-window 900`。
Expected: 全绿；任何失败先修再重跑，不放宽断言。

- [ ] **Step 3: 晋升证据与登记**

把日志/截图/摘要复制到 `docs/dev/verify/4-pd-2026-11-23/`，写 `readme.md`（门禁→命令→结论）与 `SHA256SUMS`；`verification-log.md` 登记阶段/日期/提交号/证据 SHA256/结论。失败截图也入库（`-fail`）。

- [ ] **Step 4: 更新文档与 deferred**

AGENTS §3 目录（新页面/脚本/ADR）、§5.1 脚本表、§5.5 门禁行；roadmap P4-D 行与阶段 4 状态；parity spec P4-D ✅。新增 deferred：DSYN/FM6 patch 改名文本输入、采样 WAV UI 导入、DSYN-only 修改的延迟写、FM6 live patch 掉电语义（如实施中发现）；删除已解决条目。

- [ ] **Step 5: 整分支终审、合并与 push**

整分支 review clean 后：`git checkout dev && git merge --no-ff feat/phase4d`，重跑 `scripts/build.sh && scripts/build.sh && scripts/host-test.sh && scripts/state-test.sh`，`git push origin dev`。阶段报告写明机器已验证项与仍待人工的听感/触摸/目检项。

---

## Self-Review

- **Spec coverage:** §1 目标 1 → Task 4/7/8/11；目标 2 → Task 2/5/6/9；目标 3 → Task 3/6/10；目标 4 → Task 7/8/9/10/11；目标 5 → Task 1/11/12。§3 各取舍 → Task 2/4/5/8。§4 接口 → Task 2–10。§5 门禁 → Task 1/11/12。
- **类型一致性:** `sloop_core_dsyn_*`、`sloop_store_sample_*`、`CMD_DSYN_*`、`APP_PAGE_DSYN/FM6P`、`state_set_dsyn_select` 在各任务间同名同签名。
- **Review Focus:** 5 条均落到测试（Task 4/5/3/8/11/12）。
