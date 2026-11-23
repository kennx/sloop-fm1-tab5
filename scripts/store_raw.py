#!/usr/bin/env python3
"""store_raw.py — 经 console `STORE RAW` 通道把任意 payload 原样写入一个存储对象

用途（P4-B spec §4.4/§6.3）：旧格式工程 fixture（FUN5/FUN4/FUN1，`gen_project_fixtures.py`
生成）单行放不进 console，故分块（一行 ≤240 hex = 120B：USB-Serial-JTAG 驱动 RX 环 256B，
整行必须一次装进环里，否则设备忙时静默丢字符；总长 ≤3,840B = 一个 flash 对象的 payload
上限，超出 BEGIN 当场回 `size`）：
    STORE RAW BEGIN <len>   → 回 "STORE RAW BEGIN len=<len>"
    STORE RAW <hex>         → 每块一次（≤240 hex = 120B），回 "STORE RAW received=<n>"
    STORE RAW END <obj> <slot> → 以 st_save 原样写入（CRC/提交头由 storage.c 生成），
                                回 "STORE RAW END <obj> <slot> saved len=<len>"
    STORE RAW ABORT         → 放弃（错误路径）
每行都等 console 提示符并断言回显；任一步失败抛 RawUploadError / 非零退出。
本模块的 `upload_raw()` 同时被 `scripts/inject/inject.py` 的 store 场景复用。

用法：
    python3 scripts/store_raw.py /dev/ttyACM0 fun5.bin --obj project --slot 3
"""

import argparse
import re
import sys
import time

BAUD = 115200
RAW_MAX = 3840        # sloop_store.h 的 SLOOP_STORE_RAW_MAX（= ST_PAYLOAD_MAX）
CHUNK = 120           # 240 hex 字符：整行 251B 必须一次装进 USJ 驱动 RX 环（256B）
PROMPT = b"sloop>"


class RawUploadError(RuntimeError):
    pass


def upload_raw(cmd, data: bytes, obj: str = "project", slot: int = 3, chunk: int = CHUNK,
               progress=None) -> None:
    """分块上传 data（1..RAW_MAX 字节）并以 st_save 原样写入 obj/slot。

    `cmd(line) -> str` 由调用方提供（读到 console 提示符为止；inject 场景传 Session.cmd）。
    逐行断言固件回显：RED 阶段（旧固件没有 STORE RAW）第一条 BEGIN 就失败。"""
    if not 1 <= len(data) <= RAW_MAX:
        raise RawUploadError(f"payload 长度 {len(data)} 越界（1..{RAW_MAX}）")
    if chunk <= 0 or chunk * 2 + len("STORE RAW ") + 1 > 256:
        raise RawUploadError(f"块大小 {chunk} 会让一行超过 USJ 驱动 RX 环（256B → ≤120 字节）")

    def must(line: str, pattern: str, label: str) -> None:
        text = cmd(line)
        if re.search(pattern, text, re.MULTILINE) is None:
            raise RawUploadError(
                f"{label}: 回显缺少 /{pattern}/（响应 {text.strip()[-200:]!r}）")

    must(f"STORE RAW BEGIN {len(data)}", rf"^STORE RAW BEGIN len={len(data)}\r?$",
         "STORE RAW BEGIN")
    for off in range(0, len(data), chunk):
        blk = data[off: off + chunk]
        must(f"STORE RAW {blk.hex().upper()}", rf"^STORE RAW received={off + len(blk)}\r?$",
             f"STORE RAW 块 @{off}")
        if progress is not None:
            progress(off + len(blk), len(data))
    must(f"STORE RAW END {obj} {slot}",
         rf"^STORE RAW END {re.escape(obj)} {slot} saved len={len(data)}\r?$",
         "STORE RAW END")


class Console:
    """独立运行时的最小 console：写一行、读到提示符。"""

    def __init__(self, port: str, timeout: float = 5.0):
        import serial
        self.timeout = timeout
        self.ser = serial.Serial(port, BAUD, timeout=0.05)

    def close(self) -> None:
        self.ser.close()

    def _read_to_prompt(self, timeout: float) -> str:
        buf = bytearray()
        deadline = time.monotonic() + timeout
        self.ser.timeout = 0.05
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf.extend(chunk)
                continue
            i = buf.rfind(PROMPT)
            if i >= 0 and buf[i + len(PROMPT):].strip() == b"":
                return buf.decode(errors="replace")
        raise RawUploadError(f"等待 console 提示符超时（已收 {len(buf)} 字节）")

    def cmd(self, line: str) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        return self._read_to_prompt(self.timeout)

    def wait_ready(self) -> None:
        try:
            self.cmd("")
        except RawUploadError:
            raise RawUploadError("console 无响应（设备未启动/串口被占用？）") from None


def main() -> int:
    ap = argparse.ArgumentParser(description="经 STORE RAW 分块上传一个对象 payload")
    ap.add_argument("port", help="串口设备，如 /dev/ttyACM0")
    ap.add_argument("file", help="payload 文件（如 gen_project_fixtures.py 的 fun5.bin）")
    ap.add_argument("--obj", default="project", help="对象名（project | settings）")
    ap.add_argument("--slot", type=int, default=3, help="对象槽（project: 0..3，默认诊断槽 3；settings: 只能 0）")
    ap.add_argument("--timeout", type=float, default=5.0, help="单行回显超时秒数（默认 5）")
    args = ap.parse_args()

    try:
        data = open(args.file, "rb").read()
    except OSError as e:
        sys.exit(f"读取 {args.file} 失败: {e}")

    con = Console(args.port, args.timeout)
    try:
        con.wait_ready()
        upload_raw(con.cmd, data, args.obj, args.slot,
                   progress=lambda n, total: print(f"  {n}/{total} 字节", flush=True))
    except RawUploadError as e:
        con.cmd("STORE RAW ABORT")  # 尽力收口（旧固件无此命令也无害）
        sys.exit(f"[FAIL] {args.file}: {e}")
    finally:
        con.close()
    print(f"[OK] {args.file} → {args.obj} {args.slot}（{len(data)} 字节）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
