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


# R300_TRIANGLE_SLOT_COLOR then R300_TRIANGLE_SLOT_VERTEX, the emitter's
# fixed command-stream order.
EXPECTED_RELOC_SLOTS = (1, 0)
# CP_PACKET3(R300_PM4_PACKET3_NOP, 0), as defined by r300_reg.h and
# r300_pm4_builder.h.  The payload dword follows this header in ib.bin.
RELOC_NOP_HEADER = 0xC0001000


def fail(message: str) -> None:
    print(f"manifest check: {message}", file=sys.stderr)
    sys.exit(1)


def validate_reloc_sites(sites: object, ib: bytes) -> None:
    """Validate the ordered slot and payload contract independently."""
    dword_count = len(ib) // 4
    if not isinstance(sites, list) or len(sites) != len(EXPECTED_RELOC_SLOTS):
        raise ValueError(f"reloc_sites malformed: {sites}")

    previous_index = -1
    for position, expected_slot in enumerate(EXPECTED_RELOC_SLOTS):
        site = sites[position]
        if not isinstance(site, dict):
            raise ValueError(f"reloc site {position} is not an object: {site}")
        if site.get("slot") != expected_slot:
            raise ValueError(
                f"reloc site {position} slot {site.get('slot')} != "
                f"expected {expected_slot}")
        index = site.get("ib_index")
        if (not isinstance(index, int) or isinstance(index, bool) or
                not 0 < index < dword_count):
            raise ValueError(f"reloc site outside the stream: {site}")
        if index <= previous_index:
            raise ValueError(f"reloc sites out of emission order: {sites}")

        header = int.from_bytes(ib[(index - 1) * 4:index * 4], "little")
        if header != RELOC_NOP_HEADER:
            raise ValueError(
                f"dword {index - 1} before reloc site {position} is not "
                f"the relocation NOP header: {header:#x}")
        payload = int.from_bytes(ib[index * 4:(index + 1) * 4], "little")
        expected_payload = expected_slot * 4
        if payload != expected_payload:
            raise ValueError(
                f"reloc site {position} payload {payload:#x} != "
                f"slot payload {expected_payload:#x}")
        previous_index = index


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
        try:
            json.loads((outdir / "bo_table.json").read_text())
        except (OSError, json.JSONDecodeError) as e:
            fail(f"bo_table.json unusable: {e}")

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

        sites = manifest.get("reloc_sites")
        try:
            validate_reloc_sites(sites, ib)
        except ValueError as e:
            fail(str(e))

        # Swapping slot values while retaining indices is the known-bad
        # calibration: set membership and index ordering alone must reject it.
        swapped_sites = [dict(site) for site in sites]
        swapped_sites[0]["slot"], swapped_sites[1]["slot"] = (
            swapped_sites[1]["slot"], swapped_sites[0]["slot"])
        try:
            validate_reloc_sites(swapped_sites, ib)
        except ValueError:
            pass
        else:
            fail("known-bad swapped-slot calibration passed")

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

    print(f"manifest artifacts consistent; relocation mapping calibrated; "
          f"blake3: {blake3_leg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
