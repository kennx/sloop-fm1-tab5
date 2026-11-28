#!/usr/bin/env python3
"""5B-1 Task 8：Sound 仪表盘注入场景（`scripts/inject/sound.sh`）的基线 / 还原核对（证据脚本，随证据包入库）。

Usage:
    python3 task-8-baseline.py capture  PORT BASELINE.txt TRANSCRIPT.log
    python3 task-8-baseline.py verify   PORT BASELINE.txt RESTORE.txt TRANSCRIPT.log [--reset]

capture：只读地记下整份工作状态——`ENG SNAP` 四轨（引擎 / 预置 / level / mute / len / div）、bpm /
    选中轨 / 步窗、三条合成轨各 71 个轨道参数 + 全局参数 `PARAM GLOBAL 0..55`、鼓轨 `P_E0`（kit）、
    settings 生效副本（`STORE SETTINGS INFO`）、autosave 三项、工程槽 / 用户预置 / FM6 银行的逐槽清单。
    读轨道参数要先选轨（`PARAM <id>` 读选中轨）：选轨经 Ctrl+数字键（inject.py `key_with_layer` 同一写法），
    收尾把选中轨选回入口值。全程不发任何写命令（`PARAM <id>` / `PARAM GLOBAL <id>` 都是无值只读形式）。

verify：重新读一遍，与 BASELINE 逐项比对，结论写进 RESTORE.txt（逐项相同 = `RESTORE OK`，否则列出
    差异并以非零退出）。`--reset` 先 esptool 硬复位（不写 flash）再读：复位后设备从 flash 里的 autosave
    副本恢复，读数相同 = 还原已落盘。串口字节逐字节写 TRANSCRIPT（原始回显），工具注记一律
    `# [baseline] ...` 前缀。
"""
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts"))
import screenshot as shot  # noqa: E402

BAUD = 115200
NOTE = "# [baseline]"
P_COUNT = 71            # 上游 P_COUNT（三条合成轨的 p[] 长度；63..70 = P_E0..P_E7）
G_PROBE = 56            # 全局参数只读探测 0..55（越界的 id 回 PARAM ERR，记 invalid）
VOLATILE = ("voices", "pos")   # ENG SNAP 里随运行变化的字段，不参与比对


class Console:
    def __init__(self, port, log_path):
        self.port = port
        self.log = open(log_path, "ab")
        self.ser = serial.Serial(port, BAUD, timeout=0.05)

    def note(self, text):
        line = f"{NOTE} {text}"
        print(line, flush=True)
        self.log.write((line + "\n").encode())
        self.log.flush()

    def read_until_prompt(self, timeout=8.0):
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf.extend(chunk)
                self.log.write(chunk)
                self.log.flush()
                continue
            if buf.rfind(b"sloop>") >= 0:
                return buf.decode(errors="replace")
        raise TimeoutError(f"等待提示符超时（已收 {len(buf)} 字节）")

    def cmd(self, line, settle=0.0, timeout=8.0):
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        text = self.read_until_prompt(timeout)
        if settle:
            time.sleep(settle)
        return text

    def wait_ready(self, timeout=20.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.ser.write(b"\n")
            self.ser.flush()
            try:
                self.read_until_prompt(1.0)
                return
            except TimeoutError:
                continue
        raise SystemExit(f"{NOTE} FAIL: 等待 console 提示符超时")

    def reset(self):
        self.ser.close()
        shot.reset_device(self.port, BAUD, on_chunk=self.log.write)
        self.log.flush()
        self.ser = serial.Serial(self.port, BAUD, timeout=0.05)

    def close(self):
        self.ser.close()
        self.log.close()


def key(c, code):
    c.cmd(f"KEY 0x{code:02X} down")
    c.cmd(f"KEY 0x{code:02X} up")


def select_track_checked(c, k):
    for code, ev in ((0x40, "down"), (k + 1, "down"), (k + 1, "up"), (0x40, "up")):
        c.cmd(f"KEY 0x{code:02X} {ev}")
    text = c.cmd("STATE SNAP")
    got = re.search(r"selected_track=(\d+)", text)
    if got is None or int(got.group(1)) != k:
        raise SystemExit(f"{NOTE} FAIL: 选轨 {k} 未生效（{text.strip()[-120:]!r}）")


def read_state(c, out: dict) -> None:
    text = c.cmd("ENG SNAP")
    m = re.search(r"ENG SNAP playing=(\d+) bpm=(\d+) sel=(\d+)", text)
    if m is None:
        raise SystemExit(f"{NOTE} FAIL: ENG SNAP 首行缺失")
    out["playing"], out["bpm"], out["sel"] = m.groups()
    rows = re.findall(r"ENG SNAP trk k=(\d) engine=(\d+) preset=(\d+) level=(\d+) mute=(\d) armed=(\d) "
                      r"solo=(\d) len=(\d+) div=(\d+)", text)
    if len(rows) != 4:
        raise SystemExit(f"{NOTE} FAIL: ENG SNAP trk 行不完整（{len(rows)}/4）")
    for k, e, p, lvl, mute, armed, solo, ln, div in rows:
        out[f"trk{k}"] = f"engine={e} preset={p} level={lvl} mute={mute} armed={armed} solo={solo} len={ln} div={div}"
    text = c.cmd("STATE SNAP")
    for f in ("selected_track", "bpm", "playing", "window_bank", "window_start", "window_len", "set_zoom",
              "set_palette", "set_theme"):
        m = re.search(rf"\b{f}=(\d+)", text)
        if m is None:
            raise SystemExit(f"{NOTE} FAIL: STATE SNAP 缺少 {f}=")
        out[f"state.{f}"] = m.group(1)
    out["state.param"] = re.search(r"STATE SNAP param [^\r\n]*", text).group(0)
    out["state.global"] = re.search(r"STATE SNAP global [^\r\n]*", text).group(0)
    text = c.cmd("UI WINDOW")
    out["window"] = re.search(r"UI WINDOW [^\r\n]*", text).group(0)
    entry = int(out["state.selected_track"])
    for k in range(3):
        select_track_checked(c, k)
        for pid in range(P_COUNT):
            text = c.cmd(f"PARAM {pid}")
            m = re.search(rf"PARAM track={k} id={pid} value=(-?\d+) min=(-?\d+) max=(-?\d+)", text)
            if m:
                out[f"p{k}.{pid}"] = f"{m.group(1)} [{m.group(2)}..{m.group(3)}]"
            elif re.search(r"reason=invalid_id", text):
                out[f"p{k}.{pid}"] = "invalid"
            else:
                raise SystemExit(f"{NOTE} FAIL: PARAM {pid}（轨 {k}）回显异常：{text.strip()[-120:]!r}")
    select_track_checked(c, 3)
    text = c.cmd("PARAM 63")
    m = re.search(r"PARAM track=3 id=63 value=(-?\d+) min=(-?\d+) max=(-?\d+)", text)
    if m is None:
        raise SystemExit(f"{NOTE} FAIL: 鼓轨 PARAM 63 回显异常")
    out["kit"] = f"{m.group(1)} [{m.group(2)}..{m.group(3)}]"
    for gid in range(G_PROBE):
        text = c.cmd(f"PARAM GLOBAL {gid}")
        m = re.search(rf"PARAM GLOBAL id={gid} value=(-?\d+) min=(-?\d+) max=(-?\d+)", text)
        if m:
            out[f"g.{gid}"] = f"{m.group(1)} [{m.group(2)}..{m.group(3)}]"
        elif re.search(r"reason=invalid_id", text):
            out[f"g.{gid}"] = "invalid"
        else:
            raise SystemExit(f"{NOTE} FAIL: PARAM GLOBAL {gid} 回显异常：{text.strip()[-120:]!r}")
    text = c.cmd("STORE SETTINGS INFO")
    m = re.search(r"STORE SETTINGS INFO [^\r\n]*", text)
    if m is None:
        raise SystemExit(f"{NOTE} FAIL: STORE SETTINGS INFO 缺失（存储未就绪？）")
    out["settings"] = re.sub(r" dirty=\d+", "", m.group(0))
    text = c.cmd("STORE STATS")
    m = re.search(r"autosave=(\d)", text)
    if m is None:
        raise SystemExit(f"{NOTE} FAIL: STORE STATS 缺少 autosave")
    out["autosave_on"] = m.group(1)      # autosave_hash 是「上一次提交时」的值（提交可能落在选轨 ≠ 入口轨的
    #   时刻，song.sel 是哈希的一部分）⇒ 不比对；整份工程的等价性由收尾的 livehash 证明
    for name in ("PROJ", "PRESET", "BANK"):
        text = c.cmd(f"STORE {name} LIST")
        rows = [ln.strip() for ln in text.splitlines()
                if ln.startswith(f"{name} ") and "COUNT" not in ln]
        out[f"list.{name}"] = " | ".join(rows)
    select_track_checked(c, entry)
    text = c.cmd("STORE LIVEHASH")
    m = re.search(r"LIVEHASH ([0-9A-F]{8})", text)
    if m is None:
        raise SystemExit(f"{NOTE} FAIL: STORE LIVEHASH 缺失")
    out["livehash"] = f"{m.group(1)}（选中轨 = 入口的 {entry}）"


def dump(state: dict) -> str:
    return "\n".join(f"{k} = {v}" for k, v in state.items())


def main() -> int:
    args = sys.argv[1:]
    mode = args[0]
    rest = [a for a in args[1:] if not a.startswith("--")]
    if len(rest) != (3 if mode == "capture" else 4 if mode == "verify" else 0):
        sys.exit(__doc__)
    c = Console(rest[0], rest[-1])
    reset = "--reset" in args
    try:
        c.wait_ready()
        if mode == "verify" and reset:
            c.note("esptool 硬复位（flash_id --after hard_reset，不写 flash）；以下至存储就绪为启动日志原始回显")
            c.reset()
            c.wait_ready()
            for _ in range(80):          # 存储在 console 起来之后才 init：等到 STATS 有 autosave= 为止
                if re.search(r"autosave=", c.cmd("STORE STATS")):
                    break
                time.sleep(0.25)
        state = {}
        read_state(c, state)
        text = dump(state)
        if mode == "capture":
            Path(rest[1]).write_text(text + "\n", encoding="utf-8")
            c.note(f"基线 {len(state)} 项已写入 {rest[1]}")
            return 0
        base = dict(ln.split(" = ", 1) for ln in Path(rest[1]).read_text(encoding="utf-8").splitlines()
                    if " = " in ln)
        diff = [k for k in sorted(set(base) | set(state)) if base.get(k) != state.get(k)]
        lines = [f"# 还原核对（基线 = {Path(rest[1]).name}；{'硬复位后' if reset else '未复位'}重读，"
                 f"{len(state)} 项）", f"# 结论：{'RESTORE OK' if not diff else 'RESTORE MISMATCH'}"
                 f"（差异 {len(diff)} 项）"]
        lines += [f"#   {k}: 基线 {base.get(k)!r} → 现在 {state.get(k)!r}" for k in diff]
        lines += ["", "# ---- 基线 ----", dump(base), "", "# ---- 现在 ----", text]
        Path(rest[2]).write_text("\n".join(lines) + "\n", encoding="utf-8")
        c.note(lines[1])
        return 0 if not diff else 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
