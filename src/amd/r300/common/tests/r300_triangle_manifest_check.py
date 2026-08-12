# SPDX-License-Identifier: MIT
"""Integration check over the manifest writer's real artifacts.

The staging-manifest unit test proves the shared digest helper and the
published constants; this check runs the r300_triangle_manifest executable
itself, parses the manifest.json it wrote, and validates the artifacts
against each other, so a writer that omits ib_blake3, publishes a stale
digest, records wrong relocation sites, or emits malformed JSON fails here
even while every unit assertion passes.

The BLAKE3 comparison recomputes the digest with the host b3sum utility, an
implementation independent of the in-tree hasher.  b3sum is a required
dependency because the integration verdict includes this independent oracle.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


# R300_TRIANGLE_SLOT_COLOR then R300_TRIANGLE_SLOT_VERTEX, the emitter's
# fixed command-stream order.  The enum values come from
# `r300_tcl_bypass_triangle.h` (rg --fixed-strings
# "R300_TRIANGLE_SLOT_COLOR"
# src/amd/r300/common/r300_tcl_bypass_triangle.h), and the order mirrors
# `expected_slots` in `r300_tcl_bypass_triangle_validate_reloc_sites`
# (rg --fixed-strings "expected_slots"
# src/amd/r300/common/r300_tcl_bypass_triangle.c).
EXPECTED_RELOC_SLOTS = (1, 0)
# CP_PACKET3(R300_PM4_PACKET3_NOP, 0), as emitted by `r300_pm4_reloc_nop`
# (rg --fixed-strings "r300_pm4_reloc_nop"
# src/amd/r300/common/r300_pm4_builder.c) and defined by
# `r300_pm4_builder.h`
# (rg --fixed-strings "R300_PM4_PACKET3_NOP"
# src/amd/r300/common/r300_pm4_builder.h).  The payload dword follows
# this header in ib.bin.
RELOC_NOP_HEADER = 0xC0001000


def validate_bo_table(table: object) -> None:
    """Validate the transport slot-to-role mapping published by the writer."""
    if not isinstance(table, dict):
        raise ValueError(f"bo_table is not an object: {table}")
    slots = table.get("slots")
    if not isinstance(slots, list) or len(slots) != 2:
        raise ValueError(f"bo_table slots malformed: {slots}")
    # The role strings mirror the writer's bo_table entries (rg
    # --fixed-strings '"role": "vertex"'
    # src/amd/r300/common/r300_triangle_manifest.c).
    expected = ((0, "vertex"), (1, "color"))
    for entry, (expected_slot, expected_role) in zip(slots, expected):
        if not isinstance(entry, dict):
            raise ValueError(f"bo_table slot is not an object: {entry}")
        if entry.get("slot") != expected_slot:
            raise ValueError(
                f"bo_table slot {entry.get('slot')} != {expected_slot}"
            )
        if entry.get("role") != expected_role:
            raise ValueError(
                f"bo_table role {entry.get('role')!r} != {expected_role!r}"
            )
        if entry.get("domain") != "GTT":
            raise ValueError(
                f"bo_table {expected_role} domain is not GTT: "
                f"{entry.get('domain')!r}"
            )
        size = entry.get("size")
        if (not isinstance(size, int) or isinstance(size, bool) or
                size <= 0):
            raise ValueError(
                f"bo_table {expected_role} size is invalid: {size!r}"
            )


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
        run = subprocess.run(
            [tool, tmp], capture_output=True, text=True, check=False
        )
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
        try:
            validate_bo_table(bo_table)
        except ValueError as e:
            fail(str(e))

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

        swapped_bo_table = {
            "slots": [dict(bo_table["slots"][1]),
                      dict(bo_table["slots"][0])]
        }
        try:
            validate_bo_table(swapped_bo_table)
        except ValueError:
            pass
        else:
            fail("known-bad swapped-BO-table calibration passed")

        def expect_bad_ib(label: str, index: int, value: int) -> None:
            mutated = bytearray(ib)
            mutated[index * 4:index * 4 + 4] = value.to_bytes(4, "little")
            try:
                validate_reloc_sites(sites, bytes(mutated))
            except ValueError:
                return
            fail(f"known-bad {label} calibration passed")

        first_site = sites[0]["ib_index"]
        expect_bad_ib("relocation NOP header", first_site - 1,
                      RELOC_NOP_HEADER ^ 1)
        expect_bad_ib("relocation payload", first_site,
                      EXPECTED_RELOC_SLOTS[0] * 4 ^ 1)

        declared = manifest.get("ib_blake3", "")
        if len(declared) != 64:
            fail(f"ib_blake3 is not a 64-hex digest: {declared!r}")
        b3sum = shutil.which("b3sum")
        if b3sum is None:
            fail("b3sum is required for independent ib_blake3 verification")
        recomputed = subprocess.run(
            [b3sum, "--no-names", str(outdir / "ib.bin")],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        if recomputed != declared:
            fail(f"ib_blake3 {declared} != independent b3sum "
                 f"{recomputed}")
        blake3_leg = "independently verified"

    print(f"manifest artifacts consistent; relocation mapping calibrated; "
          f"blake3: {blake3_leg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
