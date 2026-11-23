#!/usr/bin/env python3
"""sample_upload.py — 把 WAV 传进 Tab5 的 USR1..USR4 采样槽（console `SMP` 协议）

用法:
    python3 scripts/sample_upload.py [--port PORT] info
    python3 scripts/sample_upload.py [--port PORT] erase SLOT
    python3 scripts/sample_upload.py [--port PORT] load SLOT NAME file.wav[:ROOT[:LO-HI]] …
    python3 scripts/sample_upload.py --sine NAME OUT        # 无设备：写 OUT.hdr / OUT.bin
    python3 scripts/sample_upload.py --selftest             # 无设备：校验 header/CRC/zone

SLOT 0..3 = USR1..USR4（与 `SMP BEGIN <0..3>` 同一编号）。每个文件一个 zone：
单声道 22050 Hz IMA ADPCM（third_party/sloop-fm1/tools/sampleio.py 的 `user_slot`，
与上游 `fm1_sample_upload.py` / web 编辑器逐字节同一编码器）。ROOT 是 MIDI 音符
（缺省从文件名取，C4 = 60）；不给 LO-HI 时 zone 按根音均分键盘。

上传协议（spec §4.3 / `SMP` console 命令）:
    SMP BEGIN <slot>               擦 header 扇区并失效镜像
    SMP DATA <slot> <off> <hex>    槽内绝对偏移 ≥ 512，逐行连续；console 攒 256B 再写
    SMP HDR <hex>                  4 行 × 120B = 480B header
    SMP END <slot>                 校验 magic/version/nz/zone/CRC 后提交 header

一行 hex 的上限受 USB-Serial-JTAG 驱动 RX 环 256B 约束（整行必须一次装进环里），
故每行按 240 字节总长动态收缩：`SMP DATA <slot> <off> ` 前缀占 15B（3 位偏移）时
每块 112B，偏移到 5 位数时 111B；120B 只是单行 hex 上限，实际上不会达到。
"""
import argparse
import glob
import math
import re
import struct
import sys
import time
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "third_party" / "sloop-fm1" / "tools"))
import sampleio as sio  # noqa: E402

SMP_MAGIC = 0x504D5346  # "FSMP"（上游 eng_sample.c 的 smp_user_hdr_t.magic）
SMP_VERSION = 1
SMP_HDR_LEN = 480
SMP_DATA_OFF = 512
SMP_SLOT_SIZE = 0x14000
SMP_ZONES = 16
SMP_NSLOTS = 4
LINE_BUDGET = 240      # 一行 console 命令的字节上限（RX 环 256B 的余量）
BASELINE_BAUD = 115200
CONSOLE_PROMPT = b"sloop>"


def build_sine_slot(name, n=1000, hz=440):
    """1 zone / n 样本正弦（22050 Hz，根音 60）→ (header 480B, ADPCM data)

    走 vendored 的 user_slot：编码与键区表（key_split）与上游固件/编辑器同一实现，
    fixture 的字节因此就是设备会写进 flash 的字节。
    """
    x = [math.sin(2 * math.pi * hz * i / sio.SLOT_RATE) for i in range(n)]
    return sio.user_slot(name, [(sio.to_int16(x), 60, None, None)])


# ---------------------------------------------------------------------------
# header 校验（与固件 smp_hdr_ok / smp_zone_ok 同口径；设备端才是权威判据，
# 这里给 --selftest 与上传前的本地预检用）
# ---------------------------------------------------------------------------

def parse_header(hdr):
    """480B header -> dict(magic, version, nz, name, data_len, crc, zones)"""
    if len(hdr) != SMP_HDR_LEN:
        raise ValueError(f"header 长度 {len(hdr)} != {SMP_HDR_LEN}")
    magic, version, nz, _rsv, name, data_len, crc = struct.unpack_from("<IHBB8sII", hdr, 0)
    zones = []
    for i in range(SMP_ZONES):
        off, n, ls, le, rate, root16, pred, idx, lo, hi, _ = struct.unpack_from(
            "<5I2h4B", hdr, 32 + i * 28)
        zones.append(dict(off=off, n=n, ls=ls, le=le, rate=rate, root=root16 / 16,
                          pred=pred, idx=idx, lo=lo, hi=hi))
    return dict(magic=magic, version=version, nz=nz, name=name.rstrip(b"\0").decode(errors="replace"),
                data_len=data_len, crc=crc, zones=zones)


def zone_ok(z, data_len):
    """上游 smp_zone_ok 的同一判据（off/n/ls/le/rate/idx/lo/hi）"""
    return (z["off"] <= data_len and z["n"] <= 2 * SMP_SLOT_SIZE and
            z["off"] + (z["n"] + 1) // 2 <= data_len and z["idx"] <= 88 and
            0 < z["rate"] <= 4 << 16 and z["ls"] <= z["le"] < z["n"] and z["lo"] <= z["hi"])


def check_slot(hdr, data):
    """返回 header 事实；不合法抛 ValueError（--selftest 与 load 前的本地预检共用）"""
    h = parse_header(hdr)
    if h["magic"] != SMP_MAGIC:
        raise ValueError(f"magic 0x{h['magic']:08X} != 0x{SMP_MAGIC:08X}")
    if h["version"] != SMP_VERSION:
        raise ValueError(f"version {h['version']} != {SMP_VERSION}")
    if not 1 <= h["nz"] <= SMP_ZONES:
        raise ValueError(f"nz {h['nz']} 不在 1..{SMP_ZONES}")
    if h["data_len"] != len(data):
        raise ValueError(f"data_len {h['data_len']} != 实际 {len(data)}")
    if h["data_len"] > SMP_SLOT_SIZE - SMP_DATA_OFF:
        raise ValueError(f"data_len {h['data_len']} 超过槽容量")
    if h["crc"] != zlib.crc32(data) & 0xFFFFFFFF:
        raise ValueError("CRC 不符")
    for i in range(h["nz"]):
        if not zone_ok(h["zones"][i], h["data_len"]):
            raise ValueError(f"zone {i} 非法: {h['zones'][i]}")
    return h


# ---------------------------------------------------------------------------
# 离线自测
# ---------------------------------------------------------------------------

def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _selftest_sine():
    hdr, data = build_sine_slot("SINE")
    h = check_slot(hdr, data)
    _assert(len(hdr) == SMP_HDR_LEN, "header 不是 480B")
    _assert(len(data) == (1000 + 1) // 2, f"1000 样本应编成 500B ADPCM，得 {len(data)}")
    _assert(h["nz"] == 1 and h["name"] == "SINE", f"nz/name 不符: {h['nz']} {h['name']!r}")
    _assert(h["data_len"] == len(data), "data_len 不符")
    _assert(h["crc"] == zlib.crc32(data) & 0xFFFFFFFF, "CRC 不符")
    z = h["zones"][0]
    _assert(z["off"] == 0 and z["n"] == 1000, f"zone off/n 不符: {z['off']} {z['n']}")
    _assert(z["lo"] == 0 and z["hi"] == 127, f"单 zone 应覆盖全键盘: {z['lo']}..{z['hi']}")
    _assert(z["rate"] == round(sio.SLOT_RATE / 44100 * 65536), "rate 不是 22050 Hz 的定点值")


def _selftest_rejects():
    hdr, data = build_sine_slot("SINE")
    cases = [
        ("magic", lambda h, d: h.__setitem__(0, h[0] ^ 0xFF)),
        ("version", lambda h, d: struct.pack_into("<H", h, 4, 2)),
        ("nz", lambda h, d: h.__setitem__(6, 0)),
        ("data_len", lambda h, d: struct.pack_into("<I", h, 16, len(d) + 2)),
        ("crc", lambda h, d: struct.pack_into("<I", h, 20, 0)),
        ("zone", lambda h, d: struct.pack_into("<I", h, 32 + 4, 0)),  # zone0.off=0/n 破坏
    ]
    for name, mutate in cases:
        h, d = bytearray(hdr), bytearray(data)
        mutate(h, d)
        try:
            check_slot(bytes(h), bytes(d))
        except ValueError:
            continue
        raise AssertionError(f"坏 header（{name}）未被拒绝")
    # 数据被改一个字节而 header 不动 ⇒ CRC 必须报错
    d = bytearray(data)
    d[0] ^= 0x55
    try:
        check_slot(hdr, bytes(d))
    except ValueError as e:
        _assert("CRC" in str(e), f"数据损坏应报 CRC: {e}")
    else:
        raise AssertionError("数据损坏未被 CRC 拒绝")


def _selftest_upload():
    """无设备验证上传序列：行预算、偏移连续、4×120B header、BEGIN→DATA→HDR→END。"""
    class FakeSession:
        def __init__(self):
            self.lines = []

        def expect(self, line, prefix, timeout=10.0):
            assert line.startswith(prefix), f"回显前缀不符: {line!r} / {prefix!r}"
            self.lines.append(line)
            return line

    hdr, data = build_sine_slot("SINE")
    sess = FakeSession()
    upload(sess, 3, "SINE", hdr, data)
    for line in sess.lines:
        # DATA 行按 240B 预算切块；HDR 固定 4×120B（248 字符 + '\n' = 249 < 256 的 RX 环）
        budget = 256 if line.startswith("SMP HDR ") else LINE_BUDGET
        _assert(len(line.encode()) + 1 <= budget, f"命令行超过 {budget}B: {len(line) + 1}")
    _assert(sess.lines[0] == "SMP BEGIN 3", "首行应为 BEGIN")
    _assert(sess.lines[-1] == "SMP END 3", "末行应为 END")
    datas = [ln for ln in sess.lines if ln.startswith("SMP DATA 3 ")]
    off, blob = SMP_DATA_OFF, bytearray()
    for ln in datas:
        off_txt, hex_txt = ln.split(" ", 3)[3].split(" ", 1)
        _assert(int(off_txt) == off, f"DATA 偏移不连续: {off_txt} != {off}")
        chunk = bytes.fromhex(hex_txt)
        _assert(len(chunk) <= 112, "DATA 块超过 112B（240B 行预算的实际上限）")
        off += len(chunk)
        blob.extend(chunk)
    _assert(bytes(blob) == data, "DATA 拼回的字节与 ADPCM 不一致")
    hdrs = [ln for ln in sess.lines if ln.startswith("SMP HDR ")]
    _assert(len(hdrs) == 4 and all(len(ln) == 8 + 240 for ln in hdrs), "HDR 不是 4×120B")


def run_selftest():
    _selftest_sine()
    _selftest_rejects()
    _selftest_upload()
    print("SAMPLE SELFTEST OK")


# ---------------------------------------------------------------------------
# 设备侧（console `SMP`；串口打开/提示符等待沿用 screenshot.py/inject.py 的风格）
# ---------------------------------------------------------------------------

class UploadError(RuntimeError):
    """上传失败（命令回 ERR、回显不符或串口异常）——脚本非零退出。"""


# 固件异步日志行（esp_log）：提示符之后仍可能紧跟一行，不算「命令未完成」。
_ASYNC_LOG = re.compile(r"^[IWE] \(\d+\) \S")


def _prompt_at(buf: bytearray) -> bool:
    i = buf.rfind(CONSOLE_PROMPT)
    if i < 0:
        return False
    tail = buf[i + len(CONSOLE_PROMPT):].decode(errors="replace")
    return all(_ASYNC_LOG.match(line) for line in tail.strip().splitlines())


def find_port() -> str:
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    raise UploadError("未找到串口（/dev/ttyACM* 或 /dev/ttyUSB*）：用 --port 指定")


class Session:
    def __init__(self, port: str, baud: int = BASELINE_BAUD, timeout: float = 15.0):
        try:
            import serial
        except ImportError:
            sys.exit("缺少依赖 pyserial：pip install -r scripts/requirements.txt")
        try:
            self.ser = serial.Serial(port, baud, timeout=0.05)
        except (serial.SerialException, OSError) as e:
            raise UploadError(f"打开串口 {port} 失败: {e}") from None
        self.wait_ready(timeout)

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def _read_response(self, timeout: float) -> str:
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf.extend(chunk)
                continue
            if _prompt_at(buf):
                return buf.decode(errors="replace")
        raise UploadError(f"等待 console 提示符超时（已收 {len(buf)} 字节）")

    def wait_ready(self, timeout: float = 15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.ser.write(b"\n")
            self.ser.flush()
            try:
                self._read_response(1.0)
                return
            except UploadError:
                continue
        raise UploadError("等待 console 提示符超时（设备未启动或串口被占用？）")

    def cmd(self, line: str, timeout: float = 10.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        return self._read_response(timeout)

    def expect(self, line: str, prefix: str, timeout: float = 10.0) -> str:
        """发一行并断言回显：提示符后最后一条 `prefix` 行；`SMP ERR` 直接失败。

        console 会回显输入行，故先把与发送内容完全相同的那一行排除——否则断言可能
        落在回显上而不是设备的应答上。"""
        resp = self.cmd(line, timeout)
        body = [ln.strip() for ln in resp.replace(CONSOLE_PROMPT.decode(), "").splitlines()
                if ln.strip() and ln.strip() != line]
        for ln in body:
            if "SMP ERR" in ln:
                raise UploadError(f"{line.split()[1] if len(line.split()) > 1 else line}: {ln}")
        hits = [ln for ln in body if ln.startswith(prefix)]
        if not hits:
            raise UploadError(f"{line!r} 回显缺少 {prefix!r}（收到: {body!r}）")
        return hits[-1]


def slot_lines(resp: str):
    """`SMP INFO` 回显里的槽行（"SMP k USED|EMPTY"）；固件报 ERR 时抛错。"""
    body = [ln.strip() for ln in resp.replace(CONSOLE_PROMPT.decode(), "").splitlines() if ln.strip()]
    for ln in body:
        if "SMP ERR" in ln:
            raise UploadError(ln)
    return [ln for ln in body if re.match(r"^SMP \d+ (USED|EMPTY)", ln)]


def chunk_lines(slot: int, data: bytes):
    """按 console 行预算切块：整行（含 "SMP DATA slot off " 与 '\\n'）≤ LINE_BUDGET 字节。

    240B 预算下实际是 112B/块（前缀 15B + 224 hex），偏移到 5 位数时 111B；120 只是
    上界。超过行预算的整行会撞 USJ 驱动 RX 环 256B（设备忙时环溢出、字符被静默丢弃）。"""
    off, pos = SMP_DATA_OFF, 0
    while pos < len(data):
        head = f"SMP DATA {slot} {off} "
        n = min(120, (LINE_BUDGET - len(head) - 1) // 2)
        yield off, data[pos:pos + n]
        off += n
        pos += n


def upload(sess: Session, slot: int, name: str, hdr: bytes, data: bytes):
    check_slot(hdr, data)  # 本地预检：失败不必开始擦写
    sess.expect(f"SMP BEGIN {slot}", "SMP BEGIN", timeout=20.0)  # BEGIN 擦 header 扇区
    for off, chunk in chunk_lines(slot, data):
        sess.expect(f"SMP DATA {slot} {off} {chunk.hex().upper()}", "SMP DATA")
    for i in range(0, SMP_HDR_LEN, 120):
        sess.expect(f"SMP HDR {hdr[i:i + 120].hex().upper()}", "SMP HDR")
    sess.expect(f"SMP END {slot}", "SMP END", timeout=30.0)  # 整槽回读 + CRC + 写 header


def build(name: str, specs):
    """specs: "file.wav[:ROOT[:LO-HI]]" → (header, ADPCM data)（与上游同一切分规则）"""
    zones = []
    for spec in specs:
        parts = spec.split(":")
        # Windows 盘符（C:\…）与可选的 :ROOT:LO-HI 后缀分开
        if len(parts) > 1 and len(parts[0]) == 1 and parts[0].isalpha() and parts[1].startswith(("/", "\\")):
            parts = [parts[0] + ":" + parts[1], *parts[2:]]
        path = Path(parts[0])
        root = int(parts[1]) if len(parts) > 1 and parts[1] else sio.note_from_name(path.stem)
        lo, hi = (int(v) for v in parts[2].split("-")) if len(parts) > 2 else (None, None)
        sr, x = sio.read_any_wav(path)
        x = sio.resample(x, sr, sio.SLOT_RATE)
        if not x:
            raise UploadError(f"{path}: 没有音频")
        zones.append((sio.to_int16(x), root, lo, hi))
    return sio.user_slot(name, zones)


def slot_arg(text: str) -> int:
    m = re.fullmatch(r"\d+", text.strip())
    if not m or not 0 <= int(text) < SMP_NSLOTS:
        raise UploadError(f"SLOT 必须是 0..{SMP_NSLOTS - 1}（USR1..USR4），收到 {text!r}")
    return int(text)


def run_device(args) -> int:
    mode = args.rest[0].lower()
    try:
        if mode == "info":
            with Session(args.port or find_port()) as sess:
                lines = slot_lines(sess.cmd("SMP INFO"))
            if not lines:
                raise UploadError("SMP INFO 没有返回槽行（固件不是本分支？）")
            print("\n".join(lines))
            return 0
        if mode == "erase":
            if len(args.rest) != 2:
                raise UploadError("用法: erase SLOT（0..3）")
            slot = slot_arg(args.rest[1])
            with Session(args.port or find_port()) as sess:
                sess.expect(f"SMP ERASE {slot}", "SMP ERASE", timeout=30.0)
            print(f"USR{slot + 1} 已擦除")
            return 0
        if mode == "load":
            if len(args.rest) < 4:
                raise UploadError("用法: load SLOT NAME file.wav[:ROOT[:LO-HI]] …")
            slot, name = slot_arg(args.rest[1]), args.rest[2]
            hdr, data = build(name, args.rest[3:])
            check_slot(hdr, data)
            print(f"{name}: {len(data)}B ADPCM, {parse_header(hdr)['nz']} zone(s)")
            with Session(args.port or find_port()) as sess:
                upload(sess, slot, name, hdr, data)
                print(f"已上传 {name}: {len(data)}B ADPCM / 480B header → USR{slot + 1}")
                lines = slot_lines(sess.cmd("SMP INFO"))
            for ln in lines:
                if ln.startswith(f"SMP {slot} "):
                    print(ln)
            return 0
        raise UploadError(f"未知模式 {mode!r}（info | erase SLOT | load SLOT NAME file…）")
    except (UploadError, OSError, ValueError) as e:
        print(f"sample_upload: {e}", file=sys.stderr)
        return 1



def main(argv=None):
    ap = argparse.ArgumentParser(add_help=True, description="Tab5 采样槽上传（console SMP 协议）")
    ap.add_argument("--port", help="串口（缺省自动探测 /dev/ttyACM* 或 /dev/ttyUSB*）")
    ap.add_argument("--sine", nargs=2, metavar=("NAME", "OUT"),
                    help="无设备：生成 1 zone / 1000 样本 / 440 Hz 的 OUT.hdr + OUT.bin")
    ap.add_argument("--selftest", action="store_true", help="无设备：校验 header/CRC/zone")
    ap.add_argument("rest", nargs="*", help="info | erase SLOT | load SLOT NAME file.wav[:ROOT[:LO-HI]] …")
    args = ap.parse_args(argv)

    if args.selftest:
        run_selftest()
        return 0
    if args.sine:
        name, out = args.sine
        hdr, data = build_sine_slot(name)
        check_slot(hdr, data)
        Path(out + ".hdr").write_bytes(hdr)
        Path(out + ".bin").write_bytes(data)
        print(f"{out}: {len(data)}B ADPCM, header 480B, 1 zone")
        return 0
    if not args.rest:
        ap.error("需要 info | erase SLOT | load SLOT NAME file…")
    return run_device(args)


if __name__ == "__main__":
    sys.exit(main())
