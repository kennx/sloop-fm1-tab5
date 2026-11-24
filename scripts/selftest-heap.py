#!/usr/bin/env python3
"""连续两次全量 ENG SELFTEST 的内部堆泄漏门禁（原 DI-26：每次全量泄漏 ~40KB）。

Usage:
    python3 scripts/selftest-heap.py /dev/ttyACM0 [--record-only] [--timeout SEC]

流程：esptool 硬复位 → `ENG MEM`（开机读数）→ `STORE AUTO OFF` + `ENG AUDIO OFF`（与
device-selftest.py 同一套准备/收尾：自检会回滚引擎与 autosave，流式时自检被拒）→
`ENG MEM` → 全量 `ENG SELFTEST` → `ENG MEM` → `ENG MEM` → 全量 → `ENG MEM` → `ENG AUDIO ON`。

默认断言（全部满足才退出码 0）：
  - 两次全量自检的设备侧汇总行都是 `result=PASS`（固件用内嵌 golden 逐例比对并自判覆盖数/健康/
    确定性；本脚本不另做 host golden 比对——那是 device-selftest.py `compare` 的事，这里只复用
    它的 `set_autosave_off`/`set_streaming`/`SUMMARY_FIELD`）；
  - 第二次自检前后的 internal_free 差 ≤1,024B；
  - 第二次之后的 internal_free ≥61,440B（soak 的 60KB 门线，§5.5）。
`--record-only` 只断言两次自检 PASS，打印差值、不因堆差值失败（定位未收口时的矩阵兜底）。

transcript（原始回显 + `# [selftest-heap] ...` 工具注记）落
`firmware/build/verify/selftest-heap-<时间戳>.log`。
"""
import argparse
import importlib.util
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import screenshot as shot  # noqa: E402

BAUD = 115200
HEAP_DELTA_MAX = 1024
HEAP_FLOOR = 61440
MEM = re.compile(r"ENG MEM internal_free=(\d+) internal_min=(\d+) psram_free=(\d+)")
BOOT_BANNER = re.compile(r"^ESP-ROM:|^rst:0x")
NOTE = "# [selftest-heap]"


def load_device_selftest():
    spec = importlib.util.spec_from_file_location("device_selftest", ROOT / "scripts" / "device-selftest.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_line(ser):
    raw = ser.readline()
    return raw.decode(errors="replace").rstrip("\r\n") if raw else ""


class Transcript:
    def __init__(self, path):
        self.path = path

    def line(self, text):
        print(text, flush=True)
        with self.path.open("a", encoding="utf-8") as f:
            f.write(text + "\n")


def wait_for(ser, log, pattern, timeout, what):
    """发命令后读到匹配 pattern 的行为止；超时/设备重启（boot 横幅）抛 RuntimeError。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = read_line(ser)
        if not line:
            continue
        log.line(line)
        if BOOT_BANNER.search(line):
            raise RuntimeError(f"等待 {what} 时设备重启（内部堆耗尽或崩溃）")
        match = pattern.search(line)
        if match:
            return match, line
    raise TimeoutError(f"等待 {what} 超时（{timeout:.0f}s）")


def read_mem(ser, log, label):
    ser.write(b"ENG MEM\n")
    ser.flush()
    match, _ = wait_for(ser, log, MEM, 15, "ENG MEM")
    internal = int(match.group(1))
    log.line(f"{NOTE} {label} internal_free={internal} psram_free={match.group(3)}")
    return internal


def run_selftest(ser, log, timeout):
    ser.write(b"ENG SELFTEST\n")
    ser.flush()
    _, line = wait_for(ser, log, re.compile(r"^ENG SELFTEST cases="), timeout, "ENG SELFTEST 汇总行")
    return line


def main():
    parser = argparse.ArgumentParser(description="连续两次全量 ENG SELFTEST 的内部堆泄漏门禁")
    parser.add_argument("port", nargs="?")
    parser.add_argument("--record-only", action="store_true",
                        help="只断言两次自检 PASS；打印堆差值，不因差值失败")
    parser.add_argument("--timeout", type=float, default=600, help="单次全量自检的超时秒数（实测约 160s）")
    args = parser.parse_args()
    if not args.port:
        parser.error("需要串口 PORT")
    shot.require("serial", "pyserial")
    import serial
    ds = load_device_selftest()

    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = ROOT / "firmware" / "build" / "verify" / f"selftest-heap-{stamp}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")
    log = Transcript(log_path)
    log.line(f"{NOTE} mode={'record-only' if args.record_only else 'gate'} port={args.port}")

    readings = {}
    ok = True
    try:
        ser = serial.Serial(args.port, BAUD, timeout=1)
    except (OSError, serial.SerialException) as e:
        sys.exit(f"打开串口 {args.port} 失败: {e}")
    try:
        ser.close()
        shot.reset_device(args.port, BAUD)
        ser = serial.Serial(args.port, BAUD, timeout=1)
        time.sleep(5)
        ser.reset_input_buffer()
        readings["boot"] = read_mem(ser, log, "boot")
        ds.set_autosave_off(ser)
        ds.set_streaming(ser, False)
        summaries = []
        for run in (1, 2):
            readings[f"before{run}"] = read_mem(ser, log, f"run{run} before")
            summaries.append(run_selftest(ser, log, args.timeout))
            readings[f"after{run}"] = read_mem(ser, log, f"run{run} after")
        for run, summary in zip((1, 2), summaries):
            fields = dict(ds.SUMMARY_FIELD.findall(summary))
            verdict = fields.get("result")
            log.line(f"{NOTE} run{run}: before={readings[f'before{run}']} after={readings[f'after{run}']} "
                     f"delta={readings[f'before{run}'] - readings[f'after{run}']} result={verdict} "
                     f"cases={fields.get('cases')}/{fields.get('expected')} identical={fields.get('identical')}")
            if verdict != "PASS":
                ok = False
        delta2 = readings["before2"] - readings["after2"]
        log.line(f"{NOTE} 第二次自检前后 internal_free 差 = {delta2}B（门限 ≤{HEAP_DELTA_MAX}），"
                 f"收尾 {readings['after2']}B（门限 ≥{HEAP_FLOOR}）")
        if not args.record_only:
            if delta2 > HEAP_DELTA_MAX:
                ok = False
                log.line(f"{NOTE} FAIL: 差值 {delta2}B > {HEAP_DELTA_MAX}B")
            if readings["after2"] < HEAP_FLOOR:
                ok = False
                log.line(f"{NOTE} FAIL: 收尾 {readings['after2']}B < {HEAP_FLOOR}B")
    except (RuntimeError, TimeoutError) as e:
        ok = False
        log.line(f"{NOTE} FAIL: {e}")
    finally:
        try:
            ds.set_streaming(ser, True)
        except (OSError, serial.SerialException, RuntimeError) as e:
            log.line(f"{NOTE} 警告: ENG AUDIO ON 未确认: {e}")
        ser.close()
    log.line(f"{NOTE} {'PASS' if ok else 'FAIL'} log={log_path.relative_to(ROOT)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
