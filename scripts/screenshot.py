#!/usr/bin/env python3
"""screenshot.py — 经串口从 Tab5 固件截取帧缓冲并保存 PNG（AGENTS.md §5.3 协议）

协议:
    PC  -> "SCR\\n"
    FW  -> "SCR <width> <height> <fmt> <len>\\n"   (fmt: rgb565 | rgb888)
    FW  -> <len 字节裸二进制帧缓冲>                (rgb565 为小端)
    FW  -> "SCR OK\\n"

固件未注册 SCR 命令时会超时并给出提示。

用法:
    python3 scripts/screenshot.py PORT [OUT.png] [--timeout 20] [--baud 115200]
    python3 scripts/screenshot.py --selftest          # 离线自测（无需设备）
默认输出: $SLOOP_TAB5_PROJECT/build/verify/screenshot-<时间戳>.png（AGENTS.md §5.4 约定路径）
依赖: pyserial, Pillow（numpy 可选，加速 RGB565 解码）
"""
import argparse
import importlib.util
import os
import struct
import sys
import time
from datetime import datetime
from pathlib import Path


def require(name: str, pip_name: str | None = None) -> None:
    if importlib.util.find_spec(name) is None:
        pip = pip_name or name
        sys.exit(f"缺少依赖 {name}：请执行 pip install -r scripts/requirements.txt（或 pip install {pip}）")


def read_exact(ser, n: int, timeout: float) -> bytes:
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"读取帧数据超时：已收 {len(buf)}/{n} 字节")
        ser.timeout = min(1.0, remaining)
        chunk = ser.read(n - len(buf))
        if chunk:
            buf.extend(chunk)
    return bytes(buf)


def read_header(ser, timeout: float):
    """读行直到 SCR 头；返回 (w, h, fmt, ln)。跳过启动噪音行。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode(errors="replace").strip()
        if text.startswith("SCR ") and len(text.split()) == 5:
            _, w, h, fmt, ln = text.split()
            if fmt not in ("rgb565", "rgb888"):
                raise ValueError(f"不支持的像素格式: {fmt}")
            return int(w), int(h), fmt, int(ln)
        if text:  # 透传固件侧其他输出，便于诊断
            print(f"  [fw] {text}", file=sys.stderr)
    raise TimeoutError("等待 SCR 响应头超时")


def rgb565_to_rgb(data: bytes, w: int, h: int):
    require("PIL", "pillow")
    from PIL import Image
    if len(data) != w * h * 2:
        raise ValueError(f"rgb565 数据长度不符: {len(data)} != {w * h * 2}")
    if importlib.util.find_spec("numpy"):
        import numpy as np
        px = np.frombuffer(data, dtype="<u2")
        r = ((px >> 11) & 0x1F).astype(np.uint8) << 3
        g = ((px >> 5) & 0x3F).astype(np.uint8) << 2
        b = (px & 0x1F).astype(np.uint8) << 3
        arr = np.stack([r, g, b], axis=-1).reshape(h, w, 3)
        return Image.fromarray(arr, "RGB")
    # 纯 Python 回退（约数秒）
    # 注: 5/6bit 通道用位扩展（<<3/<<2）而非等比缩放，纯白为 (248,252,248)。
    # 对撕裂/错位/缺色检查足够；将来若需像素级比对再改为 round(v*255/31) 型缩放。
    words = struct.unpack(f"<{w * h}H", data)
    out = bytearray(w * h * 3)
    for i, v in enumerate(words):
        out[i * 3] = ((v >> 11) & 0x1F) << 3
        out[i * 3 + 1] = ((v >> 5) & 0x3F) << 2
        out[i * 3 + 2] = (v & 0x1F) << 3
    return Image.frombytes("RGB", (w, h), bytes(out))


def rgb888_to_rgb(data: bytes, w: int, h: int):
    require("PIL", "pillow")
    from PIL import Image
    if len(data) != w * h * 3:
        raise ValueError(f"rgb888 数据长度不符: {len(data)} != {w * h * 3}")
    return Image.frombytes("RGB", (w, h), data)


def _open_serial(port: str, baud: int):
    require("serial", "pyserial")
    import serial
    return serial.Serial(port, baud, timeout=1.0)


def capture(port: str, out: str, baud: int, timeout: float) -> None:
    with _open_serial(port, baud) as ser:
        ser.reset_input_buffer()
        ser.write(b"SCR\n")
        ser.flush()
        w, h, fmt, ln = read_header(ser, timeout)
        print(f"  帧缓冲: {w}x{h} {fmt} ({ln} 字节)")
        expected = w * h * (2 if fmt == "rgb565" else 3)
        if ln != expected:
            raise ValueError(f"长度与分辨率不符: len={ln}, 期望 {expected}")
        data = read_exact(ser, ln, timeout)
        ser.timeout = 1.0  # read_exact 可能将其压到接近 0，尾标记需重新计时
        tail = ser.readline().decode(errors="replace").strip()
        if tail != "SCR OK":
            raise ValueError(f"尾标记异常: {tail!r}（期望 'SCR OK'）")

    img = rgb565_to_rgb(data, w, h) if fmt == "rgb565" else rgb888_to_rgb(data, w, h)
    img.save(out)
    print(f"已保存: {out}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口截屏（SCR 协议）")
    ap.add_argument("port", nargs="?", default=None,
                    help="串口设备，如 /dev/ttyACM0（--selftest 时省略）")
    ap.add_argument("out", nargs="?", default=None,
                    help="输出 PNG 路径（默认 $SLOOP_TAB5_PROJECT/build/verify/screenshot-<时间戳>.png）")
    ap.add_argument("--baud", type=int, default=115200,
                    help="波特率（USB Serial/JTAG 下不影响实际速率，默认 115200）")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="单阶段超时秒数（帧头/帧数据/尾标记各自计时，默认 20）")
    ap.add_argument("--selftest", action="store_true",
                    help="离线自测：合成帧走完整收发/解码路径，无需设备")
    args = ap.parse_args()

    if args.selftest:
        try:
            run_selftest()
        except AssertionError as e:
            sys.exit(f"selftest FAILED: {e}")
        print("selftest OK")
        return

    if not args.port:
        ap.error("缺少串口参数 PORT（或使用 --selftest）")

    if args.out:
        out = args.out
    else:  # AGENTS.md §5.4: 截图统一存 firmware/build/verify/
        project = os.environ.get("SLOOP_TAB5_PROJECT") or str(Path(__file__).resolve().parent.parent / "firmware")
        out_dir = Path(project) / "build" / "verify"
        out_dir.mkdir(parents=True, exist_ok=True)
        out = str(out_dir / f"screenshot-{datetime.now():%Y%m%d-%H%M%S}.png")

    try:
        capture(args.port, out, args.baud, args.timeout)
    except TimeoutError as e:
        sys.exit(f"超时: {e}\n提示: 固件 console 需实现 SCR 命令（AGENTS.md §5.3）；"
                 f"确认设备已启动、串口选择正确（{args.port}）")
    except (ValueError, OSError) as e:
        sys.exit(f"失败: {e}")


# ---------------------------------------------------------------------------
# 离线自测（AGENTS.md §5.5 门禁: scripts 改动须 `--selftest` 通过）
# ---------------------------------------------------------------------------

class _FakeSerial:
    """按行/按块回放预置字节流的假串口，用于无设备环境验证 capture() 全流程。"""

    def __init__(self, payload: bytes):
        self._buf = bytearray(payload)
        self.timeout = 1.0
        self.written = bytearray()

    def write(self, b):
        self.written.extend(b)

    def flush(self):
        pass

    def reset_input_buffer(self):
        pass

    def readline(self):
        i = self._buf.find(b"\n")
        if i < 0:
            line, self._buf = bytes(self._buf), bytearray()
        else:
            line = bytes(self._buf[:i + 1])
            del self._buf[:i + 1]
        return line

    def read(self, n):
        chunk = bytes(self._buf[:n])
        del self._buf[:n]
        return chunk

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


# 4x2 rgb565: 红 绿 蓝 白 / 黑 暗黄 黄 品
_W565 = [0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x7C00, 0xFFE0, 0xF81F]
# 位扩展（<<3/<<2）后的期望 RGB
_EXP565 = [(248, 0, 0), (0, 252, 0), (0, 0, 248), (248, 252, 248),
           (0, 0, 0), (120, 128, 0), (248, 252, 0), (248, 0, 248)]


def _decode_checks() -> None:
    data = struct.pack("<8H", *_W565)
    img = rgb565_to_rgb(data, 4, 2)
    assert [img.getpixel((x, y)) for y in range(2) for x in range(4)] == _EXP565, "rgb565 解码不符"
    d3 = bytes((255, 0, 0, 0, 255, 0, 0, 0, 255))
    img3 = rgb888_to_rgb(d3, 3, 1)
    assert [img3.getpixel((i, 0)) for i in range(3)] == [(255, 0, 0), (0, 255, 0), (0, 0, 255)], "rgb888 解码不符"


def _capture_checks() -> None:
    import tempfile
    from PIL import Image

    mod = sys.modules[__name__]

    def with_fake(payload: bytes, fn) -> None:
        orig = mod._open_serial
        mod._open_serial = lambda port, baud: _FakeSerial(payload)
        try:
            fn()
        finally:
            mod._open_serial = orig

    frame = struct.pack("<8H", *_W565)

    def good_path() -> None:
        with tempfile.TemporaryDirectory() as td:
            out = str(Path(td) / "out.png")
            capture("/dev/null", out, 115200, 5.0)
            img = Image.open(out)
            assert img.size == (4, 2), f"尺寸不符: {img.size}"
            assert [img.getpixel((x, y)) for y in range(2) for x in range(4)] == _EXP565, "端到端像素不符"

    with_fake(b"boot noise line\n"                        # 噪音行应被跳过
              b"SCR 4 2 rgb565 16\n" + frame + b"SCR OK\n", good_path)

    def bad_tail() -> None:
        with tempfile.TemporaryDirectory() as td:
            try:
                capture("/dev/null", str(Path(td) / "bad.png"), 115200, 5.0)
            except ValueError:
                pass
            else:
                raise AssertionError("坏尾标记未报错")

    with_fake(b"SCR 4 2 rgb565 16\n" + frame + b"SCR BAD\n", bad_tail)


def run_selftest() -> None:
    _decode_checks()
    _capture_checks()
    print("自测断言 5 组全部通过（rgb565/rgb888 解码、噪音跳过、端到端收发、坏尾标记报错）")


if __name__ == "__main__":
    main()
