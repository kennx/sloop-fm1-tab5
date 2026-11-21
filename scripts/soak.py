#!/usr/bin/env python3
"""soak.py — 稳定性能 soak（阶段 3 完成判据 4；阶段 4 长时 soak 复用同一口径）

在真机上循环「播放 → 步编辑 → 选轨/切页 → 键盘音符」，保持 TRANSPORT 页播放中，
每 --interval 秒采样一次引擎水位：
    ENG MEM   internal_free / internal_min
    ENG PERF  streaming / xrun / audio_stack_free / render_mean_us / render_max_us /
              duty_mean / duty_max / min_free_since_boot_*
    KEY STATS count / 输入延迟均值峰值 / 输入任务栈余量
    UI MEM   pool_used / pool_free（九页全建后的 LVGL 池水位）
    STATE SNAP 页面/播放/选轨（行为回归）
判定（全部满足才退出码 0）：
    1. 内部堆：全程 internal_free ≥ 60KB（HEAP_FLOOR，AGENTS §5.5 内存门禁）；
       首末差 ≥ HEAP_NET_FLOOR（-8KB）；任一连续 3 个采样窗口「非增且累计下滑
       > HEAP_JITTER（2KB）」视为泄漏趋势（覆盖逐次下降与带平段的下滑）；
       全程非增（含平段、无任何回升）且净下降 ≥ HEAP_JITTER 亦判失败；internal_min ≥ 60KB；
    2. 每次采样 streaming=1 且 xrun=0（流式音频无欠载）；
    3. 渲染占空比 duty_mean ≤ 2× 阶段 2 静默渲染基线 9.44%（= 18.88%），duty_max 记录；
    4. 音频任务栈余量 ≥ 2KB；
    5. KEY STATS count 增量 == 期望事件数（每轮 2 个：down/up）。注意：KEY 注入走
       与物理键盘相同的入口，计数增量证明「console 注入的命令被执行了」，**不**证明
       物理键盘事件链（I2C 轮询/解码）在跑——键盘链路由人工逐键核对（DI-4）；
    6. 原始串口日志无 panic / Guru Meditation / task_wdt / abort / Backtrace；
    7. 每周期的切页回显与 STATE SNAP 页面断言全过（无切页回归）；
    8. 实测时长 ≥ --duration（阶段收口口径 1800s；冒烟可用小值）。

产物（默认 $SLOOP_TAB5_PROJECT/build/verify/）：soak-<时间戳>.log（原始串口回显，
同 inject.py 口径：逐字节写盘，唯一非固件字节是 `# [inject] ...` 注记）与
soak-summary-<时间戳>.txt（采样表 + 判定）。退出码 0 = 判定通过。

用法：python3 scripts/soak.py /dev/ttyACM0 [--duration 1800] [--interval 300] [--cycle 12]
"""
import argparse
import math
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

PAGES = {"play": 0, "tracks": 1, "step": 2, "transport": 3,
         "global": 4, "fx": 5, "analog": 6, "fm6": 7, "sample": 8}
HEAP_FLOOR = 60 * 1024          # AGENTS §5.5 内存门禁：internal_free / internal_min 下限
HEAP_NET_FLOOR = -8 * 1024      # 采样首末差下限（30min 内累计下降超过 8KB 视为泄漏）
HEAP_JITTER = 2 * 1024          # 连续 3 个采样窗口的非增累计下滑容差
DUTY_BASELINE_PCT = 9.44        # 阶段 2 静默渲染基线（2-engine-2026-11-20/task-4-audio-perf.log）
DUTY_MAX_PCT = 2 * DUTY_BASELINE_PCT
STACK_FLOOR = 2048
KEY_EVENTS_PER_CYCLE = 2        # 每轮 KEY down + up
PANIC_TOKENS = ("panic", "Guru Meditation", "task_wdt", "abort()", "Backtrace", "assert failed")


def kv_int(text: str) -> dict:
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def kv_raw(text: str) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", text))


def kv_float(raw: dict, key: str) -> float:
    return float(str(raw.get(key, "nan")).rstrip("%"))


def sample(s) -> dict:
    """一次水位采样（同时是行为回归探针：STATE 页面/播放状态必须符合预期）。"""
    s.cmd("ENG MEM")
    mem = kv_int(s.last)
    s.cmd("ENG PERF")
    perf = kv_raw(s.last)
    s.cmd("KEY STATS")
    stats = kv_int(s.last)
    s.cmd("UI MEM")
    ui_mem = kv_int(s.last)
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
        "render_blocks": int(perf.get("render_blocks", -1)),
        "render_mean_us": int(perf.get("render_mean_us", -1)),
        "render_max_us": int(perf.get("render_max_us", -1)),
        "duty_mean": kv_float(perf, "duty_mean"),
        "duty_max": kv_float(perf, "duty_max"),
        "key_count": stats.get("count", -1),
        "key_mean_us": stats.get("mean_us", -1),
        "key_max_us": stats.get("max_us", -1),
        "input_stack_free": stats.get("input_stack_free", -1),
        "ui_pool_free": ui_mem.get("pool_free", -1),
        "page": state.get("page", -1),
        "bpm": state.get("bpm", -1),
    }


def switch(s, page: str) -> None:
    """同步切页并核验回显页 == 请求页（切页确定性回归）。"""
    s.cmd(f"UI {page}")
    s.expect(rf"UI page={page} \({PAGES[page]}\)", f"UI {page}")
    s.cmd("STATE SNAP")
    s.expect_int(r"page=(\d+)", PAGES[page], f"STATE page（{page}）")


def visit_all_pages(s) -> None:
    """采样前把 9 页都建出来：soak 的池水位/懒建状态与产品实际使用（九页全建）一致，
    避免「采样时只有 4 页已建」掩盖参数页的池压力。"""
    for page in PAGES:
        switch(s, page)
    switch(s, "transport")


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
    # 卡中心 = 卡 2 中心 + (card-2)*卡距（卡距见 inject.TRACKS_CARD_PITCH）
    s.cmd("TAP %d %d" % (inject.TRACKS_CARD2[0] + (card - 2) * inject.TRACKS_CARD_PITCH,
                         inject.TRACKS_CARD2[1]))
    s.expect(rf"-> TRACKS select track={card} CMD_SELECT_TRACK", f"TAP 卡 {card}")

    switch(s, "play")
    s.cmd("KEY 0x42 down")
    s.expect(r"KEY 0x42 down", "KEY 音符 down")
    s.cmd("KEY 0x42 up")
    s.expect(r"KEY 0x42 up", "KEY 音符 up")

    switch(s, "transport")


def verdict(rows: list, log_path: Path, elapsed: float, requested: float) -> list:
    """按判定口径给出问题列表（空 = 通过）。"""
    problems = []
    if len(rows) < 2:
        problems.append(f"采样不足：{len(rows)} 次")
        return problems
    free = [r["internal_free"] for r in rows]
    mins = [r["internal_min"] for r in rows]
    if min(free) < HEAP_FLOOR:
        problems.append(f"内部堆低于 60KB：min={min(free)}")
    if min(mins) < HEAP_FLOOR:
        problems.append(f"自启动最低内部堆低于 60KB：min={min(mins)}")
    if free[-1] - free[0] < HEAP_NET_FLOOR:
        problems.append(f"内部堆首末下降 {free[0]} -> {free[-1]}（阈值 {HEAP_NET_FLOOR}）")
    for i in range(len(free) - 2):
        window = free[i:i + 3]
        if window[0] >= window[1] >= window[2] and window[0] - window[2] > HEAP_JITTER:
            problems.append(f"内部堆连续 3 次非增下滑 {window}（> {HEAP_JITTER}B）")
    if free[-1] - free[0] <= -HEAP_JITTER and all(b <= a for a, b in zip(free, free[1:])):
        problems.append(f"内部堆全程非增（含平段、无回升），净 {free[-1] - free[0]}B：{free}")
    for r in rows:
        if r["streaming"] != "1":
            problems.append(f"t={r['t']}s 未流式输出")
        if r["xrun"] != 0:
            problems.append(f"t={r['t']}s xrun={r['xrun']}")
        if 0 <= r["audio_stack_free"] < STACK_FLOOR:
            problems.append(f"t={r['t']}s 音频任务栈余量 {r['audio_stack_free']}B < {STACK_FLOOR}B")
        if math.isnan(r["duty_mean"]):
            problems.append(f"t={r['t']}s ENG PERF 缺占空比字段（duty_mean）")
        elif r["duty_mean"] > DUTY_MAX_PCT:
            problems.append(f"t={r['t']}s 占空比 duty_mean={r['duty_mean']:.2f}% > "
                            f"2×基线 {DUTY_MAX_PCT:.2f}%")
    text = log_path.read_text(errors="replace")
    hits = [tok for tok in PANIC_TOKENS if tok.lower() in text.lower()]
    if hits:
        problems.append(f"串口日志含异常标记：{hits}")
    if elapsed < requested - 5:
        problems.append(f"实测时长 {elapsed:.0f}s < 请求 {requested:.0f}s")
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

    project = os.environ.get("SLOOP_TAB5_PROJECT") or str(SCRIPTS_DIR.parent / "firmware")
    out_dir = Path(args.out_dir) if args.out_dir else Path(project) / "build" / "verify"
    out_dir.mkdir(parents=True, exist_ok=True)

    stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
    log_path = out_dir / f"soak-{stamp}.log"
    sum_path = out_dir / f"soak-summary-{stamp}.txt"
    # Session 自建 LoggedSerial（DI-5 硬化后的签名）；soak 不用截屏重试
    s = inject.Session(args.port, log_path, out_dir, stamp, "soak", timeout=3.0)

    rows = []
    problems = []
    print(f"== soak {args.duration:.0f}s @ {args.port}（采样 {args.interval:.0f}s，循环 {args.cycle:.0f}s）")
    print(f"   串口原始日志: {log_path}")
    try:
        s.wait_ready()
        s.ser.note(f"soak 开始 duration={args.duration:.0f}s interval={args.interval:.0f}s")
        s.cmd("ENG AUDIO")
        s.expect(r"ENG AUDIO streaming=1", "soak 前置：音频流已开")
        # 步窗是持久的 UI 局部态：soak 的 STEP 注入几何按默认窗口派生，先归一到 0/16
        s.cmd("UI WINDOW 0 16")
        s.expect(r"UI WINDOW bank=0 start=0 len=16", "soak 前置：步窗归一 0/16")
        visit_all_pages(s)  # 九页全建后再采样（池水位 = 产品实际使用）
        ensure_playing(s)

        t0 = time.monotonic()
        next_sample = 0.0
        n = 0
        errors = 0
        last_key_count = None
        expected_events = 0  # 距上次采样的期望 KEY 事件数（每轮 down+up）

        def take_sample() -> dict:
            nonlocal last_key_count, expected_events
            row = sample(s)
            row["t"] = round(time.monotonic() - t0)
            if last_key_count is not None:
                got = row["key_count"] - last_key_count
                if got != expected_events:
                    s.fail(f"KEY 事件计数增量 {got} ≠ 期望 {expected_events}（丢/多事件）")
            last_key_count = row["key_count"]
            expected_events = 0
            rows.append(row)
            print(f"   t={row['t']:>4}s internal_free={row['internal_free']:>6}B "
                  f"min={row['internal_min']:>6}B xrun={row['xrun']} "
                  f"audio_stack_free={row['audio_stack_free']} render_blocks={row['render_blocks']} "
                  f"duty_mean={row['duty_mean']:.2f}% duty_max={row['duty_max']:.2f}% "
                  f"ui_pool_free={row['ui_pool_free']} "
                  f"key_mean={row['key_mean_us']}us key_max={row['key_max_us']}us "
                  f"page={row['page']} bpm={row['bpm']}")
            if row["page"] != PAGES["transport"]:
                s.fail(f"soak 页面漂移：page={row['page']}（期望 3=transport）")
            return row

        while True:
            elapsed = time.monotonic() - t0
            if elapsed >= args.duration:
                break
            if elapsed >= next_sample:
                next_sample += args.interval
                take_sample()
            try:
                cycle(s, n)
                expected_events += KEY_EVENTS_PER_CYCLE
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
        take_sample()

        # 收尾：停播放、回 PLAY 页；音频流保持产品态（开机即流式）
        switch(s, "transport")
        s.cmd("TAP %d %d" % inject.TRANSPORT_STOP)
        s.expect(r"-> TRANSPORT CMD_STOP", "soak 收尾 STOP")
        switch(s, "play")
        problems = verdict(rows, log_path, elapsed, args.duration)
        s.ser.note(f"soak 结束 cycles={n} samples={len(rows)} failures={s.failures} "
                   f"problems={len(problems)}")
    except inject.InjectError as e:
        s.ser.note(f"soak 异常终止: {e}")
        problems.append(f"串口会话异常：{e}")
    finally:
        s.ser.close()

    measured = rows[-1]["t"] if rows else 0
    if not rows:
        problems.append("未采集到任何采样（会话未就绪或首采样前中断）")
    lines = [f"# soak {stamp} port={args.port} requested={args.duration:.0f}s measured={measured}s "
             f"interval={args.interval:.0f}s cycle={args.cycle:.0f}s",
             "# t_s internal_free internal_min psram_free streaming xrun audio_stack_free "
             "render_blocks render_mean_us render_max_us duty_mean duty_max "
             "key_count key_mean_us key_max_us input_stack_free ui_pool_free page bpm"]
    for r in rows:
        lines.append("  ".join(str(r[k]) for k in
                               ("t", "internal_free", "internal_min", "psram_free", "streaming",
                                "xrun", "audio_stack_free", "render_blocks", "render_mean_us",
                                "render_max_us", "duty_mean", "duty_max", "key_count",
                                "key_mean_us", "key_max_us", "input_stack_free", "ui_pool_free",
                                "page", "bpm")))
    lines.append("")
    lines.append(f"内存判定：HEAP_FLOOR={HEAP_FLOOR}B（internal_free 与 internal_min），"
                 f"HEAP_NET_FLOOR={HEAP_NET_FLOOR}B（首末差），"
                 f"HEAP_JITTER={HEAP_JITTER}B（连续 3 采样非增累计下滑容差）")
    lines.append(f"占空比判定：基线 {DUTY_BASELINE_PCT:.2f}%（阶段 2 静默渲染），"
                 f"上限 2× = {DUTY_MAX_PCT:.2f}%（流式 render_mean_us / 块预算 5804µs）")
    lines.append(f"实测时长 {measured}s / 请求 {args.duration:.0f}s")
    lines.append(f"断言失败 {s.failures} 项")
    lines.append("判定问题：无" if not problems else "判定问题：\n- " + "\n- ".join(problems))
    lines.append("VERDICT: " + ("PASS" if not problems and s.failures == 0 else "FAIL"))
    sum_path.write_text("\n".join(lines) + "\n")
    print(f"   摘要: {sum_path}")
    for p in problems:
        print(f"  [FAIL] {p}")
    if s.failures or problems:
        sys.exit(f"[FAIL] soak: {s.failures} 项断言失败，{len(problems)} 项判定问题")
    print(f"[PASS] soak: {len(rows)} 次采样（{measured}s），内部堆 {rows[0]['internal_free']} -> "
          f"{rows[-1]['internal_free']}B，xrun=0，duty_mean 最大 "
          f"{max(r['duty_mean'] for r in rows):.2f}%（上限 {DUTY_MAX_PCT:.2f}%），无 panic/WDT")

if __name__ == "__main__":
    main()
