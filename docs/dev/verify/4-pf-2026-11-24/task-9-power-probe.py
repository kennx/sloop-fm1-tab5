#!/usr/bin/env python3
"""Task 9 device probe: boot log, ENG POWER x3 while streaming, xrun window around 20 reads.

Evidence: transcript (raw echo) + summary. Usage: power_probe.py PORT OUT_PREFIX
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, "/home/kenn/projects/sloop-fm1-tab5/scripts/inject")
import inject  # noqa: E402

port, prefix = sys.argv[1], Path(sys.argv[2])
stamp = time.strftime("%H%M%S")
sess = inject.Session(port, Path(f"{prefix}-readings.log"), prefix.parent, stamp, "task9-power")
summary = []


def note(text):
    print(text)
    summary.append(text)


failures = 0


def check(cond, label):
    global failures
    if cond:
        print(f"  [PASS] {label}")
        summary.append(f"  [PASS] {label}")
    else:
        failures += 1
        print(f"  [FAIL] {label}")
        summary.append(f"  [FAIL] {label}")


LINE = re.compile(r"ENG POWER vbus_mv=(-?\d+|na) shunt_uv=(-?\d+|na) cur_ma=(-?\d+|na) "
                  r"temp_c=(-?\d+\.\d|na) ina=(ok|err)")


def perf_line(r):
    m = re.search(r"ENG PERF streaming=1[^\n]*", r)
    return m.group(0) if m else r.strip()


def inject_search(r):
    m = re.search(r"ENG AUDIO [^\n]*", r)
    return m.group(0) if m else r.strip()


def power(tag):
    r = sess.cmd("ENG POWER")
    m = LINE.search(r)
    check(m is not None, f"{tag}: 单行 ENG POWER 格式")
    note(f"{tag}: {m.group(0) if m else r.strip()!r}")
    return m


try:
    sess.reboot()
    boot = sess.boot_text
    note(f"boot: App version={sess.boot_version()!r}")
    bad = [w for w in ("Guru Meditation", "abort()", "Backtrace", "Rebooting...") if w in boot]
    check(not bad, f"boot: 无 fatal/abort/backtrace（{bad}）")
    time.sleep(5.0)
    audio = sess.cmd("ENG AUDIO")
    check(re.search(r"ENG AUDIO streaming=1", audio) is not None, f"开机即流式: {inject_search(audio)}")
    readings = []
    for i in range(3):
        m = power(f"ENG POWER #{i + 1}")
        if m:
            vbus = int(m.group(1)) if m.group(1) != "na" else None
            temp = float(m.group(4)) if m.group(4) != "na" else None
            check(m.group(5) == "ok", f"#{i + 1}: ina=ok")
            note(f"  [INFO] #{i + 1}: brief 预期 vbus 4750–5100mV / temp 35–70℃；实测 vbus_mv={vbus} temp_c={temp}"
                 "（记录项，不判 PASS/FAIL：VBUS 引脚按原理图接电池侧 SYS_BAT_PRE）")
            readings.append((vbus, m.group(2), m.group(3), temp))
        time.sleep(1.0)
    vb = [r[0] for r in readings if r[0] is not None]
    tp = [r[3] for r in readings if r[3] is not None]
    check(len(vb) == 3 and max(vb) - min(vb) <= 50, f"三次读数稳定: vbus_mv 极差 {max(vb) - min(vb) if vb else 'n/a'} mV（≤50）")
    check(len(tp) == 3 and max(tp) - min(tp) <= 5, f"三次读数稳定: temp_c 极差 {max(tp) - min(tp) if tp else 'n/a'} ℃（≤5）")
    for rnd in range(2):
        p0 = sess.cmd("ENG PERF")
        x0 = int(re.search(r"xrun=(\d+)", p0).group(1))
        t0 = time.monotonic()
        for _ in range(20):
            sess.cmd("ENG POWER")
        dt = time.monotonic() - t0
        while time.monotonic() - t0 < 3.0:
            time.sleep(0.05)
        p1 = sess.cmd("ENG PERF")
        x1 = int(re.search(r"xrun=(\d+)", p1).group(1))
        note(f"窗 {rnd + 1}: 20×ENG POWER 用时 {dt:.2f}s；窗口前 {perf_line(p0)}")
        note(f"窗 {rnd + 1}: 窗口后 {perf_line(p1)}")
        check(f"streaming=1" in p1 and x1 == x0, f"窗 {rnd + 1}: streaming=1 且 xrun 不变（{x0} → {x1}）")
    a2 = sess.cmd("ENG AUDIO")
    check("streaming=1" in a2, "收尾: 流仍在")
except Exception as e:  # noqa: BLE001
    failures += 1
    note(f"EXCEPTION: {e!r}")
finally:
    note(f"RESULT: {'PASS' if failures == 0 else 'FAIL'} failures={failures}")
    Path(f"{prefix}-summary.txt").write_text("\n".join(summary) + "\n", encoding="utf8")
    sess.ser.close()
sys.exit(0 if failures == 0 else 1)
