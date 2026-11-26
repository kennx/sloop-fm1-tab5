#!/usr/bin/env python3
"""5B-1 Task 3 修复轮 1：上下文键变化时每次重解析失效多少张卡片（证据脚本，随证据包入库）。

Usage:
    python3 task-3-fix1-measure.py PORT TRANSCRIPT.log

依赖固件里一行**临时**日志 `TEMP-FIX1 重解析 失效卡片=N`（测量后即从源码删除，不进提交）。
流程：硬复位 → `STORE AUTO OFF` → Ctrl+1 → `UI sound` → 对两个槽各做一轮注入拖动：
Engine edit 卡首槽（`P_E0`，引擎槽值在上下文键里）与 FX 卡的 ITYPE 槽（`P_ITYPE` 在键里、且会换掉
INSERT A/B/C 的标签）。每条 `UI SOUND KNOB … drag` 之后读固件日志里的失效卡片数、再用
`UI SOUND CARD n SLOT m` 读回值，核对与 KNOB 回显一致、且标签在 ITYPE 换档时跟着换。
拖回锚点还原初值；收尾硬复位。
"""
import re
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts"))
import screenshot as shot  # noqa: E402

BAUD = 115200
NOTE = "# [measure]"
RE_INV = re.compile(r"TEMP-FIX1 重解析 失效卡片=(\d+)")
RE_KNOB = re.compile(r"UI SOUND KNOB flat=(\d+) (press|drag) dx=(-?\d+) dy=(-?\d+) (?:value=(-?\d+)|ignored \((\w+(?: \w+)?)\))")
RE_SLOT = re.compile(r'UI SOUND SLOT card=(\d+) slot=(\d+) flat=(\d+) present=(\d) scope=(\S+) id=(\S+) label="([^"]*)" '
                     r'locked=(\d) value=(-?\d+)')
# 半径 40 的环上取 8 个点（顺时针，自顶部起；每步 45° = 值域的 1/6）
RING = [(0, -40), (28, -28), (40, 0), (28, 28), (0, 40), (-28, 28), (-40, 0), (-28, -28)]
TARGETS = [("Engine edit WAVE (P_E0)", 4, 0, None), ("FX ITYPE (P_ITYPE)", 6, 17, (6, 18))]


class Console:
    def __init__(self, port, log_path):
        self.log = open(log_path, "ab")
        self.ser = serial.Serial(port, BAUD, timeout=0.05)

    def note(self, text):
        line = f"{NOTE} {text}"
        print(line, flush=True)
        self.log.write((line + "\n").encode())
        self.log.flush()

    def cmd(self, line, settle=0.15, timeout=8.0):
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf.extend(chunk)
                self.log.write(chunk)
                self.log.flush()
                continue
            if buf.rfind(b"sloop>") >= 0:
                break
        time.sleep(settle)  # 让 30ms UI 拍（重解析）落在下一个读之前
        self.log.write(f"{NOTE} ----\n".encode())
        return buf.decode(errors="replace")


def select_track(c, k):
    for code, ev in ((0x40, "down"), (k + 1, "down"), (k + 1, "up"), (0x40, "up")):
        c.cmd(f"KEY 0x{code:02X} {ev}")


def slot(c, card, s):
    return RE_SLOT.search(c.cmd(f"UI SOUND CARD {card} SLOT {s}"))


def main():
    port, log_path = sys.argv[1], Path(sys.argv[2])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    c = Console(port, log_path)
    try:
        c.ser.close()
        shot.reset_device(port, BAUD, on_chunk=c.log.write)
        c.ser = serial.Serial(port, BAUD, timeout=0.05)
        c.cmd("", settle=0.5)
        c.cmd("STORE AUTO OFF")
        select_track(c, 0)
        c.cmd("UI sound")
        for name, card, s, labels in TARGETS:
            m = slot(c, card, s)
            flat, v0 = int(m.group(3)), int(m.group(9))
            c.note(f"{name}: flat={flat} 初值 value={v0} label={m.group(7)}")
            c.cmd(f"UI SOUND KNOB {flat} press 0 -40")
            counts, ok = [], True
            for dx, dy in RING:
                text = c.cmd(f"UI SOUND KNOB {flat} drag {dx} {dy}")
                k = RE_KNOB.search(text)
                inv = [int(x) for x in RE_INV.findall(text)]
                counts.append(sum(inv) if inv else 0)
                after = slot(c, card, s)
                label = slot(c, *labels) if labels else None
                same = k is not None and k.group(5) is not None and after.group(9) == k.group(5)
                ok = ok and same
                c.note(f"drag {dx:>3} {dy:>3} ⇒ KNOB value={k.group(5) if k else None} 失效卡片={inv} "
                       f"读回 value={after.group(9)}{' 一致' if same else ' 不一致'}"
                       + (f" | INSERT A label={label.group(7)}" if label else ""))
            c.note(f"{name}: 8 次移动的失效卡片数 = {counts}（每次重解析一张卡片计一次；0 = 该次移动没触发重解析）")
            c.note(f"{name}: 每次移动后读回值与 KNOB 回显一致 = {ok}")
            text = c.cmd(f"UI SOUND KNOB {flat} drag 0 -40")
            k = RE_KNOB.search(text)
            c.note(f"{name}: 拖回锚点 ⇒ value={k.group(5) if k else None}（初值 {v0}）")
        c.note("收尾：硬复位重新载入 autosave 对象")
        c.ser.close()
        shot.reset_device(port, BAUD, on_chunk=c.log.write)
    finally:
        c.log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
