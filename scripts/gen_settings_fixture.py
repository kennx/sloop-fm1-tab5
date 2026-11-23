#!/usr/bin/env python3
"""生成 P4-E RAW settings/side 设备用 fixture。

per3.bin：88B PER3，palette=3，其余设置为 0，歌曲为上游 arr_defaults。
side.bin：80B side record（40B THM2 theme + FAV1 + 16 个空收藏 + CRC）。
CRC 与 storage.c 的 st_crc32 同为 CRC-32/ISO-HDLC（zlib.crc32）。

用法：python3 scripts/gen_settings_fixture.py --theme NAME [--out DIR]
      python3 scripts/gen_settings_fixture.py --selftest
"""

import argparse
import struct
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = REPO_ROOT / "firmware" / "build" / "verify" / "settings-fixture"
THEME_HI = 0xF81F


def build_per3() -> bytes:
    arr = bytearray(36)
    arr[0] = 4
    for i in range(16):
        arr[4 + 2 * i] = i % 4
        arr[5 + 2 * i] = 4
    return struct.pack("<IIII", 0x50455233, 3, 0, 0) + bytes(32) + bytes(arr) + bytes(4)


def build_side(name: str) -> bytes:
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError as e:
        raise ValueError("theme 名必须是 ASCII") from e
    if not 1 <= len(encoded) <= 8 or any(not 0x20 <= b <= 0x7E for b in encoded):
        raise ValueError("theme 名须为 1..8 个可打印 ASCII 字符")
    theme = bytearray(40)
    struct.pack_into("<I", theme, 0, 0x324D4854)
    theme[4:4 + len(encoded)] = encoded
    colors = (0x0000, 0x4208, 0x8410, 0xC618, 0xFFFF)
    tracks = (0x001F, 0x07E0, 0xFFE0, 0xF800)
    struct.pack_into("<5H", theme, 12, *colors)
    struct.pack_into("<4H", theme, 22, *tracks)
    struct.pack_into("<HHH", theme, 30, 0x0000, THEME_HI, 0)
    struct.pack_into("<I", theme, 36, zlib.crc32(theme[:36]))

    side = bytearray(80)
    side[:40] = theme
    struct.pack_into("<I", side, 40, 0x31564146)
    struct.pack_into("<16H", side, 44, *([0xFFFF] * 16))
    struct.pack_into("<I", side, 76, zlib.crc32(side[40:76]))
    return bytes(side)


def validate(per3: bytes, side: bytes, name: str) -> None:
    assert len(per3) == 88 and len(side) == 80
    magic, palette, lowcut, zoom = struct.unpack_from("<IIII", per3)
    assert (magic, palette, lowcut, zoom) == (0x50455233, 3, 0, 0)
    assert per3[16:48] == bytes(32) and per3[84:88] == bytes(4)
    assert per3[48] == 4 and per3[49] == 0 and per3[50:52] == bytes(2)
    for i in range(16):
        assert per3[52 + 2 * i:54 + 2 * i] == bytes((i % 4, 4))
    assert struct.unpack_from("<I", side, 0)[0] == 0x324D4854
    assert side[4:12] == name.encode("ascii").ljust(8, b"\0")
    assert struct.unpack_from("<I", side, 36)[0] == zlib.crc32(side[:36])
    assert struct.unpack_from("<H", side, 32)[0] == THEME_HI
    assert struct.unpack_from("<I", side, 40)[0] == 0x31564146
    assert struct.unpack_from("<16H", side, 44) == (0xFFFF,) * 16
    assert struct.unpack_from("<I", side, 76)[0] == zlib.crc32(side[40:76])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--theme", default="TAB5", help="THM2 名（1..8 个可打印 ASCII，默认 TAB5）")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="输出目录")
    ap.add_argument("--selftest", action="store_true", help="只离线生成并逐字段校验")
    args = ap.parse_args()
    try:
        per3, side = build_per3(), build_side(args.theme)
        validate(per3, side, args.theme)
    except (ValueError, AssertionError) as e:
        ap.exit(1, f"[FAIL] settings fixture: {e or '布局校验失败'}\n")
    if args.selftest:
        print(f"SETTINGS FIXTURE SELFTEST OK per3=88 side=80 theme={args.theme} "
              f"hi=0x{THEME_HI:04X}")
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "per3.bin").write_bytes(per3)
    (args.out / "side.bin").write_bytes(side)
    print(f"[OK] {args.out}/per3.bin (88B), side.bin (80B), theme={args.theme}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
