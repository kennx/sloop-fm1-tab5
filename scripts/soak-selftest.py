#!/usr/bin/env python3
"""soak-selftest.py — `scripts/soak.py` 的窗口前「停止 → 排空 dirty → 取基线」顺序与
dirty 解析的 host 单测（无设备、无串口）。

背景（Task 12 终审 I）：`sloop_store_sections_tick()` 在 `song.playing || transport_req ||
!audio_quiet()` 时直接跳过，走带中调用 `wait_dirty_clear()` 只会空等到超时（假 FAIL）。
本测试用假 Session 证明：
  1. 源码顺序：`save_window` 里 `inject.ensure_stopped` → `wait_dirty_clear` →
     `perf_counters`（基线）；
  2. 运行时顺序（假 Session 记录命令）：`--section-capture` 的播放中 STORE → STOP →
     第一个 `SEQ SECTION` 轮询 → `STORE PROJ SAVE 0`；
  3. `wait_dirty_clear` 的解析/超时/`expect_dirty` 语义（回显畸形即 fail，不死循环）；
  4. `ENG POWER` 采样列（P4-F Task 9）：ok / ina=err / 行缺失三种回显的解析、摘要的
     记录行（min/max 与有效次数），且 `verdict()` 不因功耗/温度列产生判定问题（仅记录）。

用法: python3 scripts/soak-selftest.py    （退出码 0 = 全过）
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import soak  # noqa: E402

FAILS = []


def check(ok: bool, what: str) -> None:
    print(f"  {'[PASS]' if ok else '[FAIL]'} {what}")
    if not ok:
        FAILS.append(what)


class FakeSer:
    def __init__(self):
        self.notes = []

    def note(self, text: str) -> None:
        self.notes.append(text)


class FakeSession:
    """最小 Session 替身：按命令给出回显；记录命令序、playing 与 dirty 演化。"""

    def __init__(self):
        self.last = ""
        self.failures = 0
        self.cmds = []
        self.ser = FakeSer()
        self.playing = True
        self.dirty_sec = 0
        self.dirty_song = 0
        self._stopped_polls = 0
        self.bad_dirty = False
        self.power = "ENG POWER vbus_mv=918 shunt_uv=-5 cur_ma=-1 temp_c=27.1 ina=ok"

    # ---- Session 接口 ----
    def cmd(self, line: str, timeout: float = None) -> str:
        self.cmds.append(line)
        if line == "STORE AUTO OFF":
            self.last = "STORE AUTO off\nsloop>"
        elif line == "SEQ SECTION A STORE":
            self.dirty_sec = 1
            self.last = "SEQ SECTION A STORE OK\nsloop>"
        elif line == "SEQ SECTION":
            if self.bad_dirty:
                self.last = "SEQ ERR invalid\nsloop>"
            else:
                if self.playing:
                    d = 1 if self.dirty_sec else 0      # 播放中 tick 不写
                else:
                    self._stopped_polls += 1
                    if self.dirty_sec and self._stopped_polls >= 2:
                        self.dirty_sec = 0              # 停止后 1s 任务落盘
                    d = self.dirty_sec
                self.last = (f"SEQ SECTION ready=0x03 live=A dirty={d} index=0 bar=0\n"
                             "sloop>")
        elif line == "STORE SONG":
            self.last = (f"STORE SONG count=1 loop=0 dirty={self.dirty_song}\n"
                         "SONG 0 scene=0 bars=4\nsloop>")
        elif line == "STATE SNAP":
            self.last = (f"STATE SNAP page=3 focus_track=0 focus_step=0 layer_held=0x00 "
                         f"selected_track=0 last_note=0 bpm=120 playing={1 if self.playing else 0} "
                         "window_bank=0 window_start=0 window_len=16 seq_status=0 rec=off\n"
                         "sloop>")
        elif line.startswith("TAP 560 210"):
            self.playing = False
            self.last = "TAP 560,210 page=3 -> TRANSPORT CMD_STOP\nsloop>"
        elif line.startswith("TAP 208 210"):
            self.playing = True
            self.last = "TAP 208,210 page=3 -> TRANSPORT CMD_PLAY\nsloop>"
        elif line == "UI transport":
            self.last = "UI page=transport (3)\nsloop>"
        elif line == "STORE PROJ SAVE 0":
            self.last = "PROJ 0 SAVED hash=DEADBEEF\nsloop>"
        elif line == "STORE PROJ LOAD 0":
            self.last = "PROJ 0 LOADED hash=DEADBEEF\nsloop>"
        elif line == "ENG PERF":
            self.last = ("ENG PERF streaming=1 xrun=0 audio_stack_free=3128 render_blocks=1 "
                         "render_mean_us=700 duty_mean=1.0% duty_max=2.0%\nsloop>")
        elif line == "STORE AUTO ON":
            self.last = "STORE AUTO on\nsloop>"
        elif line == "ENG POWER":
            self.last = f"{self.power}\nsloop>"
        else:
            self.last = "sloop>"
        return self.last

    def expect(self, pattern: str, label: str = "") -> None:
        if re.search(pattern, self.last, re.MULTILINE) is None:
            self.fail(f"expect 未匹配：{label or pattern}（{self.last!r}）")

    def snap_int(self, pattern: str) -> int:
        m = re.search(pattern, self.last, re.MULTILINE)
        if m is None:
            raise RuntimeError(f"snap_int 缺少 {pattern!r}: {self.last!r}")
        return int(m.group(1))

    def expect_int(self, pattern: str, value: int, label: str) -> None:
        m = re.search(pattern, self.last, re.MULTILINE)
        if m is None or int(m.group(1)) != value:
            self.fail(f"expect_int 未匹配：{label}（期望 {value}，{self.last!r}）")

    def ok(self, text: str) -> None:
        pass

    def fail(self, text: str) -> None:
        self.failures += 1
        print(f"     [fake FAIL] {text}")


def fake_sleep(_seconds: float) -> None:   # 不真的等窗口/dirty 轮询
    return None


def test_source_order() -> None:
    src = Path(soak.__file__).read_text()
    body = src[src.index("def save_window("):src.index("def cycle(", src.index("def save_window("))]
    i_stop = body.index("inject.ensure_stopped(s)")
    i_wait = body.index("wait_dirty_clear(s")
    i_base = body.index("perf_counters(s)")
    check(i_stop < i_wait < i_base,
          f"save_window 源码顺序 stop({i_stop}) < wait_dirty_clear({i_wait}) < 基线({i_base})")


def test_save_window_order(section_capture: bool) -> None:
    s = FakeSession()
    real_sleep = soak.time.sleep
    soak.time.sleep = fake_sleep
    try:
        win = soak.save_window(s, 0, lambda: {"t": 0}, section_capture)
    finally:
        soak.time.sleep = real_sleep
    cmds = s.cmds
    stop_i = cmds.index("TAP 560 210")
    dirty_i = next(i for i, c in enumerate(cmds) if c == "SEQ SECTION" and i > stop_i)
    save_i = cmds.index("STORE PROJ SAVE 0")
    tag = "--section-capture" if section_capture else "默认"
    check(stop_i < dirty_i < save_i,
          f"{tag}：STOP({stop_i}) < dirty 轮询({dirty_i}) < SAVE({save_i})")
    if section_capture:
        cap_i = cmds.index("SEQ SECTION A STORE")
        check(cap_i < stop_i, f"{tag}：播放中 STORE section A({cap_i}) 在 STOP({stop_i}) 之前")
        check(s.dirty_sec == 0 and s._stopped_polls >= 2,
              f"{tag}：停止后轮询 {s._stopped_polls} 次把 dirty 排空（期望 ≥2）")
    check(s.failures == 0 and win["delta"] == 0,
          f"{tag}：save_window 全断言通过（failures={s.failures}, xrun 增量={win['delta']}）")


def test_wait_dirty_clear() -> None:
    # 1) normal: 1,1,0 → True（且观察到 dirty=1）
    s = FakeSession()
    s.playing = False
    s.dirty_sec = 1
    real_sleep = soak.time.sleep
    soak.time.sleep = fake_sleep
    try:
        check(soak.wait_dirty_clear(s, timeout=1.0), "dirty 1→0：返回 True")
        check(s.failures == 0, "dirty 1→0：无 fail")
    finally:
        soak.time.sleep = real_sleep
    # 2) expect_dirty=True 但一直 0 → False + fail
    s = FakeSession()
    s.playing = False
    check(not soak.wait_dirty_clear(s, timeout=1.0, expect_dirty=True),
          "expect_dirty 但恒 0：返回 False")
    check(s.failures == 1, "expect_dirty 但恒 0：记 1 次 fail")
    # 3) 回显畸形 → False + fail（不死循环/不误判为干净）
    s = FakeSession()
    s.playing = False
    s.bad_dirty = True
    check(not soak.wait_dirty_clear(s, timeout=1.0), "dirty 回显畸形：返回 False")
    check(s.failures >= 1, "dirty 回显畸形：记 fail")
    # 4) 超时（恒 dirty=1）→ False + fail
    s = FakeSession()
    s.playing = False
    s.dirty_sec = 1
    s._stopped_polls = -10**6          # 让假 tick 永远不落盘
    check(not soak.wait_dirty_clear(s, timeout=0.05), "恒 dirty=1：超时返回 False")
    check(s.failures == 1, "恒 dirty=1：记 1 次 fail")


def test_power_columns() -> None:
    import tempfile
    rows, sessions = [], []

    def sampled(power: str = None) -> dict:
        s = FakeSession()
        if power is not None:
            s.power = power                       # 旧固件没有 POWER 时回显的是用法行
        sessions.append(s)
        return soak.sample(s)
    rows.append(sampled())                         # 首行 = 正常读数
    check(rows[0]["vbus_mv"] == 918 and rows[0]["shunt_uv"] == -5 and rows[0]["cur_ma"] == -1
          and rows[0]["temp_c"] == 27.1 and rows[0]["ina"] == "ok",
          f"ENG POWER ok 行入采样列：{ {k: rows[0][k] for k in soak.POWER_COLS} }")
    rows.append(sampled("ENG POWER vbus_mv=na shunt_uv=na cur_ma=na temp_c=na ina=err"))
    check(all(rows[1][k] is None for k in soak.POWER_COLS[:4]) and rows[1]["ina"] == "err",
          "ina=err 行：四个读数列为 None、ina=err")
    rows.append(sampled("用法: ENG INFO|SNAP|…"))
    check(all(rows[2][k] is None for k in soak.POWER_COLS), "行缺失/畸形：五列全 None（不判失败）")
    for i, r in enumerate(rows):
        r["t"] = i * 60
    lines = soak.power_record(rows)
    text = "\n".join(lines)
    check(len(lines) == 2 and "功耗记录（仅记录，不设门禁）" in lines[0]
          and "温度记录（仅记录，不设门禁）" in lines[1], "两条记录行且标明「仅记录，不设门禁」")
    check("ina=ok 1/3 次" in text and "vbus_mv 918..918 mV（1/3 次有效）" in text
          and "shunt_uv -5..-5 µV（1/3 次有效）" in text and "cur_ma -1..-1 mA（1/3 次有效）" in text,
          f"功耗记录：min/max 与有效次数 → {lines[0]}")
    check("temp_c 27.1..27.1 ℃（1/3 次有效），首 27.1 → 末 27.1" in text, f"温度记录：首末 → {lines[1]}")
    check(all(s.failures == 0 for s in sessions), "soak.sample 的既有断言（playing=1）不受 ENG POWER 影响")
    # 假 Session 不回 ENG MEM / KEY STATS / UI MEM：核心列补成合格值，只留功耗/温度列不合格
    for r in rows:
        r.update(internal_free=70000, internal_min=69000, streaming="1", xrun=0,
                 audio_stack_free=3128)
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "soak.log"
        log.write_text("sloop> ENG POWER vbus_mv=na ina=err\nsloop>")
        check(soak.verdict(rows, log, 100.0, 100.0) == [],
              "verdict 不因功耗/温度列（ina=err、na）产生判定问题（无新门禁）")
        rows[1]["streaming"] = "0"
        check(len(soak.verdict(rows, log, 100.0, 100.0)) == 1, "对照：同一组行把 streaming 置 0 仍判 1 项（门禁未被削弱）")
    s = FakeSession()
    s.power = "ENG POWER vbus_mv=na shunt_uv=na cur_ma=na temp_c=na ina=err"
    lines = soak.power_record([dict(t=0, **soak.parse_power(s.cmd("ENG POWER")))])
    check("vbus_mv na（0/1 次有效）" in lines[0] and "temp_c na（0/1 次有效）" in lines[1],
          f"全程无有效读数：记录行回 na → {lines}")


FIRMWARE_SRC = Path(soak.inject.__file__).resolve().parents[2] / "firmware" / "components"


def test_tables() -> None:
    """页表定稿（P4-F Task 7；5B-1 起五张族页退役）：soak 的 PAGES / NAV_ORDER / GROUP_PAGES /
    页签几何常量与代表页必须和固件源码说同一张导航——组表与几何取自 ui_nav.h（UI_GROUP_LIST、
    UI_GROUP_W…），页 id 取自 sloop_state.h 的 app_page_t（APP_PAGE_FM6=7 是空位；GLOBAL/FX/
    MOD/SCALE/VOICE 是不注册的退役 id——都不在 PAGES 里）。"""
    nav_h = (FIRMWARE_SRC / "sloop_ui" / "src" / "ui_nav.h").read_text(encoding="utf-8")
    state_h = (FIRMWARE_SRC / "sloop_state" / "include" / "sloop_state.h").read_text(encoding="utf-8")
    body = state_h[state_h.index("typedef enum {", state_h.index("#define APP_LAYER_GLO")):]
    body = re.sub(r"/\*.*?\*/", "", body[:body.index("} app_page_t;")], flags=re.S)
    page_id = {name.strip(): i
               for i, name in enumerate(body[len("typedef enum {"):].split(",")) if name.strip()}
    fw_groups = re.findall(r"^\s*X\((\w+)((?:, APP_PAGE_\w+)+)\)", nav_h, re.MULTILINE)
    fw = {name: tuple(page_id[p.strip()] for p in pages.split(",") if p.strip())
          for name, pages in fw_groups}
    flat_fw = [pid for members in fw.values() for pid in members]
    soak_groups = {g.upper(): v for g, v in soak.GROUP_PAGES.items()}
    check(len(fw_groups) == 9 and list(fw) == list(soak_groups) == list(soak.inject.UI_GROUP_NAMES),
          "九组且组顺序一致（ui_nav.h = GROUP_PAGES = inject.UI_GROUP_NAMES）")
    check(soak_groups == fw, "组成员与页 id 逐一一致（ui_nav.h UI_GROUP_LIST = GROUP_PAGES）")
    retired = {4, 5, 7, 15, 16, 17}
    check(len(soak.PAGES) == 13 and set(soak.PAGES.values()) == set(flat_fw) == set(range(19)) - retired,
          "13 页：PAGES 的 id 集合 = 组表平铺 = 0..18 去掉空位 7 与退役页 4/5/15/16/17")
    check([pid for pid, _ in soak.NAV_ORDER] == flat_fw,
          "NAV_ORDER 的页 id 序列 = 组表平铺（TAB 扁平轮转顺序）")
    check({name: pid for pid, name in soak.NAV_ORDER} == soak.PAGES,
          "NAV_ORDER 的页名 = PAGES 的键（页 id 同值）")
    defs = {k: int(v) for k, v in re.findall(
        r"^#define (UI_TAB_X0|UI_TAB_GAP|UI_GROUP_W|UI_SUB_W|UI_GROUP_MAX)\s+(\d+)", nav_h, re.M)}
    check((soak.TAB_X0, soak.TAB_GROUP_W, soak.TAB_GROUP_GAP, soak.TAB_SUB_W, soak.TAB_SUB_SLOTS)
          == (defs["UI_TAB_X0"], defs["UI_GROUP_W"], defs["UI_TAB_GAP"], defs["UI_SUB_W"],
              defs["UI_GROUP_MAX"]) == (16, 104, 8, 176, 5),
          "页签几何与 ui_nav.h 一致且为 16/104/8/176/5（组签 104px）")
    reps = {g: (p if isinstance(p, tuple) else (p,)) for g, p in soak.POOL_RT_REP.items()}
    check(set(reps) == set(soak.GROUP_PAGES) - {"play"} and len(soak.pool_rt_pages(0)) == 8
          and len(soak.pool_rt_pages(1)) == 8
          and all(soak.PAGES[page] in soak.GROUP_PAGES[g] for g, pages in reps.items() for page in pages),
          "POOL_RT_REP = 八个非 PLAY 组，每轮 8 张代表页，各页属于所代表的组")
    check(soak.POOL_RT_GROUPS == soak.pool_rt_pages(0) and "tools" in soak.pool_rt_pages(0),
          "MIX 代表页 = tools（5B-1 起 MIX 只有 TOOLS，不再按轮交替）")
    check(all(soak.pool_rt_pages(r) == soak.POOL_RT_GROUPS for r in range(soak.POOL_RT_ROUNDS)),
          "POOL_RT_ROUNDS 内每轮都是同样 8 张代表页")


def main() -> int:
    print("== soak-selftest（无设备）")
    test_tables()
    test_source_order()
    test_save_window_order(False)
    test_save_window_order(True)
    test_wait_dirty_clear()
    test_power_columns()
    print(f"\n结果: {'PASS' if not FAILS else 'FAIL (%d)' % len(FAILS)}")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
