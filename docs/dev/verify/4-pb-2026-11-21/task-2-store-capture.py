#!/usr/bin/env python3
"""P4-B Task 2 证据抓取：STORE 后端状态 + RAWTEST + 故障注入计数语义（R11/R12）。

用法（仓库根目录）：
    python3 docs/dev/verify/4-pb-2026-11-21/task-2-store-capture.py
产物：同目录 task-2-store.log（串口原始回显 + `# [inject]` 工具注记）。
"""
import sys
from pathlib import Path

sys.path.insert(0, 'scripts/inject')
import inject  # noqa: E402

out_dir = Path('docs/dev/verify/4-pb-2026-11-21')
log = out_dir / 'task-2-store.log'
if log.exists():
    log.unlink()

CMDS = [
    # 后端状态与空闲空间量
    'STORE STATS',
    'ENG MEM',
    'STORE RAWTEST',            # 基线：PASS
    # R11/R12 覆盖：第 1 次写（erase）命中
    'STORE FAULT 1',
    'STORE RAWTEST',            # FAIL step=erase
    'STORE STATS',              # faults=1
    'STORE FAULT CLEAR',        # cleared taken=1
    'STORE RAWTEST',            # PASS（清除后恢复）
    # 第 2 次写（首个 prog）命中：erase #1 成功，prog #2 失败
    'STORE FAULT 2',
    'STORE RAWTEST',            # FAIL step=prog
    'STORE STATS',              # faults=1
    'STORE FAULT CLEAR',
    'STORE RAWTEST',            # PASS
    # half：prog 只写前 n/2 字节再失败
    'STORE FAULT 2 half',
    'STORE RAWTEST',            # FAIL step=prog
    'STORE STATS',              # faults=1
    'STORE FAULT CLEAR',
    'STORE RAWTEST',            # PASS
    # read 不消耗写序号：RAWTEST 共 18 次写（erase + 16 prog + cleanup erase），
    # arm 19 时写永不命中；armed 期间 verify 的 read 返回 -1（FAIL step=verify）但不取序号
    'STORE FAULT 19',
    'STORE RAWTEST',            # FAIL step=verify
    'STORE STATS',              # faults=0
    'STORE FAULT CLEAR',
    'STORE RAWTEST',            # PASS
    'STORE STATS',
    'ENG MEM',
]

s = inject.Session('/dev/ttyACM0', log, out_dir, 'task2', 'store')
s.wait_ready()
s.ser.note('capture: python3 docs/dev/verify/4-pb-2026-11-21/task-2-store-capture.py '
           '(Session.cmd of scripts/inject/inject.py)')
for cmd in CMDS:
    r = s.cmd(cmd)
    print('### ' + cmd)
    print(r.rstrip())
s.ser.close()
