#!/usr/bin/env python3
"""inject.py — Tab5 固件串口注入序列（AGENTS.md §5.4 注入约定）

六条场景脚本（transport.sh / step_edit.sh / tracks.sh / nav.sh / params.sh / store.sh）
是本文的薄封装。每个场景：
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
        try:
            return LoggedSerial(serial.Serial(self.port, BAUD, timeout=0.05), self.log_path)
        except (serial.SerialException, OSError) as e:
            # 重试耗尽后的重开失败（端口被占/设备已掉）要让上层按 InjectError 收口，
            # 不能把 pyserial 异常漏成 traceback（脚本断言/退出码口径一致）。
            raise InjectError(f"打开串口 {self.port} 失败: {e}") from None

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

    # ---- 复位（断电语义） ----
    def reboot(self) -> None:
        """esptool 硬复位并等 console 就绪（只读 flash_id，不写 flash）：验证开机路径
        （如 RAM 槽的开机 fetch）用。要赶在 init 之前发命令时不用它——见
        store_early_load_probe（wait_boot 会把命令推迟到 init 之后）。"""
        self.ser.note("复位设备（esptool flash_id --after hard_reset，不写 flash）")
        self._reset_and_reopen()

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
# 页签条几何随注册页数收窄（ADR-008）：9 页（4 基础 + 5 参数页）时 tab_w = 107、
# 间距 8 → 页签 i 的 x 区间 = 16 + i*115 .. +107。探针取页签**左缘内 8px** 的底色
# （y=20，文本行 ~26..46 之上）：9 页时 107px 只比 "TRANSPORT" 标签宽 5px，y=36
# 会落在字形笔画/抗锯齿上（终审轮实测 (369,36) 命中 T 的竖笔），故探针行上移到
# 文本之上；4 页布局时用过的中心点同理。
C_TAB_PLAY = (24, 20)
C_TAB_TRACKS = (139, 20)
C_TAB_STEP = (254, 20)
C_TAB_TRANSPORT = (369, 20)
C_TAB_GLOBAL = (484, 20)
C_TAB_FX = (599, 20)
C_TAB_ANALOG = (714, 20)
C_TAB_FM6 = (829, 20)
C_TAB_SAMPLE = (944, 20)
C_ERROR = (224, 84, 40)  # UI_ERROR 0xE0542A → rgb565 (224,84,40)：锁定页页眉红字


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
# 页面注册表 = 9 页（PLAY/TRACKS/STEP/TRANSPORT/GLOBAL/FX/ANALOG/FM6/SAMPLE）时
# ui.c 的页签宽度：min(UI_TAB_W=176, (1280 - 2*16 - 220(右侧留位) - 8*8) / 9) = 107。
UI_TAB_X0, UI_TAB_W, UI_TAB_GAP = 16, 107, 8
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


def knob_cmd(s: Session, slot: int, op: str, dx: int, dy: int, want: int = None) -> None:
    """`UI KNOB <slot> <press|drag> <dx> <dy>`：合成指针事件（物理触摸同一路径）。
    want=None 断言环带外被忽略（`ignored (ring band)`）；否则断言回显 value=want。"""
    s.cmd(f"UI KNOB {slot} {op} {dx} {dy}")
    if want is None:
        s.expect(r"ignored \(ring band\)", f"UI KNOB slot={slot} {op} ({dx},{dy}) 环带外")
        return
    s.expect(rf"UI KNOB page=\d+ slot={slot} {op} dx={dx} dy={dy} value={want}",
             f"UI KNOB slot={slot} {op} ({dx},{dy}) value={want}")


def knob_locked(s: Session, slot: int, op: str, dx: int, dy: int) -> None:
    """引擎页不适用（选中轨引擎与页声明不符）：旋钮事件被拒，令牌 `ignored (locked)`
    ——与环带外的 `ignored (ring band)` 区分（ADR-008 §8.2）。"""
    s.cmd(f"UI KNOB {slot} {op} {dx} {dy}")
    s.expect(rf"UI KNOB page=\d+ slot={slot} {op} dx={dx} dy={dy} ignored \(locked\)",
             f"UI KNOB slot={slot} {op} ({dx},{dy}) 引擎页锁定")


def select_track(s: Session, track: int) -> None:
    """TRACKS 页点卡 k（卡中心 x = 24 + k*312 + 148，y = 347）并断言选中轨。"""
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "UI tracks（选轨）")
    s.cmd("TAP %d 347" % (24 + track * TRACKS_CARD_PITCH + 148))
    s.expect(rf"-> TRACKS select track={track} CMD_SELECT_TRACK", f"TAP 卡 {track}")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", track, f"STATE SNAP selected_track={track}")


def param_tap_step(s: Session, xy: tuple, slot: int, param_id: int, delta: int, want: int,
                   label: str) -> None:
    s.cmd("TAP %d %d" % xy)
    s.expect(rf"-> PARAM slot={slot} id={param_id} delta=[+-]1 value={want}", label)


# GLOBAL 页「旋钮 0 弧线底（环顶）」探针按**工厂 BPM 的弧长**设计（环顶 12 点方向必须是面板
# 底色）。BPM 高到一定程度后弧线会盖住那一点：同一个探针在 bpm=90/137 绿、140 红（实测）。
# `STORE` 场景的最后一次自动存档是 bpm=140（⑪f），复位后 autosave_resume 会把它带进新会话
# ——故本场景先把 BPM 归一到 TP 默认值（与 ENG_LEVEL_DEF 同一口径），否则同一条命令会随
# 上一场景的状态时红时绿。
PARAM_BPM_DEF = 90


def run_params(s: Session) -> None:
    """④ 参数页：GLOBAL/FX/ANALOG 构建 + TAP ± / 键盘编辑生效 + 池水位 + 截屏。"""
    # 归一到轨 0（三页都按选中轨显示；TRACKS 卡 0 = x=24+148, y=72+40+235）
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "UI tracks（归一选轨）")
    s.cmd("TAP 172 347")
    s.expect(r"-> TRACKS select track=0 CMD_SELECT_TRACK", "TAP 卡 0")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 0, "STATE SNAP selected_track=0")
    # BPM 回到 TP 默认（截图探针的弧长前提，见 PARAM_BPM_DEF 的注释）
    s.cmd(f"PARAM GLOBAL 0 {PARAM_BPM_DEF}")
    param_expect_value(s, "PARAM GLOBAL 0", PARAM_BPM_DEF,
                       f"归一 GLOBAL BPM={PARAM_BPM_DEF}（环顶探针的弧长前提）")

    mem0 = param_mem(s)
    (s.ok if mem0["head"]["registered"] == 9 else s.fail)(
        f"UI MEM registered=9（4 基础 + 5 参数页）实得 {mem0['head']['registered']}")

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

    # ---------- 物理触摸路径（合成事件）：按下不改值，相对拖拽改值 ----------
    # BPM range 200：+90° = +67（270° 扫角 = 整个值域）。方向按基线取不撞钳位的一侧。
    drag = 1 if bpm0 + 67 <= 240 else -1
    drag_dx = 73 * drag
    want_after = bpm0 + 67 * drag
    knob_cmd(s, 0, "press", 0, -73, bpm0)  # 按下只设焦点 + 锚点，不改值
    param_expect_value(s, "PARAM GLOBAL 0", bpm0, "按下不改值（PARAM GLOBAL 0）")
    knob_cmd(s, 0, "drag", drag_dx, 0, want_after)  # +90°/-90° → +67/-67
    param_expect_value(s, "PARAM GLOBAL 0", want_after, "相对拖拽改值（引擎真值）")
    s.cmd("STATE SNAP")
    s.expect_int(r"\bbpm=(\d+)", want_after, "STATE SNAP bpm（拖拽后）")
    knob_cmd(s, 0, "drag", 0, -73, bpm0)  # 拖回锚点角度 → 回基线
    param_expect_value(s, "PARAM GLOBAL 0", bpm0, "拖回锚点（值回基线）")
    # 环带外：中心按下不建锚点、内圈拖动被忽略（不跳值）
    knob_cmd(s, 0, "press", 0, 0)
    knob_cmd(s, 0, "drag", 73, 0)
    param_expect_value(s, "PARAM GLOBAL 0", bpm0, "中心按下后拖拽被忽略（值不变）")
    knob_cmd(s, 0, "press", 0, -73, bpm0)
    knob_cmd(s, 0, "drag", 0, -40)  # 半径 40 < 55：环带外
    param_expect_value(s, "PARAM GLOBAL 0", bpm0, "环带外拖动被忽略（值不变）")
    # 焦点回到 SWING（截图探针按非焦点文本色断言）
    s.cmd("TAP %d %d" % param_knob_xy(1))
    s.expect(r"-> PARAM slot=1 focus", "TAP 旋钮 1 复位焦点")

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

    # ---------- 引擎页锁定路径：UI KNOB 独立令牌（选中轨仍是 ANALOG） ----------
    s.cmd("UI fm6")
    s.expect(r"UI page=fm6 \(7\)", "UI fm6（轨 0 是 ANALOG → 页锁定）")
    knob_locked(s, 0, "press", 0, -73)
    knob_locked(s, 0, "drag", 73, 0)
    s.cmd("TAP %d %d" % PARAM_BTN_PLUS)
    s.expect(r"-> PARAM slot=\d+ locked", "FM6 锁定页 TAP + 回 locked")
    s.cmd("PARAM 63")
    alg_locked = param_value(s)
    s.ser.note(f"FM6 锁定页基线 alg（ANALOG WAVE 槽位）= {alg_locked}")
    img = s.capture("params-fm6-locked")
    s.probe_region(img, "锁定页页眉红字（FM6 · TRACK 1 USES ENG 0 — N/A）",
                   (16, 72 + 6, 760, 72 + 40), C_ERROR, 16, 200)

    # ---------- FM6 页（ENGINES[9]->edit[0..7]）：ALG 33 名、宏编辑生效 ----------
    s.cmd("ENG TRACK 1 9 0")
    s.expect(r"ENG TRACK k=1 engine=9 preset=0 OK", "ENG TRACK 轨 1 → FM6")
    select_track(s, 1)
    s.cmd("UI fm6")
    s.expect(r"UI page=fm6 \(7\)", "UI fm6（FM6 页解锁）")
    s.cmd("ENG DESC engine 9 0")
    s.expect(r'label="ALG" fmt=\d+ min=0 max=32 def=0 unit="[^"]*" enums=33 first="PAT" last="32"',
             "ENG DESC FM6 ALG：33 名（PAT..32）")
    s.cmd("PARAM 63")
    alg0 = param_value(s)
    s.cmd("PARAM 64")
    fb0 = param_value(s)
    s.ser.note(f"FM6 基线 alg={alg0} fb={fb0}")

    # 焦点槽 0（ALG）：名称用 UI_ACCENT、数值用 UI_TEXT——先 TAP 定型再截屏
    s.cmd("TAP %d %d" % param_knob_xy(0))
    s.expect(r"-> PARAM slot=0 focus", "TAP 旋钮 0（ALG）设焦点")
    img = s.capture("params-fm6")
    s.probe(img, "页签选中（FM6）", *C_TAB_FM6, C_YELLOW)
    s.probe(img, "PLAY 页签未选中", *C_TAB_PLAY, C_PANEL_HI, 10)
    s.probe(img, "旋钮 0 弧线底（环顶）", 166, 158, C_PANEL_HI, 10)
    s.probe_region(img, "旋钮 0 数值文本（ALG 枚举名）", (100, 214, 232, 246), C_TEXT, 16, 30)

    # TAP ± 细调（ALG F_INT 0..32）
    if alg0 < 32:
        param_tap_step(s, PARAM_BTN_PLUS, 0, 63, +1, alg0 + 1, "TAP +（FM6 ALG）")
        param_expect_value(s, "PARAM 63", alg0 + 1, "PARAM 63（ALG +1）")
        param_tap_step(s, PARAM_BTN_MINUS, 0, 63, -1, alg0, "TAP −（FM6 ALG 回基线）")
        param_expect_value(s, "PARAM 63", alg0, "PARAM 63（ALG 回基线）")
    else:
        s.ser.note("FM6 基线 ALG=32（钳位态），跳过 +/− 断言")

    # 触摸路径：按下不改值；+90°（270°=整值域）→ +11（range 33）
    drag = 1 if alg0 <= 21 else -1
    knob_cmd(s, 0, "press", 0, -73, alg0)
    param_expect_value(s, "PARAM 63", alg0, "按下不改值（FM6 ALG）")
    knob_cmd(s, 0, "drag", 73 * drag, 0, alg0 + 11 * drag)
    param_expect_value(s, "PARAM 63", alg0 + 11 * drag, "相对拖拽改 ALG（引擎真值）")
    knob_cmd(s, 0, "drag", 0, -73, alg0)
    param_expect_value(s, "PARAM 63", alg0, "拖回锚点（ALG 回基线）")

    # 键盘：→ 到槽 1（FB，-7..7），↑/↓ ±1
    s.cmd("KEY 0x4C down")
    s.cmd("KEY 0x4C up")
    s.cmd("KEY 0x3B down")
    s.cmd("KEY 0x3B up")
    param_expect_value(s, "PARAM 64", min(fb0 + 1, 7), "键盘 ↑（FM6 FB）")
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    param_expect_value(s, "PARAM 64", fb0, "键盘 ↓（FM6 FB 回基线）")

    # 改值态截屏：ALG=12（枚举名 "12"）；焦点回槽 0（键盘用例把它移到了槽 1）
    s.cmd("TAP %d %d" % param_knob_xy(0))
    s.expect(r"-> PARAM slot=0 focus", "TAP 旋钮 0 复位焦点（改值态截屏）")
    s.cmd("PARAM 63 12")
    s.expect(r"PARAM track=1 id=63 value=12 applied=12", "PARAM 63 12（FM6 ALG 改值态）")
    img = s.capture("params-fm6-alg")
    s.probe(img, "页签选中（FM6，改值态）", *C_TAB_FM6, C_YELLOW)
    s.probe(img, "旋钮 0 弧线底（改值态）", 166, 158, C_PANEL_HI, 10)
    s.probe_region(img, "旋钮 0 数值文本（ALG=12）", (100, 214, 232, 246), C_TEXT, 16, 30)
    s.cmd(f"PARAM 63 {alg0}")
    s.expect(rf"PARAM track=1 id=63 value={alg0} applied={alg0}", "FM6 ALG 回基线")

    # ---------- SAMPLE 页（ENGINES[4]->edit[0..7]，空槽 5/7 不建旋钮） ----------
    s.cmd("ENG TRACK 1 4 0")
    s.expect(r"ENG TRACK k=1 engine=4 preset=0 OK", "ENG TRACK 轨 1 → SAMPLE")
    s.cmd("UI sample")
    s.expect(r"UI page=sample \(8\)", "UI sample（SAMPLE 页解锁）")
    s.cmd("ENG DESC engine 4 0")
    s.expect(r'label="SET" fmt=\d+ min=0 max=11 def=0 unit="[^"]*" enums=12 first="PIANO" last="USR4"',
             "ENG DESC SAMPLE SET：12 名（PIANO..USR4）")
    s.cmd("PARAM 63")
    set0 = param_value(s)
    s.cmd("PARAM 66")
    loop0 = param_value(s)
    s.cmd("PARAM 67")
    cut0 = param_value(s)
    s.ser.note(f"SAMPLE 基线 set={set0} loop={loop0} cut={cut0}")

    img = s.capture("params-sample")
    s.probe(img, "页签选中（SAMPLE）", *C_TAB_SAMPLE, C_YELLOW)
    s.probe(img, "旋钮 0 弧线底（环顶）", 166, 158, C_PANEL_HI, 10)
    # 空槽 5/7 没有旋钮：其环顶像素应是页面背景（UI_BG）
    s.probe(img, "空槽 5（行 1 列 1）无旋钮", 482, 454, C_BG, 12)
    s.probe(img, "空槽 7（行 1 列 3）无旋钮", 1114, 454, C_BG, 12)

    # SET（F_ENUM 12 名）：TAP + / −
    s.cmd("TAP %d %d" % param_knob_xy(0))
    s.expect(r"-> PARAM slot=0 focus", "TAP 旋钮 0（SET）设焦点")
    if set0 < 11:
        param_tap_step(s, PARAM_BTN_PLUS, 0, 63, +1, set0 + 1, "TAP +（SAMPLE SET）")
        param_expect_value(s, "PARAM 63", set0 + 1, "PARAM 63（SET +1）")
        param_tap_step(s, PARAM_BTN_MINUS, 0, 63, -1, set0, "TAP −（SAMPLE SET 回基线）")
        param_expect_value(s, "PARAM 63", set0, "PARAM 63（SET 回基线）")
    else:
        s.ser.note("SAMPLE 基线 SET=11（钳位态），跳过 +/− 断言")

    # LOOP（槽 3，F_ENUM 0..1）
    s.cmd("TAP %d %d" % param_knob_xy(3))
    s.expect(r"-> PARAM slot=3 focus", "TAP 旋钮 3（LOOP）设焦点")
    if loop0 == 0:
        param_tap_step(s, PARAM_BTN_PLUS, 3, 66, +1, 1, "TAP +（SAMPLE LOOP）")
        param_expect_value(s, "PARAM 66", 1, "PARAM 66（LOOP=1）")
        param_tap_step(s, PARAM_BTN_MINUS, 3, 66, -1, 0, "TAP −（LOOP 回基线）")
        param_expect_value(s, "PARAM 66", 0, "PARAM 66（LOOP 回基线）")
    else:
        param_tap_step(s, PARAM_BTN_MINUS, 3, 66, -1, 0, "TAP −（LOOP=1 → 0）")
        param_expect_value(s, "PARAM 66", 0, "PARAM 66（LOOP=0）")
        param_tap_step(s, PARAM_BTN_PLUS, 3, 66, +1, 1, "TAP +（LOOP 回基线）")
        param_expect_value(s, "PARAM 66", 1, "PARAM 66（LOOP=1）")

    # CUT（槽 4，F_INT 0..127）：TAP +1 + 触摸拖拽
    s.cmd("TAP %d %d" % param_knob_xy(4))
    s.expect(r"-> PARAM slot=4 focus", "TAP 旋钮 4（CUT）设焦点")
    if cut0 < 127:
        param_tap_step(s, PARAM_BTN_PLUS, 4, 67, +1, cut0 + 1, "TAP +（SAMPLE CUT）")
        param_expect_value(s, "PARAM 67", cut0 + 1, "PARAM 67（CUT +1）")
        knob_cmd(s, 4, "press", 0, -73, cut0 + 1)
        knob_cmd(s, 4, "drag", 73, 0, cut0 + 43)  # 127 range：+90° → +42.33 → 43（四舍五入）
        param_expect_value(s, "PARAM 67", cut0 + 43, "相对拖拽改 CUT（引擎真值）")
        knob_cmd(s, 4, "drag", 0, -73, cut0 + 1)
        param_tap_step(s, PARAM_BTN_MINUS, 4, 67, -1, cut0, "TAP −（CUT 回基线）")
        param_expect_value(s, "PARAM 67", cut0, "PARAM 67（CUT 回基线）")
    else:
        s.ser.note("SAMPLE 基线 CUT=127（钳位态），跳过编辑断言")

    # 改值态截屏：SET=1（枚举名 "BASS"）
    s.cmd("PARAM 63 1")
    s.expect(r"PARAM track=1 id=63 value=1 applied=1", "PARAM 63 1（SAMPLE SET 改值态）")
    img = s.capture("params-sample-set")
    s.probe(img, "页签选中（SAMPLE，改值态）", *C_TAB_SAMPLE, C_YELLOW)
    s.probe(img, "旋钮 0 弧线底（改值态）", 166, 158, C_PANEL_HI, 10)
    s.probe_region(img, "旋钮 0 数值文本（SET 枚举名）", (100, 214, 232, 246), C_TEXT_DIM, 16, 30)
    s.cmd(f"PARAM 63 {set0}")
    s.expect(rf"PARAM track=1 id=63 value={set0} applied={set0}", "SAMPLE SET 回基线")

    # 复位引擎/轨：轨 1 回 ANALOG（共享 P_E* 槽位先归零），再选回轨 0
    for pid in (63, 64, 66, 67):
        s.cmd(f"PARAM {pid} 0")
    s.cmd("ENG TRACK 1 0 0")
    s.expect(r"ENG TRACK k=1 engine=0 preset=0 OK", "ENG TRACK 轨 1 → ANALOG（复位）")
    select_track(s, 0)

    # ---------- 池水位总表：五页 delta ≤ est，全 9 页可建 ----------
    mem2 = param_mem(s)
    s.cmd("UI play")
    s.cmd("UI transport")
    s.cmd("UI step")
    s.cmd("UI fx")
    s.cmd("UI global")
    s.cmd("UI analog")
    s.cmd("UI fm6")
    s.cmd("UI sample")
    mem3 = param_mem(s)
    (s.ok if mem3["head"]["built"] == 9 else s.fail)(
        f"UI MEM built={mem3['head']['built']}（9 页全建；free={mem3['head']['free']}）")
    # 真不变量是「九页全建后 pool_free > 0」；reserve = total - sum_est 只是展示值
    # （按 63,120B 池与 7,016B 基础占用算真实账面为负，见 ADR-008 §5），不得当门禁。
    (s.ok if mem3["head"]["free"] > 0 else s.fail)(
        f"UI MEM pool_free={mem3['head']['free']}（九页全建后 > 0，ADR-008 §5）")
    for pid, title in ((4, "GLOBAL"), (5, "FX"), (6, "ANALOG"), (7, "FM6"), (8, "SAMPLE")):
        st, est, delta = mem2["pages"].get(pid, ("?", 0, 0))
        (s.ok if st == "built" and 0 < delta <= est else s.fail)(
            f"UI MEM {title} state={st} delta={delta} est={est}（0<delta≤est）")

    # 复位：PLAY 页、轨 0（脚本可重复运行）
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "复位 UI play")


# ---------------------------------------------------------------- STORE ---
# 存储门禁（ADR-009 §5/§9）：工程 A–D 的「RAM 槽（PSRAM）+ flash」双层往返，以及
# 开机 fetch。console 契约（main/console.c 的 STORE 命令族）：
#   STORE STATS                 -> "STORE STATS ready=… base=… span=… map=… objects=… faults=… armed=…"
#   STORE PROJ LIST             -> 每槽一行 "PROJ n EMPTY" / "PROJ n USED hash=XXXXXXXX"
#   STORE PROJ SAVE|LOAD|ERASE n-> "PROJ n SAVED|LOADED hash=XXXXXXXX" / "PROJ n ERASED"
#   STORE PROJ HASH n           -> "PROJ n HASH=XXXXXXXX"（只读 RAM 槽；空槽回 STORE ERR empty）
#   STORE LIVEHASH              -> "LIVEHASH XXXXXXXX"（当前引擎状态的捕获 hash）
#   错误统一 "STORE ERR <no_storage|playing|empty|invalid|save>"
STORE_HASH8 = r"([0-9A-Fa-f]{8})"
# 热复位早探的重对齐次数：主机侧可能晚于 init 才读到提示符（不是固件失败）
STORE_WARM_ATTEMPTS = 5
# 用户预置（upreset.c）：32 槽 / 2 个银行对象（0xDC000..0xDFFFF，A/B 扇区对）。
#   STORE PRESET LIST            -> 32 行 "PRESET k USED <ENGINE> <NAME>" / "PRESET k EMPTY" + "PRESET COUNT used=n total=32"
#   STORE PRESET SAVE n          -> "PRESET n SAVED <ENGINE> <NAME>"（当前选中轨 → 槽）
#   STORE PRESET LOAD n [track]  -> "PRESET n LOADED track=t"（缺省 track = STATE SNAP 的选中轨）
#   STORE PRESET ERASE n         -> "PRESET n ERASED"
#   错误令牌：no_storage（未就绪）/ playing（走带中）/ empty（空槽）/ invalid（槽或轨号越界）/
#   save（擦写或回读校验失败）/ drum（鼓轨没有声音可存可载）
STORE_PRESET_SLOTS = 32
# 记录里的 8 个「声音」参数：P_ATK..P_ED_FX（id 1..8）。都不在上游 param_kept 里
# （载入预置会覆盖），且 ANALOG 轨默认值 ≠ 下面两组 ⇒ SAVE/LOAD 的往返可判读。
PRESET_PARAM_IDS = (1, 2, 3, 4, 5, 6, 7, 8)
PRESET_PARAM_A = {1: 20, 2: 40, 3: 60, 4: 70, 5: -10, 6: 15, 7: -25, 8: 8}
PRESET_PARAM_B = {1: 100, 2: 110, 3: 20, 4: 30, 5: 30, 6: -30, 7: 20, 8: -8}


def store_expect(s: Session, pattern: str, label: str) -> re.Match:
    """断言当前回显匹配 pattern；不匹配抛 InjectError（固件缺命令/回显改名时不得
    静默通过，RED 阶段就是这条路径先炸）。"""
    m = re.search(pattern, s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"{label}: 回显缺少 /{pattern}/（响应 {s.last.strip()!r}）")
    s.ok(label)
    return m


def store_cmd(s: Session, cmd: str, pattern: str, label: str = "") -> re.Match:
    s.cmd(cmd)
    return store_expect(s, pattern, label or cmd)


def store_hash(s: Session, cmd: str, pattern: str, label: str = "") -> int:
    return int(store_cmd(s, cmd, pattern, label).group(1), 16)


def store_bpm(s: Session) -> int:
    s.cmd("STATE SNAP")
    return s.snap_int(r"STATE SNAP global bpm=(-?\d+)")


def store_bpm_settled(s: Session, want: int, timeout: float = 1.0, step: float = 0.05) -> bool:
    """`PROJ LOAD` 之后等 AppState 的 g[] 投影对齐：LOAD 直接写引擎，`STATE SNAP` 读的是
    状态层每 30ms 同步一次的投影——不能假定命令返回时已经过了下一拍。轮询到 want 或超时
    （超时返回 False，由调用方判失败）。"""
    deadline = time.monotonic() + timeout
    while True:
        got = store_bpm(s)
        if got == want:
            return True
        if time.monotonic() >= deadline:
            print(f"  [NOTE] STATE SNAP bpm 轮询 {timeout:.1f}s 后仍为 {got}（期望 {want}）")
            return False
        time.sleep(step)


def preset_list(s: Session) -> tuple:
    """`STORE PRESET LIST` → ({(slot): (engine, name)}, 已列出的槽号集合, (used, total))。
    枚举/计数行缺失时不报错（由调用方断言），但行数不符会被 `seen` 暴露。"""
    s.cmd("STORE PRESET LIST")
    used, seen = {}, set()
    for raw in s.last.splitlines():
        line = raw.strip()
        m = re.match(r"^PRESET (\d+) USED (\S+) (.+)$", line)
        if m:
            k = int(m.group(1))
            seen.add(k)
            used[k] = (m.group(2), m.group(3))
            continue
        m = re.match(r"^PRESET (\d+) EMPTY$", line)
        if m:
            seen.add(int(m.group(1)))
    m = re.search(r"^PRESET COUNT used=(\d+) total=(\d+)\r?$", s.last, re.MULTILINE)
    count = (int(m.group(1)), int(m.group(2))) if m else None
    return used, seen, count


def preset_params_write(s: Session, values: dict, label: str) -> dict:
    """`PARAM BATCH` 一次写 8 个参数（一次状态层批量命令）；返回**回显的 applied**
    （引擎钳位后的真值，作为后面 LOAD 的期望值）。"""
    pairs = " ".join(f"{pid} {values[pid]}" for pid in PRESET_PARAM_IDS)
    s.cmd(f"PARAM BATCH {pairs}")
    applied = {int(m.group(1)): int(m.group(2)) for m in
               re.finditer(r"id=(\d+) value=-?\d+ applied=(-?\d+)", s.last)}
    if set(applied) != set(PRESET_PARAM_IDS):
        raise InjectError(f"{label}: PARAM BATCH 回显缺参数（{s.last!r}）")
    s.ok(f"{label}: applied={[applied[pid] for pid in PRESET_PARAM_IDS]}")
    return applied


def preset_params_read(s: Session) -> dict:
    """`PARAM <id>` 只读回读 8 个参数的引擎真值（不走状态层投影，无需等 30ms 拍）。"""
    out = {}
    for pid in PRESET_PARAM_IDS:
        s.cmd(f"PARAM {pid}")
        out[pid] = param_value(s, "value")
    return out


# ---- 换引擎/预置 = 换声音（上游 apply_preset_to/set_engine_of 移植，Task 5） ----
# `ENG PRESET <k> <engine> <preset>` 打印 71 行 `P<id>=<v>`：同一「当前 p[] + 引擎 +
# 预置」的**纯写值向量**（无副作用：不改引擎/预置、不触发 panic）。`ENG TRACK` 应用后
# 逐项读回引擎真值（`PARAM <id>` 只读、不经状态层投影）必须与它 71/71 相同——这正是
# 旧 `sloop_core_track` 的缺陷面（只写 eng_req/preset 索引，音频路径读的 p[] 不变）。
ENG_PARAM_COUNT = 71
ENG_P_LEVEL = 0   # P_LEVEL：混音（上游 param_kept，载入声音不覆盖）
ENG_P_ATK = 1     # P_ATK：包络（预置覆盖）
ENG_P_SLEN = 29   # P_SLEN：图案参数（param_kept，载入声音不覆盖）
ENG_P_E0 = 63     # P_E0..P_E7：引擎槽
# 与 ENG SNAP 的默认值同源（TP[] 表：LEVEL 104、LEN 16）；⑩ 收口时还原，后续用例
# 的归一基线不受本节影响。
ENG_LEVEL_DEF = 104
ENG_SLEN_DEF = 16

# ---- 独立 oracle（评审 R17）：硬编码的上游表字面量，不来自固件 ---------------------
# 71/71 的「ENG PRESET 期望向量 vs ENG TRACK 之后的 p[]」两步都走固件的 preset_values，
# 写值实现内部的错会被同时复制到期望与实际两边、门禁全盲。下面两组期望值直接抄自
# vendored 上游表（只读参考，不改 third_party/），用于独立的硬编码判等。
#
# 参数 id = 上游 core.h 的 P_* 枚举（与 ENG DESC/PARAM 同口径）：
#   1 ATK, 2 DEC, 3 SUS, 4 REL, 5 ED_FLT, 6 ED_PIT, 8 ED_FX, 9 LRATE, 16 LD_AMP,
#   28 TRANS, 33 DIST, 34 CHOR, 35 DLY, 36 REV, 37 VOICE, 38 GLIDE, 41 GLMODE,
#   63..70 P_E0..P_E7。V_POLY=0 / V_LEGATO=2（core.h:14）。
#
# A) 引擎 1（DIGITAL）预置 0 "RHODES"——third_party/sloop-fm1/firmware/src/eng_digital.c:117：
#    e[] = {4, 1, 1, 14, 26, 35, 0, 0}；env = {0, 85, 35, 55}；fenv = 0；mono = 0（→ V_POLY）；
#    FX(0, 30, 12, 26) → fx[] 存 +1，写值时 -1 ⇒ DIST/CHOR/DLY/REV = 0/30/12/26；
#    XP(P_LD_AMP+1, 26, P_LRATE+1, 84) ⇒ LD_AMP=26、LRATE=84；
#    P_ED_FX = preset_trim(1, 0) = PRESET_TRIM[1][0] = 23（third_party/.../preset_trim.h）。
DIGITAL_RHODES_ORACLE = {
    63: 4, 64: 1, 65: 1, 66: 14, 67: 26, 68: 35, 69: 0, 70: 0,   # e[]（eng_digital.c:117）
    1: 0, 2: 85, 3: 35, 4: 55,                                    # env[0..3]（同上）
    5: 0, 8: 23, 37: 0,                                          # fenv / PRESET_TRIM[1][0] / mono→V_POLY
    33: 0, 34: 30, 35: 12, 36: 26,                               # FX(0,30,12,26)
    9: 84, 16: 26,                                               # XP(P_LRATE,84, P_LD_AMP,26)
}
# B) 引擎 0（ANALOG）预置 0 "808 BOOM"——third_party/sloop-fm1/firmware/src/eng_analog.c:128：
#    e[] = {3, 0, 0, 0, 127, 0, 48, 0}；env = {0, 112, 0, 52}；fenv = 0；mono = 1（→ V_LEGATO）；
#    FX(0, 0, 0, 0)：表里存 +1（即 1）⇒ 写值时 -1 ⇒ 四个发送都是 0；
#    XP(P_GLIDE+1,59, P_GLMODE+1,1, P_ED_PIT+1,18, P_TRANS+1,-24)；
#    P_ED_FX = preset_trim(0, 0) = PRESET_TRIM[0][0] = -9（third_party/.../preset_trim.h）。
ANALOG_808_ORACLE = {
    63: 3, 64: 0, 65: 0, 66: 0, 67: 127, 68: 0, 69: 48, 70: 0,   # e[]（eng_analog.c:128）
    1: 0, 2: 112, 3: 0, 4: 52,                                   # env[0..3]（同上）
    5: 0, 8: -9, 37: 2,                                          # fenv / PRESET_TRIM[0][0] / mono→V_LEGATO
    33: 0, 34: 0, 35: 0, 36: 0,                                  # FX(0,0,0,0)
    6: 18, 28: -24, 38: 59, 41: 1,                               # XP(P_ED_PIT,18, P_TRANS,-24, P_GLIDE,59, P_GLMODE,1)
}


def eng_param_vector(s: Session) -> dict:
    """选中轨的 71 个参数（引擎真值）。`PARAM <id>` 只读回显，cmd_fast 往返 ~1ms。"""
    out = {}
    for pid in range(ENG_PARAM_COUNT):
        s.cmd_fast(f"PARAM {pid}")
        out[pid] = param_value(s, "value")
    return out


def eng_preset_vector(s: Session, track: int, engine: int, preset: int) -> dict:
    """`ENG PRESET` → 期望向量（71 行 `P<id>=<v>`）。缺行/缺命令直接 InjectError
    （RED 阶段固件还没有该命令，就在这里失败，不得静默通过）。"""
    s.cmd(f"ENG PRESET {track} {engine} {preset}")
    vec = {int(m.group(1)): int(m.group(2))
           for m in re.finditer(r"^P(\d+)=(-?\d+)\r?$", s.last, re.MULTILINE)}
    if set(vec) != set(range(ENG_PARAM_COUNT)):
        raise InjectError(f"ENG PRESET {track} {engine} {preset}: 回显 {len(vec)}/"
                          f"{ENG_PARAM_COUNT} 项（{s.last!r}）")
    return vec


def eng_vector_expect(s: Session, expected: dict, actual: dict, label: str) -> None:
    bad = [(pid, expected[pid], actual[pid]) for pid in sorted(expected)
           if expected[pid] != actual[pid]]
    if bad:
        s.fail(f"{label}: {len(bad)}/{ENG_PARAM_COUNT} 项不符"
               f"（id 期望/实得：{bad[:8]}）")
    else:
        s.ok(f"{label}: {ENG_PARAM_COUNT}/{ENG_PARAM_COUNT} 项一致")


def eng_oracle_expect(s: Session, oracle: dict, actual: dict, label: str) -> None:
    """独立 oracle（评审 R17）：期望值是**硬编码的上游表字面量**，不经过固件的
    `preset_values`/`ENG PRESET`——否则「预览 vs 应用」的 71/71 只是同一份实现自己
    和自己比，写值实现内部的错会被同时复制到两边。每组 oracle 的来源见调用点的注释。"""
    bad = [(pid, want, actual[pid]) for pid, want in sorted(oracle.items())
           if actual[pid] != want]
    if bad:
        s.fail(f"{label}: {len(bad)}/{len(oracle)} 项与上游表不符"
               f"（id 上游/实得：{bad}）")
    else:
        s.ok(f"{label}: {len(oracle)}/{len(oracle)} 项与上游表一致（{sorted(oracle)}）")


def eng_voices_poll(s: Session, want: int, track: int = 0, timeout: float = 1.0) -> int:
    """轮询 `ENG SNAP` 的 trk 行 `voices=<n>`（引擎真值：该轨 active 的话音数）。
    回到 want 或超时即返回当前值（由调用方判定）。"""
    deadline = time.monotonic() + timeout
    while True:
        s.cmd_fast("ENG SNAP")
        m = re.search(rf"^ENG SNAP trk k={track} .* voices=(\d+)\r?$", s.last, re.MULTILINE)
        if m is None:
            raise InjectError(f"ENG SNAP trk k={track} 回显缺 voices=（{s.last!r}）")
        got = int(m.group(1))
        if got == want or time.monotonic() >= deadline:
            return got
        time.sleep(0.02)


def run_store_engine(s: Session) -> None:
    """⑩ 换引擎/预置 = 换声音（`sloop_core_track` 的缺陷：只写索引、p[] 不变）。

    放在 ⓪ 归一之后最先跑：RED 阶段（旧固件无 `ENG PRESET`）快速失败，不拖整套场景。

    ① 归一 `ENG TRACK 0 0 0` → 写混音/图案参数（P_LEVEL=90、P_SLEN=12）→
       `ENG PRESET 0 2 1` 取期望向量 → `ENG TRACK 0 2 1` ⇒ 71/71 一致；且
       P_LEVEL/P_SLEN 保持（上游 param_kept：混音与图案不随声音走）；
    ② 换引擎 `ENG TRACK 0 1 0` ⇒ 71/71 与「引擎 1 预置 0」的期望向量一致，P_E0..P_E7
       取**预置写值**（不是 `ENG DESC` 的 edit[] def：DIGITAL RHODES 的 ALG=4 覆盖 def 0
       ——上游 apply_preset_to 先写默认、再用预置的 e[] 覆盖）；
    ③ 独立 oracle（评审 R17）：上面 71/71 的两边都来自固件的 `preset_values`，实现内部的
       错会被同时复制；故另用**硬编码上游表字面量**判等（DIGITAL RHODES/ANALOG 808 BOOM，
       来源见 `DIGITAL_RHODES_ORACLE`/`ANALOG_808_ORACLE` 的注释）；
    ④ panic：按住一个键（`KEY 0x42 down` = 低八度行 C）⇒ `ENG SNAP` voices≥1；换预置后
       挂音在 ≤1s 内释放（voices=0），`STATE SNAP playing` 不变；
    ⑤ project_load 的两条 fallback（评审 R18，`STORE PROJ SYNTH fmt1|foreign`）：格式 1
       的 `preset == 0xFF` 轨取默认声音 + steps_clear、未知引擎的轨走 `proj_foreign_parts`
       （图案保留）——诊断槽 3，破坏性，收口 ERASE。
    """
    s.ser.note("⑩ 换引擎/预置 = 换声音：ENG PRESET 期望向量 vs ENG TRACK 应用后的 p[]")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⑩ 归一 engine=0 preset=0（ANALOG）")

    # 评审 minor 1：诊断冻结时 `ENG PRESET` 必须明确拒绝，不得打印零向量（否则注入侧
    # 会把「全零」当成合法期望值）。冻结用 `ENG FREEZE on`（与 `ENG SELFTEST` 内部同一
    # 入口；`UI QUIESCE` 只停 UI 定时器/输入，不冻结引擎读写，故测不到这条路径）。
    s.ser.note("⑩0 冻结态 ENG PRESET → ENG PRESET ERR frozen（不打印 P<id>= 行）")
    s.cmd("ENG FREEZE on")
    s.expect(r"^ENG FREEZE on\r?$", "⑩ ENG FREEZE on（诊断冻结）")
    s.cmd("ENG PRESET 0 2 1")
    s.expect(r"^ENG PRESET ERR frozen\r?$", "⑩ 冻结态 ENG PRESET → ERR frozen")
    (s.fail if re.search(r"^P\d+=-?\d+", s.last, re.MULTILINE) else s.ok)(
        "⑩ 冻结态没有打印任何 P<id>=<v>（零向量不得被当成期望）")
    s.cmd("ENG FREEZE off")
    s.expect(r"^ENG FREEZE off\r?$", "⑩ ENG FREEZE off（恢复）")
    s.cmd("ENG PRESET 0 2 1")
    s.expect(r"^ENG PRESET k=0 engine=2 preset=1\r?$", "⑩ 解冻后 ENG PRESET 恢复出向量")

    s.cmd(f"PARAM {ENG_P_LEVEL} 90")
    s.expect(rf"PARAM track=0 id={ENG_P_LEVEL} value=90 applied=90", "⑩ P_LEVEL=90（混音）")
    s.cmd(f"PARAM {ENG_P_SLEN} 12")
    s.expect(rf"PARAM track=0 id={ENG_P_SLEN} value=12 applied=12", "⑩ P_SLEN=12（图案）")

    s.ser.note("⑩a ENG PRESET 0 2 1（期望向量）→ ENG TRACK 0 2 1（应用）→ 71/71")
    expected = eng_preset_vector(s, 0, 2, 1)
    s.ok(f"⑩ ENG PRESET 0 2 1：{len(expected)}/71 项")
    s.cmd("ENG TRACK 0 2 1")
    s.expect(r"ENG TRACK k=0 engine=2 preset=1 OK", "⑩ ENG TRACK 0 2 1")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=2 preset=1 ", "⑩ ENG SNAP engine=2 preset=1")
    actual = eng_param_vector(s)
    eng_vector_expect(s, expected, actual, "⑩ ENG TRACK 0 2 1 的 p[] == ENG PRESET 向量")
    (s.ok if actual[ENG_P_LEVEL] == 90 else s.fail)(
        f"⑩ 换预置后 P_LEVEL={actual[ENG_P_LEVEL]}（混音保留 90）")
    (s.ok if actual[ENG_P_SLEN] == 12 else s.fail)(
        f"⑩ 换预置后 P_SLEN={actual[ENG_P_SLEN]}（图案参数保留 12）")

    s.ser.note("⑩b 换引擎 ENG TRACK 0 1 0（引擎 1 预置 0）→ 71/71")
    expected = eng_preset_vector(s, 0, 1, 0)
    s.cmd("ENG TRACK 0 1 0")
    s.expect(r"ENG TRACK k=0 engine=1 preset=0 OK", "⑩ ENG TRACK 0 1 0（换引擎）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=1 preset=0 ", "⑩ ENG SNAP engine=1 preset=0")
    actual = eng_param_vector(s)
    eng_vector_expect(s, expected, actual, "⑩ 换引擎后的 p[] == ENG PRESET 0 1 0 向量")
    (s.ok if actual[ENG_P_ATK] == expected[ENG_P_ATK] else s.fail)(
        f"⑩ P_ATK={actual[ENG_P_ATK]} == 新引擎预置的 ATK {expected[ENG_P_ATK]}"
        "（包络随声音走）")
    engine_slots = [actual[ENG_P_E0 + i] for i in range(8)]
    expected_slots = [expected[ENG_P_E0 + i] for i in range(8)]
    (s.ok if engine_slots == expected_slots else s.fail)(
        f"⑩ P_E0..P_E7={engine_slots} 由新引擎的预置写值覆盖（期望 {expected_slots}）")
    # R17：独立 oracle（硬编码上游表字面量，不经过 preset_values）——写值实现内部的错
    # 不会被「预览 vs 应用」同时复制，这一组必须炸。
    eng_oracle_expect(s, DIGITAL_RHODES_ORACLE, actual,
                      "⑩ R17 oracle：引擎 1 预置 0 = DIGITAL RHODES（eng_digital.c:117 + preset_trim.h）")

    s.ser.note("⑩b2 独立 oracle 2：ENG TRACK 0 0 0 → ANALOG 808 BOOM（eng_analog.c:128）")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⑩ ENG TRACK 0 0 0（回 ANALOG）")
    actual = eng_param_vector(s)
    eng_oracle_expect(s, ANALOG_808_ORACLE, actual,
                      "⑩ R17 oracle：引擎 0 预置 0 = ANALOG 808 BOOM（eng_analog.c:128 + preset_trim.h）")

    s.ser.note("⑩c panic：按住 C（KEY 0x42 down）→ 换预置 ⇒ 挂音释放、playing 不变")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 preset=0 ", "⑩ panic 基线 engine=0 preset=0")
    s.cmd("ENG AUDIO")
    s.expect(r"ENG AUDIO streaming=1", "⑩ ENG AUDIO streaming=1（panic 由渲染块消费）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d+)", 0, "⑩ STATE SNAP playing=0（基线）")
    s.cmd("KEY 0x42 down")
    s.expect(r"KEY 0x42 down", "⑩ KEY 0x42 down（按住 C）")
    v = eng_voices_poll(s, 1)
    (s.ok if v >= 1 else s.fail)(f"⑩ 按住一个键后 voices={v}（≥1 = 确实在发声）")
    s.cmd("ENG TRACK 0 2 1")
    s.expect(r"ENG TRACK k=0 engine=2 preset=1 OK", "⑩ 发声时换预置 ENG TRACK 0 2 1")
    v = eng_voices_poll(s, 0)
    (s.ok if v == 0 else s.fail)(f"⑩ 换预置后 voices={v}（panic 路径：挂音已释放）")
    s.cmd("KEY 0x42 up")
    s.expect(r"KEY 0x42 up", "⑩ KEY 0x42 up（收口）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d+)", 0, "⑩ STATE SNAP playing=0（换预置不动走带）")

    s.ser.note("⑩d 收口：还原混音/图案参数（后续用例的归一基线不受本节影响）")
    s.cmd(f"PARAM {ENG_P_LEVEL} {ENG_LEVEL_DEF}")
    s.expect(rf"PARAM track=0 id={ENG_P_LEVEL} value={ENG_LEVEL_DEF} applied={ENG_LEVEL_DEF}",
             "⑩ P_LEVEL 回默认")
    s.cmd(f"PARAM {ENG_P_SLEN} {ENG_SLEN_DEF}")
    s.expect(rf"PARAM track=0 id={ENG_P_SLEN} value={ENG_SLEN_DEF} applied={ENG_SLEN_DEF}",
             "⑩ P_SLEN 回默认")

    # R18：project_load 的两条 fallback（真机上原本不可达）经诊断命令 `STORE PROJ SYNTH`
    # 触发。命令以当前引擎状态捕获工程 → 改写出要验的边界 → 存到**诊断槽 3**（破坏性，
    # 槽 0–2 不碰）→ 清 RAM 槽强制 flash 回落（proj_import）→ 公共 LOAD → 回读。
    # 回显每轨 engine/preset/steps/atk/e0；期望值指向上游 project.c 的语义：
    #   ① fmt1：格式 1 的 `preset == 0xFF` 轨取 `TRK_DEF[k][1]` 默认声音 + steps_clear
    #      （project.c:663-667）——工程里的 P_ATK=99/WAVE=99/1 个步都必须被换成
    #      ANALOG 808 BOOM（engine 0 preset 0、atk 0、e0 3）且步清空；
    #   ② foreign：引擎号 ≥ NENGINES 的轨改回默认声音（project.c:639-647 的
    #      proj_foreign_parts，TRK_DEF[1] = DIGITAL/RHODES）——engine 1 preset 0、
    #      atk 0、e0 4（RHODES 的 ALG），而它的图案保留（steps 1）。
    s.ser.note("⑩e R18 fmt1：preset==0xFF 的轨取默认声音 + steps_clear（诊断槽 3，破坏性）")
    store_cmd(s, "STORE PROJ SYNTH fmt1", r"^PROJ 3 SYNTH case=fmt1 OK\r?$",
              "⑩e SYNTH fmt1")
    store_expect(s, r"^PROJ 3 SYNTH case=fmt1 trk k=0 engine=0 preset=0 steps=0 atk=0 e0=3\r?$",
                 "⑩e 轨 0：默认声音 ANALOG 808 BOOM（e0=3）+ 工程里的 99 被覆盖 + 步清空")
    store_expect(s, r"^PROJ 3 SYNTH case=fmt1 trk k=1 engine=1 preset=1 ",
                 "⑩e 轨 1：不带 0xFF ⇒ 保留工程里的 engine/preset（1/1）")

    s.ser.note("⑩f R18 foreign：引擎号 ≥ NENGINES 的轨走 proj_foreign_parts（图案保留）")
    store_cmd(s, "STORE PROJ SYNTH foreign", r"^PROJ 3 SYNTH case=foreign OK\r?$",
              "⑩f SYNTH foreign")
    store_expect(s,
                 r"^PROJ 3 SYNTH case=foreign trk k=1 engine=1 preset=0 steps=1 atk=0 e0=4\r?$",
                 "⑩f 轨 1：未知引擎 → DIGITAL RHODES（engine 1 preset 0、e0=4）、图案保留")
    store_expect(s, r"^PROJ 3 SYNTH case=foreign trk k=0 engine=0 preset=0 ",
                 "⑩f 轨 0：不受 foreign 分支影响（engine/preset 保持 0/0）")
    store_cmd(s, "STORE PROJ ERASE 3", r"^PROJ 3 ERASED\r?$", "⑩f 收口 ERASE 3（诊断槽）")


# ---- 自动存档与上电恢复（上游 autosave_tick/autosave_resume，Task 6） ----
# console 契约（main/console.c）：
#   STORE STATS  -> 行尾 "autosave=<0|1> autosave_hash=XXXXXXXX autosave_writes=n"
#   STORE AUTO ON|OFF -> "STORE AUTO on" / "STORE AUTO off"（诊断开关，默认 on）
# 上游门限原值：无输入 ≥2.5s（AUTOSAVE_IDLE）、两次写间隔 ≥20s（AUTOSAVE_GAP）、
# 停走带且无发声（audio_quiet）、捕获 hash 与上次不同才写；tick 1s 一拍。
AUTOSAVE_IDLE_S = 2.5
AUTOSAVE_GAP_S = 20.0
AUTOSAVE_STEP_TRACK = 0
AUTOSAVE_STEP_COL = 5    # 窗口 0..16 内的绝对步 5（轨 0）：「步表随工程恢复」的判据


def autosave_fields(s: Session) -> dict:
    """`STORE STATS` 的自动存档三项。缺字段（旧固件）抛 InjectError——RED 阶段在这里炸。"""
    s.cmd("STORE STATS")
    m = re.search(r"^STORE STATS .*autosave=(\d) autosave_hash=" + STORE_HASH8 +
                  r" autosave_writes=(\d+)\r?$", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(
            "STORE STATS: 缺少 autosave=…/autosave_hash=…/autosave_writes=… 字段"
            f"（{s.last.strip()!r}）")
    return {"on": int(m.group(1)), "hash": int(m.group(2), 16), "writes": int(m.group(3))}


def autosave_wait_writes(s: Session, want: int, timeout: float, label: str) -> float:
    """轮询到 `autosave_writes == want`，返回实测耗时（秒）；超时判失败并返回耗时。
    轮询只发 `STORE STATS`（不产生输入活动、不改引擎），每 0.5s 一次。"""
    t0 = time.monotonic()
    while True:
        got = autosave_fields(s)
        elapsed = time.monotonic() - t0
        if got["writes"] == want:
            s.ok(f"{label}：{elapsed:.1f}s 后 autosave_writes={want}")
            return elapsed
        if got["writes"] > want:
            s.fail(f"{label}：autosave_writes 超出期望（实得 {got['writes']}，期望 {want}）")
            return elapsed
        if elapsed >= timeout:
            s.fail(f"{label}：{timeout:.1f}s 内 autosave_writes 未到 {want}"
                   f"（实得 {got['writes']}）")
            return elapsed
        time.sleep(0.5)


def autosave_no_write(s: Session, writes: int, wait: float, label: str) -> None:
    """等 wait 秒再读计数：不得变化（不该写的时候一个字节都不写）。"""
    time.sleep(wait)
    got = autosave_fields(s)
    (s.ok if got["writes"] == writes else s.fail)(
        f"{label}（{wait:.1f}s 后 autosave_writes={got['writes']}，期望仍 {writes}）")


def autosave_bump_bpm(s: Session, prefer: int) -> int:
    """改全局 BPM 到一个**与当前不同**的值（同一状态不会触发写，hash 相同就测不出东西）。
    返回落地值。PARAM GLOBAL 经状态层 → 引擎，同时算一次输入活动（重置 idle 计时）。"""
    cur = store_bpm(s)
    want = prefer if cur != prefer else (prefer + 7 if prefer + 7 <= 240 else prefer - 7)
    store_cmd(s, f"PARAM GLOBAL 0 {want}", rf"^PARAM GLOBAL id=0 value={want} applied={want} ",
              f"BPM {cur} → {want}")
    (s.ok if store_bpm_settled(s, want) else s.fail)(f"STATE SNAP bpm={want}（改值生效）")
    return want


def step_fields(s: Session, track: int, idx: int) -> tuple:
    """`ENG STEP` 的 (n, note 列表, vel, time)：步表的逐字段判据。"""
    s.cmd(f"ENG STEP {track} {idx}")
    m = re.search(r"^ENG STEP k=(\d+) i=(\d+) n=(\d+) note=([\d,]+) vel=(\d+) time=(\d+)",
                  s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"ENG STEP: 回显不匹配（{s.last.strip()!r}）")
    return (int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6)))


def ensure_step_note(s: Session, track: int, col: int) -> tuple:
    """保证轨 track 的绝对步 col（窗口 0..16 内）有音（幂等：先读，空才 TAP 建）。
    返回落地后的步字段，供复位前后逐字段比较。"""
    st = step_fields(s, track, col)
    if st[0] == 0:
        x, y = step_cell_xy(col, track)
        s.cmd(f"TAP {x} {y}")
        s.expect(rf"-> STEP track={track} step={col} CMD_TOGGLE_STEP",
                 f"TAP 步 ({track},{col}) 建音")
        st = step_fields(s, track, col)
    (s.ok if st[0] >= 1 else s.fail)(f"轨 {track} 绝对步 {col} 有音（n={st[0]} note={st[1]}）")
    return st


def ensure_silent_pattern(s: Session) -> None:
    """让**播放中**没有音符发声：三条合成轨的 pattern 长度缩到 1 步（`P_SLEN=1`，播放只走
    步 0）并清掉步 0。上游 seq 的 `trk_len()` 用 `p[P_SLEN]`（clamp ≥1）作循环长度，所以
    「1 步 + 步 0 空」⇒ 走带在跑但一次都不触发。

    为什么需要：⑪d 要证明「只有 `song.playing` 能拦住写入」。若播放期间有音符在响，
    `audio_quiet()` 也会拦住同一个写——把 playing 门删掉，断言照样绿（评审 R23 的同一类
    问题）。静音后窗口内 `voices` 恒 0，⑪d 前后各采样一次 `ENG SNAP voices` 作前提断言，
    发声门则交给 ⑪e（走带已停、按住一个键）单独隔离。"""
    for k in range(3):
        select_track(s, k)
        store_cmd(s, f"PARAM {ENG_P_SLEN} 1",
                  rf"^PARAM track={k} id={ENG_P_SLEN} value=1 applied=1 ",
                  f"⑪d 轨 {k} P_SLEN=1（pattern 只有步 0）")
        s.cmd("UI step")
        s.expect(r"UI page=step \(2\)", f"⑪d UI step（轨 {k}）")
        if step_fields(s, k, 0)[0] != 0:
            x, y = step_cell_xy(0, k)
            s.cmd(f"TAP {x} {y}")
            s.expect(rf"-> STEP track={k} step=0 CMD_TOGGLE_STEP", f"⑪d 清空轨 {k} 步 0")
        (s.ok if step_fields(s, k, 0)[0] == 0 else s.fail)(f"⑪d 轨 {k} 步 0 为空（播放不会触发）")
    select_track(s, 0)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑪d UI step（收口回轨 0）")


def store_early_load_probe(s: Session, timeout: float = 20.0) -> str:
    """热复位后的**早探**：console 一接受命令就发 `STORE PROJ LOAD 0`（不等 init 完成）。

    `.noinit` 的哨兵/指针跨热复位存活，而 PSRAM 堆与 HAL 的 ready（.bss）是新的；若公共
    API 只看哨兵，这一发就会拿着上一次会话的悬垂信号量取锁 → 挂死或崩溃。故这里复位后
    **不调 wait_boot**（它会等到 boot 日志不再插行、天然错过 3.0s→4.1s 的窗口），而是
    自己读串口，一看到 `sloop>` 就立刻发命令，读到「STORE 结果令牌」为止。

    返回的原始回显里含 boot 日志，调用方按 `sloop_store: ready` 的位置判断这次命令是
    落在 init 之前（窗口内）还是之后（主机侧迟到）。
    """
    s.ser.note("⑦ 早探：esptool 复位（不等提示符）→ 见到 sloop> 立即发 STORE PROJ LOAD 0")
    s.ser.close()
    try:
        shot.esptool_reset(s.port, BAUD)
    except shot.ResetError as e:
        raise InjectError(f"⑦ 早探复位失败: {e}") from None
    deadline = time.monotonic() + 5.0
    while True:  # 复位后端口可能短暂重新枚举
        try:
            s.ser = s._open()
            break
        except InjectError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)

    needle = (b"STORE ERR", b"PROJ 0 ")
    buf = bytearray()
    sent = False
    deadline = time.monotonic() + timeout
    s.ser.timeout = 0.05
    while time.monotonic() < deadline:
        try:
            chunk = s.ser.read(256)
        except Exception:  # 重新枚举期间读失败：稍等再试
            time.sleep(0.05)
            continue
        if chunk:
            buf.extend(chunk)
            if not sent and b"sloop>" in buf:
                s.ser.write(b"STORE PROJ LOAD 0\n")
                s.ser.flush()
                sent = True
            if sent and any(n in buf for n in needle):
                break
    s.last = buf.decode(errors="replace")
    return s.last


def store_warm_verdict(text: str):
    """解析早探回显：(在 init 之前?, 结果令牌)。令牌 = 回显里最早出现的 STORE 结果行。"""
    tokens = ("STORE ERR no_storage", "STORE ERR empty", "STORE ERR playing",
              "STORE ERR invalid", "STORE ERR save", "PROJ 0 LOADED", "PROJ 0 SAVED")
    hit = min(((text.find(t), t) for t in tokens if text.find(t) >= 0), default=None)
    ready = text.find("sloop_store: ready")
    if hit is None:
        return (None, None)  # 没有结果令牌（挂死/崩溃）
    in_window = ready < 0 or hit[0] < ready
    return (in_window, hit[1])


def run_store(s: Session) -> None:
    """store 场景：工程 A–D 的 RAM 槽 + flash 双层往返 + 开机 fetch（brief Step 1）。

    ① 归一：四槽 ERASE → LIST 全 EMPTY（脚本可重复）；
    ② BPM 100 → LIVEHASH=h0 → SAVE 0 → HASH 0==h0（RAM 槽 hash 与捕获同源）；
    ③ BPM 120 → LIVEHASH=h1≠h0（保存的确实是快照，不是活状态）；
    ④ LOAD 0 → LIVEHASH==h0、STATE SNAP bpm 回 100；
    ⑤ ERASE 0 → LIST EMPTY、LIVEHASH 不变（擦除只动存储，不动引擎）；
    ⑥ 再 SAVE 0 → h2==h0（同一引擎状态 ⇒ 同一 hash，往返稳定）；
    ⑦ esptool 硬复位 → **早探**（console 一接受命令就发 LOAD 0，必须回 STORE ERR
       no_storage、绝不挂死：`.noinit` 哨兵跨热复位存活，而 HAL ready 在 .bss）→
       LIST 仍 USED、**HASH 0==h2**（HASH 只读 RAM 槽、不走 flash 回落 ⇒ 这是「开机
       fetch 填充了 RAM 槽」的证据）、LOAD 0 → LIVEHASH==h2（bpm 轮询等投影对齐）；
    ⑧ 单例 ENG SELFTEST 之后 HASH 0 仍==h2（PSRAM 指针在 .noinit，自检 restore
       不回滚——本任务最险的失效模式）；
    ⑨ 用户预置 32 槽（run_store_presets）：SAVE/LIST/LOAD/ERASE 往返 + 跨轨/跨银行 +
       播放守卫 + 鼓轨拒绝 + SELFTEST/硬复位后仍可读（up_boot）。
    ⑩ 换引擎/预置 = 换声音（run_store_engine，最先跑）：`ENG PRESET` 的期望向量 vs
       `ENG TRACK` 应用后的 p[]（71/71）+ param_kept 保留 + 换引擎 + **独立上游表 oracle**
       （R17）+ 发声时换预置的 panic 路径（`ENG SNAP` 的 voices 归零）+ **project_load 的
       两条 fallback**（R18：`STORE PROJ SYNTH fmt1|foreign`，诊断槽 3）。
    ⑪ 自动存档与上电恢复（run_store_autosave）：空闲写 / 20s gap / 同态不写 /
       播放与发声抑制 / `STORE AUTO OFF` 抑制（每一条都只有被测的那道门能拦住写入，
       见该函数的 docstring）/ 硬复位后 LIVEHASH+bpm+步表恢复且开机不重写。
       它留一个 autosave 对象，故**第二次运行起** ⓪ 的「硬复位到工厂态」实际是「硬复位到
       上一轮 ⑪ 结束时的状态」——本场景的断言全是同轮相对比较（h0/h1/h2 只在同轮内比），
       不受影响；证据里的 h0 从第 2 轮起稳定。
    ⑫ FM6 银行 27 槽（run_store_bank，最后跑）：live patch → PUT/GET 往返 → LIST →
       自检后**紧跟**写（`.bss` 的 used 位回滚，ADR-009 §8）→ 硬复位后仍 USED →
       PTCH=11 读回槽 3 的字节 → ERASE/空槽/越界/播放与鼓轨拒绝。本段全程
       `STORE AUTO OFF`（它改了轨 1 的引擎；AUTO 开着会把它写进 autosave 漂进下一轮），
       结束时槽 3/4/5 已擦除。

    ⓪ 归一（两次运行起点必须相同，否则 LIVEHASH 不可比）：esptool 硬复位——引擎参数/选轨/
       引擎号不带上一次运行 ⑨ 载入的预置参数（实测：不归一时 run 2 的 h0 变成上一轮载入后
       的状态，h0/h2 与 run 1 不同）。Task 6 起复位还会恢复上一轮的 autosave（⑪ 段留下的
       工作工程），故「起点」= 上一轮 ⑪ 结束时的状态；它由本脚本自己写出，从第 2 轮起逐位
       相同（实测 run 2/3 的 h0/h1/h2 全同）。再 `ENG AUDIO ON` + 停止：走带请求
       （transport_req）只由渲染循环消费，停流时残留的请求会让写 API 回 playing。
       选中轨（`song.sel`，工程的一部分、随 autosave 恢复）显式归一到 0：⑩ 的 `PARAM`
       作用于选中轨，不归一就会写到上一轮剩下的轨上（设备实测：autosave 里 sel=1）。
    """
    # 归一：硬复位到工厂态（不写 flash，只 esptool 硬复位）
    s.ser.note("⓪ 归一：esptool 硬复位到工厂态（不带上一轮载入的预置参数）")
    s.reboot()
    # 走带请求（transport_req）只由渲染循环消费——音频停流时它会一直挂着，让每个写 API
    # 都回 STORE ERR playing。上一轮若在 ENG AUDIO OFF 处中断就会这样。
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "⓪ ENG AUDIO streaming=1（归一：走带请求靠渲染循环消费）")
    ensure_stopped(s)
    store_cmd(s, "STORE STATS", r"^STORE STATS ready=1 ", "STORE STATS ready=1")
    # 选中轨是**工程的一部分**（proj_capture 的 sel）：Task 6 起复位会从 autosave 恢复
    # 上一轮的选中轨，故 ⓪ 必须显式归一到轨 0——否则 ⑩ 的 `PARAM`（作用于选中轨）会写到
    # 别的轨、而 `ENG PRESET 0 …` 比对的是轨 0（设备上真实踩到：autosave 里 sel=1）。
    select_track(s, 0)
    # 自动存档（Task 6）默认 ON：本场景的断言是「某个状态在某个时刻的 hash/对象」，
    # 空闲时的一次擦写会让 LIVEHASH 与 STORE STATS 的读数错拍，故先关掉；⑪ 段自己打开。
    # 注意 **`STORE AUTO` 不跨复位**（init 每次回到默认 ON）：⑦/⑨g/⑪g 每次硬复位后
    # 都重新下发一次（⑪g 之后是本场景最后一段，⑪ 段自己会在复位后按需再次打开）。
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⓪ STORE AUTO OFF（本场景其余段免干扰）")

    # ⑩ 换引擎/预置 = 换声音（Task 5）：最先跑——旧固件没有 `ENG PRESET`，RED 快速失败
    run_store_engine(s)

    s.ser.note("① 归一：四槽 ERASE + LIST 全 EMPTY")
    for i in range(4):
        store_cmd(s, f"STORE PROJ ERASE {i}", rf"^PROJ {i} ERASED\r?$", f"① ERASE {i}")
    s.cmd("STORE PROJ LIST")
    for i in range(4):
        store_expect(s, rf"^PROJ {i} EMPTY\r?$", f"① LIST PROJ {i} EMPTY")

    s.ser.note("② BPM 100 → LIVEHASH h0 → SAVE 0")
    store_cmd(s, "PARAM GLOBAL 0 100", r"^PARAM GLOBAL id=0 value=100 applied=100 ",
              "② BPM=100")
    (s.ok if store_bpm(s) == 100 else s.fail)("② STATE SNAP bpm=100")
    h0 = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "② LIVEHASH h0")
    # 保存前先归一 hash 非零（0 = 无效工程；捕获失败会静默「通过」后续相等断言）
    (s.ok if h0 != 0 else s.fail)(f"② h0={h0:08X} 非零（捕获有效）")
    store_cmd(s, "STORE PROJ SAVE 0", rf"^PROJ 0 SAVED hash={STORE_HASH8}\r?$", "② SAVE 0")
    (s.ok if store_hash(s, "STORE PROJ HASH 0", r"^PROJ 0 HASH=" + STORE_HASH8,
                        "② HASH 0（RAM 槽）") == h0 else s.fail)(f"② HASH 0 == h0={h0:08X}")

    s.ser.note("③ BPM 120 → LIVEHASH h1 != h0")
    store_cmd(s, "PARAM GLOBAL 0 120", r"^PARAM GLOBAL id=0 value=120 applied=120 ",
              "③ BPM=120")
    (s.ok if store_bpm(s) == 120 else s.fail)("③ STATE SNAP bpm=120")
    h1 = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "③ LIVEHASH h1")
    (s.ok if h1 != h0 else s.fail)(f"③ h1={h1:08X} != h0={h0:08X}")

    s.ser.note("④ LOAD 0 → 引擎回到 h0（含 bpm）")
    store_cmd(s, "STORE PROJ LOAD 0", rf"^PROJ 0 LOADED hash={STORE_HASH8}\r?$", "④ LOAD 0")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "④ LIVEHASH==h0") == h0 else s.fail)(
        f"④ LIVEHASH 回到 h0={h0:08X}")
    (s.ok if store_bpm_settled(s, 100) else s.fail)("④ STATE SNAP bpm=100（保存时的值）")

    s.ser.note("⑤ ERASE 0 → LIST EMPTY、LIVEHASH 不变")
    store_cmd(s, "STORE PROJ ERASE 0", r"^PROJ 0 ERASED\r?$", "⑤ ERASE 0")
    s.cmd("STORE PROJ LIST")
    for i in range(4):
        store_expect(s, rf"^PROJ {i} EMPTY\r?$", f"⑤ LIST PROJ {i} EMPTY")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⑤ LIVEHASH 不变") == h0 else s.fail)("⑤ LIVEHASH 仍为 h0（擦除不动引擎）")

    s.ser.note("⑥ 再 SAVE 0（为开机 fetch 用例留一个 USED 槽）")
    h2 = store_hash(s, "STORE PROJ SAVE 0", rf"^PROJ 0 SAVED hash={STORE_HASH8}\r?$", "⑥ SAVE 0")
    (s.ok if h2 == h0 else s.fail)(f"⑥ 再保存 hash h2={h2:08X} == h0={h0:08X}")

    s.ser.note("⑦ 硬复位（不写 flash）→ 先在 console 一接受命令时早探 LOAD 0（热复位哨兵）")
    early = False
    for attempt in range(1, STORE_WARM_ATTEMPTS + 1):
        text = store_early_load_probe(s)
        in_window, token = store_warm_verdict(text)
        s.ser.note(f"⑦ 早探 {attempt}/{STORE_WARM_ATTEMPTS}: in_window={in_window} "
                   f"token={token!r} 回显={text.strip()[-400:]!r}")
        if token is None:
            raise InjectError(
                "⑦ 热复位后早探 STORE PROJ LOAD 0 无结果令牌（挂死或崩溃？）："
                f"{text.strip()[-400:]!r}")
        if in_window and token == "STORE ERR no_storage":
            early = True
            s.ok(f"⑦ init 之前发 STORE PROJ LOAD 0 → {token}（第 {attempt} 次，窗口内）")
            s.wait_ready(timeout=20.0)  # 等 boot 日志结束 + init 完成
            break
        if in_window:
            s.fail(f"⑦ init 之前发 STORE PROJ LOAD 0 却回了 {token!r}（应为 STORE ERR no_storage）")
            s.wait_ready(timeout=20.0)
            break
        print(f"  [NOTE] ⑦ 早探 {attempt}: 主机侧迟到（init 已完成，回 {token!r}），重新对齐")
    if not early and s.failures == 0:
        print(f"  [NOTE] ⑦ {STORE_WARM_ATTEMPTS} 次早探都没命中 init 之前的窗口"
              "（测试前提未成立；不判固件失败，但本项无证据）")
        s.wait_ready(timeout=20.0)

    store_cmd(s, "STORE STATS", r"^STORE STATS ready=1 ", "⑦ 复位后 STORE STATS ready=1")
    # STORE AUTO 不跨复位（init 每次都回到默认 ON）：每次硬复位后重新关掉，否则本段的
    # 空闲擦写会与下面的 hash/对象断言抢时序（⑪ 段自己再打开）。
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑦ 复位后 STORE AUTO OFF")
    s.cmd("STORE PROJ LIST")
    store_expect(s, rf"^PROJ 0 USED hash={STORE_HASH8}\r?$", "⑦ LIST PROJ 0 USED")
    (s.ok if store_hash(s, "STORE PROJ HASH 0", r"^PROJ 0 HASH=" + STORE_HASH8,
                        "⑦ HASH 0（RAM 槽开机 fetch）") == h2 else s.fail)(
        f"⑦ HASH 0 == h2={h2:08X}")
    store_cmd(s, "STORE PROJ LOAD 0", rf"^PROJ 0 LOADED hash={STORE_HASH8}\r?$", "⑦ LOAD 0")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⑦ LIVEHASH==h2") == h2 else s.fail)(
        f"⑦ LIVEHASH 恢复 h2={h2:08X}")
    (s.ok if store_bpm_settled(s, 100) else s.fail)("⑦ STATE SNAP bpm=100（工程内的值）")

    s.ser.note("⑧ 诊断期 selftest 之后存储仍可用（.noinit 指针/RAM 槽不被 restore 回滚）")
    s.cmd("ENG AUDIO OFF")
    s.expect(r"ENG AUDIO streaming=0", "⑧ ENG AUDIO OFF")
    s.cmd("ENG SELFTEST preset/ANALOG/00_808_BOOM", timeout=90)
    store_expect(s, r"^ENG SELFTEST .*result=PASS\r?$", "⑧ 单例 selftest PASS")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "⑧ ENG AUDIO ON")
    (s.ok if store_hash(s, "STORE PROJ HASH 0", r"^PROJ 0 HASH=" + STORE_HASH8,
                        "⑧ selftest 后 HASH 0（RAM 槽）") == h2 else s.fail)(
        f"⑧ HASH 0 == h2={h2:08X}（自检 restore 未回滚存储指针与 RAM 槽）")

    run_store_presets(s)
    run_store_autosave(s)
    run_store_bank(s)


def run_store_presets(s: Session) -> None:
    """⑨ 用户预置 32 槽（upreset.c，Task 4）：

    ① 归一基线：选中轨 0 + `ENG TRACK 0 0 0`（ANALOG/工厂预置）+ `PARAM BATCH` 写 8 个
       已知参数（ATK/DEC/…/TRIM，回显 applied）→ 两次运行的起点逐位相同；再 ERASE 0/1 →
       LIST 32 槽全 EMPTY、COUNT used=0 total=32；
    ② SAVE 0（默认名 = 引擎名 + 槽号 "ANALOG 01"）→ LIST 与 COUNT 反映 USED；
    ③ 改成另一组值 → LOAD 0 0 → 8 个参数逐项回到 SAVE 时（预置覆盖 param_kept 之外的全部）；
    ④ LOAD 0（缺省 track = 选中轨）同样生效；
    ⑤ `ENG TRACK 0 4 0` 换成 SAMPLE 后 LOAD ⇒ 引擎回到预置里的 ANALOG、参数不变（换声音）；
    ⑥ 跨轨载入（LOAD 0 1）：选中轨 0 的 8 参数不变、轨 1 取到预置的 8 参数；
    ⑦ 银行 1（槽 16）：SAVE/LIST/LOAD 往返 + ERASE 不碰银行 0 的槽 0；
    ⑧ ERASE 0 → EMPTY；再 SAVE（为复位用例留一个 USED 槽）；
    ⑨ 鼓轨（TAP TRACKS 卡 3）SAVE 1 / LOAD 0 3 → `STORE ERR drum`，槽 1 仍 EMPTY；
    ⑩ 单例 ENG SELFTEST 之后 LIST/LOAD 仍可用（读路径重扫）；第二次 selftest 后**紧跟**
       SAVE 1（不先读预置）⇒ 同银行的槽 0 必须保留（写路径重扫）；
    ⑪ 播放中 SAVE/ERASE ⇒ `STORE ERR playing`；播放中鼓轨 SAVE ⇒ `STORE ERR drum`
       （上游 up_ui 的判定顺序：鼓轨先于播放守卫）；
    ⑫ esptool 硬复位后 LIST 仍 USED（`up_boot` 的 flash 扫描证据）+ LOAD 0 0 还原同一组值。
    """
    s.ser.note("⑨ 用户预置：归一基线 → ERASE → SAVE/LIST/LOAD → 跨轨/跨银行 → 鼓轨 → 自检/复位")
    # 归一基线：已知轨/引擎/工厂预置/已知参数 + 已知页面（select_track 落 TRACKS 页）。
    # 两次运行（以及任何上一个场景）都从这里起跑，SAVE 写出的记录与期望值才逐位可比。
    select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⑨ 归一基线 engine=0 preset=0（ANALOG）")
    saved = preset_params_write(s, PRESET_PARAM_A, "⑨ 归一基线参数（8 个，ATK…TRIM）")
    for k in (0, 1):
        store_cmd(s, f"STORE PRESET ERASE {k}", rf"^PRESET {k} ERASED\r?$", f"⑨ ERASE {k}（归一）")
    used, seen, count = preset_list(s)
    (s.ok if seen == set(range(STORE_PRESET_SLOTS)) else s.fail)(
        f"⑨ LIST 列出全部 {STORE_PRESET_SLOTS} 槽（实得 {len(seen)}）")
    (s.ok if not used else s.fail)(f"⑨ 归一后 LIST 无 USED（实得 {sorted(used)}）")
    (s.ok if count == (0, STORE_PRESET_SLOTS) else s.fail)(
        f"⑨ PRESET COUNT used=0 total={STORE_PRESET_SLOTS}（实得 {count}）")

    s.ser.note("⑨a SAVE 0（当前轨 0 的声音 = 归一基线的 8 参数）")
    store_cmd(s, "STORE PRESET SAVE 0", r"^PRESET 0 SAVED ANALOG ANALOG 01\r?$",
              "⑨ SAVE 0（默认名 = 引擎名 + 槽号）")
    used, _, count = preset_list(s)
    (s.ok if used.get(0) == ("ANALOG", "ANALOG 01") else s.fail)(
        f"⑨ LIST → PRESET 0 USED ANALOG ANALOG 01（实得 {used.get(0)}）")
    (s.ok if count == (1, STORE_PRESET_SLOTS) else s.fail)(
        f"⑨ PRESET COUNT used=1（实得 {count}）")

    s.ser.note("⑨b 改参数组 B → LOAD 0 0 → 8 参数逐项回到 A")
    preset_params_write(s, PRESET_PARAM_B, "⑨ 写参数组 B")
    changed = preset_params_read(s)
    (s.ok if changed != saved else s.fail)(
        f"⑨ 改值生效（{changed[1]},{changed[2]}… ≠ SAVE 时的 {saved[1]},{saved[2]}…）")
    store_cmd(s, "STORE PRESET LOAD 0 0", r"^PRESET 0 LOADED track=0\r?$", "⑨ LOAD 0 0")
    back = preset_params_read(s)
    for pid in PRESET_PARAM_IDS:
        (s.ok if back[pid] == saved[pid] else s.fail)(
            f"⑨ LOAD 后 id={pid} value={back[pid]} == SAVE 时 {saved[pid]}")

    s.ser.note("⑨c LOAD 0（缺省 track = 选中轨）")
    store_cmd(s, "STORE PRESET LOAD 0", r"^PRESET 0 LOADED track=0\r?$", "⑨ LOAD 0（缺省轨）")

    s.ser.note("⑨c' 换成 SAMPLE 引擎再 LOAD：预置带回它自己的引擎（SAVE 的是 ANALOG）")
    s.cmd("ENG TRACK 0 4 0")
    s.expect(r"ENG TRACK k=0 engine=4 preset=0 OK", "⑨ ENG TRACK 0 4 0（切 SAMPLE）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=4 ", "⑨ 换引擎后 ENG SNAP engine=4")
    store_cmd(s, "STORE PRESET LOAD 0 0", r"^PRESET 0 LOADED track=0\r?$", "⑨ 换引擎后 LOAD 0 0")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 ", "⑨ LOAD 后 engine 回到 0（预置里的引擎）")
    back = preset_params_read(s)
    for pid in PRESET_PARAM_IDS:
        (s.ok if back[pid] == saved[pid] else s.fail)(
            f"⑨ 换引擎后 LOAD id={pid} value={back[pid]} == SAVE 时 {saved[pid]}")

    s.ser.note("⑨c2 跨轨载入：LOAD 0 1 → 选中轨(0) 的 p[] 不变、轨 1 取到预置")
    before0 = preset_params_read(s)  # 选中轨仍是 0
    store_cmd(s, "STORE PRESET LOAD 0 1", r"^PRESET 0 LOADED track=1\r?$", "⑨ LOAD 0 1（跨轨）")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 0, "⑨ 跨轨 LOAD 后 selected_track 仍 0")
    after0 = preset_params_read(s)
    for pid in PRESET_PARAM_IDS:
        (s.ok if after0[pid] == before0[pid] else s.fail)(
            f"⑨ 跨轨 LOAD 后选中轨(0) id={pid} value={after0[pid]} 不变")
    select_track(s, 1)
    t1 = preset_params_read(s)
    for pid in PRESET_PARAM_IDS:
        (s.ok if t1[pid] == saved[pid] else s.fail)(
            f"⑨ 轨 1 id={pid} value={t1[pid]} == 预置 {saved[pid]}")
    select_track(s, 0)

    s.ser.note("⑨c3 银行 1（槽 16）：SAVE/LIST/LOAD 往返；ERASE 16 不碰银行 0 的槽 0")
    b16 = preset_params_write(s, PRESET_PARAM_B, "⑨ 写参数组 B（槽 16 用）")
    store_cmd(s, "STORE PRESET SAVE 16", r"^PRESET 16 SAVED ANALOG ANALOG 17\r?$",
              "⑨ SAVE 16（bank1）")
    used, _, count = preset_list(s)
    (s.ok if used.get(16) == ("ANALOG", "ANALOG 17") and used.get(0) == ("ANALOG", "ANALOG 01")
     and count == (2, STORE_PRESET_SLOTS) else s.fail)(
        f"⑨ LIST 槽 0/16 均 USED、COUNT used=2（实得 {sorted(used)} count={count}）")
    preset_params_write(s, PRESET_PARAM_A, "⑨ 改回参数组 A")
    store_cmd(s, "STORE PRESET LOAD 16 0", r"^PRESET 16 LOADED track=0\r?$", "⑨ LOAD 16")
    back16 = preset_params_read(s)
    for pid in PRESET_PARAM_IDS:
        (s.ok if back16[pid] == b16[pid] else s.fail)(
            f"⑨ LOAD 16 后 id={pid} value={back16[pid]} == 保存时 {b16[pid]}")
    preset_params_write(s, PRESET_PARAM_A, "⑨ 收口写回参数组 A")
    store_cmd(s, "STORE PRESET ERASE 16", r"^PRESET 16 ERASED\r?$", "⑨ 收口 ERASE 16")
    used, _, count = preset_list(s)
    (s.ok if 16 not in used and used.get(0) == ("ANALOG", "ANALOG 01")
     and count == (1, STORE_PRESET_SLOTS) else s.fail)(
        f"⑨ ERASE 16 后槽 0 仍 USED、槽 16 EMPTY（实得 {sorted(used)} count={count}）")

    s.ser.note("⑨d ERASE 0 → EMPTY；再 SAVE（为后面的复位用例留一个 USED 槽）")
    store_cmd(s, "STORE PRESET ERASE 0", r"^PRESET 0 ERASED\r?$", "⑨ ERASE 0")
    used, _, count = preset_list(s)
    (s.ok if 0 not in used and count == (0, STORE_PRESET_SLOTS) else s.fail)(
        f"⑨ ERASE 后 PRESET 0 EMPTY、COUNT used=0（实得 used={sorted(used)} count={count}）")
    store_cmd(s, "STORE PRESET SAVE 0", r"^PRESET 0 SAVED ANALOG ANALOG 01\r?$", "⑨ 再 SAVE 0")

    s.ser.note("⑨e 鼓轨：SAVE 1 / LOAD 0 3 均被拒（STORE ERR drum），槽 1 仍 EMPTY")
    select_track(s, 3)
    store_cmd(s, "STORE PRESET SAVE 1", r"^STORE ERR drum\r?$", "⑨ 鼓轨 SAVE → STORE ERR drum")
    store_expect(s, r"^DRUM TRACK: NO SOUND\r?$", "⑨ 鼓轨 SAVE 出上游消息 DRUM TRACK: NO SOUND")
    store_cmd(s, "STORE PRESET LOAD 0 3", r"^STORE ERR drum\r?$", "⑨ 鼓轨 LOAD → STORE ERR drum")
    store_expect(s, r"^DRUM TRACK: NO SOUND\r?$",
                 "⑨ 鼓轨 LOAD 也出 DRUM TRACK: NO SOUND（对齐上游 up_ui）")
    used, _, _ = preset_list(s)
    (s.ok if 1 not in used else s.fail)(f"⑨ 鼓轨 SAVE 未写槽 1（used={sorted(used)}）")
    select_track(s, 0)

    s.ser.note("⑨f 单例 ENG SELFTEST 之后预置仍可用（银行指针在 .bss，restore 会清空）")
    s.cmd("ENG AUDIO OFF")
    s.expect(r"ENG AUDIO streaming=0", "⑨ ENG AUDIO OFF")
    s.cmd("ENG SELFTEST preset/ANALOG/00_808_BOOM", timeout=90)
    store_expect(s, r"^ENG SELFTEST .*result=PASS\r?$", "⑨ 单例 selftest PASS")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "⑨ ENG AUDIO ON")
    # 读路径：restore 清空了银行索引，LIST/LOAD 必须自己重扫（否则预置读成空）
    used, _, _ = preset_list(s)
    (s.ok if used.get(0) == ("ANALOG", "ANALOG 01") else s.fail)(
        f"⑨ selftest 后 LIST → PRESET 0 USED（实得 {used.get(0)}）")
    store_cmd(s, "STORE PRESET LOAD 0 0", r"^PRESET 0 LOADED track=0\r?$", "⑨ selftest 后 LOAD 0 0")

    # 写路径：再来一次 selftest（索引重新被清），**紧跟** SAVE 1、不先做任何预置读——
    # 写路径必须自己重扫，否则 up_put 从空的 XIP 副本起笔，把同银行里已用的槽 0 抹掉。
    s.ser.note("⑨f' 第二次 selftest → 紧跟 SAVE 1（不先读预置）：同银行的槽 0 必须保留")
    s.cmd("ENG AUDIO OFF")
    s.expect(r"ENG AUDIO streaming=0", "⑨ ENG AUDIO OFF（第二次）")
    s.cmd("ENG SELFTEST preset/ANALOG/00_808_BOOM", timeout=90)
    store_expect(s, r"^ENG SELFTEST .*result=PASS\r?$", "⑨ 第二次单例 selftest PASS")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "⑨ ENG AUDIO ON（第二次）")
    store_cmd(s, "STORE PRESET SAVE 1", r"^PRESET 1 SAVED ANALOG ANALOG 02\r?$",
              "⑨ selftest 后 SAVE 1（未经过任何预置读）")
    used, _, _ = preset_list(s)
    (s.ok if used.get(0) == ("ANALOG", "ANALOG 01") and used.get(1) == ("ANALOG", "ANALOG 02")
     else s.fail)(f"⑨ selftest 后 SAVE 1：槽 0/1 均 USED（实得 {used}）")
    store_cmd(s, "STORE PRESET ERASE 1", r"^PRESET 1 ERASED\r?$", "⑨ 收口 ERASE 1")
    used, _, _ = preset_list(s)
    (s.ok if used.get(0) == ("ANALOG", "ANALOG 01") and 1 not in used else s.fail)(
        f"⑨ ERASE 1 后槽 0 仍 USED、槽 1 EMPTY（实得 {sorted(used)}）")

    s.ser.note("⑨g esptool 硬复位 → LIST 仍 USED（up_boot）+ LOAD 还原同一组值")
    s.reboot()
    store_cmd(s, "STORE STATS", r"^STORE STATS ready=1 ", "⑨ 复位后 STORE STATS ready=1")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑨ 复位后 STORE AUTO OFF")
    used, _, _ = preset_list(s)
    (s.ok if used.get(0) == ("ANALOG", "ANALOG 01") else s.fail)(
        f"⑨ 复位后 LIST → PRESET 0 USED（实得 {used.get(0)}）")
    select_track(s, 0)
    store_cmd(s, "STORE PRESET LOAD 0 0", r"^PRESET 0 LOADED track=0\r?$", "⑨ 复位后 LOAD 0 0")
    back = preset_params_read(s)
    for pid in PRESET_PARAM_IDS:
        (s.ok if back[pid] == saved[pid] else s.fail)(
            f"⑨ 复位后 LOAD id={pid} value={back[pid]} == 保存时 {saved[pid]}")

    s.ser.note("⑨h 播放守卫：SAVE/ERASE ⇒ STORE ERR playing；播放中鼓轨 SAVE ⇒ drum（上游顺序）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑨ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑨ TAP PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑨ STATE SNAP playing=1")
    store_cmd(s, "STORE PRESET SAVE 0", r"^STORE ERR playing\r?$", "⑨ 播放中 SAVE → playing")
    store_cmd(s, "STORE PRESET ERASE 0", r"^STORE ERR playing\r?$", "⑨ 播放中 ERASE → playing")
    select_track(s, 3)
    store_cmd(s, "STORE PRESET SAVE 2", r"^STORE ERR drum\r?$",
              "⑨ 播放中鼓轨 SAVE → drum（鼓轨检查先于播放守卫）")
    store_expect(s, r"^DRUM TRACK: NO SOUND\r?$", "⑨ 播放中鼓轨 SAVE 的 DRUM TRACK: NO SOUND")
    select_track(s, 0)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑨ UI transport（收口）")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑨ TAP STOP")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑨ STATE SNAP playing=0（收口）")
    used, _, _ = preset_list(s)
    (s.ok if used.get(0) == ("ANALOG", "ANALOG 01") and 2 not in used else s.fail)(
        f"⑨ 被拒的 SAVE/ERASE 未改存储：槽 0 仍 USED、槽 2 仍 EMPTY（实得 {sorted(used)}）")


def run_store_autosave(s: Session) -> None:
    """⑪ 自动存档与上电恢复（Task 6，上游 autosave_tick/autosave_resume 语义）。

    `STORE AUTO OFF` 由 run_store ⓪ 下发（其它段的断言不受「空闲时的一次 flash 擦写」
    干扰；**它不跨复位**——⑦/⑨g 每次硬复位后都重新下发）；本段归一再打开。

    每条断言都必须**只有被测的那一道门**能拦住写入——否则删掉门也照样绿（评审 R20）：
    ⓐ 首次空闲写：改 BPM → 1.0s 时不得写（idle 2.5s 未到）→ 轮询到写一次；`autosave_hash`
       == `LIVEHASH`（写的就是当前工作工程）。本次距 reboot 不足 20s，上游 `autosave_ms`
       从 0 起算的**boot gap** 也参与（首个写只能落在 uptime ≥20s 之后），故实测可能 3–20s。
    ⓒ gap 20s（紧跟 ⓐ 的写）：改 BPM → 4s 内不写；若 gap 门被删，idle 一到（2.5s +
       一拍 ≤3.5s）就会写，4s 的读数必须抓到；随后在 20s 窗口到点才写，实测「改值 → 写」≥6s。
    ⓑ hash 门（紧跟 ⓒ 的写）：状态**不变**等 25s（>gap 20s + idle 2.5s ⇒ gap/idle 都已满足，
       只有「hash 相同」能拦住）→ 计数不变。
    ⓓ 播放门（ⓑ 之后，gap 已在 25s 外过期）：先把三条合成轨的 pattern 缩到 1 步并清空步 0
       （`ensure_silent_pattern`：走带在跑但一次都不触发 ⇒ `audio_quiet` 恒成立；窗口结束后
       把 `P_SLEN` 归回默认 16，避免这个副作用随 autosave 漂进下一轮的 ② h0）→ TAP PLAY →
       **改 BPM**（`proj_capture` 真捕获的值）⇒ 前提断言 `autosave_hash != LIVEHASH`（hash
       门不成立）→ 10s + 3s 两读都不写，且窗口**前后**各采样一次 `ENG SNAP voices`（都须为 0）
       ——此刻 idle/gap/hash/发声四道门都成立，唯一能拦的是 `song.playing`。
    ⓔ 发声门（`audio_quiet`，上一次写已在 20s 之外）：按住 C（`voices≥1`）→ 改 BPM →
       **先睡 3.5s 让 idle 过期**，再观察 3s 不写（整个窗口 idle/gap 都满足，只有发声能拦）；
       松开 → 2.5–6s（纯 idle）写一次。
    ⓕ 开关门：`STORE AUTO OFF` → 改 BPM（到 140）→ 等 26s（>gap 20s ⇒ idle/gap 都满足）→
       再观察 5s 窗口（两次读）不写；`STORE AUTO ON` → 开闸即写一次（≤5s，写的就是 OFF
       期间改出来的现状，`autosave_hash == LIVEHASH`）——证明是开关在拦，不是 gap。
    ⓖ 复位恢复（bpm=140 就是 ⑪f 那一份）：`ENG STEP 0 5` 已有音 → esptool 硬复位 →
       `LIVEHASH` == 该 hash、`STATE SNAP` bpm=140、`ENG STEP 0 5` 逐字段相同、
       `autosave_writes=0`；再等过 boot 的 20s gap → 计数仍 0（resume 记住了 hash，
       第一拍不把刚读回的东西重写一遍）。

    RED 判据：旧固件没有 `STORE AUTO`（run_store ⓪ 就炸）也没有 autosave=… 字段。
    """
    s.ser.note("⑪ 自动存档：空闲写 → gap 20s → 同态不写 → 播放/发声抑制 → OFF → 复位恢复")
    select_track(s, 0)
    ensure_stopped(s)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑪ UI step")
    ensure_window(s, 0, 16)
    step_before = ensure_step_note(s, AUTOSAVE_STEP_TRACK, AUTOSAVE_STEP_COL)

    store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⑪ STORE AUTO ON")
    st = autosave_fields(s)
    (s.ok if st["on"] == 1 else s.fail)("⑪ STORE STATS autosave=1")

    s.ser.note("⑪a 改 BPM → 首次写一次、autosave_hash == LIVEHASH")
    autosave_bump_bpm(s, 100)
    autosave_no_write(s, st["writes"], 1.0, "⑪a 改值后 1.0s 不写（idle 2.5s 未到）")
    w1 = st["writes"] + 1
    elapsed = autosave_wait_writes(s, w1, 25.0,
                                   "⑪a 空闲后写一次（含 boot 的 20s gap：autosave_ms 从 0 起）")
    (s.ok if elapsed >= AUTOSAVE_IDLE_S else s.fail)(
        f"⑪a 首次写不早于 idle 2.5s（实测 {elapsed:.1f}s；本次距 boot 不足 20s，"
        "上游从 0 起算的 boot gap 也参与——⑪e 单独量纯 idle）")
    st = autosave_fields(s)
    live = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "⑪a LIVEHASH")
    (s.ok if st["hash"] == live else s.fail)(
        f"⑪a autosave_hash={st['hash']:08X} == LIVEHASH={live:08X}")

    s.ser.note("⑪c gap 20s（紧跟上一次写）：改值后 4s 不写、20s 窗口到点才写")
    autosave_bump_bpm(s, 110)
    # 4.0s > idle 2.5s + 一拍任务 1s：若 gap 门被删，这一拍就会写，4.0s 的读数必须抓到；
    # 窗口内前 2.5s 是 idle 在拦，后 1.5s 只有 gap 能拦（读数落在后者）。
    autosave_no_write(s, w1, 4.0,
                      "⑪c 改值后 4s 不写（若忽略 gap，idle 一到 2.5s+一拍就会写）")
    elapsed = autosave_wait_writes(s, w1 + 1, 20.0, "⑪c gap 到点后写一次")
    total = 4.0 + elapsed
    (s.ok if total >= 5.5 else s.fail)(
        f"⑪c「改值 → 写」实测 {total:.1f}s ≥ 5.5s（gap 20s 在起作用，不是 idle 一到就写）")
    (s.ok if total <= AUTOSAVE_GAP_S + 2.0 else s.fail)(
        f"⑪c 实测 {total:.1f}s ≤ {AUTOSAVE_GAP_S + 2.0:.1f}s（gap 到点即写）")
    w2 = w1 + 1

    s.ser.note("⑪b hash 门（紧跟上一次写）：状态不变等 25s（>gap+idle）不写")
    autosave_no_write(s, w2, 25.0, "⑪b 同一状态 25s 不重复写（gap/idle 都已满足，只有 hash 门在拦）")

    s.ser.note("⑪d 播放门（R23）：先让播放无声，再 PLAY 中改 BPM ⇒ 只有 song.playing 能拦")
    ensure_silent_pattern(s)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑪ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑪ TAP PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑪ STATE SNAP playing=1")
    # 只停走带不改值的话，hash 与上次写相同 ⇒ **hash 门**也会拦住写入，删掉 playing 门
    # 照样绿（评审 R23）。所以这里改一个 proj_capture 真正捕获的值（全局 BPM），让 hash
    # 与上次写不同；此后 idle（2.5s）与 gap（20s，上次写已在 25s 之外）都满足，唯一还能
    # 拦住写入的就是 song.playing。
    autosave_bump_bpm(s, 120)
    live_play = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                           "⑪d 播放中的 LIVEHASH")
    st_play = autosave_fields(s)
    (s.ok if st_play["hash"] != live_play else s.fail)(
        f"⑪d 前提：autosave_hash={st_play['hash']:08X} != 当前 LIVEHASH={live_play:08X}"
        "（hash 门不成立 ⇒ 只剩 playing 门）")
    autosave_no_write(s, w2, 10.0,
                      "⑪d 播放中改 BPM 后 10s 不写（hash 已不同、idle/gap 满足，只有 playing 能拦）")
    s.cmd("ENG SNAP")
    voices_play = [int(v) for v in re.findall(r"ENG SNAP trk k=\d+ .*voices=(\d+)", s.last)]
    (s.ok if voices_play and max(voices_play) == 0 else s.fail)(
        f"⑪d 窗口前前提：播放中无发声 voices={voices_play}"
        "（0 ⇒ audio_quiet 恒成立，只有 song.playing 能拦）")
    autosave_no_write(s, w2, 3.0, "⑪d 播放中再 3s 不写")
    s.cmd("ENG SNAP")
    voices_play2 = [int(v) for v in re.findall(r"ENG SNAP trk k=\d+ .*voices=(\d+)", s.last)]
    (s.ok if voices_play2 and max(voices_play2) == 0 else s.fail)(
        f"⑪d 窗口后前提：仍无发声 voices={voices_play2}（整个窗口 audio_quiet 都成立）")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑪ TAP STOP")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑪ STATE SNAP playing=0")
    # 收尾（必须在 STOP 之后：select_track 会把当前页切到 TRACKS，STOP 的 TAP 就命不中了）：
    # 把 pattern 长度归回默认 16——⑪d 只是为了让窗口里没有发声才缩到 1 步。不归的话它会随
    # autosave 进入下一轮的开机状态，② h0 就跨轮漂移（实测踩到：h0=D160C9B7 → 7F23D623）。
    for k in range(3):
        select_track(s, k)
        store_cmd(s, f"PARAM {ENG_P_SLEN} {ENG_SLEN_DEF}",
                  rf"^PARAM track={k} id={ENG_P_SLEN} value={ENG_SLEN_DEF} "
                  rf"applied={ENG_SLEN_DEF} ", f"⑪d 轨 {k} P_SLEN 归默认 {ENG_SLEN_DEF}")
    select_track(s, 0)

    s.ser.note("⑪e 发声门（audio_quiet）：按住 C 改值、先睡过 idle，再观察 3s 不写")
    s.cmd("KEY 0x42 down")
    s.expect(r"KEY 0x42 down", "⑪ KEY 0x42 down（按住 C）")
    s.cmd("ENG SNAP")
    voices = int(re.search(r"ENG SNAP trk k=0 .*voices=(\d+)", s.last, re.MULTILINE).group(1))
    (s.ok if voices >= 1 else s.fail)(f"⑪ 按住 C 时 voices={voices}（≥1 = 确实在发声）")
    autosave_bump_bpm(s, 130)
    # 先睡过 idle（改值即活动）：这样**整个**观察窗里 idle 与 gap 都已满足，唯一能拦住
    # 写入的是 audio_quiet()（发声）。不先睡的话，窗口前 2.5s 是 idle 在拦（评审 minor 4）。
    time.sleep(AUTOSAVE_IDLE_S + 1.0)
    autosave_no_write(s, w2, 3.0,
                      "⑪e 发声时 3s 不写（idle 已过期、gap 满足，只有 audio_quiet 能拦）")
    s.cmd("KEY 0x42 up")
    s.expect(r"KEY 0x42 up", "⑪ KEY 0x42 up（松开）")
    w3 = w2 + 1
    elapsed = autosave_wait_writes(s, w3, 12.0, "⑪e 释放尾音结束后写一次")
    (s.ok if AUTOSAVE_IDLE_S <= elapsed <= 6.0 else s.fail)(
        f"⑪e 纯 idle 写实测 {elapsed:.1f}s（idle 2.5s + 一拍 1s 任务；上一次写已在 20s 之外）")

    s.ser.note("⑪f 开关门：AUTO OFF 后改值、等过 gap（26s）再观察 5s 不写 → AUTO ON 开闸即写")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑪ STORE AUTO OFF")
    st = autosave_fields(s)
    (s.ok if st["on"] == 0 else s.fail)("⑪ STORE STATS autosave=0")
    bpm_off = autosave_bump_bpm(s, 140)
    time.sleep(AUTOSAVE_GAP_S + 6.0)  # 26s：idle 2.5s 与 gap 20s 都已满足
    autosave_no_write(s, w3, 2.5, "⑪f AUTO OFF：过 gap 后 2.5s 不写")
    autosave_no_write(s, w3, 2.5, "⑪f AUTO OFF：再过 2.5s 仍不写（5s 窗口）")
    # 开关一开就应立刻写（idle/gap 都早已满足）：这里**不再改值**——改值在 OFF 期间已经
    # 做了，若 ON 后再改一次，可能正好落在 tick 之后、让「刚写下的」与「当前状态」错拍。
    store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⑪ STORE AUTO ON（恢复）")
    w4 = w3 + 1
    elapsed = autosave_wait_writes(s, w4, 8.0, "⑪f AUTO ON 后立即写一次（开关在拦，不是 gap）")
    (s.ok if elapsed <= 5.0 else s.fail)(
        f"⑪f 开闸即写（实测 {elapsed:.1f}s ≤ 5s：idle/gap 早已满足，写的是 OFF 期间改的现状）")
    st = autosave_fields(s)
    live = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "⑪f 写入后的 LIVEHASH")
    (s.ok if st["hash"] == live else s.fail)(
        f"⑪f autosave_hash={st['hash']:08X} == LIVEHASH={live:08X}（写的就是现状 bpm={bpm_off}）")

    s.ser.note("⑪g 复位恢复：硬复位 → LIVEHASH/bpm/步表回来，且开机不重写")
    step_saved = ensure_step_note(s, AUTOSAVE_STEP_TRACK, AUTOSAVE_STEP_COL)
    (s.ok if step_saved == step_before else s.fail)(
        f"⑪ 步表未被本段改动（{step_saved} == {step_before}）")
    bpm_saved = store_bpm(s)
    (s.ok if bpm_saved == bpm_off else s.fail)(f"⑪g 复位前 bpm={bpm_saved} == {bpm_off}")

    s.reboot()
    store_cmd(s, "STORE STATS", r"^STORE STATS ready=1 ", "⑪g 复位后 STORE STATS ready=1")
    st = autosave_fields(s)
    (s.ok if st["writes"] == 0 else s.fail)(
        f"⑪g 本次开机 autosave_writes={st['writes']}（0 = 还没写）")
    (s.ok if st["hash"] == live else s.fail)(
        f"⑪g resume 记住的 autosave_hash={st['hash']:08X} == 复位前的 {live:08X}")
    got = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "⑪g 复位后 LIVEHASH")
    (s.ok if got == live else s.fail)(f"⑪g 复位后 LIVEHASH={got:08X} == {live:08X}（工作工程回来了）")
    (s.ok if store_bpm_settled(s, bpm_saved) else s.fail)(
        f"⑪g STATE SNAP bpm={bpm_saved}（工程内的值）")
    step_after = step_fields(s, AUTOSAVE_STEP_TRACK, AUTOSAVE_STEP_COL)
    (s.ok if step_after == step_saved else s.fail)(
        f"⑪g ENG STEP {AUTOSAVE_STEP_TRACK} {AUTOSAVE_STEP_COL} 复位后 {step_after} == 复位前 {step_saved}")

    s.ser.note("⑪h 开机不重写：等过 boot 的 20s gap，autosave_writes 仍为 0")
    time.sleep(AUTOSAVE_GAP_S + 5.0)
    st = autosave_fields(s)
    (s.ok if st["writes"] == 0 else s.fail)(
        f"⑪h boot gap 过后 autosave_writes={st['writes']}（0 = resume 的 hash 让第一拍跳过）")
    (s.ok if st["hash"] == live else s.fail)(
        f"⑪h autosave_hash 仍为 {st['hash']:08X}")


# ---- FM6 银行 27 槽（上游 fm6_bank.c，Task 7） ----
# console 契约（main/console.c 的 STORE BANK）：
#   STORE BANK LIST     -> 27 行 "B<k> USED" / "B<k> EMPTY" + "BANK COUNT used=n total=27"
#   STORE BANK PUT n    -> "BANK n SAVED"（选中轨 live patch；播放中 "STORE ERR playing"）
#   STORE BANK GET n    -> "BANK n GET <256 hex>"；空槽 "STORE ERR empty"
#   STORE BANK LIVE     -> "BANK LIVE <256 hex>"（选中轨当前 packed patch，不写 flash）
#   STORE BANK ERASE n  -> "BANK n ERASED"
# 上游语义：27 个 128B packed patch 在一个 storage.c 对象里（A/B 0xE5000/0xE6000）；
# **PTCH = 8 + 槽号**（fm6_slot_get：< 8 是工厂槽 F1..F8）。PTCH 是 FM6 的 P_E7
# （core.h 的引擎槽第 8 项，参数 id 70）——`ENG TRACK 1 9 <preset>` 选的是**工厂预置**
# （FM6_PRESETS[i] 把 P_E7 写成 i），故 PTCH=11 要经 `PARAM 70 11`。
BANK_SLOTS = 27
BANK_PACKED_HEX = 256   # 128B packed 记录 = 256 个 hex 字符
BANK_PTCH_ID = 70       # P_E7：FM6 引擎的 8 号槽（描述符名 PTCH，范围 0..34）
BANK_PTCH_BASE = 8      # PTCH ≥ 8 读银行（FM6_NFACTORY）


def bank_hex(s: Session, cmd: str, pattern: str, label: str) -> str:
    """发命令并取回显里的 hex 记录。

    回显里**没有** hex 行（命令缺失/回 STORE ERR）时抛 InjectError——拿不到值就没法继续
    断言（RED 阶段固件还没有该命令，就在这里失败）；长度不是 128 字节只记 FAIL 并返回，
    让后续的字节比较也把差异暴露出来（不掩盖「读到了但读错了」）。"""
    s.cmd(cmd)
    m = re.search(pattern, s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"{label}: 回显缺少 hex 记录（{s.last.strip()!r}）")
    val = m.group(1).upper()
    (s.ok if len(val) == BANK_PACKED_HEX else s.fail)(
        f"{label}: {len(val)} hex 字符（{BANK_PACKED_HEX // 2} 字节 packed 记录）")
    return val


def bank_live(s: Session) -> str:
    return bank_hex(s, "STORE BANK LIVE", r"^BANK LIVE ([0-9A-Fa-f]+)\r?$", "STORE BANK LIVE")


def bank_get(s: Session, slot: int) -> str:
    return bank_hex(s, f"STORE BANK GET {slot}", rf"^BANK {slot} GET ([0-9A-Fa-f]+)\r?$",
                    f"STORE BANK GET {slot}")


def bank_list(s: Session) -> tuple:
    """`STORE BANK LIST` → ({槽号: used}, (used, total))。缺行由调用方的数量断言暴露。"""
    s.cmd("STORE BANK LIST")
    used = {}
    for raw in s.last.splitlines():
        m = re.match(r"^B(\d+) (USED|EMPTY)$", raw.strip())
        if m:
            used[int(m.group(1))] = m.group(2) == "USED"
    m = re.search(r"^BANK COUNT used=(\d+) total=(\d+)\r?$", s.last, re.MULTILINE)
    count = (int(m.group(1)), int(m.group(2))) if m else None
    return used, count


def bank_wait_live(s: Session, want: str, timeout: float = 1.0) -> str:
    """轮询 `STORE BANK LIVE` 到 want：PTCH 改动后 patch 由渲染拍的 fm6_poll 载入
    （`fm6_poll` 在主循环/引擎锁内），命令返回不等于已经过了下一拍。"""
    deadline = time.monotonic() + timeout
    while True:
        got = bank_live(s)
        if got == want or time.monotonic() >= deadline:
            return got
        time.sleep(0.02)


def bank_normalize_fm6(s: Session, preset: int = 0) -> None:
    """把选中轨归一到 FM6 工厂预置 `preset`（FM6_PRESETS[i] 把 P_E7 写成 i ⇒ PTCH=i ⇒
    工厂 patch F<preset+1>，同步载入）。

    **两步**是必须的：`sloop_core_track` 只在 (engine, preset) 变化时应用预置，而
    `fm6_track_loaded` 才把 patch 同步写进 fm6_patch（引擎未变则是无操作）。上一轮结束时
    轨 1 就是 FM6 的某个预置（只是 PTCH 被改成 11），直接 `ENG TRACK 1 9 n` 会什么都不做、
    LIVE 读到的仍是银行槽的 patch。先换到 DIGITAL 再换回 FM6 ⇒ 必定走一次引擎变更。
    """
    s.cmd("ENG TRACK 1 1 0")
    s.expect(r"ENG TRACK k=1 engine=1 preset=0 OK",
             "⑫ ENG TRACK 1 1 0（先离开 FM6：保证下一步是「换引擎」而不是无操作）")
    s.cmd(f"ENG TRACK 1 9 {preset}")
    s.expect(rf"ENG TRACK k=1 engine=9 preset={preset} OK",
             f"⑫ ENG TRACK 1 9 {preset}（FM6 预置 {preset} ⇒ PTCH={preset} ⇒ 工厂 patch "
             f"F{preset + 1} 同步载入）")


def run_store_bank(s: Session) -> None:
    """⑫ FM6 银行 27 槽（上游 fm6_bank.c，Task 7）：

    ① 归一：`STORE AUTO OFF`（本段改轨 1 的引擎，AUTO 开着会把它写进 autosave、漂到下一轮
       的 ② h0）；槽 3/4/5 若 USED 先 ERASE；选中轨 1 → FM6 工厂预置 0（`bank_normalize_fm6`）
       ⇒ `STORE BANK LIVE` = L（工厂 patch F1 的 packed 记录）；空槽 GET 3 ⇒ `STORE ERR empty`；
    ② PUT 3（L）→ GET 3 == L（PUT/GET 往返字节一致）→ LIST `B3 USED` + COUNT used=1；
    ③ 换工厂预置 1（PTCH=1 ⇒ patch F2）⇒ L2 ≠ L（两个槽的内容可区分）→ PUT 4（L2）⇒
       GET 3 仍是 L（整银行重写不碰别的槽）、GET 4 == L2、LIST B3+B4、COUNT used=2；
    ④ 单例 ENG SELFTEST 之后**紧跟** PUT 5（中间不做任何银行读）：自检 restore 把
       `fm6_bank_cur/fm6_bank_used`（fm6_bank.c 的普通 .bss）清成 -1/0，写路径若不重扫就会
       从空副本起笔、把 B3/B4 抹掉——LIST 必须仍是 B3+B4+B5（ADR-009 §8 的判别口）；
    ⑤ ERASE 5 ⇒ B3+B4、GET 5 → empty；从未用过的槽 GET 26 → empty；
    ⑥ esptool 硬复位（`fm6_bank_boot` 的开机扫描证据）⇒ LIST 仍 B3+B4、GET 3/4 字节不变；
    ⑦ PTCH=11（= bank 槽 3）⇒ `STORE BANK LIVE` 从 F2（归一到 PTCH=1 时同步载入的工厂
       patch）变成槽 3 的 L（**银行槽的字节真的载入了轨道**，不是 init voice、也不是停在
       F2——R24：归一到 F1 会让这条断言假绿）+ `ENG TRIG synth 1 60` 出声无错；
    ⑧ ERASE 3/4 ⇒ COUNT used=0、GET 3 → empty；槽号越界（27）⇒ `STORE ERR invalid`；
    ⑨ 播放守卫：PLAY 中 PUT/ERASE ⇒ `STORE ERR playing`，停止后 COUNT 仍 0（未写）；
    ⑩ 鼓轨（选中轨 3）PUT/LIVE ⇒ `STORE ERR drum`（fm6_patch 只对合成轨存在）。
    """
    s.ser.note("⑫ FM6 银行 27 槽：live patch → PUT/GET → LIST → 自检后写路径 → 复位 → PTCH=11")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑫ STORE AUTO OFF（本段免自动存档干扰）")
    select_track(s, 1)
    bank_normalize_fm6(s)

    used, _ = bank_list(s)
    for k in (3, 4, 5):
        if used.get(k):
            store_cmd(s, f"STORE BANK ERASE {k}", rf"^BANK {k} ERASED\r?$", f"⑫ 归一 ERASE {k}")
    used, count = bank_list(s)
    (s.ok if set(used) == set(range(BANK_SLOTS)) else s.fail)(
        f"⑫ LIST 列出全部 {BANK_SLOTS} 槽（实得 {len(used)}）")
    (s.ok if not any(used.values()) and count == (0, BANK_SLOTS) else s.fail)(
        f"⑫ 归一后 27 槽全 EMPTY（实得 used={[k for k, v in used.items() if v]} count={count}）")

    s.ser.note("⑫a 空槽 GET / LIVE：空槽回 STORE ERR empty，LIVE = 工厂 patch F1 的 packed 记录")
    store_cmd(s, "STORE BANK GET 3", r"^STORE ERR empty\r?$", "⑫ 空槽 GET 3 → STORE ERR empty")
    live1 = bank_live(s)
    print(f"  [NOTE] ⑫ L(F1)={live1[:32]}…")

    s.ser.note("⑫b PUT 3（选中轨 live patch）→ GET 3 == LIVE")
    store_cmd(s, "STORE BANK PUT 3", r"^BANK 3 SAVED\r?$", "⑫ PUT 3")
    (s.ok if bank_get(s, 3) == live1 else s.fail)("⑫ GET 3 == LIVE（PUT/GET 往返字节一致）")
    used, count = bank_list(s)
    (s.ok if used.get(3) and count == (1, BANK_SLOTS) else s.fail)(
        f"⑫ LIST → B3 USED、COUNT used=1（实得 {used.get(3)} count={count}）")

    s.ser.note("⑫c 换工厂预置 1（PTCH=1 ⇒ F2）→ L2 ≠ L → PUT 4：不碰槽 3")
    s.cmd("ENG TRACK 1 9 1")
    s.expect(r"ENG TRACK k=1 engine=9 preset=1 OK", "⑫ ENG TRACK 1 9 1（FM6 预置 1 ⇒ PTCH=1）")
    live2 = bank_live(s)
    (s.ok if live2 != live1 else s.fail)(
        f"⑫ 工厂 patch F2 的 packed 记录 ≠ F1（{live2[:16]}… vs {live1[:16]}…）")
    store_cmd(s, "STORE BANK PUT 4", r"^BANK 4 SAVED\r?$", "⑫ PUT 4（F2）")
    (s.ok if bank_get(s, 3) == live1 else s.fail)("⑫ PUT 4 后 GET 3 不变（整银行重写不碰别的槽）")
    (s.ok if bank_get(s, 4) == live2 else s.fail)("⑫ GET 4 == LIVE2")
    used, count = bank_list(s)
    (s.ok if used.get(3) and used.get(4) and count == (2, BANK_SLOTS) else s.fail)(
        f"⑫ LIST → B3+B4 USED、COUNT used=2（实得 {sorted(k for k, v in used.items() if v)} "
        f"count={count}）")

    s.ser.note("⑫d 单例 SELFTEST 后**紧跟** PUT 5（中间不做银行读）：写路径必须自己重扫")
    s.cmd("ENG AUDIO OFF")
    s.expect(r"ENG AUDIO streaming=0", "⑫ ENG AUDIO OFF")
    s.cmd("ENG SELFTEST preset/ANALOG/00_808_BOOM", timeout=90)
    store_expect(s, r"^ENG SELFTEST .*result=PASS\r?$", "⑫ 单例 selftest PASS")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "⑫ ENG AUDIO ON")
    # selftest restore 把引擎回滚到工厂态（选中轨 0、轨 1 = DIGITAL）：重新归一到 FM6 F1；
    # 这两条命令都不读银行（PTCH=0 < FM6_NFACTORY 走工厂表），故 PUT 仍是「紧跟」。
    select_track(s, 1)
    bank_normalize_fm6(s)
    store_cmd(s, "STORE BANK PUT 5", r"^BANK 5 SAVED\r?$",
              "⑫ 自检后紧跟 PUT 5（未经过任何银行读）")
    used, count = bank_list(s)
    (s.ok if used.get(3) and used.get(4) and used.get(5) and count == (3, BANK_SLOTS) else s.fail)(
        f"⑫ 自检后 PUT 5：B3/B4/B5 均 USED（实得 "
        f"{sorted(k for k, v in used.items() if v)} count={count}）")
    (s.ok if bank_get(s, 3) == live1 else s.fail)("⑫ 自检后 GET 3 仍是 L（同银行其它槽未丢）")
    (s.ok if bank_get(s, 4) == live2 else s.fail)("⑫ 自检后 GET 4 仍是 L2")

    s.ser.note("⑫e ERASE 5 → B3+B4；GET 5/26 → STORE ERR empty")
    store_cmd(s, "STORE BANK ERASE 5", r"^BANK 5 ERASED\r?$", "⑫ ERASE 5")
    used, count = bank_list(s)
    (s.ok if used.get(3) and used.get(4) and not used.get(5) and count == (2, BANK_SLOTS) else s.fail)(
        f"⑫ ERASE 5 后 B3+B4 USED、B5 EMPTY（实得 "
        f"{sorted(k for k, v in used.items() if v)} count={count}）")
    store_cmd(s, "STORE BANK GET 5", r"^STORE ERR empty\r?$", "⑫ ERASE 后 GET 5 → empty")
    store_cmd(s, "STORE BANK GET 26", r"^STORE ERR empty\r?$", "⑫ 从未用过的槽 GET 26 → empty")

    s.ser.note("⑫f esptool 硬复位 → LIST 仍 B3+B4（fm6_bank_boot 的开机扫描）+ 字节不变")
    s.reboot()
    store_cmd(s, "STORE STATS", r"^STORE STATS ready=1 ", "⑫ 复位后 STORE STATS ready=1")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑫ 复位后 STORE AUTO OFF")
    used, count = bank_list(s)
    (s.ok if used.get(3) and used.get(4) and count == (2, BANK_SLOTS) else s.fail)(
        f"⑫ 复位后 LIST → B3+B4 USED（实得 {sorted(k for k, v in used.items() if v)} count={count}）")
    (s.ok if bank_get(s, 3) == live1 else s.fail)("⑫ 复位后 GET 3 == L（flash 里的字节）")
    (s.ok if bank_get(s, 4) == live2 else s.fail)("⑫ 复位后 GET 4 == L2")

    # R24：归一到 PTCH=0（F1）时，槽 3 里存的正是 F1 的字节——即使 PTCH→银行的重载是死的，
    # LIVE 也仍是归一时同步载入的 F1，断言会**假绿**。故先把轨归一到 PTCH=1（工厂 F2）、
    # 断言 LIVE 是 F2 的字节（且 ≠ 槽 3 的 F1），再 `PARAM 70 11`：LIVE 必须从 F2 变成槽 3 的
    # F1'（= ⑫b 存下的 L）——只有「真的去读了银行」才会发生。
    s.ser.note("⑫g PTCH=1（工厂 F2）→ PARAM 70 11（银行槽 3）⇒ LIVE 从 F2 变成槽 3 的 F1'")
    select_track(s, 1)
    bank_normalize_fm6(s, 1)
    live_f2 = bank_live(s)
    (s.ok if live_f2 == live2 else s.fail)(
        f"⑫ 前提：PTCH=1 的 LIVE 是 F2 的字节（== ⑫c 的 L2 {live2[:16]}…；实得 {live_f2[:16]}…）")
    (s.ok if live_f2 != live1 else s.fail)(
        f"⑫ 前提：F2（{live_f2[:16]}…）≠ 槽 3 里的 F1（{live1[:16]}…）⇒ 切换可观测")
    s.cmd(f"PARAM {BANK_PTCH_ID} 11")
    s.expect(rf"PARAM track=1 id={BANK_PTCH_ID} value=11 applied=11 ", "⑫ PARAM 70 11（PTCH=11）")
    got = bank_wait_live(s, live1)
    (s.ok if got == live1 else s.fail)(
        f"⑫ PTCH=11 后 LIVE 轮询到槽 3 的字节（实得 {got[:16]}…，期望 {live1[:16]}…）")
    (s.ok if got != live_f2 else s.fail)(
        f"⑫ 真的发生了 F2 → 槽 3 的切换（不是停在归一后的 F2：{got[:16]}…）")
    s.cmd("ENG TRIG synth 1 60")
    store_expect(s, r"^ENG TRIG synth track=1 note=60 .* OK\r?$", "⑫ PTCH=11 时 ENG TRIG 出声无错")

    s.ser.note("⑫h ERASE 3/4 → COUNT used=0；槽号越界 → STORE ERR invalid")
    store_cmd(s, "STORE BANK ERASE 3", r"^BANK 3 ERASED\r?$", "⑫ ERASE 3")
    store_cmd(s, "STORE BANK ERASE 4", r"^BANK 4 ERASED\r?$", "⑫ ERASE 4")
    used, count = bank_list(s)
    (s.ok if not any(used.values()) and count == (0, BANK_SLOTS) else s.fail)(
        f"⑫ 收口后 27 槽全 EMPTY（实得 {sorted(k for k, v in used.items() if v)} count={count}）")
    store_cmd(s, "STORE BANK GET 3", r"^STORE ERR empty\r?$", "⑫ ERASE 后 GET 3 → empty")
    store_cmd(s, "STORE BANK GET 27", r"^STORE ERR invalid\r?$", "⑫ 槽号越界 GET 27 → invalid")
    store_cmd(s, "STORE BANK PUT 27", r"^STORE ERR invalid\r?$", "⑫ 槽号越界 PUT 27 → invalid")
    store_cmd(s, "STORE BANK ERASE 27", r"^STORE ERR invalid\r?$", "⑫ 槽号越界 ERASE 27 → invalid")

    s.ser.note("⑫i 播放守卫：PLAY 中 PUT/ERASE ⇒ STORE ERR playing，存储不变")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑫ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑫ TAP PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑫ STATE SNAP playing=1")
    store_cmd(s, "STORE BANK PUT 3", r"^STORE ERR playing\r?$", "⑫ 播放中 PUT → playing")
    store_cmd(s, "STORE BANK ERASE 3", r"^STORE ERR playing\r?$", "⑫ 播放中 ERASE → playing")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑫ UI transport（收口）")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑫ TAP STOP")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑫ STATE SNAP playing=0")
    used, count = bank_list(s)
    (s.ok if not any(used.values()) and count == (0, BANK_SLOTS) else s.fail)(
        f"⑫ 被拒的 PUT/ERASE 未写存储（实得 {sorted(k for k, v in used.items() if v)} count={count}）")

    s.ser.note("⑫j 鼓轨（选中轨 3）：fm6_patch 只对合成轨存在 ⇒ PUT/LIVE 回 STORE ERR drum")
    select_track(s, 3)
    store_cmd(s, "STORE BANK PUT 3", r"^STORE ERR drum\r?$", "⑫ 鼓轨 PUT → STORE ERR drum")
    store_cmd(s, "STORE BANK LIVE", r"^STORE ERR drum\r?$", "⑫ 鼓轨 LIVE → STORE ERR drum")
    select_track(s, 0)


SCENARIOS = {
    "transport": run_transport,
    "params": run_params,
    "step_edit": run_step_edit,
    "tracks": run_tracks,
    "nav": run_nav,
    "store": run_store,
}


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口注入序列（AGENTS.md §5.4）")
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS),
                    help="注入场景（六条 .sh 各对应一个）")
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
