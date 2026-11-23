import re, sys, time
from pathlib import Path
ROOT = Path("<repo>")
sys.path.insert(0, str(ROOT / "scripts" / "inject"))
import inject  # noqa: E402
PORT = "/dev/ttyACM0"
OUT = ROOT / "firmware" / "build" / "verify"
stamp = time.strftime("%Y%m%d-%H%M%S")
s = inject.Session(PORT, OUT / f"probe-postselftest4-{stamp}.log", OUT, stamp, "probe", timeout=60.0, retries=0)
s.wait_ready()
def perf(label):
    s.cmd("ENG PERF")
    m = re.search(r"ENG PERF streaming=(\d) xrun=(\d+).*?render_blocks=(\d+) render_mean_us=(\d+) duty_mean=([0-9.]+)", s.last)
    print(f"[{label}] xrun={m.group(2)} blocks={m.group(3)} mean_us={m.group(4)} duty={m.group(5)}%")
def mem(label):
    s.cmd("ENG MEM"); m = re.search(r"internal_free=(\d+) internal_min=(\d+)", s.last)
    print(f"[{label}] internal_free={m.group(1)} internal_min={m.group(2)}")
s.reboot(); time.sleep(8)
s.cmd("STORE AUTO OFF")
mem("自检前")
s.cmd("ENG AUDIO OFF")
t0 = time.monotonic(); s.cmd("ENG SELFTEST", timeout=400.0)
print(f"全量自检 {time.monotonic()-t0:.1f}s:", [l for l in s.last.splitlines() if "result=" in l][:1])
mem("自检后（未起流）")
t0 = time.monotonic(); s.cmd("ENG AUDIO ON", timeout=60.0)
print(f"[全量自检后] ON 响应 {time.monotonic()-t0:.2f}s")
perf("全量自检后")
s.cmd("KEY PANIC"); time.sleep(1.5); perf("KEY PANIC 后")
s.cmd("ENG SNAP"); print("ENG SNAP:", [l[:120] for l in s.last.splitlines() if l.startswith("ENG SNAP")][:5])
s.cmd("ENG AUDIO OFF"); time.sleep(3); s.cmd("ENG AUDIO ON", timeout=60.0); perf("停流 3s 再起")
mem("结束时")
s.cmd("STORE AUTO ON")
s.ser.note("probe4 结束"); s.ser.close()
