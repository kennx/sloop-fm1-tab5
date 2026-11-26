#!/usr/bin/env python3
"""soak.py — 稳定性能 soak（阶段 3 完成判据 4；阶段 4 长时 soak 复用同一口径）

在真机上循环「播放 → 步编辑 → 音序动作（micro/fill/lock、SONG 页表格导航；可选
--section-capture 的播放中 section A 捕获）→ 选轨/切页 → 键盘音符」，保持 TRANSPORT
页播放中，每 --interval 秒采样一次引擎水位：
    ENG MEM   internal_free / internal_min
    ENG PERF  streaming / xrun / audio_stack_free / render_mean_us / render_max_us /
              duty_mean / duty_max / min_free_since_boot_*
    KEY STATS count / 输入延迟均值峰值 / 输入任务栈余量
    UI MEM   pool_used / pool_free（组作用域：采样时只驻留当前组的 LVGL 池水位，
             另打印分组行 `built`/`sum_delta`；ADR-005 §5）
    STATE SNAP 页面/播放/选轨（行为回归）
    ENG POWER vbus_mv / shunt_uv / cur_ma / temp_c / ina（INA226 + 芯片温度，**仅记录**：
             入采样表与摘要的 min/max，不设门禁；P4-F Task 9，spec A5）
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
    8. 实测时长 ≥ --duration（阶段收口口径 1800s；冒烟可用小值）；
    9. --save-window N（存储写窗口，P4-B）：窗口前 `STORE AUTO OFF`（只量脚本写）→
       `ensure_stopped()` → 等 section/song 的延迟写排空（**必须停止后**：走带中 1s 任务
       拒绝写）→ 取 perf 基线；窗口内做一次 `STORE PROJ SAVE 0` + `STORE PROJ LOAD 0`
       （音频流**不停**），窗口内 xrun 增量 ≤ 16；窗口后 streaming=1；收尾在最终采样后
       恢复 ON；
       窗口外累计 xrun 不得再增长（xrun 是自启动累计值，基线可为非 0）。不带该参数时行为不变
       （每次采样 xrun 必须为 0）；
   10. 跨组池检查：PLAY 稳态下 10 轮「八个非 PLAY 组的代表页各访问一次 → 回 PLAY」，
       `UI MEM` 的 pool_free 首末差与单次下降都 ≤ 128B（组作用域不变量 + R34 实测
       噪声 84B；测量点固定在 PLAY 稳态，避开各页面 TLSF 碎片的涨落）。

产物（默认 $SLOOP_TAB5_PROJECT/build/verify/）：soak-<时间戳>.log（原始串口回显，
同 inject.py 口径：逐字节写盘，唯一非固件字节是 `# [inject] ...` 注记）与
soak-summary-<时间戳>.txt（采样表 + 判定）。退出码 0 = 判定通过。

`--section-capture`（默认关）每 SEQ_ACTION_EVERY 轮在**播放中** `SEQ SECTION A STORE`
（**破坏性**：覆盖工程槽 A）：捕获只改 RAM 槽与 dirty，**flash 由 1s 任务在停止 + 空闲
门限后延迟写**；save 窗口与 soak 收尾都在 `ensure_stopped()` 之后 `dirty=0` 才继续
（走带中 tick 拒绝写，等在这里只会假 FAIL）。只用于压 sections 写窗口的门禁。

用法：python3 scripts/soak.py /dev/ttyACM0 [--duration 1800] [--interval 300] [--cycle 12]
                              [--save-window 0] [--section-capture]
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
         "sound": 6, "sample": 8,                                    # id 7 = 空位（FM6 页已删）
         "projects": 9, "presets": 10, "song": 11,
         "dsyn": 12, "fm6p": 13, "system": 14,
         "tools": 18}                # 13 页：5B-1 退役 GLOBAL/FX/MOD/SCALE/VOICE（id 4/5/15/16/17 不注册）
# 组表（与 ui_nav.h 的 UI_GROUP_LIST 同源）：组顺序 = 组签顺序，成员顺序 = 子签顺序 = TAB
# 扁平轮转顺序（NAV_ORDER，取 inject 的同一张表）。PAGES / NAV_ORDER / GROUP_PAGES 必须说
# 同一张导航——import 时校验；与固件源码（ui_nav.h 的组表与页签几何 104/176/5）的一致性由
# soak-selftest.py 对源码校验。
GROUP_PAGES = {"play": (0,), "tracks": (1,), "edit": (6,), "mix": (18,),
               "synth": (8, 12, 13), "seq": (2, 3), "song": (11,), "file": (9, 10), "sys": (14,)}
NAV_ORDER = tuple(inject.NAV_ORDER)   # ((页 id, 页名) …)，13 页
TAB_X0, TAB_GROUP_W, TAB_GROUP_GAP = inject.UI_TAB_X0, inject.UI_GROUP_W, inject.UI_TAB_GAP   # 16 / 104 / 8
TAB_SUB_W, TAB_SUB_SLOTS = inject.UI_SUB_W, inject.UI_GROUP_SLOTS                            # 176 / 5
if ([pid for members in GROUP_PAGES.values() for pid in members] != [pid for pid, _ in NAV_ORDER]
        or list(GROUP_PAGES) != [g.lower() for g in inject.UI_GROUP_NAMES]):
    raise SystemExit("soak 页表不一致：GROUP_PAGES 平铺 ≠ NAV_ORDER（5B-1 定稿应为 13 页 / 9 组）")
HEAP_FLOOR = 60 * 1024          # AGENTS §5.5 内存门禁：internal_free / internal_min 下限
HEAP_NET_FLOOR = -8 * 1024      # 采样首末差下限（30min 内累计下降超过 8KB 视为泄漏）
HEAP_JITTER = 2 * 1024          # 连续 3 个采样窗口的非增累计下滑容差
DUTY_BASELINE_PCT = 9.44        # 阶段 2 静默渲染基线（2-engine-2026-11-20/task-4-audio-perf.log）
DUTY_MAX_PCT = 2 * DUTY_BASELINE_PCT
STACK_FLOOR = 2048
KEY_EVENTS_PER_CYCLE = 2        # 每轮 KEY down + up
SAVE_WINDOW_XRUN_MAX = 16       # --save-window 内允许的 xrun 增量（flash 擦写不饿死音频）
POOL_RT_NOISE = 128             # 跨组往返 pool_free 容差（R34 实测 84B；nav ⑧ 同口径）
POOL_RT_ROUNDS = 10             # 测量轮数（每轮访问 pool_rt_pages(轮) 的 8 张代表页各一次）
# 八个非 PLAY 组各取代表页（组名 → 页名，或页名元组 = 按轮交替；P4-F Task 7 定稿：九组
# 全覆盖——TRACKS 入表；EDIT 是 SOUND：页名 `sound`，`analog`/`fm6` 是它的 console 别名；MIX 自
# 5B-1 起只有 TOOLS，不再按轮交替）。代表页必须属于它代表的组，且每张在 POOL_RT_ROUNDS 里
# 被测同样多轮（import 时校验）。轮数与阈值不变。
POOL_RT_REP = {"tracks": "tracks", "edit": "sound", "mix": "tools", "synth": "dsyn",
               "seq": "transport", "song": "song", "file": "projects", "sys": "system"}


def pool_rt_pages(round_index: int = 0) -> tuple:
    """第 round_index 轮的代表页（组顺序 = GROUP_PAGES；多代表页的组按轮下标取模交替）。"""
    return tuple(pages[round_index % len(pages)] if isinstance(pages, tuple) else pages
                 for g, pages in POOL_RT_REP.items() if g != "play")


POOL_RT_GROUPS = pool_rt_pages(0)   # 每轮的代表页（nav/AGENTS 提到的基名；各组都是单代表页）
POOL_RT_DESC = "→".join(
    f"{g.upper()}:{'/'.join(p) if isinstance(p, tuple) else p}" for g, p in POOL_RT_REP.items())
if (set(POOL_RT_REP) != set(GROUP_PAGES) - {"play"}
        or any(PAGES[page] not in GROUP_PAGES[group]
               for group, pages in POOL_RT_REP.items()
               for page in (pages if isinstance(pages, tuple) else (pages,)))
        or any(POOL_RT_ROUNDS % (len(p) if isinstance(p, tuple) else 1) for p in POOL_RT_REP.values())):
    raise SystemExit("soak 代表页不一致：POOL_RT_REP 须覆盖八个非 PLAY 组、页属于该组、"
                     "且多代表页的组在 POOL_RT_ROUNDS 内各轮被测同样多轮")
PANIC_TOKENS = ("panic", "Guru Meditation", "task_wdt", "abort()", "Backtrace", "assert failed")


def kv_int(text: str) -> dict:
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def kv_raw(text: str) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", text))


def kv_float(raw: dict, key: str) -> float:
    return float(str(raw.get(key, "nan")).rstrip("%"))


POWER_LINE = re.compile(r"^ENG POWER vbus_mv=(-?\d+|na) shunt_uv=(-?\d+|na) cur_ma=(-?\d+|na) "
                        r"temp_c=(-?\d+\.\d|na) ina=(ok|err)\r?$", re.MULTILINE)


def parse_power(text: str) -> dict:
    """`ENG POWER` 行 → 采样列（na / 行缺失或畸形 ⇒ None；只记录，不判失败）。"""
    m = POWER_LINE.search(text)
    if m is None:
        return {"vbus_mv": None, "shunt_uv": None, "cur_ma": None, "temp_c": None, "ina": None}

    def num(v, cast):
        return None if v == "na" else cast(v)
    return {"vbus_mv": num(m.group(1), int), "shunt_uv": num(m.group(2), int),
            "cur_ma": num(m.group(3), int), "temp_c": num(m.group(4), float), "ina": m.group(5)}


def fmt_power(v) -> str:
    return "na" if v is None else str(v)


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
    # ENG POWER 放在 STATE SNAP 之前：playing 的断言读的是 s.last，必须还是 STATE 的回显
    s.cmd("ENG POWER")
    power = parse_power(s.last)
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
        **power,
    }


def switch(s, page: str) -> None:
    """同步切页并核验回显页 == 请求页（切页确定性回归）。"""
    s.cmd(f"UI {page}")
    s.expect(rf"UI page={page} \({PAGES[page]}\)", f"UI {page}")
    s.cmd("STATE SNAP")
    s.expect_int(r"page=(\d+)", PAGES[page], f"STATE page（{page}）")


GROUPS = ("play", "tracks", "edit", "mix", "synth", "seq", "song", "file", "sys")  # 九组（P4-F Task 4）


def visit_all_pages(s) -> None:
    """采样前把 13 页都访问一遍（每页的首次构建都过一次分配面），并逐个点过九个组签
    （与物理组签同一动作函数），最终停在 TRANSPORT——组作用域（ADR-005 §5）下采样时
    只驻留 SEQ 组的页面；`UI MEM` 的分组行给出该组的 built/sum_delta。"""
    for group in GROUPS:
        s.cmd(f"UI TAB {group}")
        m = re.search(rf"UI TAB (\w+) page=(\w+) \((\d+)\)", s.last)
        # 点组落在该组的某个成员页上（「上次子页」记忆 ⇒ 不假定是哪一张）
        if m is None or int(m.group(3)) not in GROUP_PAGES[group]:
            s.fail(f"UI TAB {group} 未落在组成员页：{s.last.strip()!r}")
        else:
            s.ok(f"UI TAB {group} → {m.group(2)} ({m.group(3)}) 是 {m.group(1)} 的成员页")
    for _, page in NAV_ORDER:   # TAB 扁平顺序访问 13 页（每页首次构建都过一次分配面）
        switch(s, page)
    switch(s, "transport")


def ensure_playing(s) -> None:
    s.cmd("STATE SNAP")
    if s.snap_int(r"playing=(\d)") == 0:
        s.cmd("TAP %d %d" % inject.TRANSPORT_PLAY)
        s.expect(r"-> TRANSPORT CMD_PLAY", "TAP PLAY（恢复播放）")


def wait_playing(s, timeout: float = 2.0) -> None:
    """走带请求只由渲染循环消费：轮询到 playing=1（否则紧随的 sample() 会假红）。"""
    deadline = time.monotonic() + timeout
    while True:
        s.cmd("STATE SNAP")
        if s.snap_int(r"playing=(\d)") == 1:
            return
        if time.monotonic() >= deadline:
            s.fail("soak 恢复播放超时（2s 内 playing 仍为 0）")
            return
        time.sleep(0.05)


def perf_counters(s):
    """ENG PERF 的 (xrun, streaming)。不要求走带播放——save 窗口在停走带时取边界，
    此时 sample() 的 playing=1 断言不适用。"""
    s.cmd("ENG PERF")
    perf = kv_raw(s.last)
    return int(perf.get("xrun", -1)), perf.get("streaming", "?")


def ui_pool_free(s) -> int:
    s.cmd("UI MEM")
    return kv_int(s.last).get("pool_free", -1)


def pool_round_trip(s) -> dict:
    """组作用域池检查（PLAY 稳态口径，与 nav ⑧ / song ⑩ 同源）：每轮把八个非 PLAY 组
    （TRACKS/EDIT/MIX/SYNTH/SEQ/SONG/FILE/SYS）的代表页各访问一次再回 PLAY，**测量点固定在 PLAY 稳态**（离开组即销毁，PLAY 的池
    水位不含任何页面的 TLSF 碎片涨落）。R34 的每往返 ~268B 泄漏（styles_init 每页只跑
    一次）会被 10 轮累计显著超过；首末差与单次下降都 ≤ POOL_RT_NOISE。

    先做一轮预热：跨组销毁后的首次重建会做一次性文本/字形缓存（Task 11 §3 实测
    ~400B），不是逐次泄漏。"""
    s.ser.note(f"soak 池检查：PLAY 稳态下 {POOL_RT_ROUNDS} 轮（组:代表页 {POOL_RT_DESC} →play；"
               "多代表页的组按轮交替）")
    for r in range(2):             # 预热：每个代表页（含交替页）的首次重建
        for page in pool_rt_pages(r):
            switch(s, page)
    switch(s, "play")
    start = ui_pool_free(s)
    values = [start]
    for trip in range(POOL_RT_ROUNDS):
        for page in pool_rt_pages(trip):
            switch(s, page)
        switch(s, "play")
        values.append(ui_pool_free(s))
    steps = [b - a for a, b in zip(values, values[1:])]
    pool = {"start": start, "end": values[-1], "delta": values[-1] - start,
            "min_step": min(steps) if steps else 0, "rounds": POOL_RT_ROUNDS,
            "values": values}
    print(f"   池检查（PLAY 稳态 ×{POOL_RT_ROUNDS} 轮）: pool_free {start} -> {values[-1]} "
          f"(Δ={pool['delta']}B, 单次最小 {pool['min_step']}B, 容差 ±{POOL_RT_NOISE}B)")
    print(f"   逐轮: {values}")
    switch(s, "transport")   # 采样断言要求当前页 = TRANSPORT（池检查的最后一轮停在 PLAY）
    return pool


# sections/song 延迟写的 dirty 位（严格匹配回显；解析失败即 fail，不用子串猜测）
SECTION_DIRTY = re.compile(r"^SEQ SECTION ready=0x[0-9A-F]{2} live=\S+ dirty=(\d) ",
                           re.MULTILINE)
SONG_DIRTY = re.compile(r"^STORE SONG count=\d+ loop=\d+ dirty=(\d)\r?$", re.MULTILINE)


def parse_dirty(s, cmd: str, pattern, timeout: float = 3.0) -> int:
    """发一条回显带 dirty 的诊断命令并解析（-1 = 回显不符，已记 fail）。"""
    s.cmd(cmd, timeout=timeout)
    m = pattern.search(s.last)
    if m is None:
        s.fail(f"dirty 回显异常（{cmd}）：{s.last.strip()!r}")
        return -1
    return int(m.group(1))


def dirty_state(s):
    """(section dirty, song dirty)；任一解析失败返回 (-1, -1)。"""
    d_sec = parse_dirty(s, "SEQ SECTION", SECTION_DIRTY)
    d_song = parse_dirty(s, "STORE SONG", SONG_DIRTY)
    return d_sec, d_song


def wait_dirty_clear(s, timeout: float = 12.0, expect_dirty: bool = False) -> bool:
    """**停止态**等 1s 任务把 section/song 的延迟写落盘（返回 True = dirty=0）。

    1s 任务的 sections_tick 在 `song.playing || transport_req || !audio_quiet()` 时直接
    跳过——走带中调用这里只会空等到超时（假 FAIL）。调用点必须在 ensure_stopped 之后、
    perf 基线之前。
    `expect_dirty=True` 时要求先观察到一次 dirty=1（证明这次排空不是空转）。"""
    t0 = time.monotonic()
    seen_dirty = False
    while True:
        d_sec, d_song = dirty_state(s)
        if d_sec < 0 or d_song < 0:
            return False
        if d_sec or d_song:
            seen_dirty = True
        else:
            if expect_dirty and not seen_dirty:
                s.fail("wait_dirty_clear：期望先观察到 dirty=1，实际一直为 0（排空是空转）")
                return False
            print(f"   save 窗口前 dirty=0（{time.monotonic() - t0:.1f}s"
                  f"{'，已观察到 dirty=1' if seen_dirty else ''}）")
            return True
        if time.monotonic() - t0 > timeout:
            s.fail(f"停止后 {timeout:.0f}s 内 section/song dirty 未清"
                   f"（section={d_sec} song={d_song}）")
            return False
        time.sleep(0.3)


def save_window(s, seconds: float, take_sample, section_capture: bool = False) -> dict:
    """--save-window：停走带 → 排空延迟写 → 一次 STORE PROJ SAVE 0 + LOAD 0 → 保持 N 秒 →
    恢复播放。

    `section_capture` 时先在**播放中**捕获一次 section A（dirty=1；播放中允许），再停走带
    等 1s 任务把延迟写排空——这同时是「先停再等」顺序的运行时证明（走带中 tick 拒绝写）。
    排空之后才取 perf 基线，窗口只量脚本化的 SAVE/LOAD。
    """
    s.ser.note(f"soak save 窗口：停走带 SAVE 0 + LOAD 0，保持 {seconds:.0f}s")
    # 窗口判据只量脚本化的 SAVE+LOAD：自动存档若在窗口里插一次写，xrun 会记到窗口账上。
    # AUTO 在窗口前关、整个 soak 期间保持关（收尾时恢复默认 ON，见 main）。
    s.cmd("STORE AUTO OFF")
    s.expect(r"^STORE AUTO off", "save 窗口：STORE AUTO OFF（窗口只量脚本写）")
    pre_dirty = False
    if section_capture:
        s.cmd("SEQ SECTION A STORE")
        s.expect(r"^SEQ SECTION A STORE OK", "save 窗口：播放中捕获 section A（故意置 dirty）")
        d_sec, _ = dirty_state(s)
        pre_dirty = d_sec == 1
        (s.ok if pre_dirty else s.fail)(
            f"save 窗口：播放中捕获后 section dirty={d_sec}（期望 1，待停止后排空）")
    inject.ensure_stopped(s)          # 1s 任务只在停止态写：必须先停
    wait_dirty_clear(s, expect_dirty=pre_dirty)   # 停止后把延迟写排空，再取基线
    x0, st0 = perf_counters(s)
    s.cmd("STORE PROJ SAVE 0")
    s.expect(r"PROJ 0 SAVED hash=[0-9A-F]{8}", "save 窗口：STORE PROJ SAVE 0")
    s.cmd("STORE PROJ LOAD 0")
    s.expect(r"PROJ 0 LOADED hash=[0-9A-F]{8}", "save 窗口：STORE PROJ LOAD 0")
    time.sleep(seconds)
    x1, st1 = perf_counters(s)
    ensure_playing(s)
    wait_playing(s)
    row = take_sample()
    win = {"seconds": seconds, "xrun_before": x0, "xrun_after": x1, "delta": x1 - x0,
           "streaming_before": st0, "streaming_after": st1, "row_t": row["t"]}
    print(f"   save 窗口 {seconds:.0f}s: xrun {x0} -> {x1}（增量 {win['delta']} ≤ "
          f"{SAVE_WINDOW_XRUN_MAX}）, streaming={st1}, 恢复后 t={row['t']}s")
    return win


SEQ_ACTION_EVERY = 3   # 每 N 轮做一次 live section 捕获（其余轮只做轻量音序动作）


def sequence_cycle(s, n: int, section_capture: bool) -> None:
    """音序动作（P4-C Task 12）：micro/fill/lock 写后清（不累积）+ SONG 页表格导航；
    `section_capture` 时每 SEQ_ACTION_EVERY 轮在**播放中**捕获一次 section A（**破坏性**：
    覆盖工程槽 A）。捕获只置 RAM 槽与 dirty；flash 由 1s 任务在**停止 + 空闲门限后**
    延迟写——save 窗口与 soak 收尾都先 `ensure_stopped()` 再等 `dirty=0`。"""
    track, step = ((0, 2), (1, 5))[n % 2]
    s.cmd(f"SEQ MICRO {track} {step} {2 if n % 2 else -2}")
    s.expect(rf"SEQ MICRO track={track} step={step} value=-?\d+ OK", "SEQ MICRO")
    s.cmd(f"SEQ FILL {track} {step} 1")
    s.expect(rf"SEQ FILL track={track} step={step} value=1 OK", "SEQ FILL")
    s.cmd(f"SEQ LOCK SET {track} {step} 1 50")
    s.expect(rf"SEQ LOCK SET track={track} step={step} param=1 value=50 OK", "SEQ LOCK SET")
    s.cmd(f"SEQ LOCK DEL {track} {step} 1")
    s.expect(rf"SEQ LOCK DEL track={track} step={step} param=1 OK", "SEQ LOCK DEL")
    switch(s, "song")
    s.cmd("TAP %d %d" % inject.song_card_xy(n % 4))
    s.expect(r"-> SONG card=[A-D] select", "TAP SONG card")
    s.cmd("TAP %d %d" % inject.song_entry_xy(0))
    s.expect(r"-> SONG entry=0 select", "TAP SONG entry 0")
    if section_capture and n % SEQ_ACTION_EVERY == 0:
        s.cmd("SEQ SECTION A STORE")
        s.expect(r"SEQ SECTION A STORE OK", "播放中 section 捕获（live STORE）")


def cycle(s, n: int, section_capture: bool = False) -> None:
    """一轮：保持播放 → STEP 翻一格 → 音序动作（micro/fill/lock + SONG 页）→ TRACKS 选卡
    → PLAY 弹一个音 → 回 TRANSPORT。"""
    ensure_playing(s)

    switch(s, "step")
    cells = (inject.STEP_CELL_A, inject.STEP_CELL_B)
    track, step = ((0, 2), (1, 5))[n % 2]
    s.cmd("TAP %d %d" % cells[n % 2])
    s.expect(rf"-> STEP track={track} step={step} CMD_TOGGLE_STEP", f"TAP STEP {track}/{step}")

    sequence_cycle(s, n, section_capture)

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


POWER_COLS = ("vbus_mv", "shunt_uv", "cur_ma", "temp_c", "ina")


def power_record(rows: list) -> list:
    """功耗/温度的记录行（仅记录，不设门禁；spec A5）。na = 该次读数无效或行缺失。"""
    def span(key: str, unit: str) -> str:
        vals = [r.get(key) for r in rows if r.get(key) is not None]
        if not vals:
            return f"{key} na（0/{len(rows)} 次有效）"
        return f"{key} {min(vals)}..{max(vals)} {unit}（{len(vals)}/{len(rows)} 次有效）"
    ina = [r.get("ina") for r in rows]
    temps = [r["temp_c"] for r in rows if r.get("temp_c") is not None]
    trend = f"，首 {temps[0]} → 末 {temps[-1]}" if temps else ""
    return [f"功耗记录（仅记录，不设门禁）：INA226 ina=ok {ina.count('ok')}/{len(rows)} 次；"
            + "；".join(span(k, u) for k, u in (("vbus_mv", "mV"), ("shunt_uv", "µV"), ("cur_ma", "mA"))),
            f"温度记录（仅记录，不设门禁）：{span('temp_c', '℃')}{trend}"]


def verdict(rows: list, log_path: Path, elapsed: float, requested: float,
            window: dict = None, pool: dict = None) -> list:
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
        window_free = free[i:i + 3]
        if window_free[0] >= window_free[1] >= window_free[2] and \
                window_free[0] - window_free[2] > HEAP_JITTER:
            problems.append(f"内部堆连续 3 次非增下滑 {window_free}（> {HEAP_JITTER}B）")
    if free[-1] - free[0] <= -HEAP_JITTER and all(b <= a for a, b in zip(free, free[1:])):
        problems.append(f"内部堆全程非增（含平段、无回升），净 {free[-1] - free[0]}B：{free}")
    if window is None:
        for r in rows:
            if r["xrun"] != 0:
                problems.append(f"t={r['t']}s xrun={r['xrun']}")
    else:
        # xrun 是自启动累计计数：判据是窗口内**增量**（基线可以是之前会话留下的计数，
        # 例如同一会话先跑过 SELFTEST/注入场景），窗口后冻结在 win_after。
        if window["delta"] > SAVE_WINDOW_XRUN_MAX:
            problems.append(f"save 窗口内 xrun 增量 {window['delta']} > {SAVE_WINDOW_XRUN_MAX}")
        if window["streaming_after"] != "1":
            problems.append(f"save 窗口后 streaming={window['streaming_after']}")
        for r in rows:
            if r["xrun"] > window["xrun_after"]:
                problems.append(f"t={r['t']}s save 窗口外新增 xrun：{r['xrun']} > "
                                f"窗口后基线 {window['xrun_after']}")
    for r in rows:
        if r["streaming"] != "1":
            problems.append(f"t={r['t']}s 未流式输出")
        if 0 <= r["audio_stack_free"] < STACK_FLOOR:
            problems.append(f"t={r['t']}s 音频任务栈余量 {r['audio_stack_free']}B < {STACK_FLOOR}B")
        if math.isnan(r["duty_mean"]):
            problems.append(f"t={r['t']}s ENG PERF 缺占空比字段（duty_mean）")
        elif r["duty_mean"] > DUTY_MAX_PCT:
            problems.append(f"t={r['t']}s 占空比 duty_mean={r['duty_mean']:.2f}% > "
                            f"2×基线 {DUTY_MAX_PCT:.2f}%")
    if pool is not None:
        if pool["delta"] < -POOL_RT_NOISE:
            problems.append(f"跨组往返池首末差 {pool['delta']}B（< -{POOL_RT_NOISE}B）")
        if pool["min_step"] < -POOL_RT_NOISE:
            problems.append(f"跨组往返单次最大下降 {-pool['min_step']}B（> {POOL_RT_NOISE}B）")
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
    ap.add_argument("--save-window", type=float, default=0.0,
                    help="存储写窗口秒数：窗口内停走带做一次 STORE PROJ SAVE 0 + LOAD 0，"
                         "断言窗口内 xrun 增量 ≤16、窗口后 streaming=1（0 = 关闭，默认）")
    ap.add_argument("--section-capture", action="store_true",
                    help="每 3 轮在播放中 STORE section A（**破坏性**：覆盖工程槽 A；默认关）；"
                         "flash 由 1s 任务在停止后延迟写，窗口/收尾先排空 dirty=0")
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
    pool = None
    window = None
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
        visit_all_pages(s)  # 13 页各访问一次后采样（组作用域：只驻留 SEQ 组）
        ensure_playing(s)
        pool = pool_round_trip(s)  # 跨组往返的池水位（组作用域不变量）

        t0 = time.monotonic()
        next_sample = 0.0
        n = 0
        errors = 0
        last_key_count = None
        expected_events = 0  # 距上次采样的期望 KEY 事件数（每轮 down+up）
        window = None

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
                  f"page={row['page']} bpm={row['bpm']} "
                  f"vbus_mv={fmt_power(row['vbus_mv'])} temp_c={fmt_power(row['temp_c'])}")
            if row["page"] != PAGES["transport"]:
                s.fail(f"soak 页面漂移：page={row['page']}（期望 3=transport）")
            return row

        if args.save_window > 0:
            window = save_window(s, args.save_window, take_sample, args.section_capture)
            next_sample = args.interval  # 窗口后的采样已经取过，下一次按间隔

        while True:
            elapsed = time.monotonic() - t0
            if elapsed >= args.duration:
                break
            if elapsed >= next_sample:
                next_sample += args.interval
                take_sample()
            try:
                cycle(s, n, args.section_capture)
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

        # 收尾：停播放、等延迟写排空（teardown 证明 dirty=0）、回 PLAY 页；
        # 音频流保持产品态（开机即流式）
        switch(s, "transport")
        s.cmd("TAP %d %d" % inject.TRANSPORT_STOP)
        s.expect(r"-> TRANSPORT CMD_STOP", "soak 收尾 STOP")
        # STOP 由渲染循环消费：先确认 playing 回 0（wait_playing 等的是 1，不能复用）
        (s.ok if inject.seq_wait_playing(s, 0, 2.0) else s.fail)(
            "soak 收尾：STOP 后 2s 内 playing=0")
        wait_dirty_clear(s)            # 停止后排空 --section-capture 的收尾 dirty
        switch(s, "play")
        if window is not None:
            # save 窗口关掉的自动存档在**最终采样之后**恢复默认 ON（此后的写不再被采样，
            # 不会把 xrun 记到窗口账上）；设备回到产品默认态。
            s.cmd("STORE AUTO ON")
            s.expect(r"^STORE AUTO on", "soak 收尾：STORE AUTO ON（恢复默认）")
        problems = verdict(rows, log_path, elapsed, args.duration, window, pool)
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
             "key_count key_mean_us key_max_us input_stack_free ui_pool_free page bpm "
             + " ".join(POWER_COLS)]
    for r in rows:
        lines.append("  ".join([str(r[k]) for k in
                                ("t", "internal_free", "internal_min", "psram_free", "streaming",
                                 "xrun", "audio_stack_free", "render_blocks", "render_mean_us",
                                 "render_max_us", "duty_mean", "duty_max", "key_count",
                                 "key_mean_us", "key_max_us", "input_stack_free", "ui_pool_free",
                                 "page", "bpm")]
                               + [fmt_power(r.get(k)) for k in POWER_COLS]))
    lines.append("")
    lines.append(f"内存判定：HEAP_FLOOR={HEAP_FLOOR}B（internal_free 与 internal_min），"
                 f"HEAP_NET_FLOOR={HEAP_NET_FLOOR}B（首末差），"
                 f"HEAP_JITTER={HEAP_JITTER}B（连续 3 采样非增累计下滑容差）")
    lines.append(f"占空比判定：基线 {DUTY_BASELINE_PCT:.2f}%（阶段 2 静默渲染），"
                 f"上限 2× = {DUTY_MAX_PCT:.2f}%（流式 render_mean_us / 块预算 5804µs）")
    if pool is not None:
        lines.append(f"池检查：PLAY 稳态 ×{pool['rounds']} 轮（组:代表页 " + POOL_RT_DESC + "，按轮交替"
                     + f" →play）；pool_free {pool['start']} -> {pool['end']}"
                     f"（Δ={pool['delta']}B，单次最小 {pool['min_step']}B，容差 ±{POOL_RT_NOISE}B）")
        lines.append("逐轮 pool_free: " + " ".join(str(v) for v in pool["values"]))
    else:
        lines.append("池检查：未执行")
    if window is not None:
        lines.append(f"save 窗口：{window['seconds']:.0f}s，STORE PROJ SAVE 0 + LOAD 0（停走带），"
                     f"xrun {window['xrun_before']} -> {window['xrun_after']}"
                     f"（增量 {window['delta']} ≤ {SAVE_WINDOW_XRUN_MAX}），"
                     f"窗口后 streaming={window['streaming_after']}")
    else:
        lines.append("save 窗口：关闭（xrun 必须为 0）")
    lines.append(f"实测时长 {measured}s / 请求 {args.duration:.0f}s")
    lines.extend(power_record(rows))
    lines.append(f"断言失败 {s.failures} 项")
    lines.append("判定问题：无" if not problems else "判定问题：\n- " + "\n- ".join(problems))
    lines.append("VERDICT: " + ("PASS" if not problems and s.failures == 0 else "FAIL"))
    sum_path.write_text("\n".join(lines) + "\n")
    print(f"   摘要: {sum_path}")
    for line in power_record(rows):
        print(f"  [记录] {line}")
    for p in problems:
        print(f"  [FAIL] {p}")
    if s.failures or problems:
        sys.exit(f"[FAIL] soak: {s.failures} 项断言失败，{len(problems)} 项判定问题")
    xrun_text = (f"xrun 窗口增量 {window['delta']}" if window is not None else "xrun=0")
    print(f"[PASS] soak: {len(rows)} 次采样（{measured}s），内部堆 {rows[0]['internal_free']} -> "
          f"{rows[-1]['internal_free']}B，{xrun_text}，duty_mean 最大 "
          f"{max(r['duty_mean'] for r in rows):.2f}%（上限 {DUTY_MAX_PCT:.2f}%），无 panic/WDT")

if __name__ == "__main__":
    main()
