#!/usr/bin/env python3
"""gen_project_fixtures.py — 旧格式工程 fixture（FUN5 / FUN4 / FUN1）与设备 FIELDS 的期望值

P4-B spec §6.3：工程**写** FUN6、**读**兼容 FUN5/4/3/2/1（上游 `project.c` 的 `proj_import`）。
设备侧只有 FUN6 有覆盖；旧格式 fixture 需要从外部灌进 flash。console 单行上限容不下
3,816B，故经 `STORE RAW BEGIN/<hex>/END`（`scripts/store_raw.py`）写入诊断槽 3，再用
`STORE PROJ FIELDS 3` 读回字段逐项比对（见 `scripts/inject/inject.py` 的 store ⑭ 段）。

**字节布局不发明**：结构体与构造器照上游 `third_party/sloop-fm1/firmware/src/project.c`
（`project_v5_t` / `project_v4_t` / `project_v1_t`，`_Static_assert` 钉死尺寸 3840/3112/688）
与 `tests/project_test.c` 的构造器（`fill_old_steps` / `fill_v2_track` 的取值公式）。

**期望值独立于固件**：`fields_from_bytes()` 用 Python 独立实现上游的格式转换
（`proj_from_v5` / `proj_from_v4` / `proj_from_v1`→`proj_trk_v2_to_v3`→`proj_trk_from_v3`），
从**生成的字节**读出 canonical 工程再投影字段。FIELDS 的实现若错（错字段、错偏移、
错转换），比对就会失败；不是用固件的输出去生成期望值。

`--host-check`：再编一个 C 宿主程序（`scripts/fixture_host_check.c`，include 同一份
vendored `project.c`，与 `tests/project_test.c` 同一编译纪律）导入这三个 fixture 并按同一
投影打印字段，与 `expected.json` 比对——把「Python 模型 vs 上游 C 语义」的差异在主机上
暴露，不必占设备轮次。需要 `third_party/sloop-fm1/build/gen` 的生成头（先跑
`scripts/host-test.sh`）。

用法：
    python3 scripts/gen_project_fixtures.py --out DIR
    python3 scripts/gen_project_fixtures.py --out DIR --host-check   # 主机交叉校验
产物：DIR/fun5.bin、DIR/fun4.bin、DIR/fun1.bin、DIR/expected.json
"""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = REPO_ROOT / "third_party" / "sloop-fm1"

# ---- 上游常量（project.c / core.h / params.c，权威在 vendored 源码） --------------
MAGIC = {"fun5": 0x46554E35, "fun4": 0x46554E34, "fun1": 0x46554E31}
FMT = {"fun5": 5, "fun4": 4, "fun1": 1}
SIZES = {"fun5": 3840, "fun4": 3112, "fun1": 688}

NTRK, NSTEP, NPART, TRK_DRUM = 4, 64, 3, 3
PROJ_NG = 32               # project.c: PROJ_NG（G_BPM..G_NEWPRJ；G_DRDLY 在 sel 后的字节）
PROJ_NG_V3 = 27            # 格式 1..3 的全局个数（G_BPM..G_DRREV）
PROJ_NP_V5 = 61
PROJ_NP_V4 = 58
PROJ_NP_V3 = 57
PROJ_NP_V2 = 53
PROJ_NC6 = 53              # P_ITYPE：格式 6 的 16 位参数个数（P_LEVEL..P_VLEAD）
NLOCK = 24
LOCK_FREE = 0xFF
ST_NOTE, ST_TIE, ST_REST = 0, 1, 2
SF_ACCENT = 1

# params.c 的 TP[] 默认值（只列本文件投影到的 id；core.h 的枚举）
P_LEVEL, P_ATK = 0, 1
P_DIST = 33
TP_LEVEL_DEF, TP_ATK_DEF = 104, 10

# ui.c:581 的 TRK_DEF（ANALOG 808 BOOM / DIGITAL RHODES / SAMPLE LOFI FLUTE）；
# 格式 1 的第 2..4 轨用它（proj_from_v1），engine = trk_def_engine(i)，i >= NPART → 0
TRK_DEF = ((0, 0), (1, 0), (4, 5))
# 各引擎 edit[] 前两项的 def（eng_analog.c:172-173 / eng_digital.c:159-160 /
# eng_sample.c:253-254）；格式 1 的默认轨把 P_E0..P_E7 设成引擎 def
ENGINE_E01 = {0: (0, 10), 1: (0, 1), 4: (0, 0)}

STEP = struct.Struct("<4s6B")      # note[4], n, time, flags, vel, lvl, rat（step_t / dstep_t 同尺寸 10B）
STEP8 = struct.Struct("<4s4B")     # note[4], n, time, flags, vel（格式 1..3 的步，8B）
LOCK = struct.Struct("<BBh")       # step, param, val（plock_t，4B）


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def swing_from_v3(v):  # project.c: swing_from_v3: (v * 4 + 2) / 5, 0..100
    return _clamp((v * 4 + 2) // 5, 0, 100)


def dstep_set(blob: bytearray, lane, lvl, rat):
    """drums.c:143 的 dstep_set：lane on，带 level / ratchet（blob 是 10B 的 dstep_t）"""
    sh, b = (lane & 3) * 2, (lane >> 2) & 3
    blob[(lane >> 3) & 1] |= 1 << (lane & 7)
    blob[2 + b] = (blob[2 + b] & ~(3 << sh)) | ((lvl & 3) << sh)
    blob[6 + b] = (blob[6 + b] & ~(3 << sh)) | ((rat & 3) << sh)


# ---------------------------------------------------------------- 构造器（= project_test.c）
def oldv(t, k):
    """project_test.c 的 oldv：旧格式参数 k 在轨 t 上的值"""
    return t * 100 + k * 3 + 1


def old_steps(t):
    """project_test.c fill_old_steps：8 字节步的内容（格式 1..3）"""
    out = []
    for k in range(NSTEP):
        out.append(STEP8.pack(bytes([36 + (k + t) % 40, 38 + k % 5, 0, 0]), k % 3, k % 3, k & 3, 64 + t))
    return b"".join(out)


def new_steps(t):
    """fill_old_steps 的内容经 step_from8 进 10 字节步（无 level / ratchet）"""
    out = []
    for k in range(NSTEP):
        out.append(STEP.pack(bytes([36 + (k + t) % 40, 38 + k % 5, 0, 0]), k % 3, k % 3, k & 3, 64 + t, 0, 0))
    return b"".join(out)


def drum_steps():
    """鼓轨的 16 lane 步：0..7 步有图案（其余空）——FIELDS 投影 mask / 置位数"""
    out = []
    for k in range(NSTEP):
        st = bytearray(10)
        if k % 4 == 0:
            dstep_set(st, 0, 0, 0)
            dstep_set(st, 1, 0, 0)       # LV_NORM
        elif k % 4 == 1:
            dstep_set(st, 4, 3, 0)       # LV_HARD
        elif k % 4 == 3:
            dstep_set(st, 15, 1, 2)      # LV_GHOST + ratchet x3
        out.append(bytes(st))
    return b"".join(out)


def fnv_plug(blob: bytearray, size):
    out = bytearray(blob)
    out += b"\0" * (size - len(out))          # 尾部对齐填充（格式 1 的 sum 在 684）
    struct.pack_into("<I", out, size - 4, fnv1a(bytes(out[: size - 4])))
    return bytes(out)


def build_fun5():
    """project_v5_t：magic/size + g[32] + sel/drdly/rsv[2] + 4×(p[61], engine, preset,
    step[64], micro[64], lock[24], fill[16]) + sum"""
    blob = bytearray()
    blob += struct.pack("<II", MAGIC["fun5"], SIZES["fun5"])
    g = [0] * PROJ_NG
    g[0], g[1], g[27] = 105, 50, 33          # BPM / SWING（FUN5 不缩放）/ DUST
    blob += struct.pack(f"<{PROJ_NG}h", *g)
    blob += struct.pack("<BB2s", 2, 55, b"\0\0")   # sel=2, drdly=55
    for t in range(NTRK):
        p = [t * 10 + i if i < PROJ_NP_V5 - 8 else 100 + t * 10 + i for i in range(PROJ_NP_V5)]
        blob += struct.pack(f"<{PROJ_NP_V5}h", *p)
        blob += struct.pack("<BB", t, 3 + t)           # engine = t, preset = 3+t
        blob += drum_steps() if t == TRK_DRUM else new_steps(t)
        micro = bytearray(NSTEP)
        if t == 0:
            micro[3], micro[4] = 0xE0, 31              # -32 / +31（int8）
        if t == TRK_DRUM:
            micro[9] = 0xF9                            # -7
        blob += bytes(micro)
        locks = bytearray()
        for i in range(NLOCK):
            if t == 1 and i == 0:
                locks += LOCK.pack(3, PROJ_NP_V5 - 8 + 1, 9)   # 格式 5 的 P_E1 → 新 id 64
            elif t == 1 and i == 1:
                locks += LOCK.pack(4, P_DIST, 100)
            else:
                locks += LOCK.pack(LOCK_FREE, 0, 0)
        blob += bytes(locks)
        fill = bytearray(NSTEP // 4)
        if t == 0:
            fill[0] = 0x09                              # 步 1/2：FILL / NOFILL
        if t == TRK_DRUM:
            fill[15] = 0x40                             # 步 64：FILL
        blob += bytes(fill)
    return fnv_plug(blob, SIZES["fun5"])


def build_fun4():
    """project_v4_t：magic/size + g[32] + sel/rsv[3] + 4×(p[58], engine, preset, step[64]) + sum"""
    blob = bytearray()
    blob += struct.pack("<II", MAGIC["fun4"], SIZES["fun4"])
    g = [0] * PROJ_NG
    g[0], g[1], g[27] = 111, 60, 44
    blob += struct.pack(f"<{PROJ_NG}h", *g)
    blob += struct.pack("<B3s", 1, b"\0\0\0")          # sel=1（格式 4 没有 drdly 字节）
    for t in range(NTRK):
        # project_test.c：i < P_TFLT(50) 取 q.p[i]，其后 P_E0..（格式 4 的 P_E0 = 50）
        p = [t * 10 + i if i < PROJ_NP_V4 - 8 else 100 + t * 10 + i for i in range(PROJ_NP_V4)]
        blob += struct.pack(f"<{PROJ_NP_V4}h", *p)
        blob += struct.pack("<BB", 8 if t == 1 else t, 3 + t)
        steps = bytearray(drum_steps() if t == TRK_DRUM else new_steps(t))
        if t == 0:
            steps[3 * 10 + 8] = 0x9C                   # step 3: lvl（每音 2 位）
            steps[3 * 10 + 9] = 0x27                   # step 3: rat
        blob += bytes(steps)
    return fnv_plug(blob, SIZES["fun4"])


def build_fun1():
    """project_v1_t：magic/size + g[27] + 一个轨（p[53], engine, preset, step8[64]）+ 2B 填充 + sum"""
    blob = bytearray()
    blob += struct.pack("<II", MAGIC["fun1"], SIZES["fun1"])
    g = [0] * PROJ_NG_V3
    g[0], g[1], g[2], g[26] = 123, 50, 1, 77           # BPM / SWING / CLICK / DR REV
    blob += struct.pack(f"<{PROJ_NG_V3}h", *g)
    blob += struct.pack(f"<{PROJ_NP_V2}h", *[oldv(0, k) for k in range(PROJ_NP_V2)])
    blob += struct.pack("<BB", 7, 5)                    # WHEEL / preset 5
    blob += old_steps(0)
    assert len(blob) == SIZES["fun1"] - 6, len(blob)   # 没有 sel；sum 前有 2B 对齐填充
    return fnv_plug(blob, SIZES["fun1"])


# ---------------------------------------------------------------- 解码（= 上游 proj_import）
def _decode_steps_synth(blob, off):
    return [STEP.unpack_from(blob, off + i * 10) for i in range(NSTEP)]


def _decode_steps_drum(blob, off):
    out = []
    for i in range(NSTEP):
        st = blob[off + i * 10: off + (i + 1) * 10]
        out.append(st[0] | (st[1] << 8))
    return out


def _plain_track(p, e, engine, preset, steps, drum=False):
    """canonical 轨：p[53]（P_LEVEL..P_VLEAD）、e[8]、x[10]（2.6，格式 <6 取 TP 默认）、
    engine/preset、步（合成：(n, note0)；鼓：lane mask）"""
    return {"p": list(p[:PROJ_NC6]), "e": list(e), "x": [0] * 10,
            "engine": engine, "preset": preset, "steps": steps, "drum": drum}


def decode_fun5(blob):
    assert struct.unpack_from("<I", blob, 0)[0] == MAGIC["fun5"]
    assert struct.unpack_from("<I", blob, 4)[0] == SIZES["fun5"]
    assert fnv1a(blob[: SIZES["fun5"] - 4]) == struct.unpack_from("<I", blob, SIZES["fun5"] - 4)[0]
    g = list(struct.unpack_from(f"<{PROJ_NG}h", blob, 8))
    sel, drdly = blob[8 + PROJ_NG * 2], blob[8 + PROJ_NG * 2 + 1]
    off = 8 + PROJ_NG * 2 + 4
    tracks = []
    for t in range(NTRK):
        p = list(struct.unpack_from(f"<{PROJ_NP_V5}h", blob, off))
        engine, preset = blob[off + PROJ_NP_V5 * 2], blob[off + PROJ_NP_V5 * 2 + 1]
        base = off + PROJ_NP_V5 * 2 + 2
        if t == TRK_DRUM:
            steps = _decode_steps_drum(blob, base)          # memcpy 到 dstep_t
        else:
            steps = [(s[1], s[0][0]) for s in _decode_steps_synth(blob, base)]
        # proj_from_v5：前 53 个 p 原样、后 8 个进 e；2.6 参数取 TP 默认（0）；步原样
        tracks.append(_plain_track(p[:PROJ_NC6], p[PROJ_NP_V5 - 8:], engine, preset, steps,
                                   t == TRK_DRUM))
        off = base + NSTEP * 10 + NSTEP + NLOCK * 4 + NSTEP // 4
    return {"fmt": 5, "g": g, "sel": sel, "drdly": drdly, "tracks": tracks}


def decode_fun4(blob):
    assert struct.unpack_from("<I", blob, 0)[0] == MAGIC["fun4"]
    assert struct.unpack_from("<I", blob, 4)[0] == SIZES["fun4"]
    assert fnv1a(blob[: SIZES["fun4"] - 4]) == struct.unpack_from("<I", blob, SIZES["fun4"] - 4)[0]
    g = list(struct.unpack_from(f"<{PROJ_NG}h", blob, 8))
    sel = blob[8 + PROJ_NG * 2]
    off = 8 + PROJ_NG * 2 + 4
    tracks = []
    for t in range(NTRK):
        p = list(struct.unpack_from(f"<{PROJ_NP_V4}h", blob, off))
        engine, preset = blob[off + PROJ_NP_V4 * 2], blob[off + PROJ_NP_V4 * 2 + 1]
        base = off + PROJ_NP_V4 * 2 + 2
        if t == TRK_DRUM:
            steps = _decode_steps_drum(blob, base)
        else:
            steps = [(s[1], s[0][0]) for s in _decode_steps_synth(blob, base)]
        # proj_from_v4：P_TFLT(50) 之前按 id 拷，其后 TP 默认；e 取格式 4 的 P_E0..（50..57）
        tracks.append(_plain_track([p[k] if k < PROJ_NP_V4 - 8 else 0 for k in range(PROJ_NC6)],
                                   p[PROJ_NP_V4 - 8:], engine, preset, steps, t == TRK_DRUM))
        off = base + NSTEP * 10
    return {"fmt": 4, "g": g, "sel": sel, "drdly": 0, "tracks": tracks}


def _v2_to_v3(p2):
    """proj_trk_v2_to_v3：前 45 个按序；P_ITYPE.. 取 TP 默认（只投影 P_LEVEL/P_ATK），
    后 8 个 p 进 v3 的 P_E0.."""
    return [p2[k] if k < PROJ_NP_V2 - 8 else 0 for k in range(PROJ_NP_V3 - 8)] + \
        list(p2[PROJ_NP_V2 - 8:])


def _v3_to_v6(p3, engine, preset, steps8):
    """proj_trk_from_v3：按 id 到 P_SLDEPTH（49），其后 TP 默认；swing 缩放；
    e 取 v3 的 P_E0..；步 step_from8（无 level / ratchet）"""
    p = [p3[k] if k < PROJ_NP_V3 - 8 else 0 for k in range(PROJ_NC6)]
    p[31] = swing_from_v3(p3[31])                 # P_SSWING
    p[21] = swing_from_v3(p3[21])                 # P_ASWING
    steps = [(s[1], s[0][0]) for s in steps8]
    return _plain_track(p, p3[PROJ_NP_V3 - 8:], engine, preset, steps)


def decode_fun1(blob):
    assert struct.unpack_from("<I", blob, 0)[0] == MAGIC["fun1"]
    assert struct.unpack_from("<I", blob, 4)[0] == SIZES["fun1"]
    assert fnv1a(blob[: SIZES["fun1"] - 4]) == struct.unpack_from("<I", blob, SIZES["fun1"] - 4)[0]
    g = list(struct.unpack_from(f"<{PROJ_NG_V3}h", blob, 8))
    off = 8 + PROJ_NG_V3 * 2
    p2 = list(struct.unpack_from(f"<{PROJ_NP_V2}h", blob, off))
    engine, preset = blob[off + PROJ_NP_V2 * 2], blob[off + PROJ_NP_V2 * 2 + 1]
    base = off + PROJ_NP_V2 * 2 + 2
    steps8 = [STEP8.unpack_from(blob, base + i * 8) for i in range(NSTEP)]
    # proj_g_from_old：前 27 个拷贝，SWING 缩放；其后 GP 默认（只投影 dust=0）
    g6 = [g[i] if i < PROJ_NG_V3 else 0 for i in range(PROJ_NG)]
    g6[1] = swing_from_v3(g[1])
    tracks = [_v3_to_v6(_v2_to_v3(p2), engine, preset, steps8)]
    for t in range(1, NTRK):   # proj_from_v1：其余轨 = 引擎默认声音、preset 0xFF、无步
        eng = TRK_DEF[t][0] if t < NPART else 0
        tracks.append({"p": [TP_LEVEL_DEF, TP_ATK_DEF] + [0] * (PROJ_NC6 - 2),
                       "e": list(ENGINE_E01[eng]) + [0] * 6, "x": [0] * 10,
                       "engine": eng, "preset": 0xFF,
                       "steps": [0] * NSTEP if t == TRK_DRUM
                       else [(0, 0)] * NSTEP, "drum": t == TRK_DRUM})
    return {"fmt": 1, "g": g6, "sel": 0, "drdly": 0, "tracks": tracks}


DECODERS = {"fun5": decode_fun5, "fun4": decode_fun4, "fun1": decode_fun1}
BUILDERS = {"fun5": build_fun5, "fun4": build_fun4, "fun1": build_fun1}


# ---------------------------------------------------------------- FIELDS 投影（console 同序）
def field_keys():
    keys = ["fmt", "sel", "bpm", "swing", "dust", "drdly"]
    for t in range(NTRK):
        keys += [f"t{t}.eng", f"t{t}.pre", f"t{t}.lvl", f"t{t}.atk", f"t{t}.e0", f"t{t}.e1"]
        keys += [f"t{t}.s{i}" for i in range(8)]
    return keys


def fields_from_project(proj):
    """canonical 工程 → {key: value}；值一律是十进制整数（步 = `n:note`，鼓轨 note = lane 掩码）"""
    out = {"fmt": proj["fmt"], "sel": proj["sel"], "bpm": proj["g"][0],
           "swing": proj["g"][1], "dust": proj["g"][27], "drdly": proj["drdly"]}
    for t, trk in enumerate(proj["tracks"]):
        out[f"t{t}.eng"] = trk["engine"]
        out[f"t{t}.pre"] = trk["preset"]
        out[f"t{t}.lvl"] = trk["p"][P_LEVEL]
        out[f"t{t}.atk"] = trk["p"][P_ATK]
        out[f"t{t}.e0"] = trk["e"][0]
        out[f"t{t}.e1"] = trk["e"][1]
        for i in range(8):
            if trk["drum"]:
                mask = trk["steps"][i]
                out[f"t{t}.s{i}"] = f"{bin(mask).count('1')}:{mask}"
            else:
                n, note = trk["steps"][i]
                out[f"t{t}.s{i}"] = f"{n}:{note}"
    return out


def fields_from_bytes(blob, fmt_key):
    return fields_from_project(DECODERS[fmt_key](blob))


def generate(out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    expected = {}
    for key in ("fun5", "fun4", "fun1"):
        blob = BUILDERS[key]()
        assert len(blob) == SIZES[key], (key, len(blob))
        (out_dir / f"{key}.bin").write_bytes(blob)
        expected[key] = fields_from_bytes(blob, key)
    expected_doc = {"order": field_keys(), "fixtures": expected}
    (out_dir / "expected.json").write_text(json.dumps(expected_doc, indent=2) + "\n")
    return expected_doc


# ---------------------------------------------------------------- 主机交叉校验
def host_check(out_dir: Path, cc: str) -> int:
    """编 scripts/fixture_host_check.c（vendored project.c 的 PROJ_HOST 段）导入 fixture，
    打印同一投影，与 expected.json 逐项比对。需要 third_party/sloop-fm1/build/gen。"""
    gen_dir = UPSTREAM / "build" / "gen"
    if not (gen_dir / "felucca_tables.h").is_file():
        print(f"[host-check] 缺少 {gen_dir}/felucca_tables.h：先跑 scripts/host-test.sh",
              file=sys.stderr)
        return 2
    src = REPO_ROOT / "scripts" / "fixture_host_check.c"
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "fixture_host_check"
        cmd = [cc, "-O1", "-w", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off",
               "-I", str(gen_dir), "-I", str(UPSTREAM / "firmware" / "src"),
               "-o", str(exe), str(src), "-lm"]
        subprocess.run(cmd, check=True, cwd=UPSTREAM)
        doc = json.loads((out_dir / "expected.json").read_text())
        keys = doc["order"]
        bad = 0
        for key in ("fun5", "fun4", "fun1"):
            res = subprocess.run([str(exe), str(out_dir / f"{key}.bin")], check=True,
                                 cwd=UPSTREAM, capture_output=True, text=True)
            got = {}
            for tok in res.stdout.split():
                k, _, v = tok.partition("=")
                got[k] = v
            want = {k: str(v) for k, v in doc["fixtures"][key].items()}
            diff = [(k, want.get(k), got.get(k)) for k in keys if want.get(k) != got.get(k)]
            if list(got) != keys:
                print(f"[host-check] {key}: 键顺序/键集不一致（{len(got)} 键）", file=sys.stderr)
                bad += 1
            elif diff:
                print(f"[host-check] {key}: {len(diff)} 项不一致（前 5）：{diff[:5]}", file=sys.stderr)
                bad += 1
            else:
                print(f"[host-check] {key}: {len(keys)} 项与上游 project.c 一致")
        return 0 if bad == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="生成旧格式工程 fixture 与 FIELDS 期望值")
    ap.add_argument("--out", required=True, help="产物目录（写 fun5/fun4/fun1.bin + expected.json）")
    ap.add_argument("--host-check", action="store_true",
                    help="再用 vendored project.c 的宿主程序导入 fixture 并交叉校验期望值")
    ap.add_argument("--cc", default="cc", help="主机 C 编译器（--host-check 用，默认 cc）")
    args = ap.parse_args()
    out_dir = Path(args.out)
    doc = generate(out_dir)
    for key, fields in doc["fixtures"].items():
        print(f"{key}: {SIZES[key]}B  fmt={fields['fmt']} sel={fields['sel']} "
              f"bpm={fields['bpm']} t0={fields['t0.eng']}/{fields['t0.pre']}")
    print(f"写入 {out_dir}/（expected.json: {len(doc['order'])} 项）")
    if args.host_check:
        return host_check(out_dir, args.cc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
