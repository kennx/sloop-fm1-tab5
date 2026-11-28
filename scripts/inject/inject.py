#!/usr/bin/env python3
"""inject.py — Tab5 固件串口注入序列（AGENTS.md §5.4 注入约定）

十一条场景脚本（transport.sh / step_edit.sh / tracks.sh / nav.sh / params.sh / store.sh /
song.sh / sequence.sh / synth.sh / sys.sh / sound.sh）是本文的薄封装。每个场景：
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

用法（一般经各场景脚本调用）：
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
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))
import screenshot as shot  # noqa: E402
import gen_project_fixtures as fixtures  # noqa: E402
import gen_settings_fixture as settings_fixtures  # noqa: E402
import store_raw  # noqa: E402

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
        self._log_path = log_path
        self._log = open(log_path, "ab")
        self.silent = False

    def raw(self, data: bytes) -> None:
        """端口关闭期间（复位 → 等 boot）补写串口原始字节：transcript 仍是逐字节回显。"""
        if not data:
            return
        log = open(self._log_path, "ab")
        try:
            log.write(data)
        finally:
            log.close()

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


# 固件异步日志行（esp_log：`I (1234) tag: ...`）——显示/引擎任务在 console 之后启动，
# 提示符打印后仍可能紧跟一行日志；这种尾随日志不改变「命令已执行完、提示符已出现」，
# 若按「尾部必须恰以 sloop> 结束」判定会把正常启动误报成超时（store 首个
# `ENG AUDIO ON` 的现场：响应与提示符都已到达，只差尾部多一行 sloop_display 日志）。

def _prompt_at(buf: bytearray) -> bool:
    """提示符本身表示上一条命令已完成；其后可能立即跟任意异步输出（SELFTEST
    的逐例哈希并不都是 ESP_LOG 格式），不能再要求尾部只含空白/日志行。"""
    return buf.rfind(b"sloop>") >= 0


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
        # 场景快照的基线 {名字: 任意值}：**跨 ScenarioRestart 存活**（终审 C-I1）。工作工程
        # 快照用 (hash0, 槽入口是否为空)，见 proj_snapshot_open；其它跨重跑只能取一次的值
        # （如 params ⑪ 入口的歌曲顺序）走 session_once。
        self.snapshots: dict = {}
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
                read_response(self.ser, 1.0)  # 提示符由 _prompt_at 判定（允许尾随日志）
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
        store_early_load_probe（wait_boot 会把命令推迟到 init 之后）。

        启动期间的串口字节写进 transcript（原始回显）并留一份 `boot_text`
        （`boot_version()` 取 IDF 的 `App version:`）。"""
        self.ser.note("复位设备（esptool flash_id --after hard_reset，不写 flash）；"
                      "以下至 console 就绪为启动日志原始回显")
        self._reset_and_reopen()

    def boot_version(self) -> str:
        """最近一次 reboot() 的 `App version:`（IDF app_init 行；缺失回空串）。"""
        m = re.search(r"App version:\s+(\S+)", getattr(self, "boot_text", ""))
        return m.group(1) if m else ""

    # ---- 截屏与像素抽检 ----
    def _reset_and_reopen(self) -> None:
        self.ser.close()  # 关日志 + 关端口，esptool 才能进入 bootloader
        chunks = []

        def on_chunk(data: bytes) -> None:
            chunks.append(data)
            self.ser.raw(data)
        try:
            shot.reset_device(self.port, BAUD, on_chunk=on_chunk)
        except shot.ResetError as e:
            raise InjectError(f"复位失败，无法从截屏传输停滞恢复: {e}") from None
        self.boot_text = b"".join(chunks).decode(errors="replace")
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
# 页签条几何（ADR-005「导航决策」两行页签；ui_nav.h UI_GROUP_*/UI_SUB_* 同源）：
# 上行组签 y∈[4,36)、**104px** 宽、间距 8（9×104+8×8=1,000，右端 1,016 ≤ 1,280−216；
# P4-F Task 4 起 9 组（新增 MIX），122→104 见 ADR-005 §2 注）；下行子签 y∈[38,68)、
# 176px，只显示当前组已注册页（固定 5 槽复用，右端 928）。页面区起点仍是 y=72，
# 页内几何全部不变。
# 探针取页签**左上内缘**（x = 签左 + 20，y = 行顶 + 4）：避开 10px 圆角与 16pt
# 文本行（组签文本 ≈10..30，子签 ≈43..63），命中填充色（选中 UI_ACCENT / 未选中
# UI_PANEL_HI）而不是字形抗锯齿。
UI_TAB_X0, UI_TAB_GAP = 16, 8
UI_GROUP_W, UI_GROUP_H, UI_GROUP_Y = 104, 32, 4
UI_SUB_W, UI_SUB_H, UI_SUB_Y = 176, 30, 38
UI_GROUP_SLOTS = 5                # 下行子签固定 5 槽（ui_nav.h UI_GROUP_MAX）
UI_GROUP_NAMES = ("PLAY", "TRACKS", "EDIT", "MIX", "SYNTH", "SEQ", "SONG", "FILE", "SYS")


def group_tab_xy(i: int) -> tuple:
    """上行组签 i 的中心（屏幕绝对坐标；仅点击用途）。"""
    return (UI_TAB_X0 + i * (UI_GROUP_W + UI_TAB_GAP) + UI_GROUP_W // 2,
            UI_GROUP_Y + UI_GROUP_H // 2)


def group_tab_probe(i: int) -> tuple:
    """上行组签 i 的左缘内探针（避开圆角/文本）。"""
    return (UI_TAB_X0 + i * (UI_GROUP_W + UI_TAB_GAP) + 20, UI_GROUP_Y + 4)


def sub_tab_probe(i: int) -> tuple:
    """下行子签 i（当前组内下标）的左缘内探针。"""
    return (UI_TAB_X0 + i * (UI_SUB_W + UI_TAB_GAP) + 20, UI_SUB_Y + 4)


# 单页组（PLAY/TRACKS/EDIT/MIX/SONG/SYS）的选中态在上行组签、子行留空（5B-1 起五张族页
# 退役，EDIT/MIX 也只剩一页）；多页组（SYNTH/SEQ/FILE）的选中态在下行子签。注意 C_TAB_STEP 与
# C_TAB_SAMPLE 坐标相同：都是「当前组第一张子签」，语义由当前组决定——探针只在对应组为当前
# 组时使用。
C_TAB_PLAY = group_tab_probe(0)
C_TAB_TRACKS = group_tab_probe(1)
C_TAB_EDIT = group_tab_probe(2)
C_TAB_MIX = group_tab_probe(3)     # 5B-1 起 MIX 只有 TOOLS
C_TAB_SYNTH = group_tab_probe(4)   # P4-D Task 8：SAMPLE/DSYN/FM6P 组
C_TAB_SEQ = group_tab_probe(5)
C_TAB_SONG = group_tab_probe(6)
C_TAB_FILE = group_tab_probe(7)
C_TAB_SYS = group_tab_probe(8)
C_TAB_STEP = sub_tab_probe(0)
C_TAB_TRANSPORT = sub_tab_probe(1)
C_TAB_SUB_EMPTY = sub_tab_probe(0)  # 单页组的子签槽 0：页签条底色（子行为空）
C_TAB_SAMPLE = sub_tab_probe(0)
C_TAB_DSYN = sub_tab_probe(1)
C_TAB_FM6P = sub_tab_probe(2)
C_TAB_PROJECTS = sub_tab_probe(0)
C_TAB_PRESETS = sub_tab_probe(1)
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


# 鼓格内的 16 lane 位（P4-C Task 9）：page_step.c 的 DRUM_GRID_X0/Y0/DRUM_LANE_PITCH/
# DRUM_LANE_DOT 同名同值（格内相对坐标）。4×4 网格，lane = row*4+col（与上游
# drums.c 的 lane 序号一致）；点中 dot 命中框 = 选中该 lane 并切换该步的原始位。
STEP_DRUM_GRID_X0 = 8
STEP_DRUM_GRID_Y0 = 30
STEP_DRUM_LANE_PITCH = 14
STEP_DRUM_LANE_DOT = 8


def drum_lane_xy(col: int, lane: int) -> tuple:
    """鼓格（窗口列 col、行 3）内 lane 位（0..15）的屏幕绝对中心。"""
    lx = STEP_DRUM_GRID_X0 + (lane % 4) * STEP_DRUM_LANE_PITCH + STEP_DRUM_LANE_DOT // 2
    ly = STEP_DRUM_GRID_Y0 + (lane // 4) * STEP_DRUM_LANE_PITCH + STEP_DRUM_LANE_DOT // 2
    return (STEP_GRID_X0 + col * STEP_CELL_PITCH + lx,
            STEP_GRID_Y0 + 3 * STEP_ROW_PITCH + ly)


# 格内文本/标记的局部锚点（page_step.c 的自绘布局同源；屏幕坐标用 step_cell_box 派生）
STEP_TOP_STRIP_Y = 4    # flags 行（A/S/F/N 字母）文字顶（16–20px 高）
STEP_NOTE_Y = 22        # 音符名两行：22 与 42
STEP_NOTE_ROW = 20
STEP_BOT_STRIP_Y = 84   # micro/rat 行文字顶


def step_cell_box(col: int, row: int) -> tuple:
    """步格左上角的屏幕绝对坐标（与建格同一组宏推导）。"""
    return (STEP_GRID_X0 + col * STEP_CELL_PITCH, STEP_GRID_Y0 + row * STEP_ROW_PITCH)

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

# TRANSPORT 工具区（P4-C Task 10；page_transport.c 的 TR_* 宏同名同值）。
# 录音/fill 行 y=384（页内）+56 高；聚焦步字段行 y=464、88 宽、间距 96（6 个
# 单对象 chip：LVL / RAT / MIC- / MIC+ / SPAN / SLIDE）；pattern 区 x=792 起：
# y=292 行 4×100（间距 8）、y=360 行 120/120/84/84、y=428 行 CLEAR 432 宽。
# TAP 坐标 = 页内坐标 + 页签条 72。
TR_REC = (48 + 70, 72 + 384 + 28)
TR_FILL = (200 + 60, 72 + 384 + 28)
TR_NEXT = (328 + 60, 72 + 384 + 28)
TR_REC_W, TR_REC_H = 140, 56
TR_FILL_W, TR_FILL_H = 120, 56
TR_PAT_H = 56
TR_STEP_X0, TR_STEP_Y, TR_STEP_W, TR_STEP_H, TR_STEP_PITCH = 24, 464, 88, 56, 96


def tr_step_chip(i: int) -> tuple:
    """聚焦步字段 chip i 的屏幕绝对中心（0 LVL / 1 RAT / 2 MIC- / 3 MIC+ /
    4 SPAN / 5 SLIDE）。"""
    return (TR_STEP_X0 + i * TR_STEP_PITCH + TR_STEP_W // 2,
            72 + TR_STEP_Y + TR_STEP_H // 2)


TR_LVL, TR_RAT = tr_step_chip(0), tr_step_chip(1)
TR_MIC_DN, TR_MIC_UP = tr_step_chip(2), tr_step_chip(3)
TR_SPAN, TR_SLIDE = tr_step_chip(4), tr_step_chip(5)

TR_PAT_ROW0, TR_PAT_ROW1, TR_PAT_ROW2 = 72 + 292 + 28, 72 + 360 + 28, 72 + 428 + 28
TR_ROT_DN = (842, TR_PAT_ROW0)
TR_ROT_UP = (950, TR_PAT_ROW0)
TR_LEN_X2 = (1058, TR_PAT_ROW0)
TR_LEN_D2 = (1166, TR_PAT_ROW0)
TR_TRANS_DN = (852, TR_PAT_ROW1)
TR_TRANS_UP = (980, TR_PAT_ROW1)
TR_UNDO = (1090, TR_PAT_ROW1)
TR_REDO = (1182, TR_PAT_ROW1)
TR_CLEAR = (1008, TR_PAT_ROW2)
# 默认窗口 {0,0,16} 下的两个编辑格：轨 0 窗口列 2（绝对步 2）/ 轨 1 列 5（绝对步 5）。
# soak.py 直接复用这两个点，故保留模块级常量（窗口默认口径下与 step_cell_xy 一致）。
STEP_CELL_A = step_cell_xy(2, 0)
STEP_CELL_B = step_cell_xy(5, 1)
# 卡 2：x=CARD_X0+2*(CARD_W+16)+CARD_W/2=24+624+148；y=72+CARD_Y+CARD_H/2=72+40+235
TRACKS_CARD2 = (796, 347)
# 卡距：page_tracks.c CARD_PITCH = CARD_W + 16 = 312（soak 循环选卡按此换算）
TRACKS_CARD_PITCH = 312

# 页签条（theme.h UI_GROUP_*/UI_SUB_* + ui.c 的两行页签建成）：上行组签 i 的 x 区间 =
# UI_TAB_X0 + i*(UI_GROUP_W+UI_TAB_GAP) .. +UI_GROUP_W，y = UI_GROUP_Y .. +UI_GROUP_H。
# nav 用例①取相邻组签之间 8px 缝的中心：在页签条内、却在所有组签/子签矩形之外
# （TAP 对 y<72 一律 miss，页签切换走 LVGL 回调/UI/KEY tab）。
def tab_gap_xy(i: int) -> tuple:
    """组签 i 与 i+1 之间缝隙的中心（屏幕绝对坐标）。"""
    return (UI_TAB_X0 + (i + 1) * (UI_GROUP_W + UI_TAB_GAP) - UI_TAB_GAP // 2,
            UI_GROUP_Y + UI_GROUP_H // 2)


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
    """① TRANSPORT：播放 + BPM 步进 → 截屏；再切 STEP 截播放头；最后复位为停止。

    前置：`normalize_pattern()`（LEN=16/DIV=2）——播放头「各轨同列」的判据依赖它，
    设备上的 autosave 可能带着别的工程的 LEN/DIV（场景假设，原 DI-23 已按此思路收口）。"""
    normalize_pattern(s)
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

    # 停止态跑工具区（P4-C Task 10）：REC/fill 的状态机、聚焦步字段与 pattern
    # 工具都需要确定性的停止态；结束复位为停止（下面的播放头用例再自行起播）。
    run_transport_tools(s)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（回到走带用例）")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑩ 工具区收口：停止")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑩ 工具区收口：playing=0")

    # 播放中切 STEP：先归一窗口（窗口是持久 UI 状态；播放头列扫描按 window_len 几何）。
    # 播放头由引擎 step_pos 驱动；SCR 抓的是"上一已刷新帧"（≤30ms 旧），所以每帧的
    # 播放头列应等于抓屏前 ENG SNAP 的 pos（或紧邻 1–2 列）；两帧的 pos 不同即证明
    # 播放推进（16 列环绕会让"列号不同"本身不可靠，故以引擎为锚）。
    wlen = ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    time.sleep(0.4)  # ≥10 个 30ms tick，播放头与步位置同步稳定

    def ensure_playing(label):
        """起播前提自愈：幽灵按键（DI-10 同类）或前序中断可能把走带停掉；
        重新 KEY space 起播并记注记（不是放宽断言——播放头判据仍以引擎 pos 为锚）。"""
        s.cmd("STATE SNAP")
        if s.snap_int(r"playing=(\d)") == 1:
            return
        s.ser.note(f"{label}：引擎不在播放（幽灵按键/前序中断？）→ KEY space 重新起播")
        s.cmd("KEY 0x4D down")
        s.cmd("KEY 0x4D up")
        s.cmd("STATE SNAP")
        s.expect_int(r"playing=(\d)", 1, f"{label}：重新起播")
        time.sleep(0.4)

    def shot_with_pos(label):
        ensure_playing(label)
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
        if r in PLAYHEAD_AMBIGUOUS_ROWS:
            ok_a = any(c in want_a for c in cls_a[r])
            ok_b = any(c in want_b for c in cls_b[r])
        else:
            ok_a = len(cls_a[r]) == 1 and cls_a[r][0] in want_a
            ok_b = len(cls_b[r]) == 1 and cls_b[r][0] in want_b
        s.ok(f"行 {r}: 帧 A 列 {cls_a[r]}（pos={pre_a}）/ 帧 B 列 {cls_b[r]}（pos={pre_b}）")
        if not ok_a or not ok_b:
            s.fail(f"行 {r} 播放头列与引擎 step_pos 不符（帧 A 允许 {sorted(want_a)}，"
                   f"帧 B 允许 {sorted(want_b)}）")
    # 播放推进的判据：引擎 pos 读数或**两帧的播放头列**任一变化即可。pos 是取帧前的
    # ENG SNAP 读数、循环 1s（240 BPM×16 步），两次 16s 级的截屏恰好落在同一相位是
    # 1/16 概率的采样巧合；帧列是独立观测（本次实测 pos 均 11、帧列 12→13），用它兜底
    # 不是放宽断言——两帧各行的列仍必须落在各自 pos 的 ±2 窗口内。
    advanced = pre_a != pre_b or cls_a != cls_b
    (s.ok if advanced else s.fail)(
        f"播放推进：引擎 pos {pre_a} -> {pre_b}，帧列 {cls_a} -> {cls_b}"
        if advanced else f"两帧均未推进（pos={pre_a} 列={cls_a}）")

    # 复位：停止并回 PLAY 页（脚本可重复运行）
    s.cmd("UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "TAP STOP（复位）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "STATE SNAP playing（复位）")
    s.cmd("UI play")


# LVL 的响度序（page_transport.c 的 lvl_step 同源）：raw 1 GHOST < 2 SOFT < 0 NORM < 3 HARD
TR_LVL_ORDER = (1, 2, 0, 3)


def tr_lvl_step(raw: int, up: bool) -> int:
    i = TR_LVL_ORDER.index(raw & 3)
    return TR_LVL_ORDER[(i + (1 if up else 3)) % 4]


def tr_focus_step(s: Session, track: int, step: int) -> None:
    """在 STEP 页用键盘把焦点归一到 (track, step)（只写 UI 局部态、不发引擎写），
    再回 TRANSPORT 页。"""
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step（焦点归一）")
    s.cmd("STATE SNAP")
    t0, s0 = s.snap_int(r"focus_track=(\d+)"), s.snap_int(r"focus_step=(\d+)")
    if t0 >= 3:  # 鼓轨上 ↑ 逐 lane 离开（最多 16 lane + 一步）
        for _ in range(17):
            s.cmd("KEY 0x3B down")
            s.cmd("KEY 0x3B up")
            s.cmd("STATE SNAP")
            t0 = s.snap_int(r"focus_track=(\d+)")
            if t0 < 3:
                break
    # 焦点环不是三轨循环：T1↑→DRM L16、T3↓→DRM L1，所以按方向走（T3→T1 用 ↑ 两次，
    # T1→T3 用 ↓ 两次）；从鼓轨出来已由上面的循环落到 T3。
    key = 0x4B if track > t0 else 0x3B
    for _ in range(abs(track - t0)):
        s.cmd(f"KEY 0x{key:02X} down")
        s.cmd(f"KEY 0x{key:02X} up")
    for _ in range((step - s0) % 16):
        s.cmd("KEY 0x4C down")
        s.cmd("KEY 0x4C up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", track, f"⑩ 焦点轨 = {track}")
    s.expect_int(r"focus_step=(\d+)", step, f"⑩ 焦点列 = {step}")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（焦点已归一）")


def tr_focus_col(s: Session, step: int) -> None:
    """在 STEP 页只移动**焦点列**到 `step`（窗口列，0..window_len-1），不动焦点轨；
    再回 TRANSPORT 页。鼓轨用例（焦点轨 3）用它对齐引擎步号。"""
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step（焦点列）")
    s.cmd("STATE SNAP")
    s0 = s.snap_int(r"focus_step=(\d+)")
    wlen = s.snap_int(r"window_len=(\d+)")
    for _ in range((step - s0) % wlen):
        s.cmd("KEY 0x4C down")
        s.cmd("KEY 0x4C up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_step=(\d+)", step, f"⑩ 焦点列 = {step}")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（焦点列已归一）")


def run_transport_tools(s: Session) -> None:
    """⑩ TRANSPORT 工具区（P4-C Task 10）：REC 五态、fill held/next、聚焦步字段
    （LVL/RAT/MIC/SPAN/SLIDE）与 pattern 工具（ROT/LEN/TRANS/CLEAR/UNDO/REDO）。

    判据：每个 TAP 回显一条对应 EditCmd（`-> TRANSPORT <CMD> <op> value=<写后回读>`），
    引擎真值随之变化；非法动作（空步 LVL、鼓轨 SPAN/SLIDE、鼓轨 TRANSPOSE）回
    ERR/ignored 且**不改状态**。pattern 工具会整轨改写（CLEAR 连 lock/micro/fill
    一起清、UNDO 不含它们），所以先用诊断槽 3 快照工程、结束时 LOAD 还原，用 `STORE
    LIVEHASH` 逐位证明还原。**破坏性**：占用诊断槽 3——入口非空则其原内容被工作工程快照
    覆盖（之后留着快照）；入口为空则收口擦回空。快照基线跨截屏复位重跑保持（终审 C-I1）。
    结束时焦点/选轨/走带都归一，脚本可重复。"""
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑩ 停止（工具区的确定性前提）")
    normalize_pattern(s, "⑩ 归一 LEN/DIV")
    ensure_window(s, 0, 16)
    select_track(s, 0)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（回页）")

    # 聚焦步 = 轨 0 / 窗口列 2（绝对步 2）：清空后经 TAP 开一步（C4=60，脚本可重复）。
    # 先「开一步再清」：孤立 TIE 步（上一轮中断留下的 span 尾巴）对 `SEQ EDIT CLEAR`
    # 是 no-op（适配层的 STEP_CLEAR 只删 note，不把 time 写回 REST；上游 `step_clear`
    # 会 memset+REST——DI-31），直接 CLEAR 清不掉，会让 SPAN 基点从 1 变成 2+。
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step（归一孤立 TIE 步）")
    for step in (1, 2, 3, 4, 5):
        s.cmd("TAP %d %d" % step_cell_xy(step, 0))  # NOTE→清；TIE/REST→开成 NOTE
        s.cmd(f"SEQ EDIT CLEAR 0 {step}")             # NOTE→REST
        s.expect(rf"SEQ EDIT CLEAR track=0 step={step} OK", f"⑩ 归一轨 0 步 {step} 为 REST")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（归一后）")
    for step in (1, 2, 3, 4, 5):
        s.cmd(f"SEQ EDIT CLEAR 0 {step}")
        s.expect(rf"SEQ EDIT CLEAR track=0 step={step} OK", f"⑩ 清空轨 0 步 {step}")
    s.cmd("KEY 0x21 down")  # q = C4（设 last_note，TAP 开步用它）
    s.cmd("KEY 0x21 up")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step（造聚焦步）")
    s.cmd("TAP %d %d" % step_cell_xy(2, 0))
    s.expect(r"-> STEP track=0 step=2 CMD_TOGGLE_STEP", "⑩ 开轨 0 步 2")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport")
    base = eng_step(s, 0, 2)
    (s.ok if base["n"] == 1 else s.fail)(f"⑩ 聚焦步基线 n={base['n']}（期望 1）")

    # ---------- ⑩a REC 五态（arm/off；先归一，脚本可重复） ----------
    s.cmd("STATE SNAP")
    if re.search(r"\brec=armed\b", s.last):
        s.ser.note("⑩ 基线：REC 已 ARMED（上一轮中断留下的状态）→ 先归一到 OFF")
        s.cmd("TAP %d %d" % TR_REC)
        s.expect(r"-> TRANSPORT CMD_REC_TOGGLE value=0", "⑩ TAP REC（归一到 OFF）")
        s.cmd("STATE SNAP")
        s.expect(r"\brec=off\b", "⑩ REC 五态：OFF（归一）")
    s.cmd("TAP %d %d" % TR_REC)
    s.expect(r"-> TRANSPORT CMD_REC_TOGGLE value=0", "⑩ TAP REC → CMD_REC_TOGGLE")
    s.cmd("STATE SNAP")
    s.expect(r"\brec=armed\b", "⑩ REC 五态：ARMED")
    s.cmd("TAP %d %d" % TR_REC)
    s.expect(r"-> TRANSPORT CMD_REC_TOGGLE value=0", "⑩ TAP REC（再切换）")
    s.cmd("STATE SNAP")
    s.expect(r"\brec=off\b", "⑩ REC 五态：OFF")

    # ---------- ⑩b fill：HOLD 电平 / NEXT 脉冲（两个独立变量） ----------
    # FILL chip 是切换：先读基线，再断言「写入的电平 = 1 - 基线」且引擎跟随；
    # 两次切换回到基线（脚本可重复，不依赖上一轮留下的 fill 状态）。
    s.cmd("SEQ REC")
    held0 = s.snap_int(r"fill_held=(\d)")
    s.cmd("TAP %d %d" % TR_FILL)
    s.expect(rf"-> TRANSPORT CMD_FILL_CONTROL hold value={1 - held0}",
             f"⑩ TAP FILL → held={1 - held0}")
    s.cmd("SEQ REC")
    s.expect_int(r"fill_held=(\d)", 1 - held0, "⑩ FILL 保持电平跟随")
    s.cmd("TAP %d %d" % TR_FILL)
    s.expect(rf"-> TRANSPORT CMD_FILL_CONTROL hold value={held0}", "⑩ TAP FILL（回到基线）")
    s.cmd("SEQ REC")
    s.expect_int(r"fill_held=(\d)", held0, "⑩ FILL 保持电平回基线")
    arm0 = s.snap_int(r"fill_arm=(\d)")
    s.cmd("TAP %d %d" % TR_NEXT)
    s.expect(r"-> TRANSPORT CMD_FILL_CONTROL next value=1", "⑩ TAP NEXT → 脉冲")
    s.cmd("SEQ REC")
    s.expect_int(r"fill_arm=(\d)", 1 - arm0, "⑩ FILL 下一小节待命翻转（脉冲只翻转 arm）")
    s.cmd("TAP %d %d" % TR_NEXT)
    s.expect(r"-> TRANSPORT CMD_FILL_CONTROL next value=1", "⑩ TAP NEXT（再翻转）")
    s.cmd("SEQ REC")
    s.expect_int(r"fill_arm=(\d)", arm0, "⑩ FILL 下一小节待命回基线")
    s.expect_int(r"fill_held=(\d)", held0, "⑩ NEXT 脉冲不读写保持电平")

    # ---------- ⑩c 聚焦步字段：LVL / RAT 循环、MIC ±（含边界） ----------
    # LVL chip = 循环前进（响度序 GHOST<SOFT<NORM<HARD）：从 NORM(0) 起
    # HARD(3) → GHOST(1) → SOFT(2) → NORM(0)，四拍回到基线。
    for want in (3, 1, 2, 0):
        s.cmd("TAP %d %d" % TR_LVL)
        s.expect(rf"-> TRANSPORT CMD_STEP_EDIT lvl value={want}", f"⑩ TAP LVL → {want}")
        got = eng_step(s, 0, 2)
        (s.ok if (got["lvl"] & 3) == want else s.fail)(
            f"⑩ 引擎 lvl={got['lvl'] & 3}（期望 {want}）")
    s.cmd("UI TOOL")
    s.expect(r"UI TOOL field=0 name=lvl", "⑩ TAP LVL chip 同时落键盘焦点（lvl）")
    # RAT chip = x1→x2→x3→x4→x1（raw 0..3 循环）
    for want in (1, 2, 3, 0):
        s.cmd("TAP %d %d" % TR_RAT)
        s.expect(rf"-> TRANSPORT CMD_STEP_EDIT rat value={want}", f"⑩ TAP RAT → {want}")
        got = eng_step(s, 0, 2)
        (s.ok if (got["rat"] & 3) == want else s.fail)(
            f"⑩ 引擎 rat={got['rat'] & 3}（期望 {want}）")

    s.cmd("TAP %d %d" % TR_MIC_UP)
    s.expect(r"-> TRANSPORT CMD_SET_MICRO micro value=1", "⑩ TAP MIC+ → +1")
    got = eng_step(s, 0, 2)
    (s.ok if got["micro"] == 1 else s.fail)(f"⑩ 引擎 micro={got['micro']}（期望 1）")
    s.cmd("TAP %d %d" % TR_MIC_DN)
    s.expect(r"-> TRANSPORT CMD_SET_MICRO micro value=0", "⑩ TAP MIC- → 0")
    # 上界 31：UI 侧钳位后下发（值不变、不产生 invalid）
    s.cmd("SEQ MICRO 0 2 31")
    s.expect(r"SEQ MICRO track=0 step=2 value=31 OK", "⑩ micro 置 31（上界）")
    s.cmd("TAP %d %d" % TR_MIC_UP)
    s.expect(r"-> TRANSPORT CMD_SET_MICRO micro value=31", "⑩ TAP MIC+ 在 31 钳位")
    got = eng_step(s, 0, 2)
    (s.ok if got["micro"] == 31 else s.fail)(f"⑩ 引擎 micro={got['micro']}（期望 31）")
    s.cmd("SEQ MICRO 0 2 0")
    s.expect(r"SEQ MICRO track=0 step=2 value=0 OK", "⑩ micro 归零")

    # ---------- ⑩d SPAN / SLIDE（合成轨；鼓轨不适用在 ⑩f 覆盖） ----------
    s.cmd("TAP %d %d" % TR_SPAN)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT span value=2", "⑩ TAP SPAN → 2")
    s.cmd("UI TOOL")
    s.expect(r"UI TOOL field=3 name=span", "⑩ TAP SPAN chip 落焦点 = span")
    t3 = eng_step(s, 0, 3)
    (s.ok if t3["time"] == 1 and t3["n"] == 0 else s.fail)(
        f"⑩ 步 3 = TIE（time={t3['time']} n={t3['n']}，期望 time=1 n=0）")
    # 焦点已在 span：键盘 ↓ = SPAN-（触摸 chip 只有循环前进，双向由键盘补）
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    t3 = eng_step(s, 0, 3)
    (s.ok if t3["time"] == 2 and t3["n"] == 0 else s.fail)(
        f"⑩ 键盘 ↓：SPAN 回到 1（time={t3['time']} n={t3['n']}，期望 time=2 n=0）")
    s.cmd("TAP %d %d" % TR_SLIDE)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT slide value=1", "⑩ TAP SLIDE → ON")
    got = eng_step(s, 0, 2)
    (s.ok if (got["flags"] & 0x02) != 0 else s.fail)(
        f"⑩ 引擎 flags=0x{got['flags']:02X}（期望 SLIDE 位）")
    s.cmd("TAP %d %d" % TR_SLIDE)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT slide value=0", "⑩ TAP SLIDE → OFF")
    got = eng_step(s, 0, 2)
    (s.ok if (got["flags"] & 0x02) == 0 else s.fail)(
        f"⑩ 引擎 flags=0x{got['flags']:02X}（SLIDE 已清）")

    # ---------- ⑩e 非法动作不改状态：空步 LVL ----------
    tr_focus_step(s, 0, 5)  # 步 5 已清空（聚焦步基线）
    empty_before = eng_step(s, 0, 5)
    s.cmd("TAP %d %d" % TR_LVL)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT lvl ERR not_found value=0",
             "⑩ 空步 LVL → ERR not_found")
    empty_after = eng_step(s, 0, 5)
    (s.ok if empty_after == empty_before else s.fail)(
        "⑩ 非法动作不改状态（空步逐字段不变）")

    # ---------- ⑩e2 空步 SPAN（NOT_FOUND）与 freeze chip（FROZEN，不改状态） ----------
    s.cmd("TAP %d %d" % TR_SPAN)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT span ERR not_found value=1 status=\"SPAN: EMPTY\"",
             "⑩ 空步 SPAN → ERR not_found + 屏幕状态")
    (s.ok if eng_step(s, 0, 5) == empty_before else s.fail)("⑩ 空步 SPAN 不改状态")

    tr_focus_step(s, 0, 2)
    frozen_before = eng_step(s, 0, 2)
    s.cmd("ENG FREEZE ON")
    s.expect(r"ENG FREEZE on", "⑩ ENG FREEZE ON（chip 拒绝路径）")
    s.cmd("TAP %d %d" % TR_LVL)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT lvl ERR frozen value=\d+ status=\"LVL: FROZEN\"",
             "⑩ 冻结时 TAP LVL → ERR frozen + 屏幕状态")
    s.cmd("ENG FREEZE OFF")
    s.expect(r"ENG FREEZE off", "⑩ ENG FREEZE OFF")
    (s.ok if eng_step(s, 0, 2) == frozen_before else s.fail)("⑩ 冻结期间的 chip 动作不改状态")

    # ---------- ⑩e3 隐藏 bank（窗口 16..23）：目标 / 写入都用绝对步号 ----------------
    ensure_window(s, 16, 8)
    tr_focus_step(s, 0, 2)  # 窗口列 2 = 绝对步 18
    s.cmd("ENG STEP 0 18")
    n18 = s.snap_int(r"\bn=(\d+)")
    if n18 == 0:
        s.ser.note("⑩ 绝对步 18 为空：先造一个音（本用例不额外清；⑩g 的工程快照"
                    "会把它一并保存/还原，下一次运行看到非空即跳过）")
        s.cmd("UI step")
        s.expect(r"UI page=step \(2\)", "⑩ UI step（bank1 造音）")
        s.cmd("TAP %d %d" % step_cell_xy(2, 0))
        s.expect(r"-> STEP track=0 step=18 CMD_TOGGLE_STEP", "⑩ TAP 窗口列 2 = 绝对步 18")
    lvl18 = eng_step(s, 0, 18)["lvl"] & 3
    want = tr_lvl_step(lvl18, True)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（bank1）")
    s.cmd("TAP %d %d" % TR_LVL)
    s.expect(rf"-> TRANSPORT CMD_STEP_EDIT lvl value={want} status=\"LVL: ",
             "⑩ bank1 窗口：TAP LVL 写到绝对步")
    got = eng_step(s, 0, 18)
    (s.ok if (got["lvl"] & 3) == want else s.fail)(
        f"⑩ 绝对步 18 lvl={got['lvl'] & 3}（期望 {want}，不是窗口列号 2）")
    s.cmd("KEY 0x4B down")  # 焦点已落到 lvl：↓ 还原
    s.cmd("KEY 0x4B up")
    (s.ok if (eng_step(s, 0, 18)["lvl"] & 3) == lvl18 else s.fail)("⑩ bank1 lvl 还原")
    ensure_window(s, 0, 16)
    tr_focus_step(s, 0, 2)

    # ---------- ⑩f 鼓轨：选中 lane 的 LVL/RAT 读改写；SPAN/SLIDE 不适用 ----------
    select_track(s, 3)
    tr_focus_col(s, 5)  # 焦点步 = 5：下面的引擎断言按绝对步 5 读（窗口 0..15）
    s.cmd("UI LANE 5")
    s.expect(r"UI LANE lane=5", "⑩ 选中鼓 lane 5")
    d0 = seq_dstep(s, 5)
    lvl0 = dstep_bits(d0["lvl"], 5)
    s.cmd("TAP %d %d" % TR_LVL)
    want_lvl = tr_lvl_step(lvl0, True)
    s.expect(rf"-> TRANSPORT CMD_DRUM_STEP_WRITE lvl value={want_lvl}",
             "⑩ 鼓轨 TAP LVL → CMD_DRUM_STEP_WRITE")
    d1 = seq_dstep(s, 5)
    (s.ok if dstep_bits(d1["lvl"], 5) == want_lvl and d1["on"] == d0["on"] else s.fail)(
        f"⑩ 鼓 lane 5 lvl={dstep_bits(d1['lvl'], 5)}（期望 {want_lvl}），on 位图不变")
    s.cmd("KEY 0x4B down")  # 焦点 = lvl（TAP 已设）→ LVL_DN 还原
    s.cmd("KEY 0x4B up")
    d2 = seq_dstep(s, 5)
    (s.ok if dstep_bits(d2["lvl"], 5) == lvl0 else s.fail)(
        f"⑩ 键盘 ↓：鼓 lane 5 lvl 回到 {lvl0}")

    rat0 = dstep_bits(d2["rat"], 5)
    if rat0 < 3:
        s.cmd("TAP %d %d" % TR_RAT)
        s.expect(rf"-> TRANSPORT CMD_DRUM_STEP_WRITE rat value={rat0 + 1}",
                 "⑩ 鼓轨 TAP RAT → CMD_DRUM_STEP_WRITE")
        d3 = seq_dstep(s, 5)
        (s.ok if dstep_bits(d3["rat"], 5) == rat0 + 1 else s.fail)(
            f"⑩ 鼓 lane 5 rat={dstep_bits(d3['rat'], 5)}（期望 {rat0 + 1}）")
        s.cmd("KEY 0x4B down")  # 焦点 = rat → RAT_DN 还原
        s.cmd("KEY 0x4B up")
    else:
        s.ser.note(f"⑩ 鼓 lane 5 rat 已在 x4（raw 3）：键盘 ↓/↑ 往返验证双向")
        s.cmd("UI TOOL 1")
        s.expect(r"UI TOOL field=1 name=rat", "⑩ UI TOOL 1 → rat")
        s.cmd("KEY 0x4B down")
        s.cmd("KEY 0x4B up")
        s.cmd("KEY 0x3B down")
        s.cmd("KEY 0x3B up")
    d4 = seq_dstep(s, 5)
    (s.ok if dstep_bits(d4["rat"], 5) == rat0 else s.fail)(
        f"⑩ 鼓 lane 5 rat 回到 {rat0}（实得 {dstep_bits(d4['rat'], 5)}）")

    s.cmd("TAP %d %d" % TR_SPAN)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT span ignored \(not_synth\) value=0",
             "⑩ 鼓轨 SPAN → ignored (not_synth)")
    s.cmd("TAP %d %d" % TR_SLIDE)
    s.expect(r"-> TRANSPORT CMD_STEP_EDIT slide ignored \(not_synth\) value=0",
             "⑩ 鼓轨 SLIDE → ignored (not_synth)")
    (s.ok if seq_dstep(s, 5) == d4 else s.fail)("⑩ 不适用动作不改鼓步（逐字节不变）")

    # ---------- ⑩g pattern 工具（先快照工程：CLEAR 会清 lock/micro/fill，UNDO 不含它们） ----------
    # **破坏性**：占用诊断槽 3（入口非空则被工作工程快照覆盖）；基线跨截屏复位重跑保持
    # （proj_snapshot_open，终审 C-I1）。
    proj_snapshot_open(s, "transport.pattern", 3, "⑩g")
    select_track(s, 0)
    tr_focus_step(s, 0, 2)

    s.cmd("TAP %d %d" % TR_ROT_UP)
    s.expect(r"-> TRANSPORT CMD_PATTERN rotate value=1", "⑩ TAP ROT+ → rotate +1")
    moved = eng_step(s, 0, 2)
    got3 = eng_step(s, 0, 3)
    (s.ok if moved["n"] == 0 and got3["note0"] == 60 else s.fail)(
        f"⑩ 旋转后：步 2 n={moved['n']}、步 3 note0={got3['note0']}（期望 0 / 60）")
    s.cmd("TAP %d %d" % TR_ROT_DN)
    s.expect(r"-> TRANSPORT CMD_PATTERN rotate value=-1", "⑩ TAP ROT- → rotate -1")
    back = eng_step(s, 0, 2)
    (s.ok if back["note0"] == 60 else s.fail)(f"⑩ 反向旋转回位：步 2 note0={back['note0']}")

    s.cmd("TAP %d %d" % TR_LEN_X2)
    s.expect(r"-> TRANSPORT CMD_PATTERN length value=32", "⑩ TAP LEN x2 → LEN 32")
    s.cmd("ENG SNAP")
    s.expect_int(r"trk k=0 .*?\blen=(\d+)", 32, "⑩ ENG SNAP 轨 0 len（×2）")
    s.cmd("TAP %d %d" % TR_LEN_D2)
    s.expect(r"-> TRANSPORT CMD_PATTERN length value=16", "⑩ TAP LEN /2 → LEN 16")
    s.cmd("ENG SNAP")
    s.expect_int(r"trk k=0 .*?\blen=(\d+)", 16, "⑩ ENG SNAP 轨 0 len（÷2）")

    s.cmd("TAP %d %d" % TR_TRANS_UP)
    s.expect(r"-> TRANSPORT CMD_PATTERN transpose value=1", "⑩ TAP TRANS+ → +1")
    got = eng_step(s, 0, 2)
    (s.ok if got["note0"] == 61 else s.fail)(f"⑩ 移调后 note0={got['note0']}（期望 61）")
    s.cmd("TAP %d %d" % TR_TRANS_DN)
    s.expect(r"-> TRANSPORT CMD_PATTERN transpose value=-1", "⑩ TAP TRANS- → -1")
    got = eng_step(s, 0, 2)
    (s.ok if got["note0"] == 60 else s.fail)(f"⑩ 移调回位 note0={got['note0']}（期望 60）")

    s.cmd("TAP %d %d" % TR_CLEAR)
    s.expect(r"-> TRANSPORT CMD_PATTERN clear value=0", "⑩ TAP CLEAR → clear")
    got = eng_step(s, 0, 2)
    (s.ok if got["n"] == 0 else s.fail)(f"⑩ CLEAR 后步 2 n={got['n']}（期望 0）")
    s.cmd("TAP %d %d" % TR_UNDO)
    s.expect(r"-> TRANSPORT CMD_PATTERN undo value=0", "⑩ TAP UNDO → undo")
    got = eng_step(s, 0, 2)
    (s.ok if got["n"] == 1 and got["note0"] == 60 else s.fail)(
        f"⑩ UNDO 恢复步 2（n={got['n']} note0={got['note0']}）")
    s.cmd("TAP %d %d" % TR_REDO)
    s.expect(r"-> TRANSPORT CMD_PATTERN redo value=0", "⑩ TAP REDO → redo")
    got = eng_step(s, 0, 2)
    (s.ok if got["n"] == 0 else s.fail)(f"⑩ REDO 后步 2 n={got['n']}（期望 0）")

    # 鼓轨上的 TRANSPOSE 由上游拒绝（invalid），图案不变
    select_track(s, 3)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（鼓轨 TRANSPOSE）")
    d_before = seq_dstep(s, 5)
    s.cmd("TAP %d %d" % TR_TRANS_UP)
    s.expect(r"-> TRANSPORT CMD_PATTERN transpose ERR invalid value=1",
             "⑩ 鼓轨 TRANSPOSE → ERR invalid")
    (s.ok if seq_dstep(s, 5) == d_before else s.fail)("⑩ 被拒的 TRANSPOSE 不改鼓步")

    # 工程还原（诊断槽 3 的 LOAD 连 lock/micro/fill 一起回原状态）+ 槽收口（入口为空才擦：
    # 入口非空的槽留着快照——它原来的内容已被覆盖，擦掉只是再丢一份）
    proj_snapshot_close(s, "transport.pattern", 3, "⑩g")

    # ---------- ⑩h 键盘：←/→ 移工具焦点、↑/↓ 调焦点字段、ENTER 切换 ----------
    select_track(s, 0)
    tr_focus_step(s, 0, 2)
    s.cmd("UI TOOL 0")
    s.expect(r"UI TOOL field=0 name=lvl", "⑩ UI TOOL 0 → lvl")
    s.cmd("UI TOOL 8")
    s.expect(r"UI ERR tool", "⑩ UI TOOL 越界 → UI ERR tool")
    s.cmd("UI TOOL")
    s.expect(r"UI TOOL field=0 name=lvl", "⑩ 越界不改焦点（仍 lvl）")

    lvl0 = eng_step(s, 0, 2)["lvl"] & 3
    s.cmd("KEY 0x3B down")  # ↑ = 焦点字段 +1
    s.cmd("KEY 0x3B up")
    got = eng_step(s, 0, 2)
    want = tr_lvl_step(lvl0, True)
    (s.ok if (got["lvl"] & 3) == want else s.fail)(
        f"⑩ 键盘 ↑：lvl {lvl0} -> {got['lvl'] & 3}（期望 {want}）")
    s.cmd("KEY 0x4B down")  # ↓ = -1
    s.cmd("KEY 0x4B up")
    got = eng_step(s, 0, 2)
    (s.ok if (got["lvl"] & 3) == lvl0 else s.fail)(
        f"⑩ 键盘 ↓：lvl 回到 {lvl0}（实得 {got['lvl'] & 3}）")

    s.cmd("KEY 0x4C down")  # → = 焦点字段 +1（lvl -> rat）
    s.cmd("KEY 0x4C up")
    s.cmd("UI TOOL")
    s.expect(r"UI TOOL field=1 name=rat", "⑩ 键盘 →：焦点 = rat")
    rat0 = eng_step(s, 0, 2)["rat"] & 3
    s.cmd("KEY 0x3B down")
    s.cmd("KEY 0x3B up")
    got = eng_step(s, 0, 2)
    want = (rat0 + 1) & 3  # RAT 是 x1..x4 的循环（x4 再前进回 x1）
    (s.ok if (got["rat"] & 3) == want else s.fail)(
        f"⑩ 键盘 ↑：rat {rat0} -> {got['rat'] & 3}（期望 {want}）")
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    got = eng_step(s, 0, 2)
    (s.ok if (got["rat"] & 3) == rat0 else s.fail)(
        f"⑩ 键盘 ↓：rat 回到 {rat0}（实得 {got['rat'] & 3}）")

    s.cmd("UI TOOL 4")  # slide
    s.expect(r"UI TOOL field=4 name=slide", "⑩ UI TOOL 4 → slide")
    slide0 = 1 if (eng_step(s, 0, 2)["flags"] & 0x02) else 0
    s.cmd("KEY 0x3D down")  # ENTER = 切换
    s.cmd("KEY 0x3D up")
    got = eng_step(s, 0, 2)
    (s.ok if ((got["flags"] & 0x02) != 0) != bool(slide0) else s.fail)(
        f"⑩ 键盘 ENTER：SLIDE {slide0} -> {(got['flags'] & 0x02) != 0}")
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    got = eng_step(s, 0, 2)
    (s.ok if ((got["flags"] & 0x02) != 0) == bool(slide0) else s.fail)(
        f"⑩ 键盘 ENTER（再按）：SLIDE 回到 {slide0}")

    s.cmd("UI TOOL 7")
    s.expect(r"UI TOOL field=7 name=trans", "⑩ UI TOOL 7 → trans")
    s.cmd("KEY 0x4C down")  # 7 → 0 环绕
    s.cmd("KEY 0x4C up")
    s.cmd("UI TOOL")
    s.expect(r"UI TOOL field=0 name=lvl", "⑩ 键盘 → 在字段环上环绕（trans -> lvl）")

    # ---------- ⑩h2 焦点轨 ≠ 选中轨：屏幕上报的目标 = 引擎实际写入的轨 ----------
    # 选中轨 0（走带/播放头），焦点轨 2：第二行必须以 `T3 S<abs>` 开头、颜色 = 轨 2
    # （黄），工具也只写轨 2。轨 2 的步 2 先造一个音（结束清回），保证 LVL 不是 NOT_FOUND。
    select_track(s, 0)  # 上面的 LOAD 还原带回快照里的选中轨（SAVE 时是鼓轨）
    s.cmd("SEQ EDIT CLEAR 2 2")
    s.expect(r"SEQ EDIT CLEAR track=2 step=2 OK", "⑩ 清空轨 2 步 2（焦点轨目标用例）")
    s.cmd("KEY 0x21 down")  # q = C4（last_note）
    s.cmd("KEY 0x21 up")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step（给焦点轨造音）")
    s.cmd("TAP %d %d" % step_cell_xy(2, 2))
    s.expect(r"-> STEP track=2 step=2 CMD_TOGGLE_STEP", "⑩ 轨 2 步 2 开音")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d)", 2, "⑩ 焦点轨 = 2（选中轨仍是 0）")
    s.expect_int(r"selected_track=(\d)", 0, "⑩ 选中轨仍 = 0")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（焦点轨目标）")
    t0_lvl = eng_step(s, 0, 2)["lvl"] & 3
    t2_lvl = eng_step(s, 2, 2)["lvl"] & 3
    want = tr_lvl_step(t2_lvl, True)
    s.cmd("TAP %d %d" % TR_LVL)
    s.expect(rf"-> TRANSPORT CMD_STEP_EDIT lvl value={want} status=\"LVL: ",
             "⑩ 焦点轨 TAP LVL（回显带屏幕状态）")
    got2 = eng_step(s, 2, 2)
    got0 = eng_step(s, 0, 2)
    (s.ok if (got2["lvl"] & 3) == want else s.fail)(
        f"⑩ 引擎写入轨 2：lvl={got2['lvl'] & 3}（期望 {want}）")
    (s.ok if (got0["lvl"] & 3) == t0_lvl else s.fail)(
        f"⑩ 选中轨 0 未被写：lvl={got0['lvl'] & 3}（期望 {t0_lvl}）")
    img = s.capture("transport-focus")
    # 第二行（工具目标）与第一行同标签：文本颜色 = **焦点轨 2**（黄），不含轨 0 蓝。
    detail_box = (48, 72 + 320, 760, 72 + 320 + 50)
    yellow = count_region(img, detail_box, C_YELLOW, 16)
    blue = count_region(img, detail_box, C_BLUE, 16)
    (s.ok if yellow >= 100 and blue == 0 else s.fail)(
        f"⑩ 步摘要文本颜色 = 焦点轨黄 {yellow} / 选中轨蓝 {blue}（期望黄 ≥100、蓝 0）")
    # 收口：LVL 回原值（键盘 ↓ 作用在同一焦点）→ 清轨 2 步 2 → 焦点回轨 0 步 2
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    got2 = eng_step(s, 2, 2)
    (s.ok if (got2["lvl"] & 3) == t2_lvl else s.fail)(
        f"⑩ 焦点轨 2 lvl 回到 {t2_lvl}（实得 {got2['lvl'] & 3}）")
    s.cmd("SEQ EDIT CLEAR 2 2")
    s.expect(r"SEQ EDIT CLEAR track=2 step=2 OK", "⑩ 清回轨 2 步 2")
    tr_focus_step(s, 0, 2)

    # ---------- ⑩i 截屏：REC ARMED + FILL 保持 + 状态行的视觉证据 ----------
    s.cmd("STATE SNAP")
    if not re.search(r"\brec=armed\b", s.last):
        s.cmd("TAP %d %d" % TR_REC)
        s.expect(r"-> TRANSPORT CMD_REC_TOGGLE value=0", "⑩ 截屏前提：REC → ARMED")
        s.cmd("STATE SNAP")
        s.expect(r"\brec=armed\b", "⑩ 截屏前提：REC 已 ARMED")
    s.cmd("SEQ REC")
    if s.snap_int(r"fill_held=(\d)") == 0:
        s.cmd("TAP %d %d" % TR_FILL)
        s.expect(r"-> TRANSPORT CMD_FILL_CONTROL hold value=1", "⑩ 截屏前提：FILL 保持")
        s.cmd("SEQ REC")
        s.expect_int(r"fill_held=(\d)", 1, "⑩ 截屏前提：FILL 保持已建立")
    img = s.capture("transport-tools")
    # 探针取 chip 的**内缘**（左上/左下 14px）：避开 16pt 文本行与 12px 圆角。
    rec_bg = (TR_REC[0] - TR_REC_W // 2 + 14, TR_REC[1] - TR_REC_H // 2 + 14)
    fill_bg = (TR_FILL[0] - TR_FILL_W // 2 + 14, TR_FILL[1] - TR_FILL_H // 2 + 14)
    lvl_bg = (TR_LVL[0] - TR_STEP_W // 2 + 14, TR_LVL[1] + TR_STEP_H // 2 - 14)
    clear_bg = (TR_CLEAR[0] - 432 // 2 + 14, TR_CLEAR[1] + TR_PAT_H // 2 - 14)
    s.probe(img, "REC chip 底（ARMED = UI_ACCENT）", *rec_bg, C_YELLOW)
    s.probe(img, "FILL chip 底（held = UI_ACCENT）", *fill_bg, C_YELLOW)
    s.probe(img, "聚焦步字段 chip 底（UI_PANEL_HI）", *lvl_bg, C_PANEL_HI)
    s.probe(img, "pattern chip 底（UI_PANEL_HI）", *clear_bg, C_PANEL_HI)
    s.probe_region(img, "状态行文本（STOPPED + 动作结果）",
                   (48, 72 + 264, 808, 72 + 264 + 40), C_TEXT, 16, 30)

    mem = param_mem(s)
    state, est, delta = mem["pages"].get(3, ("?", 0, 0))
    (s.ok if state == "built" and 0 < delta <= est else s.fail)(
        f"⑩ UI MEM TRANSPORT state={state} delta={delta} est={est}（0 < delta ≤ est）")
    seq_est = mem["groups"]["SEQ"]["sum_est"]
    (s.ok if seq_est == 23040 + est else s.fail)(
        f"⑩ UI MEM GROUP SEQ sum_est={seq_est}（期望 STEP 23040 + TRANSPORT {est}）")
    pair = UI_BASE_BYTES + seq_est + 12800 + 4096
    s.ser.note(f"⑩ TRANSPORT 预算：est={est} delta={delta}（余量 "
               f"{(est - delta) * 100.0 / max(delta, 1):.1f}%）；SEQ Σest={seq_est} ⇒ "
               f"SEQ→PRESETS 有序对 = {UI_BASE_BYTES} + {seq_est} + 12800 + 4096 = {pair}B"
               f"（纸面账，按重测 base 可能略超 63120——门禁是下面的实测池剩余，DI-56）")

    # ---------- ⑩j 跨组过渡峰值：SEQ 全建（STEP+TRANSPORT）→ 首建 PRESETS ----------------
    # ADR-005 §5 的新最坏有序对 `32,256 + 12,800`（账面 62,952B、余 168B）此前只有
    # 账面值，没有实测。这里把 SEQ 两页建满，再从 SEQ 直接切 PRESETS（跨组：目标页
    # 构建期间源组仍存活），从构建日志读**构建后的 pool_free** —— 它就是过渡峰值点。
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step（SEQ 全建）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport（SEQ 全建）")
    mem = param_mem(s)
    seq = mem["groups"]["SEQ"]
    (s.ok if seq["built"] == 2 and seq["sum_est"] == 32256 else s.fail)(
        f"⑩ 过渡前 GROUP SEQ built={seq['built']} sum_est={seq['sum_est']}（期望 2 / 32,256）")
    s.cmd("UI presets", timeout=5.0)
    s.expect(r"UI page=presets \(10\)", "⑩ UI presets（SEQ 全建时首建 PRESETS）")
    m = re.search(r"页面构建 page=PRESETS\(10\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B", s.last)
    if m is None:
        s.fail("⑩ 过渡构建日志缺少 PRESETS 行（池剩余不可测）")
    else:
        delta, est, free_after = (int(g) for g in m.groups())
        (s.ok if free_after >= 4096 else s.fail)(
            f"⑩ 过渡峰值 pool_free={free_after}B（≥4,096；PRESETS delta={delta} 预算={est}）")
        s.ser.note(f"⑩ SEQ→PRESETS 过渡实测：PRESETS delta={delta}B、构建后 pool_free={free_after}B"
                   f"（ADR-005 账面：base {UI_BASE_BYTES} + 32,256 + 12,800 + 4,096 = "
                   f"{UI_BASE_BYTES + 32256 + 12800 + 4096}B）")
    mem = param_mem(s)
    (s.ok if mem["groups"]["SEQ"]["built"] == 0 and mem["groups"]["FILE"]["built"] == 1 else s.fail)(
        f"⑩ 过渡后组作用域：SEQ built={mem['groups']['SEQ']['built']}、"
        f"FILE built={mem['groups']['FILE']['built']}（期望 0 / 1）")
    # 同组两页都建出来（PROJECTS）+ 手指导航往返，最后回 SEQ（调用方继续走带用例）
    s.cmd("UI TAB FILE")
    s.expect(r"UI TAB FILE page=", "⑩ UI TAB FILE")
    s.cmd("UI SUB 0")
    s.expect(r"UI SUB 0 page=projects \(9\)", "⑩ UI SUB 0 → PROJECTS")
    s.cmd("UI SUB 1")
    s.expect(r"UI SUB 1 page=presets \(10\)", "⑩ UI SUB 1 → PRESETS")
    mem = param_mem(s)
    (s.ok if mem["groups"]["FILE"]["built"] == 2 else s.fail)(
        f"⑩ FILE 两页已建（built={mem['groups']['FILE']['built']}）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ 回 SEQ（重建 TRANSPORT）")

    # 收口：REC OFF + FILL 释放 + 默认焦点；走带仍停止（调用方再断言一次）
    s.cmd("STATE SNAP")
    if re.search(r"\brec=armed\b", s.last):
        s.cmd("TAP %d %d" % TR_REC)
        s.expect(r"-> TRANSPORT CMD_REC_TOGGLE value=0", "⑩ 收口：REC OFF")
    s.cmd("SEQ REC")
    if s.snap_int(r"fill_held=(\d)") == 1:
        s.cmd("TAP %d %d" % TR_FILL)
        s.expect(r"-> TRANSPORT CMD_FILL_CONTROL hold value=0", "⑩ 收口：FILL 释放")
    s.cmd("UI TOOL 0")
    s.expect(r"UI TOOL field=0 name=lvl", "⑩ 收口：工具焦点 = lvl")
    s.cmd("STATE SNAP")
    s.expect(r"\brec=off\b", "⑩ 收口：rec=off")
    s.expect_int(r"playing=(\d)", 0, "⑩ 收口：playing=0")


def run_step_edit(s: Session) -> None:
    """② STEP：窗口列编辑闭环 + 分窗（bank1 绝对步号/隐藏列/播放头映射）+ 截屏。
    可重复运行：目标格已亮先经同一 TAP 路径点灭；结束复位默认窗口与停止态。"""
    ensure_stopped(s)
    normalize_pattern(s)  # LEN=16：bank1 的 DIM 判据（绝对步 ≥ LEN 才暗显）
    ensure_window(s, 0, 16)  # 归一到默认窗口：注入几何由此派生（列 c = 绝对步 c）
    # ②e 的 CONT 前提是轨 0 为 POLY：`normalize_pattern` 末尾选中轨 2，且设备可能从
    # 工程 fixture / 预置恢复成 MONO/LEGATO（LEGATO 的音符不会在后续音符下方延续 →
    # span 停在下一个 NOTE，CONT 不出现）。显式选轨 0 并写 P_VOICE=POLY（脚本可重复；
    # 状态假设原 DI-23 的同类问题）。
    select_track(s, 0)
    s.cmd(f"PARAM {P_VOICE_ID} 0")  # P_VOICE = V_POLY
    s.expect(rf"PARAM track=0 id={P_VOICE_ID} value=0", "② VOICE 归一轨 0 = POLY（CONT 用例前提）")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")

    cells = (("0", "2", step_cell_xy(2, 0)), ("1", "5", step_cell_xy(5, 1)))
    lit = []
    for track, step, _ in cells:
        s.cmd(f"ENG STEP {track} {step}")
        lit.append(step_is_on(s))
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

    # 空位探针（轨 0 步 3）的前提：它可能带着载入工程/旧格式 fixture 的音符（状态假设，
    # 原 DI-23）⇒ 先用同一 TAP 路径清掉。必须在下面两格 TAP **之前**（TAP 会设焦点，
    # 之后的两格 TAP 才把焦点落到轨道 1 / 列 5 的断言值上）。
    s.cmd("ENG STEP 0 3")
    if step_is_on(s):
        s.ser.note("基线：轨 0 步 3 有音（工程带进来），先点灭再作「空位」探针")
        s.cmd("TAP %d %d" % step_cell_xy(3, 0))
        s.expect(r"-> STEP track=0 step=3 CMD_TOGGLE_STEP", "TAP 点灭轨 0 步 3（空位探针前提）")

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
    if step_is_on(s):
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
    # 「列 1 空位」探针的前提：步 17 可能带着工程/fixture 的音符 ⇒ 先清掉
    s.cmd("ENG STEP 0 17")
    if step_is_on(s):
        s.ser.note("基线：轨 0 绝对步 17 有音（工程带进来），先点灭再作「空位」探针")
        s.cmd("TAP %d %d" % step_cell_xy(1, 0))
        s.expect(r"-> STEP track=0 step=17 CMD_TOGGLE_STEP", "TAP 点灭轨 0 步 17（空位探针前提）")
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
    if not step_is_on(s):
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
        good = (len(cols_a[r]) >= 1 if r in PLAYHEAD_AMBIGUOUS_ROWS else len(cols_a[r]) == 1)
        (s.ok if good else s.fail)(
            f"bank0 行 {r} 播放头列 {cols_a[r]}" if good
            else f"bank0 行 {r} 播放头应唯一（实得 {cols_a[r]}）")
    ensure_window(s, 16, 16)
    time.sleep(0.4)
    img = s.capture("step-bank1-no-playhead")
    cols_b = playhead_columns(img)
    # 行 2 的轨色 = UI_ACCENT：亮格与播放头同色，像素上无法区分 ⇒ 该行不参与
    # 「窗口外无残留描边」判据（PLAYHEAD_AMBIGUOUS_ROWS）。
    stray = {r: cols for r, cols in cols_b.items() if cols and r not in PLAYHEAD_AMBIGUOUS_ROWS}
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

    # ②e 深度步态（P4-C Task 9）：同一个 64 格对象在格内呈现 TIE/SLIDE/fill/lvl/
    # micro/rat/CONT，不新增每格 label。断言口径 = 引擎真值（ENG STEP）＋像素抽检
    # （TIE 空心格与 NOTE 实心格不同、白色 S / 黄色 F / 灰 m± / 白色 xN / 黄色 HARD 音符名）。
    run_step_deep_visuals(s)
    # ②f 鼓 lane（P4-C Task 9）：点 dot 选 lane 并写原始 16 lane 位图；键盘 ↑/↓ 选
    # lane、ENTER 切换位；鼓轨行不再是只读。
    run_step_drum_lanes(s)
    # ②g EDIT 层 a–g（Aa + 音名键）擦除聚焦步里的同名音（P4-F Task 7）
    run_step_edit_erase(s)

    s.cmd("UI play")


# EDIT 层擦除用例的步（轨 0 的窗口列 = 绝对步）：②a–②f 结束后这些都应为空；入口/收口各清一次
# （上一轮异常终止的残留），它们不在其它段的探针格之列。
ERASE_COLS = (5, 6, 8, 10, 12, 13, 14)
KEY_NOTE_C4, KEY_NOTE_D4, KEY_NOTE_E4, KEY_NOTE_F4 = 0x21, 0x22, 0x23, 0x24   # q w e r
KEY_NOTE_C3 = 0x42                                                           # z（低八度行）
KEY_A, KEY_D, KEY_F, KEY_G = 0x32, 0x34, 0x35, 0x36                          # EDIT 层 = 音名 A/D/F/G


def erase_clear(s: Session, cols, label: str) -> None:
    for col in cols:
        s.cmd(f"SEQ EDIT CLEAR 0 {col}")
        s.expect(rf"SEQ EDIT CLEAR track=0 step={col} OK", f"{label}：清轨 0 步 {col}")


def erase_steps(s: Session) -> dict:
    """{列: ENG STEP 全字段}（擦除用例的「其余步不变」判据）。"""
    return {col: eng_step(s, 0, col) for col in ERASE_COLS}


def erase_tap_note(s: Session, col: int, key: int, note: int) -> None:
    """弹一下键（记 last_note）再 TAP 格 ⇒ 该列的 NOTE = 该键的音高（CMD_TOGGLE_STEP 取
    last_note）；与 ②a 的建音同一路径。"""
    s.cmd(f"KEY 0x{key:02X} down")
    s.expect(rf"KEY 0x{key:02X} down", f"②g 弹键 {key:#04x}（last_note={note}）")
    s.cmd(f"KEY 0x{key:02X} up")
    s.expect(rf"KEY 0x{key:02X} up", f"②g 松键 {key:#04x}")
    x, y = step_cell_xy(col, 0)
    s.cmd(f"TAP {x} {y}")
    s.expect(rf"-> STEP track=0 step={col} CMD_TOGGLE_STEP", f"②g TAP 步 {col} 建音")
    st = eng_step(s, 0, col)
    (s.ok if st["n"] == 1 and st["note0"] == note and st["time"] == 0 else s.fail)(
        f"②g 步 {col} = NOTE {note}（实得 n={st['n']} note={st['note0']} time={st['time']}）")


def erase_audible_norm(s: Session) -> tuple:
    """②g 发声探针的前提（R20）：轨 0 未静音且 LEVEL = 默认 104。入口工程可能带着静音/极低
    音量的轨 0（autosave 恢复的场景残留）⇒ 记原值、经 PARAM 归一，`erase_audible_restore`
    写回。必须在 `proj_snapshot_open` **之后**调用：快照里是入口值，收口 LOAD 还原的也是
    入口值（选中轨同理：收口后回到入口选中轨）。"""
    s.cmd("STATE SNAP")
    sel0 = s.snap_int(r"selected_track=(\d+)")
    select_track(s, 0)
    s.cmd(f"PARAM {ENG_P_MUTE}")
    mute0 = param_value(s)
    s.cmd(f"PARAM {ENG_P_LEVEL}")
    level0 = param_value(s)
    s.cmd(f"PARAM {ENG_P_MUTE} 0")
    s.expect(rf"PARAM track=0 id={ENG_P_MUTE} value=0 applied=0", "②g 归一 P_MUTE=0（发声探针前提）")
    s.cmd(f"PARAM {ENG_P_LEVEL} {ENG_LEVEL_DEF}")
    s.expect(rf"PARAM track=0 id={ENG_P_LEVEL} value={ENG_LEVEL_DEF} applied={ENG_LEVEL_DEF}",
             f"②g 归一 P_LEVEL={ENG_LEVEL_DEF}（发声探针前提）")
    return sel0, mute0, level0


def erase_audible_restore(s: Session, orig: tuple) -> None:
    sel0, mute0, level0 = orig
    select_track(s, 0)
    s.cmd(f"PARAM {ENG_P_MUTE} {mute0}")
    s.expect(rf"PARAM track=0 id={ENG_P_MUTE} value={mute0} applied={mute0}",
             f"②g 还原 P_MUTE={mute0}")
    s.cmd(f"PARAM {ENG_P_LEVEL} {level0}")
    s.expect(rf"PARAM track=0 id={ENG_P_LEVEL} value={level0} applied={level0}",
             f"②g 还原 P_LEVEL={level0}")
    if sel0 != 0:
        select_track(s, sel0)


def run_step_edit_erase(s: Session) -> None:
    """②g EDIT 层 a–g：Aa + 音名键删除聚焦步里**所有该音级**的音符（跨八度；按精确音高下发
    `CMD_ERASE_NOTE`，`input.c` 的 act_erase）。断言引擎真值 `ENG STEP`：命中的音消失、
    同步其余音（和弦）与其余步不变；无匹配音级 ⇒ 步不变；不按 Aa 时 c 是演奏键（聚焦在持
    C4 的步 8 上按住 c：音符发声、步表不变）；鼓轨无此语义（聚焦鼓轨时 no-op）；诊断冻结时
    同一个会擦的键（Aa+f 对准 F4 步）不写引擎。收口：`STORE PROJ LOAD 3` 把工作工程还原成
    入口快照（LIVEHASH 逐位证明；入口为空的诊断槽 D 擦回空）、last_note 回 60（C4，其余场景
    TAP 建音的缺省音高）、焦点回 T1 入口列。"""
    s.ser.note("②g EDIT 层擦除：Aa + 音名键 ⇒ 聚焦步的同名音消失（ENG STEP 真值）")
    ensure_stopped(s)
    s.cmd("STATE SNAP")
    focus0 = s.snap_int(r"focus_step=(\d+)")
    # 工作工程快照（诊断槽 D = 3，与 transport/sequence 的约定一致）：本段在轨 0 造/擦音符，
    # 收口 LOAD 还原。入口槽 D 若非空，其内容被本快照覆盖（注记）；入口为空则收口擦回空。
    # 基线跨截屏复位重跑保持（proj_snapshot_open，终审 C-I1）。
    proj_snapshot_open(s, "step_edit.erase", 3, "②g")
    audible0 = erase_audible_norm(s)
    ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "②g UI step")
    erase_clear(s, ERASE_COLS, "②g 入口")
    # 步 8 = C4、步 10 = E4、步 12 = D4（无匹配音级的对照）、步 13 = C3（跨八度）、
    # 步 14 = F4、步 5 = C4 + 步 6 = D4（随后 SPAN 出 CONT(C4) 的两音步）
    for col, key, note in ((8, KEY_NOTE_C4, 60), (10, KEY_NOTE_E4, 64), (12, KEY_NOTE_D4, 62),
                           (13, KEY_NOTE_C3, 48), (14, KEY_NOTE_F4, 65), (5, KEY_NOTE_C4, 60),
                           (6, KEY_NOTE_D4, 62)):
        erase_tap_note(s, col, key, note)
    # 步 6 = NOTE(D4) + CONT(C4)：步 5 的 C4 长过去（与 ②e 同一造法，flags 的 SF_CONT(1)）
    s.cmd("SEQ EDIT SPAN 0 5 2")
    s.expect(r"SEQ EDIT SPAN track=0 step=5 OK", "②g SPAN 2（C4 从步 5 长到步 6 的 D4 上）")
    st6 = eng_step(s, 0, 6)
    (s.ok if st6["n"] == 2 and tuple(sorted((st6["note0"], st6["note1"]))) == (60, 62) else s.fail)(
        f"②g 步 6 是两音步（n={st6['n']} note={st6['note0']},{st6['note1']}，期望 60+62）")
    before = erase_steps(s)

    # 不按 Aa：c 是演奏键（C3+4=52），不擦除。聚焦先放到持有 C4 的步 8——同一聚焦步、同一
    # 个键，按住 Aa 就是下面的擦除；这里按住期间轨 0 必须真的发声（voices ≥ 1），步表不变。
    nav_set_focus(s, 0, 8)
    s.cmd("KEY 0x44 down")
    s.expect(r"KEY 0x44 down c elapsed_us=", "②g 无 Aa：c 是演奏键")
    voices = eng_voices_max(s, 0, 1.0)
    s.cmd("KEY 0x44 up")
    s.expect(r"KEY 0x44 up c elapsed_us=", "②g 松开 c")
    nav_same(s, "②g 无 Aa 时 c 不擦除聚焦步 8", before, erase_steps(s))
    (s.ok if voices >= 1 else s.fail)(
        f"②g 无 Aa 按住 c：轨 0 voices 峰值 {voices}（≥1 = 发声）")

    # Aa+c @ 步 8：C4 消失（单音步 ⇒ 整步清空）
    nav_set_focus(s, 0, 8)
    key_with_layer(s, (KEY_AA,), (0x44,), 0x02, "②g Aa+c")
    after = erase_steps(s)
    st8 = after[8]
    # time 的极性（实测口径，与 state_step_is_on 同）：NOTE = 0 才是「开」，整步清空后是
    # ST_REST = 2（非 0）——所以「已清空」断言 n == 0 且 time != 0。
    (s.ok if st8["n"] == 0 and st8["time"] != 0 else s.fail)(
        f"②g Aa+c ⇒ 步 8 的 C4 擦除（n={st8['n']} time={st8['time']}）")
    nav_same(s, "②g Aa+c 只动步 8", {c: v for c, v in before.items() if c != 8},
             {c: v for c, v in after.items() if c != 8})
    before = after

    # Aa+e @ 步 10：E4 消失
    nav_set_focus(s, 0, 10)
    key_with_layer(s, (KEY_AA,), (KEY_NOTE_E4,), 0x02, "②g Aa+e")
    after = erase_steps(s)
    (s.ok if after[10]["n"] == 0 else s.fail)(
        f"②g Aa+e ⇒ 步 10 的 E4 擦除（n={after[10]['n']}）")
    nav_same(s, "②g Aa+e 只动步 10", {c: v for c, v in before.items() if c != 10},
             {c: v for c, v in after.items() if c != 10})
    before = after

    # 无匹配音级：步 12 是 D4 ⇒ Aa+c / Aa+g 不改步；Aa+d 才擦除
    nav_set_focus(s, 0, 12)
    key_with_layer(s, (KEY_AA,), (0x44, KEY_G), 0x02, "②g Aa+c、Aa+g（无匹配）")
    nav_same(s, "②g 无匹配音级 ⇒ 步不变", before, erase_steps(s))
    key_with_layer(s, (KEY_AA,), (KEY_D,), 0x02, "②g Aa+d")
    after = erase_steps(s)
    (s.ok if after[12]["n"] == 0 else s.fail)(
        f"②g Aa+d ⇒ 步 12 的 D4 擦除（n={after[12]['n']}）")
    nav_same(s, "②g Aa+d 只动步 12", {c: v for c, v in before.items() if c != 12},
             {c: v for c, v in after.items() if c != 12})
    before = after

    # 跨八度：步 13 是 C3（48）⇒ Aa+c 同样擦除
    nav_set_focus(s, 0, 13)
    key_with_layer(s, (KEY_AA,), (0x44,), 0x02, "②g Aa+c（C3）")
    after = erase_steps(s)
    (s.ok if after[13]["n"] == 0 else s.fail)(
        f"②g Aa+c ⇒ 步 13 的 C3 擦除（跨八度按音级；n={after[13]['n']}）")
    nav_same(s, "②g 跨八度擦除只动步 13", {c: v for c, v in before.items() if c != 13},
             {c: v for c, v in after.items() if c != 13})
    before = after

    # 两音步 6（D4 + CONT(C4)）：Aa+c 只摘掉 C4，D4 留下（上游 note_del：和弦的一根音）
    nav_set_focus(s, 0, 6)
    key_with_layer(s, (KEY_AA,), (0x44,), 0x02, "②g Aa+c（两音步）")
    after = erase_steps(s)
    st6 = after[6]
    # 存活音仍是 NOTE：time == 0（NOTE 的极性见上）；被摘的 k=1 的 CONT 位随 note_del 左移清零
    (s.ok if st6["n"] == 1 and st6["note0"] == 62 and st6["time"] == 0
     and st6["flags"] & 0xF0 == 0 else s.fail)(
        f"②g 两音步只摘 C4：n={st6['n']} note={st6['note0']} time={st6['time']} "
        f"flags={st6['flags']:#04x}（期望 n=1 note=62 NOTE、CONT 位清）")
    nav_same(s, "②g 两音步擦除只动步 6", {c: v for c, v in before.items() if c != 6},
             {c: v for c, v in after.items() if c != 6})

    # 聚焦鼓轨：ACT_ERASE 没有鼓语义（act_erase 对 k ≥ 3 直接返回）⇒ 位图不变
    s.cmd("SEQ EDIT CLEAR 3 4")
    s.expect(r"SEQ EDIT CLEAR track=3 step=4 OK", "②g 清鼓步 4")
    drum_xy = drum_lane_xy(4, 3)
    s.cmd("TAP %d %d" % drum_xy)
    s.expect(r"-> STEP drum step=4 lane=3 bit=1 CMD_DRUM_STEP_WRITE mask=0x0008",
             "②g TAP 鼓 lane 3（聚焦落到鼓轨）")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 3, "②g 焦点轨 = 鼓轨")
    key_with_layer(s, (KEY_AA,), (0x44, KEY_NOTE_C3), 0x02, "②g Aa+c / Aa+z（鼓轨）")
    (s.ok if seq_dstep_mask(s, 4) == 0x0008 else s.fail)(
        f"②g 鼓轨上 EDIT 层 c / z 无擦除语义：位图 0x{seq_dstep_mask(s, 4):04X}（期望 0x0008）")
    s.cmd("SEQ EDIT CLEAR 3 4")
    s.expect(r"SEQ EDIT CLEAR track=3 step=4 OK", "②g 收口清鼓步 4")
    s.cmd("UI LANE 0")
    s.expect(r"UI LANE lane=0", "②g 收口 lane 归零")

    # 像素：步 8（已擦）与步 10（已擦）回空格底色；步 14（F4 未擦）仍是轨色实心
    nav_set_focus(s, 0, 11)
    time.sleep(0.15)
    img = s.capture("step-edit-erase")
    s.probe(img, "②g 步 8 擦除后空格", *step_cell_xy(8, 0), C_PANEL_HI, 10)
    s.probe(img, "②g 步 10 擦除后空格", *step_cell_xy(10, 0), C_PANEL_HI, 10)
    s.probe(img, "②g 步 14（F4）未受影响", *step_cell_xy(14, 0), C_BLUE)

    # 诊断冻结：同一个**会擦**的键不写引擎——步 14 持有 F4（音级 5），用 Aa+f（不是 c：
    # 音级不匹配时步本来就不变，那样的断言永远绿）。整份 ENG STEP 逐字段比对。
    nav_set_focus(s, 0, 14)
    frozen0 = eng_step(s, 0, 14)
    (s.ok if frozen0["n"] == 1 and frozen0["note0"] == 65 and frozen0["time"] == 0 else s.fail)(
        f"②g 冻结前步 14 = NOTE F4（n={frozen0['n']} note={frozen0['note0']} "
        f"time={frozen0['time']}）")
    s.cmd("ENG FREEZE ON")
    s.expect(r"ENG FREEZE on", "②g ENG FREEZE on")
    key_with_layer(s, (KEY_AA,), (KEY_F,), 0x02, "②g Aa+f（冻结）")
    s.cmd("ENG FREEZE OFF")
    s.expect(r"ENG FREEZE off", "②g ENG FREEZE off")
    frozen1 = eng_step(s, 0, 14)   # 解冻后读：冻结期间 `ENG STEP` 按诊断冻结的读语义回全零
    (s.ok if frozen1 == frozen0 else s.fail)(
        "②g 冻结期间 Aa+f 未写引擎：步 14 逐字段不变" if frozen1 == frozen0 else
        f"②g 冻结期间步 14 被改写：{frozen0} → {frozen1}")

    # 收口：LOAD 入口快照还原工作工程（本段造/擦的音随之消失），LIVEHASH 逐位证明；
    # last_note 回 60、焦点回 T1 入口列。槽 D 入口为空则擦回空。
    proj_snapshot_close(s, "step_edit.erase", 3, "②g")
    erase_audible_restore(s, audible0)
    s.cmd("KEY 0x%02X down" % KEY_NOTE_C4)
    s.expect(rf"KEY 0x{KEY_NOTE_C4:02X} down", "②g 收口 last_note = 60")
    s.cmd("KEY 0x%02X up" % KEY_NOTE_C4)
    s.expect(rf"KEY 0x{KEY_NOTE_C4:02X} up", "②g 收口松键")
    ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "②g 收口 UI step")
    s.cmd("STATE SNAP")
    nav_set_focus(s, min(s.snap_int(r"focus_track=(\d+)"), 2), focus0)  # 入口焦点轨（≤ T3）


def run_step_deep_visuals(s: Session) -> None:
    """②e STEP 深度态：TIE 与 NOTE 不同的空心呈现 + SLIDE/fill/lvl/rat/micro/CONT 可见。

    先用同一 TAP/console 动作造出真实引擎状态（不是伪造渲染），再截屏做像素抽检：
      格 2：NOTE + SLIDE + fill-only + micro +5 + rat 2 击 + HARD；
      格 3：TIE（SPAN 3 从格 2 长出来的 hold）；
      格 4/6：CONT（格 4 的音符长到格 6 的音符上，SF_CONT 标记）。
    结束把测试步清回空（脚本可重复）。"""
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    ensure_window(s, 0, 16)
    ensure_stopped(s)

    # 基线：清掉轨 0 的测试列（CLEAR 对空步是幂等的）
    for step in range(2, 7):
        s.cmd(f"SEQ EDIT CLEAR 0 {step}")
        s.expect(rf"SEQ EDIT CLEAR track=0 step={step} OK", f"②e 基线清步 0/{step}")

    # 音高确定化：q = C4(60)，w = D4(62)（TAP 开步写 last_note）
    for code, label in ((0x21, "q=C4"), (0x22, "w=D4")):
        s.cmd(f"KEY 0x{code:02X} down")
        s.cmd(f"KEY 0x{code:02X} up")
        s.ser.note(f"②e 设置 last_note（{label}）")
    s.cmd("KEY 0x21 down")
    s.cmd("KEY 0x21 up")

    def focus_step_to(target: int) -> None:
        """←/→ 把焦点列移到 target（只前进，不发引擎写）——截屏探针避开焦点白框。"""
        s.cmd("STATE SNAP")
        cur = s.snap_int(r"focus_step=(\d+)")
        for _ in range((target - cur) % 16):
            s.cmd("KEY 0x4C down")
            s.cmd("KEY 0x4C up")
        s.cmd("STATE SNAP")
        s.expect_int(r"focus_step=(\d+)", target, f"②e 焦点列移到 {target}（避开描边）")

    # 格 2 = NOTE(C4)；SPAN 3 → 格 3/4 = TIE
    xy2 = step_cell_xy(2, 0)
    s.cmd("TAP %d %d" % xy2)
    s.expect(r"-> STEP track=0 step=2 CMD_TOGGLE_STEP", "②e TAP 格 2（开）")
    s.cmd("SEQ EDIT SPAN 0 2 3")
    s.expect(r"SEQ EDIT SPAN track=0 step=2 OK", "②e SPAN 3（格 3 长成 TIE）")
    st3 = eng_step(s, 0, 3)
    (s.ok if st3["time"] == 1 and st3["n"] == 0 else s.fail)(
        f"②e 格 3 = TIE（time={st3['time']} n={st3['n']}，期望 time=1 n=0）")

    # 格 2 的深度字段：SLIDE + fill-only + micro +5 + rat（2 击）+ HARD
    s.cmd("SEQ EDIT SLIDE 0 2 1")
    s.expect(r"SEQ EDIT SLIDE track=0 step=2 OK", "②e SLIDE on")
    s.cmd("SEQ FILL 0 2 1")
    s.expect(r"SEQ FILL track=0 step=2 value=1 OK", "②e fill-only")
    s.cmd("SEQ MICRO 0 2 5")
    s.expect(r"SEQ MICRO track=0 step=2 value=5 OK", "②e micro +5")
    s.cmd("SEQ EDIT RAT 0 2 0 1")
    s.expect(r"SEQ EDIT RAT track=0 step=2 OK", "②e rat index0 = 1（x2）")
    s.cmd("SEQ EDIT LVL 0 2 0 3")
    s.expect(r"SEQ EDIT LVL track=0 step=2 OK", "②e lvl index0 = 3（HARD）")

    st2 = eng_step(s, 0, 2)
    for key, want, label in (("time", 0, "NOTE"), ("flags", 2, "SLIDE"),
                             ("fill", 1, "fill-only"), ("micro", 5, "micro +5"),
                             ("lvl", 3, "lvl HARD"), ("rat", 1, "rat x2")):
        (s.ok if st2[key] == want else s.fail)(
            f"②e 格 2 {key}={st2[key]}（期望 {want}，{label}）")

    time.sleep(0.15)  # ≥5 个 UI tick：自绘层按新快照重画
    focus_step_to(10)  # 焦点白框移出被测格（探针不把 3px 描边当字形）
    time.sleep(0.1)
    img = s.capture("step-deep")
    x0, y0 = step_cell_box(2, 0)
    x3, y3 = step_cell_box(3, 0)
    # TIE ≠ NOTE：NOTE 实心轨色；TIE 空心（面板底）+ 轨色内框
    s.probe(img, "②e 格 2 NOTE 实心", x0 + STEP_CELL_W // 2, y0 + STEP_CELL_H // 2, C_BLUE)
    s.probe(img, "②e 格 3 TIE 空心（中心为面板底）", x3 + STEP_CELL_W // 2,
            y3 + STEP_CELL_H // 2, C_PANEL_HI, 10)
    s.probe_region(img, "②e 格 3 TIE 内框（轨色，区别于空格）",
                   (x3 + 2, y3 + 2, x3 + STEP_CELL_W - 2, y3 + 30), C_BLUE, 16, 40)
    # 白色 S（slide）、黄色 F（fill-only）、灰 m+5、白色 x2、黄色 HARD 音符名
    s.probe_region(img, "②e 格 2 SLIDE 标记 S", (x0 + 20, y0 + STEP_TOP_STRIP_Y,
                                                 x0 + 32, y0 + STEP_TOP_STRIP_Y + 14),
                   C_TEXT, 16, 3)
    s.probe_region(img, "②e 格 2 FILL 标记 F", (x0 + 36, y0 + STEP_TOP_STRIP_Y,
                                                x0 + 48, y0 + STEP_TOP_STRIP_Y + 14),
                   C_YELLOW, 16, 3)
    s.probe_region(img, "②e 格 2 micro 文本 m+5", (x0 + 2, y0 + STEP_BOT_STRIP_Y,
                                                   x0 + 40, y0 + STEP_BOT_STRIP_Y + 16),
                   C_TEXT_DIM, 16, 5)
    s.probe_region(img, "②e 格 2 rat 文本 x2", (x0 + 40, y0 + STEP_BOT_STRIP_Y,
                                                x0 + STEP_CELL_W - 4, y0 + STEP_BOT_STRIP_Y + 16),
                   C_TEXT, 16, 3)
    s.probe_region(img, "②e 格 2 HARD 音符名（黄）",
                   (x0 + 10, y0 + STEP_NOTE_Y, x0 + STEP_CELL_W - 4,
                    y0 + STEP_NOTE_Y + STEP_NOTE_ROW), C_YELLOW, 16, 5)

    # CONT：格 6 放 D4，格 2 的 C4 长过去 → 格 6 = NOTE(D4) + CONT(C4)，带左缘标记
    xy6 = step_cell_xy(6, 0)
    s.cmd("KEY 0x22 down")
    s.cmd("KEY 0x22 up")
    s.cmd("TAP %d %d" % xy6)
    s.expect(r"-> STEP track=0 step=6 CMD_TOGGLE_STEP", "②e TAP 格 6（D4）")
    s.cmd("SEQ EDIT SPAN 0 2 5")
    s.expect(r"SEQ EDIT SPAN track=0 step=2 OK", "②e SPAN 5（C4 长到格 6 的 D4 上）")
    st6 = eng_step(s, 0, 6)
    # 字面引擎真值：k=0 = D4(62) 不延续，k=1 = C4(60) 从格 2 延续（SF_CONT(1) = 0x10）
    cont_ok = (st6["n"] == 2 and st6["flags"] == 0x10 and st6["note0"] == 62
               and st6["note1"] == 60)
    (s.ok if cont_ok else s.fail)(
        f"②e 格 6 = NOTE(D4)+CONT(C4)（n={st6['n']} flags=0x{st6['flags']:02X} "
        f"note={st6['note0']},{st6['note1']}，期望 n=2 flags=0x10 note=62,60）")
    time.sleep(0.15)
    focus_step_to(10)  # TAP 格 6 把焦点移回 6；再移开描边
    time.sleep(0.1)
    img = s.capture("step-cont")
    x6, y6 = step_cell_box(6, 0)
    # CONT 标记必须属于**该音符自己**：k=1 的 12×2 下划线在 x+38..49（名字正下方），
    # k=0（D4 无 CONT）既没有下划线、也不该有旧实现的「格左缘竖条」（x+4）。
    s.probe_region(img, "②e 格 6 CONT 下划线在 k=1 名字下方（x+38）",
                   (x6 + 38, y6 + STEP_NOTE_Y + 15, x6 + 50, y6 + STEP_NOTE_Y + 17),
                   C_TEXT, 16, 5)
    under0 = count_region(img, (x6 + 12, y6 + STEP_NOTE_Y + 15, x6 + 24,
                                y6 + STEP_NOTE_Y + 17), C_TEXT, 16)
    (s.ok if under0 == 0 else s.fail)(
        f"②e 格 6 k=0（无 CONT）名字下方无下划线（白像素 {under0}，期望 0）")
    stray = count_region(img, (x6 + 2, y6 + STEP_NOTE_Y + 2, x6 + 9,
                               y6 + STEP_NOTE_Y + 14), C_TEXT, 16)
    (s.ok if stray == 0 else s.fail)(
        f"②e 格 6 k=0 名字左缘无旧实现竖条（白像素 {stray}，期望 0）")

    # 清理：SPAN 1 收回 TIE/CONT，再清两格（脚本可重复）
    s.cmd("SEQ EDIT SPAN 0 2 1")
    s.expect(r"SEQ EDIT SPAN track=0 step=2 OK", "②e SPAN 1（收回 TIE/CONT）")
    for step in (2, 6):
        s.cmd(f"SEQ EDIT CLEAR 0 {step}")
        s.expect(rf"SEQ EDIT CLEAR track=0 step={step} OK", f"②e 清理格 0/{step}")
        d = eng_step(s, 0, step)
        (s.ok if d["n"] == 0 and d["time"] != 0 else s.fail)(
            f"②e 清理后格 0/{step} 为空（n={d['n']} time={d['time']}）")


def run_step_drum_lanes(s: Session) -> None:
    """②f 鼓 lane：点 dot 选 lane 并切换该步原始位；键盘 ↑/↓ 选 lane、ENTER 切位。

    断言引擎真值（SEQ DSTEP 的 on 位图）＋ TAP 回显（lane/bit/mask）＋ UI LANE
    （选中 lane）＋ 像素（点亮的 dot 取 lane 主题色、选中 ring 白框）。"""
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    ensure_window(s, 0, 16)
    ensure_stopped(s)

    s.cmd("SEQ EDIT CLEAR 3 5")
    s.expect(r"SEQ EDIT CLEAR track=3 step=5 OK", "②f 鼓步 5 清空")
    (s.ok if seq_dstep_mask(s, 5) == 0 else s.fail)("②f 鼓步 5 基线位图 = 0")

    s.cmd("UI LANE")
    s.expect(r"UI LANE lane=(\d+)", "②f UI LANE 读回")
    s.cmd("UI LANE 0")
    s.expect(r"UI LANE lane=0", "②f UI LANE 归零")
    s.cmd("UI LANE 16")  # 越界：UI ERR 令牌（不是用法文案），且不改选中 lane
    s.expect(r"UI ERR lane", "②f UI LANE 越界回 UI ERR lane")
    s.cmd("UI LANE 99")
    s.expect(r"UI ERR lane", "②f UI LANE 99 回 UI ERR lane")
    s.cmd("UI LANE x")
    s.expect(r"UI ERR lane", "②f UI LANE 非数字回 UI ERR lane")
    s.cmd("UI LANE")
    s.expect(r"UI LANE lane=0", "②f 越界不改选中 lane")

    # 点 lane 2 的 dot：选中 + 置位；再点一次：清除（同一路径）
    lane2 = drum_lane_xy(5, 2)
    s.cmd("TAP %d %d" % lane2)
    s.expect(r"-> STEP drum step=5 lane=2 bit=1 CMD_DRUM_STEP_WRITE mask=0x0004",
             "②f TAP lane dot（选中并置位）")
    (s.ok if seq_dstep_mask(s, 5) == 0x0004 else s.fail)(
        f"②f 鼓步 5 位图 = 0x{seq_dstep_mask(s, 5):04X}（期望 0x0004）")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 3, "②f TAP 后焦点轨 = 鼓轨")

    s.cmd("TAP %d %d" % lane2)
    s.expect(r"-> STEP drum step=5 lane=2 bit=0 CMD_DRUM_STEP_WRITE mask=0x0000",
             "②f 再点同一 dot（清除）")
    (s.ok if seq_dstep_mask(s, 5) == 0 else s.fail)("②f 鼓步 5 位图回 0")

    # 格内非 dot 位置 = 切换「选中 lane」（仍为 2）
    cell = step_cell_xy(5, 3)
    s.cmd("TAP %d %d" % (cell[0], cell[1] - STEP_CELL_H // 2 + 12))
    s.expect(r"-> STEP drum step=5 lane=2 bit=1 CMD_DRUM_STEP_WRITE mask=0x0004",
             "②f TAP 格内空白（切换选中 lane）")
    s.cmd("UI LANE")
    s.expect(r"UI LANE lane=2", "②f 选中 lane 不变")
    (s.ok if seq_dstep_mask(s, 5) == 0x0004 else s.fail)("②f 位图 = 0x0004（选中 lane 置位）")

    # 键盘：ENTER = 切换聚焦步的选中 lane；↑/↓ = lane 步进（环：T1,T2,T3,L1..L16）
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    s.expect(r"KEY 0x3D", "②f KEY ENTER")
    (s.ok if seq_dstep_mask(s, 5) == 0 else s.fail)("②f ENTER 清除选中 lane 位")
    s.cmd("KEY 0x3B down")  # ↑
    s.cmd("KEY 0x3B up")
    s.cmd("UI LANE")
    s.expect(r"UI LANE lane=1", "②f ↑ 选 lane 1")
    s.cmd("KEY 0x4B down")  # ↓
    s.cmd("KEY 0x4B up")
    s.cmd("UI LANE")
    s.expect(r"UI LANE lane=2", "②f ↓ 回 lane 2")
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    (s.ok if seq_dstep_mask(s, 5) == 0x0004 else s.fail)("②f ENTER 置位 lane 2")

    time.sleep(0.15)
    img = s.capture("step-drum-lane")
    dx, dy = drum_lane_xy(5, 2)
    s.probe(img, "②f lane 2 dot 已点亮（鼓轨色）", dx, dy, C_ORANGE, 12)
    s.probe_region(img, "②f 选中 lane 2 ring（白框）", (dx - 6, dy - 6, dx + 6, dy + 6),
                   C_TEXT, 16, 8)
    ex, ey = drum_lane_xy(5, 3)
    s.probe(img, "②f lane 3 dot 未点亮（暗网格）", ex, ey, C_PANEL_SEL, 10)

    # 环的边界：lane 15 ↓ 离开鼓轨到 T1；T1 ↑ 从 lane 15 进入鼓轨；lane 0 ↑ 回 T3
    s.cmd("UI LANE 15")
    s.expect(r"UI LANE lane=15", "②f 归一到 lane 15")
    s.cmd("KEY 0x4B down")
    s.cmd("KEY 0x4B up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 0, "②f lane 15 ↓ → 焦点轨回 T1")
    s.cmd("KEY 0x3B down")
    s.cmd("KEY 0x3B up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 3, "②f T1 ↑ → 回到鼓轨")
    s.cmd("UI LANE")
    s.expect(r"UI LANE lane=15", "②f 从 T1 进入落在 lane 15")
    s.cmd("UI LANE 0")
    s.cmd("KEY 0x3B down")
    s.cmd("KEY 0x3B up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 2, "②f lane 0 ↑ → 焦点轨 T3")

    # 冻结：TAP 必须回 ERR frozen 并报**写后真值**（位图不变），不能伪造翻转后的 mask。
    # （诊断冻结只冻结引擎读写，UI/TAP 不处于 quiesce，命令仍会被派发。）
    (s.ok if seq_dstep_mask(s, 5) == 0x0004 else s.fail)("②f 冻结前鼓步 5 位图 = 0x0004")
    s.cmd("ENG FREEZE ON")
    s.expect(r"ENG FREEZE on", "②f ENG FREEZE on")
    s.cmd("TAP %d %d" % lane2)
    s.expect(r"-> STEP drum step=5 lane=2 ERR frozen mask=0x0004",
             "②f 冻结时 TAP 回 ERR frozen 且 mask 为写前真值")
    s.cmd("ENG FREEZE OFF")
    s.expect(r"ENG FREEZE off", "②f ENG FREEZE off")
    (s.ok if seq_dstep_mask(s, 5) == 0x0004 else s.fail)("②f 冻结期间位图未变（0x0004）")

    # 隐藏列：窗口 len=8 时鼓列 8 不在命中表（TAP miss，不写位图）
    ensure_window(s, 0, 8)
    time.sleep(0.15)
    hidden = drum_lane_xy(8, 2)
    s.cmd("TAP %d %d" % hidden)
    s.expect(rf"TAP miss {hidden[0]},{hidden[1]} page=2", "②f 隐藏鼓列 8 TAP miss")
    (s.ok if seq_dstep_mask(s, 5) == 0x0004 else s.fail)("②f 隐藏列不写鼓位图")
    ensure_window(s, 0, 16)
    time.sleep(0.15)

    # 合成轨上的 ENTER 是无操作（不写鼓、不改 seq_status、不改引擎）
    for _ in range(20):
        s.cmd("STATE SNAP")
        if s.snap_int(r"focus_track=(\d+)") == 0:
            break
        s.cmd("KEY 0x3B down")
        s.cmd("KEY 0x3B up")
    s.cmd("STATE SNAP")
    s.expect_int(r"focus_track=(\d+)", 0, "②f 焦点归一 T1（ENTER no-op 前提）")
    s.cmd("STATE SNAP")
    seq0 = s.snap_int(r"seq_status=(\d+)")
    # ENTER 在合成轨由 page_step.c:1118 直接返回；voices 是前序音频工作的异步读数，不能当 ENTER 的副作用。
    eng0 = [re.sub(r" voices=\d+", "", line) for line in nav_eng_snapshot(s)]
    mask0 = seq_dstep_mask(s, 5)
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    s.cmd("STATE SNAP")
    s.expect_int(r"seq_status=(\d+)", seq0, "②f ENTER 在合成轨不改 seq_status")
    (s.ok if seq_dstep_mask(s, 5) == mask0 else s.fail)("②f ENTER 在合成轨不写鼓步")
    eng1 = [re.sub(r" voices=\d+", "", line) for line in nav_eng_snapshot(s)]
    (s.ok if eng1 == eng0 else s.fail)("②f ENTER 在合成轨不改引擎快照（排除异步 voices）")

    # 收口：清鼓步、lane 归零、停止 + 默认窗口
    s.cmd("SEQ EDIT CLEAR 3 5")
    s.expect(r"SEQ EDIT CLEAR track=3 step=5 OK", "②f 收口清鼓步 5")
    s.cmd("UI LANE 0")
    s.expect(r"UI LANE lane=0", "②f 收口 lane 归零")
    ensure_window(s, 0, 16)


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


    run_tracks_keys(s)


# 卡内 "MUTE x   SOLO y" 文本（page_tracks.c：卡内 (24,250)，宽 248）：屏幕绝对坐标的像素区。
TRACKS_MIX_BOX = tuple((24 + k * TRACKS_CARD_PITCH + 24, 72 + 40 + 250,
                        24 + k * TRACKS_CARD_PITCH + 24 + 248, 72 + 40 + 250 + 30)
                       for k in range(4))
KEY_GLO, KEY_SEL = 0x41, 0x40      # K_ALT = APP_LAYER_GLO(0x08) / K_CTRL = APP_LAYER_SEL(0x04)
# 演奏键位表（input_map.h 的 ACT_NOTE 行）：音符 → 键码，48 = 低八度行 z（C3）… 72 = 高八度行 i
# C4（60）两个键都可达（低八度行 "," = 高八度行 q）；取 q，与其它场景的 last_note 归一同一键。
KEY_FOR_NOTE = {48: 0x42, 49: 0x33, 50: 0x43, 51: 0x34, 52: 0x44, 53: 0x45, 54: 0x36, 55: 0x46,
                56: 0x37, 57: 0x47, 58: 0x38, 59: 0x48, 60: 0x21, 61: 0x02, 62: 0x22, 63: 0x03,
                64: 0x23, 65: 0x24, 66: 0x05, 67: 0x25, 68: 0x06, 69: 0x26, 70: 0x07, 71: 0x27,
                72: 0x28}
KEY_1 = 0x01                       # K_1..K_8 = 0x01..0x08：GLO 1–4 mute / 5–8 solo，SEL 1–4 选轨


def trk_mix(s: Session) -> dict:
    """`ENG SNAP` 四轨的 (mute, armed, solo)——引擎真值（轨号 → 三元组）。"""
    s.cmd("ENG SNAP")
    rows = re.findall(r"ENG SNAP trk k=(\d) \S+ .*? mute=(\d) armed=(\d) solo=(\d)", s.last)
    if len(rows) != 4:
        raise InjectError(f"ENG SNAP trk 行不完整（{len(rows)}/4）：{s.last!r}")
    return {int(k): (int(m), int(a), int(so)) for k, m, a, so in rows}


def key_with_layer(s: Session, layers: tuple, codes: tuple, held_mask: int, label: str) -> None:
    """按住层键 → 依次按下/释放 codes → 松开层键：一次 `cmd_batch` 写入（行间无主机延迟，
    每个 down 都配 up），批内两份 `STATE SNAP` 自证按下期间 `layer_held` == held_mask、
    松开后回 0。逐行断言 `KEY` 回显的顺序（命令都被派发、未被吞）。"""
    lines = [f"KEY 0x{c:02X} down" for c in layers] + ["STATE SNAP"]
    for c in codes:
        lines += [f"KEY 0x{c:02X} down", f"KEY 0x{c:02X} up"]
    lines += [f"KEY 0x{c:02X} up" for c in reversed(layers)] + ["STATE SNAP"]
    s.cmd_batch(lines)
    want = [("down", c) for c in layers] + [(ev, c) for c in codes for ev in ("down", "up")] + \
           [("up", c) for c in reversed(layers)]
    got = [(ev, int(c, 16)) for c, ev in
           re.findall(r"KEY 0x([0-9A-F]{2}) (down|up) \S+ elapsed_us=\d+", s.last)]
    (s.ok if got == want else s.fail)(
        f"{label}: KEY 回显 {len(got)}/{len(want)} 行、顺序{'一致' if got == want else '不一致'}"
        + ("" if got == want else f"（实得 {got}）"))
    held = re.findall(r"layer_held=0x([0-9A-F]{2})", s.last)
    (s.ok if held == [f"{held_mask:02X}", "00"] else s.fail)(
        f"{label}: 按下期间 layer_held={'/'.join(held) or '缺'}（期望 {held_mask:#04x} → 0x00）")


def run_tracks_keys(s: Session) -> None:
    """GLO 层（Alt）与 SEL 层（Ctrl）的 KEY 注入（P4-F Task 7，ADR-006 / 012 §2.3）：
    Alt+1..4 = 轨 0..3 静音翻转、Alt+5..8 = 独奏翻转、Ctrl+1..4 = 选轨（同时设焦点轨）。
    断言引擎真值（`ENG SNAP trk`）+ 状态投影（`STATE SNAP`）+ TRACKS 卡像素区；
    两层同按时同键取 GLO（查表「GLO > SEL > EDIT」）；不按层键时数字键不是混音键。
    停止态（走带中的 mute 是 armed 语义，不在本用例）；结束把四轨 mute/solo 与选轨还原。"""
    s.ser.note("GLO/SEL 层键：Alt+1..4 mute / Alt+5..8 solo / Ctrl+1..4 选轨（停止态）")
    ensure_stopped(s)
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "层键：UI tracks")
    s.cmd("STATE SNAP")
    sel0 = s.snap_int(r"selected_track=(\d+)")
    last0 = s.snap_int(r"last_note=(\d+)")
    base = trk_mix(s)
    # 鼓轨上的数字键：没有 lane 映射 ⇒ 不发声；不按层键时更不是混音键（判别：层键是必要前提）
    select_track(s, 3)
    key_with_layer(s, (), (KEY_1, 0x05), 0x00, "无层键：1 / 5")
    nav_same(s, "无层键时 1 / 5 不改混音", base, trk_mix(s))
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 3, "无层键时 1 / 5 不改选轨")
    s.cmd("KEY PANIC")
    s.expect(r"KEY PANIC", "层键：KEY PANIC（收口按键状态）")

    # GLO 1–4：逐轨静音翻转（一次按住、四键一批）
    key_with_layer(s, (KEY_GLO,), tuple(KEY_1 + k for k in range(4)), 0x08, "Alt+1..4")
    got = trk_mix(s)
    for k in range(4):
        want = (1 - base[k][0],) + base[k][1:]
        (s.ok if got[k] == want else s.fail)(
            f"Alt+{k + 1} ⇒ 轨 {k} mute {base[k][0]} → {got[k][0]}（REC 位/solo 不变："
            f"{got[k][1:]}）")
    # GLO 5–8：逐轨独奏翻转；静音位不再变
    mid = got
    key_with_layer(s, (KEY_GLO,), tuple(KEY_1 + 4 + k for k in range(4)), 0x08, "Alt+5..8")
    got = trk_mix(s)
    for k in range(4):
        want = mid[k][:2] + (1 - mid[k][2],)
        (s.ok if got[k] == want else s.fail)(
            f"Alt+{k + 5} ⇒ 轨 {k} solo {mid[k][2]} → {got[k][2]}（mute/armed 不变）")
    # TRACKS 卡片反映键盘混音：先把轨 1 的静音翻回去，卡 1 的 "MUTE x   SOLO y" 像素区变化，
    # 其余三卡逐字节不变（选中态仍是鼓轨，卡的底色/描边不动）
    select_track(s, 3)
    time.sleep(0.15)  # ≥5 拍：卡的脏检查刷新
    img_before = s.capture("tracks-keys-before")
    key_with_layer(s, (KEY_GLO,), (0x02,), 0x08, "Alt+2（卡 1 静音回翻）")
    time.sleep(0.15)
    img_after = s.capture("tracks-keys-after")
    crops = [img.crop(b).tobytes() for img in (img_before, img_after) for b in TRACKS_MIX_BOX]
    (s.ok if crops[1] != crops[5] else s.fail)(
        "Alt+2 后 TRACKS 卡 1 的 MUTE/SOLO 文本像素区变化")
    for k in (0, 2, 3):
        (s.ok if crops[k] == crops[4 + k] else s.fail)(
            f"Alt+2 后卡 {k} 像素区逐字节不变")
    got = trk_mix(s)
    (s.ok if got[1][0] == base[1][0] else s.fail)(
        f"Alt+2 回翻：轨 1 mute={got[1][0]}（期望 {base[1][0]}）")

    # 两层同按：同键取 GLO（静音/独奏），不是选轨（`armed` 是该轨的 REC 位，全程不变）
    s.cmd("STATE SNAP")
    sel_before = s.snap_int(r"selected_track=(\d+)")
    mix_before = trk_mix(s)
    key_with_layer(s, (KEY_GLO, KEY_SEL), (KEY_1, 0x05), 0x0C, "Alt+Ctrl 同按")
    mix_both = trk_mix(s)
    (s.ok if mix_both[0][0] == 1 - mix_before[0][0] and mix_both[0][2] == 1 - mix_before[0][2]
     and all(mix_both[k][1:] == mix_before[k][1:] for k in (1, 2, 3))
     and all(mix_both[k][0] == mix_before[k][0] for k in (1, 2, 3)) else s.fail)(
        "Alt+Ctrl 同按：1 / 5 走 GLO（轨 0 mute 与 solo 翻转），不选轨")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", sel_before, "Alt+Ctrl 同按：选轨不变")

    # SEL 1–4：选轨（同时设焦点轨）；重复按同键幂等
    for key, track in ((0x03, 2), (KEY_1, 0), (KEY_1, 0), (0x04, 3), (0x02, 1)):
        key_with_layer(s, (KEY_SEL,), (key,), 0x04, f"Ctrl+{key - KEY_1 + 1}")
        s.cmd("STATE SNAP")
        s.expect_int(r"selected_track=(\d+)", track, f"Ctrl+{key - KEY_1 + 1} ⇒ selected_track")
        s.expect_int(r"focus_track=(\d+)", track, f"Ctrl+{key - KEY_1 + 1} ⇒ focus_track")
    nav_same(s, "SEL 层不改混音", mix_both, trk_mix(s))
    # 层键松开后 layer_held 回 0：此刻的 4 键不再是选轨（若 SEL 仍生效，选轨会变 3；
    # 当前选中轨 1 ≠ 3 ⇒ 有判别力。K_4 在基础层**没有**绑定（高行黑键是 2/3/5/6/7），
    # 不按层键时它既不出声也不改 last_note；收口仍按入口值还原 last_note）
    s.cmd("KEY 0x04 down")
    s.expect(r"KEY 0x04 down 4 elapsed_us=", "松开 Ctrl 后按 4（基础层无绑定：不出声）")
    s.cmd("KEY 0x04 up")
    s.cmd("STATE SNAP")
    s.expect_int(r"layer_held=0x([0-9A-F]{2})", 0, "松开 Ctrl 后 layer_held = 0")
    s.expect_int(r"selected_track=(\d+)", 1, "松开 Ctrl 后 4 键不选轨（仍轨 1）")
    # 像素：Ctrl+4 选到鼓轨 ⇒ 卡 3 选中（6px 橙描边 + 亮底）
    key_with_layer(s, (KEY_SEL,), (0x04,), 0x04, "Ctrl+4（像素）")
    time.sleep(0.15)
    img = s.capture("tracks-keys-sel4")
    s.probe(img, "Ctrl+4 卡 3 选中描边（6px）", 24 + 3 * TRACKS_CARD_PITCH - 4, 347, C_ORANGE)
    s.probe(img, "Ctrl+4 卡 3 选中底", 24 + 3 * TRACKS_CARD_PITCH + 148, 552, C_PANEL_SEL, 8)

    # 收口：mute/solo 回基线（同一 GLO 键路径），选轨回起点
    cur = trk_mix(s)
    fix = []
    for k in range(4):
        if cur[k][0] != base[k][0]:
            fix.append(KEY_1 + k)
        if cur[k][2] != base[k][2]:
            fix.append(KEY_1 + 4 + k)
    if fix:
        key_with_layer(s, (KEY_GLO,), tuple(fix), 0x08, "收口 GLO")
    nav_same(s, "收口后四轨 mute/armed/solo 回基线", base, trk_mix(s))
    key_with_layer(s, (KEY_SEL,), (KEY_1 + sel0,), 0x04, "收口 SEL")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", sel0, "收口 selected_track")
    # last_note 回入口值（上面的无层 4 键把它改成了 63）：弹一下对应的键；入口值不在键位表内
    # （0 = 从未弹过，或超出 48..72）时回 60
    restore = last0 if last0 in KEY_FOR_NOTE else 60
    k = KEY_FOR_NOTE[restore]
    s.cmd(f"KEY 0x{k:02X} down")
    s.expect(rf"KEY 0x{k:02X} down", f"收口 last_note 回 {restore}")
    s.cmd(f"KEY 0x{k:02X} up")
    s.expect(rf"KEY 0x{k:02X} up", "收口松键")
    s.cmd("STATE SNAP")
    s.expect_int(r"last_note=(\d+)", restore, "收口 last_note")
    s.cmd("KEY PANIC")
    s.expect(r"KEY PANIC", "层键：收口 KEY PANIC")


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
# 跨组切换 / 组内首次构建兄弟页的延迟预算（ADR-005 §3；UI MEM 的 GROUP build_ms）
NAV_BUILD_BUDGET_MS = 100
# TAB 扁平整轮顺序（5B-1 起 13 页 / 9 组：SOUND 取代 ANALOG+FM6，独立 FM6 页删除、
# APP_PAGE_FM6=7 是空位 id；五张族页 GLOBAL/FX/MOD/SCALE/VOICE 退役、id 4/5/15/16/17 不注册）——
# group 顺序 + 组内顺序，与 ui_nav.h 的 UI_GROUP_LIST 同源。
NAV_ORDER = ((0, "play"), (1, "tracks"), (6, "sound"), (18, "tools"), (8, "sample"), (12, "dsyn"),
             (13, "fm6p"), (2, "step"), (3, "transport"), (11, "song"), (9, "projects"),
             (10, "presets"), (14, "system"))
# 组表（与 ui_nav.h 的 UI_GROUP_LIST 同源：组顺序 = 组签顺序，成员顺序 = 子签顺序）；平铺后
# 必须等于 NAV_ORDER（run_nav 入口校验）。有序对全矩阵（⑩）与九组签逐组像素（⑨）由它驱动。
NAV_GROUP_PAGES = {"PLAY": ("play",), "TRACKS": ("tracks",),
                   "EDIT": ("sound",),
                   "MIX": ("tools",), "SYNTH": ("sample", "dsyn", "fm6p"),
                   "SEQ": ("step", "transport"), "SONG": ("song",),
                   "FILE": ("projects", "presets"), "SYS": ("system",)}
# 跨组过渡峰值的门限（ADR-005 §5：构建目标页时源组仍全部驻留，构建日志的「池剩余」即峰值点）
NAV_POOL_FLOOR = 4096
# 跨组往返的池泄漏回归（R34/ADR-008 §3）：每页 styles_init 只跑一次后，PLAY↔TRACKS
# 往返不应改变 pool_free。阈值 = 「噪声」（TLSF 块拆分/文本缓存的个位数字节；实测 0）。
NAV_LEAK_TRIPS = 10
NAV_LEAK_NOISE = 128
# 非页面基础占用（`pool_used − Σdelta(已建页)`，只建 PLAY 的稳态）：P4-C Task 11 时点
# 11,624B，P4-F 重测 13,950–14,250B（九组组签、当时的 18 页注册表、ZOOM label），文档取 14,000
# 记账（ADR-005 §5.5「base 重测」）。按它记，最坏对 SEQ→PRESETS 纸面超 32B——真门禁是
# 实测（构建日志池剩余 ≥4,096），见 DI-56。只用于 transcript 注记。
UI_BASE_BYTES = 14000
# STEP 页右上角状态行（page_step.c：状态标签 (656,14) 宽 600 右对齐，16pt）：
# 文本由聚焦步/选中 drum lane 派生（P4-C Task 9）：合成轨 `T1 S3 NOTE C4 …`（轨色），
# 鼓轨 `DRM S6 LANE 3 SNARE ON …`（橙）。焦点换轨即换色 ⇒ nav 用它判「焦点落到鼓轨」。
NAV_STATUS_BOX = (1040, 82, 1258, 112)


def count_region(img, box: tuple, rgb: tuple, tol: int) -> int:
    """区域内与 rgb 各通道差 ≤ tol 的像素数（RGB565 量化后口径，同 probe_region）。"""
    colors = img.crop(box).getcolors(maxcolors=1 << 20) or []
    return sum(c for c, px in colors
               if max(abs(px[0] - rgb[0]), abs(px[1] - rgb[1]), abs(px[2] - rgb[2])) <= tol)


# 行 2（track 2 / SAMPLE）的轨色 = UI_ACCENT（黄）⇒ 亮格的填充与播放头描边同色，
# 像素上无法区分「亮格」与「播放头」；该行的播放头判据只要求期望列**在**检测到的列里，
# 不要求唯一（bank1 窗口外的「无残留描边」判据同样排除该行）。
PLAYHEAD_AMBIGUOUS_ROWS = {2}

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
    ↑/↓ 在「T1,T2,T3,DRM L1..L16」环上走、←/→ 在窗口列循环，都不发引擎写。"""
    s.cmd("STATE SNAP")
    t0, s0 = s.snap_int(r"focus_track=(\d+)"), s.snap_int(r"focus_step=(\d+)")
    if t0 >= 3:
        # 鼓轨上 ↑/↓ 是 lane 步进：先逐 lane 上行离开鼓轨（最多 16 lane），再按三轨环归一
        for _ in range(16 + 1):
            s.cmd("KEY 0x3B down")
            s.cmd("KEY 0x3B up")
            s.cmd("STATE SNAP")
            t0 = s.snap_int(r"focus_track=(\d+)")
            if t0 < 3:
                break
    # STEP 页的轨轴是焦点环 T1,T2,T3,DRM L1..L16（↓ = 环上下一项）：目标在下方且不出 T3
    # 才按 ↓，否则按 ↑（从鼓轨 ↑ 出来落在 T3 后要 ↓ 绕回 T1 会重新进鼓轨）
    if t0 + (track - t0) % 3 <= 2:
        key_down, n = 0x4B, (track - t0) % 3
    else:
        key_down, n = 0x3B, t0 - track
    for _ in range(n):
        s.cmd(f"KEY 0x{key_down:02X} down")
        s.cmd(f"KEY 0x{key_down:02X} up")
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


def nav_pool_free(s: Session) -> int:
    """`UI MEM` 首行的 pool_free（LVGL 池剩余，字节）。"""
    mem = param_mem(s)
    return mem["head"]["free"]


def nav_wait_page(s: Session, name: str, pid: int, label: str, timeout: float = 2.0) -> None:
    """轮询 `STATE SNAP page=` 等到异步 tab 切页落地，再用 `UI` 回显复核。

    不能用 `UI` 轮询：同步切页会**作废**待执行的异步请求（R30），把要等的切页取消掉。
    STATE SNAP 只读状态层投影，不碰邮箱。拍迟滞实测可达 ~270ms，故超时取 2s。"""
    deadline = time.monotonic() + timeout
    while True:
        s.cmd("STATE SNAP")
        m = re.search(r"STATE SNAP page=(\d+)", s.last)
        if m and int(m.group(1)) == pid:
            s.cmd("UI")
            s.expect(rf"UI page={name} \({pid}\)", label)
            return
        if time.monotonic() > deadline:
            raise InjectError(f"{label}: 等待 {timeout:.1f}s 仍不在 {name} ({pid})："
                              f"{s.last.strip()!r}")
        time.sleep(0.05)


def run_nav(s: Session) -> None:
    """nav 场景：导航行为（阶段 4 路由重构最易静默破坏）+ 组作用域生命周期。

    ① 组签缝与组签矩形内的 TAP 均 → `TAP miss`，STATE/ENG 快照逐字段不变；
    ② STEP 鼓轨行 TAP 编辑 lane：回显 `-> STEP drum … CMD_DRUM_STEP_WRITE`、焦点落到
       鼓轨（T9 起鼓轨不再只读）、ENG SNAP 除该步的 dstep 行外逐字段不变（R9：聚合行在
       起点位图已含该位时不变形，原始位图另由 `SEQ DSTEP` 断言）、鼓格像素变化；
       再 TAP 一次复位位图，状态行由合成轨（蓝）换成鼓轨（橙）；
    ③ 同一 30ms 拍内 KEY tab + KEY PANIC：批内 UI 自证请求尚未被消费，PANIC 后
       1s（≈33 个 30ms 拍）仍停在原页（撤销待切页，无回跳）；
    ④ 同一 30ms 拍内 3 组 KEY tab 按下/释放：1s 后只前进一页（ADR-006「一拍
       一页」），再 1s 无第二次前进；`TAB` 顺序 = 组顺序扁平轮转
       PLAY→TRACKS→SOUND→…（全序列见 ⑦）；
    ⑤ 组作用域生命周期：MIX→SEQ 后 MIX 组 built=0（含原当前页 TOOLS），同组切换只改
       显隐（STEP 不重建），build_ms 0 < x ≤100ms（ADR-005 §3/§5）；
    ⑥ 手指导航（`UI TAB`/`UI SUB` 与物理组签/子签同一动作函数，R32）：点组进上次
       子页且记忆跨往返、跨组点组销毁源组并建目标首个子页、子签同组切换不重建；
       同一 30ms 拍内 `KEY tab` 排队时 `UI TAB` 胜出且待切页被丢弃（R30）；
       5B-1 起 EDIT（SOUND）/ MIX（TOOLS）是单页组：子行为空、`UI SUB` 一律越界；FILE 组
       两页、SONG 单页组、**SYNTH 组三页**（P4-D Task 8：`UI TAB synth` / `UI SUB 0..2`、
       离开组 built=0）各走一遍；
    ⑦ `TAB` 全 13 页轮转（5B-1：PLAY→TRACKS→SOUND→TOOLS→SAMPLE→DSYN→FM6P→STEP→
       TRANSPORT→SONG→PROJECTS→PRESETS→SYSTEM→PLAY），异步切页用 STATE SNAP 轮询等
       落地、回卷到 PLAY；
    ⑧ 跨组往返不泄漏（R34）：PLAY↔TRACKS ×10 的 `UI MEM` pool_free 首末差 ≤ 噪声，
       且无逐次递增（每页 `styles_init` 只跑一次）；
    ⑨ 九组签逐组像素（P4-F Task 7 定稿）：`UI TAB <g>` 后该组签唯一选中（104px 的左右
       内缘）、其余未选中、子签数 = 成员数、只有本组已建；
    ⑩ 有序对全矩阵（ADR-005 §5）：源组全建 → 跨组首建目标页，构建日志 `池剩余 ≥ 4,096`
       （9×8 组对、逐目标页；含 EDIT→STEP / MIX→STEP / SEQ→MIX / MIX→FILE），
       收尾断言 `lvgl_stack_free ≥ 2,048`。
    """
    if [n for members in NAV_GROUP_PAGES.values() for n in members] != [n for _, n in NAV_ORDER]:
        raise InjectError("NAV_GROUP_PAGES 平铺与 NAV_ORDER 不一致（组表漂移）")
    ensure_stopped(s)
    ensure_window(s, 0, 16)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "UI step")
    time.sleep(0.1)  # ≥3 拍：切页首帧刷新稳定

    # 基线归一（所有截屏的确定性来源）：合成轨格 TAP 两次（落回原值，脚本可重复），
    # 顺带把状态行（聚焦步派生文本）从上一轮残留换成 T1 的文本——这样每次运行的
    # 状态行都从同一文本出发，PNG 跨运行逐字节一致；也让「焦点落到鼓轨后状态行变橙」
    # 在重复运行下仍可判定。第 2 列与用例②的目标焦点列一致，焦点只需换轨。
    ctrl_xy = step_cell_xy(2, 0)
    s.cmd("TAP %d %d" % ctrl_xy)
    s.expect(r"-> STEP track=0 step=2 CMD_TOGGLE_STEP", "基线：合成轨格 TAP（对照组）")
    s.cmd("TAP %d %d" % ctrl_xy)
    s.expect(r"-> STEP track=0 step=2 CMD_TOGGLE_STEP", "基线：合成轨格再 TAP（落回原值）")

    # ---------- ① 页签区 TAP：miss 且状态/引擎不变 ----------
    # 两个点都必须在 TAP 路线上 miss：组签缝（页签条内、所有组签/子签矩形外）与
    # 组签矩形内部（PLAY 组签中心）。后者钉住「页签矩形被接进 TAP 路由」的回归——
    # 两行页签的切换走 LVGL 回调/`UI`/`KEY tab`，TAP 只派发页面区控件（y≥72）。
    gap_xy = tab_gap_xy(0)
    tab_xy = group_tab_xy(0)  # PLAY 组签中心
    s.ser.note(f"用例①：TAP 组签缝 {gap_xy[0]},{gap_xy[1]} 与组签矩形内 "
               f"{tab_xy[0]},{tab_xy[1]}（均须 miss）")
    st1 = nav_state_fields(s)
    eng1 = nav_eng_snapshot(s)
    s.cmd("TAP %d %d" % gap_xy)
    s.expect(rf"TAP miss {gap_xy[0]},{gap_xy[1]} page=2", "① 组签缝 TAP miss")
    s.cmd("TAP %d %d" % tab_xy)
    s.expect(rf"TAP miss {tab_xy[0]},{tab_xy[1]} page=2", "① 组签矩形内 TAP miss")
    nav_same(s, "① STATE SNAP", st1, nav_state_fields(s))
    nav_same(s, "① ENG SNAP", eng1, nav_eng_snapshot(s))
    img_step = s.capture("nav-01-tabbar")
    s.probe(img_step, "① 组签缝底（页签条 UI_PANEL）", *gap_xy, C_PANEL, 10)
    s.probe(img_step, "① 子签选中（STEP）", *C_TAB_STEP, C_YELLOW)
    s.probe(img_step, "① 组签选中（SEQ）", *C_TAB_SEQ, C_YELLOW)
    s.probe(img_step, "① 组签未选中（PLAY）", *C_TAB_PLAY, C_PANEL_HI, 10)
    # 104px 组签（P4-F Task 4：9×104+8×8=1,000，右端 1,016）：选中签的左右内缘都是
    # 强调色（签宽确实 104，不是旧的 122）、签间 8px 缝与第 9 张（SYS）右缘之外是
    # 页签条底色、新组签 MIX 存在且未选中。
    g5 = UI_TAB_X0 + 5 * (UI_GROUP_W + UI_TAB_GAP)
    g8 = UI_TAB_X0 + 8 * (UI_GROUP_W + UI_TAB_GAP)
    s.probe(img_step, "① 组签选中（SEQ）左内缘", g5 + 20, UI_GROUP_Y + 4, C_YELLOW)
    s.probe(img_step, "① 组签选中（SEQ）右内缘（104px）", g5 + UI_GROUP_W - 8, UI_GROUP_Y + 4,
            C_YELLOW)
    s.probe(img_step, "① 组签缝（SEQ|SONG 8px）", g5 + UI_GROUP_W + 4, UI_GROUP_Y + 12, C_PANEL, 10)
    s.probe(img_step, "① 第 9 张组签（SYS）填充", *C_TAB_SYS, C_PANEL_HI, 10)
    s.probe(img_step, "① SYS 右内缘（右端 1,016）", g8 + UI_GROUP_W - 8, UI_GROUP_Y + 4,
            C_PANEL_HI, 10)
    s.probe(img_step, "① 组签行右端之外是页签条底色", g8 + UI_GROUP_W + 4, UI_GROUP_Y + 12,
            C_PANEL, 10)
    s.probe(img_step, "① MIX 组签（未选中）", *C_TAB_MIX, C_PANEL_HI, 10)

    # ---------- ② STEP 鼓轨行：TAP 编辑 lane（不再只读） ----------
    s.cmd("UI LANE 0")
    s.expect(r"UI LANE lane=0", "② 选中 lane 归零")
    nav_set_focus(s, 0, 2)  # 合成轨焦点：焦点必须换到鼓轨才有判别力
    drum_xy = step_cell_xy(3, 3)  # 行 3 = DRM，窗口列 3 = 绝对步 3
    s.ser.note(f"用例②：TAP 鼓轨格 {drum_xy[0]},{drum_xy[1]}（DRM 行 lane 编辑）")
    mask0 = seq_dstep_mask(s, 3)
    bit0 = (mask0 >> 5) & 1  # 格中心 = 4×4 lane 网格的 (col 1,row 1) = lane 5
    st2 = nav_state_fields(s)
    eng2 = nav_eng_snapshot(s)
    img_before = s.capture("nav-02-drum-before")
    blue_before = count_region(img_before, NAV_STATUS_BOX, C_BLUE, 16)
    orange_before = count_region(img_before, NAV_STATUS_BOX, C_ORANGE, 16)
    (s.ok if blue_before >= 25 and orange_before == 0 else s.fail)(
        f"② 状态行基线：蓝字（合成轨 T1）{blue_before} / 橙字 {orange_before}"
        f"（期望蓝 ≥25、橙 0）")

    s.cmd("TAP %d %d" % drum_xy)
    s.expect(rf"-> STEP drum step=3 lane=5 bit={1 - bit0} CMD_DRUM_STEP_WRITE "
             rf"mask=0x{mask0 ^ 0x20:04X}", "② 鼓轨 TAP 写原始位图回显")
    mask1 = seq_dstep_mask(s, 3)
    (s.ok if mask1 == mask0 ^ 0x20 else s.fail)(
        f"② 鼓步 3 on 位图 0x{mask0:04X} -> 0x{mask1:04X}（只翻 lane 5 = 0x20）")
    st2_after = nav_state_fields(s)
    s.expect_int(r"focus_track=(\d+)", 3, "② 焦点轨落到鼓轨")
    s.expect_int(r"focus_step=(\d+)", 3, "② 焦点列 = 窗口列 3")
    nav_same(s, "② STATE SNAP（除焦点外）",
             {k: v for k, v in st2.items() if not k.startswith("focus")},
             {k: v for k, v in st2_after.items() if not k.startswith("focus")})
    eng2_after = nav_eng_snapshot(s)
    diff = [f"{a} -> {b}" for a, b in zip(eng2, eng2_after) if a != b]
    # ENG SNAP 的 dstep 是聚合投影（有音符/lvl/rat），单个 lane 位翻转不保证改变该行
    # （如起点 mask 0x0001 → 0x0021 时 on/lvl/rat 都不变）；写入真值由上面的 `SEQ DSTEP`
    # 原始位图断言，这里只要求「除鼓步 3 那一行外，其余 20 行逐字段不变」。
    only_dstep = all(d.startswith("ENG SNAP dstep i=3 ") for d in diff)
    (s.ok if only_dstep else s.fail)(
        f"② ENG SNAP 除鼓步 3 的 dstep 行外无变化（diff={diff}）")

    img_after = s.capture("nav-03-drum-lane")
    box = (drum_xy[0] - STEP_CELL_W // 2 - 2, drum_xy[1] - STEP_CELL_H // 2 - 2,
           drum_xy[0] + STEP_CELL_W // 2 + 2, drum_xy[1] + STEP_CELL_H // 2 + 2)
    cell_changed = img_before.crop(box).tobytes() != img_after.crop(box).tobytes()
    (s.ok if cell_changed else s.fail)(
        f"② 鼓格像素区 {box} 点击后变化（lane 位点亮）" if cell_changed
        else f"② 鼓格像素区 {box} 点击前后逐字节相同（lane 位未点亮）")
    orange_after = count_region(img_after, NAV_STATUS_BOX, C_ORANGE, 16)
    blue_after = count_region(img_after, NAV_STATUS_BOX, C_BLUE, 16)
    (s.ok if orange_after >= 25 and blue_after == 0 else s.fail)(
        f"② 状态行点击后：橙字（鼓轨）{orange_after} / 蓝字 {blue_after}"
        f"（期望 ≥25 / 0）")

    # 复位该 lane 位（脚本可重复）：同一 TAP 路径再点一次
    s.cmd("TAP %d %d" % drum_xy)
    s.expect(rf"-> STEP drum step=3 lane=5 bit={bit0} CMD_DRUM_STEP_WRITE "
             rf"mask=0x{mask0:04X}", "② 再 TAP 复位 lane 5 位")
    (s.ok if seq_dstep_mask(s, 3) == mask0 else s.fail)(
        f"② 复位后鼓步 3 位图 = 0x{mask0:04X}")

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
        s.expect(r"UI page=sound \(6\)", "④ 3 次 tab 只前进一页（TRACKS→SOUND，EDIT 组唯一页）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=sound \(6\)", "④ 再 6 拍无第二次前进")
        img = s.capture("nav-05-tab3-final")
        s.probe(img, "④ 组签选中（EDIT，单页组）", *C_TAB_EDIT, C_YELLOW)
        s.probe(img, "④ 子签槽 0 为空（EDIT 单页组的子行留空）", *C_TAB_SUB_EMPTY, C_PANEL, 10)
        s.probe(img, "④ 组签未选中（PLAY）", *C_TAB_PLAY, C_PANEL_HI, 10)
    else:
        s.fail(f"④ {NAV_ATTEMPTS} 次对齐都未能把 3 次 tab 压进同一拍（测试前提不成立）")

    # ---------- ⑤ 组作用域生命周期：离开组即销毁（ADR-005 §3/§5，DI-19） ----------
    # 先落到 MIX 组的 TOOLS（5B-1 起 MIX 的唯一页）。MIX→SEQ 后 MIX built 必须回 0（含原
    # 当前页），SEQ 首次进入只建 STEP；再从 STEP 切 TRANSPORT（同组）只改显隐、STEP 不重建。
    s.cmd("UI tools")
    s.expect(r"UI page=tools \(18\)", "⑤ 起点 MIX 组 TOOLS")
    mem_a = param_mem(s)
    ga = mem_a["groups"]
    (s.ok if ga["MIX"]["built"] == 1 and ga["SEQ"]["built"] == 0 else s.fail)(
        f"⑤ 进入前 MIX built={ga['MIX']['built']} / SEQ built={ga['SEQ']['built']}"
        f"（期望 1 / 0）")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑤ 跨组 MIX→SEQ")
    mem_b = param_mem(s)
    gb = mem_b["groups"]
    (s.ok if gb["MIX"]["built"] == 0 and gb["MIX"]["sum_delta"] == 0 else s.fail)(
        f"⑤ 离开 MIX 后 built={gb['MIX']['built']} sum_delta={gb['MIX']['sum_delta']}"
        f"（期望 0 / 0，源组页面含原当前页全部销毁）")
    (s.ok if mem_b["pages"].get(18, ("?",))[0] == "unbuilt" else s.fail)(
        f"⑤ 原当前页 TOOLS state={mem_b['pages'].get(18, ('?',))[0]}（期望 unbuilt）")
    (s.ok if gb["SEQ"]["built"] == 1 else s.fail)(
        f"⑤ SEQ built={gb['SEQ']['built']}（期望 1 = 只建 STEP）")
    (s.ok if 0 < gb["SEQ"]["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑤ 跨组进入 SEQ build_ms={gb['SEQ']['build_ms']}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")
    img = s.capture("nav-06-groupscope")
    s.probe(img, "⑤ 组签选中（SEQ）", *C_TAB_SEQ, C_YELLOW)
    s.probe(img, "⑤ 组签未选中（MIX）", *C_TAB_MIX, C_PANEL_HI, 10)
    s.probe(img, "⑤ 子签选中（STEP）", *C_TAB_STEP, C_YELLOW)
    s.probe(img, "⑤ 子签未选中（TRANSPORT）", *C_TAB_TRANSPORT, C_PANEL_HI, 10)

    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑤ 同组 STEP→TRANSPORT")
    mem_c = param_mem(s)
    gc = mem_c["groups"]
    (s.ok if gc["SEQ"]["built"] == 2 and mem_c["pages"].get(2, ("?",))[0] == "built" else s.fail)(
        f"⑤ 同组切换后 SEQ built={gc['SEQ']['built']} STEP={mem_c['pages'].get(2, ('?',))[0]}"
        f"（期望 2 / built：同组切换只改显隐、不重建）")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑤ 回 PLAY（SEQ 组销毁）")
    mem_d = param_mem(s)
    (s.ok if mem_d["groups"]["SEQ"]["built"] == 0 and mem_d["head"]["built"] == 1 else s.fail)(
        f"⑤ 离开 SEQ 后 SEQ built={mem_d['groups']['SEQ']['built']} 总 built="
        f"{mem_d['head']['built']}（期望 0 / 1 = 仅 PLAY）")

    # ---------- ⑥ 手指路径（R32）：组签/子签点击与 R30 同拍取消 ----------
    # TAP 对 y<72 一律 miss，页签只能经 LVGL 回调；`UI TAB`/`UI SUB` 与回调共用同一
    # 动作函数，故这三段覆盖：①点组进「上次子页」且记忆跨往返；②跨组点组销毁源组
    # 并建目标首个子页；③同拍 `KEY tab` 排队时 `UI TAB` 胜出且待切页被丢弃（R30）。
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑥ 起点 PLAY")
    # ⑥a EDIT 单页组（5B-1：族页退役后只剩 SOUND）：点组落 SOUND、只建这一页、Σ est =
    # SOUND 的 est；单页组的子行为空 ⇒ `UI SUB` 任意下标都是组内越界（不是用法错误），
    # 注入不改页。停在 SOUND 作为 ⑥b 的源组。
    s.cmd("UI tab edit")
    s.expect(r"UI TAB EDIT page=sound \(6\)", "⑥ UI TAB edit（→ SOUND，EDIT 唯一页）")
    mem_e = param_mem(s)
    (s.ok if mem_e["groups"]["EDIT"]["built"] == 1
     and mem_e["groups"]["EDIT"]["sum_est"] == PARAM_EDIT_SUM_EST
     and mem_e["pages"].get(6, ("?",))[0] == "built" else s.fail)(
        f"⑥ 点组只建 SOUND：EDIT built={mem_e['groups']['EDIT']['built']} "
        f"sum_est={mem_e['groups']['EDIT']['sum_est']} "
        f"SOUND={mem_e['pages'].get(6, ('?',))[0]}（期望 1 / {PARAM_EDIT_SUM_EST:,} / built）")
    (s.ok if 0 < mem_e["groups"]["EDIT"]["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑥ 点组点击 SOUND build_ms={mem_e['groups']['EDIT']['build_ms']}ms"
        f"（0 < x ≤{NAV_BUILD_BUDGET_MS}）")
    s.cmd("UI SUB 0")
    s.expect(r"UI ERR sub 0", "⑥ EDIT 单页组子行为空：UI SUB 0 → UI ERR sub")
    s.cmd("UI SUB 4")
    s.expect(r"UI ERR sub 4", "⑥ EDIT 单页组：UI SUB 4 越界")
    s.cmd("UI SUB 5")
    s.expect(r"用法: UI SUB <index=0\.\.4>",
             "⑥ UI SUB 5 超出下行槽位（0..4）→ 用法错误（不是组内越界的 UI ERR sub）")
    s.cmd("UI")
    s.expect(r"UI page=sound \(6\)", "⑥ 越界注入不改页（仍 SOUND）")
    # ⑥b 跨组点组：销毁源组 + 建目标组首个子页（TRACKS 单页组，首个子页恒为 TRACKS）
    s.cmd("UI tab tracks")
    s.expect(r"UI TAB TRACKS page=tracks \(1\)", "⑥ 跨组 UI TAB tracks（目标首个子页）")
    mem_g = param_mem(s)
    (s.ok if mem_g["groups"]["EDIT"]["built"] == 0
     and mem_g["pages"].get(6, ("?",))[0] == "unbuilt"
     and mem_g["groups"]["TRACKS"]["built"] == 1 else s.fail)(
        f"⑥ 跨组点击后 EDIT built={mem_g['groups']['EDIT']['built']} "
        f"SOUND={mem_g['pages'].get(6, ('?',))[0]} / "
        f"TRACKS built={mem_g['groups']['TRACKS']['built']}（期望 0 / unbuilt / 1）")
    (s.ok if 0 < mem_g["groups"]["TRACKS"]["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑥ 跨组点击 TRACKS build_ms={mem_g['groups']['TRACKS']['build_ms']}ms"
        f"（0 < x ≤{NAV_BUILD_BUDGET_MS}）")
    # ⑥c EDIT/MIX 单页组（5B-1）：回到 EDIT 仍落 SOUND；MIX 点组落 TOOLS、Σ est = TOOLS 的
    # est、`UI SUB` 越界、子行为空（像素：子签槽是页签条底色）；离开组两页各自销毁。
    s.cmd("UI tab edit")
    s.expect(r"UI TAB EDIT page=sound \(6\)", "⑥ 回 EDIT（仍落 SOUND）")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY")
    s.cmd("UI TAB MIX")
    s.expect(r"UI TAB MIX page=tools \(18\)", "⑥ UI TAB MIX（→ TOOLS，MIX 唯一页）")
    mem_mix = param_mem(s)
    (s.ok if mem_mix["groups"]["MIX"]["built"] == 1
     and mem_mix["groups"]["MIX"]["sum_est"] == PARAM_MIX_SUM_EST else s.fail)(
        f"⑥ MIX built={mem_mix['groups']['MIX']['built']} "
        f"sum_est={mem_mix['groups']['MIX']['sum_est']}（期望 1 / {PARAM_MIX_SUM_EST:,}）")
    s.cmd("UI SUB 0")
    s.expect(r"UI ERR sub 0", "⑥ MIX 单页组子行为空：UI SUB 0 → UI ERR sub")
    s.cmd("UI SUB 1")
    s.expect(r"UI ERR sub 1", "⑥ MIX 单页组：UI SUB 1 越界")
    s.cmd("UI")
    s.expect(r"UI page=tools \(18\)", "⑥ 越界注入不改页（仍 TOOLS）")
    img_mix = s.capture("nav-06b-mix")
    s.probe(img_mix, "⑥ MIX 组签选中", *C_TAB_MIX, C_YELLOW)
    s.probe(img_mix, "⑥ MIX 子签槽 0 为空（单页组）", *C_TAB_SUB_EMPTY, C_PANEL, 10)
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY（MIX 的 TOOLS 销毁）")
    mem_mix = param_mem(s)
    (s.ok if mem_mix["groups"]["MIX"]["built"] == 0
     and mem_mix["pages"].get(18, ("?",))[0] == "unbuilt" else s.fail)(
        f"⑥ 离开 MIX 后 built={mem_mix['groups']['MIX']['built']} "
        f"18={mem_mix['pages'].get(18, ('?',))[0]}（期望 0 / unbuilt）")
    s.cmd("UI SUB 0")
    s.expect(r"UI ERR sub 0", "⑥ 单页组子行为空：UI SUB 0 → UI ERR sub")
    s.cmd("UI")
    s.expect(r"UI page=play \(0\)", "⑥ 单页组 SUB 注入不改页")
    # FILE 组（P4-B Task 12 注册两页）：点组进上次子页、组内子签切换、离开组销毁两页。
    # 先把「上次子页」归一成 PROJECTS（先直达再离开），⑥ 在重复运行/⑦ 之后也确定。
    s.cmd("UI projects")
    s.expect(r"UI page=projects \(9\)", "⑥ UI projects（FILE 组页面已注册）")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 离开 FILE 组（源组销毁）")
    mem_file = param_mem(s)
    (s.ok if mem_file["groups"]["FILE"]["built"] == 0
     and mem_file["pages"].get(9, ("?",))[0] == "unbuilt"
     and mem_file["pages"].get(10, ("?",))[0] == "unbuilt" else s.fail)(
        f"⑥ 离开 FILE 后 built={mem_file['groups']['FILE']['built']} "
        f"PROJECTS={mem_file['pages'].get(9, ('?',))[0]} "
        f"PRESETS={mem_file['pages'].get(10, ('?',))[0]}（期望 0 / unbuilt / unbuilt）")
    s.cmd("UI TAB FILE")
    s.expect(r"UI TAB FILE page=projects \(9\)", "⑥ UI TAB FILE → 上次子页 PROJECTS")
    mem_file = param_mem(s)
    (s.ok if mem_file["groups"]["FILE"]["built"] == 1 else s.fail)(
        f"⑥ 点组只建目标页：FILE built={mem_file['groups']['FILE']['built']}（期望 1）")
    (s.ok if mem_file["groups"]["FILE"]["sum_est"] > 0 else s.fail)(
        f"⑥ FILE sum_est={mem_file['groups']['FILE']['sum_est']}（两页 est 之和 > 0）")
    s.cmd("UI SUB 1")
    s.expect(r"UI SUB 1 page=presets \(10\)", "⑥ UI SUB 1 切 PRESETS（同组不重建 PROJECTS）")
    mem_file = param_mem(s)
    (s.ok if mem_file["groups"]["FILE"]["built"] == 2
     and mem_file["pages"].get(9, ("?",))[0] == "built" else s.fail)(
        f"⑥ FILE built={mem_file['groups']['FILE']['built']} "
        f"PROJECTS={mem_file['pages'].get(9, ('?',))[0]}（期望 2 / built）")
    s.cmd("UI SUB 2")
    s.expect(r"UI ERR sub 2", "⑥ FILE 组只有 2 张子签：UI SUB 2 越界")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY（FILE 两页销毁）")
    # SONG 独立组（P4-C Task 11）：单页组——点组进 SONG、离开后 built=0、子行为空。
    s.cmd("UI TAB song")
    s.expect(r"UI TAB SONG page=song \(11\)", "⑥ UI TAB song（SONG 独立组）")
    mem_song = param_mem(s)
    (s.ok if mem_song["groups"]["SONG"]["built"] == 1
     and mem_song["groups"]["SONG"]["sum_est"] == 12288
     and mem_song["pages"].get(11, ("?",))[0] == "built" else s.fail)(
        f"⑥ SONG built={mem_song['groups']['SONG']['built']} "
        f"sum_est={mem_song['groups']['SONG']['sum_est']} "
        f"page={mem_song['pages'].get(11, ('?',))[0]}（期望 1 / 12288 / built）")
    (s.ok if 0 < mem_song["groups"]["SONG"]["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑥ SONG build_ms={mem_song['groups']['SONG']['build_ms']}ms"
        f"（0 < x ≤{NAV_BUILD_BUDGET_MS}）")
    s.cmd("UI SUB 0")
    s.expect(r"UI ERR sub 0", "⑥ SONG 单页组：UI SUB 0 → UI ERR sub")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY（SONG 页销毁）")
    mem_song = param_mem(s)
    (s.ok if mem_song["groups"]["SONG"]["built"] == 0
     and mem_song["pages"].get(11, ("?",))[0] == "unbuilt" else s.fail)(
        f"⑥ 离开 SONG 后 built={mem_song['groups']['SONG']['built']} "
        f"page={mem_song['pages'].get(11, ('?',))[0]}（期望 0 / unbuilt）")
    # SYNTH 组三页（P4-D Task 8：SAMPLE 从 EDIT 移入，DSYN/FM6P 同组）：点组进上次
    # 子页、子签 0..2 依次到 SAMPLE/DSYN/FM6P、`UI SUB 3` 越界、离开组 built=0。
    # 「上次子页」先归一成 SAMPLE（先直达再离开；同 FILE 块）——上一轮可能停在 FM6P。
    s.cmd("UI sample")
    s.expect(r"UI page=sample \(8\)", "⑥ 归一 SYNTH 上次子页 = SAMPLE")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 离开 SYNTH（归一）")
    s.cmd("UI TAB synth")
    s.expect(r"UI TAB SYNTH page=sample \(8\)", "⑥ UI TAB synth（SYNTH 组首个/上次子页）")
    mem_synth = param_mem(s)
    (s.ok if mem_synth["groups"]["SYNTH"]["built"] == 1
     and mem_synth["groups"]["SYNTH"]["sum_est"] == 20480
     and mem_synth["pages"].get(8, ("?",))[0] == "built" else s.fail)(
        f"⑥ SYNTH built={mem_synth['groups']['SYNTH']['built']} "
        f"sum_est={mem_synth['groups']['SYNTH']['sum_est']} "
        f"SAMPLE={mem_synth['pages'].get(8, ('?',))[0]}（期望 1 / 20480 / built）")
    (s.ok if 0 < mem_synth["groups"]["SYNTH"]["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑥ SYNTH build_ms={mem_synth['groups']['SYNTH']['build_ms']}ms"
        f"（0 < x ≤{NAV_BUILD_BUDGET_MS}）")
    s.cmd("UI SUB 1")
    s.expect(r"UI SUB 1 page=dsyn \(12\)", "⑥ UI SUB 1 切 DSYN（同组不重建 SAMPLE）")
    mem_synth = param_mem(s)
    (s.ok if mem_synth["groups"]["SYNTH"]["built"] == 2
     and mem_synth["pages"].get(8, ("?",))[0] == "built" else s.fail)(
        f"⑥ SYNTH built={mem_synth['groups']['SYNTH']['built']} "
        f"SAMPLE={mem_synth['pages'].get(8, ('?',))[0]}（期望 2 / built）")
    s.cmd("UI SUB 2")
    s.expect(r"UI SUB 2 page=fm6p \(13\)", "⑥ UI SUB 2 切 FM6P")
    mem_synth = param_mem(s)
    (s.ok if mem_synth["groups"]["SYNTH"]["built"] == 3 else s.fail)(
        f"⑥ SYNTH 三页已建：built={mem_synth['groups']['SYNTH']['built']}（期望 3）")
    s.cmd("UI SUB 3")
    s.expect(r"UI ERR sub 3", "⑥ SYNTH 组只有 3 张子签：UI SUB 3 越界")
    s.cmd("UI")
    s.expect(r"UI page=fm6p \(13\)", "⑥ 越界注入不改页（仍 FM6P）")
    s.cmd("UI tab synth")
    s.expect(r"UI TAB SYNTH page=fm6p \(13\)", "⑥ 点组回到上次子页（FM6P，记忆跨往返）")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY（SYNTH 三页销毁）")
    mem_synth = param_mem(s)
    (s.ok if mem_synth["groups"]["SYNTH"]["built"] == 0
     and all(mem_synth["pages"].get(p, ("?",))[0] == "unbuilt" for p in (8, 12, 13))
     else s.fail)(
        f"⑥ 离开 SYNTH 后 built={mem_synth['groups']['SYNTH']['built']} "
        + " ".join(f"{p}={mem_synth['pages'].get(p, ('?',))[0]}" for p in (8, 12, 13))
        + "（期望 0 / unbuilt ×3）")
    # SEQ 组两页（STEP/TRANSPORT）：子签 0..1、`UI SUB 2` 越界、同组不重建、离开组两页销毁
    s.cmd("UI TAB seq")
    s.expect(r"UI TAB SEQ page=\w+ \([23]\)", "⑥ UI TAB seq（→ 上次子页）")
    s.cmd("UI SUB 0")
    s.expect(r"UI SUB 0 page=step \(2\)", "⑥ UI SUB 0 切 STEP")
    s.cmd("UI SUB 1")
    s.expect(r"UI SUB 1 page=transport \(3\)", "⑥ UI SUB 1 切 TRANSPORT")
    mem_seq = param_mem(s)
    (s.ok if mem_seq["groups"]["SEQ"]["built"] == 2
     and mem_seq["groups"]["SEQ"]["sum_est"] == 32256 else s.fail)(
        f"⑥ SEQ built={mem_seq['groups']['SEQ']['built']} "
        f"sum_est={mem_seq['groups']['SEQ']['sum_est']}（期望 2 / 32,256）")
    s.cmd("UI SUB 2")
    s.expect(r"UI ERR sub 2", "⑥ SEQ 组只有 2 张子签：UI SUB 2 越界")
    s.cmd("UI")
    s.expect(r"UI page=transport \(3\)", "⑥ 越界注入不改页（仍 TRANSPORT）")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY（SEQ 两页销毁）")
    mem_seq = param_mem(s)
    (s.ok if mem_seq["groups"]["SEQ"]["built"] == 0
     and all(mem_seq["pages"].get(p, ("?",))[0] == "unbuilt" for p in (2, 3)) else s.fail)(
        f"⑥ 离开 SEQ 后 built={mem_seq['groups']['SEQ']['built']} "
        + " ".join(f"{p}={mem_seq['pages'].get(p, ('?',))[0]}" for p in (2, 3))
        + "（期望 0 / unbuilt ×2）")
    # SYS 单页组（P4-E Task 6）：点组进 SYSTEM、子行为空、离开后 built=0
    s.cmd("UI TAB sys")
    s.expect(r"UI TAB SYS page=system \(14\)", "⑥ UI TAB sys（SYS 独立组）")
    s.cmd("UI SUB 0")
    s.expect(r"UI ERR sub 0", "⑥ SYS 单页组：UI SUB 0 → UI ERR sub")
    s.cmd("UI tab play")
    s.expect(r"UI TAB PLAY page=play \(0\)", "⑥ 回 PLAY（SYSTEM 页销毁）")
    mem_sys = param_mem(s)
    (s.ok if mem_sys["groups"]["SYS"]["built"] == 0
     and mem_sys["pages"].get(14, ("?",))[0] == "unbuilt" else s.fail)(
        f"⑥ 离开 SYS 后 built={mem_sys['groups']['SYS']['built']} "
        f"page={mem_sys['pages'].get(14, ('?',))[0]}（期望 0 / unbuilt）")
    s.cmd("UI TAB nope")
    s.expect(r"UI ERR group nope", "⑥ 未知组名 → UI ERR group")
    # ⑥d R30：同一 30ms 拍内 KEY tab 排队 + UI TAB 点击 ⇒ 点击胜出、待切页丢弃。
    # 锚点 TRACKS 的单按 tab 目标 = SOUND（与 UI TAB play 的目标不同），故若未取消，
    # 下一拍会跳到 SOUND（1s 后读页可判别）。批内 UI 自证「点击执行时请求仍在邮箱」。
    premise = False
    for attempt in range(1, NAV_ATTEMPTS + 1):
        s.cmd_fast("UI tracks")
        s.expect(r"UI page=tracks \(1\)", f"⑥d 起点 TRACKS（对齐 {attempt}/{NAV_ATTEMPTS}）")
        span = s.cmd_batch(["KEY 0x20 down", "KEY 0x20 up", "UI", "UI TAB play", "UI"])
        pages = re.findall(r"UI page=\w+ \((\d+)\)", s.last)
        if len(pages) < 2:
            raise InjectError(f"⑥d 批内缺少 UI 页回显：{s.last!r}")
        mid, after = int(pages[0]), int(pages[-1])
        s.ser.note(f"⑥d 对齐 {attempt}: 批次 {span:.1f}ms，批内 UI：点击前={mid} 点击后={after}")
        if mid != 1:
            s.ser.note("⑥d 前提不成立：批内 UI 已前进 → 拍插在 tab 与点击之间；重新对齐")
            print(f"  [NOTE] ⑥d 对齐 {attempt}: 拍插在 tab 与 UI TAB 之间（批内点击前={mid}），重新对齐")
            continue
        premise = True
        break
    if premise:
        (s.ok if after == 0 else s.fail)(
            f"⑥d 批内点击后页={after}（期望 0 = PLAY；UI TAB 立即生效）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=play \(0\)", "⑥d 同拍点击胜出：1s 后仍 PLAY（待切页已丢弃，R30）")
        time.sleep(NAV_SETTLE_S)
        s.cmd_fast("UI")
        s.expect(r"UI page=play \(0\)", "⑥d 再 6 拍仍 PLAY（无 SOUND 回跳）")
    else:
        s.fail(f"⑥d {NAV_ATTEMPTS} 次对齐都未能把 tab 与 UI TAB 压进同一拍（测试前提不成立）")

    # ---------- ⑦ TAB 全 13 页轮转（5B-1：EDIT/MIX 各一页） ----------
    # 一拍一页从 PLAY 走完 13 页并回卷到 PLAY：每个目标页都由**当前页**计算
    # （sloop_ui_page_order_next），任何「注册了但不在组表」或「组表与子签顺序不一致」
    # 都会让某一步偏离。异步切页用 STATE SNAP 轮询等落地（UI 会取消待执行请求）。
    s.ser.note("⑦ TAB 扁平整轮：PLAY→TRACKS→SOUND→TOOLS→SAMPLE→DSYN→FM6P→STEP→TRANSPORT→"
               "SONG→PROJECTS→PRESETS→SYSTEM→PLAY")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑦ 起点 PLAY")
    for pid, name in NAV_ORDER[1:] + NAV_ORDER[:1]:
        s.cmd("KEY 0x20 down")
        s.cmd("KEY 0x20 up")
        nav_wait_page(s, name, pid, f"⑦ TAB → {name} ({pid})")
    img = s.capture("nav-07-tab-cycle")
    s.probe(img, "⑦ 回卷到 PLAY（组签选中）", *C_TAB_PLAY, C_YELLOW)

    # ---------- ⑧ 跨组往返不泄漏（R34 / ADR-008 §3）：PLAY↔TRACKS ×10 ----------
    # 组作用域生命周期会反复销毁/重建页面；`styles_init()` 若每次重跑 `lv_style_init`
    # 会丢掉静态样式已分配的 props 数组（实测修复前每往返 ~300B）。本用例在修复后要求
    # 首末 `pool_free` 差 ≤ 噪声、且逐次往返无单调下降。
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑧ 起点 PLAY")
    # 预热一次：PLAY/TRACKS 的全部状态串首次显示会触发一次性文本/字形缓存（~100B 量级，
    # 不是逐次泄漏；R34 的回归是每往返 ~268B）。之后 10 个测量轮从同一稳态出发，
    # 判据仍是首末差 ≤128B 与单次下降 ≤128B。
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "⑧ 预热：UI tracks")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑧ 预热：UI play")
    free0 = nav_pool_free(s)
    per_trip = []
    for trip in range(1, NAV_LEAK_TRIPS + 1):
        s.cmd("UI tracks")
        s.expect(r"UI page=tracks \(1\)", f"⑧ 往返 {trip}/{NAV_LEAK_TRIPS}：UI tracks")
        s.cmd("UI play")
        s.expect(r"UI page=play \(0\)", f"⑧ 往返 {trip}/{NAV_LEAK_TRIPS}：UI play")
        per_trip.append(nav_pool_free(s))
    free1 = per_trip[-1]
    drop = free0 - free1
    s.ser.note(f"⑧ pool_free：起点 {free0} → 逐次 {per_trip}（{NAV_LEAK_TRIPS} 次往返）")
    (s.ok if abs(drop) <= NAV_LEAK_NOISE else s.fail)(
        f"⑧ {NAV_LEAK_TRIPS} 次 PLAY↔TRACKS 往返后 pool_free 变化 {free0} → {free1}"
        f"（Δ={-drop:+d}B，允许 |Δ| ≤ {NAV_LEAK_NOISE}B：styles_init 只跑一次）")
    # 「无逐次增长」：任一单次往返的池下降都不超过噪声（否则是逐次泄漏而非一次性漂移）
    steps = [free0] + per_trip
    worst = max(steps[i] - steps[i + 1] for i in range(len(steps) - 1))
    (s.ok if worst <= NAV_LEAK_NOISE else s.fail)(
        f"⑧ 单次往返的最大池下降 {worst}B ≤ {NAV_LEAK_NOISE}B（无逐次增长）")

    # ---------- ⑨ 九组签逐组像素（104px）+ 组作用域通用断言 ----------
    # 对每个组 `UI TAB <g>`：该组签是唯一选中（强调色，左/右内缘都是 ⇒ 签宽 104）、其余 8 张
    # 未选中、签间缝是页签条底色；下行子签数 = 组成员数（第 n 槽起是页签条底色）、选中
    # 子签 = 当前页在组内的下标；`UI MEM` 只有本组已建（built = 成员数）。
    s.ser.note("⑨ 九组逐组：UI TAB <g> → 组签选中色 / 其余未选中 / 子签数 / 只有本组已建")
    for gi, (gname, members) in enumerate(NAV_GROUP_PAGES.items()):
        s.cmd(f"UI TAB {gname.lower()}")
        m = re.search(rf"UI TAB {gname} page=(\w+) \((\d+)\)", s.last)
        if m is None:
            raise InjectError(f"⑨ UI TAB {gname} 回显异常：{s.last!r}")
        cur_name = m.group(1)
        (s.ok if cur_name in members else s.fail)(
            f"⑨ UI TAB {gname} → {cur_name}（{m.group(2)}）是本组成员")
        mem = param_mem(s)
        built_groups = [g for g in UI_GROUP_NAMES if mem["groups"][g]["built"] != 0]
        # 点组只建落地的那一页（其余成员按需懒建）：总 built = 1，且只有它在组内是 built
        (s.ok if built_groups == [gname] and mem["head"]["built"] == 1
         and mem["head"]["group"] == gname else s.fail)(
            f"⑨ {gname} 为当前组：已建组 {built_groups}、总 built={mem['head']['built']}"
            f"（期望仅 {gname} / 1 = 点组只建落地页）")
        member_ids = [pid for pid, name in NAV_ORDER if name in members]
        (s.ok if len(member_ids) == len(members)
         and all(mem["pages"].get(pid, ("?",))[0] == ("built" if name == cur_name else "unbuilt")
                 for pid, name in NAV_ORDER if name in members) else s.fail)(
            f"⑨ {gname} 的成员页状态：{cur_name} built、其余 unbuilt")
        time.sleep(0.15)  # ≥5 拍：页签条刷新稳定
        img = s.capture(f"nav-09-tab-{gname.lower()}")
        for j, other in enumerate(UI_GROUP_NAMES):
            s.probe(img, f"⑨ {gname}：组签 {other}{'（选中）' if j == gi else '（未选中）'}",
                    *group_tab_probe(j), C_YELLOW if j == gi else C_PANEL_HI, 10)
        gx = UI_TAB_X0 + gi * (UI_GROUP_W + UI_TAB_GAP)
        s.probe(img, f"⑨ {gname} 组签右内缘（签宽 104px）", gx + UI_GROUP_W - 8, UI_GROUP_Y + 4,
                C_YELLOW)
        s.probe(img, f"⑨ {gname} 组签右缘之外（签缝/行尾）", gx + UI_GROUP_W + 4, UI_GROUP_Y + 12,
                C_PANEL, 10)
        # 单页组的选中态在上行组签（下行无子签）；多页组的选中子签 = 落地页在组内的下标
        n_subs = len(members) if len(members) > 1 else 0
        sel_idx = members.index(cur_name) if n_subs and cur_name in members else -1
        for k in range(UI_GROUP_SLOTS):
            want = (C_YELLOW if k == sel_idx else C_PANEL_HI) if k < n_subs else C_PANEL
            s.probe(img, f"⑨ {gname}：子签槽 {k}"
                    + ("（选中）" if k == sel_idx else "（未选中）" if k < n_subs else "（空）"),
                    *sub_tab_probe(k), want, 10)

    # ---------- ⑩ 有序对全矩阵（ADR-005 §5）：源组全建 → 跨组首建目标页 ----------
    # 过渡峰值 = 目标页构建完成那一刻的池余量（源组页面尚未销毁）。构建日志的「池剩余」就是
    # 这个点。9 组 × 8 个目标组、逐目标页（源组每次重新全建）：任何一页低于 4,096 都 FAIL。
    s.ser.note("⑩ 有序对全矩阵：源组全建 → 跨组首建目标页，构建日志 池剩余 ≥ "
               f"{NAV_POOL_FLOOR:,}（{len(UI_GROUP_NAMES)}×{len(UI_GROUP_NAMES) - 1} 组对、逐目标页）")
    pair_min = {}
    for g_src, src_pages in NAV_GROUP_PAGES.items():
        for g_dst, dst_pages in NAV_GROUP_PAGES.items():
            if g_dst == g_src:
                continue
            frees = []
            for d in dst_pages:
                for p in src_pages:  # 上一次跨组已销毁源组：每个目标页之前重新全建
                    s.cmd(f"UI {p}")
                s.cmd(f"UI {d}")
                m = re.search(r"页面构建 page=(\w+)\((\d+)\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B",
                              s.last)
                if m is None or m.group(1).lower() != d:
                    s.fail(f"⑩ {g_src} 全建 → {d}：构建日志缺失或页不符（池剩余不可测）")
                    continue
                free = int(m.group(5))
                frees.append(free)
                (s.ok if free >= NAV_POOL_FLOOR else s.fail)(
                    f"⑩ {g_src} 全建 → {d}（{g_dst}）：池剩余={free}B"
                    f"（≥{NAV_POOL_FLOOR:,}）")
            pair_min[(g_src, g_dst)] = min(frees) if frees else -1
            worst = min((f, d) for f, d in zip(frees, dst_pages)) if frees else (-1, "?")
            (s.ok if worst[0] >= NAV_POOL_FLOOR else s.fail)(
                f"有序对 {g_src}→{g_dst}：{len(frees)}/{len(dst_pages)} 个目标页，最小池剩余 "
                f"{worst[0]}B（{worst[1]}）")
    good = sum(1 for v in pair_min.values() if v >= NAV_POOL_FLOOR)
    worst_pair = min(pair_min, key=pair_min.get) if pair_min else None
    s.ser.note(f"⑩ 组对 {good}/{len(pair_min)} 个全部目标页 ≥{NAV_POOL_FLOOR:,}；最紧 "
               f"{worst_pair[0]}→{worst_pair[1]}={pair_min[worst_pair]}B；涉及 EDIT/MIX 的组合 "
               + " ".join(f"{a}→{b}={pair_min[(a, b)]}B" for a, b in (
                   ("EDIT", "SEQ"), ("MIX", "SEQ"), ("SEQ", "MIX"), ("MIX", "FILE"))))
    # LVGL 任务栈余量（R10）：高水位覆盖到此为止的 30ms 拍、切页构建与渲染
    lv = param_mem(s)["head"]["lvgl_stack_free"]
    s.ser.note(f"UI MEM lvgl_stack_free={lv}B（nav 全场景收尾；门禁 ≥{LVGL_STACK_FLOOR}）")
    (s.ok if lv >= LVGL_STACK_FLOOR else s.fail)(
        f"LVGL 任务栈最小余量（nav 全场景）lvgl_stack_free={lv}B（≥{LVGL_STACK_FLOOR}）")

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

# 组 Σ est（ADR-008 §3 / ADR-005 §5.1；组表见 NAV_GROUP_PAGES）：EDIT = SOUND 8,704（5B-1
# 卡片仪表盘 + 页眉两行 + 常驻隐藏的 bank 行）；MIX = TOOLS 3,584（P4-F Task 6：8 个单对象
# label，实测 2,568–2,732）；SYNTH = SAMPLE 6,144 + DSYN 7,168 + FM6P 7,168 = **20,480**。
# 页面 est 改了这里必须同步（params 场景的组预算断言）。
PARAM_EDIT_SUM_EST = 8704
PARAM_MIX_SUM_EST = 3584
TOOLS_EST = 3584
PARAM_SYNTH_SUM_EST = 6144 + 7168 + 7168
# 5B-1 起两组都是单页：EDIT 只有 SOUND，MIX 只有 TOOLS（原 GLOBAL/FX/MOD/SCALE/VOICE 退役）
PARAM_EDIT_PAGES = ("sound",)
PARAM_MIX_PAGES = ("tools",)


def param_knob_xy(slot: int) -> tuple:
    """参数页槽位 slot（0..7，行优先）旋钮盒中心的屏幕绝对坐标。"""
    r, c = divmod(slot, 4)
    x = PARAM_GRID_X0 + c * PARAM_CELL_PITCH + PARAM_OBJ_DX + PARAM_OBJ_SIZE // 2
    y = PARAM_GRID_Y0 + r * PARAM_ROW_PITCH + PARAM_OBJ_DY + PARAM_OBJ_SIZE // 2
    return (x, y)


# bank 存取行（P4-B Task 13 原属 FM6 页；P4-F Task 5 起在 SOUND 页、仅 FM6 引擎时显示）：
# x/宽与 page_params.c 的 BANK_* 宏同名同值。5B-1 仪表盘页把它从页面区底部挪到页眉下方
# （页面区相对 y = 76 = page_layout.h 的 SOUND_BANK_ROW_Y），行高 44；屏幕绝对 y = 72 + 76；
# 行隐藏时这一带被卡片视口盖住（行缺席的判据见 bank_row_absent_probes）。
BANK_ROW_Y = 72 + 76
BANK_ROW_H = 44
BANK_LABEL_X, BANK_LABEL_W = 16, 88
BANK_DEC_X = 112
BANK_VAL_X, BANK_VAL_W = 184, 96
BANK_INC_X = 288
BANK_SLOT_W = 64
BANK_SAVE_X = 368
BANK_ERASE_X = 484
BANK_ACT_W = 100

# SOUND 页眉（P4-F Task 5；page_layout.h 的 SOUND_ROW_* 同名同值）：row 0 = ENGINE（鼓轨 =
# KIT）x=16 w=288、row 1 = PRESET x=320 w=384；页面区相对 y=4 h=32 ⇒ 屏幕 y=76..108；左右各
# 42px 是 ∓1 区、中段是名字区（点开引擎/预置/kit 选择器，归 sound 场景）。行对象不建时（鼓轨的
# PRESET 行）其矩形 TAP miss。
SOUND_ROW_Y = 72 + 4
SOUND_ROW_H = 32
SOUND_ROW_ZONE = 42
SOUND_ROWS = ((16, 288), (320, 384))
SOUND_EST = 8704            # 卡片仪表盘 + 页眉两行 + 常驻隐藏的 bank 行（= PARAM_EDIT_SUM_EST）


def sound_row_xy(row: int, delta: int) -> tuple:
    """SOUND 页眉行的 − / + 区中心（delta<0 = 减；row 0 = ENGINE/KIT、row 1 = PRESET）。"""
    x, w = SOUND_ROWS[row]
    return (x + SOUND_ROW_ZONE // 2 if delta < 0 else x + w - SOUND_ROW_ZONE // 2,
            SOUND_ROW_Y + SOUND_ROW_H // 2)


# SAMPLE 槽管理行（P4-D Task 8）：与 BANK 行同一底部带（页面区相对 y=552、44 高 ⇒
# 屏幕 y=624..668）；槽卡 x0=16、宽 200、间距 208，ERASE 在 x=856（宽 BANK_ACT_W）。
# `SAMPLE_ROW_CY` 是行中心（TAP 用）；`SAMPLE_ROW_Y` 只是顶边（区域探针用）。
SAMPLE_ROW_Y = 72 + 552
SAMPLE_SLOT_X0, SAMPLE_SLOT_W, SAMPLE_SLOT_PITCH = 16, 200, 208
SAMPLE_ERASE_X, SAMPLE_STATUS_X, SAMPLE_STATUS_W = 856, 972, 292
SAMPLE_ROW_H, SAMPLE_ROW_CY = 44, SAMPLE_ROW_Y + 22
SAMPLE_SLOT_BOX = (SAMPLE_SLOT_X0, SAMPLE_ROW_Y, SAMPLE_SLOT_X0 + SAMPLE_SLOT_W,
                   SAMPLE_ROW_Y + SAMPLE_ROW_H)
SAMPLE_STATUS_BOX = (SAMPLE_STATUS_X, SAMPLE_ROW_Y, SAMPLE_STATUS_X + SAMPLE_STATUS_W,
                     SAMPLE_ROW_Y + SAMPLE_ROW_H)


def sample_slot_xy(k: int) -> tuple:
    """槽管理行 U1..U4 卡中心（k=0..3）。"""
    return (SAMPLE_SLOT_X0 + k * SAMPLE_SLOT_PITCH + SAMPLE_SLOT_W // 2, SAMPLE_ROW_CY)


def sample_erase_xy() -> tuple:
    return (SAMPLE_ERASE_X + BANK_ACT_W // 2, SAMPLE_ROW_CY)

# DSYN / FM6P 页眉与底部几何：与 page_dsyn.c / page_fm6p.c 的 **同名宏**一一对应
# （页内坐标 + 页签条 72）。TAP 命中点与像素探针一律由这些宏派生，不写死数字；
# 页面布局改动时先改页面宏、再同步这里（与 step_cell_xy / param_knob_xy 同纪律）。
DSYN_HEAD_Y, DSYN_HEAD_H = 72 + 4, 32          # DSYN_HEAD_Y / DSYN_CHIP_H
DSYN_KIT_X0, DSYN_KIT_W, DSYN_KIT_PITCH = 16, 64, 70
DSYN_LANE_X, DSYN_LANE_W, DSYN_LANE_ZONE = 298, 180, 42
DSYN_PAGE_X0, DSYN_PAGE_W, DSYN_PAGE_PITCH = 526, 68, 74
DSYN_AUD_X, DSYN_AUD_W = 828, 116
DSYN_STORE_X, DSYN_STORE_W = 952, 88
DSYN_STATUS_X, DSYN_STATUS_W = 1048, 216
DSYN_ADJ_Y, DSYN_ADJ_H = 72 + 552, 44
DSYN_ADJ_DEC_X, DSYN_ADJ_INC_X, DSYN_ADJ_W = 16, 104, 80

FM6P_HEAD_Y, FM6P_HEAD_H = 72 + 4, 32          # FM6P_HEAD_Y / FM6P_CHIP_H
FM6P_OP_X0, FM6P_OP_W, FM6P_OP_PITCH = 16, 56, 60
FM6P_PAGE_X0, FM6P_PAGE_W, FM6P_PAGE_PITCH = 380, 64, 68
FM6P_BANK_X, FM6P_BANK_W, FM6P_BANK_ZONE = 792, 124, 40
FM6P_SAVE_X, FM6P_SAVE_W = 924, 72
FM6P_STATUS_X, FM6P_STATUS_W = 1004, 260
FM6P_ADJ_X, FM6P_ADJ_Y, FM6P_ADJ_W, FM6P_ADJ_H, FM6P_ADJ_ZONE = 16, 72 + 552, 168, 44, 72


def _chip_xy(x0: int, w: int, y: int, h: int, index: int = 0, pitch: int = 0) -> tuple:
    """页眉 chip 中心（屏幕绝对坐标）。"""
    return (x0 + index * pitch + w // 2, y + h // 2)


def _chip_probe(x0: int, y: int, index: int = 0, pitch: int = 0) -> tuple:
    """页眉 chip 左上内缘（x0+20, top+4）：避开 10px 圆角与字形，命中 chip 底色。"""
    return (x0 + index * pitch + 20, y + 4)


def dsyn_kit_chip_xy(k: int) -> tuple:
    return _chip_xy(DSYN_KIT_X0, DSYN_KIT_W, DSYN_HEAD_Y, DSYN_HEAD_H, k, DSYN_KIT_PITCH)


def dsyn_kit_chip_probe(k: int) -> tuple:
    return _chip_probe(DSYN_KIT_X0, DSYN_HEAD_Y, k, DSYN_KIT_PITCH)


def dsyn_lane_zone_xy(delta: int) -> tuple:
    """LANE 三区对象的 −（delta<0）/ +（delta>0）区中心。"""
    x = DSYN_LANE_X + (DSYN_LANE_ZONE // 2 if delta < 0 else DSYN_LANE_W - DSYN_LANE_ZONE // 2)
    return (x, DSYN_HEAD_Y + DSYN_HEAD_H // 2)


def dsyn_lane_mid_xy() -> tuple:
    """LANE 对象的中段（纯标签，TAP 必须 miss）。"""
    return (DSYN_LANE_X + DSYN_LANE_W // 2, DSYN_HEAD_Y + DSYN_HEAD_H // 2)


def dsyn_page_chip_xy(p: int) -> tuple:
    return _chip_xy(DSYN_PAGE_X0, DSYN_PAGE_W, DSYN_HEAD_Y, DSYN_HEAD_H, p, DSYN_PAGE_PITCH)


def dsyn_page_chip_probe(p: int) -> tuple:
    return _chip_probe(DSYN_PAGE_X0, DSYN_HEAD_Y, p, DSYN_PAGE_PITCH)


def dsyn_audition_xy() -> tuple:
    return _chip_xy(DSYN_AUD_X, DSYN_AUD_W, DSYN_HEAD_Y, DSYN_HEAD_H)


def dsyn_store_xy() -> tuple:
    return _chip_xy(DSYN_STORE_X, DSYN_STORE_W, DSYN_HEAD_Y, DSYN_HEAD_H)


def dsyn_adj_xy(inc: bool) -> tuple:
    x = DSYN_ADJ_INC_X if inc else DSYN_ADJ_DEC_X
    return (x + DSYN_ADJ_W // 2, DSYN_ADJ_Y + DSYN_ADJ_H // 2)


DSYN_STATUS_BOX = (DSYN_STATUS_X, DSYN_HEAD_Y, DSYN_STATUS_X + DSYN_STATUS_W,
                   DSYN_HEAD_Y + DSYN_HEAD_H)


def fm6p_op_chip_xy(k: int) -> tuple:
    return _chip_xy(FM6P_OP_X0, FM6P_OP_W, FM6P_HEAD_Y, FM6P_HEAD_H, k, FM6P_OP_PITCH)


def fm6p_op_chip_probe(k: int) -> tuple:
    return _chip_probe(FM6P_OP_X0, FM6P_HEAD_Y, k, FM6P_OP_PITCH)


def fm6p_page_chip_xy(p: int) -> tuple:
    return _chip_xy(FM6P_PAGE_X0, FM6P_PAGE_W, FM6P_HEAD_Y, FM6P_HEAD_H, p, FM6P_PAGE_PITCH)


def fm6p_page_chip_probe(p: int) -> tuple:
    return _chip_probe(FM6P_PAGE_X0, FM6P_HEAD_Y, p, FM6P_PAGE_PITCH)


def fm6p_bank_xy(delta: int) -> tuple:
    """BANK 三区对象的 −（delta<0）/ +（delta>0）区中心（中段 44px 是槽号标签）。"""
    x = (FM6P_BANK_X + FM6P_BANK_ZONE // 2 if delta < 0
         else FM6P_BANK_X + FM6P_BANK_W - FM6P_BANK_ZONE // 2)
    return (x, FM6P_HEAD_Y + FM6P_HEAD_H // 2)


def fm6p_save_xy() -> tuple:
    return _chip_xy(FM6P_SAVE_X, FM6P_SAVE_W, FM6P_HEAD_Y, FM6P_HEAD_H)


def fm6p_adj_xy(inc: bool) -> tuple:
    """底部三区对象的 − / + 区中心（中段 24px 死区，TAP 必须 miss）。"""
    x = (FM6P_ADJ_X + FM6P_ADJ_ZONE // 2 if not inc
         else FM6P_ADJ_X + FM6P_ADJ_W - FM6P_ADJ_ZONE // 2)
    return (x, FM6P_ADJ_Y + FM6P_ADJ_H // 2)


def fm6p_adj_dead_xy() -> tuple:
    return (FM6P_ADJ_X + FM6P_ADJ_W // 2, FM6P_ADJ_Y + FM6P_ADJ_H // 2)


FM6P_STATUS_BOX = (FM6P_STATUS_X, FM6P_HEAD_Y, FM6P_STATUS_X + FM6P_STATUS_W,
                   FM6P_HEAD_Y + FM6P_HEAD_H)


def bank_slot_btn_xy(delta: int) -> tuple:
    """bank 目标槽 `-` / `+` 按钮中心（delta<0 = 减）。"""
    x = BANK_DEC_X if delta < 0 else BANK_INC_X
    return (x + BANK_SLOT_W // 2, BANK_ROW_Y + BANK_ROW_H // 2)


def bank_act_xy(act: int) -> tuple:
    """bank SAVE（act=0）/ ERASE（act=2）按钮中心。"""
    x = BANK_SAVE_X if act == 0 else BANK_ERASE_X
    return (x + BANK_ACT_W // 2, BANK_ROW_Y + BANK_ROW_H // 2)


def bank_btn_probe(act: int) -> tuple:
    """bank 动作按钮的像素探针点（按钮内左侧 12px，避开居中文本的字形）。"""
    x = BANK_SAVE_X if act == 0 else BANK_ERASE_X
    return (x + 12, BANK_ROW_Y + BANK_ROW_H // 2)


def bank_row_absent_probes(s: Session, img, what: str) -> None:
    """仪表盘页「bank 行缺席」的像素判据（A10）。行隐藏时它原来那一带（页面区 y=76..120）被
    卡片视口盖住——该带的 TAP 落在卡片标题行上（翻折叠位），**不是** miss，所以行缺席不再用
    TAP/底色证明，而是断言 SAVE（绿）/ ERASE（橙）的按钮色在整带内为 0。"""
    band = (BANK_SAVE_X, BANK_ROW_Y, BANK_ERASE_X + BANK_ACT_W, BANK_ROW_Y + BANK_ROW_H)
    for name, rgb in (("SAVE", C_GREEN), ("ERASE", C_ORANGE)):
        n = count_region(img, band, rgb, 16)
        (s.ok if n == 0 else s.fail)(f"{what}：bank 带没有 {name} 按钮色（行缺席）：{n} px")


def param_value(s: Session, key: str = "value") -> int:
    """`PARAM <id>` / `PARAM GLOBAL <id>` 回显里的数值字段。缺字段直接 InjectError
    （固件回显改名时不得静默通过）。"""
    m = re.search(rf"\b{key}=(-?\d+)", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"PARAM 回显缺少 {key}=：{s.last!r}")
    return int(m.group(1))


def param_mem(s: Session) -> dict:
    """`UI MEM` → {page_id: (state, est, delta)} + 首行字段 + 分组行。

    首行字段另含 `lvgl_stack_free`（LVGL 任务栈的历史最小余量，字节；缺失记 -1 —— 固件回显
    改名时不得静默通过，见 LVGL_STACK_FLOOR 的断言）。

    分组行（ADR-005 §5）：`UI MEM GROUP <name> built=<n> sum_est=<B> sum_delta=<B>
    build_ms=<ms>`；sum_est = 组内已注册成员 est（静态预算），sum_delta = 组内已建
    成员的池增量（离开组后 built/sum_delta 回 0）。"""
    s.cmd("UI MEM")
    m = re.search(r"UI MEM pool_used=(\d+) pool_free=(\d+) pool_total=(\d+) registered=(\d+) "
                  r"built=(\d+) failed=(\d+) sum_est=(\d+) reserve=(-?\d+)", s.last)
    if m is None:
        raise InjectError("UI MEM 首行缺失")
    head = dict(zip(("used", "free", "total", "registered", "built", "failed", "sum_est", "reserve"),
                    map(int, m.groups())))
    gm = re.search(r"group=(\w+|-)", s.last)
    head["group"] = gm.group(1) if gm else "?"
    sm = re.search(r"lvgl_stack_free=(\d+)", s.last)
    head["lvgl_stack_free"] = int(sm.group(1)) if sm else -1
    pages = {}
    for line in s.last.splitlines():
        pm = re.search(r"UI MEM page id=(\d+) title=(\S+) state=(\w+) est=(\d+) delta=(-?\d+)", line)
        if pm:
            pages[int(pm.group(1))] = (pm.group(3), int(pm.group(4)), int(pm.group(5)))
    groups = {}
    for line in s.last.splitlines():
        gr = re.search(r"UI MEM GROUP (\w+) built=(\d+) sum_est=(\d+) sum_delta=(\d+) "
                       r"build_ms=(\d+)", line)
        if gr:
            groups[gr.group(1)] = {
                "built": int(gr.group(2)),
                "sum_est": int(gr.group(3)),
                "sum_delta": int(gr.group(4)),
                "build_ms": int(gr.group(5)),
            }
    if len(groups) != len(UI_GROUP_NAMES):
        raise InjectError(f"UI MEM 分组行不完整（{len(groups)}/{len(UI_GROUP_NAMES)}）：{s.last!r}")
    return {"head": head, "pages": pages, "groups": groups}


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


# 上游 core.h 的 per-track 参数 id（与 console 的 CONSOLE_P_SLEN=29 同源）：
# P_SLEN = 图案长度（引擎快照 trk.len，STEP 的 DIM 判据）、P_SDIV = 时钟分频
# （trk.div，各轨播放头是否同步）。
P_SLEN_ID, P_SDIV_ID = 29, 30
P_VOICE_ID = 37  # P_VOICE（POLY/MONO/LEGATO/UNISON）：②e CONT 用例要求 POLY
PATTERN_LEN_DEF, PATTERN_DIV_DEF = 16, 2


def step_is_on(s: Session) -> bool:
    """当前 `ENG STEP` 回显的步是否「开」——与状态层 state_step_is_on 同口径
    （n>0 且 time==NOTE(0)）。TIE 步（n>0、time=1）也算空：CMD_TOGGLE_STEP 对它是
    「空 → 开」，脚本的基线判定不能只看 n（否则会把 TIE 步点成 NOTE、再被下一次
    TAP 关掉——设备实测踩到：store 场景载入的旧格式 fixture 在步 16 留了 TIE）。"""
    n = s.snap_int(r"\bn=(\d+)")
    t = s.snap_int(r"\btime=(\d+)")
    return n != 0 and t == 0


def eng_step(s: Session, track: int, step: int) -> dict:
    """`ENG STEP` 全字段（步的引擎真值；P4-C Task 9 的深度态断言入口）。"""
    s.cmd(f"ENG STEP {track} {step}")
    m = re.search(r"ENG STEP k=(\d+) i=(\d+) n=(\d+) note=(\d+),(\d+),(\d+),(\d+) vel=(\d+) "
                  r"time=(\d+) flags=(\d+) lvl=(\d+) rat=(\d+) micro=(-?\d+) fill=(\d+)", s.last)
    if m is None:
        raise InjectError(f"ENG STEP 回显缺少字段：{s.last.strip()!r}")
    keys = ("k", "i", "n", "note0", "note1", "note2", "note3", "vel", "time", "flags",
            "lvl", "rat", "micro", "fill")
    return {k: int(v) for k, v in zip(keys, m.groups())}


def live_hash(s: Session) -> int:
    """`STORE LIVEHASH` 的当前引擎捕获 hash（十六进制；工程往返门禁）。"""
    s.cmd("STORE LIVEHASH")
    m = re.search(r"LIVEHASH ([0-9A-F]{8})", s.last)
    if m is None:
        raise InjectError(f"STORE LIVEHASH 回显缺少哈希：{s.last.strip()!r}")
    return int(m.group(1), 16)


def seq_dstep_mask(s: Session, step: int) -> int:
    """`SEQ DSTEP <step>` 的原始 16 lane on 位图（bit k = lane k）。"""
    s.cmd(f"SEQ DSTEP {step}")
    m = re.search(r"SEQ DSTEP step=(\d+) on=([0-9A-Fa-f]{2}),([0-9A-Fa-f]{2})", s.last)
    if m is None:
        raise InjectError(f"SEQ DSTEP 回显缺少字段：{s.last.strip()!r}")
    return int(m.group(3), 16) << 8 | int(m.group(2), 16)


def seq_dstep(s: Session, step: int) -> dict:
    """`SEQ DSTEP <step>` 的完整原始字段：on 位图 + lvl/rat（各 4 字节，每字节 4 lane×2bit）。"""
    s.cmd(f"SEQ DSTEP {step}")
    m = re.search(r"SEQ DSTEP step=(\d+) on=([0-9A-F]{2}),([0-9A-F]{2}) "
                  r"lvl=([0-9A-F]{2}),([0-9A-F]{2}),([0-9A-F]{2}),([0-9A-F]{2}) "
                  r"rat=([0-9A-F]{2}),([0-9A-F]{2}),([0-9A-F]{2}),([0-9A-F]{2})", s.last)
    if m is None:
        raise InjectError(f"SEQ DSTEP 回显缺少字段：{s.last.strip()!r}")
    g = [int(x, 16) for x in m.groups()[1:]]
    return {"on": g[1] << 8 | g[0], "lvl": g[2:6], "rat": g[6:10]}


def dstep_bits(arr, lane: int) -> int:
    """鼓步打包字段（lvl/rat）的 lane 2-bit 值（page_transport.c 的 dstep_bits 同源）。"""
    return (arr[lane // 4] >> (2 * (lane % 4))) & 3


def normalize_pattern(s: Session, label: str = "归一图案参数") -> None:
    """把三条合成轨的 LEN/DIV 归一到默认（16 / 2）。

    为什么需要：transport 的「各行播放头与引擎 step_pos 同列」与 step_edit 的
    「绝对步 ≥ LEN 暗显」都以这两个值为前提，而设备开机恢复的 autosave 可能带着别的
    工程/旧格式 fixture 的 LEN=64/DIV=8（div 不同 ⇒ 各轨播放头不同步；LEN=64 ⇒ bank1
    的格不暗显）——这是**场景假设**问题（原 DI-23），不是固件行为差异。脚本须幂等
    （AGENTS §5.4.1），故在依赖它们的场景开头显式归一。选中轨临时改动由调用方
    后续用例自行归一（transport/step_edit 都会显式选轨或只读快照）。"""
    for k in range(3):
        select_track(s, k)
        for pid, want in ((P_SLEN_ID, PATTERN_LEN_DEF), (P_SDIV_ID, PATTERN_DIV_DEF)):
            s.cmd(f"PARAM {pid} {want}")
            s.expect(rf"PARAM track={k} id={pid} value={want} applied={want} ",
                     f"{label}：轨 {k} id={pid}={want}")


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


LVGL_STACK_FLOOR = 2048         # LVGL 任务栈最小余量门禁（字节；栈 10,240，ADR-008 §8.10）


# ---------------------------------------------------------------- TOOLS（P4-F Task 6）---
# TOOLS 页三张 chip（page_tools.c 的 TOOLS_CHIP_*：x = 24 + i*384、y=120、360×140）⇒ 屏幕中心
# （y + UI_TAB_BAR_H=72）。顺序 = sloop_tool_t：0 CLEAR SEQ / 1 INIT SOUND / 2 NEW。
TOOLS_CHIP = tuple((24 + i * 384 + 180, 72 + 120 + 70) for i in range(3))
TOOLS_NAMES = ("CLRSEQ", "INITSOUND", "NEWPRJ")
TOOLS_STATUS_BOX = (24, 92, 700, 130)   # 状态行 label (24,20) 28pt
# TRANSPORT 的 REC 配置行（page_transport.c 的 TR_CFG_*：一个自绘对象、三格 x=456+i*108、
# y=384、100×56；格间 8px 缝是页面背景）。格顺序 = SLOOP_TR_REC_QNT / MODE / START。
TR_CFG = tuple((456 + i * 108 + 50, 72 + 384 + 28) for i in range(3))
TR_CFG_GAP = (456 + 104, 72 + 384 + 28)
TR_CFG_TOKEN = ("quant", "mode", "start")
TR_UNDO = (1048 + 42, 72 + 360 + 28)    # UNDO chip（84 宽）
# STEP 页 EDIT 层步工具键（input_map.h / ADR-006）：Aa 0x31 按住 + k 0x39 / l 0x3A / 9 0x09
KEY_AA, KEY_K, KEY_L, KEY_9 = 0x31, 0x39, 0x3A, 0x09
INPUT_STACK_FLOOR = 2048         # 输入任务栈（8,192B）的最小余量门禁（R13）
P_SLEN = 29                      # 上游 P_SLEN：LEN 之外的聚焦步不可锁/不可设条件（R12）


def wait_playing(s: Session, want: int, label: str, timeout: float = 2.5) -> None:
    """轮询 `STATE SNAP playing=`（走带请求在音频块边界才被消费，不能假定命令返回即生效）。"""
    deadline = time.monotonic() + timeout
    got = -1
    while time.monotonic() < deadline:
        s.cmd("STATE SNAP")
        got = s.snap_int(r"playing=(\d)")
        if got == want:
            break
        time.sleep(0.03)
    (s.ok if got == want else s.fail)(f"{label}: STATE SNAP playing={got}（期望 {want}）")


def tools_tap(s: Session, idx: int):
    """点 TOOLS 页第 idx 张 chip。返回 (是否只进入预备态, 状态行文本)；回显缺失记 FAIL。"""
    s.cmd("TAP %d %d" % TOOLS_CHIP[idx])
    m = re.search(rf"-> TOOLS {TOOLS_NAMES[idx]} (armed|value={idx}) status=\"([^\"]*)\"", s.last)
    if m is None:
        lines = [ln for ln in s.last.splitlines() if "TOOLS" in ln]
        s.fail(f"TOOLS {TOOLS_NAMES[idx]} TAP 回显缺失：{lines}")
        return None, None
    armed = m.group(1) == "armed"
    s.ok(f"TAP {TOOLS_NAMES[idx]} → {'armed' if armed else 'executed'} status=\"{m.group(2)}\"")
    return armed, m.group(2)


def rec_cfg_read(s: Session) -> tuple:
    """`SEQ REC` 的三项配置 (quant_off, tempo_mode, count_mode)，各 0/1。"""
    s.cmd("SEQ REC")
    m = re.search(r"quant=(on|off) tempo=(free|tempo) count=(note|count)", s.last)
    if m is None:
        raise InjectError(f"SEQ REC 回显缺少配置三项：{s.last.strip()!r}")
    return (m.group(1) == "off", m.group(2) == "tempo", m.group(3) == "count")


def ensure_step_on(s: Session, track: int, col: int) -> dict:
    """保证轨 track 的绝对步 col 是**开**的 NOTE（n>0 且 time==NOTE(0)），返回 `ENG STEP` 全字段。
    与状态层 state_step_is_on 同口径：残留的 n>0 / time=REST·TIE 步算空（PARAM 页此前
    的 SPAN/UNDO 用例会留下这种步）——CMD_TOGGLE_STEP 把它开成 NOTE。"""
    e = eng_step(s, track, col)
    if not (e["n"] > 0 and e["time"] == 0):
        x, y = step_cell_xy(col, track)
        s.cmd(f"TAP {x} {y}")
        s.expect(rf"-> STEP track={track} step={col} CMD_TOGGLE_STEP",
                 f"TAP 步 ({track},{col}) 建音（残留 n={e['n']} time={e['time']} 算空）")
        e = eng_step(s, track, col)
    (s.ok if e["n"] > 0 and e["time"] == 0 else s.fail)(
        f"轨 {track} 绝对步 {col} 是开的 NOTE（n={e['n']} note={e['note0']},{e['note1']}）")
    return e


def ensure_step_on2(s: Session, track: int, col: int):
    """ensure_step_on 的变体：返回 (ENG STEP 全字段, 是否由本次 TAP 新建)。"""
    before = eng_step(s, track, col)
    e = ensure_step_on(s, track, col)
    return e, not (before["n"] > 0 and before["time"] == 0)


def param_input_stack(s: Session) -> None:
    """`KEY STATS` 的输入任务栈余量断言（R13）。**该读数只在物理按键走过深路径后才会下降**：
    console 的 `KEY` 注入跑在 console 任务上，量不到 input 任务的栈。`phys` = input 任务自己
    从键盘轮询派发的事件数；phys == 0 表示本轮没有任何物理按键，读数只说明轮询循环很浅——
    此时显式跳过（记 note，不计 PASS），不得当作通过。"""
    s.cmd("KEY STATS")
    m = re.search(r"input_stack_free=(\d+) phys=(\d+)", s.last)
    if m is None:
        s.fail("KEY STATS 缺少 input_stack_free=/phys= 字段")
        return
    free, phys = int(m.group(1)), int(m.group(2))
    if phys == 0:
        s.ser.note(f"输入任务栈断言跳过：KEY STATS phys=0（本轮没有物理按键，input_stack_free="
                   f"{free}B 只是轮询循环的读数；需人手按一次 Aa+k 等深路径后再读）")
        return
    (s.ok if free >= INPUT_STACK_FLOOR else s.fail)(
        f"输入任务栈最小余量 input_stack_free={free}B（phys={phys} 个物理事件之后；≥{INPUT_STACK_FLOOR}）")


def param_rec_config(s: Session) -> None:
    """TRANSPORT 的 REC 配置行：三格 TAP = 整份 CMD_REC_CONFIG（只翻转自己那一项）、
    文本/底色像素、格间缝 TAP miss、诊断冻结拒绝。三项在结束时回到起始值。"""
    s.ser.note("⑪ TRANSPORT REC 配置行（QUANT / MODE / START）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑪ UI transport")
    cfg0 = rec_cfg_read(s)
    img = s.capture("task6-transport-rec-config")
    for i, name in enumerate(("QNT", "MODE", "START")):
        cx, cy = TR_CFG[i]
        s.probe(img, f"⑪ REC 配置 {name} 格底 = UI_PANEL_HI", cx - 40, cy - 22, C_PANEL_HI)
        s.probe_region(img, f"⑪ REC 配置 {name} 格内有白字", (cx - 46, cy - 10, cx + 46, cy + 12),
                       C_TEXT, 16, 30)
    s.probe(img, "⑪ REC 配置格间缝 = 页面背景", *TR_CFG_GAP, C_BG)
    # 索引 = 写后回读的原始值：quant_off 0 ON / 1 OFF；tempo_mode 0 FREE / 1 TEMPO；
    # count_mode 0 NOTE / 1 COUNT（上游 seq.c 的 REC 屏变量）
    status_of = (("QNT: ON", "QNT: OFF"), ("MODE: FREE", "MODE: TEMPO"),
                 ("START: NOTE", "START: COUNT"))
    for i in range(3):
        for flip in (1, 0):   # 翻转一次、再翻回来
            before = rec_cfg_read(s)
            want = list(before)
            want[i] ^= True
            s.cmd("TAP %d %d" % TR_CFG[i])
            s.expect(rf"-> TRANSPORT CMD_REC_CONFIG {TR_CFG_TOKEN[i]} value={int(want[i])} "
                     rf'status="{re.escape(status_of[i][int(want[i])])}"',
                     f"⑪ TAP {TR_CFG_TOKEN[i].upper()}（第 {2 - flip} 次）")
            got = rec_cfg_read(s)
            (s.ok if got == tuple(want) else s.fail)(
                f"⑪ SEQ REC 三项 = {got}（期望 {tuple(want)}：只翻转 {TR_CFG_TOKEN[i]}）")
    s.cmd("TAP %d %d" % TR_CFG_GAP)
    s.expect(r"TAP miss", "⑪ REC 配置格间缝 TAP miss")
    cfg1 = rec_cfg_read(s)
    (s.ok if cfg1 == cfg0 else s.fail)(f"⑪ REC 配置回到起始值 {cfg1}")
    # 诊断冻结：命令被拒、配置不变
    s.cmd("ENG FREEZE ON")
    s.cmd("TAP %d %d" % TR_CFG[0])
    s.expect(r'-> TRANSPORT CMD_REC_CONFIG quant ERR frozen .*status="QNT: FROZEN"',
             "⑪ 冻结：REC 配置 TAP → ERR frozen")
    s.cmd("ENG FREEZE OFF")
    s.expect(r"ENG FREEZE off", "⑪ 解除冻结")
    # 冻结期间 `SEQ REC` 读到的是零快照（sloop_seq_rec_read 的冻结契约），配置是否不变
    # 只能在解除冻结之后判。
    (s.ok if rec_cfg_read(s) == cfg0 else s.fail)("⑪ 冻结：REC 配置不变（解除冻结后读）")


def param_step_keys(s: Session) -> None:
    """STEP 页 EDIT 层步工具：SOUND 仪表盘的焦点槽经键盘改参数 ⇒ last_param；Aa+k 锁到聚焦步、Aa+l 解锁、
    Aa+9 循环 fill 条件；不可锁参数 ⇒ NOT LOCKABLE；只有 TIE 的步同样可锁（R12）；本轨 LEN
    之外的步 ⇒ NO STEP（R12）；诊断冻结下加锁/解锁都回 FROZEN（锁保持）。本段新建的音符在
    收尾撤掉（只撤销自己建的）。"""
    s.ser.note("⑪ STEP 键盘参数锁 / fill 条件（Aa+k / Aa+l / Aa+9）")
    select_track(s, 0)
    ensure_window(s, 0, 16)
    # SOUND 仪表盘 Envelope 卡槽 0（轨 0 的 ATK，与旧 MOD·ENV chip 的槽 0 同一个参数）：
    # `UI SOUND FOCUS` 设焦点、键盘 ↑/↓ 细调 1（经状态层的 CMD_SET_PARAM ⇒ 记 last_param）。
    # 折叠位是跨页面重建保留的 UI 局部态（折叠卡里的槽不可聚焦）：先全部展开。值改用引擎回读
    # 断言（仪表盘 ↑/↓ 的回显归 sound 场景）。
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "⑪ UI sound")
    s.cmd("UI SOUND STATS")
    m = re.search(r"collapsed=0x([0-9A-Fa-f]+)", s.last)
    if m is None:
        raise InjectError(f"UI SOUND STATS 回显缺少 collapsed=：{s.last!r}")
    for card in range(13):
        if (int(m.group(1), 16) >> card) & 1:
            s.cmd(f"UI SOUND CARDTOGGLE {card}")
            s.expect(rf"UI SOUND CARDTOGGLE card={card} collapsed=0", f"⑪ 展开 card {card}（归一）")
    s.cmd("UI SOUND CARD 2 SLOT 0")
    m = re.search(r"UI SOUND SLOT card=2 slot=0 flat=(\d+) present=1 scope=0 id=(\d+) "
                  r"label=\"([^\"]+)\"", s.last)
    if m is None:
        raise InjectError(f"UI SOUND CARD 2 SLOT 0（Envelope·ATK）的槽行缺失/不是轨道参数：{s.last!r}")
    flat, pid, label = int(m.group(1)), int(m.group(2)), m.group(3)
    s.cmd(f"UI SOUND FOCUS {flat}")
    s.expect(rf"UI SOUND FOCUS flat={flat} card=2 slot=0 ", "⑪ SOUND Envelope 卡槽 0：设焦点")
    s.cmd(f"PARAM {pid}")
    v0, vmax = param_value(s), param_value(s, "max")
    up, down = (0x3B, 0x4B) if v0 < vmax else (0x4B, 0x3B)   # ↑ / ↓
    step = 1 if v0 < vmax else -1
    s.cmd("KEY 0x%02X down" % up)
    s.cmd("KEY 0x%02X up" % up)
    param_expect_value(s, f"PARAM {pid}", v0 + step, f"⑪ SOUND 槽（{label}）键盘细调经状态层写参数")
    s.cmd("KEY 0x%02X down" % down)
    s.cmd("KEY 0x%02X up" % down)
    param_expect_value(s, f"PARAM {pid}", v0, f"⑪ {label} 回基线")

    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑪ UI step")
    nav_set_focus(s, 0, 5)
    st5, created5 = ensure_step_on2(s, 0, 5)
    s.cmd(f"PARAM {pid}")
    lock_v = param_value(s)
    s.cmd("KEY 0x%02X down" % KEY_AA)
    s.cmd("KEY 0x%02X down" % KEY_K)
    s.expect(rf'KEY 0x{KEY_K:02X} down k elapsed_us=\d+ step="LOCKED {re.escape(label)}"',
             f"⑪ Aa+k ⇒ LOCKED {label}（KEY 回显）")
    s.cmd("KEY 0x%02X up" % KEY_K)
    s.cmd("SEQ LOCK LIST 0")
    s.expect(rf"SEQ LOCK step=5 param={pid} value={lock_v}", "⑪ SEQ LOCK LIST：聚焦步 × last_param")
    s.expect(r"SEQ LOCK LIST track=0 count=1", "⑪ 轨 0 恰一把锁")
    img = s.capture("task6-step-locked")
    s.probe_region(img, "⑪ STEP 状态行显示结果（成功绿）", NAV_STATUS_BOX, C_GREEN, 16, 30)
    s.cmd("KEY 0x%02X down" % KEY_L)
    s.expect(rf'KEY 0x{KEY_L:02X} down l elapsed_us=\d+ step="UNLOCKED"', "⑪ Aa+l ⇒ UNLOCKED")
    s.cmd("KEY 0x%02X up" % KEY_L)
    s.cmd("SEQ LOCK LIST 0")
    s.expect(r"SEQ LOCK LIST track=0 count=0", "⑪ 解锁后轨 0 无锁")
    s.cmd("KEY 0x%02X down" % KEY_L)
    s.expect(rf'KEY 0x{KEY_L:02X} down l elapsed_us=\d+ step="NO LOCK"', "⑪ 再解锁 ⇒ NO LOCK")
    s.cmd("KEY 0x%02X up" % KEY_L)

    fill0 = eng_step(s, 0, 5)["fill"] & 3
    fill_name = ("FILL NORMAL", "FILL FILL ONLY", "FILL NO FILL")   # 下标 = 原始 fill 值
    for k, (want_fill, text) in enumerate((
            ((fill0 + 1) % 3, fill_name[(fill0 + 1) % 3]),
            ((fill0 + 2) % 3, fill_name[(fill0 + 2) % 3]),
            (fill0 % 3, fill_name[fill0 % 3]))):
        s.cmd("KEY 0x%02X down" % KEY_9)
        s.expect(rf'KEY 0x{KEY_9:02X} down 9 elapsed_us=\d+ step="{text}"',
                 f"⑪ Aa+9 第 {k + 1} 次 ⇒ {text}")
        s.cmd("KEY 0x%02X up" % KEY_9)
        got = eng_step(s, 0, 5)["fill"] & 3
        (s.ok if got == want_fill else s.fail)(
            f"⑪ ENG STEP fill={got}（期望 {want_fill}；SEQ FILL 同一字段）")

    # 不可锁的参数：上游 p_lockable 只含声音类（id ≤ P_LD_AMP、SGATE、FX、E0..E7、INSERT…）；
    # 图案/调性/琶音类不在其中。P_TRANS=28（移调）无发声副作用：改它 ⇒ last_param 换成它
    # ⇒ NOT LOCKABLE。（P_LEVEL=0 **是**可锁的：id ≤ P_LD_AMP——设备实测 LOCKED LVL。）
    s.cmd("PARAM 28")
    trans = param_value(s)
    s.cmd("PARAM 28 %d" % (trans + 1 if trans < 24 else trans - 1))
    s.expect(r"PARAM track=0 id=28 .*applied=", "⑪ 改一个不可锁的参数（P_TRANS）")
    s.cmd("KEY 0x%02X down" % KEY_K)
    s.expect(rf'KEY 0x{KEY_K:02X} down k elapsed_us=\d+ step="NOT LOCKABLE"', "⑪ 不可锁参数 ⇒ NOT LOCKABLE")
    s.cmd("KEY 0x%02X up" % KEY_K)
    s.cmd("SEQ LOCK LIST 0")
    s.expect(r"SEQ LOCK LIST track=0 count=0", "⑪ NOT LOCKABLE 不留锁")
    s.cmd("PARAM 28 %d" % trans)
    param_expect_value(s, "PARAM 28", trans, "⑪ P_TRANS 回基线")

    def touch_atk():
        """把 last_param 摸回可锁的 ATK（P_ATK=1）：上一段改 P_TRANS 之后 last_param 是它——
        那次写能生效（有描述符），只是不可锁。"""
        s.cmd("PARAM 1 5")
        s.expect(r"PARAM track=0 id=1 value=5 applied=5 ", "⑪ 再改一次 ATK（last_param 回到可锁参数）")
        s.cmd("PARAM 1 0")
        param_expect_value(s, "PARAM 1", 0, "⑪ ATK 回基线")

    # R12：只有 TIE 的步（n>0、time=TIE，不是 NOTE）同样在锁的范围内（上游 held-step 循环
    # 只要求 idx < trk_len）：把步 8 的音拉成 3 步 TIE 链，聚焦到 TIE 步 9 再锁。
    touch_atk()
    _, created8 = ensure_step_on2(s, 0, 8)
    s.cmd("SEQ EDIT SPAN 0 8 3")
    s.expect(r"SEQ EDIT SPAN track=0 step=8 OK", "⑪ 步 8 拉成 3 步 TIE 链")
    tie = eng_step(s, 0, 9)
    # TIE 步的编码：time=TIE(1)、n=0（上游 note_set_len 对延续步 memset 后只置 time）
    (s.ok if tie["n"] == 0 and tie["time"] == 1 else s.fail)(
        f"⑪ 前提：步 9 是 TIE（n={tie['n']} time={tie['time']}）")
    nav_set_focus(s, 0, 9)
    s.cmd("KEY 0x%02X down" % KEY_K)
    s.expect(rf'KEY 0x{KEY_K:02X} down k elapsed_us=\d+ step="LOCKED {re.escape(label)}"',
             "⑪ 只有 TIE 的步也可以加锁（R12）")
    s.cmd("KEY 0x%02X up" % KEY_K)
    s.cmd("SEQ LOCK LIST 0")
    s.expect(rf"SEQ LOCK step=9 param={pid} ", "⑪ TIE 步的锁在锁表里（step=9）")
    s.cmd("KEY 0x%02X down" % KEY_L)
    s.expect(rf'KEY 0x{KEY_L:02X} down l elapsed_us=\d+ step="UNLOCKED"', "⑪ TIE 步解锁")
    s.cmd("KEY 0x%02X up" % KEY_L)
    s.cmd("SEQ EDIT SPAN 0 8 1")
    s.expect(r"SEQ EDIT SPAN track=0 step=8 OK", "⑪ TIE 链收回 1 步")

    # 诊断冻结：加锁与解锁都回 FROZEN，且不查锁表（否则解锁会被误报成 NO LOCK）、锁保持
    touch_atk()
    nav_set_focus(s, 0, 5)
    s.cmd("KEY 0x%02X down" % KEY_K)
    s.expect(rf'KEY 0x{KEY_K:02X} down k elapsed_us=\d+ step="LOCKED {re.escape(label)}"', "⑪ 冻结前加锁")
    s.cmd("KEY 0x%02X up" % KEY_K)
    s.cmd("ENG FREEZE ON")
    for key, name in ((KEY_L, "l"), (KEY_K, "k")):
        s.cmd("KEY 0x%02X down" % key)
        s.expect(rf'KEY 0x{key:02X} down {name} elapsed_us=\d+ step="FROZEN"', f"⑪ 冻结：Aa+{name} ⇒ FROZEN")
        s.cmd("KEY 0x%02X up" % key)
    s.cmd("ENG FREEZE OFF")
    s.cmd("SEQ LOCK LIST 0")
    s.expect(rf"SEQ LOCK step=5 param={pid} ", "⑪ 冻结期间锁没有被解锁（解冻后仍在）")
    s.cmd("KEY 0x%02X down" % KEY_L)
    s.expect(rf'KEY 0x{KEY_L:02X} down l elapsed_us=\d+ step="UNLOCKED"', "⑪ 解冻后解锁")
    s.cmd("KEY 0x%02X up" % KEY_L)

    # R12：本轨 LEN 之外的聚焦步 ⇒ NO STEP、不调引擎（不管该步有没有音）
    s.cmd("PARAM %d" % P_SLEN)
    slen = param_value(s)
    s.cmd("PARAM %d 8" % P_SLEN)
    s.expect(r"PARAM track=0 id=29 value=8 applied=8 ", "⑪ LEN 临时改为 8")
    nav_set_focus(s, 0, 10)
    s.cmd("KEY 0x%02X down" % KEY_K)
    s.expect(rf'KEY 0x{KEY_K:02X} down k elapsed_us=\d+ step="NO STEP"', "⑪ LEN 之外的步 ⇒ NO STEP（R12）")
    s.cmd("KEY 0x%02X up" % KEY_K)
    s.cmd("KEY 0x%02X down" % KEY_9)
    s.expect(rf'KEY 0x{KEY_9:02X} down 9 elapsed_us=\d+ step="NO STEP"', "⑪ LEN 之外的步 ⇒ FILL 也 NO STEP")
    s.cmd("KEY 0x%02X up" % KEY_9)
    s.cmd("PARAM %d %d" % (P_SLEN, slen))
    param_expect_value(s, "PARAM %d" % P_SLEN, slen, "⑪ LEN 回基线")
    s.cmd("SEQ LOCK LIST 0")
    s.expect(r"SEQ LOCK LIST track=0 count=0", "⑪ 本段收尾：轨 0 无锁")
    # 空步：先找一个没音的列并把焦点移过去
    s.cmd("KEY 0x%02X up" % KEY_AA)
    s.cmd("STATE SNAP")
    s.expect_int(r"layer_held=0x([0-9A-Fa-f]+)", 0, "⑪ Aa 松开后 layer_held=0")
    # 撤掉本段自己建的音符（step 8 的 TIE 链已收回；只清「原本没有、由本段 TAP 建出」的步）
    nav_set_focus(s, 0, 8)
    if created8:
        s.cmd("TAP %d %d" % step_cell_xy(8, 0))
        s.expect(r"-> STEP track=0 step=8 CMD_TOGGLE_STEP", "⑪ 撤掉本段在步 8 建的音")
    nav_set_focus(s, 0, 5)
    if created5:
        s.cmd("TAP %d %d" % step_cell_xy(5, 0))
        s.expect(r"-> STEP track=0 step=5 CMD_TOGGLE_STEP", "⑪ 撤掉本段在步 5 建的音")
    param_input_stack(s)


def param_tools(s: Session) -> None:
    """TOOLS 页：两段式确认（未确认 ⇒ 状态不变；超时取消）、走带守卫（CLRSEQ/INITSOUND 走带中
    允许、NEW 在 SONG 播放中拒绝）、NEW 的副作用。**破坏性**（NEW 清空工作工程）：先把工作
    工程存进诊断槽 3，结束 LOAD 还原并以 LIVEHASH 逐位证明；section 用诊断槽 D——它与工程
    槽 3 是同一个存储槽，故在捕获 D 之前先 LOAD 回快照，收口以 SECTION ERASE 清空该槽。
    **破坏性**：诊断槽 3 / section D 的入口内容无法保留（工程快照与 D 捕获先后覆盖它；
    入口非空时收口后该槽为空）。歌曲顺序与 SONG/LOOP 模式按入口快照还原（session_once：
    截屏复位后的重跑沿用第一轮的快照）；入口歌曲若引用 D，D 已空——那一项还原后缺 scene。
    快照基线跨重跑保持（proj_snapshot_open，终审 C-I1）。"""
    s.ser.note("⑪ TOOLS 页：两段式确认 / 走带守卫 / NEW（工程快照 → 槽 3，结束还原）")
    # 先停走带再快照：SAVE/LOAD 在走带中都被存储层拒绝（STOP BEFORE …）
    ensure_stopped(s)

    def song_state():
        cfg = song_cfg(s)
        s.cmd("SEQ SONG")
        m = re.search(r"^SEQ SONG mode=(song|loop) ", s.last, re.MULTILINE)
        if m is None:
            raise InjectError(f"SEQ SONG 回显缺少 mode：{s.last.strip()!r}")
        return cfg, m.group(1)

    song0, song_mode0 = session_once(s, "params.tools.song", song_state, "⑪ 歌曲顺序快照")
    s.ser.note(f"⑪ 入口歌曲：count={song0['count']} loop={song0['loop']} mode={song_mode0} "
               f"entries={sorted(song0['entry'].items())[:song0['count']]}")
    h0, _ = proj_snapshot_open(s, "params.tools", 3, "⑪")
    select_track(s, 0)
    ensure_window(s, 0, 16)
    s.cmd("UI step")
    nav_set_focus(s, 0, 5)
    st5 = ensure_step_on(s, 0, 5)
    s.cmd("UI tools")
    s.expect(r"UI page=tools \(18\)", "⑪ UI tools")
    img = s.capture("task6-tools")
    for i, name in enumerate(("CLEAR SEQ", "INIT SOUND", "NEW")):
        s.probe(img, f"⑪ {name} chip 底 = UI_PANEL_HI", TOOLS_CHIP[i][0] - 150,
                TOOLS_CHIP[i][1] - 52, C_PANEL_HI)
    s.probe_region(img, "⑪ 状态行 READY（灰字）", TOOLS_STATUS_BOX, C_TEXT_DIM, 16, 40)

    # 未确认 ⇒ 状态不变；预备 = chip 强调底 + AGAIN TO CONFIRM
    armed, st = tools_tap(s, 0)
    (s.ok if armed is True and st == "AGAIN TO CONFIRM" else s.fail)(
        f"⑪ 第一次点 CLEAR SEQ 只预备：armed={armed} status={st!r}")
    (s.ok if eng_step(s, 0, 5) == st5 else s.fail)("⑪ 未确认：步表不变")
    img = s.capture("task6-tools-armed")
    s.probe(img, "⑪ 预备 chip 底 = UI_ACCENT", TOOLS_CHIP[0][0] - 150, TOOLS_CHIP[0][1] - 52, C_YELLOW)
    for i in (1, 2):
        s.probe(img, f"⑪ 非预备 chip {i} 底仍是 UI_PANEL_HI", TOOLS_CHIP[i][0] - 150,
                TOOLS_CHIP[i][1] - 52, C_PANEL_HI)
    s.probe_region(img, "⑪ 状态行 AGAIN TO CONFIRM（强调色）", TOOLS_STATUS_BOX, C_YELLOW, 16, 40)
    # 预备期间点另一张 = 改为预备那一张
    tools_tap(s, 2)
    armed, st = tools_tap(s, 1)
    (s.ok if armed is True and st == "AGAIN TO CONFIRM" else s.fail)("⑪ 预备 NEW 时点 INIT SOUND = 改预备")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK .*OK", "⑪ 轨 0 归一 ANALOG 预置 0")
    time.sleep(1.7)     # > 1.5s 窗口：上一次预备已超时取消
    s.cmd("ENG TRACK 0 0 3")
    armed, st = tools_tap(s, 1)
    (s.ok if armed is True else s.fail)(
        f"⑪ 超时后再点是**重新预备**（未执行：armed={armed} status={st!r}）")
    armed, st = tools_tap(s, 1)
    (s.ok if armed is False and st == "SOUND INIT" else s.fail)(
        f"⑪ 窗口内再点 INIT SOUND 执行：armed={armed} status={st!r}")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 preset=0 ", "⑪ INIT SOUND：引擎保留、预置 0")
    s.cmd("ENG TRACK 0 0 0")

    # CLEAR SEQ 走带中允许且不中断；UNDO 可撤销
    s.cmd("UI step")
    nav_set_focus(s, 0, 5)
    ensure_step_on(s, 0, 5)
    s.cmd("KEY 0x4D down")
    s.cmd("KEY 0x4D up")
    wait_playing(s, 1, "⑪ 物理 PLAY")
    s.cmd("UI tools")
    tools_tap(s, 0)
    armed, st = tools_tap(s, 0)
    (s.ok if armed is False and st == "PATTERN CLEARED" else s.fail)(
        f"⑪ 走带中 CLEAR SEQ 执行：armed={armed} status={st!r}")
    (s.ok if step_fields(s, 0, 5)[0] == 0 else s.fail)("⑪ CLEAR SEQ 后步表已清")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑪ CLEAR SEQ 走带不中断")
    img = s.capture("task6-tools-cleared")
    s.probe(img, "⑪ 执行后 chip 回普通底", TOOLS_CHIP[0][0] - 150, TOOLS_CHIP[0][1] - 52, C_PANEL_HI)
    s.probe_region(img, "⑪ 状态行 PATTERN CLEARED（成功绿）", TOOLS_STATUS_BOX, C_GREEN, 16, 40)
    ensure_stopped(s)
    s.cmd("TAP %d %d" % TR_UNDO)
    s.expect(r'-> TRANSPORT CMD_PATTERN undo value=0 status="UNDO: OK"', "⑪ TRANSPORT UNDO")
    (s.ok if eng_step(s, 0, 5) == st5 else s.fail)("⑪ UNDO 撤销 CLEAR SEQ（全字段）")

    # 鼓轨的 INIT SOUND：上游 page_desc 不给鼓轨这一项
    select_track(s, 3)
    s.cmd("PARAM 63")
    kit = param_value(s)
    s.cmd("UI tools")
    tools_tap(s, 1)
    armed, st = tools_tap(s, 1)
    (s.ok if armed is False and st == "DRUM TRACK: NO SOUND" else s.fail)(
        f"⑪ 鼓轨 INIT SOUND 拒绝：armed={armed} status={st!r}")
    s.cmd("PARAM 63")
    param_expect_value(s, "PARAM 63", kit, "⑪ 鼓轨 kit 不变")

    # 还原工作工程（此刻还原：section A–D 与工程槽 A–D 是**同一组四个存储槽**，下面的
    # 诊断 section D 就是槽 3——先 LOAD 回快照，D 捕获到的才是与快照相同的状态）
    if proj_slot_state(s, 3) != h0:
        raise InjectError("⑪ 捕获 section D 之前：诊断槽 3 不是工作工程快照——终止（不让 D 捕获"
                          "覆盖来历不明的内容后继续）")
    store_cmd(s, "STORE PROJ LOAD 3", rf"^PROJ 3 LOADED hash={h0:08X}\r?$",
              "⑪ 还原工作工程（捕获 section D 之前）")
    (s.ok if live_hash(s) == h0 else s.fail)("⑪ 还原后 LIVEHASH == 快照")

    # NEW 的唯一守卫：SONG 播放中且 arrangement_enabled ⇒ STOP THE SONG FIRST
    select_track(s, 0)
    s.cmd("SEQ SECTION 3 STORE")
    s.expect(r"SEQ SECTION D STORE OK", "⑪ 诊断 section D = 当前 loop")
    s.cmd("STORE SONG SET 1 0 3:1")
    s.expect(r"STORE SONG SET count=1 loop=0 dirty=1", "⑪ 歌曲 = D ×1")
    s.cmd("SEQ SONG MODE song")
    s.expect(r"SEQ SONG mode=song", "⑪ SONG 模式")
    s.cmd("PARAM GLOBAL 0 137")
    s.expect(r"PARAM GLOBAL id=0 value=137 applied=137 ", "⑪ 标记 BPM=137")
    s.cmd("UI tools")
    s.cmd("KEY 0x4D down")
    s.cmd("KEY 0x4D up")
    wait_playing(s, 1, "⑪ SONG 起播")
    s.cmd("SEQ SONG")
    s.expect(r"SEQ SONG mode=song running=1 ", "⑪ 歌曲播放中")
    ready_before = s.snap_int(r"ready=0x([0-9A-Fa-f]+)") if re.search(r"ready=0x", s.last) else -1
    tools_tap(s, 2)
    armed, st = tools_tap(s, 2)
    (s.ok if armed is False and st == "STOP THE SONG FIRST" else s.fail)(
        f"⑪ SONG 播放中 NEW 拒绝：armed={armed} status={st!r}")
    s.cmd("STATE SNAP")
    s.expect_int(r"bpm=(\d+)", 137, "⑪ 被拒的 NEW 不改 BPM")
    s.expect_int(r"playing=(\d)", 1, "⑪ 被拒的 NEW 不停走带")
    ensure_stopped(s)
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⑪ 回 loop 模式")

    # loop 走带中 NEW：自行停走带后执行（四轨默认声音与参数、BPM 90、A–D 段保留）
    s.cmd("KEY 0x4D down")
    s.cmd("KEY 0x4D up")
    wait_playing(s, 1, "⑪ loop 起播")
    s.cmd("UI tools")
    tools_tap(s, 2)
    armed, st = tools_tap(s, 2)
    (s.ok if armed is False and st == "NEW LOOP, A-D KEPT" else s.fail)(
        f"⑪ NEW 执行：armed={armed} status={st!r}")
    s.cmd("STATE SNAP")
    s.expect_int(r"bpm=(\d+)", 90, "⑪ NEW：BPM 回默认 90")
    wait_playing(s, 0, "⑪ NEW 自行停走带")
    s.cmd("ENG SNAP")
    for k, eng, pre in ((0, 0, 0), (1, 1, 0), (2, 4, 5)):
        s.expect(rf"ENG SNAP trk k={k} engine={eng} preset={pre} ", f"⑪ NEW：轨 {k} = TRK_DEF 声音")
    (s.ok if step_fields(s, 0, 5)[0] == 0 else s.fail)("⑪ NEW：图案清空")
    s.cmd("SEQ SECTION")
    s.expect(rf"SEQ SECTION ready=0x0*{ready_before:X} ", "⑪ NEW：section A–D 保留")

    # 还原：工作工程（LIVEHASH 逐位）→ 擦诊断 section D（= 清空诊断槽 3；先擦 D 再还歌曲：
    # SECTION ERASE 的 arr_drop_scene 会摘掉引用 D 的歌曲项）→ 入口的歌曲顺序与 SONG/LOOP 模式。
    # 槽 3 在此无条件清空：D 捕获已覆盖过快照，入口非空的内容留不住（见段首）。
    proj_snapshot_close(s, "params.tools", 3, "⑪", erase=False)
    s.cmd("SEQ SECTION 3 ERASE")
    s.expect(r"SEQ SECTION D ERASE OK", "⑪ 擦诊断 section D")
    ent = [song0["entry"].get(i, (0, 4)) for i in range(song0["count"])]
    s.cmd("STORE SONG SET %d %d %s" % (song0["count"], song0["loop"],
                                       " ".join(f"{sc}:{bars}" for sc, bars in ent)))
    s.expect(rf"STORE SONG SET count={song0['count']} loop={song0['loop']} dirty=1",
             "⑪ 歌曲顺序回入口快照")
    s.cmd(f"SEQ SONG MODE {song_mode0}")
    s.expect(rf"SEQ SONG mode={song_mode0}", "⑪ SONG/LOOP 模式回入口快照")
    song1 = song_cfg(s)
    (s.ok if (song1["count"], song1["loop"]) == (song0["count"], song0["loop"]) and
     all(song1["entry"].get(i) == song0["entry"].get(i) for i in range(song0["count"])) else s.fail)(
        f"⑪ 歌曲顺序逐项还原：count={song1['count']} loop={song1['loop']}")
    s.cmd("STORE PROJ LIST")
    s.expect(r"PROJ 3 EMPTY", "⑪ 诊断槽 3（= section D）已空")
    del s.snapshots["params.tools.song"]
    select_track(s, 0)



def run_params(s: Session) -> None:
    """④ 参数页：SOUND 页眉/bank 行 + SAMPLE/DSYN/FM6P 编辑 + 池水位 + 截屏。

    5B-1 起 SOUND 是卡片仪表盘、原 GLOBAL/FX/MOD/SCALE/VOICE 五张族页退役（EDIT = SOUND、
    MIX = TOOLS，页表 13 页）。本场景只留与 8 槽旋钮网格无关的部分：SOUND 的 console 别名与
    退役页的 not_registered、页眉 ENGINE/PRESET ±（鼓轨 KIT 行）、FM6 引擎才出现的 bank 行；
    SAMPLE/DSYN/FM6P（SYNTH 组）；TOOLS / TRANSPORT 的 REC 配置行 / STEP 键盘参数锁。"""
    # 族页（GLOBAL/FX/MOD/SCALE/VOICE）与 SOUND 8 槽旋钮段的断言由 sound 场景接管（Task 8）。
    # 归一到轨 0（页眉行按选中轨显示；TRACKS 卡 0 = x=24+148, y=72+40+235）
    s.cmd("UI tracks")
    s.expect(r"UI page=tracks \(1\)", "UI tracks（归一选轨）")
    s.cmd("TAP 172 347")
    s.expect(r"-> TRACKS select track=0 CMD_SELECT_TRACK", "TAP 卡 0")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 0, "STATE SNAP selected_track=0")
    # 引擎归一到 ANALOG 预置 0：页眉 ± 的环绕断言（ENGINE − → NOISE、PRESET 的 808 DIRTY /
    # 808 BOOM）按 ANALOG 的预置表写；上一轮会话（store 场景的工程载入 / autosave resume）
    # 可能把轨 0 留在别的引擎上。引擎已经是 0 时该命令可能无操作。
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "归一轨 0 引擎 = ANALOG 预置 0")

    mem0 = param_mem(s)
    (s.ok if mem0["head"]["registered"] == 13 else s.fail)(
        f"UI MEM registered=13（4 基础 + EDIT 1 + MIX 1 + 3 SYNTH + 2 FILE + SONG + SYSTEM；"
        f"5B-1 退役 GLOBAL/FX/MOD/SCALE/VOICE，id 保留为不注册的枚举值；id 7 仍是空位）"
        f"实得 {mem0['head']['registered']}")

    # ---------- SOUND 页（5B-1 卡片仪表盘；轨 0 = ANALOG）：console 别名、退役页 not_registered、
    # 页眉 ENGINE/PRESET 三区对象、非 FM6 轨没有 bank 行 ----------
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "UI sound")
    mem_sound = param_mem(s)
    st, est, delta = mem_sound["pages"].get(6, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est <= SOUND_EST else s.fail)(
        f"UI MEM SOUND state={st} delta={delta} est={est}（0<delta≤est≤{SOUND_EST}）")
    (s.ok if mem_sound["groups"]["EDIT"]["built"] == 1
     and mem_sound["groups"]["EDIT"]["sum_est"] == PARAM_EDIT_SUM_EST else s.fail)(
        f"UI MEM EDIT built={mem_sound['groups']['EDIT']['built']} "
        f"sum_est={mem_sound['groups']['EDIT']['sum_est']}（期望 1 / {PARAM_EDIT_SUM_EST:,}：5B-1 起 "
        f"EDIT 只有 SOUND）")
    # 切页保值：经 `UI analog` / `UI fm6` 两个别名切回都是 SOUND
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "切 PLAY")
    s.cmd("UI analog")
    s.expect(r"UI page=sound \(6\)", "切回 SOUND（`UI analog` 是 SOUND 的 console 别名）")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "切 PLAY（别名对照）")
    s.cmd("UI fm6")
    s.expect(r"UI page=sound \(6\)", "切回 SOUND（`UI fm6` 是 SOUND 的 console 别名）")
    for name, pid in (("global", 4), ("fx", 5), ("mod", 15), ("scale", 16), ("voice", 17)):
        s.cmd(f"UI {name}")
        s.expect(rf"UI ERR not_registered {pid}", f"UI {name}（5B-1 退役页）→ not_registered")
    s.cmd("UI unused")
    s.expect(r"UI ERR not_registered 7", "UI unused（空位 id 7）→ not_registered")
    s.cmd("UI")
    s.expect(r"UI page=sound \(6\)", "越界注入不改页（仍 SOUND）")

    # 页眉 ENGINE / PRESET ±（三区对象；TAP 与控件回调同一动作函数 = 一条状态层命令）
    s.cmd("TAP %d %d" % sound_row_xy(0, -1))
    s.expect(r'-> SOUND ENGINE value=11 name="NOISE" status="OK"',
             "SOUND ENGINE −（ANALOG 环绕到 NOISE）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=11 preset=0 level=", "ENG SNAP 轨 0 = NOISE 预置 0")
    s.cmd("TAP %d %d" % sound_row_xy(0, +1))
    s.expect(r'-> SOUND ENGINE value=0 name="ANALOG" status="OK"',
             "SOUND ENGINE +（NOISE 环绕回 ANALOG）")
    s.cmd("TAP %d %d" % sound_row_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=1 name="808 DIRTY" status="OK"', "SOUND PRESET + → 预置 1")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 preset=1 level=", "ENG SNAP 轨 0 预置 = 1")
    s.cmd("TAP %d %d" % sound_row_xy(1, -1))
    s.cmd("TAP %d %d" % sound_row_xy(1, -1))
    s.expect(r'-> SOUND PRESET value=21 name="SINE PAD" status="OK"',
             "SOUND PRESET −（预置 0 环绕到 21）")
    s.cmd("TAP %d %d" % sound_row_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=0 name="808 BOOM" status="OK"', "SOUND PRESET + 回预置 0")

    img = s.capture("params-sound")
    s.probe(img, "组签选中（EDIT）", *C_TAB_EDIT, C_YELLOW)
    s.probe(img, "子签槽 0 为空（EDIT 单页组）", *C_TAB_SUB_EMPTY, C_PANEL, 10)
    # 页眉两行（ENGINE / PRESET 三区对象）：行底色、名字列强调色、上下文行灰字
    s.probe(img, "ENGINE 行底色", 16 + 3, SOUND_ROW_Y + 4, C_PANEL_HI, 10)
    s.probe(img, "PRESET 行底色", 320 + 3, SOUND_ROW_Y + 4, C_PANEL_HI, 10)
    s.probe(img, "页眉行上方 2px 仍是页面背景", 16 + 3, SOUND_ROW_Y - 2, C_BG, 10)
    s.probe_region(img, "ENGINE 行名字列（ANALOG，UI_ACCENT）",
                   (16 + 130, SOUND_ROW_Y + 4, 16 + 288 - 54, SOUND_ROW_Y + 32), C_YELLOW, 24, 12)
    s.probe_region(img, "PRESET 行名字列（808 BOOM，UI_ACCENT）",
                   (320 + 130, SOUND_ROW_Y + 4, 320 + 384 - 54, SOUND_ROW_Y + 32), C_YELLOW, 24, 12)
    s.probe_region(img, "上下文行（SOUND · TRACK 1，灰字）",
                   (720, SOUND_ROW_Y + 4, 1080, SOUND_ROW_Y + 32), C_TEXT_DIM, 32, 12)
    # A10：非 FM6 轨没有 bank 行（仪表盘页上行缺席 = 卡片视口盖住那一带，见 bank_row_absent_probes）
    bank_row_absent_probes(s, img, "SOUND（ANALOG 轨）")

    # ---------- 鼓轨：页眉换成单行 KIT（写 P_E0）、没有 PRESET 行、bank 行隐藏 ----------
    select_track(s, 3)
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "UI sound（轨 3 = 鼓轨）")
    s.cmd("PARAM 63")
    kit0 = param_value(s)
    kit_max = param_value(s, "max")
    (s.ok if kit_max == 45 else s.fail)(f"鼓轨 P_E0 的描述符 = DRUM_KIT_DESC（max={kit_max}，期望 45）")
    want = kit0 + 1 if kit0 < kit_max else 0
    s.cmd("TAP %d %d" % sound_row_xy(0, +1))
    s.expect(rf'-> SOUND KIT value={want} name="[A-Z0-9+ ]+" status="OK"',
             "鼓轨 KIT + 写 P_E0（一条 CMD_SET_PARAM）")
    param_expect_value(s, "PARAM 63", want, "PARAM 63（KIT +1 回读）")
    s.cmd("TAP %d %d" % sound_row_xy(0, -1))
    param_expect_value(s, "PARAM 63", kit0, "PARAM 63（KIT 回基线）")
    for d in (-1, +1):
        s.cmd("TAP %d %d" % sound_row_xy(1, d))
        s.expect(r"TAP miss", f"鼓轨没有 PRESET 行：其 {'+' if d > 0 else '−'} 区 TAP miss")
    img = s.capture("params-sound-drum")
    bank_row_absent_probes(s, img, "鼓轨 SOUND")
    s.probe_region(img, "鼓轨页眉红字（SOUND · TRACK 4 (DRUM) — NO SOUND）",
                   (320, SOUND_ROW_Y + 4, 1080, SOUND_ROW_Y + 32), C_ERROR, 16, 200)
    s.probe(img, "KIT 行底色（row 0 仍在）", 16 + 3, SOUND_ROW_Y + 4, C_PANEL_HI, 10)
    s.probe(img, "PRESET 行矩形左上角是页面背景（行缺席）", 320 + 3, SOUND_ROW_Y + 4, C_BG, 10)

    # ---------- 页眉 ENGINE / PRESET ±（轨 1）：环绕、ENG SNAP 真值、FM6 才出现的 bank 行 ----------
    select_track(s, 1)
    s.cmd("ENG TRACK 1 9 0")
    s.expect(r"ENG TRACK k=1 engine=9 preset=0 OK", "归一轨 1 = FM6 预置 0")
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "UI sound（轨 1）")
    s.cmd("TAP %d %d" % sound_row_xy(0, -1))
    s.expect(r'-> SOUND ENGINE value=8 name="GRAIN" status="OK"',
             "SOUND ENGINE −（FM6 → GRAIN；预置回 0）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=1 engine=8 preset=0 level=", "ENG SNAP 轨 1 = GRAIN 预置 0")
    img = s.capture("params-sound-grain")
    bank_row_absent_probes(s, img, "GRAIN 轨 SOUND")
    s.cmd("TAP %d %d" % sound_row_xy(0, +1))
    s.expect(r'-> SOUND ENGINE value=9 name="FM6" status="OK"', "SOUND ENGINE +（GRAIN → FM6）")
    s.cmd("TAP %d %d" % bank_slot_btn_xy(-1))
    s.expect(r"-> BANK slot=26 delta=-1", "FM6 轨：bank 行出现（槽 − 命中：B1 → B27，环绕）")
    s.cmd("TAP %d %d" % bank_slot_btn_xy(+1))
    s.expect(r"-> BANK slot=0 delta=\+1", "FM6 轨：bank 槽 + 回 B1")
    s.cmd("TAP %d %d" % sound_row_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=1 name="[A-Z0-9 ]+" status="OK"', "SOUND PRESET + → 预置 1（FM6）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=1 engine=9 preset=1 level=", "ENG SNAP 轨 1 预置 = 1")
    s.cmd("TAP %d %d" % sound_row_xy(1, -1))
    s.cmd("TAP %d %d" % sound_row_xy(1, -1))
    s.expect(r'-> SOUND PRESET value=7 name="[A-Z0-9 ]+" status="OK"',
             "SOUND PRESET −（FM6 预置 0 环绕到 7）")
    s.cmd("TAP %d %d" % sound_row_xy(1, +1))
    s.expect(r'-> SOUND PRESET value=0 name="[A-Z0-9 ]+" status="OK"', "SOUND PRESET + 回预置 0")
    # bank 行在仪表盘页挪到页眉下方（BANK_ROW_Y）：SAVE 绿 / ERASE 橙
    img = s.capture("params-sound-bank")
    s.probe(img, "BANK 行 SAVE 按钮底（绿）", *bank_btn_probe(0), C_GREEN)
    s.probe(img, "BANK 行 ERASE 按钮底（橙）", *bank_btn_probe(2), C_ORANGE)
    # bank 行 SAVE / ERASE 往返（槽 0；SAVE 的字节与 console 的 LIVE 一致）
    s.cmd("STORE BANK ERASE 0")
    s.expect(r"(^BANK 0 ERASED\r?$)|(^STORE ERR empty\r?$)", "归一 bank 槽 0（空）")
    s.cmd("TAP %d %d" % bank_act_xy(0))
    s.expect(r'-> BANK slot=0 action=save status="SAVE OK"', "FM6 轨 TAP BANK SAVE → SAVE OK")
    s.cmd("STORE BANK GET 0")
    got = s.last.split("BANK 0 GET ")[1].split()[0].strip()
    s.cmd("STORE BANK LIVE")
    live = s.last.split("BANK LIVE ")[1].split()[0].strip()
    (s.ok if got == live and len(live) == 256 else s.fail)(
        f"TAP SAVE 后 GET 0 == LIVE（{len(live)} hex）")
    s.cmd("TAP %d %d" % bank_act_xy(2))
    s.expect(r'-> BANK slot=0 action=erase status="ERASE OK"', "FM6 轨 TAP BANK ERASE → ERASE OK")
    store_cmd(s, "STORE BANK GET 0", r"^STORE ERR empty\r?$", "TAP ERASE 后 GET 0 → empty")

    # ---------- SAMPLE 页（SYNTH 组首个子页；ENGINES[4]->edit[0..7]，空槽 5/7 不建旋钮） ----------
    s.cmd("ENG TRACK 1 4 0")
    s.expect(r"ENG TRACK k=1 engine=4 preset=0 OK", "ENG TRACK 轨 1 → SAMPLE")
    s.cmd("UI sample")
    s.expect(r"UI page=sample \(8\)", "UI sample（SAMPLE 页解锁；SYNTH 组）")
    mem_synth0 = param_mem(s)
    (s.ok if mem_synth0["groups"]["SYNTH"]["built"] == 1
     and mem_synth0["head"]["group"] == "SYNTH" else s.fail)(
        f"UI MEM 当前组 SYNTH built={mem_synth0['groups']['SYNTH']['built']} "
        f"group={mem_synth0['head']['group']}（期望 1 / SYNTH；SAMPLE 已不在 EDIT）")
    s.cmd("ENG DESC engine 4 0")
    s.expect(r'label="SET" fmt=\d+ min=0 max=11 def=0 unit="[^"]*" enums=12 first="PIANO" last="USR4"',
             "ENG DESC SAMPLE SET：12 名（PIANO..USR4）")
    s.cmd("PARAM 63 0")  # SET=0：环顶探针不被指示弧盖住（恢复的工程可能 SET>0）
    s.expect(r"PARAM track=1 id=63 value=0 applied=0", "归一 SAMPLE SET=0（环顶探针前提）")
    s.cmd("PARAM 63")
    set0 = param_value(s)
    s.cmd("PARAM 66")
    loop0 = param_value(s)
    s.cmd("PARAM 67")
    cut0 = param_value(s)
    s.ser.note(f"SAMPLE 基线 set={set0} loop={loop0} cut={cut0}")

    # 槽管理行（P4-D Task 8）：点槽 = 把 SET 宏切到 USR1（8）；ERASE 只在 SET ∈ 8..11
    # 时下发。几何由 SAMPLE_SLOT_*/SAMPLE_ERASE_X 派生（与 page_params.c 同名宏）。

    s.cmd("TAP %d %d" % sample_slot_xy(0))
    s.expect(r"-> SAMPLE slot=0 select", "TAP 槽 U1 → SET 宏切到 USR1")
    param_expect_value(s, "PARAM 63", 8, "PARAM 63（SET=8 = USR1）")
    s.cmd("TAP %d %d" % sample_erase_xy())
    s.expect(r'-> SAMPLE slot=0 action=erase status="ERASE OK"',
             "TAP ERASE（USR1 空槽擦除 → ERASE OK）")
    # 状态行「SET U1」= 选中槽文本（绿）；槽 U1 标签文字用 UI_ACCENT（选中色）
    img = s.capture("params-sample")
    s.probe(img, "组签选中（SYNTH）", *C_TAB_SYNTH, C_YELLOW)
    s.probe(img, "子签选中（SAMPLE）", *C_TAB_SAMPLE, C_YELLOW)
    s.probe(img, "子签未选中（DSYN）", *C_TAB_DSYN, C_PANEL_HI, 10)
    # SET=8（USR1）时旋钮 0 的指示弧跨过 12 点方向，环顶探针在改值态（SET=1）另做
    # 空槽 5/7 没有旋钮：其环顶像素应是页面背景（UI_BG）
    s.probe(img, "空槽 5（行 1 列 1）无旋钮", 482, 454, C_BG, 12)
    s.probe(img, "空槽 7（行 1 列 3）无旋钮", 1114, 454, C_BG, 12)
    s.probe_region(img, "槽 U1 标签（选中 = UI_ACCENT）", SAMPLE_SLOT_BOX, C_YELLOW, 24, 30)
    s.probe_region(img, "状态行（SET U1 = 轨 1 绿）", SAMPLE_STATUS_BOX, C_GREEN, 24, 30)
    s.cmd("TAP %d %d" % sample_slot_xy(0))  # 重复点槽：幂等（重发 SET=8）
    s.expect(r"-> SAMPLE slot=0 select", "TAP 槽 U1 幂等")
    mem_sample = param_mem(s)
    st, est, delta = mem_sample["pages"].get(8, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est else s.fail)(
        f"UI MEM SAMPLE state={st} delta={delta} est={est}（0<delta≤est）")
    # 槽点击把 SET 写成 8：环顶/± 的下一段按基线 set0 断言，先还原
    s.cmd(f"PARAM 63 {set0}")
    s.expect(rf"PARAM track=1 id=63 value={set0} applied={set0}", "SAMPLE SET 回基线（槽行测后）")

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

    # 非 USR 选择（SET=0 PIANO）：ERASE 由页面本地拒绝（`NOT USR`，不下发命令）
    s.cmd("PARAM 63 0")
    s.expect(r"PARAM track=1 id=63 value=0 applied=0", "SAMPLE SET=0（非 USR 选择）")
    s.cmd("TAP %d %d" % sample_erase_xy())
    s.expect(r'-> SAMPLE slot=- action=erase status="NOT USR"',
             "TAP ERASE（SET 不在 USR1..USR4 → NOT USR、无命令）")
    param_expect_value(s, "PARAM 63", 0, "PARAM 63（NOT USR 拒绝不改 SET）")

    # 锁定页（选中轨引擎 ≠ SAMPLE）：槽点击/ERASE/KNOB 全部拒绝，且保留 `NOT USR`
    s.cmd("ENG TRACK 1 0 0")
    s.expect(r"ENG TRACK k=1 engine=0 preset=0 OK", "ENG TRACK 轨 1 → ANALOG（SAMPLE 页锁定）")
    s.cmd("TAP %d %d" % sample_slot_xy(0))
    s.expect(r'-> SAMPLE slot=- select status="NOT USR"',
             "锁定页 TAP 槽 → SAMPLE slot=- select status=NOT USR（未下发命令）")
    s.cmd("TAP %d %d" % sample_erase_xy())
    s.expect(r'-> SAMPLE slot=- action=erase status="NOT USR"',
             "锁定页 TAP ERASE → NOT USR")
    knob_locked(s, 0, "press", 0, -73)
    s.cmd("ENG TRACK 1 4 0")
    s.expect(r"ENG TRACK k=1 engine=4 preset=0 OK", "ENG TRACK 轨 1 → SAMPLE（解锁）")

    # 改值态截屏：SET=1（枚举名 "BASS"）
    s.cmd("PARAM 63 1")
    s.expect(r"PARAM track=1 id=63 value=1 applied=1", "PARAM 63 1（SAMPLE SET 改值态）")
    img = s.capture("params-sample-set")
    s.probe(img, "页签选中（SAMPLE，改值态）", *C_TAB_SAMPLE, C_YELLOW)
    s.probe(img, "旋钮 0 弧线底（改值态）", 166, 158, C_PANEL_HI, 10)
    s.probe_region(img, "旋钮 0 数值文本（SET 枚举名）", (100, 214, 232, 246), C_TEXT_DIM, 16, 30)
    s.cmd(f"PARAM 63 {set0}")
    s.expect(rf"PARAM track=1 id=63 value={set0} applied={set0}", "SAMPLE SET 回基线")

    # 复位引擎/轨：轨 1 回 ANALOG（共享 P_E* 槽位先归零）；当前页仍在 SYNTH 组
    for pid in (63, 64, 66, 67):
        s.cmd(f"PARAM {pid} 0")
    s.cmd("ENG TRACK 1 0 0")
    s.expect(r"ENG TRACK k=1 engine=0 preset=0 OK", "ENG TRACK 轨 1 → ANALOG（复位）")

    # ---------- DSYN 页（SYNTH 第 2 张子签；与选中轨无关，无锁定态） ----------
    s.cmd("UI dsyn")
    s.expect(r"UI page=dsyn \(12\)", "UI dsyn（SYNTH 第 2 张子签）")
    mem_dsyn = param_mem(s)
    st, est, delta = mem_dsyn["pages"].get(12, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est else s.fail)(
        f"UI MEM DSYN state={st} delta={delta} est={est}（0<delta≤est）")
    s.cmd("UI DSYN KIT 0")
    s.expect(r"UI DSYN kit=0 lane=\d+ page=\d+", "UI DSYN KIT 0")
    s.cmd("UI DSYN LANE 0")
    s.expect(r"UI DSYN kit=0 lane=0 page=\d+", "UI DSYN LANE 0")
    s.cmd("UI DSYN PAGE 1")
    s.expect(r"UI DSYN kit=0 lane=0 page=1", "UI DSYN PAGE 1（SND2）")
    s.cmd("STATE SNAP")
    s.expect_int(r"dsyn_kit=(\d+)", 0, "STATE SNAP dsyn_kit（与页面选择同源）")
    s.expect_int(r"dsyn_lane=(\d+)", 0, "STATE SNAP dsyn_lane（与页面选择同源）")
    s.cmd("TAP %d %d" % dsyn_page_chip_xy(2))   # 字段页 chip SND3
    s.expect(r"-> DSYN kit=0 lane=0 page=2 select", "TAP 字段页 chip SND3 → page=2")
    lane_mid = dsyn_lane_mid_xy()
    s.cmd("TAP %d %d" % lane_mid)                # LANE 三区对象的中段（纯标签）
    s.expect(rf"TAP miss {lane_mid[0]},{lane_mid[1]} page=12",
             "DSYN LANE 中段（标签）→ TAP miss")
    s.cmd("TAP %d %d" % dsyn_audition_xy())
    s.expect(r"-> DSYN AUDITION lane=0", "TAP AUDITION（试听队列）")
    s.cmd("UI DSYN PAGE 0")
    s.expect(r"UI DSYN kit=0 lane=0 page=0", "UI DSYN PAGE 0（SND1）")

    # ---------- FM6P 页（SYNTH 第 3 张子签）：锁定页拒绝（选中轨不是 FM6） ----------
    s.cmd("UI fm6p")
    s.expect(r"UI page=fm6p \(13\)", "UI fm6p（SYNTH 第 3 张子签）")
    mem_fm6p = param_mem(s)
    st, est, delta = mem_fm6p["pages"].get(13, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est else s.fail)(
        f"UI MEM FM6P state={st} delta={delta} est={est}（0<delta≤est）")
    s.cmd("UI FM6P OP 0")
    s.expect(r"UI FM6P op=0 page=\d+", "UI FM6P OP 0（锁定页也可写页内量）")
    s.cmd("UI FM6P PAGE 0")
    s.expect(r"UI FM6P op=0 page=0", "UI FM6P PAGE 0（EG）")
    s.cmd("STATE SNAP")
    s.expect_int(r"fm6p_engine_ok=(\d+)", 0, "STATE SNAP fm6p_engine_ok=0（轨 1 = ANALOG）")
    s.cmd("TAP %d %d" % fm6p_op_chip_xy(1))      # OP2 chip
    s.expect(r"-> FM6P op=0 page=0 select", "锁定页 TAP OP2 → 选择不变（op=0）")
    s.cmd("TAP %d %d" % fm6p_page_chip_xy(2))    # PAGE PITCH chip
    s.expect(r"-> FM6P op=0 page=0 select", "锁定页 TAP PAGE PITCH → 字段页不变（page=0）")
    s.cmd("TAP %d %d" % fm6p_bank_xy(-1))
    s.expect(r"-> FM6P BANK slot=0 select", "锁定页 TAP BANK − → 槽号不变（slot=0）")
    s.cmd("TAP %d %d" % fm6p_bank_xy(+1))
    s.expect(r"-> FM6P BANK slot=0 select", "锁定页 TAP BANK + → 槽号不变（slot=0）")
    s.cmd("TAP %d %d" % fm6p_save_xy())
    s.expect(r'-> FM6P SAVE ERR locked status="NOT FM6"',
             "锁定页 TAP SAVE → ERR locked / NOT FM6（未下发 CMD_BANK_PUT）")
    s.cmd("TAP %d %d" % fm6p_adj_xy(False))      # 底部三区 − 区
    s.expect(r'-> FM6P \S+ value=\d+ ERR locked status="NOT FM6"',
             "锁定页 TAP 底部 − → 字段写被拒（写前真值）")
    s.cmd("TAP %d %d" % fm6p_adj_xy(True))       # 底部三区 + 区
    s.expect(r'-> FM6P \S+ value=\d+ ERR locked status="NOT FM6"',
             "锁定页 TAP 底部 + → 字段写被拒（写前真值）")
    s.cmd("TAP %d %d" % fm6p_adj_dead_xy())      # 中段 24px 死区
    s.expect(rf"TAP miss {fm6p_adj_dead_xy()[0]},{fm6p_adj_dead_xy()[1]} page=13",
             "锁定页 TAP 底部中段（死区）→ TAP miss")
    img = s.capture("params-fm6p-locked")
    s.probe(img, "组签选中（SYNTH）", *C_TAB_SYNTH, C_YELLOW)
    s.probe(img, "子签选中（FM6P）", *C_TAB_FM6P, C_YELLOW)
    s.probe(img, "子签未选中（SAMPLE）", *C_TAB_SAMPLE, C_PANEL_HI, 10)
    s.probe_region(img, "锁定页状态行红字（NOT FM6）", FM6P_STATUS_BOX, C_ERROR, 16, 40)

    # ---------- SYNTH 组 Σ（三页全建：built=3 / sum_est 20,480） ----------
    mem_synth = param_mem(s)
    gs = mem_synth["groups"]["SYNTH"]
    (s.ok if gs["built"] == 3 else s.fail)(
        f"UI MEM SYNTH built={gs['built']}（期望 3 = SAMPLE/DSYN/FM6P）")
    (s.ok if gs["sum_est"] == PARAM_SYNTH_SUM_EST else s.fail)(
        f"UI MEM SYNTH sum_est={gs['sum_est']}"
        f"（期望 {PARAM_SYNTH_SUM_EST:,} = 6144 + 7168 + 7168）")
    (s.ok if 0 < gs["sum_delta"] <= gs["sum_est"] else s.fail)(
        f"UI MEM SYNTH sum_delta={gs['sum_delta']}（0 < Σdelta ≤ Σest）")
    (s.ok if 0 < gs["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"UI MEM SYNTH build_ms={gs['build_ms']}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")

    # ---------- 池水位：组作用域（ADR-005 §3/§5；DI-19 收口口径） ----------
    # 中途 select_track(1) 走过 UI tracks（跨组）⇒ EDIT 的 SOUND 已被销毁；进 SYNTH
    # （SAMPLE/DSYN/FM6P）时 EDIT 组全部销毁。离开 SYNTH 后 built 必须回 0（含原当前页）；
    # 每页 delta ≤ est 已各自断言。
    select_track(s, 0)
    mem_left = param_mem(s)
    (s.ok if mem_left["groups"]["SYNTH"]["built"] == 0
     and all(mem_left["pages"].get(p, ("?",))[0] == "unbuilt" for p in (8, 12, 13))
     else s.fail)(
        f"UI MEM 离开 SYNTH 后 built={mem_left['groups']['SYNTH']['built']} "
        + " ".join(f"{p}={mem_left['pages'].get(p, ('?',))[0]}" for p in (8, 12, 13))
        + "（期望 0 / unbuilt ×3）")

    # 逐组访问：同一时刻只保留**当前组**的页面（组作用域生命周期）。
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "池水位：UI play")
    mem_play = param_mem(s)
    (s.ok if mem_play["head"]["built"] == 1 and mem_play["groups"]["PLAY"]["built"] == 1
     else s.fail)(
        f"UI MEM PLAY 组 built={mem_play['groups']['PLAY']['built']} 总 built="
        f"{mem_play['head']['built']}（期望 1 / 1）")
    s.cmd("UI transport")
    s.cmd("UI step")
    s.cmd("UI tools")
    mem_mix = param_mem(s)
    gm = mem_mix["groups"]
    (s.ok if mem_mix["head"]["built"] == 1 and gm["MIX"]["built"] == 1
     and gm["MIX"]["sum_est"] == PARAM_MIX_SUM_EST else s.fail)(
        f"UI MEM built={mem_mix['head']['built']} / MIX built={gm['MIX']['built']} "
        f"sum_est={gm['MIX']['sum_est']}（期望 1 / 1 / {PARAM_MIX_SUM_EST:,}：5B-1 起 MIX 只有 "
        f"TOOLS；PLAY/SEQ 已随跨组销毁）")
    s.cmd("UI sound")
    mem3 = param_mem(s)
    g3 = mem3["groups"]
    (s.ok if mem3["head"]["built"] == 1 and g3["EDIT"]["built"] == 1 else s.fail)(
        f"UI MEM built={mem3['head']['built']} / EDIT built={g3['EDIT']['built']}"
        f"（期望 1 / 1：5B-1 起只有当前组 EDIT 的 SOUND 常驻）")
    (s.ok if all(g3[g]["built"] == 0 for g in ("PLAY", "TRACKS", "MIX", "SYNTH", "SEQ", "SONG",
                                               "FILE")) else s.fail)(
        "UI MEM 非当前组 built=0（"
        + " ".join(f"{g}={g3[g]['built']}" for g in ("PLAY", "TRACKS", "MIX", "SYNTH", "SEQ", "SONG",
                                                    "FILE")) + "）")
    # 组作用域后的池余量必须显著高于九页平铺的 2.2–2.7KB（DI-19 的池悬崖），
    # 取 ≥20KB 作为回归下界：base 重测后见证据文件。
    (s.ok if mem3["head"]["free"] >= 20480 else s.fail)(
        f"UI MEM pool_free={mem3['head']['free']}（组作用域后 ≥20KB，DI-19 收口）")
    # EDIT Σ est = SOUND 8,704（卡片仪表盘 + 页眉两行 + 常驻隐藏的 bank 行）
    (s.ok if g3["EDIT"]["sum_est"] == PARAM_EDIT_SUM_EST else s.fail)(
        f"UI MEM EDIT sum_est={g3['EDIT']['sum_est']}（期望 {PARAM_EDIT_SUM_EST:,}）")
    (s.ok if 0 < g3["EDIT"]["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"UI MEM EDIT build_ms={g3['EDIT']['build_ms']}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")

    # 有序对峰值（ADR-005 §5）：源组全建 → 跨组首建目标页，构建日志的池剩余 ≥ 4,096
    # （EDIT/MIX 自 5B-1 起是单页组；全矩阵见 nav ⑩）。
    for label, src, dst in (("EDIT 全建 → STEP", ("sound",), "step"),
                            ("MIX 全建 → STEP", ("tools",), "step"),
                            ("SEQ 全建 → MIX（TOOLS）", ("step", "transport"), "tools")):
        for p in src:
            s.cmd(f"UI {p}")
        s.cmd(f"UI {dst}")
        m = re.search(r"池剩余=(\d+)B", s.last)
        free = int(m.group(1)) if m else -1
        (s.ok if free >= 4096 else s.fail)(
            f"有序对 {label}：构建日志 池剩余={free}B（≥4,096）")
    # LVGL 任务栈余量（Task 5 评审 R10）：高水位是累计最小值，覆盖到此为止的 30ms 拍、
    # TAB/切页构建与渲染（控件回调跑在同一任务，但 TAP 注入在 console 任务，量不到）。
    lv = param_mem(s)["head"]["lvgl_stack_free"]
    s.ser.note(f"UI MEM lvgl_stack_free={lv}B（SOUND/SYNTH 段之后；门禁 ≥{LVGL_STACK_FLOOR}）")
    (s.ok if lv >= LVGL_STACK_FLOOR else s.fail)(
        f"LVGL 任务栈最小余量 lvgl_stack_free={lv}B（≥{LVGL_STACK_FLOOR}）")

    # P4-F Task 6：TOOLS 页 / TRANSPORT 的 REC 配置行 / STEP 页键盘参数锁与 fill 条件
    param_rec_config(s)
    param_step_keys(s)
    param_tools(s)
    # 段落之后的 TOOLS 池水位（上面的有序对已建过一次；这里在 MIX 组内复核 Σ est）
    s.cmd("UI tools")
    mem_t = param_mem(s)
    gt = mem_t["groups"]["MIX"]
    tt = mem_t["pages"][18]
    (s.ok if gt["built"] == 1 and gt["sum_est"] == PARAM_MIX_SUM_EST and tt[0] == "built"
     and tt[1] == TOOLS_EST and 0 < tt[2] <= TOOLS_EST
     and 0 < gt["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑪ UI MEM TOOLS state={tt[0]} est={tt[1]} delta={tt[2]} / MIX built={gt['built']} "
        f"sum_est={gt['sum_est']} build_ms={gt['build_ms']}（期望 built / {TOOLS_EST} / "
        f"0<delta≤est / 1 / {PARAM_MIX_SUM_EST:,} / ≤{NAV_BUILD_BUDGET_MS}）")

    # FILE 页不是参数页：`UI KNOB` 必须回文档化的 UI ERR not_params（Task 14 终审：
    # 旧判定只排除 page < GLOBAL，PROJECTS(9)/PRESETS(10) 会掉进旋钮路径）。
    s.cmd("UI projects")
    s.expect(r"UI page=projects \(9\)", "⑨ UI KNOB on FILE：UI projects")
    s.cmd("UI KNOB 0 press 0 0")
    s.expect(r"^UI ERR not_params\r?$", "⑨ UI KNOB on FILE → UI ERR not_params")

    # 全场景收尾再读一次：高水位只降不升，这里是整个 params 的最小余量
    lv_end = param_mem(s)["head"]["lvgl_stack_free"]
    s.ser.note(f"UI MEM lvgl_stack_free={lv_end}B（params 全场景收尾）")
    (s.ok if lv_end >= LVGL_STACK_FLOOR else s.fail)(
        f"LVGL 任务栈最小余量（全场景）lvgl_stack_free={lv_end}B（≥{LVGL_STACK_FLOOR}）")

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


def proj_erase(s: Session, slot: int, label: str) -> None:
    """`STORE PROJ ERASE n`：空槽现在回 `STORE ERR empty`（上游 `EMPTY SECTION`：槽本来就空，
    不写 flash），非空才 `PROJ n ERASED`。两种回显都表示「结束时该槽为空」——调用方仍用
    LIST/FIELDS 断言终态。"""
    s.cmd(f"STORE PROJ ERASE {slot}")
    if re.search(rf"^PROJ {slot} ERASED\r?$", s.last, re.MULTILINE):
        s.ok(label)
        return
    if re.search(r"^STORE ERR empty\r?$", s.last, re.MULTILINE):
        s.ok(label + "（空槽：上游 EMPTY SECTION，未写 flash）")
        return
    raise InjectError(f"{label}: 回显缺少 ERASED/STORE ERR empty（响应 {s.last.strip()!r}）")


def proj_slot_state(s: Session, slot: int) -> int:
    """`STORE PROJ HASH <slot>`：空槽回 -1，否则 RAM 槽的哈希。"""
    s.cmd(f"STORE PROJ HASH {slot}")
    if re.search(r"^STORE ERR empty\r?$", s.last, re.MULTILINE):
        return -1
    m = re.search(rf"^PROJ {slot} HASH=([0-9A-F]{{8}})\r?$", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"STORE PROJ HASH {slot}: 回显缺少哈希（响应 {s.last.strip()!r}）")
    return int(m.group(1), 16)


def session_once(s: Session, key: str, produce, tag: str):
    """跨 ScenarioRestart **只求值一次**的快照：首轮调用执行 produce() 并记在 Session 上，
    截屏复位后的重跑直接回第一轮的值（重跑时设备已复位，此刻读到的可能已是场景改过的
    状态）。"""
    if key not in s.snapshots:
        s.snapshots[key] = produce()
    else:
        s.ser.note(f"{tag} 重跑：沿用第一轮的快照")
    return s.snapshots[key]


def proj_snapshot_open(s: Session, name: str, slot: int, tag: str) -> tuple:
    """工作工程快照的**开括号**：把当前工作工程存进诊断槽，返回 (hash0, 槽入口是否为空)。

    **基线跨 ScenarioRestart 保持**（终审 C-I1）：截屏传输失败 ⇒ 设备复位 ⇒ 场景从头重跑，
    本函数被第二次调到。复位不擦 flash——槽里已经是第一轮的快照；旧实现在重跑时看到
    「槽非空」就再 SAVE 一次，快照被复位后的状态覆盖，收口 LOAD 还原的不再是入口的工作
    工程，而场景照样 exit 0。现在：基线记在 Session 上，重跑**不再 SAVE**，只核对槽里仍是
    快照（`STORE PROJ HASH`）。
    - 相等 ⇒ 沿用第一轮的基线；
    - 入口为空且槽现在仍空 ⇒ 第一轮的 SAVE 根本没发生，补存一次不覆盖任何东西；
    - 其它（槽被改写 / 第一轮 SAVE 未落盘而槽里是别的工程）⇒ InjectError 终止场景（非零
      退出），不再拿一个来历不明的槽去「还原」。
    """
    base = s.snapshots.get(name)
    if base is not None:
        hash0, was_empty = base
        cur = proj_slot_state(s, slot)
        if cur == hash0:
            s.ok(f"{tag} 重跑：诊断槽 {slot} 仍是第一轮的快照 hash={hash0:08X}（不再 SAVE）")
            return hash0, was_empty
        if was_empty and cur < 0:
            hash0 = live_hash(s)
            store_cmd(s, f"STORE PROJ SAVE {slot}", rf"^PROJ {slot} SAVED hash={hash0:08X}\r?$",
                      f"{tag} 重跑：第一轮的快照未落盘，补存")
            s.snapshots[name] = (hash0, True)
            return hash0, True
        got = "empty" if cur < 0 else f"hash={cur:08X}"
        raise InjectError(
            f"{tag} 重跑后诊断槽 {slot} 不是工作工程快照（槽 {got}，快照 hash={hash0:08X}）："
            f"快照已被改写，无法还原工作工程——终止场景。设备 RAM 里的工作工程可能停在场景"
            f"中途态；若期间没有落进 autosave 窗口，硬复位即回到最近一次 autosave 副本")
    empty = proj_slot_state(s, slot) < 0
    if not empty:
        s.ser.note(f"{tag} 诊断槽 {slot} 入口非空（用户工程？）：被工作工程快照覆盖，收口不擦")
    hash0 = live_hash(s)
    s.snapshots[name] = (hash0, empty)
    store_cmd(s, f"STORE PROJ SAVE {slot}", rf"^PROJ {slot} SAVED hash={hash0:08X}\r?$",
              f"{tag} 工作工程快照 → 诊断槽 {slot}（hash = LIVEHASH）")
    return hash0, empty


def proj_snapshot_close(s: Session, name: str, slot: int, tag: str, erase=None) -> None:
    """工作工程快照的**闭括号**（与 proj_snapshot_open 成对）：槽必须仍是快照（否则
    InjectError 终止）→ LOAD → LIVEHASH 逐位等于快照（断言）→ 擦槽。
    erase：None = 入口为空才擦（非空入口的槽留着快照，step_edit ②g / transport ⑩g）；
    True = 无条件擦（调用方之后还要用这个槽或断言它为空，params ⑪）。"""
    hash0, was_empty = s.snapshots[name]
    cur = proj_slot_state(s, slot)
    if cur != hash0:
        got = "empty" if cur < 0 else f"hash={cur:08X}"
        raise InjectError(
            f"{tag} 收口：诊断槽 {slot} 不是工作工程快照（槽 {got}，快照 hash={hash0:08X}）——"
            f"不 LOAD 来历不明的内容，终止场景")
    store_cmd(s, f"STORE PROJ LOAD {slot}", rf"^PROJ {slot} LOADED hash={hash0:08X}\r?$",
              f"{tag} 收口 LOAD {slot}（工作工程快照）")
    hash1 = live_hash(s)
    (s.ok if hash1 == hash0 else s.fail)(
        f"{tag} 收口后 LIVEHASH {hash1:08X} == 入口 {hash0:08X}（工作工程逐位还原）")
    if erase or (erase is None and was_empty):
        proj_erase(s, slot, f"{tag} 收口擦诊断槽 {slot}")
    del s.snapshots[name]


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
ENG_P_MUTE = 40   # P_MUTE：静音（⑪d 用，走上游 trk_silent() 的可听路径）
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

    ⑩ 的起点来自开机恢复的 autosave（原 DI-23，已按此思路收口）：选中轨的 `P_MUTE` 与
    轨 1 的步表是 `param_kept`/工程的一部分，人工改过设备或在别的注入场景之后直接跑会让
    ④ 的 `voices≥1` 与 ⑤ 的绝对 `steps=1` 假红。故本段①显式写 `P_MUTE=0`（④ 的发声
    前提），⑤ 先把轨 1 的步 5 清空、再保证一个非 5 的步，foreign 的步数期望 = 同轮
    `fmt1` 捕获的轨 1 步数 + 1（只多出 diagnostic 自己放的步 5）。
    """
    s.ser.note("⑩ 换引擎/预置 = 换声音：ENG PRESET 期望向量 vs ENG TRACK 应用后的 p[]")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⑩ 归一 engine=0 preset=0（ANALOG）")
    # 原 DI-23：P_MUTE 是 param_kept（换引擎/预置都不覆盖），autosave 恢复后可能仍置位；
    # ④ 的「按住键 ⇒ voices≥1」需要选中轨未静音。显式清零，不依赖历史。
    s.cmd("PARAM 40 0")
    s.expect(r"PARAM track=0 id=40 value=0 applied=0", "⑩ 归一 P_MUTE=0（原 DI-23：④ 的发声前提）")

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
    # 场景假设（原 DI-23）：⑩ 的起点来自 autosave 恢复的历史，轨 1 的图案步数不可假定；
    # 且 `STORE PROJ SYNTH foreign` 自己会在轨 1 的**步 5** 补一个音（diagnostic 的构造），
    # fmt1 不碰轨 1。故先把「步 5 空 + 另有一个非 5 的步」造好：foreign 的步数必须恰为
    # fmt1 同轮捕获的轨 1 步数 +1（图案保留 + 只多了诊断自己放的那个音）。
    s.ser.note("⑩e0 轨 1 图案归一：步 5 空、步 6 有音（foreign 的 +1 步可预测）")
    select_track(s, 1)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩e0 UI step（轨 1）")
    ensure_window(s, 0, 16)
    if step_fields(s, 1, 5)[0] != 0:
        x, y = step_cell_xy(5, 1)
        s.cmd(f"TAP {x} {y}")
        s.expect(r"-> STEP track=1 step=5 CMD_TOGGLE_STEP", "⑩e0 清空轨 1 步 5")
    (s.ok if step_fields(s, 1, 5)[0] == 0 else s.fail)("⑩e0 轨 1 步 5 为空（foreign 的 +1 可预测）")
    if step_fields(s, 1, 6)[0] == 0:
        x, y = step_cell_xy(6, 1)
        s.cmd(f"TAP {x} {y}")
        s.expect(r"-> STEP track=1 step=6 CMD_TOGGLE_STEP", "⑩e0 建轨 1 步 6（图案判别力）")
    (s.ok if step_fields(s, 1, 6)[0] != 0 else s.fail)(
        "⑩e0 轨 1 步 6 有音（步数 ≥1，清空型错误会被抓到）")
    select_track(s, 0)

    s.ser.note("⑩e R18 fmt1：preset==0xFF 的轨取默认声音 + steps_clear（诊断槽 3，破坏性）")
    store_cmd(s, "STORE PROJ SYNTH fmt1", r"^PROJ 3 SYNTH case=fmt1 OK\r?$",
              "⑩e SYNTH fmt1")
    store_expect(s, r"^PROJ 3 SYNTH case=fmt1 trk k=0 engine=0 preset=0 steps=0 atk=0 e0=3\r?$",
                 "⑩e 轨 0：默认声音 ANALOG 808 BOOM（e0=3）+ 工程里的 99 被覆盖 + 步清空")
    store_expect(s, r"^PROJ 3 SYNTH case=fmt1 trk k=1 engine=1 preset=1 ",
                 "⑩e 轨 1：不带 0xFF ⇒ 保留工程里的 engine/preset（1/1）")
    m = re.search(r"^PROJ 3 SYNTH case=fmt1 trk k=1 .*?steps=(\d+)", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"⑩e 缺少轨 1 的 steps 字段（无法相对比较）：{s.last[-300:]!r}")
    track1_steps = int(m.group(1))
    (s.ok if track1_steps >= 1 else s.fail)(
        f"⑩ 轨 1 的捕获含 {track1_steps} 步（≥1 = 图案保留断言有判别力）")

    s.ser.note("⑩f R18 foreign：引擎号 ≥ NENGINES 的轨走 proj_foreign_parts（图案保留）")
    store_cmd(s, "STORE PROJ SYNTH foreign", r"^PROJ 3 SYNTH case=foreign OK\r?$",
              "⑩f SYNTH foreign")
    store_expect(s,
                 rf"^PROJ 3 SYNTH case=foreign trk k=1 engine=1 preset=0 "
                 rf"steps={track1_steps + 1} atk=0 e0=4\r?$",
                 f"⑩f 轨 1：未知引擎 → DIGITAL RHODES（engine 1 preset 0、e0=4）、"
                 f"图案保留（steps={track1_steps} → {track1_steps + 1}：只多了诊断放的步 5）")
    store_expect(s, r"^PROJ 3 SYNTH case=foreign trk k=0 engine=0 preset=0 ",
                 "⑩f 轨 0：不受 foreign 分支影响（engine/preset 保持 0/0）")
    proj_erase(s, 3, "⑩f 收口 ERASE 3（诊断槽）")


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
                  r" autosave_writes=(\d+)(?:\s[^\r\n]*)?\r?$", s.last, re.MULTILINE)
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


def ensure_silent_tracks(s: Session) -> list:
    """让**播放中**没有音符发声（⑪d 的前提），且**不破坏用户工程**：把三条合成轨的
    `P_MUTE` 置 1（走上游 `trk_silent()`：播放触发不分配 voice）；返回原值，由
    `restore_mutes()` 还原。

    为什么需要：⑪d 要证明「只有 `song.playing` 能拦住写入」。若播放期间有音符在响，
    `audio_quiet()` 也会拦住同一个写——把 playing 门删掉，断言照样绿（评审 R23 的同一类
    问题）。静音后窗口内 `voices` 恒 0，⑪d 前后各采样一次 `ENG SNAP voices` 作前提断言，
    发声门则交给 ⑪e（走带已停、按住一个键）单独隔离。

    早先的写法是缩 pattern（P_SLEN=1）并清步 0——只恢复了 P_SLEN，被清掉的步没恢复，
    场景因此不幂等（同一设备连跑两轮会丢用户的步）。静音不需要动图案。"""
    saved = []
    for k in range(3):
        select_track(s, k)
        s.cmd(f"PARAM {ENG_P_MUTE}")
        m = re.search(rf"PARAM track={k} id={ENG_P_MUTE} value=(\d+)", s.last)
        if m is None:
            raise InjectError(f"⑪d 读不到轨 {k} 的 P_MUTE：{s.last[-200:]!r}")
        saved.append(int(m.group(1)))
        if saved[-1] != 1:
            store_cmd(s, f"PARAM {ENG_P_MUTE} 1",
                      rf"^PARAM track={k} id={ENG_P_MUTE} value=1 applied=1 ",
                      f"⑪d 轨 {k} P_MUTE=1（播放不发声）")
    select_track(s, 0)
    return saved


def restore_mutes(s: Session, saved: list) -> None:
    """把 `ensure_silent_tracks()` 暂存的 P_MUTE 原值写回（幂等；调用方已停走带）。"""
    for k, v in enumerate(saved):
        select_track(s, k)
        store_cmd(s, f"PARAM {ENG_P_MUTE} {v}",
                  rf"^PARAM track={k} id={ENG_P_MUTE} value={v} applied={v} ",
                  f"⑪d 轨 {k} P_MUTE 还原 {v}")
    select_track(s, 0)


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
    ⑬ 设备侧 torn-write 套件（run_store_torn，最后跑）：先 `SAVE 3`（非空槽，空槽擦除
       现在不写 flash）→ `FAULT 1` + `PROJ ERASE 3` ⇒ `STORE ERR save`、`STORE STATS
       faults=1 armed=0`、LIST 仍 USED + `FIELDS 3` 证 flash 侧旧副本 → `STORE TEST`
       六场景 PASS 6/6（两次）→ 播放中拒绝 → 停止后仍 PASS。
    ⑭ 旧格式兼容门禁（run_store_fields）：FUN5/FUN4/FUN1 fixture 经
       `STORE RAW BEGIN/<hex>/END` 写诊断槽 3 → `STORE PROJ LOAD 3` → `STORE PROJ FIELDS 3`
       与 `gen_project_fixtures.py` 的 62 项期望逐项比对；截断 fixture ⇒ LOAD 回
       `STORE ERR invalid` 且 `LIVEHASH` 不变；槽 0–2 不动、收口 ERASE 3。
    ⓯ FILE 组两页（run_store_file，最后跑；Task 12）：`UI projects` 的空态（初始**无选中
       槽**，提示 SELECT A SLOT）/used 截屏 → TAP SAVE/LOAD/ERASE（`STORE PROJ LIST`/
       `HASH`/`LIVEHASH` 证明与 console 命令同一动作函数）→ 键盘从「无选中」`→`×4 +
       `ENTER` 落在 D（槽 3，判别力：若选中槽没动，ENTER 不动作）→ 播放中 TAP LOAD/SAVE
       都回 `STOP BEFORE <ACT>` 且 LIST 逐槽不变（project LOAD 会整份替换引擎，上游也要求
       先停走带）；`UI presets` 32 槽 + TAP SAVE/LOAD（换引擎后回到预置引擎）/ERASE +
       鼓轨 `DRUM TRACK: NO SOUND` + 播放中 **LOAD 允许**而 SAVE 拒绝（`up_load_to` 只
       读不写 flash）+ `UI MEM` 的 FILE 组水位；跨组 `UI step` 量过渡峰值（≥4KB）；离开组
       两页回 `unbuilt`；收口全空 + 轨 0 回 ANALOG。
    ⓰ SOUND 页的 bank 存取行（run_store_ui_bank，最后跑；Task 13 / P4-F Task 5 A10）：**行
       缺席**（轨 0 = ANALOG / 轨 3 = 鼓轨：bank 带没有 SAVE/ERASE 按钮色——5B-1 仪表盘页上
       该带被卡片视口盖住，不发 TAP；原「锁定页 NOT FM6」页面拒绝态随独立 FM6 页删除）→ `UI sound`（轨 1 =
       FM6 ⇒ 行出现）+ BANK 行截屏（SAVE 绿/ERASE 橙/槽号 B1）→ 槽环绕
       （`−` B1→B27、`+` 回 B1、再 `+`×3 到 B4；`PARAM 70` 的 PTCH 不变）→ `TAP BANK
       SAVE` ⇒ `status="SAVE OK"` 且 `STORE BANK GET 3` == `STORE BANK LIVE` → `TAP BANK
       ERASE` ⇒ `status="ERASE OK"`、`GET 3` → empty → **先 ERASE 再 PLAY** 的 SAVE ⇒
       `STOP BEFORE SAVE` 且槽保持空；收口擦槽 + 轨 0/1 回 ANALOG + `STORE AUTO ON`。

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
        proj_erase(s, i, f"① ERASE {i}")
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
    # spec §5 的时序口径：STORE STATS 报告上一次写动作的 xrun 增量与耗时（≤150ms 目标）
    s.cmd("STORE STATS")
    m = re.search(r"last_xrun=(-?\d+) last_save_ms=(\d+)", s.last)
    if m is None:
        s.fail("② STORE STATS 缺 last_xrun/last_save_ms（spec §4.4/§5 的报告面）")
    else:
        xr, ms = int(m.group(1)), int(m.group(2))
        (s.ok if xr <= 16 else s.fail)(f"② 上次 SAVE 的 xrun 增量 {xr} ≤16")
        (s.ok if 0 < ms <= 150 else s.fail)(f"② 上次 SAVE 耗时 {ms}ms ∈ (0,150]（spec §5）")
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
    proj_erase(s, 0, "⑤ ERASE 0")
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
    run_store_torn(s)
    run_store_fields(s)
    run_store_file(s)
    run_store_ui_bank(s)


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
    ⓓ 播放门（ⓑ 之后，gap 已在 25s 外过期）：先把三条合成轨临时静音（`ensure_silent_tracks`：
       P_MUTE=1，播放不分配 voice ⇒ `audio_quiet` 恒成立；窗口结束后
       还原 P_MUTE；不动图案/长度，场景可重复）→ TAP PLAY →
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
    # 原 DI-23（场景假设）：⑨g 等硬复位会把 autosave 恢复的工作工程装回引擎，而
    # P_MUTE / P_LEVEL / P_AMODE（ARP）/ P_AHOLD（KBD HOLD）都是 param_kept
    # （`ENG TRACK` 不重置）。实测从旧格式 fixture 漂进来的 mute=1 / AMODE=14 / AHOLD=1
    # 会让 ⑪e 的「按住 C ⇒ voices≥1」与「松开 ⇒ audio_quiet 恢复」前提不成立：
    #   - ARP：走带停止时不发声（音符被 arp 持有）⇒ voices=0；
    #   - KBD HOLD（`!AMODE && AHOLD`）：松开键后音符继续持有 ⇒ voice 永不释放、
    #     audio_quiet 恒 false ⇒ ⑪e/⑪f 的写入断言假红。
    # 四项显式归一（只影响选中轨 0）：
    s.cmd("PARAM 40 0")
    s.expect(r"PARAM track=0 id=40 value=0 applied=0", "⑪ 归一 P_MUTE=0（发声门前提）")
    s.cmd(f"PARAM {ENG_P_LEVEL} {ENG_LEVEL_DEF}")
    s.expect(rf"PARAM track=0 id={ENG_P_LEVEL} value={ENG_LEVEL_DEF} applied={ENG_LEVEL_DEF}",
             "⑪ 归一 P_LEVEL=104（发声门前提）")
    s.cmd("PARAM 17 0")
    s.expect(r"PARAM track=0 id=17 value=0 applied=0", "⑪ 归一 P_AMODE=0（ARP 关）")
    s.cmd("PARAM 23 0")
    s.expect(r"PARAM track=0 id=23 value=0 applied=0",
             "⑪ 归一 P_AHOLD=0（KBD HOLD 关：松开键必须释放）")
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
    mutes = ensure_silent_tracks(s)
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
    # 把 ⑪d 临时置上的 P_MUTE 还原。场景不动图案/长度，故连跑两轮不丢用户的步。
    restore_mutes(s, mutes)

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


def store_test_diag(s: Session, tag: str) -> None:
    """`STORE TEST` 的内部诊断（Task 8 评审 minor 1/2）：seq 的双向胜者（copy1→copy0）与
    三个 arm 场景的故障分类/胜出副本。缺任一行即失败。"""
    store_expect(s, r"sloop_store: STORE TEST seq: v2 winner=copy1 seq=2 ok=1; "
                    r"v3 winner=copy0 seq=3 ok=1",
                 f"{tag} seq 双向（v2→copy1、v3→copy0）")
    for case, op in (("erase-fail", "erase"), ("half-write", "prog-payload"),
                     ("header-fail", "prog-header")):
        store_expect(s, rf"sloop_store: STORE TEST {case}: faults=1 armed=0 op={op} "
                        r"survivor=copy1",
                     f"{tag} {case}: faults=1 op={op} survivor=copy1")


def run_store_torn(s: Session) -> None:
    """⑬ 设备侧 torn-write 故障注入套件（Task 8）：`STORE TEST` 的六场景。

    对象 = OBJ_SETTINGS（P4-B 不写设置；套件内部 `st_load` 快照 / `st_save` 恢复——
    **破坏性**诊断，console 帮助里写明）。六个场景全部由固件断言（返回
    `STORE TEST PASS 6/6` / `STORE TEST FAIL <name>`），脚本断言结果令牌与「故障真的
    走到 HAL」，不让套件自证：

    ① **HAL 故障探针**（脚本可见的 `STORE STATS` 证据）：**先给槽 3 存一份工程**（erase
       空槽现在不写 flash，见下）→ `STORE FAULT 1` → `STORE PROJ ERASE 3`。erase 是
       `st_save` 的第一次写操作 ⇒ 命中故障 ⇒ `STORE ERR save`；`STORE STATS` 的
       `faults=1 armed=0` 证明命中一次且自动解除（故障不会漏到后续命令）；槽 3 的**旧
       副本仍在**（`STORE PROJ LIST` 仍 USED、`STORE PROJ FIELDS 3` 可读——被拒的擦除
       没落盘）；`STORE FAULT CLEAR` 回 `cleared taken=1`（计数器看到的命中次数），随后
       正常 ERASE 3 收口。
    ② `STORE TEST` ⇒ `STORE TEST PASS 6/6`；`STORE STATS` 的 faults=0 armed=0（套件
       结束必须 `fault_clear`，否则下一次写命令会被残留注入打坏）。
    ③ 再跑一次 ⇒ 仍 PASS 6/6（套件结束恢复了设置对象，场景可重复）。
    ④ 播放中 `STORE TEST` ⇒ `STORE ERR playing`（与其它写 API 同一走带守卫；守卫在取
       锁/快照**之前**，故这次拒绝不消费故障、也碰不到设置对象），停止后再跑 ⇒ PASS 6/6。
       收口 LIST 槽 3 仍 EMPTY。

    另外断言套件的内部诊断日志（Task 8 评审 minor 2）：`seq` 的**双向**判定
    （v2 在 copy1 胜、v3 回 copy0 胜——不是「永远偏好 B」）与三个 arm 场景的
    `faults=1 op=<erase|prog-payload|prog-header> survivor=copy1`。这些是套件内部断言的
    可读投影：日志缺行 = 断言消失，不能只靠代码。
    """
    s.ser.note("⑬ 设备侧 torn-write 套件：HAL 故障探针 + STORE TEST 六场景")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑬ STORE AUTO OFF")
    ensure_stopped(s)
    s.cmd("STORE STATS")
    store_expect(s, r"^STORE STATS ready=1 .*faults=0 armed=0 ",
                 "⑬ 起点 STORE STATS faults=0 armed=0（无残留注入）")

    s.ser.note("⑬a HAL 故障探针：槽 3 有旧副本 → FAULT 1 命中擦除 ⇒ STORE ERR save")
    proj_erase(s, 3, "⑬a 归一 ERASE 3（诊断槽为空）")
    store_cmd(s, "STORE PROJ SAVE 3", r"^PROJ 3 SAVED hash=[0-9A-F]{8}\r?$",
              "⑬a SAVE 3（给擦除一个非空槽：空槽不写 flash）")
    store_cmd(s, "STORE FAULT 1", r"^STORE FAULT armed nth=1 half=0\r?$", "⑬a STORE FAULT 1")
    store_cmd(s, "STORE PROJ ERASE 3", r"^STORE ERR save\r?$",
              "⑬a 擦除命中故障 → STORE ERR save（第一次写操作）")
    s.cmd("STORE STATS")
    store_expect(s, r"^STORE STATS ready=1 .*faults=1 armed=0 ",
                 "⑬a STORE STATS faults=1 armed=0（命中一次并自动解除）")
    s.cmd("STORE PROJ LIST")
    store_expect(s, r"^PROJ 3 USED hash=[0-9A-F]{8}\r?$", "⑬a LIST 槽 3 仍 USED（被拒的擦除没落盘）")
    s.cmd("STORE PROJ FIELDS 3")
    store_expect(s, r"^PROJ 3 FIELDS fmt=6 ",
                 "⑬a FIELDS 3 仍可读（flash 侧旧副本完好；LIST 只证 RAM 槽）")
    store_cmd(s, "STORE FAULT CLEAR", r"^STORE FAULT cleared taken=1\r?$",
              "⑬a FAULT CLEAR cleared taken=1")
    proj_erase(s, 3, "⑬a 探针收口 ERASE 3（旧副本清掉）")

    s.ser.note("⑬b STORE TEST ⇒ PASS 6/6；套件清故障（faults=0 armed=0）")
    store_cmd(s, "STORE TEST", r"^STORE TEST PASS 6/6\r?$", "⑬b STORE TEST PASS 6/6")
    store_test_diag(s, "⑬b")
    s.cmd("STORE STATS")
    store_expect(s, r"^STORE STATS ready=1 .*faults=0 armed=0 ",
                 "⑬b 套件结束 STORE STATS faults=0 armed=0（fault_clear）")

    s.ser.note("⑬c 再跑一次 ⇒ 仍 PASS 6/6（快照恢复后场景可重复）")
    store_cmd(s, "STORE TEST", r"^STORE TEST PASS 6/6\r?$", "⑬c STORE TEST 第二轮 PASS 6/6")

    s.ser.note("⑬d 播放守卫：PLAY 中 STORE TEST ⇒ playing；停止后仍 PASS 6/6")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑬d UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑬d TAP PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑬d STATE SNAP playing=1")
    store_cmd(s, "STORE TEST", r"^STORE ERR playing\r?$", "⑬d 播放中 STORE TEST → STORE ERR playing")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑬d TAP STOP")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑬d STATE SNAP playing=0")
    store_cmd(s, "STORE TEST", r"^STORE TEST PASS 6/6\r?$",
              "⑬d 停止后 STORE TEST PASS 6/6（播放中的拒绝早于取锁/快照，套件仍可跑）")
    s.cmd("STORE PROJ LIST")
    store_expect(s, r"^PROJ 3 EMPTY\r?$", "⑬d 收口 LIST 槽 3 EMPTY")


# ------------------------------------------------ ⑭ 旧格式 fixture（FUN5/FUN4/FUN1） ---
def store_proj_slots(s: Session) -> dict:
    """`STORE PROJ LIST` → {槽号: "USED hash=XXXXXXXX" / "EMPTY"}（槽 0–2 的「未被动过」判据）"""
    s.cmd("STORE PROJ LIST")
    out = {}
    for raw in s.last.splitlines():
        m = re.match(r"^PROJ (\d+) (USED hash=[0-9A-F]{8}|EMPTY)\r?$", raw.strip())
        if m:
            out[int(m.group(1))] = m.group(2)
    return out


def store_fields_read(s: Session, slot: int = 3):
    """`STORE PROJ FIELDS <slot>` → (键顺序列表, {key: value}, 原始行)。

    行首 `PROJ <slot> FIELDS ` 之后的每个 token 是 `key=value`；键顺序即固件的固定打印序
    （脚本与生成器共用 field_keys()，顺序错也判失败）。"""
    s.cmd(f"STORE PROJ FIELDS {slot}")
    line = None
    for raw in s.last.splitlines():
        if raw.strip().startswith(f"PROJ {slot} FIELDS "):
            line = raw.strip()
    if line is None:
        raise InjectError(f"STORE PROJ FIELDS {slot}: 回显缺少 `PROJ {slot} FIELDS ` 行"
                          f"（响应 {s.last.strip()[-300:]!r}）")
    keys, vals = [], {}
    for tok in line.split()[3:]:
        key, sep, value = tok.partition("=")
        if not sep:
            raise InjectError(f"STORE PROJ FIELDS {slot}: token {tok!r} 不是 key=value")
        keys.append(key)
        vals[key] = value
    return keys, vals, line


def store_fields_expect(s: Session, expected: dict, order: list, label: str) -> None:
    """逐项比对：键顺序一致 + 62 项全部相等（任何一项不同都单独 FAIL，带值）。"""
    keys, vals, line = store_fields_read(s)
    (s.ok if keys == order else s.fail)(
        f"{label} 键顺序 == 生成器的 field_keys()（实得 {len(keys)} 键）")
    bad = [(k, expected.get(k), vals.get(k)) for k in order if vals.get(k) != str(expected.get(k))]
    for k in order:
        (s.ok if vals.get(k) == str(expected.get(k)) else s.fail)(
            f"{label} {k}={vals.get(k)}（期望 {expected.get(k)}）")
    s.ser.note(f"{label} 行（{len(keys)} 项，{len(bad)} 项不符）：{line}")


def store_raw_grammar(s: Session) -> None:
    """RAW 通道的语法/错误令牌（不含 fixture）：超长、越界块、未 BEGIN、不完整、ABORT，
    以及 R27/R28 的三条：长度上限 = 3,840（一个 flash 对象的 payload）、穿插其它 STORE
    子命令会丢弃未完成上传（END 回 invalid、不落盘、之后可重新上传）、播放中 END 拒绝。"""
    s.ser.note("⑭a STORE RAW 语法与错误令牌：size / invalid / ABORT / R27 / R28")
    store_cmd(s, "STORE RAW BEGIN 4097", r"^STORE ERR size\r?$",
              "⑭a BEGIN 4097（> 3,840）→ STORE ERR size")
    store_cmd(s, "STORE RAW BEGIN 3841", r"^STORE ERR size\r?$",
              "⑭a BEGIN 3841（= ST_PAYLOAD_MAX + 1）→ 当场 size，不等到 END 的 save")
    store_cmd(s, "STORE RAW BEGIN 3840", r"^STORE RAW BEGIN len=3840\r?$",
              "⑭a BEGIN 3840（= ST_PAYLOAD_MAX）接受")
    store_cmd(s, "STORE RAW ABORT", r"^STORE RAW ABORTED\r?$", "⑭a 收口 ABORT")
    store_cmd(s, "STORE RAW BEGIN 2", r"^STORE RAW BEGIN len=2\r?$", "⑭a BEGIN 2")
    store_cmd(s, "STORE RAW 010203", r"^STORE ERR size\r?$",
              "⑭a 追加 3B 超过声明的 2B → STORE ERR size")
    store_cmd(s, "STORE RAW ABORT", r"^STORE RAW ABORTED\r?$", "⑭a ABORT")
    store_cmd(s, "STORE RAW END project 3", r"^STORE ERR invalid\r?$",
              "⑭a 未 BEGIN 的 END → STORE ERR invalid")
    store_cmd(s, "STORE RAW BEGIN 4", r"^STORE RAW BEGIN len=4\r?$", "⑭a BEGIN 4")
    store_cmd(s, "STORE RAW 0102", r"^STORE RAW received=2\r?$", "⑭a 追加 2B")
    store_cmd(s, "STORE RAW END project 3", r"^STORE ERR invalid\r?$",
              "⑭a 只收到 2/4 字节的 END → STORE ERR invalid（不写 flash）")
    store_cmd(s, "STORE RAW ABORT", r"^STORE RAW ABORTED\r?$", "⑭a 收口 ABORT")
    s.cmd("STORE PROJ LIST")
    store_expect(s, r"^PROJ 3 EMPTY\r?$", "⑭a 被拒的 END 没写槽 3（仍 EMPTY）")

    s.ser.note("⑭a2 R27：上传中途穿插其它 STORE 子命令 ⇒ 未完成上传被丢弃，END 回 invalid")
    store_cmd(s, "STORE RAW BEGIN 4", r"^STORE RAW BEGIN len=4\r?$", "⑭a2 BEGIN 4")
    store_cmd(s, "STORE RAW AABB", r"^STORE RAW received=2\r?$", "⑭a2 追加 2B")
    store_cmd(s, "STORE PROJ LIST", r"^PROJ 0 (USED|EMPTY)", "⑭a2 穿插 STORE PROJ LIST")
    store_cmd(s, "STORE RAW CCDD", r"^STORE ERR invalid\r?$",
              "⑭a2 穿插命令后的追加 → invalid（上传已被丢弃，状态机不再接受数据）")
    store_cmd(s, "STORE RAW END project 3", r"^STORE ERR invalid\r?$",
              "⑭a2 END → invalid（不会把别人的字节写进 flash）")
    s.cmd("STORE PROJ LIST")
    store_expect(s, r"^PROJ 3 EMPTY\r?$", "⑭a2 被丢弃的上传没有落盘（槽 3 仍 EMPTY）")
    store_cmd(s, "STORE RAW BEGIN 3", r"^STORE RAW BEGIN len=3\r?$", "⑭a2 重新 BEGIN 3")
    store_cmd(s, "STORE RAW 010203", r"^STORE RAW received=3\r?$", "⑭a2 追加 3B")
    store_cmd(s, "STORE RAW END project 3", r"^STORE RAW END project 3 saved len=3\r?$",
              "⑭a2 新上传仍能 END（状态机已复位）")
    proj_erase(s, 3, "⑭a2 收口 ERASE 3")

    s.ser.note("⑭a3 R28：播放中 STORE RAW END 拒绝（写 API 的走带守卫）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑭a3 UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑭a3 TAP PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑭a3 STATE SNAP playing=1")
    store_cmd(s, "STORE RAW BEGIN 4", r"^STORE RAW BEGIN len=4\r?$",
              "⑭a3 播放中 BEGIN 允许（只写暂存）")
    store_cmd(s, "STORE RAW AABB", r"^STORE RAW received=2\r?$", "⑭a3 播放中追加 2B")
    store_cmd(s, "STORE RAW END project 3", r"^STORE ERR playing\r?$",
              "⑭a3 播放中 END → STORE ERR playing（未写 flash、上传态保持）")
    s.cmd("STORE PROJ LIST")
    store_expect(s, r"^PROJ 3 EMPTY\r?$", "⑭a3 播放中的 END 没落盘（槽 3 仍 EMPTY）")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑭a3 TAP STOP")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑭a3 STATE SNAP playing=0")
    store_cmd(s, "STORE RAW ABORT", r"^STORE RAW ABORTED\r?$", "⑭a3 收口 ABORT")


def run_store_fields(s: Session) -> None:
    """⑭ 旧格式兼容门禁（Task 9，spec §6.3）：FUN5/FUN4/FUN1 fixture 经 `STORE RAW` 上传到
    诊断槽 3 → `STORE PROJ LOAD 3` → `STORE PROJ FIELDS 3` 与生成器的 62 项期望逐项比对。

    为什么需要这条通道：console 单行放不下 3,816B（一行 ≤240 hex = 120B：USB-Serial-JTAG
    驱动 RX 环 256B，整行必须一次入环），firmware 用 `STORE RAW BEGIN/<hex>/END` 把
    payload 原样交给 `st_save`（CRC/提交头由 storage.c 生成），`scripts/store_raw.py`
    是同一实现（独立入口/注入场景共用）。

    期望值的来源（独立性）：`scripts/gen_project_fixtures.py` 按上游 `project.c` 的
    结构体与 `tests/project_test.c` 的构造器**生成字节**，再用 Python 独立实现的上游
    转换（proj_from_v5/v4、v1→v3→v6）从字节算出字段；`--host-check` 另用 vendored
    `project.c` 的宿主程序交叉校验（`scripts/fixture_host_check.c`）。故固件侧 FIELDS
    若错（字段/偏移/转换），比对必红——不是拿固件输出当期望。

    断言：
      ① 槽 0–2 的 `PROJ LIST` 行逐字节不变（诊断槽 3 专用）；
      ② 三个 fixture 各：RAW 上传 → LOAD 3 回 `PROJ 3 LOADED` → LIST 槽 3 USED →
         FIELDS 3 的 62 项（含键顺序）与 expected.json 一致；
      ③ 截断 fixture（fun5[:100]）：`STORE PROJ LOAD 3` 回 `STORE ERR invalid`（对象存在但
         不是任何已知格式——与「空槽」的 empty 区分），`STORE LIVEHASH` 不变（LOAD 在
         导入失败处退出，从未 proj_apply），`FIELDS 3` 同样回 invalid；收口 ERASE 3。
    """
    s.ser.note("⑭ 旧格式兼容：生成 FUN5/FUN4/FUN1 fixture（含 expected.json）")
    out_dir = s.out_dir / f"fixtures-{s.stamp}"
    doc = fixtures.generate(out_dir)
    order = doc["order"]
    print(f"  [NOTE] ⑭ fixtures → {out_dir}（{len(order)} 项/格式）")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑭ STORE AUTO OFF（本段不写 autosave）")
    ensure_stopped(s)
    proj_erase(s, 3, "⑭ 归一 ERASE 3")
    before = store_proj_slots(s)
    (s.ok if all(i in before for i in range(4)) else s.fail)(
        f"⑭ PROJ LIST 列出 4 槽（{before}）")

    store_raw_grammar(s)

    for key in ("fun5", "fun4", "fun1"):
        data = (out_dir / f"{key}.bin").read_bytes()
        s.ser.note(f"⑭b {key}.bin（{len(data)}B）→ STORE RAW → LOAD 3 → FIELDS 3")
        proj_erase(s, 3, f"⑭ {key} 归一 ERASE 3")
        store_raw.upload_raw(s.cmd, data, "project", 3)
        s.ok(f"⑭ {key} STORE RAW 上传（{len(data)}B，{(len(data) + 119) // 120} 块）")
        store_cmd(s, "STORE PROJ LOAD 3", rf"^PROJ 3 LOADED hash={STORE_HASH8}\r?$",
                  f"⑭ {key} LOAD 3（proj_import + proj_apply）")
        slots = store_proj_slots(s)
        (s.ok if slots.get(3, "").startswith("USED") else s.fail)(
            f"⑭ {key} LIST 槽 3 USED（{slots.get(3)}）")
        store_fields_expect(s, doc["fixtures"][key], order, f"⑭ {key} FIELDS 3")

    s.ser.note("⑭c 截断 FUN5（前 100B）：LOAD 3 → STORE ERR invalid，LIVEHASH 不变")
    truncated = (out_dir / "fun5.bin").read_bytes()[:100]
    proj_erase(s, 3, "⑭c 归一 ERASE 3")
    live = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "⑭c 截断前的 LIVEHASH")
    store_raw.upload_raw(s.cmd, truncated, "project", 3)
    s.ok(f"⑭c 截断 fixture 上传（{len(truncated)}B）")
    store_cmd(s, "STORE PROJ LOAD 3", r"^STORE ERR invalid\r?$",
              "⑭c 截断对象 LOAD 3 → STORE ERR invalid（不是空槽的 empty）")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⑭c LOAD 失败后的 LIVEHASH") == live else s.fail)(
        f"⑭c LIVEHASH 不变（{live:08X}；LOAD 在导入失败处退出，从未 proj_apply）")
    store_cmd(s, "STORE PROJ FIELDS 3", r"^STORE ERR invalid\r?$",
              "⑭c FIELDS 3 也回 STORE ERR invalid（st_load 成功但 proj_import 拒绝）")
    store_cmd(s, "STORE PROJ LOAD 9", r"^STORE ERR invalid\r?$", "⑭c 槽号越界 LOAD 9 → invalid")

    s.ser.note("⑭d 收口：ERASE 3；槽 0–2 与 ⑭ 开始时逐字节相同")
    proj_erase(s, 3, "⑭d 收口 ERASE 3")
    after = store_proj_slots(s)
    untouched = {i: before.get(i) for i in range(3)}
    (s.ok if {i: after.get(i) for i in range(3)} == untouched else s.fail)(
        f"⑭d 槽 0–2 未被触碰（{untouched} vs { {i: after.get(i) for i in range(3)} }）")
    (s.ok if after.get(3) == "EMPTY" else s.fail)(
        f"⑭d 槽 3 收口 EMPTY（{after.get(3)}）")


# ---- FILE 组（PROJECTS / PRESETS 页，P4-B Task 12）----
# 几何与 page_projects.c / page_presets.c 的布局宏同源（屏幕绝对坐标 = 页内 + 72）：
# PROJECTS 卡 296×400 @ (24 + k·312, 100)，卡内按钮 100×40 @ x=98、y=170/234/298；
# PRESETS 格 144×72 @ (30 + c·152, 88 + r·80)，底部按钮 100×40 @ y=460、x=24/140/256。
PROJ_CARD_X0, PROJ_CARD_Y, PROJ_CARD_W, PROJ_CARD_H, PROJ_CARD_PITCH = 24, 100, 296, 400, 312
PROJ_BTN_X, PROJ_BTN_Y0, PROJ_BTN_W, PROJ_BTN_H, PROJ_BTN_PITCH = 98, 170, 100, 40, 64
PRE_CELL_X0, PRE_CELL_Y0, PRE_CELL_W, PRE_CELL_H = 30, 88, 144, 72
PRE_CELL_PITCH_X, PRE_CELL_PITCH_Y = 152, 80
PRE_BTN_X0, PRE_BTN_Y, PRE_BTN_W, PRE_BTN_H, PRE_BTN_PITCH = 24, 460, 100, 40, 116
FILE_ACT_NAMES = ("save", "load", "erase")
# 状态行（两页共用：页内 (24,20) 28pt，宽 1200）；OK=轨绿、STOP/ERROR/NO STORAGE=
# UI_ERROR、DRUM=UI_ACCENT、EMPTY/初始=UI_TEXT_DIM（theme.h 的 ui_file_status_text）。
FILE_STATUS_BOX = (24, 72 + 18, 720, 72 + 62)


def proj_card_xy(k: int) -> tuple:
    """PROJECTS 卡 k 的字母区中心（屏幕绝对坐标；x 在卡内按钮列 x=98 左侧）。"""
    return (PROJ_CARD_X0 + k * PROJ_CARD_PITCH + 56, 72 + PROJ_CARD_Y + 70)


def proj_btn_xy(k: int, act: int) -> tuple:
    """PROJECTS 卡 k 的第 act 个按钮中心（0=SAVE 1=LOAD 2=ERASE）。"""
    return (PROJ_CARD_X0 + k * PROJ_CARD_PITCH + PROJ_BTN_X + PROJ_BTN_W // 2,
            72 + PROJ_CARD_Y + PROJ_BTN_Y0 + act * PROJ_BTN_PITCH + PROJ_BTN_H // 2)


def pre_cell_xy(slot: int) -> tuple:
    """PRESETS 槽 slot（0..31，行优先 4×8）中心。"""
    r, c = divmod(slot, 8)
    return (PRE_CELL_X0 + c * PRE_CELL_PITCH_X + PRE_CELL_W // 2,
            72 + PRE_CELL_Y0 + r * PRE_CELL_PITCH_Y + PRE_CELL_H // 2)


def pre_btn_xy(layout: int) -> tuple:
    """PRESETS 底部按钮的**点击**中心（版面 0=LOAD 1=SAVE 2=ERASE）。

    版面顺序 ≠ 动作语义（page_presets.c 的 k_btn_act：LOAD=1/SAVE=0/ERASE=2，
    TAP 回显的 action= 是语义名）——几何只按版面 x 计算。"""
    return (PRE_BTN_X0 + layout * PRE_BTN_PITCH + PRE_BTN_W // 2,
            72 + PRE_BTN_Y + PRE_BTN_H // 2)


def proj_btn_probe(k: int, act: int) -> tuple:
    """PROJECTS 按钮的**像素探针**点：按钮内左侧 12px（避开居中文本的字形）。"""
    return (PROJ_CARD_X0 + k * PROJ_CARD_PITCH + PROJ_BTN_X + 12,
            72 + PROJ_CARD_Y + PROJ_BTN_Y0 + act * PROJ_BTN_PITCH + PROJ_BTN_H // 2)


def pre_btn_probe(layout: int) -> tuple:
    """PRESETS 按钮的像素探针点（按钮内左侧 12px，避开居中文本的字形）。"""
    return (PRE_BTN_X0 + layout * PRE_BTN_PITCH + 12, 72 + PRE_BTN_Y + PRE_BTN_H // 2)


def file_action(s: Session, page: str, xy: tuple, slot: int, act: int, want: str,
                label: str) -> None:
    """TAP 一个 FILE 页动作按钮，断言回显里的槽号/动作/**状态行文案**。

    状态行文案是 TAP 回显的一部分（console 从 `sloop_ui_file_status_text()` 取，
    与屏幕上的文本同源），所以「播放中 → STOP BEFORE SAVE」是机器可判的。"""
    s.cmd("TAP %d %d" % xy)
    s.expect(rf'-> FILE {page} slot={slot} action={FILE_ACT_NAMES[act]} '
             rf'status="{re.escape(want)}"', label)


def file_keys(s: Session, code: int, times: int = 1, label: str = "") -> None:
    """FILE 页键盘：←/→（0x4A/0x4C）改选中槽、ENTER（0x3D）= LOAD。按下沿触发。"""
    for _ in range(times):
        s.cmd(f"KEY {code} down")
        s.cmd(f"KEY {code} up")
    if label:
        s.ok(label)


def key_space(s: Session) -> None:
    """space（0x4D）= 播放/停止切换（InputManager 的 ACT_PLAY_TOGGLE）。"""
    s.cmd("KEY 0x4D down")
    s.cmd("KEY 0x4D up")


def run_store_file(s: Session) -> None:
    """⓯ FILE 组两页（Task 12）：TAP 动作与 console 命令走**同一动作函数**的机器证明。

    判据链（不是「页面画出来了」）：
      - PROJECTS：TAP 卡 k 的 SAVE ⇒ `STORE PROJ LIST` 槽 k USED 且 `HASH k` == SAVE 前的
        `LIVEHASH`；改 BPM 后 TAP LOAD ⇒ `LIVEHASH` 回到保存值、`STATE SNAP bpm` 回到
        保存时的值；TAP ERASE ⇒ 槽 EMPTY 且 LIVEHASH 不变（擦除不动引擎）；
      - 键盘：TAP 卡 0 把选中槽归零 → `→`×3 + `ENTER` ⇒ LOAD 落在**槽 3**（选中槽已移动，
        否则 LOAD 0 空槽不会改引擎）；
      - 播放中 TAP SAVE ⇒ 状态行 `STOP BEFORE SAVE` 且 `STORE PROJ LIST` 逐字节不变；
      - PRESETS：TAP 槽 3 + SAVE ⇒ `PRESET 3 USED ANALOG ANALOG 04`；换引擎后 TAP LOAD ⇒
        `ENG SNAP` 的 engine/preset 回到预置里的值；键盘 `→`×3 + ENTER 同样生效；
        鼓轨 SAVE ⇒ 状态行 `DRUM TRACK: NO SOUND`；播放中 SAVE ⇒ `STOP BEFORE SAVE`；
      - `UI MEM`：FILE 组 built/sum_delta/sum_est 与两页 `delta ≤ est`；
      - 离开 FILE 组后两页回 `unbuilt`（组作用域生命周期，ADR-005 §3）。
    """
    s.ser.note("⓯ FILE 组：PROJECTS（卡/SAVE/LOAD/ERASE/键盘 ←→ ENTER/播放拒绝）"
               "+ PRESETS（32 槽/换引擎 LOAD/鼓轨/播放拒绝）+ 组作用域")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⓯ STORE AUTO OFF（本段不写 autosave）")
    ensure_stopped(s)
    select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⓯ 归一轨 0 = ANALOG 预置 0")
    for i in range(4):
        proj_erase(s, i, f"⓯ 归一 ERASE PROJ {i}")
    for i in (0, 1, 3):
        store_cmd(s, f"STORE PRESET ERASE {i}", rf"^PRESET {i} ERASED\r?$",
                  f"⓯ 归一 ERASE PRESET {i}")

    # ---------- PROJECTS 页：注册 / 池水位 / 空态截屏 ----------
    s.cmd("UI projects")
    s.expect(r"UI page=projects \(9\)", "⓯ UI projects（FILE 组页面已注册）")
    mem = param_mem(s)
    (s.ok if mem["head"]["group"] == "FILE" else s.fail)(
        f"⓯ UI MEM group={mem['head']['group']}（期望 FILE）")
    gr = mem["groups"]["FILE"]
    (s.ok if gr["built"] == 1 and gr["sum_est"] > 0 else s.fail)(
        f"⓯ FILE built={gr['built']} sum_est={gr['sum_est']}（期望 1 / 两页 est 之和 > 0）")
    pg = mem["pages"].get(9, ("?", 0, 0))
    (s.ok if pg[0] == "built" and 0 < pg[2] <= pg[1] else s.fail)(
        f"⓯ PROJECTS state={pg[0]} delta={pg[2]} ≤ est={pg[1]}")
    (s.ok if 0 < gr["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⓯ FILE build_ms={gr['build_ms']}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")

    img = s.capture("task-12-projects-empty")
    # 初始**无选中槽**：提示 "SELECT A SLOT" 时没有任何卡被描边成已选（minor 6）
    s.probe(img, "⓯ 卡 0 未选中底（初始无选中槽）", PROJ_CARD_X0 + 40, 72 + PROJ_CARD_Y + 200,
            C_PANEL, 8)
    s.probe(img, "⓯ 卡 1 未选中底", PROJ_CARD_X0 + PROJ_CARD_PITCH + 40,
            72 + PROJ_CARD_Y + 200, C_PANEL, 8)
    s.probe(img, "⓯ 卡 0 未选中描边（3px）", PROJ_CARD_X0 + 100, 72 + PROJ_CARD_Y - 3,
            C_PANEL_HI, 10)
    s.probe(img, "⓯ SAVE 按钮底（卡 0）", *proj_btn_probe(0, 0), C_GREEN)
    s.probe(img, "⓯ LOAD 按钮底（卡 0）", *proj_btn_probe(0, 1), C_YELLOW)
    s.probe(img, "⓯ ERASE 按钮底（卡 0）", *proj_btn_probe(0, 2), C_ORANGE)
    s.probe_region(img, "⓯ 状态行初始文本（SELECT A SLOT 灰字）", FILE_STATUS_BOX, C_TEXT_DIM,
                   24, 20)
    s.probe(img, "⓯ 组签选中（FILE）", *C_TAB_FILE, C_YELLOW)
    s.probe(img, "⓯ 子签选中（PROJECTS）", *C_TAB_PROJECTS, C_YELLOW)
    s.probe(img, "⓯ 子签未选中（PRESETS）", *C_TAB_PRESETS, C_PANEL_HI, 10)

    # 点卡设选中槽（TAP 与物理点击同一动作函数；回显 kind/slot）
    s.cmd("TAP %d %d" % proj_card_xy(1))
    s.expect(r"-> FILE projects slot=1 select", "⓯ TAP 卡 1 设选中槽")

    # ---------- TAP SAVE：槽 2 USED 且 hash == 捕获 ----------
    b0 = store_bpm(s)
    live = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "⓯ SAVE 前的 LIVEHASH")
    (s.ok if live != 0 else s.fail)(f"⓯ LIVEHASH={live:08X} 非零（捕获有效）")
    file_action(s, "projects", proj_btn_xy(2, 0), 2, 0, "SAVE OK", "⓯ TAP SAVE（卡 2）→ SAVE OK")
    store_cmd(s, "STORE PROJ LIST", r"^PROJ 0 EMPTY\r?$", "⓯ TAP SAVE 只写槽 2（槽 0 EMPTY）")
    saved = store_expect(s, rf"^PROJ 2 USED hash={STORE_HASH8}\r?$", "⓯ TAP SAVE 后 LIST 槽 2 USED")
    (s.ok if int(saved.group(1), 16) == live else s.fail)(
        f"⓯ 槽 2 的 hash {int(saved.group(1), 16):08X} == SAVE 前 LIVEHASH {live:08X}")
    (s.ok if store_hash(s, "STORE PROJ HASH 2", r"^PROJ 2 HASH=" + STORE_HASH8,
                        "⓯ HASH 2（RAM 槽）") == live else s.fail)("⓯ HASH 2 == LIVEHASH")

    img = s.capture("task-12-projects-used")
    s.probe(img, "⓯ 卡 2 选中底（SAVE 后）", PROJ_CARD_X0 + 2 * PROJ_CARD_PITCH + 40,
            72 + PROJ_CARD_Y + 200, C_PANEL_SEL, 8)
    s.probe(img, "⓯ 卡 0 未选中底（SAVE 后）", PROJ_CARD_X0 + 40, 72 + PROJ_CARD_Y + 200,
            C_PANEL, 8)
    s.probe(img, "⓯ 卡 2 选中描边（SAVE 后）", PROJ_CARD_X0 + 2 * PROJ_CARD_PITCH + 100,
            72 + PROJ_CARD_Y - 3, C_YELLOW)
    s.probe_region(img, "⓯ 状态行文本（SAVE OK 绿字）", FILE_STATUS_BOX, C_GREEN, 24, 20)
    s.probe_region(img, f"⓯ 卡 2 详情区文本（USED + hash 前 4 位）",
                   (PROJ_CARD_X0 + 2 * PROJ_CARD_PITCH + 8, 72 + PROJ_CARD_Y + 76,
                    PROJ_CARD_X0 + 2 * PROJ_CARD_PITCH + 170, 72 + PROJ_CARD_Y + 130),
                   C_TEXT, 24, 40)

    # ---------- TAP LOAD：改值后回到保存时的引擎状态 ----------
    other = 120 if b0 != 120 else 100
    store_cmd(s, f"PARAM GLOBAL 0 {other}", rf"^PARAM GLOBAL id=0 value={other} applied={other} ",
              f"⓯ 改 BPM={other}（LOAD 的判别力）")
    (s.ok if store_bpm(s) == other else s.fail)(f"⓯ STATE SNAP bpm={other}")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⓯ 改值后 LIVEHASH") != live else s.fail)(
        "⓯ 改值后 LIVEHASH ≠ 保存值（LOAD 有判别力）")
    file_action(s, "projects", proj_btn_xy(2, 1), 2, 1, "LOAD OK", "⓯ TAP LOAD（卡 2）→ LOAD OK")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⓯ LOAD 后 LIVEHASH") == live else s.fail)(
        f"⓯ TAP LOAD 后 LIVEHASH == 保存值 {live:08X}")
    (s.ok if store_bpm_settled(s, b0) else s.fail)(
        f"⓯ STATE SNAP bpm={b0}（LOAD 恢复保存时的值）")

    # ---------- TAP ERASE：槽空、引擎不变 ----------
    file_action(s, "projects", proj_btn_xy(2, 2), 2, 2, "ERASE OK", "⓯ TAP ERASE（卡 2）→ ERASE OK")
    store_cmd(s, "STORE PROJ LIST", r"^PROJ 2 EMPTY\r?$", "⓯ TAP ERASE 后槽 2 EMPTY")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⓯ ERASE 后 LIVEHASH") == live else s.fail)(
        "⓯ ERASE 不动引擎（LIVEHASH 不变）")

    # ---------- 键盘：←/→ 选槽 + ENTER = LOAD（从「无选中」开始） ----------
    # 槽 3 存一份（TAP 卡 3 的 SAVE）→ 离开并重进 FILE（组作用域重建把选中态复位）→
    # 改 BPM → `→`×4（无选中 → A→B→C→D）+ ENTER。若 → 没移动选中槽，ENTER 不动作
    # ⇒ LIVEHASH 停在改后的值（判别力来自「选中槽必须移动」）。
    file_action(s, "projects", proj_btn_xy(3, 0), 3, 0, "SAVE OK", "⓯ TAP SAVE（卡 3）→ SAVE OK")
    live3 = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8, "⓯ 槽 3 的 LIVEHASH")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⓯ 离开 FILE（重建后选中态复位）")
    s.cmd("UI projects")
    s.expect(r"UI page=projects \(9\)", "⓯ 重进 PROJECTS（无选中槽）")
    store_cmd(s, f"PARAM GLOBAL 0 {other}", rf"^PARAM GLOBAL id=0 value={other} applied={other} ",
              f"⓯ 改 BPM={other}（键盘 LOAD 的判别力）")
    file_keys(s, 0x4C, 4, "⓯ 键盘 → ×4（无选中 → A→B→C→D）")
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⓯ 键盘 ENTER 后 LIVEHASH") == live3 else s.fail)(
        f"⓯ 键盘 ENTER = LOAD：LIVEHASH 回到槽 3 的 {live3:08X}（选中槽已随 → 移动）")
    (s.ok if store_bpm_settled(s, b0) else s.fail)(f"⓯ 键盘 LOAD 后 bpm={b0}")

    # ---------- 播放中：SAVE 与 LOAD 都被拒（STOP BEFORE …），存储逐字节不变 ----------
    # project LOAD 会整份替换引擎状态并停走带——storage 层用 store_playing() 拒绝
    # （与 SAVE/ERASE 同序）；PRESETS 的 LOAD 是例外（下一段覆盖）。
    ensure_stopped(s)
    key_space(s)
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⓯ KEY space → playing=1")
    store_cmd(s, f"PARAM GLOBAL 0 {other}", rf"^PARAM GLOBAL id=0 value={other} applied={other} ",
              f"⓯ 播放中改 BPM={other}（拒绝的判别力：LOAD 若被放行 bpm 会变回）")
    live_play = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                           "⓯ 播放中的 LIVEHASH（= 改值后）")
    (s.ok if live_play != live3 else s.fail)(
        "⓯ 播放中改值后 LIVEHASH ≠ 槽 3（被拒的 LOAD 有判别力）")
    before_list = store_proj_slots(s)
    file_action(s, "projects", proj_btn_xy(3, 1), 3, 1, "STOP BEFORE LOAD",
                "⓯ 播放中 TAP LOAD → STOP BEFORE LOAD")
    (s.ok if store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                        "⓯ 被拒的 LOAD 后 LIVEHASH") == live_play else s.fail)(
        "⓯ 被拒的 LOAD 不动引擎（LIVEHASH 不变）")
    file_action(s, "projects", proj_btn_xy(3, 0), 3, 0, "STOP BEFORE SAVE",
                "⓯ 播放中 TAP SAVE → STOP BEFORE SAVE")
    after_list = store_proj_slots(s)
    (s.ok if after_list == before_list else s.fail)(
        f"⓯ 播放中被拒后 PROJ LIST 逐槽不变（{before_list} vs {after_list}）")
    (s.ok if after_list.get(3, "").startswith("USED") else s.fail)(
        "⓯ 播放中被拒的 SAVE 没有改写槽 3（仍 USED）")
    img = s.capture("task-12-save-playing")
    s.probe_region(img, "⓯ 状态行 STOP BEFORE SAVE（UI_ERROR 红字）", FILE_STATUS_BOX,
                   C_ERROR, 24, 20)
    key_space(s)
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⓯ 再按 space → playing=0（收口）")
    file_action(s, "projects", proj_btn_xy(3, 2), 3, 2, "ERASE OK", "⓯ 收口 TAP ERASE（卡 3）")

    # ---------- PRESETS 页：32 槽网格 / SAVE / LOAD / ERASE ----------
    s.cmd("UI presets")
    s.expect(r"UI page=presets \(10\)", "⓯ UI presets（同组切页）")
    mem = param_mem(s)
    gr = mem["groups"]["FILE"]
    (s.ok if gr["built"] == 2 and mem["pages"].get(9, ("?",))[0] == "built" else s.fail)(
        f"⓯ 同组切页不重建兄弟页：FILE built={gr['built']} PROJECTS="
        f"{mem['pages'].get(9, ('?',))[0]}（期望 2 / built）")
    pg9, pg10 = mem["pages"].get(9, ("?", 0, 0)), mem["pages"].get(10, ("?", 0, 0))
    (s.ok if pg10[0] == "built" and 0 < pg10[2] <= pg10[1] and pg9[2] <= pg9[1] else s.fail)(
        f"⓯ 两页 delta ≤ est：PROJECTS {pg9[2]}/{pg9[1]}、PRESETS {pg10[2]}/{pg10[1]}")
    (s.ok if gr["sum_delta"] <= gr["sum_est"] else s.fail)(
        f"⓯ FILE sum_delta={gr['sum_delta']} ≤ sum_est={gr['sum_est']}")

    # 点槽 3（选中描边）+ TAP SAVE → PRESET 3 USED ANALOG ANALOG 04
    s.cmd("TAP %d %d" % pre_cell_xy(3))
    s.expect(r"-> FILE presets slot=3 select", "⓯ TAP PRESETS 槽 3 设选中槽")
    file_action(s, "presets", pre_btn_xy(1), 3, 0, "SAVE OK", "⓯ TAP SAVE（PRESETS）→ SAVE OK")
    used, seen, count = preset_list(s)
    (s.ok if used.get(3) == ("ANALOG", "ANALOG 04") else s.fail)(
        f"⓯ TAP SAVE 后 LIST PRESET 3 USED ANALOG ANALOG 04（实得 {used.get(3)}）")
    (s.ok if count == (1, STORE_PRESET_SLOTS) else s.fail)(
        f"⓯ PRESET COUNT used=1（实得 {count}）")

    img = s.capture("task-12-presets")
    s.probe(img, "⓯ 槽 3 选中底", *pre_cell_xy(3), C_PANEL_SEL, 8)
    s.probe(img, "⓯ 槽 0 未选中底", *pre_cell_xy(0), C_PANEL, 8)
    s.probe(img, "⓯ LOAD 按钮底", *pre_btn_probe(0), C_YELLOW)
    s.probe(img, "⓯ SAVE 按钮底", *pre_btn_probe(1), C_GREEN)
    s.probe(img, "⓯ ERASE 按钮底", *pre_btn_probe(2), C_ORANGE)
    s.probe(img, "⓯ 子签选中（PRESETS）", *C_TAB_PRESETS, C_YELLOW)
    s.probe_region(img, "⓯ 槽 3 文本（U04 + 引擎名）",
                   (pre_cell_xy(3)[0] - 60, pre_cell_xy(3)[1] - 26,
                    pre_cell_xy(3)[0] + 60, pre_cell_xy(3)[1] + 26), C_TEXT, 24, 40)
    s.probe_region(img, "⓯ 状态行文本（SAVE OK 绿字）", FILE_STATUS_BOX, C_GREEN, 24, 20)

    # 换引擎 → TAP LOAD 恢复预置里的引擎/预置（LOAD 落选中轨 = 轨 0）
    s.cmd("ENG TRACK 0 1 0")
    s.expect(r"ENG TRACK k=0 engine=1 preset=0 OK", "⓯ 换引擎 DIGITAL（LOAD 的判别力）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=1 ", "⓯ ENG SNAP 轨 0 engine=1")
    file_action(s, "presets", pre_btn_xy(0), 3, 1, "LOAD OK", "⓯ TAP LOAD（PRESETS）→ LOAD OK")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 ", "⓯ TAP LOAD 后轨 0 回到 ANALOG（预置里的引擎）")

    # 键盘：选中槽归零 → → ×3 + ENTER 也载入槽 3（先再换一次引擎作判别）
    s.cmd("TAP %d %d" % pre_cell_xy(0))
    s.expect(r"-> FILE presets slot=0 select", "⓯ TAP 槽 0 归零")
    s.cmd("ENG TRACK 0 3 0")
    s.expect(r"ENG TRACK k=0 engine=3 preset=0 OK", "⓯ 换引擎 LOFI（键盘 LOAD 的判别力）")
    file_keys(s, 0x4C, 3, "⓯ PRESETS 键盘 → ×3（槽 0→3）")
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 ",
             "⓯ PRESETS 键盘 ENTER = LOAD（轨 0 回到 ANALOG）")

    # ERASE：槽空
    file_action(s, "presets", pre_btn_xy(2), 3, 2, "ERASE OK", "⓯ TAP ERASE（PRESETS）→ ERASE OK")
    used, _, count = preset_list(s)
    (s.ok if 3 not in used and count == (0, STORE_PRESET_SLOTS) else s.fail)(
        f"⓯ TAP ERASE 后 PRESET 3 EMPTY / COUNT used=0（实得 {used.get(3)} / {count}）")

    # 鼓轨：SAVE/LOAD 都被拒（status=DRUM），存储不变。选中轨 = 3 用 SEL 层 Ctrl+4
    # （与物理键盘同入口，不离开 FILE 页）。层键按下/松开成对。
    s.cmd("KEY 0x40 down")
    s.cmd("KEY 0x04 down")
    s.cmd("KEY 0x04 up")
    s.cmd("KEY 0x40 up")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 3, "⓯ SEL 层 Ctrl+4 → 选中鼓轨")
    before_list = preset_list(s)[0]
    file_action(s, "presets", pre_btn_xy(1), 3, 0, "DRUM TRACK: NO SOUND",
                "⓯ 鼓轨 TAP SAVE → DRUM TRACK: NO SOUND")
    file_action(s, "presets", pre_btn_xy(0), 3, 1, "DRUM TRACK: NO SOUND",
                "⓯ 鼓轨 TAP LOAD → DRUM TRACK: NO SOUND")
    (s.ok if preset_list(s)[0] == before_list else s.fail)(
        "⓯ 鼓轨拒绝后 PRESET LIST 不变（没有写入）")
    s.cmd("KEY 0x40 down")
    s.cmd("KEY 0x01 down")
    s.cmd("KEY 0x01 up")
    s.cmd("KEY 0x40 up")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 0, "⓯ SEL 层 Ctrl+1 → 选中轨 0")

    # 播放中：PRESETS 的 LOAD **允许**（up_load_to 只读 flash、panic 该轨），SAVE 拒绝。
    # 先存回槽 3（上一段 ERASE 过），再在播放中换引擎 → LOAD 应生效。
    ensure_stopped(s)
    file_action(s, "presets", pre_btn_xy(1), 3, 0, "SAVE OK", "⓯ 播放前 SAVE PRESET 3（下面播放中 LOAD 用）")
    before_list = preset_list(s)[0]
    key_space(s)
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⓯ PRESETS：KEY space → playing=1")
    s.cmd("ENG TRACK 0 1 0")
    s.expect(r"ENG TRACK k=0 engine=1 preset=0 OK", "⓯ 播放中换引擎 DIGITAL")
    file_action(s, "presets", pre_btn_xy(0), 3, 1, "LOAD OK",
                "⓯ 播放中 TAP LOAD（PRESETS）→ LOAD OK（不写 flash，上游 MIDI PC 语义）")
    s.cmd("ENG SNAP")
    s.expect(r"ENG SNAP trk k=0 engine=0 ", "⓯ 播放中 LOAD 生效：轨 0 回到 ANALOG")
    file_action(s, "presets", pre_btn_xy(1), 3, 0, "STOP BEFORE SAVE",
                "⓯ 播放中 TAP SAVE（PRESETS）→ STOP BEFORE SAVE")
    (s.ok if preset_list(s)[0] == before_list else s.fail)(
        "⓯ 播放中被拒后 PRESET LIST 不变")
    key_space(s)
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⓯ PRESETS：再按 space → playing=0（收口）")

    # ---------- 过渡峰值（ADR-005 §5）与组作用域 ----------
    # FILE 两页全建 → 跨组首建 STEP：构建日志里「池剩余」是销毁 FILE 之前的 free
    # （峰值），必须 ≥ 4KB 的弧线 mask 余量（ADR-005 §5 的不变量口径）。
    s.ser.note("⓯ 过渡峰值：FILE 两页全建 → 跨组首建 STEP（销毁源组之前的 free）")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⓯ 跨组 FILE→SEQ 首建 STEP")
    m = re.search(r"页面构建 page=STEP\(2\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B", s.last)
    if m is None:
        raise InjectError(f"⓯ 缺少 STEP 构建日志行（无法量过渡峰值）：{s.last[-400:]!r}")
    peak_delta, step_est, peak_free = (int(g) for g in m.groups())
    (s.ok if peak_free >= 4096 else s.fail)(
        f"⓯ 过渡峰值 free={peak_free}B ≥4096B（STEP 首建增量 {peak_delta}B / est {step_est}B；"
        f"销毁 FILE 之前）")
    mem = param_mem(s)
    gr = mem["groups"]["FILE"]
    (s.ok if gr["built"] == 0 and gr["sum_delta"] == 0 else s.fail)(
        f"⓯ 离开 FILE 后 built={gr['built']} sum_delta={gr['sum_delta']}（期望 0 / 0）")
    (s.ok if mem["pages"].get(9, ("?",))[0] == "unbuilt"
     and mem["pages"].get(10, ("?",))[0] == "unbuilt" else s.fail)(
        f"⓯ 离开 FILE 后 PROJECTS={mem['pages'].get(9, ('?',))[0]} "
        f"PRESETS={mem['pages'].get(10, ('?',))[0]}（期望 unbuilt / unbuilt）")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⓯ 复位 PLAY")

    # 收口：四槽 + 预置槽全空、轨 0 回 ANALOG（脚本可重复、下一段/下一轮起点确定）
    for i in range(4):
        proj_erase(s, i, f"⓯ 收口 ERASE PROJ {i}")
    for i in (0, 1, 3):
        store_cmd(s, f"STORE PRESET ERASE {i}", rf"^PRESET {i} ERASED\r?$",
                  f"⓯ 收口 ERASE PRESET {i}")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⓯ 收口轨 0 = ANALOG 预置 0")


def run_store_ui_bank(s: Session) -> None:
    """⓰ SOUND 页的 bank 存取行（Task 13；P4-F Task 5 起随 FM6 引擎出现在 SOUND 页底部）：
    TAP 与 console 命令走**同一动作函数**的证明。

    判据链（不是「页面画出来了」）：
      - **行缺席（A10）**：选中轨 0（ANALOG）时 SOUND 页没有 bank 行——bank 带内 SAVE（绿）/
        ERASE（橙）按钮色为 0（`bank_row_absent_probes`；5B-1 仪表盘页上该带被卡片视口盖住，
        TAP 会落在卡片上，所以不发 TAP）；鼓轨同理（行只在「合成轨 + 引擎 FM6」时出现，
        比原 R36 的页面拒绝更强）；
      - `UI sound`（选中轨 1 = FM6 ⇒ 行出现）+ `UI MEM`：SOUND `delta ≤ est`、EDIT 组
        built=1；截屏抽检 BANK 行的标签/槽号/SAVE(绿)/ERASE(橙) 像素；
      - 目标槽：`−` 从 B1 → B27（环绕）、`+` 回 B1，再 `+`×3 → B4；槽 ± 序列**前后**
        各读一次 `PARAM 70`（PTCH = 音源选择）并断言相等——± 只改**目标槽**（单次
        前后对照，不是每步都读）；
      - `TAP BANK SAVE` ⇒ 回显 `status="SAVE OK"`、`STORE BANK GET 3` == `STORE BANK LIVE`
        （按钮发的 CMD_BANK_PUT 与 console 的 PUT 同一存储入口，字节一致）；
      - `TAP BANK ERASE` ⇒ `status="ERASE OK"`、`GET 3` → `STORE ERR empty`；
      - 播放中 `TAP BANK SAVE` ⇒ `status="STOP BEFORE SAVE"` 且槽**保持空**
        （先 ERASE 再 PLAY：若拒绝被绕过，这次 SAVE 会写出一个非空槽——有判别力）。

    关于「非 FM6 轨的 patch」：每个合成轨都有 `fm6_patch` 槽（上电是 INIT VOICE，之后是
    该轨最后一次载入的 patch），所以旧行为下锁定页 SAVE 会把这份 patch 写进用户槽；A10
    让整行只在 FM6 引擎时出现（TAP 不命中），本段因此先归一到 FM6 工厂预置 0
    （`bank_normalize_fm6`）再做 SAVE——此时 LIVE 才是真实的工厂 patch。另注意 FM6 的 8
    个 EDIT 值是宏（上游 `fm6_sync` 叠加在 patch 上）、不写回 `fm6_patch`，故 `PARAM 63 0`
    不影响 LIVE/GET 的字节比较。
    """
    restored = False
    try:
        s.ser.note("⓰ SOUND 页 bank 存取行：行缺席（按钮色缺席） → UI sound → 槽环绕/±/SAVE/ERASE → 播放拒绝 + 截屏")
        store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⓰ STORE AUTO OFF（本段不写 autosave）")
        ensure_stopped(s)

        # bank 归一（脚本可重复）：已用槽全部擦掉 → COUNT used=0
        used, _ = bank_list(s)
        for k in sorted(used):
            if used[k]:
                store_cmd(s, f"STORE BANK ERASE {k}", rf"^BANK {k} ERASED\r?$",
                          f"⓰ 归一 ERASE {k}")
        used, count = bank_list(s)
        (s.ok if not any(used.values()) and count == (0, BANK_SLOTS) else s.fail)(
            f"⓰ 归一后 BANK LIST 全 EMPTY（实得 {sorted(k for k, v in used.items() if v)} "
            f"count={count}）")

        # ---------- A10：行缺席（选中轨不是 FM6 引擎的合成轨，含鼓轨）⇒ bank 带没有按钮 ----------
        # 选中轨 0 = ANALOG（上电/⑮ 收口状态）：SOUND 页没有 bank 行（原 R36 的「锁定页 NOT FM6」
        # 页面拒绝态随独立 FM6 页删除）。5B-1 仪表盘页上这一带被卡片视口盖住，TAP 会落在卡片上，
        # 故不发 TAP；行缺席由像素判定（bank_row_absent_probes），下面只对 FM6 轨发 SAVE/ERASE。
        select_track(s, 0)
        s.cmd("UI sound")
        s.expect(r"UI page=sound \(6\)", "⓰ UI sound（轨 0 是 ANALOG ⇒ 没有 bank 行）")
        before = bank_list(s)[0]
        img = s.capture("task5-sound-bank-absent")
        bank_row_absent_probes(s, img, "⓰ ANALOG 轨")
        (s.ok if bank_list(s)[0] == before else s.fail)(
            "⓰ 行缺席段前后 BANK LIST 逐槽不变")

        # 鼓轨（选中轨 3）同样没有 bank 行（行只在「合成轨 + 引擎 FM6」时出现）
        select_track(s, 3)
        s.cmd("UI sound")
        s.expect(r"UI page=sound \(6\)", "⓰ UI sound（轨 3 是鼓轨 ⇒ 没有 bank 行）")
        img = s.capture("task5-sound-bank-absent-drum")
        bank_row_absent_probes(s, img, "⓰ 鼓轨")

        # ---------- 解锁：轨 1 = FM6 工厂预置 0（PTCH=0 ⇒ patch F1） ----------
        select_track(s, 1)
        bank_normalize_fm6(s, 0)  # 轨 1：DIGITAL → FM6 预置 0
        # 环顶像素探针前提：ALG=0（宏值非 0 时指示弧会盖住 12 点方向；与 params 场景同口径）。
        # 宏不写回 fm6_patch ⇒ 不影响下面的 LIVE / GET 字节比较。
        s.cmd("PARAM 63 0")
        s.expect(r"PARAM track=1 id=63 value=0 applied=0", "⓰ 归一 FM6 ALG=0（环顶探针前提）")
        live = bank_live(s)
        (s.ok if set(live) != {"0"} and set(live) != {"0", "F"} else s.fail)(
            f"⓰ LIVE 是真实的 FM6 工厂 patch（非全 0/全 F，{live[:16]}…）")
        s.cmd(f"PARAM {BANK_PTCH_ID}")
        pit0 = param_value(s)  # PTCH（音源）基线

        # ---------- 页面：FM6 引擎 ⇒ bank 行出现 + 截屏 ----------
        s.cmd("UI sound")
        s.expect(r"UI page=sound \(6\)", "⓰ UI sound（轨 1 是 FM6 ⇒ bank 行出现）")
        mem = param_mem(s)
        st, est, delta = mem["pages"].get(6, ("?", 0, 0))
        (s.ok if st == "built" and 0 < delta <= est else s.fail)(
            f"⓰ SOUND state={st} delta={delta} est={est}（bank 行建入，0<delta≤est）")
        (s.ok if mem["groups"]["EDIT"]["built"] == 1 else s.fail)(
            f"⓰ EDIT built={mem['groups']['EDIT']['built']}（期望 1：只有 SOUND）")
        # 仪表盘没有 chip 行也没有 8 槽旋钮：裸读恒回 chip=255、不打逐槽行（卡片槽的机读口是 UI SOUND CARD/SLOT）
        s.cmd("UI SOUND CHIP")
        (s.ok if re.search(r"UI sound chip=255\r?\n(?!UI sound slot=)", s.last) else s.fail)(
            "⓰ UI SOUND CHIP 只读回 chip=255、无逐槽行（SOUND 仪表盘无 chip 行）")

        img = s.capture("task5-sound-bank")
        s.probe(img, "⓰ BANK 行 SAVE 按钮底（绿）", *bank_btn_probe(0), C_GREEN)
        s.probe(img, "⓰ BANK 行 ERASE 按钮底（橙）", *bank_btn_probe(2), C_ORANGE)
        s.probe_region(img, "⓰ BANK 行目标槽文本（B1，UI_ACCENT）",
                       (BANK_VAL_X, BANK_ROW_Y, BANK_VAL_X + BANK_VAL_W, BANK_ROW_Y + BANK_ROW_H),
                       C_YELLOW, 16, 8)
        s.probe_region(img, "⓰ BANK 行标签文本（BANK 灰字）",
                       (BANK_LABEL_X, BANK_ROW_Y, BANK_LABEL_X + BANK_LABEL_W,
                        BANK_ROW_Y + BANK_ROW_H), C_TEXT_DIM, 16, 24)
        # 旋钮区仍在原位（bank 行不挤压旋钮）：旋钮 0 环顶探针（与 params 场景同点；
        # 已归一 ALG=0 ⇒ 指示弧不盖 12 点方向）
        s.probe(img, "⓰ 旋钮 0 弧线底（环顶，未被 bank 行挤压）", 166, 158, C_PANEL_HI, 10)

        # ---------- 目标槽：B1 → B27（− 环绕）→ B1（+ 环绕）→ B4；PTCH 不变 ----------
        s.cmd("TAP %d %d" % bank_slot_btn_xy(-1))
        s.expect(r"-> BANK slot=26 delta=-1", "⓰ TAP BANK −（B1 → B27，环绕）")
        s.cmd("TAP %d %d" % bank_slot_btn_xy(+1))
        s.expect(r"-> BANK slot=0 delta=\+1", "⓰ TAP BANK +（B27 → B1，环绕）")
        for k in range(1, 4):
            s.cmd("TAP %d %d" % bank_slot_btn_xy(+1))
            s.expect(rf"-> BANK slot={k} delta=\+1", f"⓰ TAP BANK +（{k}/3 → slot {k}）")
        s.cmd("TAP %d %d" % bank_slot_btn_xy(-1))
        s.expect(r"-> BANK slot=2 delta=-1", "⓰ TAP BANK −（3 → 2）")
        s.cmd("TAP %d %d" % bank_slot_btn_xy(+1))
        s.expect(r"-> BANK slot=3 delta=\+1", "⓰ TAP BANK +（2 → 3，回到目标槽）")
        param_expect_value(s, f"PARAM {BANK_PTCH_ID}", pit0,
                           "⓰ 槽 ± 序列前后 PTCH 不变（PARAM 70）")

        # ---------- TAP SAVE → 槽 3 == 当前 live patch ----------
        s.cmd("TAP %d %d" % bank_act_xy(0))
        s.expect(r'-> BANK slot=3 action=save status="SAVE OK"',
                 "⓰ TAP BANK SAVE → SAVE OK")
        (s.ok if bank_get(s, 3) == live else s.fail)(
            "⓰ TAP SAVE 后 GET 3 == 保存前的 LIVE（字节一致）")
        (s.ok if bank_live(s) == live else s.fail)("⓰ SAVE 不动 live patch（LIVE 不变）")
        used, count = bank_list(s)
        (s.ok if used.get(3) and count == (1, BANK_SLOTS) else s.fail)(
            f"⓰ LIST → B3 USED、COUNT used=1（实得 {sorted(k for k, v in used.items() if v)} "
            f"count={count}）")

        img = s.capture("task5-sound-bank-saved")
        s.probe_region(img, "⓰ 状态行文本（SAVE OK 绿字）",
                       (600, BANK_ROW_Y, 1000, BANK_ROW_Y + BANK_ROW_H), C_GREEN, 24, 20)
        s.probe_region(img, "⓰ 目标槽文本（B4）",
                       (BANK_VAL_X, BANK_ROW_Y, BANK_VAL_X + BANK_VAL_W, BANK_ROW_Y + BANK_ROW_H),
                       C_YELLOW, 16, 8)

        # ---------- TAP ERASE → 槽空 ----------
        s.cmd("TAP %d %d" % bank_act_xy(2))
        s.expect(r'-> BANK slot=3 action=erase status="ERASE OK"',
                 "⓰ TAP BANK ERASE → ERASE OK")
        store_cmd(s, "STORE BANK GET 3", r"^STORE ERR empty\r?$", "⓰ TAP ERASE 后 GET 3 → empty")
        used, count = bank_list(s)
        (s.ok if not used.get(3) and count == (0, BANK_SLOTS) else s.fail)(
            f"⓰ TAP ERASE 后 B3 EMPTY、COUNT used=0（实得 count={count}）")

        # ---------- 播放中 SAVE：拒绝且槽保持空（判别力：先 ERASE 再 PLAY） ----------
        # 槽 3 此刻是空的；若「播放中拒绝」被绕过，这次 SAVE 会写出一个非空槽。
        key_space(s)
        s.cmd("STATE SNAP")
        s.expect_int(r"playing=(\d)", 1, "⓰ KEY space → playing=1")
        s.cmd("TAP %d %d" % bank_act_xy(0))
        s.expect(r'-> BANK slot=3 action=save status="STOP BEFORE SAVE"',
                 "⓰ 播放中 TAP BANK SAVE → STOP BEFORE SAVE")
        store_cmd(s, "STORE BANK GET 3", r"^STORE ERR empty\r?$",
                  "⓰ 播放中被拒的 SAVE 没写槽 3（GET 3 → empty）")
        used, count = bank_list(s)
        (s.ok if not any(used.values()) and count == (0, BANK_SLOTS) else s.fail)(
            f"⓰ 播放中被拒后 BANK 仍全 EMPTY（实得 count={count}）")
        img = s.capture("task5-sound-bank-playing")
        s.probe_region(img, "⓰ 状态行 STOP BEFORE SAVE（UI_ERROR 红字）",
                       (600, BANK_ROW_Y, 1000, BANK_ROW_Y + BANK_ROW_H), C_ERROR, 24, 20)
        key_space(s)
        s.cmd("STATE SNAP")
        s.expect_int(r"playing=(\d)", 0, "⓰ 再按 space → playing=0（收口）")

        # 收口：bank 空、轨 0/1 回 ANALOG、选中轨 0、PLAY；最后恢复自动存档（默认 ON）
        used, _ = bank_list(s)
        for k in sorted(used):
            if used[k]:
                store_cmd(s, f"STORE BANK ERASE {k}", rf"^BANK {k} ERASED\r?$",
                          f"⓰ 收口 ERASE {k}")
        used, count = bank_list(s)
        (s.ok if not any(used.values()) and count == (0, BANK_SLOTS) else s.fail)(
            f"⓰ 收口后 BANK 全 EMPTY（count={count}）")
        s.cmd("ENG TRACK 1 0 0")
        s.expect(r"ENG TRACK k=1 engine=0 preset=0 OK", "⓰ 收口轨 1 = ANALOG 预置 0")
        select_track(s, 0)
        s.cmd("ENG TRACK 0 0 0")
        s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⓰ 收口轨 0 = ANALOG 预置 0")
        s.cmd("UI play")
        s.expect(r"UI page=play \(0\)", "⓰ 复位 UI play")
        # 自动存档恢复默认 ON（store.sh 其余段为了确定性把它关掉；后续场景/下一轮按默认跑）
        store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⓰ 收口 STORE AUTO ON（恢复默认）")
        restored = True
    finally:
        # 异常中止也要把自动存档开关恢复默认（后续场景/下一轮按默认跑）；
        # 正常路径已在收口断言过 STORE AUTO on，这里不重复。
        if not restored:
            try:
                s.cmd("STORE AUTO ON")
            except (InjectError, OSError):  # 会话已坏时尽力而为，不掩盖原始异常
                pass


# ---- song 场景（P4-C Task 11）：SONG 编排页 + 六组导航 + 组预算 ----
# 几何与 page_song.c 的布局宏**同源**（页内相对坐标；屏幕绝对 y = 页内 + 72 页签条）。
# 卡/芯/条目的命中矩形、选中描边都由同一组宏派生——页面改布局时这里必须同步。
SONG_CARD_X0, SONG_CARD_Y = 24, 96
SONG_CARD_W, SONG_CARD_H, SONG_CARD_PITCH = 288, 104, 304
SONG_SEC_Y, SONG_SEC_X0, SONG_SEC_PITCH = 216, 24, 148
SONG_MODE_X0 = 640
SONG_CHIP_W, SONG_CHIP_H = 140, 48
SONG_ENTRY_X0, SONG_ENTRY_Y0 = 24, 300
SONG_ENTRY_W, SONG_ENTRY_H = 148, 60
SONG_ENTRY_PITCH_X, SONG_ENTRY_PITCH_Y = 153, 70
SONG_CFG_X0, SONG_CFG_Y = 24, 456
SONG_CFG_PITCH, SONG_CFG_W, SONG_CFG_H = 148, 140, 52
SONG_SEC_ACTS = ("STORE", "LOAD", "ERASE", "JUMP")
SONG_TOP_ACTS = ("SONG", "CHAIN", "CANCEL", "REC")
SONG_CFG_NAMES = ("SCENE", "BARS-", "BARS+", "COUNT-", "COUNT+", "LOOP")
SONG_EST = 12288  # 计划上限（spec §4.4）；实测 delta 必须 ≤ 它，不得反向抬 est


def song_card_xy(k: int) -> tuple:
    return (SONG_CARD_X0 + k * SONG_CARD_PITCH + SONG_CARD_W // 2,
            72 + SONG_CARD_Y + SONG_CARD_H // 2)


def song_sec_xy(i: int) -> tuple:
    return (SONG_SEC_X0 + i * SONG_SEC_PITCH + SONG_CHIP_W // 2,
            72 + SONG_SEC_Y + SONG_CHIP_H // 2)


def song_top_xy(i: int) -> tuple:
    return (SONG_MODE_X0 + i * SONG_SEC_PITCH + SONG_CHIP_W // 2,
            72 + SONG_SEC_Y + SONG_CHIP_H // 2)


def song_entry_xy(i: int) -> tuple:
    r, c = divmod(i, 8)
    return (SONG_ENTRY_X0 + c * SONG_ENTRY_PITCH_X + SONG_ENTRY_W // 2,
            72 + SONG_ENTRY_Y0 + r * SONG_ENTRY_PITCH_Y + SONG_ENTRY_H // 2)


def song_cfg_xy(i: int) -> tuple:
    return (SONG_CFG_X0 + i * SONG_CFG_PITCH + SONG_CFG_W // 2,
            72 + SONG_CFG_Y + SONG_CFG_H // 2)


def song_action(s: Session, xy: tuple, name: str, label: str, want: str = None,
                entry: int = None, section: str = None, err: str = None) -> None:
    """TAP 一个 SONG 页控件并断言回显（动作名 + status 文本与屏幕同源）。

    `want` = 期望的 status 文本片段（如 `STORE A: OK`）；`err` = 期望的 ERR 令牌。"""
    s.cmd("TAP %d %d" % xy)
    prefix = rf"TAP \d+,\d+ page=11 -> SONG {re.escape(name)}"
    if entry is not None:
        prefix += rf" entry={entry}"
    if section is not None:
        prefix += rf" section={section}"
    if err is not None:
        s.expect(prefix + rf" ERR {err} .*status=\"", label)
    else:
        s.expect(prefix + r" value=-?\d+ status=\"", label)
    if want is not None:
        s.expect(r'status="[^"]*' + re.escape(want) + r'[^"]*"', label + f" → status {want}")


def song_cfg(s: Session) -> dict:
    """`STORE SONG` 解析：count/loop + 每项 scene/bars。"""
    s.cmd("STORE SONG")
    m = re.search(r"STORE SONG count=(\d+) loop=(\d+) dirty=(\d)", s.last)
    if m is None:
        raise InjectError(f"STORE SONG 回显缺少 count/loop：{s.last.strip()!r}")
    entries = {}
    for em in re.finditer(r"^SONG (\d+) scene=(\d+) bars=(\d+)\r?$", s.last, re.MULTILINE):
        entries[int(em.group(1))] = (int(em.group(2)), int(em.group(3)))
    return {"count": int(m.group(1)), "loop": int(m.group(2)), "entry": entries}


def song_section(s: Session) -> dict:
    """`SEQ SECTION` 解析：ready 位、live、播放位置。"""
    s.cmd("SEQ SECTION")
    m = re.search(r"SEQ SECTION ready=0x([0-9A-F]{2}) live=(\S+) dirty=(\d) index=(\d+) bar=(\d+)",
                  s.last)
    if m is None:
        raise InjectError(f"SEQ SECTION 回显异常：{s.last.strip()!r}")
    return {"ready": int(m.group(1), 16), "live": m.group(2), "dirty": int(m.group(3))}


def song_chain(s: Session) -> dict:
    s.cmd("SEQ CHAIN")
    m = re.search(r"SEQ CHAIN count=(\d+) index=(\d+) bars=(\d+)", s.last)
    if m is None:
        raise InjectError(f"SEQ CHAIN 回显异常：{s.last.strip()!r}")
    scenes = [int(x) for x in re.findall(r"^SEQ CHAIN \d+ scene=(\d+)\r?$", s.last, re.MULTILINE)]
    return {"count": int(m.group(1)), "index": int(m.group(2)), "bars": int(m.group(3)),
            "scenes": scenes}


def song_pool_free(s: Session) -> int:
    return nav_pool_free(s)


def song_wait_running(s: Session, want: int, timeout: float = 2.0) -> bool:
    """轮询 `SEQ SONG` 的 running 位（PLAY/STOP 请求只由渲染循环消费）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s.cmd("SEQ SONG")
        m = re.search(r"SEQ SONG mode=\w+ running=(\d)", s.last)
        if m is not None and int(m.group(1)) == want:
            return True
        time.sleep(0.05)
    return False


def song_wait_srec(s: Session, want: str, timeout: float = 4.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s.cmd("SEQ SREC")
        m = re.search(r"SEQ SREC srec=(\w+) last=(\w+)", s.last)
        if m is not None and m.group(1) == want:
            return True
        time.sleep(0.02)
    return False


def run_song(s: Session) -> None:
    """song 场景：SONG 页（A–D section 卡 / 16 项 / bars·loop / SONG·CHAIN·REC·
    STORE·ERASE）、六组导航与组预算（P4-C Task 11）。

    覆盖：
      ① `UI song`(11)/`UI TAB SONG`/`UI SUB` 单页组 + `UI MEM` 的 SONG 行与 est；
      ② 空态截屏（A–D 卡 EMPTY、16 项配置槽、状态行）；
      ③ 卡选 + STORE A/B（`SEQ SECTION ready` 与页面状态行同源）；
      ④ 16 项配置编辑（count/scene/bars/loop）**全部经 CMD_SONG_SET**：屏幕 TAP 后
         `STORE SONG` 逐字段一致；COUNT/BARS 的上下钳位；④b COUNT+ 只 seed 结构非法
         槽（8 项非 A4 tail → COUNT-4 → COUNT+5 保留项 5=C9，仅 index8 seed A4）；
      ⑤ 页面 ERASE chip 后 section/歌曲引用同步消失（arr_drop_scene）+ 列表立即刷新；
         COUNT+ 保留合法 tail（不被 A4 覆盖）；
      ⑥ live 播放中 JUMP scene B（`SEQ SECTION live=B`）+ 空 scene 的可见拒绝
         （`JUMP C: EMPTY`）；截屏「播放 scene B」；
      ⑦ quick chain：CHAIN 起链（begin 立即请求首项）、卡 tap 进临时列表、再按
         CHAIN 原子 commit（`SEQ CHAIN A B A`）；空 scene 整份拒绝（ERR not_found）；
         CANCEL 清 chain；截屏 chain 态；
      ⑧ SONG 模式 + 缺 scene 整首拒播（`playing=0`/`running=0`，READY 行可见空位）；
      ⑨ SONG REC 的 NO SONG（空 take 不覆盖歌曲）+ 截屏；
      ⑩ 组预算：SONG↔PLAY ×10、SEQ↔SONG ×10 的 pool_free 首末差 ≤128B；FILE 全建
         → 首建 STEP 的过渡峰值 ≥4,096B；
      ⑪ 收口：擦 A/B、恢复默认歌曲（A4 B4 C4 D4）与 AUTO ON、回 PLAY。

    本场景**破坏性**：占用 A/B 与歌曲顺序（收口恢复默认歌曲；A/B 擦除）。它自带归一，
    可重复运行；`STORE AUTO OFF` 防止 autosave 把中途状态写进工作工程。
    """
    s.ser.note("song 场景开始：归一（停走带 / 窗口 / AUTO OFF / 清 A–D / 默认歌曲）")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⓪ STORE AUTO OFF")
    ensure_stopped(s)
    ensure_window(s, 0, 16)
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⓪ 归一到 loop 模式")
    for k in range(4):
        proj_erase(s, k, f"⓪ 清空 section {'ABCD'[k]}")
    store_cmd(s, "STORE SONG SET 4 0 0:4 1:4 2:4 3:4",
              r"^STORE SONG SET count=4 loop=0", "⓪ 默认歌曲 A4 B4 C4 D4")
    sec = song_section(s)
    (s.ok if sec["ready"] == 0 else s.fail)(
        f"⓪ 归一后 ready=0x{sec['ready']:02X}（期望 0）")

    # ---------- ① 页面入口与组水位 ----------
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "① UI song → 页 11")
    s.cmd("UI TAB song")
    s.expect(r"UI TAB SONG page=song \(11\)", "① UI TAB song（与物理组签同一路径）")
    s.cmd("UI SUB 0")
    s.expect(r"UI ERR sub 0", "① SONG 单页组：UI SUB 0 → UI ERR sub")
    mem = param_mem(s)
    page11 = mem["pages"].get(11, ("?", 0, 0))
    (s.ok if page11[0] == "built" and page11[1] == SONG_EST and 0 < page11[2] <= SONG_EST
     else s.fail)(
        f"① SONG 页 state={page11[0]} est={page11[1]} delta={page11[2]}"
        f"（0 < delta ≤ {SONG_EST}）")
    grp = mem["groups"]["SONG"]
    (s.ok if grp["built"] == 1 and grp["sum_est"] == SONG_EST else s.fail)(
        f"① GROUP SONG built={grp['built']} sum_est={grp['sum_est']}"
        f"（期望 1 / {SONG_EST}）")
    (s.ok if 0 < grp["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"① SONG build_ms={grp['build_ms']}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")

    # ---------- ② 空态截屏 + 像素抽检 ----------
    img = s.capture("task-11-song-empty")
    for k, letter in enumerate("ABCD"):
        x0 = SONG_CARD_X0 + k * SONG_CARD_PITCH
        # 卡选中态是 s_sec 的描边+UI_PANEL_SEL 底（初始 A 选中）；顶部内缘取填充色
        want_bg = C_PANEL_SEL if k == 0 else C_PANEL
        s.probe(img, f"② 卡 {letter} 底（{'选中' if k == 0 else '未选中'}）",
                x0 + 20, 72 + SONG_CARD_Y + 12, want_bg, 14)
        s.probe_region(img, f"② 卡 {letter} 文本",
                       (x0 + 70, 72 + SONG_CARD_Y + 20, x0 + SONG_CARD_W - 70,
                        72 + SONG_CARD_Y + SONG_CARD_H - 20), C_TEXT_DIM, 12, 20)
    for i, name in enumerate(SONG_SEC_ACTS):
        x0 = SONG_SEC_X0 + i * SONG_SEC_PITCH
        s.probe(img, f"② {name} chip 底", x0 + 20, 72 + SONG_SEC_Y + 6, C_PANEL_HI, 14)
    for i, name in enumerate(SONG_TOP_ACTS):
        if name == "CANCEL":  # 非链采集态隐藏（不可见/不可命中）
            continue
        x0 = SONG_MODE_X0 + i * SONG_SEC_PITCH
        s.probe(img, f"② {name} chip 底", x0 + 20, 72 + SONG_SEC_Y + 6, C_PANEL_HI, 14)
    for i in range(16):
        x0 = SONG_ENTRY_X0 + (i % 8) * SONG_ENTRY_PITCH_X
        y0 = 72 + SONG_ENTRY_Y0 + (i // 8) * SONG_ENTRY_PITCH_Y
        want = C_PANEL_SEL if i == 0 else C_PANEL_HI  # entry 0 初始选中
        s.probe(img, f"② 条目 {i + 1:02d} chip 底", x0 + 20, y0 + 6, want, 14)
    for i, name in enumerate(SONG_CFG_NAMES):
        x0 = SONG_CFG_X0 + i * SONG_CFG_PITCH
        s.probe(img, f"② {name} chip 底", x0 + 20, 72 + SONG_CFG_Y + 6, C_PANEL_HI, 14)
    s.probe(img, "② SONG 组签选中（六组导航）", *C_TAB_SONG, C_YELLOW)

    # ---------- ③ 卡选 + STORE A/B ----------
    for k, letter in enumerate("ABCD"):
        s.cmd("TAP %d %d" % song_card_xy(k))
        s.expect(rf"-> SONG card={letter} select", f"③ TAP 卡 {letter} 设选中 section")
    s.cmd("TAP %d %d" % song_card_xy(0))
    s.expect(r"-> SONG card=A select", "③ 回到卡 A（STORE A 的落点）")
    song_action(s, song_sec_xy(0), "STORE", "③ TAP STORE(A)", "STORE A: OK", section="A")
    sec = song_section(s)
    (s.ok if sec["ready"] & 0x01 else s.fail)(f"③ STORE A 后 ready=0x{sec['ready']:02X}（bit0）")
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "③ TAP 卡 B")
    song_action(s, song_sec_xy(0), "STORE", "③ TAP STORE(B)", "STORE B: OK", section="B")
    sec = song_section(s)
    (s.ok if sec["ready"] & 0x03 == 0x03 else s.fail)(
        f"③ STORE B 后 ready=0x{sec['ready']:02X}（bit0+bit1）")

    # ---------- ④ 16 项配置：count 2 / A2 / B3 / loop ----------
    s.cmd("TAP %d %d" % song_entry_xy(15))
    s.expect(r"-> SONG entry=15 select", "④ 第 16 项存在（TAP entry=15 命中）")
    s.cmd("TAP %d %d" % song_entry_xy(0))
    s.expect(r"-> SONG entry=0 select scene=0 bars=4", "④ TAP entry 0（默认 A4）")
    song_action(s, song_cfg_xy(3), "COUNT-", "④ COUNT- → 3", "COUNT 3")
    song_action(s, song_cfg_xy(3), "COUNT-", "④ COUNT- → 2", "COUNT 2")
    cfg = song_cfg(s)
    (s.ok if cfg["count"] == 2 else s.fail)(f"④ STORE SONG count={cfg['count']}（期望 2）")
    # 第 3 项起不可见（count 之外的槽保持原值，不参与 STORE SONG 输出）
    s.cmd("TAP %d %d" % song_entry_xy(2))
    s.expect(r"-> SONG entry=2 select", "④ TAP count 外的 entry 2（选择有效、不可编辑）")
    song_action(s, song_cfg_xy(1), "BARS-", "④ count 外 BARS- 不下发", "ENTRY 3: EMPTY")
    # entry 0：BARS 4 → 2；entry 1：默认 B4 → B3（默认歌曲 A4 B4 C4 D4）
    s.cmd("TAP %d %d" % song_entry_xy(0))
    s.expect(r"-> SONG entry=0 select scene=0 bars=4", "④ 重选 entry 0（A4）")
    song_action(s, song_cfg_xy(1), "BARS-", "④ entry0 BARS- → 3", "BARS 3")
    song_action(s, song_cfg_xy(1), "BARS-", "④ entry0 BARS- → 2", "BARS 2")
    s.cmd("TAP %d %d" % song_entry_xy(1))
    s.expect(r"-> SONG entry=1 select scene=1 bars=4", "④ TAP entry 1（默认 B4）")
    song_action(s, song_cfg_xy(1), "BARS-", "④ entry1 BARS- → 3", "BARS 3")
    cfg = song_cfg(s)
    ok = (cfg["count"] == 2 and cfg["entry"].get(0) == (0, 2) and cfg["entry"].get(1) == (1, 3))
    (s.ok if ok else s.fail)(
        f"④ A2+B3 经状态层落地：count={cfg['count']} entry={cfg['entry']}（期望 2 / 0:2 / 1:3）")
    song_action(s, song_cfg_xy(5), "LOOP", "④ LOOP ON", "LOOP ON")
    (s.ok if song_cfg(s)["loop"] == 1 else s.fail)("④ STORE SONG loop=1")
    song_action(s, song_cfg_xy(5), "LOOP", "④ LOOP OFF", "LOOP OFF")
    (s.ok if song_cfg(s)["loop"] == 0 else s.fail)("④ STORE SONG loop=0")
    # 钳位：count 16 再 + 仍 16；bars 64 再 + 仍 64（引擎/状态层钳位，页面不自行造值）
    store_cmd(s, "STORE SONG SET 16 0 " + " ".join(f"{i % 4}:4" for i in range(16)),
              r"^STORE SONG SET count=16", "④ 归一 count=16")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "④ 回 SONG（同组）")
    song_action(s, song_cfg_xy(4), "COUNT+", "④ COUNT+ 在 16 钳位", "COUNT 16")
    store_cmd(s, "STORE SONG SET 2 0 0:64 1:3", r"^STORE SONG SET count=2",
              "④ 归一 count=2 / entry0 bars=64")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "④ 回 SONG（同组）")
    s.cmd("TAP %d %d" % song_entry_xy(0))
    s.expect(r"-> SONG entry=0 select scene=0 bars=64", "④ entry0 = A64")
    song_action(s, song_cfg_xy(2), "BARS+", "④ BARS+ 在 64 钳位", "BARS 64")
    store_cmd(s, "STORE SONG SET 2 0 0:2 1:3", r"^STORE SONG SET count=2",
              "④ 恢复 A2+B3")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "④ 回 SONG（同组）")
    img = s.capture("task-11-song-edit")
    # entry 0 的选中态 = UI_PANEL_SEL 底 + 4px UI_ACCENT 描边（内缘取填充色）
    s.probe(img, "④ entry0 选中底（UI_PANEL_SEL）",
            SONG_ENTRY_X0 + 20, 72 + SONG_ENTRY_Y0 + 6, C_PANEL_SEL, 14)
    s.probe_region(img, "④ 状态行文本（UI_TEXT_DIM）", (24, 72 + 20, 1000, 72 + 80),
                   C_TEXT_DIM, 16, 40)

    # ---------- ④b COUNT+ 只 seed 结构非法的槽（合法 tail 不被 A4 覆盖） ----------
    # 8 项、项 5（0 起 index 4）= C9；COUNT-4 → 4，再 COUNT+5 → 9：
    # index 4..7 的已持久化项必须原样保留，只有从未初始化的 index 8 才 seed 成 A4。
    store_cmd(s, "STORE SONG SET 8 0 0:2 1:3 2:5 3:7 2:9 3:13 1:5 0:7",
              r"^STORE SONG SET count=8", "④b 归一 8 项（项 5 = C9）")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "④b 回 SONG")
    s.cmd("TAP %d %d" % song_entry_xy(4))
    s.expect(r"-> SONG entry=4 select scene=2 bars=9", "④b 项 5 = C9（0 起 entry=4）")
    for want in (7, 6, 5, 4):
        song_action(s, song_cfg_xy(3), "COUNT-", f"④b COUNT- → {want}", f"COUNT {want}")
    for want in (5, 6, 7, 8, 9):
        song_action(s, song_cfg_xy(4), "COUNT+", f"④b COUNT+ → {want}", f"COUNT {want}")
    cfg = song_cfg(s)
    (s.ok if cfg["count"] == 9 and cfg["entry"].get(4) == (2, 9)
     and cfg["entry"].get(5) == (3, 13) and cfg["entry"].get(6) == (1, 5)
     and cfg["entry"].get(7) == (0, 7) else s.fail)(
        f"④b COUNT+ 保留合法 tail：count={cfg['count']} entry={cfg['entry']}"
        "（期望 9 / 4:(2,9) 5:(3,13) 6:(1,5) 7:(0,7)）")
    (s.ok if cfg["entry"].get(8) == (0, 4) else s.fail)(
        f"④b 只有未初始化的 index 8 seed 为 A4（实得 {cfg['entry'].get(8)}）")
    store_cmd(s, "STORE SONG SET 2 0 0:2 1:3", r"^STORE SONG SET count=2",
              "④b 恢复 A2+B3")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "④b 回 SONG（恢复 A2+B3）")

    # ---------- ⑤ ERASE chip：section 引擎真值 + 歌曲引用同步消失 ----------
    # 页面 ERASE = CMD_SECTION_ERASE（与 console `STORE PROJ ERASE` 同一存储对象）；
    # B 在歌曲里有引用 ⇒ 同时验证 arr_drop_scene 把第 2 项拿走。
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑤ 选中 B（ERASE 落点）")
    song_action(s, song_sec_xy(2), "ERASE", "⑤ TAP ERASE(B)", "ERASE B: OK", section="B")
    cfg = song_cfg(s)
    (s.ok if cfg["count"] == 1 and cfg["entry"].get(0) == (0, 2) else s.fail)(
        f"⑤ ERASE B 后歌曲 count={cfg['count']} entry={cfg['entry']}（期望 1 / 0:2）")
    sec = song_section(s)
    (s.ok if not (sec["ready"] & 0x02) else s.fail)(
        f"⑤ ERASE B 后 ready=0x{sec['ready']:02X}（bit1 清零）")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑤ 回 SONG 刷新列表")
    # 重建 B 并补回第 2 项（新增项默认 A4；再改成 B3）
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑤ 重选 B")
    song_action(s, song_sec_xy(0), "STORE", "⑤ 重建 B", "STORE B: OK", section="B")
    s.cmd("TAP %d %d" % song_entry_xy(0))
    s.expect(r"-> SONG entry=0 select scene=0 bars=2", "⑤ entry0 仍是 A2")
    song_action(s, song_cfg_xy(4), "COUNT+", "⑤ COUNT+ → 2（新项默认 A4）", "COUNT 2")
    s.cmd("TAP %d %d" % song_entry_xy(1))
    # COUNT+ 只 seed 结构非法的槽：ERASE 前的 tail（B4）仍合法，必须原样保留
    s.expect(r"-> SONG entry=1 select scene=1 bars=4", "⑤ COUNT+ 保留合法 tail B4")
    song_action(s, song_cfg_xy(1), "BARS-", "⑤ tail BARS- → 3", "BARS 3")
    cfg = song_cfg(s)
    (s.ok if cfg["count"] == 2 and cfg["entry"].get(1) == (1, 3) else s.fail)(
        f"⑤ 恢复 A2+B3：count={cfg['count']} entry={cfg['entry']}")

    # ---------- ⑤b 键盘路由：←/→ 条目、↑/↓ section、ENTER = LOAD ----------
    # 到达 SONG 页（同组已建），选中项/状态由动作回显或 SEQ SECTION 观测。
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑤b 回 SONG（键盘路由）")
    s.cmd("TAP %d %d" % song_entry_xy(0))
    s.expect(r"-> SONG entry=0 select scene=0 bars=2", "⑤b 归一到 entry 0（A2）")
    s.cmd("KEY 0x4C down")  # →：条目 0→1
    s.cmd("KEY 0x4C up")
    song_action(s, song_cfg_xy(2), "BARS+", "⑤b → 后焦点 entry 1", "ENTRY 2 BARS 4")
    song_action(s, song_cfg_xy(1), "BARS-", "⑤b entry 1 还原 B3", "ENTRY 2 BARS 3")
    s.cmd("KEY 0x4A down")  # ←：条目 1→0
    s.cmd("KEY 0x4A up")
    song_action(s, song_cfg_xy(2), "BARS+", "⑤b ← 后焦点 entry 0", "ENTRY 1 BARS 3")
    song_action(s, song_cfg_xy(1), "BARS-", "⑤b entry 0 还原 A2", "ENTRY 1 BARS 2")
    # ↑/↓ 选 section：按 ↓ 到 B → LOAD 回显 section=B 且引擎 live=B；按 ↑ 回 A。
    s.cmd("TAP %d %d" % song_card_xy(0))
    s.expect(r"-> SONG card=A select", "⑤b 归一到 section A")
    s.cmd("KEY 0x4B down")  # ↓：A→B
    s.cmd("KEY 0x4B up")
    song_action(s, song_sec_xy(1), "LOAD", "⑤b ↓ 后 LOAD 落点 B", "LOAD B: OK", section="B")
    (s.ok if song_section(s)["live"] == "B" else s.fail)("⑤b ↓ 选 section 后 live=B")
    s.cmd("KEY 0x3B down")  # ↑：B→A
    s.cmd("KEY 0x3B up")
    song_action(s, song_sec_xy(1), "LOAD", "⑤b ↑ 后 LOAD 落点 A", "LOAD A: OK", section="A")
    (s.ok if song_section(s)["live"] == "A" else s.fail)("⑤b ↑ 选 section 后 live=A")
    # ENTER = LOAD 选中 section（输入任务路径，无 TAP 回显）：选 B 后 ENTER。
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑤b 选中 B（ENTER 落点）")
    s.cmd("KEY 0x3D down")
    s.cmd("KEY 0x3D up")
    deadline = time.monotonic() + 1.0
    live = None
    while time.monotonic() < deadline:
        live = song_section(s)["live"]
        if live == "B":
            break
        time.sleep(0.05)
    (s.ok if live == "B" else s.fail)(f"⑤b ENTER → LOAD B：live={live}")
    s.cmd("TAP %d %d" % song_card_xy(0))
    s.expect(r"-> SONG card=A select", "⑤b 收口回 section A")

    # ---------- ⑥ live 播放 + JUMP scene B + 空 scene 可见拒绝 ----------
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑥ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑥ TAP PLAY（live loop）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 1, "⑥ STATE SNAP playing")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑥ 回 SONG（重建）")
    # 页面输入守卫（上游 SONG 屏语义）：走带中 MODE 与 16 项配置编辑回 STOP FIRST、
    # 不发命令；section/chain 的播放语义不受影响（紧接的 JUMP/CHAIN 仍可用）。
    cfg_before = song_cfg(s)
    for xy, name, label in ((song_cfg_xy(1), "BARS-", "⑥ 播放中 BARS-"),
                            (song_cfg_xy(0), "SCENE", "⑥ 播放中 SCENE"),
                            (song_cfg_xy(4), "COUNT+", "⑥ 播放中 COUNT+"),
                            (song_cfg_xy(5), "LOOP", "⑥ 播放中 LOOP"),
                            (song_top_xy(0), "SONG", "⑥ 播放中 MODE")):
        song_action(s, xy, name, label, "STOP FIRST", err="blocked")
    cfg_after = song_cfg(s)
    (s.ok if cfg_after == cfg_before else s.fail)(
        f"⑥ 走带中的配置编辑不改歌曲：{cfg_after} vs {cfg_before}")
    s.cmd("SEQ SONG")
    s.expect(r"SEQ SONG mode=loop", "⑥ 走带中的 MODE 未切换")
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑥ 选中 B")
    song_action(s, song_sec_xy(3), "JUMP", "⑥ TAP JUMP(B)", "JUMP B: OK", section="B")
    deadline = time.monotonic() + 3.0
    live = None
    while time.monotonic() < deadline:
        live = song_section(s)["live"]
        if live == "B":
            break
        time.sleep(0.05)
    (s.ok if live == "B" else s.fail)(f"⑥ 下一小节生效：SEQ SECTION live={live}（期望 B）")
    img = s.capture("task-11-song-live-b")
    s.probe_region(img, "⑥ 状态行含 LIVE/PLAY 文本", (24, 72 + 20, 1000, 72 + 80),
                   C_TEXT_DIM, 16, 40)
    # 空 scene：C 从未 STORE → JUMP 必须回 not_found，且 live 仍是 B（可见拒绝）
    s.cmd("TAP %d %d" % song_card_xy(2))
    s.expect(r"-> SONG card=C select", "⑥ 选中 C（空）")
    song_action(s, song_sec_xy(3), "JUMP", "⑥ JUMP 空 scene C → EMPTY", "JUMP C: EMPTY",
                section="C", err="not_found")
    (s.ok if song_section(s)["live"] == "B" else s.fail)("⑥ 空 scene 被拒后 live 仍是 B")
    img = s.capture("task-11-song-empty-jump")  # 可见拒绝：状态行红字 `JUMP C: EMPTY`
    s.probe_region(img, "⑥ 状态行拒绝文本（UI_ERROR 红字）", (24, 72 + 20, 1232, 72 + 80),
                   C_ERROR, 20, 20)

    # ---------- ⑦ quick chain：begin → 卡进列表 → commit / 空 scene 拒绝 / cancel ----------
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑦ 选 B 起链")
    song_action(s, song_top_xy(1), "CHAIN", "⑦ CHAIN begin（立即请求 B）", "CHAIN: B")
    s.cmd("TAP %d %d" % song_card_xy(0))
    s.expect(r"-> SONG card=A select", "⑦ 链中 tap A 进临时列表")
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑦ 链中 tap B 进临时列表")
    song_action(s, song_top_xy(1), "CHAIN", "⑦ CHAIN commit（原子安装）", "CHAIN: B A B")
    chain = song_chain(s)
    (s.ok if chain["count"] == 3 and chain["scenes"] == [1, 0, 1] else s.fail)(
        f"⑦ SEQ CHAIN count={chain['count']} scenes={chain['scenes']}（期望 3 / [1,0,1]）")
    img = s.capture("task-11-song-chain")
    s.probe_region(img, "⑦ 状态行 CHAIN 文本", (24, 72 + 20, 1100, 72 + 80),
                   C_TEXT_DIM, 16, 40)
    # 空 scene 整份拒绝（不安装半条 chain）：B + C(empty)
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑦ 空链用例：选 B")
    song_action(s, song_top_xy(1), "CHAIN", "⑦ 再起链 B", "CHAIN: B")
    s.cmd("TAP %d %d" % song_card_xy(2))
    s.expect(r"-> SONG card=C select", "⑦ 链中 tap C（空）")
    song_action(s, song_top_xy(1), "CHAIN", "⑦ commit 空 scene → ERR not_found",
                "CHAIN: EMPTY", err="not_found")
    chain = song_chain(s)
    (s.ok if chain["count"] == 0 else s.fail)(
        f"⑦ 空 scene 整份拒绝后 SEQ CHAIN count={chain['count']}（期望 0，不装半条）")
    # CANCEL：begin 后取消，chain 清空、已排队的 section 保留（单 tap 语义）
    s.cmd("TAP %d %d" % song_card_xy(1))
    s.expect(r"-> SONG card=B select", "⑦ 取消用例：选 B")
    song_action(s, song_top_xy(1), "CHAIN", "⑦ begin B", "CHAIN: B")
    song_action(s, song_top_xy(2), "CANCEL", "⑦ CANCEL", "CHAIN: CANCEL")
    (s.ok if song_chain(s)["count"] == 0 else s.fail)("⑦ CANCEL 后 chain count=0")

    # ---------- ⑧ SONG 模式 + 缺 scene 整首拒播 ----------
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑧ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "⑧ STOP（停止 live loop）")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑧ STATE SNAP playing=0")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑧ 回 SONG")
    song_action(s, song_top_xy(0), "SONG", "⑧ SONG 模式", "MODE: SONG")
    s.cmd("SEQ SONG")
    s.expect(r"SEQ SONG mode=song", "⑧ SEQ SONG mode=song")
    # 缺 scene：把第 2 项改成 C（空），PLAY 必须不进入播放（arrangement_start 拒绝）
    s.cmd("TAP %d %d" % song_entry_xy(1))
    s.expect(r"-> SONG entry=1 select scene=1 bars=3", "⑧ 选 entry 1（B3）")
    song_action(s, song_cfg_xy(0), "SCENE", "⑧ entry1 SCENE B→C（空）", "SCENE C")
    cfg = song_cfg(s)
    (s.ok if cfg["entry"].get(1) == (2, 3) else s.fail)(
        f"⑧ entry1 = C3（实得 {cfg['entry'].get(1)}）")
    sec = song_section(s)
    (s.ok if not (sec["ready"] & 0x04) else s.fail)(
        f"⑧ READY 行可见 C 为空：ready=0x{sec['ready']:02X}（bit2 = 0）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑧ 去 TRANSPORT 按 PLAY")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑧ TAP PLAY（缺 scene 的 SONG）")
    time.sleep(1.0)  # 渲染循环消费 transport_req；拒绝路径不得进入播放
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑧ 缺 scene 拒播：playing 仍 0")
    s.cmd("SEQ SONG")
    s.expect(r"SEQ SONG mode=song running=0", "⑧ 缺 scene 拒播：SEQ SONG running=0")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑧ 回 SONG（截缺 scene 态）")
    img = s.capture("task-11-song-empty-scene")
    s.probe_region(img, "⑧ READY 行（UI_TEXT_DIM）", (24, 72 + 20, 1100, 72 + 80),
                   C_TEXT_DIM, 16, 40)
    # 恢复 B3 与 loop 模式（后续 REC/预算用例的基线）
    s.cmd("TAP %d %d" % song_entry_xy(1))
    s.expect(r"-> SONG entry=1 select scene=2 bars=3", "⑧ 重选 entry 1（C3）")
    song_action(s, song_cfg_xy(0), "SCENE", "⑧ entry1 SCENE C→D", "SCENE D")
    song_action(s, song_cfg_xy(0), "SCENE", "⑧ entry1 SCENE D→A", "SCENE A")
    song_action(s, song_cfg_xy(0), "SCENE", "⑧ entry1 SCENE A→B", "SCENE B")
    song_action(s, song_top_xy(0), "SONG", "⑧ 回 LOOP 模式", "MODE: LOOP")
    s.cmd("SEQ SONG")
    s.expect(r"SEQ SONG mode=loop", "⑧ SEQ SONG mode=loop")

    # ---------- ⑨ SONG REC 的 NO SONG（空 take 不覆盖歌曲） ----------
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑨ 回 SONG")
    s.cmd("TAP %d %d" % song_card_xy(0))
    s.expect(r"-> SONG card=A select", "⑨ 选 A（live_sec=A 需要 LOAD）")
    song_action(s, song_sec_xy(1), "LOAD", "⑨ LOAD A（停止态，live=A）", "LOAD A: OK",
                section="A")
    (s.ok if song_section(s)["live"] == "A" else s.fail)("⑨ SEQ SECTION live=A")
    song_action(s, song_top_xy(3), "REC", "⑨ REC arm", "REC: ARMED")
    s.cmd("SEQ SREC")
    s.expect(r"SEQ SREC srec=armed", "⑨ SEQ SREC srec=armed")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑨ UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "⑨ PLAY（SONG REC 从下一小节起）")
    (s.ok if song_wait_srec(s, "recording", 4.0) else s.fail)(
        "⑨ SONG REC 进入 recording（下一个小节）")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)  # 小节内立即 STOP：srec_n=0 ⇒ NO SONG
    s.expect(r"-> TRANSPORT CMD_STOP", "⑨ STOP 收尾空 take")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d)", 0, "⑨ STOP 后 playing=0")
    s.cmd("SEQ SREC")
    s.expect(r"SEQ SREC srec=off last=no_song", "⑨ 空 take = last=no_song")
    cfg = song_cfg(s)
    (s.ok if cfg["count"] == 2 and cfg["entry"].get(1) == (1, 3) else s.fail)(
        f"⑨ NO SONG 不覆盖原歌曲：count={cfg['count']} entry={cfg['entry']}")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑨ 回 SONG")
    img = s.capture("task-11-song-nosong")
    s.probe_region(img, "⑨ 状态行 LAST NO SONG 文本", (24, 72 + 20, 1200, 72 + 80),
                   C_TEXT_DIM, 16, 40)

    # ---------- ⑩ 组预算：往返池差与最坏有序对 ----------
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑩ 起点 PLAY")
    free0 = song_pool_free(s)
    per_trip = []
    for trip in range(1, NAV_LEAK_TRIPS + 1):
        s.cmd("UI song")
        s.expect(r"UI page=song \(11\)", f"⑩ SONG↔PLAY {trip}/{NAV_LEAK_TRIPS}：UI song")
        s.cmd("UI play")
        s.expect(r"UI page=play \(0\)", f"⑩ SONG↔PLAY {trip}/{NAV_LEAK_TRIPS}：UI play")
        per_trip.append(song_pool_free(s))
    drop = free0 - per_trip[-1]
    s.ser.note(f"⑩ SONG↔PLAY pool_free：{free0} → 逐次 {per_trip}")
    (s.ok if abs(drop) <= NAV_LEAK_NOISE else s.fail)(
        f"⑩ {NAV_LEAK_TRIPS} 次 SONG↔PLAY 往返后 pool_free 变化 {free0} → {per_trip[-1]}"
        f"（Δ={-drop:+d}B，允许 |Δ| ≤ {NAV_LEAK_NOISE}B）")
    steps = [free0] + per_trip
    worst = max(steps[i] - steps[i + 1] for i in range(len(steps) - 1))
    (s.ok if worst <= NAV_LEAK_NOISE else s.fail)(
        f"⑩ 单次 SONG↔PLAY 的最大池下降 {worst}B ≤ {NAV_LEAK_NOISE}B（无逐次增长）")

    # SEQ（STEP+TRANSPORT）全建 → SONG → 回 PLAY，×10。测量点在**每组都销毁后的
    # PLAY 稳态**（与 ⑧ 的 R34 泄漏口径一致：跨组往返的净变化，排除 STEP 22K 重建
    # 的 TLSF 碎片涨落——SEQ 两页建满时的 pool_free 本身有 ±200B 的分配序噪声）。
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑩ SEQ↔SONG 起点 PLAY")
    # 预热一轮：首次把 SONG/STEP/TRANSPORT 三页的**新状态串**（STOP FIRST / RECORDING /
    # CHAIN …）交给 LVGL 会做一次性文本/字形缓存（实测 ~400B，之后逐轮稳定）；它不是
    # 逐次泄漏，不计入 10 轮判据。10 个测量轮仍从同一 PLAY 稳态出发。
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ SEQ↔SONG 预热：UI step")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ SEQ↔SONG 预热：UI transport")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑩ SEQ↔SONG 预热：UI song")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑩ SEQ↔SONG 预热：回 PLAY")
    free0 = song_pool_free(s)
    per_trip = []
    for trip in range(1, NAV_LEAK_TRIPS + 1):
        s.cmd("UI step")
        s.expect(r"UI page=step \(2\)", f"⑩ SEQ↔SONG {trip}/{NAV_LEAK_TRIPS}：UI step")
        s.cmd("UI transport")
        s.expect(r"UI page=transport \(3\)", f"⑩ SEQ↔SONG {trip}/{NAV_LEAK_TRIPS}：UI transport")
        s.cmd("UI song")
        s.expect(r"UI page=song \(11\)", f"⑩ SEQ↔SONG {trip}/{NAV_LEAK_TRIPS}：UI song")
        s.cmd("UI play")
        s.expect(r"UI page=play \(0\)", f"⑩ SEQ↔SONG {trip}/{NAV_LEAK_TRIPS}：UI play")
        per_trip.append(song_pool_free(s))
    drop = free0 - per_trip[-1]
    s.ser.note(f"⑩ SEQ↔SONG pool_free：{free0} → 逐次 {per_trip}")
    (s.ok if abs(drop) <= NAV_LEAK_NOISE else s.fail)(
        f"⑩ {NAV_LEAK_TRIPS} 次 SEQ↔SONG 往返后 pool_free 变化 {free0} → {per_trip[-1]}"
        f"（Δ={-drop:+d}B，允许 |Δ| ≤ {NAV_LEAK_NOISE}B）")
    steps = [free0] + per_trip
    worst = max(steps[i] - steps[i + 1] for i in range(len(steps) - 1))
    (s.ok if worst <= NAV_LEAK_NOISE else s.fail)(
        f"⑩ 单次 SEQ↔SONG 的最大池下降 {worst}B ≤ {NAV_LEAK_NOISE}B（无逐次增长）")

    # 最坏有序对仍应是 SEQ→PRESETS；FILE 全建 → 首建 STEP 的过渡峰值必须 ≥4KB
    s.cmd("UI TAB FILE")
    s.expect(r"UI TAB FILE page=", "⑩ UI TAB FILE")
    s.cmd("UI SUB 0")
    s.expect(r"UI SUB 0 page=projects \(9\)", "⑩ UI SUB 0 → PROJECTS")
    s.cmd("UI SUB 1")
    s.expect(r"UI SUB 1 page=presets \(10\)", "⑩ UI SUB 1 → PRESETS")
    mem = param_mem(s)
    (s.ok if mem["groups"]["FILE"]["built"] == 2 else s.fail)(
        f"⑩ FILE 两页全建 built={mem['groups']['FILE']['built']}")
    s.cmd("UI step", timeout=5.0)
    s.expect(r"UI page=step \(2\)", "⑩ FILE 全建 → 首建 STEP")
    m = re.search(r"页面构建 page=STEP\(2\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B", s.last)
    if m is None:
        s.fail("⑩ 过渡构建日志缺少 STEP 行（池剩余不可测）")
    else:
        delta, est, free_after = (int(g) for g in m.groups())
        (s.ok if free_after >= 4096 else s.fail)(
            f"⑩ FILE→STEP 过渡峰值 pool_free={free_after}B ≥4,096"
            f"（STEP delta={delta} 预算={est}）")
        s.ser.note(f"⑩ FILE→STEP 过渡实测：STEP delta={delta}B、构建后 pool_free={free_after}B"
                   f"（账面 {UI_BASE_BYTES} + 20,992 + 23,040 + 4,096 = "
                   f"{UI_BASE_BYTES + 20992 + 23040 + 4096}B）")

    # ---------- ⑩b 九组页签与子签截图（EDIT / SEQ 两组） ----------
    # 9 个组签（104px）+ 当前组子签（176px、5 槽复用）——页签单对象化后的视觉归档。
    s.cmd("UI TAB edit")
    s.expect(r"UI TAB EDIT page=sound \(6\)", "⑩b UI TAB edit（→ SOUND，EDIT 唯一页）")
    img = s.capture("task-11-tabs-edit")
    s.probe(img, "⑩b 组签选中（EDIT）", *C_TAB_EDIT, C_YELLOW)
    s.probe(img, "⑩b EDIT 单页组子行留空（子签槽 0 = 页签条底色）", *C_TAB_SUB_EMPTY, C_PANEL, 10)
    s.probe(img, "⑩b 组签未选中（SEQ）", *C_TAB_SEQ, C_PANEL_HI, 10)
    s.cmd("UI TAB seq")
    s.expect(r"UI TAB SEQ page=\w+ \(\d+\)", "⑩b UI TAB seq")
    s.cmd("UI SUB 0")
    s.expect(r"UI SUB 0 page=step \(2\)", "⑩b SEQ 子签归一到 STEP")
    img = s.capture("task-11-tabs-seq")
    s.probe(img, "⑩b 组签选中（SEQ）", *C_TAB_SEQ, C_YELLOW)
    s.probe(img, "⑩b 子签选中（STEP）", *C_TAB_STEP, C_YELLOW)
    s.probe(img, "⑩b 子签未选中（TRANSPORT）", *C_TAB_TRANSPORT, C_PANEL_HI, 10)

    # ---------- ⑪ 收口 ----------
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑪ 回 PLAY")
    s.cmd("STATE SNAP")
    if s.snap_int(r"playing=(\d)") != 0:
        s.cmd("UI transport")
        s.expect(r"UI page=transport \(3\)", "⑪ UI transport")
        s.cmd("TAP %d %d" % TRANSPORT_STOP)
        s.expect(r"-> TRANSPORT CMD_STOP", "⑪ STOP")
    for k in (0, 1):
        proj_erase(s, k, f"⑪ 收口擦 section {'ABCD'[k]}")
    store_cmd(s, "STORE SONG SET 4 0 0:4 1:4 2:4 3:4",
              r"^STORE SONG SET count=4 loop=0", "⑪ 收口默认歌曲 A4 B4 C4 D4")
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⑪ 收口 loop 模式")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑪ 复位 UI play")
    store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⑪ 收口 STORE AUTO ON（恢复默认）")


# ---- sequence 场景（P4-C Task 12）：步/锁/工具 → 录音 → sections → chain → SONG ---
# 这是第八条设备场景，把 P4-C 的公共 API 与三页 UI（STEP/TRANSPORT/SONG）串成一条
# 端到端矩阵。段与段之间显式归一（AUTO / 走带 / 选中轨 / 步窗 / 诊断槽 D / 工程槽
# A–C / 四条轨的图案与 LEN / BPM / 歌曲顺序），因此可重复运行。
#
# 设备侧的「拍」边界（transport_req 由渲染循环消费、live_bar 在块尾推进、section
# 延迟写由 1s 任务按 idle 门限完成）都比一条命令慢，故所有走带/小节/落盘相关断言
# 都**轮询**到落地值；只有变量级的两种 FILL 语义与锁容量是同步可读的。
#
# Task 6 的 deferral 在这里按设备口径收口：
#   - take 结果（`take=`）是一次性令牌，30ms sync 通常已消费（DI-27）：断言录音的
#     **持久效果**（LEN/BPM/步内容/playing），令牌只在自由录音进行中读 state；
#   - 物理 PLAY 与 API PLAY 的分叉（drop vs close）各一条用例；
#   - 松开键在切到鼓轨后仍回原轨（`ENG SNAP ... voices` 归 0）；
#   - REC 配置不持久化（本场景只在本会话内设置）。
# DI-31（孤立 TIE 步的 CLEAR 是 no-op）的归一用 `SEQ PATTERN CLEAR`（memset + REST）
# 与「开一步再清」；入口不依赖 CLEAR 的窄路径。
SEQ_TRACK = 0
SEQ_DRUM = 3
SEQ_DIAG_SLOT = 3           # 诊断槽 D：入口/收口擦除（旧格式 fixture 的落点）
SEQ_SEC_A, SEQ_SEC_B, SEQ_SEC_C = 0, 1, 2
SEQ_LOCK_MAX = 24           # 上游 NLOCK
SEQ_LOCK_PARAM = 1          # P_ATK：p_lockable（id ≤ P_LD_AMP）
SEQ_LOCK_BAD = 29           # P_SLEN：console 接受（0..70），adapter 判非声音参数 → invalid
SEQ_LEN_ID, SEQ_DIV_ID, SEQ_MUTE_ID, SEQ_AMODE_ID, SEQ_AHOLD_ID, SEQ_VOICE_ID = 29, 30, 40, 17, 23, 37
SEQ_KEY_NOTE = 0x42         # 低行白键 z：合成轨 = 音符，鼓轨 = lane 0
SEQ_DRUM_LOW = (0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49)
SEQ_DRUM_HIGH = (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28)
SEQ_FREE_TAKE_S = 8.0       # 自由录音按 4 小节（BPM 120）：门限 ±3% 内的抖动不影响 takt
# section A/B 用**轨道字段**区分（BPM 是 globals，arranger 刻意不随 scene 应用）：
# A 轨 0 level=104、B 轨 0 level=80；SONG 恢复的判别字段落在**选中轨 1**（level 70），
# 因此「恢复工作 loop」与「停在最后应用的 scene」hash 可区分。
SEQ_LEVEL_A = 104
SEQ_LEVEL_B = 80
SEQ_LEVEL_WORK = 70


def seq_rec_fields(s: Session) -> dict:
    """`SEQ REC` 的状态/配置投影（`take` 是一次性令牌，DI-27 不作判据）。"""
    s.cmd("SEQ REC")
    m = re.search(r"SEQ REC state=(\w+) track=(\d+) quant=(\w+) tempo=(\w+) count=(\w+) "
                  r"take=(\w+) fill_held=(\d) fill_arm=(\d)", s.last)
    if m is None:
        raise InjectError(f"SEQ REC 回显异常：{s.last.strip()!r}")
    return {"state": m.group(1), "track": int(m.group(2)), "quant": m.group(3),
            "tempo": m.group(4), "count": m.group(5), "take": m.group(6),
            "fill_held": int(m.group(7)), "fill_arm": int(m.group(8))}


def seq_locks(s: Session, track: int) -> tuple:
    """`SEQ LOCK LIST <track>` → (count, [(step, param, value)])。"""
    s.cmd(f"SEQ LOCK LIST {track}")
    m = re.search(r"SEQ LOCK LIST track=(\d+) count=(\d+)", s.last)
    if m is None:
        raise InjectError(f"SEQ LOCK LIST 回显异常：{s.last.strip()!r}")
    items = [(int(a), int(b), int(c)) for a, b, c in re.findall(
        r"^SEQ LOCK step=(\d+) param=(\d+) value=(-?\d+)\r?$", s.last, re.MULTILINE)]
    return int(m.group(2)), items


def seq_song(s: Session) -> dict:
    """`SEQ SONG` 头行（mode/running/index/bar/ready/live/dirty/chain/srec/count/loop）。"""
    s.cmd("SEQ SONG")
    m = re.search(r"SEQ SONG mode=(\w+) running=(\d+) index=(\d+) bar=(\d+) "
                  r"ready=0x([0-9A-F]{2}) live=(\S+) dirty=(\d) chain=(\d+) "
                  r"srec=(\w+) count=(\d+) loop=(\d+)", s.last)
    if m is None:
        raise InjectError(f"SEQ SONG 回显异常：{s.last.strip()!r}")
    return {"mode": m.group(1), "running": int(m.group(2)), "index": int(m.group(3)),
            "bar": int(m.group(4)), "ready": int(m.group(5), 16), "live": m.group(6),
            "dirty": int(m.group(7)), "chain": int(m.group(8)), "srec": m.group(9),
            "count": int(m.group(10)), "loop": int(m.group(11))}


def seq_srec(s: Session) -> dict:
    s.cmd("SEQ SREC")
    m = re.search(r"SEQ SREC srec=(\w+) last=(\w+)", s.last)
    if m is None:
        raise InjectError(f"SEQ SREC 回显异常：{s.last.strip()!r}")
    return {"srec": m.group(1), "last": m.group(2)}


def seq_param(s: Session, param_id: int) -> int:
    """选中轨的参数引擎真值（`PARAM <id>` 只读）。"""
    s.cmd(f"PARAM {param_id}")
    m = re.search(rf"PARAM track=(\d+) id={param_id} value=(-?\d+)", s.last)
    if m is None:
        raise InjectError(f"PARAM {param_id} 回显异常：{s.last.strip()!r}")
    return int(m.group(2))


def eng_track_level(s: Session, track: int) -> int:
    """`ENG SNAP` 的轨道 level（section 应用/恢复的判别字段；BPM 是 globals 不随 scene 走）。"""
    s.cmd("ENG SNAP")
    m = re.search(rf"ENG SNAP trk k={track} .* level=(\d+) ", s.last)
    if m is None:
        raise InjectError(f"ENG SNAP 缺少 trk k={track} 的 level：{s.last.strip()!r}")
    return int(m.group(1))


def seq_playing(s: Session) -> int:
    s.cmd("STATE SNAP")
    return s.snap_int(r"\bplaying=(\d)")


def seq_wait_playing(s: Session, want: int, timeout: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if seq_playing(s) == want:
            return True
        time.sleep(0.05)
    return False


def seq_play_raw(s: Session, label: str) -> None:
    """只发 PLAY 的 TAP（负例用：缺 scene 的 PLAY 必须被拒，不能断言 playing=1）。"""
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", f"{label} UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", f"{label} TAP PLAY")


def seq_play(s: Session, label: str) -> None:
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", f"{label} UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", f"{label} TAP PLAY")
    (s.ok if seq_wait_playing(s, 1) else s.fail)(
        f"{label} playing=1（渲染循环消费走带请求）")


def seq_stop(s: Session, label: str) -> None:
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", f"{label} UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", f"{label} TAP STOP")
    (s.ok if seq_wait_playing(s, 0) else s.fail)(f"{label} playing=0")


def seq_wait_song(s: Session, key: str, want, timeout: float = 6.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if seq_song(s)[key] == want:
            return True
        time.sleep(0.1)
    return False


def seq_wait_srec(s: Session, want: str, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if seq_srec(s)["srec"] == want:
            return True
        time.sleep(0.05)
    return False


def seq_wait_dirty_clear(s: Session, label: str, timeout: float = 16.0) -> bool:
    """等 1s 任务的延迟写把 dirty 清掉（停止 + 无发声 + 输入空闲 ≥1.5s + 间隔 ≥5s）。"""
    t0 = time.monotonic()
    sec = song_section(s)
    while time.monotonic() - t0 < timeout:
        sec = song_section(s)
        song = seq_song(s)
        if sec["dirty"] == 0 and song["dirty"] == 0:
            s.ok(f"{label}：{time.monotonic() - t0:.1f}s 后 dirty=0（段落+歌曲已落盘）")
            return True
        time.sleep(0.4)
    s.fail(f"{label}：{timeout:.0f}s 内 dirty 未清（section={sec}）")
    return False


def pattern_clear(s: Session, track: int, label: str) -> None:
    """整轨清空（上游 steps_clear：memset + REST + 清 lock/micro/fill，不改 LEN）。
    这是 DI-31 的**归一入口**：孤立 TIE 步也回 REST，不经过 STEP_CLEAR 的窄路径。"""
    s.cmd(f"SEQ PATTERN CLEAR {track}")
    s.expect(rf"SEQ PATTERN CLEAR track={track} arg=0 OK", label)


def seq_key(s: Session, code: int, down: bool, label: str = "") -> None:
    s.cmd(f"KEY 0x{code:02X} {'down' if down else 'up'}")
    s.expect(rf"KEY 0x{code:02X} {'down' if down else 'up'}", label)


def seq_set_track_param(s: Session, track: int, param_id: int, value: int, label: str) -> None:
    s.cmd(f"PARAM {param_id} {value}")
    s.expect(rf"PARAM track={track} id={param_id} value={value} applied={value} ", label)


def seq_normalize_tracks(s: Session, label: str) -> None:
    """四条轨的 LEN 16 / DIV 1/16（`section_bars` 取各轨最长图案；自由录音收尾会把
    NTRK 全部设成 16×bars，不归一会让 chain/section 的 bars 变成 4）。"""
    for k in range(4):
        select_track(s, k)
        seq_set_track_param(s, k, SEQ_LEN_ID, 16, f"{label}：轨 {k} LEN 16")
        seq_set_track_param(s, k, SEQ_DIV_ID, 2, f"{label}：轨 {k} DIV 1/16")
    select_track(s, SEQ_TRACK)


def seq_tap_step(s: Session, track: int, col: int, label: str) -> str:
    # 选轨（TAP 卡）会把当前页切到 TRACKS 组：步格 TAP 前先回到 STEP（同组时是幂等切页）
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", f"{label}：UI step")
    x, y = step_cell_xy(col, track)
    s.cmd(f"TAP {x} {y}")
    s.expect(rf"-> STEP track={track} step={col} CMD_TOGGLE_STEP", label)
    return s.last


def seq_notes_in(s: Session, track: int, steps=range(16)) -> list:
    """扫描步表，返回 [(步, 音高), …]（录音门禁用：断言持久效果，不看一次性 take 令牌）。
    音高由键位（INPUT_BASE_NOTE + offset）决定，不写死 C4——只看「有音被记下」。"""
    hits = []
    for idx in steps:
        st = eng_step(s, track, idx)
        for k in range(min(st["n"], 4)):
            hits.append((idx, st["note%d" % k]))
    return hits


def run_sequence(s: Session) -> None:
    """sequence 场景（P4-C Task 12）：音序器的端到端设备矩阵。

    覆盖（每段自带归一）：
      ⓪ 归一：AUTO OFF / 停走带 / 选中轨 0 / 步窗 0..16 / 擦诊断槽 D 与 section A–C /
         默认歌曲与 loop 模式 / BPM 120 / 轨 0 的 POLY·非静音·ARP OFF·HOLD OFF /
         四条轨的图案清空；
      ① 深度步与工具：SPAN 的 TIE 链、micro/fill、LVL/RAT/SLIDE、24+25 锁与非法锁、
         ROTATE/UNDO/REDO 的「只动 step[]」语义、TRANSPOSE（鼓轨拒绝）、LENGTH×2÷2、
         CLEAR 清 lock/micro/fill 且不改 LEN、DI-31 的孤立 TIE 归一；
      ② 录音：自由录音的 API PLAY 收尾（LEN/BPM/步内容）与物理 PLAY 丢弃（不留步）、
         TEMPO 模式的常规录音、16 lane 鼓录音（KEY 全键位）、切轨后松键回原轨、
         FILL held/arm 两个独立变量（STOP 清 held）；
      ③ live sections：STORE 的 dirty 延迟写落盘（FIELDS/HASH 证明）、播放中 STORE
         允许、下一小节 REQUEST 生效；A/B 在**轨道字段**（轨 0 level 104/80）上也不同
         （BPM 是 globals，arranger 不随 scene 应用），HASH 因此可判别；
      ④ quick chain：A B A 的逐小节推进、9 项整份拒绝、8 项接受 + 单 tap 取消、
         空 scene 的 STORE/REQUEST/CHAIN 拒绝；
      ⑤ SONG：工作 loop 用**选中轨 1 的 level 70** 与 A/B 区分；A2+B3 自然结束与
         SONG 模式 loop=1 的 STOP 都恢复该工作 loop（LIVEHASH+轨 level+选中轨）、
         播放中应用 scene（A=104 / B=80）可观测；缺 scene 整首拒播 + 空 scene JUMP；
      ⑥ SONG REC：未到下一小节取消不写配置（REC IS ON 时 MODE 被拒）、真实 take 录到
         A→B 两项（count/entries/dirty/SAVE/INFO/LOAD）；**loop 模式**下 REQUEST B 是
         live 段切换、STOP 后停在 B（无 song_restore），与 ⑤ 的 SONG 模式区分；
      ⑦ STORE TEST 与单例 SELFTEST 之后歌曲保持（自检的 `.bss` 回滚 + song_reload）；
      ⑧ esptool 硬复位后 PER3 歌曲与 section A/B 从 flash 恢复；
      ⑨ 收口：擦 section A–C/D、默认歌曲、loop 模式、BPM 120、选轨 0、PLAY 页、AUTO ON。

    本场景**破坏性**：占用工程槽 A–C（section）与 D（诊断）、歌曲顺序与四条轨的图案。
    它自带归一，可重复运行。
    """
    s.ser.note("sequence 场景开始：⓪ 归一（AUTO/走带/遗留录音/选轨/步窗/诊断槽/工程槽/图案/歌曲）")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⓪ STORE AUTO OFF")
    # 上一轮异常中断可能留下 free take / SONG REC / 非默认 REC 配置：先收口再停走带
    # （REC 关闭 free take 会启动走带，故 ensure_stopped 放在它后面）。
    rec = seq_rec_fields(s)
    if rec["state"] != "off":
        s.cmd("SEQ REC TOGGLE")
        s.expect(r"SEQ REC state=\w+", "⓪ 收口遗留录音/free take")
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and seq_rec_fields(s)["state"] != "off":
            time.sleep(0.05)
    srec = seq_srec(s)
    if srec["srec"] != "off":
        s.cmd("SEQ SREC TOGGLE")
        s.expect(r"SEQ SREC srec=off", "⓪ 收口遗留 SONG REC")
    s.cmd("SEQ REC CONFIG 0 0 0")
    s.expect(r"SEQ REC state=\w+ track=\d+ quant=on tempo=free count=note",
             "⓪ REC 配置归一（quant on / FREE / NOTE）")
    ensure_stopped(s)
    ensure_window(s, 0, 16)
    select_track(s, SEQ_TRACK)
    for slot in (SEQ_SEC_A, SEQ_SEC_B, SEQ_SEC_C, SEQ_DIAG_SLOT):
        proj_erase(s, slot, f"⓪ 清工程/诊断槽 {'ABCD'[slot]}")
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⓪ loop 模式")
    store_cmd(s, "STORE SONG SET 4 0 0:4 1:4 2:4 3:4",
              r"^STORE SONG SET count=4 loop=0", "⓪ 默认歌曲 A4 B4 C4 D4")
    s.cmd("PARAM GLOBAL 0 120")
    s.expect(r"PARAM GLOBAL id=0 value=120 applied=120 ", "⓪ BPM 120")
    seq_normalize_tracks(s, "⓪")
    for tid, want, label in ((0, SEQ_LEVEL_A, "level 104"), (SEQ_MUTE_ID, 0, "非静音"),
                             (SEQ_AMODE_ID, 0, "ARP OFF"), (SEQ_AHOLD_ID, 0, "HOLD OFF"),
                             (SEQ_VOICE_ID, 0, "POLY")):
        seq_set_track_param(s, SEQ_TRACK, tid, want, f"⓪ 轨 0 {label}")
    select_track(s, 1)
    seq_set_track_param(s, 1, 0, SEQ_LEVEL_A, "⓪ 轨 1 level 104（⑤ 工作 loop 判别字段归一）")
    select_track(s, SEQ_DRUM)
    seq_set_track_param(s, SEQ_DRUM, SEQ_MUTE_ID, 0, "⓪ 鼓轨非静音")
    for k in range(4):
        pattern_clear(s, k, f"⓪ 清轨 {k} 图案（DI-31 归一入口）")
    select_track(s, SEQ_TRACK)
    sec = song_section(s)
    (s.ok if sec["ready"] == 0 else s.fail)(
        f"⓪ 归一后 ready=0x{sec['ready']:02X}（期望 0）")

    # ---------- ① 深度步 / 锁 / 工具 ----------
    s.ser.note("① 深度步（SPAN/TIE/micro/fill）+ 24+25 锁与非法锁 + pattern 工具")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "① UI step")
    ensure_window(s, 0, 16)
    seq_tap_step(s, SEQ_TRACK, 0, "① TAP 步 0 建音（与 STEP 页同一 toggle 动作）")
    st0 = eng_step(s, SEQ_TRACK, 0)
    (s.ok if st0["n"] == 1 and st0["time"] == 0 else s.fail)(
        f"① 步 0 = NOTE（n={st0['n']} time={st0['time']}）")
    s.cmd("SEQ EDIT SPAN 0 0 3")
    s.expect(r"SEQ EDIT SPAN track=0 step=0 OK", "① SPAN 3（步 1/2 长成 TIE）")
    t1 = eng_step(s, SEQ_TRACK, 1)
    t2 = eng_step(s, SEQ_TRACK, 2)
    (s.ok if (t1["time"] == 1 and t1["n"] == 0 and t2["time"] == 1) else s.fail)(
        f"① TIE 链：步1 time={t1['time']} n={t1['n']}、步2 time={t2['time']}（期望 1/0/1）")
    for cmd, pattern, label in (
            ("SEQ MICRO 0 0 -5", r"SEQ MICRO track=0 step=0 value=-5 OK", "① micro -5"),
            ("SEQ FILL 0 0 2", r"SEQ FILL track=0 step=0 value=2 OK", "① fill x2"),
            ("SEQ EDIT LVL 0 0 0 3", r"SEQ EDIT LVL track=0 step=0 OK", "① LVL HARD"),
            ("SEQ EDIT RAT 0 0 0 1", r"SEQ EDIT RAT track=0 step=0 OK", "① RAT x2"),
            ("SEQ EDIT SLIDE 0 0 1", r"SEQ EDIT SLIDE track=0 step=0 OK", "① SLIDE on")):
        s.cmd(cmd)
        s.expect(pattern, label)
    st0 = eng_step(s, SEQ_TRACK, 0)
    ok = (st0["micro"] == -5 and st0["fill"] == 2 and (st0["lvl"] & 3) == 3
          and (st0["rat"] & 3) == 1 and (st0["flags"] & 2) == 2)
    (s.ok if ok else s.fail)(
        f"① 步 0 深度字段 micro={st0['micro']} fill={st0['fill']} lvl={st0['lvl']} "
        f"rat={st0['rat']} flags=0x{st0['flags']:02X}（期望 -5/2/…3/…1/SLIDE）")

    # 视觉证据：NOTE 实心 vs TIE 空心 + micro 文本（焦点框移出被测列）
    s.cmd("STATE SNAP")
    focus = s.snap_int(r"focus_step=(\d+)")
    for _ in range((10 - focus) % 16):
        seq_key(s, 0x4C, True)
        seq_key(s, 0x4C, False)
    time.sleep(0.2)
    img = s.capture("step-deep")
    x0, y0 = step_cell_box(0, 0)
    x1, y1 = step_cell_box(1, 0)
    s.probe(img, "① 步 0 NOTE 实心（轨色中心）",
            x0 + STEP_CELL_W // 2, y0 + STEP_CELL_H // 2, C_BLUE)
    s.probe(img, "① 步 1 TIE 空心（中心为面板底）",
            x1 + STEP_CELL_W // 2, y1 + STEP_CELL_H // 2, C_PANEL_HI, 10)
    s.probe_region(img, "① 步 1 TIE 内框（轨色，区别于空格）",
                   (x1 + 2, y1 + 2, x1 + STEP_CELL_W - 2, y1 + 30), C_BLUE, 16, 40)
    s.probe_region(img, "① 步 0 micro 文本 m-5",
                   (x0 + 2, y0 + STEP_BOT_STRIP_Y, x0 + 44, y0 + STEP_BOT_STRIP_Y + 16),
                   C_TEXT_DIM, 16, 5)
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "① 回 STEP（截屏后同组重建）")

    # 24 锁 + 第 25 锁 full + 非法（非声音参数）锁不变
    for i in range(SEQ_LOCK_MAX):
        s.cmd(f"SEQ LOCK SET 0 {i} {SEQ_LOCK_PARAM} {10 + i}")
        s.expect(rf"SEQ LOCK SET track=0 step={i} param={SEQ_LOCK_PARAM} value={10 + i} OK",
                 f"① 锁 {i + 1}/{SEQ_LOCK_MAX}")
    count, items = seq_locks(s, SEQ_TRACK)
    (s.ok if count == SEQ_LOCK_MAX and len(items) == SEQ_LOCK_MAX else s.fail)(
        f"① 锁满：count={count} 条目={len(items)}（期望 {SEQ_LOCK_MAX}）")
    s.cmd(f"SEQ LOCK SET 0 {SEQ_LOCK_MAX} {SEQ_LOCK_PARAM} 99")
    s.expect(r"^SEQ ERR full\r?$", "① 第 25 锁 → SEQ ERR full（判别性反例）")
    s.cmd(f"SEQ LOCK SET 0 0 {SEQ_LOCK_BAD} 5")
    s.expect(r"^SEQ ERR invalid\r?$", "① 非法锁（P_SLEN 非声音参数）→ SEQ ERR invalid")
    count2, items2 = seq_locks(s, SEQ_TRACK)
    unchanged = (count2 == SEQ_LOCK_MAX and items2 == items
                 and not any(p == SEQ_LOCK_BAD for _, p, _ in items2))
    (s.ok if unchanged else s.fail)(
        f"① 被拒的锁不改状态：count={count2}（期望 {SEQ_LOCK_MAX}）且无 param={SEQ_LOCK_BAD}")
    # CLEAR 清 lock/micro/fill（不改 LEN），锚点删除顺带把 TIE 尾巴收回
    s.cmd("SEQ EDIT CLEAR 0 0")
    s.expect(r"SEQ EDIT CLEAR track=0 step=0 OK", "① CLEAR 步 0")
    st0 = eng_step(s, SEQ_TRACK, 0)
    count3, items3 = seq_locks(s, SEQ_TRACK)
    ok = (st0["n"] == 0 and st0["micro"] == 0 and st0["fill"] == 0
          and count3 == SEQ_LOCK_MAX - 1 and all(s != 0 for s, _, _ in items3)
          and seq_param(s, SEQ_LEN_ID) == 16)
    (s.ok if ok else s.fail)(
        f"① CLEAR 清 lock/micro/fill 且不改 LEN：count={count3} micro={st0['micro']} "
        f"fill={st0['fill']} len={seq_param(s, SEQ_LEN_ID)}")
    for i in range(1, SEQ_LOCK_MAX):
        s.cmd(f"SEQ LOCK DEL 0 {i} 71")
        s.expect(rf"SEQ LOCK DEL track=0 step={i} param=71 OK", f"① 删锁步 {i}")
    count4, _ = seq_locks(s, SEQ_TRACK)
    (s.ok if count4 == 0 else s.fail)(f"① 锁清空 count={count4}")

    # 重建锚点 + SPAN + micro，做 ROTATE/UNDO/REDO 的「只动 step[]」语义
    seq_tap_step(s, SEQ_TRACK, 0, "① 重建步 0 锚点")
    s.cmd("SEQ EDIT SPAN 0 0 3")
    s.expect(r"SEQ EDIT SPAN track=0 step=0 OK", "① 重建 TIE 链")
    s.cmd("SEQ MICRO 0 0 -5")
    s.expect(r"SEQ MICRO track=0 step=0 value=-5 OK", "① 重建 micro -5")
    s.cmd("SEQ PATTERN ROTATE 0 1")
    s.expect(r"SEQ PATTERN ROTATE track=0 arg=1 OK", "① ROTATE +1")
    r0 = eng_step(s, SEQ_TRACK, 0)
    r1 = eng_step(s, SEQ_TRACK, 1)
    r2 = eng_step(s, SEQ_TRACK, 2)
    ok = (r0["n"] == 0 and r1["n"] == 1 and r2["time"] == 1 and r0["micro"] == -5)
    (s.ok if ok else s.fail)(
        f"① ROTATE 只动 step[]：步0 n={r0['n']} micro={r0['micro']}、步1 n={r1['n']}、"
        f"步2 time={r2['time']}（期望 0/-5、1、1）")
    s.cmd("SEQ PATTERN UNDO 0")
    s.expect(r"SEQ PATTERN UNDO track=0 arg=0 OK", "① UNDO")
    u0 = eng_step(s, SEQ_TRACK, 0)
    u1 = eng_step(s, SEQ_TRACK, 1)
    (s.ok if (u0["n"] == 1 and u1["time"] == 1 and u0["micro"] == -5) else s.fail)(
        f"① UNDO 恢复 steps/micro：步0 n={u0['n']} micro={u0['micro']} 步1 time={u1['time']}")
    s.cmd("SEQ PATTERN REDO 0")
    s.expect(r"SEQ PATTERN REDO track=0 arg=0 OK", "① REDO")
    d1 = eng_step(s, SEQ_TRACK, 1)
    (s.ok if d1["n"] == 1 else s.fail)(f"① REDO 回到旋转态：步1 n={d1['n']}")
    note_before = d1["note0"]
    # 上游 pattern_transpose 只取 arg 的符号（每次 1 个半音），不是「+arg」
    s.cmd("SEQ PATTERN TRANSPOSE 0 2")
    s.expect(r"SEQ PATTERN TRANSPOSE track=0 arg=2 OK", "① TRANSPOSE（arg>0）")
    d1b = eng_step(s, SEQ_TRACK, 1)
    (s.ok if d1b["note0"] == note_before + 1 else s.fail)(
        f"① TRANSPOSE arg>0：note {note_before} → {d1b['note0']}（上游每次 1 个半音）")
    s.cmd("SEQ PATTERN TRANSPOSE 0 -1")
    s.expect(r"SEQ PATTERN TRANSPOSE track=0 arg=-1 OK", "① TRANSPOSE（arg<0）")
    d1c = eng_step(s, SEQ_TRACK, 1)
    (s.ok if d1c["note0"] == note_before else s.fail)(
        f"① TRANSPOSE arg<0 回到原音：{d1c['note0']}（期望 {note_before}）")
    select_track(s, SEQ_DRUM)
    s.cmd("SEQ PATTERN TRANSPOSE 3 2")
    s.expect(r"^SEQ ERR invalid\r?$", "① 鼓轨 TRANSPOSE → SEQ ERR invalid（判别性反例）")
    select_track(s, SEQ_TRACK)
    s.cmd("SEQ PATTERN LENGTH 0 1")
    s.expect(r"SEQ PATTERN LENGTH track=0 arg=1 OK", "① LENGTH ×2")
    (s.ok if seq_param(s, SEQ_LEN_ID) == 32 else s.fail)(
        f"① LENGTH ×2：LEN={seq_param(s, SEQ_LEN_ID)}（期望 32）")
    l17 = eng_step(s, SEQ_TRACK, 17)
    (s.ok if l17["note0"] == d1c["note0"] else s.fail)(
        f"① LENGTH 复制图案：步 17 note={l17['note0']}（期望 {d1c['note0']}）")
    s.cmd("SEQ PATTERN LENGTH 0 -1")
    s.expect(r"SEQ PATTERN LENGTH track=0 arg=-1 OK", "① LENGTH ÷2")
    (s.ok if seq_param(s, SEQ_LEN_ID) == 16 else s.fail)(
        f"① LENGTH ÷2：LEN={seq_param(s, SEQ_LEN_ID)}（期望 16）")

    # DI-31：ROTATE -1 把 TIE 挪到 0、锚点挪到 15 → 孤立 TIE；CLEAR 是 no-op（已知
    # deferral，只记录不判失败），用「开一步再清」自愈。先清轨：SPAN 只往**空步**长，
    # 前面用例在步 1 留下的音符会让它停在 1 步（前提不成立）。
    pattern_clear(s, SEQ_TRACK, "① DI-31 用例：清轨 0（保证步 1 是空步）")
    seq_tap_step(s, SEQ_TRACK, 0, "① DI-31 用例：步 0 开音")
    s.cmd("SEQ EDIT SPAN 0 0 2")
    s.expect(r"SEQ EDIT SPAN track=0 step=0 OK", "① DI-31 用例：SPAN 2")
    s.cmd("SEQ PATTERN ROTATE 0 -1")
    s.expect(r"SEQ PATTERN ROTATE track=0 arg=-1 OK", "① DI-31 用例：ROTATE -1")
    orphan = eng_step(s, SEQ_TRACK, 0)
    # 先证明前提真的成立（旋转把 TIE 与锚点分离）：否则后面的「复现」只是空话
    orphan_ok = orphan["time"] == 1 and orphan["n"] == 0
    (s.ok if orphan_ok else s.fail)(
        f"① DI-31 前提：旋转后步 0 是孤立 TIE（time={orphan['time']} n={orphan['n']}，期望 1/0）")
    s.cmd("SEQ EDIT CLEAR 0 0")
    s.expect(r"SEQ EDIT CLEAR track=0 step=0 OK", "① DI-31 用例：CLEAR（已知 no-op 路径）")
    after_clear = eng_step(s, SEQ_TRACK, 0)
    if orphan_ok and after_clear["time"] == 1 and after_clear["n"] == 0:
        s.ser.note("① DI-31 复现（前提已断言）：孤立 TIE 步 CLEAR 后仍 time=1，"
                   "自愈走「开一步再清」")
    elif orphan_ok:
        s.ser.note(f"① DI-31 未复现（前提成立但 CLEAR 后 time={after_clear['time']} "
                   f"n={after_clear['n']}）：deferral 可能已修")
    else:
        s.ser.note(f"① DI-31 前提未成立（旋转后 time={orphan['time']} n={orphan['n']}）："
                   "本用例不判定复现")
    seq_tap_step(s, SEQ_TRACK, 0, "① DI-31 自愈：TAP 把 TIE 开成 NOTE")
    s.cmd("SEQ EDIT CLEAR 0 0")
    s.expect(r"SEQ EDIT CLEAR track=0 step=0 OK", "① DI-31 自愈：再 CLEAR")
    healed = eng_step(s, SEQ_TRACK, 0)
    (s.ok if healed["n"] == 0 and healed["time"] == 2 else s.fail)(
        f"① DI-31 自愈后步 0 = REST（n={healed['n']} time={healed['time']}，期望 0/2）")
    for k in range(4):
        pattern_clear(s, k, f"① 收口清轨 {k} 图案（自由录音要求整工程为空）")
    select_track(s, SEQ_TRACK)

    # ---------- ② 录音 / free take / 16 lane 鼓 / cross-track release / fill ----------
    s.ser.note("② 录音：自由录音 API PLAY 收尾、物理 PLAY 丢弃、TEMPO 常规录音、16 lane 鼓、"
               "切轨松键、FILL held/arm")
    s.cmd("SEQ REC CONFIG 0 0 0")
    s.expect(r"SEQ REC state=\w+ track=0 quant=on tempo=free count=note",
             "② REC 配置归一（quant on / FREE / NOTE）")
    s.cmd("SEQ REC TOGGLE")
    s.expect(r"SEQ REC state=armed track=0", "② 自由录音：REC arm（停止 + 空工程）")
    seq_key(s, SEQ_KEY_NOTE, True, "② 自由录音：按下第一个音（ft_start）")
    rec = seq_rec_fields(s)
    (s.ok if rec["state"] == "free_take" else s.fail)(
        f"② 自由录音进行中：SEQ REC state={rec['state']}（期望 free_take）")
    time.sleep(SEQ_FREE_TAKE_S)
    seq_key(s, SEQ_KEY_NOTE, False, "② 自由录音：松开")
    seq_play(s, "② 自由录音 API PLAY 收尾")
    rec = seq_rec_fields(s)
    (s.ok if rec["state"] == "off" else s.fail)(f"② 收尾后 SEQ REC state={rec['state']}（off）")
    length = seq_param(s, SEQ_LEN_ID)
    bpm = store_bpm(s)
    hits = seq_notes_in(s, SEQ_TRACK)
    (s.ok if length == 64 else s.fail)(
        f"② API PLAY 收尾的持久效果：LEN={length}（8s@120 ⇒ 4 小节 = 64）")
    # ft_fit 以 CTL 块计时、Tab5 的渲染块是 8×CTL：实测 8s 的 take 拟合到 112 BPM
    # （4 小节），LEN=64 才是判据；BPM 只要求落在 4 小节拟合的合理范围。
    (s.ok if 100 <= bpm <= 140 else s.fail)(
        f"② 收尾的 take 拟合出 4 小节 loop：BPM={bpm}（观测 108..132 区间；LEN=64 为判据）")
    (s.ok if hits else s.fail)(f"② 录制写回步表：{hits}")
    seq_stop(s, "② 自由录音收尾后停走带")
    for k in range(4):
        pattern_clear(s, k, f"② 清轨 {k}（丢弃收尾的 take，重建空工程）")

    # 物理 PLAY 丢弃：不留步、不改 LEN（与 API PLAY 的 close 分叉）。
    # 收尾（API close）后的 300ms 内 REC 会被 ft_owns_press() 吞掉：显式跨过窗口再 arm。
    time.sleep(0.4)
    length0 = seq_param(s, SEQ_LEN_ID)
    s.cmd("SEQ REC TOGGLE")
    s.expect(r"SEQ REC state=armed track=0", "② 丢弃用例：REC arm（再次空工程）")
    seq_key(s, SEQ_KEY_NOTE, True, "② 丢弃用例：按下音符启动自由录音")
    (s.ok if seq_rec_fields(s)["state"] == "free_take" else s.fail)(
        "② 丢弃用例：free_take 已启动")
    time.sleep(1.5)
    seq_key(s, SEQ_KEY_NOTE, False, "② 丢弃用例：松开")
    s.cmd("SEQ REC BUTTON 1")   # 物理面板 PLAY 的按下沿（ft_block 丢弃 take）
    s.expect(r"SEQ REC BUTTON down=1 OK", "② 物理 PLAY down")
    time.sleep(0.1)             # 跨过至少一个音频块（ft_block 的上升沿）
    s.cmd("SEQ REC BUTTON 0")
    s.expect(r"SEQ REC BUTTON down=0 OK", "② 物理 PLAY up")
    time.sleep(0.1)
    rec = seq_rec_fields(s)
    (s.ok if rec["state"] == "off" else s.fail)(
        f"② 物理 PLAY 丢弃后 state={rec['state']}（off，不进入播放）")
    (s.ok if seq_playing(s) == 0 else s.fail)("② 物理 PLAY 丢弃不启动走带（playing=0）")
    st0 = eng_step(s, SEQ_TRACK, 0)
    (s.ok if st0["n"] == 0 else s.fail)(
        f"② 丢弃的 take 不留步（步 0 n={st0['n']}，期望 0）")
    length_now = seq_param(s, SEQ_LEN_ID)
    (s.ok if length_now == length0 else s.fail)(
        f"② 丢弃的 take 不改 LEN（{length_now}，期望 {length0}）")

    # TEMPO 模式常规录音（播放中 rec_begin + 量化的首音）；丢弃 take 后同样等过 300ms 窗口
    time.sleep(0.4)
    s.cmd("SEQ REC CONFIG 0 1 0")
    s.expect(r"SEQ REC state=\w+ track=0 quant=on tempo=tempo count=note",
             "② TEMPO 模式配置（quant on / TEMPO / NOTE）")
    seq_set_track_param(s, SEQ_TRACK, SEQ_LEN_ID, 16, "② 常规录音：LEN 16")
    s.cmd("SEQ REC TOGGLE")
    s.expect(r"SEQ REC state=armed track=0", "② 常规录音：arm")
    seq_play(s, "② 常规录音 PLAY（arm + 播放 ⇒ 从顶录制）")
    rec = seq_rec_fields(s)
    (s.ok if rec["state"] == "recording" else s.fail)(
        f"② 播放中录音：state={rec['state']}（recording，rec 位跟随选中轨）")
    time.sleep(0.4)
    seq_key(s, SEQ_KEY_NOTE, True, "② 常规录音：弹一个音")
    time.sleep(0.1)
    seq_key(s, SEQ_KEY_NOTE, False, "② 常规录音：松开")
    time.sleep(0.4)
    s.cmd("SEQ REC TOGGLE")   # 再按一次：关录音，播放继续
    s.expect(r"SEQ REC state=off", "② 再按 REC：关录音（播放继续）")
    (s.ok if seq_playing(s) == 1 else s.fail)("② REC off 不停播放（playing=1）")
    hits = seq_notes_in(s, SEQ_TRACK)
    (s.ok if hits else s.fail)(f"② 常规录音写回步表：{hits}")
    seq_stop(s, "② 常规录音收尾 STOP")
    for k in range(4):
        pattern_clear(s, k, f"② 清轨 {k}（常规录音用例收口）")

    # cross-track release：按住合成轨音符 → 切到鼓轨 → 松开必须回原轨
    select_track(s, SEQ_TRACK)
    seq_key(s, SEQ_KEY_NOTE, True, "② 跨轨松键：轨 0 按下音符")
    (s.ok if eng_voices_poll(s, 1, SEQ_TRACK, 1.0) >= 1 else s.fail)(
        "② 跨轨松键：轨 0 voices≥1（音符在响）")
    select_track(s, SEQ_DRUM)
    seq_key(s, SEQ_KEY_NOTE, False, "② 跨轨松键：切到鼓轨后松开同一个键")
    (s.ok if eng_voices_poll(s, 0, SEQ_TRACK, 1.5) == 0 else s.fail)(
        "② 跨轨松键：轨 0 voices=0（释放回按下时的轨，未被鼓分支吞掉）")

    # 16 lane 鼓：KEY 全键位（低行 0..7 / 高行 8..15）经 CMD_DRUM_HIT 写入 dstep
    s.cmd("PARAM GLOBAL 0 40")
    s.expect(r"PARAM GLOBAL id=0 value=40 applied=40 ", "② 鼓录音用 40 BPM（步长足够容纳 32 个键事件）")
    seq_set_track_param(s, SEQ_DRUM, SEQ_LEN_ID, 16, "② 鼓轨 LEN 16")
    seq_play(s, "② 鼓录音 PLAY")
    s.cmd("SEQ REC TOGGLE")
    s.expect(r"SEQ REC state=recording track=3", "② 鼓轨录音开启（rec 位 = 选中鼓轨）")
    for lane, code in enumerate(SEQ_DRUM_LOW + SEQ_DRUM_HIGH):
        seq_key(s, code, True, f"② 鼓 lane {lane} down")
        seq_key(s, code, False, f"② 鼓 lane {lane} up")
    s.cmd("SEQ REC TOGGLE")
    s.expect(r"SEQ REC state=off", "② 鼓录音关闭")
    seq_stop(s, "② 鼓录音收尾 STOP")
    union = 0
    hit_steps = []
    for idx in range(16):
        d = seq_dstep(s, idx)
        if d["on"]:
            hit_steps.append((idx, d["on"]))
            union |= d["on"]
    (s.ok if union == 0xFFFF else s.fail)(
        f"② 16 lane 鼓录音：步 {hit_steps} 的 on 位并集=0x{union:04X}（期望 0xFFFF）")
    select_track(s, SEQ_TRACK)

    # FILL held / arm 是两个独立变量（NEXT 只翻转 arm；STOP 清 held 与 arm）
    for cmd, pattern, label in (
            ("SEQ REC FILL HOLD 1", r"SEQ REC FILL HOLD 1 OK", "② FILL 保持 down"),
            ("SEQ REC FILL NEXT", r"SEQ REC FILL NEXT OK", "② FILL NEXT 脉冲")):
        s.cmd(cmd)
        s.expect(pattern, label)
    rec = seq_rec_fields(s)
    (s.ok if rec["fill_held"] == 1 and rec["fill_arm"] == 1 else s.fail)(
        f"② 两个变量并存：held={rec['fill_held']} arm={rec['fill_arm']}（期望 1/1）")
    s.cmd("SEQ REC FILL HOLD 0")
    s.expect(r"SEQ REC FILL HOLD 0 OK", "② FILL 保持 up")
    rec = seq_rec_fields(s)
    (s.ok if rec["fill_held"] == 0 and rec["fill_arm"] == 1 else s.fail)(
        f"② NEXT 不写 held：held={rec['fill_held']} arm={rec['fill_arm']}（期望 0/1）")
    s.cmd("SEQ REC FILL NEXT")
    s.expect(r"SEQ REC FILL NEXT OK", "② FILL NEXT 再脉冲")
    rec = seq_rec_fields(s)
    (s.ok if rec["fill_arm"] == 0 else s.fail)(f"② 脉冲翻转 arm={rec['fill_arm']}（期望 0）")
    seq_play(s, "② FILL 的 STOP 收口")
    s.cmd("SEQ REC FILL HOLD 1")
    s.expect(r"SEQ REC FILL HOLD 1 OK", "② 走带中按住 FILL")
    time.sleep(0.1)
    s.cmd("SEQ REC FILL NEXT")
    s.expect(r"SEQ REC FILL NEXT OK", "② 走带中再 NEXT")
    seq_stop(s, "② STOP 清 FILL")
    rec = seq_rec_fields(s)
    (s.ok if rec["fill_held"] == 0 and rec["fill_arm"] == 0 else s.fail)(
        f"② STOP 清 FILL：held={rec['fill_held']} arm={rec['fill_arm']}（期望 0/0）")
    for k in range(4):
        pattern_clear(s, k, f"② 收口清轨 {k}")
    select_track(s, SEQ_TRACK)
    s.cmd("PARAM GLOBAL 0 120")
    s.expect(r"PARAM GLOBAL id=0 value=120 applied=120 ", "② 回 BPM 120")

    # ---------- ③ live sections / dirty 延迟写 ----------
    s.ser.note("③ live sections：STORE 的 dirty 落盘、播放中 STORE、下一小节 REQUEST")
    seq_normalize_tracks(s, "③")
    seq_set_track_param(s, SEQ_TRACK, SEQ_LEN_ID, 4, "③ section 用 LEN 4（section_bars=1）")
    seq_tap_step(s, SEQ_TRACK, 0, "③ 步 0 建音（section A 的内容）")
    s.cmd("SEQ SECTION A STORE")
    s.expect(r"SEQ SECTION A STORE OK", "③ STORE A（停止态）")
    sec = song_section(s)
    (s.ok if (sec["ready"] & 0x01) and sec["live"] == "A" and sec["dirty"] == 1 else s.fail)(
        f"③ STORE A 后 ready=0x{sec['ready']:02X} live={sec['live']} dirty={sec['dirty']}"
        f"（期望 bit0/A/1）")
    seq_wait_dirty_clear(s, "③ A 的延迟写")
    hash_a = store_hash(s, "STORE PROJ HASH 0", r"^PROJ 0 HASH=([0-9A-Fa-f]{8})\r?$",
                        "③ A 的 RAM 槽 hash")
    (s.ok if hash_a != 0 else s.fail)(f"③ A 的 RAM 槽 hash={hash_a:08X}（非空）")
    s.cmd("STORE PROJ FIELDS 0")
    store_expect(s, r"^PROJ 0 FIELDS fmt=6 .*t0\.lvl=104 .*t0\.s0=1:\d+",
                 "③ A 已落 flash（FIELDS 走 st_load + proj_import；轨 0 level=104）")

    # section B 与 A 在**轨道字段**上也不同（level 104 → 80）：BPM 是 globals，arranger
    # 刻意不随 scene 应用，只靠 BPM 的 A/B 在「应用/恢复」上不可判别。
    seq_set_track_param(s, SEQ_TRACK, 0, SEQ_LEVEL_B, "③ section B 的轨 0 level=80")
    s.cmd("PARAM GLOBAL 0 132")
    s.expect(r"PARAM GLOBAL id=0 value=132 applied=132 ", "③ 改 BPM（section B 的内容）")
    seq_play(s, "③ 播放中 STORE B")
    s.cmd("SEQ SECTION B STORE")
    s.expect(r"SEQ SECTION B STORE OK", "③ 播放中 STORE B（允许）")
    sec = song_section(s)
    playing_now = seq_playing(s)
    (s.ok if (sec["ready"] & 0x02) and sec["live"] == "B" and playing_now == 1 else s.fail)(
        f"③ 播放中 STORE B 不停走带：ready=0x{sec['ready']:02X} live={sec['live']} "
        f"playing={playing_now}")
    s.cmd("SEQ SECTION A REQUEST")
    s.expect(r"SEQ SECTION A REQUEST OK", "③ 下一小节 REQUEST A")
    if seq_wait_song(s, "live", "A", 6.0):
        s.ok("③ REQUEST 在下一小节生效：SEQ SECTION live=A")
    else:
        s.fail("③ REQUEST 6s 内未生效（live 仍非 A）")
    seq_stop(s, "③ 停走带等 B 落盘")
    seq_wait_dirty_clear(s, "③ B 的延迟写")
    hash_b = store_hash(s, "STORE PROJ HASH 1", r"^PROJ 1 HASH=([0-9A-Fa-f]{8})\r?$",
                        "③ B 的 RAM 槽 hash")
    (s.ok if hash_b != 0 and hash_b != hash_a else s.fail)(
        f"③ A/B 是不同的 section：A={hash_a:08X} B={hash_b:08X}")
    s.cmd("STORE PROJ FIELDS 1")
    store_expect(s, r"^PROJ 1 FIELDS fmt=6 .*bpm=132 .*t0\.lvl=80 ",
                 "③ B 已落 flash（FIELDS：bpm=132、轨 0 level=80）")
    seq_set_track_param(s, SEQ_TRACK, 0, SEQ_LEVEL_A, "③ 轨 0 level 回 104（后续 LOAD A 同值）")

    # ---------- ④ quick chain ----------
    s.ser.note("④ quick chain：A B A 的逐小节推进、9 项整份拒绝、8 项接受 + 取消、空 scene 拒绝")
    s.cmd("SEQ SECTION A LOAD")
    s.expect(r"SEQ SECTION A LOAD OK", "④ LOAD A（工作 loop + live=A）")
    seq_play(s, "④ chain 起播")
    s.cmd("SEQ CHAIN A B A")
    s.expect(r"SEQ CHAIN count=3 index=0 bars=1", "④ SEQ CHAIN A B A")
    chain = song_chain(s)
    (s.ok if chain["scenes"] == [0, 1, 0] else s.fail)(
        f"④ chain 场景序={chain['scenes']}（期望 [0,1,0]）")
    t0 = time.monotonic()
    advanced = False
    while time.monotonic() - t0 < 8.0:
        ch = song_chain(s)
        if ch["index"] >= 1:
            advanced = True
            break
        time.sleep(0.2)
    (s.ok if advanced else s.fail)(
        "④ chain 按 section_bars 推进：8s 内 index ≥ 1（每小节 1 项）")
    s.cmd("SEQ CHAIN A B A B A B A B A")
    s.expect(r"^SEQ ERR invalid\r?$", "④ 9 项 chain → SEQ ERR invalid（判别性反例）")
    chain = song_chain(s)
    (s.ok if chain["count"] == 3 else s.fail)(
        f"④ 9 项被拒后 chain 不变：count={chain['count']}（期望 3）")
    s.cmd("SEQ CHAIN A B A B A B A B")
    s.expect(r"SEQ CHAIN count=8 index=0 bars=1", "④ 8 项 chain 接受")
    s.cmd("SEQ CHAIN A")
    s.expect(r"SEQ CHAIN count=0 index=0", "④ 单 tap（再次 CHAIN 一项）= 取消")
    s.cmd("SEQ CHAIN A C")
    s.expect(r"^SEQ ERR not_found\r?$", "④ 空 scene C 的 chain → SEQ ERR not_found")
    (s.ok if song_chain(s)["count"] == 0 else s.fail)("④ 空 scene 整份拒绝：不装半条（count=0）")
    s.cmd("SEQ SECTION C REQUEST")
    s.expect(r"^SEQ ERR not_found\r?$", "④ 空 scene C 的 REQUEST → SEQ ERR not_found（判别性反例）")

    # ---------- ⑤ live loop JUMP / SONG A2+B3 / loop 恢复 / 缺 scene 拒播 ----------
    s.ser.note("⑤ live loop 的 JUMP（空 scene 可见拒绝 + B 生效）；SONG A2+B3 自然结束与 "
               "SONG 模式 loop=1 的 STOP 都恢复 playback 前的**工作 loop**；缺 scene 整首拒播")
    seq_stop(s, "⑤ 先停走带")
    s.cmd("SEQ SECTION A LOAD")
    s.expect(r"SEQ SECTION A LOAD OK", "⑤ 工作 loop 归一到 A")
    lvl = eng_track_level(s, SEQ_TRACK)
    (s.ok if lvl == SEQ_LEVEL_A else s.fail)(
        f"⑤ LOAD A：轨 0 level={lvl}（section A 的 {SEQ_LEVEL_A}）")
    select_track(s, 1)
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⑤ loop 模式")
    seq_play(s, "⑤ live loop 播放（JUMP 用例）")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑤ UI song（live loop）")
    s.cmd("TAP %d %d" % song_card_xy(SEQ_SEC_C))
    s.expect(r"-> SONG card=C select", "⑤ 选中空 scene C")
    song_action(s, song_sec_xy(3), "JUMP", "⑤ live loop 中 JUMP 空 scene C", "JUMP C: EMPTY",
                section="C", err="not_found")
    (s.ok if seq_song(s)["live"] != "C" else s.fail)("⑤ 空 scene 被拒后 live 不变")
    s.cmd("TAP %d %d" % song_card_xy(SEQ_SEC_B))
    s.expect(r"-> SONG card=B select", "⑤ 选中 B")
    song_action(s, song_sec_xy(3), "JUMP", "⑤ live loop 中 JUMP B", "JUMP B: OK", section="B")
    (s.ok if seq_wait_song(s, "live", "B", 6.0) else s.fail)(
        "⑤ JUMP 在下一小节生效：SEQ SECTION live=B")
    # JUMP 真的应用了**另一份**工程：section B 的轨 0 level = 80（A = 104）
    lvl = eng_track_level(s, SEQ_TRACK)
    (s.ok if lvl == SEQ_LEVEL_B else s.fail)(
        f"⑤ JUMP B 应用 section B：轨 0 level={lvl}（B 的 {SEQ_LEVEL_B}）")
    seq_stop(s, "⑤ JUMP 用例收尾")
    # 工作 loop 与 A、B 都不同：**选中轨 1** 的 level = 70。song_restore 若失效会停在最后
    # 应用的 scene（A=104 / B=80），不会撞上 h0 ⇒ 「恢复工作 loop」的断言可判别。
    seq_set_track_param(s, 1, 0, SEQ_LEVEL_WORK, "⑤ 工作 loop 的判别字段：轨 1 level 70")
    h0 = live_hash(s)
    s.ser.note(f"⑤ 判别哈希：A={hash_a:08X} B={hash_b:08X} 工作 loop(h0)={h0:08X}")
    (s.ok if h0 not in (hash_a, hash_b) else s.fail)(
        f"⑤ 工作 loop 的 hash 与 A/B 都不同（h0={h0:08X}）")
    # SONG：A2+B3 自然结束（5 小节 @120 = 10s）后必须恢复 playback 前的工作 loop 与选中轨
    store_cmd(s, "STORE SONG SET 2 0 0:2 1:3", r"^STORE SONG SET count=2 loop=0",
              "⑤ 歌曲 A2+B3 / loop=0")
    s.cmd("SEQ SONG MODE song")
    s.expect(r"SEQ SONG mode=song", "⑤ SONG 模式")
    seq_play(s, "⑤ SONG 播放")
    (s.ok if seq_wait_song(s, "running", 1, 3.0) else s.fail)("⑤ SEQ SONG running=1")
    time.sleep(1.0)   # 第一个小节：应用 section A（轨 0/1 level 都回到 104）
    l0, l1 = eng_track_level(s, SEQ_TRACK), eng_track_level(s, 1)
    (s.ok if l0 == SEQ_LEVEL_A and l1 == SEQ_LEVEL_A else s.fail)(
        f"⑤ SONG 播放中应用 section A：轨 0/1 level={l0}/{l1}（期望 {SEQ_LEVEL_A}/{SEQ_LEVEL_A}）")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑤ 回 TRANSPORT 等自然结束")
    if seq_wait_playing(s, 0, 18.0):
        s.ok("⑤ A2+B3 自然结束：playing=0")
    else:
        s.fail("⑤ A2+B3 18s 内未自然结束")
    song = seq_song(s)
    (s.ok if song["running"] == 0 and song["index"] == 1 else s.fail)(
        f"⑤ 自然结束：running={song['running']} index={song['index']}（期望 0/1，count 夹取）")
    h_end = live_hash(s)
    (s.ok if h_end == h0 else s.fail)(
        f"⑤ 自然结束恢复工作 loop：LIVEHASH={h_end:08X}（期望 {h0:08X}）")
    l1 = eng_track_level(s, 1)
    (s.ok if l1 == SEQ_LEVEL_WORK else s.fail)(
        f"⑤ 恢复后轨 1 level={l1}（工作 loop 的 {SEQ_LEVEL_WORK}，不是 scene 的 "
        f"{SEQ_LEVEL_A}/{SEQ_LEVEL_B}）")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 1, "⑤ 恢复选中轨 1（song_backup/song_restore）")
    (s.ok if song_section(s)["live"] == "B" else s.fail)("⑤ live 保持 B（song 应用不改 live）")
    # SONG 模式 + loop=1：中途 STOP 走 seq_stop 的 arrangement_clock.running 分支 → song_restore
    store_cmd(s, "STORE SONG SET 2 1 0:2 1:3", r"^STORE SONG SET count=2 loop=1",
              "⑤ 歌曲 loop=1")
    s.cmd("SEQ SONG MODE song")
    s.expect(r"SEQ SONG mode=song", "⑤ SONG 模式（loop=1）")
    seq_play(s, "⑤ loop=1 播放")
    (s.ok if seq_wait_song(s, "running", 1, 3.0) else s.fail)("⑤ loop=1 running=1")
    time.sleep(4.5)   # A2 播完 → section B 生效（轨 0 level=80、轨 1=104）
    l0, l1 = eng_track_level(s, SEQ_TRACK), eng_track_level(s, 1)
    (s.ok if l0 == SEQ_LEVEL_B and l1 == SEQ_LEVEL_A else s.fail)(
        f"⑤ loop=1 播到 section B：轨 0/1 level={l0}/{l1}（期望 {SEQ_LEVEL_B}/{SEQ_LEVEL_A}）")
    h_mid = live_hash(s)
    s.ser.note(f"⑤ loop=1 播到 B 时的 hash={h_mid:08X}（工作 loop h0={h0:08X}）")
    (s.ok if h_mid != h0 else s.fail)(
        "⑤ loop=1 中途引擎是 scene B 的状态（≠ 工作 loop，恢复断言不是空话）")
    # 视觉证据：SONG 页在 SONG 播放中（loop=1 会一直循环，截屏期间不结束）
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑤ UI song（截 SONG 播放态）")
    time.sleep(0.3)
    img = s.capture("song-live")
    s.probe_region(img, "⑤ SONG 状态行文本（UI_TEXT_DIM）", (24, 72 + 20, 1100, 72 + 80),
                   C_TEXT_DIM, 16, 40)
    s.probe_region(img, "⑤ 卡文本（4 张 A–D）", (40, 72 + 96 + 16, 1240, 72 + 96 + 90),
                   C_TEXT_DIM, 14, 80)
    s.probe_region(img, "⑤ 条目行文本（16 项）", (24, 72 + 300, 1240, 72 + 440),
                   C_TEXT_DIM, 14, 40)
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑤ 回 TRANSPORT")
    time.sleep(0.2)
    seq_stop(s, "⑤ loop=1 中途 STOP")
    h_stop = live_hash(s)
    (s.ok if h_stop == h0 else s.fail)(
        f"⑤ SONG 模式 loop=1 的 STOP 恢复工作 loop：LIVEHASH={h_stop:08X}（期望 {h0:08X}）")
    l1 = eng_track_level(s, 1)
    (s.ok if l1 == SEQ_LEVEL_WORK else s.fail)(
        f"⑤ STOP 后轨 1 level={l1}（恢复工作 loop 的 {SEQ_LEVEL_WORK}）")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 1, "⑤ STOP 后选中轨仍 1")
    song = seq_song(s)
    # loop=1 的 STOP 可以落在循环任一 entry（index 0/1 都可能）；恢复由 hash/level 判
    (s.ok if song["running"] == 0 and song["index"] in (0, 1) else s.fail)(
        f"⑤ STOP 后 SEQ SONG running={song['running']} index={song['index']}（期望 0，index∈0/1）")
    (s.ok if song_section(s)["live"] == "B" else s.fail)("⑤ STOP 后 live 仍是 B（song 不改 live）")
    # 缺 scene 整首拒播（C 为空）：PLAY 被拒，playing/running 都不动
    store_cmd(s, "STORE SONG SET 2 0 0:2 2:3", r"^STORE SONG SET count=2 loop=0",
              "⑤ 歌曲第 2 项改到空 scene C")
    s.cmd("SEQ SONG MODE song")
    s.expect(r"SEQ SONG mode=song", "⑤ SONG 模式（缺 scene）")
    seq_play_raw(s, "⑤ 缺 scene PLAY")
    time.sleep(1.5)
    (s.ok if seq_playing(s) == 0 else s.fail)("⑤ 缺 scene 整首拒播：playing=0")
    song = seq_song(s)
    (s.ok if song["running"] == 0 else s.fail)(
        f"⑤ 缺 scene 拒播：SEQ SONG running={song['running']}（期望 0）")
    store_cmd(s, "STORE SONG SET 2 0 0:2 1:3", r"^STORE SONG SET count=2 loop=0",
              "⑤ 恢复 A2+B3")

    # ---------- ⑥ SONG REC ----------
    # loop 模式下 REQUEST B 是**live 段切换**（arrangement_apply），没有 song_restore：
    # STOP 后引擎停在 B——不声称「恢复原 loop」（与 ⑤ 的 SONG 模式区分）。
    s.ser.note("⑥ SONG REC：未到下一小节取消不写配置、REC IS ON 拒 MODE、真实 take 录到 A→B；"
               "loop 模式的 live 切换在 STOP 后保留（停在 B，无 song_restore）")
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⑥ loop 模式")
    s.cmd("SEQ SECTION A LOAD")
    s.expect(r"SEQ SECTION A LOAD OK", "⑥ 工作 loop = A")
    h1 = live_hash(s)
    lvl = eng_track_level(s, SEQ_TRACK)
    (s.ok if lvl == SEQ_LEVEL_A else s.fail)(
        f"⑥ LOAD A：轨 0 level={lvl}（A 的 {SEQ_LEVEL_A}）")
    cfg0 = song_cfg(s)
    dirty0 = seq_song(s)["dirty"]
    seq_play(s, "⑥ 空 SONG REC 用例播放")
    s.cmd("SEQ SONG REC")
    s.expect(r"SEQ SREC srec=armed", "⑥ SONG REC arm（下一小节起录）")
    s.cmd("SEQ SONG MODE song")
    s.expect(r"^SEQ ERR blocked\r?$", "⑥ REC IS ON：MODE 切换 → SEQ ERR blocked（判别性反例）")
    s.cmd("SEQ SREC TOGGLE")     # armed 态再按一次：取消，不写配置
    s.expect(r"SEQ SREC srec=off last=none", "⑥ 未到下一小节取消：last=none（无 take）")
    seq_stop(s, "⑥ 空录制收尾")
    cfg1 = song_cfg(s)
    (s.ok if cfg1 == cfg0 else s.fail)(
        f"⑥ 空 SONG REC 不写配置：{cfg1}（期望 {cfg0}）")
    dirty1 = seq_song(s)["dirty"]
    (s.ok if dirty1 == dirty0 else s.fail)(
        f"⑥ 空 SONG REC 不置 dirty：{dirty1}（期望 {dirty0}）")

    seq_play(s, "⑥ 真实 take 播放")
    s.cmd("SEQ SECTION A LOAD")   # 播放中 LOAD 会被拒；这里只为记录拒绝路径
    s.expect(r"^SEQ ERR blocked\r?$", "⑥ 播放中 LOAD → SEQ ERR blocked（判别性反例）")
    s.cmd("SEQ SONG REC")
    s.expect(r"SEQ SREC srec=armed", "⑥ 真实 take：arm")
    (s.ok if seq_wait_srec(s, "recording", 4.0) else s.fail)(
        "⑥ 下一小节进入 recording")
    time.sleep(2.0)               # A 播满一小节（120 BPM = 2s）
    s.cmd("SEQ SECTION B REQUEST")
    s.expect(r"SEQ SECTION B REQUEST OK", "⑥ take 中 REQUEST B（下一小节切段）")
    time.sleep(2.4)               # B 从下一小节起播 ≥1 小节
    live_take = song_section(s)["live"]
    (s.ok if live_take == "B" else s.fail)(
        f"⑥ take 中 live 段={live_take}（期望 B）")
    lvl = eng_track_level(s, SEQ_TRACK)
    (s.ok if lvl == SEQ_LEVEL_B else s.fail)(
        f"⑥ REQUEST B 应用 section B：轨 0 level={lvl}（B 的 {SEQ_LEVEL_B}）")
    h_take = live_hash(s)
    s.ser.note(f"⑥ take STOP 前：live=B、轨 0 level={SEQ_LEVEL_B}、hash={h_take:08X}"
               f"（A 工作 loop h1={h1:08X}）")
    (s.ok if h_take != h1 else s.fail)("⑥ live 段切到 B 后引擎 ≠ A 工作 loop（判别的前提）")
    seq_stop(s, "⑥ take 收尾 STOP")
    srec = seq_srec(s)
    (s.ok if srec["srec"] == "off" and srec["last"] == "2" else s.fail)(
        f"⑥ take 结果：srec={srec['srec']} last={srec['last']}（期望 off/2 项）")
    cfg = song_cfg(s)
    ok = (cfg["count"] == 2 and cfg["entry"].get(0, (None, 0))[0] == SEQ_SEC_A
          and cfg["entry"].get(1, (None, 0))[0] == SEQ_SEC_B
          and cfg["entry"][0][1] >= 1 and cfg["entry"][1][1] >= 1 and cfg["loop"] == 0)
    (s.ok if ok else s.fail)(
        f"⑥ 录到的歌曲顺序：{cfg['entry']}（期望 count=2、A…/B… 各 ≥1 小节、loop=0）")
    (s.ok if seq_song(s)["dirty"] == 1 else s.fail)("⑥ take 结束置 song dirty=1（延迟写待办）")
    # loop 模式没有 song_restore：STOP 后停在 live 段 B（引擎与 STOP 前一致）
    h_after = live_hash(s)
    live_after = song_section(s)["live"]
    (s.ok if live_after == "B" else s.fail)(
        f"⑥ STOP 后 live 段={live_after}（期望 B：live 切换保留）")
    (s.ok if h_after == h_take else s.fail)(
        f"⑥ STOP 后引擎停在 B 的 live 段：LIVEHASH={h_after:08X}（期望 {h_take:08X}）")
    (s.ok if h_after != h1 else s.fail)(
        "⑥ 未回到 A 工作 loop（与 ⑤ SONG 模式的 song_restore 区分）")
    lvl = eng_track_level(s, SEQ_TRACK)
    (s.ok if lvl == SEQ_LEVEL_B else s.fail)(
        f"⑥ STOP 后轨 0 level={lvl}（B 的 {SEQ_LEVEL_B}）")
    store_cmd(s, "STORE SONG SAVE", r"^STORE SONG SAVED len=\d+ side=1\r?$",
              "⑥ STORE SONG SAVE（写 PER3）")
    s.cmd("STORE SONG INFO")
    store_expect(s, r"^STORE SONG INFO kind=per3 len=\d+ side=1 count=2 loop=0\r?$",
                 "⑥ flash 里的 PER3 歌曲 count=2 loop=0")
    s.cmd("STORE SONG LOAD")
    store_expect(s, r"^STORE SONG LOAD count=2 loop=0 dirty=0\r?$",
                 "⑥ STORE SONG LOAD 读回同一首歌")
    cfg2 = song_cfg(s)
    (s.ok if cfg2["entry"] == cfg["entry"] else s.fail)(
        f"⑥ LOAD 后逐项一致：{cfg2['entry']}")
    s.cmd("UI song")
    s.expect(r"UI page=song \(11\)", "⑥ UI song（截 take 态）")
    img = s.capture("song-rec")
    s.probe_region(img, "⑥ SONG 状态行 LAST 文本（UI_TEXT_DIM）", (24, 72 + 20, 1100, 72 + 80),
                   C_TEXT_DIM, 16, 40)
    s.probe_region(img, "⑥ 条目行文本（16 项）", (24, 72 + 300, 1240, 72 + 440),
                   C_TEXT_DIM, 14, 60)

    # ---------- ⑦ STORE TEST / SELFTEST 之后歌曲保持 ----------
    s.ser.note("⑦ STORE TEST 与单例 ENG SELFTEST 之后歌曲与 section 保持")
    seq_stop(s, "⑦ 停走带（STORE TEST 要求）")
    s.cmd("STORE TEST")
    store_expect(s, r"^STORE TEST PASS 6/6\r?$", "⑦ STORE TEST PASS 6/6")
    store_test_diag(s, "⑦")
    cfg3 = song_cfg(s)
    (s.ok if cfg3["entry"] == cfg["entry"] else s.fail)(
        f"⑦ STORE TEST 后歌曲逐项不变：{cfg3['entry']}")
    s.cmd("ENG AUDIO OFF")
    s.expect(r"ENG AUDIO streaming=0", "⑦ ENG AUDIO OFF（自检前置）")
    s.cmd("ENG SELFTEST drums/gm_kit", timeout=30.0)
    s.expect(r"^ENG SELFTEST cases=1 identical=1 mismatch=0", "⑦ 单例 SELFTEST 逐例同源")
    s.expect(r"result=PASS", "⑦ 单例 SELFTEST result=PASS")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "⑦ ENG AUDIO ON（恢复流式）")
    cfg4 = song_cfg(s)
    (s.ok if cfg4["entry"] == cfg["entry"] and cfg4["count"] == 2 else s.fail)(
        f"⑦ SELFTEST 后歌曲保持（song_reload 以 flash 为准）：{cfg4['entry']}")
    s.cmd("STORE SONG INFO")
    store_expect(s, r"^STORE SONG INFO kind=per3 .*count=2 loop=0\r?$",
                 "⑦ SELFTEST 后 flash 歌曲仍 per3/2 项")
    dirty7 = seq_song(s)["dirty"]
    (s.ok if dirty7 == 0 else s.fail)(f"⑦ SELFTEST 后 song dirty={dirty7}（期望 0）")
    sec = song_section(s)
    (s.ok if sec["ready"] & 0x03 == 0x03 else s.fail)(
        f"⑦ SELFTEST 后 section A/B 仍在（ready=0x{sec['ready']:02X}）")

    # ---------- ⑧ 硬复位：PER3 歌曲与 section 从 flash 恢复 ----------
    s.ser.note("⑧ esptool 硬复位（不写 flash）→ PER3 歌曲/section/工程槽开机恢复")
    seq_wait_dirty_clear(s, "⑧ 复位前的段落/歌曲落盘")
    s.cmd("STORE SONG SAVE")
    s.expect(r"^STORE SONG SAVED len=\d+ side=1\r?$", "⑧ 复位前 SAVE 歌曲")
    s.reboot()
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "⑧ 复位后 STORE AUTO OFF")
    s.cmd("STORE SONG")
    store_expect(s, r"^STORE SONG count=2 loop=0 dirty=0\r?$", "⑧ 复位后歌曲 count=2 loop=0")
    cfg5 = song_cfg(s)
    (s.ok if cfg5["entry"] == cfg["entry"] else s.fail)(
        f"⑧ 复位后歌曲逐项恢复：{cfg5['entry']}")
    s.cmd("STORE SONG INFO")
    store_expect(s, r"^STORE SONG INFO kind=per3 .*count=2 loop=0\r?$",
                 "⑧ 复位后 PER3 对象从 flash 读出")
    song = seq_song(s)
    (s.ok if song["count"] == 2 and song["loop"] == 0 else s.fail)(
        f"⑧ 复位后 SEQ SONG count={song['count']} loop={song['loop']}")
    sec = song_section(s)
    (s.ok if sec["ready"] & 0x03 == 0x03 else s.fail)(
        f"⑧ 复位后 section A/B 就绪：ready=0x{sec['ready']:02X}（开机 fetch RAM 槽）")
    # 开机 fetch 的 RAM 槽必须与 ③ 捕获的字节一致（hash 相等，不是「非空」）
    for slot, ref in ((SEQ_SEC_A, hash_a), (SEQ_SEC_B, hash_b)):
        got = store_hash(s, f"STORE PROJ HASH {slot}",
                         rf"^PROJ {slot} HASH=([0-9A-Fa-f]{{8}})\r?$",
                         f"⑧ 复位后工程槽 {'AB'[slot]} 的 RAM hash")
        (s.ok if got == ref else s.fail)(
            f"⑧ 复位后工程槽 {'AB'[slot]} 的 RAM hash={got:08X} == ③ 捕获的 {ref:08X}")

    # ---------- ⑨ 收口 ----------
    s.ser.note("⑨ 收口：擦 section A–C/D、默认歌曲、loop 模式、BPM 120、选轨 0、PLAY、AUTO ON")
    seq_stop(s, "⑨ 收口停走带")
    for slot in (SEQ_SEC_A, SEQ_SEC_B, SEQ_SEC_C, SEQ_DIAG_SLOT):
        proj_erase(s, slot, f"⑨ 擦工程槽 {'ABCD'[slot]}")
    store_cmd(s, "STORE SONG SET 4 0 0:4 1:4 2:4 3:4",
              r"^STORE SONG SET count=4 loop=0", "⑨ 恢复默认歌曲 A4 B4 C4 D4")
    store_cmd(s, "STORE SONG SAVE", r"^STORE SONG SAVED len=\d+ side=1\r?$",
              "⑨ 默认歌曲落盘")
    s.cmd("SEQ SONG MODE loop")
    s.expect(r"SEQ SONG mode=loop", "⑨ loop 模式")
    select_track(s, SEQ_TRACK)
    for tid, want, label in ((0, SEQ_LEVEL_A, "level 104"), (SEQ_AMODE_ID, 0, "ARP OFF"),
                             (SEQ_AHOLD_ID, 0, "HOLD OFF"), (SEQ_VOICE_ID, 0, "POLY"),
                             (SEQ_LEN_ID, 16, "LEN 16")):
        seq_set_track_param(s, SEQ_TRACK, tid, want, f"⑨ 轨 0 {label}")
    select_track(s, 1)
    seq_set_track_param(s, 1, 0, SEQ_LEVEL_A, "⑨ 轨 1 level 回 104（⑤ 的工作 loop 字段）")
    select_track(s, SEQ_TRACK)
    s.cmd("PARAM GLOBAL 0 120")
    s.expect(r"PARAM GLOBAL id=0 value=120 applied=120 ", "⑨ BPM 120")
    for k in range(4):
        pattern_clear(s, k, f"⑨ 清轨 {k} 图案")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑨ 回 PLAY")
    store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⑨ STORE AUTO ON（恢复默认）")


# ---- synth 场景（P4-D Task 11）：SYNTH 组三页 + 采样槽 + DSYN/FM6P 持久化 ----
#
# 覆盖 spec §5「设备」段：采样上传/擦除/BEGIN 中断/CRC 负例/走带拒绝、DSYN 编辑 → STORE →
# 硬复位 → SELFTEST 后仍逐字节一致、FM6P 编辑/ALG 清宏/锁定拒绝/银行往返与复位保持、
# 三页截屏与像素抽检、SYNTH 组 Σ/delta、SYNTH 全建 → STEP 首建 ≥4KB、跨组往返 ≤128B。
#
# 采样上传复用 scripts/sample_upload.py 的编码器与切块器（与上游 web 编辑器逐字节同一
# 实现）：本文件只把每行经 Session.cmd 发给 `SMP` console（同一串口会话，不重开端口）。
# fixture 是 2s 正弦（44,100 样本 @ 22050Hz）——1,000 样本（~45ms）太短，一次「按住键 →
# ENG SNAP voices」的读数会与采样结束赛跑；2s 给 ~0.5s 的观测窗留足余量。
SYNTH_SAMPLE_SLOT = 0          # USR1（`SMP BEGIN 0` / `PARAM 63 8`）
SYNTH_SAMPLE_SAMPLES = 44100   # 2s @ 22050 Hz
SYNTH_BANK_SLOT = 1
SYNTH_DSYN_KIT = 0
SYNTH_DSYN_LANE = 0
SYNTH_FM6_TRACK = 1
SYNTH_FM6_LOCKED_TRACK = 0
SYNTH_MUTE_ID = 40     # P_MUTE（param_kept：autosave 恢复后可能仍置位 ⇒ 发声前提先清零）
# FM6 155B patch 的绝对偏移（Task 3 冻结：op 0..5 各 21B，全局块 FP_PR1=126）
FM6P_OFF_ALG = 126 + 8         # 全局字段 8 = FP_ALG − FP_PR1
FM6P_OFF_R1 = 0                # op0 字段 0
FM6P_OFF_OL = 16               # op0 字段 16


def _smp_module():
    """延迟导入 sample_upload（编码器在 third_party/sloop-fm1/tools/sampleio.py）。"""
    import sample_upload
    return sample_upload


def smp_send_slot(s, slot: int, hdr: bytes, data: bytes, end_pattern: str,
                  label: str) -> None:
    """BEGIN → DATA（120B/块，整行 ≤240B）→ 4×HDR(120B) → END；END 的回显按
    end_pattern 断言（正常 `SMP END slot=… OK`、CRC 负例 `SMP ERR crc`）。"""
    smp = _smp_module()
    s.cmd(f"SMP BEGIN {slot}", timeout=20.0)
    s.expect(rf"^SMP BEGIN slot={slot} OK\r?$", f"{label}：SMP BEGIN {slot}")
    blocks = 0
    for off, chunk in smp.chunk_lines(slot, data):
        s.cmd(f"SMP DATA {slot} {off} {chunk.hex().upper()}")
        s.expect(rf"^SMP DATA slot={slot} off={off} n={len(chunk)} OK\r?$",
                 f"{label}：SMP DATA off={off}")
        blocks += 1
    for i in range(0, smp.SMP_HDR_LEN, 120):
        s.cmd(f"SMP HDR {hdr[i:i + 120].hex().upper()}")
        s.expect(r"^SMP HDR n=120 total=\d+ OK\r?$", f"{label}：SMP HDR {i // 120}")
    s.cmd(f"SMP END {slot}", timeout=30.0)
    s.expect(end_pattern, f"{label}：SMP END {slot} → {end_pattern}")
    s.ser.note(f"{label}：{len(data)}B ADPCM / {blocks} 块 DATA / 480B header（USR{slot + 1}）")


def smp_upload_sine(s, slot: int = SYNTH_SAMPLE_SLOT, name: str = "SINE") -> bytes:
    """上传 2s 正弦到 USR{slot+1}，返回 ADPCM 数据（CRC 负例据此改坏一个字节）。"""
    smp = _smp_module()
    hdr, data = smp.build_sine_slot(name, n=SYNTH_SAMPLE_SAMPLES)
    smp.check_slot(hdr, data)  # 本地预检：失败不必开始擦写
    smp_send_slot(s, slot, hdr, data, rf"^SMP END slot={slot} OK\r?$", f"上传 {name}")
    return data


def smp_info_slot(s, slot: int):
    """`SMP INFO` 里槽 slot 的事实：None = EMPTY；dict(name, zones, kb) = USED。"""
    s.cmd("SMP INFO")
    m = re.search(rf"^SMP {slot} (EMPTY|USED name=(\S+) zones=(\d+) kb=(\d+))\r?$",
                  s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"SMP INFO 缺少槽 {slot} 的行：{s.last!r}")
    if m.group(1) == "EMPTY":
        return None
    return {"name": m.group(2), "zones": int(m.group(3)), "kb": int(m.group(4))}


def eng_voices(s, track: int) -> int:
    """`ENG SNAP` 的 trk track 行 voices（引擎真值）。"""
    s.cmd_fast("ENG SNAP")
    m = re.search(rf"^ENG SNAP trk k={track} .* voices=(\d+)\r?$", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"ENG SNAP trk k={track} 缺 voices=（{s.last!r}）")
    return int(m.group(1))


def eng_voices_max(s, track: int, seconds: float) -> int:
    """在 seconds 窗口内**最大**的 voices（空格静音的判据：整窗恒 0）。"""
    end = time.monotonic() + seconds
    best = 0
    while time.monotonic() < end:
        best = max(best, eng_voices(s, track))
        time.sleep(0.02)
    return best


def synth_key(s, code: int, down: bool) -> None:
    s.cmd(f"KEY 0x{code:02X} {'down' if down else 'up'}")
    s.expect(rf"KEY 0x{code:02X} {'down' if down else 'up'}",
             f"KEY 0x{code:02X} {'down' if down else 'up'}")


def dsyn_get(s, kit: int, lane: int) -> dict:
    """`DSYN GET <kit> <lane>` → name/crush/src/dirty/sound(44 hex)。"""
    s.cmd(f"DSYN GET {kit} {lane}")
    m = re.search(r"DSYN GET k=(\d+) l=(\d+) name=(\S+) crush=(\d+) src=(\d+) dirty=(\d+) "
                  r"sound=([0-9A-Fa-f]{44})", s.last)
    if m is None:
        raise InjectError(f"DSYN GET 回显缺字段：{s.last.strip()!r}")
    return {"kit": int(m.group(1)), "lane": int(m.group(2)), "name": m.group(3),
            "crush": int(m.group(4)), "src": int(m.group(5)), "dirty": int(m.group(6)),
            "sound": m.group(7).upper()}


def dsyn_fields(s) -> dict:
    """`DSYN INFO` 的 f=（24 个虚拟字段值，顺序即协议）+ kit/lane/crush/src/dirty。"""
    s.cmd("DSYN INFO")
    m = re.search(r"^DSYN INFO kit=(\d+) lane=(\d+) name=(\S+) crush=(\d+) src=(\d+) "
                  r"dirty=(\d+) f=([-\d,]+)\r?$", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"DSYN INFO 回显缺字段：{s.last.strip()!r}")
    values = [int(v) for v in m.group(7).split(",")]
    if len(values) != 24:
        raise InjectError(f"DSYN INFO f= 不是 24 项（{len(values)}）")
    return {"kit": int(m.group(1)), "lane": int(m.group(2)), "name": m.group(3),
            "crush": int(m.group(4)), "src": int(m.group(5)), "dirty": int(m.group(6)),
            "f": values}


def dsyn_set(s, kit: int, lane: int, field: int, value: int, label: str) -> None:
    s.cmd(f"DSYN SET {kit} {lane} {field} {value}")
    s.expect(rf"DSYN SET k={kit} l={lane} f={field} value={value} status=\"ok\"", label)


def fm6p_get(s, track: int) -> dict:
    s.cmd(f"FM6P GET {track}")
    m = re.search(r"FM6P GET track=(\d+) slot=(\d+) engine_ok=(\d+) ([0-9A-Fa-f]{310})", s.last)
    if m is None:
        raise InjectError(f"FM6P GET 回显缺字段：{s.last.strip()!r}")
    return {"track": int(m.group(1)), "slot": int(m.group(2)),
            "engine_ok": int(m.group(3)), "patch": m.group(4).upper()}


def fm6p_info(s, track: int) -> dict:
    s.cmd(f"FM6P INFO {track}")
    m = re.search(r'FM6P INFO track=(\d+) slot=(\d+) engine_ok=(\d+) name="([^"]*)" '
                  r'alg=(-?\d+) macro=(\d+)', s.last)
    if m is None:
        raise InjectError(f"FM6P INFO 回显缺字段：{s.last.strip()!r}")
    return {"track": int(m.group(1)), "slot": int(m.group(2)), "engine_ok": int(m.group(3)),
            "name": m.group(4), "alg": int(m.group(5)), "macro": int(m.group(6))}


def fm6p_byte(patch_hex: str, offset: int) -> int:
    return int(patch_hex[offset * 2:offset * 2 + 2], 16)


def run_synth(s: Session) -> None:
    """synth 场景（P4-D Task 11）：SYNTH 组三页 + 采样槽 + DSYN/FM6P 持久化 + 池水位。

    ⓪ 归一（AUTO OFF / 停走带 / 轨 0 / USR1 空 / DSYN 工厂 808 / 银行 0..2 空 / 窗口）
    ① 采样：INFO 空 → 上传 2s 正弦 → INFO USED → 按住键 voices≥1 → ERASE → 静音；
       BEGIN 无 END（镜像失效 ⇒ 静音）；CRC 坏数据 ⇒ `SMP ERR crc` 且槽仍空；
       走带中 BEGIN/ERASE ⇒ `SMP ERR playing`
    ② DSYN：GET 22B → SET 四字段 + CRUSH → INFO 回读 → AUDITION → STORE（dirty=0）→
       硬复位 GET 逐字节相同（DSU 从 PER3 恢复）→ 单例 SELFTEST 后仍相同（DSU reload）
    ③ FM6P：FM6 轨 GET 155B → SET ALG/OP1 R1/OL → 回读字节 → ALG 清宏 → 非 FM6 轨
       SET `locked` 且 patch 不变 → `STORE BANK PUT` + GET==LIVE → 硬复位后银行槽仍在
    ④ UI：三页截屏 + 像素抽检（组签/子签/SAMPLE 槽行/DSYN 字段页/FM6P 锁定 `--`）、
       `UI MEM` SYNTH Σ 20,480 与逐页 delta ≤ est、TAP 回显与屏幕状态行同源
    ⑤ 池：SYNTH 三页全建 → STEP 首建 `pool_free ≥4,096`；SYNTH/PLAY/SONG/FILE 往返 ≤128B
    ⑥ 收口：擦 USR1、DSYN 工厂 808 + STORE、银行槽擦除、轨 1/0 → ANALOG、AUTO ON

    **破坏性**（占用 USR1、SYN kit 0、银行槽 1、可能改写 autosave 工程），自带归一、可重复。
    注：DSYN 的 AUDITION 只断言命令/状态行——鼓引擎的 voices 不在 `ENG SNAP` 里
    （trk[3].voices 恒 0，鼓声走 drums.v[]），可听性仍是人耳门禁。
    """
    # ---------- ⓪ 归一 ----------
    s.ser.note("⓪ 归一：AUTO OFF / 停走带 / 轨 0 / USR1 空 / DSYN 工厂 808 / 银行 0..2 空")
    s.cmd("STORE AUTO OFF")
    s.expect(r"^STORE AUTO off", "⓪ STORE AUTO OFF")
    ensure_window(s, 0, 16)
    ensure_stopped(s)
    select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⓪ 轨 0 → ANALOG 预置 0")
    # P_MUTE 是 param_kept：上一轮场景/autosave 可能把轨 0 留在静音（原 DI-23），
    # 静音时 note-on 不分配 voice ⇒ ① 的 voices 断言会假红。记原值、清零、收口还原。
    s.cmd(f"PARAM {SYNTH_MUTE_ID}")
    mute0 = param_value(s)
    s.cmd(f"PARAM {SYNTH_MUTE_ID} 0")
    s.expect(rf"PARAM track=0 id={SYNTH_MUTE_ID} value=0 applied=0",
             "⓪ 归一 P_MUTE=0（① 的发声前提）")
    s.cmd(f"SMP ERASE {SYNTH_SAMPLE_SLOT}")
    s.expect(rf"^SMP ERASE slot={SYNTH_SAMPLE_SLOT} OK\r?$", "⓪ 擦 USR1")
    s.cmd("SMP INFO")
    s.expect(rf"^SMP {SYNTH_SAMPLE_SLOT} EMPTY\r?$", "⓪ SMP INFO：USR1 空")
    s.cmd(f"DSYN KIT {SYNTH_DSYN_KIT} 0")   # 工厂 kit 0 = 808（src 0）
    s.expect(rf"DSYN KIT k={SYNTH_DSYN_KIT} src=0 status=\"ok\"",
             "⓪ DSYN kit 0 = 工厂 808")
    for slot in range(3):
        s.cmd(f"STORE BANK ERASE {slot}")
        s.expect(rf"^BANK {slot} ERASED\r?$", f"⓪ 擦银行槽 {slot}")

    # ---------- ① 采样槽：上传 → 出声 → 擦除 → 静音 ----------
    s.ser.note("① 采样：上传 → voices>0 → ERASE → 静音；BEGIN 中断 / CRC / 走带拒绝")
    s.cmd("ENG TRACK 0 4 0")
    s.expect(r"ENG TRACK k=0 engine=4 preset=0 OK", "① ENG TRACK 轨 0 → SAMPLE")
    s.cmd(f"PARAM 63 {8 + SYNTH_SAMPLE_SLOT}")   # SET = SMP_NSETS + 槽（USR1 = 8）
    s.expect(rf"PARAM track=0 id=63 value={8 + SYNTH_SAMPLE_SLOT} applied={8 + SYNTH_SAMPLE_SLOT}",
             "① PARAM 63 = 8（SET = USR1）")
    data = smp_upload_sine(s)
    info = smp_info_slot(s, SYNTH_SAMPLE_SLOT)
    (s.ok if info is not None and info["name"] == "SINE" and info["zones"] == 1
     and info["kb"] == 21 else s.fail)(
        f"① SMP INFO：USR1 {info}（期望 name=SINE zones=1 kb=21 = 22,050B/1024）")
    synth_key(s, 0x42, True)
    vmax = eng_voices_max(s, 0, 0.6)
    (s.ok if vmax >= 1 else s.fail)(
        f"① 按住 C：轨 0 voices 峰值 {vmax}（期望 ≥1 = 采样出声）")
    synth_key(s, 0x42, False)
    (s.ok if eng_voices_poll(s, 0, 0, 1.0) == 0 else s.fail)("① 松键后轨 0 voices 回 0")

    s.cmd(f"SMP ERASE {SYNTH_SAMPLE_SLOT}")
    s.expect(rf"^SMP ERASE slot={SYNTH_SAMPLE_SLOT} OK\r?$", "① SMP ERASE USR1")
    (s.ok if smp_info_slot(s, SYNTH_SAMPLE_SLOT) is None else s.fail)(
        "① 擦除后 SMP INFO：USR1 EMPTY")
    synth_key(s, 0x42, True)
    vmax = eng_voices_max(s, 0, 0.6)
    (s.ok if vmax == 0 else s.fail)(
        f"① 空槽按住 C：轨 0 voices 峰值 {vmax}（期望 0 = 静音）")
    synth_key(s, 0x42, False)

    # BEGIN 不 END：header 扇区已擦 + 镜像失效 ⇒ INFO 空且触发静音
    s.cmd(f"SMP BEGIN {SYNTH_SAMPLE_SLOT}", timeout=20.0)
    s.expect(rf"^SMP BEGIN slot={SYNTH_SAMPLE_SLOT} OK\r?$", "① SMP BEGIN（不 END）")
    (s.ok if smp_info_slot(s, SYNTH_SAMPLE_SLOT) is None else s.fail)(
        "① BEGIN 未 END 后 SMP INFO：USR1 EMPTY（镜像已失效）")
    synth_key(s, 0x42, True)
    vmax = eng_voices_max(s, 0, 0.6)
    (s.ok if vmax == 0 else s.fail)(
        f"① BEGIN 中断后按住 C：轨 0 voices 峰值 {vmax}（期望 0）")
    synth_key(s, 0x42, False)

    # CRC 负例：数据坏一个字节、header 仍合法 ⇒ END 回 crc 且槽仍空
    smp = _smp_module()
    hdr, _ = smp.build_sine_slot("SINE", n=SYNTH_SAMPLE_SAMPLES)
    bad = bytearray(data)
    bad[5] ^= 0x5A
    smp_send_slot(s, SYNTH_SAMPLE_SLOT, hdr, bytes(bad), r"^SMP ERR crc\r?$",
                  "① CRC 负例")
    (s.ok if smp_info_slot(s, SYNTH_SAMPLE_SLOT) is None else s.fail)(
        "① CRC 拒绝后 SMP INFO：USR1 EMPTY（header 未提交）")

    # 走带中拒绝上传/擦除（Tab5 安全差异，spec §3.5）
    seq_play(s, "① 走带")
    s.cmd(f"SMP BEGIN {SYNTH_SAMPLE_SLOT}", timeout=20.0)
    s.expect(r"^SMP ERR playing\r?$", "① 播放中 SMP BEGIN → SMP ERR playing")
    s.cmd(f"SMP ERASE {SYNTH_SAMPLE_SLOT}")
    s.expect(r"^SMP ERR playing\r?$", "① 播放中 SMP ERASE → SMP ERR playing")
    seq_stop(s, "① 停走带")

    # 为 UI 证据再上传一次（SAMPLE 页槽行显示 USED）
    smp_upload_sine(s)

    # ---------- ② DSYN：编辑 → STORE → 硬复位 / SELFTEST 后逐字节一致 ----------
    s.ser.note("② DSYN：GET 22B → 四字段 + CRUSH → AUDITION → STORE → 硬复位/SELFTEST")
    base = dsyn_get(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE)
    s.ser.note(f"② DSYN 基线 {base['name']} crush={base['crush']} src={base['src']} "
               f"sound={base['sound']}")
    dsyn_set(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE, 0, 2, "② WAVE = 2 (TRI)")
    dsyn_set(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE, 3, 12, "② PITCH = 12")
    dsyn_set(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE, 8, 20, "② DECAY = 20")
    dsyn_set(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE, 23, 180, "② LEVEL = 180")
    s.cmd("DSYN CRUSH 0 40")
    s.expect(r"DSYN CRUSH k=0 crush=40 status=\"ok\"", "② DSYN CRUSH 0 = 40")
    s.cmd(f"UI DSYN KIT {SYNTH_DSYN_KIT}")
    s.expect(rf"UI DSYN kit={SYNTH_DSYN_KIT} lane=\d+ page=\d+", "② UI DSYN KIT 0")
    s.cmd(f"UI DSYN LANE {SYNTH_DSYN_LANE}")
    s.expect(rf"UI DSYN kit={SYNTH_DSYN_KIT} lane={SYNTH_DSYN_LANE} page=\d+", "② UI DSYN LANE 0")
    f = dsyn_fields(s)
    got = {"WAVE": f["f"][0], "PITCH": f["f"][3], "DECAY": f["f"][8], "LEVEL": f["f"][23]}
    (s.ok if got == {"WAVE": 2, "PITCH": 12, "DECAY": 20, "LEVEL": 180} else s.fail)(
        f"② DSYN INFO 字段回读 {got}（期望 WAVE=2 PITCH=12 DECAY=20 LEVEL=180）")
    (s.ok if f["crush"] == 40 and f["dirty"] == 1 else s.fail)(
        f"② DSYN INFO crush={f['crush']} dirty={f['dirty']}（期望 40 / 1=待落盘）")
    s.cmd(f"DSYN AUDITION {SYNTH_DSYN_KIT} {SYNTH_DSYN_LANE}")
    s.expect(r'DSYN AUDITION k=0 l=0 vel=100 status="ok"', "② DSYN AUDITION（试听队列）")
    s.cmd("DSYN STORE")
    s.expect(r'DSYN STORE dirty=0 status="ok"', "② DSYN STORE（DSU 写回 PER3）")
    edited = dsyn_get(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE)
    (s.ok if edited["sound"] != base["sound"] else s.fail)(
        "② 编辑后 22B 与工厂 808 不同（判别的前提）")
    (s.ok if edited["dirty"] == 0 else s.fail)(f"② STORE 后 dirty={edited['dirty']}（期望 0）")

    s.reboot()
    s.cmd("STORE AUTO OFF")
    s.expect(r"^STORE AUTO off", "② 复位后 STORE AUTO OFF")
    after = dsyn_get(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE)
    (s.ok if after["sound"] == edited["sound"] else s.fail)(
        f"② 硬复位后 DSYN GET 逐字节一致：{after['sound']}（期望 {edited['sound']}）")
    (s.ok if after["crush"] == 40 and after["dirty"] == 0 else s.fail)(
        f"② 硬复位后 crush={after['crush']} dirty={after['dirty']}（期望 40 / 0）")
    (s.ok if smp_info_slot(s, SYNTH_SAMPLE_SLOT) is not None else s.fail)(
        "② 硬复位后 USR1 仍 USED（采样槽从 flash 装载）")
    ensure_stopped(s)
    s.cmd("ENG AUDIO OFF")
    s.expect(r"ENG AUDIO streaming=0", "② ENG AUDIO OFF（自检前置）")
    s.cmd("ENG SELFTEST drums/gm_kit", timeout=30.0)
    s.expect(r"^ENG SELFTEST cases=1 identical=1 mismatch=0", "② 单例 SELFTEST 同源")
    s.expect(r"result=PASS", "② 单例 SELFTEST PASS")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "② ENG AUDIO ON（恢复流式）")
    post = dsyn_get(s, SYNTH_DSYN_KIT, SYNTH_DSYN_LANE)
    (s.ok if post["sound"] == edited["sound"] else s.fail)(
        f"② SELFTEST 后 DSYN GET 仍一致（DSU reload）：{post['sound']}")

    # ---------- ③ FM6P：编辑 / ALG 清宏 / 锁定拒绝 / 银行槽 ----------
    s.ser.note("③ FM6P：FM6 轨编辑 + ALG 清宏 + 非 FM6 锁定 + 银行往返 + 复位保持")
    select_track(s, SYNTH_FM6_TRACK)
    s.cmd(f"ENG TRACK {SYNTH_FM6_TRACK} 9 0")
    s.expect(r"ENG TRACK k=1 engine=9 preset=0 OK", "③ 轨 1 → FM6 预置 0")
    info = fm6p_info(s, SYNTH_FM6_TRACK)
    (s.ok if info["engine_ok"] == 1 else s.fail)(
        f"③ FM6P INFO engine_ok={info['engine_ok']}（期望 1）")
    p0 = fm6p_get(s, SYNTH_FM6_TRACK)
    (s.ok if p0["engine_ok"] == 1 else s.fail)("③ FM6P GET 155B（engine_ok=1）")
    s.cmd(f"FM6P SET {SYNTH_FM6_TRACK} 6 8 12")
    s.expect(r'FM6P SET track=1 op=6 field=8 value=12 status="ok"', "③ SET ALG = 12")
    s.cmd(f"FM6P SET {SYNTH_FM6_TRACK} 0 0 20")
    s.expect(r'FM6P SET track=1 op=0 field=0 value=20 status="ok"', "③ SET OP1 R1 = 20")
    s.cmd(f"FM6P SET {SYNTH_FM6_TRACK} 0 16 70")
    s.expect(r'FM6P SET track=1 op=0 field=16 value=70 status="ok"', "③ SET OP1 OL = 70")
    p1 = fm6p_get(s, SYNTH_FM6_TRACK)
    got = (fm6p_byte(p1["patch"], FM6P_OFF_ALG), fm6p_byte(p1["patch"], FM6P_OFF_R1),
           fm6p_byte(p1["patch"], FM6P_OFF_OL))
    (s.ok if got == (12, 20, 70) else s.fail)(
        f"③ 155B 偏移回读 ALG@134={got[0]} R1@0={got[1]} OL@16={got[2]}（期望 12/20/70）")
    info = fm6p_info(s, SYNTH_FM6_TRACK)
    (s.ok if info["alg"] == 12 and info["macro"] == 0 else s.fail)(
        f"③ FM6P INFO alg={info['alg']} macro={info['macro']}（期望 12 / 0）")
    # ALG 宏覆盖：`PARAM 63 5`（ALG 宏 = 5）→ `FM6P ALG 0`（PAT）只清宏、不动 patch
    s.cmd("PARAM 63 5")
    s.expect(r"PARAM track=1 id=63 value=5 applied=5", "③ PARAM 63 = 5（ALG 宏）")
    s.cmd(f"FM6P ALG {SYNTH_FM6_TRACK} 0")
    s.expect(r'FM6P ALG track=1 value=0 alg=12 macro=0 status="ok"',
             "③ FM6P ALG 0 = PAT：宏清零、patch 的 ALG 保持 12")
    param_expect_value(s, "PARAM 63", 0, "③ PARAM 63（宏已被清）")
    # 锁定：非 FM6 轨（track 0 = SAMPLE）写入被拒且 patch 不变
    select_track(s, SYNTH_FM6_LOCKED_TRACK)
    locked0 = fm6p_get(s, SYNTH_FM6_LOCKED_TRACK)
    s.cmd(f"FM6P SET {SYNTH_FM6_LOCKED_TRACK} 0 0 50")
    s.expect(r"^FM6P ERR locked\r?$", "③ 非 FM6 轨 FM6P SET → FM6P ERR locked")
    locked1 = fm6p_get(s, SYNTH_FM6_LOCKED_TRACK)
    (s.ok if locked1["patch"] == locked0["patch"] else s.fail)(
        "③ 锁定页 SET 被拒后 patch 155B 逐字节不变")
    # 银行：PUT = 选中轨 live patch；GET == LIVE
    select_track(s, SYNTH_FM6_TRACK)
    s.cmd(f"STORE BANK PUT {SYNTH_BANK_SLOT}")
    s.expect(rf"^BANK {SYNTH_BANK_SLOT} SAVED\r?$", f"③ STORE BANK PUT {SYNTH_BANK_SLOT}")
    saved_bank = bank_get(s, SYNTH_BANK_SLOT)
    (s.ok if saved_bank == bank_live(s) else s.fail)(
        f"③ BANK GET {SYNTH_BANK_SLOT} == BANK LIVE（128B packed 逐字节）")

    s.reboot()
    s.cmd("STORE AUTO OFF")
    s.expect(r"^STORE AUTO off", "③ 复位后 STORE AUTO OFF")
    got_bank = bank_get(s, SYNTH_BANK_SLOT)
    (s.ok if got_bank == saved_bank else s.fail)(
        f"③ 硬复位后 BANK GET {SYNTH_BANK_SLOT} 逐字节保持（{got_bank[:16]}…）")
    s.cmd("STORE BANK LIST")
    s.expect(rf"^B{SYNTH_BANK_SLOT} USED\r?$", f"③ 复位后 B{SYNTH_BANK_SLOT} USED")
    ensure_stopped(s)

    # ---------- ④ UI：三页截屏 + 像素抽检 + SYNTH Σ ----------
    s.ser.note("④ UI：SAMPLE/DSYN/FM6P 三页截屏与像素抽检 + SYNTH Σ/delta")
    # SAMPLE：轨 0 = SAMPLE + SET=USR1（USR1 已上传）
    select_track(s, 0)
    s.cmd("ENG TRACK 0 4 0")
    s.expect(r"ENG TRACK k=0 engine=4 preset=0 OK", "④ 轨 0 → SAMPLE")
    s.cmd(f"PARAM 63 {8 + SYNTH_SAMPLE_SLOT}")
    s.expect(r"PARAM track=0 id=63 value=8 applied=8", "④ SET = USR1")
    s.cmd("UI sample")
    s.expect(r"UI page=sample \(8\)", "④ UI sample")
    s.cmd("TAP %d %d" % sample_slot_xy(0))   # 槽 U1（选中 → SET U1）
    s.expect(r"-> SAMPLE slot=0 select", "④ TAP 槽 U1")
    s.cmd("TAP %d %d" % param_knob_xy(3))   # 焦点移到 LOOP：旋钮 0 的文本才是非焦点色
    s.expect(r"-> PARAM slot=3 focus", "④ TAP 旋钮 3（焦点移开 SET）")
    img = s.capture("synth-sample")
    s.probe(img, "④ 组签选中（SYNTH）", *C_TAB_SYNTH, C_YELLOW)
    s.probe(img, "④ 子签选中（SAMPLE）", *C_TAB_SAMPLE, C_YELLOW)
    # SET=8 的指示弧会盖住 12 点方向：改探非焦点旋钮的名称/数值文本
    s.probe_region(img, "④ 旋钮 0 名称/数值文本", (100, 214, 232, 246), C_TEXT_DIM, 16, 30)
    s.probe_region(img, "④ 槽 U1 文本（USED name=SINE，选中色）",
                   SAMPLE_SLOT_BOX, C_YELLOW, 24, 30)
    s.probe_region(img, "④ 状态行（SET U1 = 轨 1 绿）", SAMPLE_STATUS_BOX, C_GREEN, 24, 30)
    mem = param_mem(s)
    st, est, delta = mem["pages"].get(8, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est else s.fail)(
        f"④ UI MEM SAMPLE state={st} delta={delta} est={est}（0<delta≤est）")

    # DSYN：SND3 字段页 + 选中 SYN1；TAP 底部 + 写一个字段再 − 回原位
    s.cmd("UI dsyn")
    s.expect(r"UI page=dsyn \(12\)", "④ UI dsyn")
    s.cmd("UI DSYN PAGE 2")
    s.expect(r"UI DSYN kit=0 lane=0 page=2", "④ UI DSYN PAGE 2（SND3）")
    # FCUT 归一 100（工厂 kit 0 的默认是 127）：± 各一步才有真值变化
    s.cmd(f"DSYN SET {SYNTH_DSYN_KIT} {SYNTH_DSYN_LANE} 18 100")
    s.expect(r"DSYN SET k=0 l=0 f=18 value=100 status=\"ok\"", "④ DSYN SET FCUT = 100")
    s.cmd("TAP %d %d" % param_knob_xy(2))   # 旋钮 2（FCUT，0..127）设焦点
    s.expect(r"-> DSYN kit=0 lane=0 page=2 select", "④ TAP DSYN 旋钮 2（FCUT）")
    img = s.capture("synth-dsyn")
    s.probe(img, "④ 组签选中（SYNTH）", *C_TAB_SYNTH, C_YELLOW)
    s.probe(img, "④ 子签选中（DSYN）", *C_TAB_DSYN, C_YELLOW)
    s.probe(img, "④ kit chip SYN1（选中）", *dsyn_kit_chip_probe(0), C_YELLOW)
    s.probe(img, "④ 字段页 SND3（选中 chip 底）", *dsyn_page_chip_probe(2), C_YELLOW)
    # 非焦点旋钮的名称/数值文本（UI_TEXT_DIM）；环顶会被 FCUT=100 的指示弧盖住，不探
    s.probe_region(img, "④ 旋钮 0 名称/数值文本", (100, 214, 232, 246), C_TEXT_DIM, 16, 30)
    s.cmd("TAP %d %d" % dsyn_adj_xy(True))   # 底部 +（焦点槽 FCUT +1）
    s.expect(r'-> DSYN field=FCUT value=101 status="EDIT OK( DIRTY)?"',
             "④ DSYN 底部 + → 写后回读 101 + EDIT OK")
    s.cmd("TAP %d %d" % dsyn_adj_xy(False))  # 底部 −（回 100）
    s.expect(r'-> DSYN field=FCUT value=100 status="EDIT OK( DIRTY)?"',
             "④ DSYN 底部 − → 写后回读 100")
    # 键盘 ↑/↓（sloop_ui_dsyn_adjust → dsyn_step_st → dsyn_write_st：输入任务路径上整条链
    # 只持一份快照，R13；焦点槽仍是 FCUT）。此前 synth 只走 TAP/旋钮，这条路径没有设备用例。
    def dsyn_fcut() -> int:
        s.cmd("DSYN INFO")
        m = re.search(r"f=([-\d,]+)", s.last)
        if m is None:
            raise InjectError("DSYN INFO 缺少 f= 字段表")
        return int(m.group(1).split(",")[18])   # FCUT = 第 18 个虚拟字段（SND3）

    f0 = dsyn_fcut()
    s.cmd("KEY 0x3B down")   # ↑：焦点槽 +1
    s.cmd("KEY 0x3B up")
    f1 = dsyn_fcut()
    (s.ok if f1 == f0 + 1 else s.fail)(f"④ DSYN 键盘 ↑ → FCUT {f0} → {f1}")
    s.cmd("KEY 0x4B down")   # ↓：回原值
    s.cmd("KEY 0x4B up")
    (s.ok if dsyn_fcut() == f0 else s.fail)(f"④ DSYN 键盘 ↓ → FCUT 回 {f0}")
    mem = param_mem(s)
    st, est, delta = mem["pages"].get(12, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est else s.fail)(
        f"④ UI MEM DSYN state={st} delta={delta} est={est}（0<delta≤est）")

    # FM6P：VOICE 页（OP3）+ 解锁态（轨 1 = FM6）… 先把轨 1 置 FM6
    select_track(s, SYNTH_FM6_TRACK)
    s.cmd(f"ENG TRACK {SYNTH_FM6_TRACK} 9 0")
    s.expect(r"ENG TRACK k=1 engine=9 preset=0 OK", "④ 轨 1 → FM6（解锁 FM6P 页）")
    s.cmd("UI fm6p")
    s.expect(r"UI page=fm6p \(13\)", "④ UI fm6p")
    s.cmd("UI FM6P OP 2")
    s.expect(r"UI FM6P op=2 page=\d+", "④ UI FM6P OP 2（OP3）")
    s.cmd("UI FM6P PAGE 4")
    s.expect(r"UI FM6P op=2 page=4", "④ UI FM6P PAGE 4（VOICE）")
    # ALG 归一到 4（工厂预置的 ALG 很高，指示弧会盖住环顶探针；写 ALG 也会清 ALG 宏）
    s.cmd(f"FM6P ALG {SYNTH_FM6_TRACK} 5")
    s.expect(r'FM6P ALG track=1 value=5 alg=4 macro=0 status="ok"',
             "④ FM6P ALG = 4（环顶探针前提 + 宏清零）")
    s.cmd("TAP %d %d" % param_knob_xy(0))   # 旋钮 0（ALG）设焦点
    s.expect(r"-> FM6P op=2 page=4 select", "④ TAP FM6P 旋钮 0（ALG）")
    img = s.capture("synth-fm6p")
    s.probe(img, "④ 组签选中（SYNTH）", *C_TAB_SYNTH, C_YELLOW)
    s.probe(img, "④ 子签选中（FM6P）", *C_TAB_FM6P, C_YELLOW)
    s.probe(img, "④ OP3 chip（选中）", *fm6p_op_chip_probe(2), C_YELLOW)
    s.probe(img, "④ VOICE chip（选中）", *fm6p_page_chip_probe(4), C_YELLOW)
    s.probe(img, "④ 旋钮 0 弧线底（环顶）", 166, 158, C_PANEL_HI, 10)
    # 空槽（VOICE 页 4..7）不建旋钮：行 1 的四列环顶是页面背景
    for col in range(4):
        s.probe(img, f"④ VOICE 空槽 {4 + col}（行 1 列 {col}）无旋钮",
                PARAM_GRID_X0 + col * PARAM_CELL_PITCH + PARAM_OBJ_DX + PARAM_OBJ_SIZE // 2,
                PARAM_GRID_Y0 + PARAM_ROW_PITCH + 32, C_BG, 12)
    s.cmd("TAP %d %d" % fm6p_adj_xy(True))   # 底部 +（焦点槽 ALG +1）
    s.expect(r'-> FM6P ALG value=\d+ status="[^"]*"', "④ FM6P 底部 + → 写后回读")
    s.cmd("TAP %d %d" % fm6p_adj_xy(False))  # 底部 −（回原位）
    s.expect(r'-> FM6P ALG value=\d+ status="[^"]*"', "④ FM6P 底部 −（回原位）")
    # 键盘 ↑/↓（sloop_ui_fm6p_adjust → fm6p_step_st → fm6p_write_st：输入任务路径上整条链
    # 只持一份快照，R13；焦点槽仍是 ALG）。断言用 FM6P INFO 的 alg（引擎回读）。
    def fm6p_alg() -> int:
        s.cmd(f"FM6P INFO {SYNTH_FM6_TRACK}")
        return s.snap_int(r"alg=(-?\d+)")

    a0 = fm6p_alg()
    s.cmd("KEY 0x3B down")   # ↑：焦点槽 +1
    s.cmd("KEY 0x3B up")
    a1 = fm6p_alg()
    (s.ok if a1 == a0 + 1 else s.fail)(f"④ FM6P 键盘 ↑ → ALG {a0} → {a1}")
    s.cmd("KEY 0x4B down")   # ↓：回原值
    s.cmd("KEY 0x4B up")
    (s.ok if fm6p_alg() == a0 else s.fail)(f"④ FM6P 键盘 ↓ → ALG 回 {a0}")
    s.cmd("STATE SNAP")
    s.expect_int(r"fm6p_engine_ok=(\d+)", 1, "④ STATE SNAP fm6p_engine_ok=1")
    # select_track(1) 走过 TRACKS（跨组）已销毁 SYNTH 的 SAMPLE/DSYN：同组重建兄弟页，
    # 让 SYNTH Σ 覆盖三页（同组切换只改显隐、不重建本页）
    s.cmd("UI sample")
    s.expect(r"UI page=sample \(8\)", "④ 回 SAMPLE（同组重建兄弟页）")
    s.cmd("UI dsyn")
    s.expect(r"UI page=dsyn \(12\)", "④ 回 DSYN（SYNTH 三页全建）")
    mem = param_mem(s)
    st, est, delta = mem["pages"].get(13, ("?", 0, 0))
    (s.ok if st == "built" and 0 < delta <= est else s.fail)(
        f"④ UI MEM FM6P state={st} delta={delta} est={est}（0<delta≤est）")
    gs = mem["groups"]["SYNTH"]
    (s.ok if gs["built"] == 3 and gs["sum_est"] == 20480 else s.fail)(
        f"④ UI MEM SYNTH built={gs['built']} sum_est={gs['sum_est']}（期望 3 / 20,480）")
    (s.ok if 0 < gs["sum_delta"] <= gs["sum_est"] else s.fail)(
        f"④ UI MEM SYNTH sum_delta={gs['sum_delta']}（0 < Σdelta ≤ Σest）")
    (s.ok if 0 < gs["build_ms"] <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"④ UI MEM SYNTH build_ms={gs['build_ms']}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")

    # ---------- ⑤ 池：SYNTH 全建 → STEP 首建；跨组往返 ----------
    s.ser.note("⑤ 池：SYNTH 三页全建 → STEP 首建 ≥4KB；SYNTH/PLAY/SONG/FILE 往返 ≤128B")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑤ SYNTH → STEP（跨组销毁 SYNTH）")
    free_step = nav_pool_free(s)
    (s.ok if free_step >= 4096 else s.fail)(
        f"⑤ SYNTH 全建 → STEP 首建后 pool_free={free_step}B（期望 ≥4,096B）")
    mem = param_mem(s)
    (s.ok if mem["groups"]["SYNTH"]["built"] == 0 else s.fail)(
        f"⑤ 离开 SYNTH 后 built={mem['groups']['SYNTH']['built']}（期望 0）")

    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑤ 池往返起点 PLAY")
    # 预热**两轮**：跨组首次重建有一次文本/字形缓存（DSYN 的 KIT 名/状态行、SAMPLE 的
    # 槽行文本），一轮后仍有 ~776B 的一次性落账（实测第一轮 −776B、之后 ±60B）。
    # SYNTH 的代表页**每轮轮换**（DSYN→SAMPLE→FM6P）：只固定一个代表会让另两页的
    # 重建泄漏永远不被测到。
    synth_reps = ("dsyn", "sample", "fm6p")
    for _ in range(2):
        for group in ("transport", "song", "projects") + synth_reps:
            s.cmd(f"UI {group}")
        s.cmd("UI play")
    free0 = nav_pool_free(s)
    per_trip = []
    for trip in range(1, NAV_LEAK_TRIPS + 1):
        rep = synth_reps[(trip - 1) % len(synth_reps)]
        for group in ("transport", "song", "projects", rep):
            s.cmd(f"UI {group}")
        s.cmd("UI play")
        per_trip.append(nav_pool_free(s))
    drop = free0 - per_trip[-1]
    s.ser.note(f"⑤ pool_free：起点 {free0} → 逐次 {per_trip}（{NAV_LEAK_TRIPS} 次 "
               "transport/song/projects + SYNTH 代表页 dsyn/sample/fm6p 轮换往返）")
    (s.ok if abs(drop) <= NAV_LEAK_NOISE else s.fail)(
        f"⑤ {NAV_LEAK_TRIPS} 次 SYNTH/FILE/SONG/SEQ 往返后 pool_free {free0} → {per_trip[-1]}"
        f"（Δ={-drop:+d}B，允许 |Δ| ≤ {NAV_LEAK_NOISE}B）")
    steps = [free0] + per_trip
    worst = max(steps[i] - steps[i + 1] for i in range(len(steps) - 1))
    (s.ok if worst <= NAV_LEAK_NOISE else s.fail)(
        f"⑤ 单次往返最大池下降 {worst}B ≤ {NAV_LEAK_NOISE}B（无逐次增长）")

    # ---------- ⑥ 收口 ----------
    s.ser.note("⑥ 收口：擦 USR1、DSYN 工厂 808 + STORE、银行槽擦除、轨 0/ANALOG、AUTO ON")
    ensure_stopped(s)
    s.cmd(f"SMP ERASE {SYNTH_SAMPLE_SLOT}")
    s.expect(rf"^SMP ERASE slot={SYNTH_SAMPLE_SLOT} OK\r?$", "⑥ 擦 USR1")
    s.cmd(f"DSYN KIT {SYNTH_DSYN_KIT} 0")
    s.expect(rf"DSYN KIT k={SYNTH_DSYN_KIT} src=0 status=\"ok\"", "⑥ DSYN kit 0 回工厂 808")
    s.cmd("DSYN STORE")
    s.expect(r'DSYN STORE dirty=0 status="ok"', "⑥ DSYN STORE（dirty=0）")
    for slot in range(3):
        s.cmd(f"STORE BANK ERASE {slot}")
        s.expect(rf"^BANK {slot} ERASED\r?$", f"⑥ 擦银行槽 {slot}")
    # 轨 1 复位：③/④ 把它留在 FM6（引擎 9）与编辑后的 live patch；只复位轨 0 会让
    # AUTO ON 后的自动存档把「轨 1 = FM6」写进工作工程（评审发现的泄漏）。
    # `ENG TRACK` 换引擎会按预置整份写 p[]（含 ALG 宏 P_E0 / PTCH P_E7），故一次即可。
    select_track(s, SYNTH_FM6_TRACK)
    s.cmd(f"ENG TRACK {SYNTH_FM6_TRACK} 0 0")
    s.expect(r"ENG TRACK k=1 engine=0 preset=0 OK", "⑥ 轨 1 → ANALOG（复位引擎/预置）")
    s.cmd("PARAM 63 0")
    s.expect(r"PARAM track=1 id=63 value=0 applied=0", "⑥ 轨 1 PARAM 63 = 0（ALG 宏/PAT）")
    select_track(s, 0)
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⑥ 轨 0 → ANALOG")
    s.cmd(f"PARAM {SYNTH_MUTE_ID} {mute0}")
    s.expect(rf"PARAM track=0 id={SYNTH_MUTE_ID} value={mute0} applied={mute0}",
             f"⑥ P_MUTE 还原 {mute0}")
    s.cmd("PARAM 63 0")
    s.expect(r"PARAM track=0 id=63 value=0 applied=0", "⑥ PARAM 63 = 0")
    s.cmd("UI play")
    s.expect(r"UI page=play \(0\)", "⑥ 回 PLAY")
    s.cmd("STORE AUTO ON")
    s.expect(r"^STORE AUTO on", "⑥ STORE AUTO ON（恢复默认）")
    # 收口的机读证据：AUTO ON 之后读一次引擎真值——轨 0 与轨 1 都必须回 ANALOG
    # （③/④ 把轨 1 留在 FM6；只信 ENG TRACK 的回显不够，这里读 ENG SNAP）。
    s.cmd("ENG SNAP")
    for k in (0, 1):
        s.expect(rf"^ENG SNAP trk k={k} engine=0 ", f"⑥ ENG SNAP 轨 {k} engine=0（无 FM6 泄漏）")


# ---- sys 场景（P4-E 设置页）：存储/状态/SYSTEM 页/持久化/golden/池全矩阵 -----------
SETTINGS_INFO_RE = (
    r"^STORE SETTINGS INFO kind=(\w+) len=(\d+) side=([01]) "
    r"palette=(\d+) lowcut=(\d+) zoom=(\d+) lights=(\d+) theme_on=(\d+) reso=(\d+) "
    r"rec_tempo=(\d+) rec_count=(\d+) rec_quant=(\d+) dirty=([01]) "
    r"theme=(\S+) theme_valid=([01])\r?$"
)


def settings_info(s: Session) -> dict:
    """`STORE SETTINGS INFO` → dict：flash 对象的 kind/len/side + 九个生效字段 + dirty +
    theme 名/有效位。INFO 读的是 flash 的事实与 RAM 生效副本，不写任何东西。"""
    s.cmd("STORE SETTINGS INFO")
    m = store_expect(s, SETTINGS_INFO_RE, "STORE SETTINGS INFO")
    keys = ("kind", "len", "side", "palette", "lowcut", "zoom", "lights", "theme_on", "reso",
            "rec_tempo", "rec_count", "rec_quant", "dirty", "theme", "theme_valid")
    info = {k: m.group(i + 1) for i, k in enumerate(keys)}
    for k in keys:
        if k not in ("kind", "theme"):
            info[k] = int(info[k])
    return info


def settings_set(s: Session, field: str, value: int, dirty: int = 1, label: str = "") -> None:
    """`STORE SETTINGS SET <field> <v>`：回显写后值（钳位后）与 dirty。"""
    s.cmd(f"STORE SETTINGS SET {field} {value}")
    s.expect(rf"^STORE SETTINGS SET {field}={value} dirty={dirty}\r?$",
             label or f"STORE SETTINGS SET {field} {value}")


def settings_save(s: Session, label: str) -> int:
    """`STORE SETTINGS SAVE`：回显提交的 payload 长度与 side 有效性，返回 len。"""
    s.cmd("STORE SETTINGS SAVE")
    m = store_expect(s, r"^STORE SETTINGS SAVED len=(\d+) side=([01])\r?$",
                     label or "STORE SETTINGS SAVE")
    return int(m.group(1))


def settings_expect(s: Session, want: dict, label: str) -> None:
    """逐字段比对 `STORE SETTINGS INFO`（want 里没有的键不断言）。"""
    info = settings_info(s)
    for key, value in want.items():
        (s.ok if info[key] == value else s.fail)(
            f"{label} {key}={info[key]}（期望 {value}）")


MONO_PALETTE = 4


def palette_read(s: Session, timeout: float = 5.0) -> tuple:
    """生效的 (palette, theme_on)（`STORE SETTINGS INFO`，只读、不打印断言）。存储未就绪
    （console 先于存储 init 起来，复位/上电刚见提示符）在 timeout 内重试；仍读不到抛
    InjectError。"""
    deadline = time.monotonic() + timeout
    while True:
        s.cmd("STORE SETTINGS INFO")
        m = re.search(SETTINGS_INFO_RE, s.last, re.MULTILINE)
        if m:
            return int(m.group(4)), int(m.group(8))    # SETTINGS_INFO_RE：palette / theme_on
        if not re.search(r"^STORE ERR no_storage\r?$", s.last, re.MULTILINE) or \
                time.monotonic() >= deadline:
            raise InjectError(f"STORE SETTINGS INFO: 回显缺少 palette/theme_on"
                              f"（响应 {s.last.strip()!r}）")
        time.sleep(0.25)


def palette_persist(s: Session, what: str) -> None:
    """把生效副本立即落盘（`STORE SETTINGS SAVE`，需要停止态；播放中先停走带）。SAVE 只改
    settings 对象与 NVS，不动 autosave/工程对象。被拒（停流时挂着的走带请求、损坏对象按
    设计拒写）只记 transcript 注记——RAM 副本照常生效，1s 任务的静默窗口会再写。"""
    s.cmd("STATE SNAP")
    if s.snap_int(r"playing=(\d)") != 0:
        ensure_stopped(s)
    s.cmd("STORE SETTINGS SAVE")
    if re.search(r"^STORE SETTINGS SAVED len=\d+ side=[01]\r?$", s.last, re.MULTILINE):
        s.ok(f"R23 {what}：settings 已落盘")
    else:
        s.ser.note(f"R23 {what}：STORE SETTINGS SAVE 被拒（{s.last.strip()!r}）——调色板只在 "
                   f"RAM 生效，1s 任务的静默窗口落盘；此前的硬复位会回到 flash 里的调色板")


def palette_normalise(s: Session) -> tuple:
    """像素探针场景的**入口归一**（R23，与 R20 同类）：场景的颜色常量（`C_*` 与页签/轨色）
    全部是 MONO 主题的 rgb565 实测值；设备停在别的调色板/主题时（上一轮 `sys` 中断、用户
    改过），像素探针会成片假红（实测 `palette=2 CYAN` 上 `step_edit` 306/14）。归一为 MONO
    （palette 4）+ 主题开关关，返回入口的 (palette, theme_on)，由 `palette_restore` 闭括号。

    - 入口值**跨 ScenarioRestart 只读一次**（session_once）；重跑时读设备当前值——复位 ⇒
      flash 里的调色板，未落盘时可能又是入口调色板，所以每一轮都重新核对并归一。
    - 已是 MONO 且主题关 ⇒ 不发任何写命令（默认入口零副作用，断言计数不变）。
    - 归一之后立即落盘：SET 只改 RAM 副本，落盘本由 1s 任务的静默窗口完成（idle ≥1.5s +
      gap ≥5s）；场景开头/中途的硬复位（`store` ⓪、`sys` ① …）赶在它之前，调色板就随复位
      回到入口值。入口值记在 `s.snapshots["palette_entry"]`：归一途中异常时 main 的
      finally 仍拿得到它。"""
    first = "palette_entry" not in s.snapshots
    entry = session_once(s, "palette_entry", lambda: palette_read(s), "R23 调色板入口")
    cur = entry if first else palette_read(s)
    if cur == (MONO_PALETTE, 0):
        return entry
    s.ser.note(f"R23 调色板归一：入口 palette={entry[0]} theme_on={entry[1]} → MONO（像素探针前提）")
    if cur[0] != MONO_PALETTE:
        settings_set(s, "palette", MONO_PALETTE, label=f"R23 归一 palette=4（入口 {cur[0]}）")
    if cur[1] != 0:
        settings_set(s, "theme_on", 0, label=f"R23 归一 theme_on=0（入口 {cur[1]}）")
    palette_persist(s, "归一")
    return entry


def palette_restore(s: Session, entry) -> None:
    """`palette_normalise` 的**闭括号**（main 的 finally：正常结束、断言未过、InjectError
    异常终止都走到）：写回入口调色板/主题开关并落盘——不落盘则紧接着的下一条场景开头的硬
    复位会把还原丢掉。入口本就是 MONO、或归一从没发生过（entry 为 None）⇒ 不发任何命令。
    还原失败只记 FAIL + 注记，不抛：不盖掉场景自己的结果/异常。"""
    if entry is None or entry == (MONO_PALETTE, 0):
        return
    try:
        cur = palette_read(s)
        if cur[0] != entry[0]:
            settings_set(s, "palette", entry[0], label=f"R23 还原入口 palette={entry[0]}")
        if cur[1] != entry[1]:
            settings_set(s, "theme_on", entry[1], label=f"R23 还原入口 theme_on={entry[1]}")
        deadline = time.monotonic() + 2.0            # console 直调的 SET 由下一拍 sync 采纳
        while True:
            s.cmd("STATE SNAP")
            got = (s.snap_int(r"set_palette=(\d+)"), s.snap_int(r"set_theme=(\d+)"))
            if got == tuple(entry) or time.monotonic() >= deadline:
                break
            time.sleep(0.05)
        (s.ok if got == tuple(entry) else s.fail)(
            f"R23 调色板回到入口：palette={got[0]} theme_on={got[1]}（期望 {entry[0]}/{entry[1]}）")
        palette_persist(s, "还原")
    except (InjectError, OSError) as e:
        s.ser.note(f"R23 调色板还原未完成：{e}")
        s.fail(f"R23 调色板还原未完成：{e}")


def sys_palette_default(s: Session, label: str, info_palette: int) -> None:
    """R13：settings 读不到（空/损坏/未就绪）⇒ 生效 palette = 4（MONO）。INFO 的 palette 是
    生效副本（reload 之后），`STATE SNAP set_palette` 是 UI 消费的同一投影，两者都查。"""
    (s.ok if info_palette == 4 else s.fail)(f"{label} INFO palette={info_palette}（期望 4 = MONO，R13）")
    s.cmd("STATE SNAP")
    s.expect_int(r"set_palette=(\d+)", 4, f"{label} STATE SNAP set_palette=4")


# 上游 arr_defaults（arranger.c:23）填 count 之外条目的方式：scene = i % 4、bars = 4。
SYS_DEFAULT_BARS = 4
# 上游 panel.c PANEL_DEFAULT = song_persist.h 的 SONG_PANEL_DEF（"PAN5" + 14 按钮 + 7 编码器 + 7 方向）
SYS_PANEL_DEF = bytes([
    0x35, 0x4E, 0x41, 0x50,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1,
    0, 1, 6, 2, 3, 4, 5,
    1, 1, 1, 1, 1, 1, 1,
])


def sys_per3_88(palette: int, song: dict) -> bytes:
    """88B 的合法 PER3（上游 persist_t：magic "PER3" / palette@4 / lowcut@8 / zoom@12 /
    panel@16 / arrangement@48 / lights@84；没有 DSU 与 MIDI 尾段，kind=per3、len=88）。
    48..83 是 arr_config_t（count / loop / reserved[2] / 16×(scene, bars)）：`song` 给出
    count/loop 与 count 内的条目，count 之外按上游 arr_defaults 填。下一次保存会由固件补成
    canonical 1,552B（当前 RAM 的 DSU）。"""
    arr = bytearray(36)
    arr[0], arr[1] = song["count"], song["loop"]
    for i in range(16):
        scene, bars = song["entry"].get(i, (i % 4, SYS_DEFAULT_BARS))
        arr[4 + 2 * i], arr[5 + 2 * i] = scene, bars
    lights = struct.pack("<I", 0)  # 84：上游默认 lights word（本固件拥有的位全 0）
    return struct.pack("<IIII", 0x50455233, palette, 0, 0) + SYS_PANEL_DEF + bytes(arr) + lights


# 与 page_system.c 的布局宏同源；坐标换成屏幕绝对坐标（页区 y + 72）。
SYS_ROW_Y = (32, 76, 106, 174, 254, 298, 342, 372, 510)
SYS_CHIP_ROWS = {1, 6}


def sys_tap(s: Session, row: int, delta: int = 1) -> None:
    if row in SYS_CHIP_ROWS:
        x, y = 304 + 70, SYS_ROW_Y[row] + 15
    elif row == 8:
        x, y = 24 + 70, SYS_ROW_Y[row] + 22
    else:
        x = 24 + (21 if delta < 0 else 420 - 21)
        y = SYS_ROW_Y[row] + 22
    s.cmd(f"TAP {x} {y + 72}", timeout=10.0 if row == 8 else None)


def sys_key(s: Session, code: int) -> None:
    s.cmd(f"KEY 0x{code:02X} down")
    s.cmd(f"KEY 0x{code:02X} up")


def sys_wait_nvs(s: Session, want: int, label: str, timeout: float = 16.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s.cmd("STATE SNAP")
        if s.snap_int(r"set_nvs=(\d+)") == want:
            s.ok(f"{label} set_nvs={want}")
            return
        time.sleep(0.2)
    s.fail(f"{label} 超时（set_nvs 未到 {want}）")


def sys_wait_settings_dirty(s: Session, want: int, label: str, timeout: float = 16.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = settings_info(s)
        if info["dirty"] == want:
            s.ok(f"{label} dirty={want}")
            return
        time.sleep(0.2)
    s.fail(f"{label} 超时（dirty 未到 {want}）")


# soak 的流式占空比上限（2× 阶段 2 静默渲染基线 9.44%，AGENTS §5.5）；自检后的坏态实测 ≈220%。
SYS_DUTY_MAX_PCT = 18.88


def sys_perf_healthy(s: Session, label: str, window: float = 3.0) -> None:
    """DI-40 回归：自检之后音频任务不得进入持续超预算态。流式 `ENG PERF` 的 duty_mean ≤ soak
    上限、`window` 秒内 xrun 增量为 0（坏态：音频任务栈落进 LP SRAM ⇒ 每块 ≈13ms、
    duty ≈220%、xrun == render_blocks，直到复位）。坏态下 console 还会被饿死（最长 >400s
    无提示符）——那由 Session 的命令超时以 InjectError 判场景失败。"""
    s.cmd("ENG PERF")
    s.expect_int(r"streaming=(\d+) xrun=(\d+)", 1, f"{label} streaming=1")
    xrun0 = s.snap_int(r"streaming=1 xrun=(\d+)")
    m = re.search(r"render_mean_us=(\d+) render_max_us=(\d+) duty_mean=([\d.]+)%", s.last, re.MULTILINE)
    if m is None:
        raise InjectError(f"{label}: ENG PERF 回显缺少 duty_mean：{s.last!r}")
    duty = float(m.group(3))
    (s.ok if duty <= SYS_DUTY_MAX_PCT else s.fail)(
        f"{label} duty_mean={duty:.2f}% render_mean_us={m.group(1)}（≤ {SYS_DUTY_MAX_PCT}%）")
    time.sleep(window)
    s.cmd("ENG PERF")
    xrun1 = s.snap_int(r"streaming=1 xrun=(\d+)")
    (s.ok if xrun1 == xrun0 else s.fail)(
        f"{label} {window:.0f}s 窗口 xrun {xrun0} → {xrun1}（期望增量 0）")


def sys_wait_selftest(s: Session, timeout: float = 180.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s.cmd("STATE SNAP")
        if s.snap_int(r"selftest_done=(\d+)") == 1:
            ran = s.snap_int(r"selftest_ran=(\d+)")
            failed = s.snap_int(r"selftest_failed=(\d+)")
            (s.ok if ran == 182 and failed == 0 else s.fail)(
                f"⑧ SYSTEM 状态行数据 SELFTEST {ran - failed}/{ran} "
                f"{'PASS' if ran > 0 and failed == 0 else 'FAIL'}")
            return
        time.sleep(0.5)
    s.fail("⑧ SELFTEST chip 超时（180s 内无结果；既有全量实测约 157–165s）")


def sys_device_selftest(s: Session) -> None:
    """释放串口后运行真实 182 例设备 golden 门禁，再恢复原 Session。"""
    s.ser.note("⑤ device-selftest.py（本 Session 暂时关闭串口；完整 182 例 golden 比对）")
    s.ser.close()
    try:
        cp = subprocess.run([sys.executable, str(SCRIPTS_DIR / "device-selftest.py"), s.port],
                            cwd=SCRIPTS_DIR.parent, capture_output=True, text=True, timeout=240)
        print(cp.stdout, end="")
        if cp.stderr:
            print(cp.stderr, end="", file=sys.stderr)
    finally:
        s.ser = s._open()
    # console 全量自检之后 device-selftest.py 已 `ENG AUDIO ON`：这是 DI-40 的原始路径
    # （chip worker 走同一段起流），先断言音频没进入持续超预算态再复位取干净基线（DI-26 的堆泄漏已修，复位不再是必需）。
    sys_perf_healthy(s, "⑤ console 全量自检后音频")
    # 同一 boot 内读（复位之前）：自检的 restore 把 s_set/fx_lowcut/flt_reso 回滚成 0，收尾的
    # settings_reload 以 flash 重放——INFO 读的是生效副本，这里才是 D7「收尾 reload」那一半的断言
    # （复位后的读数只是开机 reload，证明不了它）。
    settings_expect(s, {"lowcut": 1, "reso": 6, "dirty": 0}, "⑤ 同一 boot 自检后 settings（收尾 reload）")
    s.reboot()
    (s.ok if cp.returncode == 0 else s.fail)(f"⑤ device-selftest.py exit={cp.returncode}（期望 0）")


def run_sys(s: Session) -> None:
    """sys 场景（P4-E）：设置页的存储层 → 状态层 → SYSTEM 页的端到端矩阵。

    ① settings 生效副本：`STORE SETTINGS INFO` 解析 → `SET palette 2` ⇒ `dirty=1` →
       `SAVE` ⇒ `STORE SETTINGS SAVED len=<n> side=<0|1>`、`dirty=0` → esptool 硬复位 →
       INFO 仍 `palette=2`（开机 reload 以 flash 为准）
    ①b `STORE RAW END settings 0` 往返：上传 88B PER3（palette=1、歌曲 count=2 loop=1）⇒
       INFO/SONG INFO 报上传的字节；RAM 的 `STORE SONG` 也是上传的歌（R15：歌曲/DSU/生效
       副本按新对象重载）且随后一次 `STORE SONG SAVE` 不把它写回旧值；**工程槽 A 的 RAM
       缓存逐位不变**（`STORE PROJ HASH 0` 前后相同——settings 与 project 的 slot 都是 0）
    ② SYSTEM/SYS 注册、10,240B 预算、单页组导航、第 9 组签像素与 ABOUT 版本行（P4-F Task 7：
       两行暗色文字的像素探针；① 另断言启动日志 `App version`，transcript 含启动日志回显）；
    ③ 全部设置行 + THEME 第 6 档 + 键盘 + 播放中 20 次平台写（xrun 不增）；
    ④ settings/NVS/REC 三项的延迟写与硬复位保持；⑤ lowcut=1/reso=6 下 182 例 golden；
    ⑥ SEQ↔SYS 有序对过渡峰值；⑦ 归一 SAVE + AUTO ON；⑧ SELFTEST chip 的一次性 worker、
    STOP/RESET 守卫与工作工程恢复（先 `SYS SELFTEST ARM`，worker 全路径必跑；DI-40：⑤/⑧ 自检后
    `ENG PERF` duty ≤ soak 上限、xrun 增量 0；DI-42：①/①b-a 空/损坏对象 ⇒ palette 4）；
    ⑨ RAW settings + SIDE theme 往返、硬复位与 STORE TEST。
    ①b 收口先恢复槽 A 的空/非空状态，再恢复歌曲，避免 `arr_drop_scene` 截断待恢复歌曲。

    **破坏性**（整份替换 OBJ_SETTINGS：上传的 88B 没有 SYN kit 银行 ⇒ 之后落工厂 SYN kit；
    palette/歌曲顺序由收口恢复），自带归一、可重复；`STORE AUTO OFF` 防止 autosave 把
    中途状态写进工作工程（不跨复位，复位后重新下发）。
    """
    s.ser.note("sys 场景开始：① 归一（AUTO OFF / 停走带 / ENG AUDIO ON）")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "① STORE AUTO OFF")
    s.cmd("ENG AUDIO ON")
    s.expect(r"ENG AUDIO streaming=1", "① ENG AUDIO streaming=1")
    ensure_stopped(s)
    base = settings_info(s)
    # R13 的默认 palette 只在 reload 那一刻成立（开机/RAW 之后）：起点 RAM 里的 palette 可能
    # 是用户此后改过的，故空/损坏对象的断言放在 ①b-a（RAW 写入触发的 reload）而不是这里。
    if base["kind"] == "invalid":
        # 起点是损坏对象（上一轮在 ①b-a 之后异常终止）：SAVE 按设计被拒（`STORE ERR invalid`、
        # 不覆盖），先用 RAW 写回一份合法的默认 PER3（palette=MONO、默认歌曲）再继续。
        store_raw.upload_raw(s.cmd, sys_per3_88(4, {"count": 4, "loop": 0, "entry": {}}), "settings", 0)
        base = settings_info(s)
        (s.ok if base["kind"] == "per3" and base["palette"] == 4 else s.fail)(
            f"① 损坏对象归一 kind={base['kind']} palette={base['palette']}（期望 per3 / 4）")
    if base["dirty"] != 0:
        settings_save(s, "① 起点 dirty=1：先 SAVE 归一（允许上次中断后续跑）")
        base = settings_info(s)
    (s.ok if base["kind"] in ("per3", "empty") and base["dirty"] == 0 else s.fail)(
        f"① 起点 kind={base['kind']} dirty={base['dirty']}（期望 per3|empty / 0）")

    s.ser.note("① settings 生效副本：SET → dirty → SAVE → 硬复位 → INFO")
    settings_set(s, "palette", 2, label="① SET palette 2")
    settings_expect(s, {"palette": 2, "dirty": 1}, "① SET 后 INFO")
    before = settings_info(s)
    len_saved = settings_save(s, "① SAVE")
    settings_expect(s, {"palette": 2, "dirty": 0, "len": len_saved}, "① SAVE 后 INFO")
    (s.ok if len_saved >= 1552 else s.fail)(
        f"① SAVE 的 len={len_saved}（canonical PER3 ≥ 1,552）")
    (s.ok if before["len"] in (0, len_saved) or len_saved >= before["len"] else s.fail)(
        f"① SAVE 不缩短对象：{before['len']} → {len_saved}")

    s.reboot()
    # ABOUT 行（② 的像素探针）对照的版本号：IDF app_init 的 `App version:`（本脚本的 reboot 把
    # 启动日志写进 transcript）。与主机构建树的 HEAD 只作注记——构建与运行之间若有提交，
    # 固件版本本就落后于 HEAD，是否 HEAD 构建由烧录门禁判（AGENTS §5.5），不在这里判。
    boot_ver = s.boot_version()
    (s.ok if re.fullmatch(r"[0-9a-f]{7,40}(-dirty)?", boot_ver) else s.fail)(
        f"① 启动日志 App version={boot_ver!r}（期望 git 短哈希，可带 -dirty）")
    host_ver = subprocess.run(["git", "rev-parse", "--short=7", "HEAD"], cwd=SCRIPTS_DIR.parent,
                              capture_output=True, text=True).stdout.strip()
    s.ser.note(f"固件 App version={boot_ver} / 主机 HEAD={host_ver}"
               f"{'（一致）' if boot_ver.split('-')[0] == host_ver else '（不一致：见上注记）'}")
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "① 复位后 STORE AUTO OFF")
    settings_expect(s, {"palette": 2, "dirty": 0, "kind": "per3", "len": len_saved},
                    "① 硬复位后 INFO")

    # ---------- ①b STORE RAW END settings 0：原样落盘 + RAM 重载 + 不碰工程槽 A ----------
    s.ser.note("①b RAW settings 往返：88B PER3 → 落盘 + 重载；工程槽 A 的 RAM 缓存不受影响")
    ensure_stopped(s)
    song0 = song_cfg(s)
    side0 = settings_info(s)["side"]
    s.cmd("STORE PROJ HASH 0")
    created_slot0 = False
    if re.search(r"^STORE ERR empty\r?$", s.last, re.MULTILINE):
        created_slot0 = True
        store_cmd(s, "STORE PROJ SAVE 0", r"^PROJ 0 SAVED hash=[0-9A-F]{8}\r?$",
                  "①b 槽 A 为空：先 SAVE 0 造一个非空槽（HASH 往返的判别前提）")
    else:
        s.expect(r"^PROJ 0 HASH=[0-9A-F]{8}\r?$", "①b RAW 前 STORE PROJ HASH 0（槽 A 已在用）")
    hash0 = store_hash(s, "STORE PROJ HASH 0", r"^PROJ 0 HASH=([0-9A-F]{8})\r?$", "①b RAW 前 HASH 0")
    # 上传的歌：固定值；起点恰好就是它（上一轮在上传之后异常终止）时换一首，保证
    # 「RAM 歌曲被重载」的断言有判别力。
    up_song = {"count": 2, "loop": 1, "entry": {0: (1, 2), 1: (0, 3)}}
    if (song0["count"], song0["loop"], song0["entry"]) == (up_song["count"], up_song["loop"], up_song["entry"]):
        up_song = {"count": 3, "loop": 0, "entry": {0: (2, 1), 1: (3, 2), 2: (0, 1)}}
    up_want = (up_song["count"], up_song["loop"])
    s.ser.note(f"①b 上传歌曲 count={up_want[0]} loop={up_want[1]}（起点 count={song0['count']} loop={song0['loop']}）")
    # R13：没有对象 / 对象损坏 / 存储未就绪 ⇒ 同一个 `!from_flash` 分支，生效 palette 必须是 4
    # （MONO）。开发机上永远有对象，这里用 64B 垃圾对象走同一分支（INFO 报 invalid）；随后的
    # 88B PER3 上传把它覆盖回合法对象。
    store_raw.upload_raw(s.cmd, bytes([0xA5]) * 64, "settings", 0)
    info = settings_info(s)
    (s.ok if info["kind"] == "invalid" and info["len"] == 64 else s.fail)(
        f"①b-a 损坏对象 kind={info['kind']} len={info['len']}（期望 invalid / 64）")
    sys_palette_default(s, "①b-a 损坏对象", info["palette"])
    store_raw.upload_raw(s.cmd, sys_per3_88(1, up_song), "settings", 0)
    s.ok("①b STORE RAW END settings 0 saved len=88（88B PER3，palette=1）")
    settings_expect(s, {"kind": "per3", "len": 88, "palette": 1, "dirty": 0, "side": side0},
                    "①b RAW 后 INFO（上传的字节 + 现有 side 随新副本）")
    s.cmd("STORE SONG INFO")
    s.expect(rf"^STORE SONG INFO kind=per3 len=88 side=\d count={up_want[0]} loop={up_want[1]}\r?$",
             f"①b RAW 后 STORE SONG INFO = 上传的歌曲 count={up_want[0]} loop={up_want[1]}")
    s.cmd("STORE SONG")
    s.expect(rf"^STORE SONG count={up_want[0]} loop={up_want[1]} dirty=0\r?$",
             "①b RAW 后 RAM 歌曲已重载（R15）：count/loop")
    for i, (sc, bars) in sorted(up_song["entry"].items()):
        s.expect(rf"^SONG {i} scene={sc} bars={bars}\r?$", f"①b RAW 后 RAM 歌曲条目 {i}（R15）")
    hash1 = store_hash(s, "STORE PROJ HASH 0", r"^PROJ 0 HASH=([0-9A-F]{8})\r?$", "①b RAW 后 HASH 0")
    (s.ok if hash1 == hash0 else s.fail)(
        f"①b 工程槽 A 的 RAM 缓存不受 RAW settings 影响：HASH 0 {hash1:08X}（期望 {hash0:08X}）")
    s.cmd("STORE SONG SAVE")
    m = store_expect(s, r"^STORE SONG SAVED len=(\d+) side=([01])\r?$", "①b RAW 之后 STORE SONG SAVE")
    (s.ok if int(m.group(1)) >= 1552 else s.fail)(
        f"①b 保存把 88B 补成 canonical：len={m.group(1)}（≥ 1,552）")
    s.cmd("STORE SONG INFO")
    s.expect(rf"^STORE SONG INFO kind=per3 len=\d+ side=\d count={up_want[0]} loop={up_want[1]}\r?$",
             "①b 随后的保存写的是已重载的歌（不是上传前的旧 RAM）")

    s.ser.note("收口：槽 A 先回起点状态，再恢复 palette/歌曲顺序 + SAVE；AUTO ON")
    if created_slot0:
        proj_erase(s, 0, "收口 先擦回起点为空的槽 A（避免 arr_drop_scene 截断待恢复歌曲）")
    song_restore = " ".join(f"{song0['entry'][i][0]}:{song0['entry'][i][1]}" for i in range(song0["count"]))
    store_cmd(s, f"STORE SONG SET {song0['count']} {song0['loop']} {song_restore}",
              rf"^STORE SONG SET count={song0['count']} loop={song0['loop']} ", "收口 恢复起点歌曲顺序")
    settings_set(s, "palette", base["palette"], label="收口 SET palette")
    len_restored = settings_save(s, "收口 SAVE（歌曲 + settings 同一次提交）")
    settings_expect(s, {"palette": base["palette"], "dirty": 0, "len": len_restored}, "收口后 INFO")
    (s.ok if len_restored >= 1552 else s.fail)(f"收口后对象 len={len_restored}（≥ 1,552）")
    s.cmd("STORE SONG")
    s.expect(rf"^STORE SONG count={song0['count']} loop={song0['loop']} dirty=0\r?$",
             "收口后 STORE SONG（dirty=0）")
    for i in range(song0["count"]):
        scene, bars = song0["entry"][i]
        s.expect(rf"^SONG {i} scene={scene} bars={bars}\r?$", f"收口后 SONG {i}")

    # ---------- ② SYSTEM 页 / SYS 单页组 / 9 组签（P4-F Task 4 起 SYS 是第 9 张） ----------
    s.ser.note("② SYSTEM 页：UI system / UI MEM / UI TAB sys / UI SUB 0 / 截屏")
    settings_set(s, "palette", 4, label="② 归一 MONO（组签像素锚点）")
    settings_save(s, "② 归一 SAVE")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "② UI system → SYSTEM(14)")
    mem = param_mem(s)
    grp = mem["groups"].get("SYS", {})
    (s.ok if grp.get("built") == 1 and grp.get("sum_est") == 10240 else s.fail)(
        f"② GROUP SYS built={grp.get('built')} sum_est={grp.get('sum_est')}（期望 1 / 10,240）")
    (s.ok if 0 < grp.get("build_ms", 0) <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"② GROUP SYS build_ms={grp.get('build_ms')}ms（0 < x ≤{NAV_BUILD_BUDGET_MS}）")
    state, est, delta = mem["pages"].get(14, ("?", 0, 0))
    (s.ok if state == "built" and est == 10240 and 0 < delta <= est else s.fail)(
        f"② SYSTEM state={state} est={est} delta={delta}（期望 built / 10,240 / 0<delta≤est）")
    s.cmd("UI TAB sys")
    s.expect(r"^UI TAB SYS page=system \(14\)\r?$", "② UI TAB sys")
    s.cmd("UI SUB 0")
    s.expect(r"^UI ERR sub 0\r?$", "② SYS 单页组：UI SUB 0 回 ERR（子行为空）")
    img = s.capture("system")
    s.probe_region(img, "② 第 9 个组签（SYS）高亮", (926, 4, 1048, 36), C_YELLOW, 16, 500)
    # ABOUT：SYSTEM 节下的版本行（page_system.c：label (24,454) 700×48、UI_TEXT_DIM 16pt，
    # 两行 "APP <esp_app_desc.version>" / "SLOOP 2.8.5 (bac9c05)"）。屏幕绝对 y = 72+454=526。
    # 像素只能证明「两行都渲染出暗色文字、文字不越过 label 盒」；与启动日志版本号同源
    # （esp_app_get_description），这里按行 1 的文字宽度与版本串长度的量级对照。
    ver_box = (24, 72 + 454, 24 + 700, 72 + 454 + 48)
    n1 = count_region(img, (ver_box[0], ver_box[1], ver_box[2], ver_box[1] + 22), C_TEXT_DIM, 16)
    n2 = count_region(img, (ver_box[0], ver_box[1] + 22, ver_box[2], ver_box[3]), C_TEXT_DIM, 16)
    (s.ok if n1 >= 120 else s.fail)(
        f"② 版本行第 1 行（APP <version>）暗色文字像素 {n1}（≥120）")
    (s.ok if n2 >= 120 else s.fail)(
        f"② 版本行第 2 行（SLOOP 2.8.5 (bac9c05)）暗色文字像素 {n2}（≥120）")
    spill = count_region(img, (ver_box[2], ver_box[1], 1100, ver_box[3]), C_TEXT_DIM, 16)
    (s.ok if spill == 0 else s.fail)(
        f"② 版本行文字不越出 label 盒（x≥724 的暗色像素 {spill}，期望 0）")
    px = img.load()
    xs = [x for y in range(ver_box[1], ver_box[1] + 22) for x in range(ver_box[0], ver_box[2])
          if max(abs(px[x, y][0] - C_TEXT_DIM[0]), abs(px[x, y][1] - C_TEXT_DIM[1]),
                 abs(px[x, y][2] - C_TEXT_DIM[2])) <= 16]
    w1 = (max(xs) - min(xs) + 1) if xs else 0
    # 16pt 等宽量级 ≈16px/字符："APP " + 7 位短哈希 ≈176px（实测 176）；带 -dirty ≈250
    (s.ok if 100 <= w1 <= 420 else s.fail)(
        f"② 版本行第 1 行文字宽度 {w1}px（与 App version={boot_ver!r} 的长度量级一致；100..420）")
    s.ser.note(f"ABOUT 版本行：App version={boot_ver!r}（启动日志）；第 1 行文字 x "
               f"{min(xs) if xs else '-'}..{max(xs) if xs else '-'}（{w1}px），第 2 行像素 {n2}")

    # ---------- ③ TAP / 键盘 / 走带中平台写 ----------
    s.ser.note("③ SYSTEM 行编辑：COLOR/ZOOM/BRIGHT/LIGHTS/VOLUME/OUTPUT/LOWCUT/RESO + 键盘")
    ensure_stopped(s)
    settings_set(s, "palette", 4, label="③ 归一 palette=MONO")
    settings_set(s, "theme_on", 1, label="③ 归一到 THEME 档（下一档才是 GREEN）")
    settings_set(s, "zoom", 0, label="③ 归一 ZOOM OFF")
    settings_set(s, "lights", 0, label="③ 归一 LIGHTS OFF")
    settings_set(s, "lowcut", 0, label="③ 归一 LOWCUT OFF")
    settings_set(s, "reso", 0, label="③ 归一 RESO OFF")
    s.cmd("DISP BRIGHT 100")
    s.expect(r"^DISP BRIGHT applied=100\r?$", "③ 归一 BRIGHT 100")
    s.cmd("AUD VOL 70")
    s.expect(r"^AUD VOL applied=70\r?$", "③ 归一 VOLUME 70")
    s.cmd("AUD OUT speaker")
    s.expect(r"^AUD OUT applied=speaker\r?$", "③ 归一 OUTPUT speaker")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "③ UI system")

    palette_rgb = ((120, 255, 148), (255, 164, 40), (140, 220, 255),
                   (255, 108, 92), C_YELLOW)
    palette_names = ("GREEN", "AMBER", "CYAN", "RED", "MONO")
    mono_accent = None
    for palette in range(5):
        sys_tap(s, 0, 1)
        s.expect(rf"-> SYS COLOR value={palette_names[palette]} status=\"OK\"",
                 f"③ COLOR + → {palette_names[palette]}")
        settings_expect(s, {"palette": palette, "theme_on": 0}, f"③ COLOR {palette_names[palette]} INFO")
        img = s.capture(f"color-{palette_names[palette].lower()}")
        s.probe(img, f"③ {palette_names[palette]} accent", *C_TAB_SYS, palette_rgb[palette])
        if palette == 4:
            mono_accent = img.getpixel(C_TAB_SYS)
        if palette == 2:
            s.cmd("UI tracks")
            s.expect(r"^UI page=tracks \(1\)\r?$", "③ CYAN 兄弟页 UI tracks")
            img = s.capture("tracks-cyan")
            s.probe(img, "③ TRACKS 兄弟页采用 CYAN accent", *C_TAB_TRACKS, palette_rgb[palette])
            s.cmd("UI system")
            s.expect(r"^UI page=system \(14\)\r?$", "③ 回 SYSTEM")

    theme = settings_info(s)
    if theme["theme_valid"]:
        sys_tap(s, 0, 1)
        s.expect(rf"-> SYS COLOR value={re.escape(theme['theme'])} status=\"OK\"",
                 "③ COLOR 第 6 档 = side theme")
        settings_expect(s, {"palette": 4, "theme_on": 1}, "③ THEME_ON=1 且 palette 保持 MONO")
        img = s.capture("color-theme")
        (s.ok if img.getpixel(C_TAB_SYS) != mono_accent else s.fail)(
            "③ THEME accent 与 MONO 不同")
        sys_tap(s, 0, 1)
        s.expect(r"-> SYS COLOR value=GREEN status=\"OK\"",
                 "③ THEME→GREEN：先 THEME_ON=0 再 PALETTE=0")
        settings_expect(s, {"palette": 0, "theme_on": 0}, "③ 回内置 palette")

    sys_tap(s, 2, -1)
    s.expect(r"-> SYS BRIGHT value=90 status=\"OK\"", "③ BRIGHT − → 90")
    sys_tap(s, 2, -1)
    s.expect(r"-> SYS BRIGHT value=80 status=\"OK\"", "③ BRIGHT − → 80")
    s.cmd("DISP BRIGHT")
    s.expect(r"^DISP BRIGHT applied=80\r?$", "③ DISP BRIGHT 读回 80")
    s.cmd("STATE SNAP")
    s.expect_int(r"sys_bright=(\d+)", 80, "③ STATE sys_bright")
    s.expect_int(r"set_nvs=(\d+)", 1, "③ BRIGHT 置 NVS dirty")

    sys_tap(s, 4, 1)
    s.expect(r"-> SYS VOLUME value=75 status=\"OK\"", "③ VOLUME + → 75")
    s.cmd("AUD VOL")
    s.expect(r"^AUD VOL applied=75\r?$", "③ AUD VOL 读回 75")

    sys_tap(s, 3, 1)
    s.expect(r"-> SYS LIGHTS value=1 status=\"OK\"", "③ LIGHTS + → LOW")
    time.sleep(0.1)
    s.cmd("KBD RGB")
    s.expect(r"^KBD RGB applied=low\r?$", "③ LIGHTS 经 30ms UI 拍驱动 KBD RGB low")

    sys_tap(s, 5, 1)
    s.expect(r"-> SYS OUTPUT value=0 status=\"N/A\"", "③ OUTPUT headphone 不支持：N/A")
    s.cmd("STATE SNAP")
    s.expect_int(r"sys_out=(\d+)", 0, "③ OUTPUT 失败后 sys.out 不变")

    sys_tap(s, 6)
    s.expect(r"-> SYS LOWCUT value=ON status=\"OK\"", "③ LOWCUT chip ON")
    settings_expect(s, {"lowcut": 1}, "③ LOWCUT INFO")
    for reso in (1, 2, 3):
        sys_tap(s, 7, 1)
        s.expect(rf"-> SYS RESO value={reso} status=\"OK\"", f"③ RESO + → {reso}")
    settings_expect(s, {"reso": 3}, "③ RESO INFO")

    sys_tap(s, 1)
    s.expect(r"-> SYS ZOOM value=ON status=\"OK\"", "③ ZOOM chip ON")
    sys_key(s, 0x4B)  # ↓：ZOOM → BRIGHT
    sys_key(s, 0x3B)  # ↑：BRIGHT → ZOOM
    sys_key(s, 0x3D)  # ENTER：ZOOM toggle
    settings_expect(s, {"zoom": 0}, "③ 键盘焦点环 + ENTER 将 ZOOM 切回 OFF")
    sys_tap(s, 7, 1)  # 焦点落到 RESO，3→4
    sys_key(s, 0x4A)  # ←：4→3
    settings_expect(s, {"reso": 3}, "③ 键盘 ← 调 RESO")
    sys_key(s, 0x3D)  # ENTER：3→4
    sys_key(s, 0x4A)  # ←：回 3
    settings_expect(s, {"reso": 3}, "③ 键盘 ENTER/← 往返")

    s.ser.note("③ 走带中平台写：TRANSPORT PLAY + 20 次亮度交替，xrun 增量必须为 0")
    s.cmd("UI transport")
    s.cmd("TAP %d %d" % TRANSPORT_PLAY)
    s.expect(r"-> TRANSPORT CMD_PLAY", "③ PLAY")
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d+)", 1, "③ 走带中 playing=1")
    s.cmd("ENG PERF")
    xrun0 = s.snap_int(r"xrun=(\d+)")
    for i in range(20):
        value = 90 if i % 2 == 0 else 80
        s.cmd(f"DISP BRIGHT {value}")
        s.expect(rf"^DISP BRIGHT applied={value}\r?$", f"③ 播放中 DISP BRIGHT {value}")
    s.cmd("ENG PERF")
    s.expect_int(r"xrun=(\d+)", xrun0, "③ 20 次播放中亮度写 xrun 增量 0")
    s.cmd("STATE SNAP")
    s.expect_int(r"set_nvs=(\d+)", 1, "③ 播放中 NVS dirty 持续置位")
    s.cmd("TAP %d %d" % TRANSPORT_STOP)
    s.expect(r"-> TRANSPORT CMD_STOP", "③ STOP")
    sys_wait_nvs(s, 0, "③ 停止后静默窗口提交 NVS")
    s.cmd("UI system")

    # ---------- ④ settings/NVS 延迟写与硬复位保持；REC 三项 ----------
    s.ser.note("④ 持久化：settings dirty 窗口、side 不变、硬复位、REC CONFIG")
    ensure_stopped(s)
    s.cmd("STORE SONG INFO")
    side0 = s.snap_int(r"side=(\d+)")
    settings_set(s, "palette", 3, label="④ SET palette=RED")
    settings_expect(s, {"palette": 3, "dirty": 1}, "④ 改值后即时 INFO")
    sys_wait_settings_dirty(s, 0, "④ 停止+空闲窗口落盘 settings")
    s.cmd("STORE SONG INFO")
    s.expect_int(r"side=(\d+)", side0, "④ settings 保存不改变 side")
    expected = settings_info(s)
    s.reboot()
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "④ 复位后 AUTO OFF")
    settings_expect(s, {k: expected[k] for k in
                        ("palette", "lowcut", "zoom", "lights", "theme_on", "reso", "dirty")},
                    "④ 硬复位保持 settings")
    s.cmd("DISP BRIGHT")
    s.expect(r"^DISP BRIGHT applied=80\r?$", "④ 硬复位保持 NVS bright=80")
    s.cmd("AUD VOL")
    s.expect(r"^AUD VOL applied=75\r?$", "④ 硬复位保持 NVS vol=75")

    s.cmd("SEQ REC CONFIG 1 1 1")
    s.expect(r"quant=off tempo=tempo count=count", "④ REC CONFIG 1 1 1 即时生效")
    settings_expect(s, {"rec_tempo": 1, "rec_count": 1, "rec_quant": 1, "dirty": 1},
                    "④ REC 三项置 settings dirty")
    sys_wait_settings_dirty(s, 0, "④ REC 三项过静默窗口落盘")
    s.reboot()
    store_cmd(s, "STORE AUTO OFF", r"^STORE AUTO off\r?$", "④ REC 复位后 AUTO OFF")
    s.cmd("SEQ REC")
    s.expect(r"quant=off tempo=tempo count=count", "④ 硬复位保持 REC quant/tempo/count")
    settings_expect(s, {"rec_tempo": 1, "rec_count": 1, "rec_quant": 1}, "④ REC settings INFO")

    # ---------- ⑤ lowcut=1 / reso=6 下完整 golden 自检 ----------
    s.ser.note("⑤ golden：lowcut=1 reso=6，自检运行期归零、结束后按 settings 重载")
    settings_set(s, "lowcut", 1, label="⑤ SET lowcut=1")
    settings_set(s, "reso", 6, label="⑤ SET reso=6")
    settings_save(s, "⑤ 自检前 SAVE")
    sys_device_selftest(s)
    settings_expect(s, {"lowcut": 1, "reso": 6}, "⑤ 自检后 settings 保持")
    # sys_device_selftest 已硬复位，后续池与 chip 门禁不继承自检后的设备状态（原 DI-26 的堆泄漏
    # 已修，复位只为干净基线）。

    # ---------- ⑥ SEQ↔SYS 有序对过渡峰值 ----------
    s.ser.note("⑥ 有序对：SEQ 两页全建→SYS、SYS→STEP，构建后池剩余 ≥4,096")
    s.cmd("UI step")
    s.expect(r"^UI page=step \(2\)\r?$", "⑥ UI step")
    s.cmd("UI transport")
    s.expect(r"^UI page=transport \(3\)\r?$", "⑥ UI transport（SEQ 两页全建）")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "⑥ SEQ→SYSTEM")
    m = re.search(r"页面构建 page=SYSTEM\(14\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B", s.last)
    if m is None:
        s.fail("⑥ SEQ→SYS 构建日志缺 SYSTEM 池剩余")
    else:
        (s.ok if int(m.group(3)) >= 4096 else s.fail)(
            f"⑥ SEQ→SYS 过渡池剩余={m.group(3)}B（≥4,096）")
    mem = param_mem(s)
    (s.ok if mem["groups"]["SEQ"]["sum_est"] == 32256 else s.fail)(
        f"⑥ GROUP SEQ sum_est={mem['groups']['SEQ']['sum_est']}（期望 32,256）")
    s.cmd("UI step")
    s.expect(r"^UI page=step \(2\)\r?$", "⑥ SYS→STEP")
    m = re.search(r"页面构建 page=STEP\(2\) 池增量=(\d+)B 预算=(\d+)B 池剩余=(\d+)B", s.last)
    if m is None:
        s.fail("⑥ SYS→STEP 构建日志缺 STEP 池剩余")
    else:
        (s.ok if int(m.group(3)) >= 4096 else s.fail)(
            f"⑥ SYS→STEP 过渡池剩余={m.group(3)}B（≥4,096）")

    # ---------- ⑦ 收口 ----------
    s.ser.note("⑦ 收口：palette 4 / zoom 0 / lights 0 / lowcut 0 / reso 0 / bright 100 / vol 70")
    ensure_stopped(s)
    for field, value in (("palette", 4), ("zoom", 0), ("lights", 0),
                         ("lowcut", 0), ("reso", 0), ("theme_on", 0)):
        settings_set(s, field, value, label=f"⑦ SET {field}={value}")
    s.cmd("DISP BRIGHT 100")
    s.expect(r"^DISP BRIGHT applied=100\r?$", "⑦ BRIGHT 100")
    s.cmd("AUD VOL 70")
    s.expect(r"^AUD VOL applied=70\r?$", "⑦ VOLUME 70")
    s.cmd("AUD OUT speaker")
    s.expect(r"^AUD OUT applied=speaker\r?$", "⑦ OUTPUT speaker")
    s.cmd("SEQ REC CONFIG 0 0 0")
    s.expect(r"quant=on tempo=free count=note", "⑦ REC 配置回默认")
    settings_save(s, "⑦ STORE SETTINGS SAVE（含 NVS flush）")
    settings_expect(s, {"palette": 4, "zoom": 0, "lights": 0, "lowcut": 0,
                        "reso": 0, "theme_on": 0, "dirty": 0}, "⑦ 收口 INFO")
    s.cmd("STATE SNAP")
    s.expect_int(r"set_nvs=(\d+)", 0, "⑦ NVS dirty 已清")
    s.cmd("STORE AUTO ON")
    s.expect(r"^STORE AUTO on", "⑦ STORE AUTO ON（恢复默认）")

    # ---------- ⑧ SELFTEST chip：每 boot 一次，独立 worker 停流/恢复 ----------
    # ⑤ 已跑过一次全量 console 自检；先复位取干净基线，再测产品 chip。
    s.ser.note("⑧ SELFTEST chip：SYS SELFTEST ARM → 播放守卫 → 未落盘编辑+设置 → 完整 worker（先落盘）"
               " → 工作工程/设置/流/AUTO 恢复 → PERF")
    s.reboot()
    ensure_stopped(s)
    # R8 的「每 boot 一次」标记在 .noinit，跨 esptool 硬复位存活；R25 的诊断注入清掉它与结果
    # 缓存，chip 才走完整 worker 路径（否则下面只会得到 RESET FIRST + 上一轮的缓存结果）。
    s.cmd("SYS SELFTEST ARM")
    s.expect(r"^SYS SELFTEST ARMED\r?$", "⑧ SYS SELFTEST ARM（R25：清每 boot 一次标记与结果缓存）")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "⑧ UI system")
    auto = autosave_fields(s)
    if auto["on"] == 0:
        store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⑧ 准备 autosave：AUTO ON")
        auto = autosave_fields(s)
    autosave_bump_bpm(s, 120)
    autosave_wait_writes(s, auto["writes"] + 1, 25.0,
                         "⑧ 先落一份当前工作工程（worker 的 resume 来源）")

    seq_play(s, "⑧ 走带中守卫前提")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "⑧ 回 SYSTEM")
    sys_tap(s, 8)
    s.expect(r"-> SYS SELFTEST status=\"STOP FIRST\"", "⑧ 播放中 chip 预热（建立状态文本路径）")
    s.cmd("ENG MEM")
    mem_before = s.snap_int(r"internal_free=(\d+)")
    mem_min_before = s.snap_int(r"internal_min=(\d+)")
    sys_tap(s, 8)
    s.expect(r"-> SYS SELFTEST status=\"STOP FIRST\"", "⑧ 播放中 chip → STOP FIRST")
    s.cmd("ENG MEM")
    mem_after = s.snap_int(r"internal_free=(\d+)")
    mem_min_after = s.snap_int(r"internal_min=(\d+)")
    # 启动自检会建一个 5,632B 栈的 worker：free 与 min 都掉 >5KB 并持续到自检结束——free 的
    # ≤64B 门限已经能判。`internal_min` 是自启动以来的低水位，只在**一次瞬时分配把 free 压到
    # 旧低水位以下**时才移动：那需要 ≈ free − min（本机 ≈3.8KB）的瞬时分配，越过后新低水位
    # 只比旧的低「超出量」。所以 min 的下移量本身很小（首跑实测下移 44B，超出量的来源未定位）
    # 与是否启动 worker 无关；对 min 用与 free 同一 64B 噪声门限是安全的，不要求逐字节不变。
    (s.ok if abs(mem_after - mem_before) <= 64 and mem_min_after >= mem_min_before - 64
     else s.fail)(
        f"⑧ STOP FIRST 未启动自检（free {mem_before}→{mem_after}，噪声 ≤64B；"
        f"min {mem_min_before}→{mem_min_after}，下移 ≤64B）")
    s.cmd("ENG AUDIO")
    s.expect_int(r"streaming=(\d+)", 1, "⑧ STOP FIRST 后音频仍 ON")
    s.cmd("STATE SNAP")
    s.expect_int(r"selftest_req=(\d+)", 0, "⑧ STOP FIRST 未发 CMD_SELFTEST_RUN")
    ensure_stopped(s)
    # 终审 I1：chip 之前刻意**不等**落盘——改 BPM（工作工程，autosave 的 20s gap 未到）与 RESO
    # （settings 的 1.5s/5s 延迟写窗口未到），随后立刻点 chip。没有 worker 的自检前落盘，
    # 自检的 restore 会把这两处都丢掉（BPM 回到上一次 autosave、RESO 回到 0）。
    autosave_bump_bpm(s, 124)
    settings_set(s, "reso", 7, label="⑧ 自检前未落盘 RESO=7")
    st0 = autosave_fields(s)
    (s.ok if st0["writes"] == auto["writes"] + 1 else s.fail)(
        f"⑧ chip 前 autosave_writes={st0['writes']}（期望仍 {auto['writes'] + 1}：编辑尚未自动存档）")
    settings_expect(s, {"reso": 7, "dirty": 1}, "⑧ chip 前设置仍 dirty（延迟写窗口未到）")
    hash_before = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                             "⑧ 自检前 LIVEHASH（含未存档的 BPM）")
    (s.ok if hash_before != st0["hash"] else s.fail)(
        f"⑧ 判别力：LIVEHASH {hash_before:08X} != autosave_hash {st0['hash']:08X}")
    s.cmd("UI system")
    sys_tap(s, 8)
    chip_ran = "RESET FIRST" not in s.last
    if chip_ran:
        s.expect(r"-> SYS SELFTEST status=\"SELFTEST RUNNING\"", "⑧ 停止后 chip 只入队")
        s.cmd("SYS SELFTEST ARM")
        s.expect(r"^SYS SELFTEST ARM ERR busy\r?$", "⑧ 自检进行中 ARM 被拒（busy）")
        sys_wait_selftest(s)
        s.cmd("STORE STATS")
        s.expect(r"^STORE STATS .*autosave=1 ", "⑧ worker 结束后 STORE AUTO 回 on")
        hash_after = store_hash(s, "STORE LIVEHASH", r"^LIVEHASH " + STORE_HASH8,
                                "⑧ 自检后 LIVEHASH")
        (s.ok if hash_after == hash_before else s.fail)(
            f"⑧ 工作工程恢复：LIVEHASH {hash_after:08X} == 自检前 {hash_before:08X}")
        st1 = autosave_fields(s)
        (s.ok if st1["hash"] == hash_after else s.fail)(
            f"⑧ 未存档的编辑已由 worker 落盘：autosave_hash {st1['hash']:08X} == LIVEHASH（I1）")
        (s.ok if store_bpm_settled(s, 124, 3.0) else s.fail)(
            "⑧ 自检前未存档的 BPM 编辑保持（STATE SNAP bpm=124）")
        s.cmd("ENG AUDIO")
        s.expect_int(r"streaming=(\d+)", 1, "⑧ worker 恢复流式")
        sys_perf_healthy(s, "⑧ worker 恢复流式后音频")
        # settings：worker 自检前落盘 + 收尾 reload ⇒ chip 之前**没保存**的 RESO 原值回来、dirty 清。
        settings_expect(s, {"reso": 7, "dirty": 0}, "⑧ 自检后 settings（chip 前未保存的 RESO 保持）")
        autosave_bump_bpm(s, 120)  # ⑨ 的工作工程回到 120（下一次 autosave 窗口自会落盘）
    else:
        s.fail("⑧ ARM 之后 chip 仍回 RESET FIRST：worker 全路径未执行")
    # 离开/重返不得再次清 R8 标记；PASS 或此前的 PASS 缓存都在重建后显示。
    s.cmd("UI play")
    s.expect(r"^UI page=play \(0\)\r?$", "⑧ 自检后离开 SYSTEM")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "⑧ 自检后重返 SYSTEM")
    img = s.capture("selftest-pass")
    (s.ok if img.size == (1280, 720) and img.mode == "RGB" else s.fail)(
        f"⑧ SELFTEST PASS PNG 解码 size={img.size} mode={img.mode}（期望 1280×720 RGB）")
    s.probe_region(img, "⑧ SELFTEST PASS 状态行（accent）", (24, 672, 700, 712),
                   C_YELLOW, 16, 40)
    sys_tap(s, 8)
    s.expect(r"-> SYS SELFTEST status=\"RESET FIRST\"", "⑧ 第二次 chip → RESET FIRST")

    # ---------- ⑨ RAW settings + SIDE theme：写后/复位/STORE TEST ----------
    s.ser.note("⑨ RAW settings + SIDE：palette=3、THM2 theme、COLOR THEME、复位与 STORE TEST")
    ensure_stopped(s)
    s.cmd("STATE SNAP")
    s.expect_int(r"playing=(\d+)", 0, "⑨ RAW settings 上传前走带已停")
    if chip_ran:
        s.expect_int(r"selftest_done=(\d+)", 1, "⑨ RAW settings 上传前 SELFTEST 已收尾")
    theme_name = "P4E7"
    per3 = settings_fixtures.build_per3()
    side = settings_fixtures.build_side(theme_name)
    settings_fixtures.validate(per3, side, theme_name)
    store_raw.upload_raw(s.cmd, per3, "settings", 0)
    s.ok("⑨ STORE RAW END settings 0 saved len=88")
    store_cmd(s, "STORE RAW SIDE 00", r"^STORE ERR invalid\r?$", "⑨ SIDE 短记录 → invalid")
    store_cmd(s, "STORE RAW SIDE " + "G" * 160, r"^STORE ERR invalid\r?$",
              "⑨ SIDE 非 hex → invalid")
    store_raw.upload_side(s.cmd, side)
    s.ok("⑨ STORE RAW SIDE saved len=80")
    settings_expect(s, {"kind": "per3", "len": 88, "palette": 3, "theme": theme_name,
                        "theme_valid": 1, "theme_on": 0, "side": 1}, "⑨ RAW settings+SIDE 后 INFO")
    s.cmd("UI system")
    s.expect(r"^UI page=system \(14\)\r?$", "⑨ UI system（COLOR 有第 6 档）")
    sys_tap(s, 0, 1)
    s.expect(r"-> SYS COLOR value=MONO status=\"OK\"", "⑨ COLOR RED→MONO")
    sys_tap(s, 0, 1)
    s.expect(rf"-> SYS COLOR value={theme_name} status=\"OK\"", "⑨ COLOR MONO→THEME")
    settings_expect(s, {"palette": 4, "theme": theme_name, "theme_valid": 1, "theme_on": 1},
                    "⑨ 选中 THEME 后 INFO")
    img = s.capture("raw-theme")
    s.probe(img, "⑨ THEME accent == side.hi RGB565(0xF81F)", *C_TAB_SYS, (255, 0, 255))
    s.reboot()
    ensure_stopped(s)
    settings_expect(s, {"palette": 4, "theme": theme_name, "theme_valid": 1, "theme_on": 1,
                        "side": 1}, "⑨ 硬复位后 theme/palette 保持")
    s.cmd("UI system")
    img = s.capture("raw-theme-reset")
    s.probe(img, "⑨ 复位后 THEME accent 仍 == side.hi", *C_TAB_SYS, (255, 0, 255))
    store_cmd(s, "STORE TEST", r"^STORE TEST PASS 6/6\r?$", "⑨ STORE TEST PASS 6/6")
    settings_set(s, "theme_on", 0, label="⑨ 收口 THEME_ON=0")
    settings_set(s, "palette", 4, label="⑨ 收口 palette=MONO")
    settings_save(s, "⑨ 收口 SAVE（保留有效 side，回内置 MONO）")
    store_cmd(s, "STORE AUTO ON", r"^STORE AUTO on\r?$", "⑨ 收口 AUTO ON")


# ---------------------------------------------------------------- SOUND（5B-1 卡片仪表盘）---
# Sound 仪表盘（page_sound.c / sound_layout.h / page_sound_picker.c）的几何与成员表镜像：TAP 坐标、
# 像素探针与「期望存在性 / 锁定态」都由这里派生（与 step_cell_xy / param_knob_xy 同纪律：页面宏改了
# 先改这里，不写死数字）。槽级旋钮注入 `UI SOUND KNOB <flat> <press|drag> <dx> <dy>`：dx/dy 相对槽圆心，
# 环带 30..52px（SOUND_RING_MIN_R/MAX_R，比页级 `UI KNOB` 的 55..95 小一圈）；锚点角度 =
# lv_atan2(dy, dx)（LVGL 的整数反正切：轴点与 45° 对角点都是整度数），drag = 锚点值 + 角度差×值域/270。
SOUND_TAB = 72                                   # UI_TAB_BAR_H：页面区 → 屏幕坐标
SOUND_COL_W, SOUND_COL_GAP, SOUND_CARD_PAD, SOUND_TITLE_H = 616, 16, 12, 36
SOUND_CELL_W, SOUND_CELL_H = 140, 132
SOUND_RING_TOP, SOUND_KNOB_D = 12, 96            # 环外缘到槽矩形顶 / 旋钮直径
SOUND_RING_R = SOUND_KNOB_D // 2 - 4             # 环中心线半径 44
SOUND_RING_MIN_R, SOUND_RING_MAX_R = 30, 52      # 环带（SOUND_RING_MIN_R/MAX_R）
SOUND_CARD_GAP = 8
SOUND_VIEW_X, SOUND_VIEW_W, SOUND_VIEW_BOTTOM = 16, 1248, 644
SOUND_VIEW_Y, SOUND_VIEW_Y_BANK = 80, 128        # 视口顶（银行行隐藏 / 显示）
SOUND_SEL_X, SOUND_SEL_Y, SOUND_SEL_CELL_W = 960, 4, 76      # 选轨条（页面区 (960,4) 304×32，格宽 76）
SOUND_BTN_MINUS, SOUND_BTN_PLUS = (1096 + 40, SOUND_TAB + 40 + 20), (1184 + 40, SOUND_TAB + 40 + 20)
SOUND_PICK_X, SOUND_PICK_Y, SOUND_PICK_W, SOUND_PICK_H = 160, 120, 960, 440      # 选择器面板（页面区）
SOUND_PICK_LIST_X, SOUND_PICK_LIST_W, SOUND_PICK_ROW_H = 176, 928, 32
SOUND_PICK_LIST_Y_SYNTH, SOUND_PICK_LIST_Y_DRUM = 56, 24     # 列表顶（页面区相对；合成轨 = chip 行 8+40+8）
SOUND_PICK_CHIP_Y = SOUND_TAB + 120 + 8 + 20                 # chip 行中心（屏幕 y = 220）
SOUND_PICK_CHIP_X0, SOUND_PICK_CHIP_GAP = 176, 8
SOUND_PICK_CHIP_PAD, SOUND_PICK_CHIP_CHAR_W = 24, 11         # chip 宽 = 11px/字符 + 24
SOUND_NAME_ROW0, SOUND_NAME_ROW1 = (160, 92), (512, 92)      # 页眉 ENGINE/KIT 行、PRESET 行的名字区
# 13 卡成员数（sound_layout.c 的 SOUND_CARD_LIST）与鼓轨可编辑的轨道参数（TD 宏：(卡, 卡内槽)）
SOUND_CARD_TITLE = ("Global", "Master", "Envelope", "LFO", "Engine edit", "Voice", "FX", "Scale", "Arp",
                    "Pattern", "Drums", "MIDI", "Texture")
SOUND_CARD_SLOTS = (4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7)
SOUND_CARD_BASE = tuple(sum(SOUND_CARD_SLOTS[:c]) for c in range(len(SOUND_CARD_SLOTS)))
SOUND_FLAT_MAX = sum(SOUND_CARD_SLOTS)           # 成员表总数 = 109（扁平下标 0..108）
# 成员表（sound_layout.c 的 SOUND_CARD_LIST × param_ids.h 的 id）：扁平下标 → (作用域, 参数 id)。
# GS = 全局、TS/TD/TA = 轨道；描述符查询与 scope/id 回显核对都用它（ absent 槽的回显是 scope=- id=-）。
SOUND_SCOPE_TRACK, SOUND_SCOPE_GLOBAL = 0, 1
_G, _T = SOUND_SCOPE_GLOBAL, SOUND_SCOPE_TRACK


def _members():
    table, flat = [], 0
    for scope_ids in (
            ((_G, 0), (_G, 1), (_G, 2), (_G, 3)),                                   # Global
            ((_G, 27), (_G, 28), (_G, 29), (_G, 30), (_T, 50)),                      # Master
            tuple((_T, pid) for pid in (1, 2, 3, 4, 5, 6, 7, 8, 59)),               # Envelope
            tuple((_T, pid) for pid in (9, 10, 11, 12, 60, 61, 62, 13, 14, 15, 16)),  # LFO
            tuple((_T, 63 + k) for k in range(8)) + ((_T, 58),),                    # Engine edit（+ P_FTYPE）
            tuple((_T, pid) for pid in (0, 37, 38, 41, 42, 43, 44, 39, 40)),         # Voice
            tuple((_T, pid) for pid in (33, 34, 35, 36, 45, 46, 47, 48)) + ((_G, 49),)
            + tuple((_G, pid) for pid in (4, 5, 6, 7, 8, 9, 10, 11))
            + tuple((_T, pid) for pid in (53, 54, 55, 56, 57)),                      # FX
            tuple((_T, pid) for pid in (25, 26, 27, 49, 28, 51, 52)),               # Scale
            tuple((_T, pid) for pid in (17, 18, 19, 20, 21, 22, 23, 24)),            # Arp
            tuple((_T, pid) for pid in (29, 30, 31, 32)),                            # Pattern
            tuple((_G, pid) for pid in (24, 25, 26, 32)),                            # Drums
            tuple((_G, pid) for pid in (33, 34, 35, 24, 36, 44, 45, 46, 47, 48)),    # MIDI
            tuple((_G, pid) for pid in (37, 38, 39, 40, 41, 42, 43))):               # Texture
        table += list(scope_ids)
    assert list(SOUND_CARD_SLOTS) == [4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7]
    return {i: m for i, m in enumerate(table)}


SOUND_MEMBERS = _members()
assert len(SOUND_MEMBERS) == SOUND_FLAT_MAX == 109
del _G, _T, _members
SOUND_DRUM_EDITABLE = frozenset({(1, 4), (5, 0), (5, 7), (6, 4), (6, 5), (6, 6), (6, 7), (6, 17), (6, 18),
                                 (6, 19), (6, 20), (6, 21), (9, 0), (9, 1), (9, 2), (9, 3)})
assert len(SOUND_DRUM_EDITABLE) == 16
SOUND_FTYPE = (4, 8)                             # P_FTYPE：仅 ANALOG 合成轨出现（TA 宏）
SOUND_FLAT_FTYPE = SOUND_CARD_BASE[4] + 8        # = 37
SOUND_CARD_ENGINE, SOUND_CARD_PATTERN, SOUND_CARD_FX, SOUND_CARD_ENV = 4, 9, 6, 2
SOUND_SLOT_ATK = SOUND_CARD_BASE[SOUND_CARD_ENV]         # Envelope 卡首槽 = P_ATK（STATE SNAP 的 atk 字段）
SOUND_SLOT_SLEN = SOUND_CARD_BASE[SOUND_CARD_PATTERN]    # Pattern 卡首槽 = P_SLEN
SOUND_SLOT_ITYPE = SOUND_CARD_BASE[SOUND_CARD_FX] + 17   # FX 卡第 17 槽 = P_ITYPE（其后 4 槽随它改名）
SOUND_EST = 8704                                 # SOUND 页预算（= PARAM_EDIT_SUM_EST，不得抬）
SOUND_KEY_LEFT, SOUND_KEY_RIGHT, SOUND_KEY_UP, SOUND_KEY_DOWN = 0x4A, 0x4C, 0x3B, 0x4B
SOUND_KEY_ENTER, SOUND_KEY_ESC = 0x3D, 0x00
SOUND_PARAM_LOCKED = r"locked \(page or slot N/A\)"
# `STATE SNAP` 能直接对照的槽（参数投影行 / 全局行）；其余槽的真值走 `PARAM <id>` / `PARAM GLOBAL <id>`
SOUND_STATE_TRACK = {0: "level", 1: "atk", 2: "dec", 3: "sus", 4: "rel", 29: "slen", 40: "mute"}
SOUND_STATE_GLOBAL = {0: "bpm", 1: "swing"}
# 拖动选项（按下点, 拖动点, 角度差）：按下点 = 锚点方向（270° / 0°）；对角点 (±28,∓28) 的半径 39.6
# 仍在环带内。+90° 优先；值域太小（R<3 时 +90° 不足 1）或已在端点时依次换 −90° / 180° / −135° / ±45°
SOUND_DRAGS = (((0, -40), (40, 0), 90), ((0, -40), (-40, 0), -90), ((0, -40), (0, 40), 180),
               ((40, 0), (-28, -28), -135), ((0, -40), (28, -28), 45), ((0, -40), (-28, -28), -45))
RE_SOUND_SLOT = re.compile(r"UI SOUND SLOT card=(\d+) slot=(\d+) flat=(\d+) present=(\d) scope=(-|\d+) "
                           r"id=(-|\d+) label=\"([^\"]*)\" locked=(\d) value=(-?\d+) "
                           r"x=(-?\d+) y=(-?\d+) w=(-?\d+) h=(-?\d+)")
RE_ENG_DESC = re.compile(r"ENG DESC scope=(\w+) (?:track|engine)=(\d+) id=(\d+) label=\"([^\"]*)\" fmt=(\d+) "
                         r"min=(-?\d+) max=(-?\d+) def=(-?\d+) unit=\"[^\"]*\" enums=(\d+)")
RE_SOUND_FOCUS = re.compile(r'UI SOUND FOCUS flat=(\d+) card=(\d+) slot=(\d+) label="([^"]*)"')
RE_SOUND_STATS = re.compile(r"UI SOUND STATS cards=(\d+) slots=(\d+) scroll_max=(-?\d+) collapsed=0x([0-9A-F]{4})")
RE_SOUND_SCROLL = re.compile(r"UI SOUND SCROLL y=(-?\d+) max=(-?\d+)")
RE_SOUND_KNOB = re.compile(r"UI SOUND KNOB flat=(\d+) (press|drag) dx=(-?\d+) dy=(-?\d+) "
                           r"(?:value=(-?\d+)|ignored \((ring band|locked)\))")
RE_SOUND_PICK = re.compile(r"UI SOUND PICK open=1 row=(\d) engine=(\d+) highlight=(\d+) rows=(\d+)")
RE_SOUND_PICK_ROW = re.compile(r'UI SOUND PICK n=(\d+) text="([^"]*)" current=(\d) x=(-?\d+) y=(-?\d+) '
                               r"w=(-?\d+) h=(-?\d+)")
RE_SOUND_BUILD = re.compile(r"页面构建 page=SOUND\(6\) 池增量=(-?\d+)B 预算=(\d+)B 池剩余=(-?\d+)B 耗时=(\d+)ms")


def sound_place(collapsed: int):
    """sound_layout_place 的镜像：每卡 (列, 内容 y, 高) 与内容总高（内容坐标）。"""
    pos, bottom = [], [0, 0]
    for c, n in enumerate(SOUND_CARD_SLOTS):
        h = (SOUND_TITLE_H if collapsed >> c & 1
             else SOUND_TITLE_H + ((n + 3) // 4) * SOUND_CELL_H + SOUND_CARD_PAD)
        col = 1 if bottom[1] < bottom[0] else 0
        pos.append((col, bottom[col], h))
        bottom[col] += h + SOUND_CARD_GAP
    return pos, max(bottom) - SOUND_CARD_GAP


def sound_scroll_max(collapsed: int, bank: bool = False) -> int:
    _, height = sound_place(collapsed)
    return max(0, height - (SOUND_VIEW_BOTTOM - (SOUND_VIEW_Y_BANK if bank else SOUND_VIEW_Y)))


def sound_slot_rect(collapsed: int, scroll: int, card: int, slot: int) -> tuple:
    """槽矩形（屏幕绝对坐标，银行行隐藏）：与 `UI SOUND CARD n SLOT m` 的 x/y/w/h 同源比对。"""
    col, y, _ = sound_place(collapsed)[0][card]
    return (SOUND_VIEW_X + col * (SOUND_COL_W + SOUND_COL_GAP) + SOUND_CARD_PAD + slot % 4 * SOUND_CELL_W,
            SOUND_TAB + SOUND_VIEW_Y + y + SOUND_TITLE_H + (slot // 4) * SOUND_CELL_H - scroll,
            SOUND_CELL_W, SOUND_CELL_H)


def sound_card_rect(collapsed: int, scroll: int, card: int) -> tuple:
    """卡片矩形（屏幕绝对坐标，银行行隐藏）。"""
    col, y, h = sound_place(collapsed)[0][card]
    return (SOUND_VIEW_X + col * (SOUND_COL_W + SOUND_COL_GAP), SOUND_TAB + SOUND_VIEW_Y + y - scroll,
            SOUND_COL_W, h)


def sound_card_probe(collapsed: int, scroll: int, card: int) -> tuple:
    """卡片左内边距里的一点（x = 卡左 + 5，槽从 +12 起；折叠卡 = 标题行内）：卡片底 UI_PANEL。"""
    x, y, _, h = sound_card_rect(collapsed, scroll, card)
    return (x + 5, y + min(h - 12, 100))


def sound_gap_probe(collapsed: int, scroll: int, card: int) -> tuple:
    """卡片上方 8px 间隙的中点（卡片之间 / 折叠卡与下一张卡之间）：页面背景 UI_BG。"""
    x, y, _, _ = sound_card_rect(collapsed, scroll, card)
    return (x + 100, y - SOUND_CARD_GAP // 2)


def sound_slot_xy(rect: tuple) -> tuple:
    """槽圆心（屏幕绝对坐标）：x = 矩形中心，y = 矩形顶 + 12 + 48。"""
    return (rect[0] + rect[2] // 2, rect[1] + SOUND_RING_TOP + SOUND_KNOB_D // 2)


def sound_sel_xy(k: int) -> tuple:
    """选轨条第 k 格中心（避开居中的轨号字形：取格左缘 + 10）。"""
    return (SOUND_SEL_X + k * SOUND_SEL_CELL_W + 2 + 10, SOUND_TAB + SOUND_SEL_Y + 16)


def sound_sel_tap_xy(k: int) -> tuple:
    return (SOUND_SEL_X + k * SOUND_SEL_CELL_W + SOUND_SEL_CELL_W // 2, SOUND_TAB + SOUND_SEL_Y + 16)


def sound_chip_probe(names: tuple, e: int) -> tuple:
    """chip 内左上的一点（chip 左缘 + 24、顶 + 4：避开 10px 圆角与居中的字形）：chip 底色。"""
    return (sound_chip_xy(names, e)[0] - (SOUND_PICK_CHIP_CHAR_W * len(names[e]) + SOUND_PICK_CHIP_PAD) // 2
            + 24, SOUND_PICK_CHIP_Y - 16)


def sound_chip_xy(names: tuple, e: int) -> tuple:
    """选择器 chip 行第 e 个 chip 的中心（屏幕坐标；只用于前几个 chip——整行放得下、不滚动）。"""
    x = SOUND_PICK_CHIP_X0
    for name in names[:e]:
        x += SOUND_PICK_CHIP_CHAR_W * len(name) + SOUND_PICK_CHIP_PAD + SOUND_PICK_CHIP_GAP
    return (x + (SOUND_PICK_CHIP_CHAR_W * len(names[e]) + SOUND_PICK_CHIP_PAD) // 2, SOUND_PICK_CHIP_Y)


def knob_drag_expect(lo: int, hi: int, anchor: int, delta_deg: int) -> int:
    """knob_math.c 的 knob_drag_value：anchor + 角度差×值域/270（半值远离零取整、钳位到 [lo, hi]）。"""
    num = delta_deg * (hi - lo)
    dv = (num + 135) // 270 if num >= 0 else -((-num + 135) // 270)
    return max(lo, min(hi, anchor + dv))


def sound_slot(s: Session, card: int, slot: int) -> dict:
    s.cmd(f"UI SOUND CARD {card} SLOT {slot}")
    m = RE_SOUND_SLOT.search(s.last)
    if m is None:
        raise InjectError(f"UI SOUND CARD {card} SLOT {slot} 回显异常：{s.last.strip()[-160:]!r}")
    g = m.groups()
    return {"card": int(g[0]), "slot": int(g[1]), "flat": int(g[2]), "present": g[3] == "1",
            "scope": -1 if g[4] == "-" else int(g[4]), "id": -1 if g[5] == "-" else int(g[5]),
            "label": g[6], "locked": g[7] == "1", "value": int(g[8]),
            "rect": tuple(int(v) for v in g[9:13])}


def sound_flat(s: Session, flat: int) -> dict:
    """按扁平下标取槽（卡片 / 卡内槽由成员表换算）。"""
    card = max(c for c in range(len(SOUND_CARD_SLOTS)) if SOUND_CARD_BASE[c] <= flat)
    return sound_slot(s, card, flat - SOUND_CARD_BASE[card])


def sound_desc(s: Session, scope: int, pid: int, track: int = 0):
    """槽的描述符（与页面同一来源）：轨道参数走 `ENG DESC live <track> <id>`（随 ITYPE/ESYNC/引擎变），
    全局走 `ENG DESC global 0 <id>`；该上下文没有这个参数回 None。"""
    s.cmd(f"ENG DESC live {track} {pid}" if scope == SOUND_SCOPE_TRACK else f"ENG DESC global 0 {pid}")
    m = RE_ENG_DESC.search(s.last)
    if m is not None:
        return {"label": m.group(4), "min": int(m.group(6)), "max": int(m.group(7)), "enums": int(m.group(9))}
    if re.search(r"^ENG DESC ERR ", s.last, re.MULTILINE):
        return None
    raise InjectError(f"ENG DESC（scope={scope} id={pid}）回显异常：{s.last.strip()[-160:]!r}")


def sound_expect_present(card: int, slot: int, d, analog_synth: bool) -> bool:
    """页面解析存在性的规则（page_sound.c resolve_slots）：描述符取到、名称非空、值域不退化
    （min≠max 或有枚举），且引擎门槛满足（只有 P_FTYPE 要 ANALOG 合成轨）。"""
    return (d is not None and d["label"] != "" and (d["min"] != d["max"] or d["enums"] > 0)
            and (analog_synth or (card, slot) != SOUND_FTYPE))


def sound_truth(s: Session, scope: int, pid: int) -> tuple:
    """槽的引擎真值 (value, min, max)：`PARAM <id>`（选中轨）/ `PARAM GLOBAL <id>`（只读形式）。"""
    s.cmd(f"PARAM {pid}" if scope == SOUND_SCOPE_TRACK else f"PARAM GLOBAL {pid}")
    m = re.search(r"value=(-?\d+) min=(-?\d+) max=(-?\d+)", s.last)
    if m is None:
        raise InjectError(f"PARAM（scope={scope} id={pid}）回显异常：{s.last.strip()[-160:]!r}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def sound_truth_expect(s: Session, sl: dict, want: int, label: str) -> None:
    """真值核对：PARAM（引擎读回）；`STATE SNAP` 有对应字段的槽再核一次（投影真值）。"""
    got, _, _ = sound_truth(s, sl["scope"], sl["id"])
    (s.ok if got == want else s.fail)(f"{label}: PARAM 真值 {got}（期望 {want}）")
    line, field = (("param", SOUND_STATE_TRACK.get(sl["id"])) if sl["scope"] == SOUND_SCOPE_TRACK
                   else ("global", SOUND_STATE_GLOBAL.get(sl["id"])))
    if field is not None:
        s.cmd("STATE SNAP")
        m = re.search(rf"STATE SNAP {line} [^\r\n]*\b{field}=(-?\d+)", s.last)
        got_state = int(m.group(1)) if m else None
        (s.ok if got_state == want else s.fail)(f"{label}: STATE SNAP {field}={got_state}（期望 {want}）")


def sound_focus(s: Session, flat: int = None) -> dict:
    s.cmd("UI SOUND FOCUS" if flat is None else f"UI SOUND FOCUS {flat}")
    m = RE_SOUND_FOCUS.search(s.last)
    if m is None:
        raise InjectError(f"UI SOUND FOCUS 回显异常：{s.last.strip()[-160:]!r}")
    return {"flat": int(m.group(1)), "card": int(m.group(2)), "slot": int(m.group(3)), "label": m.group(4)}


def sound_stats(s: Session) -> dict:
    s.cmd("UI SOUND STATS")
    m = RE_SOUND_STATS.search(s.last)
    if m is None:
        raise InjectError(f"UI SOUND STATS 回显异常：{s.last.strip()[-160:]!r}")
    return {"cards": int(m.group(1)), "slots": int(m.group(2)), "scroll_max": int(m.group(3)),
            "collapsed": int(m.group(4), 16)}


def sound_scroll(s: Session, y: int = None) -> tuple:
    s.cmd("UI SOUND SCROLL" if y is None else f"UI SOUND SCROLL {y}")
    m = RE_SOUND_SCROLL.search(s.last)
    if m is None:
        raise InjectError(f"UI SOUND SCROLL 回显异常：{s.last.strip()[-160:]!r}")
    return int(m.group(1)), int(m.group(2))


def sound_knob(s: Session, flat: int, op: str, dx: int, dy: int):
    """`UI SOUND KNOB`：返回 ("value", v) / ("ring band", None) / ("locked", None)。"""
    s.cmd(f"UI SOUND KNOB {flat} {op} {dx} {dy}")
    m = RE_SOUND_KNOB.search(s.last)
    if m is None or int(m.group(1)) != flat or m.group(2) != op:
        raise InjectError(f"UI SOUND KNOB {flat} {op} 回显异常：{s.last.strip()[-160:]!r}")
    return ("value", int(m.group(5))) if m.group(5) is not None else (m.group(6), None)


def sound_knob_ignored(s: Session, flat: int, op: str, dx: int, dy: int, why: str, label: str) -> None:
    kind, _ = sound_knob(s, flat, op, dx, dy)
    (s.ok if kind == why else s.fail)(
        f"{label}: UI SOUND KNOB {op} ({dx},{dy}) → {kind}（期望 {why}）")


def sound_keys(s: Session, codes, label: str) -> None:
    """一次按键序列（每个码 down + up 配对，一次批写）；断言 KEY 回显按序 2n 行。"""
    want = [(f"{c:02X}", ev) for c in codes for ev in ("down", "up")]
    s.cmd_batch([f"KEY 0x{c} {ev}" for c, ev in want])
    got = re.findall(r"KEY 0x([0-9A-F]{2}) (down|up) \S+ elapsed_us=\d+", s.last)
    (s.ok if got == want else s.fail)(f"{label}: KEY 回显 {len(got)}/{len(want)} 行、顺序"
                                      f"{'一致' if got == want else '不一致'}")


def sound_select(s: Session, track: int) -> None:
    """Ctrl+数字键选轨（SEL 层，与 TRACKS 卡 / 选轨条同一命令）并断言选中轨。"""
    key_with_layer(s, (KEY_SEL,), (track + 1,), 0x04, f"Ctrl+{track + 1} 选轨 {track}")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", track, f"STATE SNAP selected_track={track}")


def sound_eng(s: Session, track: int) -> tuple:
    """`ENG SNAP` 里轨 track 的 (engine, preset)。"""
    s.cmd("ENG SNAP")
    m = re.search(rf"ENG SNAP trk k={track} engine=(\d+) preset=(\d+) ", s.last)
    if m is None:
        raise InjectError(f"ENG SNAP 缺少轨 {track} 行：{s.last.strip()[-160:]!r}")
    return int(m.group(1)), int(m.group(2))


def sound_read_vector(s: Session) -> dict:
    """选中轨的 71 个轨道参数真值（`PARAM <id>` 只读）；该轨上无描述符的 id 不进字典。"""
    out = {}
    for pid in range(ENG_PARAM_COUNT):
        s.cmd(f"PARAM {pid}")
        m = re.search(rf"PARAM track=\d+ id={pid} value=(-?\d+)", s.last)
        if m:
            out[pid] = int(m.group(1))
        elif not re.search(r"reason=invalid_id", s.last):
            raise InjectError(f"PARAM {pid} 回显异常：{s.last.strip()[-160:]!r}")
    return out


def sound_write_vector(s: Session, vec: dict, label: str) -> None:
    ids = sorted(vec)
    bad = []
    for i in range(0, len(ids), 14):            # PARAM BATCH ≤14 对（esp_console 的参数上限）
        chunk = ids[i:i + 14]
        s.cmd("PARAM BATCH " + " ".join(f"{pid} {vec[pid]}" for pid in chunk))
        got = {int(a): int(b) for a, b in re.findall(r" id=(\d+) value=-?\d+ applied=(-?\d+)", s.last)}
        bad += [pid for pid in chunk if got.get(pid) != vec[pid]]
    (s.ok if not bad else s.fail)(f"{label}: 参数向量回写 {len(ids) - len(bad)}/{len(ids)}"
                                  + ("" if not bad else f"；不符 {bad}"))


def sound_read_globals(s: Session) -> dict:
    out = {}
    for gid in range(50):                       # 上游 PROJ_NG = 50
        s.cmd(f"PARAM GLOBAL {gid}")
        m = re.search(rf"PARAM GLOBAL id={gid} value=(-?\d+)", s.last)
        if m:
            out[gid] = int(m.group(1))
    return out


def sound_kit(s: Session) -> int:
    """鼓轨的 kit（选中轨须是 3）。"""
    s.cmd("PARAM 63")
    m = re.search(r"PARAM track=3 id=63 value=(-?\d+)", s.last)
    if m is None:
        raise InjectError(f"鼓轨 PARAM 63（KIT）回显异常：{s.last.strip()[-160:]!r}")
    return int(m.group(1))


def sound_expand_all(s: Session, label: str) -> None:
    for card in range(len(SOUND_CARD_SLOTS)):
        if sound_stats(s)["collapsed"] >> card & 1:
            s.cmd(f"UI SOUND CARDTOGGLE {card}")
    st = sound_stats(s)
    (s.ok if st["collapsed"] == 0 else s.fail)(f"{label}: 全部卡片展开 collapsed=0x{st['collapsed']:04X}")


def sound_drag_to(s: Session, sl: dict, target: int, label: str) -> None:
    """按下并拖到使槽值 == target 的方向（按下点 = 新锚点 = 当前值）；没有这样的方向时改用
    `PARAM <id> <target>`（console 写，同一状态层命令）并在回显里核对 applied。"""
    cur, lo, hi = sound_truth(s, sl["scope"], sl["id"])
    if cur == target:
        return
    for press, drag, deg in SOUND_DRAGS:
        if knob_drag_expect(lo, hi, cur, deg) == target:
            kind, got = sound_knob(s, sl["flat"], "press", *press)
            (s.ok if kind == "value" and got == cur else s.fail)(
                f"{label}: KNOB press {press} → {kind} {got}（期望 value {cur}）")
            kind, got = sound_knob(s, sl["flat"], "drag", *drag)
            (s.ok if kind == "value" and got == target else s.fail)(
                f"{label}: KNOB drag {drag}（{deg:+d}°）→ {kind} {got}（期望 value {target}）")
            return
    s.cmd(f"PARAM {sl['id']} {target}" if sl["scope"] == SOUND_SCOPE_TRACK
          else f"PARAM GLOBAL {sl['id']} {target}")
    s.expect(rf"applied={target} ", f"{label}: PARAM 写回 {target}（没有一步到位的拖动方向）")


def sound_drag_change(s: Session, sl: dict, label: str) -> int:
    """在槽上按下并拖到一个**会改值**的方向：断言 press 不改值、drag 回显 = knob_drag_value 的期望
    与 PARAM 真值；返回拖后的值（调用方负责还原）。"""
    v0, lo, hi = sound_truth(s, sl["scope"], sl["id"])
    for press, drag, deg in SOUND_DRAGS:
        want = knob_drag_expect(lo, hi, v0, deg)
        if want != v0:
            break
    else:
        raise InjectError(f"{label}: 值 {v0} 在 [{lo}..{hi}] 上任何拖动方向都不改值（值域退化）")
    kind, got = sound_knob(s, sl["flat"], "press", *press)
    (s.ok if kind == "value" and got == v0 else s.fail)(
        f"{label}: KNOB press {press} → {kind} {got}（期望 value {v0}：按下只记锚点、不改值）")
    kind, got = sound_knob(s, sl["flat"], "drag", *drag)
    (s.ok if kind == "value" and got == want else s.fail)(
        f"{label}: KNOB drag {drag}（{deg:+d}°）→ {kind} {got}"
        f"（期望 value {want} = {v0} {deg:+d}°×[{lo}..{hi}]/270）")
    sound_truth_expect(s, sl, want, label + " 拖后")
    return want


def sound_structure(s: Session, label: str, engine: int, drum: bool, per_card: bool,
                     track: int = 0) -> int:
    """结构核对：逐成员读 `UI SOUND CARD n SLOT m`，present / label 与描述符对照（规则见
    sound_expect_present；鼓轨另核锁定态 = 轨道参数且不在鼓轨可编辑表里），并与 STATS 的 slots 对账；
    返回存在槽数。per_card：逐卡一条断言（ANALOG 基线）；否则整上下文一条。"""
    analog = not drum and engine == 0
    bad, per_card_bad, n_present = [], [[] for _ in SOUND_CARD_SLOTS], 0
    for card, n in enumerate(SOUND_CARD_SLOTS):
        for slot in range(n):
            sl = sound_slot(s, card, slot)
            scope, pid = SOUND_MEMBERS[sl["flat"]]     # absent 槽的回显是 scope=- id=-：作用域/id 取成员表
            d = sound_desc(s, scope, pid, track)
            want = sound_expect_present(card, slot, d, analog)
            if sl["present"] != want:
                per_card_bad[card].append(f"卡{card}槽{slot} present={int(sl['present'])}≠{int(want)}")
            elif want:
                n_present += 1
                if (sl["scope"], sl["id"]) != (scope, pid):
                    per_card_bad[card].append(f"flat={sl['flat']} scope/id={(sl['scope'], sl['id'])}"
                                              f"≠{(scope, pid)}")
                if sl["label"] != d["label"]:
                    per_card_bad[card].append(f"flat={sl['flat']} label={sl['label']!r}≠{d['label']!r}")
                want_lock = drum and scope == SOUND_SCOPE_TRACK and (card, slot) not in SOUND_DRUM_EDITABLE
                if sl["locked"] != want_lock:
                    per_card_bad[card].append(f"flat={sl['flat']} locked={int(sl['locked'])}≠{int(want_lock)}")
            if sl["flat"] != SOUND_CARD_BASE[card] + slot:
                per_card_bad[card].append(f"flat={sl['flat']}≠{SOUND_CARD_BASE[card] + slot}")
        bad += per_card_bad[card]
        if per_card:
            (s.ok if not per_card_bad[card] else s.fail)(
                f"{label}：{SOUND_CARD_TITLE[card]} 卡 {n} 槽 flat/present/label"
                f"{'/locked' if drum else ''} 与成员表·ENG DESC 一致" + ("" if not per_card_bad[card]
                                                                      else f"：{per_card_bad[card]}"))
    what = "/label/locked" if drum else "/label"
    if not per_card:
        (s.ok if not bad else s.fail)(f"{label}：{n_present} 个存在槽 present{what} 与 ENG DESC 一致"
                                      + ("" if not bad else f"：{bad}"))
    st = sound_stats(s)
    (s.ok if st["cards"] == 13 and st["slots"] == n_present else s.fail)(
        f"{label}: UI SOUND STATS cards={st['cards']} slots={st['slots']}（期望 13 / {n_present} = 逐槽核对的存在数）")
    return n_present


def sound_slot_matches_desc(s: Session, sl: dict) -> tuple:
    """槽的 present / label 与描述符对照（ANALOG 合成轨）；返回 (一致?, 槽)。"""
    scope, pid = SOUND_MEMBERS[sl["flat"]]
    d = sound_desc(s, scope, pid)
    return (sl["present"] and sl["label"] == d["label"]
            if sound_expect_present(sl["card"], sl["slot"], d, True) else not sl["present"]), sl


def sound_pick_status(s: Session) -> dict:
    s.cmd("UI SOUND PICK")
    m = RE_SOUND_PICK.search(s.last)
    if m is None:
        if re.search(r"^UI SOUND PICK open=0\r?$", s.last, re.MULTILINE):
            return {"open": False}
        raise InjectError(f"UI SOUND PICK 回显异常：{s.last.strip()[-160:]!r}")
    return {"open": True, "row": int(m.group(1)), "engine": int(m.group(2)), "highlight": int(m.group(3)),
            "rows": int(m.group(4))}


def sound_pick_expect(s: Session, row: int, engine: int, highlight: int, rows: int, label: str) -> None:
    got = sound_pick_status(s)
    (s.ok if got == {"open": True, "row": row, "engine": engine, "highlight": highlight, "rows": rows}
     else s.fail)(f"{label}: UI SOUND PICK {got}（期望 open row={row} engine={engine} highlight={highlight} "
                  f"rows={rows}）")


def sound_pick_row(s: Session, n: int) -> dict:
    """列表第 n 行（文字 / 当前项 / 屏幕矩形）。不在一屏里的行查询时先滚进视口——要点它就先查再点。"""
    s.cmd(f"UI SOUND PICK {n}")
    m = RE_SOUND_PICK_ROW.search(s.last)
    if m is None:
        raise InjectError(f"UI SOUND PICK {n} 回显异常：{s.last.strip()[-160:]!r}")
    x, y, w, h = (int(v) for v in m.groups()[3:7])
    return {"n": int(m.group(1)), "text": m.group(2), "current": m.group(3) == "1",
            "xy": (x + w // 2, y + h // 2), "rect": (x, y, w, h)}


def sound_pick_tap(s: Session, n: int, pattern: str, label: str) -> dict:
    """TAP 列表第 n 行的中心（先 `UI SOUND PICK n` 取矩形）。"""
    row = sound_pick_row(s, n)
    s.cmd("TAP %d %d" % row["xy"])
    s.expect(pattern, label)
    return row


def sound_ring_probe(s: Session, img, rect: tuple, what: str, colour: tuple) -> None:
    """环起点 135° 的左下点（中心线半径 44）：page_sound.c 先画整圈 UI_PANEL_HI，再从 135° 起叠
    数值弧；rounded 起点帽不盖这个像素，而 150° 已落在数值弧上。名称画在槽顶，也不经过这里。"""
    cx, cy = sound_slot_xy(rect)
    s.probe(img, what, cx - 31, cy + 31, colour)


def run_sound(s: Session) -> None:
    """Sound 仪表盘（5B-1）注入：归一 → 池 → 结构 → 写入（KNOB / 键盘 / ±）→ 滚动与折叠 → 选轨条与
    选择器 → 鼓轨锁定 → ITYPE 标签与切页保值 → 12 引擎 → 有序对与往返 → 6 张截屏；收口还原。

    真值一律取 `PARAM <id>` / `PARAM GLOBAL <id>`（引擎读回；能对上 `STATE SNAP` 字段的另核一次）；
    存在性 / 标签的期望取自 `ENG DESC`（与页面同一描述符来源）。入口快照（轨 0 引擎·预置·71 参数、
    鼓轨 kit、50 个全局、选中轨、页）在归一**之前**拍，`try/finally` 还原——断言不过、InjectError、
    截屏复位重跑都走到；autosave 为 ON 时落盘的就是还原后的状态（折叠位 / 滚动 / 焦点是 UI 局部态，
    收口清零）。轨 0 的声音须来自工厂预置：换过引擎后 `ENG TRACK` 回不去用户预置。"""
    s.cmd("UI")
    m = re.search(r"UI page=(\w+) \((\d+)\)", s.last)
    entry_page = (m.group(1), m.group(2))
    s.cmd("STATE SNAP")
    entry_sel = s.snap_int(r"selected_track=(\d+)")
    eng0 = sound_eng(s, 0)
    sound_select(s, 0)
    vec0 = sound_read_vector(s)
    glob0 = sound_read_globals(s)
    sound_select(s, 3)
    kit0 = sound_kit(s)
    s.cmd("STATE SNAP")
    zoom0 = s.snap_int(r"set_zoom=(\d+)")
    if zoom0 != 0:                              # ZOOM 开时提示行换成读数行（UI_TEXT）：像素探针按 ZOOM 关
        s.cmd("STORE SETTINGS SET zoom 0")
        s.expect(r"STORE SETTINGS SET zoom=0 dirty=", "归一 ZOOM 关（提示行像素前提）")
    s.ser.note(f"入口快照：轨 0 engine={eng0[0]} preset={eng0[1]}，kit={kit0}，选中轨 {entry_sel}，"
               f"页 {entry_page[0]}，轨 0 参数 {len(vec0)} 项，全局 {len(glob0)} 项，zoom={zoom0}")

    def restore() -> None:
        try:
            sound_select(s, 0)
            if sound_eng(s, 0) != eng0:
                s.cmd(f"ENG TRACK 0 {eng0[0]} {eng0[1]}")
                s.expect(rf"ENG TRACK k=0 engine={eng0[0]} preset={eng0[1]} OK", "收口：轨 0 引擎·预置")
            sound_write_vector(s, vec0, "收口：轨 0 参数向量")
            after = sound_read_vector(s)
            (s.ok if after == vec0 else s.fail)(
                f"收口核对：轨 0 参数 {sum(after.get(k) == v for k, v in vec0.items())}/{len(vec0)} 回到入口值")
            glob1 = sound_read_globals(s)
            for gid, v in glob0.items():
                if glob1.get(gid) != v:
                    s.cmd(f"PARAM GLOBAL {gid} {v}")
            glob1 = sound_read_globals(s)
            (s.ok if glob1 == glob0 else s.fail)(
                f"收口核对：全局参数 {sum(glob1.get(k) == v for k, v in glob0.items())}/{len(glob0)} 回到入口值")
            sound_select(s, 3)
            if sound_kit(s) != kit0:
                s.cmd(f"PARAM 63 {kit0}")
                s.expect(rf"PARAM track=3 id=63 value={kit0} applied={kit0} ", "收口：鼓轨 kit")
            s.cmd("UI sound")
            sound_expand_all(s, "收口")
            s.cmd("UI SOUND SCROLL 0")
            if zoom0 != 0:
                s.cmd(f"STORE SETTINGS SET zoom {zoom0}")
                s.expect(rf"STORE SETTINGS SET zoom={zoom0} dirty=", "收口：ZOOM 回入口值")
            sound_select(s, entry_sel)
            s.cmd(f"UI {entry_page[0]}")
            s.expect(rf"UI page={entry_page[0]} \({entry_page[1]}\)", "收口：回入口页")
        except InjectError as e:
            s.ser.note(f"sound 收口还原未完成：{e}")
            s.fail(f"收口还原未完成：{e}")

    try:
        sound_segments(s, kit0)
    finally:
        restore()


def sound_segments(s: Session, kit0: int) -> None:
    """run_sound 的段落 ①–⑫（快照与还原在 run_sound）。"""
    n_cards = len(SOUND_CARD_SLOTS)

    # ---------- ① 归一：停走带、轨 0、ANALOG 预置 0、SOUND 页、滚动 0、全部展开 ----------
    ensure_stopped(s)
    sound_select(s, 0)
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "① 归一轨 0 = ANALOG 预置 0")
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "① UI sound")
    sound_expand_all(s, "①")
    y, _ = sound_scroll(s, 0)
    (s.ok if y == 0 else s.fail)(f"① UI SOUND SCROLL 0 → y={y}")
    s.cmd("UI sound CHIP")
    s.expect(r"^UI sound chip=255\r?$", "① `UI sound CHIP` → chip=255（仪表盘没有 chip 行）")
    (s.ok if "UI sound slot=" not in s.last else s.fail)("① `UI sound CHIP` 没有逐槽行")

    # ---------- ② 池：est 8,704（不得抬）/ 0 < delta ≤ est / build_ms ≤ 100 ----------
    mem = param_mem(s)
    state_, est, delta = mem["pages"].get(6, ("?", 0, 0))
    (s.ok if state_ == "built" and est == SOUND_EST else s.fail)(
        f"② UI MEM SOUND state={state_} est={est}（期望 built / {SOUND_EST:,}）")
    (s.ok if 0 < delta <= SOUND_EST else s.fail)(f"② UI MEM SOUND delta={delta}（0 < delta ≤ {SOUND_EST:,}）")
    g = mem["groups"]["EDIT"]
    s.ser.note(f"② UI MEM SOUND est={est} delta={delta}；EDIT build_ms={g['build_ms']}")
    (s.ok if g["built"] == 1 and g["sum_est"] == SOUND_EST and 0 < g["build_ms"] <= NAV_BUILD_BUDGET_MS
     else s.fail)(f"② UI MEM EDIT built={g['built']} sum_est={g['sum_est']} build_ms={g['build_ms']}ms"
                  f"（期望 1 / {SOUND_EST:,} / 0 < x ≤ {NAV_BUILD_BUDGET_MS}）")

    # ---------- ③ 结构（ANALOG）：13 卡 109 槽，逐槽 label == ENG DESC；P_FTYPE 只在 ANALOG ----------
    st = sound_stats(s)
    (s.ok if st["cards"] == 13 and st["scroll_max"] == sound_scroll_max(0) else s.fail)(
        f"③ UI SOUND STATS cards={st['cards']} scroll_max={st['scroll_max']}（期望 13 / {sound_scroll_max(0)}）")
    n = sound_structure(s, "③ ANALOG", 0, False, per_card=True)
    (s.ok if n == 109 else s.fail)(f"③ ANALOG 存在槽 {n}/109")
    ft = sound_slot(s, *SOUND_FTYPE)
    (s.ok if ft["present"] and ft["flat"] == SOUND_FLAT_FTYPE else s.fail)(
        f"③ P_FTYPE（Engine edit 卡槽 8）present={int(ft['present'])} flat={ft['flat']} label={ft['label']!r}"
        f"（期望 present=1 / flat={SOUND_FLAT_FTYPE}）")

    # ---------- ④ 写：每张卡的首个可编辑槽——press 不改值、drag 改值（= knob_drag_value）、拖回还原 ----------
    for card in range(n_cards):
        sl = next(x for x in (sound_slot(s, card, k) for k in range(SOUND_CARD_SLOTS[card]))
                  if x["present"] and not x["locked"])
        v0, _, _ = sound_truth(s, sl["scope"], sl["id"])
        label = f"④ {SOUND_CARD_TITLE[card]} 卡 {sl['label']}"
        sound_drag_change(s, sl, label)
        sound_drag_to(s, sl, v0, label + " 拖回")

    # ---------- ⑤ 拖动门禁：环带外 / 无锚点忽略且值不变（锁定槽见 ⑨）----------
    atk = sound_flat(s, SOUND_SLOT_ATK)
    v0, _, _ = sound_truth(s, atk["scope"], atk["id"])
    sound_knob_ignored(s, atk["flat"], "press", 0, SOUND_RING_MIN_R - 10, "ring band", "⑤ 环带内缘之内")
    sound_knob_ignored(s, atk["flat"], "drag", 40, 0, "ring band", "⑤ 环带外按下之后的 drag")
    sound_knob_ignored(s, atk["flat"], "press", 0, SOUND_RING_MAX_R + 8, "ring band", "⑤ 环带外缘之外")
    sound_truth_expect(s, atk, v0, "⑤ 环带外两次注入之后")
    sound_select(s, 1)                           # 换轨 ⇒ 页面作废拖动锚点
    sound_select(s, 0)
    sound_knob_ignored(s, atk["flat"], "drag", 40, 0, "ring band", "⑤ 无锚点（换轨后）的 drag")
    sound_truth_expect(s, atk, v0, "⑤ 无锚点 drag 之后")

    # ---------- ⑥ 键盘：焦点 ±1 与环绕、↑↓ / ENTER / ± 调值、折叠跳过、全折叠拒绝 ----------
    sound_focus(s, SOUND_SLOT_ATK)
    sound_keys(s, (SOUND_KEY_LEFT,), "⑥ ←")
    f = sound_focus(s)
    (s.ok if f["flat"] == SOUND_SLOT_ATK - 1 else s.fail)(
        f"⑥ ←：焦点 flat={f['flat']} {f['label']!r}（期望 {SOUND_SLOT_ATK - 1}）")
    sound_keys(s, (SOUND_KEY_RIGHT, SOUND_KEY_RIGHT), "⑥ → →")
    f = sound_focus(s)
    (s.ok if f["flat"] == SOUND_SLOT_ATK + 1 else s.fail)(
        f"⑥ → →：焦点 flat={f['flat']} {f['label']!r}（期望 {SOUND_SLOT_ATK + 1}）")
    sound_focus(s, 0)
    sound_keys(s, (SOUND_KEY_LEFT,), "⑥ 焦点在首槽按 ←")
    f = sound_focus(s)
    (s.ok if f["flat"] == SOUND_FLAT_MAX - 1 else s.fail)(
        f"⑥ ← 环绕：flat={f['flat']}（期望 {SOUND_FLAT_MAX - 1} = 末槽）")
    sound_keys(s, (SOUND_KEY_RIGHT,), "⑥ 焦点在末槽按 →")
    f = sound_focus(s)
    (s.ok if f["flat"] == 0 else s.fail)(f"⑥ → 环绕：flat={f['flat']}（期望 0）")
    sound_focus(s, SOUND_SLOT_ATK)
    orig, lo, hi = sound_truth(s, atk["scope"], atk["id"])
    if not lo < orig < hi:                      # 入口值在端点：先挪到中值，±1 才都有判别力
        sound_drag_to(s, atk, (lo + hi) // 2, "⑥ ATK 入口值在端点：先挪到中值")
    v0, lo, hi = sound_truth(s, atk["scope"], atk["id"])
    (s.ok if lo < v0 < hi else s.fail)(f"⑥ ATK 基线 {v0} 在 ({lo}..{hi}) 内（±1 都有判别力）")
    sound_keys(s, (SOUND_KEY_UP,), "⑥ ↑")
    sound_truth_expect(s, atk, v0 + 1, "⑥ ↑ = +1")
    sound_keys(s, (SOUND_KEY_DOWN,), "⑥ ↓")
    sound_truth_expect(s, atk, v0, "⑥ ↓ = −1（回基线）")
    sound_keys(s, (SOUND_KEY_ENTER,), "⑥ ENTER")
    sound_truth_expect(s, atk, v0 + 1, "⑥ ENTER = +1")
    s.cmd("TAP %d %d" % SOUND_BTN_MINUS)
    s.expect(rf"-> PARAM slot={SOUND_SLOT_ATK} id={atk['id']} delta=-1 value={v0}", "⑥ TAP −（焦点槽）")
    sound_truth_expect(s, atk, v0, "⑥ TAP − 之后")
    s.cmd("TAP %d %d" % SOUND_BTN_PLUS)
    s.expect(rf"-> PARAM slot={SOUND_SLOT_ATK} id={atk['id']} delta=\+1 value={v0 + 1}", "⑥ TAP +（焦点槽）")
    sound_truth_expect(s, atk, v0 + 1, "⑥ TAP + 之后")
    s.cmd("TAP %d %d" % SOUND_BTN_MINUS)
    s.expect(rf"-> PARAM slot={SOUND_SLOT_ATK} id={atk['id']} delta=-1 value={v0}", "⑥ TAP −（回基线）")
    sound_truth_expect(s, atk, v0, "⑥ TAP − 回基线")
    if v0 != orig:
        sound_drag_to(s, atk, orig, "⑥ ATK 回入口值")
    # 折叠跳过：焦点在 Envelope 卡（卡 2）槽 0，折叠该卡 ⇒ 焦点重指到扁平序里下一个可聚焦槽；←/→ 跳过整卡
    sound_focus(s, SOUND_SLOT_ATK)
    s.cmd(f"UI SOUND CARDTOGGLE {SOUND_CARD_ENV}")
    s.expect(rf"UI SOUND CARDTOGGLE card={SOUND_CARD_ENV} collapsed=1", "⑥ 折叠 Envelope 卡")
    f = sound_focus(s)
    (s.ok if f["flat"] == SOUND_CARD_BASE[SOUND_CARD_ENV + 1] else s.fail)(
        f"⑥ 焦点槽被折叠：重指到 flat={f['flat']} {f['label']!r}（期望 {SOUND_CARD_BASE[SOUND_CARD_ENV + 1]}）")
    sound_keys(s, (SOUND_KEY_LEFT,), "⑥ ←（跳过折叠卡）")
    f = sound_focus(s)
    (s.ok if f["flat"] == SOUND_CARD_BASE[SOUND_CARD_ENV] - 1 else s.fail)(
        f"⑥ ← 跳过折叠的 Envelope 卡：flat={f['flat']}（期望 {SOUND_CARD_BASE[SOUND_CARD_ENV] - 1}）")
    sound_keys(s, (SOUND_KEY_RIGHT,), "⑥ →（跳过折叠卡）")
    f = sound_focus(s)
    (s.ok if f["flat"] == SOUND_CARD_BASE[SOUND_CARD_ENV + 1] else s.fail)(
        f"⑥ → 跳过折叠的 Envelope 卡：flat={f['flat']}（期望 {SOUND_CARD_BASE[SOUND_CARD_ENV + 1]}）")
    s.cmd(f"UI SOUND CARDTOGGLE {SOUND_CARD_ENV}")
    s.expect(rf"UI SOUND CARDTOGGLE card={SOUND_CARD_ENV} collapsed=0", "⑥ 展开 Envelope 卡")
    # R12：全部折叠 ⇒ 焦点保持原槽（它仍存在）、↑/↓ 与 ± 拒绝（值不变）
    for card in range(n_cards):
        s.cmd(f"UI SOUND CARDTOGGLE {card}")
    st = sound_stats(s)
    (s.ok if st["collapsed"] == (1 << n_cards) - 1 else s.fail)(
        f"⑥ 全部折叠：collapsed=0x{st['collapsed']:04X}（期望 0x{(1 << n_cards) - 1:04X}）")
    # R12：逐卡折叠时焦点按「扁平序里下一个可聚焦槽」顺次重指（随折叠顺序走），**全部折叠后**每拍的重指
    # 找不到可聚焦槽 ⇒ 焦点保持（它仍是个存在的槽，只是所在卡被折叠）；这里断言「是个存在槽」+ 跨拍稳定
    f = sound_focus(s)
    slk = sound_flat(s, f["flat"])
    time.sleep(0.1)
    f2 = sound_focus(s)
    (s.ok if slk["present"] and f2["flat"] == f["flat"] else s.fail)(
        f"⑥ 全折叠下焦点保持：flat={f['flat']} {f['label']!r}（存在的槽；跨拍读回 {f2['flat']}）")
    vk, _, _ = sound_truth(s, slk["scope"], slk["id"])
    sound_keys(s, (SOUND_KEY_UP, SOUND_KEY_DOWN, SOUND_KEY_ENTER), "⑥ 全折叠下 ↑ ↓ ENTER")
    s.cmd("TAP %d %d" % SOUND_BTN_PLUS)
    s.expect(rf"-> PARAM slot={f['flat']} {SOUND_PARAM_LOCKED}", "⑥ 全折叠下 TAP + → locked")
    sound_truth_expect(s, slk, vk, "⑥ 全折叠下 ↑↓/ENTER/+ 之后（值不变）")
    sound_expand_all(s, "⑥")

    # ---------- ⑦ 滚动与折叠：钳位、重排、焦点槽自动滚入视口；截屏 default / scrolled / collapsed ----------
    sound_focus(s, 0)
    y, mx = sound_scroll(s, 0)
    (s.ok if (y, mx) == (0, sound_scroll_max(0)) else s.fail)(
        f"⑦ SCROLL 0 → y={y} max={mx}（期望 0 / {sound_scroll_max(0)} = 内容高 − 视口高）")
    img = s.capture("default")
    s.probe(img, "⑦ 默认帧：Global 卡底（UI_PANEL）", *sound_card_probe(0, 0, 0), C_PANEL, 10)
    s.probe(img, "⑦ 默认帧：Master 卡底（UI_PANEL）", *sound_card_probe(0, 0, 1), C_PANEL, 10)
    s.probe(img, "⑦ 默认帧：卡间 8px 缝是页面背景", *sound_gap_probe(0, 0, SOUND_CARD_ENV), C_BG, 10)
    s.probe(img, "⑦ 默认帧：列间 16px 缝是页面背景", SOUND_VIEW_X + SOUND_COL_W + SOUND_COL_GAP // 2, 400,
            C_BG, 10)
    s.probe(img, "⑦ 默认帧：选轨条第 1 格（当前轨）UI_PANEL_SEL", *sound_sel_xy(0), C_PANEL_SEL, 10)
    s.probe(img, "⑦ 默认帧：选轨条第 2 格 UI_PANEL", *sound_sel_xy(1), C_PANEL, 10)
    s.probe_region(img, "⑦ 默认帧：提示行灰字", (16, SOUND_TAB + 46, 1088, SOUND_TAB + 72), C_TEXT_DIM, 24, 40)
    rect = sound_slot(s, 0, 0)["rect"]
    (s.ok if rect == sound_slot_rect(0, 0, 0, 0) else s.fail)(
        f"⑦ 槽 0 矩形 {rect}（期望 {sound_slot_rect(0, 0, 0, 0)}：页面区 + 视口顶 + 标题行）")
    s.probe(img, "⑦ 默认帧：焦点槽（BPM）描边 UI_ACCENT", rect[0] + 2, rect[1] + 66, C_YELLOW, 10)
    sound_ring_probe(s, img, rect, "⑦ 默认帧：BPM 环顶 UI_PANEL_HI", C_PANEL_HI)
    cx, cy = sound_slot_xy(rect)
    s.probe_region(img, "⑦ 默认帧：BPM 值文本（UI_TEXT）", (cx - 40, cy - 9, cx + 40, cy + 9), C_TEXT, 24, 20)
    s.probe_region(img, "⑦ 默认帧：BPM 名称灰字", (cx - 40, rect[1], cx + 40, rect[1] + 16), C_TEXT_DIM, 24, 12)
    y, mx = sound_scroll(s, 99999)
    (s.ok if (y, mx) == (sound_scroll_max(0), sound_scroll_max(0)) else s.fail)(
        f"⑦ SCROLL 99999 钳到底：y={y}（期望 max={sound_scroll_max(0)}）")
    img = s.capture("scrolled")
    s.probe(img, "⑦ 滚到底：Texture 卡底（UI_PANEL）", *sound_card_probe(0, y, 12), C_PANEL, 10)
    s.probe(img, "⑦ 滚到底：MIDI 卡底（UI_PANEL）", *sound_card_probe(0, y, 11), C_PANEL, 10)
    s.probe(img, "⑦ 滚到底：Drums/Texture 卡间缝是页面背景", *sound_gap_probe(0, y, 12), C_BG, 10)
    s.probe_region(img, "⑦ 滚到底：提示行灰字（不随卡片滚动）", (16, SOUND_TAB + 46, 1088, SOUND_TAB + 72),
                   C_TEXT_DIM, 24, 40)
    sl = sound_slot(s, SOUND_CARD_ENGINE, 0)
    (s.ok if sl["rect"] == sound_slot_rect(0, y, SOUND_CARD_ENGINE, 0) else s.fail)(
        f"⑦ 滚到底后 Engine edit 卡槽 0 矩形 {sl['rect']}（期望 {sound_slot_rect(0, y, SOUND_CARD_ENGINE, 0)}）")
    y, _ = sound_scroll(s, -5)
    (s.ok if y == 0 else s.fail)(f"⑦ SCROLL -5 钳到顶：y={y}")
    y, _ = sound_scroll(s, 500)
    (s.ok if y == 500 else s.fail)(f"⑦ SCROLL 500 → y={y}")
    sl = sound_slot(s, SOUND_CARD_ENGINE, 0)
    (s.ok if sl["rect"] == sound_slot_rect(0, 500, SOUND_CARD_ENGINE, 0) else s.fail)(
        f"⑦ 滚动 500 后 Engine edit 卡槽 0 矩形 {sl['rect']}（期望 {sound_slot_rect(0, 500, SOUND_CARD_ENGINE, 0)}）")
    sound_focus(s, SOUND_SLOT_ATK)
    y, _ = sound_scroll(s)
    top = sound_slot(s, SOUND_CARD_ENV, 0)["rect"][1]
    content_y = sound_slot_rect(0, 0, SOUND_CARD_ENV, 0)[1] - SOUND_TAB - SOUND_VIEW_Y     # 224：卡 y + 标题行
    want_y = max(0, content_y - 8)               # 焦点槽滚进视口：越过上缘（留 8px 边距）才滚、滚到刚好可见
    (s.ok if y == want_y and top == sound_slot_rect(0, want_y, SOUND_CARD_ENV, 0)[1]
     and top >= SOUND_TAB + SOUND_VIEW_Y else s.fail)(
        f"⑦ 焦点落到 ATK：自动滚入视口 y={y}，槽矩形顶 {top}（期望 y={want_y} = 内容 y {content_y} − 8、"
        f"顶 {sound_slot_rect(0, want_y, SOUND_CARD_ENV, 0)[1]} ≥ 视口顶 {SOUND_TAB + SOUND_VIEW_Y}）")
    # 折叠重排：Envelope / FX 折成 36px 标题行 ⇒ 右列 / 左列后续卡上移；scroll_max 按镜像对账
    mask = (1 << SOUND_CARD_ENV) | (1 << (SOUND_CARD_ENV + 1))
    for card in (SOUND_CARD_ENV, SOUND_CARD_ENV + 1):     # Envelope / LFO：折叠后仍在首屏（视口 152..716）
        s.cmd(f"UI SOUND CARDTOGGLE {card}")
    st = sound_stats(s)
    (s.ok if st["collapsed"] == mask and st["scroll_max"] == sound_scroll_max(mask) else s.fail)(
        f"⑦ 折叠 Envelope + LFO：scroll_max={st['scroll_max']} collapsed=0x{st['collapsed']:04X}"
        f"（期望 {sound_scroll_max(mask)} / 0x{mask:04X}）")
    y, _ = sound_scroll(s, 0)
    img = s.capture("collapsed")
    for card in (SOUND_CARD_ENV, SOUND_CARD_ENV + 1):
        dead = sound_slot(s, card, 0)
        (s.ok if dead["present"] and dead["rect"] == (0, 0, 0, 0) else s.fail)(
            f"⑦ 折叠卡 {SOUND_CARD_TITLE[card]} 的槽仍有描述符但无矩形：present={int(dead['present'])} "
            f"rect={dead['rect']}（期望 present=1 / (0, 0, 0, 0)）")
        s.probe(img, f"⑦ 折叠帧：{SOUND_CARD_TITLE[card]} 卡折成标题行（UI_PANEL）",
                *sound_card_probe(mask, 0, card), C_PANEL, 10)
    # 折叠卡原来的槽区：后面的卡顶上来（Engine edit 卡的 WAVE 槽环顶），折叠的 LFO 与它之间是 8px 缝
    nxt = sound_slot(s, SOUND_CARD_ENGINE, 0)["rect"]
    (s.ok if nxt == sound_slot_rect(mask, 0, SOUND_CARD_ENGINE, 0) else s.fail)(
        f"⑦ 折叠后重排：Engine edit 卡槽 0 矩形 {nxt}（期望 {sound_slot_rect(mask, 0, SOUND_CARD_ENGINE, 0)}："
        f"原 {sound_slot_rect(0, 0, SOUND_CARD_ENGINE, 0)}）")
    s.probe(img, "⑦ 折叠帧：LFO 卡与其下 Engine edit 卡之间是页面背景", *sound_gap_probe(mask, 0, SOUND_CARD_ENGINE),
            C_BG, 10)
    sound_ring_probe(s, img, nxt, "⑦ 折叠帧：上移后的 Engine edit 首槽环顶 UI_PANEL_HI", C_PANEL_HI)
    sound_expand_all(s, "⑦")
    y, mx = sound_scroll(s, 0)
    (s.ok if mx == sound_scroll_max(0) else s.fail)(f"⑦ 重新展开：scroll_max={mx}（期望 {sound_scroll_max(0)}）")

    # ---------- ⑧ 选轨条 + 页眉选择器（R16）；截屏 picker-engine / picker-preset ----------
    s.cmd("TAP %d %d" % sound_sel_tap_xy(1))
    s.expect(r"-> TRACKS select track=1 CMD_SELECT_TRACK", "⑧ TAP 选轨条第 2 格")
    s.cmd("UI")
    s.expect(r"UI page=sound \(6\)", "⑧ 选轨后仍在 SOUND 页")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 1, "⑧ STATE SNAP selected_track=1")
    st = sound_stats(s)
    (s.ok if st["slots"] == 109 else s.fail)(f"⑧ 轨 1 的仪表盘 slots={st['slots']}（轨 1 = ANALOG ⇒ 109）")
    s.cmd("TAP %d %d" % sound_sel_tap_xy(0))
    s.expect(r"-> TRACKS select track=0 CMD_SELECT_TRACK", "⑧ TAP 选轨条第 1 格回轨 0")
    s.cmd("STATE SNAP")
    s.expect_int(r"selected_track=(\d+)", 0, "⑧ STATE SNAP selected_track=0")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    s.expect(r"-> SOUND pick open row=0", "⑧ TAP 页眉 ENGINE 行名字区 → 选择器 row 0")
    sound_pick_expect(s, 0, 0, 0, 12, "⑧ 打开引擎选择器")
    names = tuple(sound_pick_row(s, e)["text"] for e in range(12))
    (s.ok if names[0] == "ANALOG" and names[9] == "FM6" and len(set(names)) == 12 else s.fail)(
        f"⑧ 引擎列表 12 行：{' / '.join(names)}（期望 ANALOG 起、FM6 = 第 10 行、互不重名）")
    s.cmd("UI SOUND PICK 12")
    s.expect(r"^UI ERR sound \(absent\)\r?$", "⑧ `UI SOUND PICK 12`（越界）→ absent")
    s.cmd("UI SOUND PICK x")
    s.expect(r"^UI ERR sound \(invalid\)\r?$", "⑧ `UI SOUND PICK x` → invalid")
    row = sound_pick_row(s, 0)
    (s.ok if row["current"] else s.fail)("⑧ 引擎行 0（ANALOG）current=1")
    sound_pick_tap(s, 0, r"-> SOUND pick engine=0 unchanged", "⑧ TAP 当前引擎行 → unchanged（不下发）")
    (s.ok if sound_pick_status(s) == {"open": False} else s.fail)("⑧ 确认当前项后选择器关闭")
    (s.ok if sound_eng(s, 0) == (0, 0) else s.fail)(f"⑧ unchanged 之后 ENG SNAP 轨 0 = {sound_eng(s, 0)}")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    s.expect(r"-> SOUND pick open row=0", "⑧ 再开引擎选择器")
    s.cmd("TAP %d %d" % sound_chip_xy(names, 1))
    s.expect(r"-> SOUND pick engine=1 highlight", "⑧ TAP chip DIGITAL → 只高亮")
    sound_pick_expect(s, 0, 0, 1, 12, "⑧ chip 高亮 ≠ 当前引擎")
    (s.ok if sound_eng(s, 0) == (0, 0) else s.fail)("⑧ chip 高亮不下发（ENG SNAP 轨 0 仍 ANALOG）")
    s.cmd("TAP %d %d" % sound_chip_xy(names, 1))
    s.expect(r"-> SOUND pick engine=1 CMD_SET_ENGINE", "⑧ 再 TAP 已高亮的 chip → CMD_SET_ENGINE")
    (s.ok if sound_eng(s, 0) == (1, 0) and sound_pick_status(s) == {"open": False} else s.fail)(
        f"⑧ 换引擎后 ENG SNAP 轨 0 = {sound_eng(s, 0)}，选择器关闭")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    sound_pick_expect(s, 0, 1, 1, 12, "⑧ 当前 = DIGITAL 时再开")
    row = sound_pick_row(s, 1)
    (s.ok if row["current"] and row["text"] == "DIGITAL" else s.fail)(f"⑧ 引擎行 1：{row}")
    img = s.capture("picker-engine")
    s.probe(img, "⑧ 选择器帧：面板左缘 2px 描边 UI_PANEL_HI", SOUND_PICK_X, 420, C_PANEL_HI, 10)
    s.probe(img, "⑧ 选择器帧：面板顶缘 2px 描边 UI_PANEL_HI", 600, SOUND_TAB + SOUND_PICK_Y, C_PANEL_HI, 10)
    s.probe(img, "⑧ 选择器帧：面板底 UI_PANEL（列表右端的空白）", 1000, 420, C_PANEL, 10)
    s.probe(img, "⑧ 选择器帧：面板外的页眉行底 UI_PANEL_HI", 16 + 3, SOUND_ROW_Y + 4, C_PANEL_HI, 10)
    chip = sound_chip_xy(names, 1)
    s.probe_region(img, "⑧ 选择器帧：高亮 chip（DIGITAL）UI_ACCENT", (chip[0] - 20, chip[1] - 16, chip[0] + 20, chip[1] + 16),
                   C_YELLOW, 24, 200)
    s.probe(img, "⑧ 选择器帧：未高亮 chip（ANALOG）UI_PANEL_HI", *sound_chip_probe(names, 0), C_PANEL_HI, 10)
    y0 = SOUND_TAB + SOUND_PICK_Y + SOUND_PICK_LIST_Y_SYNTH
    s.probe(img, "⑧ 选择器帧：高亮行（DIGITAL）条 UI_PANEL_HI", 1000, y0 + SOUND_PICK_ROW_H + SOUND_PICK_ROW_H // 2,
            C_PANEL_HI, 10)
    s.probe(img, "⑧ 选择器帧：非高亮行（ANALOG）是面板底", 1000, y0 + SOUND_PICK_ROW_H // 2, C_PANEL, 10)
    s.probe_region(img, "⑧ 选择器帧：当前项强调色文字", (SOUND_PICK_LIST_X + 12, y0 + SOUND_PICK_ROW_H + 4,
                                                  SOUND_PICK_LIST_X + 200, y0 + 2 * SOUND_PICK_ROW_H - 2),
                   C_YELLOW, 24, 20)
    s.probe_region(img, "⑧ 选择器帧：底行提示灰字", (SOUND_PICK_LIST_X, SOUND_TAB + SOUND_PICK_Y + SOUND_PICK_H - 28,
                                                SOUND_PICK_LIST_X + SOUND_PICK_LIST_W, SOUND_TAB + SOUND_PICK_Y + SOUND_PICK_H - 4),
                   C_TEXT_DIM, 24, 40)
    sound_pick_tap(s, 0, r"-> SOUND pick engine=0 CMD_SET_ENGINE", "⑧ TAP 引擎行 0（ANALOG）")
    (s.ok if sound_eng(s, 0) == (0, 0) else s.fail)(f"⑧ TAP 引擎行后 ENG SNAP 轨 0 = {sound_eng(s, 0)}")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    sound_keys(s, (SOUND_KEY_UP,), "⑧ 选择器里 ↑（列表高亮环绕）")
    sound_pick_expect(s, 0, 0, 11, 12, "⑧ ↑ 之后高亮 NOISE")
    sound_keys(s, (SOUND_KEY_ENTER,), "⑧ 选择器里 ENTER")
    (s.ok if sound_eng(s, 0) == (11, 0) and sound_pick_status(s) == {"open": False} else s.fail)(
        f"⑧ row 0 列表焦点 ENTER ⇒ ENG SNAP 轨 0 = {sound_eng(s, 0)}，选择器关闭")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    sound_pick_tap(s, 0, r"-> SOUND pick engine=0 CMD_SET_ENGINE", "⑧ TAP 引擎行 0 回 ANALOG")
    # 预置选择器（row 1 = 高亮引擎的预置；current 只在 chip 高亮 == 当前引擎时可能为 1）
    s.cmd("TAP %d %d" % SOUND_NAME_ROW1)
    s.expect(r"-> SOUND pick open row=1", "⑧ TAP 页眉 PRESET 行名字区 → 选择器 row 1")
    sound_pick_expect(s, 1, 0, 0, 22, "⑧ 打开预置选择器（ANALOG 22 个预置）")
    row = sound_pick_row(s, 0)
    (s.ok if row["current"] and row["text"].endswith("808 BOOM") else s.fail)(f"⑧ 预置行 0：{row}")
    img = s.capture("picker-preset")
    s.probe(img, "⑧ 预置选择器帧：面板左缘描边 UI_PANEL_HI", SOUND_PICK_X, 420, C_PANEL_HI, 10)
    s.probe_region(img, "⑧ 预置选择器帧：高亮 chip（ANALOG）UI_ACCENT", (sound_chip_xy(names, 0)[0] - 20,
                                                                   chip[1] - 16, sound_chip_xy(names, 0)[0] + 20,
                                                                   chip[1] + 16), C_YELLOW, 24, 200)
    s.probe(img, "⑧ 预置选择器帧：高亮行（预置 1）条 UI_PANEL_HI", 1000, y0 + SOUND_PICK_ROW_H // 2, C_PANEL_HI, 10)
    s.probe(img, "⑧ 预置选择器帧：非高亮行（预置 2）是面板底", 1000, y0 + SOUND_PICK_ROW_H + SOUND_PICK_ROW_H // 2,
            C_PANEL, 10)
    s.probe_region(img, "⑧ 预置选择器帧：当前项强调色文字", (SOUND_PICK_LIST_X + 12, y0 + 4, SOUND_PICK_LIST_X + 200,
                                                  y0 + SOUND_PICK_ROW_H - 2), C_YELLOW, 24, 20)
    sound_pick_tap(s, 3, r"-> SOUND pick preset=3 CMD_SET_PRESET", "⑧ TAP 预置行 3")
    (s.ok if sound_eng(s, 0) == (0, 3) else s.fail)(f"⑧ TAP 预置行后 ENG SNAP 轨 0 = {sound_eng(s, 0)}")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW1)
    (s.ok if sound_pick_row(s, 3)["current"] else s.fail)("⑧ 再开：预置行 3 current=1")
    sound_pick_tap(s, 3, r"-> SOUND pick preset=3 unchanged", "⑧ TAP 当前预置行 → unchanged")
    (s.ok if sound_eng(s, 0) == (0, 3) else s.fail)("⑧ unchanged 之后预置仍 3")
    # R16：chip 高亮 ≠ 当前引擎 ⇒ 列表只是预览：点行不执行、列表焦点的 ENTER 不动作；chip 焦点 ENTER 换引擎
    s.cmd("TAP %d %d" % SOUND_NAME_ROW1)
    sound_keys(s, (SOUND_KEY_RIGHT,), "⑧ 预置选择器里 →（chip 高亮到 DIGITAL）")
    st = sound_pick_status(s)
    (s.ok if (st["row"], st["engine"], st["highlight"]) == (1, 0, 1) and st["rows"] > 0 else s.fail)(
        f"⑧ → 之后 {st}（row 1 / 当前引擎 0 / 高亮 1）")
    row = sound_pick_row(s, 0)
    (s.ok if not row["current"] else s.fail)(f"⑧ 预览态：预置行 0 {row['text']!r} current=0")
    sound_pick_tap(s, 0, r"-> SOUND pick preview engine=1", "⑧ 预览态 TAP 预置行 → preview（不下发）")
    sound_pick_expect(s, 1, 0, 1, st["rows"], "⑧ 预览态点行后面板保持打开")
    (s.ok if sound_eng(s, 0) == (0, 3) else s.fail)("⑧ 预览态点行：ENG SNAP 轨 0 仍 ANALOG 预置 3")
    sound_keys(s, (SOUND_KEY_DOWN, SOUND_KEY_ENTER), "⑧ 预览态 ↓ + ENTER（焦点在列表）")
    sound_pick_expect(s, 1, 0, 1, st["rows"], "⑧ 列表焦点的 ENTER 不动作（面板仍开）")
    (s.ok if sound_eng(s, 0) == (0, 3) else s.fail)("⑧ 列表焦点的 ENTER：轨 0 仍 ANALOG 预置 3")
    sound_keys(s, (SOUND_KEY_RIGHT,), "⑧ 预览态 →（焦点回 chip 行；高亮 1 → 2）")
    st2 = sound_pick_status(s)
    (s.ok if (st2["open"], st2["row"], st2["engine"], st2["highlight"]) == (True, 1, 0, 2) and st2["rows"] > 0
     else s.fail)(f"⑧ → 之后 {st2}（期望 row 1 / 当前引擎 0 / 高亮 2；rows = 高亮引擎的预置数）")
    sound_keys(s, (SOUND_KEY_ENTER,), "⑧ 预览态 chip 焦点 ENTER")
    (s.ok if sound_eng(s, 0) == (2, 0) and sound_pick_status(s) == {"open": False} else s.fail)(
        f"⑧ chip 焦点 ENTER ⇒ ENG SNAP 轨 0 = {sound_eng(s, 0)}（期望 (2, 0)：CMD_SET_ENGINE），选择器关闭")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "⑧ 轨 0 回 ANALOG 预置 0")
    time.sleep(0.1)  # ENG TRACK 直写 core；等 state_sync 刷新 sound 投影，否则选择器下一拍按上下文变化关闭
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    s.expect(r"-> SOUND pick open row=0", "⑧ 再开选择器（ESC 用例）")
    sound_keys(s, (SOUND_KEY_ESC,), "⑧ ESC")
    (s.ok if sound_pick_status(s) == {"open": False} else s.fail)("⑧ ESC 关闭选择器")
    s.cmd("UI")
    s.expect(r"UI page=sound \(6\)", "⑧ ESC 不切页（仍 SOUND）")

    # ---------- ⑨ 锁定：鼓轨上未标 drum_ok 的轨道参数全锁（PATTERN 卡可编辑）；截屏 drum ----------
    sound_select(s, 3)
    s.cmd("UI")
    s.expect(r"UI page=sound \(6\)", "⑨ Ctrl+4 后仍在 SOUND 页")
    st = sound_stats(s)
    (s.ok if st["slots"] == 101 else s.fail)(f"⑨ 鼓轨 UI SOUND STATS slots={st['slots']}（期望 101 = 109 − E1..E7 − FTYPE）")
    n = sound_structure(s, "⑨ 鼓轨", 0, True, per_card=False, track=3)
    (s.ok if n == 101 else s.fail)(f"⑨ 鼓轨存在槽 {n}/101")
    eng_card = [sound_slot(s, SOUND_CARD_ENGINE, k) for k in range(SOUND_CARD_SLOTS[SOUND_CARD_ENGINE])]
    (s.ok if sum(x["locked"] for x in eng_card) == 1 and sum(x["present"] for x in eng_card) == 1
     and eng_card[0]["present"] and eng_card[0]["label"] == "KIT" else s.fail)(
        f"⑨ Engine edit 卡：{' '.join(f'{x['label'] if x['present'] else '-'}/{'L' if x['locked'] else 'e'}' for x in eng_card)}"
        f"（期望只有 KIT present 且 locked；E1..E7 与 P_FTYPE absent）")
    pat = [sound_slot(s, SOUND_CARD_PATTERN, k) for k in range(4)]
    (s.ok if all(x["present"] and not x["locked"] for x in pat) else s.fail)(
        f"⑨ Pattern 卡：{' '.join(f'{x['label']}/{'L' if x['locked'] else 'e'}' for x in pat)}（期望 4 个 present 且可编辑）")
    atk = sound_slot(s, SOUND_CARD_ENV, 0)
    (s.ok if atk["locked"] else s.fail)(f"⑨ 鼓轨 ATK 槽 locked={int(atk['locked'])}")
    va, _, _ = sound_truth(s, SOUND_SCOPE_TRACK, atk["id"])
    sound_knob_ignored(s, atk["flat"], "press", 0, -40, "locked", "⑤/⑨ 锁定槽 press")
    sound_knob_ignored(s, atk["flat"], "drag", 40, 0, "locked", "⑤/⑨ 锁定槽 drag")
    sound_focus(s, atk["flat"])
    sound_keys(s, (SOUND_KEY_UP, SOUND_KEY_ENTER), "⑨ 锁定槽 ↑ ENTER")
    s.cmd("TAP %d %d" % SOUND_BTN_PLUS)
    s.expect(rf"-> PARAM slot={atk['flat']} {SOUND_PARAM_LOCKED}", "⑨ 锁定槽 TAP + → locked")
    s.cmd("STATE SNAP")
    s.expect_int(r"atk=(-?\d+)", va, "⑨ STATE SNAP atk（锁定槽注入后不变）")
    sound_truth_expect(s, atk, va, "⑤/⑨ 锁定槽注入之后")
    sl = sound_slot(s, SOUND_CARD_PATTERN, 0)
    vs, lo, hi = sound_truth(s, SOUND_SCOPE_TRACK, sl["id"])
    label = "⑨ Pattern 卡 LEN（鼓轨可编辑）"
    want = sound_drag_change(s, sl, label)
    sound_drag_to(s, sl, vs, label + " 拖回")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW1)
    s.expect(r"TAP miss 512,92 page=6", "⑨ 鼓轨没有 PRESET 行：其名字区 TAP miss")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    s.expect(r"-> SOUND pick open row=0", "⑨ TAP 页眉 KIT 行名字区 → 选择器 row 0")
    sound_pick_expect(s, 0, 0, 0, 46, "⑨ 打开 kit 选择器（46 个 kit）")
    row = sound_pick_row(s, 1)
    (s.ok if not row["current"] and row["rect"][1] == SOUND_TAB + SOUND_PICK_Y + SOUND_PICK_LIST_Y_DRUM + SOUND_PICK_ROW_H
     else s.fail)(f"⑨ kit 行 1：{row}（鼓轨列表从面板顶起、非当前项）")
    want_kit = 1 if kit0 != 1 else 2
    sound_pick_tap(s, want_kit, rf"-> SOUND pick kit={want_kit} CMD_SET_PARAM", "⑨ TAP kit 行")
    (s.ok if sound_kit(s) == want_kit else s.fail)(f"⑨ TAP kit 行后 PARAM 63 = {sound_kit(s)}（期望 {want_kit}）")
    s.cmd("TAP %d %d" % SOUND_NAME_ROW0)
    (s.ok if sound_pick_row(s, want_kit)["current"] else s.fail)(f"⑨ 再开：kit 行 {want_kit} current=1")
    sound_pick_tap(s, want_kit, rf"-> SOUND pick kit={want_kit} unchanged", "⑨ TAP 当前 kit 行 → unchanged")
    (s.ok if sound_kit(s) == want_kit else s.fail)("⑨ unchanged 之后 kit 不变")
    s.cmd(f"PARAM 63 {kit0}")
    s.expect(rf"PARAM track=3 id=63 value={kit0} applied={kit0} ", "⑨ kit 回入口值")
    sound_focus(s, SOUND_CARD_BASE[1] + 4)      # FILT：Master 卡的轨道参数、鼓轨可编辑
    s.cmd("UI SOUND SCROLL 0")
    atk = sound_flat(s, SOUND_SLOT_ATK)
    img = s.capture("drum")
    s.probe(img, "⑨ 鼓轨帧：KIT 行底色 UI_PANEL_HI", 16 + 3, SOUND_ROW_Y + 4, C_PANEL_HI, 10)
    s.probe(img, "⑨ 鼓轨帧：PRESET 行缺席（页面背景）", 320 + 3, SOUND_ROW_Y + 4, C_BG, 10)
    s.probe_region(img, "⑨ 鼓轨帧：页眉红字（NO SOUND）", (320, SOUND_ROW_Y + 4, 1080, SOUND_ROW_Y + 32),
                   C_ERROR, 16, 200)
    s.probe(img, "⑨ 鼓轨帧：选轨条第 4 格（当前轨）UI_PANEL_SEL", *sound_sel_xy(3), C_PANEL_SEL, 10)
    sound_ring_probe(s, img, atk["rect"], "⑨ 鼓轨帧：锁定槽（ATK）环位是卡片底 UI_PANEL", C_PANEL)
    cx, cy = sound_slot_xy(atk["rect"])
    s.probe_region(img, "⑨ 鼓轨帧：锁定槽的值 `--`（UI_TEXT）", (cx - 20, cy - 9, cx + 20, cy + 9), C_TEXT, 24, 8)
    filt = sound_slot(s, 1, 4)["rect"]
    sound_ring_probe(s, img, filt, "⑨ 鼓轨帧：可编辑槽（FILT）环顶 UI_PANEL_HI", C_PANEL_HI)
    s.probe(img, "⑨ 鼓轨帧：焦点槽（FILT）描边 UI_ACCENT", filt[0] + 2, filt[1] + 66, C_YELLOW, 10)
    s.cmd("TAP %d %d" % SOUND_NAME_ROW1)
    s.expect(r"TAP miss 512,92 page=6", "⑨ 鼓轨 PRESET 行矩形 TAP miss")
    sound_select(s, 0)
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "⑨ 回轨 0（SOUND 页）")
    st = sound_stats(s)
    (s.ok if st["slots"] == 109 else s.fail)(f"⑨ 回合成轨：slots={st['slots']}（期望 109）")

    # ---------- ⑩ INSERT 槽标签随 ITYPE 重解析；编辑跨页保值 ----------
    ity = sound_flat(s, SOUND_SLOT_ITYPE)
    ins = [sound_flat(s, SOUND_SLOT_ITYPE + k) for k in (1, 2, 3, 4)]
    labels0 = [x["label"] for x in ins]
    v_it, lo_it, hi_it = sound_truth(s, SOUND_SCOPE_TRACK, ity["id"])
    want_it = v_it + 1 if v_it < hi_it else v_it - 1
    sound_drag_to(s, ity, want_it, "⑩ ITYPE 槽改值")
    (s.ok if sound_truth(s, SOUND_SCOPE_TRACK, ity["id"])[0] == want_it else s.fail)(
        f"⑩ ITYPE {v_it} → {want_it}（PARAM {ity['id']}）")
    after = [sound_slot_matches_desc(s, sound_flat(s, x["flat"])) for x in ins]
    (s.ok if all(ok for ok, _ in after) else s.fail)(
        "⑩ ITYPE 改后 INS A / INS B / INS C / MIX 四槽 present/label 重解析 == ENG DESC live："
        + " | ".join(f"{a['label']!r}→{b['label']!r}" for a, (_, b) in zip(ins, after))
        + ("" if all(ok for ok, _ in after) else f"；不符 {[m for _, (ok, m) in zip(after, after) if not ok]}"))
    (s.ok if sum(a["label"] != b["label"] for a, (_, b) in zip(ins, after)) >= 1 else s.fail)(
        "⑩ ITYPE 改后至少一个 INSERT 槽的标签变了（随 ITYPE 重解析）")
    sound_drag_to(s, ity, v_it, "⑩ ITYPE 槽拖回")
    back = [sound_slot_matches_desc(s, sound_flat(s, x["flat"])) for x in ins]
    (s.ok if all(ok for ok, _ in back) and all(b["label"] == lab for (_, b), lab in zip(back, labels0)) else s.fail)(
        "⑩ ITYPE 还原后四槽标签回到入口值：" + " | ".join(f"{lab!r}" for lab in labels0))
    atk = sound_flat(s, SOUND_SLOT_ATK)
    va, _, _ = sound_truth(s, SOUND_SCOPE_TRACK, atk["id"])
    want = sound_drag_change(s, atk, "⑩ ATK 经 KNOB 改值")
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑩ UI step")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑩ UI transport")
    s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "⑩ UI sound")
    y = sound_flat(s, SOUND_SLOT_ATK)
    (s.ok if y["value"] == want else s.fail)(f"⑩ 切页回来槽显示 ATK value={y['value']}（期望 {want}）")
    sound_truth_expect(s, atk, want, "⑩ 切页回来")
    sound_drag_to(s, atk, va, "⑩ ATK 还原")

    # ---------- ③b 12 引擎轮换：每个引擎的每个存在槽 label == ENG DESC live；P_FTYPE 只在 ANALOG ----------
    counts = {}
    for e in range(12):
        s.cmd(f"ENG TRACK 0 {e} 0")
        s.expect(rf"ENG TRACK k=0 engine={e} preset=0 OK", f"③ 换引擎 {e}（{names[e]}）")
        time.sleep(0.1)
        n = sound_structure(s, f"③ 引擎 {e}（{names[e]}）", e, False, per_card=False)
        ft = sound_slot(s, *SOUND_FTYPE)
        (s.ok if ft["present"] == (e == 0) else s.fail)(
            f"③ 引擎 {e}（{names[e]}）：P_FTYPE present={int(ft['present'])}（期望 {int(e == 0)}：仅 ANALOG）"
            f"；slots={n}")
        counts[e] = n
    s.ser.note(f"③ 12 引擎存在槽数：{counts}")
    (s.ok if counts[9] == 108 else s.fail)(f"③ FM6（引擎 9）slots={counts[9]}（期望 108）")
    s.cmd("ENG TRACK 0 0 0")
    s.expect(r"ENG TRACK k=0 engine=0 preset=0 OK", "③ 回 ANALOG")
    st = sound_stats(s)
    (s.ok if st["slots"] == 109 and st["scroll_max"] == sound_scroll_max(0) else s.fail)(
        f"③ 回 ANALOG：slots={st['slots']} scroll_max={st['scroll_max']}")

    # ---------- ⑪ 有序对 SEQ→SOUND 过渡峰值；SOUND↔STEP ×10 往返不泄漏 ----------
    s.cmd("UI step")
    s.expect(r"UI page=step \(2\)", "⑪ UI step")
    s.cmd("UI transport")
    s.expect(r"UI page=transport \(3\)", "⑪ UI transport")
    s.cmd("UI sound")
    m = RE_SOUND_BUILD.search(s.last)
    if m is None:
        raise InjectError(f"SEQ → SOUND 的构建日志缺失：{s.last.strip()[-200:]!r}")
    inc, budget, free, ms = (int(v) for v in m.groups())
    (s.ok if free >= NAV_POOL_FLOOR else s.fail)(
        f"⑪ 有序对 SEQ（STEP + TRANSPORT 全建）→ SOUND：构建日志 池剩余={free}B（≥{NAV_POOL_FLOOR}）")
    (s.ok if 0 < inc <= budget == SOUND_EST and 0 < ms <= NAV_BUILD_BUDGET_MS else s.fail)(
        f"⑪ SOUND 构建日志 池增量={inc}B 预算={budget}B 耗时={ms}ms（期望 0 < 增量 ≤ 8,704、≤100ms）")
    # 12 引擎扫描给 LVGL 文本缓存留下多批新标签；一轮预热不足（首轮实测其后 5 轮才稳），先往返 5 轮。
    for _ in range(5):
        s.cmd("UI step")
        s.cmd("UI sound")
    s.expect(r"UI page=sound \(6\)", "⑪ 预热：UI sound")
    free0 = nav_pool_free(s)
    for _ in range(5):                           # 起点须与下一次预热读数同处噪声带，排除单次 TLSF 碎片离群值
        s.cmd("UI step")
        s.cmd("UI sound")
        free1 = nav_pool_free(s)
        if abs(free1 - free0) <= NAV_LEAK_NOISE:
            free0 = free1
            break
        free0 = free1
    else:
        raise InjectError("SOUND↔STEP 预热 10 轮后池读数仍未稳定（相邻两轮 >128B）")
    per_trip = []
    for trip in range(1, NAV_LEAK_TRIPS + 1):
        s.cmd("UI step")
        s.cmd("UI sound")
        per_trip.append(nav_pool_free(s))
    s.ser.note(f"⑪ pool_free：起点 {free0} → 逐次 {per_trip}（{NAV_LEAK_TRIPS} 次 SOUND↔STEP 往返）")
    (s.ok if abs(free0 - per_trip[-1]) <= NAV_LEAK_NOISE else s.fail)(
        f"⑪ {NAV_LEAK_TRIPS} 次 SOUND↔STEP 往返后 pool_free 变化 {free0} → {per_trip[-1]}"
        f"（Δ={per_trip[-1] - free0:+d}B，允许 |Δ| ≤ {NAV_LEAK_NOISE}B）")
    steps = [free0] + per_trip
    worst = max(steps[i] - steps[i + 1] for i in range(len(steps) - 1))
    (s.ok if worst <= NAV_LEAK_NOISE else s.fail)(f"⑪ 单次往返的最大池下降 {worst}B ≤ {NAV_LEAK_NOISE}B（无逐次增长）")
    st = sound_stats(s)
    (s.ok if st["slots"] == 109 and st["collapsed"] == 0 else s.fail)(
        f"⑪ 往返后重建的仪表盘：slots={st['slots']} collapsed=0x{st['collapsed']:04X}（焦点/折叠位跨重建保留）")



SCENARIOS = {
    "transport": run_transport,
    "params": run_params,
    "step_edit": run_step_edit,
    "tracks": run_tracks,
    "nav": run_nav,
    "store": run_store,
    "song": run_song,
    "sequence": run_sequence,
    "synth": run_synth,
    "sys": run_sys,
    "sound": run_sound,
}


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 固件串口注入序列（AGENTS.md §5.4）")
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS),
                    help="注入场景（十一条 .sh 各对应一个）")
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
            # R23：像素探针假定 MONO。每一轮（含截屏复位后的重跑）入口归一，finally 还原。
            palette_normalise(s)
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
        # R23 闭括号：正常结束 / 断言未过 / 异常终止都还原入口调色板（入口值跨重跑存活；
        # 归一从没发生时不发命令）。还原自身出错也不能漏关串口。
        try:
            palette_restore(s, s.snapshots.get("palette_entry"))
        finally:
            s.ser.close()

    if s.failures:
        sys.exit(f"[FAIL] {args.scenario}: {s.failures} 项断言/抽检未通过（见上）")
    print(f"[PASS] {args.scenario}: 全部断言与像素抽检通过"
          + (f"（截屏复位重试 {s.resets} 次）" if s.resets else ""))


if __name__ == "__main__":
    main()
