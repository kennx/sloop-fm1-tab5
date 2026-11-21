"""Task 9 设备断言会话（ad-hoc 证据留档）：pyserial 原始回显 + PARAM/STATE/ENG 断言。

用法: python3 task-9-device-check.py <evidence-dir> [port]
前置: 固件已烧录、console 就绪。产物: task-9-device-transcript.log（原始回显 +
`# [param-check]` 工具注记）。
"""
import os
import re
import sys
import time

import serial

E = sys.argv[1]
PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"

log = open(os.path.join(E, "task-9-device-transcript.log"), "wb")
ser = serial.Serial(PORT, 115200, timeout=0.05)
time.sleep(0.2)
ser.reset_input_buffer()
fails = []


def note(text):
    log.write(("# [param-check] %s\n" % text).encode())


def cmd(line, timeout=8.0):
    note(line or "<sync>")
    ser.reset_input_buffer()
    ser.write((line + "\n").encode())
    ser.flush()
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf.extend(chunk)
            continue
        if buf.rstrip().endswith(b"sloop>"):
            log.write(bytes(buf))
            log.flush()
            text = buf.decode(errors="replace").replace("sloop>", "")
            return [ln.strip() for ln in text.splitlines() if ln.strip()]
    log.write(bytes(buf))
    log.flush()
    raise RuntimeError("无响应: %r" % line)


def check(name, lines, pat):
    ok = any(re.search(pat, ln) for ln in lines)
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  <- " + repr(lines)))
    if not ok:
        fails.append(name)


def check_eq(name, got, want):
    ok = got == want
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        fails.append(name)
        for ln in got:
            if ln not in want:
                print("    extra:", ln)
        for ln in want:
            if ln not in got:
                print("    missing:", ln)


def find(lines, pat):
    for ln in lines:
        if re.search(pat, ln):
            return ln
    return ""


cmd("")
snap0 = cmd("STATE SNAP")
check("基线 param 行（LVL104 ATK10 SLEN16）", snap0, r"STATE SNAP param trk=0 level=104 atk=10 dec=70 sus=90 rel=60 slen=16 mute=0")
check("基线 global 行（bpm90 swing0）", snap0, r"STATE SNAP global bpm=90 swing=0")
eng0 = cmd("ENG SNAP")
step0 = cmd("ENG STEP 0 0")

r = cmd("PARAM 1 50")
check("PARAM 1 50 回显", r, r"^PARAM track=0 id=1 value=50 applied=50 min=0 max=127$")
snap1 = cmd("STATE SNAP")
check("STATE SNAP 立即反映 atk=50", snap1, r"STATE SNAP param trk=0 level=104 atk=50 dec=70")
check_eq("ENG SNAP 不受参数写影响", cmd("ENG SNAP"), eng0)
check_eq("ENG STEP 不受参数写影响", cmd("ENG STEP 0 0"), step0)

r = cmd("PARAM 1 500")
check("PARAM 上限钳位 127", r, r"^PARAM track=0 id=1 value=500 applied=127 min=0 max=127$")
r = cmd("PARAM 1 -50")
check("PARAM 下限钳位 0", r, r"^PARAM track=0 id=1 value=-50 applied=0 min=0 max=127$")
r = cmd("PARAM 999 1")
check("PARAM 越界 id 拒绝", r, r"^PARAM ERR track=0 id=999 reason=invalid_id$")
r = cmd("PARAM 63 5")
check("PARAM 引擎槽 id=63（ANALOG WAVE 0..4）", r, r"^PARAM track=0 id=63 value=5 applied=4 min=0 max=4$")

r = cmd("PARAM BATCH 0 100 1 20 29 8")
check("PARAM BATCH 三对回显", r, r"^PARAM BATCH track=0 n=3 id=0 value=100 applied=100 id=1 value=20 applied=20 id=29 value=8 applied=8$")
snap2 = cmd("STATE SNAP")
check("BATCH 后投影 level=100 atk=20 slen=8", snap2, r"STATE SNAP param trk=0 level=100 atk=20 dec=70 sus=90 rel=60 slen=8")
eng1 = cmd("ENG SNAP")
check("ENG SNAP trk0 level→100 len→8（其余不变）", [find(eng1, r"ENG SNAP trk k=0 ")],
      r"^ENG SNAP trk k=0 engine=0 preset=0 level=100 mute=0 armed=0 solo=0 len=8 div=2$")

r = cmd("UI tracks")
check("切 TRACKS", r, r"UI page=tracks")
r = cmd("TAP 1108 347")
check("TAP 卡 3 选鼓轨", r, r"-> TRACKS select track=3 CMD_SELECT_TRACK")
snap3 = cmd("STATE SNAP")
check("鼓轨参数投影 trk=3", snap3, r"STATE SNAP param trk=3 ")
r = cmd("PARAM 63 99")
check("鼓轨 P_E0 kit 钳位 0..45", r, r"^PARAM track=3 id=63 value=99 applied=45 min=0 max=45$")
r = cmd("PARAM 64 5")
check("鼓轨 P_E1 无描述符（仅 P_E0=kit）", r, r"^PARAM ERR track=3 id=64 reason=invalid_id$")
r = cmd("PARAM 1 30")
check("鼓轨 TP 参数（P_ATK）可写", r, r"^PARAM track=3 id=1 value=30 applied=30 min=0 max=127$")

r = cmd("TAP 172 347")
check("还原选轨 0", r, r"-> TRACKS select track=0 CMD_SELECT_TRACK")
r = cmd("PARAM BATCH 0 104 1 10 29 16 63 0")
check("还原轨 0 默认值", r, r"^PARAM BATCH track=0 n=4 id=0 value=104 applied=104 id=1 value=10 applied=10 id=29 value=16 applied=16 id=63 value=0 applied=0$")
snap4 = cmd("STATE SNAP")
check("还原后 STATE SNAP", snap4, r"STATE SNAP param trk=0 level=104 atk=10 dec=70")
cmd("UI play")
ser.close()
log.close()
print("RESULT: %s (failed=%d)" % ("PASS" if not fails else "FAIL", len(fails)))
sys.exit(0 if not fails else 1)
