#!/usr/bin/env python3
"""soak.py — 稳定性能 soak（阶段 3 完成判据 4；阶段 4 长时 soak 复用同一口径）

在真机上循环「播放 → 步编辑 → 选轨/切页 → 键盘音符」，保持 TRANSPORT 页播放中，
每 --interval 秒采样一次引擎水位：
    ENG MEM   internal_free / internal_min
    ENG PERF  streaming / xrun / audio_stack_free / min_free_since_boot_*
    KEY STATS 输入延迟均值/峰值与输入任务栈余量
    STATE SNAP 页面/播放/选轨（行为回归）
判定（全部满足才退出码 0）：
    1. 内部堆非单调下降：采样首末差 ≥ -8KB，且 ≥3 个采样点不全为逐次下降；
       全程 internal_free ≥ 60KB（AGENTS §5.5 内存门禁）；
    2. 每次采样 streaming=1 且 xrun=0（流式音频无欠载）；
    3. 音频任务栈余量 ≥ 2KB；
    4. 原始串口日志无 panic / Guru Meditation / task_wdt / abort / Backtrace；
    5. 每周期的切页回显与 STATE SNAP 页面断言全过（无切页回归）。

产物（默认 $SLOOP_TAB5_PROJECT/build/verify/）：soak-<时间戳>.log（原始串口回显，
同 inject.py 口径：逐字节写盘，唯一非固件字节是 `# [inject] ...` 注记）与
soak-summary-<时间戳>.txt（采样表 + 判定）。退出码 0 = 判定通过。

用法：python3 scripts/soak.py /dev/ttyACM0 [--duration 1800] [--interval 300] [--cycle 12]
"""
import argparse
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS_DIR))
sys.path.insert(0, str(SCRIPTS_DIR / "inject"))
import inject  # noqa: E402

PAGES = {"play": 0, "tracks": 1, "step": 2, "transport": 3}
HEAP_FLOOR = 60 * 1024
HEAP_NET_FLOOR = -8 * 1024
STACK_FLOOR = 2048
PANIC_TOKENS = ("panic", "Guru Meditation", "task_wdt", "abort()", "Backtrace", "assert failed")


def kv_int(text: str) -> dict:
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def kv_raw(text: str) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", text))


def sample(s) -> dict:
    """一次水位采样（同时是行为回归探针：STAT 页面/播放状态必须符合预期）。"""
    s.cmd("ENG MEM")
    mem = kv_int(s.last)
    s.cmd("ENG PERF")
    perf = kv_raw(s.last)
    s.cmd("KEY STATS")
    stats = kv_int(s.last)
    s.cmd("STATE SNAP")
    state = kv_int(s.last)
    s.expect_int(r"playing=(\d)", 1, "soak STATE playing")
    if perf.get("streaming") != "1":
        s.fail(f"soak 采样时未流式输出：{s.last.strip()}")
    return {
        "internal_free": mem.get("internal_free", -1),
        "internal_min": mem.get("internal_min", -1),
        "psram_free": mem.get("psram_free", -1),
        "streaming": perf.get("streaming", "?"),
        "xrun": int(perf.get("xrun", -1)),
        "audio_stack_free": int(perf.get("audio_stack_free", -1)),
        "key_mean_us": stats.get("mean_us", -1),
        "key_max_us": stats.get("max_us", -1),
        "input_stack_free": stats.get("input_stack_free", -1),
        "page": state.get("page", -1),
        "bpm": state.get("bpm", -1),
    }


def switch(s, page: str) -> None:
    """同步切页并核验回显页 == 请求页（切页确定性回归）。"""
    s.cmd(f"UI {page}")
    s.expect(rf"UI page={page} \({PAGES[page]}\)", f"UI {page}")
    s.cmd("STATE SNAP")
    s.expect_int(r"page=(\d+)", PAGES[page], f"STATE page（{page}）")


def ensure_playing(s) -> None:
    s.cmd("STATE SNAP")
    if s.snap_int(r"playing=(\d)") == 0:
        s.cmd("TAP %d %d" % inject.TRANSPORT_PLAY)
        s.expect(r"-> TRANSPORT CMD_PLAY", "TAP PLAY（恢复播放）")


def cycle(s, n: int) -> None:
    """一轮：保持播放 → STEP 翻一格 → TRACKS 选一张卡 → PLAY 弹一个音 → 回 TRANSPORT。"""
    ensure_playing(s)

    switch(s, "step")
    cells = (inject.STEP_CELL_A, inject.STEP_CELL_B)
    track, step = ((0, 2), (1, 5))[n % 2]
    s.cmd("TAP %d %d" % cells[n % 2])
    s.expect(rf"-> STEP track={track} step={step} CMD_TOGGLE_STEP", f"TAP STEP {track}/{step}")

    switch(s, "tracks")
    card = n % 4
    s.cmd("TAP %d %d" % (inject.TRACKS_CARD2[0] + (card - 2) * 312, inject.TRACKS_CARD2[1]))
    s.expect(rf"-> TRACKS select track={card} CMD_SELECT_TRACK", f"TAP 卡 {card}")

    switch(s, "play")
    s.cmd("KEY 0x42 down")
    s.expect(r"KEY 0x42 down", "KEY 音符 down")
    s.cmd("KEY 0x42 up")
    s.expect(r"KEY 0x42 up", "KEY 音符 up")

    switch(s, "transport")


def verdict(rows: list, log_path: Path, duration: float) -> list:
    """按判定口径给出问题列表（空 = 通过）。"""
    problems = []
    if len(rows) < 2:
        problems.append(f"采样不足：{len(rows)} 次")
        return problems
    free = [r["internal_free"] for r in rows]
    if min(free) < HEAP_FLOOR:
        problems.append(f"内部堆低于 60KB：min={min(free)}")
    if free[-1] - free[0] < HEAP_NET_FLOOR:
        problems.append(f"内部堆首末下降 {free[0]} -> {free[-1]}（阈值 {HEAP_NET_FLOOR}）")
    if len(free) >= 3 and all(b < a for a, b in zip(free, free[1:])):
        problems.append(f"内部堆逐次单调下降：{free}")
    for r in rows:
        if r["streaming"] != "1":
            problems.append(f"t={r['t']}s 未流式输出")
        if r["xrun"] != 0:
            problems.append(f"t={r['t']}s xrun={r['xrun']}")
        if 0 <= r["audio_stack_free"] < STACK_FLOOR:
            problems.append(f"t={r['t']}s 音频任务栈余量 {r['audio_stack_free']}B < {STACK_FLOOR}B")
    text = log_path.read_text(errors="replace")
    hits = [tok for tok in PANIC_TOKENS if tok.lower() in text.lower()]
    if hits:
        problems.append(f"串口日志含异常标记：{hits}")
    if duration < 1800:
        problems.append(f"soak 时长 {duration:.0f}s < 1800s")
    return problems


def main() -> None:
    ap = argparse.ArgumentParser(description="Tab5 稳定性能 soak（AGENTS §5.5）")
    ap.add_argument("port", help="串口设备，如 /dev/ttyACM0")
    ap.add_argument("--duration", type=float, default=1800, help="总时长秒（默认 1800）")
    ap.add_argument("--interval", type=float, default=300, help="采样间隔秒（默认 300）")
    ap.add_argument("--cycle", type=float, default=12, help="动作循环周期秒（默认 12）")
    ap.add_argument("--out-dir", default=None,
                    help="产物目录（默认 $SLOOP_TAB5_PROJECT/build/verify/）")
    args = ap.parse_args()

    inject.require_deps()
    import serial

    project = os.environ.get("SLOOP_TAB5_PROJECT") or str(SCRIPTS_DIR.parent / "firmware")
    out_dir = Path(args.out_dir) if args.out_dir else Path(project) / "build" / "verify"
    out_dir.mkdir(parents=True, exist_ok=True)

    stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
    log_path = out_dir / f"soak-{stamp}.log"
    sum_path = out_dir / f"soak-summary-{stamp}.txt"
    ser = inject.LoggedSerial(serial.Serial(args.port, inject.BAUD, timeout=0.05), log_path)
    s = inject.Session(ser, out_dir, stamp, "soak", timeout=3.0)

    rows = []
    problems = []
    print(f"== soak {args.duration:.0f}s @ {args.port}（采样 {args.interval:.0f}s，循环 {args.cycle:.0f}s）")
    print(f"   串口原始日志: {log_path}")
    try:
        s.wait_ready()
        ser.note(f"soak 开始 duration={args.duration:.0f}s interval={args.interval:.0f}s")
        s.cmd("ENG AUDIO")
        s.expect(r"ENG AUDIO streaming=1", "soak 前置：音频流已开")
        switch(s, "transport")
        ensure_playing(s)

        t0 = time.monotonic()
        next_sample = 0.0
        n = 0
        errors = 0
        while True:
            elapsed = time.monotonic() - t0
            if elapsed >= args.duration:
                break
            if elapsed >= next_sample:
                next_sample += args.interval
                row = sample(s)
                row["t"] = round(elapsed)
                rows.append(row)
                print(f"   t={row['t']:>4}s internal_free={row['internal_free']:>6}B "
                      f"min={row['internal_min']:>6}B xrun={row['xrun']} "
                      f"audio_stack_free={row['audio_stack_free']} "
                      f"key_mean={row['key_mean_us']}us key_max={row['key_max_us']}us "
                      f"page={row['page']} bpm={row['bpm']}")
                if row["page"] != PAGES["transport"]:
                    s.fail(f"soak 页面漂移：page={row['page']}（期望 3=transport）")
            try:
                cycle(s, n)
                errors = 0
            except inject.InjectError as e:
                s.fail(f"动作循环 {n} 异常：{e}")
                errors += 1
                if errors >= 3:
                    raise inject.InjectError(f"连续 {errors} 个动作循环失败：{e}") from None
            n += 1
            wake = t0 + n * args.cycle
            if wake > time.monotonic():
                time.sleep(wake - time.monotonic())

        elapsed = time.monotonic() - t0
        row = sample(s)
        row["t"] = round(elapsed)
        rows.append(row)
        print(f"   t={row['t']:>4}s internal_free={row['internal_free']:>6}B xrun={row['xrun']} "
              f"audio_stack_free={row['audio_stack_free']}")

        # 收尾：停播放、回 PLAY 页；音频流保持产品态（开机即流式）
        switch(s, "transport")
        s.cmd("TAP %d %d" % inject.TRANSPORT_STOP)
        s.expect(r"-> TRANSPORT CMD_STOP", "soak 收尾 STOP")
        switch(s, "play")
        problems = verdict(rows, log_path, elapsed)
        ser.note(f"soak 结束 cycles={n} samples={len(rows)} failures={s.failures} "
                 f"problems={len(problems)}")
    except inject.InjectError as e:
        ser.note(f"soak 异常终止: {e}")
        problems.append(f"串口会话异常：{e}")
    finally:
        ser.close()

    lines = [f"# soak {stamp} port={args.port} duration={args.duration:.0f}s "
             f"interval={args.interval:.0f}s cycle={args.cycle:.0f}s",
             "# t_s internal_free internal_min psram_free streaming xrun audio_stack_free "
             "key_mean_us key_max_us input_stack_free page bpm"]
    for r in rows:
        lines.append("  ".join(str(r[k]) for k in
                               ("t", "internal_free", "internal_min", "psram_free", "streaming",
                                "xrun", "audio_stack_free", "key_mean_us", "key_max_us",
                                "input_stack_free", "page", "bpm")))
    lines.append("")
    lines.append(f"断言失败 {s.failures} 项")
    lines.append("判定问题：无" if not problems else "判定问题：\n- " + "\n- ".join(problems))
    lines.append("VERDICT: " + ("PASS" if not problems and s.failures == 0 else "FAIL"))
    sum_path.write_text("\n".join(lines) + "\n")
    print(f"   摘要: {sum_path}")
    for p in problems:
        print(f"  [FAIL] {p}")
    if s.failures or problems:
        sys.exit(f"[FAIL] soak: {s.failures} 项断言失败，{len(problems)} 项判定问题")
    print(f"[PASS] soak: {len(rows)} 次采样，内部堆 {rows[0]['internal_free']} -> "
          f"{rows[-1]['internal_free']}B，xrun=0，无 panic/WDT")


if __name__ == "__main__":
    main()
