#!/usr/bin/env python3
"""Run ENG SELFTEST on Tab5 and compare its hashes with upstream golden.txt.

Usage:
    python3 scripts/device-selftest.py /dev/ttyACM0 [--output LOG]
    python3 scripts/device-selftest.py /dev/ttyACM0 --case NAME [--output LOG]
    python3 scripts/device-selftest.py --compare LOG [--case NAME]

Default is the FULL suite (182 cases): the firmware quiesces the UI for the
diagnostic run (pauses the 30ms timer and the LVGL timers, drops input events,
freezes engine reads/writes) and restores it afterwards, so a UI-live boot can
run the whole gate. --case keeps the old single-case selector; the diagnostic
mode (quiesce/freeze) applies the same way. The device path disables autosave for the run (DI-20: the
selftest restore would otherwise let the autosave tick overwrite the work project) and wraps it in
ENG AUDIO OFF/ON, so the
device ends in product state. --compare only re-checks a saved transcript and
never touches the device.

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
FULL_CASES = 182  # golden 条目数（不含 repeat）；全量口径的覆盖数
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
        # 覆盖数口径：全量必须恰 182 例（少了说明用例表/选择器退化），单例恰 1 例
        want_cases = 1 if case else FULL_CASES
        if summary.get("cases") != str(want_cases):
            print(f"DEVICE SUMMARY cases={summary.get('cases')} 期望 {want_cases}")
            ok = False
    else:
        print("DEVICE SUMMARY 缺失（日志中没有 ENG SELFTEST cases= 汇总行）")
        ok = False
    print(f"SELFTEST COMPARE identical={identical} mismatch={len(mismatches)} "
          f"missing={len(missing)} extra={len(extra)} result={'PASS' if ok else 'FAIL'}")
    if summary_note:
        print(f"DEVICE SUMMARY {summary_note}")
    return 0 if ok else 1


def send_command(ser, command):
    ser.write((command + "\n").encode())
    ser.flush()


def read_line(ser):
    raw = ser.readline()
    return raw.decode(errors="replace").strip() if raw else ""


def set_autosave_off(ser, timeout=10):
    """关掉自动存档（DI-20）：自检结束时 `selftest_restore()` 把引擎回滚到工厂态，
    autosave 的 hash 门/空闲门随后会把工厂态写回 OBJ_AUTOSAVE、覆盖用户的工作工程。
    门禁脚本在自检前关掉它，并且**不恢复**——工作工程的原 autosave 对象因此保持原样；
    需要自动存档的后续场景（soak 的 save 窗口等）自行开/关。"""
    send_command(ser, "STORE AUTO OFF")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = read_line(ser)
        if not line:
            continue
        print(line, flush=True)
        if "STORE AUTO off" in line:
            return True
    print("警告: STORE AUTO OFF 未在 %ds 内确认（自检可能覆盖 autosave，DI-20）" % timeout,
          file=sys.stderr)
    return False


def set_streaming(ser, on, timeout=10):
    """切 ENG AUDIO ON|OFF 并等待回执（打印读到的一切）。切不动只告警。"""
    send_command(ser, f"ENG AUDIO {'ON' if on else 'OFF'}")
    want = f"ENG AUDIO streaming={1 if on else 0}"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = read_line(ser)
        if not line:
            continue
        print(line, flush=True)
        if want in line:
            return True
    print(f"警告: ENG AUDIO {'ON' if on else 'OFF'} 未在 {timeout}s 内确认", file=sys.stderr)
    return False


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
        try:
            # DI-20：自检会把引擎回滚到工厂态；自动存档若还开着，随后会把工厂态写回
            # OBJ_AUTOSAVE、覆盖用户的工作工程。先关掉，且不恢复（见 set_autosave_off）。
            set_autosave_off(ser)
            # 固件开机即启流（DI-8），流式时 SELFTEST 会被拒绝：先停流
            set_streaming(ser, False)
            send_command(ser, command)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                line = read_line(ser)
                if not line:
                    continue
                print(line, flush=True)
                lines.append(line)
                if SUMMARY.match(line):
                    break
            else:
                raise TimeoutError(f"等待 ENG SELFTEST 汇总超时（{timeout}s）")
        finally:
            # 无论自检成败都恢复产品态（流式）；恢复失败只告警，不改 PASS/FAIL 语义
            set_streaming(ser, True)
    if not any(SUMMARY.match(line) for line in lines):
        raise RuntimeError("未收到 ENG SELFTEST 汇总")
    if output:
        Path(output).write_text("\n".join(lines) + "\n")
    return compare(lines, case=case)


def main():
    parser = argparse.ArgumentParser(
        description="运行设备 ENG SELFTEST 并与 host golden 比对。默认跑全量 182 例"
                    "（固件在诊断期静默 UI：暂停 UI 定时器/丢输入/冻结引擎，跑完恢复）；"
                    "--case 走单例旧口径。设备路径先自动 STORE AUTO OFF（不恢复，保护工作工程）"
                    "与 ENG AUDIO OFF，结束后恢复 ENG AUDIO ON")
    parser.add_argument("port", nargs="?")
    parser.add_argument("--case", help=f"只跑单个 use case（旧口径；默认全量 {FULL_CASES} 例）")
    parser.add_argument("--output", help="把设备回显逐行存盘（原始 transcript）")
    parser.add_argument("--timeout", type=float, default=900,
                        help="等待汇总行的超时秒数（全量实测约 160s，默认 900）")
    parser.add_argument("--compare", metavar="LOG",
                        help="离线复核已存 transcript；默认按全量 182 例校验，"
                             "旧单例 transcript 需同时给 --case NAME")
    args = parser.parse_args()
    if args.compare:
        return compare(Path(args.compare).read_text().splitlines(), case=args.case)
    if not args.port:
        parser.error("需要串口 PORT，或 --compare LOG")
    return run_serial(args.port, args.case, args.output, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
