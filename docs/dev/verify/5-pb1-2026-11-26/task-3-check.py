#!/usr/bin/env python3
"""5B-1 Task 3：Sound 仪表盘的槽值/锁定/拖动改值/滚动/TAP 设备核对（证据脚本，随证据包入库）。

Usage:
    python3 task-3-check.py PORT TRANSCRIPT.log

写法照 scripts/selftest-heap.py：pyserial、BAUD 115200、读到 console 提示符（sloop>）为止；
串口字节逐字节写 TRANSCRIPT（原始回显，不裁剪），工具注记一律 `# [check] ...` 前缀。
开头 esptool 硬复位（开机日志进 transcript），`STORE AUTO OFF` 后才动手；收尾再硬复位 =
重新载入 autosave 对象，本脚本的参数改动不落盘。

相位：base（选轨 0/ANALOG、存在性、池/内部堆）→ labels（13 卡全部槽的 label 对照 `ENG DESC`）
→ drag（Voice·LVL：press 不改值、drag 改值、ENG SNAP 相符、锚点/反向、环带外）
→ ctx（INSERT TYPE / ENV SYNC 换标签 = 上下文键）→ lock（Ctrl+4 鼓轨：锁定、drum_ok、GLOBAL、
不存在的槽）→ freeze → scroll（SCROLL 400、矩形上移、TAP 命中、折叠位不变）→ guard（非仪表盘页）。
全部断言通过退出码 0，末行 `# [check] PASS n/n`。
"""
import re
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts"))
import screenshot as shot  # noqa: E402

BAUD = 115200
NOTE = "# [check]"
SLOT_COUNTS = [4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7]
RE_SLOT = re.compile(r"UI SOUND SLOT card=(\d+) slot=(\d+) flat=(\d+) present=(\d) scope=(\S+) "
                     r"id=(\S+) label=\"([^\"]*)\" locked=(\d) value=(-?\d+) x=(-?\d+) y=(-?\d+) "
                     r"w=(-?\d+) h=(-?\d+)")
RE_KNOB = re.compile(r"UI SOUND KNOB flat=(\d+) (press|drag) dx=(-?\d+) dy=(-?\d+) "
                     r"(?:value=(-?\d+)|ignored \((ring band|locked)\))")
RE_SCROLL = re.compile(r"UI SOUND SCROLL y=(-?\d+) max=(\d+)")
RE_STATS = re.compile(r"UI SOUND STATS cards=(\d+) slots=(\d+) scroll_max=(-?\d+) collapsed=0x([0-9A-F]{4})")
RE_ENG = re.compile(r"ENG SNAP trk k=(\d) engine=(\d+) preset=(\d+) level=(\d+)")
RE_DESC = re.compile(r"ENG DESC scope=(\S+) (?:engine|track)=(\d+) id=(\d+) label=\"([^\"]*)\"")
RE_TAPF = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d+) -> SOUND focus card=(\d+) slot=(\d+) flat=(\d+) "
                     r"CMD_(SET_PARAM|SET_GLOBAL)( locked=1)?")
RE_TAPC = re.compile(r"TAP (-?\d+),(-?\d+) page=(\d+) -> SOUND card=(\d+) collapsed=(\d)")
RE_HEAP = re.compile(r"ENG MEM internal_free=(\d+) internal_min=(\d+)")
RE_PAGE6 = re.compile(r"UI MEM page id=6 title=SOUND state=(\S+) est=(\d+) delta=(-?\d+) build_ms=(\d+)")
RE_BUILD = re.compile(r"页面构建 page=(\S+)\((\d+)\) 池增量=(-?\d+)B 预算=(\d+)B 池剩余=(-?\d+)B 耗时=(\d+)ms")


class Console:
    def __init__(self, port, log_path):
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
        raise SystemError(f"{NOTE} FAIL: 响应里没有 {what}")
    return m


def select_track(c, k):
    """Ctrl（0x40，SEL 层）+ 数字键 k+1：写法照 inject.py 的 key_with_layer（逐条，down 都配 up）。"""
    for code, ev in ((0x40, "down"), (k + 1, "down"), (k + 1, "up"), (0x40, "up")):
        c.cmd(f"KEY 0x{code:02X} {ev}")


def slot(c, card, s):
    return RE_SLOT.search(c.cmd(f"UI SOUND CARD {card} SLOT {s}"))


def knob(c, flat, op, dx, dy):
    return RE_KNOB.search(c.cmd(f"UI SOUND KNOB {flat} {op} {dx} {dy}"))


def eng_rows(c):
    return re.findall(r"ENG SNAP trk k=\d+ engine=\d+ preset=\d+ level=\d+ mute=\d+ armed=\d+ solo=\d+ "
                      r"len=\d+ div=\d+ voices=\d+", c.cmd("ENG SNAP"))


def main():
    port, log_path = sys.argv[1], Path(sys.argv[2])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    c = Console(port, log_path)
    ck = Checks()
    c.note(f"port={port}")
    try:
        # 开机日志进 transcript（App version 等）
        c.ser.close()
        shot.reset_device(port, BAUD, on_chunk=c.log.write)
        c.log.flush()
        c.ser = serial.Serial(port, BAUD, timeout=0.05)
        c.cmd("", settle=0.5)
        c.cmd("STORE AUTO OFF")
        select_track(c, 0)

        # ---- base ----
        st = c.cmd("STATE SNAP")
        ck.ok(re.search(r"selected_track=0 ", st) is not None, "Ctrl+1 后 selected_track=0")
        rows = eng_rows(c)
        ck.ok(len(rows) == 4 and rows[0].startswith("ENG SNAP trk k=0 engine=0 "), f"轨 0 引擎 ANALOG：{rows[0]}")
        before = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        text = c.cmd("UI sound")
        bm = grab(text, RE_BUILD, "页面构建")
        c.note(f"SOUND 首建 池增量={bm.group(3)}B 预算={bm.group(4)}B 池剩余={bm.group(5)}B 耗时={bm.group(6)}ms")
        s48 = slot(c, 4, 8)
        ck.ok(s48 is not None and s48.group(4) == "1" and s48.group(7) == "TYPE",
              f"CARD 4 SLOT 8 ⇒ present=1 label=TYPE：{s48.group(0) if s48 else None}")
        s13 = slot(c, 1, 3)
        ck.ok(s13 is not None and s13.group(4) == "1" and s13.group(7) == "ROLL",
              f"CARD 1 SLOT 3 ⇒ label=ROLL：{s13.group(0) if s13 else None}")
        p6 = RE_PAGE6.search(c.cmd("UI MEM"))
        delta = int(p6.group(3))
        ck.ok(p6.group(1) == "built" and delta <= 12288, f"UI MEM {p6.group(0)}（delta ≤ 12,288）")
        after = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        c.note(f"进入页面前后 internal_free {before.group(1)} -> {after.group(1)}（差 "
               f"{int(before.group(1)) - int(after.group(1))}B）；internal_min={after.group(2)}")
        stats = grab(c.cmd("UI SOUND STATS"), RE_STATS, "UI SOUND STATS")
        ck.ok(stats.group(1) == "13" and stats.group(4) == "0000", f"{stats.group(0)}")
        ck.ok(int(stats.group(2)) == sum(SLOT_COUNTS), f"ANALOG 轨存在槽数 = {stats.group(2)}（表 109）")

        # ---- labels：13 卡全部槽 vs ENG DESC ----
        bad, present, absent = [], 0, []
        first = []
        for card, count in enumerate(SLOT_COUNTS):
            for s in range(count):
                m = slot(c, card, s)
                if m.group(4) == "0":
                    absent.append(f"card={card} slot={s}")
                    continue
                present += 1
                if s == 0:
                    first.append(f"card {card} slot 0 = {m.group(7)}")
                desc = c.cmd(f"ENG DESC global 0 {m.group(6)}" if m.group(5) == "1"
                             else f"ENG DESC live 0 {m.group(6)}")
                dm = RE_DESC.search(desc)
                if dm is None or dm.group(4) != m.group(7):
                    bad.append(f"card={card} slot={s} flat={m.group(3)} page=\"{m.group(7)}\" "
                               f"desc=\"{dm.group(4) if dm else 'ERR'}\"")
        for f in first:
            c.note(f"首槽 {f}")
        ck.ok(not bad, f"全部 {present} 个存在槽的 label == ENG DESC（不一致 {len(bad)}）" +
              ("" if not bad else "：" + "; ".join(bad)))
        ck.ok(not absent, f"ANALOG 轨 0 上不存在的槽：{absent if absent else '无'}")

        # ---- drag：Voice 卡 LEVEL ----
        lvl = slot(c, 5, 0)
        flat, v0 = int(lvl.group(3)), int(lvl.group(9))
        c.note(f"Voice·LEVEL flat={flat} 初值={v0}：{lvl.group(0)}")
        k = knob(c, flat, "press", 0, -40)
        ck.ok(k is not None and k.group(5) == str(v0), f"press 0 -40 ⇒ 值不变：{k.group(0)}")
        k = knob(c, flat, "drag", 40, 0)
        v1 = int(k.group(5)) if k else None
        ck.ok(v1 is not None and v1 != v0, f"drag 40 0 ⇒ 值变化：{k.group(0)}")
        rows = eng_rows(c)
        state = re.search(r"STATE SNAP param trk=0 level=(-?\d+)", c.cmd("STATE SNAP"))
        ck.ok(rows[0].split(" level=")[1].split()[0] == str(v1), f"ENG SNAP 与之相符：{rows[0]}")
        ck.ok(state is not None and state.group(1) == str(v1), f"STATE SNAP param level={state.group(1)}")
        again = slot(c, 5, 0)
        ck.ok(again.group(9) == str(v1), f"CARD 5 SLOT 0 回读 value={again.group(9)}")
        k = knob(c, flat, "drag", -40, 0)
        c.note(f"drag -40 0 ⇒ value={k.group(5)}（相对锚点 −90° = 锚点值 − 值域/3；回原值要拖回锚点，见下一行）")
        k = knob(c, flat, "drag", 0, -40)
        ck.ok(k.group(5) == str(v0), f"drag 0 -40（锚点）⇒ 回到原值：{k.group(0)}")
        k = knob(c, flat, "press", 0, 0)
        ck.ok(k is not None and k.group(6) == "ring band", f"press 0 0 ⇒ {k.group(0)}")
        stats = grab(c.cmd("UI SOUND STATS"), RE_STATS, "UI SOUND STATS")
        ck.ok(stats.group(4) == "0000", f"拖动手势后 {stats.group(0)}")

        # ---- ctx：INSERT TYPE / ENV SYNC 换标签 ----
        ia = slot(c, 6, 18)   # FX 卡第 18 槽 = P_IA（INSERT A，TD）
        atk = slot(c, 2, 0)   # Envelope 卡首槽 = P_ATK
        c.note(f"初值：card 6 slot 18 = {ia.group(7)}；card 2 slot 0 = {atk.group(7)}")
        c.cmd("PARAM 53 1")
        ia1 = slot(c, 6, 18)
        ck.ok(ia1.group(4) == "1" and ia1.group(7) != ia.group(7),
              f"PARAM 53 1（ITYPE）⇒ card 6 slot 18 标签 {ia.group(7)} -> {ia1.group(7)}")
        c.cmd("PARAM 53 0")
        ia0 = slot(c, 6, 18)
        ck.ok(ia0.group(7) == ia.group(7), f"PARAM 53 0 ⇒ 标签回到 {ia0.group(7)}")
        c.cmd("PARAM 59 1")
        atk1 = slot(c, 2, 0)
        c.note(f"PARAM 59 1（ESYNC）⇒ card 2 slot 0 标签 {atk.group(7)} -> {atk1.group(7)}"
               "（上游 ESYNC_DESC 换 fmt/names 不换 label：标签看不出重解析，屏幕上的值才变成音符名）")
        c.cmd("PARAM 59 0")
        atk0 = slot(c, 2, 0)
        ck.ok(atk0.group(4) == "1" and atk0.group(7) == atk.group(7), f"PARAM 59 0 ⇒ 槽仍在、标签 {atk0.group(7)}")

        # ---- lock：Ctrl+4 鼓轨 ----
        rows_before = eng_rows(c)
        select_track(c, 3)
        st = c.cmd("STATE SNAP")
        ck.ok(re.search(r"selected_track=3 ", st) is not None, "Ctrl+4 后 selected_track=3")
        s40 = slot(c, 4, 0)
        ck.ok(s40.group(4) == "1" and s40.group(8) == "1", f"CARD 4 SLOT 0 ⇒ locked=1：{s40.group(0)}")
        f40 = int(s40.group(3))
        k = knob(c, f40, "drag", 40, 0)
        ck.ok(k is not None and k.group(6) == "locked", f"KNOB {f40} drag 40 0 ⇒ {k.group(0)}")
        k = knob(c, f40, "press", 0, -40)
        ck.ok(k is not None and k.group(6) == "locked", f"KNOB {f40} press 0 -40 ⇒ {k.group(0)}")
        ck.ok(eng_rows(c) == rows_before, "ENG SNAP 四轨行不变")
        ck.ok(slot(c, 4, 0).group(9) == s40.group(9), f"槽值不变 value={s40.group(9)}")
        s90 = slot(c, 9, 0)
        ck.ok(s90.group(4) == "1" and s90.group(8) == "0", f"CARD 9 SLOT 0（PATTERN）⇒ locked=0：{s90.group(0)}")
        f90, l0 = int(s90.group(3)), int(s90.group(9))
        knob(c, f90, "press", 0, -40)
        k = knob(c, f90, "drag", 40, 0)
        ck.ok(k is not None and k.group(5) != str(l0), f"鼓轨上 drum_ok 槽可拖：{k.group(0)}")
        k = knob(c, f90, "drag", 0, -40)
        ck.ok(k.group(5) == str(l0), f"拖回锚点 ⇒ {l0}：{k.group(0)}")
        s00 = slot(c, 0, 0)
        ck.ok(s00.group(4) == "1" and s00.group(8) == "0", f"鼓轨上 GLOBAL 槽不锁：{s00.group(0)}")
        stats = grab(c.cmd("UI SOUND STATS"), RE_STATS, "UI SOUND STATS")
        c.note(f"鼓轨 {stats.group(0)}")
        ck.ok(int(stats.group(2)) == sum(SLOT_COUNTS) - 7, "鼓轨存在槽数 = 102（P_E1..P_E7 无描述符）")
        s41 = slot(c, 4, 1)
        ck.ok(s41.group(4) == "0" and s41.group(5) == "-" and s41.group(6) == "-" and s41.group(8) == "0",
              f"CARD 4 SLOT 1（鼓轨不存在）⇒ {s41.group(0)}")
        raw = c.cmd(f"UI SOUND KNOB {s41.group(3)} drag 40 0")
        ck.ok("UI ERR sound (absent)" in raw, f"KNOB 不存在的槽 ⇒ {result_line(raw)}")
        # 锁定槽的 TAP：找一个此刻在视口里的锁定槽（屏幕 y 152..716 = 页面区 80..644）
        c.cmd("UI SOUND SCROLL 0")
        onscreen = None
        for card, count in enumerate(SLOT_COUNTS):
            for si in range(count):
                m = slot(c, card, si)
                if m.group(4) == "1" and m.group(8) == "1" and 152 <= int(m.group(11)) and \
                        int(m.group(11)) + int(m.group(13)) <= 716:
                    onscreen = m
                    break
            if onscreen:
                break
        ck.ok(onscreen is not None, "鼓轨上有一个在视口里的锁定槽")
        t = c.cmd(f"TAP {int(onscreen.group(10)) + 70} {int(onscreen.group(11)) + 66}")
        tf = RE_TAPF.search(t)
        ck.ok(tf is not None and tf.group(8) == " locked=1" and tf.group(6) == onscreen.group(3),
              f"TAP 锁定槽 ⇒ {result_line(t)}")
        # 不存在的槽的 TAP：把 Engine edit 卡滚进视口，点它右边那一格（P_E1，鼓轨上无描述符）
        c.cmd(f"UI SOUND SCROLL {max(0, int(s40.group(11)) - 300)}")
        r40 = slot(c, 4, 0)
        t = c.cmd(f"TAP {int(r40.group(10)) + 140 + 70} {int(r40.group(11)) + 66}")
        ck.ok("TAP miss" in t, f"TAP 不存在的槽（card 4 slot 1）⇒ {result_line(t)}")
        t = c.cmd(f"TAP {int(r40.group(10)) + 70} {int(r40.group(11)) + 66}")
        ck.ok(RE_TAPF.search(t) is not None, f"同一滚动位置 TAP 它左边的 KIT 槽 ⇒ {result_line(t)}")
        c.cmd("UI SOUND SCROLL 0")
        select_track(c, 0)

        # ---- freeze ----
        v_before = int(slot(c, 5, 0).group(9))
        rows_before = eng_rows(c)
        c.cmd("ENG FREEZE ON")
        k = knob(c, flat, "press", 0, -40)
        c.note(f"冻结期 press ⇒ {k.group(0)}（状态层投影在冻结时读全零，见 sloop_core.h 的 read_params 契约）")
        k = knob(c, flat, "drag", 40, 0)
        ck.ok(k is not None and k.group(5) == "0", f"冻结期 drag ⇒ 值不变（0）：{k.group(0)}")
        ck.ok(eng_rows(c) == rows_before, "冻结期 ENG SNAP 不变")
        c.cmd("ENG FREEZE OFF")
        v_after = int(slot(c, 5, 0).group(9))
        ck.ok(v_after == v0, f"解冻后 value 回到 {v0}：{v_after}")

        # ---- scroll + TAP ----
        sc = grab(c.cmd("UI SOUND SCROLL"), RE_SCROLL, "UI SOUND SCROLL")
        ck.ok(sc.group(1) == "0", f"{sc.group(0)}")
        r0 = slot(c, 5, 0)
        sc = grab(c.cmd("UI SOUND SCROLL 400"), RE_SCROLL, "UI SOUND SCROLL 400")
        ck.ok(sc.group(1) == "400", f"{sc.group(0)}")
        r1 = slot(c, 5, 0)
        ck.ok(int(r1.group(10)) == int(r0.group(10)) and int(r1.group(11)) == int(r0.group(11)) - 400,
              f"滚动后矩形上移 400：y {r0.group(11)} -> {r1.group(11)}")
        t = c.cmd(f"TAP {int(r1.group(10)) + 70} {int(r1.group(11)) + 66}")
        tf = RE_TAPF.search(t)
        ck.ok(tf is not None and tf.group(6) == str(flat) and tf.group(7) == "SET_PARAM",
              f"滚动后 TAP 槽 ⇒ {result_line(t)}")
        card_x, card_y = int(r1.group(10)) - 12, int(r1.group(11)) - 36
        t = c.cmd(f"TAP {card_x + 40} {card_y + 18}")
        tc = RE_TAPC.search(t)
        ck.ok(tc is not None and tc.group(4) == "5" and tc.group(5) == "0", f"滚动后 TAP 卡片标题行 ⇒ {result_line(t)}")
        sc = grab(c.cmd("UI SOUND SCROLL 99999"), RE_SCROLL, "UI SOUND SCROLL 99999")
        ck.ok(sc.group(1) == sc.group(2), f"SCROLL 越界钳到底：{sc.group(0)}")
        sc = grab(c.cmd("UI SOUND SCROLL -50"), RE_SCROLL, "UI SOUND SCROLL -50")
        ck.ok(sc.group(1) == "0", f"SCROLL 负值钳到 0：{sc.group(0)}")
        stats = grab(c.cmd("UI SOUND STATS"), RE_STATS, "UI SOUND STATS")
        ck.ok(stats.group(4) == "0000", f"滚动/拖动之后 {stats.group(0)}")
        c.cmd("UI SOUND SCROLL 0")

        # ---- guard：非仪表盘页 ----
        c.cmd("UI play")
        for line in ("UI SOUND STATS", "UI SOUND CARD 0 SLOT 0", "UI SOUND KNOB 0 press 0 -40",
                     "UI SOUND SCROLL 10"):
            raw = c.cmd(line)
            ck.ok("UI ERR sound (not_dashboard)" in raw, f"PLAY 页上 {line} ⇒ {result_line(raw)}")
        c.cmd("UI sound")
        heap = grab(c.cmd("ENG MEM"), RE_HEAP, "ENG MEM")
        ck.ok(int(heap.group(2)) >= 61440, f"{heap.group(0)}（internal_min ≥ 61,440）")
        c.note("收尾：硬复位重新载入 autosave 对象")
        c.ser.close()
        shot.reset_device(port, BAUD, on_chunk=c.log.write)
        c.log.flush()
    finally:
        c.log.close()
    total = ck.n - ck.failed
    print(f"{NOTE} {'PASS' if ck.failed == 0 else 'FAIL'} {total}/{ck.n}", flush=True)
    with log_path.open("a", encoding="utf-8") as f:
        f.write(f"{NOTE} {'PASS' if ck.failed == 0 else 'FAIL'} {total}/{ck.n}\n")
    return 0 if ck.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
