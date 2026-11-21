#!/usr/bin/env python3
"""inject.py — Tab5 固件串口注入序列（AGENTS.md §5.4 注入约定）

三条场景脚本（transport.sh / step_edit.sh / tracks.sh）是本文的薄封装。每个场景：
  1. `UI <page>` 同步切页（步窗场景用 `UI WINDOW <start> <len>` 落窗口）；
  2. 若干 `TAP <x> <y>`（或 `KEY`）注入，逐条断言固件回显；STEP 的注入坐标由
     页面布局宏派生（step_cell_xy），列语义对齐当前窗口——列 c = 引擎绝对步
     window_start + c，列数 = STATE SNAP 的 window_len；
  3. `STATE SNAP` / `ENG STEP` 断言状态变化；
  4. 经 SCR 协议截屏（复用 scripts/screenshot.py 的读取/解码函数），对产物做像素抽检。

原始串口输出逐字节写入日志（不裁剪、不后编辑）。日志里唯一非固件字节是
`# [inject] ...` 前缀的工具注记（标记截屏位置等）；SCR 的 1.8MB 二进制帧体不入日志。

截屏传输失败（DI-5：帧数据超时/短帧/坏尾）时与 `scripts/screenshot.py` 同源处理：
esptool 硬复位 → 等 console 提示符 → 重开串口 → 场景从头重跑（复位把页面/走带
状态清回基线，重跑才能让断言与截图同源），默认最多 3 次。

用法（一般经三条脚本调用）：
    python3 scripts/inject/inject.py --scenario transport --port /dev/ttyACM0 [--retry 3]
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


class ScenarioRestart(Exception):
    """截屏传输失败已复位设备——场景从头重跑（复位后页面/走带状态已回基线）。"""


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
    """一条串口会话：命令发送、断言、截屏；所有 PASS/FAIL 都打印到 stdout。

    截屏传输失败（DI-5）时经 esptool 复位设备并重开串口，当前场景从头重跑
    （复位把页面/走带状态清回基线，重跑才能保证断言与截图同源）。
    """

    def __init__(self, port: str, log_path: Path, out_dir: Path, stamp: str, scenario: str,
                 timeout: float = 3.0, retries: int = shot.DEFAULT_RETRIES):
        self.port = port
        self.log_path = log_path
        self.out_dir = out_dir
        self.stamp = stamp
        self.scenario = scenario
        self.timeout = timeout
        self.retries = retries
        self.resets = 0
        self.last = ""
        self.failures = 0
        self.ser = self._open()

    def _open(self) -> LoggedSerial:
        import serial
        return LoggedSerial(serial.Serial(self.port, BAUD, timeout=0.05), self.log_path)

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
    def _reset_and_reopen(self) -> None:
        self.ser.close()  # 关日志 + 关端口，esptool 才能进入 bootloader
        try:
            shot.reset_device(self.port, BAUD)
        except shot.ResetError as e:
            raise InjectError(f"复位失败，无法从截屏传输停滞恢复: {e}") from None
        self.ser = self._open()
        self.wait_ready()

    def capture(self, label: str, timeout: float = 25.0):
        out = self.out_dir / f"inject-{self.scenario}-{self.stamp}-{label}.png"
        self.ser.note(f"SCR 截屏 -> {out.name}")
        self.ser.write(b"SCR\n")
        self.ser.flush()
        try:
            w, h, fmt, data = shot.read_frame(
                self.ser, timeout, lambda on: setattr(self.ser, "silent", on))
        except shot.FirmwareError as e:
            raise InjectError(f"截屏 {label} 失败: {e}") from None
        except shot.TransferError as e:
            if self.resets >= self.retries:
                fail_path = shot.save_fail_evidence(str(out), e.partial, e, self.resets + 1)
                self.ser.note(f"截屏 {label} 失败（重试耗尽）；证据 {fail_path.name}；"
                              f"做一次 best-effort 复位")
                self.ser.close()
                shot.best_effort_reset(self.port, BAUD)
                self.ser = self._open()
                raise InjectError(
                    f"截屏 {label} 失败（已复位重试 {self.resets} 次）: {e}\n"
                    f"证据: {fail_path}") from None
            self.resets += 1
            self.ser.note(f"截屏 {label} 传输失败（{e}）；esptool 复位后场景重跑"
                          f"（第 {self.resets}/{self.retries} 次重试）")
            self._reset_and_reopen()
            raise ScenarioRestart(f"截屏 {label} 传输失败，设备已复位") from None
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


def dim(rgb: tuple, pct: int = 45) -> tuple:
    """page_step.c 的 DIM_PCT 口径：绝对步 ≥ 本轨 LEN 的格取原色 45%。"""
    return tuple(c * pct // 100 for c in rgb)


# 暗色（LEN 之外）：bank1 的绝对步 ≥ 默认 LEN(16)，故整窗格都是暗色系
C_PANEL_HI_DIM = dim(C_PANEL_HI)


def playhead_columns(img, window_len: int = 16) -> dict:
    """按行读出播放头所在列（播放头 = 格子四周 5px UI_ACCENT 描边）。
    几何与 page_step.c 同源：行 y 起点 = 72(页签条) + 84，行距 124；
    列 x 起点 = 88，列距 73，格 67×104。只扫窗口内列（隐藏列不可能有描边）。"""
    out = {}
    for r in range(4):
        y = STEP_GRID_Y0 + r * STEP_ROW_PITCH
        cols = []
        for c in range(window_len):
            x0 = STEP_GRID_X0 + c * STEP_CELL_PITCH
            hit = 0
            for x in range(x0, x0 + STEP_CELL_W):
                for yy in (y + 1, y + STEP_CELL_H - 2):
                    px = img.getpixel((x, yy))
                    if max(abs(px[i] - C_YELLOW[i]) for i in range(3)) <= 16:
                        hit += 1
            if hit > 20:
                cols.append(c)
        out[r] = cols
    return out

# 步网格几何（屏幕绝对坐标 = 页内坐标 + 页签条 UI_TAB_BAR_H；与 page_step.c 的宏
# 同名同值，页面布局改动时同步这里）。注入坐标一律由这些宏派生，不写死中心点。
#   GRID_X0/GRID_Y0/CELL_W/CELL_H/CELL_STEP_X/ROW_PITCH = page_step.c 步网格宏
#   NUM_Y                                        = 列号标签行（页内 y）
STEP_GRID_X0 = 88
STEP_GRID_Y0 = 72 + 84
STEP_CELL_W = 67
STEP_CELL_H = 104
STEP_CELL_PITCH = 73
STEP_ROW_PITCH = 124


def step_cell_xy(col: int, row: int) -> tuple:
    """步窗**窗口列** col（0 起）× 行 row 的屏幕绝对中心。
    列在屏上的 x 与窗口起点无关（16 列固定几何）；当前窗口能命中的列数 =
    STATE SNAP 的 window_len，绝对步号 = window_start + col。"""
    return (STEP_GRID_X0 + col * STEP_CELL_PITCH + STEP_CELL_W // 2,
            STEP_GRID_Y0 + row * STEP_ROW_PITCH + STEP_CELL_H // 2)

# 注入坐标（屏幕绝对坐标 = 页内坐标 + 页签条 UI_TAB_BAR_H，各点取控件矩形中心）。
# 每个数字对应固件里的布局宏——页面布局改动时按同名宏同步这里：
#   UI_TAB_BAR_H  = 72   （ui.c）
#   TRANSPORT_*   = page_transport.c 建按钮的 TRANSPORT_{PLAY,STOP,BPM}_{X,Y,W,H}
#   GRID_*/CELL_*/ROW_PITCH = page_step.c 步网格宏（见上）
#   CARD_*        = page_tracks.c 卡宏（卡距 = CARD_W + 16）
TRANSPORT_PLAY = (208, 210)       # TRANSPORT_PLAY 中心：x=48+320/2，y=72+48+180/2
TRANSPORT_STOP = (560, 210)       # TRANSPORT_STOP 中心：x=400+320/2，y 同 PLAY
TRANSPORT_BPM_PLUS = (1050, 262)  # TRANSPORT_BPM_PLUS 中心：x=1000+100/2，y=72+140+100/2
TRANSPORT_BPM_MINUS = (910, 262)  # TRANSPORT_BPM_MINUS 中心：x=860+100/2，y 同 "+"
# 默认窗口 {0,0,16} 下的两个编辑格：轨 0 窗口列 2（绝对步 2）/ 轨 1 列 5（绝对步 5）。
# soak.py 直接复用这两个点，故保留模块级常量（窗口默认口径下与 step_cell_xy 一致）。
STEP_CELL_A = step_cell_xy(2, 0)
STEP_CELL_B = step_cell_xy(5, 1)
# 卡 2：x=CARD_X0+2*(CARD_W+16)+CARD_W/2=24+624+148；y=72+CARD_Y+CARD_H/2=72+40+235
TRACKS_CARD2 = (796, 347)
# 卡距：page_tracks.c CARD_PITCH = CARD_W + 16 = 312（soak 循环选卡按此换算）
TRACKS_CARD_PITCH = 312


def ensure_window(s: Session, start: int, window_len: int) -> None:
    """经 console `UI WINDOW <start> <len>` 落步窗，并用 STATE SNAP 断言落地值。
    start 必须是 16 的倍数（0/16/32/48）；len 1..16。"""
    s.cmd(f"UI WINDOW {start} {window_len}")
    s.expect(rf"UI WINDOW bank={start // 16} start={start} len={window_len}",
             f"UI WINDOW {start} {window_len}")
    s.cmd("STATE SNAP")
    s.expect_int(r"window_bank=(\d+)", start // 16, "STATE SNAP window_bank")
    s.expect_int(r"window_start=(\d+)", start, "STATE SNAP window_start")
    s.expect_int(r"window_len=(\d+)", window_len, "STATE SNAP window_len")


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
    """② STEP：窗口列编辑闭环 + 分窗（bank1 绝对步号/隐藏列/播放头映射）+ 截屏。
    可重复运行：目标格已亮先经同一 TAP 路径点灭；结束复位默认窗口与停止态。"""
    ensure_stopped(s)
    ensure_window(s, 0, 16)  # 归一到默认窗口：注入几何由此派生（列 c = 绝对步 c）
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")

    cells = (("0", "2", step_cell_xy(2, 0)), ("1", "5", step_cell_xy(5, 1)))
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

    # ②b 分窗：bank1 的列 0 = 引擎绝对步 16（列号标签 17..32）。轨 0 与 bank0 的
    # 两格互不干扰（bank0 步 2/5 在窗口外）；先归零再点亮，脚本可重复。
    ensure_window(s, 16, 16)
    time.sleep(0.15)  # ≥5 个 30ms UI tick：列号/窗口投影刷新
    bank1_xy = step_cell_xy(0, 0)
    s.cmd("ENG STEP 0 16")
    if s.snap_int(r"\bn=(\d+)") != 0:
        s.ser.note("基线：轨 0 绝对步 16 已亮，先点灭")
        s.cmd("TAP %d %d" % bank1_xy)
        s.expect(r"-> STEP track=0 step=16 CMD_TOGGLE_STEP", "TAP 点灭 bank1 列 0")
    s.cmd("TAP %d %d" % bank1_xy)
    s.expect(r"-> STEP track=0 step=16 CMD_TOGGLE_STEP", "TAP bank1 列 0（绝对步 16）")
    s.cmd("ENG STEP 0 16")
    s.expect_int(r"\bn=(\d+)", 1, "ENG STEP 0 16 n（窗口列 0 写绝对步）")
    s.expect_int(r"\btime=(\d+)", 0, "ENG STEP 0 16 time（NOTE）")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 0, "STATE SNAP focus_track（bank1）")
    s.expect_int(r"focus_step=(\d+)", 0, "STATE SNAP focus_step（窗口列）")
    s.expect_int(r"window_start=(\d+)", 16, "STATE SNAP window_start（bank1）")
    time.sleep(0.15)
    img = s.capture("step-bank1")
    # bank1 的绝对步 16..31 ≥ 默认 LEN(16)：格按 DIM 口径显示（暗色亮格），
    # 编辑语义不变（ENG STEP 断言在上；LEN 编辑属 P4-C）
    s.probe(img, "bank1 轨 0 列 0 亮格（绝对步 16，DIM）", *bank1_xy, dim(C_BLUE), 16)
    s.probe(img, "bank1 轨 0 列 1 空位（DIM）", *step_cell_xy(1, 0), C_PANEL_HI_DIM, 10)
    # 列号标签 17..32 与顶栏 WIN 17..32 由视觉复核（PNG 入库，见报告）

    # ②c window_len=8（窗口 0..7）：列 8 起隐藏（TAP 命不中），可见列 7 命中并可写。
    ensure_window(s, 0, 8)
    time.sleep(0.15)
    hidden_xy = step_cell_xy(8, 0)
    s.cmd("TAP %d %d" % hidden_xy)
    s.expect(rf"TAP miss {hidden_xy[0]},{hidden_xy[1]} page=2", "TAP 隐藏列 8 miss")
    vis_xy = step_cell_xy(7, 1)
    s.cmd("ENG STEP 1 7")
    if s.snap_int(r"\bn=(\d+)") == 0:
        s.cmd("TAP %d %d" % vis_xy)
        s.expect(r"-> STEP track=1 step=7 CMD_TOGGLE_STEP", "TAP 可见列 7（绝对步 7）")
        s.cmd("ENG STEP 1 7")
        s.expect_int(r"\bn=(\d+)", 1, "ENG STEP 1 7 n（窗口列 7 写绝对步）")
    time.sleep(0.15)
    img = s.capture("step-len8")
    s.probe(img, "len=8 列 7 亮格", *vis_xy, C_GREEN)
    s.probe(img, "len=8 列 8 隐藏（页面背景）", *hidden_xy, C_BG, 10)

    # 清掉分窗阶段的临时步，复位默认窗口（脚本可重复）
    s.cmd("TAP %d %d" % vis_xy)
    s.expect(r"-> STEP track=1 step=7 CMD_TOGGLE_STEP", "TAP 点灭可见列 7（清理）")
    s.cmd("ENG STEP 1 7")
    s.expect_int(r"\bn=(\d+)", 0, "ENG STEP 1 7 n（清理）")
    ensure_window(s, 16, 16)
    time.sleep(0.05)
    s.cmd("TAP %d %d" % bank1_xy)
    s.expect(r"-> STEP track=0 step=16 CMD_TOGGLE_STEP", "TAP 点灭 bank1 列 0（清理）")
    s.cmd("ENG STEP 0 16")
    s.expect_int(r"\bn=(\d+)", 0, "ENG STEP 0 16 n（清理）")

    # ②d 播放头映射：bank0 播放时各行有播放头；窗口切到 bank1（默认 LEN=16）后
    # 播放头落在窗口外 → 不显示（无残留强调色描边）。
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "TAP PLAY（播放头证据）")
    ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    time.sleep(0.4)  # ≥10 个 30ms tick：播放头与步位置同步
    img = s.capture("step-bank0-playhead")
    cols_a = playhead_columns(img)
    for r in range(4):
        (s.ok if len(cols_a[r]) == 1 else s.fail)(
            f"bank0 行 {r} 播放头列 {cols_a[r]}" if len(cols_a[r]) == 1
            else f"bank0 行 {r} 播放头应唯一（实得 {cols_a[r]}）")
    ensure_window(s, 16, 16)
    time.sleep(0.4)
    img = s.capture("step-bank1-no-playhead")
    cols_b = playhead_columns(img)
    stray = {r: cols for r, cols in cols_b.items() if cols}
    (s.fail if stray else s.ok)(
        f"bank1 窗口外播放头不显示（无强调色描边）" if not stray
        else f"bank1 窗口外仍有播放头描边：{stray}")

    # 复位：停止 + 默认窗口 + PLAY 页（脚本可重复运行）
    s.cmd("UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "TAP STOP（复位）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d+)", 0, "STATE SNAP playing（复位）")
    ensure_window(s, 0, 16)
    s.cmd("UI play")


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
    ap.add_argument("--retry", type=int, default=shot.DEFAULT_RETRIES, metavar="N",
                    help="截屏传输失败时经 esptool 复位后场景重跑的次数（默认 "
                         f"{shot.DEFAULT_RETRIES}；0 = 不重试）")
    args = ap.parse_args()

    require_deps()

    if args.retry < 0:
        ap.error("--retry 不能为负")

    if args.out_dir:
        out_dir = Path(args.out_dir)
    else:
        project = os.environ.get("SLOOP_TAB5_PROJECT") or str(SCRIPTS_DIR.parent / "firmware")
        out_dir = Path(project) / "build" / "verify"
    out_dir.mkdir(parents=True, exist_ok=True)

    stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
    log_path = out_dir / f"inject-{args.scenario}-{stamp}.log"
    s = Session(args.port, log_path, out_dir, stamp, args.scenario,
                timeout=args.timeout, retries=args.retry)

    print(f"== inject {args.scenario} @ {args.port}")
    print(f"   串口原始日志: {log_path}")
    try:
        s.wait_ready()
        run = 0
        while True:
            retry_note = "" if run == 0 else f"（第 {run + 1} 轮：复位后重跑）"
            s.ser.note(f"场景 {args.scenario} 开始{retry_note}")
            try:
                SCENARIOS[args.scenario](s)
            except ScenarioRestart as e:
                print(f"  [RETRY] {e}（{run + 1}/{args.retry}），场景从头重跑")
                run += 1
                continue
            s.ser.note(f"场景 {args.scenario} 结束 failures={s.failures}")
            break
    except InjectError as e:
        s.ser.note(f"场景 {args.scenario} 异常终止: {e}")
        sys.exit(f"[FAIL] {args.scenario}: {e}")
    finally:
        s.ser.close()

    if s.failures:
        sys.exit(f"[FAIL] {args.scenario}: {s.failures} 项断言/抽检未通过（见上）")
    print(f"[PASS] {args.scenario}: 全部断言与像素抽检通过"
          + (f"（截屏复位重试 {s.resets} 次）" if s.resets else ""))


if __name__ == "__main__":
    main()
