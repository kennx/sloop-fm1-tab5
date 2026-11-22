# P4-B Task 8 证据索引（设备侧 torn-write 故障注入套件）

日期：2026-11-22 ｜ 受测固件：首轮 `7c3f5d0-dirty`（工作区＝Task 8 源码）、修复轮
`d2adf1d-dirty`（工作区＝Task 8 修复源码；`task-8fix-boot-log.txt` 的 `App version`）
详细报告：`.superpowers/sdd/2026-11-21-p4b-storage-presets/task-8-report.md`

| 文件 | 内容 |
|---|---|
| `task-8-store-red.txt` / `task-8-store-red.log` | **RED**：新 `store` 场景（追加 ⑬ torn-write 段）在 Task 7 固件（`7c3f5d0`）上跑 ⇒ 前 427 项全绿；⑬a 的 HAL 故障探针全过（`FAULT 1` + `PROJ ERASE 3` → `STORE ERR save`、`STORE STATS faults=1 armed=0`、槽 3 仍 EMPTY、`FAULT CLEAR taken=1`）；⑬b 的 `STORE TEST` 回用法行（命令不存在）⇒ `store_expect` 抛错、exit=1。新断言先落地、且只缺被测功能 |
| `task-8-build1-clean.log` | `scripts/build.sh --clean`：rc=0、**编译警告 0**（全量重编，新函数 `store_test_*`/`sloop_store_test`/`store_test_cmd` 均无警告） |
| `task-8-build2.log` | 第二遍 `scripts/build.sh`：rc=0、**警告 0、0 kconfig 提示**；镜像 `0x17f9a0`（分区余量 81%） |
| `task-8-bin-sha.txt` | 受测（已烧录）镜像 `sloop_fm1_tab5.bin` SHA256 `a6aa0f21…`；烧录后只改文档/证据，重跑 `scripts/build.sh` 得到的镜像 SHA256 相同（受测镜像 = 提交源码） |
| `task-8-flash1.log` | `scripts/flash.sh /dev/ttyACM0`：三段 `Hash of data verified` |
| `task-8-boot-log.txt` | 启动日志：`App version: 7c3f5d0-dirty`、`sloop_store: ready … base=0x97000 span=0x68000`、`autosave 恢复：hash=5307834C`（上一轮 ⑪ 的工作工程）、`UI 就绪 9 页/1 建`、内部堆 105,419B；无 fatal/abort/backtrace/panic |
| `task-8-store-green{1,2}.txt` / `task-8-store-transcript{1,2}.log` | **GREEN ×2**：`bash scripts/inject/store.sh` 退出码 0、**439 PASS / 0 FAIL**（两轮断言行逐字节相同——`diff` 只剩 ⑪ 的实测秒数 11.6→10.9、19.1→19.7；`h0/h1/h2=D160C9B7/07F7D713/D160C9B7`、autosave hash `95215AE4/29A696DD/5307834C` 两轮相同）。⑬ 段：`FAULT 1` + `PROJ ERASE 3` 探针（`STORE ERR save`、`STORE STATS faults=1 armed=0`、`FAULT CLEAR taken=1`）→ `STORE TEST PASS 6/6` ×2（六条 `STORE TEST <name> ok` 日志：seq / erase-fail / half-write / header-fail / corrupt / empty-commit）→ `STORE STATS faults=0 armed=0`（套件清故障）→ 播放中 `STORE ERR playing` → 停止后 `PASS 6/6`；收口 `PROJ 3 EMPTY`（探针的失败擦除未落盘） |
| `task-8fix-store-green{1,2}.txt` / `task-8fix-store-transcript{1,2}.log` | **评审修复轮 GREEN ×2**（R26 + minors 1–6）：`store.sh` 退出码 0、**443 PASS / 0 FAIL**（439 + 4 条新诊断日志断言）。⑬b 断言并记录套件的内部诊断（每次运行 3 份、两轮共 6 份，逐字段相同）：`STORE TEST seq: v2 winner=copy1 seq=2 ok=1; **v3 winner=copy0 seq=3 ok=1**`（第三次提交让 copy 0 胜——不是「永远偏好 B」）与 `erase-fail: faults=1 armed=0 op=erase survivor=copy1` / `half-write: … op=prog-payload survivor=copy1` / `header-fail: … op=prog-header survivor=copy1`。两轮 `diff` 只剩 ⑪ 的实测秒数与起点 bpm（90/140，脚本自身归一前的状态） |
| `task-8fix-build{1,2}.log` / `task-8fix-bin-sha.txt` / `task-8fix-flash.log` / `task-8fix-boot-log.txt` | 修复轮构建 `--clean` + 二遍：rc=0、**警告 0、0 kconfig 提示**，镜像 `0x17fdd0`；受测镜像 SHA256 `a82ca9ba…`（烧录后只改文档/证据，重建相同）；三段 `Hash of data verified`；启动 `App version: d2adf1d-dirty`、store ready、autosave 恢复、UI 9 页/1 建、内部堆 105,419B，无 fatal/abort/panic |
| `task-8-selftest.log` | `scripts/device-selftest.py`：全量 **182/182**（`identical=182 mismatch=0 checks=7 deterministic=1 result=PASS`）；开头 `ENG AUDIO streaming=0 xrun=3` 是**自 ⓪ 起流以来累计**的 3 次渲染超预算（含 ⑪ 自动存档写、⑫ 银行写与 ⑬ 三次套件各 26 次扇区擦写），不是套件单独造成，ADR-009 §4 的设计内现象 |
| `task-8fix-selftest.log` | 修复轮 `device-selftest.py`：全量 **182/182**（同上；`xrun=3` 同样是累计值） |
| `task-8-params.txt` / `task-8-params.log` | `bash scripts/inject/params.sh`（**先硬复位**，DI-19）：**135 PASS / 0 FAIL**（五参数页既有断言不回退） |
| `task-8fix-params.txt` / `task-8fix-params.log` | 修复轮 `params.sh`（硬复位后）：**135 PASS / 0 FAIL** |
| `task-8-host-test.log` | `bash scripts/host-test.sh`：`ALL HOST TESTS PASSED (subset)`（未改 DSP/格式层） |
| `task-8-screenshot-selftest.log` | `python3 scripts/screenshot.py --selftest`：rc=0 |

## ⑬ 的断言设计（为什么不是自证）

1. **对象 = `OBJ_SETTINGS`**（P4-B 不写设置）：套件先 `st_load` 快照、结束 `st_save` 恢复，
   再 `sloop_hal_storage_fault_clear()`；`STORE TEST` 在 console 帮助/用法里标注**破坏性**。
2. **旧副本先在 B**：HAL 在 arm 期间连 `read` 也失败 ⇒ `st_save` 的 `st_current` 找不到有效
   副本、写目标恒为 copy A。故每个失败场景先擦净两个扇区，按 v1→A（seq 1）、v2→B（seq 2）
   提交，arm 前断言 `st_current==1 && seq==2` 且 v2 逐字节可读；失败后断言胜出副本仍是
   **copy 1 且逐字节 = v2**（`store_test_old_copy`）——不是 A 上的 v1、也不是新值 v3。
3. **故障命中的操作可判**（`faults_taken` 只数次数、分不出 erase/prog）：读回 A 扇区原始字节
   ——erase 失败 ⇒ A 头+payload 与 arm 前逐字节相同；payload 半写 ⇒ A 头为擦除态、payload 前
   32B = v3 其余 0xFF；头写失败 ⇒ payload 完整、头为擦除态（`st_head` 拒绝）。三个版本
   0x11/0x22/0x33… 逐字节不同（场景内显式 `memcmp` 断言），旧副本断言不会因新旧同值假绿。
4. 每个失败场景都断言 `rc != 0 && faults_taken == 1 && !armed`；任一前提不成立即 FAIL。
5. **已知限制（评审 R26）**：快照/恢复只覆盖对象 payload（`st_load` 的 `len` 字节），
   **不覆盖 settings 的 side record**（副本扇区偏移 64..255，P4-E 的主题/收藏计划落这里）
   ——场景的擦除会清掉它。P4-B 没有写入者（不可达）；P4-E 接线时必须扩为整扇区快照。
   已写进 `sloop_store.h` 契约、ADR-009 §3/§8、console help 与 AGENTS。

## 评审修复轮（R26 + minors 1–6）

- **R26（Important，限制文档化）**：`STORE TEST` 只快照/恢复 `OBJ_SETTINGS` 的 **payload**，
  不恢复 settings 的 **side record**（副本扇区 64..255，`ST_SIDE_OFF/ST_SIDE_MAX`，P4-E
  主题/收藏计划落这里）。P4-B 无写入者（不可达），**P4-E 必须扩为整扇区快照**；已写入
  `sloop_store.h` 契约、ADR-009 §3/§8、console help/用法 与 AGENTS 三处。
- **minor 1（copy 0 可胜）**：场景 ① 加第三次提交 v3→copy A（seq 3），断言
  `st_current()==0` 且字节 == v3；日志 `v3 winner=copy0 seq=3 ok=1`，脚本对这条日志做断言。
- **minor 2（可 triage 的日志）**：每个 arm 场景一行
  `STORE TEST <case>: faults=N armed=… op=<erase|prog-payload|prog-header> survivor=copyX`；
  脚本在 ⑬b 对 3 条分类行 + seq 的双向胜者行做断言（不是只在代码里）。
- **minor 3（标签）**：⑬d 改为「播放中的拒绝早于取锁/快照，套件仍可跑」——不再声称
  「未破坏设置对象」（套件只快照当时的内容）。
- **minor 4（DI）**：新增 `docs/deferred-issues.md` 的 **DI-21**（场景 ⑤ 的 1 字节 prog
  依赖未加密分区；加密构建下 `STORE TEST FAIL corrupt`，响亮失败）与 **DI-22**
  （`STORE FAULT`/`STORE RAWTEST` 不持 `s_store_lock`，autosave tick 可消费 arm；脚本先
  `STORE AUTO OFF`）。
- **minor 5（顺序）**：header 的句子改为与代码同序（结束先 `fault_clear`、再 `st_save` 恢复）。
- **minor 6（报告算术）**：套件每次 **26 次**扇区擦写（①5、②4、③5、④5、⑤3、⑥4），栈上
  局部最大 **≈430B**（场景 ② 的 3×64B payload + 2×96B 扇区指纹），`xrun=3` 是**累计**值。
