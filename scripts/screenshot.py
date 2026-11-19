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
依赖: pyserial, Pillow（numpy 可选，加速 RGB565 解码）
"""
import argparse
import importlib.util
import struct
import sys
import time
from datetime import datetime


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


def capture(port: str, out: str, baud: int, timeout: float) -> None:
    require("serial", "pyserial")
    import serial

    with serial.Serial(port, baud, timeout=1.0) as ser:
        ser.reset_input_buffer()
        ser.write(b"SCR\n")
        ser.flush()
        w, h, fmt, ln = read_header(ser, timeout)
        print(f"  帧缓冲: {w}x{h} {fmt} ({ln} 字节)")
        expected = w * h * (2 if fmt == "rgb565" else 3)
        if ln != expected:
            raise ValueError(f"长度与分辨率不符: len={ln}, 期望 {expected}")
        data = read_exact(ser, ln, timeout)
        tail = ser.readline().decode(errors="replace").strip()
        if tail != "SCR OK":
            raise ValueError(f"尾标记异常: {tail!r}（期望 'SCR OK'）")

    img = rgb565_to_rgb(data, w, h) if fmt == "rgb565" else rgb888_to_rgb(data, w, h)
    img.save(out)
    print(f"已保存: {out}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口截屏（SCR 协议）")
    ap.add_argument("port", help="串口设备，如 /dev/ttyACM0")
    ap.add_argument("out", nargs="?", default=None,
                    help="输出 PNG 路径（默认 screenshot-<时间戳>.png）")
    ap.add_argument("--baud", type=int, default=115200,
                    help="波特率（USB Serial/JTAG 下不影响实际速率，默认 115200）")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="整帧超时秒数（默认 20）")
    args = ap.parse_args()

    out = args.out or f"screenshot-{datetime.now():%Y%m%d-%H%M%S}.png"
    try:
        capture(args.port, out, args.baud, args.timeout)
    except TimeoutError as e:
        sys.exit(f"超时: {e}\n提示: 固件 console 需实现 SCR 命令（AGENTS.md §5.3）；"
                 f"确认设备已启动、串口选择正确（{args.port}）")
    except (ValueError, OSError) as e:
        sys.exit(f"失败: {e}")


if __name__ == "__main__":
    main()
