#!/usr/bin/env python3
"""P4-F Task 4 设备取证（一次性探针，不是场景脚本）：chip 页框架 + MOD/SCALE/VOICE + 九组导航。

覆盖 brief 的设备项：UI MEM 三页 delta ≤ est / build_ms ≤ 100；每 chip 的 8 槽 label 与
`ENG DESC live <track> <id>` 一致（逐槽，非抽样）；每 chip 首槽 TAP + / − 写后回读；
chip 行 TAP 与 console 选择同源；键盘 ←→↑↓/ENTER；鼓轨逐槽规则（可编辑槽写、锁定槽
`locked` 且值不变）；20 次切 chip pool_free 首末差 ≤128B；FILTER 条件 chip（DIGITAL 隐藏、
ANALOG 出现、选中后换引擎回落）；EDIT→STEP / MIX→STEP 过渡峰值 ≥4,096B；104px 组签像素。
产物：firmware/build/verify/p4f-task4-verify-<时间戳>.log + PNG。退出码 0 = 全过。
"""
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/inject"))
sys.path.insert(0, str(ROOT / "scripts"))
import inject  # noqa: E402

PAGE = {"mod": 15, "scale": 16, "voice": 17, "fx": 5, "global": 4, "analog": 6, "fm6": 7, "step": 2}
CHIP_EST = 3584
CHIPS = {
    "mod": [("ENV", 4), ("ENV DEST", 4), ("LFO", 4), ("LFO 2", 3), ("LFO DEST", 4)],
    "scale": [("SEL", 4), ("SEL 2", 3), ("ARP", 4), ("ARP 2", 4)],
    "voice": [("VOICE", 4), ("VOICE 2", 4), ("PATTERN", 4), ("FILTER", 4)],
}
# chip 行几何（page_params.c CHIP_* 与 page_layout_chip_width 同源）
CHIP_X0, CHIP_Y, CHIP_H, CHIP_GAP = 16, 4, 32, 8
TAB_X0, TAB_GAP, GROUP_W = 16, 8, 104
C_YELLOW = inject.C_YELLOW
C_PANEL = inject.C_PANEL
C_PANEL_HI = inject.C_PANEL_HI
K = {"left": "0x4A", "right": "0x4C", "up": "0x3B", "down": "0x4B", "enter": "0x3D"}


def chip_w(name: str) -> int:
    return 11 * len(name) + 16


def chip_xy(name_page, c: int):
    x = CHIP_X0
    for i in range(c):
        x += chip_w(CHIPS[name_page][i][0]) + CHIP_GAP
    return (x + chip_w(CHIPS[name_page][c][0]) // 2, 72 + CHIP_Y + CHIP_H // 2)


def key(s, code: str) -> None:
    s.cmd(f"KEY {code} down")
    s.cmd(f"KEY {code} up")


def slot_lines(s, page: str):
    s.cmd(f"UI {page.upper()} CHIP")
    chip = int(re.search(r"UI \w+ chip=(\d+)", s.last).group(1))
    slots = {}
    for m in re.finditer(rf"UI {page} slot=(\d+) id=(\d+) label=\"([^\"]*)\" locked=(\d)", s.last):
        slots[int(m.group(1))] = (int(m.group(2)), m.group(3), int(m.group(4)))
    return chip, slots


def track_of(s) -> int:
    s.cmd("STATE SNAP")
    return int(re.search(r"selected_track=(\d+)", s.last).group(1))


def select_track(s, track: int) -> None:
    s.cmd("UI tracks")
    s.cmd("TAP %d 347" % (24 + track * inject.TRACKS_CARD_PITCH + 148))
    got = track_of(s)
    (s.ok if got == track else s.fail)(f"选轨 {track}（STATE SNAP selected_track={got}）")


def main() -> int:
    out = ROOT / "firmware/build/verify"
    stamp = f"{time.strftime('%Y%m%d-%H%M%S')}"
    s = inject.Session("/dev/ttyACM0", out / f"p4f-task4-verify-{stamp}.log", out, stamp,
                       "p4f-task4-verify")
    print(f"== P4-F Task 4 device verify @ {stamp}")
    s.wait_ready()
    s.cmd("STORE AUTO OFF")
    s.cmd("STORE PROJ SAVE 3")            # 工作工程快照（收口 LOAD 回来）
    s.expect(r"PROJ 3 SAVED", "⓪ 工作工程存诊断槽 3（收口还原）")
    select_track(s, 0)

    # ---------- ① UI MEM：三页 delta ≤ est、build_ms ≤100、EDIT/MIX 组 Σ ----------
    for p in ("analog", "fm6", "mod", "scale", "voice"):
        s.cmd(f"UI {p}")
        s.expect(rf"UI page={p} \({PAGE[p]}\)", f"① UI {p}")
    mem = inject.param_mem(s)
    (s.ok if mem["head"]["registered"] == 18 else s.fail)(
        f"① registered=18 实得 {mem['head']['registered']}")
    for p in ("mod", "scale", "voice"):
        st, est, delta = mem["pages"][PAGE[p]]
        (s.ok if st == "built" and est == CHIP_EST and 0 < delta <= CHIP_EST else s.fail)(
            f"① {p.upper()} state={st} est={est} delta={delta}（≤ {CHIP_EST}）")
    s.cmd("UI MEM")
    ms = {m.group(1): int(m.group(2)) for m in
          re.finditer(r"UI MEM page id=\d+ title=(\w+) state=\w+ est=\d+ delta=-?\d+ build_ms=(\d+)", s.last)}
    for p in ("mod", "scale", "voice"):
        (s.ok if 0 < ms[p.upper()] <= 100 else s.fail)(f"① {p.upper()} build_ms={ms[p.upper()]}（≤100）")
    order = re.findall(r"UI MEM GROUP (\w+) ", s.last)
    (s.ok if order == ["PLAY", "TRACKS", "EDIT", "MIX", "SYNTH", "SEQ", "SONG", "FILE", "SYS"]
     else s.fail)(f"① 九组顺序 {order}")
    g = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in
         re.finditer(r"UI MEM GROUP (\w+) built=(\d+) sum_est=(\d+)", s.last)}
    (s.ok if g["EDIT"] == (5, 20480) else s.fail)(f"① GROUP EDIT built/sum_est={g['EDIT']}（期望 (5, 20480)）")

    # ---------- ② EDIT 全建 → STEP 首建的过渡峰值（源组销毁前的池剩余） ----------
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "② EDIT 五页全建 → UI step")
    peak = re.search(r"页面构建 page=STEP\(2\) .*池剩余=(\d+)B", s.last)
    (s.ok if peak and int(peak.group(1)) >= 4096 else s.fail)(
        f"② EDIT→STEP 过渡峰值 pool_free={peak.group(1) if peak else '?'}B（≥4,096）")

    # ---------- ③ 逐 chip：8 槽 label == ENG DESC live；首槽 TAP +/− 写后回读 ----------
    t = track_of(s)
    for p in ("mod", "scale", "voice"):
        s.cmd(f"UI {p}")
        s.expect(rf"UI page={p} \({PAGE[p]}\)", f"③ UI {p}")
        for c, (cname, nslots) in enumerate(CHIPS[p]):
            s.cmd(f"UI {p.upper()} CHIP {c}")
            s.expect(rf"UI {p} chip={c}", f"③ {p.upper()} CHIP {c}（{cname}）")
            _, slots = slot_lines(s, p)
            (s.ok if len(slots) == nslots else s.fail)(
                f"③ {p.upper()}·{cname} 槽数 {len(slots)}（期望 {nslots}）")
            bad = []
            for k, (pid, label, locked) in sorted(slots.items()):
                s.cmd(f"ENG DESC live {t} {pid}")
                m = re.search(r'label="([^"]*)"', s.last)
                if not m or m.group(1) != label or locked != 0:
                    bad.append(f"slot{k}:id={pid} label={label} locked={locked} desc={m.group(1) if m else '?'}")
            (s.ok if not bad else s.fail)(f"③ {p.upper()}·{cname} 逐槽 label == ENG DESC live（{len(slots)} 槽）"
                                         + ("" if not bad else f" 不符 {bad}"))
            # 首槽 TAP：旋钮设焦点 → + → PARAM 回读 → − 回基线
            k0 = min(slots)
            pid0 = slots[k0][0]
            s.cmd(f"PARAM {pid0}")
            v0 = inject.param_value(s)
            mx = int(re.search(r"max=(-?\d+)", s.last).group(1))
            cx = inject.PARAM_GRID_X0 + (k0 % 4) * inject.PARAM_CELL_PITCH + inject.PARAM_OBJ_DX + 80
            cy = inject.PARAM_GRID_Y0 + (k0 // 4) * inject.PARAM_ROW_PITCH + inject.PARAM_OBJ_DY + 80
            s.cmd(f"TAP {cx} {cy}")
            s.expect(rf"-> PARAM slot={k0} focus", f"③ {p.upper()}·{cname} TAP 旋钮（焦点 slot={k0}）")
            step = 1 if v0 < mx else -1
            s.cmd("TAP %d %d" % (inject.PARAM_BTN_PLUS if step > 0 else inject.PARAM_BTN_MINUS))
            s.expect(rf"-> PARAM slot={k0} id={pid0} delta=[+-]1 value={v0 + step}",
                     f"③ {p.upper()}·{cname} TAP {'+' if step > 0 else '−'}（写后回读 {v0 + step}）")
            inject.param_expect_value(s, f"PARAM {pid0}", v0 + step, f"③ {p.upper()}·{cname} 引擎回读")
            s.cmd("TAP %d %d" % (inject.PARAM_BTN_MINUS if step > 0 else inject.PARAM_BTN_PLUS))
            inject.param_expect_value(s, f"PARAM {pid0}", v0, f"③ {p.upper()}·{cname} 回基线")
    img = s.capture("task4-mod")
    img2 = s.capture("task4-voice")

    # ---------- ④ chip 行 TAP 与 console 选择同源 ----------
    s.cmd("UI mod")
    for c, (cname, _) in enumerate(CHIPS["mod"]):
        x, y = chip_xy("mod", c)
        s.cmd(f"TAP {x} {y}")
        s.expect(rf"-> PARAMS mod chip={c} select", f"④ TAP chip「{cname}」")
        s.cmd("UI MOD CHIP")
        s.expect(rf"UI mod chip={c}\r?\n", f"④ console 读回 chip={c}")
    s.cmd("UI MOD CHIP 5")
    s.expect(r"UI ERR chip", "④ CHIP 5 越界 → UI ERR chip")
    s.cmd("UI MOD CHIP 1")

    # ---------- ⑤ 键盘：←→ 跳过空槽环绕、↑↓ 调值、ENTER +1 ----------
    s.cmd("UI MOD CHIP 3")   # LFO 2：3 槽
    _, slots = slot_lines(s, "mod")
    x2, y2 = inject.param_knob_xy(2)
    s.cmd(f"TAP {x2} {y2}")
    s.expect(r"-> PARAM slot=2 focus", "⑤ LFO 2：焦点 slot 2")
    key(s, K["right"])     # slot 2 → 环绕到 slot 0（3..7 隐藏，跳过）
    key(s, K["up"])
    pid0 = slots[0][0]
    s.cmd(f"PARAM {pid0}")
    v0s0 = inject.param_value(s)
    key(s, K["up"])
    inject.param_expect_value(s, f"PARAM {pid0}", v0s0 + 1, "⑤ → 后 ↑ 调的是 slot 0（环绕且跳过隐藏槽）")
    key(s, K["down"])
    inject.param_expect_value(s, f"PARAM {pid0}", v0s0, "⑤ ↓ 回基线")
    key(s, K["enter"])
    inject.param_expect_value(s, f"PARAM {pid0}", v0s0 + 1, "⑤ ENTER = 焦点槽 +1")
    key(s, K["down"])
    inject.param_expect_value(s, f"PARAM {pid0}", v0s0, "⑤ 回基线")

    # ---------- ⑥ 20 次切 chip：零分配（pool_free 首末差 ≤128B） ----------
    for p in ("mod", "scale", "voice"):
        s.cmd(f"UI {p}")
        n = len(CHIPS[p])
        s.cmd("UI MEM")
        f0 = int(re.search(r"pool_free=(\d+)", s.last).group(1))
        for i in range(20):
            s.cmd(f"UI {p.upper()} CHIP {i % n}")
        s.cmd("UI MEM")
        f1 = int(re.search(r"pool_free=(\d+)", s.last).group(1))
        (s.ok if abs(f1 - f0) <= 128 else s.fail)(
            f"⑥ {p.upper()} 20 次切 chip pool_free {f0} → {f1}（Δ={f1 - f0:+d}B，|Δ| ≤128）")

    # ---------- ⑦ 鼓轨逐槽规则 ----------
    select_track(s, 3)
    for p, want in (("voice", {0: [1, 1, 1, 1], 1: [1, 1, 0, 1], 2: [0, 0, 0, 0]}),
                    ("mod", {0: [1] * 4, 1: [1] * 4, 2: [1] * 4, 3: [1] * 3, 4: [1] * 4}),
                    ("scale", {0: [1] * 4, 1: [1] * 3, 2: [1] * 4, 3: [1] * 4})):
        s.cmd(f"UI {p}")
        for c, locks in want.items():
            s.cmd(f"UI {p.upper()} CHIP {c}")
            _, slots = slot_lines(s, p)
            got = [slots[k][2] for k in sorted(slots)]
            (s.ok if got == locks else s.fail)(f"⑦ 鼓轨 {p.upper()}·{CHIPS[p][c][0]} locked={got}（期望 {locks}）")
    s.cmd("UI fx")
    s.cmd("UI FX CHIP")
    fx = {int(m.group(1)): int(m.group(2)) for m in
          re.finditer(r"UI fx slot=(\d+) id=\d+ label=\"[^\"]*\" locked=(\d)", s.last)}
    (s.ok if [fx[k] for k in sorted(fx)] == [1, 1, 1, 1, 0, 0, 0, 1] else s.fail)(
        f"⑦ 鼓轨 FX 逐槽 locked={[fx[k] for k in sorted(fx)]}（期望 TFLT/LEVEL/PAN 可编辑）")
    s.cmd("UI global")
    s.cmd("UI GLOBAL CHIP")
    s.expect(r"UI global chip=0", "⑦ GLOBAL（旧式整页）逐槽行")
    gl = {int(m.group(1)): int(m.group(2)) for m in
          re.finditer(r"UI global slot=(\d+) id=\d+ label=\"[^\"]*\" locked=(\d)", s.last)}
    (s.ok if len(gl) == 8 and not any(gl.values()) else s.fail)(
        f"⑦ 鼓轨 GLOBAL 全局参数不锁 locked={gl}")
    s.cmd("UI GLOBAL CHIP 1")
    s.expect(r"UI ERR chip", "⑦ 旧式整页没有 chip 行：CHIP 1 → UI ERR chip")
    # 鼓轨上 FILTER 隐藏：选择被拒、位置是空位（TAP miss）
    s.cmd("UI voice")
    s.cmd("UI VOICE CHIP 3")
    s.expect(r"UI ERR chip \(hidden\)", "⑦ 鼓轨 VOICE·FILTER 隐藏：CHIP 3 → UI ERR chip (hidden)")
    s.cmd("TAP %d %d" % chip_xy("voice", 3))
    s.expect(r"TAP miss", "⑦ 鼓轨 FILTER 那一格 TAP miss")
    # 可编辑槽写：VOICE 2 的 PAN（slot 2）；锁定槽：VOICE 2 的 ALLOC（slot 0）
    s.cmd("UI VOICE CHIP 1")
    pan = (inject.param_knob_xy(2)[0], inject.param_knob_xy(2)[1])
    s.cmd("PARAM 39")
    pan0 = inject.param_value(s)
    s.cmd("TAP %d %d" % pan)
    s.cmd("TAP %d %d" % inject.PARAM_BTN_PLUS)
    s.expect(rf"-> PARAM slot=2 id=39 delta=\+1 value={pan0 + 1}", "⑦ 鼓轨 PAN TAP +（可编辑）")
    s.cmd("TAP %d %d" % inject.PARAM_BTN_MINUS)
    inject.param_expect_value(s, "PARAM 39", pan0, "⑦ 鼓轨 PAN 回基线")
    s.cmd("PARAM 43")
    alloc0 = inject.param_value(s)
    s.cmd("TAP %d %d" % inject.param_knob_xy(0))
    s.cmd("TAP %d %d" % inject.PARAM_BTN_PLUS)
    s.expect(r"-> PARAM slot=0 locked", "⑦ 鼓轨 ALLOC（锁定槽）TAP + → locked")
    inject.param_expect_value(s, "PARAM 43", alloc0, "⑦ 锁定槽值不变")
    img3 = s.capture("task4-voice-drum")

    # ---------- ⑧ FILTER 条件 chip：DIGITAL 隐藏、ANALOG 出现、选中后换引擎回落 ----------
    select_track(s, 0)
    s.cmd("UI voice")
    s.cmd("UI VOICE CHIP 3")
    s.expect(r"UI voice chip=3", "⑧ ANALOG 轨：FILTER 可选")
    _, slots = slot_lines(s, "voice")
    (s.ok if sorted(slots) == [0, 1, 2, 3] and slots[1][0] == 67 else s.fail)(
        f"⑧ FILTER 槽 {slots}（期望 FTYPE + E4/E5/E7，id 58/67/68/70）")
    s.cmd("ENG TRACK 0 1 0")
    s.cmd("UI VOICE CHIP 3")
    s.expect(r"UI ERR chip \(hidden\)", "⑧ DIGITAL 轨：FILTER 隐藏")
    s.cmd("UI VOICE CHIP")
    s.expect(r"UI voice chip=0", "⑧ FILTER 选中时换到 DIGITAL ⇒ 生效 chip 回落到首个可见")
    s.cmd("TAP %d %d" % chip_xy("voice", 3))
    s.expect(r"TAP miss", "⑧ DIGITAL 轨 FILTER 那一格 TAP miss")
    img4 = s.capture("task4-voice-digital")
    s.cmd("ENG TRACK 0 0 0")
    s.cmd("UI VOICE CHIP 3")
    s.expect(r"UI voice chip=3", "⑧ 回 ANALOG：FILTER 再出现")
    img5 = s.capture("task4-voice-filter")

    # ---------- ⑨ MIX 组与 104px 组签像素 ----------
    s.cmd("UI global")
    s.expect(r"UI page=global \(4\)", "⑨ UI global（MIX 组）")
    s.cmd("UI TAB SEQ")
    s.expect(r"UI TAB SEQ page=step \(2\)", "⑨ UI TAB SEQ")
    img6 = s.capture("task4-tabs-seq")
    gx = lambda i: TAB_X0 + i * (GROUP_W + TAB_GAP)
    # 组签 y∈[4,36)：探针 y=8（避开 10px 圆角与 16pt 文本行）
    s.probe(img6, "⑨ 组签选中（SEQ）左缘", gx(5) + 20, 8, C_YELLOW)
    s.probe(img6, "⑨ 组签选中（SEQ）右缘（104px 宽）", gx(5) + GROUP_W - 8, 8, C_YELLOW)
    s.probe(img6, "⑨ 组签缝（SEQ|SONG）是页签条底色", gx(5) + GROUP_W + 4, 16, C_PANEL, 10)
    s.probe(img6, "⑨ 第 9 张组签（SYS）未选中填充", gx(8) + 20, 8, C_PANEL_HI, 10)
    s.probe(img6, "⑨ SYS 右缘仍是填充", gx(8) + GROUP_W - 8, 8, C_PANEL_HI, 10)
    s.probe(img6, "⑨ 组签行右端之外（1,016+）是页签条底色", gx(8) + GROUP_W + 4, 16, C_PANEL, 10)
    s.cmd("UI TAB MIX")
    s.expect(r"UI TAB MIX page=global \(4\)", "⑨ UI TAB MIX → 上次子页 GLOBAL")
    s.cmd("UI SUB 1")
    s.expect(r"UI SUB 1 page=fx \(5\)", "⑨ UI SUB 1 → FX")
    s.cmd("UI SUB 2")
    s.expect(r"UI ERR sub 2", "⑨ MIX 组只有 2 张子签")
    s.cmd("UI MEM")
    g = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in
         re.finditer(r"UI MEM GROUP (\w+) built=(\d+) sum_est=(\d+)", s.last)}
    (s.ok if g["MIX"] == (2, 7168) and g["EDIT"][0] == 0 else s.fail)(
        f"⑨ GROUP MIX built/sum_est={g['MIX']}、EDIT built={g['EDIT'][0]}（期望 (2, 7168) / 0）")
    s.cmd("UI step")
    peak = re.search(r"页面构建 page=STEP\(2\) .*池剩余=(\d+)B", s.last)
    (s.ok if peak and int(peak.group(1)) >= 4096 else s.fail)(
        f"⑨ MIX→STEP 过渡峰值 pool_free={peak.group(1) if peak else '?'}B（≥4,096）")
    s.cmd("UI global")
    peak = re.search(r"页面构建 page=GLOBAL\(4\) .*池剩余=(\d+)B", s.last)
    (s.ok if peak and int(peak.group(1)) >= 4096 else s.fail)(
        f"⑨ SEQ 两页全建 → MIX 首建 GLOBAL 过渡峰值 pool_free={peak.group(1) if peak else '?'}B（≥4,096）")
    s.cmd("UI MEM")
    fr = int(re.search(r"pool_free=(\d+)", s.last).group(1))
    (s.ok if fr >= 4096 else s.fail)(f"⑨ SEQ 两页全建 → MIX 首建后 pool_free={fr}B（≥4,096）")

    # ---------- 收口 ----------
    s.cmd("UI play")
    select_track(s, 0)
    s.cmd("STORE PROJ LOAD 3")
    s.expect(r"PROJ 3 LOADED", "收口：还原工作工程")
    s.cmd("STORE PROJ ERASE 3")
    s.cmd("STORE AUTO ON")
    s.ser.note(f"p4f-task4-verify 结束 failures={s.failures}")
    s.ser.close()
    print(f"[{'PASS' if s.failures == 0 else 'FAIL'}] p4f-task4-verify: failures={s.failures}")
    return 0 if s.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
