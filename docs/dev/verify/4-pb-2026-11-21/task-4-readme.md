# P4-B Task 4 证据索引（用户预置 32 槽）

日期：2026-11-21 ｜ 受测固件：`959f73c-dirty`（工作区＝Task 4 源码，任务提交前的现场）
详细报告：`.superpowers/sdd/2026-11-21-p4b-storage-presets/task-4-report.md`

| 文件 | 内容 |
|---|---|
| `task-4-store-red-run.txt` | **RED**：同一 `store.sh` 在 Task 3 固件上跑（前 48 项全绿后 `STORE PRESET ERASE 0` 回用法行，exit=1） |
| `task-4-store-red.log` | RED 的串口原始回显 |
| `task-4-store-green1.txt` / `green2.txt` | **GREEN ×2**：`bash scripts/inject/store.sh` 退出码 0、**183 PASS / 0 FAIL**。场景开头先 esptool 硬复位到工厂态（R15 归一），故两次运行的 h0/h2 相同（`CCB3B3D7`）、断言与数值全同；`-run.txt` 只差两行：日志路径（时间戳）与 reboot 的主机侧计时（见 `task-4-store-green-diff.txt`） |
| `task-4-store-green-diff.txt` | 两次 GREEN 的 `-run.txt` 规范化 diff（去掉日志路径行后仅 1 行：`console 就绪 4.2s/4.1s`）+ 两次的 h0/h1/h2 |
| `task-4-store-transcript{1,2}.log` | GREEN ×2 的串口原始回显；差异仅 boot 日志/主机命令的插行顺序与时间戳（固件断言行逐字节相同） |
| `task-4-saveafter-red-run.txt` / `.log` | **RED（自审发现的数据丢失缺陷）**：selftest 之后紧跟 `SAVE 1`（未先读预置）⇒ `up_put` 从 NULL 的 XIP 副本起笔，把同银行的槽 0 抹掉；修复后同一脚本 GREEN（⑨f 读路径 / ⑨f' 写路径各用一次 selftest 分开证明） |
| `task-4-build1.log` / `task-4-build2.log` | 构建 ×2：首遍 rc=0 但**含 §5.5 豁免的 kconfig 提示对**（`build.cmake:631` + `Missing kconfig option`，CMake 重新 configure 时出现）；第二遍 rc=0、警告 0、0 提示——门禁按第二遍判定 |
| `task-4-flash.log` | `scripts/flash.sh`：esptool 三段 `Hash of data verified` |
| `task-4-boot.log` | 复位后的启动日志（`sloop_store: ready`、无 fatal/abort/backtrace/task_wdt）+ `ENG MEM` + `STORE STATS` + `STORE PRESET LIST` |
| `task-4-memory.txt` | 段归属：`flash_ok` 在 `.noinit`（窗口外）、vendored `up_bank_xip` 在 `.bss`（SELFTEST 快照窗口内 ⇒ 预置入口重扫的原因） |
| `task-4-selftest.log` | `scripts/device-selftest.py`：全量 **182/182** `identical=182 mismatch=0` |
| `task-4-nav.log` | `bash scripts/inject/nav.sh`：40 PASS / 0 FAIL |
| `task-4-host-test.log` | `bash scripts/host-test.sh`：`ALL HOST TESTS PASSED (subset)`（未改 DSP/格式层） |

`third_party/sloop-fm1/**` 零修改：`sha256sum -c third_party/MANIFEST.sha256` → 233/233 OK。
