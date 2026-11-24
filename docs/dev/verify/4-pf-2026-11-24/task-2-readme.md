# P4-F Task 2 证据（DI-26 堆泄漏定位修复 / `selftest-heap.py` / worker 栈收缩 DI-43(1)）

受测固件：`feat/phase4f` 的 `d22a656`（修复）+ `c7ee880`（脚本）+ `c4af6c6`（栈）。门禁在评审修复之前的工作树上跑（固件代码同 `c4af6c6`）；
评审修复只改了注释/文档并重排了未推送的提交（脚本与栈两个提交经 fixup 改写，哈希随之变化），固件代码语义不变，另跑一次 `scripts/build.sh` 0 警告。
构建 ×2 0 警告、烧录 3× `Hash of data verified`。

| 文件 | 内容 |
|---|---|
| `task-2-root-cause-callstacks.txt` | 根因：临时 heap-trace 诊断构建（未入库）抓到的 4 笔分配及其调用栈（`cpu1_host_init` → `pthread_cond_init`/`pthread_mutex_init`） |
| `task-2-selftest-heap-prefix-fail.log` | **修复前**（`a6e6898`）`selftest-heap.py` 的 transcript：run1 −40,284B，run2 设备自复位 ⇒ FAIL |
| `task-2-selftest-heap.log` | **修复后**门禁 transcript：boot 79,863；run1 −268B；run2 −36B；两次 182/182 ⇒ PASS |
| `task-2-device-selftest.log` | 同一 boot 的第 3 次全量自检：182/182 PASS |
| `task-2-inject-sys-stdout.log` | `inject/sys.sh`：348 PASS / 0 FAIL，chip 全路径 182/182，duty 12.42% / 9.79%，xrun 0 |
| `task-2-worker-hwm-16384.txt` / `-5632.txt` | worker 栈高水位：16KB 栈 hwm=12,236（峰值 4,148B）→ 5,632 栈 hwm=1,484；栈地址 0x4ff30a84（L2MEM） |
| `task-2-di55-quiesce-residual.txt` | 残余：`UI QUIESCE on/off` 每次 ~40B（DI-55，未定位） |
| `task-2-gates-summary.txt` | 全部门禁命令的退出码与日志尾部（build ×2 / flash / selftest-heap / 182 / sys / seq-test / state-test） |

说明：`inject` transcript 是命令响应的原始回显，worker 收尾的异步 `ESP_LOG` 行可能被下一条命令的 `reset_input_buffer` 丢掉
（`inject-sys-20261124-170210.log` 里没有 `stack hwm=`），chip 完成以「⑧ SYSTEM 状态行数据 SELFTEST 182/182 PASS」与 PNG 为证；
hwm 读数取自同一份 `sys_selftest.c` 的手工 chip 全路径运行（16:50）。
