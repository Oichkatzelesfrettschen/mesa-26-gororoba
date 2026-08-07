# SPDX-License-Identifier: MIT
"""Integration check over the direct-write manifest writer's artifacts.

The direct-write unit test proves the emitter and the oracle; this check
runs the r300_direct_write_manifest executable itself, parses the
manifest.json it wrote, and validates the artifacts against each other,
so a writer that omits ib_blake3, publishes a stale digest, records
wrong relocation sites, or emits malformed JSON fails here even while
every unit assertion passes.

The check calibrates itself: after the writer's real output passes, four
mutated copies -- truncated IB, zeroed relocation site, undersized BO,
out-of-range pixel -- must each fail through validation, and with b3sum
present a digest-mutated copy must fail the recomputation. A validation
pass that cannot reject its own mutants proves nothing.

The BLAKE3 comparison recomputes the digest with the host b3sum utility,
an implementation independent of the in-tree hasher; where b3sum is
absent that leg and its mutant report not run and the structural checks
still decide.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class CheckFailure(Exception):
    pass


def validate(outdir: Path, have_b3sum: bool) -> None:
    try:
        manifest = json.loads((outdir / "manifest.json").read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise CheckFailure(f"manifest.json unusable: {e}")
    ib = (outdir / "ib.bin").read_bytes()
    try:
        bo_table = json.loads((outdir / "bo_table.json").read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise CheckFailure(f"bo_table.json unusable: {e}")

    if manifest.get("schema") != "r300-direct-write-cell/1":
        raise CheckFailure(f"unexpected schema: {manifest.get('schema')!r}")
    if len(ib) == 0 or len(ib) % 4 != 0:
        raise CheckFailure(f"ib.bin carries {len(ib)} bytes, not whole dwords")
    if manifest.get("ib_dwords") != len(ib) // 4:
        raise CheckFailure(f"manifest ib_dwords {manifest.get('ib_dwords')} "
                           f"!= ib.bin dword count {len(ib) // 4}")

    # One color BO covering target plus canary rows within the
    # allocation, and the pixel geometry the oracle scans.
    slots = bo_table.get("slots")
    if (not isinstance(slots, list) or len(slots) != 1 or
            slots[0].get("slot") != 0 or
            slots[0].get("role") != "color" or
            slots[0].get("domain") != "GTT"):
        raise CheckFailure(f"bo_table slots malformed: {slots}")
    pitch = manifest.get("target_pitch_pixels")
    rows = manifest.get("allocation_rows")
    if not (isinstance(pitch, int) and isinstance(rows, int)):
        raise CheckFailure("pitch or allocation_rows missing")
    if slots[0].get("size", 0) < pitch * 4 * rows:
        raise CheckFailure(f"color BO size {slots[0].get('size')} below the "
                           f"oracle-covered {pitch * 4 * rows} bytes")
    for name in ("pixel_a", "pixel_b"):
        p = manifest.get(name)
        if (not isinstance(p, dict) or
                not 0 <= p.get("x", -1) < manifest.get("target_width", 0) or
                not 0 <= p.get("y", -1) < manifest.get("target_height", 0)):
            raise CheckFailure(f"{name} outside the target: {p}")

    # The emitter records the one relocation site as a NOP payload:
    # the header dword before the site is a type-3 packet header.
    sites = manifest.get("reloc_sites")
    if (not isinstance(sites, list) or len(sites) != 1 or
            sites[0].get("slot") != 0):
        raise CheckFailure(f"reloc_sites malformed: {sites}")
    idx = sites[0].get("ib_index", -1)
    if not 0 < idx < len(ib) // 4:
        raise CheckFailure(f"reloc site outside the stream: {sites[0]}")
    header = int.from_bytes(ib[(idx - 1) * 4:idx * 4], "little")
    if header >> 30 != 3:
        raise CheckFailure(f"dword {idx - 1} before the reloc site is not a "
                           f"type-3 packet header: {header:#x}")

    declared = manifest.get("ib_blake3", "")
    if len(declared) != 64:
        raise CheckFailure(f"ib_blake3 is not a 64-hex digest: {declared!r}")
    if have_b3sum:
        recomputed = subprocess.run(
            ["b3sum", "--no-names", str(outdir / "ib.bin")],
            capture_output=True, text=True)
        if recomputed.returncode != 0:
            raise CheckFailure(f"b3sum failed: {recomputed.stderr}")
        if recomputed.stdout.strip() != declared:
            raise CheckFailure(f"ib_blake3 {declared} != b3sum "
                               f"{recomputed.stdout.strip()}")


def fail(message: str) -> None:
    print(f"direct-write manifest check: {message}", file=sys.stderr)
    sys.exit(1)


def clone(src: Path, dst: Path) -> None:
    dst.mkdir()
    for name in ("ib.bin", "bo_table.json", "manifest.json"):
        shutil.copy(src / name, dst / name)


def expect_reject(outdir: Path, have_b3sum: bool, label: str) -> None:
    try:
        validate(outdir, have_b3sum)
    except CheckFailure:
        return
    fail(f"calibration mutant '{label}' passed validation")


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: r300_direct_write_manifest_check.py <manifest-tool>")
    tool = sys.argv[1]
    have_b3sum = shutil.which("b3sum") is not None

    with tempfile.TemporaryDirectory(prefix="r300-dw-manifest-") as tmp:
        good = Path(tmp) / "good"
        good.mkdir()
        run = subprocess.run([tool, str(good)], capture_output=True, text=True)
        if run.returncode != 0:
            fail(f"manifest tool exited {run.returncode}: {run.stderr}")
        try:
            validate(good, have_b3sum)
        except CheckFailure as e:
            fail(str(e))

        manifest = json.loads((good / "manifest.json").read_text())

        m = Path(tmp) / "truncated-ib"
        clone(good, m)
        ib = (good / "ib.bin").read_bytes()
        (m / "ib.bin").write_bytes(ib[:len(ib) - 8])
        expect_reject(m, have_b3sum, "truncated-ib")

        m = Path(tmp) / "zeroed-site"
        clone(good, m)
        mut = dict(manifest)
        mut["reloc_sites"] = [{"slot": 0, "ib_index": 0}]
        (m / "manifest.json").write_text(json.dumps(mut))
        expect_reject(m, have_b3sum, "zeroed-site")

        m = Path(tmp) / "undersized-bo"
        clone(good, m)
        bo = json.loads((good / "bo_table.json").read_text())
        bo["slots"][0]["size"] = 4096
        (m / "bo_table.json").write_text(json.dumps(bo))
        expect_reject(m, have_b3sum, "undersized-bo")

        m = Path(tmp) / "pixel-out-of-range"
        clone(good, m)
        mut = dict(manifest)
        mut["pixel_b"] = dict(manifest["pixel_b"], x=manifest["target_width"])
        (m / "manifest.json").write_text(json.dumps(mut))
        expect_reject(m, have_b3sum, "pixel-out-of-range")

        if have_b3sum:
            m = Path(tmp) / "mutated-digest"
            clone(good, m)
            d = manifest["ib_blake3"]
            mut = dict(manifest)
            mut["ib_blake3"] = ("0" if d[0] != "0" else "1") + d[1:]
            (m / "manifest.json").write_text(json.dumps(mut))
            expect_reject(m, have_b3sum, "mutated-digest")
        else:
            print("b3sum absent; BLAKE3 recomputation and its mutant not run",
                  file=sys.stderr)

    print("direct-write manifest artifacts are mutually consistent and the "
          "checker rejects its calibration mutants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
