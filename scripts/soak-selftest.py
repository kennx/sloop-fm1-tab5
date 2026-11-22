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
  3. `wait_dirty_clear` 的解析/超时/`expect_dirty` 语义（回显畸形即 fail，不死循环）。

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


def main() -> int:
    print("== soak-selftest（无设备）")
    test_source_order()
    test_save_window_order(False)
    test_save_window_order(True)
    test_wait_dirty_clear()
    print(f"\n结果: {'PASS' if not FAILS else 'FAIL (%d)' % len(FAILS)}")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
