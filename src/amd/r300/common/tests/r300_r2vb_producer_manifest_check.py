# SPDX-License-Identifier: MIT
"""Validate producer-manifest artifacts and calibrate their rejection paths.

The producer unit test checks the emitter in memory and the replay wrapper
checks kernel-parser admission.  This checker closes the retained-artifact
boundary: it parses every generated JSON file, joins the metadata to ib.bin,
checks the complete VAP tuple, and proves that malformed copies are refused.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class CheckFailure(Exception):
    """A manifest or command-stream invariant does not hold."""


R300_PACKET3_DRAW_IMMD_2 = 0x35
R300_VAP_OUTPUT_VTX_FMT_0 = 0x2090
R300_VAP_OUTPUT_VTX_FMT_1 = 0x2094
R300_VAP_VTX_SIZE = 0x20B4
R300_VAP_VF_MAX_VTX_INDX = 0x2134
R300_VAP_VTX_STATE_CNTL = 0x2180
R300_VAP_VSM_VTX_ASSM = 0x2184
R300_VAP_PROG_STREAM_CNTL_0 = 0x2150
R300_VAP_PROG_STREAM_CNTL_EXT_0 = 0x21E0
R300_VAP_PROG_STREAM_TAIL_COUNT = 8
R300_R2VB_REFERENCE_COUNT = 3
R300_R2VB_REFERENCE_PITCH = 4
R300_R2VB_REFERENCE_HEIGHT = 1
R300_R2VB_REFERENCE_CPP = 16
R300_R2VB_REFERENCE_VTX_DWORDS = 8
R300_R2VB_REFERENCE_PSC = 0x26030003
R300_R2VB_REFERENCE_PSC_EXT = 0xF688F688
R300_R2VB_REFERENCE_VTX_STATE = 0x5555
R300_R2VB_REFERENCE_VSM_ASSM = 0x401
R300_R2VB_REFERENCE_OUTPUT_0 = 0x1
R300_R2VB_REFERENCE_OUTPUT_1 = 4
R300_PACKET3_NOP = 0xC0001000


def fail(message: str) -> None:
    print(f"producer manifest check: {message}", file=sys.stderr)
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
    return [int.from_bytes(data[offset:offset + 4], "little")
            for offset in range(0, len(data), 4)]


def write_words(path: Path, words: list[int]) -> None:
    path.write_bytes(b"".join(word.to_bytes(4, "little") for word in words))


def decode_stream(words: list[int]) -> tuple[dict[int, int], list[tuple[int, int]]]:
    registers: dict[int, int] = {}
    draws: list[tuple[int, int]] = []
    index = 0
    while index < len(words):
        header = words[index]
        packet_type = header >> 30
        raw_count = (header >> 16) & 0x3FFF
        payload_count = raw_count + 1
        end = index + 1 + payload_count
        if end > len(words):
            raise CheckFailure("packet payload extends past ib.bin")

        if packet_type == 0:
            register = (header & 0x1FFF) << 2
            one_register = (header >> 15) & 1
            for payload_index in range(payload_count):
                current_register = (register if one_register else
                                    register + payload_index * 4)
                registers[current_register] = words[index + 1 + payload_index]
        elif packet_type == 3:
            opcode = (header >> 8) & 0xFF
            if opcode == R300_PACKET3_DRAW_IMMD_2:
                draws.append((index, raw_count))
        elif packet_type not in (1, 2):
            raise CheckFailure(f"unsupported packet type {packet_type}")
        index = end
    return registers, draws


def mutate_register(path: Path, register: int, value: int) -> None:
    words = read_words(path)
    index = 0
    found = False
    while index < len(words):
        header = words[index]
        packet_type = header >> 30
        raw_count = (header >> 16) & 0x3FFF
        payload_count = raw_count + 1
        end = index + 1 + payload_count
        if end > len(words):
            raise CheckFailure("cannot mutate a truncated stream")
        if packet_type == 0:
            packet_register = (header & 0x1FFF) << 2
            one_register = (header >> 15) & 1
            for payload_index in range(payload_count):
                current_register = (packet_register if one_register else
                                    packet_register + payload_index * 4)
                if current_register == register:
                    words[index + 1 + payload_index] = value
                    found = True
        index = end
    if not found:
        raise CheckFailure(f"register 0x{register:04x} absent from ib.bin")
    write_words(path, words)


def validate(outdir: Path, have_b3sum: bool) -> None:
    manifest = read_json(outdir / "manifest.json")
    bo_table = read_json(outdir / "bo_table.json")
    words = read_words(outdir / "ib.bin")
    if not isinstance(manifest, dict) or not isinstance(bo_table, dict):
        raise CheckFailure("manifest and BO table must be JSON objects")

    if manifest.get("schema") != "r300-r2vb-producer-pass/1":
        raise CheckFailure(f"unexpected schema: {manifest.get('schema')!r}")
    if manifest.get("emitter") != "r300_r2vb_producer_pass":
        raise CheckFailure(f"unexpected emitter: {manifest.get('emitter')!r}")
    if manifest.get("ib_dwords") != len(words):
        raise CheckFailure(f"ib_dwords {manifest.get('ib_dwords')} != "
                           f"ib.bin dword count {len(words)}")
    if manifest.get("vertex_count") != R300_R2VB_REFERENCE_COUNT:
        raise CheckFailure("vertex_count does not describe the reference pass")
    if manifest.get("carrier_pitch_pixels") != R300_R2VB_REFERENCE_PITCH:
        raise CheckFailure("carrier pitch does not describe the reference pass")
    if manifest.get("carrier_height") != R300_R2VB_REFERENCE_HEIGHT:
        raise CheckFailure("carrier height does not describe the reference pass")
    if manifest.get("carrier_cpp_bytes") != R300_R2VB_REFERENCE_CPP:
        raise CheckFailure("carrier cpp does not describe C4_32_FP")

    slots = bo_table.get("slots")
    if (not isinstance(slots, list) or len(slots) != 1 or
            not isinstance(slots[0], dict) or slots[0].get("slot") != 0 or
            slots[0].get("role") != "carrier" or
            slots[0].get("domain") != "GTT"):
        raise CheckFailure(f"carrier BO entry malformed: {slots}")
    minimum_size = (R300_R2VB_REFERENCE_PITCH *
                    R300_R2VB_REFERENCE_HEIGHT * R300_R2VB_REFERENCE_CPP)
    if not isinstance(slots[0].get("size"), int) or slots[0]["size"] < minimum_size:
        raise CheckFailure(f"carrier BO is below {minimum_size} bytes")

    sites = manifest.get("reloc_sites")
    if (not isinstance(sites, list) or len(sites) != 1 or
            not isinstance(sites[0], dict) or sites[0].get("slot") != 0):
        raise CheckFailure(f"reloc_sites malformed: {sites}")
    site_index = sites[0].get("ib_index", -1)
    if not isinstance(site_index, int) or not 0 < site_index < len(words):
        raise CheckFailure(f"relocation site outside ib.bin: {sites[0]}")
    if words[site_index - 1] != R300_PACKET3_NOP or words[site_index] != 0:
        raise CheckFailure("relocation site is not a carrier NOP payload")

    registers, draws = decode_stream(words)
    if registers.get(R300_VAP_PROG_STREAM_CNTL_0) != R300_R2VB_REFERENCE_PSC:
        raise CheckFailure("PSC pair does not carry the calibrated two-FP32x4 tuple")
    if registers.get(R300_VAP_PROG_STREAM_CNTL_EXT_0) != R300_R2VB_REFERENCE_PSC_EXT:
        raise CheckFailure("PSC EXT pair does not carry identity XYZW swizzles")
    for offset in range(1, R300_VAP_PROG_STREAM_TAIL_COUNT):
        if registers.get(R300_VAP_PROG_STREAM_CNTL_0 + offset * 4, 0) != 0:
            raise CheckFailure("PSC control tail is not zero")
        if registers.get(R300_VAP_PROG_STREAM_CNTL_EXT_0 + offset * 4, 0) != 0:
            raise CheckFailure("PSC EXT tail is not zero")
    if registers.get(R300_VAP_VTX_SIZE) != R300_R2VB_REFERENCE_VTX_DWORDS:
        raise CheckFailure("VAP_VTX_SIZE does not match two FP32x4 inputs")
    if registers.get(R300_VAP_VTX_STATE_CNTL) != R300_R2VB_REFERENCE_VTX_STATE:
        raise CheckFailure("VAP_VTX_STATE_CNTL does not select user colors")
    if registers.get(R300_VAP_VSM_VTX_ASSM) != R300_R2VB_REFERENCE_VSM_ASSM:
        raise CheckFailure("VAP_VSM_VTX_ASSM does not select position and TEX0")
    if registers.get(R300_VAP_OUTPUT_VTX_FMT_0) != R300_R2VB_REFERENCE_OUTPUT_0:
        raise CheckFailure("VAP_OUTPUT_VTX_FMT_0 is not position-only")
    if registers.get(R300_VAP_OUTPUT_VTX_FMT_1) != R300_R2VB_REFERENCE_OUTPUT_1:
        raise CheckFailure("VAP_OUTPUT_VTX_FMT_1 is not one four-component varying")
    if registers.get(R300_VAP_VF_MAX_VTX_INDX) != R300_R2VB_REFERENCE_COUNT - 1:
        raise CheckFailure("VAP_VF_MAX_VTX_INDX does not match the reference count")
    if len(draws) != 1:
        raise CheckFailure(f"expected one immediate draw, got {len(draws)}")
    draw_index, raw_draw_count = draws[0]
    expected_payload = 1 + R300_R2VB_REFERENCE_COUNT * R300_R2VB_REFERENCE_VTX_DWORDS
    if raw_draw_count + 1 != expected_payload:
        raise CheckFailure("immediate draw payload does not match VAP_VTX_SIZE")
    vf_cntl = words[draw_index + 1]
    expected_vf_cntl = (R300_R2VB_REFERENCE_COUNT << 16) | 0x31
    if vf_cntl != expected_vf_cntl:
        raise CheckFailure(f"unexpected embedded VF control 0x{vf_cntl:08x}")

    declared_digest = manifest.get("ib_blake3")
    if not isinstance(declared_digest, str) or len(declared_digest) != 64:
        raise CheckFailure("ib_blake3 is not a 64-hex digest")
    if have_b3sum:
        recomputed = subprocess.run(
            ["b3sum", "--no-names", str(outdir / "ib.bin")],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        if recomputed != declared_digest:
            raise CheckFailure("ib_blake3 does not match b3sum")


def clone(good: Path, target: Path) -> None:
    target.mkdir()
    for name in ("ib.bin", "bo_table.json", "manifest.json"):
        shutil.copy(good / name, target / name)


def expect_reject(outdir: Path, have_b3sum: bool, label: str) -> None:
    try:
        validate(outdir, have_b3sum)
    except CheckFailure:
        return
    fail(f"calibration mutant '{label}' passed validation")


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: r300_r2vb_producer_manifest_check.py <manifest-tool>")
    tool = sys.argv[1]
    have_b3sum = shutil.which("b3sum") is not None

    with tempfile.TemporaryDirectory(prefix="r300-r2vb-producer-manifest-") as tmp:
        root = Path(tmp)
        good = root / "good"
        good.mkdir()
        run = subprocess.run([tool, str(good)], capture_output=True, text=True)
        if run.returncode != 0:
            fail(f"manifest tool exited {run.returncode}: {run.stderr}")
        try:
            validate(good, have_b3sum)
        except CheckFailure as error:
            fail(str(error))

        manifest = read_json(good / "manifest.json")
        if not isinstance(manifest, dict):
            fail("manifest is not an object")

        mutant = root / "truncated-ib"
        clone(good, mutant)
        ib = (good / "ib.bin").read_bytes()
        (mutant / "ib.bin").write_bytes(ib[:-4])
        expect_reject(mutant, have_b3sum, "truncated-ib")

        mutant = root / "undersized-bo"
        clone(good, mutant)
        bo = read_json(good / "bo_table.json")
        assert isinstance(bo, dict)
        bo["slots"][0]["size"] = 32
        (mutant / "bo_table.json").write_text(json.dumps(bo))
        expect_reject(mutant, have_b3sum, "undersized-bo")

        mutant = root / "wrong-pitch"
        clone(good, mutant)
        changed = dict(manifest)
        changed["carrier_pitch_pixels"] = 2
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "wrong-pitch")

        mutant = root / "wrong-relocation"
        clone(good, mutant)
        changed = dict(manifest)
        changed["reloc_sites"] = [{"slot": 0, "ib_index": 0}]
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "wrong-relocation")

        mutant = root / "undersized-vtx"
        clone(good, mutant)
        mutate_register(mutant / "ib.bin", R300_VAP_VTX_SIZE, 4)
        expect_reject(mutant, have_b3sum, "undersized-vtx")

        mutant = root / "stale-psc"
        clone(good, mutant)
        mutate_register(mutant / "ib.bin", R300_VAP_PROG_STREAM_CNTL_0,
                        R300_R2VB_REFERENCE_PSC ^ 1)
        expect_reject(mutant, have_b3sum, "stale-psc")

        mutant = root / "stale-output-format"
        clone(good, mutant)
        mutate_register(mutant / "ib.bin", R300_VAP_OUTPUT_VTX_FMT_1, 0)
        expect_reject(mutant, have_b3sum, "stale-output-format")

        if have_b3sum:
            mutant = root / "stale-digest"
            clone(good, mutant)
            digest = manifest["ib_blake3"]
            changed = dict(manifest)
            changed["ib_blake3"] = ("0" if digest[0] != "0" else "1") + digest[1:]
            (mutant / "manifest.json").write_text(json.dumps(changed))
            expect_reject(mutant, have_b3sum, "stale-digest")
        else:
            print("b3sum absent; digest recomputation mutant not run",
                  file=sys.stderr)

    print("producer manifest artifacts are consistent and reject calibrated mutants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
