"""Task 8 接管者的一次性诊断探针（不入库、不改固件）。

A. chip 探针：esptool 硬复位后第一次点 SELFTEST chip——看是 RUNNING 还是 RESET FIRST
   （.noinit 每-boot 标记是否跨 esptool 硬复位存活）。RUNNING 则等到结果并核对
   AUTO / LIVEHASH / 流恢复。
B. 自检后音频重启探针：对照（OFF→ON 不自检）与 `ENG SELFTEST` 后 ON 的响应延迟、
   xrun、`ENG PERF` / `ENG SNAP voices` 时间序列。
"""
import re
import sys
import time
from pathlib import Path

ROOT = Path("<repo>")
sys.path.insert(0, str(ROOT / "scripts" / "inject"))
import inject  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
OUT = ROOT / "firmware" / "build" / "verify"
stamp = time.strftime("%Y%m%d-%H%M%S")
s = inject.Session(PORT, OUT / f"probe-postselftest-{stamp}.log", OUT, stamp, "probe",
                   timeout=60.0, retries=0)
s.wait_ready()


def audio_on_timed(label):
    t0 = time.monotonic()
    s.cmd("ENG AUDIO ON", timeout=60.0)
    dt = time.monotonic() - t0
    print(f"[{label}] ENG AUDIO ON 响应 {dt:.2f}s: {s.last.strip().splitlines()[-1]}")


print("== A. chip 探针")
s.reboot()
s.cmd("STATE SNAP")
print("playing =", s.snap_int(r"playing=(\d)"), "selftest_done =", s.snap_int(r"selftest_done=(\d)"))
s.cmd("UI system")
s.cmd("TAP 94 604", timeout=10.0)
line = [l for l in s.last.splitlines() if "SYS SELFTEST" in l]
print("首按 chip:", line)
chip_running = bool(line) and "RUNNING" in line[0]
if chip_running:
    t0 = time.monotonic()
    done = False
    while time.monotonic() - t0 < 200:
        s.cmd("STATE SNAP")
        if s.snap_int(r"selftest_done=(\d)") == 1:
            done = True
            break
        time.sleep(1.0)
    print(f"chip worker 完成={done} 耗时 {time.monotonic() - t0:.1f}s ran={s.snap_int(r'selftest_ran=(\d+)')} "
          f"failed={s.snap_int(r'selftest_failed=(\d+)')}")
    s.cmd("STORE STATS")
    print("STATS:", [l for l in s.last.splitlines() if "autosave=" in l])
    s.cmd("ENG AUDIO")
    print("AUDIO:", s.last.strip().splitlines()[-1])
    s.cmd("STORE LIVEHASH")
    print("LIVEHASH:", s.last.strip().splitlines()[-1])

print("== B. 自检后音频重启探针")
s.cmd("STORE AUTO OFF")
s.cmd("ENG AUDIO OFF")
print("对照 OFF:", s.last.strip().splitlines()[-1])
audio_on_timed("对照：无自检")
s.cmd("ENG PERF")
print("对照 PERF:", s.last.strip().splitlines()[-1][:200])
s.cmd("ENG AUDIO OFF")
t0 = time.monotonic()
s.cmd("ENG SELFTEST", timeout=400.0)
print(f"ENG SELFTEST 墙钟 {time.monotonic() - t0:.1f}s:",
      [l for l in s.last.splitlines() if "result=" in l or "FAIL" in l][:2])
audio_on_timed("自检后")
t_on = time.monotonic()
for i in range(14):
    s.cmd("ENG PERF")
    perf = [l for l in s.last.splitlines() if l.startswith("ENG PERF streaming")]
    s.cmd("ENG SNAP")
    voices = re.findall(r"trk k=(\d).*?voices=(\d+)", s.last)
    xrun = re.search(r"xrun=(\d+)", perf[0]).group(1) if perf else "?"
    duty = re.search(r"duty_mean=([0-9.]+)%", perf[0]).group(1) if perf else "?"
    print(f"  t+{time.monotonic() - t_on:5.1f}s xrun={xrun} duty_mean={duty}% voices={voices}")
    time.sleep(1.0)
s.cmd("STORE AUTO ON")
s.cmd("UI play")
s.ser.note("探针结束")
s.ser.close()
print("log:", OUT / f"probe-postselftest-{stamp}.log")
