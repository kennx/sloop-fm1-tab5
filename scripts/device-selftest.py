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
SUMMARY_FIELD = re.compile(r"(\w+)=(\S+)")


def summary_fields(lines):
    """最后一条 ENG SELFTEST 汇总行的 key=value 字段（旧固件可能缺少新字段）。"""
    for line in reversed(lines):
        if SUMMARY.match(line.strip()):
            return dict(SUMMARY_FIELD.findall(line))
    return {}


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

    # 设备侧汇总行是自检结论的权威（覆盖数/健康/结论）；旧日志缺少的新字段只在
    # 出现时校验，保持对既有证据的兼容。main_rc 只随汇总行打印、不参与判定：设备
    # 侧不跑 J_CPU 批量，上游会把跳过的 job 记为崩溃，main_rc 恒为非 0（见 selftest.c）。
    summary = summary_fields(lines)
    summary_note = ""
    if summary:
        summary_note = " ".join(f"{k}={v}" for k, v in summary.items())
        if summary.get("result") != "PASS":
            print(f"DEVICE SUMMARY result={summary.get('result')}")
            ok = False
        if summary.get("mismatch", "0") != "0":
            ok = False
        if "expected" in summary and summary.get("cases") != summary["expected"]:
            print(f"DEVICE SUMMARY cases={summary.get('cases')} expected={summary['expected']}")
            ok = False
    else:
        print("DEVICE SUMMARY 缺失（日志中没有 ENG SELFTEST cases= 汇总行）")
        ok = False
    print(f"SELFTEST COMPARE identical={identical} mismatch={len(mismatches)} "
          f"missing={len(missing)} extra={len(extra)} result={'PASS' if ok else 'FAIL'}")
    if summary_note:
        print(f"DEVICE SUMMARY {summary_note}")
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
