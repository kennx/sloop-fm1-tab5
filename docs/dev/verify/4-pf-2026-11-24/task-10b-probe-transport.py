import sys, time, serial
sys.path.insert(0, "/home/kenn/projects/sloop-fm1-tab5/scripts/inject")
import inject
ser = serial.Serial("/dev/ttyACM0", 115200, timeout=0.05)
def cmd(c, t=0.6):
    ser.write((c + "\n").encode()); ser.flush()
    b = b""; t0 = time.time()
    while time.time() - t0 < t:
        ch = ser.read(4096)
        if ch:
            b += ch
            if b.rstrip().endswith(b"sloop>"): break
    return b.decode("utf-8", "replace")
print(cmd("ENG SNAP", 2.0)[:600])
cmd("UI transport"); time.sleep(0.5)
t_play = time.time()
r = cmd("TAP %d %d" % inject.TRANSPORT_PLAY, 0.3)
print(f"PLAY ack after {time.time()-t_play:.3f}s: {r.strip().splitlines()[-2] if len(r.strip().splitlines())>1 else r!r}")
for i in range(24):
    s = cmd("ENG SNAP", 0.25)
    line = [l for l in s.splitlines() if "pos=" in l]
    print(f"+{time.time()-t_play:.3f}s {line[0][:150] if line else s.strip()[:120]!r}")
cmd("TAP %d %d" % inject.TRANSPORT_STOP); cmd("UI play")
ser.close()
