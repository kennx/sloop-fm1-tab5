#!/usr/bin/env python3
"""screenshot_matrix.py — 全页面截屏矩阵 + 联系表（spec 2026-11-24 §A4）

矩阵 = 18 页 × 关键状态（默认 / 编辑中 / 播放中 / 鼓轨·引擎·预备·锁定 / SELFTEST 结果行），
定义在 MATRIX 表。每个条目：页、状态名、到达该状态的 console 命令序列、3–5 个像素锚点
（类别名 → 主题色，随 `STATE SNAP set_palette=` 选档，palette 4 = MONO = 当前观感）。

复用（不复制）：`inject.py` 的 `Session`（命令 / 断言 / esptool 复位）与页表、页面几何，
`screenshot.py` 的 `read_frame` / 解码（与 `Session.capture` 同一条读取路径）。

产物（默认 $SLOOP_TAB5_PROJECT/build/verify/matrix-<时间戳>/，--out 覆盖父目录）:
    <page>-<state>.png      每条目一张（1280×720 RGB）
    matrix.csv              页 / 状态 / 置态命令 / 逐锚点读数 / 判定 / PNG / 重试次数
                            （commands 只含到达该状态的命令；条目后的还原动作不在此列）
    sheet-<group>.png       每组一张联系表（4 列 ≤5 行，格标 `<page> <state>`）
    sheet-all.png           每页首个状态，6×3
    matrix.log              串口原始回显（只有 `# [inject] ...` 工具注记，前缀沿用 Session）

锚点（anchor）三种写法:
    (x, y, 类别[, 容差])                       点探针：该像素 ≈ 主题色（默认容差 16）
    (x0, y0, x1, y1, 类别, 最少像素数[, 容差]) 区域探针：框内 ≥N 个像素 ≈ 主题色
    callable(ctx) → 上述元组或 None            ctx = STATE SNAP 的 focus_track / focus_step /
                                              window_start / window_len；None = 不适用（记 n/a）
类别: accent panel panel_hi panel_sel bg text text_dim error track0..track3

幂等（可重复运行，全程不写 flash；SELFTEST 条目例外——chip worker 自检前会落盘一次）:
    前提：`STATE SNAP rec` 必须是 off（走带会录 take）——否则在改动任何状态之前拒绝运行。
    开头记录起点（页 / 选中轨 / 步窗 / palette·theme / autosave 开关 / 四轨引擎·预置 / LIVEHASH）→
    `STORE AUTO OFF` → 停走带 → `UI WINDOW 0 16` + 选轨 0；
    条目内 TAP 改过的参数逐个 `PARAM` 写回，换过引擎的轨 = `ENG TRACK` 回原引擎·预置 + 71 参数
    整份 `PARAM BATCH` 回写（读回核对）。
    **每个条目之后都核对 `LIVEHASH` == 归一起点**（先选回轨 0：song.sel 也是哈希的一部分；无例外）。
    截屏传输失败 ⇒ esptool 复位后整轮重跑；重跑**不重新拍快照**——复位后的 LIVEHASH 必须仍是首轮
    起点，否则按漂移处理（复位会丢掉未落盘的改动）。
    **唯一的收口路径**（正常结束 / 命令失败 / 其它异常 / Ctrl-C 都走 close_bracket）：
      - 正常：还原选轨 / 步窗 / 页 / palette·theme，核对 LIVEHASH == 起点 ⇒ autosave 回到**原来的**开关；
      - 异常：尽力执行登记的还原，再同样核对；相等 ⇒ autosave 照常恢复；
      - **漂移**（某条目后或收尾核对 LIVEHASH ≠ 起点）：不再做任何还原、不碰 flash，
        打印 `ABORT: hash drift … — autosave left OFF; reboot the device to recover from flash` 并非零退出
        ——让 flash 里的 autosave 副本保持最后一次正确的写入，复位即从它恢复；
      - 核对不了（设备无响应）：同样留在 OFF。
    这是对「还原漏项」的闸门（首版曾因空参数向量把一条轨的参数重置成预置默认，靠读 flash 的 autosave
    旧副本才恢复）。`--inject-drift`（隐藏参数，**仅测试用**）在第一条目后不登记还原地改写轨 0 的
    一个参数，用来验证漂移终止路径；跑完须手动 `PARAM 1 <原值>` + `STORE AUTO ON`。

用法:
    python3 scripts/screenshot_matrix.py PORT [--out DIR] [--pages a,b,…] [--retry N]
    python3 scripts/screenshot_matrix.py --selftest       # 离线自测（无设备）
退出码: 0 = 全部 PNG 解码为 1280×720 RGB、命令回显全部匹配、锚点全部命中、每条目后 LIVEHASH 不变、
起点还原；1 = 任一断言/锚点未过、命令失败或哈希漂移；130 = Ctrl-C。
像素锚点只是抽检；**人眼复核联系表是外部门禁**。
依赖: pyserial + Pillow。
"""
import argparse
import csv
import os
import re
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS_DIR))
sys.path.insert(0, str(SCRIPTS_DIR / "inject"))
import screenshot as shot  # noqa: E402
import inject  # noqa: E402

W, H = 1280, 720
TOL = 16                      # 默认容差（与 inject 的探针同口径，容忍 RGB565 位扩展）
TABCAT = 72                   # 页签条高度（页面区起点）
PAGES = {name: pid for pid, name in inject.NAV_ORDER}   # 页名 → 页 id（inject 的扁平页表）
GROUP_PAGES = inject.NAV_GROUP_PAGES                 # 组 → 成员页（与 ui_nav.h 同源）
GROUP_OF = {p: g for g, ps in GROUP_PAGES.items() for p in ps}
PAGE_NAME = dict(inject.NAV_ORDER)                   # 页 id → 页名
PARAMS = 71                   # 上游 P_COUNT（三条合成轨的 p[] 长度）
BATCH_PAIRS = 14              # PARAM BATCH 每命令的对数：esp_console max_cmdline_args=32 含命令名，
                              # 15 对（32 个词）实测被拒（设备 matrix-20261125-112557 的用法行回显）
ENGINE_SAMPLE, ENGINE_FM6 = 4, 9
MATRIX_PNG = "{page}-{state}.png"
CSV_NAME = "matrix.csv"
SETTLE_S = 0.25               # 置态后、SCR 前的稳定等待（≥ 若干个 30ms UI 拍）
ARMED_TIMEOUT_S = 3.0         # TOOLS 预备窗口 ≈1.5s（ui.arm_t=90 拍）的 2 倍
SELFTEST_TIMEOUT_S = 180.0    # 全量自检实测约 157–165s
SHEET_COLS, ALL_COLS = 4, 6
SELFTEST_CASES = 182


def png_name(entry) -> str:
    return MATRIX_PNG.format(page=entry["page"], state=entry["state"])


# ---------------------------------------------------------------- 主题色 ---
# theme_derive.h 的 Python 镜像：内置色阶（palette 0..3 = GREEN/AMBER/CYAN/RED；4 = MONO，
# ui.c 走 ui_theme_derive_mono）→ ui_theme_t，再按帧缓冲 rgb565 位扩展量化
# （screenshot.py 的解码口径）。换 palette 时锚点自动跟随。
PALETTE_RAMP_RGB = (
    ((0, 40, 12), (0, 84, 30), (16, 140, 54), (56, 200, 92), (120, 255, 146)),
    ((60, 26, 0), (110, 50, 0), (170, 82, 0), (225, 120, 8), (255, 166, 40)),
    ((0, 30, 50), (0, 62, 96), (16, 112, 160), (56, 172, 222), (140, 222, 255)),
    ((52, 8, 8), (100, 18, 14), (170, 36, 26), (226, 64, 48), (255, 112, 92)),
    ((40, 40, 40), (80, 80, 80), (130, 130, 130), (186, 186, 186), (226, 226, 226)),
)
MONO = PALETTE_RAMP_RGB[4]


def _tint(base: int, colour, mono) -> tuple:
    # ui_tint_channel：C 整除向零截断
    return tuple(min(255, max(0, ((base >> (16 - 8 * i)) & 0xFF) + int((colour[i] - mono[i]) / 2)))
                 for i in range(3))


def _rgb565(rgb: tuple) -> int:
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _expand(v: int) -> tuple:
    return (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)


def theme_rgb(palette: int) -> dict:
    """ui_theme_t 的 RGB888（ui.c sloop_ui_theme_apply 的分支）。"""
    th = {"bg": (16, 20, 24), "text": (242, 245, 248), "error": (224, 84, 42)}
    for k, rgb in enumerate(((59, 123, 216), (63, 163, 77), (224, 179, 42), (224, 106, 27))):
        th[f"track{k}"] = rgb
    if palette == 4:
        th.update(panel=(27, 32, 38), panel_hi=(36, 43, 51), panel_sel=(44, 53, 64),
                  text_dim=(138, 148, 158), accent=(224, 179, 42))
    else:
        c = PALETTE_RAMP_RGB[palette if palette < 4 else 0]
        sel = tuple((c[1][i] + c[2][i]) // 2 for i in range(3))
        th.update(panel=_tint(0x1B2026, c[0], MONO[0]), panel_hi=_tint(0x242B33, c[1], MONO[1]),
                  panel_sel=_tint(0x2C3540, sel, (105, 105, 105)),
                  text_dim=_tint(0x8A949E, c[2], MONO[2]), accent=c[4])
    return th


def theme_pixels(palette: int) -> dict:
    """主题色经帧缓冲 rgb565 量化后的像素值（锚点的期望值）。"""
    return {k: _expand(_rgb565(v)) for k, v in theme_rgb(palette).items()}


def check_anchor(img, anchor, pixels: dict, ctx: dict):
    """返回 (ok, 摘要)；anchor 解析为 None 时返回 (None, "n/a")。"""
    a = anchor(ctx) if callable(anchor) else anchor
    if a is None:
        return None, "n/a"
    if len(a) >= 6:
        x0, y0, x1, y1, cls, min_count = a[:6]
        tol = a[6] if len(a) > 6 else TOL
        want = pixels[cls]
        n, best = 0, None
        for count, rgb in (img.crop((x0, y0, x1, y1)).getcolors(1 << 20) or []):
            d = max(abs(rgb[i] - want[i]) for i in range(3))
            if d <= tol:
                n += count
            if best is None or d < best[0]:
                best = (d, rgb)
        got = best[1] if best else None
        return n >= min_count, (f"region({x0},{y0},{x1},{y1}) {cls} 命中 {n}（≥{min_count}）"
                                f" 最近 {got} 期望 {want}")
    x, y, cls = a[:3]
    tol = a[3] if len(a) > 3 else TOL
    got = img.getpixel((x, y))
    want = pixels[cls]
    d = max(abs(got[i] - want[i]) for i in range(3))
    return d <= tol, f"({x},{y}) {cls} 实测 {got} 期望 {want}（容差 {tol}）"


# ---------------------------------------------------------------- 锚点几何 ---
# 全部由 inject 的页面几何常量派生（与各页 *_xy / *_probe 同源），不另抄坐标。

def tab_anchors(page: str) -> list:
    """页签两锚：当前页的选中签（多页组 = 子签、单页组 = 组签）= accent；另一张未选中 =
    panel_hi（多页组取兄弟子签，单页组取下一组签）。"""
    gi = inject.UI_GROUP_NAMES.index(GROUP_OF[page])
    members = GROUP_PAGES[GROUP_OF[page]]
    if len(members) > 1:
        me = members.index(page)
        return [(*inject.sub_tab_probe(me), "accent"),
                (*inject.sub_tab_probe((me + 1) % len(members)), "panel_hi", 10)]
    return [(*inject.group_tab_probe(gi), "accent"),
            (*inject.group_tab_probe((gi + 1) % len(inject.UI_GROUP_NAMES)), "panel_hi", 10)]


def chip_x(page: str, c: int) -> int:
    return inject.UI_TAB_X0 + sum(inject.chip_width(n) + 8 for n, _ in inject.CHIP_PAGES[page][:c])


def chip_fill(page: str, c: int, cls: str):
    """chip 行第 c 张的左缘内 6px、文字区之上（inject 的 (x0+6, 79)）。"""
    return (chip_x(page, c) + 6, TABCAT + 7, cls, 24 if cls == "accent" else 10)


def chip_header(page: str):
    """chip 页上下文行（page_params.c：header_x = chip 行右端 − CHIP_GAP + 16）。"""
    x = chip_x(page, len(inject.CHIP_PAGES[page]))
    return (x + 16, TABCAT + 8, x + 256, TABCAT + 36, "text_dim", 20)


def knob_box(y1: int, y2: int, cls: str, min_count: int, tol: int = TOL):
    """旋钮 0 的文本行（knob.c 的 KNOB_*_Y 相对旋钮对象左上角）。"""
    x, y = inject.param_knob_xy(0)
    return (x - 66, y - 80 + y1, x + 66, y - 80 + y2, cls, min_count, tol)


KNOB0_NAME = knob_box(40, 64, "accent", 10)      # 焦点槽的名称 = UI_ACCENT
KNOB0_VALUE = knob_box(64, 94, "text", 30)        # 焦点槽的数值 = UI_TEXT
SOUND_ROW_Y = inject.SOUND_ROW_Y                         # 76：SOUND 页眉两行的屏幕 y
SOUND_ENGINE_NAME = (16 + 130, SOUND_ROW_Y + 4, 16 + 288 - 54, SOUND_ROW_Y + 32, "accent", 12, 24)
SOUND_PRESET_NAME = (320 + 130, SOUND_ROW_Y + 4, 320 + 384 - 54, SOUND_ROW_Y + 32, "accent", 12, 24)
SOUND_HEADER_ERR = (320, SOUND_ROW_Y + 4, 1080, SOUND_ROW_Y + 32, "error", 200)
SAMPLE_HEADER_ERR = (24, SOUND_ROW_Y + 4, 600, SOUND_ROW_Y + 32, "error", 40)
TOOLS_FILL = lambda i: (inject.TOOLS_CHIP[i][0] - 150, inject.TOOLS_CHIP[i][1] - 52)  # noqa: E731
TR_STATUS = (48, TABCAT + 264, 808, TABCAT + 264 + 40)   # TRANSPORT 状态行 label (48,264) 28pt


def _step_focus_frame(ctx: dict):
    """焦点格左描边（UI_TEXT 3px）；焦点步不在窗口内时不可见 ⇒ 不适用。"""
    col = ctx["focus_step"] - ctx["window_start"]
    if not 0 <= col < ctx["window_len"]:
        return None
    return (inject.STEP_GRID_X0 + col * inject.STEP_CELL_PITCH + 1,
            inject.STEP_GRID_Y0 + ctx["focus_track"] * inject.STEP_ROW_PITCH + 52, "text", 10)


def _step_status_row(ctx: dict):
    """STEP 右上状态行文本 = 焦点轨色。"""
    return (*inject.NAV_STATUS_BOX, f"track{ctx['focus_track']}", 30)


def _param_default(page: str) -> list:
    return [chip_fill(page, 0, "accent"), chip_fill(page, 1, "panel_hi"), KNOB0_NAME]


def _param_editing(page: str) -> list:
    return [chip_fill(page, 1, "accent"), chip_fill(page, 0, "panel_hi"), KNOB0_VALUE]


def E(page: str, state: str, setup: list, specific: list, post: tuple = ()) -> dict:
    return {"page": page, "state": state, "setup": list(setup),
            "anchors": tab_anchors(page) + list(specific), "post": list(post)}


def _page_cmd(page: str) -> tuple:
    return (f"UI {page}", rf"UI page={page} \({PAGES[page]}\)")


# ---------------------------------------------------------------- 矩阵定义 ---
# setup / post 的每一项: "console 命令" | ("console 命令", 期望正则) | "@宏"（见 MACROS）。
# 顺序即执行顺序（页按 NAV_ORDER）；SYSTEM 的 selftest 放最后——worker 收尾把 AUTO 打开。
CHIP_PAGES = ("mod", "scale", "voice", "fx", "global")
MATRIX = [
    E("play", "default", [_page_cmd("play")], [
        (940 + 20, TABCAT + 48 + 20, "panel"),                                  # NOTE 闪现框底
        (48, TABCAT + 24, 48 + 180, TABCAT + 54, "track0", 10, 24)]),           # `TRACK n` 标签
    E("tracks", "default", [_page_cmd("tracks")], [
        (24 - 4, 347, "track0"),                                                # 卡 0 选中描边
        (24 + 148, 552, "panel_sel", 8), (24 + inject.TRACKS_CARD_PITCH + 148, 552, "panel", 8)]),
    E("sound", "default", ["@track0", _page_cmd("sound")], [
        (16 + 3, SOUND_ROW_Y + 4, "panel_hi", 10), SOUND_ENGINE_NAME, SOUND_PRESET_NAME]),
    E("sound", "editing", ["@track0", _page_cmd("sound"), "@edit0"], [
        SOUND_ENGINE_NAME, KNOB0_VALUE]),
    E("sound", "drum", ["@track3", _page_cmd("sound")], [
        SOUND_HEADER_ERR, (320 + 3, SOUND_ROW_Y + 4, "bg", 10), (16 + 3, SOUND_ROW_Y + 4, "panel_hi", 10)]),
    E("sound", "fm6", ["@fm6", _page_cmd("sound")], [
        SOUND_ENGINE_NAME, (*inject.bank_btn_probe(0), "track1"), (*inject.bank_btn_probe(2), "track3")]),
    E("mod", "default", ["@track0", _page_cmd("mod"), ("UI MOD CHIP 0", r"UI mod chip=0")], _param_default("mod")),
    E("mod", "editing", ["@track0", _page_cmd("mod"), "@edit0"], _param_editing("mod")),
    E("mod", "locked", ["@track3", _page_cmd("mod"), ("UI MOD CHIP 0", r"UI mod chip=0"),
                        ("UI MOD CHIP", r"UI mod slot=0 id=\d+ [^\n]*locked=1")],
      [chip_fill("mod", 0, "accent"), chip_fill("mod", 1, "panel_hi"), chip_header("mod")]),
    # 锁定态本身由置态命令的 `locked=1` 回显断言：锁定槽的 `--` 与无指示弧在颜色上与「值为最小的
    # 可编辑槽」不可区分（实测 mod-locked 与 mod-default 的旋钮环像素分布相同），不设像素锚点。
    E("scale", "default", ["@track0", _page_cmd("scale"), ("UI SCALE CHIP 0", r"UI scale chip=0")],
      _param_default("scale")),
    E("scale", "editing", ["@track0", _page_cmd("scale"), "@edit0"], _param_editing("scale")),
    E("voice", "default", ["@track0", _page_cmd("voice"), ("UI VOICE CHIP 0", r"UI voice chip=0")],
      _param_default("voice")),
    E("voice", "editing", ["@track0", _page_cmd("voice"), "@edit0"], _param_editing("voice")),
    E("global", "default", ["@track0", _page_cmd("global"), ("UI GLOBAL CHIP 0", r"UI global chip=0")],
      _param_default("global")),
    E("global", "editing", ["@track0", _page_cmd("global"), "@edit0"], _param_editing("global")),
    E("fx", "default", ["@track0", _page_cmd("fx"), ("UI FX CHIP 0", r"UI fx chip=0")], _param_default("fx")),
    E("fx", "editing", ["@track0", _page_cmd("fx"), "@edit0"], _param_editing("fx")),
    E("tools", "default", [_page_cmd("tools")], [
        (*TOOLS_FILL(0), "panel_hi"), (*inject.TOOLS_STATUS_BOX, "text_dim", 40)]),
    E("tools", "armed", [_page_cmd("tools"), "@armed"], [
        (*TOOLS_FILL(0), "accent"), (*TOOLS_FILL(1), "panel_hi"), (*inject.TOOLS_STATUS_BOX, "accent", 40)],
       post=("@armed_timeout",)),           # 截屏后等窗口过期，再点一次证明「未执行」
    E("sample", "default", ["@track0", _page_cmd("sample")], [
        SAMPLE_HEADER_ERR, (482, 454, "bg", 12)]),                              # 空槽 5 不建旋钮
    E("sample", "usr", ["@sample"], [
        (*inject.SAMPLE_SLOT_BOX, "accent", 30, 24), (*inject.SAMPLE_STATUS_BOX, "track1", 30, 24)]),
    E("dsyn", "default", [_page_cmd("dsyn")], [
        (*inject.dsyn_kit_chip_probe(0), "accent"), (*inject.dsyn_page_chip_probe(0), "accent")]),
    E("fm6p", "default", ["@track0", _page_cmd("fm6p")], [(*inject.FM6P_STATUS_BOX, "error", 40)]),
    E("fm6p", "engine", ["@fm6", _page_cmd("fm6p")], [
        (*inject.fm6p_op_chip_probe(0), "accent"), (*inject.fm6p_page_chip_probe(0), "accent"), KNOB0_VALUE]),
    E("step", "default", ["@track0", _page_cmd("step")], [_step_focus_frame, _step_status_row]),
    E("step", "playing", ["@track0", _page_cmd("step"), "@play", _page_cmd("step"), "@playing"],
      [_step_focus_frame, _step_status_row], post=("@stop",)),
    E("transport", "default", [_page_cmd("transport")], [
        (inject.TRANSPORT_PLAY[0], inject.TRANSPORT_PLAY[1], "track1"),
        (inject.TRANSPORT_STOP[0], inject.TRANSPORT_STOP[1], "track3"), (*TR_STATUS, "text_dim", 30)]),
    E("transport", "playing", ["@play", "@playing"], [
        (inject.TRANSPORT_PLAY[0], inject.TRANSPORT_PLAY[1], "track1"),
        (inject.TRANSPORT_STOP[0], inject.TRANSPORT_STOP[1], "track3"), (*TR_STATUS, "text", 30)],
       post=("@stop",)),
    E("song", "default", [_page_cmd("song")], [
        (24 + 20, TABCAT + inject.SONG_CARD_Y + 12, "panel_sel", 14),          # 卡 A 选中底
        (24 + 20, TABCAT + inject.SONG_SEC_Y + 6, "panel_hi", 14),             # STORE chip
        (24, TABCAT + 20, 1000, TABCAT + 80, "text_dim", 40)]),
    E("projects", "default", [_page_cmd("projects")], [
        (inject.PROJ_CARD_X0 + 40, TABCAT + inject.PROJ_CARD_Y + 200, "panel", 8),
        (*inject.proj_btn_probe(0, 0), "track1"), (*inject.FILE_STATUS_BOX, "text_dim", 20)]),
    E("presets", "default", [_page_cmd("presets")], [
        (*inject.pre_btn_probe(0), "accent"), (*inject.pre_btn_probe(1), "track1"),
        (*inject.pre_cell_xy(0), "panel_sel", 8),                                  # 初始选中槽 0
        (*inject.pre_cell_xy(1), "panel", 8)]),
    E("system", "default", [_page_cmd("system")], [
        (24 + 420 - 20, TABCAT + inject.SYS_ROW_Y[0] + 4, "panel"),           # COLOR 行底（右端）
        (304 + 20, TABCAT + inject.SYS_ROW_Y[1] + 4, "panel_hi"),             # ZOOM chip 底
        (24 + 140 - 20, TABCAT + inject.SYS_ROW_Y[8] + 4, "panel")]),         # SELFTEST chip 底
    E("system", "selftest", ["@selftest"], [   # 收尾把 AUTO 打开：post 里关回去，括号保持闭合
        (24, 672, 700, 712, "accent", 40),                                     # PASS 结果行
        (24 + 140 - 20, TABCAT + inject.SYS_ROW_Y[8] + 4, "panel")], post=("@auto_off",)),
]


# ---------------------------------------------------------------- 联系表 ---

def _draw_text2(sheet, xy, text: str, fill=(255, 255, 255)) -> None:
    """Pillow 默认点阵字体放大 ×2（NEAREST，任意 Pillow 版本可用）。"""
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.load_default()
    x0, y0, x1, y1 = font.getbbox(text)
    mask = Image.new("L", (max(1, x1 - x0), max(1, y1 - y0)), 0)
    ImageDraw.Draw(mask).text((-x0, -y0), text, font=font, fill=255)
    mask = mask.resize((mask.width * 2, mask.height * 2), Image.NEAREST)
    sheet.paste(fill, (xy[0], xy[1]), mask)


SHEET_LABEL_H, SHEET_GAP, SHEET_PAD = 24, 8, 8


def sheet_size(n: int, cols: int, thumb_w: int) -> tuple:
    """联系表的像素尺寸：每格 = 缩略图（1280×720 等比）+ 格标行。"""
    rows = -(-n // cols)
    return (2 * SHEET_PAD + cols * (thumb_w + SHEET_GAP) - SHEET_GAP,
            2 * SHEET_PAD + rows * (thumb_w * H // W + SHEET_LABEL_H + SHEET_GAP) - SHEET_GAP)


def build_sheet(entries: list, img_dir: Path, out_path: Path, cols: int, thumb_w: int) -> None:
    """把 entries 的 PNG 缩略拼成一张联系表；每格上方标 `<page> <state>`。"""
    from PIL import Image
    inject.require_deps()
    rows = -(-len(entries) // cols)
    thumb_h = thumb_w * H // W
    label_h, gap, pad = SHEET_LABEL_H, SHEET_GAP, SHEET_PAD
    sheet = Image.new("RGB", sheet_size(len(entries), cols, thumb_w), (0, 0, 0))
    for i, e in enumerate(entries):
        r, c = divmod(i, cols)
        x, y = pad + c * (thumb_w + gap), pad + r * (thumb_h + label_h + gap)
        with Image.open(img_dir / png_name(e)) as src:
            img = src.convert("RGB")
            if img.size != (W, H):
                raise ValueError(f"{png_name(e)} 尺寸 {img.size} ≠ {W}×{H}")
            sheet.paste(img.resize((thumb_w, thumb_h), Image.LANCZOS), (x, y + label_h))
        _draw_text2(sheet, (x, y + 4), f"{e['page']} {e['state']}")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)


def make_sheets(entries: list, img_dir: Path, out_dir: Path) -> list:
    outs = []
    for group in GROUP_PAGES:
        items = group_entries_of(entries, group)
        if items:
            p = out_dir / f"sheet-{group.lower()}.png"
            build_sheet(items, img_dir, p, SHEET_COLS, 320)
            outs.append(p)
    firsts = first_entries_of(entries)
    p = out_dir / "sheet-all.png"
    build_sheet(firsts, img_dir, p, ALL_COLS, 208)
    return outs + [p]


def group_entries_of(entries: list, group: str) -> list:
    return [e for e in entries if GROUP_OF[e["page"]] == group]


def group_entries(group: str) -> list:
    return group_entries_of(MATRIX, group)


def first_entries_of(entries: list) -> list:
    """entries 中每页的首个条目，页顺序按 NAV_ORDER。"""
    out = []
    for _, name in inject.NAV_ORDER:
        hit = [e for e in entries if e["page"] == name]
        if hit:
            out.append(hit[0])
    return out



# ---------------------------------------------------------------- 运行器 ---

SAMPLE_CTX = {"focus_track": 0, "focus_step": 5, "window_start": 0, "window_len": 16}


def _ints(text: str, start: str) -> dict:
    """以 start 开头的那一行里的 `key=整数` 字段。"""
    for line in text.splitlines():
        if line.startswith(start):
            return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)\b", line)}
    raise inject.InjectError(f"回显缺少 `{start}` 行：{text.strip()[:200]!r}")


class Restart(Exception):
    """截屏传输失败已复位设备——矩阵从头重跑（复位后页面/走带状态已回基线）。"""


class HashDrift(Exception):
    """工程哈希（LIVEHASH）离开了起点且原因未知：工作工程已被改写——立即终止，不再叠加，
    也不再做任何还原（含还挂着的还原动作：可能把事情弄得更糟），autosave 保持 OFF
    （见 close_bracket；复位重跑时同样先清空还原动作）。"""


class Refuse(Exception):
    """运行前提不满足（如录音未关）：在改动任何状态之前拒绝运行。"""


class Runner:
    def __init__(self, s: inject.Session, out_dir: Path, inject_drift: bool = False):
        self.s = s
        self.out_dir = out_dir
        self.inject_drift = inject_drift   # 测试钩子（--inject-drift）
        self.rec = "?"          # 最近一次 STATE SNAP 的 rec 状态
        self.trace = []        # 当前条目实际发出的命令（CSV 的 commands 列）
        self.undo = []         # 条目内还原动作（LIFO，截屏后执行）
        self.state = {}        # 最近一次 STATE SNAP
        self.snapshot = {}     # 起点快照（bracket_begin）
        self.rows = []
        self.retries = 0
        self.hash0 = 0

    def run(self, line: str, pattern: str = None, timeout: float = None) -> None:
        self.trace.append(line)
        self.s.cmd(line, timeout=timeout)
        if pattern is not None:
            self.s.expect(pattern, line)

    def snap(self) -> None:
        """只读 STATE SNAP（不进 commands 列）。"""
        self.s.cmd("STATE SNAP")
        self.state = _ints(self.s.last, "STATE SNAP page=")
        m = re.search(r"\brec=(\w+)", self.s.last)
        self.rec = m.group(1) if m else "?"

    def step(self, item) -> None:
        if isinstance(item, str) and item.startswith("@"):
            MACROS[item[1:]](self)
        elif isinstance(item, tuple):
            self.run(item[0], item[1])
        else:
            self.run(item)


def select_track(r: Runner, track: int) -> None:
    inject.select_track(r.s, track)
    r.trace.append(f"select_track({track})")


def read_vector(r: Runner, track: int) -> dict:
    """轨 track 的 71 个参数真值（`PARAM <id>` 只读，先选中该轨）；该轨上无描述符的 id 跳过。
    读不到任何一项即报错：空向量会让随后的还原变成「只换回引擎、参数留在预置默认」。"""
    s = r.s
    select_track(r, track)
    out = {}
    r.trace.append(f"PARAM 0..{PARAMS - 1}（读轨 {track} 的参数向量，供条目后还原）")
    for pid in range(PARAMS):
        s.cmd(f"PARAM {pid}")
        m = re.search(rf"PARAM track={track} id={pid} value=(-?\d+)", s.last)
        if m:
            out[pid] = int(m.group(1))
    if len(out) != PARAMS:
        raise inject.InjectError(f"轨 {track} 的参数向量只读到 {len(out)}/{PARAMS} 项，拒绝换引擎")
    return out


def restore_engine(r: Runner, track: int, engine: int, preset: int, vec: dict) -> None:
    """`ENG TRACK` 回原引擎·预置，再把 71 参数整份写回（BATCH 的 applied 是读回值）。"""
    s = r.s
    select_track(r, track)
    r.run(f"ENG TRACK {track} {engine} {preset}",
          rf"ENG TRACK k={track} engine={engine} preset={preset} OK")
    ids = sorted(vec)
    for i in range(0, len(ids), BATCH_PAIRS):
        chunk = ids[i:i + BATCH_PAIRS]
        r.run("PARAM BATCH " + " ".join(f"{pid} {vec[pid]}" for pid in chunk))
        got = {int(a): int(b) for a, b in
               re.findall(r" id=(\d+) value=-?\d+ applied=(-?\d+)", s.last)}
        bad = [pid for pid in chunk if got.get(pid) != vec[pid]]
        (s.ok if not bad else s.fail)(
            f"轨 {track} 参数向量回写 {len(chunk) - len(bad)}/{len(chunk)}" + (f"；不符 {bad}" if bad else ""))


def macro_track0(r: Runner) -> None:
    select_track(r, 0)


def macro_track3(r: Runner) -> None:
    select_track(r, 3)


def ensure_engine(r: Runner, engine: int) -> int:
    """返回一条引擎 == engine 的合成轨并选中它；没有就换轨 0..2 中第一条非 FM6 轨
    （原轨若是有 live patch 编辑的 FM6，换引擎会丢编辑）并登记整份还原。"""
    s = r.s
    for k in range(3):
        if r.snapshot["eng"][k][0] == engine:
            track, switch = k, False
            break
    else:
        cand = [k for k in range(3) if r.snapshot["eng"][k][0] != ENGINE_FM6]
        if not cand:
            raise inject.InjectError("三条合成轨都是 FM6：换引擎会丢 live patch 编辑，拒绝执行")
        track, switch = cand[0], True
    if switch:
        vec = read_vector(r, track)
        old = r.snapshot["eng"][track]
        r.run(f"ENG TRACK {track} {engine} 0", rf"ENG TRACK k={track} engine={engine} preset=0 OK")
        r.undo.append(lambda rr: restore_engine(rr, track, old[0], old[1], vec))
    select_track(r, track)
    r.trace.append(f"ensure_engine({engine}) → track {track}{'（换引擎，条目后还原）' if switch else ''}")


def macro_fm6(r: Runner) -> None:
    ensure_engine(r, ENGINE_FM6)


def macro_sample(r: Runner) -> None:
    """SAMPLE 引擎轨 + TAP U1（写该轨的 SET 宏）。命中设备自己的 SAMPLE 轨时没有换引擎，
    但 SET 仍被改写 ⇒ 读原值并登记还原。"""
    s = r.s
    ensure_engine(r, ENGINE_SAMPLE)
    r.run("UI sample", r"UI page=sample \(8\)")
    r.run("UI SAMPLE CHIP")
    m = re.search(r"UI sample slot=0 id=(\d+) ", s.last)
    if m is None:
        raise inject.InjectError(f"UI SAMPLE CHIP 回显缺少 slot=0：{s.last.strip()[:200]!r}")
    pid = int(m.group(1))
    r.trace.append(f"PARAM {pid}")
    s.cmd(f"PARAM {pid}")
    m = re.search(r"value=(-?\d+)", s.last)
    if m is None:
        raise inject.InjectError(f"PARAM {pid} 回显缺少 value：{s.last.strip()[:200]!r}")
    value = int(m.group(1))
    r.run("TAP %d %d" % inject.sample_slot_xy(0), r"-> SAMPLE slot=0 select")
    r.undo.append(lambda rr: rr.run(f"PARAM {pid} {value}", rf"applied={value} "))


def macro_edit0(r: Runner) -> None:
    """编辑中：chip 页选第 2 张 chip；TAP 旋钮 0 设焦点，再用页眉 `+`/`−` 把焦点槽写成一个
    非默认值；原值登记还原（条目后 `PARAM` 写回）、chip 选回 0。"""
    s = r.s
    page = r.entry["page"]
    if page in inject.CHIP_PAGES:
        r.run("TAP %d %d" % inject.chip_center(page, 1), rf"-> PARAMS {page} chip=1 select")
    r.run(f"UI {page.upper()} CHIP")
    m = re.search(rf"UI {page} slot=0 id=(\d+) ", s.last)
    if m is None:
        raise inject.InjectError(f"UI {page.upper()} CHIP 回显缺少 slot=0：{s.last.strip()[:200]!r}")
    pid = int(m.group(1))
    read = inject.chip_param_cmd(page, pid, 0)
    r.trace.append(read)
    s.cmd(read)
    m = re.search(r"value=(-?\d+) min=(-?\d+) max=(-?\d+)", s.last)
    if m is None:
        raise inject.InjectError(f"{read} 回显缺少 value/min/max：{s.last.strip()[:200]!r}")
    value, lo, hi = (int(x) for x in m.groups())
    r.run("TAP %d %d" % inject.param_knob_xy(0), r"-> PARAM slot=0 focus")
    if value < hi:
        xy, sign, new = inject.PARAM_BTN_PLUS, r"\+1", value + 1
    elif value > lo:
        xy, sign, new = inject.PARAM_BTN_MINUS, r"-1", value - 1
    else:
        xy = None
    if xy is not None:
        r.run("TAP %d %d" % xy, rf"-> PARAM slot=0 id={pid} delta={sign} value={new}")
        r.undo.append(lambda rr: rr.run(f"{read} {value}", rf"applied={value} "))
    if page in inject.CHIP_PAGES:
        r.undo.append(lambda rr: rr.run(f"UI {page.upper()} CHIP 0", rf"UI {page} chip=0"))


def macro_play(r: Runner) -> None:
    r.run("UI transport", r"UI page=transport \(3\)")
    r.run("TAP %d %d" % inject.TRANSPORT_PLAY, r"-> TRANSPORT CMD_PLAY")
    inject.wait_playing(r.s, 1, "起播")


def macro_playing(r: Runner) -> None:
    r.snap()
    (r.s.ok if r.state.get("playing") == 1 else r.s.fail)(f"截屏前 playing={r.state.get('playing')}（期望 1）")


def macro_stop(r: Runner) -> None:
    inject.ensure_stopped(r.s)
    r.trace.append("ensure_stopped（UI transport + TAP STOP）")


def macro_armed(r: Runner) -> None:
    r.trace.append("STORE LIVEHASH")
    h0 = inject.live_hash(r.s)
    r.trace.append("TAP %d %d" % inject.TOOLS_CHIP[0])
    armed, st = inject.tools_tap(r.s, 0)
    (r.s.ok if armed is True and st == "AGAIN TO CONFIRM" else r.s.fail)(
        f"TOOLS CLRSEQ 只预备：armed={armed} status={st!r}")
    r.armed_hash = h0


def macro_armed_timeout(r: Runner) -> None:
    """截屏（17s 级传输）早已越过 1.5s 预备窗口；这里再等一拍，然后**再点一次 CLRSEQ**：
    仍回 `armed` 才证明上一次按下已过期（窗口未过期时第二次点同一张会执行），最后以
    LIVEHASH 不变收口。等待取窗口的 2 倍：UI 拍若被拖慢，过早的第二次按下会真的清轨。"""
    time.sleep(ARMED_TIMEOUT_S)
    h1 = inject.live_hash(r.s)
    (r.s.ok if h1 == r.armed_hash else r.s.fail)(
        f"预备超时未执行：LIVEHASH {h1:08X} == {r.armed_hash:08X}")
    armed, st = inject.tools_tap(r.s, 0)
    (r.s.ok if armed is True and st == "AGAIN TO CONFIRM" else r.s.fail)(
        f"再次点 CLRSEQ 仍是预备（上一次已过期）：armed={armed} status={st!r}")
    time.sleep(ARMED_TIMEOUT_S)
    h2 = inject.live_hash(r.s)
    (r.s.ok if h2 == r.armed_hash else r.s.fail)(
        f"第二次预备也已过期：LIVEHASH {h2:08X} == {r.armed_hash:08X}")


def macro_auto_off(r: Runner) -> None:
    r.run("STORE AUTO OFF", r"STORE AUTO off")


def macro_selftest(r: Runner) -> None:
    """SYSTEM 页的 SELFTEST chip（plan Task 8 Step 1 的「SELFTEST PASS 结果行」）：本 boot 已有
    结果则直接截缓存行；否则点 chip 走 worker 全路径（RESET FIRST 时先 `SYS SELFTEST ARM`）。"""
    s = r.s
    r.run("UI system", r"UI page=system \(14\)")
    r.snap()
    if r.state.get("selftest_done") != 1:
        inject.sys_tap(s, 8)
        r.trace.append("TAP <SELFTEST chip>")
        st = re.search(r'-> SYS SELFTEST status="([^"]*)"', s.last)
        if st and st.group(1) == "RESET FIRST":
            r.run("SYS SELFTEST ARM", "SYS SELFTEST ARMED")
            inject.sys_tap(s, 8)
            r.trace.append("TAP <SELFTEST chip>")
            st = re.search(r'-> SYS SELFTEST status="([^"]*)"', s.last)
        status = st.group(1) if st else None
        (s.ok if status == "SELFTEST RUNNING" else s.fail)(
            f"SELFTEST chip 起跑：status={status!r}")
        deadline = time.monotonic() + SELFTEST_TIMEOUT_S
        while time.monotonic() < deadline:
            r.snap()
            if r.state.get("selftest_done") == 1:
                break
            time.sleep(0.5)
        else:
            s.fail(f"SELFTEST chip 超时（{SELFTEST_TIMEOUT_S:.0f}s 内无结果）")
    r.snap()
    ran, failed = r.state.get("selftest_ran", -1), r.state.get("selftest_failed", -1)
    (s.ok if ran == SELFTEST_CASES and failed == 0 else s.fail)(
        f"SYSTEM 状态行数据 SELFTEST {ran - failed}/{ran}")
    r.run("ENG AUDIO", r"ENG AUDIO streaming=1")


MACROS = {
    "track0": macro_track0, "track3": macro_track3, "fm6": macro_fm6, "sample": macro_sample,
    "edit0": macro_edit0, "play": macro_play, "playing": macro_playing, "stop": macro_stop,
    "armed": macro_armed, "armed_timeout": macro_armed_timeout, "selftest": macro_selftest,
    "auto_off": macro_auto_off,
}


def capture(r: Runner, entry: dict):
    """SCR → PNG（screenshot.py 的读取/解码路径）；传输失败复位后整轮重跑。"""
    s = r.s
    out = r.out_dir / png_name(entry)
    time.sleep(SETTLE_S)
    s.ser.note(f"SCR 截屏 -> {out.name}")
    s.ser.write(b"SCR\n")
    s.ser.flush()
    try:
        w, h, fmt, data = shot.read_frame(s.ser, 30.0, lambda on: setattr(s.ser, "silent", on))
    except shot.FirmwareError as e:
        raise inject.InjectError(f"截屏 {out.name} 失败: {e}") from None
    except shot.TransferError as e:
        if r.retries >= s.retries:
            fail_path = shot.save_fail_evidence(str(out), e.partial, e, r.retries + 1)
            s.ser.note(f"截屏 {out.name} 失败（重试耗尽）；证据 {fail_path.name}")
            s.ser.close()
            shot.best_effort_reset(s.port, inject.BAUD)
            raise inject.InjectError(f"截屏 {out.name} 失败（已复位重试 {r.retries} 次）: {e}\n"
                                     f"证据: {fail_path}") from None
        r.retries += 1
        s.ser.note(f"截屏 {out.name} 传输失败（{e}）；esptool 复位后矩阵重跑"
                   f"（第 {r.retries}/{s.retries} 次重试）")
        s.reboot()
        raise Restart(f"截屏 {out.name} 传输失败，设备已复位") from None
    img = shot.rgb565_to_rgb(data, w, h) if fmt == "rgb565" else shot.rgb888_to_rgb(data, w, h)
    img.save(out)
    print(f"  [SHOT] {out}  ({w}x{h} {fmt})")
    return img, out


def run_entry(r: Runner, entry: dict) -> None:
    s = r.s
    r.entry, r.trace, r.undo = entry, [], []
    s.ser.note(f"条目 {entry['page']}-{entry['state']}")
    for item in entry["setup"]:
        r.step(item)
    r.snap()
    ctx = {k: r.state.get(k, 0) for k in SAMPLE_CTX}
    (s.ok if r.state.get("page") == PAGES[entry["page"]] else s.fail)(
        f"{entry['page']}-{entry['state']}: 落地页 page={r.state.get('page')}（期望 {PAGES[entry['page']]}）")
    img, out = capture(r, entry)
    decoded = img.size == (W, H) and img.mode == "RGB"
    (s.ok if decoded else s.fail)(
        f"{out.name} 解码 size={img.size} mode={img.mode}（期望 1280×720 RGB）")
    pixels = theme_pixels(r.state.get("set_palette", 4))
    checks = []
    for a in entry["anchors"]:
        ok, msg = check_anchor(img, a, pixels, ctx)
        checks.append((ok, msg))
        if ok is None:
            print(f"  [n/a] {out.name}: {msg}")
        else:
            (s.ok if ok else s.fail)(f"{out.name}: {msg}")
    commands = " | ".join(r.trace)          # 只记到达状态的命令；post 与还原动作不进 CSV
    for item in entry["post"]:
        r.step(item)
    for undo in reversed(r.undo):
        undo(r)
    r.undo = []
    r.snap()
    if r.state.get("selected_track") != 0:          # song.sel 是哈希的一部分：先回到归一选轨
        select_track(r, 0)
    h1 = inject.live_hash(s)
    if h1 != r.hash0:
        s.fail(f"{out.name}: 条目后 LIVEHASH {h1:08X} ≠ 归一起点 {r.hash0:08X}")
        raise HashDrift(f"{entry['page']}-{entry['state']} 条目后 LIVEHASH {h1:08X} ≠ {r.hash0:08X}")
    s.ok(f"{out.name}: 条目后 LIVEHASH {h1:08X} == 归一起点 {r.hash0:08X}")
    if r.inject_drift and not r.rows:            # 第一条目之后、第二条目之前
        inject_drift(r)
    row = {"page": entry["page"], "state": entry["state"], "commands": commands}
    for i, (ok, msg) in enumerate(checks):
        row[f"a{i + 1}_ok"] = {True: "pass", False: "FAIL", None: "n/a"}[ok]
        row[f"a{i + 1}_reading"] = msg
    row["anchors"] = len(checks)
    row["png"] = out.name
    row["retries"] = r.retries
    row["verdict"] = "PASS" if decoded and all(c[0] is not False for c in checks) else "FAIL"
    r.rows.append(row)


def inject_drift(r: Runner) -> None:
    """测试钩子（`--inject-drift`，仅用于验证漂移终止路径）：第一条目通过之后，不登记还原地
    改写轨 0 的一个参数 ⇒ 下一条目的哈希核对必须判漂移并终止。"""
    s = r.s
    s.cmd("PARAM 1")
    m = re.search(r"value=(-?\d+) min=(-?\d+) max=(-?\d+)", s.last)
    if m is None:
        raise inject.InjectError(f"--inject-drift: PARAM 1 回显缺少 value/min/max：{s.last.strip()[:200]!r}")
    old, lo, hi = (int(x) for x in m.groups())
    new = old + 1 if old < hi else old - 1
    s.cmd(f"PARAM 1 {new}")
    s.ser.note(f"[inject-drift] 测试注入：轨 0 PARAM 1 {old} → {new}（未登记还原；手动恢复 = 选轨 0 后 PARAM 1 {old}）")
    print(f"  [inject-drift] 测试注入：轨 0 PARAM 1 {old} → {new}（下一条目应判漂移并终止；"
          f"手动恢复 = 选轨 0 后 `PARAM 1 {old}` + `STORE AUTO ON`）")


def _engines(s: inject.Session) -> dict:
    s.cmd("ENG SNAP")
    return {int(k): (int(e), int(p)) for k, e, p in
            re.findall(r"ENG SNAP trk k=(\d) engine=(\d+) preset=(\d+)", s.last)}


def bracket_begin(r: Runner) -> None:
    """进入归一括号。首轮：先记起点（什么都不改之前），再 AUTO OFF / 停走带 / 归一；
    复位重跑（Restart）之后：**不重新拍快照**——设备已从 flash 恢复，工程哈希必须仍是首轮
    起点，否则按漂移处理（复位会丢掉未落盘的改动，或复位前已有改动）。"""
    s = r.s
    if r.snapshot:
        h = inject.live_hash(s)
        if h != r.snapshot["hash"]:
            raise HashDrift(f"复位后 LIVEHASH {h:08X} ≠ 首轮起点 {r.snapshot['hash']:08X}"
                            f"（复位丢失了未落盘的状态，或复位前已有改动）")
        r.run("STORE AUTO OFF", r"STORE AUTO off")      # 开机默认 on
    else:
        r.snap()
        if r.rec != "off":
            raise Refuse(f"STATE SNAP rec={r.rec}（期望 off）：走带会录下一轨 take，拒绝运行；先停录音再试")
        s.cmd("STORE STATS")
        m = re.search(r"autosave=(\d)", s.last)
        if m is None:
            raise inject.InjectError(f"STORE STATS 回显缺少 autosave=：{s.last.strip()[:200]!r}")
        r.snapshot = {"page": r.state["page"], "track": r.state["selected_track"],
                      "window": (r.state["window_start"], r.state["window_len"]),
                      "palette": r.state["set_palette"], "theme_on": r.state["set_theme"],
                      "autosave": int(m.group(1))}
        r.snapshot["eng"] = _engines(s)
        r.snapshot["hash"] = inject.live_hash(s)
        r.run("STORE AUTO OFF", r"STORE AUTO off")
        s.ser.note(f"起点：page={PAGE_NAME[r.snapshot['page']]} track={r.snapshot['track']} "
                   f"window={r.snapshot['window']} palette={r.snapshot['palette']} "
                   f"theme_on={r.snapshot['theme_on']} autosave={r.snapshot['autosave']} "
                   f"eng={r.snapshot['eng']} hash={r.snapshot['hash']:08X}")
    inject.ensure_stopped(s)
    snap = r.snapshot
    if snap["theme_on"] != 0:
        set_setting(r, "theme_on", 0)
    if snap["palette"] != 4:
        set_setting(r, "palette", 4)
    inject.ensure_window(s, 0, 16)
    select_track(r, 0)
    h0 = inject.live_hash(s)                 # 归一（选轨 0）后的基线：逐条目核对的参照
    if r.hash0 and h0 != r.hash0:
        raise HashDrift(f"复位并归一后 LIVEHASH {h0:08X} ≠ 首轮归一起点 {r.hash0:08X}")
    r.hash0 = h0


def set_setting(r: Runner, field: str, value: int) -> None:
    r.run(f"STORE SETTINGS SET {field} {value}", rf"STORE SETTINGS SET {field}={value} dirty=")
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        r.snap()
        if r.state.get(f"set_{field}") == value:
            return
        time.sleep(0.05)
    r.s.fail(f"settings {field} 未生效（set_{field}={r.state.get(f'set_{field}')}，期望 {value}）")


DRIFT_MSG = "ABORT: hash drift ({why}) — autosave left OFF; reboot the device to recover from flash"
UNVERIFIED_MSG = ("ABORT: cannot verify the working project ({why}) — autosave left OFF; "
                  "reboot the device to recover from flash")


def close_bracket(r: Runner, drift: str | None, error: str | None) -> tuple:
    """**唯一的收口路径**：正常结束、InjectError、其它异常、Ctrl-C 都到这里。返回 (是否已核对还原, 结论行)。

    已知漂移 ⇒ 不做任何还原（含还挂着的还原动作：可能把事情弄得更糟）、不碰 flash，
    autosave 保持 OFF；否则：
    1. 还挂着的还原动作（条目中途异常时）尽力执行；
    2. 还原选轨 / 步窗 / 页 / palette·theme，再以 LIVEHASH 对照起点：
       相等 ⇒ autosave 回到**原来的**开关状态（不一定是 on）；不等 ⇒ 按漂移处理；
       核对不了（设备无响应）⇒ 同样留在 OFF。"""
    s = r.s
    if not r.snapshot:
        return True, "未进入归一括号（运行前提不满足或设备未就绪）：没有改动任何状态，autosave 未动"
    snap = r.snapshot
    s.ser.note(f"收尾：drift={drift!r} error={error!r}")
    unverified = None
    if drift is None:
        while r.undo:
            try:
                r.undo.pop()(r)
            except inject.InjectError as e:
                s.fail(f"还原动作失败: {e}")
        try:
            inject.ensure_stopped(s)
            if snap["palette"] != 4:
                set_setting(r, "palette", snap["palette"])
            if snap["theme_on"] != 0:
                set_setting(r, "theme_on", snap["theme_on"])
            select_track(r, snap["track"])
            inject.ensure_window(s, *snap["window"])
            r.run(f"UI {PAGE_NAME[snap['page']]}", rf"UI page={PAGE_NAME[snap['page']]} \({snap['page']}\)")
            eng = _engines(s)
            (s.ok if eng == snap["eng"] else s.fail)(f"四轨引擎·预置回到起点：{eng}（期望 {snap['eng']}）")
            r.snap()
            got = (r.state.get("selected_track"), (r.state.get("window_start"), r.state.get("window_len")),
                   r.state.get("page"), r.state.get("set_palette"), r.state.get("set_theme"))
            want = (snap["track"], snap["window"], snap["page"], snap["palette"], snap["theme_on"])
            (s.ok if got == want else s.fail)(f"选轨/步窗/页/palette/theme 回到起点：{got}（期望 {want}）")
            h1 = inject.live_hash(s)
            (s.ok if h1 == snap["hash"] else s.fail)(
                f"LIVEHASH {h1:08X} == 起点 {snap['hash']:08X}")
            if h1 != snap["hash"]:
                drift = f"收尾核对：LIVEHASH {h1:08X} ≠ 起点 {snap['hash']:08X}"
        except inject.InjectError as e:
            unverified = f"{e}"
            if error is None:
                error = f"收尾还原/核对未完成：{e}"
    if drift is not None or unverified is not None:
        try:
            s.cmd("STORE AUTO OFF")           # 不写 flash
        except (inject.InjectError, OSError):
            pass
        msg = (DRIFT_MSG if drift is not None else UNVERIFIED_MSG).format(why=drift or unverified)
        s.ser.note(msg)
        return False, (msg + f"；选轨/步窗/页未还原（仍为归一值）；已出的 PNG 与 matrix.csv 在 {r.out_dir}")
    want_auto = "on" if snap["autosave"] else "off"
    r.run(f"STORE AUTO {want_auto}", rf"STORE AUTO {want_auto}")
    tail = f"工作工程已核对回到起点（LIVEHASH {snap['hash']:08X}）；autosave 回到原来的 {want_auto}"
    s.ser.note(f"收尾完成：{tail}")
    return True, tail


def write_csv(rows: list, path: Path) -> None:
    n = max((int(k[1:-3]) for row in rows for k in row if k.startswith("a") and k.endswith("_ok")),
            default=0)
    fields = ["page", "state", "commands"] + [f"a{i}_ok" for i in range(1, n + 1)] + \
             [f"a{i}_reading" for i in range(1, n + 1)] + ["anchors", "png", "retries", "verdict"]
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def run_matrix(port: str, out_root: Path, pages: set, retries: int, inject_drift: bool = False) -> int:
    inject.require_deps()
    entries = [e for e in MATRIX if not pages or e["page"] in pages]
    stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
    out_dir = out_root / f"matrix-{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    s = inject.Session(port, out_dir / "matrix.log", out_dir, stamp, "matrix", retries=retries)
    print(f"== screenshot_matrix @ {port}")
    print(f"   产物目录: {out_dir}")
    r = Runner(s, out_dir, inject_drift)
    drift = error = None
    interrupted = complete = False
    try:
        s.wait_ready()
        run_no = 0
        while True:
            r.rows = []
            try:
                bracket_begin(r)
                for e in entries:
                    run_entry(r, e)
                complete = True
                break
            except Restart as e:
                if run_no >= retries:
                    raise inject.InjectError(f"复位重跑次数耗尽（{retries}）: {e}") from None
                run_no += 1
                r.undo = []      # 复位前的还原动作已无意义：设备已从 flash 恢复，不能拿旧快照去写未知状态
                print(f"  [RETRY] {e}（{run_no}/{retries}），矩阵从头重跑")
    except HashDrift as e:
        drift = str(e)
    except Refuse as e:
        error = str(e)
    except inject.InjectError as e:
        error = str(e)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        # 每一种终止都走同一条收口路径；收尾自身出错也不能漏关串口
        try:
            restored, closing = close_bracket(r, drift, error)
        except Exception as e:  # noqa: BLE001
            restored, closing = False, f"收尾异常: {e!r}；autosave 可能仍是 OFF"
            error = error or f"收尾异常: {e!r}"
        finally:
            s.ser.close()
        if r.rows:
            write_csv(r.rows, out_dir / CSV_NAME)
        print(f"[INFO] PNG {len(r.rows)}/{len(entries)} 张；锚点 {sum(int(row['anchors']) for row in r.rows)} 个；"
              f"截屏复位重试 {r.retries} 次")
        print(f"[INFO] {closing}")
    sheets = []
    if complete and restored and error is None:
        sheets = make_sheets(entries, out_dir, out_dir)
        print(f"[INFO] 联系表 {len(sheets)} 张")
    where = f"（{out_dir}）"
    if interrupted:
        print(f"[ABORT] 用户中断：{closing}；已出的 PNG 与 matrix.csv 在 {out_dir}")
        return 130
    if not restored:
        print(closing)
        return 1
    if error is not None:
        print(f"[FAIL] screenshot_matrix: {error}；{closing}；已出的 PNG 与 matrix.csv 在 {out_dir}")
        return 1
    if s.failures:
        print(f"[FAIL] screenshot_matrix: {s.failures} 项断言/锚点未通过（见上与 {out_dir}）")
        return 1
    print(f"[PASS] screenshot_matrix: {len(entries)} 条目全部解码、锚点命中、每条目后 LIVEHASH 不变、起点还原{where}")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 全页面截屏矩阵 + 联系表")
    ap.add_argument("port", nargs="?", default=None, help="串口设备，如 /dev/ttyACM0（--selftest 时省略）")
    ap.add_argument("--out", default=None, help="产物父目录（默认 $SLOOP_TAB5_PROJECT/build/verify/）")
    ap.add_argument("--selftest", action="store_true", help="离线自测：矩阵完整性 + 占位 PNG 拼联系表，无需设备")
    ap.add_argument("--pages", default=None, metavar="a,b,…", help="只跑这些页的条目（逗号分隔的页名）")
    ap.add_argument("--retry", type=int, default=shot.DEFAULT_RETRIES, metavar="N",
                    help="截屏传输失败时经 esptool 复位后整轮重跑的次数")
    ap.add_argument("--inject-drift", action="store_true", help=argparse.SUPPRESS)   # 测试钩子，见 inject_drift()
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)      # 重定向到文件时也逐行可见（后台运行）
    if args.selftest:
        try:
            run_selftest()
        except AssertionError as e:
            sys.exit(f"selftest FAILED: {e}")
        print("screenshot_matrix selftest OK")
        return
    if not args.port:
        ap.error("缺少串口参数 PORT（或使用 --selftest）")
    if args.retry < 0:
        ap.error("--retry 不能为负")
    pages = None
    if args.pages:
        pages = {p.strip() for p in args.pages.split(",") if p.strip()}
        unknown = sorted(pages - set(PAGES))
        if unknown:
            ap.error(f"--pages 含未知页名 {unknown}（可选：{[n for _, n in inject.NAV_ORDER]}）")
    out_root = Path(args.out) if args.out else Path(
        os.environ.get("SLOOP_TAB5_PROJECT") or str(SCRIPTS_DIR.parent / "firmware")) / "build" / "verify"
    sys.exit(run_matrix(args.port, out_root, pages, args.retry, args.inject_drift))


# ---------------------------------------------------------------------------
# 离线自测
# ---------------------------------------------------------------------------

def _check_matrix() -> None:
    seen = set()
    for e in MATRIX:
        key = (e["page"], e["state"])
        assert key not in seen, f"MATRIX 的 (page,state) 重复：{key}"
        seen.add(key)
        assert e["page"] in PAGES, f"未知页名 {e['page']!r}"
        assert len(e["anchors"]) >= 3, f"{key} 的锚点不足 3 个（{len(e['anchors'])}）"
        for a in e["anchors"]:
            res = a(SAMPLE_CTX) if callable(a) else a
            assert res is not None, f"{key} 的锚点在样例 ctx 下不适用（自测要求可解析）"
            assert len(res) in (3, 4, 6, 7), f"{key} 的锚点元组长度 {len(res)} 不合法：{res}"
            xs, ys = ((res[0], res[2]), (res[1], res[3])) if len(res) >= 6 else ((res[0],), (res[1],))
            assert all(0 <= x < W for x in xs) and all(0 <= y < H for y in ys), \
                f"{key} 的锚点越界：{res}"
            if len(res) >= 6:
                assert res[0] < res[2] and res[1] < res[3] and res[5] > 0, f"{key} 的区域锚点退化：{res}"
            else:
                assert len(res) == 3 or res[3] > 0, f"{key} 的点锚点容差非法：{res}"
            cls = res[4] if len(res) >= 6 else res[2]
            assert cls in theme_pixels(4), f"{key} 的锚点类别未知：{cls!r}"
    assert {e["page"] for e in MATRIX} == set(PAGES), "MATRIX 没有覆盖全部 18 页"
    for page in PAGES:
        assert any(e["page"] == page and e["state"] == "default" for e in MATRIX), \
            f"{page} 缺少 default 条目"
    assert len(MATRIX) == len(seen)
    for e in MATRIX:
        for item in e["setup"] + e["post"]:
            name = item[1:] if isinstance(item, str) and item.startswith("@") else None
            assert name is None or name in MACROS, f"{(e['page'], e['state'])} 引用了未定义的宏 @{name}"
    required = ({(p, "default") for p in PAGES}
                | {(p, "editing") for p in ("sound", "mod", "scale", "voice", "fx", "global")}
                | {("step", "playing"), ("transport", "playing"), ("sound", "drum"), ("tools", "armed"),
                   ("sound", "fm6"), ("mod", "locked")})
    have = {(e["page"], e["state"]) for e in MATRIX}
    assert required <= have, f"MATRIX 缺少派单要求的状态：{sorted(required - have)}"
    for group in GROUP_PAGES:
        n = len(group_entries(group))
        assert n <= SHEET_COLS * 5, f"{group} 组 {n} 个条目超过一张联系表的 {SHEET_COLS * 5} 格"
    assert len(first_entries_of(MATRIX)) == len(PAGES) <= ALL_COLS * 3, "sheet-all 应为每页一格（6×3）"


def _check_theme() -> None:
    px = theme_pixels(4)
    for cls, const in (("accent", inject.C_YELLOW), ("panel", inject.C_PANEL), ("panel_hi", inject.C_PANEL_HI),
                       ("panel_sel", inject.C_PANEL_SEL), ("text", inject.C_TEXT), ("error", inject.C_ERROR),
                       ("bg", inject.C_BG), ("track0", inject.C_BLUE), ("track1", inject.C_GREEN),
                       ("track2", inject.C_YELLOW), ("track3", inject.C_ORANGE)):
        assert px[cls] == const, f"MONO 的 {cls} 派生 {px[cls]} ≠ inject 的实测常量 {const}"
    assert max(abs(a - b) for a, b in zip(px["text_dim"], inject.C_TEXT_DIM)) <= 8, \
        f"MONO 的 text_dim 派生 {px['text_dim']} 与 inject 的 {inject.C_TEXT_DIM} 相差超过位扩展误差"
    for pal in range(5):
        assert set(theme_pixels(pal)) == set(px), f"palette {pal} 的类别集合不一致"
    assert theme_rgb(1)["accent"] == (255, 166, 40) and theme_rgb(0)["accent"] == (120, 255, 146), \
        "palette 0/1 的 accent 应取色阶第 5 档"


def _check_anchor_checker() -> None:
    from PIL import Image, ImageDraw
    px = theme_pixels(4)
    img = Image.new("RGB", (W, H), px["bg"])
    d = ImageDraw.Draw(img)
    d.rectangle((100, 100, 200, 150), fill=px["accent"])
    d.rectangle((400, 100, 420, 120), fill=px["error"])
    ok, msg = check_anchor(img, (150, 120, "accent"), px, SAMPLE_CTX)
    assert ok is True, f"accent 点应命中：{msg}"
    ok, msg = check_anchor(img, (150, 120, "error"), px, SAMPLE_CTX)
    assert ok is False, "error 点不应命中 accent 像素"
    ok, msg = check_anchor(img, (100, 100, 200, 150, "accent", 5000), px, SAMPLE_CTX)
    assert ok is True, f"accent 区域计数不足：{msg}"
    ok, msg = check_anchor(img, (100, 100, 200, 150, "error", 1), px, SAMPLE_CTX)
    assert ok is False, "区域内没有 error 像素却判命中"
    ok, msg = check_anchor(img, lambda c: None, px, SAMPLE_CTX)
    assert ok is None and msg == "n/a", "None 锚点应记 n/a"
    ok, msg = check_anchor(img, lambda c: (410, 110, c["cls"]), px, {**SAMPLE_CTX, "cls": "error"})
    assert ok is True, f"ctx 锚点应解析到 error 块：{msg}"
    ok, msg = check_anchor(img, (1279, 719, "bg"), px, SAMPLE_CTX)
    assert ok is True, "右下角 bg 像素应命中"


def _placeholder(entry: dict, path: Path) -> None:
    from PIL import Image, ImageDraw
    px = theme_pixels(4)
    img = Image.new("RGB", (W, H), px["bg"])
    d = ImageDraw.Draw(img)
    d.rectangle((16, TABCAT, W - 16, H - 16), fill=px["panel_hi"])
    d.rectangle((0, 0, 300, TABCAT - 4), fill=px["accent"])
    d.text((40, 200), f"{entry['page']} {entry['state']}", fill=px["text"])
    img.save(path)


def _check_sheets() -> None:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for e in MATRIX:
            _placeholder(e, tmp / png_name(e))
        sheets = make_sheets(MATRIX, tmp, tmp)
        from PIL import Image
        expect = [f"sheet-{g.lower()}.png" for g in GROUP_PAGES if group_entries(g)] + ["sheet-all.png"]
        assert [p.name for p in sheets] == expect, f"联系表清单不符：{[p.name for p in sheets]}"
        for p in sheets:
            with Image.open(p) as im:
                cols = ALL_COLS if p.name == "sheet-all.png" else SHEET_COLS
                items = (first_entries_of(MATRIX) if cols == ALL_COLS
                         else group_entries(p.stem.split("-", 1)[1].upper()))
                thumb_w = 208 if cols == ALL_COLS else 320
                assert im.size == sheet_size(len(items), cols, thumb_w), \
                    f"{p.name} 尺寸 {im.size} ≠ {sheet_size(len(items), cols, thumb_w)}"
                assert len(items) <= cols * (3 if cols == ALL_COLS else 5), f"{p.name} 超出格数上限"
                assert im.convert("RGB").getpixel((0, 0)) == (0, 0, 0), f"{p.name} 底色"
                # 格标已画：第一格标签区有非底色像素
                strip = im.convert("RGB").crop((8, 8, 8 + thumb_w, 8 + 24))
                assert any(c != (0, 0, 0) for _, c in strip.getcolors(1 << 20)), f"{p.name} 未画格标"
        with Image.open(tmp / png_name(MATRIX[0])) as im:
            im.resize((64, 36)).save(tmp / png_name(MATRIX[0]))      # 尺寸不符的占位图
        try:
            build_sheet([MATRIX[0]], tmp, tmp / "x.png", 4, 320)
        except ValueError as e:
            assert "尺寸" in str(e), f"尺寸不符应报 ValueError：{e}"
        else:
            raise AssertionError("占位 PNG 尺寸不符时 build_sheet 未报错")


def _check_csv() -> None:
    rows = [{"page": "play", "state": "default", "commands": "UI play", "a1_ok": "pass",
             "a1_reading": "(x,y) accent", "anchors": 5, "png": "play-default.png", "retries": 0,
             "verdict": "PASS"}]
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / CSV_NAME
        write_csv(rows, p)
        with p.open(encoding="utf-8") as f:
            got = list(csv.DictReader(f))
        assert len(got) == 1 and got[0]["verdict"] == "PASS" and got[0]["a1_ok"] == "pass", \
            "matrix.csv 往返不符"
        assert list(got[0])[:3] == ["page", "state", "commands"], "CSV 列顺序"


def run_selftest() -> None:
    _check_matrix()
    _check_theme()
    _check_anchor_checker()
    _check_sheets()
    _check_csv()
    print(f"自测断言全部通过（MATRIX {len(MATRIX)} 条目 / {len(PAGES)} 页：唯一性、页名、宏名全部可解析、派单要求的状态齐全、锚点 ≥3 且在 1280×720 内、"
          f"每组 ≤{SHEET_COLS * 5} 格；MONO 主题色派生 = inject 实测常量；点/区域/ctx 锚点判定；"
          f"占位 PNG → {len(GROUP_PAGES) + 1} 张联系表；CSV 往返）")


if __name__ == "__main__":
    main()
