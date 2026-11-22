#!/usr/bin/env python3
"""P4-B Task 3 证据抓取：工程 A–D 存读回显 + STORE STATS + ENG MEM（内部堆门禁）。

用法（仓库根目录）：
    python3 docs/dev/verify/4-pb-2026-11-21/task-3-store-capture.py
产物：同目录 task-3-store.log（串口原始回显 + `# [inject]` 工具注记）。

顺序说明：先硬复位拿**开机空闲态**的 ENG MEM（门禁 ≥61,440B 的口径，不带导航/截屏
留下来的 LVGL 池占用），随后做存储读/错误令牌面，最后跑一次单例 selftest 并再取
ENG MEM（存储 API 在诊断期 restore 之后仍可用 ⇒ 指针在 .noinit）。
"""
import sys
from pathlib import Path

sys.path.insert(0, 'scripts/inject')
import inject  # noqa: E402

out_dir = Path('docs/dev/verify/4-pb-2026-11-21')
log = out_dir / 'task-3-store.log'
if log.exists():
    log.unlink()

CMDS = [
    # 开机空闲态：内部堆门禁（本任务新增内部 RAM 仅 main_scratch 3,840B）
    'ENG MEM',
    'STORE STATS',        # ready=1 / objects=N
    'STORE PROJ LIST',    # 四槽状态（store.sh ⑦ 留下槽 0 USED）
    'STORE PROJ HASH 0',
    'STORE LIVEHASH',     # 复位后引擎是默认态，故与 HASH 0 不同（不是 bug）
    # 错误令牌面：越界槽 / 空槽
    'STORE PROJ SAVE 9',  # STORE ERR invalid
    'STORE PROJ ERASE 2', # PROJ 2 ERASED
    'STORE PROJ HASH 2',  # STORE ERR empty
    'STORE PROJ LOAD 2',  # STORE ERR empty
    'STATE SNAP',
]

# 诊断期 restore 之后存储仍可用（.noinit 指针未被回滚）——与 store.sh ⑧ 同口径；
# 随后在**音频流活着**时做一次 LOAD（R14 的「停走带 + panic 全部轨」前置要在这个
# 并发面上不出事：不挂死、返回 LOADED、引擎落到工程状态）
TAIL = [
    'ENG AUDIO OFF',
    'ENG SELFTEST preset/ANALOG/00_808_BOOM',
    'ENG AUDIO ON',
    'STORE PROJ HASH 0',
    'ENG TRIG synth 0 60',   # 起流 + 发声（流式时 LOAD 仍然是允许的：走带未启动）
    'STORE PROJ LOAD 0',     # R14 前置 + proj_apply
    'STORE LIVEHASH',
    'STATE SNAP',
    'ENG AUDIO OFF',
    'ENG MEM',
]

s = inject.Session('/dev/ttyACM0', log, out_dir, 'task3', 'store')
s.wait_ready()
s.reboot()
s.ser.note('capture: python3 docs/dev/verify/4-pb-2026-11-21/task-3-store-capture.py '
           '(Session.cmd of scripts/inject/inject.py)')
for cmd in CMDS:
    r = s.cmd(cmd)
    print('### ' + cmd)
    print(r.rstrip())
for cmd in TAIL:
    r = s.cmd(cmd, timeout=90)
    print('### ' + cmd)
    print(r.rstrip())
s.ser.close()
