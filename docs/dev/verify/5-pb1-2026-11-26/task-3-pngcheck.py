#!/usr/bin/env python3
"""5B-1 Task 3：Sound 仪表盘截屏的像素核对（证据脚本，随证据包入库）。

Usage:
    python3 task-3-pngcheck.py SCREEN.png [--scroll Y]

Task 2 的 task-2-proto-pngcheck.py 核对的是骨架（环 / 名称 / `--` 占位）；本脚本核对 Task 3 新画
出来的东西。本会话没有图像查看能力，用像素核对代替「读图」。按 sound_layout_place 的同一规则
复算 13 张卡片与槽的位置（--scroll Y = 截屏前的 `UI SOUND SCROLL Y`），对每个**完整露出**的槽：
  - 环：圆心上方 44px（270°）与左方 44px（180°）至少一处是环色（面板亮 = 底环 / 次级文本色 =
    值弧），圆心下方 44px（90°，扫角的缺口）是卡片面板色；
  - 值：弧内墨迹（既非背景也非面板色的像素）不少于 Task 2 占位 `--` 的 340，且至少 10 个槽的
    墨迹数不等于 340（值文本已替换占位）；
  - 值弧：值 > 0 的槽在 135°（扫角起点）是次级文本色，值 = 0 的槽（Global·SWING）是面板亮色。
颜色取 MONO 主题（theme_derive.h）：背景 101418 / 面板 182020 / 面板亮 202830 / 次级文本 889498。
末行 `PNG CHECK PASS`（退出码 0）或 `PNG CHECK FAIL`（退出码 1）。**人眼读图仍是外部门禁**。
"""
import sys

from PIL import Image

SLOTS = [4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7]  # k_sound_cards[].slot_count
BG, PANEL, PANEL_HI, TEXT_DIM = (0x10, 0x14, 0x18), (0x18, 0x20, 0x20), (0x20, 0x28, 0x30), (0x88, 0x94, 0x98)
VIEW_X, VIEW_BOTTOM, VIEW_TOP = 16, 644, 80
TITLE_H, CELL_W, CELL_H, COL_W, COL_GAP, PAD = 36, 140, 132, 616, 16, 12
RING = {PANEL_HI, TEXT_DIM}
PLACEHOLDER_INK = 340  # task-2-proto-sound-*.png 里每个槽的 `--` 墨迹

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print(("PASS " if cond else "FAIL ") + name, detail)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    scroll = int(sys.argv[sys.argv.index("--scroll") + 1]) if "--scroll" in sys.argv else 0
    im = Image.open(args[0]).convert("RGB")
    W, H = im.size
    px = im.load()
    top, bottom = 72 + VIEW_TOP, 72 + VIEW_BOTTOM  # 视口在屏幕上的 y 范围

    def ink(x1, y1, x2, y2):  # 既非背景也非面板色的像素 = 文字/弧线/抗锯齿
        return sum(1 for yy in range(max(0, y1), min(H, y2))
                   for xx in range(max(0, x1), min(W, x2)) if px[xx, yy] not in (BG, PANEL))

    bottoms = [0, 0]
    cards = []
    for n in SLOTS:
        col = 1 if bottoms[1] < bottoms[0] else 0
        h = TITLE_H + ((n + 3) // 4) * CELL_H + PAD
        cards.append((col * (COL_W + COL_GAP), bottoms[col]))
        bottoms[col] += h + 8

    full, changed, bad = 0, 0, []
    for i, (x, y) in enumerate(cards):
        for s in range(SLOTS[i]):
            cx = VIEW_X + x + PAD + (s % 4) * CELL_W + CELL_W // 2
            cy = top + y + TITLE_H + (s // 4) * CELL_H + 12 + 48 - scroll
            if cy - 60 < top or cy + 60 > bottom:
                continue  # 只核对完整露出的槽（环外缘到槽矩形顶 12px、值行到圆心 ±10px）
            full += 1
            up, left, down = px[cx, cy - 44], px[cx - 44, cy], px[cx, cy + 44]
            val = ink(cx - 44, cy - 10, cx + 44, cy + 10)
            start = px[cx - 31, cy - 31]  # 135°：扫角起点（值弧从这里起画）
            changed += val != PLACEHOLDER_INK
            good = (up in RING or left in RING) and down == PANEL and val >= PLACEHOLDER_INK
            if not good:
                bad.append(f"card {i} slot {s}: up={up} left={left} down={down} value_ink={val}")
            if i == 0 and s in (0, 1):
                print(f"card 0 slot {s}: 135°={start} 180°={left} 值墨迹={val}")
    check("分辨率 1280x720", (W, H) == (1280, 720), f"{W}x{H}")
    check(f"滚动 {scroll} 时完整露出 {full} 个槽：环在位、缺口是面板色、值墨迹 ≥ 占位 {PLACEHOLDER_INK}",
          full > 0 and not bad, "; ".join(bad) if bad else "")
    check(f"值文本替换了占位（墨迹 ≠ {PLACEHOLDER_INK} 的槽 {changed} 个 ≥ 10）", changed >= 10)
    if scroll == 0:
        c = (VIEW_X + PAD + CELL_W // 2, top + TITLE_H + 60)
        bpm = (px[c[0] - 31, c[1] - 31], px[c[0] - 44, c[1]])
        swing = (px[c[0] + CELL_W - 31, c[1] - 31], px[c[0] + CELL_W - 44, c[1]])
        check("值弧：BPM（120）135°/180° = 次级文本色", bpm == (TEXT_DIM, TEXT_DIM), f"{bpm}")
        check("底环：SWING（0）135°/180° = 面板亮色", swing == (PANEL_HI, PANEL_HI), f"{swing}")
    print("PNG CHECK PASS" if ok else "PNG CHECK FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
