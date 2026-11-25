#!/usr/bin/env python3
"""P4-F Task 5 设备取证（一次性探针，不是场景脚本）：SOUND 页 + GLOBAL/FX chip 页 + 导航 17 页。

覆盖 brief 的设备项：SOUND 12 引擎轮换 ⇒ 8 槽 label == `ENG DESC live`；ENGINE/PRESET ± 经 TAP
（环绕、回读、中段 miss）；鼓轨 KIT 行写 P_E0、旋钮锁定、PRESET 行缺席；FM6 轨 bank 行
SAVE/ERASE 往返、非 FM6 轨 bank 带 TAP miss；GLOBAL/FX 每 chip 逐槽 label/首槽写后回读/鼓轨
逐槽规则；20 次切 chip 零分配；UI MEM（SOUND/GLOBAL/FX delta ≤ est、EDIT/MIX Σ est、registered
=17）；有序对 EDIT→STEP / MIX→STEP / SEQ→MIX 的构建日志 池剩余 ≥ 4,096；`analog`/`fm6` 别名。
产物：firmware/build/verify/p4f-task5-verify-<时间戳>.log + PNG。退出码 0 = 全过。
"""
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/inject"))
sys.path.insert(0, str(ROOT / "scripts"))
import inject  # noqa: E402

SOUND = 6
SOUND_EST = 6144
CHIP_EST = 3584
CHIP_PAGES = {
    "global": [("GLO", 8), ("REV", 8), ("DRUMS", 4), ("TEXTURE", 7)],
    "fx": [("FX", 8), ("SLICER", 4), ("INSERT", 4), ("INSERT 2", 1)],
}
CHIP_PAGE_ID = {"global": 4, "fx": 5}
# SOUND 页眉（page_layout.h SOUND_ROW_*）：row0 x=16 w=288、row1 x=320 w=384、y=4 h=32、区宽 42
SOUND_ROW_Y = 72 + 4 + 16
SOUND_ROWS = [(16, 288), (320, 384)]
ZONE = 42
NENGINES = 12
# 上游 drums.c DRUM_KIT_NAMES 的前几项（kit 号 → 名；只列本探针用到的）
KIT_NAMES = {0: "ACOUSTIC", 1: "DEEP", 2: "TIGHT", 3: "BRIGHT", 4: "DUST", 5: "808"}


def sound_xy(row: int, delta: int) -> tuple:
    x, w = SOUND_ROWS[row]
    return (x + ZONE // 2 if delta < 0 else x + w - ZONE // 2, SOUND_ROW_Y)


def sound_mid(row: int) -> tuple:
    x, w = SOUND_ROWS[row]
    return (x + w // 2, SOUND_ROW_Y)


def chip_width(name: str) -> int:
    return 11 * len(name) + 16


def chip_center(page: str, c: int) -> tuple:
    x = 16 + sum(chip_width(n) + 8 for n, _ in CHIP_PAGES[page][:c])
    return (x + chip_width(CHIP_PAGES[page][c][0]) // 2, 72 + 4 + 16)


def slot_lines(s, page: str):
    s.cmd(f"UI {page.upper()} CHIP")
    chip = int(re.search(r"UI \w+ chip=(\d+)", s.last).group(1))
    slots = {int(m.group(1)): (int(m.group(2)), m.group(3), int(m.group(4))) for m in
             re.finditer(rf"UI {page} slot=(\d+) id=(\d+) label=\"([^\"]*)\" locked=(\d)", s.last)}
    return chip, slots


def eng_trk(s, track: int) -> tuple:
    s.cmd("ENG SNAP")
    m = re.search(rf"ENG SNAP trk k={track} engine=(\d+) preset=(\d+)", s.last)
    return int(m.group(1)), int(m.group(2))


def build_peak(s, page: str, pid: int, label: str) -> None:
    s.cmd(f"UI {page}")
    s.expect(rf"UI page={page} \({pid}\)", f"{label}: UI {page}")
    m = re.search(r"池剩余=(\d+)B", s.last)
    if m is None:
        s.fail(f"{label}: 缺构建日志 池剩余")
        return
    free = int(m.group(1))
    (s.ok if free >= 4096 else s.fail)(f"{label}: 构建日志 池剩余={free}B（≥4,096）")


def main() -> int:
    out = ROOT / "firmware/build/verify"
    stamp = f"{time.strftime('%Y%m%d-%H%M%S')}"
    s = inject.Session("/dev/ttyACM0", out / f"p4f-task5-verify-{stamp}.log", out, stamp,
                       "p4f-task5-verify")
    print(f"== P4-F Task 5 device verify @ {stamp}")
    n_pass = [0]
    _ok = s.ok
    s.ok = lambda text: (n_pass.__setitem__(0, n_pass[0] + 1), _ok(text))
    s.wait_ready()
    s.cmd("STORE AUTO OFF")
    s.cmd("UI play")
    inject.select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0")

    # ---- ① SOUND：12 引擎轮换 ⇒ 8 槽 label == ENG DESC live；页眉引擎名随动
    s.cmd("UI sound")
    s.expect(rf"UI page=sound \({SOUND}\)", "① UI sound")
    m = re.search(rf"页面构建 page=SOUND\({SOUND}\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B "
                  r"耗时=(\d+)ms", s.last)
    if m:
        delta, est, free, ms = map(int, m.groups())
        (s.ok if delta <= est <= SOUND_EST and ms <= 100 else s.fail)(
            f"① SOUND 首建 delta={delta}B 预算={est}B 池剩余={free}B 耗时={ms}ms（delta ≤ est ≤ {SOUND_EST}）")
    else:
        s.ser.note("① SOUND 已建（无构建日志）")
    for e in range(NENGINES):
        s.cmd(f"ENG TRACK 0 {e} 0")
        chip, slots = slot_lines(s, "sound")
        bad = []
        for k, (pid, label, locked) in sorted(slots.items()):
            s.cmd(f"ENG DESC live 0 {pid}")
            mm = re.search(r'label="([^"]*)"', s.last)
            if locked != 0 or mm is None or mm.group(1) != label:
                bad.append(f"slot{k}:id={pid} {label}/locked={locked} desc={mm.group(1) if mm else '?'}")
        (s.ok if chip == 0 and sorted(slots) == list(range(8)) and not bad else s.fail)(
            f"① 引擎 {e:2d}：8 槽 label == ENG DESC live 且无锁定"
            + ("" if not bad else f" 不符 {bad}"))
        if e == 3:   # LOFI：页眉 + 预置 + 的回显名字
            s.cmd("TAP %d %d" % sound_xy(1, +1))
            s.expect(r'-> SOUND PRESET value=1 name="[A-Z0-9 ]+" status="OK"', "① LOFI PRESET + 回显")
    s.cmd("ENG TRACK 0 0 0")

    # ---- ② ENGINE / PRESET ± 经 TAP：环绕、回读、中段 miss
    s.cmd("TAP %d %d" % sound_xy(0, -1))
    s.expect(r'-> SOUND ENGINE value=11 name="NOISE" status="OK"', "② ENGINE − 从 ANALOG 环绕到 NOISE")
    (s.ok if eng_trk(s, 0) == (11, 0) else s.fail)(f"② ENG SNAP engine/preset={eng_trk(s, 0)}（期望 11/0）")
    s.cmd("TAP %d %d" % sound_xy(0, +1))
    s.expect(r'-> SOUND ENGINE value=0 name="ANALOG" status="OK"', "② ENGINE + 环绕回 ANALOG")
    s.cmd("TAP %d %d" % sound_mid(0))
    s.expect(r"TAP miss", "② ENGINE 行中段（标签）TAP miss")
    s.cmd("TAP %d %d" % sound_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=1 name="808 DIRTY" status="OK"', "② PRESET + → 预置 1（808 DIRTY）")
    (s.ok if eng_trk(s, 0) == (0, 1) else s.fail)(f"② ENG SNAP engine/preset={eng_trk(s, 0)}（期望 0/1）")
    s.cmd("TAP %d %d" % sound_xy(1, -1))
    s.cmd("TAP %d %d" % sound_xy(1, -1))
    s.expect(r'-> SOUND PRESET value=21 name="SINE PAD" status="OK"', "② PRESET − 从 0 环绕到 21（SINE PAD）")
    (s.ok if eng_trk(s, 0) == (0, 21) else s.fail)(f"② ENG SNAP preset={eng_trk(s, 0)}（期望 21）")
    s.cmd("TAP %d %d" % sound_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=0 name="808 BOOM" status="OK"', "② PRESET + 回 0（808 BOOM）")
    s.cmd("TAP %d %d" % sound_mid(1))
    s.expect(r"TAP miss", "② PRESET 行中段 TAP miss")
    s.cmd("ENG TRACK 0 0 0")
    # 非 FM6 轨：底部 bank 带整行隐藏 ⇒ TAP miss（槽 ± / SAVE / ERASE）
    for label, xy in (("槽 −", inject.bank_slot_btn_xy(-1)), ("槽 +", inject.bank_slot_btn_xy(+1)),
                      ("SAVE", inject.bank_act_xy(0)), ("ERASE", inject.bank_act_xy(2)),
                      ("标签", (40, inject.BANK_ROW_Y + 22)), ("状态行", (900, inject.BANK_ROW_Y + 22))):
        s.cmd("TAP %d %d" % xy)
        s.expect(r"TAP miss", f"② ANALOG 轨 bank 带「{label}」TAP miss")
    img = s.capture("task5-sound-analog")
    # 页眉两行都有底色；名字列有强调色像素（ENGINE 行右对齐止于 16+288−54）
    for row, (rx, rw) in enumerate(SOUND_ROWS):
        s.probe(img, f"② 页眉行 {row} 左上内侧是行底色", rx + 3, 72 + 6, (32, 40, 48))
        s.probe(img, f"② 页眉行 {row} 上方 2px 仍是页面背景", rx + 3, 72 + 2, (16, 20, 24))
    s.probe_region(img, "② ENGINE 行名字列有强调色", (16 + 130, 72 + 8, 16 + 288 - 54, 72 + 32),
                   inject.C_YELLOW, 24, 12)
    s.probe_region(img, "② PRESET 行名字列有强调色", (320 + 130, 72 + 8, 320 + 384 - 54, 72 + 32),
                   inject.C_YELLOW, 24, 12)
    s.probe_region(img, "② 上下文行在两行右侧（x≥720）有灰字", (720, 72 + 6, 1080, 72 + 32),
                   (144, 148, 160), 32, 12)

    # ---- ③ 鼓轨：KIT 行写 P_E0、旋钮区锁定、PRESET 行缺席
    inject.select_track(s, 3)
    s.cmd("UI sound")
    chip, slots = slot_lines(s, "sound")
    (s.ok if chip == 0 and sorted(slots) == list(range(8)) and slots[0][1] == "KIT" and
     all(v[2] == 1 for v in slots.values()) and all(slots[k][1] == "-" for k in range(1, 8))
     else s.fail)(f"③ 鼓轨 SOUND 逐槽 {slots}（期望 slot0 KIT、其余无控件，全锁定）")
    s.cmd("PARAM 63")
    kit0 = inject.param_value(s)
    kit_max = inject.param_value(s, "max")
    (s.ok if kit_max == 45 else s.fail)(f"③ KIT 描述符 max={kit_max}（期望 45 = DRUM_KITS−1）")
    want = kit0 + 1 if kit0 < kit_max else 0
    s.cmd("TAP %d %d" % sound_xy(0, +1))
    s.expect(rf'-> SOUND KIT value={want} name="{KIT_NAMES[want]}" status="OK"', "③ KIT + 写 P_E0（名字同拍）")
    inject.param_expect_value(s, "PARAM 63", want, "③ P_E0 回读")
    s.cmd("TAP %d %d" % sound_xy(0, -1))
    s.expect(rf'-> SOUND KIT value={kit0} name="{KIT_NAMES[kit0]}" status="OK"', "③ KIT − 回基线")
    inject.param_expect_value(s, "PARAM 63", kit0, "③ P_E0 回基线")
    s.cmd("TAP %d %d" % inject.param_knob_xy(0))
    s.cmd("TAP %d %d" % inject.PARAM_BTN_PLUS)
    s.expect(r"-> PARAM slot=0 locked", "③ 鼓轨旋钮槽 0 TAP + → locked")
    s.cmd("TAP %d %d" % sound_xy(1, +1))
    s.expect(r"TAP miss", "③ 鼓轨没有 PRESET 行：其 + 区 TAP miss")
    s.cmd("TAP %d %d" % sound_xy(1, -1))
    s.expect(r"TAP miss", "③ 鼓轨没有 PRESET 行：其 − 区 TAP miss")
    img = s.capture("task5-sound-drum")
    # 帧内像素：KIT 行底色在、PRESET 行矩形内只有页面背景（行缺席）
    s.probe(img, "③ KIT 行有底色（row 0 仍在）", 16 + 3, 72 + 6, (32, 40, 48))
    s.probe(img, "③ PRESET 行矩形左上角是页面背景（行缺席）", 320 + 3, 72 + 6, (16, 20, 24))
    s.probe_region(img, "③ 上下文行 NO SOUND 红字在 KIT 行右侧", (320, 72 + 6, 900, 72 + 32),
                   (224, 84, 42), 24, 12)

    # ---- ④ FM6 轨：bank 行出现，SAVE/ERASE 往返；换回非 FM6 ⇒ 行消失
    s.cmd("ENG TRACK 1 9 0")
    inject.select_track(s, 1)
    s.cmd("UI sound")
    (s.ok if eng_trk(s, 1) == (9, 0) else s.fail)(f"④ 轨 1 换 FM6：{eng_trk(s, 1)}")
    s.cmd("TAP %d %d" % inject.bank_slot_btn_xy(-1))
    s.expect(r"-> BANK slot=26 delta=-1", "④ bank 槽 −（FM6 轨行可见；0 环绕到 26）")
    s.cmd("TAP %d %d" % inject.bank_slot_btn_xy(+1))
    s.expect(r"-> BANK slot=0 delta=\+1", "④ bank 槽 + → B1")
    s.cmd("TAP %d %d" % inject.bank_act_xy(0))
    s.expect(r'-> BANK slot=0 action=save status="SAVE OK"', "④ TAP SAVE → 槽 0")
    s.cmd("STORE BANK GET 0")
    got = s.last.split("BANK 0 GET ")[1].split()[0].strip()
    s.cmd("STORE BANK LIVE")
    live = s.last.split("BANK LIVE ")[1].split()[0].strip()
    (s.ok if got == live and len(live) == 256 else s.fail)(f"④ GET 0 == LIVE（{len(live)} hex）")
    s.cmd("TAP %d %d" % inject.bank_act_xy(2))
    s.expect(r'-> BANK slot=0 action=erase status="ERASE OK"', "④ TAP ERASE")
    s.cmd("STORE BANK GET 0")
    s.expect(r"STORE ERR empty", "④ 擦后 GET 0 → empty")
    img = s.capture("task5-sound-fm6")
    save_bg = img.crop((inject.BANK_SAVE_X + 4, inject.BANK_ROW_Y + 4,
                        inject.BANK_SAVE_X + 8, inject.BANK_ROW_Y + 8)).getcolors(1 << 16)
    (s.ok if save_bg and len(save_bg) == 1 and save_bg[0][1] != (32, 40, 48) and
     save_bg[0][1] != (16, 20, 24) else s.fail)(f"④ SAVE 按钮有强调底色 {save_bg[:1] if save_bg else None}")
    s.cmd("ENG TRACK 1 1 0")
    s.cmd("TAP %d %d" % inject.bank_act_xy(0))
    s.expect(r"TAP miss", "④ 换回 DIGITAL：bank 带 TAP SAVE ⇒ miss")
    s.cmd("TAP %d %d" % inject.bank_slot_btn_xy(+1))
    s.expect(r"TAP miss", "④ 换回 DIGITAL：bank 槽 + ⇒ miss")
    s.cmd("ENG TRACK 1 1 0")

    # ---- ⑤ GLOBAL / FX：chip 页、逐槽 label、首槽写后回读、20 次切 chip、鼓轨逐槽
    for p, chips in CHIP_PAGES.items():
        s.cmd(f"UI {p}")
        mm = re.search(rf"页面构建 page={p.upper()}\({CHIP_PAGE_ID[p]}\) 池增量=(\d+)B 预算=(\d+)B "
                       r"池剩余=(\d+)B 耗时=(\d+)ms", s.last)
        if mm:
            delta, est, free, ms = map(int, mm.groups())
            (s.ok if delta <= est <= CHIP_EST and ms <= 100 else s.fail)(
                f"⑤ {p.upper()} 首建 delta={delta}B 预算={est}B 池剩余={free}B 耗时={ms}ms")
        for c, (cname, nslots) in enumerate(chips):
            s.cmd(f"UI {p.upper()} CHIP {c}")
            s.expect(rf"UI {p} chip={c}\r?\n", f"⑤ {p.upper()}·{cname} 选中")
            slots = slot_lines(s, p)[1]
            (s.ok if len(slots) == nslots else s.fail)(
                f"⑤ {p.upper()}·{cname} 槽数 {len(slots)}（期望 {nslots}）")
            bad = []
            for k, (pid, label, locked) in sorted(slots.items()):
                if p == "global":
                    s.cmd(f"ENG DESC global 0 {pid}")
                else:
                    s.cmd(f"ENG DESC live 1 {pid}")
                mm2 = re.search(r'label="([^"]*)"', s.last)
                if locked != 0 or mm2 is None or mm2.group(1) != label:
                    bad.append(f"slot{k}:id={pid} {label} desc={mm2.group(1) if mm2 else '?'}")
            (s.ok if not bad else s.fail)(
                f"⑤ {p.upper()}·{cname} {len(slots)} 槽 label == 描述符且无锁定"
                + ("" if not bad else f" 不符 {bad}"))
            k0 = min(slots)
            pid0 = slots[k0][0]
            if p == "global":
                s.cmd(f"PARAM GLOBAL {pid0}")
                v0, mx = inject.param_value(s), inject.param_value(s, "max")
                s.cmd("TAP %d %d" % inject.param_knob_xy(k0))
                step = 1 if v0 < mx else -1
                s.cmd("TAP %d %d" % (inject.PARAM_BTN_PLUS if step > 0 else inject.PARAM_BTN_MINUS))
                inject.param_expect_value(s, f"PARAM GLOBAL {pid0}", v0 + step, f"⑤ GLOBAL·{cname} 首槽写")
                s.cmd("TAP %d %d" % (inject.PARAM_BTN_MINUS if step > 0 else inject.PARAM_BTN_PLUS))
                inject.param_expect_value(s, f"PARAM GLOBAL {pid0}", v0, f"⑤ GLOBAL·{cname} 回基线")
        s.cmd(f"UI {p.upper()} CHIP 0")
        s.cmd("UI MEM")
        f0 = inject.param_mem(s)["head"]["free"]
        for i in range(20):
            s.cmd(f"UI {p.upper()} CHIP {i % len(chips)}")
        s.cmd("UI MEM")
        f1 = inject.param_mem(s)["head"]["free"]
        (s.ok if abs(f1 - f0) <= 128 else s.fail)(
            f"⑤ {p.upper()} 20 次切 chip pool_free {f0} → {f1}（Δ={f1 - f0:+d}B）")
        img = s.capture(f"task5-{p}-chip0")

    # 鼓轨逐槽：FX chip 内混锁（TFLT/LEVEL/PAN 可编辑），其余 chip 全开；GLOBAL 全开（含 DRUMS）
    inject.select_track(s, 3)
    want_fx = {0: [1, 1, 1, 1, 0, 0, 0, 1], 1: [0] * 4, 2: [0] * 4, 3: [0]}
    for c, locks in want_fx.items():
        s.cmd("UI fx")
        s.cmd(f"UI FX CHIP {c}")
        slots = slot_lines(s, "fx")[1]
        got = [slots[k][2] for k in sorted(slots)]
        (s.ok if got == locks else s.fail)(
            f"⑤ 鼓轨 FX·{CHIP_PAGES['fx'][c][0]} locked={got}（期望 {locks}）")
    s.cmd("UI FX CHIP 0")
    s.cmd("PARAM 33")
    d0 = inject.param_value(s)
    s.cmd("TAP %d %d" % inject.param_knob_xy(0))
    s.cmd("TAP %d %d" % inject.PARAM_BTN_PLUS)
    s.expect(r"-> PARAM slot=0 locked", "⑤ 鼓轨 FX·DIST（锁定槽）TAP + → locked")
    inject.param_expect_value(s, "PARAM 33", d0, "⑤ 锁定槽值不变")
    s.cmd("UI FX CHIP 1")
    s.cmd("PARAM 45")
    sl0 = inject.param_value(s)
    slmax = inject.param_value(s, "max")
    s.cmd("TAP %d %d" % inject.param_knob_xy(0))
    step = 1 if sl0 < slmax else -1
    s.cmd("TAP %d %d" % (inject.PARAM_BTN_PLUS if step > 0 else inject.PARAM_BTN_MINUS))
    s.expect(rf"-> PARAM slot=0 id=45 delta=[+-]1 value={sl0 + step}", "⑤ 鼓轨 SLICER·SLCR 可编辑")
    s.cmd("TAP %d %d" % (inject.PARAM_BTN_MINUS if step > 0 else inject.PARAM_BTN_PLUS))
    s.cmd("UI FX CHIP 3")
    slots = slot_lines(s, "fx")[1]
    (s.ok if sorted(slots) == [0] and slots[0][0] == 57 else s.fail)(
        f"⑤ INSERT 2 = 单槽 IMIX(57)：{slots}")
    s.cmd("UI global")
    s.cmd("UI GLOBAL CHIP 2")
    slots = slot_lines(s, "global")[1]
    (s.ok if sorted(slots) == [0, 1, 2, 3] and [slots[k][0] for k in range(4)] == [24, 25, 26, 32]
     and not any(v[2] for v in slots.values()) else s.fail)(
        f"⑤ 鼓轨 GLOBAL·DRUMS id={[slots[k][0] for k in sorted(slots)]} 全可编辑")
    s.cmd("PARAM GLOBAL 24")
    ch0 = inject.param_value(s)
    chmax = inject.param_value(s, "max")
    s.cmd("TAP %d %d" % inject.param_knob_xy(0))
    step = 1 if ch0 < chmax else -1
    s.cmd("TAP %d %d" % (inject.PARAM_BTN_PLUS if step > 0 else inject.PARAM_BTN_MINUS))
    inject.param_expect_value(s, "PARAM GLOBAL 24", ch0 + step, "⑤ 鼓轨 DRCH 可编辑")
    s.cmd("TAP %d %d" % (inject.PARAM_BTN_MINUS if step > 0 else inject.PARAM_BTN_PLUS))
    inject.param_expect_value(s, "PARAM GLOBAL 24", ch0, "⑤ DRCH 回基线")
    img = s.capture("task5-global-drum")
    inject.select_track(s, 0)

    # ---- ⑥ UI MEM 与有序对峰值
    s.cmd("UI sound")
    mem = inject.param_mem(s)
    (s.ok if mem["head"]["registered"] == 17 else s.fail)(
        f"⑥ registered={mem['head']['registered']}（期望 17）")
    st, est, delta = mem["pages"][SOUND]
    (s.ok if st == "built" and est <= SOUND_EST and 0 < delta <= est else s.fail)(
        f"⑥ UI MEM SOUND state={st} est={est} delta={delta}")
    (s.ok if mem["groups"]["EDIT"]["sum_est"] == 16896 else s.fail)(
        f"⑥ GROUP EDIT sum_est={mem['groups']['EDIT']['sum_est']}（期望 16,896）")
    s.cmd("UI global")
    s.cmd("UI fx")
    mem = inject.param_mem(s)
    for pid, name, est_max in ((4, "GLOBAL", 7168), (5, "FX", 7168)):
        st, est, delta = mem["pages"][pid]
        (s.ok if st == "built" and est <= est_max and 0 < delta <= est else s.fail)(
            f"⑥ UI MEM {name} state={st} est={est} delta={delta}")
    (s.ok if mem["groups"]["MIX"]["sum_est"] == 7168 else s.fail)(
        f"⑥ GROUP MIX sum_est={mem['groups']['MIX']['sum_est']}（期望 7,168）")
    # EDIT 全建（SOUND/MOD/SCALE/VOICE）→ STEP；MIX 全建 → STEP；SEQ 全建 → MIX
    for p in ("sound", "mod", "scale", "voice"):
        s.cmd(f"UI {p}")
    build_peak(s, "step", 2, "⑥ EDIT 全建 → STEP")
    for p in ("global", "fx"):
        s.cmd(f"UI {p}")
    build_peak(s, "step", 2, "⑥ MIX 全建 → STEP")
    for p in ("step", "transport"):
        s.cmd(f"UI {p}")
    build_peak(s, "global", 4, "⑥ SEQ 全建 → MIX（GLOBAL）")
    for p in ("step", "transport"):
        s.cmd(f"UI {p}")
    build_peak(s, "fx", 5, "⑥ SEQ 全建 → MIX（FX）")

    # ---- ⑦ 别名与空位 id
    s.cmd("UI play")
    for name in ("fm6", "analog", "sound"):
        s.cmd(f"UI {name}")
        s.expect(rf"UI page=sound \({SOUND}\)", f"⑦ UI {name} → SOUND")
    s.cmd("UI unused")
    s.expect(r"UI ERR not_registered 7", "⑦ UI unused（空位 id 7）→ not_registered")
    s.cmd("UI FORCEFAIL 7")
    s.expect(r"UI ERR forcefail", "⑦ FORCEFAIL 7（空位页）→ UI ERR forcefail")
    s.cmd("UI fm6")
    s.expect(rf"UI page=sound \({SOUND}\)", "⑦ FORCEFAIL off 后 SOUND 仍可进入")
    s.cmd("UI sound CHIP")
    s.expect(r"UI sound chip=0", "⑦ UI sound CHIP 只读逐槽行")
    s.cmd("UI SOUND CHIP 1")
    s.expect(r"UI ERR chip", "⑦ SOUND 无 chip 行：CHIP 1 → UI ERR chip")

    # ---- 收口
    s.cmd("ENG TRACK 0 0 0")
    s.cmd("ENG TRACK 1 1 0")
    s.cmd("ENG TRACK 2 4 5")
    s.cmd("UI play")
    s.cmd("STORE AUTO ON")
    print(f"\n== {n_pass[0]} PASS / {s.failures} FAIL")
    return 0 if s.failures == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except inject.InjectError as exc:
        print(f"INJECT ERROR: {exc}")
        sys.exit(2)
EOF