#!/usr/bin/env python3
"""Run ENG SELFTEST on Tab5 and compare its hashes with upstream golden.txt.

Usage:
    python3 scripts/device-selftest.py /dev/ttyACM0 [--case NAME] [--output LOG]
    python3 scripts/device-selftest.py --compare LOG

A hash mismatch is always a failure. The error-vector/SNR channel is a separate
analysis step; this comparator never applies a tolerance or silently accepts one.
"""
import argparse
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOLDEN = ROOT / "third_party/sloop-fm1/tests/golden.txt"
HASH_LINE = re.compile(r"^(\S+) ([0-9a-f]{16})$")
SUMMARY = re.compile(r"^ENG SELFTEST cases=")


def read_key_values(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, value = line.split()[:2]
        result[name] = value
    return result


def device_hashes(lines):
    result = {}
    for line in lines:
        match = HASH_LINE.match(line.strip())
        if match:
            result[match.group(1)] = match.group(2)
    return result


def compare(lines, golden_path=GOLDEN, case=None):
    golden = read_key_values(golden_path)
    if case:
        golden = {case: golden[case]} if case in golden else {}
    device = device_hashes(lines)
    names = [name for name in golden if name in device] + [name for name in device if name not in golden]
    identical = sum(device[name] == golden.get(name) for name in names)
    mismatches = [name for name in names if name in golden and device[name] != golden[name]]
    missing = [name for name in golden if name not in device]
    extra = [name for name in device if name not in golden]
    for name in mismatches:
        print(f"MISMATCH {name} host={golden[name]} device={device[name]}")
    for name in missing:
        print(f"MISSING {name} host={golden[name]}")
    for name in extra:
        print(f"EXTRA {name} device={device[name]}")
    ok = not mismatches and not missing and not extra and bool(device)
    print(f"SELFTEST COMPARE identical={identical} mismatch={len(mismatches)} "
          f"missing={len(missing)} extra={len(extra)} result={'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def run_serial(port, case, output, timeout):
    try:
        import serial
    except ImportError:
        sys.exit("缺少 pyserial：pip install -r scripts/requirements.txt")
    command = "ENG SELFTEST" + (f" {case}" if case else "")
    lines = []
    with serial.Serial(port, 115200, timeout=1) as ser:
        time.sleep(5)
        ser.reset_input_buffer()
        ser.write((command + "\n").encode())
        ser.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode(errors="replace").strip()
            if line:
                print(line, flush=True)
                lines.append(line)
            if SUMMARY.match(line):
                break
        else:
            raise TimeoutError(f"等待 ENG SELFTEST 汇总超时（{timeout}s）")
    if not any(SUMMARY.match(line) for line in lines):
        raise RuntimeError("未收到 ENG SELFTEST 汇总")
    if output:
        Path(output).write_text("\n".join(lines) + "\n")
    return compare(lines, case=case)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?")
    parser.add_argument("--case")
    parser.add_argument("--output")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--compare", metavar="LOG")
    args = parser.parse_args()
    if args.compare:
        return compare(Path(args.compare).read_text().splitlines(), case=args.case)
    if not args.port:
        parser.error("需要串口 PORT，或 --compare LOG")
    return run_serial(args.port, args.case, args.output, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
