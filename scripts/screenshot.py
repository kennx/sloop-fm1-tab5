#!/usr/bin/env python3
"""screenshot.py — 经串口从 Tab5 固件截取帧缓冲并保存 PNG（AGENTS.md §5.3 协议）

协议:
    PC  -> "SCR\\n"
    FW  -> "SCR <width> <height> <fmt> <len>\\n"   (fmt: rgb565 | rgb888)
    FW  -> <len 字节裸二进制帧缓冲>                (rgb565 为小端)
    FW  -> "SCR OK\\n"

固件失败时改回 "SCR ERR <原因>\\n"，脚本立即报错退出（不等超时）。
固件未注册 SCR 命令时会超时并给出提示。

用法:
    python3 scripts/screenshot.py PORT [OUT.png] [--timeout 20] [--baud 115200]
                                    [--check X,Y,R,G,B[,TOL]] ...
    python3 scripts/screenshot.py --selftest          # 离线自测（无需设备）
--check 像素抽检: 校验截屏 (X,Y) 处像素 ≈ (R,G,B)（各通道差 ≤ TOL，默认 16，容忍
RGB565 位扩展误差）；可多次给出，全部通过退出码 0，任一失败打印实测/期望并退出码 1。
抽检锚点见 firmware/main/display.c 文件头的 bring-up 测试图布局。
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
        if text.startswith("SCR ERR"):
            raise ValueError(f"固件返回 {text}（截屏失败，原因见固件侧日志）")
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


def check_pixel(img, x: int, y: int, r: int, g: int, b: int, tol: int = 16) -> tuple[bool, str]:
    """校验 img 在 (x,y) 的像素与期望 (r,g,b) 各通道差是否均 ≤ tol。

    返回 (是否通过, 人类可读差异)；坐标越界返回失败而非抛异常。
    tol 默认 16，容忍 rgb565 位扩展（<<3/<<2）相对目标色的通道偏差。
    """
    w, h = img.size
    if not (0 <= x < w and 0 <= y < h):
        return False, f"({x},{y}) 越界（图像 {w}x{h}）"
    pr, pg, pb = img.getpixel((x, y))
    dr, dg, db = pr - r, pg - g, pb - b
    if max(abs(dr), abs(dg), abs(db)) <= tol:
        return True, f"({x},{y}) 实测 ({pr},{pg},{pb}) ≈ 期望 ({r},{g},{b})（容差 {tol}）"
    return False, (f"({x},{y}) 实测 ({pr},{pg},{pb}) ≠ 期望 ({r},{g},{b})（容差 {tol}，"
                   f"差 R{dr:+d} G{dg:+d} B{db:+d}）")


def parse_check(spec: str) -> tuple[int, int, int, int, int, int]:
    """解析 --check 参数 X,Y,R,G,B[,TOL] → (x, y, r, g, b, tol)；非法即抛 ValueError。"""
    parts = spec.split(",")
    if len(parts) not in (5, 6):
        raise ValueError(f"格式应为 X,Y,R,G,B[,TOL]，收到 {spec!r}")
    try:
        vals = [int(p) for p in parts]
    except ValueError:
        raise ValueError(f"存在非整数字段: {spec!r}") from None
    if len(vals) == 5:
        vals.append(16)
    x, y, r, g, b, tol = vals
    if min(vals) < 0 or max(r, g, b) > 255:
        raise ValueError(f"数值超出范围（坐标非负、颜色 ≤255）: {spec!r}")
    return x, y, r, g, b, tol


def _open_serial(port: str, baud: int):
    require("serial", "pyserial")
    import serial
    return serial.Serial(port, baud, timeout=1.0)


def capture(port: str, out: str, baud: int, timeout: float) -> "Image.Image":
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
    return img


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
    ap.add_argument("--check", action="append", metavar="X,Y,R,G,B[,TOL]",
                    help="像素抽检：校验截屏 (X,Y) 处像素 ≈ (R,G,B)（各通道差 ≤ TOL，默认 16，"
                         "容忍 RGB565 位扩展误差）；可多次给出，全部通过退出码 0，任一失败退出码 1")
    args = ap.parse_args()

    if args.selftest:
        if args.check:
            ap.error("--check 需要真机截屏，不与 --selftest 同用")
        try:
            run_selftest()
        except AssertionError as e:
            sys.exit(f"selftest FAILED: {e}")
        print("selftest OK")
        return

    if not args.port:
        ap.error("缺少串口参数 PORT（或使用 --selftest）")

    checks = []
    for spec in args.check or []:
        try:
            checks.append(parse_check(spec))
        except ValueError as e:
            ap.error(f"--check 参数错误: {e}")

    if args.out:
        out = args.out
    else:  # AGENTS.md §5.4: 截图统一存 firmware/build/verify/
        project = os.environ.get("SLOOP_TAB5_PROJECT") or str(Path(__file__).resolve().parent.parent / "firmware")
        out_dir = Path(project) / "build" / "verify"
        out_dir.mkdir(parents=True, exist_ok=True)
        out = str(out_dir / f"screenshot-{datetime.now():%Y%m%d-%H%M%S}.png")

    try:
        img = capture(args.port, out, args.baud, args.timeout)
    except TimeoutError as e:
        sys.exit(f"超时: {e}\n提示: 固件 console 需实现 SCR 命令（AGENTS.md §5.3）；"
                 f"确认设备已启动、串口选择正确（{args.port}）")
    except (ValueError, OSError) as e:
        sys.exit(f"失败: {e}")

    if checks:
        n_fail = 0
        for x, y, r, g, b, tol in checks:
            ok, msg = check_pixel(img, x, y, r, g, b, tol)
            print(f"  [{'通过' if ok else '失败'}] {msg}")
            n_fail += 0 if ok else 1
        if n_fail:
            sys.exit(f"像素抽检未通过: {n_fail}/{len(checks)} 项失败")
        print(f"像素抽检 {len(checks)}/{len(checks)} 项通过")


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

    def scr_err_fast_fail() -> None:
        with tempfile.TemporaryDirectory() as td:
            try:
                capture("/dev/null", str(Path(td) / "err.png"), 115200, 3.0)
            except ValueError as e:
                assert "SCR ERR" in str(e) and "lvgl_lock_timeout" in str(e), \
                    f"错误信息应含 SCR ERR 与原因: {e}"
            else:
                raise AssertionError("固件回 SCR ERR 时应快速失败（抛 ValueError），而非等到超时")

    with_fake(b"SCR ERR lvgl_lock_timeout\n", scr_err_fast_fail)


def _check_pixel_checks() -> None:
    img = rgb565_to_rgb(struct.pack("<8H", *_W565), 4, 2)

    ok, msg = check_pixel(img, 0, 0, 248, 0, 0, 16)
    assert ok, f"(0,0) 红色命中应通过: {msg}"

    ok, msg = check_pixel(img, 1, 0, 10, 200, 10, 16)
    assert not ok, "(1,0) 绿色 vs 期望 (10,200,10) 超容差应失败"
    assert "10" in msg, f"差异文本应含期望分量 10: {msg}"

    ok, msg = check_pixel(img, 9, 9, 0, 0, 0, 16)
    assert not ok, "越界坐标应返回失败而非异常"


def run_selftest() -> None:
    _decode_checks()
    _capture_checks()
    _check_pixel_checks()
    print("自测断言 7 组全部通过（rgb565/rgb888 解码、噪音跳过、端到端收发、坏尾标记报错、SCR ERR 快速失败、像素抽检）")


if __name__ == "__main__":
    main()
