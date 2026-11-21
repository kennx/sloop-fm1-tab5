"""Task 9 修复轮设备断言会话（ad-hoc 证据留档）：选轨即刷投影 + 引擎越界 id 路径。

用法: python3 task-9-fix-device-check.py <evidence-dir> [port]
前置: 固件已烧录、console 就绪。产物: task-9-fix-device-transcript.log（原始回显 +
`# [param-check]` 工具注记）。
"""
import os
import re
import sys
import time

import serial

E = sys.argv[1]
PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"

log = open(os.path.join(E, "task-9-fix-device-transcript.log"), "wb")
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


def select(card_x, track):
    r = cmd("TAP %d 347" % card_x)
    check("TAP 选轨 %d" % track, r, r"-> TRACKS select track=%d CMD_SELECT_TRACK" % track)


def snap_param(track, atk):
    s = cmd("STATE SNAP")
    check("选轨 %d 后立即投影 trk=%d atk=%d" % (track, track, atk), s,
          r"STATE SNAP param trk=%d level=\d+ atk=%d " % (track, atk))


cmd("")
snap0 = cmd("STATE SNAP")
check("基线 param 行（LVL104 ATK10 SLEN16）", snap0, r"STATE SNAP param trk=0 level=104 atk=10 dec=70 sus=90 rel=60 slen=16 mute=0")
check("基线 global 行（bpm90 swing0）", snap0, r"STATE SNAP global bpm=90 swing=0")

# 每轨一个不同的 ATK，供「选轨即刷投影」断言
r = cmd("UI tracks")
check("切 TRACKS", r, r"UI page=tracks")
for card_x, atk in ((172, 11), (484, 22), (796, 33), (1108, 44)):
    select(card_x, (card_x - 172) // 312)
    r = cmd("PARAM 1 %d" % atk)
    check("写轨 %d atk=%d" % ((card_x - 172) // 312, atk), r,
          r"^PARAM track=%d id=1 value=%d applied=%d min=0 max=127$" % ((card_x - 172) // 312, atk, atk))

# Important 修复：选轨后**立即** STATE SNAP 即显示该轨参数（不是下一拍 sync）
for card_x, atk in ((172, 11), (484, 22), (796, 33), (1108, 44)):
    track = (card_x - 172) // 312
    select(card_x, track)
    snap_param(track, atk)
    select(172, 0)

# Minor 3：引擎「无描述符 id 逐条忽略」路径（PARAM RAW 跳过 console 的 desc 预判）
r = cmd("PARAM RAW 200 7")
check("RAW 越界 id 200 → n/a（引擎忽略）", r, r"^PARAM RAW track=0 id=200 value=7 applied=n/a$")
s = cmd("STATE SNAP")
check("RAW 越界 id 不改参数", s, r"STATE SNAP param trk=0 level=104 atk=11 ")
select(1108, 3)
r = cmd("PARAM RAW 64 5")
check("RAW 鼓轨 P_E1 → applied 仍为旧值 0（未写）", r, r"^PARAM RAW track=3 id=64 value=5 applied=0$")
r = cmd("PARAM RAW 63 99")
check("RAW 鼓轨 P_E0 kit 钳位 45", r, r"^PARAM RAW track=3 id=63 value=99 applied=45$")

# 原始断言回归（钳位/非法 id/批量）
select(172, 0)
r = cmd("PARAM 1 500")
check("PARAM 上限钳位 127", r, r"^PARAM track=0 id=1 value=500 applied=127 min=0 max=127$")
r = cmd("PARAM 999 1")
check("PARAM 越界 id 拒绝", r, r"^PARAM ERR track=0 id=999 reason=invalid_id$")
r = cmd("PARAM BATCH 0 100 1 20 29 8")
check("PARAM BATCH 三对回显", r, r"^PARAM BATCH track=0 n=3 id=0 value=100 applied=100 id=1 value=20 applied=20 id=29 value=8 applied=8$")
s = cmd("STATE SNAP")
check("BATCH 后投影 level=100 atk=20 slen=8", s, r"STATE SNAP param trk=0 level=100 atk=20 dec=70 sus=90 rel=60 slen=8")

# 还原：轨 0 默认值 + 轨 1..3 的 ATK 回默认 + 鼓 kit 回 0，最后选轨 0
r = cmd("PARAM BATCH 0 104 1 10 29 16 63 0")
check("还原轨 0 默认值", r, r"^PARAM BATCH track=0 n=4 id=0 value=104 applied=104 id=1 value=10 applied=10 id=29 value=16 applied=16 id=63 value=0 applied=0$")
for card_x, track in ((484, 1), (796, 2), (1108, 3)):
    select(card_x, track)
    cmd("PARAM 1 10")
select(1108, 3)
cmd("PARAM 63 0")
select(172, 0)
s = cmd("STATE SNAP")
check("还原后 STATE SNAP", s, r"STATE SNAP param trk=0 level=104 atk=10 dec=70")
check("还原后选轨 0", s, r"selected_track=0")
cmd("UI play")
ser.close()
log.close()
print("RESULT: %s (failed=%d)" % ("PASS" if not fails else "FAIL", len(fails)))
sys.exit(0 if not fails else 1)
