#!/usr/bin/env python3
"""screenshot.py — 经串口从 Tab5 固件截取帧缓冲并保存 PNG（AGENTS.md §5.3 协议）

协议:
    PC  -> "SCR\\n"
    FW  -> "SCR <width> <height> <fmt> <len>\\n"   (fmt: rgb565 | rgb888)
    FW  -> <len 字节裸二进制帧缓冲>                (rgb565 为小端)
    FW  -> "SCR OK\\n"

固件失败时改回 "SCR ERR <原因>\\n"，脚本立即报错退出（不等超时）。
固件未注册 SCR 命令时会超时——按传输失败处理（复位重试后仍失败则报错并提示检查 SCR）。

传输失败（帧数据超时/短帧/坏尾/长度与分辨率不符）时（重试硬化，原 DI-5）：经 esptool
硬复位设备（`--after hard_reset`）→ 等 console 提示符 → 重试，默认重试 3 次（共 4 次尝试）；
超限非零退出并把已收字节存为 `<out>-fail.bin`（无帧数据时存 `<out>-fail.txt` 记录错误）留证。
`--retry 0` 退回单次尝试的旧行为。
纯 pyserial 翻转 RTS 对 USB-Serial/JTAG 无效（实测），故复位必须走 esptool；
若 esptool 不在 PATH，可用 `SLOOP_ESPTOOL=/path/to/esptool` 指定。

用法:
    python3 scripts/screenshot.py PORT [OUT.png] [--timeout 30] [--baud 115200]
                                    [--retry 3] [--check X,Y,R,G,B[,TOL]] ...
    python3 scripts/screenshot.py --selftest          # 离线自测（无需设备）
--timeout 默认 30s（单阶段）：实测整帧 1.8MB 传输 16.4–17.8s，20s 只有约 12% 余量，
易把正常但偏慢的传输误判为停滞。
--check 像素抽检: 校验截屏 (X,Y) 处像素 ≈ (R,G,B)（各通道差 ≤ TOL，默认 16，容忍
RGB565 位扩展误差）；可多次给出，全部通过退出码 0，任一失败打印实测/期望并退出码 1。
抽检锚点见 firmware/main/display.c 文件头的 bring-up 测试图布局。
默认输出: $SLOOP_TAB5_PROJECT/build/verify/screenshot-<时间戳>.png（AGENTS.md §5.4 约定路径）
依赖: pyserial, Pillow（numpy 可选，加速 RGB565 解码）
"""
import argparse
import importlib.util
import os
import shlex
import shutil
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

DEFAULT_RETRIES = 3          # 传输失败后的复位重试次数（--retry N 覆盖）
BOOT_TIMEOUT = 15.0          # 复位后等 console 提示符的上限（实测启动约 4.5s）
CONSOLE_PROMPT = b"sloop>"


class FirmwareError(ValueError):
    """固件侧明确报错（SCR ERR、响应头格式/数值非法）——重试无意义，直接失败。"""


class TransferError(TimeoutError):
    """传输层失败（帧头/帧数据/尾标记超时、短帧、坏尾、长度与分辨率不符）——复位后重试可恢复。"""

    def __init__(self, msg: str, partial: bytes = b""):
        super().__init__(msg)
        self.partial = partial


class ResetError(RuntimeError):
    """esptool 复位失败（未安装/端口被占用/进入 bootloader 超时）。"""


class CaptureError(RuntimeError):
    """重试耗尽后的最终失败；携带失败证据路径与尝试次数。"""

    def __init__(self, msg: str, attempts: int, fail_path: Path | None):
        super().__init__(msg)
        self.attempts = attempts
        self.fail_path = fail_path


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
            raise TransferError(f"读取帧数据超时：已收 {len(buf)}/{n} 字节", bytes(buf))
        ser.timeout = min(1.0, remaining)
        try:
            chunk = ser.read(n - len(buf))
        except OSError as e:  # 设备重新枚举/端口掉线
            raise TransferError(f"串口读取出错: {e}", bytes(buf)) from None
        if chunk:
            buf.extend(chunk)
    return bytes(buf)


def read_header(ser, timeout: float):
    """读行直到 SCR 头；返回 (w, h, fmt, ln)。跳过启动噪音行。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            line = ser.readline()
        except OSError as e:
            raise TransferError(f"串口读取出错: {e}") from None
        if not line:
            continue
        text = line.decode(errors="replace").strip()
        if text.startswith("SCR ERR"):
            raise FirmwareError(f"固件返回 {text}（截屏失败，原因见固件侧日志）")
        if text.startswith("SCR ") and len(text.split()) == 5:
            _, w, h, fmt, ln = text.split()
            if fmt not in ("rgb565", "rgb888"):
                raise FirmwareError(f"不支持的像素格式: {fmt}")
            try:
                return int(w), int(h), fmt, int(ln)
            except ValueError:
                raise FirmwareError(f"响应头数值非法: {text!r}") from None
        if text:  # 透传固件侧其他输出，便于诊断
            print(f"  [fw] {text}", file=sys.stderr)
    raise TransferError("等待 SCR 响应头超时")


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


def read_frame(ser, timeout: float, on_binary=None):
    """在已打开的串口上完成一次 SCR 收发（不写命令）；返回 (w, h, fmt, data)。

    on_binary(bool) 在帧体二进制收发前后各调用一次（inject 用它静音日志）。
    """
    w, h, fmt, ln = read_header(ser, timeout)
    print(f"  帧缓冲: {w}x{h} {fmt} ({ln} 字节)")
    expected = w * h * (2 if fmt == "rgb565" else 3)
    if ln != expected:
        raise TransferError(f"长度与分辨率不符: len={ln}, 期望 {expected}")
    if on_binary:
        on_binary(True)
    try:
        data = read_exact(ser, ln, timeout)
    finally:
        if on_binary:
            on_binary(False)
    ser.timeout = 1.0  # read_exact 可能将其压到接近 0，尾标记需重新计时
    try:
        tail = ser.readline().decode(errors="replace").strip()
    except OSError as e:
        raise TransferError(f"串口读取出错: {e}", data) from None
    if tail != "SCR OK":
        raise TransferError(f"尾标记异常: {tail!r}（期望 'SCR OK'）", data)
    return w, h, fmt, data


def capture(port: str, out: str, baud: int, timeout: float) -> "Image.Image":
    """单次尝试（不复位重试）；传输失败抛 TransferError。"""
    with _open_serial(port, baud) as ser:
        ser.reset_input_buffer()
        ser.write(b"SCR\n")
        ser.flush()
        w, h, fmt, data = read_frame(ser, timeout)

    img = rgb565_to_rgb(data, w, h) if fmt == "rgb565" else rgb888_to_rgb(data, w, h)
    img.save(out)
    print(f"已保存: {out}")
    return img


def find_esptool() -> list[str] | None:
    """定位 esptool 可执行文件：$SLOOP_ESPTOOL > PATH > IDF python env。"""
    override = os.environ.get("SLOOP_ESPTOOL")
    if override:
        return shlex.split(override)
    for name in ("esptool", "esptool.py"):
        exe = shutil.which(name)
        if exe:
            return [exe]
    idf_env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if idf_env:
        for name in ("esptool", "esptool.py"):
            cand = Path(idf_env) / "bin" / name
            if cand.exists():
                return [str(cand)]
    envs = Path.home() / ".espressif" / "python_env"
    for pattern in ("idf5.5*/bin/esptool", "idf5.5*/bin/esptool.py",
                    "*/bin/esptool", "*/bin/esptool.py"):
        for cand in sorted(envs.glob(pattern)):
            if os.access(cand, os.X_OK):
                return [str(cand)]
    return None


def wait_boot(port: str, baud: int, timeout: float = BOOT_TIMEOUT, on_chunk=None) -> float:
    """等待 console 提示符（sloop>）；返回等待秒数，超时抛 ResetError。

    复位后端口可能短暂重新枚举：打开/读写遇到 OSError 时回到打开循环重试，
    直到 timeout 截止。`on_chunk(bytes)`（可选）收到启动期间的每一段串口原始字节
    （inject.py 用它把 boot 日志写进 transcript 并取 `App version:`）。
    """
    require("serial", "pyserial")
    import serial

    started = time.monotonic()
    deadline = started + timeout
    buf = bytearray()
    while time.monotonic() < deadline:
        try:
            ser = serial.Serial(port, baud, timeout=0.05)
        except OSError:
            time.sleep(0.2)
            continue
        try:
            while time.monotonic() < deadline:
                ser.write(b"\n")
                ser.flush()
                chunk = ser.read(256)
                if chunk:
                    buf.extend(chunk)
                    if on_chunk is not None:
                        on_chunk(chunk)
                i = buf.rfind(CONSOLE_PROMPT)
                if i >= 0 and buf[i + len(CONSOLE_PROMPT):].strip() == b"":
                    return time.monotonic() - started
        except OSError:  # 重新枚举/端口掉线：关掉重开
            time.sleep(0.2)
        finally:
            ser.close()
    raise ResetError(f"复位后 {timeout:.0f}s 内未等到 console 提示符（已收 {len(buf)} 字节）")


def esptool_reset(port: str, baud: int) -> None:
    """只做 esptool 硬复位（`flash_id` + `--after hard_reset`，只读不写 flash），
    **不等 console 提示符**。

    给「复位后必须立刻发命令」的场景用（如 P4-B 的热复位早探：console 在 3.0s 就接受
    命令、存储 init 在 4.1s 才跑完；`wait_boot` 要求提示符在缓冲区尾部，会被 boot 日志
    插行一直推迟到 init 之后，天然错过那个窗口）。
    """
    cmd = find_esptool()
    if cmd is None:
        raise ResetError("未找到 esptool，无法复位设备重试"
                         "（安装 ESP-IDF，或用 SLOOP_ESPTOOL 指定可执行文件）")
    argv = cmd + ["--chip", "esp32p4", "-p", port, "-b", "460800",
                  "--after", "hard_reset", "flash_id"]
    print(f"  复位设备: {' '.join(argv)}")
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        raise ResetError("esptool 复位超时（>60s）") from None
    except OSError as e:
        raise ResetError(f"esptool 无法执行: {e}") from None
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
        raise ResetError(f"esptool 复位失败（rc={proc.returncode}）: {' / '.join(tail)}")


def reset_device(port: str, baud: int, on_chunk=None) -> None:
    """esptool 硬复位设备并等 console 就绪（DI-5 恢复路径）。

    纯 pyserial 翻转 RTS 对 USB-Serial/JTAG 不生效（实测），须让 esptool 走
    bootloader 接入（默认 --before default_reset 即 USB-JTAG 复位序列），
    `--after hard_reset` 再复位回应用。`flash_id` 只读，杜绝写 flash。
    """
    esptool_reset(port, baud)
    elapsed = wait_boot(port, baud, on_chunk=on_chunk)
    print(f"  设备已重启，console 就绪（{elapsed:.1f}s）")


def save_fail_evidence(out: str, partial: bytes, error, attempts: int) -> Path:
    """把失败证据落到 out 同目录并返回路径。

    有已收字节 → `<stem>-fail.bin`（原始字节，便于分析停滞位置）；
    一字节未收（如帧头超时）→ `<stem>-fail.txt`（记录最终错误与尝试次数），
    保证任何耗尽路径都留有一份可引用的证据。
    """
    stem = Path(out).stem
    if partial:
        path = Path(out).with_name(stem + "-fail.bin")
        path.write_bytes(partial)
        detail = f"已收 {len(partial)} 字节"
    else:
        path = Path(out).with_name(stem + "-fail.txt")
        path.write_text(f"{attempts} 次尝试均失败：{error}\n", encoding="utf-8")
        detail = "无帧数据，记录错误"
    print(f"  失败证据: {path}（{detail}）")
    return path


def best_effort_reset(port: str, baud: int) -> None:
    """尽力复位一次（失败只打印）：让批量任务从可用状态继续，不掩盖主错误。"""
    print("  尝试复位设备以恢复状态（best effort）")
    try:
        reset_device(port, baud)
    except ResetError as e:
        print(f"  复位失败（已忽略）: {e}")


def capture_with_retry(port: str, out: str, baud: int, timeout: float,
                       retries: int = DEFAULT_RETRIES):
    """截屏；传输失败时 esptool 复位设备后重试，最多 `retries` 次。

    返回 (img, 重试次数)。重试耗尽抛 CaptureError（附失败证据路径）。
    固件明确报错（FirmwareError）不重试。
    """
    attempt = 0
    while True:
        attempt += 1
        try:
            img = capture(port, out, baud, timeout)
        except TransferError as e:
            if attempt > retries:
                fail_path = save_fail_evidence(out, e.partial, e, attempt)
                best_effort_reset(port, baud)
                raise CaptureError(
                    f"截屏失败（共 {attempt} 次尝试，重试上限 {retries}）: {e}\n"
                    f"提示: 传输停滞通常经复位即恢复；连续失败请查固件 SCR 实现与串口"
                    f"（{port}）是否为设备 console 口",
                    attempt, fail_path) from e
            print(f"  [retry {attempt}/{retries}] 传输失败: {e} → 复位设备后重试")
            reset_device(port, baud)
            continue
        if attempt > 1:
            print(f"  重试 {attempt - 1} 次后成功")
        return img, attempt - 1


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口截屏（SCR 协议）")
    ap.add_argument("port", nargs="?", default=None,
                    help="串口设备，如 /dev/ttyACM0（--selftest 时省略）")
    ap.add_argument("out", nargs="?", default=None,
                    help="输出 PNG 路径（默认 $SLOOP_TAB5_PROJECT/build/verify/screenshot-<时间戳>.png）")
    ap.add_argument("--baud", type=int, default=115200,
                    help="波特率（USB Serial/JTAG 下不影响实际速率，默认 115200）")
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="单阶段超时秒数（帧头/帧数据/尾标记各自计时，默认 30："
                         "实测单帧 1.8MB 传输 16.4–17.8s，20s 仅约 12%% 余量）")
    ap.add_argument("--retry", type=int, default=DEFAULT_RETRIES, metavar="N",
                    help="传输失败（超时/短帧/坏尾）时经 esptool 复位设备后重试的次数，默认 "
                         f"{DEFAULT_RETRIES}；0 = 不重试（单次尝试，旧行为）")
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
    if args.retry < 0:
        ap.error("--retry 不能为负")

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
        img, used = capture_with_retry(args.port, out, args.baud, args.timeout, args.retry)
    except CaptureError as e:
        sys.exit(str(e))
    except ResetError as e:
        sys.exit(f"复位失败: {e}")
    except FirmwareError as e:
        sys.exit(f"失败: {e}")
    except OSError as e:
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
    import contextlib
    import io
    import tempfile

    from PIL import Image

    mod = sys.modules[__name__]

    def with_fakes(payloads, fn):
        """按尝试顺序回放假串口；复位动作记录为 (种类, port)，种类 ∈ retry/final。"""
        seq = list(payloads)
        resets = []
        orig_open, orig_reset, orig_final = (mod._open_serial, mod.reset_device,
                                             mod.best_effort_reset)
        mod._open_serial = lambda port, baud: _FakeSerial(seq.pop(0) if len(seq) > 1 else seq[0])
        mod.reset_device = lambda port, baud: resets.append(("retry", port))
        mod.best_effort_reset = lambda port, baud: resets.append(("final", port))
        try:
            fn(resets)
        finally:
            mod._open_serial, mod.reset_device, mod.best_effort_reset = (
                orig_open, orig_reset, orig_final)

    frame = struct.pack("<8H", *_W565)
    good = b"SCR 4 2 rgb565 16\n" + frame + b"SCR OK\n"

    def good_path() -> None:
        with tempfile.TemporaryDirectory() as td:
            out = str(Path(td) / "out.png")
            img, used = capture_with_retry("/dev/null", out, 115200, 5.0, 3)
            assert used == 0, f"正常路径不应重试（used={used}）"
            img = Image.open(out)
            assert img.size == (4, 2), f"尺寸不符: {img.size}"
            assert [img.getpixel((x, y)) for y in range(2) for x in range(4)] == _EXP565, "端到端像素不符"

    # 噪音行应被跳过
    with_fakes([b"boot noise line\n" + good], lambda resets: (good_path(), _assert(not resets, "正常路径触发了复位")))

    def bad_tail() -> None:
        try:
            capture("/dev/null", "/dev/null", 115200, 5.0)
        except TransferError as e:
            _assert(e.partial == frame, f"坏尾标记应带完整帧体作证据（收到 {len(e.partial)} 字节）")
            _assert("尾标记异常" in str(e), f"错误信息应说明尾标记异常: {e}")
        else:
            raise AssertionError("坏尾标记未报错（应抛 TransferError）")

    with_fakes([b"SCR 4 2 rgb565 16\n" + frame + b"SCR BAD\n"], lambda resets: bad_tail())

    def scr_err_fast_fail() -> None:
        try:
            capture("/dev/null", "/dev/null", 115200, 3.0)
        except FirmwareError as e:
            assert "SCR ERR" in str(e) and "lvgl_lock_timeout" in str(e), \
                f"错误信息应含 SCR ERR 与原因: {e}"
        else:
            raise AssertionError("固件回 SCR ERR 时应快速失败（抛 FirmwareError），而非等到超时")

    with_fakes([b"SCR ERR lvgl_lock_timeout\n"], lambda resets: scr_err_fast_fail())

    def scr_err_no_retry() -> None:
        try:
            capture_with_retry("/dev/null", "/dev/null", 115200, 3.0, 3)
        except FirmwareError:
            pass
        else:
            raise AssertionError("SCR ERR 不应走复位重试")

    with_fakes([b"SCR ERR display_panel_not_ready\n"],
               lambda resets: (scr_err_no_retry(), _assert(not resets, "SCR ERR 触发了复位重试")))

    # 首轮短帧（无尾标记）→ 复位 → 次轮成功：重试路径必须可见且结果正确
    short = b"SCR 4 2 rgb565 16\n" + frame[:8]

    def retry_recovers(resets) -> None:
        with tempfile.TemporaryDirectory() as td:
            out = str(Path(td) / "retry.png")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                img, used = capture_with_retry("/dev/null", out, 115200, 0.2, 3)
            log = buf.getvalue()
            _assert(used == 1, f"应记录 1 次重试（used={used}）")
            _assert(resets == [("retry", "/dev/null")], f"应只做 1 次重试复位（实际 {resets}）")
            _assert("[retry 1/3]" in log, f"重试日志缺失: {log!r}")
            _assert([img.getpixel((x, y)) for y in range(2) for x in range(4)] == _EXP565,
                    "重试成功后的像素不符")

    with_fakes([short, good], retry_recovers)

    # 全部失败（重试 1 次）→ CaptureError + 失败证据文件 + 重试次数可读
    def retry_exhausted(resets) -> None:
        with tempfile.TemporaryDirectory() as td:
            out = str(Path(td) / "dead.png")
            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    capture_with_retry("/dev/null", out, 115200, 0.2, 1)
            except CaptureError as e:
                _assert(e.attempts == 2, f"应记录 2 次尝试（实际 {e.attempts}）")
                _assert(Path(e.fail_path).exists(), "失败证据文件未落盘")
                _assert(Path(e.fail_path).read_bytes() == frame[:8], "失败证据内容应为已收字节")
                _assert("2 次尝试" in str(e), f"错误信息应含尝试次数: {e}")
            else:
                raise AssertionError("重试耗尽后应抛 CaptureError")
            _assert([k for k, _ in resets] == ["retry", "final"],
                    f"复位序列应为 retry→final（实际 {resets}）")
            _assert("[retry 1/1]" in buf.getvalue(), "重试日志缺失")

    with_fakes([short], retry_exhausted)

    # --retry 0：单次尝试（旧行为），失败即止、不复位
    def no_retry(resets) -> None:
        with tempfile.TemporaryDirectory() as td:
            try:
                capture_with_retry("/dev/null", str(Path(td) / "x.png"), 115200, 0.2, 0)
            except CaptureError as e:
                _assert(e.attempts == 1, f"--retry 0 应只尝试 1 次（实际 {e.attempts}）")
            else:
                raise AssertionError("--retry 0 时传输失败应直接抛 CaptureError")
            _assert([k for k, _ in resets] == ["final"],
                    f"--retry 0 不应重试复位（只允许一次 best-effort 复位，实际 {resets}）")

    with_fakes([short], no_retry)

    # 响应头数值非法（SCR a b rgb565 16）：干净 FirmwareError，不重试、不打 traceback
    def malformed_header(resets) -> None:
        try:
            capture_with_retry("/dev/null", "/dev/null", 115200, 0.2, 3)
        except FirmwareError as e:
            _assert("响应头数值非法" in str(e), f"错误信息应说明响应头非法: {e}")
        else:
            raise AssertionError("响应头数值非法时应抛 FirmwareError")
        _assert(not resets, "响应头非法不应复位重试")

    with_fakes([b"SCR a b rgb565 16\n"], malformed_header)

    # 零字节耗尽（一字节未收）：仍须落失败证据 `<out>-fail.txt`（记录错误 + 尝试次数）
    def zero_byte_evidence(resets) -> None:
        with tempfile.TemporaryDirectory() as td:
            out = str(Path(td) / "void.png")
            try:
                capture_with_retry("/dev/null", out, 115200, 0.2, 0)
            except CaptureError as e:
                _assert(e.fail_path is not None, "零字节耗尽也应给出证据路径")
                p = Path(e.fail_path)
                _assert(p.suffix == ".txt", f"零字节证据应为 .txt（实际 {p.name}）")
                text = p.read_text(encoding="utf-8")
                _assert("1 次尝试均失败" in text and "响应头" in text,
                        f"证据文本应含尝试次数与错误: {text!r}")
            else:
                raise AssertionError("零字节耗尽后应抛 CaptureError")
            _assert([k for k, _ in resets] == ["final"],
                    f"零字节耗尽只应做 best-effort 复位（实际 {resets}）")

    with_fakes([b""], zero_byte_evidence)


def _cli_checks() -> None:
    """CLI 参数校验：--retry 负数必须被 argparse 拒绝（退出码 2，不产生 traceback）。"""
    import subprocess

    proc = subprocess.run([sys.executable, str(Path(__file__).resolve()), "/dev/null", "--retry", "-1"],
                          capture_output=True, text=True, timeout=30)
    _assert(proc.returncode == 2, f"--retry -1 退出码应为 2（实际 {proc.returncode}）")
    _assert("--retry 不能为负" in proc.stderr, f"stderr 应含拒绝原因: {proc.stderr!r}")


def _assert(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


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
    _cli_checks()
    print("自测断言全部通过（rgb565/rgb888 解码、噪音跳过、端到端收发、坏尾标记报错、"
          "SCR ERR 快速失败、响应头数值非法干净报错、像素抽检、首轮坏次轮好重试恢复、"
          "重试耗尽失败证据（含零字节 -fail.txt）、--retry 0 单次尝试、--retry 负数拒绝）")


if __name__ == "__main__":
    main()
