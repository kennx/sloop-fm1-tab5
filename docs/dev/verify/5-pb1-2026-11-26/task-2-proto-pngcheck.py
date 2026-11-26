#!/usr/bin/env python3
"""5B-1 Task 2：Sound 仪表盘截屏的像素核对（证据脚本，随证据包入库）。

Usage:
    python3 task-2-proto-pngcheck.py SCREEN.png [--bank]

本会话没有图像查看能力，这里用像素核对代替「读图」：按 sound_layout_place 的同一规则
（卡片高 = 36 + 行数×132 + 12、staggered 两列、间距 8）复算 13 张卡片的位置，逐张核对——
  - 卡片体内只有面板色/文字/弧线，没有页面背景色；卡片左右 4px 带与视口底边以下是背景色
    （自绘没有溢出卡片，容器裁剪生效）；
  - 标题行有文字墨迹、右端有折叠标记；
  - 视口内每个槽：环带像素数、名称墨迹（弧上方 12px 内）、值占位墨迹（弧内）；
  - 视口之外是背景：页眉与视口之间的带（按钮左侧）、列间空隙、左右页边、视口底边以下；
  - 页眉两行（ENGINE/PRESET）与 ± 按钮的面板色（--bank 时视口顶 128，按钮仍在 y=40）；
  --bank：银行行在页眉下方 y=76..120（BANK 标签墨迹、−/+ 面板、SAVE/ERASE 彩色底），行的上下都是背景。
颜色取 MONO 主题（theme_derive.h）：背景 101418 / 面板 182020 / 面板亮 202830。
末行 `PNG CHECK PASS`（退出码 0）或 `PNG CHECK FAIL`（退出码 1）。**人眼读图仍是外部门禁**
（docs/dev/reference/verification-gates.md 的 song 行同口径）。
"""
import sys
from collections import Counter

from PIL import Image

SLOTS = [4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7]  # k_sound_cards[].slot_count
BG, PANEL, PANEL_HI = (0x10, 0x14, 0x18), (0x18, 0x20, 0x20), (0x20, 0x28, 0x30)
VIEW_X, VIEW_W, VIEW_BOTTOM = 16, 1248, 644
TITLE_H, CELL_W, CELL_H, COL_W, COL_GAP, PAD = 36, 140, 132, 616, 16, 12

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print(("PASS " if cond else "FAIL ") + name, detail)


def main():
    path = sys.argv[1]
    bank = "--bank" in sys.argv
    view_top = 128 if bank else 80
    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()
    check("分辨率 1280x720", (W, H) == (1280, 720), f"{W}x{H}")
    census = Counter(im.get_flattened_data() if hasattr(im, "get_flattened_data")
                    else im.getdata()).most_common(4)
    print("最常见颜色:", [(("#%02X%02X%02X" % k), v) for k, v in census])
    check("背景/面板色 = MONO 主题", BG in dict(census) and PANEL in dict(census))

    top, bottom = 72 + view_top, 72 + VIEW_BOTTOM  # 视口在屏幕上的 y 范围

    def count(rect, colours):
        x1, y1, x2, y2 = rect
        return sum(1 for yy in range(max(0, y1), min(H, y2))
                   for xx in range(max(0, x1), min(W, x2)) if px[xx, yy] in colours)

    def area(rect):
        x1, y1, x2, y2 = rect
        return max(0, min(W, x2) - max(0, x1)) * max(0, min(H, y2) - max(0, y1))

    def ink(rect):  # 既非背景也非面板色的像素 = 文字/弧线/抗锯齿
        return area(rect) - count(rect, {BG, PANEL})

    bottoms = [0, 0]
    cards = []
    for n in SLOTS:
        rows = (n + 3) // 4
        col = 1 if bottoms[1] < bottoms[0] else 0
        h = TITLE_H + rows * CELL_H + PAD
        cards.append((col * (COL_W + COL_GAP), bottoms[col], COL_W, h))
        bottoms[col] += h + 8
    print(f"内容总高 {max(bottoms) - 8}px（视口高 {VIEW_BOTTOM - view_top}px）")

    visible = 0
    for i, (x, y, w, h) in enumerate(cards):
        sx, sy = VIEW_X + x, top + y
        vis = min(h, VIEW_BOTTOM - view_top - y)  # 视口底边（内容坐标）= 644 − 视口顶
        if vis <= TITLE_H + 20:
            print(f"SKIP card {i}（在视口之下：内容矩形 ({x},{y},{w},{h})）")
            continue
        visible += 1
        body = (sx + 14, sy + 14, sx + w - 14, sy + vis - 2)
        check(f"card {i} 体=面板色 / 左右 4px 带=背景 / 视口底边以下=背景",
              count(body, {BG}) == 0 and count((sx - 4, sy, sx, sy + vis), {BG}) == 4 * vis and
              count((sx + w, sy, sx + w + 4, sy + vis), {BG}) == 4 * vis and
              count((sx, bottom, sx + w, H), {BG}) == w * (H - bottom),
              f"内容矩形 ({x},{y},{w},{h}) 可见高 {vis}")
        check(f"card {i} 标题墨迹 / 右端折叠标记",
              ink((sx + PAD, sy + 8, sx + PAD + 200, sy + 27)) > 40 and
              ink((sx + w - PAD - 20, sy + 8, sx + w - PAD, sy + 27)) > 10,
              f"title={ink((sx + PAD, sy + 8, sx + PAD + 200, sy + 27))} "
              f"mark={ink((sx + w - PAD - 20, sy + 8, sx + w - PAD, sy + 27))}")
        for s in range(SLOTS[i]):
            cx, cy = x + PAD + (s % 4) * CELL_W, y + TITLE_H + (s // 4) * CELL_H
            scx, scy = VIEW_X + cx, top + cy
            if scy >= bottom:
                continue
            if scy + 12 > bottom:
                print(f"SKIP card {i} slot {s}（只露出 {bottom - scy}px）")
                continue
            ring = count((scx, scy, scx + CELL_W, min(scy + CELL_H, bottom)), {PANEL_HI})
            name = ink((scx, scy, scx + CELL_W, min(scy + 12, bottom)))
            full = scy + CELL_H <= bottom  # 被视口底边裁掉的槽只核对露出的部分
            val = ink((scx + 40, scy + 48, scx + 100, scy + 74)) if full else -1
            check(f"card {i} slot {s} 环 / 名称 / 值占位{'' if full else '（被视口底边裁切）'}",
                  ring > 150 and name > 8 and (val > 8 if full else True),
                  f"ring={ring} name={name} value={val} 屏幕矩形 ({scx},{scy},{CELL_W},{CELL_H})")
    print(f"视口内可见卡片 {visible}/13")

    if bank:
        check("页眉行与银行行之间（页面区 y=36..76，按钮左侧）= 背景",
              count((0, 72 + 36, 1096 - 16, 72 + 76), {BG}) == area((0, 72 + 36, 1096 - 16, 72 + 76)))
    else:
        check("页眉与视口之间的带（页面区 y=36..80，按钮左侧）= 背景",
              count((0, 72 + 36, 1096 - 16, top), {BG}) == area((0, 72 + 36, 1096 - 16, top)))
    check("列间空隙 x 632..648 = 背景",
          count((VIEW_X + COL_W, top, VIEW_X + COL_W + COL_GAP, bottom), {BG}) == COL_GAP * (bottom - top))
    check("左右页边 = 背景",
          count((0, top, VIEW_X, bottom), {BG}) == VIEW_X * (bottom - top) and
          count((VIEW_X + VIEW_W, top, 1280, bottom), {BG}) == (1280 - VIEW_X - VIEW_W) * (bottom - top))
    check("视口底边以下 = 背景", count((0, bottom, 1280, H), {BG}) == 1280 * (H - bottom))
    for name, rx, rw in (("ENGINE 行", 16, 288), ("PRESET 行", 320, 384)):
        hit = count((rx + 4, 76, rx + rw - 4, 108), {PANEL_HI})
        check(f"页眉 {name} 面板", hit > area((rx + 4, 76, rx + rw - 4, 108)) * 0.6, f"{hit}")
    for name, bx in (("按钮 −", 1096), ("按钮 +", 1184)):
        hit = count((bx + 4, 72 + 44, bx + 76, 72 + 76), {PANEL_HI})
        check(f"{name} 在页面区 y=40", hit > area((bx + 4, 72 + 44, bx + 76, 72 + 76)) * 0.6, f"{hit}")
    if bank:
        # 银行行（R8：页面区 y=76..120）：BANK 文本标签、− / + 面板、绿 SAVE、橙 ERASE（x 同旧底部行）
        by = 72 + 76
        label = ink((16, by + 10, 104, by + 34))
        panels = [count((bx, by, bx + bw, by + 44), {PANEL_HI}) for bx, bw in ((112, 64), (288, 64))]
        acts = [area((bx, by, bx + 100, by + 44)) - count((bx, by, bx + 100, by + 44), {BG, PANEL, PANEL_HI})
                for bx in (368, 484)]
        check("银行行在页眉下方 y=76..120：BANK 标签 / − + 面板 / SAVE·ERASE 彩色底",
              label > 20 and all(p > 64 * 44 * 0.6 for p in panels) and all(a > 100 * 44 * 0.6 for a in acts),
              f"label={label} panels={panels} save_erase={acts}")
        check("银行行与视口之间（页面区 y=120..128）= 背景",
              count((16, by + 44, 1096 - 16, top), {BG}) == area((16, by + 44, 1096 - 16, top)))
    else:
        check("银行行隐藏：BANK 标签处（页面区 y=76..120）没有按钮面板",
              count((16 + 4, 72 + 76 + 4, 16 + 88 - 4, 72 + 120 - 4), {PANEL_HI}) == 0)
    print("PNG CHECK PASS" if ok else "PNG CHECK FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
