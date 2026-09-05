#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Legalization differential against the kernel CS tracker replay.

For every raw rejection class in the transformation table, the script
assembles the invalid packet stream itself from raw register words and
runs it through the Linux radeon CS replay (replay_r300_cs_track, built
from the linux-radeon-gororoba tree), requiring REJECT.  It then asks the
legalize tool for the same intended byte interval, replays the legalized
stream, requires ACCEPT, and holds the union of the legalized rectangles
to the interval exactly, computed here from windows.txt and never from
the legalizer's own accounting.

R3V_CS_TRACK_REPLAY_TOOL names the replay executable; unset, the script
exits 77 and the test is recorded as skipped, never as passed.

Usage: r300_rb2d_legalization_differential.py <legalize-tool>
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

DST_PITCH_OFFSET = 0x142C
DST_Y_X = 0x1438
DP_GUI_MASTER_CNTL = 0x146C
DST_WIDTH_HEIGHT = 0x1598
DP_BRUSH_FRGD_CLR = 0x147C
SC_TOP_LEFT = 0x16EC
SC_BOTTOM_RIGHT = 0x16F0
DEFAULT_SC_BOTTOM_RIGHT = 0x16E8
DP_CNTL = 0x16C0
DP_WRITE_MSK = 0x16CC
DSTCACHE_CTLSTAT = 0x1714
WAIT_UNTIL = 0x1720

GMC_DST_PITCH_OFFSET_CNTL = 1 << 1
GMC_BRUSH_SOLID_COLOR = 13 << 4
ROP3_P = 0x00F00000
GMC_CLR_CMP_CNTL_DIS = 1 << 28
GMC_WR_MSK_DIS = 1 << 30
ARGB8888 = 6
RGB565 = 4
NOP = 0xC0001000


def packet0(reg: int, value: int) -> list[int]:
    return [(reg >> 2) & 0xFFFF, value & 0xFFFFFFFF]


def stream(
    pitch: int,
    offset: int,
    rects: list[tuple[int, int, int, int]],
    fmt: int = ARGB8888,
    reloc: bool = True,
    pitch_offset_cntl: bool = True,
    order: str = "normal",
    tail: bool = True,
) -> bytes:
    """Assemble a raw fill stream from register words."""
    words: list[int] = []
    dst = packet0(DST_PITCH_OFFSET, ((pitch // 64) << 22) | (offset // 1024))
    if reloc:
        dst += [NOP, 0]
    common = (
        packet0(SC_TOP_LEFT, 0)
        + packet0(SC_BOTTOM_RIGHT, 0x1FFF | (0x1FFF << 16))
        + packet0(DEFAULT_SC_BOTTOM_RIGHT, 0x1FFF | (0x1FFF << 16))
    )
    master = GMC_BRUSH_SOLID_COLOR | (fmt << 8) | ROP3_P | GMC_CLR_CMP_CNTL_DIS
    master |= GMC_WR_MSK_DIS
    if pitch_offset_cntl:
        master |= GMC_DST_PITCH_OFFSET_CNTL
    fmtw = packet0(DP_GUI_MASTER_CNTL, master)
    fmtw += packet0(DP_CNTL, 3) + packet0(DP_WRITE_MSK, 0xFFFFFFFF)
    launch: list[int] = []
    for x, y, w, h in rects:
        launch += packet0(DP_BRUSH_FRGD_CLR, 0x11223344)
        launch += packet0(DST_Y_X, (y << 16) | x)
        launch += packet0(DST_WIDTH_HEIGHT, (w << 16) | h)
    epi = packet0(DSTCACHE_CTLSTAT, 0xF) + packet0(WAIT_UNTIL, (1 << 16) | (1 << 18) | (1 << 9))
    if order == "normal":
        words = dst + common + fmtw + launch
    elif order == "geometry-before-destination":
        words = common + fmtw + launch + dst
    elif order == "geometry-before-format":
        words = dst + common + launch + fmtw
    elif order == "launch-before-origin":
        x, y, w, h = rects[0]
        words = dst + common + fmtw + packet0(DP_BRUSH_FRGD_CLR, 0)
        words += packet0(DST_WIDTH_HEIGHT, (w << 16) | h) + packet0(DST_Y_X, (y << 16) | x)
    else:
        raise ValueError(order)
    if tail:
        words += epi
    return struct.pack("<%dI" % len(words), *words)


def replay(tool: str, bundle: Path, ib: Path) -> str:
    proc = subprocess.run([tool, str(bundle), str(ib)], capture_output=True, text=True)
    if proc.returncode == 0:
        return "accept"
    if proc.returncode == 1:
        return "reject"
    raise SystemExit(f"replay tool failed on {ib}: {proc.stderr.strip()}")


def bundle(path: Path, bo_size: int) -> Path:
    path.write_text(
        "family rs480\n"
        f"bo 0 role=destination size={bo_size} read_domains=0x0 write_domain=0x2\n"
    )
    return path


# Each row: name, raw stream builder kwargs, and the intended interval the
# legalizer receives.  The raw stream is the naive single-rectangle
# rendering of the interval on a 256-byte carrier, mutated as the row
# names, so the intended bytes and the rejected words describe one
# operation.
ROWS: list[dict] = [
    {"name": "x past pitch", "bo": 4096, "off": 260, "size": 8,
     "raw": dict(pitch=256, offset=0, rects=[(65, 0, 2, 1)])},
    {"name": "width overruns pitch", "bo": 4096, "off": 12, "size": 300,
     "raw": dict(pitch=256, offset=0, rects=[(3, 0, 75, 1)])},
    {"name": "x+width field overflow", "bo": 4 * 70000 + 1024, "off": 0,
     "size": 4 * 70000, "raw": dict(pitch=256, offset=0, rects=[(0, 0, 70000 & 0xFFFF, 1)])},
    {"name": "missing relocation", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], reloc=False)},
    {"name": "unsupported datatype", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], fmt=0)},
    {"name": "geometry before destination", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], order="geometry-before-destination")},
    {"name": "geometry before format", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], order="geometry-before-format")},
    {"name": "launch before origin", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], order="launch-before-origin")},
    {"name": "DEFAULT pitch source", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], pitch_offset_cntl=False)},
    {"name": "pitch zero", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=0, offset=0, rects=[(0, 0, 64, 1)])},
    {"name": "width zero", "bo": 4096, "off": 0, "size": 256,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 0, 1)])},
    {"name": "undersized object", "bo": 8192, "off": 4096, "size": 4096,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 64)])},
    {"name": "RGB565 row overflow", "bo": 4096, "off": 0, "size": 260,
     "raw": dict(pitch=256, offset=0, rects=[(0, 0, 130, 1)], fmt=RGB565)},
    # The three attended-cell intervals.  Each raw stream is the naive
    # single-rectangle rendering the legalizer replaces -- the whole
    # interval as one row of the carrier, which overruns the row and the
    # 16-bit extent field -- so the kernel rejects it and accepts the
    # legalized decomposition, and the coverage check holds the windows to
    # the interval the cell declares.
    # DST_PITCH_OFFSET carries eight pitch bits, so 256 units reaches the
    # tracker as pitch zero and 511 units reaches it as 255 units with
    # DST_TILE_MACRO set; both raw streams are rejected and the legalized
    # stream on the widest encodable carrier, 255 units, is accepted.
    {"name": "pitch past the 8-bit field", "bo": 65536, "off": 12,
     "size": 65428, "pitch": 256,
     "raw": dict(pitch=16384, offset=0, rects=[(3, 0, 4093, 1)])},
    {"name": "widest encodable carrier 16320", "bo": 65536, "off": 12,
     "size": 65428, "pitch": 16320, "evidence": "planned",
     "raw": dict(pitch=16320, offset=0, rects=[(3, 0, 16357, 1)])},
    {"name": "cell v2_multiwindow_256", "bo": 2097152, "off": 12,
     "size": 2097012, "pitch": 256,
     "raw": dict(pitch=256, offset=0, rects=[(3, 0, 524253 & 0xFFFF, 1)])},
]

# Rows the kernel accepts and the legalizer keeps accepting: the raw
# stream is a legal encoding of the interval, so both verdicts are ACCEPT
# and the differential holds the coverage alone.
ACCEPT_ROWS: list[dict] = [
    {"name": "missing final wait (kernel accepts, epilogue appended)", "bo": 4096,
     "off": 0, "size": 256, "raw": dict(pitch=256, offset=0, rects=[(0, 0, 64, 1)], tail=False)},
    {"name": "past scissor but inside object", "bo": 256 * 0x2100, "off": 256 * 0x2000,
     "size": 256, "raw": dict(pitch=256, offset=0, rects=[(0, 0x2000, 64, 1)])},
]


def coverage(windows_txt: Path, bo_size: int) -> list[tuple[int, int]]:
    """The byte intervals the windows touch, merged, computed here from the
    rectangle geometry alone.  An overlap or a rectangle past the object is
    fatal; the merged list is compared against the requested interval, so a
    gap between two windows shows as a second entry rather than as a
    matching byte count."""
    spans: list[tuple[int, int]] = []
    for line in windows_txt.read_text().splitlines():
        base, pitch, cpp, x, y, w, h = (int(v) for v in line.split())
        for row in range(h):
            start = base + (y + row) * pitch + x * cpp
            end = start + w * cpp
            if end > bo_size:
                raise SystemExit(f"rectangle past the object in {windows_txt}")
            spans.append((start, end))
    spans.sort()
    merged: list[tuple[int, int]] = []
    for start, end in spans:
        if merged and start < merged[-1][1]:
            raise SystemExit(f"overlapping rectangles in {windows_txt}")
        if merged and start == merged[-1][1]:
            merged[-1] = (merged[-1][0], end)
        else:
            merged.append((start, end))
    return merged


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    legalize = argv[1]
    replay_tool = os.environ.get("R3V_CS_TRACK_REPLAY_TOOL", "")
    if not replay_tool:
        print("R3V_CS_TRACK_REPLAY_TOOL unset; legalization differential not run", file=sys.stderr)
        return 77
    if not os.access(replay_tool, os.X_OK):
        print(f"{replay_tool} is not executable", file=sys.stderr)
        return 1

    failures = 0
    judged = 0
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for i, row in enumerate(ROWS + ACCEPT_ROWS):
            expect_raw = "reject" if row in ROWS else "accept"
            d = work / f"row{i}"
            d.mkdir()
            b = bundle(d / "bundle.txt", row["bo"])
            raw = d / "raw.bin"
            raw.write_bytes(stream(**row["raw"]))
            got_raw = replay(replay_tool, b, raw)
            leg = d / "legal"
            leg.mkdir()
            contract = "v2"
            proc = subprocess.run(
                [legalize, str(row["off"]), str(row["size"]), str(row["bo"]), contract,
                 row.get("evidence", "silicon"), str(row.get("pitch", 0)),
                 str(leg)],
                capture_output=True, text=True,
            )
            if proc.returncode != 0:
                print(f"FAIL {row['name']}: legalizer {proc.stdout.strip()} {proc.stderr.strip()}")
                failures += 1
                continue
            got_legal = replay(replay_tool, leg / "bundle.txt", leg / "ib.bin")
            expected = [(row["off"], row["off"] + row["size"])]
            observed = coverage(leg / "windows.txt", row["bo"])
            ok = got_raw == expect_raw and got_legal == "accept" and observed == expected
            judged += 1
            status = "ok  " if ok else "FAIL"
            print(f"{status} {row['name']:<52} raw={got_raw:<6} legalized={got_legal:<6} "
                  f"coverage={'exact' if observed == expected else 'DIFFERS'}")
            if not ok:
                failures += 1
    print(f"legalization differential: {judged} rows judged, {failures} failed")
    if judged == 0:
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
