#!/usr/bin/env python3
"""ENG DESC 描述符抽样：设备回显逐字段对照 params.c 的期望表（Task 8 门禁）。

用法:
    python3 scripts/desc-check.py [PORT] [--output LOG]

PORT 缺省自动探测（/dev/ttyACM* 优先，其次 /dev/ttyUSB*）。10 个正例覆盖
F_DB / F_TIME / F_BIPCT / F_ENUM（含非空枚举名）/ F_BPM / 鼓轨 kit 哨兵 /
ANALOG·FM6·SAMPLE 三引擎 edit[]（含 unit="ct" 与 F_INT+names 表）；另验证 4 条
越界路径回 `ENG DESC ERR`（不改状态）。原始串口回显逐字节存
`$SLOOP_TAB5_PROJECT/build/verify/desc-<时间戳>.log`（只含 `# [desc-check]` 工具注记）。
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
]


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
    finally:
        session.close()

    ok = passed == len(CASES) and rejected == len(NEGATIVE)
    print(f"DESC CHECK passed={passed}/{len(CASES)} negative={rejected}/{len(NEGATIVE)} "
          f"result={'PASS' if ok else 'FAIL'}")
    print(f"transcript: {log_path}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
