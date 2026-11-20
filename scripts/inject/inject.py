#!/usr/bin/env python3
"""inject.py — Tab5 固件串口注入序列（AGENTS.md §5.4 注入约定）

三条场景脚本（transport.sh / step_edit.sh / tracks.sh）是本文的薄封装。每个场景：
  1. `UI <page>` 同步切页；
  2. 若干 `TAP <x> <y>`（或 `KEY`）注入，逐条断言固件回显；
  3. `STATE SNAP` / `ENG STEP` 断言状态变化；
  4. 经 SCR 协议截屏（复用 scripts/screenshot.py 的读取/解码函数），对产物做像素抽检。

原始串口输出逐字节写入日志（不裁剪、不后编辑）。日志里唯一非固件字节是
`# [inject] ...` 前缀的工具注记（标记截屏位置等）；SCR 的 1.8MB 二进制帧体不入日志。

用法（一般经三条脚本调用）：
    python3 scripts/inject/inject.py --scenario transport --port /dev/ttyACM0
产物（默认 $SLOOP_TAB5_PROJECT/build/verify/，可用 --out-dir 覆盖）：
    inject-<scenario>-<时间戳>.log        串口原始回显 + 工具注记
    inject-<scenario>-<时间戳>-<标签>.png 截屏
退出码：0 = 全部断言与像素抽检通过；1 = 任一失败（打印失败项）。
依赖：pyserial + Pillow（scripts/requirements.txt）。
"""
import argparse
import importlib.util
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))
import screenshot as shot  # noqa: E402

BAUD = 115200


class InjectError(RuntimeError):
    pass


def require_deps() -> None:
    for name, pip in (("serial", "pyserial"), ("PIL", "pillow")):
        if importlib.util.find_spec(name) is None:
            sys.exit(f"缺少依赖 {name}：pip install -r scripts/requirements.txt（或 pip install {pip}）")


class LoggedSerial:
    """串口代理：读到的字节原样写入日志；截屏帧体期间静默（silent）。"""

    def __init__(self, ser, log_path: Path):
        self._ser = ser
        self._log = open(log_path, "ab")
        self.silent = False

    def read(self, n: int = 1) -> bytes:
        data = self._ser.read(n)
        if data and not self.silent:
            self._log.write(data)
            self._log.flush()
        return data

    def readline(self) -> bytes:
        data = self._ser.readline()
        if data and not self.silent:
            self._log.write(data)
            self._log.flush()
        return data

    def write(self, data: bytes) -> int:
        return self._ser.write(data)

    def flush(self) -> None:
        self._ser.flush()

    def reset_input_buffer(self) -> None:
        self._ser.reset_input_buffer()

    @property
    def timeout(self):
        return self._ser.timeout

    @timeout.setter
    def timeout(self, value) -> None:
        self._ser.timeout = value

    def note(self, text: str) -> None:
        if not self.silent:
            self._log.write(f"# [inject] {text}\n".encode())
            self._log.flush()

    def close(self) -> None:
        self._log.close()
        self._ser.close()


def _prompt_at(buf: bytearray) -> bool:
    """console 提示符检测：缓冲区尾部是 "sloop>" 且其后只有空格。
    （esp_console 实际输出为 "sloop>  "——尾部可能多一个空格，不能按固定长度比较。）"""
    i = buf.rfind(b"sloop>")
    return i >= 0 and buf[i + len(b"sloop>"):].strip() == b""


def read_response(ser, timeout: float) -> str:
    """读到 console 提示符（sloop>）为止；超时抛 TimeoutError。"""
    buf = bytearray()
    deadline = time.monotonic() + timeout
    ser.timeout = 0.05
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf.extend(chunk)
            continue
        if _prompt_at(buf):
            return buf.decode(errors="replace")
    raise TimeoutError(f"等待提示符超时（已收 {len(buf)} 字节）")


class Session:
    """一条串口会话：命令发送、断言、截屏；所有 PASS/FAIL 都打印到 stdout。"""

    def __init__(self, ser: LoggedSerial, out_dir: Path, stamp: str, scenario: str,
                 timeout: float = 3.0):
        self.ser = ser
        self.out_dir = out_dir
        self.stamp = stamp
        self.scenario = scenario
        self.timeout = timeout
        self.last = ""
        self.failures = 0

    # ---- 打印 ----
    def ok(self, text: str) -> None:
        print(f"  [PASS] {text}")

    def fail(self, text: str) -> None:
        self.failures += 1
        print(f"  [FAIL] {text}")

    # ---- 命令与断言 ----
    def wait_ready(self, timeout: float = 15.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.ser.write(b"\n")
            self.ser.flush()
            try:
                if read_response(self.ser, 1.0).rstrip().endswith("sloop>"):
                    return
            except TimeoutError:
                continue
        raise InjectError("等待 console 提示符超时（设备未启动或串口被占用？）")

    def cmd(self, line: str, timeout: float = None) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        try:
            self.last = read_response(self.ser, self.timeout if timeout is None else timeout)
        except TimeoutError as e:
            raise InjectError(f"命令 {line!r} 无响应：{e}") from None
        return self.last

    def expect(self, pattern: str, label: str = "") -> None:
        if re.search(pattern, self.last, re.MULTILINE):
            self.ok(label or f"/{pattern}/")
        else:
            self.fail(f"{label or pattern}（响应未匹配，见日志）")

    def expect_int(self, pattern: str, value: int, label: str) -> None:
        m = re.search(pattern, self.last, re.MULTILINE)
        if m is None:
            self.fail(f"{label}: 响应缺少 {pattern!r}")
            return
        got = int(m.group(1))
        if got == value:
            self.ok(f"{label} = {got}")
        else:
            self.fail(f"{label} = {got}（期望 {value}）")

    def snap_int(self, pattern: str) -> int:
        m = re.search(pattern, self.last, re.MULTILINE)
        if m is None:
            raise InjectError(f"STATE/ENG 快照缺少 {pattern!r}")
        return int(m.group(1))

    # ---- 截屏与像素抽检 ----
    def capture(self, label: str, timeout: float = 25.0):
        out = self.out_dir / f"inject-{self.scenario}-{self.stamp}-{label}.png"
        self.ser.note(f"SCR 截屏 -> {out.name}")
        self.ser.write(b"SCR\n")
        self.ser.flush()
        try:
            w, h, fmt, ln = shot.read_header(self.ser, timeout)
            self.ser.silent = True
            data = shot.read_exact(self.ser, ln, timeout)
            self.ser.silent = False
            self.ser.timeout = 1.0
            tail = self.ser.readline().decode(errors="replace").strip()
        except (TimeoutError, ValueError) as e:
            self.ser.silent = False
            raise InjectError(f"截屏 {label} 失败: {e}") from None
        if tail != "SCR OK":
            raise InjectError(f"截屏 {label} 尾标记异常: {tail!r}")
        img = shot.rgb565_to_rgb(data, w, h) if fmt == "rgb565" else shot.rgb888_to_rgb(data, w, h)
        img.save(out)
        print(f"  [SHOT] {out}  ({w}x{h} {fmt})")
        return img

    def probe(self, img, label: str, x: int, y: int, rgb: tuple, tol: int = 16) -> None:
        good, msg = shot.check_pixel(img, x, y, *rgb, tol)
        (self.ok if good else self.fail)(f"{label}: {msg}")

    def probe_region(self, img, label: str, box: tuple, rgb: tuple, tol: int, min_count: int) -> None:
        colors = img.crop(box).getcolors(maxcolors=1 << 20) or []
        n = sum(c for c, px in colors
                if max(abs(px[0] - rgb[0]), abs(px[1] - rgb[1]), abs(px[2] - rgb[2])) <= tol)
        (self.ok if n >= min_count else self.fail)(
            f"{label}: 区域 {box} 内 {rgb}±{tol} 像素 {n}（阈值 ≥{min_count}）")


# 锚点颜色取 RGB565 位扩展口径（与 screenshot.py --check 一致；容差默认 16）
C_BLUE = (56, 120, 216)   # 轨 0 主题色 0x3B7BD8
C_GREEN = (56, 160, 72)   # 轨 1 主题色 0x3FA34D
C_YELLOW = (224, 176, 40) # 轨 2 / UI_ACCENT 0xE0B32A
C_ORANGE = (224, 104, 24) # 轨 3 主题色 0xE06A1B
C_BG = (16, 20, 24)       # UI_BG 0x101418
C_PANEL = (24, 32, 32)    # UI_PANEL 0x1B2026
C_PANEL_HI = (32, 40, 48) # UI_PANEL_HI 0x242B33
C_PANEL_SEL = (40, 52, 64)  # UI_PANEL_SEL 0x2C3540
C_TEXT = (240, 244, 248)  # UI_TEXT 描边（3px 焦点框）
C_TAB_PLAY = (40, 36)
C_TAB_TRACKS = (230, 36)
C_TAB_STEP = (400, 36)
C_TAB_TRANSPORT = (590, 36)


def playhead_columns(img) -> dict:
    """按行读出播放头所在列（播放头 = 格子四周 5px UI_ACCENT 描边）。
    行 y 起点 = 72(页签条) + 84，行距 124；列 x 起点 = 88，列距 73，格 67×104。"""
    out = {}
    for r in range(4):
        y = 156 + r * 124
        cols = []
        for c in range(16):
            x0 = 88 + c * 73
            hit = 0
            for x in range(x0, x0 + 67):
                for yy in (y + 1, y + 102):
                    px = img.getpixel((x, yy))
                    if max(abs(px[i] - C_YELLOW[i]) for i in range(3)) <= 16:
                        hit += 1
            if hit > 20:
                cols.append(c)
        out[r] = cols
    return out

# 注入坐标（屏幕绝对坐标 = 页内坐标 + 页签条 UI_TAB_BAR_H，各点取控件矩形中心）。
# 每个数字对应固件里的布局宏——页面布局改动时按同名宏同步这里：
#   UI_TAB_BAR_H  = 72   （ui.c）
#   TRANSPORT_*   = page_transport.c 建按钮的 TRANSPORT_{PLAY,STOP,BPM}_{X,Y,W,H}
#   GRID_*/CELL_*/ROW_PITCH = page_step.c 步网格宏
#   CARD_*        = page_tracks.c 卡宏（卡距 = CARD_W + 16）
TRANSPORT_PLAY = (208, 210)       # TRANSPORT_PLAY 中心：x=48+320/2，y=72+48+180/2
TRANSPORT_STOP = (560, 210)       # TRANSPORT_STOP 中心：x=400+320/2，y 同 PLAY
TRANSPORT_BPM_PLUS = (1050, 262)  # TRANSPORT_BPM_PLUS 中心：x=1000+100/2，y=72+140+100/2
TRANSPORT_BPM_MINUS = (910, 262)  # TRANSPORT_BPM_MINUS 中心：x=860+100/2，y 同 "+"
# 轨 0 步 2：x=GRID_X0+2*CELL_STEP_X+CELL_W/2=88+146+33，y=72+GRID_Y0+CELL_H/2=72+84+52
STEP_CELL_A = (267, 208)
# 轨 1 步 5：x=88+5*73+33；y=72+84+1*ROW_PITCH+52
STEP_CELL_B = (486, 332)
# 卡 2：x=CARD_X0+2*(CARD_W+16)+CARD_W/2=24+624+148；y=72+CARD_Y+CARD_H/2=72+40+235
TRACKS_CARD2 = (796, 347)


def ensure_stopped(s: Session) -> None:
    """STEP 页截图里的焦点框与播放头互斥：先停走带，截图判读才确定。"""
    s.cmd("STATE SNAP")
    if s.snap_int(r"playing=(\d)") != 0:
        s.cmd("UI transport")
        s.expect(r"UI page=transport \(3\)", "UI transport")
        s.cmd("TAP %d %d" % TRANSPORT_STOP)
        s.expect(r"-> TRANSPORT CMD_STOP", "TAP 停止")
        s.cmd("STATE SNAP")
        s.expect_int(r"playing=(\d)", 0, "STATE SNAP playing")


def run_transport(s: Session) -> None:
    """① TRANSPORT：播放 + BPM 步进 → 截屏；再切 STEP 截播放头；最后复位为停止。"""
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "UI transport")
    s.cmd("STATE SNAP")
    bpm0 = s.snap_int(r"bpm=(\d+)")

    s.cmd("TAP %d %d" % TRANSPORT_STOP)  # 幂等停止：无论起始是否在播放
    s.expect(r"-> TRANSPORT CMD_STOP", "TAP STOP")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "STATE SNAP playing")

    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "TAP PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "STATE SNAP playing")

    if bpm0 < 240:  # 240 时 + 被钳位，改走 −/+ 分支
        s.cmd("TAP %d %d" % TRANSPORT_BPM_PLUS)
        s.expect(r"-> TRANSPORT CMD_SET_BPM delta=\+1", "TAP BPM +")
        s.cmd("STATE SNAP")
        s.expect_int(r"bpm=(\d+)", bpm0 + 1, "STATE SNAP bpm（+1）")
        s.cmd("TAP %d %d" % TRANSPORT_BPM_MINUS)
        s.expect(r"-> TRANSPORT CMD_SET_BPM delta=-1", "TAP BPM -")
        s.cmd("STATE SNAP")
        s.expect_int(r"bpm=(\d+)", bpm0, "STATE SNAP bpm（回到基值）")
    else:
        s.cmd("TAP %d %d" % TRANSPORT_BPM_MINUS)
        s.expect(r"-> TRANSPORT CMD_SET_BPM delta=-1", "TAP BPM -")
        s.cmd("STATE SNAP")
        s.expect_int(r"bpm=(\d+)", bpm0 - 1, "STATE SNAP bpm（-1，240 钳位态）")
        s.cmd("TAP %d %d" % TRANSPORT_BPM_PLUS)
        s.expect(r"-> TRANSPORT CMD_SET_BPM delta=\+1", "TAP BPM +")
        s.cmd("STATE SNAP")
        s.expect_int(r"bpm=(\d+)", bpm0, "STATE SNAP bpm（回到 240）")

    img = s.capture("transport")
    s.probe(img, "PLAY 按钮底", 108, 210, C_GREEN)
    s.probe(img, "STOP 按钮底", 460, 210, C_ORANGE)
    s.probe(img, "页签选中（TRANSPORT）", *C_TAB_TRANSPORT, C_YELLOW)
    s.probe(img, "页面背景", 640, 690, C_BG)

    # 播放中切 STEP：播放头由引擎 step_pos 驱动。SCR 抓的是"上一已刷新帧"（≤30ms 旧），
    # 所以每帧的播放头列应等于抓屏前 ENG SNAP 的 pos（或紧邻 1–2 列）；
    # 两帧的 pos 不同即证明播放推进（16 列环绕会让"列号不同"本身不可靠，故以引擎为锚）。
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    time.sleep(0.4)  # ≥10 个 30ms tick，播放头与步位置同步稳定

    def shot_with_pos(label):
        s.cmd("ENG SNAP")
        pre = s.snap_int(r"pos=(\d+)")
        img = s.capture(label)
        return pre, playhead_columns(img), img

    pre_a, cls_a, _ = shot_with_pos("step-playhead-a")
    time.sleep(0.3)
    pre_b, cls_b, img_b = shot_with_pos("step-playhead-b")

    s.probe_region(img_b, "播放头描边", (88, 156, 88 + 16 * 73, 156 + 3 * 124 + 104),
                   C_YELLOW, 16, 100)
    s.probe(img_b, "页签选中（STEP）", *C_TAB_STEP, C_YELLOW)

    for r in range(4):
        want_a = {(pre_a + k) % 16 for k in (0, 1, 2)}
        want_b = {(pre_b + k) % 16 for k in (0, 1, 2)}
        ok_a = len(cls_a[r]) == 1 and cls_a[r][0] in want_a
        ok_b = len(cls_b[r]) == 1 and cls_b[r][0] in want_b
        s.ok(f"行 {r}: 帧 A 列 {cls_a[r]}（pos={pre_a}）/ 帧 B 列 {cls_b[r]}（pos={pre_b}）")
        if not ok_a or not ok_b:
            s.fail(f"行 {r} 播放头列与引擎 step_pos 不符（帧 A 允许 {sorted(want_a)}，"
                   f"帧 B 允许 {sorted(want_b)}）")
    (s.ok if pre_a != pre_b else s.fail)(
        f"引擎 step_pos 两帧间变化 {pre_a} -> {pre_b}（16 列环绕，按模计）" if pre_a != pre_b
        else f"引擎 step_pos 两帧间未变化（均为 {pre_a}）")

    # 复位：停止并回 PLAY 页（脚本可重复运行）
    s.cmd("UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "TAP STOP（复位）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "STATE SNAP playing（复位）")
    s.cmd("UI play")


def run_step_edit(s: Session) -> None:
    """② STEP：点两格（不同轨）→ 断言编辑器真值与焦点 → 截屏。可重复运行。"""
    ensure_stopped(s)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")

    cells = (("0", "2", STEP_CELL_A), ("1", "5", STEP_CELL_B))
    lit = []
    for track, step, _ in cells:
        s.cmd(f"ENG STEP {track} {step}")
        lit.append(s.snap_int(r"\bn=(\d+)") != 0)
    if any(lit):
        s.ser.note("基线：有格已亮，先点灭使脚本可重复")
        for (track, step, xy), on in zip(cells, lit):
            if not on:
                continue
            s.cmd("TAP %d %d" % xy)
            s.expect(r"-> STEP track=%s step=%s CMD_TOGGLE_STEP" % (track, step),
                     f"TAP 点灭 track={track} step={step}")
            s.cmd(f"ENG STEP {track} {step}")
            s.expect_int(r"\bn=(\d+)", 0, f"ENG STEP {track} {step} n")

    for track, step, xy in cells:
        s.cmd("TAP %d %d" % xy)
        s.expect(r"-> STEP track=%s step=%s CMD_TOGGLE_STEP" % (track, step),
                 f"TAP track={track} step={step}")
        s.cmd(f"ENG STEP {track} {step}")
        s.expect_int(r"\bn=(\d+)", 1, f"ENG STEP {track} {step} n")
        s.expect_int(r"\btime=(\d+)", 0, f"ENG STEP {track} {step} time（NOTE）")

    # TAP 同时设焦点（与控件回调同一动作函数）
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 1, "STATE SNAP focus_track")
    s.expect_int(r"focus_step=(\d+)", 5, "STATE SNAP focus_step")

    # 表外 → miss（返回非零，不改状态）
    s.cmd("TAP 1 700")
    s.expect(r"TAP miss 1,700 page=2", "TAP 表外 miss")

    img = s.capture("step-two-on")
    s.probe(img, "轨 0 步 2 亮格", *STEP_CELL_A, C_BLUE)
    s.probe(img, "轨 1 步 5 亮格", *STEP_CELL_B, C_GREEN)
    s.probe(img, "轨 0 步 3 空位", 340, 208, C_PANEL_HI, 10)
    s.probe(img, "焦点框（轨 1 步 5 左描边）", 454, 332, C_TEXT, 10)
    s.probe(img, "页签选中（STEP）", *C_TAB_STEP, C_YELLOW)


def run_tracks(s: Session) -> None:
    """③ TRACKS：点卡 2 → 断言选轨 → 截屏（选中态 = 6px 描边 + 亮底）。"""
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "UI tracks")

    s.cmd("TAP %d %d" % TRACKS_CARD2)
    s.expect(r"-> TRACKS select track=2 CMD_SELECT_TRACK", "TAP 卡 2")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 2, "STATE SNAP selected_track")
    s.expect_int(r"focus_track=(\d+)", 2, "STATE SNAP focus_track")

    s.cmd("TAP 640 260")  # 卡间 16px 缝 → miss
    s.expect(r"TAP miss 640,260 page=1", "TAP 卡缝 miss")

    img = s.capture("tracks-sel3")
    s.probe(img, "卡 2 选中描边（6px）", 644, 347, C_YELLOW)
    s.probe(img, "卡 2 选中底", 800, 552, C_PANEL_SEL, 8)
    s.probe(img, "卡 0 未选中底", 304, 552, C_PANEL, 8)
    s.probe(img, "页签选中（TRACKS）", *C_TAB_TRACKS, C_YELLOW)
    s.probe(img, "卡间缝背景", 640, 260, C_BG)


SCENARIOS = {
    "transport": run_transport,
    "step_edit": run_step_edit,
    "tracks": run_tracks,
}


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口注入序列（AGENTS.md §5.4）")
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS),
                    help="注入场景（三条 .sh 各对应一个）")
    ap.add_argument("--port", required=True, help="串口设备，如 /dev/ttyACM0")
    ap.add_argument("--out-dir", default=None,
                    help="产物目录（默认 $SLOOP_TAB5_PROJECT/build/verify/）")
    ap.add_argument("--timeout", type=float, default=3.0, help="单条命令响应超时秒数（默认 3）")
    args = ap.parse_args()

    require_deps()
    import serial

    if args.out_dir:
        out_dir = Path(args.out_dir)
    else:
        project = os.environ.get("SLOOP_TAB5_PROJECT") or str(SCRIPTS_DIR.parent / "firmware")
        out_dir = Path(project) / "build" / "verify"
    out_dir.mkdir(parents=True, exist_ok=True)

    stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
    log_path = out_dir / f"inject-{args.scenario}-{stamp}.log"
    raw = serial.Serial(args.port, BAUD, timeout=0.05)
    ser = LoggedSerial(raw, log_path)
    s = Session(ser, out_dir, stamp, args.scenario, timeout=args.timeout)

    print(f"== inject {args.scenario} @ {args.port}")
    print(f"   串口原始日志: {log_path}")
    try:
        s.wait_ready()
        ser.note(f"场景 {args.scenario} 开始")
        SCENARIOS[args.scenario](s)
        ser.note(f"场景 {args.scenario} 结束 failures={s.failures}")
    except InjectError as e:
        ser.note(f"场景 {args.scenario} 异常终止: {e}")
        sys.exit(f"[FAIL] {args.scenario}: {e}")
    finally:
        ser.close()

    if s.failures:
        sys.exit(f"[FAIL] {args.scenario}: {s.failures} 项断言/抽检未通过（见上）")
    print(f"[PASS] {args.scenario}: 全部断言与像素抽检通过")


if __name__ == "__main__":
    main()
