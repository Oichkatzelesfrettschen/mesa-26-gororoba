# SPDX-License-Identifier: MIT
"""Integration check over the direct-write manifest writer's artifacts.

The direct-write unit test proves the emitter and the oracle; this check
runs the r300_direct_write_manifest executable itself, parses the
manifest.json it wrote, and validates the artifacts against each other,
so a writer that omits ib_blake3, publishes a stale digest, records
wrong relocation sites, or emits malformed JSON fails here even while
every unit assertion passes.

The BLAKE3 comparison recomputes the digest with the host b3sum utility,
an implementation independent of the in-tree hasher; where b3sum is
absent the leg reports not run and the structural checks still decide.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    print(f"direct-write manifest check: {message}", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: r300_direct_write_manifest_check.py <manifest-tool>")
    tool = sys.argv[1]

    with tempfile.TemporaryDirectory(prefix="r300-dw-manifest-") as tmp:
        run = subprocess.run([tool, tmp], capture_output=True, text=True)
        if run.returncode != 0:
            fail(f"manifest tool exited {run.returncode}: {run.stderr}")

        outdir = Path(tmp)
        try:
            manifest = json.loads((outdir / "manifest.json").read_text())
        except (OSError, json.JSONDecodeError) as e:
            fail(f"manifest.json unusable: {e}")
        ib = (outdir / "ib.bin").read_bytes()
        try:
            bo_table = json.loads((outdir / "bo_table.json").read_text())
        except (OSError, json.JSONDecodeError) as e:
            fail(f"bo_table.json unusable: {e}")

        if manifest.get("schema") != "r300-direct-write-cell/1":
            fail(f"unexpected schema: {manifest.get('schema')!r}")
        if len(ib) == 0 or len(ib) % 4 != 0:
            fail(f"ib.bin carries {len(ib)} bytes, not whole dwords")
        if manifest.get("ib_dwords") != len(ib) // 4:
            fail(f"manifest ib_dwords {manifest.get('ib_dwords')} != "
                 f"ib.bin dword count {len(ib) // 4}")

        # One color BO covering target plus canary rows within the
        # allocation, and the pixel geometry the oracle scans.
        slots = bo_table.get("slots")
        if (not isinstance(slots, list) or len(slots) != 1 or
                slots[0].get("slot") != 0 or
                slots[0].get("role") != "color" or
                slots[0].get("domain") != "GTT"):
            fail(f"bo_table slots malformed: {slots}")
        pitch = manifest.get("target_pitch_pixels")
        rows = manifest.get("allocation_rows")
        if not (isinstance(pitch, int) and isinstance(rows, int)):
            fail("pitch or allocation_rows missing")
        if slots[0].get("size", 0) < pitch * 4 * rows:
            fail(f"color BO size {slots[0].get('size')} below the "
                 f"oracle-covered {pitch * 4 * rows} bytes")
        for name in ("pixel_a", "pixel_b"):
            p = manifest.get(name)
            if (not isinstance(p, dict) or
                    not 0 <= p.get("x", -1) < manifest.get("target_width", 0) or
                    not 0 <= p.get("y", -1) < manifest.get("target_height", 0)):
                fail(f"{name} outside the target: {p}")

        # The emitter records the one relocation site as a NOP payload:
        # the header dword before the site is a type-3 NOP.
        sites = manifest.get("reloc_sites")
        if (not isinstance(sites, list) or len(sites) != 1 or
                sites[0].get("slot") != 0):
            fail(f"reloc_sites malformed: {sites}")
        idx = sites[0].get("ib_index", -1)
        if not 0 < idx < len(ib) // 4:
            fail(f"reloc site outside the stream: {sites[0]}")
        header = int.from_bytes(ib[(idx - 1) * 4:idx * 4], "little")
        if header >> 30 != 3:
            fail(f"dword {idx - 1} before the reloc site is not a type-3 "
                 f"packet header: {header:#x}")

        declared = manifest.get("ib_blake3", "")
        if len(declared) != 64:
            fail(f"ib_blake3 is not a 64-hex digest: {declared!r}")
        if shutil.which("b3sum"):
            recomputed = subprocess.run(
                ["b3sum", "--no-names", str(outdir / "ib.bin")],
                capture_output=True, text=True)
            if recomputed.returncode != 0:
                fail(f"b3sum failed: {recomputed.stderr}")
            if recomputed.stdout.strip() != declared:
                fail(f"ib_blake3 {declared} != b3sum "
                     f"{recomputed.stdout.strip()}")
        else:
            print("b3sum absent; BLAKE3 recomputation not run",
                  file=sys.stderr)

    print("direct-write manifest artifacts are mutually consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
