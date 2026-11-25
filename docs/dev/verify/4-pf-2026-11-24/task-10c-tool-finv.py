"""Task 10c 一次性探针：TOOLS CLRSEQ 执行 + UNDO 之后 LIVEHASH 是否回到执行前；不回时差在哪里
（上游 steps_clear 清 step/lock/fill，undo_swap 只换 step/micro/LEN）。"""
import importlib.util, re, sys
from datetime import datetime
from pathlib import Path
spec = importlib.util.spec_from_file_location("inj", "scripts/inject/inject.py")
inj = importlib.util.module_from_spec(spec); spec.loader.exec_module(inj)
inj.require_deps()
out_dir = Path("/tmp/p4f_10c/probes"); stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
s = inj.Session(sys.argv[1], out_dir / f"probe-finv-{stamp}.log", out_dir, stamp, "probe-finv")
s.wait_ready()
try:
    inj.ensure_stopped(s)
    s.cmd("STATE SNAP"); sel = s.snap_int(r"selected_track=(\d+)")
    h0 = inj.live_hash(s)
    s.cmd(f"SEQ LOCK LIST {sel}"); locks0 = s.last
    def steps():
        return "\n".join(step_line(s, sel, i) for i in range(64))
    def step_line(s, t, i):
        s.cmd(f"ENG STEP {t} {i}"); return [l for l in s.last.splitlines() if not l.startswith("sloop>")][0]
    st0 = steps()
    nonempty = sum(1 for l in st0.splitlines() if not re.search(r"\bn=0\b", l))
    s.cmd("UI tools"); s.expect(r"UI page=tools \(18\)", "finv UI tools")
    s.cmd("TAP %d %d" % inj.TOOLS_CHIP[0]); s.expect(r"-> TOOLS CLRSEQ armed", "finv 预备")
    s.cmd("TAP %d %d" % inj.TOOLS_CHIP[0]); s.expect(r'-> TOOLS CLRSEQ value=0 status="PATTERN CLEARED"', "finv 窗口内第二击 ⇒ 执行")
    h1 = inj.live_hash(s)
    s.cmd(f"SEQ PATTERN UNDO {sel}"); s.expect(r"SEQ PATTERN UNDO .*OK", "finv UNDO")
    h2 = inj.live_hash(s)
    st2 = steps()
    s.cmd(f"SEQ LOCK LIST {sel}"); locks2 = s.last
    (s.ok if st2 == st0 else s.fail)("finv UNDO 后 ENG STEP ×64 逐行 == 执行前（step/micro/LEN 已还原）")
    print(f"finv: sel={sel} 非空步 {nonempty}/64；LIVEHASH 执行前 {h0:08X} / 清空后 {h1:08X} / UNDO 后 {h2:08X}")
    print("finv: 执行前 lock list:\n" + "\n".join(l for l in locks0.splitlines() if not l.startswith("sloop>")))
    print("finv: UNDO 后 lock list:\n" + "\n".join(l for l in locks2.splitlines() if not l.startswith("sloop>")))
    fill0 = [l for l in st0.splitlines() if re.search(r"fill=[1-3]", l)]
    fill2 = [l for l in st2.splitlines() if re.search(r"fill=[1-3]", l)]
    print(f"finv: fill≠0 的步：执行前 {len(fill0)} / UNDO 后 {len(fill2)}")
    s.cmd("UI play")
except inj.InjectError as e:
    s.fail(f"异常终止: {e}")
finally:
    s.ser.note(f"probe finv 结束 failures={s.failures}"); s.ser.close()
print(f"[{'PASS' if s.failures == 0 else 'FAIL'}] probe finv: failures={s.failures}")
sys.exit(1 if s.failures else 0)
