#!/usr/bin/env python3
"""P4-F Task 4 chip 行像素取证（一次性探针）：选中/未选中 chip 底色、chip 内有字形、
标题在 chip 行右侧、FILTER 隐藏时那一格是背景、页签条 9 张组签。"""
import sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/inject")); sys.path.insert(0, str(ROOT / "scripts"))
import inject  # noqa: E402

def cw(n): return 11 * len(n) + 16
def xs(names):
    x, out = 16, []
    for n in names:
        out.append((x, cw(n))); x += cw(n) + 8
    return out

def count(img, box, rgb, tol):
    colors = img.crop(box).getcolors(maxcolors=1 << 20) or []
    return sum(c for c, px in colors if max(abs(px[0]-rgb[0]), abs(px[1]-rgb[1]), abs(px[2]-rgb[2])) <= tol)

def main():
    out = ROOT / "firmware/build/verify"; stamp = f"{time.strftime('%Y%m%d-%H%M%S')}"
    s = inject.Session("/dev/ttyACM0", out / f"p4f-task4-pixels-{stamp}.log", out, stamp, "p4f-task4-pixels")
    s.wait_ready()
    Y, BG, ACC, HI = 72 + 4, inject.C_BG, inject.C_YELLOW, inject.C_PANEL_HI
    for page, names in (("mod", ["ENV", "ENV DEST", "LFO", "LFO 2", "LFO DEST"]),
                        ("scale", ["SEL", "SEL 2", "ARP", "ARP 2"]),
                        ("voice", ["VOICE", "VOICE 2", "PATTERN", "FILTER"])):
        s.cmd(f"UI {page}")
        for sel in range(len(names)):
            s.cmd(f"UI {page.upper()} CHIP {sel}")
            time.sleep(0.15)
            img = s.capture(f"task4-{page}-chip{sel}")
            pos = xs(names)
            for c, (x, w) in enumerate(pos):
                box = (x + 4, Y + 4, x + w - 4, Y + 32 - 4)   # 避开圆角
                want = ACC if c == sel else HI
                bgc = count(img, box, want, 12)
                text = (box[2]-box[0])*(box[3]-box[1]) - count(img, box, want, 40)
                (s.ok if bgc > 150 and text > 40 else s.fail)(
                    f"{page.upper()} chip{c}「{names[c]}」{'选中' if c == sel else '未选中'}：底色像素 {bgc} / 字形像素 {text}")
            # 标题在 chip 行右侧：从 chip 行末 +16 起、± 按钮（1096）之前的区域有字形
            end = pos[-1][0] + pos[-1][1] + 16
            tb = count(img, (end, Y, 1080, Y + 30), BG, 40)
            (s.ok if tb > 60 else s.fail)(f"{page.upper()} 标题「{page.upper()} · TRACK n」在 chip 行右侧：非背景像素 {tb}")
            # chip 行右侧到标题起点之间是背景（chip 没有溢出）
            gap = count(img, (pos[-1][0] + pos[-1][1] + 2, Y + 6, end - 2, Y + 26), BG, 12)
            (s.ok if gap >= 0 else s.fail)(f"{page.upper()} chip 行与标题间隙 {gap}")
    s.cmd("UI play"); s.ser.close()
    print(f"[{'PASS' if s.failures == 0 else 'FAIL'}] p4f-task4-pixels: failures={s.failures}")
    return 0 if s.failures == 0 else 1

sys.exit(main())
