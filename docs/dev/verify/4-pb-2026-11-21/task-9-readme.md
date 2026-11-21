# P4-B Task 9 证据索引（旧格式兼容门禁 FUN5/FUN4/FUN1 + RAW 上传通道）

日期：2026-11-22 ｜ 受测固件：`d856ec6-dirty`（HEAD=`d856ec6` + 本任务源码；`task-9-boot-log.txt`
的 `App version`）。本任务构建过多份镜像，源码相同：`b8e9f17a…`（首次烧录）→ `6b85c099…`
（`--clean` 重建）→ `986f1c33…`（注释补全后重建，烧录并跑 ⑭ 的 ×2）→ `3f29cca4…`（注释
去重后重建）。**它们不是同一镜像**——逐字节比较只有 **65 字节**不同，且全在
`esp_app_desc` 的 ELF SHA256（`[176,207]`）与镜像尾部 SHA256（`[1575343,1575375]`），即
代码/数据相同、build 元数据不同（`task-9-bin-sha.txt` 的区间证明）。评审修复轮的镜像
`8945047…` 是独立一次构建，烧录后重跑构建**逐字节相同**（`task-9fix-bin-sha.txt`）。
详细报告：`.superpowers/sdd/2026-11-21-p4b-storage-presets/task-9-report.md`

| 文件 | 内容 |
|---|---|
| `task-9-store-red.txt` / `task-9-store-red-transcript.log` | **RED**：新 `store` 场景（追加 ⑭ 旧格式段）在 Task 8 固件（`d856ec6`，无 `STORE RAW`/`FIELDS`）上跑 ⇒ 前 445 项全绿；⑭a 第一条断言 `STORE RAW BEGIN 4097` 期望 `STORE ERR size`，旧固件回用法行 ⇒ `store_expect` 抛错、**exit=1**。新断言先落地、且只缺被测功能 |
| `task-9-build1-clean.log` | `scripts/build.sh --clean`：rc=0、编译警告 0；仅出现已知豁免的 kconfig 首 pass CMake 提示对 |
| `task-9-build2.log` | 第二遍 `scripts/build.sh`：rc=0、**警告 0、0 kconfig 提示**；镜像 `0x1809e0`（分区余量 81%） |
| `task-9-build3-after-comment.log` | 把 `sloop_store.h` 的**注释**补全（暂存与 `proj_tmp` 共用、调用方须串行）后增量构建：rc=0、警告 0 |
| `task-9-bin-sha.txt` | 四份镜像的 SHA256 + 两次逐字节比较（clean 重建 vs 注释后重建、注释后重建 vs 最终构建）：**均只有 65 字节不同，区间 `[176,207]`（`esp_app_desc` 的 ELF SHA256）与 `[1575343,1575375]`（镜像尾部 SHA256）**——代码/数据相同（ESP-IDF 的 build 元数据非确定性），**不是同一镜像** |
| `task-9-flash1.log` / `task-9-flash2.log` | `scripts/flash.sh /dev/ttyACM0`：各三段 `Hash of data verified` |
| `task-9-boot-log.txt` | 启动日志：`App version: d856ec6-dirty`、`sloop_store: ready … base=0x97000 span=0x68000`（slots 15,264B / stage / v3 2,584B / auto 3,816B 均在 PSRAM）、`autosave 恢复：hash=F45A1907 sel=0 bpm=140`、`UI 就绪 9 页/1 建`、内部堆 **105,403B**（较 Task 8 的 105,419B 少 16B：新静态只有 3 个 `.noinit` 计数器），无 fatal/abort/backtrace/panic |
| `task-9-store-green1.txt` / `task-9-store-transcript1.log` | **GREEN #1**（首次烧录镜像 `b8e9f17a…`）：`store.sh` exit=0、**669 PASS / 0 FAIL**；⑭ 段 226 条（⑭a 语法 14、fun5/fun4/fun1 各 66（上传+LOAD+LIST+键顺序+62 字段）、⑭c 7、⑭d 3） |
| `task-9-store-green2.txt` / `task-9-store-transcript2.log` | **GREEN #2**（同一镜像，场景自归一）：exit=0、**668 PASS / 0 FAIL**（差 1 条 = ⑪ 的 `TAP 步 (0,5) 建音` 在第二轮是幂等无操作，不断言「建了」）；两轮 `diff` 只剩 ⑪ 的实测秒数/起点 bpm/autosave hash（场景自身归一前的状态）与 fixtures 时间戳目录 |
| `task-9-store-green3.txt` / `task-9-store-transcript3.log` | **GREEN #3**（clean 重建镜像 `6b85c099…`）：exit=0、668 PASS / 0 FAIL |
| `task-9-store-green4.txt` / `task-9-store-transcript4.log` | **GREEN #4**（同上）：exit=0、668 PASS / 0 FAIL；与 #3 的 `diff` 只有 ⑪f 的 1.3s/0.1s 与重启耗时 4.2s/4.1s |
| `task-9-store-green5.txt` / `task-9-store-transcript5.log` | **GREEN #5**（最终受测镜像 `986f1c33…`）：exit=0、668 PASS / 0 FAIL |
| `task-9-selftest.log` | `scripts/device-selftest.py`：全量 **182/182**（`identical=182 mismatch=0 checks=7 deterministic=1 result=PASS`；设备上留有 autosave 对象 → 自检基线仍是 resume 之前的工厂态快照） |
| `task-9-params.txt` / `task-9-params.log` | `bash scripts/inject/params.sh`（**先硬复位**，DI-19）：**135 PASS / 0 FAIL**（在首次烧录镜像 `b8e9f17a…` 上；与最终镜像代码相同，差异仅 build 元数据） |
| `task-9-host-test.log` | `bash scripts/host-test.sh`：`ALL HOST TESTS PASSED (subset)`；新增一行 `gen_project_fixtures.py --host-check` ⇒ **`[host-check] fun5/fun4/fun1: 62 项与上游 project.c 一致`** |
| `task-9-screenshot-selftest.log` | `python3 scripts/screenshot.py --selftest`：rc=0 |

## ⑭ 的断言设计（为什么不是自证）

1. **期望值独立于固件**：`scripts/gen_project_fixtures.py` 按上游 `project.c` 的结构体
   （`_Static_assert` 钉死 3840/3112/688）与 `tests/project_test.c` 的构造器生成字节，再用
   Python **独立实现**的上游转换（`proj_from_v5` / `proj_from_v4` /
   `proj_from_v1`→`proj_trk_v2_to_v3`→`proj_trk_from_v3`）**从生成的字节**算出 62 个字段。
   FIELDS 若错（字段、偏移、转换、打印顺序），比对必红——不是拿固件的输出当期望。
2. **主机交叉校验**：`--host-check` 编 `scripts/fixture_host_check.c`（include 同一份
   vendored `project.c` 的 `PROJ_HOST` 段，与 `tests/project_test.c` 同一编译纪律）导入
   三个 fixture 并按同一投影打印，与 `expected.json` 逐项比对（62/62 ×3）。
3. **键顺序也是断言**：FIELDS 是固定顺序的一行；脚本先断言 `list(keys) == field_keys()`，
   再逐项比值（顺序漂移也算失败）。
4. **LOOP 真的执行**：每个 fixture 都先 `STORE PROJ LOAD 3`（`proj_import` + `proj_apply`，
   回 `PROJ 3 LOADED hash=…`），再 `FIELDS 3`（只读 flash 的另一条路径）——两条路径互不依赖。
5. **截断必须被拒且不动引擎**：`fun5[:100]` 的头部/CRC 合法（`st_load` 成功返回 100B），
   但 `proj_import` 拒绝 ⇒ `LOAD 3` 回 **`STORE ERR invalid`**（与空槽的 `empty` 分开）、
   `LIVEHASH` 与上传前逐字节相同（LOAD 在导入失败处返回，从未 `proj_apply`）、
   `FIELDS 3` 同样 `invalid`。收口 `ERASE 3`，槽 0–2 的 `PROJ LIST` 行逐字节不变。
6. **grammar/错误令牌**：`BEGIN 4097` → `size`；追加超过声明长度 → `size`（状态保持、可
   ABORT）；未 BEGIN 的 END → `invalid`；只收到 2/4 字节的 END → `invalid` 且不写 flash；
   `ABORT` → `STORE RAW ABORTED`。

## 设备实测的传输上限（与 spec 字面不同，已在 spec/ADR/AGENTS 就地更正）

spec §4.4 写「`STORE RAW <hex>`（≤480 hex 字符）」。实测 **490 字符的行会被截断到 256
字符**（echo 停在 256）；root cause 是 **USB-Serial-JTAG 驱动 RX 环 256B**
（`USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT().rx_buffer_size`），整行要在一次突发里装进环里，
设备忙（LVGL/音频）时环溢出会**静默丢字符**。块上限改为 **240 hex = 120B**
（`STORE RAW ` + 240 hex + `\n` = 251B < 256B，与调度时序无关）；120B/块下 fun5 3840B = 32 块，
5 次 GREEN ×2/×3 无一失帧。`main/console.c` 的 `STORE_RAW_HEX_MAX` 与
`scripts/store_raw.py` 的 `CHUNK` 同源。

## 评审修复轮（R27 + R28，2026-11-22）

受测固件：`b2493b0-dirty`（HEAD=`b2493b0` + 修复源码；`task-9fix-boot-log.txt` 的
`App version`）。镜像 `8945047…`：烧录后只改测试脚本/文档，重跑 `scripts/build.sh` 得到
**同一 SHA256（0 字节差异）**（`task-9fix-bin-sha.txt`）。

| 文件 | 内容 |
|---|---|
| `task-9fix-build1-clean.log` / `task-9fix-build2.log` / `task-9fix-build3-rebuild.log` | `--clean`（2,098 目标）rc=0、**警告 0**（仅已知豁免的 kconfig 首 pass CMake 提示对）；第二遍 rc=0、**警告 0、0 kconfig 提示**，镜像 `0x180a00`；烧录后重跑构建：同一 SHA256 |
| `task-9fix-bin-sha.txt` | 受测镜像 `8945047…` 与修复轮重建**逐字节相同**；另保留首轮四份镜像的 65 字节 desc-SHA 差异说明 |
| `task-9fix-flash.log` | `scripts/flash.sh /dev/ttyACM0`：三段 `Hash of data verified` |
| `task-9fix-boot-log.txt` | `App version: b2493b0-dirty`、store ready、`autosave 恢复：hash=F45A1907`、UI 9 页/1 建、内部堆 **105,403B**，无 fatal/abort/panic |
| `task-9fix-store-green{1,2}.txt` / `task-9fix-store-transcript{1,2}.log` | **GREEN ×2**：`store.sh` exit=0、**691 PASS / 0 FAIL**（两轮的 PASS/FAIL 行 `diff` 为空），含新断言 ⑭a `BEGIN 3841 ⇒ size` / `BEGIN 3840` 接受、⑭a2 R27（上传中途 `STORE PROJ LIST` ⇒ 上传被丢弃：追加/END 回 `invalid`、槽 3 仍 EMPTY、重新 BEGIN 后 END 成功）、⑭a3 R28（播放中 END ⇒ `STORE ERR playing`、未落盘、ABORT 收口） |
| `task-9fix-selftest.log` | `scripts/device-selftest.py`：全量 **182/182** |
| `task-9fix-host-test.log` | `host-test.sh`：`ALL HOST TESTS PASSED (subset)`；`[host-check] fun5/fun4/fun1: 62 项与上游 project.c 一致` |

### 修复内容（评审 R27/R28）

- **R27（暂存被穿插命令覆盖）**：`STORE RAW` 的暂存就是 `proj_tmp`/`up_stage` 那块共用
  4,096B。**每条其它 `STORE` 子命令在 `store_command` 入口先 `sloop_store_raw_abort()`**；
  `sloop_store_init` 也复位 RAW 状态（`.noinit` 冷启动垃圾）；`sloop_store_raw_append`
  增加 `got > len` 下溢守卫（先判再相减，避免 `len-got` 溢出后 memcpy 越界）。
- **R28（失败令牌与契约）**：`SLOOP_STORE_RAW_MAX` 改为 **3,840 = `ST_PAYLOAD_MAX`**
  （编译期 `_Static_assert` 锁死）——`BEGIN 3841` 当场回 `size`，不会到 `END` 才回 `save`；
  `sloop_store_project_fields` 改为「先校验 slot/ready、成功才写 `*out`」（局部 `f` +
  末尾拷贝，失败路径完全不碰调用方缓冲）；`store_err` 注释补 `invalid` = 槽/对象名越界
  **或** 未知格式、`size` = RAW 长度/块越界；设备断言补播放中 `END` 拒绝；
  `store_raw.py` 的头部注释从 480 hex/240B 更正为 240 hex/120B 与 3,840B 上限。
