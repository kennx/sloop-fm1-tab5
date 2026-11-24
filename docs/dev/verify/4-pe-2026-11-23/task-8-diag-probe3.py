import re, sys, time
from pathlib import Path
ROOT = Path("<repo>")
sys.path.insert(0, str(ROOT / "scripts" / "inject"))
import inject  # noqa: E402
PORT = "/dev/ttyACM0"
OUT = ROOT / "firmware" / "build" / "verify"
stamp = time.strftime("%Y%m%d-%H%M%S")
s = inject.Session(PORT, OUT / f"probe-postselftest3-{stamp}.log", OUT, stamp, "probe", timeout=60.0, retries=0)
s.wait_ready()
def perf(label):
    s.cmd("ENG PERF")
    m = re.search(r"ENG PERF streaming=(\d) xrun=(\d+).*?render_blocks=(\d+) render_mean_us=(\d+) render_max_us=(\d+) duty_mean=([0-9.]+)", s.last)
    print(f"[{label}] streaming={m.group(1)} xrun={m.group(2)} blocks={m.group(3)} mean_us={m.group(4)} max_us={m.group(5)} duty={m.group(6)}%")
s.ser.note("probe3: 复位后对照")
s.reboot(); time.sleep(8)
perf("复位后"); time.sleep(2); perf("复位后+2s")
s.cmd("STORE AUTO OFF")
s.cmd("ENG AUDIO OFF")
s.cmd("ENG SELFTEST preset/ANALOG/00_808_BOOM", timeout=60.0)
t0 = time.monotonic(); s.cmd("ENG AUDIO ON", timeout=60.0)
print(f"[单例自检后] ON 响应 {time.monotonic()-t0:.2f}s")
perf("单例自检后"); time.sleep(2); perf("单例自检后+2s")
s.cmd("KEY PANIC"); time.sleep(1.5); perf("KEY PANIC 后"); time.sleep(1.5); perf("KEY PANIC 后+1.5s")
s.cmd("UI transport"); s.cmd("TAP 560 210"); time.sleep(2)   # PLAY/STOP 切换一次
s.cmd("TAP 560 210"); time.sleep(1.5); perf("PLAY/STOP 一次后")
s.cmd("ENG AUDIO OFF"); s.cmd("ENG AUDIO ON", timeout=60.0); perf("再重启流")
s.cmd("STORE AUTO ON"); s.cmd("UI play")
s.ser.note("probe3 结束"); s.ser.close()
