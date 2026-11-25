"""硬复位设备并把启动期间的串口原始字节写到文件（screenshot.py 的 reset_device + on_chunk）。"""
import importlib.util
import sys
import time

spec = importlib.util.spec_from_file_location("sc", "scripts/screenshot.py")
sc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sc)
port, out = sys.argv[1], sys.argv[2]
chunks = []
t0 = time.monotonic()


def on_chunk(b):
    chunks.append(b)
    if time.monotonic() - t0 > 40:
        raise RuntimeError("boot log capture exceeded 40s")


sc.reset_device(port, 115200, on_chunk=on_chunk)
time.sleep(3)
import serial
with serial.Serial(port, 115200, timeout=0.2) as s:
    end = time.monotonic() + 2
    while time.monotonic() < end:
        d = s.read(4096)
        if d:
            chunks.append(d)
open(out, "wb").write(b"".join(chunks))
print(f"boot log {len(b''.join(chunks))} bytes -> {out}")
