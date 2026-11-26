#!/usr/bin/env python3
"""5B-1 Task 2：Sound 仪表盘骨架的真机原型测量（证据脚本，随证据包入库）。

Usage:
    python3 task-2-proto-measure.py PORT PHASE TRANSCRIPT.log
    PHASE = base | fm6 | fm6-restore | seq | roundtrip | edit2step
          | drift <目标页名>（对照组：5 轮 UI step ↔ UI <页>，第 4 个参数给页名）

写法照 scripts/selftest-heap.py：pyserial、BAUD 115200、读到 console 提示符（sloop>）为止；
串口字节逐字节写 TRANSCRIPT（原始回显，不裁剪），工具注记一律 `# [proto] ...` 前缀。
只读为主；会改设备状态的只有 STORE AUTO OFF 与 fm6 相的引擎切换（经页眉 ENGINE 行、与手指同一
动作，fm6-restore 切回 ANALOG；收尾再硬复位 = 重新载入 autosave 对象）。
**TAP 坐标是屏幕绝对坐标（含 72px 页签条）**：首版把卡片标题行写成 y=80，那其实是页眉 ENGINE 行的
`−` 区（页面区 y=8），把选中轨引擎拨到了 NOISE——已在 hard reset 后改对，坐标注释里都标了页面区 y。
"""
import re
import sys
import time
from pathlib import Path

import serial

BAUD = 115200
NOTE = "# [proto]"
RE_POOL = re.compile(r"UI MEM pool_used=(\d+) pool_free=(\d+) pool_total=(\d+) registered=(\d+) "
                     r"built=(\d+) failed=(\d+) sum_est=(\d+)")
RE_PAGE = re.compile(r"UI MEM page id=(\d+) title=(\S+) state=(\S+) est=(\d+) delta=(-?\d+) "
                     r"build_ms=(\d+)")
RE_GROUP = re.compile(r"UI MEM GROUP (\S+) built=(\d+) sum_est=(\d+) sum_delta=(\d+)")
RE_HEAP = re.compile(r"ENG MEM internal_free=(\d+) internal_min=(\d+) psram_free=(\d+)")
RE_BUILD = re.compile(r"页面构建 page=(\S+)\((\d+)\) 池增量=(-?\d+)B 预算=(\d+)B 池剩余=(\d+)B 耗时=(\d+)ms")
RE_SWITCH = re.compile(r"切页 page=(\S+)\((\d+)\) 组=(\S+) (跨组|组内首建)耗时=(\d+)ms")


class Console:
    def __init__(self, port, log_path):
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

    def cmd(self, line, timeout=8.0):
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        return self.read_until_prompt(timeout)

    def close(self):
        self.ser.close()
        self.log.close()


def grab(text, regex, what):
    m = regex.search(text)
    if m is None:
        raise SystemExit(f"{NOTE} FAIL: 响应里没有 {what}")
    return m


def mem(c):
    text = c.cmd("ENG MEM")
    m = grab(text, RE_HEAP, "ENG MEM")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def ui_mem(c, tag):
    text = c.cmd("UI MEM")
    m = grab(text, RE_POOL, "UI MEM")
    used, free, total = int(m.group(1)), int(m.group(2)), int(m.group(3))
    c.note(f"UI MEM[{tag}] pool_used={used} pool_free={free} pool_total={total} "
           f"registered={m.group(4)} built={m.group(5)} failed={m.group(6)}")
    pages = {}
    for pm in RE_PAGE.finditer(text):
        pages[int(pm.group(1))] = pm.groups()
        c.note(f"UI MEM[{tag}] page id={pm.group(1)} title={pm.group(2)} state={pm.group(3)} "
               f"est={pm.group(4)} delta={pm.group(5)} build_ms={pm.group(6)}")
    for gm in RE_GROUP.finditer(text):
        c.note(f"UI MEM[{tag}] GROUP {gm.group(1)} built={gm.group(2)} sum_est={gm.group(3)} "
               f"sum_delta={gm.group(4)}")
    return used, free, pages, text


def engines(c):
    """四轨的 engine/preset（`ENG SNAP trk k=` 行）：每个相位前后各读一次，证明读数之外的
    设备状态没被测量动作改动（页眉行 ∓ 区、银行行 SAVE/ERASE 都是真实动作）。"""
    text = c.cmd("ENG SNAP")
    rows = re.findall(r"ENG SNAP trk k=(\d) engine=(\d+) preset=(\d+)", text)
    return ",".join(f"{e}/{p}" for _, e, p in rows)


def builds(c, text, tag):
    for bm in RE_BUILD.finditer(text):
        c.note(f"BUILD[{tag}] page={bm.group(1)}({bm.group(2)}) delta={bm.group(3)} "
               f"est={bm.group(4)} pool_free_after={bm.group(5)} build_ms={bm.group(6)}")
    for sm in RE_SWITCH.finditer(text):
        c.note(f"SWITCH[{tag}] page={sm.group(1)}({sm.group(2)}) group={sm.group(3)} "
               f"{sm.group(4)} ms={sm.group(5)}")


def main():
    port, phase, log_path = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    c = Console(port, log_path)
    c.note(f"phase={phase} port={port}")
    try:
        c.cmd("")  # 丢掉开机/上一会话的残余输出
        engines_before = engines(c)
        c.note(f"相位前 engine/preset[轨1..4]={engines_before}")
        if phase == "base":
            c.cmd("STORE AUTO OFF")
            before, imin, _ = mem(c)
            c.note(f"进入页面前 internal_free={before} internal_min={imin}")
            text = c.cmd("UI sound")
            builds(c, text, "sound-first")
            used, free, pages, _ = ui_mem(c, "sound")
            after, imin, _ = mem(c)
            c.note(f"进入页面后 internal_free={after} internal_min={imin} "
                   f"差={before - after}B（门限 ≤256）")
            c.cmd("TAP 100 200")       # 槽矩形：Global 卡槽 0
            c.cmd("TAP 22 160")        # 卡片标题行：Global（页面区 y=88 = 视口顶 80 + 8）
            c.cmd("TAP 636 272")       # 两列之间的空隙
            c.cmd("TAP 900 122")       # 页眉带（视口顶之上）
            c.cmd("TAP 1106 122")      # − 按钮（仪表盘页 y=40）
            c.cmd("TAP 1272 52")       # 旧 − 按钮位置（y=2）：现在应为 miss
            c.cmd("UI KNOB 0 press 0 -40")
            c.cmd("UI sound CHIP")
        elif phase in ("fm6", "fm6-restore"):
            # 银行行只在**选中轨**引擎为 FM6 时出现：经页眉 ENGINE 行的 ∓ 区走与手指相同的
            # 动作（CMD_SET_ENGINE，每次 ±1），不用 `ENG TRACK`（它直接写 core、不改选中轨）。
            zone_x = 283 if phase == "fm6" else 37   # 行 0 右缘 − 21 = `+` 区 / 左缘 + 21 = `−` 区
            want = 9 if phase == "fm6" else 0
            c.note(f"选中轨 engine/preset={engines_before.split(',')[1]}（轨 2）")
            for _ in range(12):
                out = c.cmd(f"TAP {zone_x} 92")
                m = re.search(r"SOUND ENGINE value=(\d+) name=\"([^\"]*)\" status=\"(\w+)\"", out)
                if m is None:
                    raise SystemExit(f"{NOTE} FAIL: ENGINE 行 TAP 未命中：{out.strip()[-160:]}")
                if int(m.group(1)) == want:
                    break
            c.note(f"ENGINE 行到达 value={m.group(1)} name={m.group(2)} status={m.group(3)}")
            time.sleep(0.3)  # 等两拍以上：银行行显隐与容器挪位在 30ms 拍里落地
            ui_mem(c, phase)
            if phase == "fm6":
                c.cmd("TAP 122 170")     # 银行行 −（目标槽 B1 → B27；只改页内目标槽，不写 flash）
                c.cmd("TAP 378 674")     # 银行行旧位置（y=552）的 SAVE x：现在应为 miss
                c.cmd("TAP 100 248")     # 视口顶 128：内容 y = 176 − 128 = 48 → Global 卡槽 0
        elif phase == "seq":
            c.cmd("UI step")
            c.cmd("UI transport")
            text = c.cmd("UI sound")
            builds(c, text, "seq-to-sound")
            ui_mem(c, "seq-to-sound-after")
        elif phase == "roundtrip":
            first = None
            for i in range(1, 6):
                c.cmd("UI step")
                c.cmd("UI sound")
                used, free, pages, _ = ui_mem(c, f"round{i}")
                d6 = pages.get(6)
                c.note(f"round{i}: SOUND delta={d6[4] if d6 else '?'} pool_free={free}")
                first = first or (d6[4] if d6 else None, free)
            c.note(f"往返 5 次：delta 首末 {first[0]} -> {d6[4]}；pool_free 首末 {first[1]} -> {free} "
                   f"（差 {free - first[1]:+d}B，门限 |Δ| ≤128）")
        elif phase == "drift":
            # 对照组：同样的 5 轮 `UI step` ↔ `UI <目标页>`，目标页 = 第 4 个参数（mod/global/
            # sample/dsyn…）。用来判断往返漂移是 SOUND 页带来的，还是 SEQ↔任意页都有的。
            target = sys.argv[4]
            first = last = None
            for i in range(1, 6):
                text = c.cmd("UI step")
                for bm in RE_BUILD.finditer(text):
                    c.note(f"BUILD[drift-{target} r{i}] {bm.group(0)}")
                text = c.cmd(f"UI {target}")
                for bm in RE_BUILD.finditer(text):
                    c.note(f"BUILD[drift-{target} r{i}] {bm.group(0)}")
                used, free, pages, _ = ui_mem(c, f"drift-{target} r{i}")
                pid = {"sound": 6, "mod": 15, "global": 4, "sample": 8, "dsyn": 12, "presets": 10,
                       "song": 11, "voice": 17, "scale": 16, "tools": 18}[target]
                d = pages.get(pid)
                c.note(f"drift[{target}] round{i}: delta={d[4] if d else '?'} pool_free={free}")
                first = first or (d[4] if d else None, free)
                last = (d[4] if d else None, free)
            c.note(f"drift[{target}] 5 轮：delta {first[0]} -> {last[0]}；pool_free {first[1]} -> {last[1]}"
                   f"（差 {last[1] - first[1]:+d}B）")
        elif phase == "edit2step":
            c.cmd("UI sound")
            c.cmd("UI mod")
            c.cmd("UI scale")
            c.cmd("UI voice")
            used, free, pages, _ = ui_mem(c, "edit-all-built")
            text = c.cmd("UI step")
            builds(c, text, "edit-to-step")
            ui_mem(c, "edit-to-step-after")
        else:
            raise SystemExit(f"未知 PHASE {phase}")
        engines_after = engines(c)
        c.note(f"相位后 engine/preset[轨1..4]={engines_after}"
               f"{'' if engines_after == engines_before else '  ⚠ 与相位前不同'}")
    finally:
        c.close()


if __name__ == "__main__":
    main()
