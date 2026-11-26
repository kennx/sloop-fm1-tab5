#!/usr/bin/env python3
"""5B-1 Task 6：Sound 页引擎 / 预置 / 鼓 kit 选择器的设备核对（证据脚本，随证据包入库）。

Usage:
    python3 task-6-check.py PORT TRANSCRIPT.log PNG_DIR

写法照 scripts/selftest-heap.py 与 task-4-check.py：pyserial、BAUD 115200、读到 console 提示符
（sloop>）为止；串口字节逐字节写 TRANSCRIPT（原始回显，不裁剪），工具注记一律 `# [check] ...`
前缀。开头 esptool 硬复位（开机日志进 transcript），`STORE AUTO OFF` 后才动手；收尾再硬复位 =
重新载入 autosave 对象，本脚本的引擎/预置/kit/选轨改动不落盘（复位后逐项核对）。

PNG 没有人眼读（本会话无图像输入能力）：每张截屏做像素核对（面板边框 / chip 行 / 高亮条 /
当前项强调色文字 / 提示行墨迹落在面板内）。

修复轮 3：row 0 的列表焦点（↓ / 页级 ± 之后）ENTER = 选中高亮的引擎（预览规则只对 row 1）。
修复轮 2：预览态、焦点在列表时 ENTER 不动作（→ 回 chip 行后 ENTER 才换引擎）、预览态提示行换文案、
`UI SOUND PICK` 只认四种形式、裸 release 不算点击。

修复轮 1（R16 + 评审两项 Important 与四项 Minor）：面板空白处 = miss、滑过的松手不算点击（`UI SOUND
PICK press|release` 注入）、预览态（chip 高亮 ≠ 当前引擎）列表只读且不标当前项、ENTER = 确认 chip、载入
用户预置后工厂预置 0 不算当前项。

相位：engine（row 0：引擎列表、chip 高亮、TAP 行选引擎、←/→ + ENTER）→ preset（row 1：预置列表、
TAP 行选预置、↓ + ENTER、ESC、滚入视口、空白处 miss、滑过不算点击、R16 预览、页级 ±、当前项守卫、
用户预置）→ context（Ctrl+2 /
选轨条 / 切页都关闭选择器）→ drum（kit 列表、TAP 行选 kit）→ pool（开/关 ×5 的池往返）
→ restore（硬复位后的快照）。全部断言通过退出码 0，末行 `# [check] PASS n/n`。
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
RE_PICK = re.compile(r"UI SOUND PICK open=1 row=(\d) engine=(\d+) highlight=(\d+) rows=(\d+)")
RE_PICKN = re.compile(r'UI SOUND PICK n=(\d+) text="([^"]*)" current=(\d) x=(-?\d+) y=(-?\d+) '
                      r"w=(-?\d+) h=(-?\d+)")
RE_TAP_OPEN = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d) -> SOUND pick open row=(\d)")
RE_TAP_PICK = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d) -> SOUND pick (engine|kit|preset)=(\d+) ([^\r\n]*)")
RE_TAP_MISS = re.compile(r"TAP miss (-?\d+),(-?\d+) page=(\d)")
RE_TAP_PREVIEW = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d) -> SOUND pick preview engine=(\d+)")
RE_TOUCH = re.compile(r"UI SOUND PICK (press|release) x=(-?\d+) y=(-?\d+) (\S+)")
RE_PRESET_EMPTY = re.compile(r"PRESET (\d+) EMPTY")
RE_TRACKSEL = re.compile(r"TAP -?\d+,-?\d+ page=\d+ -> TRACKS select track=(\d) CMD_SELECT_TRACK")
RE_ENG = re.compile(r"ENG SNAP trk k=(\d) engine=(\d+) preset=(\d+) level=(\d+) mute=\d+ armed=\d+ solo=\d+ "
                    r"len=\d+ div=\d+ voices=\d+")
RE_SNAPHEAD = re.compile(r"ENG SNAP playing=\d+ bpm=(\d+) sel=(\d)")
RE_PAGE = re.compile(r"UI page=(\w+) \((\d+)\)")
RE_POOL = re.compile(r"UI MEM pool_used=(\d+) pool_free=(\d+) pool_total=(\d+)")
RE_PAGE6 = re.compile(r"UI MEM page id=6 title=SOUND state=(\S+) est=(\d+) delta=(-?\d+) build_ms=(\d+)")
RE_HEAP = re.compile(r"ENG MEM internal_free=(\d+) internal_min=(\d+)")
RE_PARAM = re.compile(r"PARAM track=\d+ id=\d+ value=(-?\d+) min=-?\d+ max=(\d+)")
RE_SETTINGS = re.compile(r"STORE SETTINGS INFO kind=\S+ len=\d+ side=\d+ .*?zoom=(\d) .*?dirty=(\d)")
RE_STATS_STORE = re.compile(r"autosave=(\d) autosave_hash=(\S+) autosave_writes=(\d+)")
RE_FOCUS = re.compile(r'UI SOUND FOCUS flat=(\d+) card=(\d+) slot=(\d+) label="([^"]*)"')
RE_STATS = re.compile(r"UI SOUND STATS cards=13 slots=(\d+) scroll_max=(-?\d+) collapsed=0x([0-9A-F]{4})")

# 选择器几何（page_sound_picker.c；屏幕坐标 = 页面区 + 72）
PANEL = (160, 192, 1120, 632)
CHIP_ROW = (176, 200, 1104, 240)
LIST_Y_SYNTH, LIST_Y_DRUM, ROW_H, ROWS_VISIBLE, LIST_X, LIST_W = 248, 216, 32, 11, 176, 928
P_E0 = 63  # SLOOP_CORE_P_COUNT（71）- 8：鼓轨的 KIT 槽
ENGINE_NAMES = ["ANALOG", "DIGITAL", "PHASE", "LOFI", "SAMPLE", "VOICE", "TRIO", "WHEEL", "GRAIN", "FM6",
                "PHYS", "NOISE"]
CHIP_X = [176]
for _w in [11 * len(s) + 24 for s in ENGINE_NAMES[:-1]]:  # chip 宽 11×字符 + 24、间距 8
    CHIP_X.append(CHIP_X[-1] + _w + 8)


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


KEY_ESC, KEY_ENTER, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT = 0x00, 0x3D, 0x3B, 0x4B, 0x4A, 0x4C


def eng_rows(c):
    return RE_ENG.findall(c.cmd("ENG SNAP"))


def pick(c):
    return RE_PICK.search(c.cmd("UI SOUND PICK"))


def pick_n(c, n):
    return RE_PICKN.search(c.cmd(f"UI SOUND PICK {n}"))


def pool_free(c):
    return int(grab(c.cmd("UI MEM"), RE_POOL, "UI MEM pool_free").group(2))


def tap_row(c, n):
    """`UI SOUND PICK n` 的矩形中心 TAP；返回 (行回显, TAP 回显)。"""
    m = pick_n(c, n)
    if m is None:
        raise SystemError(f"{NOTE} FAIL: UI SOUND PICK {n} 没有行回显")
    x = int(m.group(4)) + int(m.group(6)) // 2
    y = int(m.group(5)) + int(m.group(7)) // 2
    return m, c.cmd(f"TAP {x} {y}")


def open_row(c, x):
    """TAP 页眉行名字区（row 0 → (160,92)；row 1 → (512,92)）。"""
    return c.cmd(f"TAP {x} 92")


def panel_pixels(png, drum, hl_row, has_current=True):
    """截屏的像素核对（代替读图）。返回 [(cond, what)]。"""
    from PIL import Image

    im = Image.open(png).convert("RGB")
    px = im.load()

    def is_accent(c):
        return c[0] > 180 and c[1] > 140 and c[2] < 90

    def count(box, pred):
        return sum(1 for yy in range(box[1], box[3]) for xx in range(box[0], box[2]) if pred(px[xx, yy]))

    def ink_bbox(box):
        xs, ys = [], []
        for yy in range(box[1], box[3]):
            for xx in range(box[0], box[2]):
                r, g, b = px[xx, yy]
                if r + g + b > 300 and not is_accent((r, g, b)):
                    xs.append(xx)
                    ys.append(yy)
        return (min(xs), min(ys), max(xs), max(ys)) if xs else None

    # 面板底的取样点：合成轨 = chip 行与列表之间（y 244）；鼓轨 = 面板顶与列表之间（y 204）
    gap_y = 204 if drum else 244
    bg, border, panel = px[640, 180], px[640, 193], px[640, gap_y]
    list_y = LIST_Y_DRUM if drum else LIST_Y_SYNTH
    bar = (px[900, list_y + hl_row * ROW_H + 4], px[900, list_y + 4] if hl_row != 0 else px[900, list_y + ROW_H + 4])
    chip_accent = count((176, 194, 1104, 214) if drum else CHIP_ROW, is_accent)
    hint = ink_bbox((LIST_X, list_y + ROWS_VISIBLE * ROW_H, LIST_X + LIST_W, PANEL[3] - 2))
    out = [
        (bg != panel and bg != border and px[300, gap_y] == panel and px[1000, gap_y] == panel,
         f"面板上方 (640,180)={bg} 不是面板底/描边；面板底 {panel} 在内部均匀"),
        (px[640, 193] == border and px[161, 400] == border and px[1118, 400] == border
         and px[640, 630] == border and border != panel and border != bg,
         f"四边 2px 描边同色 {border}（≠面板底、≠背景）"),
        (bar[0] == border and bar[0] != panel, f"第 {hl_row} 行有高亮条 {bar[0]}（= UI_PANEL_HI，同描边；面板底 {panel}）"),
        (bar[1] == panel, f"非高亮行无条 {bar[1]}"),
        ((chip_accent > 300) if not drum else (chip_accent == 0),
         f"chip 行强调色像素 {chip_accent}（合成轨 > 300 = 一个高亮 chip；鼓轨面板顶带 0 = 没有 chip 行）"),
        (hint is not None and hint[2] <= LIST_X + LIST_W - 8 and hint[1] >= list_y + ROWS_VISIBLE * ROW_H,
         f"提示行墨迹 {hint} 落在面板内（右缘 ≤ {LIST_X + LIST_W - 8}）"),
    ]
    if not drum:
        cur_accent = count((LIST_X, list_y, LIST_X + 400, list_y + ROWS_VISIBLE * ROW_H), is_accent)
        out.append(((cur_accent > 60) if has_current else (cur_accent == 0),
                    f"列表区当前项的强调色文字像素 {cur_accent}（{'> 60' if has_current else '预览态：没有当前项 ⇒ 0'}）"))
    return out


def main():
    port, log_path, png_dir = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    png_dir.mkdir(parents=True, exist_ok=True)
    c = Console(port, log_path)
    ck = Checks()
    c.note(f"port={port}")
    try:
        # 开机日志进 transcript（App version 等）
        c.reset()
        c.cmd("", settle=0.5)
        c.cmd("STORE AUTO OFF")
        boot = c.cmd("ENG SNAP")
        m = RE_SNAPHEAD.search(boot)
        sel0 = m.group(2)
        rows0 = eng_rows(c)
        set0 = grab(c.cmd("STORE SETTINGS INFO"), RE_SETTINGS, "STORE SETTINGS INFO")
        st0 = grab(c.cmd("STORE STATS"), RE_STATS_STORE, "STORE STATS")
        c.note(f"基线：sel={sel0} bpm={m.group(1)} zoom={set0.group(1)} dirty={set0.group(2)} "
               f"autosave={st0.group(1)} autosave_hash={st0.group(2)}；四轨：{' | '.join('/'.join(r) for r in rows0)}")
        ck.ok(rows0[0][0] == "0" and rows0[0][1] == "0" and rows0[0][2] == "0",
              f"基线轨 0 = ANALOG 预置 0（{' '.join(rows0[0][:3])}）")

        # ---- base：选轨 0 → 进 SOUND 页；池 / 内部堆基线 ----
        select_track(c, 0)
        ck.ok(re.search(r"selected_track=0 ", c.cmd("STATE SNAP")) is not None, "Ctrl+1 后 selected_track=0")
        heap0 = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        c.cmd("UI sound")
        p6 = RE_PAGE6.search(c.cmd("UI MEM"))
        pool0 = pool_free(c)
        c.note(f"进页前 {heap0.group(0)}；{p6.group(0)}；pool_free={pool0}")
        ck.ok(p6 is not None and p6.group(1) == "built", f"SOUND 页已建：{p6.group(0) if p6 else '无行'}")

        # ---- engine：row 0 = 引擎列表 + chip 行 ----
        t = open_row(c, 160)
        mo = grab(t, RE_TAP_OPEN, "pick open")
        ck.ok(mo.group(4) == "0", f"{mo.group(0)}")
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("0", "0", "0", "12"),
              f"row 0：{p.group(0) if p else '无'}（期望 open=1 row=0 engine=0 highlight=0 rows=12）")
        n0 = pick_n(c, 0)
        ck.ok(n0 is not None and n0.group(2) == "ANALOG" and n0.group(3) == "1"
              and n0.group(4) == "176" and n0.group(5) == "248" and n0.group(6) == "928" and n0.group(7) == "32",
              f"{n0.group(0) if n0 else '无'}（ANALOG，当前项，矩形 x=176 y=248 w=928 h=32）")
        pool_open = pool_free(c)
        ck.ok(0 < pool0 - pool_open <= 4096, f"池：打开前 pool_free={pool0} → 打开后 {pool_open}（差 {pool0 - pool_open}B，0 < 差 ≤ 4,096）")
        png_engine = png_dir / "task-6-pick-engine.png"
        c.screenshot(png_engine)
        for cond, what in panel_pixels(png_engine, drum=False, hl_row=0):
            ck.ok(cond, f"截屏 task-6-pick-engine.png：{what}")
        for n, name in ((8, "GRAIN"), (9, "FM6"), (11, "NOISE")):
            r = pick_n(c, n)
            ck.ok(r is not None and r.group(2) == name and r.group(3) == "0", f"{r.group(0) if r else '无'}")
        # 第 11 行（第 12 个）在一屏 11 行之外：矩形查询把它滚进来（列表上移一行），之后高亮再变才回去
        r = pick_n(c, 11)
        ck.ok(r is not None and int(r.group(5)) == 248 + 10 * ROW_H, f"{r.group(0) if r else '无'}（滚入后 y=568）")
        r = pick_n(c, 0)
        ck.ok(r is not None and int(r.group(5)) == 248, f"第 0 行的查询把列表滚回顶部：{r.group(0) if r else '无'}")
        ck.ok("absent" in c.cmd("UI SOUND PICK 12"), "UI SOUND PICK 12 ⇒ UI ERR sound (absent)")
        ck.ok("(invalid)" in c.cmd("UI SOUND PICK x"), "UI SOUND PICK x ⇒ UI ERR sound (invalid)")
        ck.ok(RE_PAGE.search(c.cmd("UI")).group(2) == "6", "选择器打开时仍是 SOUND 页")
        f = RE_FOCUS.search(c.cmd("UI SOUND FOCUS"))
        ck.ok(f is not None, f"卡片的焦点槽照常可读：{f.group(0) if f else '无'}")
        s = RE_STATS.search(c.cmd("UI SOUND STATS"))
        ck.ok(s is not None and s.group(1) == "109" and s.group(3) == "0000", f"卡片统计不受影响：{s.group(0) if s else '无'}")
        # 键盘：→ ×9 ⇒ chip 高亮 FM6（9），不动引擎；ENTER ⇒ CMD_SET_ENGINE 并关闭
        for _ in range(9):
            key(c, KEY_RIGHT)
        p = pick(c)
        ck.ok(p is not None and p.group(3) == "9", f"→ ×9：{p.group(0) if p else '无'}（highlight=9）")
        ck.ok(eng_rows(c)[0][1] == "0", "→ 只移动高亮：轨 0 引擎仍是 0")
        png_chips = png_dir / "task-6-pick-engine-chip-hl.png"
        c.screenshot(png_chips)
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "9" and rows[0][2] == "0", f"ENTER ⇒ 轨 0 换到 FM6：{' '.join(rows[0][:3])}")
        t = c.cmd("UI SOUND PICK")
        ck.ok("UI SOUND PICK open=0" in t, f"ENTER 后选择器关闭：{result_line(t)}")
        # TAP 行选引擎：再开 row 0（当前 FM6），TAP GRAIN 行 ⇒ engine=8
        open_row(c, 160)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("0", "9", "9", "12"), f"FM6 下再开 row 0：{p.group(0) if p else '无'}")
        rect, t = tap_row(c, 8)
        mp = grab(t, RE_TAP_PICK, "pick engine")
        ck.ok(mp.group(4) == "engine" and mp.group(5) == "8" and mp.group(6) == "CMD_SET_ENGINE", f"{mp.group(0)}")
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "8", f"轨 0 引擎 = GRAIN：{' '.join(rows[0][:3])}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "TAP 行选后选择器关闭")
        # chip：第一次点 = 只高亮；再点 = 确认。当前 GRAIN，点 DIGITAL chip（中心 x=324, y=220）
        open_row(c, 160)
        cx = CHIP_X[1] + (11 * len(ENGINE_NAMES[1]) + 24) // 2
        t = c.cmd(f"TAP {cx} 220")
        mp = grab(t, RE_TAP_PICK, "pick engine highlight")
        ck.ok(mp.group(4) == "engine" and mp.group(5) == "1" and mp.group(6) == "highlight", f"{mp.group(0)}")
        p = pick(c)
        ck.ok(p is not None and p.group(3) == "1" and p.group(1) == "0", f"{p.group(0) if p else '无'}")
        ck.ok(eng_rows(c)[0][1] == "8", "chip 第一次点不下发：轨 0 引擎仍是 GRAIN")
        t = c.cmd(f"TAP {cx} 220")
        mp = grab(t, RE_TAP_PICK, "pick engine cmd")
        ck.ok(mp.group(4) == "engine" and mp.group(5) == "1" and mp.group(6) == "CMD_SET_ENGINE", f"{mp.group(0)}")
        ck.ok(eng_rows(c)[0][1] == "1", "chip 再点一次 ⇒ 轨 0 引擎 = DIGITAL")
        # ← ×1 + ENTER 回 ANALOG；当前项上 TAP = 不下发
        open_row(c, 160)
        key(c, KEY_LEFT)
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "0" and rows[0][2] == "0", f"← + ENTER ⇒ 回 ANALOG：{' '.join(rows[0][:3])}")
        open_row(c, 160)
        rect, t = tap_row(c, 0)
        mp = grab(t, RE_TAP_PICK, "pick engine unchanged")
        ck.ok(mp.group(4) == "engine" and mp.group(5) == "0" and mp.group(6) == "unchanged", f"{mp.group(0)}")
        ck.ok(eng_rows(c)[0][1] == "0", "确认当前项不下发：轨 0 引擎仍是 ANALOG")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "确认当前项后选择器关闭")

        # ---- 修复轮 3：row 0 的列表焦点（↓ / 页级 ± 之后）ENTER = 选中高亮的引擎（上一轮被预览规则误拒）----
        open_row(c, 160)
        key(c, KEY_DOWN)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("0", "0", "1", "12"), f"row 0 ↓：{p.group(0) if p else '无'}（高亮 DIGITAL）")
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "1" and rows[0][2] == "0", f"row 0 ↓ + ENTER ⇒ 用高亮的引擎：{' '.join(rows[0][:3])}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "ENTER 后选择器关闭")
        open_row(c, 160)
        rect, t = tap_row(c, 0)
        mp = grab(t, RE_TAP_PICK, "pick engine")
        ck.ok(mp.group(5) == "0" and mp.group(6) == "CMD_SET_ENGINE" and eng_rows(c)[0][1] == "0", f"还原：{mp.group(0)}")
        open_row(c, 160)
        t = c.cmd("TAP 1136 132")  # 页级 −：列表高亮下一行（0 → 1；+ 才是上一行）
        mp = grab(t, RE_TAP_PICK, "pick engine highlight")
        ck.ok(mp.group(4) == "engine" and mp.group(5) == "1" and mp.group(6) == "highlight", f"row 0 页级 −：{mp.group(0)}")
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "1", f"row 0 页级 ± + ENTER ⇒ 用高亮的引擎：{' '.join(rows[0][:3])}")
        open_row(c, 160)
        tap_row(c, 0)
        ck.ok(eng_rows(c)[0][1] == "0", "还原：轨 0 引擎回 ANALOG")

        # ---- preset：row 1 = 当前引擎（ANALOG，22 个）的预置列表 ----
        t = open_row(c, 512)
        mo = grab(t, RE_TAP_OPEN, "pick open row 1")
        ck.ok(mo.group(4) == "1", f"{mo.group(0)}")
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "0", "0", "22"), f"row 1：{p.group(0) if p else '无'}")
        n0 = pick_n(c, 0)
        ck.ok(n0 is not None and n0.group(2).startswith(" 1  ") and n0.group(3) == "1", f"{n0.group(0) if n0 else '无'}")
        rect, t = tap_row(c, 3)
        mp = grab(t, RE_TAP_PICK, "pick preset")
        ck.ok(mp.group(4) == "preset" and mp.group(5) == "3" and mp.group(6) == "CMD_SET_PRESET", f"{mp.group(0)}")
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "0" and rows[0][2] == "3", f"轨 0 预置 = 3：{' '.join(rows[0][:3])}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "TAP 行选预置后选择器关闭")
        # 再开：高亮 = 当前预置 3；↓ ×2 + ENTER ⇒ 预置 5
        open_row(c, 512)
        r = pick_n(c, 3)
        ck.ok(r is not None and r.group(3) == "1", f"再开 row 1：{r.group(0) if r else '无'}")
        png_preset = png_dir / "task-6-pick-preset.png"
        c.screenshot(png_preset)
        for cond, what in panel_pixels(png_preset, drum=False, hl_row=3):
            ck.ok(cond, f"截屏 task-6-pick-preset.png：{what}")
        key(c, KEY_DOWN)
        key(c, KEY_DOWN)
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        ck.ok(rows[0][2] == "5", f"↓ ↓ ENTER ⇒ 轨 0 预置 = 5：{' '.join(rows[0][:3])}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "ENTER 后选择器关闭")
        # ESC：关闭且不切页
        open_row(c, 512)
        ck.ok(pick(c) is not None, "再开 row 1")
        key(c, KEY_ESC)
        t = c.cmd("UI SOUND PICK")
        ck.ok("UI SOUND PICK open=0" in t, f"ESC ⇒ 关闭：{result_line(t)}")
        pg = RE_PAGE.search(c.cmd("UI"))
        ck.ok(pg is not None and pg.group(2) == "6", f"ESC 不切页：{pg.group(0) if pg else '无'}")
        # 滚入视口：第 21 行（0 起）先滚进来，矩形落在列表里
        open_row(c, 512)
        r = pick_n(c, 21)
        ck.ok(r is not None and 248 <= int(r.group(5)) and int(r.group(5)) + 32 <= 248 + 11 * 32,
              f"{r.group(0) if r else '无'}（y 在列表 248..600 内）")
        r = pick_n(c, 0)
        ck.ok(r is not None and int(r.group(5)) == 248, f"回到第 0 行：{r.group(0) if r else '无'}")
        # ---- 修复轮 1：面板空白处 = 真 miss（不执行、不落到被盖住的卡片）----
        f0 = RE_FOCUS.search(c.cmd("UI SOUND FOCUS")).group(1)
        for x, y, what in ((640, 616, "提示行"), (269, 220, "chip 间隙")):
            t = c.cmd(f"TAP {x} {y}")
            ck.ok(RE_TAP_MISS.search(t) is not None and "page=6" in t, f"面板空白（{what}）TAP {x},{y} ⇒ {result_line(t)}")
        ck.ok(RE_FOCUS.search(c.cmd("UI SOUND FOCUS")).group(1) == f0, "空白处 TAP 没有落到被盖住的卡片（焦点槽不变）")
        ck.ok(pick(c) is not None, "空白处 TAP 后面板仍打开")
        # 修复轮 2 Minor a：只认 PICK / PICK <n> / PICK press|release <x> <y>，其余形式一律用法错误（rc 1）
        for bad in ("UI SOUND PICK press 640", "UI SOUND PICK a b c", "UI SOUND PICK 0 0", "UI SOUND PICK 0 x"):
            t = c.cmd(bad)
            ck.ok("用法: UI SOUND" in t and "UI SOUND PICK open=1" not in t and "n=0 text" not in t,
                  f"`{bad}` ⇒ 用法错误（{result_line(t)[:40]}…）")
        ck.ok(pick(c) is not None, "用法错误不动选择器")
        # 修复轮 2 Minor b：没有在前的按下点 ⇒ 裸 release 不算点击（open() 已清掉上一次的按下点）
        rows = eng_rows(c)
        t = c.cmd("UI SOUND PICK release 640 424")
        ck.ok(RE_TOUCH.search(t) is not None and RE_TOUCH.search(t).group(4) == "ignored", f"裸 release：{result_line(t)}")
        ck.ok(eng_rows(c) == rows and pick(c) is not None, "裸 release 不发命令、面板仍打开")
        # 滑过的松手不算点击：按下第 3 行、在右移 60px 处松手 ⇒ moved，不发命令；同点按下/松手 = 命中
        r = pick_n(c, 3)
        y3 = int(r.group(5)) + 16
        t = c.cmd(f"UI SOUND PICK press 640 {y3}")
        ck.ok(RE_TOUCH.search(t) is not None and RE_TOUCH.search(t).group(4) == "hit", f"按下第 3 行：{result_line(t)}")
        t = c.cmd(f"UI SOUND PICK release 700 {y3}")
        ck.ok(RE_TOUCH.search(t) is not None and RE_TOUCH.search(t).group(4) == "moved", f"右移 60px 松手：{result_line(t)}")
        t = c.cmd("UI SOUND PICK release 640 360")
        ck.ok(RE_TOUCH.search(t) is not None and RE_TOUCH.search(t).group(4) == "ignored", f"用过即清：再 release ⇒ {result_line(t)}")
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "0" and rows[0][2] == "5", f"滑过的松手不发命令：{' '.join(rows[0][:3])}")
        ck.ok(pick(c) is not None, "滑过后面板仍打开")
        r = pick_n(c, 5)
        y5 = int(r.group(5)) + 16
        c.cmd(f"UI SOUND PICK press 640 {y5}")
        t = c.cmd(f"UI SOUND PICK release 640 {y5}")
        ck.ok(RE_TOUCH.search(t) is not None and RE_TOUCH.search(t).group(4) == "hit", f"同点按下/松手（当前预置行）：{result_line(t)}")
        rows = eng_rows(c)
        ck.ok(rows[0][2] == "5" and "UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), f"同点松手 = 命中当前行（不下发、关闭）：{' '.join(rows[0][:3])}")

        # ---- R16 预览：chip 高亮 ≠ 当前引擎 ⇒ 列表只读、不标当前项 ----
        open_row(c, 512)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "0", "0", "22"), f"再开 row 1（ANALOG，预置 5）：{p.group(0) if p else '无'}")
        t = c.cmd(f"TAP {CHIP_X[1] + (11 * len(ENGINE_NAMES[1]) + 24) // 2} 220")
        mp = grab(t, RE_TAP_PICK, "pick engine highlight")
        ck.ok(mp.group(4) == "engine" and mp.group(5) == "1" and mp.group(6) == "highlight", f"{mp.group(0)}（DIGITAL chip 只高亮）")
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "0", "1", "22"), f"{p.group(0) if p else '无'}（engine=0 highlight=1 rows=22）")
        r = pick_n(c, 5)
        ck.ok(r is not None and r.group(3) == "0" and r.group(2).startswith(" 6  "), f"{r.group(0) if r else '无'}（与当前预置同下标 5，预览态 current=0）")
        png_prev = png_dir / "task-6-fix1-pick-preview.png"
        c.screenshot(png_prev)
        for cond, what in panel_pixels(png_prev, drum=False, hl_row=0, has_current=False):
            ck.ok(cond, f"截屏 task-6-fix1-pick-preview.png：{what}")
        # 提示行随预览态换文案：普通 row 1 的提示墨迹右缘 882（task-6-pick-preset.png），预览态的更短
        from PIL import Image as _I
        _px = _I.open(png_prev).convert("RGB").load()
        _xs = [x for y in range(600, 630) for x in range(176, 1104) if sum(_px[x, y]) > 300]
        ck.ok(_xs and max(_xs) != 882 and max(_xs) <= 1096, f"预览态提示行换成只读预览文案（墨迹右缘 {max(_xs) if _xs else None}：≠ 普通 row 1 的 882，且 ≤ 1096 未被裁）")
        rect, t = tap_row(c, 5)
        mp = grab(t, RE_TAP_PREVIEW, "pick preview")
        ck.ok(mp.group(4) == "1", f"{mp.group(0)}")
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "0" and rows[0][2] == "5", f"预览时点预置行不下发：{' '.join(rows[0][:3])}")
        ck.ok(pick(c) is not None, "预览时点预置行后面板保持打开")
        # R16（修复轮 2）：焦点在列表（刚 ↓ 过）时预览态的 ENTER 什么都不做；→ 一次把焦点移回 chip 行后 ENTER = 换引擎
        key(c, KEY_DOWN)
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        p = pick(c)
        ck.ok(p is not None and p.group(1) == "1" and rows[0][1] == "0" and rows[0][2] == "5",
              f"预览态、焦点在列表：ENTER 不动作 ⇒ {p.group(0) if p else '无'}；{' '.join(rows[0][:3])}")
        key(c, KEY_RIGHT)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "0", "2", "8"), f"→ 一次：焦点回 chip 行（高亮 LOFI）⇒ {p.group(0) if p else '无'}")
        key(c, KEY_ENTER)
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "2" and rows[0][2] == "0", f"焦点在 chip 行：ENTER ⇒ 换引擎：{' '.join(rows[0][:3])}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "ENTER 后选择器关闭")
        # 现在是 LOFI 预置 0：← 进入预览（DIGITAL）、←（ANALOG）、← 环绕 NOISE、→ ×3 回 LOFI；页级 + = 列表高亮上一行
        open_row(c, 512)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "2", "2", "8"), f"再开 row 1（LOFI）：{p.group(0) if p else '无'}")
        key(c, KEY_LEFT)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "2", "1", "22"), f"← ⇒ {p.group(0) if p else '无'}")
        key(c, KEY_LEFT)
        key(c, KEY_LEFT)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "2", "11", "11"), f"← ×2 环绕到 NOISE ⇒ {p.group(0) if p else '无'}")
        key(c, KEY_RIGHT)
        key(c, KEY_RIGHT)
        key(c, KEY_RIGHT)
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("1", "2", "2", "8"), f"→ ×3 回 LOFI ⇒ {p.group(0) if p else '无'}")
        t = c.cmd("TAP 1224 132")
        mp = grab(t, RE_TAP_PICK, "pick preset highlight")
        ck.ok(mp.group(4) == "preset" and mp.group(5) == "7" and mp.group(6) == "highlight", f"页级 +：{mp.group(0)}")
        rect, t = tap_row(c, 0)
        mp = grab(t, RE_TAP_PICK, "pick preset unchanged")
        ck.ok(mp.group(4) == "preset" and mp.group(5) == "0" and mp.group(6) == "unchanged", f"{mp.group(0)}")
        ck.ok(eng_rows(c)[0][2] == "0", "确认当前预置不下发：轨 0 预置仍是 0")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "确认当前预置后选择器关闭")

        # ---- 修复轮 1：载入用户预置后工厂预置 0 不算当前项 ----
        empty = [int(k) for k in RE_PRESET_EMPTY.findall(c.cmd("STORE PRESET LIST"))]
        ck.ok(len(empty) > 0, f"有空的用户预置槽（{len(empty)} 个）")
        slot = empty[0]
        t = c.cmd(f"STORE PRESET SAVE {slot}")
        ck.ok(f"PRESET {slot} SAVED" in t, f"存轨 0 的声音到用户预置槽 {slot}：{result_line(t)}")
        t = c.cmd(f"STORE PRESET LOAD {slot} 0")
        ck.ok(f"PRESET {slot} LOADED track=0" in t, f"载入到轨 0：{result_line(t)}")
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "2" and rows[0][2] == "0", f"载入用户预置后 t->preset = 0：{' '.join(rows[0][:3])}")
        open_row(c, 512)
        r = pick_n(c, 0)
        ck.ok(r is not None and r.group(3) == "0", f"{r.group(0) if r else '无'}（用户预置在位：工厂预置 0 不再是当前项）")
        png_user = png_dir / "task-6-fix1-user-preset.png"
        c.screenshot(png_user)
        rect, t = tap_row(c, 0)
        mp = grab(t, RE_TAP_PICK, "pick preset")
        ck.ok(mp.group(4) == "preset" and mp.group(5) == "0" and mp.group(6) == "CMD_SET_PRESET", f"{mp.group(0)}")
        rows = eng_rows(c)
        ck.ok(rows[0][1] == "2" and rows[0][2] == "0", f"选择工厂预置 0：{' '.join(rows[0][:3])}")
        open_row(c, 512)
        r = pick_n(c, 0)
        ck.ok(r is not None and r.group(3) == "1", f"再开：{r.group(0) if r else '无'}（工厂预置 0 = 当前项 ⇒ user 已清）")
        png_fac = png_dir / "task-6-fix1-factory-preset.png"
        c.screenshot(png_fac)

        # 页眉 PRESET 名（强调色、右对齐止于 x=650）：用户预置名 "USER nn" → 工厂预置名，两张截屏的墨迹必须不同
        def header_name_ink(png):
            from PIL import Image

            im = Image.open(png).convert("RGB")
            px = im.load()
            return {(x, y) for y in range(78, 106) for x in range(380, 652)
                    if px[x, y][0] > 180 and px[x, y][1] > 140 and px[x, y][2] < 90}

        ink_user, ink_fac = header_name_ink(png_user), header_name_ink(png_fac)
        diff = len(ink_user ^ ink_fac)
        ck.ok(len(ink_user) > 20 and len(ink_fac) > 20 and diff > 30,
              f"页眉 PRESET 名从用户预置名换成工厂预置名（强调色墨迹 {len(ink_user)} → {len(ink_fac)} 像素，不同 {diff}）")
        key(c, KEY_ESC)
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "ESC 关闭")
        t = c.cmd(f"STORE PRESET ERASE {slot}")
        ck.ok(f"PRESET {slot} ERASED" in t, f"还原用户预置槽 {slot}：{result_line(t)}")
        ck.ok(f"PRESET {slot} EMPTY" in c.cmd("STORE PRESET LIST"), f"槽 {slot} 回到 EMPTY")

        # ---- context：上下文变化关闭 ----
        c.cmd("UI sound")
        open_row(c, 160)
        ck.ok(pick(c) is not None, "开 row 0")
        select_track(c, 1)
        t = c.cmd("UI SOUND PICK")
        ck.ok("UI SOUND PICK open=0" in t, f"Ctrl+2 后：{result_line(t)}")
        ck.ok(re.search(r"selected_track=1 ", c.cmd("STATE SNAP")) is not None, "Ctrl+2 后 selected_track=1")
        open_row(c, 160)
        ck.ok(pick(c) is not None, "轨 1 上再开 row 0")
        t = c.cmd("TAP 1150 92")
        mt = RE_TRACKSEL.search(t)
        ck.ok(mt is not None and mt.group(1) == "2", f"选轨条：{mt.group(0) if mt else result_line(t)}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "选轨条换轨后选择器关闭")
        select_track(c, 0)
        # 切页：面板不跨页存活
        open_row(c, 160)
        ck.ok(pick(c) is not None, "开 row 0 后切页")
        c.cmd("UI tracks")
        t = c.cmd("UI SOUND PICK")
        ck.ok("(not_dashboard)" in t, f"TRACKS 页：{result_line(t)}")
        c.cmd("UI sound")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "切回 SOUND：选择器没有跨页存活")
        ck.ok(RE_PAGE.search(c.cmd("UI")).group(2) == "6", "仍在 SOUND 页")

        # ---- drum：row 0 = kit 列表（46）----
        select_track(c, 3)
        t = c.cmd("TAP 512 92")
        ck.ok(RE_TAP_MISS.search(t) is not None, f"鼓轨没有 PRESET 行：{result_line(t)}")
        t = open_row(c, 160)
        mo = grab(t, RE_TAP_OPEN, "pick open drum")
        ck.ok(mo.group(4) == "0", f"{mo.group(0)}")
        p = pick(c)
        ck.ok(p is not None and p.groups() == ("0", "0", "0", "46"), f"鼓轨：{p.group(0) if p else '无'}")
        k0 = pick_n(c, 0)
        ck.ok(k0 is not None and k0.group(2) == "ACOUSTIC" and k0.group(3) == "1" and k0.group(5) == "216",
              f"{k0.group(0) if k0 else '无'}")
        par = grab(c.cmd(f"PARAM {P_E0}"), RE_PARAM, "PARAM 63")
        ck.ok(par.group(1) == "0", f"基线 kit：{par.group(0)}")
        png_kit = png_dir / "task-6-pick-kit.png"
        c.screenshot(png_kit)
        for cond, what in panel_pixels(png_kit, drum=True, hl_row=0):
            ck.ok(cond, f"截屏 task-6-pick-kit.png：{what}")
        rect, t = tap_row(c, 1)
        mp = grab(t, RE_TAP_PICK, "pick kit")
        ck.ok(mp.group(4) == "kit" and mp.group(5) == "1" and mp.group(6) == "CMD_SET_PARAM", f"{mp.group(0)}")
        par = grab(c.cmd(f"PARAM {P_E0}"), RE_PARAM, "PARAM 63")
        ck.ok(par.group(1) == "1", f"kit 选择写进鼓轨 P_E0：{par.group(0)}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "kit 选择后选择器关闭")
        open_row(c, 160)
        r = pick_n(c, 1)
        ck.ok(r is not None and r.group(3) == "1", f"再开：高亮 = 当前 kit：{r.group(0) if r else '无'}")
        key(c, KEY_DOWN)
        key(c, KEY_DOWN)
        key(c, KEY_ENTER)
        par = grab(c.cmd(f"PARAM {P_E0}"), RE_PARAM, "PARAM 63")
        ck.ok(par.group(1) == "3", f"鼓轨 ↓ ↓ ENTER ⇒ 用高亮的 kit，P_E0 = 3：{par.group(0)}")
        ck.ok("UI SOUND PICK open=0" in c.cmd("UI SOUND PICK"), "鼓轨 ENTER 后选择器关闭")
        for _ in range(3):  # 还原 kit：KIT 行 − ×3
            c.cmd("TAP 37 92")
        par = grab(c.cmd(f"PARAM {P_E0}"), RE_PARAM, "PARAM 63")
        ck.ok(par.group(1) == "0", f"还原 kit：{par.group(0)}")

        # ---- pool：开 / 关 ×5 ----
        select_track(c, 0)
        before = pool_free(c)
        worst_open, worst_close = 0, 0
        for i in range(5):
            open_row(c, 512 if i % 2 else 160)
            while_open = pool_free(c)
            worst_open = max(worst_open, before - while_open)
            key(c, KEY_ESC)
            after = pool_free(c)
            worst_close = max(worst_close, abs(after - before))
            c.note(f"往返 {i + 1}：before={before} open={while_open} after={after}")
        ck.ok(0 < worst_open <= 4096, f"×5 打开时的池差最大 {worst_open}B（0 < 差 ≤ 4,096）")
        ck.ok(worst_close <= 128, f"×5 关闭后 pool_free 与打开前差最大 {worst_close}B（≤ 128）")
        heap1 = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        ck.ok(int(heap1.group(2)) >= 61440, f"{heap1.group(0)}（internal_min ≥ 61,440）")
        p6 = RE_PAGE6.search(c.cmd("UI MEM"))
        c.note(f"收尾 {p6.group(0)}；进页前 {heap0.group(0)} → 收尾 {heap1.group(0)}")
        # 未打开时的读：not_built
        ck.ok("(not_built)" in c.cmd("UI SOUND PICK 0"), "未打开时 UI SOUND PICK 0 ⇒ UI ERR sound (not_built)")

        # ---- restore：硬复位 = 重新载入 autosave 对象 ----
        c.note("收尾：硬复位重新载入 autosave 对象")
        c.reset()
        c.cmd("", settle=0.5)
        boot = c.cmd("ENG SNAP")
        m = RE_SNAPHEAD.search(boot)
        rows1 = eng_rows(c)
        set1 = grab(c.cmd("STORE SETTINGS INFO"), RE_SETTINGS, "STORE SETTINGS INFO")
        st1 = grab(c.cmd("STORE STATS"), RE_STATS_STORE, "STORE STATS")
        c.note(f"复位后：sel={m.group(2)} bpm={m.group(1)} zoom={set1.group(1)} dirty={set1.group(2)} "
               f"autosave={st1.group(1)} autosave_hash={st1.group(2)} autosave_writes={st1.group(3)}；"
               f"四轨：{' | '.join('/'.join(r) for r in rows1)}")
        ck.ok(m.group(2) == sel0 and rows1 == rows0, "复位后 sel 与四轨 ENG SNAP 行与基线逐字相同")
        ck.ok(set1.group(1) == set0.group(1) and set1.group(2) == set0.group(2),
              f"复位后 settings zoom/dirty 与基线相同（{set1.group(1)}/{set1.group(2)}）")
        ck.ok(st1.group(3) == st0.group(3) and st1.group(2) == st0.group(2),
              f"复位后 autosave_hash={st1.group(2)} autosave_writes={st1.group(3)} 与基线相同")
    finally:
        c.close()
    total = ck.n - ck.failed
    print(f"{NOTE} {'PASS' if ck.failed == 0 else 'FAIL'} {total}/{ck.n}", flush=True)
    with log_path.open("a", encoding="utf-8") as f:
        f.write(f"{NOTE} {'PASS' if ck.failed == 0 else 'FAIL'} {total}/{ck.n}\n")
    return 0 if ck.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
