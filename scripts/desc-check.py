#!/usr/bin/env python3
"""ENG DESC 描述符抽样：设备回显逐字段对照 params.c 的期望表（Task 8 门禁）。

用法:
    python3 scripts/desc-check.py [PORT] [--output LOG]

PORT 缺省自动探测（/dev/ttyACM* 优先，其次 /dev/ttyUSB*）。10 个正例覆盖
F_DB / F_TIME / F_BIPCT / F_ENUM（含非空枚举名）/ F_BPM / 鼓轨 kit 哨兵 /
ANALOG·FM6·SAMPLE 三引擎 edit[]（含 unit="ct" 与 F_INT+names 表）；另验证 7 条
越界路径回 `ENG DESC ERR`。原始串口回显逐字节存
`$SLOOP_TAB5_PROJECT/build/verify/desc-<时间戳>.log`（只含 `# [desc-check]` 工具注记）。

live scope（P4-F Task 3）：`ENG DESC live <track> <id>` = sloop_core_desc_track_live（上游
track_desc()）的 2 个抽样——在 `STATE SNAP` 的选中轨上用 `PARAM` 写入 P_ITYPE / P_ESYNC，
断言 P_IA 的标签（INSERT）与 P_ATK 的格式/音符名表（ENV SYNC）随状态变化、范围不变；
写前读原值、结束时逐项写回（结束时状态与开始时相同）。期望值硬编码自 params.c 的
INS_DESC / ESYNC_DESC。另 3 条 live 越界（鼓轨 P_E1 / 轨 4 / id ≥ P_COUNT）。
退出码 0 = 全部字段一致且越界路径被拒。
"""
import argparse
import glob
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BAUD = 115200

# core.h 的 fmt 枚举顺序
F_INT, F_PCT, F_BIPCT, F_TIME, F_LFOHZ, F_CUTOFF, F_DB, F_SEMI, F_ENUM, F_BPM, \
    F_NOTE, F_ONOFF, F_OCT, F_STEPS, F_SWING, F_FILT = range(16)

FIELDS = ("label", "fmt", "min", "max", "def", "unit", "enums", "first", "last")
RESPONSE = re.compile(r"^ENG DESC (scope=|ERR )")
FIELD = re.compile(r'(\w+)=(?:"([^"]*)"|(\S+))')

# 期望值来源：third_party/sloop-fm1/firmware/src/params.c（TP/GP/DRUM_KIT_DESC）与
# 各引擎 edit[] 表。unit 是上游可选单位（多数 NULL，显示单位由 fmt 决定）。
CASES = [
    dict(name="P_LEVEL", scope="track", engine=0, id=0, want=dict(
        label="LVL", fmt=F_DB, min=0, max=127, default=104, unit="-", enums=0, first="-", last="-")),
    dict(name="P_ATK", scope="track", engine=0, id=1, want=dict(
        label="ATK", fmt=F_TIME, min=0, max=127, default=10, unit="-", enums=0, first="-", last="-")),
    dict(name="P_ED_FLT", scope="track", engine=0, id=5, want=dict(
        label="FLT", fmt=F_BIPCT, min=-64, max=63, default=0, unit="-", enums=0, first="-", last="-")),
    dict(name="P_ITYPE", scope="track", engine=0, id=53, want=dict(
        label="INSRT", fmt=F_ENUM, min=0, max=8, default=0, unit="-", enums=9, first="OFF", last="CHOR")),
    dict(name="G_BPM", scope="global", engine=0, id=0, want=dict(
        label="BPM", fmt=F_BPM, min=40, max=240, default=90, unit="-", enums=0, first="-", last="-")),
    dict(name="DRUM_KIT", scope="engine", engine=12, id=0, want=dict(
        label="KIT", fmt=F_ENUM, min=0, max=45, default=0, unit="-", enums=46, first="ACOUSTIC", last="SYN4")),
    dict(name="ANALOG_WAVE", scope="engine", engine=0, id=0, want=dict(
        label="WAVE", fmt=F_ENUM, min=0, max=4, default=0, unit="-", enums=5, first="SAW", last="PWM")),
    dict(name="ANALOG_DTN", scope="engine", engine=0, id=1, want=dict(
        label="DTN", fmt=F_INT, min=0, max=127, default=10, unit="ct", enums=0, first="-", last="-")),
    dict(name="FM6_ALG", scope="engine", engine=9, id=0, want=dict(
        label="ALG", fmt=F_INT, min=0, max=32, default=0, unit="-", enums=33, first="PAT", last="32")),
    dict(name="SAMPLE_SET", scope="engine", engine=4, id=0, want=dict(
        label="SET", fmt=F_ENUM, min=0, max=11, default=0, unit="-", enums=12, first="PIANO", last="USR4")),
]

NEGATIVE = [
    dict(scope="track", engine=0, id=63, why="P_E0..P_E7 无轨道上下文"),
    dict(scope="global", engine=0, id=50, why="G_COUNT=50 上界外"),
    dict(scope="engine", engine=13, id=0, why="engine 13 不存在（哨兵只到 12）"),
    dict(scope="engine", engine=12, id=1, why="鼓轨只有 P_E0 = kit"),
    dict(scope="live", engine=3, id=64, why="鼓轨没有 P_E1..P_E7（只有 P_E0 = kit）"),
    dict(scope="live", engine=4, id=0, why="轨 4 不存在"),
    dict(scope="live", engine=0, id=71, why="id ≥ P_COUNT=71"),
]

# live 抽样（上游 P_* 枚举序：P_ATK=1、P_ITYPE=53、P_IA=54、P_ESYNC=59）
LIVE_P_ATK, LIVE_P_ITYPE, LIVE_P_IA, LIVE_P_ESYNC = 1, 53, 54, 59
# params.c ins_desc()：TYPE 1..4 (SOFT..FUZZ) → "DRIVE" (F_PCT)；5 (CRUSH) → "BITS" (F_INT + N_IBITS 16 名)
LIVE_INSERT = {1: dict(label="DRIVE", fmt=F_PCT, min=0, max=127, default=64, unit="-", enums=0, first="-", last="-"),
               5: dict(label="BITS", fmt=F_INT, min=0, max=127, default=64, unit="-", enums=16, first="1", last="16")}
# params.c ESYNC_DESC[0]：ATK 开 ENV SYNC → F_INT + ESYNC_NAMES 25 名（"0".."4BAR"），范围/默认值同 TP[P_ATK]
LIVE_ESYNC_ON = dict(label="ATK", fmt=F_INT, min=0, max=127, default=10, unit="-", enums=25, first="0", last="4BAR")
LIVE_ESYNC_OFF = dict(label="ATK", fmt=F_TIME, min=0, max=127, default=10, unit="-", enums=0, first="-", last="-")


def default_port():
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def _prompt_at(buf):
    i = buf.rfind(b"sloop>")
    return i >= 0 and buf[i + len(b"sloop>"):].strip() == b""


def read_response(ser, timeout):
    buf = bytearray()
    deadline = time.monotonic() + timeout
    ser.timeout = 0.05
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf.extend(chunk)
            continue
        if _prompt_at(buf):
            return bytes(buf)
    raise TimeoutError(f"等待提示符超时（已收 {len(buf)} 字节）")


class Session:
    def __init__(self, port, log_path, timeout):
        try:
            import serial
        except ImportError:
            sys.exit("缺少 pyserial：pip install -r scripts/requirements.txt")
        self.timeout = timeout
        self.log = open(log_path, "wb")
        self.ser = serial.Serial(port, BAUD, timeout=0.05)
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self.cmd("")

    def note(self, text):
        self.log.write(f"# [desc-check] {text}\n".encode())

    def cmd(self, line):
        self.note(line or "<sync>")
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        try:
            raw = read_response(self.ser, self.timeout)
        except TimeoutError as e:
            raise RuntimeError(f"命令 {line!r} 无响应：{e}") from None
        self.log.write(raw)
        self.log.flush()
        text = raw.decode(errors="replace").replace("sloop>", "")
        return [ln.strip() for ln in text.splitlines() if ln.strip()]

    def close(self):
        self.ser.close()
        self.log.close()


def parse_fields(line):
    fields = {}
    for m in FIELD.finditer(line):
        fields[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(2)
    return fields


def check_case(session, case):
    line = f"ENG DESC {case['scope']} {case['engine']} {case['id']}"
    lines = session.cmd(line)
    response = next((ln for ln in lines if RESPONSE.match(ln)), None)
    tag = f"{case['name']} ({case['scope']} {case['engine']} {case['id']})"
    if response is None or response.startswith("ENG DESC ERR"):
        print(f"  [FAIL] {tag}: 无描述符回显（{response or '空响应'}）")
        return False
    got = parse_fields(response)
    bad = []
    for key in FIELDS:
        want = case["want"]["default" if key == "def" else key]
        if got.get(key) != str(want):
            bad.append(f"{key}={got.get(key)!r}（期望 {want!r}）")
    if bad:
        print(f"  [FAIL] {tag}: " + "；".join(bad))
        return False
    print(f"  [PASS] {tag}: label={got['label']} fmt={got['fmt']} "
          f"min={got['min']} max={got['max']} def={got['def']} unit={got['unit']} "
          f"enums={got['enums']} first={got['first']} last={got['last']}")
    return True


def check_negative(session, case):
    line = f"ENG DESC {case['scope']} {case['engine']} {case['id']}"
    lines = session.cmd(line)
    tag = f"{case['scope']} {case['engine']} {case['id']}（{case['why']}）"
    if any(ln.startswith("ENG DESC ERR") for ln in lines):
        print(f"  [PASS] 越界拒绝 {tag}")
        return True
    print(f"  [FAIL] 越界未拒绝 {tag}: {lines[-1] if lines else '空响应'}")
    return False


def live_fields(session, track, pid):
    """`ENG DESC live <track> <id>` 的字段；无描述符（ERR）回 None。"""
    lines = session.cmd(f"ENG DESC live {track} {pid}")
    response = next((ln for ln in lines if RESPONSE.match(ln)), None)
    if response is None or response.startswith("ENG DESC ERR"):
        return None
    return parse_fields(response)


def live_expect(session, track, pid, want, what):
    got = live_fields(session, track, pid)
    bad = [] if got else ["无描述符回显"]
    for key in (FIELDS if got else ()):
        w = want["default" if key == "def" else key]
        if got.get(key) != str(w):
            bad.append(f"{key}={got.get(key)!r}（期望 {w!r}）")
    if bad:
        print(f"  [FAIL] live {what}: " + "；".join(bad))
        return False
    print(f"  [PASS] live {what}: label={got['label']} fmt={got['fmt']} min={got['min']} max={got['max']} "
          f"def={got['def']} unit={got['unit']} enums={got['enums']} first={got['first']} last={got['last']}")
    return True


def param_value(session, pid):
    """`PARAM <id>`（只读）的引擎真值。"""
    lines = session.cmd(f"PARAM {pid}")
    m = next((re.match(r"PARAM track=\d+ id=\d+ value=(-?\d+) ", ln) for ln in lines
              if re.match(r"PARAM track=\d+ id=\d+ value=-?\d+ ", ln)), None)
    if m is None:
        raise RuntimeError(f"PARAM {pid} 只读无回显：{lines[-1] if lines else '空响应'}")
    return int(m.group(1))


def param_write(session, pid, value):
    lines = session.cmd(f"PARAM {pid} {value}")
    if not any(re.match(rf"PARAM track=\d+ id={pid} value={value} applied={value} ", ln) for ln in lines):
        raise RuntimeError(f"PARAM {pid} {value} 未落地：{lines[-1] if lines else '空响应'}")


def check_live(session):
    """live scope 的 2 个抽样：INSERT（随 P_ITYPE）与 ENV SYNC（随 P_ESYNC）。
    在选中轨上写、断言、写回；写回放在 finally 里，断言失败也不留下改动。"""
    lines = session.cmd("STATE SNAP")
    m = next((re.search(r"selected_track=(\d+)", ln) for ln in lines if "selected_track=" in ln), None)
    if m is None:
        print("  [FAIL] live: STATE SNAP 无 selected_track")
        return 0, 2
    track = int(m.group(1))
    orig = {LIVE_P_ITYPE: param_value(session, LIVE_P_ITYPE),
            LIVE_P_ESYNC: param_value(session, LIVE_P_ESYNC)}
    session.note(f"live samples on selected track {track}: P_ITYPE={orig[LIVE_P_ITYPE]} "
                 f"P_ESYNC={orig[LIVE_P_ESYNC]}（结束后写回）")
    got = [False, False]
    try:
        # INSERT：挑一个与当前标签不同的 TYPE（原值已是 SOFT 时改用 CRUSH），否则断言没有判别力
        before = live_fields(session, track, LIVE_P_IA)
        itype = 5 if (before or {}).get("label") == "DRIVE" else 1
        param_write(session, LIVE_P_ITYPE, itype)
        got[0] = live_expect(session, track, LIVE_P_IA, LIVE_INSERT[itype], f"INSERT track={track} P_ITYPE={itype} P_IA")
        # ENV SYNC：先关（F_TIME）再开（F_INT + 音符名），两个方向都与写前状态无关
        param_write(session, LIVE_P_ESYNC, 0)
        off_ok = live_expect(session, track, LIVE_P_ATK, LIVE_ESYNC_OFF, f"ENV SYNC off track={track} P_ATK")
        param_write(session, LIVE_P_ESYNC, 1)
        on_ok = live_expect(session, track, LIVE_P_ATK, LIVE_ESYNC_ON, f"ENV SYNC on track={track} P_ATK")
        got[1] = off_ok and on_ok
    finally:
        for pid, value in orig.items():
            param_write(session, pid, value)
    restored = all(param_value(session, pid) == value for pid, value in orig.items())
    after = live_fields(session, track, LIVE_P_IA)
    if not restored or after is None or any(after.get(k) != (before or {}).get(k) for k in FIELDS):
        print("  [FAIL] live: 写回后 P_ITYPE/P_ESYNC 或 P_IA 描述符与开始时不同")
        return sum(got), 2
    print(f"  [PASS] live: P_ITYPE/P_ESYNC 写回原值，P_IA 描述符回到 {after.get('label')}")
    return sum(got), 2


def main():
    parser = argparse.ArgumentParser(
        description="设备 ENG DESC 抽样逐字段对照 params.c 期望表（Task 8 门禁）")
    parser.add_argument("port", nargs="?", help="串口（缺省自动探测 ttyACM*/ttyUSB*）")
    parser.add_argument("--output", help="原始回显日志路径（默认 build/verify/desc-<时间戳>.log）")
    parser.add_argument("--timeout", type=float, default=3.0, help="单条命令等待秒数（默认 3）")
    args = parser.parse_args()

    port = args.port or default_port()
    if port is None:
        parser.error("未探测到串口，请显式给 PORT")
    project = Path(os.environ.get("SLOOP_TAB5_PROJECT") or (ROOT / "firmware"))
    out_dir = project / "build" / "verify"
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = Path(args.output) if args.output else out_dir / time.strftime("desc-%Y%m%d-%H%M%S.log")

    session = Session(port, log_path, args.timeout)
    try:
        passed = sum(check_case(session, c) for c in CASES)
        rejected = sum(check_negative(session, c) for c in NEGATIVE)
        live_passed, live_total = check_live(session)
    finally:
        session.close()

    ok = passed == len(CASES) and rejected == len(NEGATIVE) and live_passed == live_total
    print(f"DESC CHECK passed={passed}/{len(CASES)} live={live_passed}/{live_total} "
          f"negative={rejected}/{len(NEGATIVE)} result={'PASS' if ok else 'FAIL'}")
    print(f"transcript: {log_path}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
