#!/usr/bin/env python3
"""FM-1 联动协议冒烟门禁（AGENTS.md §5.5「联动协议」；操作员在环）。

Usage:
    python3 scripts/fm1-smoke.py --selftest          # 离线自测（无需设备）
    python3 scripts/fm1-smoke.py /dev/ttyACM0        # 真机：FM-1 插在 Tab5 的 USB-A 口，操作员在场

流程（7 步，超时即失败并写明是哪一步）。需要等待的步骤都是**先发一次命令**：回复已满足就直接过，
否则打印操作员提示，再按间隔轮询（步骤 2 只有轮询、没有提示）：
  1. `FM1 STATUS`：已 link=ready 直接过；否则打印「请把刷了 SLOOP 的 FM-1 插到 Tab5 的 USB-A 口」，
     每 2s 轮询，最多 120s；
  2. `FM1 STATUS` 轮询（5s）到 link=ready / watch=3 / info=yes；`FM1 INFO` 断言版本串前缀
     `FELUCCA SLOOP 2.`、proto=12、nengines=12、ntrk=4；
  3. `FM1 PING` ×10 全部成功，记录 ms 中位数；
  4. `FM1 GET 0 1`（选中轨 ATK）→ `SET` 成相邻值 → `GET` 读回 → `SET` 原值还原并读回确认
     （失败路径同样尽力还原）；
  5. `FM1 EVENTS CLEAR` → 轮询 `FM1 EVENTS` 到 changed ≥ 1（60s；首次回复未达时打印
     「请在 FM-1 上拧任意一个旋钮」），且判定的那行 `last=` 是 `cmd=23 …`（拧旋钮 = CHANGED 推送；
     RELOAD 等不算）；
  6. 静置 10s（会话层每秒 PING 保活）→ 轮询到 changed 续增（60s；首次未达时打印「请在 FM-1 上再拧一次旋钮」）；
  7. 轮询到 link=none（60s；首次未达时打印「请拔掉 FM-1」）。判定的那行必须是 `watch=0 info=no`
     （链路断开时会话层遗忘 WATCH 与 INFO）；首行报 link=none 但尚未归零时，隔 200ms 重读一次、以第二行为准
     → `FM1 INFO` 为 `ERR no_link` → 轮询到 link=ready watch=3 info=yes（120s；首次未达时打印
     「请重新插上」）→ `FM1 PING` 成功。
  不断言 `timeouts`/`stray` 为 0：拔线瞬间与 PING 竞争的超时是合法的。
操作员 Ctrl-C 中止（等待中唯一的退出方式）同样落一条 error 注记，并以 `FM1 SMOKE FAIL` 收尾、退出码 1
（不打印 traceback）。

断言全部落在纯函数 judge(lines)（输入转录，返回失败说明，空表 = 通过）：转录里
`# [fm1-smoke] <步> <tag>` 起一段，段内最后一条非回显的 `FM1 ` 回复行是该段的判据；
GET/SET 的值关系与 EVENTS 的 changed 续增跨段比对。`--selftest` 用六组内嵌样例调 judge
（全通过 / `FM1 INFO ERR timeout` / `FM1 EVENTS` changed=0 / `FM1 STATUS` link=none /
changed 够但 `last=cmd=24` / `link=none` 但 `watch=3`）。

transcript（原始回显 + `# [fm1-smoke] ...` 工具注记）落
`firmware/build/verify/fm1-smoke-<时间戳>.log`；
末行 `FM1 SMOKE PASS n/n` 或 `FM1 SMOKE FAIL …`，退出码 0/1。
串口会话与复位写法沿用 scripts/selftest-heap.py（esptool 硬复位后重开串口）。
"""
import argparse
import re
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import screenshot as shot  # noqa: E402

BAUD = 115200
NOTE = "# [fm1-smoke]"
PINGS = 10
ANSWER_TIMEOUT = 5.0
BOOT_BANNER = re.compile(r"^ESP-ROM:|^rst:0x")

STEPS = {
    1: "接入：FM1 STATUS link=ready",
    2: "就绪与版本协商：STATUS watch=3 info=yes + FM1 INFO",
    3: "PING ×10",
    4: "GET/SET 往返并还原 ATK",
    5: "WATCH 推送：拧旋钮后 changed ≥ 1 且 last=cmd=23",
    6: "静置 10s 后保活：再拧 changed 续增",
    7: "拔线/重插：link=none watch=0 info=no → ERR no_link → 重新就绪",
}

# 期望：每条都是整行正则（console-and-injection.md 的 FM1 行，输出格式逐字固定）
_STATUS_DEV = r"vid=0x[0-9A-F]{4} pid=0x[0-9A-F]{4} product=\S+ rx=\d+ tx=\d+ drops=\d+ open_fail=\d+"
_STATUS_MIDI = r" midi_if=\d+ ep_in=0x[0-9A-F]{2} ep_out=0x[0-9A-F]{2}"
RX_STATUS_LINK = re.compile(r"^FM1 STATUS link=ready " + _STATUS_DEV + _STATUS_MIDI
                            + r" watch=[013] info=(?:yes|no) proto=(?:\d+|-)$")
RX_STATUS_READY = re.compile(r"^FM1 STATUS link=ready " + _STATUS_DEV + _STATUS_MIDI
                             + r" watch=3 info=yes proto=\d+$")
RX_STATUS_GONE = re.compile(r"^FM1 STATUS link=none " + _STATUS_DEV
                            + r"(?: midi_if=\d+ ep_in=0x[0-9A-F]{2} ep_out=0x[0-9A-F]{2})?"
                            + r" watch=[013] info=(?:yes|no) proto=(?:\d+|-)$")
RX_STATUS_FORGOT = re.compile(r"^FM1 STATUS link=none " + _STATUS_DEV
                              + r"(?: midi_if=\d+ ep_in=0x[0-9A-F]{2} ep_out=0x[0-9A-F]{2})?"
                              + r" watch=0 info=no proto=(?:\d+|-)$")
RX_INFO = re.compile(r'^FM1 INFO version="FELUCCA SLOOP 2\.[^"]*" nengines=12 p_count=\d+ g_count=\d+'
                     + r" nstep=\d+ pe0=\d+ ntrk=4 proto=12 engines=\S+$")
RX_INFO_NO_LINK = re.compile(r"^FM1 INFO ERR no_link$")
RX_GET = re.compile(r"^FM1 GET scope=0 id=1 value=(-?\d+)$")
RX_SET = re.compile(r"^FM1 SET scope=0 id=1 value=(-?\d+)$")
RX_SET_ECHO = re.compile(r"^FM1 SET 0 1 -?\d+$")  # 命令回显（值随原值变，不能按固定文本认）
RX_PING = re.compile(r"^FM1 PING ms=(\d+)$")
RX_EVENTS = re.compile(r"^FM1 EVENTS total=\d+ changed=(\d+) reload=\d+ step_changed=\d+ track_changed=\d+"
                       + r" timeouts=\d+ stray=\d+ last=(none|cmd=\d+ scope=\d+ id=\d+ value=-?\d+)$")
RX_EVENTS_CLEARED = re.compile(r"^FM1 EVENTS cleared$")
RX_LAST_CHANGED = re.compile(r"cmd=23(?: |$)")  # last= 字段里的 CHANGED；带边界，cmd=230… 不算

# 判据次序：每段一个 tag；段缺失（流程中途失败）只报第一条
PLAN = [(1, "link"), (2, "ready"), (2, "info")]
PLAN += [(3, f"ping{i}") for i in range(1, PINGS + 1)]
PLAN += [(4, "get_orig"), (4, "set_tmp"), (4, "get_tmp"), (4, "set_restore"), (4, "get_restore"),
         (5, "events_clear"), (5, "events1"), (6, "events2"),
         (7, "status_gone"), (7, "info_gone"), (7, "status_back"), (7, "ping_back")]
CMDS = {"link": "FM1 STATUS", "ready": "FM1 STATUS", "info": "FM1 INFO", "info_gone": "FM1 INFO",
        "get_orig": "FM1 GET 0 1", "get_tmp": "FM1 GET 0 1", "get_restore": "FM1 GET 0 1",
        "set_tmp": "FM1 SET 0 1 <v>", "set_restore": "FM1 SET 0 1 <v>",
        "events_clear": "FM1 EVENTS CLEAR", "events1": "FM1 EVENTS", "events2": "FM1 EVENTS",
        "status_gone": "FM1 STATUS", "status_back": "FM1 STATUS"}
CMDS.update({t: "FM1 PING" for _, t in PLAN if t.startswith("ping")})

# 整行正则就能判的段：tag → (正则, 人读期望)
SIMPLE = {
    "link": (RX_STATUS_LINK, "FM1 STATUS link=ready …"),
    "ready": (RX_STATUS_READY, "FM1 STATUS link=ready … watch=3 info=yes proto=<n>"),
    "info": (RX_INFO, 'FM1 INFO version="FELUCCA SLOOP 2.…" … nengines=12 … ntrk=4 proto=12 …'),
    "events_clear": (RX_EVENTS_CLEARED, "FM1 EVENTS cleared"),
    "info_gone": (RX_INFO_NO_LINK, "FM1 INFO ERR no_link"),
    "status_back": (RX_STATUS_READY, "FM1 STATUS link=ready … watch=3 info=yes proto=<n>"),
    "ping_back": (RX_PING, "FM1 PING ms=<n>"),
}
SIMPLE.update({f"ping{i}": (RX_PING, "FM1 PING ms=<n>") for i in range(1, PINGS + 1)})

RX_BLOCK = re.compile(re.escape(NOTE) + r" (\d) ([a-z_0-9]+)$")
RX_ERROR = re.compile(re.escape(NOTE) + r" error (.+)$")


class SmokeError(RuntimeError):
    """流程失败；`evidenced` 表示失败的那条回复已在转录里（不必再写 error 注记）。"""

    def __init__(self, msg, evidenced=False):
        super().__init__(msg)
        self.evidenced = evidenced


def answer_text(line):
    """转录行里的 `FM1 ` 回复候选（去掉行首粘连的 console 提示符 `sloop>  `）；不是则回 None。"""
    text = line.split("sloop>")[-1].strip()
    return text if text.startswith("FM1 ") and not text.startswith("FM1 SMOKE ") else None


def _split(lines):
    """转录 → {tag: [回复行]}：注记 `# [fm1-smoke] <步> <tag>` 起一段，段内非回显的 `FM1 ` 行是回复。

    段内的其它注记（操作员提示 `>>> …`、超时说明）不截断该段——轮询的回复就落在提示之后。
    """
    blocks, answers, tag = {}, None, None
    for line in lines:
        if line.startswith(NOTE):
            m = RX_BLOCK.match(line)
            if m:
                answers, tag = blocks.setdefault(m.group(2), []), m.group(2)
            continue
        text = answer_text(line)
        if answers is not None and text is not None and text != CMDS.get(tag) \
                and not RX_SET_ECHO.match(text):
            answers.append(text)
    return blocks


def _check(tag, line, vals):
    """一段最后一条回复的判据：通过回 None，否则回失败说明（跨段的值放 vals）。"""
    if tag == "status_gone":
        if not RX_STATUS_GONE.match(line):
            return f"期望 FM1 STATUS link=none …，实得 {line}"
        if not RX_STATUS_FORGOT.match(line):
            return (f"{line}：link=none 但 watch/info 没有归零（期望同一行 watch=0 info=no）"
                    "——链路断开时会话层必须遗忘 WATCH 与 INFO")
        return None
    if tag in SIMPLE:
        rx, desc = SIMPLE[tag]
        return None if rx.match(line) else f"{CMDS[tag]}：期望 {desc}，实得 {line}"
    if tag[:4] in ("get_", "set_"):
        is_get = tag.startswith("get_")
        m = (RX_GET if is_get else RX_SET).match(line)
        if not m:
            return f"期望 FM1 {'GET' if is_get else 'SET'} scope=0 id=1 value=<n>，实得 {line}"
        value = int(m.group(1))
        if tag == "get_orig":
            vals["orig"] = value
            return None
        if "orig" not in vals:
            return "缺少原值读数"
        orig = vals["orig"]
        if tag == "set_tmp":
            if value == orig:
                return f"SET 回显 {line}：新值与原值相同，读回断言无意义"
            vals["tmp"] = value
            return None
        want, what = (vals.get("tmp"), "SET 的新值") if tag == "get_tmp" else (orig, "原值")
        return None if value == want else f"期望读回 {what} {want}，实得 {line}"
    m = RX_EVENTS.match(line)
    if not m:
        return f"期望 FM1 EVENTS total=<n> changed=<n> … last=…，实得 {line}"
    changed, last = int(m.group(1)), m.group(2)
    if tag == "events1":
        if changed < 1:
            return f"拧旋钮后 changed={changed}（期望 ≥1）：WATCH 推送未到达"
        if not RX_LAST_CHANGED.match(last):
            return (f"changed={changed} 但 last={last}：最近一条推送不是 CHANGED（cmd=23）"
                    "——这一步证明的是拧旋钮的参数变化推送")
        vals["changed"] = changed
        return None
    if "changed" not in vals:
        return "缺少第一次 changed 读数"
    return (None if changed > vals["changed"]
            else f"静置后再拧 changed={changed}（期望 >{vals['changed']}）：PING 保活后 WATCH 推送未到达")


def _verdict(lines):
    """判定转录：(失败说明列表, 未通过的步骤集合)。"""
    blocks, fails, bad, vals = _split(lines), [], set(), {}
    for line in lines:  # error 注记先入列：操作员中止/串口掉线是因，其后缺的段是果
        m = RX_ERROR.match(line)
        if m:
            fails.append(m.group(1))
            bad.update(int(s) for s in re.findall(r"步骤 (\d+)", m.group(1)))
    for step, tag in PLAN:
        answers = blocks.get(tag, [])
        if not answers:
            if not fails:
                fails.append(f"步骤 {step}（{STEPS[step]}）没有 {CMDS[tag]} 的回复")
                bad.add(step)
                break
            continue
        fail = _check(tag, answers[-1], vals)
        if fail:
            fails.append(f"步骤 {step}（{STEPS[step]}）{fail}")
            bad.add(step)
    return fails, bad


def judge(lines: list) -> list:
    """纯函数：转录 → 失败说明（空表 = 通过）。"""
    return _verdict(lines)[0]


def verdict_line(lines):
    """末行：`FM1 SMOKE PASS n/7` 或 `FM1 SMOKE FAIL n/7 <首条失败>`（n = 首个失败步骤之前通过的步数）。"""
    fails, bad = _verdict(lines)
    passed = len(STEPS) if not fails else min(bad, default=1) - 1
    if not fails:
        return f"FM1 SMOKE PASS {passed}/{len(STEPS)}"
    more = f"（另 {len(fails) - 1} 条失败，见 transcript）" if len(fails) > 1 else ""
    return f"FM1 SMOKE FAIL {passed}/{len(STEPS)} {fails[0]}{more}"


def read_line(ser, buf):
    """按 `\\n` 切出一条完整行（跨串口分块到达不拆行）；没有完整行回空串。"""
    data = ser.read(ser.in_waiting or 1)
    if data:
        buf.extend(data)
    i = buf.find(b"\n")
    if i < 0:
        return ""
    raw = bytes(buf[:i])
    del buf[:i + 1]
    return raw.decode(errors="replace").rstrip("\r")


class Transcript:
    """原始回显与工具注记：逐行打印并追加到 transcript，同时留在内存里给 judge。"""

    def __init__(self, path):
        self.path = path
        self.lines = []

    def line(self, text):
        print(text, flush=True)
        self.lines.append(text)
        with self.path.open("a", encoding="utf-8") as f:
            f.write(text + "\n")

    def note(self, text):
        self.line(f"{NOTE} {text}")


class Flow:
    """真机流程：发命令、读回复、起段、轮询；失败抛 SmokeError。"""

    def __init__(self, ser, log):
        self.ser, self.log = ser, log
        self.buf = bytearray()
        self.restore = None  # 待还原的 ATK 原值
        self.step = 0  # 当前在跑的步骤（中止时写进 error 注记）

    def block(self, step, tag):
        """在转录里起一段（`# [fm1-smoke] <步> <tag>`），之后的回复行归这一段。"""
        self.step = step
        self.log.note(f"{step} {tag}")

    def where(self, step):
        return f"步骤 {step}（{STEPS[step]}）"

    def ask(self, cmd):
        """发一条命令并等到它的回复行（非回显、以 `FM1 ` 开头）；无回复/设备重启抛 SmokeError。"""
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        deadline = time.monotonic() + ANSWER_TIMEOUT
        while time.monotonic() < deadline:
            line = read_line(self.ser, self.buf)
            if not line:
                continue
            self.log.line(line)
            if BOOT_BANNER.search(line):
                raise SmokeError(f"{cmd} 等回复时设备重启（内部堆耗尽或崩溃）")
            text = answer_text(line)
            if text is not None and text != cmd and not RX_SET_ECHO.match(text):
                return text
        raise SmokeError(f"{cmd} {ANSWER_TIMEOUT:.0f}s 内无回复")

    def expect(self, step, tag, cmd, want, desc):
        """单次一问一答断言。"""
        self.block(step, tag)
        line = self.ask(cmd)
        if not want.match(line):
            raise SmokeError(f"{self.where(step)}{cmd}：期望 {desc}，实得 {line}", evidenced=True)
        return line

    def poll(self, step, tag, cmd, want, timeout, interval, what, until=None, prompt=None,
             late_prompt=False):
        """起一段后每 interval 发一次 cmd 直到回复匹配 want（及 until），最多 timeout 秒。"""
        self.block(step, tag)
        deadline, first = time.monotonic() + timeout, True
        while True:
            line = self.ask(cmd)
            m = want.match(line)
            if m and (until is None or until(m)):
                return line
            if " ERR " in line:
                raise SmokeError(f"{self.where(step)}{line}", evidenced=True)
            if prompt and (not late_prompt or not first):
                self.log.note(f">>> {prompt}")
                prompt = None
            first = False
            if time.monotonic() >= deadline:
                raise SmokeError(f"{self.where(step)}等待 {what} 超时（{timeout:.0f}s）", evidenced=True)
            time.sleep(interval)

    def set_atk(self, step, value):
        """段内发一条 `FM1 SET 0 1 <value>`，回设备回显的（钳位后）值。"""
        line = self.ask(f"FM1 SET 0 1 {value}")
        m = RX_SET.match(line)
        if not m:
            raise SmokeError(f"{self.where(step)}FM1 SET 0 1 {value}：期望 FM1 SET scope=0 id=1 value=<n>，"
                             f"实得 {line}", evidenced=True)
        return int(m.group(1))

    def restore_atk(self):
        """把选中轨的 ATK 还原成原值并读回确认；正常路径与失败路径共用。

        读回确认之前不清 `self.restore`：中途被打断（含 Ctrl-C）时 finally 还能再试一次。
        """
        orig = self.restore
        if orig is None:
            return
        self.block(4, "set_restore")
        written = self.set_atk(4, orig)
        line = self.expect(4, "get_restore", "FM1 GET 0 1", RX_GET, "FM1 GET scope=0 id=1 value=<n>")
        if written != orig or int(RX_GET.match(line).group(1)) != orig:
            raise SmokeError(f"步骤 4（{STEPS[4]}）ATK 还原未确认（SET 回显 {written}，{line}，"
                             f"原值 {orig}）——请在 FM-1 上手动把 ATK 调回 {orig}", evidenced=True)
        self.restore = None
        self.log.note(f"ATK 已还原并读回确认：{orig}")

    def run(self):
        self.poll(1, "link", "FM1 STATUS", RX_STATUS_LINK, 120, 2.0, "link=ready",
                 prompt="请把刷了 SLOOP 的 FM-1 插到 Tab5 的 USB-A 口", late_prompt=True)
        self.poll(2, "ready", "FM1 STATUS", RX_STATUS_READY, 5, 0.2, "link=ready watch=3 info=yes")
        self.expect(2, "info", "FM1 INFO", RX_INFO,
                    'FM1 INFO version="FELUCCA SLOOP 2.…" … nengines=12 … ntrk=4 proto=12 …')
        ms = []
        for i in range(1, PINGS + 1):
            line = self.expect(3, f"ping{i}", "FM1 PING", RX_PING, "FM1 PING ms=<n>")
            ms.append(int(RX_PING.match(line).group(1)))
        self.log.note(f"PING ×{PINGS} 全部成功：ms 中位数 {statistics.median(ms):.0f}"
                      f"（{min(ms)}–{max(ms)}）")
        line = self.expect(4, "get_orig", "FM1 GET 0 1", RX_GET, "FM1 GET scope=0 id=1 value=<n>")
        orig = int(RX_GET.match(line).group(1))
        self.restore = orig
        self.block(4, "set_tmp")
        want = orig + 1
        new = self.set_atk(4, want)
        if new == orig:  # 原值已是上限，被设备钳位：改试低一档
            want = orig - 1
            new = self.set_atk(4, want)
        if new != want:
            raise SmokeError(f"{self.where(4)}FM1 SET 0 1 {want}：回显 value={new}", evidenced=True)
        line = self.expect(4, "get_tmp", "FM1 GET 0 1", RX_GET, f"FM1 GET scope=0 id=1 value={new}")
        if int(RX_GET.match(line).group(1)) != new:
            raise SmokeError(f"{self.where(4)}GET 读回 {line}，期望 value={new}", evidenced=True)
        self.restore_atk()
        self.expect(5, "events_clear", "FM1 EVENTS CLEAR", RX_EVENTS_CLEARED, "FM1 EVENTS cleared")
        line = self.poll(5, "events1", "FM1 EVENTS", RX_EVENTS, 60, 1.0, "changed ≥ 1",
                         until=lambda m: int(m.group(1)) >= 1, prompt="请在 FM-1 上拧任意一个旋钮")
        first, last = int(RX_EVENTS.match(line).group(1)), RX_EVENTS.match(line).group(2)
        if not RX_LAST_CHANGED.match(last):  # changed 够了但最近一条不是 CHANGED：当场停，不等到末尾判定
            raise SmokeError(f"{self.where(5)}changed={first} 但 last={last}：最近一条推送不是 CHANGED（cmd=23）",
                             evidenced=True)
        self.log.note(f"第一次推送：changed={first} {last}")
        self.log.note("静置 10s（会话层每秒 PING 保活）")
        time.sleep(10)
        line = self.poll(6, "events2", "FM1 EVENTS", RX_EVENTS, 60, 1.0, f"changed > {first}",
                         until=lambda m: int(m.group(1)) > first, prompt="请在 FM-1 上再拧一次旋钮")
        self.log.note(f"第二次推送：changed={RX_EVENTS.match(line).group(1)} "
                      f"{RX_EVENTS.match(line).group(2)}")
        line = self.poll(7, "status_gone", "FM1 STATUS", RX_STATUS_GONE, 60, 1.0, "link=none",
                         prompt="请拔掉 FM-1")
        if not RX_STATUS_FORGOT.match(line):
            # usb_fm1 置 link=false 与会话层清 watch/info 不在同一个临界区：隔 200ms 再读一次，以第二行为准
            self.log.note("首行已 link=none 但 watch/info 未归零，200ms 后重读一次")
            time.sleep(0.2)
            line = self.ask("FM1 STATUS")
        if not RX_STATUS_FORGOT.match(line):
            raise SmokeError(f"{self.where(7)}拔线后 FM1 STATUS 应为 link=none watch=0 info=no"
                             f"（会话层须遗忘 WATCH 与 INFO），实得 {line}", evidenced=True)
        self.expect(7, "info_gone", "FM1 INFO", RX_INFO_NO_LINK, "FM1 INFO ERR no_link")
        self.poll(7, "status_back", "FM1 STATUS", RX_STATUS_READY, 120, 1.0, "link=ready watch=3 info=yes",
                  prompt="请重新插上")
        self.expect(7, "ping_back", "FM1 PING", RX_PING, "FM1 PING ms=<n>")


# ---- 离线自测：六组内嵌样例转录（数值取自 2026-11-26 的真机读数） ----

READY = ("FM1 STATUS link=ready vid=0x1209 pid=0x0001 product=Felucca rx=5 tx=5 drops=0 open_fail=0"
         " midi_if=1 ep_in=0x81 ep_out=0x01 watch=3 info=yes proto=12")
INFO = ('FM1 INFO version="FELUCCA SLOOP 2.8.5" nengines=12 p_count=71 g_count=50 nstep=64 pe0=63 ntrk=4'
        " proto=12 engines=ANALOG|DIGITAL|PHASE|LOFI|SAMPLE|VOICE|TRIO|WHEEL|GRAIN|FM6|PHYS|NOISE")
GONE = "FM1 STATUS link=none vid=0x0000 pid=0x0000 product=- rx=27 tx=27 drops=0 open_fail=0 watch=0 info=no proto=-"
EVENTS = ("FM1 EVENTS total=%d changed=%d reload=0 step_changed=0 track_changed=0 timeouts=0 stray=0"
          " last=%s")


def _block(step, tag, *answers):
    """样例段：段头 +（带粘连提示符的）命令回显 + 若干回复行；SET 的回显带具体值，由调用方给出。"""
    echo = [f"sloop>  {CMDS[tag]}"] if tag in CMDS and "<v>" not in CMDS[tag] else []
    return [f"{NOTE} {step} {tag}", *echo, *answers]


PASS_LINES = (
    _block(1, "link", READY)
    + _block(2, "ready", READY) + _block(2, "info", INFO)
    + sum((_block(3, f"ping{i}", "FM1 PING ms=1") for i in range(1, PINGS + 1)), [])
    + _block(4, "get_orig", "FM1 GET scope=0 id=1 value=50")
    + _block(4, "set_tmp", "FM1 SET 0 1 51", "FM1 SET scope=0 id=1 value=51")
    + _block(4, "get_tmp", "FM1 GET scope=0 id=1 value=51")
    + _block(4, "set_restore", "FM1 SET 0 1 50", "FM1 SET scope=0 id=1 value=50")
    + _block(4, "get_restore", "FM1 GET scope=0 id=1 value=50")
    + _block(5, "events_clear", "FM1 EVENTS cleared")
    + _block(5, "events1", EVENTS % (0, 0, "none"), f"{NOTE} >>> 请在 FM-1 上拧任意一个旋钮",
             EVENTS % (1, 1, "cmd=23 scope=0 id=3 value=17"))
    + _block(6, "events2", EVENTS % (1, 1, "cmd=23 scope=0 id=3 value=17"), f"{NOTE} >>> 请在 FM-1 上再拧一次旋钮",
             EVENTS % (2, 2, "cmd=23 scope=0 id=3 value=19"))
    + _block(7, "status_gone", READY, f"{NOTE} >>> 请拔掉 FM-1", GONE) + _block(7, "info_gone", "FM1 INFO ERR no_link")
    + _block(7, "status_back", GONE, f"{NOTE} >>> 请重新插上", READY) + _block(7, "ping_back", "FM1 PING ms=2")
)
INFO_TIMEOUT = PASS_LINES[:PASS_LINES.index(f"{NOTE} 2 info") + 2] + ["FM1 INFO ERR timeout"]
EVENTS_ZERO = PASS_LINES[:PASS_LINES.index(f"{NOTE} 5 events1") + 2] + [EVENTS % (0, 0, "none")] * 3
LINK_NONE = [f"{NOTE} 1 link", "sloop>  FM1 STATUS", GONE, f"{NOTE} >>> 请把刷了 SLOOP 的 FM-1 插到 Tab5 的 USB-A 口",
             "sloop>  FM1 STATUS", GONE]
SET_NO_REPLY = PASS_LINES[:PASS_LINES.index(f"{NOTE} 4 set_tmp") + 1]  # SET 发出后一个字节都没回
GONE_STALE_LINE = GONE.replace("watch=0 info=no proto=-", "watch=3 info=yes proto=12")
EVENTS_RELOAD = (PASS_LINES[:PASS_LINES.index(f"{NOTE} 5 events1") + 2]
                 + [EVENTS % (1, 1, "cmd=24 scope=1 id=5 value=0")])  # changed 够了，但最近一条是 RELOAD
GONE_STALE = (PASS_LINES[:PASS_LINES.index(f"{NOTE} 7 status_gone") + 2] + [GONE_STALE_LINE, f"{NOTE} 首行已 link=none 但 watch/info 未归零，200ms 后重读一次",
              "sloop>  FM1 STATUS", GONE_STALE_LINE])  # 重读一次仍是旧值：判失败
_i = PASS_LINES.index(f"{NOTE} 7 status_gone") + 2   # 成功样例的 status_gone 段：READY 的下一条就是 GONE
PASS_RACE = PASS_LINES[:_i] + [GONE_STALE_LINE, "sloop>  FM1 STATUS"] + PASS_LINES[_i:]  # 首行旧值、重读已归零：通过
EVENTS_CMD230 = (PASS_LINES[:PASS_LINES.index(f"{NOTE} 5 events1") + 2]
                 + [EVENTS % (1, 1, "cmd=230 scope=0 id=3 value=1")])  # 前缀相同但不是 cmd=23


def run_selftest():
    """六组样例：① 全通过；②–⑥ 各恰好一条失败且落在对应步骤。"""
    assert verdict_line(PASS_LINES) == "FM1 SMOKE PASS 7/7", verdict_line(PASS_LINES)
    for lines, failed, needle in ((INFO_TIMEOUT, "FM1 SMOKE FAIL 1/7 步骤 2（", "FM1 INFO ERR timeout"),
                                  (EVENTS_ZERO, "FM1 SMOKE FAIL 4/7 步骤 5（", "changed=0"),
                                  (LINK_NONE, "FM1 SMOKE FAIL 0/7 步骤 1（", "link=none"),
                                  (EVENTS_RELOAD, "FM1 SMOKE FAIL 4/7 步骤 5（", "cmd=23"),
                                  (GONE_STALE, "FM1 SMOKE FAIL 6/7 步骤 7（", "watch=0 info=no")):
        fails = judge(lines)
        assert len(fails) == 1, (len(fails), fails)
        assert verdict_line(lines).startswith(failed), verdict_line(lines)
        assert needle in fails[0], fails
    # 段内没有回复（命令石沉大海）也要给出判定，而不是在判 verdict 时抛异常
    assert verdict_line(SET_NO_REPLY).startswith("FM1 SMOKE FAIL 3/7 步骤 4（"), verdict_line(SET_NO_REPLY)
    # 拔线竞态：首行 link=none 但 watch/info 未归零，重读已归零 ⇒ 通过；重读仍是旧值 ⇒ 失败
    assert verdict_line(PASS_RACE) == "FM1 SMOKE PASS 7/7", verdict_line(PASS_RACE)
    # last=cmd=230… 与 cmd=23 只是前缀相同，不算 CHANGED
    assert len(judge(EVENTS_CMD230)) == 1 and "cmd=23" in judge(EVENTS_CMD230)[0], judge(EVENTS_CMD230)
    # 操作员中止的 error 注记按其所停步骤计数
    assert verdict_line(PASS_LINES[:PASS_LINES.index(f"{NOTE} 5 events1") + 1]
                        + [f"{NOTE} error KeyboardInterrupt: 操作员中止（Ctrl-C），停在步骤 5（WATCH 推送）"]
                        ).startswith("FM1 SMOKE FAIL 4/7"), "abort note"
    print("自测断言全部通过（全通过 7/7；INFO ERR timeout / EVENTS changed=0 / STATUS link=none / "
          "changed 够但 last=cmd=24 / link=none 但 watch=3 各恰好一条失败，落在步骤 2 / 5 / 1 / 5 / 7）")


def run_device(port):
    """真机流程：复位 → 7 步 → 判定；回退出码。"""
    shot.require("serial", "pyserial")
    import serial
    try:
        serial.Serial(port, BAUD, timeout=0.05).close()
    except (OSError, serial.SerialException) as e:
        sys.exit(f"打开串口 {port} 失败: {e}")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = ROOT / "firmware" / "build" / "verify" / f"fm1-smoke-{stamp}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")
    log = Transcript(log_path)
    log.note(f"port={port} log={log_path.relative_to(ROOT)}")
    boot, ser, flow = bytearray(), None, None
    try:
        shot.reset_device(port, BAUD, on_chunk=boot.extend)
        ser = serial.Serial(port, BAUD, timeout=0.05)
        time.sleep(5)
        ser.reset_input_buffer()
        version = re.search(r"App version:.*", boot.decode(errors="replace"))
        if version:
            log.note(f"firmware {version.group(0).strip()}")
        flow = Flow(ser, log)
        flow.run()
    except KeyboardInterrupt:
        # 操作员 Ctrl-C（等待 60s/120s 时唯一的退出方式）：同样落 error 注记并走下面的判定，
        # 不把 traceback 漏给终端、不丢 FM1 SMOKE FAIL 行
        step = flow.step if flow is not None else 0
        log.note(f"error KeyboardInterrupt: 操作员中止（Ctrl-C）"
                 + (f"，停在步骤 {step}（{STEPS[step]}）" if step else "（复位/启动阶段）"))
    except (OSError, serial.SerialException, RuntimeError) as e:
        if getattr(e, "evidenced", False):
            log.note(str(e))
        else:
            log.note(f"error {type(e).__name__}: {e}")
    finally:
        if flow is not None and flow.restore is not None:
            orig = flow.restore
            try:
                flow.restore_atk()
            except KeyboardInterrupt:
                log.note(f"error 还原 ATK 时再次被中止（Ctrl-C）：请在 FM-1 上手动把 ATK 调回 {orig}")
            except (OSError, serial.SerialException, RuntimeError) as e:
                log.note(f"{'警告' if getattr(e, 'evidenced', False) else 'error'} {type(e).__name__}: {e}")
        if ser is not None:
            ser.close()
    fails, _ = _verdict(log.lines)
    for fail in fails:
        print(f"  [FAIL] {fail}")
    log.line(verdict_line(log.lines))
    return 0 if not fails else 1


def main():
    parser = argparse.ArgumentParser(description="FM-1 联动协议冒烟门禁（操作员在环）")
    parser.add_argument("port", nargs="?", help="串口设备，如 /dev/ttyACM0（--selftest 时省略）")
    parser.add_argument("--selftest", action="store_true", help="离线自测（无需设备）")
    args = parser.parse_args()
    if args.selftest:
        if args.port:
            parser.error("--selftest 不需要串口参数")
        try:
            run_selftest()
        except AssertionError as e:
            sys.exit(f"selftest FAILED: {e}")
        print("FM1 SMOKE SELFTEST OK")
        return 0
    if not args.port:
        parser.error("缺少串口参数 PORT（或使用 --selftest）")
    return run_device(args.port)


if __name__ == "__main__":
    sys.exit(main())
