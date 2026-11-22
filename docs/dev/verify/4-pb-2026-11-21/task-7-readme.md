# P4-B Task 7 证据索引（FM6 银行 27 槽）

日期：2026-11-21 ｜ 受测固件：`78204ec-dirty`（工作区＝Task 7 源码；`App version: 78204ec-dirty`）
详细报告：`.superpowers/sdd/2026-11-21-p4b-storage-presets/task-7-report.md`

| 文件 | 内容 |
|---|---|
| `task-7-store-red-run.txt` / `task-7-store-red.log` | **RED**：新 `store` 场景（追加 ⑫ 银行段）在 Task 6 固件上跑 ⇒ 前 341 项全绿、⑫ 的第一个 `STORE BANK LIST` 回用法行 ⇒ ⑫ 的两条断言与随后的 `STORE BANK GET 3` 失败、exit=1（新断言先落地） |
| `task-7-store-green-attempt1-offsetbug.txt` / `task-7-store-transcript-attempt1-offsetbug.log` | **GREEN 首次尝试（3 项失败）——门禁抓到的真缺陷**：`PUT 3` 回 `SAVED`，但 `GET 3` 读出 **128 个 0x00**；`PUT 4` 之后 `GET 3` 回 `STORE ERR empty`（整银行失效）。根因：Task 3 写下的 `FM6_BANK_XIP(copy)` 少了 `ST_PAYLOAD_OFF = 256`（payload 从扇区偏移 256 起；上游 `fm6_bank.c` 的宿主兜底是 `st_sector(...) + ST_PAYLOAD_OFF`）。错位视图：槽 k 读到槽 k−2、写路径把提交头/留空区的 0xFF 带进 payload ⇒ 校验失败读成空。修法：`ST_XIP_PTR(0xE5000 + copy×4096 + 256)`（`store_tab5.c`、ADR-009 §2） |
| `task-7-store-green{1,2}.txt` / `task-7-store-transcript{1,2}.log` | **GREEN ×2**：`bash scripts/inject/store.sh` 退出码 0、**417 PASS / 0 FAIL**；两轮的断言行逐字节相同（只差日志路径、主机侧计时与 ⑪ 的实测秒数），`h0/h1/h2=D160C9B7/07F7D713/D160C9B7`、⑫ 的 L/L2 记录也逐字节相同。⑫ 段：`BANK LIVE`=L（工厂 patch F1）→ `PUT 3` → `GET 3==L` → 预置 1（PTCH=1）⇒ L2≠L → `PUT 4` ⇒ `GET 3` 仍 L、`GET 4==L2` → **selftest 后紧跟 `PUT 5`（无银行读）⇒ B3/B4/B5 均 USED** → `ERASE 5` → 硬复位 ⇒ B3/B4 仍 USED 且字节不变 → `PARAM 70 11` ⇒ `LIVE` 轮询到槽 3 的 L + `ENG TRIG` 出声（**该序列在修复轮被 R24 强化**：此轮归一到 PTCH=0 时 LIVE 与槽 3 的字节相同，重载死掉也会绿——见下面的修复轮小节） → `ERASE 3/4` ⇒ COUNT 0 → 越界 invalid / 播放中 playing / 鼓轨 drum |
| `task-7-mutation-probe.txt` / `task-7-mutation-probe.log` | **变异探针（ADR-009 §8 裁决的判别力）**：把 `sloop_store_bank_put` 入口的 `fm6_bank_boot()` 删掉后重烧，同一时序（selftest → 紧跟 `PUT 5`，无银行读）⇒ `after_selftest_put5=[5]`（B3/B4 丢失）；提交固件为 `[3,4,5]`。证明「自检 restore 真的清空了 `fm6_bank_cur/used`」且 ⑫d 不是自证 |
| `task-7-memory.txt` | 段归属：`fm6_bank_cur`（有 `= -1` 初始化 ⇒ `.dram0.data`）在 0x4ff1abf8 ∈ `.data` 快照窗口 [0x4ff1a468, 0x4ff1aeb0)；`fm6_bank_used`/`fm6_bank_read`/`fm6_slot`/`fm6_patch` 在 `.dram1.bss`（0x4ff51…/0x4ff99b…）∈ `_sloop_selftest_bss` 窗口 [0x4ff50bf4, 0x4ff9c654) ⇒ 自检 restore 会清空它们；`flash_ok` 在 `.noinit`（0x4ff1b168，窗口之外） |
| `task-7-build{1,3,4}.log` | `build1`＝偏移修复前的源码（首遍含 §5.5 豁免的 kconfig 提示对）；`build3`（偏移修复后）/`build4`（仅注释改动后）＝**最终源码的构建 ×2**：rc=0、**编译警告 0**、0 提示，镜像 `0x17edc0`（分区余量 81%） |
| `task-7-bin-sha.txt` | 最终源码重建的镜像与**受测（已烧录）镜像** SHA256 相同（`5633315c…`）：注释改动不改可装载段，GREEN 证据与提交源码同源 |
| `task-7-flash{1,2,3}.log` | `scripts/flash.sh`：三段 `Hash of data verified`（`flash1`＝偏移修复前、`flash2`＝最终固件、`flash3`＝变异探针之后重烧最终固件） |
| `task-7-boot-log.txt` | 启动日志：`App version: 78204ec-dirty`、`sloop_store: ready … base=0x97000 span=0x68000`、`autosave 恢复`、`UI 就绪 9 页/1 建`、内部堆 105,419B；无 fatal/abort/backtrace/panic |
| `task-7-selftest.log` | `scripts/device-selftest.py`：全量 **182/182**（`identical=182 mismatch=0 checks=7 deterministic=1 result=PASS`） |
| `task-7-params.txt` / `task-7-params.log` | `bash scripts/inject/params.sh`（硬复位后单跑，DI-19）：**134 PASS / 0 FAIL**（五参数页既有断言不回退） |
| `task-7-host-test.log` | `bash scripts/host-test.sh`：`ALL HOST TESTS PASSED (subset)`（未改 DSP/格式层） |
| `task-7-screenshot-selftest.log` | `python3 scripts/screenshot.py --selftest`：rc=0（工具路径未改，回归确认） |

## 评审修复轮（R24/R25 + minors 1–6）追加证据

日期：2026-11-22 ｜ 受测固件：`d2f35f4-dirty`（构建时 HEAD＝`d2f35f4`（修复轮的父提交/amend 前），
工作区＝修复源码；`App version: d2f35f4-dirty`；修复以 amend 并入本任务提交，哈希以 `git log` 为准）
｜ 构建后只改文档与 `scripts/inject/**`

| 文件 | 内容 |
|---|---|
| `task-7fix-build1.log` / `task-7fix-build2.log` | 修复源码构建 ×2：`build1` 首遍含 §5.5 豁免的 kconfig 提示对；`build2` **0 警告、0 提示**（镜像 `0x17edb0`，81% 余量） |
| `task-7fix-bin-sha.txt` | 受测（已烧录）镜像 `99cfa588…` |
| `task-7fix-flash.log` | `scripts/flash.sh`：三段 `Hash of data verified` |
| `task-7fix-boot-log.txt` | 启动：`App version: d2f35f4-dirty`、`sloop_store: ready …`、`autosave 恢复：hash=8284B0F3 sel=1 bpm=90`、`UI 就绪 9 页/1 建`、内部堆 105,419B；无 fatal/abort/backtrace/panic |
| `task-7fix-store-green{1,2}.txt` / `task-7fix-store-transcript{1,2}.log` | **GREEN ×2（修复轮）**：`store.sh` exit=0、**run1 421 / run2 420 PASS（0 FAIL）**（差 1 项是 `ensure_step_note` 的条件 TAP：起点有音时不打印）；`h0/h1/h2=D160C9B7/07F7D713/D160C9B7` 两轮相同。**R24 的新判别序列**（两轮逐字节相同）：`bank_normalize_fm6(s, 1)`（PTCH=1 ⇒ 工厂 **F2**）⇒ `LIVE` == ⑫c 的 L2 = `633C282D63320000…` 且 ≠ 槽 3 的 F1 = `5F281E3C63500000…` → `PARAM 70 11` ⇒ `LIVE` 轮询成 `5F281E3C63500000…`（**F2 → 槽 3 的真切换**；旧序列归一到 F1 时 LIVE 恒等于槽 3 的字节，PTCH→银行的重载死掉也会绿） |
| `task-7fix-selftest.log` | 全量 **182/182**（`identical=182 mismatch=0 checks=7 deterministic=1 result=PASS`） |
| `task-7fix-params.txt` / `task-7fix-params.log` | `params.sh`（硬复位后单跑）：**135 PASS / 0 FAIL**（含新增的 `归一 GLOBAL BPM=90`；五页「环顶」探针全绿） |
| `task-7fix-params-di19-noreboot.{txt,log}` | **反例（过程证据）**：修复轮第一次跑 params 时没重启设备（`store.sh` → `ENG SELFTEST` → `params.sh` 同一会话）⇒ `UI fx` 无响应，日志见 `Guru Meditation Error: Core 0 panic'ed (Load access fault)` + `MTVAL : 0x00000076` ⇒ `rst:0xc`——DI-19 的池悬崖 panic（与 DI-19 记录的签名一致），不是本任务回归；按 DI-19 的「脚本之间重启设备」重跑即绿（上一行） |

### 修复轮顺带修正（说明，不改变产品行为）

- `params.sh` 的 GLOBAL 页截图探针（「旋钮 0 弧线底（环顶）」= 环顶必须是面板底色）按**工厂
  BPM 的弧长**设计：`STORE` 场景最后一次自动存档是 `bpm=140`（⑪f），复位后 `autosave_resume`
  把它带进新会话，140 的弧线会盖住 12 点方向的环顶 ⇒ 同一条探针实测 **90/137 绿、140 红**。
  修法：`run_params` 开头把 BPM 归一到 TP 默认值 90（`PARAM_BPM_DEF`，与 `ENG_LEVEL_DEF`
  同一口径），场景不再随上一场景的状态时红时绿。
