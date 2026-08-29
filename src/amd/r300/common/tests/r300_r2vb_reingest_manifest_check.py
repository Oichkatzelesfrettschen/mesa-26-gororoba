# SPDX-License-Identifier: MIT
"""Validate the R2VB re-ingest manifest and calibrate metadata refusals.

The command-stream replay proves that the parser accepts the bytes it reads.
This checker closes the retained-artifact boundary first: it parses every
generated JSON file, joins the metadata to ``ib.bin``, verifies the relocation
payloads and BO domains, and rejects calibrated metadata mutations.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class CheckFailure(Exception):
    """A re-ingest artifact violates its published contract."""


R300_PACKET3_NOP = 0xC0001000
RADEON_GEM_DOMAIN_GTT = 0x2
R300_R2VB_REFERENCE_CARRIER_BYTES = 4 * 16
R300_R2VB_REFERENCE_COLOR_BYTES = 64 * 65 * 4
R300_R2VB_CONSUMER_MIN_DWORDS = 1


def fail(message: str) -> None:
    print(f"re-ingest manifest check: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_json(path: Path) -> object:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise CheckFailure(f"{path.name} is unusable: {error}") from error


def read_words(path: Path) -> list[int]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise CheckFailure(f"{path.name} is unreadable: {error}") from error
    if not data or len(data) % 4:
        raise CheckFailure(f"ib.bin has {len(data)} non-dword bytes")
    return [
        int.from_bytes(data[offset : offset + 4], "little")
        for offset in range(0, len(data), 4)
    ]


def validate(outdir: Path, have_b3sum: bool = True) -> tuple[int, int]:
    manifest = read_json(outdir / "manifest.json")
    bo_table = read_json(outdir / "bo_table.json")
    words = read_words(outdir / "ib.bin")
    if not isinstance(manifest, dict) or not isinstance(bo_table, dict):
        raise CheckFailure("manifest and BO table must be JSON objects")

    if manifest.get("schema") != "r300-r2vb-reingest-pass/1":
        raise CheckFailure(f"unexpected schema: {manifest.get('schema')!r}")
    if manifest.get("emitter") != "r300_r2vb_reingest_pass":
        raise CheckFailure(f"unexpected emitter: {manifest.get('emitter')!r}")
    if manifest.get("ib_dwords") != len(words):
        raise CheckFailure(
            f"ib_dwords {manifest.get('ib_dwords')} != ib.bin dword count "
            f"{len(words)}"
        )

    consumer_start = manifest.get("consumer_start_dwords")
    if (
        not isinstance(consumer_start, int)
        or isinstance(consumer_start, bool)
        or not R300_R2VB_CONSUMER_MIN_DWORDS <= consumer_start < len(words)
    ):
        raise CheckFailure(
            "consumer_start_dwords does not identify a non-empty consumer " "suffix"
        )

    carrier_size = manifest.get("carrier_size_bytes")
    color_size = manifest.get("color_size_bytes")
    if (
        not isinstance(carrier_size, int)
        or isinstance(carrier_size, bool)
        or carrier_size != R300_R2VB_REFERENCE_CARRIER_BYTES
    ):
        raise CheckFailure(
            "carrier_size_bytes does not describe the three FP32x4 carrier " "slots"
        )
    if (
        not isinstance(color_size, int)
        or isinstance(color_size, bool)
        or color_size != R300_R2VB_REFERENCE_COLOR_BYTES
    ):
        raise CheckFailure(
            "color_size_bytes does not describe the 64x64 target and canary " "row"
        )

    slots = bo_table.get("slots")
    if not isinstance(slots, list) or len(slots) != 2:
        raise CheckFailure(f"BO table must contain carrier and color slots: {slots}")
    expected_slots = (
        {
            "slot": 0,
            "role": "carrier",
            "domain": "GTT",
            "read_domains": RADEON_GEM_DOMAIN_GTT,
            "write_domain": RADEON_GEM_DOMAIN_GTT,
            "size": carrier_size,
        },
        {
            "slot": 1,
            "role": "color",
            "domain": "GTT",
            "read_domains": 0,
            "write_domain": RADEON_GEM_DOMAIN_GTT,
            "size": color_size,
        },
    )
    for index, expected in enumerate(expected_slots):
        entry = slots[index]
        if not isinstance(entry, dict) or any(
            entry.get(field) != value for field, value in expected.items()
        ):
            raise CheckFailure(
                f"BO slot {index} does not match the manifest contract: {entry}"
            )

    sites = manifest.get("reloc_sites")
    if not isinstance(sites, list) or len(sites) != 3:
        raise CheckFailure(f"reloc_sites must contain three entries: {sites}")
    # The producer emits its carrier target first.  The consumer emitter
    # retains its own established relocation order: color target, then
    # fetched carrier.  The order is part of the byte-level manifest
    # contract, not a set-membership check.
    expected_slots_for_sites = (0, 1, 0)
    previous_index = -1
    for position, (site, expected_slot) in enumerate(
        zip(sites, expected_slots_for_sites, strict=True)
    ):
        if not isinstance(site, dict) or site.get("slot") != expected_slot:
            raise CheckFailure(
                f"relocation site {position} names the wrong slot: {site}"
            )
        site_index = site.get("ib_index")
        if (
            not isinstance(site_index, int)
            or isinstance(site_index, bool)
            or not 0 < site_index < len(words)
            or site_index <= previous_index
        ):
            raise CheckFailure(f"relocation site {position} is out of order: {site}")
        if words[site_index - 1] != R300_PACKET3_NOP:
            raise CheckFailure(
                f"relocation site {position} lacks its NOP header at "
                f"dword {site_index - 1}"
            )
        if words[site_index] != expected_slot * 4:
            raise CheckFailure(
                f"relocation site {position} payload does not name slot "
                f"{expected_slot}"
            )
        if position == 0 and site_index >= consumer_start:
            raise CheckFailure("producer relocation lies in the consumer suffix")
        if position > 0 and site_index < consumer_start:
            raise CheckFailure("consumer relocation lies before consumer_start_dwords")
        previous_index = site_index

    declared_digest = manifest.get("ib_blake3")
    if (
        not isinstance(declared_digest, str)
        or re.fullmatch(r"[0-9a-fA-F]{64}", declared_digest) is None
    ):
        raise CheckFailure("ib_blake3 is not a 64-hex digest")
    if have_b3sum:
        recomputed = subprocess.run(
            ["b3sum", "--no-names", str(outdir / "ib.bin")],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        if recomputed != declared_digest:
            raise CheckFailure("ib_blake3 does not match b3sum")

    return carrier_size, color_size


def clone(good: Path, target: Path) -> None:
    target.mkdir()
    for name in ("ib.bin", "bo_table.json", "manifest.json"):
        shutil.copy(good / name, target / name)


def expect_reject(outdir: Path, label: str, have_b3sum: bool) -> None:
    try:
        validate(outdir, have_b3sum)
    except CheckFailure:
        return
    fail(f"calibration mutant '{label}' passed validation")


def generate_manifest(tool: str, outdir: Path, have_b3sum: bool) -> None:
    outdir.mkdir()
    run = subprocess.run([tool, str(outdir)], capture_output=True, text=True)
    if run.returncode != 0:
        fail(f"manifest tool exited {run.returncode}: {run.stderr}")
    try:
        validate(outdir, have_b3sum)
    except CheckFailure as error:
        fail(f"generated manifest is invalid: {error}")


def validate_existing(outdir: Path) -> int:
    have_b3sum = shutil.which("b3sum") is not None
    if not have_b3sum:
        fail("required independent verifier b3sum is unavailable")
    try:
        carrier_size, color_size = validate(outdir, have_b3sum)
    except CheckFailure as error:
        fail(str(error))
    print(f"re-ingest metadata validated: carrier={carrier_size} color={color_size}")
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--validate":
        return validate_existing(Path(sys.argv[2]))
    if len(sys.argv) != 2:
        fail(
            "usage: r300_r2vb_reingest_manifest_check.py <manifest-tool> or "
            "--validate <output-directory>"
        )

    tool = sys.argv[1]
    have_b3sum = shutil.which("b3sum") is not None
    if not have_b3sum:
        fail("required independent verifier b3sum is unavailable")

    with tempfile.TemporaryDirectory(prefix="r300-r2vb-reingest-manifest-") as tmp:
        root = Path(tmp)
        good = root / "good"
        generate_manifest(tool, good, have_b3sum)
        manifest = read_json(good / "manifest.json")
        bo_table = read_json(good / "bo_table.json")
        assert isinstance(manifest, dict)
        assert isinstance(bo_table, dict)

        mutant = root / "stale-digest"
        clone(good, mutant)
        changed = dict(manifest)
        digest = changed["ib_blake3"]
        changed["ib_blake3"] = ("0" if digest[0] != "0" else "1") + digest[1:]
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, "stale-digest", have_b3sum)

        mutant = root / "wrong-relocation"
        clone(good, mutant)
        changed = dict(manifest)
        changed["reloc_sites"] = [
            {"slot": 0, "ib_index": 0},
            *changed["reloc_sites"][1:],
        ]
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, "wrong-relocation", have_b3sum)

        mutant = root / "wrong-domains"
        clone(good, mutant)
        changed_bo = json.loads(json.dumps(bo_table))
        changed_bo["slots"][0]["read_domains"] = 0
        (mutant / "bo_table.json").write_text(json.dumps(changed_bo))
        expect_reject(mutant, "wrong-domains", have_b3sum)

        mutant = root / "undersized-carrier"
        clone(good, mutant)
        changed_bo = json.loads(json.dumps(bo_table))
        changed_bo["slots"][0]["size"] = R300_R2VB_REFERENCE_CARRIER_BYTES // 2
        (mutant / "bo_table.json").write_text(json.dumps(changed_bo))
        expect_reject(mutant, "undersized-carrier", have_b3sum)

        mutant = root / "wrong-manifest-size"
        clone(good, mutant)
        changed = dict(manifest)
        changed["color_size_bytes"] = R300_R2VB_REFERENCE_COLOR_BYTES // 2
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, "wrong-manifest-size", have_b3sum)

    print("re-ingest manifest artifacts are consistent and reject calibrated mutants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
