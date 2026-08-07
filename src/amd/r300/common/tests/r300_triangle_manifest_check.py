# SPDX-License-Identifier: MIT
"""Integration check over the manifest writer's real artifacts.

The staging-manifest unit test proves the shared digest helper and the
published constants; this check runs the r300_triangle_manifest executable
itself, parses the manifest.json it wrote, and validates the artifacts
against each other, so a writer that omits ib_blake3, publishes a stale
digest, records wrong relocation sites, or emits malformed JSON fails here
even while every unit assertion passes.

The BLAKE3 comparison recomputes the digest with the host b3sum utility, an
implementation independent of the in-tree hasher; where b3sum is absent the
leg reports not run and the structural checks still decide.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    print(f"manifest check: {message}", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: r300_triangle_manifest_check.py <manifest-tool>")
    tool = sys.argv[1]

    with tempfile.TemporaryDirectory(prefix="r300-manifest-check-") as tmp:
        run = subprocess.run([tool, tmp], capture_output=True, text=True)
        if run.returncode != 0:
            fail(f"manifest tool exited {run.returncode}: {run.stderr}")

        outdir = Path(tmp)
        try:
            manifest = json.loads((outdir / "manifest.json").read_text())
        except (OSError, json.JSONDecodeError) as e:
            fail(f"manifest.json unusable: {e}")
        ib = (outdir / "ib.bin").read_bytes()
        if not (outdir / "bo_table.json").is_file():
            fail("bo_table.json absent")

        if len(ib) == 0 or len(ib) % 4 != 0:
            fail(f"ib.bin carries {len(ib)} bytes, not whole dwords")
        if manifest.get("ib_dwords") != len(ib) // 4:
            fail(f"manifest ib_dwords {manifest.get('ib_dwords')} != "
                 f"ib.bin dword count {len(ib) // 4}")

        draw = manifest.get("draw_dword")
        if not isinstance(draw, int) or not 0 < draw < len(ib) // 4:
            fail(f"draw_dword {draw} outside the stream")
        # The canonical encoding is little-endian dwords, so the draw
        # header's packet type reads from the byte the manifest's index
        # names.
        header = int.from_bytes(ib[draw * 4:draw * 4 + 4], "little")
        if header >> 30 != 3:
            fail(f"dword {draw} is not a type-3 packet header: {header:#x}")

        # The emitter records sites in emission order, so the indices rise
        # strictly and the two slots are the color and vertex pair.
        sites = manifest.get("reloc_sites")
        if (not isinstance(sites, list) or len(sites) != 2 or
                {s.get("slot") for s in sites} != {0, 1}):
            fail(f"reloc_sites malformed: {sites}")
        if not sites[0].get("ib_index", 0) < sites[1].get("ib_index", 0):
            fail(f"reloc sites out of emission order: {sites}")
        for site in sites:
            if not 0 < site.get("ib_index", -1) < len(ib) // 4:
                fail(f"reloc site outside the stream: {site}")

        declared = manifest.get("ib_blake3", "")
        if len(declared) != 64:
            fail(f"ib_blake3 is not a 64-hex digest: {declared!r}")
        if shutil.which("b3sum"):
            recomputed = subprocess.run(
                ["b3sum", "--no-names", str(outdir / "ib.bin")],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            if recomputed != declared:
                fail(f"ib_blake3 {declared} != independent b3sum "
                     f"{recomputed}")
            blake3_leg = "independently verified"
        else:
            blake3_leg = "not run (b3sum absent)"

    print(f"manifest artifacts consistent; blake3: {blake3_leg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
