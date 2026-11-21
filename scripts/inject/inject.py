#!/usr/bin/env python3
"""inject.py — Tab5 固件串口注入序列（AGENTS.md §5.4 注入约定）

五条场景脚本（transport.sh / step_edit.sh / tracks.sh / nav.sh / params.sh）是本文的
薄封装。每个场景：
  1. `UI <page>` 同步切页（步窗场景用 `UI WINDOW <start> <len>` 落窗口）；
  2. 若干 `TAP <x> <y>`（或 `KEY`）注入，逐条断言固件回显；STEP 的注入坐标由
     页面布局宏派生（step_cell_xy），列语义对齐当前窗口——列 c = 引擎绝对步
     window_start + c，列数 = STATE SNAP 的 window_len；参数页的注入坐标由
     param_knob_xy() 按 cell/旋钮盒宏派生（± 按钮 + 旋钮焦点）；
  3. `STATE SNAP` / `ENG STEP` / `PARAM` 断言状态变化；
  4. 经 SCR 协议截屏（复用 scripts/screenshot.py 的读取/解码函数），对产物做像素抽检。

原始串口输出逐字节写入日志（不裁剪、不后编辑）。日志里唯一非固件字节是
`# [inject] ...` 前缀的工具注记（标记截屏位置等）；SCR 的 1.8MB 二进制帧体不入日志。

截屏传输失败（DI-5：帧数据超时/短帧/坏尾）时与 `scripts/screenshot.py` 同源处理：
esptool 硬复位 → 等 console 提示符 → 重开串口 → 场景从头重跑（复位把页面/走带
状态清回基线，重跑才能让断言与截图同源），默认最多 3 次。

用法（一般经五条脚本调用）：
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
    def in_waiting(self) -> int:
        return self._ser.in_waiting

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

    # ---- 快速命令（导航场景的 30ms tick 相位对齐；见 nav_anchor） ----
    def _read_prompts(self, count: int, timeout: float) -> str:
        """非阻塞轮询到 count 个 console 提示符；单次往返 ~1ms。

        `read_response` 用 `read(256)`（pyserial 在无数据时阻塞到 timeout）——
        每次调用 ~50ms，够不上 30ms tick 的对齐精度。这里用 in_waiting 轮询。"""
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            k = self.ser.in_waiting
            if not k:
                time.sleep(0.0003)
                continue
            buf.extend(self.ser.read(k))
            if buf.count(b"sloop>") >= count and _prompt_at(buf):
                return buf.decode(errors="replace")
        raise TimeoutError(f"等待 {count} 个提示符超时（已收 {len(buf)} 字节）")

    def cmd_fast(self, line: str, timeout: float = 2.0) -> None:
        """同 cmd()，走 in_waiting 快速读取（~1ms 往返）。"""
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        try:
            self.last = self._read_prompts(1, timeout)
        except TimeoutError as e:
            raise InjectError(f"命令 {line!r} 无响应：{e}") from None

    def cmd_batch(self, lines, timeout: float = 3.0) -> float:
        """多行一次性写入（行间无主机延迟），读到 len(lines) 个提示符。
        返回主机侧耗时 ms。回显行序即固件处理序：同 tick 用例靠它把多条命令压在
        同一个 30ms UI 周期内（若逐条发送，每条 ~50ms 往返，必然跨拍）。"""
        t0 = time.monotonic()
        self.ser.reset_input_buffer()
        self.ser.write(("\n".join(lines) + "\n").encode())
        self.ser.flush()
        try:
            self.last = self._read_prompts(len(lines), timeout)
        except TimeoutError as e:
            raise InjectError(f"批量命令 {lines!r} 无响应：{e}") from None
        return (time.monotonic() - t0) * 1000.0

    def page_fast(self) -> int:
        """UI 无参 → 当前页 id（快速路径）。"""
        self.cmd_fast("UI")
        m = re.search(r"UI page=\w+ \((\d+)\)", self.last)
        if m is None:
            raise InjectError(f"UI 回显缺少页号：{self.last!r}")
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

    def capture(self, label: str, timeout: float = 30.0):
        """截屏并落 PNG。timeout 是单阶段等待上限，与 `screenshot.py --timeout` 默认一致
        （实测整帧 16.4–17.8s，20s 仅约 12% 余量）。"""
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


# 锚点颜色取主题色经 **RGB565 量化**后的实测值（本机帧缓冲为 rgb565；与
# screenshot.py --check 同口径）。量化误差最大 ~4/通道，调用方容差 10–16 覆盖：
# 例：UI_PANEL_HI 0x242B33=(36,43,51) → (32,40,48)；UI_TEXT 0xF2F5F8=(242,245,248)
# → (240,244,248)。不要按主题 hex 直接改这些值，否则探针会因位扩展差误报。
C_BLUE = (56, 120, 216)   # 轨 0 主题色 0x3B7BD8
C_GREEN = (56, 160, 72)   # 轨 1 主题色 0x3FA34D
C_YELLOW = (224, 176, 40) # 轨 2 / UI_ACCENT 0xE0B32A
C_ORANGE = (224, 104, 24) # 轨 3 主题色 0xE06A1B
C_BG = (16, 20, 24)       # UI_BG 0x101418
C_PANEL = (24, 32, 32)    # UI_PANEL 0x1B2026
C_PANEL_HI = (32, 40, 48) # UI_PANEL_HI 0x242B33 → rgb565 (32,40,48)
C_PANEL_SEL = (40, 52, 64)  # UI_PANEL_SEL 0x2C3540
C_TEXT = (240, 244, 248)  # UI_TEXT 描边（3px 焦点框）/ 焦点旋钮的数值
C_TEXT_DIM = (144, 148, 160)  # UI_TEXT_DIM 0x8A949E → rgb565 (144,148,160)：非焦点旋钮文本
# 页签条几何随注册页数收窄（ADR-008）：7 页（4 基础 + 3 参数页）时 tab_w = 150、
# 间距 8 → 页签 i 的 x 区间 = 16 + i*158 .. +150。探针取页签**左缘内 8px** 的底色
# （y=36）：该处只有填充色，不落在居中的页签文本上（4 页布局时用过的中心点会压到
# 字形，故按新布局改左缘）。
C_TAB_PLAY = (24, 36)
C_TAB_TRACKS = (182, 36)
C_TAB_STEP = (340, 36)
C_TAB_TRANSPORT = (498, 36)
C_TAB_GLOBAL = (656, 36)
C_TAB_FX = (814, 36)
C_TAB_ANALOG = (972, 36)


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

# 页签条（theme.h UI_TAB_X0/W/GAP/H/Y + ui.c 的页签建成）：页签 i 的 x 区间 =
# UI_TAB_X0 + i*(UI_TAB_W+UI_TAB_GAP) .. +UI_TAB_W，y = UI_TAB_Y .. +UI_TAB_H。
# nav 用例①取相邻页签之间 8px 缝的中心：在页签条内、却在所有页签矩形之外。
# 页面注册表 = 7 页（PLAY/TRACKS/STEP/TRANSPORT/GLOBAL/FX/ANALOG）时 ui.c 的页签
# 宽度：min(UI_TAB_W=176, (1280 - 2*16 - 150(右侧留位) - 6*8) / 7) = 150。
UI_TAB_X0, UI_TAB_W, UI_TAB_GAP = 16, 150, 8
UI_TAB_Y, UI_TAB_H = 10, 52


def tab_gap_xy(i: int) -> tuple:
    """页签 i 与 i+1 之间缝隙的中心（屏幕绝对坐标）。"""
    return (UI_TAB_X0 + (i + 1) * (UI_TAB_W + UI_TAB_GAP) - UI_TAB_GAP // 2,
            UI_TAB_Y + UI_TAB_H // 2)


def ensure_window(s: Session, start: int, window_len: int) -> int:
    """经 console `UI WINDOW <start> <len>` 落步窗，并用 STATE SNAP 断言落地值。
    start 必须是 16 的倍数（0/16/32/48）；len 1..16。返回 window_len（供
    播放头/像素扫描按实际窗口列数设置几何）。"""
    s.cmd(f"UI WINDOW {start} {window_len}")
    s.expect(rf"UI WINDOW bank={start // 16} start={start} len={window_len}",
             f"UI WINDOW {start} {window_len}")
    s.cmd("STATE SNAP")
    s.expect_int(r"window_bank=(\d+)", start // 16, "STATE SNAP window_bank")
    s.expect_int(r"window_start=(\d+)", start, "STATE SNAP window_start")
    s.expect_int(r"window_len=(\d+)", window_len, "STATE SNAP window_len")
    return window_len


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

    # 播放中切 STEP：先归一窗口（窗口是持久 UI 状态；播放头列扫描按 window_len 几何）。
    # 播放头由引擎 step_pos 驱动；SCR 抓的是"上一已刷新帧"（≤30ms 旧），所以每帧的
    # 播放头列应等于抓屏前 ENG SNAP 的 pos（或紧邻 1–2 列）；两帧的 pos 不同即证明
    # 播放推进（16 列环绕会让"列号不同"本身不可靠，故以引擎为锚）。
    wlen = ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    time.sleep(0.4)  # ≥10 个 30ms tick，播放头与步位置同步稳定

    def shot_with_pos(label):
        s.cmd("ENG SNAP")
        pre = s.snap_int(r"pos=(\d+)")
        img = s.capture(label)
        return pre, playhead_columns(img, wlen), img

    pre_a, cls_a, _ = shot_with_pos("step-playhead-a")
    time.sleep(0.3)
    pre_b, cls_b, img_b = shot_with_pos("step-playhead-b")

    s.probe_region(img_b, "播放头描边",
                   (88, 156, 88 + wlen * 73, 156 + 3 * 124 + 104), C_YELLOW, 16, 100)
    s.probe(img_b, "页签选中（STEP）", *C_TAB_STEP, C_YELLOW)

    for r in range(4):
        want_a = {(pre_a + k) % wlen for k in (0, 1, 2)}
        want_b = {(pre_b + k) % wlen for k in (0, 1, 2)}
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


#
# ---- nav 场景（导航回归）：四条导航行为的注入方法 ----
#
# 四条用例里有两条依赖「两条命令落在同一个 30ms UI tick 内」。单条 console 命令走
# `cmd()` 的慢路径往返 ~50ms（read_response 的 read(256) 在无数据时阻塞到 timeout），
# 逐条发送必然跨拍；因此这两条用例一律**一次性写入多行**（cmd_batch：行间无主机
# 延迟），再用 in_waiting 快速读取（~1ms）确认回显。
#
# 「同一拍」不能只靠时序猜：30ms 定时器的相位随机，且实测拍间隔偶有 ~200–270ms 的
# 迟滞（LVGL 任务被渲染/刷新拖住）。因此：
#
# - 用例③ 用**批内第三条 `UI`** 在设备侧自证前提：tab 与 PANIC 之间若被一拍插入，
#   页面在 PANIC 之前就已前进，批内 UI 立即读出新页——此时前提不成立，重新对齐
#   （工具侧重试，日志注记），不算固件失败；只有「批内 UI 仍是原页（PANIC 执行时
#   请求尚未被消费）+ 观察窗（NAV_SETTLE_S=1.0s，约 33 个 30ms 拍）后无回跳」才判定
#   PANIC 撤销成功。
# - 用例④ 同理：批内第四条 `UI` 仍是起点页 ⇒ press1..press3 之间没有拍执行过
#   （任何一拍都会立即把页面推进一页），3 次请求因此压在同一个邮箱里。
# - 前提不成立的重对齐最多 NAV_ATTEMPTS 次；耗尽即 FAIL（脚本不通过，不放宽断言）。

UI_TICK_S = 0.030
NAV_SETTLE_S = 1.0  # ≈33 个 30ms 拍；实测拍迟滞可达 ~270ms，观察窗留到 1s
                     # （>1s 停摆时用例③的「仍 TRACKS」可能被误判为通过，故取 1s）
NAV_ATTEMPTS = 5
# STEP 页右上角状态行（page_step.c：状态标签 (656,14) 宽 600 右对齐，16pt）：
# 合成轨 TAP 后显示 "T1 S3"（轨色），鼓轨 TAP 后显示 "DRUM EDIT = PHASE 4"（橙）。
NAV_STATUS_BOX = (1040, 82, 1258, 112)


def count_region(img, box: tuple, rgb: tuple, tol: int) -> int:
    """区域内与 rgb 各通道差 ≤ tol 的像素数（RGB565 量化后口径，同 probe_region）。"""
    colors = img.crop(box).getcolors(maxcolors=1 << 20) or []
    return sum(c for c, px in colors
               if max(abs(px[0] - rgb[0]), abs(px[1] - rgb[1]), abs(px[2] - rgb[2])) <= tol)


NAV_STATE_FIELDS = ("page", "focus_track", "focus_step", "layer_held", "selected_track",
                    "last_note", "bpm", "playing", "window_bank", "window_start", "window_len")


def nav_state_fields(s: Session) -> dict:
    """STATE SNAP 导航相关字段（AppState 首行逐字段）。

    11 个字段必须全部出现：缺字段（固件快照改名/丢字段）时不得以 None==None 静默
    通过比较——直接 InjectError（脚本非零退出），与断言失败同口径暴露。"""
    s.cmd("STATE SNAP")
    m = re.search(r"^STATE SNAP (.*)$", s.last, re.MULTILINE)
    if m is None:
        raise InjectError("STATE SNAP 回显缺失")
    kv = dict(re.findall(r"(\w+)=(\S+)", m.group(1)))
    missing = [k for k in NAV_STATE_FIELDS if k not in kv]
    if missing:
        raise InjectError(f"STATE SNAP 缺少字段: {', '.join(missing)}")
    return {k: kv[k] for k in NAV_STATE_FIELDS}


def nav_eng_snapshot(s: Session) -> list:
    """ENG SNAP 全量：首行（playing/bpm/sel/pos）+ 4 条 trk + 16 条 dstep。"""
    s.cmd("ENG SNAP")
    lines = [l for l in s.last.splitlines()
             if l.startswith(("ENG SNAP playing=", "ENG SNAP trk ", "ENG SNAP dstep "))]
    if len(lines) != 21:
        raise InjectError(f"ENG SNAP 回显不完整（{len(lines)}/21 行）")
    return lines


def nav_same(s: Session, label: str, before, after) -> None:
    """快照逐字段/逐行比对（导航用例的「不变」断言）。"""
    if before == after:
        s.ok(f"{label} 前后一致")
        return
    if isinstance(before, dict):
        diff = [f"{k}: {before[k]} -> {after.get(k)}" for k in before
                if before[k] != after.get(k)]
    else:
        diff = [f"{b} -> {a}" for b, a in zip(before, after) if b != a]
        if len(before) != len(after):
            diff.append(f"行数 {len(before)} -> {len(after)}")
    s.fail(f"{label} 发生变化: " + "; ".join(diff))


def nav_set_focus(s: Session, track: int, step: int) -> None:
    """方向键把焦点归一到 (track, step)：焦点是 UI 局部态（state_set_focus），
    ↑/↓ 在三条合成轨循环、←/→ 在窗口列循环，都不发引擎写。"""
    s.cmd("STATE SNAP")
    t0, s0 = s.snap_int(r"focus_track=(\d+)"), s.snap_int(r"focus_step=(\d+)")
    for _ in range((track - t0) % 3):
        s.cmd("KEY 0x4B down")
        s.cmd("KEY 0x4B up")
    for _ in range((step - s0) % 16):
        s.cmd("KEY 0x4C down")
        s.cmd("KEY 0x4C up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", track, f"焦点轨归一 = {track}")
    s.expect_int(r"focus_step=(\d+)", step, f"焦点列归一 = {step}")


def nav_anchor(s: Session, start_page: int) -> dict:
    """单按 tab，快速轮询 UI 直到页翻转；返回 {"page", "lower", "detect"}。

    lower = 最后一次「读到旧页」的主机时刻（初始为 tab 写入时刻）：旧页响应先于
    翻转产生，故 lower ≤ 拍执行时刻，下一拍 ≥ lower + 30ms——批次只要在此之前
    结束，批内命令就同属一个 30ms 周期。detect = 首次读到新页的时刻。"""
    t_write = time.monotonic()
    s.cmd_fast("KEY 0x20 down")  # tab 只在按下沿请求「当前页 + 1」（ADR-006）
    lower = t_write
    polls = 0
    deadline = t_write + 2.0
    while time.monotonic() < deadline:
        page = s.page_fast()
        now = time.monotonic()
        polls += 1
        if page != start_page:
            return {"page": page, "lower": lower, "detect": now, "write": t_write,
                    "polls": polls}
        lower = now
    raise InjectError(f"tab 按下后 2s 内页面未从 {start_page} 前进（UI tick 停摆？）")


def nav_last_page(s: Session) -> int:
    """回显里最后一次 `UI page=...` 的页号（批内 UI 用）。"""
    found = re.findall(r"UI page=\w+ \((\d+)\)", s.last)
    if not found:
        raise InjectError(f"回显缺少 UI 页号：{s.last!r}")
    return int(found[-1])


def run_nav(s: Session) -> None:
    """nav 场景：阶段 3 评审点名的四条导航行为（阶段 4 路由重构最易静默破坏）。

    ① 页签缝与页签矩形内的 TAP 均 → `TAP miss`，STATE/ENG 快照逐字段不变；
    ② STEP 鼓轨行 TAP 只读：回显 `-> STEP drum ... read-only`、无 CMD_TOGGLE_STEP、
       焦点轨/列不变、ENG SNAP 全量不变、鼓格像素区不变、状态行橙字出现；
    ③ 同一 30ms 拍内 KEY tab + KEY PANIC：批内 UI 自证请求尚未被消费，PANIC 后
       1s（≈33 个 30ms 拍）仍停在原页（撤销待切页，无回跳）；
    ④ 同一 30ms 拍内 3 组 KEY tab 按下/释放：1s 后只前进一页（ADR-006「一拍
       一页」），再 1s 无第二次前进。
    """
    ensure_stopped(s)
    ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    time.sleep(0.1)  # ≥3 拍：切页首帧刷新稳定

    # 基线归一（所有截屏的确定性来源）：合成轨格 TAP 两次（落回原值，脚本可重复），
    # 顺带把状态行从上一轮必然残留的 "DRUM EDIT = PHASE 4" 换成合成轨文本（蓝字）——
    # 这样每次运行的状态行都从同一文本出发，PNG 跨运行逐字节一致；也让「鼓轨点击后
    # 状态行变橙」在重复运行下仍可判定。第 2 列与用例②的目标焦点列一致，焦点只需换轨。
    ctrl_xy = step_cell_xy(2, 0)
    s.cmd("TAP %d %d" % ctrl_xy)
    s.expect(r"-> STEP track=0 step=2 CMD_TOGGLE_STEP", "基线：合成轨格 TAP（对照组）")
    s.cmd("TAP %d %d" % ctrl_xy)
    s.expect(r"-> STEP track=0 step=2 CMD_TOGGLE_STEP", "基线：合成轨格再 TAP（落回原值）")

    # ---------- ① 页签区 TAP：miss 且状态/引擎不变 ----------
    # 两个点都必须在 TAP 路线上 miss：页签缝（页签条内、所有页签矩形外）与页签矩形
    # 内部（PLAY 页签中心 104,36）。后者钉住「页签矩形被接进 TAP 路由」的回归——一旦
    # 接进来，点页签会切页，与「TAP 只派发当前页控件」的约定冲突（切页走 UI/KEY tab）。
    gap_xy = tab_gap_xy(0)
    tab_xy = (UI_TAB_X0 + UI_TAB_W // 2, UI_TAB_Y + UI_TAB_H // 2)  # PLAY 页签中心
    s.ser.note(f"用例①：TAP 页签缝 {gap_xy[0]},{gap_xy[1]} 与页签矩形内 "
               f"{tab_xy[0]},{tab_xy[1]}（均须 miss）")
    st1 = nav_state_fields(s)
    eng1 = nav_eng_snapshot(s)
    s.cmd("TAP %d %d" % gap_xy)
    s.expect(rf"TAP miss {gap_xy[0]},{gap_xy[1]} page=2", "① 页签缝 TAP miss")
    s.cmd("TAP %d %d" % tab_xy)
    s.expect(rf"TAP miss {tab_xy[0]},{tab_xy[1]} page=2", "① 页签矩形内 TAP miss")
    nav_same(s, "① STATE SNAP", st1, nav_state_fields(s))
    nav_same(s, "① ENG SNAP", eng1, nav_eng_snapshot(s))
    img_step = s.capture("nav-01-tabbar")
    s.probe(img_step, "① 页签缝底（页签条 UI_PANEL）", *gap_xy, C_PANEL, 10)
    s.probe(img_step, "① 页签选中（STEP）", *C_TAB_STEP, C_YELLOW)

    # ---------- ② STEP 鼓轨行只读 ----------
    nav_set_focus(s, 2, 2)  # 非默认焦点：让「焦点不变」有判别力
    drum_xy = step_cell_xy(3, 3)  # 行 3 = DRM，窗口列 3 = 绝对步 3
    s.ser.note(f"用例②：TAP 鼓轨格 {drum_xy[0]},{drum_xy[1]}（DRM 行只读）")
    st2 = nav_state_fields(s)
    eng2 = nav_eng_snapshot(s)
    img_before = s.capture("nav-02-drum-before")
    blue_before = count_region(img_before, NAV_STATUS_BOX, C_BLUE, 16)
    orange_before = count_region(img_before, NAV_STATUS_BOX, C_ORANGE, 16)
    (s.ok if blue_before >= 25 and orange_before == 0 else s.fail)(
        f"② 状态行基线：蓝字（合成轨）{blue_before} / 橙字 {orange_before}"
        f"（期望蓝 ≥25、橙 0）")

    s.cmd("TAP %d %d" % drum_xy)
    s.expect(r"-> STEP drum step=3 read-only", "② 鼓轨 TAP read-only 回显")
    (s.ok if "CMD_TOGGLE_STEP" not in s.last else s.fail)("② 鼓轨 TAP 未发 CMD_TOGGLE_STEP")
    st2_after = nav_state_fields(s)
    s.expect_int(r"focus_track=(\d+)", 2, "② STATE SNAP focus_track 不变")
    s.expect_int(r"focus_step=(\d+)", 2, "② STATE SNAP focus_step 不变")
    nav_same(s, "② STATE SNAP", st2, st2_after)
    nav_same(s, "② ENG SNAP（无引擎写）", eng2, nav_eng_snapshot(s))

    img_after = s.capture("nav-03-drum-readonly")
    box = (drum_xy[0] - STEP_CELL_W // 2 - 2, drum_xy[1] - STEP_CELL_H // 2 - 2,
           drum_xy[0] + STEP_CELL_W // 2 + 2, drum_xy[1] + STEP_CELL_H // 2 + 2)
    same_cell = img_before.crop(box).tobytes() == img_after.crop(box).tobytes()
    (s.ok if same_cell else s.fail)(
        f"② 鼓格像素区 {box} 点击前后逐字节相同（只读、不点亮）" if same_cell
        else f"② 鼓格像素区 {box} 点击后变化（鼓轨行不应可编辑）")
    orange_after = count_region(img_after, NAV_STATUS_BOX, C_ORANGE, 16)
    blue_after = count_region(img_after, NAV_STATUS_BOX, C_BLUE, 16)
    (s.ok if orange_after >= 25 and blue_after == 0 else s.fail)(
        f"② 状态行点击后：橙字 {orange_after} / 蓝字 {blue_after}"
        f"（期望 ≥25 / 0，DRUM EDIT = PHASE 4）")

    # ---------- ③ 同一拍：KEY tab + KEY PANIC ----------
    premise = False
    for attempt in range(1, NAV_ATTEMPTS + 1):
        s.cmd_fast("UI play")
        s.expect(r"UI page=play \(0\)", f"③ 起点 PLAY（对齐 {attempt}/{NAV_ATTEMPTS}）")
        anchor = nav_anchor(s, 0)
        s.expect(r"UI page=tracks \(1\)", f"③ 锚点单按 tab 前进一页（对齐 {attempt}）")
        span = s.cmd_batch(["KEY 0x20 down", "KEY PANIC", "UI"])
        t_end = time.monotonic()
        mid = nav_last_page(s)
        margin = (anchor["lower"] + UI_TICK_S - t_end) * 1000.0
        s.ser.note(f"③ 对齐 {attempt}: 批次 {span:.1f}ms，锚点下界→批次结束余量 "
                   f"{margin:.1f}ms（tab 写入→翻转 "
                   f"{(anchor['detect'] - anchor['write']) * 1000:.1f}ms，下界→翻转 "
                   f"{(anchor['detect'] - anchor['lower']) * 1000:.1f}ms，轮询 "
                   f"{anchor['polls']} 次），批内 UI 页={mid}")
        if mid != 1:
            s.ser.note("③ 前提不成立：批内 UI 已前进 → 有拍插在 tab 与 PANIC 之间；重新对齐")
            print(f"  [NOTE] ③ 对齐 {attempt}: 拍插在 tab/PANIC 之间（批内页={mid}），重新对齐")
            continue
        premise = True
        break
    if premise:
        s.ok("③ 前提成立：PANIC 执行时 tab 请求尚未被拍消费（批内 UI 仍 TRACKS）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=tracks \(1\)", "③ 6 拍后仍 TRACKS（撤销待切页、无回跳）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=tracks \(1\)", "③ 再 6 拍后仍 TRACKS")
        img = s.capture("nav-04-panic-final")
        s.probe(img, "③ 页签选中（TRACKS）", *C_TAB_TRACKS, C_YELLOW)
        s.probe(img, "③ PLAY 页签未选中", *C_TAB_PLAY, C_PANEL_HI, 10)
    else:
        s.fail(f"③ {NAV_ATTEMPTS} 次对齐都未能把 tab/PANIC 压进同一拍（测试前提不成立）")

    # ---------- ④ 同一拍内 3 次 KEY tab：只前进一页 ----------
    premise = False
    for attempt in range(1, NAV_ATTEMPTS + 1):
        s.cmd_fast("UI play")
        s.expect(r"UI page=play \(0\)", f"④ 起点 PLAY（对齐 {attempt}/{NAV_ATTEMPTS}）")
        anchor = nav_anchor(s, 0)
        s.expect(r"UI page=tracks \(1\)", f"④ 锚点单按 tab 前进一页（对齐 {attempt}）")
        # down/up 配对：若输入层将来对「重复按下沿」去抖，连续 3 个 down 会被合并成
        # 1 次请求而假通过；3 组真实按下/释放仍须只前进一页。
        span = s.cmd_batch(["KEY 0x20 down", "KEY 0x20 up"] * 3 + ["UI"])
        t_end = time.monotonic()
        mid = nav_last_page(s)
        margin = (anchor["lower"] + UI_TICK_S - t_end) * 1000.0
        s.ser.note(f"④ 对齐 {attempt}: 批次 {span:.1f}ms，锚点下界→批次结束余量 {margin:.1f}ms"
                   f"（tab 写入→翻转 {(anchor['detect'] - anchor['write']) * 1000:.1f}ms，"
                   f"下界→翻转 {(anchor['detect'] - anchor['lower']) * 1000:.1f}ms，"
                   f"轮询 {anchor['polls']} 次），批内 UI 页={mid}")
        if mid != 1:
            s.ser.note("④ 前提不成立：批内 UI 已前进 → 3 次按下之间夹了拍；重新对齐")
            print(f"  [NOTE] ④ 对齐 {attempt}: 按下之间夹了拍（批内页={mid}），重新对齐")
            continue
        premise = True
        break
    if premise:
        s.ok(f"④ 前提成立：批内 UI 仍 TRACKS，3 次按下之间无拍执行"
             f"（时序下界余量 {margin:.1f}ms，仅作参考）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=step \(2\)", "④ 3 次 tab 只前进一页（TRACKS→STEP）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=step \(2\)", "④ 再 6 拍无第二次前进")
        img = s.capture("nav-05-tab3-final")
        s.probe(img, "④ 页签选中（STEP）", *C_TAB_STEP, C_YELLOW)
        s.probe(img, "④ TRANSPORT 页签未选中", *C_TAB_TRANSPORT, C_PANEL_HI, 10)
    else:
        s.fail(f"④ {NAV_ATTEMPTS} 次对齐都未能把 3 次 tab 压进同一拍（测试前提不成立）")

    # 复位：PLAY 页（脚本可重复运行）
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "复位 UI play")



# ---- params 场景（参数页框架）：8 槽旋钮 + ± 细调 + 键盘 ↑/↓ ----
# 几何与 page_params.c / knob.c 的宏同源：cell 300×290，x = 16 + c·316，
# y = 72(页签条) + 52 + r·298；旋钮盒 160×160 在单元内 (70,26)；± 按钮在页眉
# 右侧 (1096,2)/(1184,2) 80×40（屏幕绝对 y = 72+2+20 = 94）。
PARAM_GRID_X0 = 16
PARAM_GRID_Y0 = 72 + 52
PARAM_CELL_PITCH = 316
PARAM_ROW_PITCH = 298
PARAM_OBJ_DX, PARAM_OBJ_DY, PARAM_OBJ_SIZE = 70, 26, 160
PARAM_BTN_MINUS = (1136, 94)
PARAM_BTN_PLUS = (1224, 94)


def param_knob_xy(slot: int) -> tuple:
    """参数页槽位 slot（0..7，行优先）旋钮盒中心的屏幕绝对坐标。"""
    r, c = divmod(slot, 4)
    x = PARAM_GRID_X0 + c * PARAM_CELL_PITCH + PARAM_OBJ_DX + PARAM_OBJ_SIZE // 2
    y = PARAM_GRID_Y0 + r * PARAM_ROW_PITCH + PARAM_OBJ_DY + PARAM_OBJ_SIZE // 2
    return (x, y)


def param_value(s: Session, key: str = "value") -> int:
    """`PARAM <id>` / `PARAM GLOBAL <id>` 回显里的数值字段。缺字段直接 InjectError
    （固件回显改名时不得静默通过）。"""
    m = re.search(rf"\b{key}=(-?\d+)", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"PARAM 回显缺少 {key}=：{s.last!r}")
    return int(m.group(1))


def param_mem(s: Session) -> dict:
    """`UI MEM` → {page_id: (state, est, delta)} + 首行字段。"""
    s.cmd("UI MEM")
    head = {}
    m = re.search(r"UI MEM pool_used=(\d+) pool_free=(\d+) pool_total=(\d+) registered=(\d+) "
                  r"built=(\d+) failed=(\d+) sum_est=(\d+) reserve=(-?\d+)", s.last)
    if m is None:
        raise InjectError("UI MEM 首行缺失")
    head = dict(zip(("used", "free", "total", "registered", "built", "failed", "sum_est", "reserve"),
                    map(int, m.groups())))
    pages = {}
    for line in s.last.splitlines():
        pm = re.search(r"UI MEM page id=(\d+) title=(\S+) state=(\w+) est=(\d+) delta=(-?\d+)", line)
        if pm:
            pages[int(pm.group(1))] = (pm.group(3), int(pm.group(4)), int(pm.group(5)))
    return {"head": head, "pages": pages}


def param_expect_value(s: Session, read_cmd: str, want: int, label: str) -> None:
    s.cmd(read_cmd)
    got = param_value(s)
    (s.ok if got == want else s.fail)(f"{label}: {read_cmd} → {got}（期望 {want}）")


def param_tap_step(s: Session, xy: tuple, slot: int, param_id: int, delta: int, want: int,
                   label: str) -> None:
    s.cmd("TAP %d %d" % xy)
    s.expect(rf"-> PARAM slot={slot} id={param_id} delta=[+-]1 value={want}", label)


def run_params(s: Session) -> None:
    """④ 参数页：GLOBAL/FX/ANALOG 构建 + TAP ± / 键盘编辑生效 + 池水位 + 截屏。"""
    # 归一到轨 0（三页都按选中轨显示；TRACKS 卡 0 = x=24+148, y=72+40+235）
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "UI tracks（归一选轨）")
    s.cmd("TAP 172 347")
    s.expect(r"-> TRACKS select track=0 CMD_SELECT_TRACK", "TAP 卡 0")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 0, "STATE SNAP selected_track=0")

    mem0 = param_mem(s)
    (s.ok if mem0["head"]["registered"] == 7 else s.fail)(
        f"UI MEM registered=7（4 基础 + 3 参数页）实得 {mem0['head']['registered']}")

    # ---------- GLOBAL 页：BPM/SWING 经 TAP ± 与键盘 ↑/↓ ----------
    s.cmd("UI global")
    s.expect(r"UI page=global \(4\)", "UI global")
    s.cmd("PARAM GLOBAL 0")
    bpm0 = param_value(s)
    s.cmd("PARAM GLOBAL 1")
    swing0 = param_value(s)
    s.ser.note(f"GLOBAL 基线 bpm={bpm0} swing={swing0}")

    # 点旋钮 0 设焦点（与物理按下同一路径），再用 ± 细调
    s.cmd("TAP %d %d" % param_knob_xy(0))
    s.expect(r"-> PARAM slot=0 focus", "TAP 旋钮 0 设焦点")
    if bpm0 < 240:
        param_tap_step(s, PARAM_BTN_PLUS, 0, 0, +1, bpm0 + 1, "TAP +（GLOBAL BPM）")
        param_expect_value(s, "PARAM GLOBAL 0", bpm0 + 1, "PARAM GLOBAL 0（+1）")
        s.cmd("STATE SNAP")
        s.expect_int(r"\bbpm=(\d+)", bpm0 + 1, "STATE SNAP bpm（+1）")
        param_tap_step(s, PARAM_BTN_MINUS, 0, 0, -1, bpm0, "TAP −（GLOBAL BPM）")
        param_expect_value(s, "PARAM GLOBAL 0", bpm0, "PARAM GLOBAL 0（回到基线）")
    else:  # 240 钳位态：先减再加
        param_tap_step(s, PARAM_BTN_MINUS, 0, 0, -1, bpm0 - 1, "TAP −（BPM 240 钳位态）")
        param_tap_step(s, PARAM_BTN_PLUS, 0, 0, +1, bpm0, "TAP +（回到 240）")

    # 键盘：→ 焦点到槽 1（SWING），↑ 两次 +2、↓ 两次回基线
    s.cmd("KEY 0x4C down")
    s.cmd("KEY 0x4C up")
    for _ in range(2):
        s.cmd("KEY 0x3B down")
        s.cmd("KEY 0x3B up")
    param_expect_value(s, "PARAM GLOBAL 1", min(swing0 + 2, 100), "键盘 ↑×2（SWING）")
    for _ in range(2):
        s.cmd("KEY 0x4B down")
        s.cmd("KEY 0x4B up")
    param_expect_value(s, "PARAM GLOBAL 1", swing0, "键盘 ↓×2（SWING 回基线）")

    img = s.capture("params-global")
    s.probe(img, "页签选中（GLOBAL）", *C_TAB_GLOBAL, C_YELLOW)
    s.probe(img, "PLAY 页签未选中", *C_TAB_PLAY, C_PANEL_HI, 10)
    s.probe(img, "旋钮 0 弧线底（环顶）", 166, 158, C_PANEL_HI, 10)
    s.probe(img, "− 按钮底", 1100, 78, C_PANEL_HI, 10)
    s.probe(img, "+ 按钮底", 1258, 78, C_PANEL_HI, 10)
    # 焦点在槽 1（上面的键盘用例），旋钮 0 的数值文本是 UI_TEXT_DIM
    s.probe_region(img, "旋钮 0 数值文本（BPM 值）",
                   (166 - 70 + 14, 230 - 80 + 64, 166 - 70 + 146, 230 - 80 + 96), C_TEXT_DIM, 16, 40)

    mem1 = param_mem(s)
    for pid, title in ((4, "GLOBAL"), (5, "FX"), (6, "ANALOG")):
        st, est, delta = mem1["pages"].get(pid, ("?", 0, 0))
        if pid == 4:
            (s.ok if st == "built" and 0 < delta <= est else s.fail)(
                f"UI MEM {title} state={st} delta={delta} est={est}（0<delta≤est）")

    # ---------- FX 页：LEVEL（F_DB）经 TAP ±，ATK（F_TIME）经键盘 ----------
    s.cmd("UI fx")
    s.expect(r"UI page=fx \(5\)", "UI fx")
    s.cmd("PARAM 0")
    level0 = param_value(s)
    s.cmd("PARAM 1")
    atk0 = param_value(s)
    s.ser.note(f"FX 基线 level={level0} atk={atk0}")

    # 槽 5 = LEVEL（行 1 列 1）：点旋钮设焦点后 ± 细调（0..127 钳位）
    s.cmd("TAP %d %d" % param_knob_xy(5))
    s.expect(r"-> PARAM slot=5 focus", "TAP 旋钮 5（LEVEL）设焦点")
    if level0 < 127:
        param_tap_step(s, PARAM_BTN_PLUS, 5, 0, +1, level0 + 1, "TAP +（FX LEVEL）")
        s.cmd("STATE SNAP")
        s.expect_int(r"param trk=0 .*?\blevel=(\d+)", level0 + 1, "STATE SNAP param level（+1）")
        param_expect_value(s, "PARAM 0", level0 + 1, "PARAM 0（LEVEL +1）")
        param_tap_step(s, PARAM_BTN_MINUS, 5, 0, -1, level0, "TAP −（FX LEVEL）")
        param_expect_value(s, "PARAM 0", level0, "PARAM 0（LEVEL 回基线）")
    else:
        s.ser.note("FX 基线 LEVEL=127（钳位态），跳过 +/− 断言")
        s.ok("FX LEVEL 已在上限（127），钳位态不注入")

    # 槽 7 = ATK（行 1 列 3）：键盘 ↑ 一次
    s.cmd("TAP %d %d" % param_knob_xy(7))
    s.expect(r"-> PARAM slot=7 focus", "TAP 旋钮 7（ATK）设焦点")
    s.cmd("KEY 0x3B down")
    s.cmd("KEY 0x3B up")
    param_expect_value(s, "PARAM 1", min(atk0 + 1, 127), "键盘 ↑（FX ATK）")
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    param_expect_value(s, "PARAM 1", atk0, "键盘 ↓（FX ATK 回基线）")

    img = s.capture("params-fx")
    s.probe(img, "页签选中（FX）", *C_TAB_FX, C_YELLOW)
    s.probe(img, "旋钮 0 弧线底（环顶）", 166, 158, C_PANEL_HI, 10)
    s.probe_region(img, "旋钮 6 数值文本（PAN）",
                   (param_knob_xy(6)[0] - 66, param_knob_xy(6)[1] - 16,
                    param_knob_xy(6)[0] + 66, param_knob_xy(6)[1] + 16), C_TEXT_DIM, 16, 30)

    # ---------- ANALOG 页：WAVE（F_ENUM）TAP ±，CUT（F_CUTOFF）键盘；切页保值 ----------
    s.cmd("UI analog")
    s.expect(r"UI page=analog \(6\)", "UI analog")
    s.cmd("PARAM 63")
    wave0 = param_value(s)
    s.cmd("PARAM 67")
    cut0 = param_value(s)
    s.ser.note(f"ANALOG 基线 wave={wave0} cut={cut0}")

    s.cmd("TAP %d %d" % param_knob_xy(0))
    s.expect(r"-> PARAM slot=0 focus", "TAP 旋钮 0（WAVE）设焦点")
    if wave0 < 4:
        param_tap_step(s, PARAM_BTN_PLUS, 0, 63, +1, wave0 + 1, "TAP +（ANALOG WAVE）")
        param_expect_value(s, "PARAM 63", wave0 + 1, "PARAM 63（WAVE +1）")
        param_tap_step(s, PARAM_BTN_MINUS, 0, 63, -1, wave0, "TAP −（ANALOG WAVE）")
        param_expect_value(s, "PARAM 63", wave0, "PARAM 63（WAVE 回基线）")
    else:
        s.ser.note("ANALOG 基线 WAVE=4（钳位态）")

    # 键盘：→×4 到槽 4（CUT），↑ 一次、↓ 一次
    for _ in range(4):
        s.cmd("KEY 0x4C down")
        s.cmd("KEY 0x4C up")
    s.cmd("KEY 0x3B down")
    s.cmd("KEY 0x3B up")
    param_expect_value(s, "PARAM 67", cut0 + 1, "键盘 ↑（ANALOG CUT）")
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    param_expect_value(s, "PARAM 67", cut0, "键盘 ↓（ANALOG CUT 回基线）")

    # 切页保值：设 CUT=cut0+1 → 切 PLAY → 切回 → 引擎真值仍在
    if cut0 < 127:
        s.cmd("KEY 0x3B down")
        s.cmd("KEY 0x3B up")
        param_expect_value(s, "PARAM 67", cut0 + 1, "ANALOG CUT 设为 cut0+1")
        s.cmd("UI play")
        s.expect(r"UI page=play \(0\)", "切 PLAY")
        s.cmd("UI analog")
        s.expect(r"UI page=analog \(6\)", "切回 ANALOG")
        param_expect_value(s, "PARAM 67", cut0 + 1, "切页往返后 CUT 保持（引擎真值重读）")
        s.cmd("KEY 0x4B down")
        s.cmd("KEY 0x4B up")
        param_expect_value(s, "PARAM 67", cut0, "ANALOG CUT 回基线")

    img = s.capture("params-analog")
    s.probe(img, "页签选中（ANALOG）", *C_TAB_ANALOG, C_YELLOW)
    s.probe(img, "旋钮 0 弧线底（环顶）", 166, 158, C_PANEL_HI, 10)
    s.probe(img, "旋钮 4 弧线底（行 1 环顶）", 166, 454, C_PANEL_HI, 10)
    # 焦点在槽 4（CUT），旋钮 0 的 WAVE 枚举名 + 值为 UI_TEXT_DIM
    s.probe_region(img, "旋钮 0 数值文本（WAVE 枚举名）",
                   (100, 214, 232, 246), C_TEXT_DIM, 16, 30)

    # ---------- 池水位总表：三页 delta ≤ est，全 7 页可建 ----------
    mem2 = param_mem(s)
    s.cmd("UI play")
    s.cmd("UI transport")
    s.cmd("UI step")
    s.cmd("UI fx")
    s.cmd("UI global")
    s.cmd("UI analog")
    mem3 = param_mem(s)
    (s.ok if mem3["head"]["built"] == 7 else s.fail)(
        f"UI MEM built={mem3['head']['built']}（7 页全建；free={mem3['head']['free']}）")
    for pid, title in ((4, "GLOBAL"), (5, "FX"), (6, "ANALOG")):
        st, est, delta = mem2["pages"].get(pid, ("?", 0, 0))
        (s.ok if st == "built" and 0 < delta <= est else s.fail)(
            f"UI MEM {title} state={st} delta={delta} est={est}（0<delta≤est）")

    # 复位：PLAY 页、轨 0、参数回基线（脚本可重复运行）
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "复位 UI play")


SCENARIOS = {
    "transport": run_transport,
    "params": run_params,
    "step_edit": run_step_edit,
    "tracks": run_tracks,
    "nav": run_nav,
}


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口注入序列（AGENTS.md §5.4）")
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS),
                    help="注入场景（四条 .sh 各对应一个）")
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
