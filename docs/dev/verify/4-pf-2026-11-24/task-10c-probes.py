"""Task 10c 欠账探针（一次性，不入库）：(a) A1 用户预置页眉；(f) 隐藏 chip TAP / CHIP 静默令牌 / TOOLS 预备过期。
断言口径与 inject.py 相同（PASS/FAIL 行，失败非零退出）；transcript 为原始回显。"""
import importlib.util, re, sys, time
from datetime import datetime
from pathlib import Path
spec = importlib.util.spec_from_file_location("inj", "scripts/inject/inject.py")
inj = importlib.util.module_from_spec(spec); spec.loader.exec_module(inj)
inj.require_deps()
port = sys.argv[1]; what = sys.argv[2]
out_dir = Path("/tmp/p4f_10c/probes"); out_dir.mkdir(parents=True, exist_ok=True)
stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
s = inj.Session(port, out_dir / f"probe-{what}-{stamp}.log", out_dir, stamp, f"probe-{what}")
s.wait_ready()


def a1():
    inj.ensure_stopped(s)
    s.cmd("STORE AUTO OFF"); s.expect(r"STORE AUTO off", "A1 AUTO OFF")
    s.cmd("STORE PRESET LIST"); used0 = not re.search(r"^PRESET 0 EMPTY\r?$", s.last, re.M)
    (s.ser.note if used0 else s.ok)(f"A1 槽 0 入口{'非空' if used0 else '为空'}")
    inj.select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0"); s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "A1 轨 0 归一 ANALOG 预置 0")
    s.cmd("STORE PRESET ERASE 0"); s.expect(r"^PROJ|PRESET 0 ERASED|^STORE ERR empty", "A1 擦槽 0")
    s.cmd("PARAM 63"); m = re.search(r"PARAM track=0 id=63 value=(-?\d+) min=(-?\d+) max=(-?\d+)", s.last)
    v, lo, hi = (int(m.group(i)) for i in (1, 2, 3))
    v2 = v + 1 if v < hi else v - 1
    s.cmd(f"PARAM 63 {v2}"); s.expect(rf"PARAM track=0 id=63 .*applied={v2}", f"A1 改 P_E0 {v} → {v2}")
    s.cmd("STORE PRESET SAVE 0"); s.expect(r"^PRESET 0 SAVED ANALOG .+\r?$", "A1 SAVE 0")
    s.cmd(f"PARAM 63 {v}"); s.expect(rf"PARAM track=0 id=63 .*applied={v}\b", f"A1 P_E0 改回 {v}（等载入）")
    s.cmd("STORE PRESET LOAD 0 0"); s.expect(r"^PRESET 0 LOADED track=0\r?$", "A1 LOAD 0 0")
    s.cmd("PARAM 63"); s.expect(rf"PARAM track=0 id=63 value={v2} ", f"A1 载入后 P_E0 = {v2}（来自用户预置）")
    s.cmd("UI sound"); s.expect(r"UI page=sound \(6\)", "A1 UI sound")
    time.sleep(0.2)
    img_user = s.capture("a1-user-preset")
    s.cmd("TAP %d %d" % inj.sound_row_mid(1)); s.expect(r"TAP miss", "A1 PRESET 行中段（标签）TAP miss")
    s.cmd("TAP %d %d" % inj.sound_row_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=1 name="(?!USER)[^"]+" status="OK"', "A1 从用户预置按 + ⇒ 工厂预置 1（名字不是 USER nn）")
    name_plus = re.search(r'name="([^"]+)"', s.last).group(1)
    time.sleep(0.2)
    img_fac = s.capture("a1-factory-after-plus")
    # 页眉 PRESET 行的名字区域（标签中段：去掉左右 42px 的 ∓1 区）
    x, w = inj.SOUND_ROWS[1]
    box = (x + inj.SOUND_ROW_ZONE, inj.SOUND_ROW_Y, x + w - inj.SOUND_ROW_ZONE, inj.SOUND_ROW_Y + inj.SOUND_ROW_H)
    a, b = img_user.crop(box), img_fac.crop(box)
    diff = sum(1 for p, q in zip(a.getdata(), b.getdata()) if p != q)
    (s.ok if diff >= 40 else s.fail)(f"A1 页眉 PRESET 名字区 {box} 在「用户预置」与「工厂预置」两帧间不同像素 {diff}（≥40 ⇒ 页眉文字确实变了）")
    (s.ok if name_plus != "USER 01" else s.fail)(f"A1 工厂名 = {name_plus!r}")
    s.cmd("STORE PRESET LOAD 0 0"); s.expect(r"^PRESET 0 LOADED track=0\r?$", "A1 再载入用户预置")
    s.cmd("TAP %d %d" % inj.sound_row_xy(1, -1))
    got = re.search(r"-> SOUND PRESET value=(\d+) name=\"([^\"]+)\" status=\"OK\"", s.last)
    (s.ok if got and int(got.group(1)) > 1 and not got.group(2).startswith("USER") else s.fail)(
        f"A1 从用户预置按 − ⇒ 环绕到该引擎最后一个工厂预置（{got.group(0) if got else s.last.strip()[-120:]}）")
    s.cmd("STORE PRESET LIST"); s.expect(r"^PRESET 0 USED ANALOG .*\r?$", "A1 槽 0 仍 USED（± 只清 user 引用）")
    s.cmd("ENG TRACK 0 0 0"); s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "A1 收口：轨 0 回工厂预置 0")
    if not used0:
        s.cmd("STORE PRESET ERASE 0"); s.expect(r"PRESET 0 ERASED", "A1 收口擦槽 0（入口为空）")
    else:
        s.ser.note("A1 槽 0 入口非空：原内容已被本探针的 SAVE 覆盖，不擦")
    s.cmd("STORE AUTO ON"); s.expect(r"STORE AUTO on", "A1 收口 AUTO ON")


def f():
    inj.ensure_stopped(s)
    # f1 刚变隐藏的 chip：ANALOG 轨选中 VOICE·FILTER(chip 3) → 同一批里换成 DIGITAL 并立刻 TAP 那一格
    inj.select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0"); s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "f1 轨 0 = ANALOG")
    s.cmd("UI voice"); s.expect(r"UI page=voice \(17\)", "f1 UI voice")
    s.cmd("UI VOICE CHIP 3"); s.expect(r"UI voice chip=3", "f1 选中 FILTER chip")
    xy = inj.chip_center("voice", 3)
    s.cmd_batch(["ENG TRACK 0 1 0", "TAP %d %d" % xy])
    s.expect(r"TAP miss", "f1 换引擎后同批 TAP 刚隐藏的 FILTER chip ⇒ TAP miss")
    s.expect(r"(?s)^(?!.*chip=255).*$", "f1 回显不含 chip=255")
    s.cmd("TAP %d %d" % xy); s.expect(r"TAP miss", "f1 一拍之后再 TAP 隐藏 chip ⇒ TAP miss")
    s.cmd("ENG TRACK 0 0 0"); s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "f1 回 ANALOG")
    s.cmd("UI VOICE CHIP 3"); s.expect(r"UI voice chip=3", "f1 FILTER chip 重新可选")
    s.cmd("UI VOICE CHIP 0"); s.expect(r"UI voice chip=0", "f1 生效 chip 回 0")
    # f2 CHIP 静默令牌
    s.cmd("UI mod"); s.expect(r"UI page=mod \(15\)", "f2 UI mod")
    s.cmd("UI MOD CHIP"); chip0 = re.search(r"UI mod chip=(\d+)", s.last).group(1)
    s.cmd("UI QUIESCE on"); s.expect(r"UI QUIESCE on quiesced=1", "f2 QUIESCE on")
    s.cmd("UI MOD CHIP 1"); s.expect(r"^UI ERR quiesced\r?$", "f2 静默期 UI MOD CHIP 1 ⇒ UI ERR quiesced")
    s.cmd("UI MOD CHIP"); s.expect(rf"UI mod chip={chip0}\b", "f2 选中 chip 未变")
    s.cmd("UI QUIESCE off"); s.expect(r"UI QUIESCE off quiesced=0", "f2 QUIESCE off")
    s.cmd("UI MOD CHIP 1"); s.expect(r"UI mod chip=1", "f2 解除静默后 UI MOD CHIP 1 生效")
    s.cmd(f"UI MOD CHIP {chip0}")
    # f3 TOOLS 预备过期：等 >1.5s 再点同一 chip ⇒ 重新预备而不是执行
    s.cmd("STATE SNAP"); sel = s.snap_int(r"selected_track=(\d+)")
    h0 = inj.live_hash(s)
    s.cmd("UI tools"); s.expect(r"UI page=tools \(18\)", "f3 UI tools")
    s.cmd("TAP %d %d" % inj.TOOLS_CHIP[0]); s.expect(r'-> TOOLS CLRSEQ armed status="AGAIN TO CONFIRM"', "f3 第一击 = 预备")
    time.sleep(2.0)
    s.cmd("TAP %d %d" % inj.TOOLS_CHIP[0])
    s.expect(r'-> TOOLS CLRSEQ armed status="AGAIN TO CONFIRM"', "f3 等待 2.0s（>1.5s 窗口）后第二击 ⇒ 重新预备")
    s.expect(r"(?s)^(?!.*value=).*$", "f3 第二击未执行（回显无 value=）")
    h1 = inj.live_hash(s); (s.ok if h1 == h0 else s.fail)(f"f3 第二击后 LIVEHASH {h1:08X} == {h0:08X}（未清步表）")
    s.cmd("UI QUIESCE on"); s.expect(r"UI QUIESCE on quiesced=1", "f3 预备期间 QUIESCE on（没有 UI 拍）")
    time.sleep(2.0)
    s.cmd("UI QUIESCE off"); s.expect(r"UI QUIESCE off quiesced=0", "f3 QUIESCE off")
    s.cmd("TAP %d %d" % inj.TOOLS_CHIP[0])
    s.expect(r'-> TOOLS CLRSEQ armed status="AGAIN TO CONFIRM"', "f3 静默 2.0s（无 UI 拍）后第二击 ⇒ 重新预备（按下时也判过期）")
    h2 = inj.live_hash(s); (s.ok if h2 == h0 else s.fail)(f"f3 第三击后 LIVEHASH {h2:08X} == {h0:08X}（未清步表）")
    # 判别力：窗口内紧接着第三击 ⇒ 执行。UNDO 只换回 step[]/micro[]/LEN（上游 undo_swap），而 CLRSEQ 的
    # steps_clear 还清 fill/lock ⇒ 还原的判据用步表（SEQ DSTEP）而不是 LIVEHASH
    s.cmd("SEQ DSTEP"); steps0 = s.last
    s.cmd("TAP %d %d" % inj.TOOLS_CHIP[0])
    s.expect(r'-> TOOLS CLRSEQ value=0 status="PATTERN CLEARED"', "f3 判别力：窗口内紧接着第三击 ⇒ 执行")
    s.cmd("SEQ DSTEP"); (s.ok if s.last != steps0 else s.fail)("f3 执行后步表已变（清空）")
    s.cmd(f"SEQ PATTERN UNDO {sel}"); s.expect(r"SEQ PATTERN UNDO .*OK", "f3 UNDO 还原步表")
    s.cmd("SEQ DSTEP"); (s.ok if s.last == steps0 else s.fail)("f3 UNDO 后 SEQ DSTEP 逐行 == 执行前")
    h3 = inj.live_hash(s)
    (s.ok if h3 == h0 else s.ser.note)(f"f3 UNDO 后 LIVEHASH {h3:08X} == {h0:08X}" if h3 == h0 else
        f"f3 note：UNDO 后 LIVEHASH {h3:08X} != {h0:08X}（上游 undo 不含 fill/lock，见 finv 探针）")
    s.cmd("UI play"); s.expect(r"UI page=play \(0\)", "f3 收口 UI play")


try:
    {"a1": a1, "f": f}[what]()
except inj.InjectError as e:
    s.fail(f"异常终止: {e}")
finally:
    s.ser.note(f"probe {what} 结束 failures={s.failures}")
    s.ser.close()
print(f"[{'PASS' if s.failures == 0 else 'FAIL'}] probe {what}: failures={s.failures} transcript={s.log_path}")
sys.exit(1 if s.failures else 0)
