#!/usr/bin/env python3
"""5B-1 Task 4：Sound 仪表盘的键盘焦点 / 页级 ± / 选轨条 / 折叠 / 读数行设备核对（证据脚本，随证据包入库）。

Usage:
    python3 task-4-check.py PORT TRANSCRIPT.log PNG_DIR [PLACEHOLDER_REF.png]

第 4 个参数（可选）= 修复前的提示行截屏（含 4 个占位空框）：同一套字形检查必须在它上面**报出**占位框，
用来证明这套检查分得清「占位框」与「真字形」（修复轮 1，Critical 1）。

写法照 scripts/selftest-heap.py 与 task-3-check.py：pyserial、BAUD 115200、读到 console 提示符
（sloop>）为止；串口字节逐字节写 TRANSCRIPT（原始回显，不裁剪），工具注记一律 `# [check] ...`
前缀。开头 esptool 硬复位（开机日志进 transcript），`STORE AUTO OFF` 后才动手；收尾再硬复位 =
重新载入 autosave 对象，本脚本的参数/选轨/引擎改动不落盘（复位后逐项核对）。注意 settings 的
dirty 不受 STORE AUTO OFF 约束（1s 任务延迟写）：ZOOM 开/关之后显式 `STORE SETTINGS SAVE`。

相位：base（Ctrl+1 选轨 0、池/内部堆、首个存在槽）→ focus（→/←/环绕）→ reveal（焦点滚进视口）
→ adjust（↑/↓/ENTER 与页级 ± 对焦点槽 ±1；锁定槽不动作）→ selector（选轨条 TAP）→ collapse
（CARDTOGGLE 与标题行 TAP）→ absent（焦点槽随引擎消失后重指）→ zoom（SYSTEM 页 ZOOM 行开/关，
读数行截屏）→ restore（硬复位后的快照）。全部断言通过退出码 0，末行 `# [check] PASS n/n`。
"""
import pathlib
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts"))
import screenshot as shot  # noqa: E402

BAUD = 115200
NOTE = "# [check]"
RE_FOCUS = re.compile(r"UI SOUND FOCUS flat=(\d+) card=(\d+) slot=(\d+) label=\"([^\"]*)\"")
RE_SCROLL = re.compile(r"UI SOUND SCROLL y=(-?\d+) max=(\d+)")
RE_STATS = re.compile(r"UI SOUND STATS cards=(\d+) slots=(\d+) scroll_max=(-?\d+) collapsed=0x([0-9A-F]{4})")
RE_SLOT = re.compile(r"UI SOUND SLOT card=(\d+) slot=(\d+) flat=(\d+) present=(\d) scope=(\S+) "
                     r"id=(\S+) label=\"([^\"]*)\" locked=(\d) value=(-?\d+) x=(-?\d+) y=(-?\d+) "
                     r"w=(-?\d+) h=(-?\d+)")
RE_ENG = re.compile(r"ENG SNAP trk k=(\d) engine=(\d+) preset=(\d+) level=(\d+)")
RE_TRKSEL = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d+) -> TRACKS select track=(\d) CMD_SELECT_TRACK")
RE_STEP = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d+) -> PARAM slot=(\d+) id=(\d+) delta=([+-]\d) value=(-?\d+)")
RE_LOCKED = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d+) -> PARAM slot=(\d+) locked")
RE_CARD = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d+) -> SOUND card=(\d+) collapsed=(\d)")
RE_TOGGLED = re.compile(r"UI SOUND CARDTOGGLE card=(\d+) collapsed=(\d)")
RE_HEAP = re.compile(r"ENG MEM internal_free=(\d+) internal_min=(\d+)")
RE_PAGE6 = re.compile(r"UI MEM page id=6 title=SOUND state=(\S+) est=(\d+) delta=(-?\d+) build_ms=(\d+)")
RE_BUILD = re.compile(r"页面构建 page=(\S+)\((\d+)\) 池增量=(-?\d+)B 预算=(\d+)B 池剩余=(-?\d+)B 耗时=(\d+)ms")
RE_SETTINGS = re.compile(r"STORE SETTINGS INFO kind=\S+ len=\d+ side=\d+ .*?zoom=(\d) .*?dirty=(\d)")
RE_STATS_STORE = re.compile(r"autosave=(\d) autosave_hash=(\S+) autosave_writes=(\d+)")
# 布局（sound_layout_place；折叠位 0）：卡片 → (列, y)。视口高 564（银行行隐藏）
SLOT_COUNTS = [4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7]
TITLE_H, CELL_H, CARD_PAD, CARD_GAP = 36, 132, 12, 8


def layout(collapsed=0):
    bottoms, cards = [0, 0], []
    for n, count in enumerate(SLOT_COUNTS):
        rows = (count + 3) // 4
        h = TITLE_H if collapsed >> n & 1 else TITLE_H + rows * CELL_H + CARD_PAD
        col = 1 if bottoms[1] < bottoms[0] else 0
        cards.append((col, bottoms[col], h))
        bottoms[col] += h + CARD_GAP
    return cards


class Console:
    def __init__(self, port, log_path):
        self.port = port
        self.log = open(log_path, "ab")
        self.ser = serial.Serial(port, BAUD, timeout=0.05)

    def note(self, text):
        line = f"{NOTE} {text}"
        print(line, flush=True)
        self.log.write((line + "\n").encode())
        self.log.flush()

    def read_until_prompt(self, timeout=8.0):
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
                return buf.decode(errors="replace")
        raise TimeoutError(f"等待提示符超时（已收 {len(buf)} 字节）")

    def cmd(self, line, settle=0.12, timeout=8.0):
        """发一条命令并等提示符；settle 让 30ms UI 拍在下一个读之前落地。"""
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        text = self.read_until_prompt(timeout)
        time.sleep(settle)
        return text

    def reset(self):
        """esptool 硬复位（开机日志进 transcript），回来后重新打开串口。"""
        self.ser.close()
        shot.reset_device(self.port, BAUD, on_chunk=self.log.write)
        self.log.flush()
        self.ser = serial.Serial(self.port, BAUD, timeout=0.05)

    def screenshot(self, out):
        self.ser.close()
        try:
            shot.capture(self.port, str(out), BAUD, 30.0)  # scripts/screenshot.py 的默认 --timeout
        finally:
            self.ser = serial.Serial(self.port, BAUD, timeout=0.05)

    def close(self):
        self.ser.close()
        self.log.close()


class Checks:
    def __init__(self):
        self.n = 0
        self.failed = 0

    def ok(self, cond, what):
        self.n += 1
        if not cond:
            self.failed += 1
        print(f"{NOTE} {'PASS' if cond else 'FAIL'} {what}", flush=True)
        return cond


def result_line(text):
    """回显里提示符之前的最后一行（命令的结果行）。"""
    lines = text.rsplit("sloop>", 1)[0].strip().splitlines()
    return [ln for ln in lines if not ln.startswith("Command returned")][-1]


def grab(text, regex, what):
    m = regex.search(text)
    if m is None:
        raise SystemError(f"{NOTE} FAIL: 响应里没有 {what}：{result_line(text)}")
    return m


def key(c, code):
    """一次按键：down + up 配对（写法照 inject.py 的 sys_key）。"""
    c.cmd(f"KEY 0x{code:02X} down")
    c.cmd(f"KEY 0x{code:02X} up")


def select_track(c, k):
    """Ctrl（0x40，SEL 层）+ 数字键 k+1（inject.py 的 key_with_layer 同一写法）。"""
    for code, ev in ((0x40, "down"), (k + 1, "down"), (k + 1, "up"), (0x40, "up")):
        c.cmd(f"KEY 0x{code:02X} {ev}")


def focus(c):
    return RE_FOCUS.search(c.cmd("UI SOUND FOCUS"))


def scroll(c):
    return RE_SCROLL.search(c.cmd("UI SOUND SCROLL"))


def stats(c):
    return RE_STATS.search(c.cmd("UI SOUND STATS"))


def slot(c, card, s):
    return RE_SLOT.search(c.cmd(f"UI SOUND CARD {card} SLOT {s}"))


def eng_rows(c):
    return re.findall(r"ENG SNAP trk k=\d+ engine=\d+ preset=\d+ level=\d+ mute=\d+ armed=\d+ solo=\d+ "
                      r"len=\d+ div=\d+ voices=\d+", c.cmd("ENG SNAP"))


def flat_pos(flat):
    """扁平下标 → (card, slot, 内容 y)。"""
    base = 0
    for card, count in enumerate(SLOT_COUNTS):
        if flat < base + count:
            s = flat - base
            return card, s, layout()[card][1] + TITLE_H + (s // 4) * CELL_H
        base += count
    raise ValueError(flat)


def hint_glyphs(png):
    """提示行的字形检查（代替读图）。Montserrat 缺字形时 LVGL 画的是 line_height 高的**空框**
    （lv_font.c: box_w = line_height/2、box_h = line_height；14pt ⇒ 10×16，实测占满行的 16 行），
    而真字形的连通域高度 ≤ 13（大写/上伸 10–11，带下伸 12–13）。返回：连通域数、最高连通域高度、
    占位框数（高 ≥ 15 且内部无墨迹的连通域）。"""
    from PIL import Image

    bg = (0x10, 0x14, 0x18)
    im = Image.open(png).convert("RGB")
    px = im.load()
    x0, y0, x1, y1 = 16, 112, 1088, 144  # 提示/读数行：页面区 y 40..72 → 屏幕 112..144
    ink = [[max(abs(px[x, y][i] - bg[i]) for i in range(3)) > 40 for x in range(x0, x1)] for y in range(y0, y1)]
    h, w = len(ink), len(ink[0])
    seen = [[False] * w for _ in range(h)]
    comps = []
    for y in range(h):
        for x in range(w):
            if not ink[y][x] or seen[y][x]:
                continue
            stack, pts = [(x, y)], []
            seen[y][x] = True
            while stack:
                cx, cy = stack.pop()
                pts.append((cx, cy))
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < w and 0 <= ny < h and ink[ny][nx] and not seen[ny][nx]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            xs, ys = [q[0] for q in pts], [q[1] for q in pts]
            comps.append((min(xs), min(ys), max(xs), max(ys), pts))
    boxes = 0
    for bx1, by1, bx2, by2, pts in comps:
        if by2 - by1 + 1 < 15:
            continue
        inner = set(pts)
        if not any((xx, yy) in inner for yy in range(by1 + 2, by2 - 1) for xx in range(bx1 + 2, bx2 - 1)):
            boxes += 1
    return {"components": len(comps), "tallest": max(c[3] - c[1] + 1 for c in comps) if comps else 0,
            "placeholders": boxes}


def hint_text_is_ascii(src):
    """从 page_sound.c 抠出 SOUND_HINT_TEXT 的字面量：每个字符都必须在 ASCII 0x20..0x7E 内。"""
    text = pathlib.Path(src).read_text(encoding="utf-8")
    # 宏体 = 以反斜杠续行的各行；字面量 = 其中所有双引号串
    m = re.search(r"#define SOUND_HINT_TEXT(?:[^\n]*\\\n)*[^\n]*", text)
    if m is None:
        return False, None
    lit = "".join(re.findall(r'"([^"]*)"', m.group(0)))
    return all(0x20 <= ord(ch) <= 0x7E for ch in lit), lit


def visible(rect_y, rect_h):
    """槽矩形（屏幕坐标）完整落在视口 152..716 内（银行行隐藏时的视口顶 80）。"""
    return rect_y >= 72 + 80 and rect_y + rect_h <= 72 + 644


def main():
    port, log_path, png_dir = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    ref_png = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    log_path.parent.mkdir(parents=True, exist_ok=True)
    png_dir.mkdir(parents=True, exist_ok=True)
    c = Console(port, log_path)
    ck = Checks()
    c.note(f"port={port}")
    from PIL import Image

    def near(p, col, tol=18):
        return all(abs(p[i] - col[i]) <= tol for i in range(3))

    def count(im, box, pred):
        px = im.load()
        w, h = im.size
        return sum(1 for yy in range(max(0, box[1]), min(h, box[3]))
                   for xx in range(max(0, box[0]), min(w, box[2])) if pred(px[xx, yy]))

    text, accent, panel_sel, panel, bg = (0xF2, 0xF5, 0xF8), (0xE0, 0xB3, 0x2A), (0x2C, 0x35, 0x40), \
        (0x1B, 0x20, 0x26), (0x10, 0x14, 0x18)
    hint_box = (16, 112, 1088, 144)
    sel_boxes = [(960 + k * 76 + 4, 78, 960 + (k + 1) * 76 - 4, 106) for k in range(4)]
    try:
        # 开机日志进 transcript（App version 等）
        c.reset()
        c.cmd("", settle=0.5)
        c.cmd("STORE AUTO OFF")
        boot = c.cmd("ENG SNAP")
        m = re.search(r"ENG SNAP playing=\d+ bpm=(\d+) sel=(\d)", boot)
        sel0 = m.group(2)
        rows0 = eng_rows(c)
        set0 = grab(c.cmd("STORE SETTINGS INFO"), RE_SETTINGS, "STORE SETTINGS INFO")
        st0 = grab(c.cmd("STORE STATS"), RE_STATS_STORE, "STORE STATS")
        c.note(f"基线：sel={sel0} bpm={m.group(1)} zoom={set0.group(1)} dirty={set0.group(2)} "
               f"autosave={st0.group(1)} autosave_hash={st0.group(2)}；四轨：{' | '.join(rows0)}")

        # ---- base：选轨 0（ANALOG）→ 建页 ----
        select_track(c, 0)
        ck.ok(re.search(r"selected_track=0 ", c.cmd("STATE SNAP")) is not None, "Ctrl+1 后 selected_track=0")
        ck.ok(eng_rows(c)[0].startswith("ENG SNAP trk k=0 engine=0 "), "轨 0 引擎 ANALOG")
        before = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        bm = grab(c.cmd("UI sound"), RE_BUILD, "页面构建")
        c.note(f"SOUND 首建 池增量={bm.group(3)}B 预算={bm.group(4)}B 池剩余={bm.group(5)}B 耗时={bm.group(6)}ms")
        p6 = RE_PAGE6.search(c.cmd("UI MEM"))
        ck.ok(p6 is not None and p6.group(1) == "built" and int(p6.group(3)) <= int(p6.group(2)),
              f"UI MEM {p6.group(0) if p6 else None}（delta ≤ est，est 不抬）")
        after = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        c.note(f"进入页面前后 internal_free {before.group(1)} -> {after.group(1)}（差 "
               f"{int(before.group(1)) - int(after.group(1))}B）；internal_min={after.group(2)}")
        ck.ok(int(after.group(2)) >= 61440, f"internal_min={after.group(2)} ≥ 61,440")
        st = stats(c)
        ck.ok(st.group(1) == "13" and st.group(2) == "109" and st.group(4) == "0000", f"{st.group(0)}")
        max0 = int(st.group(3))

        # ---- focus：首个存在槽 / → / ← / 环绕 ----
        f = focus(c)
        ck.ok(f is not None and f.group(1) == "0" and f.group(2) == "0" and f.group(3) == "0"
              and f.group(4) == "BPM", f"首个存在槽：{f.group(0) if f else None}")
        key(c, 0x4C)
        f = focus(c)
        ck.ok(f is not None and f.group(1) == "1", f"KEY 0x4C（→）⇒ {f.group(0) if f else None}")
        key(c, 0x4A)
        f = focus(c)
        ck.ok(f is not None and f.group(1) == "0", f"KEY 0x4A（←）⇒ 回 {f.group(0) if f else None}")
        key(c, 0x4A)
        f = focus(c)
        last_card, last_slot, _ = flat_pos(108)
        ck.ok(f is not None and f.group(1) == "108" and f.group(2) == str(last_card)
              and f.group(3) == str(last_slot), f"再按 ← ⇒ 环绕到末槽：{f.group(0) if f else None}")
        key(c, 0x4C)
        f = focus(c)
        ck.ok(f is not None and f.group(1) == "0", f"末槽再按 → ⇒ 环绕回 0：{f.group(0) if f else None}")

        # ---- reveal：焦点滚进视口 ----
        c.cmd("UI SOUND SCROLL 0")
        key(c, 0x4A)  # 0 → 108（末槽在视口下方）
        f = focus(c)
        sc = scroll(c)
        ck.ok(f.group(1) == "108" and int(sc.group(1)) > 0, f"焦点到末槽 ⇒ {sc.group(0)}（y > 0）")
        r = slot(c, last_card, last_slot)
        ck.ok(visible(int(r.group(11)), int(r.group(13))), f"末槽矩形滚进视口：{r.group(0)}")
        for _ in range(4):
            key(c, 0x4C)  # 108 → 0 → 1 → 2 → 3
        f = focus(c)
        sc = scroll(c)
        ck.ok(f.group(1) == "3", f"→ ×4 ⇒ {f.group(0)}")
        ck.ok(int(sc.group(1)) > 0, f"→ ×4 后 {sc.group(0)}（y > 0：焦点槽从底部滚回顶部）")
        c.cmd("UI SOUND SCROLL 0")
        sc = scroll(c)
        ck.ok(sc.group(1) == "0", f"UI SOUND SCROLL 0 ⇒ {sc.group(0)}")
        f = RE_FOCUS.search(c.cmd("UI SOUND FOCUS 108"))
        sc = scroll(c)
        r = slot(c, last_card, last_slot)
        ck.ok(f.group(1) == "108" and int(sc.group(1)) > 0, f"UI SOUND FOCUS 108 ⇒ {f.group(0)}；{sc.group(0)}")
        ck.ok(visible(int(r.group(11)), int(r.group(13))), f"末槽矩形可见：{r.group(0)}")
        # 键盘把焦点移回顶部：视口跟着滚回去
        key(c, 0x4C)  # 108 → 0
        f = focus(c)
        sc = scroll(c)
        ck.ok(f.group(1) == "0" and int(sc.group(1)) < max0, f"→ 环绕回 0 ⇒ {sc.group(0)}")
        r = slot(c, 0, 0)
        ck.ok(visible(int(r.group(11)), int(r.group(13))), f"首槽矩形可见：{r.group(0)}")

        # ---- adjust：焦点槽 ±1（Voice·LEVEL，flat 38）----
        lvl = slot(c, 5, 0)
        flat, v0 = int(lvl.group(3)), int(lvl.group(9))
        c.note(f"Voice·LEVEL flat={flat} 初值={v0}：{lvl.group(0)}")
        raw = c.cmd(f"UI SOUND FOCUS {flat}")
        key(c, 0x3B)
        rows = eng_rows(c)
        ck.ok(rows[0].split(" level=")[1].split()[0] == str(v0 + 1), f"KEY 0x3B（↑）⇒ {rows[0]}")
        for _ in range(2):
            key(c, 0x3B)
        rows = eng_rows(c)
        ck.ok(rows[0].split(" level=")[1].split()[0] == str(v0 + 3), f"↑ ×3 ⇒ {rows[0]}")
        key(c, 0x3D)
        rows = eng_rows(c)
        ck.ok(rows[0].split(" level=")[1].split()[0] == str(v0 + 4), f"KEY 0x3D（ENTER）⇒ {rows[0]}")
        for _ in range(4):
            key(c, 0x4B)
        rows = eng_rows(c)
        ck.ok(rows[0].split(" level=")[1].split()[0] == str(v0), f"KEY 0x4B（↓）×4 ⇒ 回到原值 {rows[0]}")
        ck.ok(eng_rows(c)[1:] == rows0[1:], "其余三轨的 ENG SNAP 行不变")
        t = c.cmd("TAP 1224 132")
        m = RE_STEP.search(t)
        ck.ok(m is not None and m.group(4) == str(flat) and m.group(5) == "0" and m.group(6) == "+1"
              and int(m.group(7)) == v0 + 1, f"TAP 细调 + ⇒ {result_line(t)}")
        t = c.cmd("TAP 1136 132")
        m = RE_STEP.search(t)
        ck.ok(m is not None and m.group(6) == "-1" and int(m.group(7)) == v0, f"TAP 细调 − ⇒ {result_line(t)}")
        rows = eng_rows(c)
        ck.ok(rows == rows0, "± 往返后 ENG SNAP 四轨行与基线逐字相同")
        ck.ok(slot(c, 5, 0).group(9) == str(v0), f"CARD 5 SLOT 0 回读 value={v0}")

        # ---- 锁定槽：鼓轨上的 ATK（flat 9）不动作 ----
        select_track(c, 3)
        atk = slot(c, 2, 0)
        rows_d = eng_rows(c)
        c.cmd(f"UI SOUND FOCUS {atk.group(3)}")
        key(c, 0x3B)
        key(c, 0x4B)
        ck.ok(eng_rows(c) == rows_d and slot(c, 2, 0).group(9) == atk.group(9),
              f"鼓轨锁定槽 {atk.group(3)}（{atk.group(7)} locked={atk.group(8)}）↑/↓ 不改值")
        t = c.cmd("TAP 1224 132")
        m = RE_LOCKED.search(t)
        ck.ok(m is not None and m.group(4) == atk.group(3), f"锁定槽 TAP + ⇒ {result_line(t)}")
        f = focus(c)
        ck.ok(f.group(1) == atk.group(3), f"锁定槽可持有焦点：{f.group(0)}")
        # 换轨后 30ms 窗口内的拖动（Task 3 评审遗留）：press → 选轨 → drag 三行一次批写，设备侧背靠背
        # 处理（主机不插入往返延迟），drag 大概率落在「锚点已被换轨作废、但下一拍还没清它」的窗口里。
        # 锚点记了所属轨道/引擎：pressing 里比对不符就结束拖动、不写。旧实现在这个窗口里会把旧锚点的
        # 值写进新轨（轨 2 的 LEVEL）。
        rows_w = eng_rows(c)
        worst = 0.0
        for _ in range(5):
            c.ser.write(b"UI SOUND KNOB 38 press 0 -40\nTAP 1150 92\nUI SOUND KNOB 38 drag 40 0\nTAP 998 92\n")
            c.ser.flush()
            t0 = time.monotonic()
            out = c.read_until_prompt()
            worst = max(worst, time.monotonic() - t0)
            time.sleep(0.12)
            out += c.cmd("")  # 四行的回显若没在一次读里到齐，这里补齐
        last = [ln for ln in out.splitlines() if "KNOB flat=38 drag" in ln][-1]
        ck.ok("ignored (ring band)" in last and eng_rows(c) == rows_w,
              f"批写 press→选轨→drag ×5 ⇒ {last.strip()}；四轨 ENG SNAP 行不变（批往返最长 {worst * 1000:.0f}ms）")
        # 键盘路径的同一扇窗（评审 Important 3）：Ctrl+2 与 ↑ 一次批写、设备侧背靠背。↑ 落在换轨之后、
        # 下一拍重解析之前时，槽状态说的还是轨 0——sound_slot_cmd 比对 s_ctx 与此刻的选中轨，不符不写。
        # 若 ↑ 落在下一拍之后，它会正常地给轨 1 的 LEVEL +1：这条断言通过 = 五次都落在窗口里。
        for _ in range(5):
            select_track(c, 0)
            c.ser.write(b"KEY 0x40 down\nKEY 0x02 down\nKEY 0x02 up\nKEY 0x40 up\nKEY 0x3B down\nKEY 0x3B up\n")
            c.ser.flush()
            out = c.read_until_prompt()
            time.sleep(0.12)
            out += c.cmd("")
            ck.ok(re.search(r"selected_track=1 ", c.cmd("STATE SNAP")) is not None and eng_rows(c) == rows_w,
                  f"批写 Ctrl+2 → ↑ ⇒ selected_track=1，四轨 ENG SNAP 行不变（轨 1 LEVEL 未被写）")
        select_track(c, 0)

        # ---- selector：选轨条 ----
        t = c.cmd("TAP 998 92")
        m = RE_TRKSEL.search(t)
        ck.ok(m is not None and m.group(4) == "0", f"TAP 998,92（第 1 格）⇒ {result_line(t)}")
        t = c.cmd("TAP 1150 92")
        m = RE_TRKSEL.search(t)
        ck.ok(m is not None and m.group(4) == "2", f"TAP 1150,92（第 3 格）⇒ {result_line(t)}")
        ck.ok(re.search(r"selected_track=2 ", c.cmd("STATE SNAP")) is not None, "STATE SNAP selected_track=2")
        ck.ok("page=sound (6)" in c.cmd("UI"), "UI 仍是 page=sound")
        f = focus(c)
        r = slot(c, int(f.group(2)), int(f.group(3)))
        ck.ok(r.group(4) == "1", f"换轨后焦点仍是存在的槽：{f.group(0)}")
        t = c.cmd("TAP 998 92")
        m = RE_TRKSEL.search(t)
        ck.ok(m is not None and m.group(4) == "0", f"还原到轨 0：{result_line(t)}")

        # ---- collapse：CARDTOGGLE 与标题行 TAP ----
        t = c.cmd("UI SOUND CARDTOGGLE 6")
        m = RE_TOGGLED.search(t)
        ck.ok(m is not None and m.group(2) == "1", f"{result_line(t)}")
        st = stats(c)
        c.note(f"折叠 FX 卡后 {st.group(0)}")
        ck.ok(st.group(4) == "0040" and int(st.group(3)) < max0 and st.group(2) == "109",
              f"collapsed=0x0040、scroll_max 变小（{max0} → {st.group(3)}）、槽数不变")
        c.cmd("UI SOUND SCROLL 99999")
        sc = scroll(c)
        ck.ok(int(sc.group(1)) == int(sc.group(2)) == int(st.group(3)), f"SCROLL 99999 ⇒ y=max：{sc.group(0)}")
        r = slot(c, 6, 13)
        ck.ok(r.group(4) == "1" and r.group(13) == "0", f"折叠卡的槽仍存在但无矩形：{r.group(0)}")
        # R12：折叠卡里的槽不可聚焦——把焦点设进折叠卡 6，UI 拍按「扁平序下一个可聚焦的槽」重指
        # （card 6 覆盖 47..68 ⇒ 69 = card 7 slot 0），并把它滚进视口
        t = c.cmd("UI SOUND FOCUS 60")
        f = RE_FOCUS.search(t)
        ck.ok(f is not None and f.group(1) != "60" and f.group(2) != "6", f"焦点设进折叠卡 ⇒ 重指：{f.group(0) if f else None}")
        fr = slot(c, int(f.group(2)), int(f.group(3)))
        sc = scroll(c)
        ck.ok(fr.group(4) == "1" and visible(int(fr.group(11)), int(fr.group(13))),
              f"重指后的槽可见：{fr.group(0)}；{sc.group(0)}")
        # 页面重建保留焦点与折叠位（评审 Important 2）：离开 EDIT 组再回来
        c.cmd("UI step")
        c.cmd("UI sound")
        st2 = stats(c)
        f2 = focus(c)
        ck.ok(st2.group(4) == "0040" and f2.group(1) == f.group(1),
              f"UI step → UI sound 之后：{st2.group(0)}；{f2.group(0)}（与重建前相同）")
        st = stats(c)
        t = c.cmd("UI SOUND CARDTOGGLE 6")
        m = RE_TOGGLED.search(t)
        st = stats(c)
        ck.ok(m.group(2) == "0" and st.group(4) == "0000" and int(st.group(3)) == max0,
              f"还原：{result_line(t)}；{st.group(0)}")
        # 标题行 TAP：Envelope 卡（列 0，y=188）标题行屏幕 y 340..376。先把焦点停在首槽：折叠后的
        # 那一拍会对焦点槽再做一次滚进视口，焦点若在下方（flat 60）会把列表滚走，标题行就不在原处了
        c.cmd("UI SOUND SCROLL 0")
        c.cmd("UI SOUND FOCUS 0")
        cards = layout()
        t = c.cmd(f"TAP {16 + cards[2][0] * 632 + 60} {72 + 80 + cards[2][1] + 18}")
        m = RE_CARD.search(t)
        ck.ok(m is not None and m.group(4) == "2" and m.group(5) == "1", f"TAP 标题行 ⇒ 翻为折叠：{result_line(t)}")
        st = stats(c)
        sc = scroll(c)
        ck.ok(st.group(4) == "0004" and sc.group(1) == "0", f"{st.group(0)}；{sc.group(0)}（焦点在首槽，列表不动）")
        t = c.cmd(f"TAP {16 + cards[2][0] * 632 + 60} {72 + 80 + cards[2][1] + 18}")
        m = RE_CARD.search(t)
        st = stats(c)
        ck.ok(m.group(5) == "0" and st.group(4) == "0000", f"再 TAP ⇒ 展开：{result_line(t)}；{st.group(0)}")
        raw = c.cmd("UI SOUND CARDTOGGLE 13")
        ck.ok("UI ERR sound (invalid)" in raw, f"CARDTOGGLE 13 ⇒ {result_line(raw)}")

        # ---- R12：焦点所在的卡被折叠 ⇒ 重指；全部折叠 ⇒ 保持焦点、编辑惰性 ----
        c.cmd("UI SOUND SCROLL 0")
        f40 = RE_FOCUS.search(c.cmd("UI SOUND FOCUS 40"))
        raw = c.cmd("UI SOUND CARDTOGGLE 5")
        f = focus(c)
        ck.ok(f40.group(2) == "5" and f.group(1) == "47" and f.group(2) == "6" and f.group(3) == "0",
              f"焦点 40（card 5）时折叠 card 5 ⇒ {result_line(raw)}；{f.group(0)}（card 5 覆盖 38..46 ⇒ 下一个 47）")
        st = stats(c)
        ck.ok(st.group(4) == "0020", f"{st.group(0)}")
        c.cmd("UI SOUND CARDTOGGLE 5")
        for n in range(13):
            c.cmd(f"UI SOUND CARDTOGGLE {n}")
        st = stats(c)
        f = focus(c)
        fr = slot(c, int(f.group(2)), int(f.group(3)))
        rows_all = eng_rows(c)
        ck.ok(st.group(4) == "1FFF" and int(f.group(1)) < 109 and fr.group(4) == "1",
              f"全部折叠 ⇒ {st.group(0)}；焦点保持在某个存在的槽：{f.group(0)}")
        key(c, 0x3B)
        key(c, 0x4B)
        t = c.cmd("TAP 1224 132")
        m = RE_LOCKED.search(t)
        ck.ok(eng_rows(c) == rows_all and m is not None and m.group(4) == f.group(1),
              f"全折叠时 ↑/↓ 不写（四轨行不变）；TAP + ⇒ {result_line(t)}")
        for n in range(13):
            c.cmd(f"UI SOUND CARDTOGGLE {n}")
        st = stats(c)
        f2 = focus(c)
        ck.ok(st.group(4) == "0000" and f2.group(1) == f.group(1), f"全部展开 ⇒ {st.group(0)}；焦点不变 {f2.group(0)}")

        # ---- absent：焦点槽随引擎消失后重指 ----
        s48 = slot(c, 4, 8)
        ck.ok(s48.group(4) == "1" and s48.group(7) == "TYPE", f"ANALOG：{s48.group(0)}")
        c.cmd(f"UI SOUND FOCUS {s48.group(3)}")
        t = c.cmd("TAP 283 92")
        ck.ok("-> SOUND ENGINE value=1 " in t and 'status="OK"' in t, f"TAP 283,92（ENGINE +）⇒ {result_line(t)}")
        s48b = slot(c, 4, 8)
        ck.ok(s48b.group(4) == "0", f"换引擎后 P_FTYPE 不存在：{s48b.group(0)}")
        f = focus(c)
        fr = slot(c, int(f.group(2)), int(f.group(3)))
        ck.ok(f.group(1) != s48.group(3) and fr.group(4) == "1", f"焦点重指到存在的槽：{f.group(0)}")
        raw = c.cmd(f"UI SOUND FOCUS {s48.group(3)}")
        ck.ok("UI ERR sound (absent)" in raw, f"不存在槽设焦点 ⇒ {result_line(raw)}")
        key(c, 0x4C)
        f = focus(c)
        fr = slot(c, int(f.group(2)), int(f.group(3)))
        ck.ok(fr.group(4) == "1", f"新引擎上 → 仍落在存在的槽：{f.group(0)}")
        t = c.cmd("TAP 37 92")
        ck.ok("-> SOUND ENGINE value=0 " in t, f"TAP 37,92（ENGINE −）⇒ {result_line(t)}")
        ck.ok(eng_rows(c)[0].startswith("ENG SNAP trk k=0 engine=0 "), "轨 0 回到 ANALOG")
        s48c = slot(c, 4, 8)
        ck.ok(s48c.group(4) == "1", f"P_FTYPE 重新存在：{s48c.group(0)}")
        st = stats(c)
        ck.ok(st.group(2) == "109" and int(st.group(3)) == max0, f"回到 ANALOG：{st.group(0)}")

        # ---- zoom：读数行 ----
        c.cmd("UI SOUND SCROLL 0")
        c.cmd("UI SOUND FOCUS 0")
        c.screenshot(png_dir / "task-4-sound-hint.png")
        c.note("截屏 task-4-sound-hint.png（ZOOM 关：提示行 + 选轨条轨 0 选中 + 焦点描边 flat 0）")
        c.cmd("UI system")
        t = c.cmd("TAP 374 163")
        ck.ok('-> SYS ZOOM value=ON status="OK"' in t, f"SYSTEM 页 ZOOM chip ⇒ {result_line(t)}")
        c.cmd("UI sound")
        c.note("ZOOM 开，读数行（焦点 BPM）：截屏 task-4-sound-zoom.png")
        c.screenshot(png_dir / "task-4-sound-zoom.png")
        t = c.cmd("TAP 1150 92")  # 换轨：读数行跟着换
        c.cmd("UI SOUND FOCUS 38")
        c.note("ZOOM 开，读数行（焦点 LVL，轨 2）：截屏 task-4-sound-zoom-lvl.png")
        c.screenshot(png_dir / "task-4-sound-zoom-lvl.png")
        c.cmd("UI system")
        t = c.cmd("TAP 374 163")
        ck.ok('-> SYS ZOOM value=OFF status="OK"' in t, f"ZOOM chip 再点 ⇒ {result_line(t)}")
        # settings 的 dirty 与 STORE AUTO 无关：1s 任务的延迟写（停走带 + 安静 + 输入空闲 ≥1.5s + 距上次
        # 尝试 ≥5s）会把 settings 对象落盘（store_tab5.c sloop_store_sections_tick，P4-E Task 2）。ZOOM 开着
        # 截两张屏的 ~35s 足够它把 zoom=1 写进 flash——所以还原必须显式 SAVE（inject.py run_sys ⑦ 同法），
        # 不能指望硬复位丢弃。
        c.cmd("STORE SETTINGS SAVE")
        set1 = grab(c.cmd("STORE SETTINGS INFO"), RE_SETTINGS, "STORE SETTINGS INFO")
        ck.ok(set1.group(1) == set0.group(1) and set1.group(2) == "0",
              f"ZOOM 回到基线并落盘：zoom={set1.group(1)} dirty={set1.group(2)}")

        # 像素核对（代替读图）。矩形读数回 SOUND 页取（与截屏时同一滚动位置 0）
        c.cmd("UI sound")
        c.cmd("UI SOUND SCROLL 0")
        im = Image.open(png_dir / "task-4-sound-hint.png").convert("RGB")
        hint_bright = count(im, hint_box, lambda p: near(p, text))
        hint_ink = count(im, hint_box, lambda p: not near(p, bg, 6))
        ck.ok(hint_bright < 40 and hint_ink >= 200,
              f"ZOOM 关：提示行有墨迹（{hint_ink}）且无 28pt 亮色读数（{hint_bright}）")
        # 字形检查：行里没有占位空框（高 ≥ 15 的空心连通域），最高连通域 < 15；文案全 ASCII；
        # 同一检查在修复前的截屏上必须报出占位框（第 4 个参数给了才断言）
        g = hint_glyphs(png_dir / "task-4-sound-hint.png")
        ck.ok(g["placeholders"] == 0 and g["tallest"] < 15 and g["components"] >= 60,
              f"提示行字形：连通域 {g['components']} 个、最高 {g['tallest']}px、占位框 {g['placeholders']} 个（期望 0）")
        ascii_ok, lit = hint_text_is_ascii(ROOT / "firmware/components/sloop_ui/src/page_sound.c")
        ck.ok(ascii_ok and lit is not None and " · " not in lit,
              f"SOUND_HINT_TEXT 全 ASCII 0x20..0x7E：{lit}")
        if ref_png is not None:
            g0 = hint_glyphs(ref_png)
            ck.ok(g0["placeholders"] >= 4,
                  f"同一检查在修复前的截屏 {ref_png.name} 上报出占位框 {g0['placeholders']} 个（最高 {g0['tallest']}px）")
        c.note(f"字形检查方法：提示行（屏幕 y 112..144、x 16..1088）取墨迹（与背景色差 > 40），8 连通域；"
               f"占位框 = 高 ≥ 15px 且内部无墨迹的连通域（LVGL 缺字形画的是 line_height 高的空框，14pt ⇒ 16px；"
               f"真字形 ≤ 13px）。新截屏：{g}；修复前：{hint_glyphs(ref_png) if ref_png else '未给'}")
        cur = count(im, sel_boxes[0], lambda p: near(p, panel_sel, 8))
        others = [count(im, sel_boxes[k], lambda p: near(p, panel, 8)) for k in (1, 2, 3)]
        ck.ok(cur >= 1500 and all(n >= 1500 for n in others),
              f"选轨条：第 1 格选中底色（{cur}），其余面板底色（{others}）")
        r = slot(c, 0, 0)
        x1, y1, x2, y2 = int(r.group(10)), int(r.group(11)), int(r.group(10)) + 140, int(r.group(11)) + 132
        frame = count(im, (x1, y1, x2, y2), lambda p: near(p, accent))
        inner = count(im, (x1 + 6, y1 + 6, x2 - 6, y2 - 6), lambda p: near(p, accent))
        ck.ok(frame >= 600 and inner == 0, f"焦点描边（flat 0，强调色）在槽矩形内缘：{frame} 像素（内区 {inner}）")
        r = slot(c, 0, 1)
        frame1 = count(im, (int(r.group(10)), int(r.group(11)), int(r.group(10)) + 140, int(r.group(11)) + 132),
                       lambda p: near(p, accent))
        ck.ok(frame1 == 0, f"非焦点槽（flat 1）无描边：{frame1} 像素")
        im = Image.open(png_dir / "task-4-sound-zoom.png").convert("RGB")
        zoom_bright = count(im, hint_box, lambda p: near(p, text))
        ck.ok(zoom_bright >= 200, f"ZOOM 开：读数行有 28pt 亮色读数（{zoom_bright} 像素）")
        im2 = Image.open(png_dir / "task-4-sound-zoom-lvl.png").convert("RGB")
        px1, px2 = im.load(), im2.load()
        diff = sum(1 for yy in range(112, 144) for xx in range(16, 1088) if px1[xx, yy] != px2[xx, yy])
        ck.ok(diff >= 100, f"焦点换槽后读数行重绘（两帧读数行差异像素 {diff}）")

        # ---- restore：收尾硬复位 = 重新载入 autosave 对象 ----
        heap = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        ck.ok(int(heap.group(2)) >= 61440, f"{heap.group(0)}（internal_min ≥ 61,440）")
        p6 = RE_PAGE6.search(c.cmd("UI MEM"))
        c.note(f"收尾 {p6.group(0)}")
        c.note("收尾：硬复位重新载入 autosave 对象")
        c.reset()
        c.cmd("", settle=0.5)
        boot = c.cmd("ENG SNAP")
        m = re.search(r"ENG SNAP playing=\d+ bpm=(\d+) sel=(\d)", boot)
        rows1 = eng_rows(c)
        set1 = grab(c.cmd("STORE SETTINGS INFO"), RE_SETTINGS, "STORE SETTINGS INFO")
        st1 = grab(c.cmd("STORE STATS"), RE_STATS_STORE, "STORE STATS")
        st_param = re.search(r"STATE SNAP param trk=(\d) level=(\d+) atk=(\d+) dec=(\d+) sus=(\d+) rel=(\d+)",
                             c.cmd("STATE SNAP"))
        c.note(f"复位后：sel={m.group(2)} bpm={m.group(1)} zoom={set1.group(1)} dirty={set1.group(2)} "
               f"autosave={st1.group(1)} autosave_hash={st1.group(2)} autosave_writes={st1.group(3)}；"
               f"四轨：{' | '.join(rows1)}；{st_param.group(0)}")
        ck.ok(m.group(2) == sel0 and rows1 == rows0, "复位后 sel 与四轨 ENG SNAP 行与基线逐字相同")
        ck.ok(set1.group(1) == set0.group(1) and set1.group(2) == set0.group(2),
              f"复位后 settings zoom/dirty 与基线相同（{set1.group(1)}/{set1.group(2)}）")
        ck.ok(st1.group(3) == st0.group(3) and st1.group(2) == st0.group(2),
              f"复位后 autosave_hash={st1.group(2)} autosave_writes={st1.group(3)} 与基线相同（autosave 开关复位后回 1）")
    finally:
        c.close()
    total = ck.n - ck.failed
    print(f"{NOTE} {'PASS' if ck.failed == 0 else 'FAIL'} {total}/{ck.n}", flush=True)
    with log_path.open("a", encoding="utf-8") as f:
        f.write(f"{NOTE} {'PASS' if ck.failed == 0 else 'FAIL'} {total}/{ck.n}\n")
    return 0 if ck.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
