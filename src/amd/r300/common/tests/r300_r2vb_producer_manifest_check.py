# SPDX-License-Identifier: MIT
"""Validate producer-manifest artifacts and calibrate their rejection paths.

The producer unit test checks the emitter in memory and the replay wrapper
checks kernel-parser admission.  This checker closes the retained-artifact
boundary: it parses every generated JSON file, joins the metadata to ib.bin,
checks the complete VAP tuple, and proves that malformed copies are refused.
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
    """A manifest or command-stream invariant does not hold."""


R300_PACKET3_DRAW_IMMD_2 = 0x35
R300_VAP_OUTPUT_VTX_FMT_0 = 0x2090
R300_VAP_OUTPUT_VTX_FMT_1 = 0x2094
R300_VAP_VTX_SIZE = 0x20B4
R300_VAP_VF_MAX_VTX_INDX = 0x2134
R300_VAP_VF_MIN_VTX_INDX = 0x2138
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
RADEON_GEM_DOMAIN_GTT = 0x2
R300_RS_COUNT = 0x4300
R300_RS_INST_COUNT = 0x4304
R300_RS_IP_0 = 0x4310
R300_RS_INST_0 = 0x4330
# RS_COUNT: IT_COUNT=4 interpolated components, IC_COUNT=0, HIRES_EN.
R300_R2VB_REFERENCE_RS_COUNT = 0x40004
# RS_IP_0: TEX_PTR 0 with S/T/R/Q selecting components 0..3.
R300_R2VB_REFERENCE_RS_IP_0 = (1 << 16) | (2 << 19) | (3 << 22)
# RS_INST_0: texture 0 written to US input register 0.
R300_R2VB_REFERENCE_RS_INST_0 = 1 << 3
# The US block writes the US/FG register range; a stream carrying the
# producer program touches R300_US_CODE_ADDR_0 and R300_US_ALU_RGB_INST_0.
R300_US_CONFIG = 0x4600
R300_US_CODE_ADDR_0 = 0x4610
R300_US_ALU_RGB_INST_0 = 0x48C0
R300_R2VB_PRODUCER_FS_BLOCK = (
    Path(__file__).resolve().parents[1] / "r300_r2vb_producer_fs_block.h"
)

# Independent carrier oracles for every stream the manifest writer exposes.
# The binary32 encodings mirror the calibrated tables in
# `r300_r2vb_producer_pass_test.c` (rg --fixed-strings
# "sweep_bits" src/amd/r300/common/tests/r300_r2vb_producer_pass_test.c),
# while the reference positions come from
# `r300_tcl_bypass_triangle_vertices` in the common emitter.  Keeping these
# values here prevents a manifest that pairs a stream name with another
# stream's records from becoming self-consistent by construction.
EXPECTED_STREAM_RECORD_BITS: dict[str, tuple[tuple[int, ...], ...]] = {
    "reference": (
        (0x41000000, 0x41000000, 0x00000000, 0x3F800000),
        (0x42600000, 0x41000000, 0x00000000, 0x3F800000),
        (0x42000000, 0x42600000, 0x00000000, 0x3F800000),
    ),
    "fp24-sweep": (
        (0x00000000, 0x21000000, 0x21000080, 0x21800000),
        (0x3F800000, 0x3F800080, 0x3FFFFF80, 0x40000000),
        (0x4479C000, 0x5F800000, 0x5FFFFF00, 0x5FFFFF80),
    ),
    "fp24-bisect": (
        (0x4F800000, 0x57800000, 0x5B800000, 0x5C800000),
        (0x5D000000, 0x5D800000, 0x5E000000, 0x5E800000),
        (0x5F000000, 0x5F7FFF80, 0x5F800000, 0x5FFFFF80),
    ),
}


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
    return [
        int.from_bytes(data[offset : offset + 4], "little")
        for offset in range(0, len(data), 4)
    ]


def write_words(path: Path, words: list[int]) -> None:
    path.write_bytes(b"".join(word.to_bytes(4, "little") for word in words))


def read_reference_us_block() -> list[int]:
    """Read the generated producer fragment block as the trusted payload."""
    try:
        source = R300_R2VB_PRODUCER_FS_BLOCK.read_text()
    except OSError as error:
        raise CheckFailure(f"cannot read producer fragment block: {error}") from error
    match = re.search(
        r"static const uint32_t r300_r2vb_producer_fs_block\[\] = \{(.*?)\};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise CheckFailure("producer fragment block declaration is absent")
    words = [int(token, 16) for token in re.findall(r"0x[0-9a-fA-F]+", match.group(1))]
    if not words:
        raise CheckFailure("producer fragment block is empty")
    return words


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
                current_register = (
                    register if one_register else register + payload_index * 4
                )
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
                current_register = (
                    packet_register
                    if one_register
                    else packet_register + payload_index * 4
                )
                if current_register == register:
                    words[index + 1 + payload_index] = value
                    found = True
        index = end
    if not found:
        raise CheckFailure(f"register 0x{register:04x} absent from ib.bin")
    write_words(path, words)


def append_register_override(path: Path, register: int, value: int) -> None:
    """Append a single-register packet that overrides the effective state."""
    if register % 4 != 0 or not 0 <= register <= 0x1FFF << 2:
        raise CheckFailure(f"register 0x{register:04x} cannot be packet encoded")
    words = read_words(path)
    words.extend([(register >> 2), value])
    write_words(path, words)


def remove_sequential_register(path: Path, register: int) -> None:
    """Remove one dword from a sequential packet and keep its header valid."""
    words = read_words(path)
    index = 0
    while index < len(words):
        header = words[index]
        packet_type = header >> 30
        raw_count = (header >> 16) & 0x3FFF
        payload_count = raw_count + 1
        end = index + 1 + payload_count
        if end > len(words):
            raise CheckFailure("cannot remove a register from a truncated stream")
        if packet_type == 0 and not ((header >> 15) & 1):
            packet_register = (header & 0x1FFF) << 2
            offset = register - packet_register
            if offset >= 0 and offset % 4 == 0:
                payload_index = offset // 4
                if payload_index < payload_count:
                    payload = words[index + 1 : end]
                    del payload[payload_index]
                    if not payload:
                        del words[index:end]
                    else:
                        words[index] = (header & ~(0x3FFF << 16)) | (
                            (len(payload) - 1) << 16
                        )
                        words[index + 1 : index + 1 + len(payload)] = payload
                        del words[index + 1 + len(payload) : end]
                    write_words(path, words)
                    return
        index = end
    raise CheckFailure(f"register 0x{register:04x} absent from a sequential packet")


def remove_us_program(path: Path) -> None:
    """Retire every US-program packet by turning its header into a NOP.

    A producer stream without its own US block runs whatever program the
    previous client left resident, which is what the checker refuses.  The
    rewrite keeps every dword in place, so the relocation site and the
    dword count stay valid and the refusal comes from the missing program.
    """
    words = read_words(path)
    index = 0
    removed = False
    while index < len(words):
        header = words[index]
        packet_type = header >> 30
        payload_count = ((header >> 16) & 0x3FFF) + 1
        end = index + 1 + payload_count
        if end > len(words):
            raise CheckFailure("cannot rewrite a truncated stream")
        if packet_type == 0:
            register = (header & 0x1FFF) << 2
            if R300_US_CONFIG <= register <= R300_US_ALU_RGB_INST_0:
                words[index] = R300_PACKET3_NOP | ((payload_count - 1) << 16)
                removed = True
        index = end
    if not removed:
        raise CheckFailure("ib.bin carries no US program packet to retire")
    write_words(path, words)


def validate(
    outdir: Path, have_b3sum: bool, expected_stream: str = "reference"
) -> None:
    manifest = read_json(outdir / "manifest.json")
    bo_table = read_json(outdir / "bo_table.json")
    words = read_words(outdir / "ib.bin")
    if not isinstance(manifest, dict) or not isinstance(bo_table, dict):
        raise CheckFailure("manifest and BO table must be JSON objects")

    stream_records = EXPECTED_STREAM_RECORD_BITS.get(expected_stream)
    if stream_records is None:
        raise CheckFailure(f"unsupported expected stream: {expected_stream!r}")
    if manifest.get("stream") != expected_stream:
        raise CheckFailure(
            f"manifest stream {manifest.get('stream')!r} != "
            f"expected {expected_stream!r}"
        )

    if manifest.get("schema") != "r300-r2vb-producer-pass/1":
        raise CheckFailure(f"unexpected schema: {manifest.get('schema')!r}")
    if manifest.get("emitter") != "r300_r2vb_producer_pass":
        raise CheckFailure(f"unexpected emitter: {manifest.get('emitter')!r}")
    if manifest.get("ib_dwords") != len(words):
        raise CheckFailure(
            f"ib_dwords {manifest.get('ib_dwords')} != "
            f"ib.bin dword count {len(words)}"
        )
    stream_count = len(stream_records)
    if manifest.get("vertex_count") != stream_count:
        raise CheckFailure("vertex_count does not describe the selected stream")
    if manifest.get("carrier_pitch_pixels") != R300_R2VB_REFERENCE_PITCH:
        raise CheckFailure("carrier pitch does not describe the reference pass")
    if manifest.get("carrier_height") != R300_R2VB_REFERENCE_HEIGHT:
        raise CheckFailure("carrier height does not describe the reference pass")
    if manifest.get("carrier_cpp_bytes") != R300_R2VB_REFERENCE_CPP:
        raise CheckFailure("carrier cpp does not describe C4_32_FP")

    carrier_size = (
        R300_R2VB_REFERENCE_PITCH * R300_R2VB_REFERENCE_HEIGHT * R300_R2VB_REFERENCE_CPP
    )
    if manifest.get("carrier_size_bytes") != carrier_size:
        raise CheckFailure("carrier size does not derive from pitch * height * cpp")

    expected = manifest.get("expected_carrier_dwords")
    if not isinstance(expected, list) or len(expected) != stream_count * 4:
        raise CheckFailure("expected carrier content does not cover the written slots")
    try:
        expected_words = [int(word, 16) for word in expected]
    except (TypeError, ValueError) as error:
        raise CheckFailure(f"expected carrier dword is not hex: {error}") from error
    stream_expected_words = [word for record in stream_records for word in record]
    if expected_words != stream_expected_words:
        raise CheckFailure(
            f"expected carrier content does not match the {expected_stream} "
            "stream oracle"
        )

    # The embedded records reach the carrier through the pass, so the
    # expected content is the same little-endian FLOAT_4 bytes the draw
    # body carries, pre-swizzle.
    body_index = None
    for draw_index, _ in decode_stream(words)[1]:
        body_index = draw_index + 2
    if body_index is None:
        raise CheckFailure("no immediate draw carries the records")
    for vertex in range(stream_count):
        slot = words[
            body_index
            + vertex * R300_R2VB_REFERENCE_VTX_DWORDS
            + 4 : body_index
            + vertex * R300_R2VB_REFERENCE_VTX_DWORDS
            + 8
        ]
        record = [slot[2], slot[1], slot[0], slot[3]]
        if record != expected_words[vertex * 4 : vertex * 4 + 4]:
            raise CheckFailure(
                f"expected carrier slot {vertex} differs from the " "embedded record"
            )

    poison = manifest.get("carrier_poison_dword")
    if not isinstance(poison, str):
        raise CheckFailure("carrier poison pattern is absent")
    try:
        poison_word = int(poison, 16)
    except ValueError as error:
        raise CheckFailure(f"carrier poison pattern is not hex: {error}") from error
    if not 0 <= poison_word <= 0xFFFFFFFF:
        raise CheckFailure("carrier poison pattern is outside one dword")
    # Anti-vacuity: a poison value the pass also writes would let an
    # unwritten slot read as delivered content.
    if poison_word in expected_words:
        raise CheckFailure("carrier poison pattern collides with expected content")

    slots = bo_table.get("slots")
    if (
        not isinstance(slots, list)
        or len(slots) != 1
        or not isinstance(slots[0], dict)
        or slots[0].get("slot") != 0
        or slots[0].get("role") != "carrier"
        or slots[0].get("domain") != "GTT"
    ):
        raise CheckFailure(f"carrier BO entry malformed: {slots}")
    minimum_size = (
        R300_R2VB_REFERENCE_PITCH * R300_R2VB_REFERENCE_HEIGHT * R300_R2VB_REFERENCE_CPP
    )
    if not isinstance(slots[0].get("size"), int) or slots[0]["size"] < minimum_size:
        raise CheckFailure(f"carrier BO is below {minimum_size} bytes")
    # The color backend writes the carrier and the consuming draw fetches
    # it, so the relocation carries GTT in both domains.
    if (
        slots[0].get("read_domains") != RADEON_GEM_DOMAIN_GTT
        or slots[0].get("write_domain") != RADEON_GEM_DOMAIN_GTT
    ):
        raise CheckFailure(f"carrier BO domains are not read-write GTT: {slots[0]}")

    sites = manifest.get("reloc_sites")
    if (
        not isinstance(sites, list)
        or len(sites) != 1
        or not isinstance(sites[0], dict)
        or sites[0].get("slot") != 0
    ):
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
        tail_register = R300_VAP_PROG_STREAM_CNTL_0 + offset * 4
        if tail_register not in registers or registers[tail_register] != 0:
            raise CheckFailure("PSC control tail is not zero")
        ext_tail_register = R300_VAP_PROG_STREAM_CNTL_EXT_0 + offset * 4
        if ext_tail_register not in registers or registers[ext_tail_register] != 0:
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
    # The US program the slot pixels shade through travels in the stream:
    # the US code registers carry the complete generated block, and the RS
    # routing delivers the TEX0 varying to US input register 0.  Checking
    # only one register from the block would accept a payload mutation after
    # its outer IB digest was refreshed.
    reference_us_block = read_reference_us_block()
    if not any(
        words[index : index + len(reference_us_block)] == reference_us_block
        for index in range(len(words) - len(reference_us_block) + 1)
    ):
        raise CheckFailure(
            "complete producer US/FG block is absent or differs from the "
            "generated fragment block"
        )
    for us_register in (R300_US_CONFIG, R300_US_CODE_ADDR_0, R300_US_ALU_RGB_INST_0):
        if us_register not in registers:
            raise CheckFailure(
                f"US program register 0x{us_register:04x} absent; "
                "the pass would shade through the resident program"
            )
    if registers.get(R300_RS_COUNT) != R300_R2VB_REFERENCE_RS_COUNT:
        raise CheckFailure("RS_COUNT does not declare four interpolated components")
    if registers.get(R300_RS_INST_COUNT) != 0:
        raise CheckFailure("RS_INST_COUNT does not bound routing to instruction 0")
    if registers.get(R300_RS_IP_0) != R300_R2VB_REFERENCE_RS_IP_0:
        raise CheckFailure("RS_IP_0 does not route the TEX0 varying in order")
    if registers.get(R300_RS_INST_0) != R300_R2VB_REFERENCE_RS_INST_0:
        raise CheckFailure("RS_INST_0 does not write US input register 0")
    if registers.get(R300_VAP_VF_MAX_VTX_INDX) != stream_count - 1:
        raise CheckFailure("VAP_VF_MAX_VTX_INDX does not match the stream count")
    if registers.get(R300_VAP_VF_MIN_VTX_INDX) != 0:
        raise CheckFailure(
            "VAP_VF_MIN_VTX_INDX is not the zero lower bound; an "
            "inherited minimum would fold low indices onto it"
        )
    if len(draws) != 1:
        raise CheckFailure(f"expected one immediate draw, got {len(draws)}")
    draw_index, raw_draw_count = draws[0]
    expected_payload = 1 + stream_count * R300_R2VB_REFERENCE_VTX_DWORDS
    if raw_draw_count + 1 != expected_payload:
        raise CheckFailure("immediate draw payload does not match VAP_VTX_SIZE")
    vf_cntl = words[draw_index + 1]
    expected_vf_cntl = (stream_count << 16) | 0x31
    if vf_cntl != expected_vf_cntl:
        raise CheckFailure(f"unexpected embedded VF control 0x{vf_cntl:08x}")

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


def clone(good: Path, target: Path) -> None:
    target.mkdir()
    for name in ("ib.bin", "bo_table.json", "manifest.json"):
        shutil.copy(good / name, target / name)


def expect_reject(
    outdir: Path,
    have_b3sum: bool,
    label: str,
    expected_stream: str = "reference",
) -> None:
    try:
        validate(outdir, have_b3sum, expected_stream)
    except CheckFailure:
        return
    fail(f"calibration mutant '{label}' passed validation")


def generate_manifest(tool: str, outdir: Path, stream: str) -> dict:
    outdir.mkdir()
    command = [tool, str(outdir)]
    if stream != "reference":
        command.append(stream)
    run = subprocess.run(command, capture_output=True, text=True, check=False)
    if run.returncode != 0:
        fail(f"{stream} manifest tool exited {run.returncode}: {run.stderr}")
    manifest = read_json(outdir / "manifest.json")
    if not isinstance(manifest, dict):
        fail(f"{stream} manifest is not an object")
    try:
        validate(outdir, shutil.which("b3sum") is not None, stream)
    except CheckFailure as error:
        fail(f"{stream} manifest is invalid: {error}")
    return manifest


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: r300_r2vb_producer_manifest_check.py <manifest-tool>")
    tool = sys.argv[1]
    have_b3sum = shutil.which("b3sum") is not None

    with tempfile.TemporaryDirectory(prefix="r300-r2vb-producer-manifest-") as tmp:
        root = Path(tmp)
        good = root / "good"
        manifest = generate_manifest(tool, good, "reference")
        sweep_good = root / "fp24-sweep"
        sweep_manifest = generate_manifest(tool, sweep_good, "fp24-sweep")
        if manifest["ib_blake3"] == sweep_manifest["ib_blake3"]:
            fail("reference and fp24-sweep streams share one digest")
        if read_words(good / "ib.bin") == read_words(sweep_good / "ib.bin"):
            fail("reference and fp24-sweep streams share one IB")
        if (
            manifest["expected_carrier_dwords"]
            == sweep_manifest["expected_carrier_dwords"]
        ):
            fail("reference and fp24-sweep streams share one carrier oracle")

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

        mutant = root / "trailing-register-override"
        clone(good, mutant)
        append_register_override(mutant / "ib.bin", R300_VAP_VTX_SIZE, 4)
        changed = dict(read_json(mutant / "manifest.json"))
        changed["ib_dwords"] = len(read_words(mutant / "ib.bin"))
        if have_b3sum:
            changed["ib_blake3"] = subprocess.run(
                ["b3sum", "--no-names", str(mutant / "ib.bin")],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "trailing-register-override")

        mutant = root / "stale-psc"
        clone(good, mutant)
        mutate_register(
            mutant / "ib.bin", R300_VAP_PROG_STREAM_CNTL_0, R300_R2VB_REFERENCE_PSC ^ 1
        )
        expect_reject(mutant, have_b3sum, "stale-psc")

        mutant = root / "stale-output-format"
        clone(good, mutant)
        mutate_register(mutant / "ib.bin", R300_VAP_OUTPUT_VTX_FMT_1, 0)
        expect_reject(mutant, have_b3sum, "stale-output-format")

        mutant = root / "corrupted-us-program"
        clone(good, mutant)
        mutate_register(mutant / "ib.bin", R300_US_ALU_RGB_INST_0, 0)
        changed = dict(read_json(mutant / "manifest.json"))
        if have_b3sum:
            changed["ib_blake3"] = subprocess.run(
                ["b3sum", "--no-names", str(mutant / "ib.bin")],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "corrupted-us-program")

        mutant = root / "omitted-psc-tail"
        clone(good, mutant)
        remove_sequential_register(mutant / "ib.bin", R300_VAP_PROG_STREAM_CNTL_0 + 4)
        changed = dict(read_json(mutant / "manifest.json"))
        changed["ib_dwords"] = len(read_words(mutant / "ib.bin"))
        if have_b3sum:
            changed["ib_blake3"] = subprocess.run(
                ["b3sum", "--no-names", str(mutant / "ib.bin")],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "omitted-psc-tail")

        mutant = root / "omitted-us-program"
        clone(good, mutant)
        remove_us_program(mutant / "ib.bin")
        changed = dict(read_json(mutant / "manifest.json"))
        changed["ib_dwords"] = len(read_words(mutant / "ib.bin"))
        if have_b3sum:
            changed["ib_blake3"] = subprocess.run(
                ["b3sum", "--no-names", str(mutant / "ib.bin")],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "omitted-us-program")

        mutant = root / "collided-poison"
        clone(good, mutant)
        changed = dict(manifest)
        changed["carrier_poison_dword"] = changed["expected_carrier_dwords"][0]
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "collided-poison")

        for label, poison in (
            ("poison-too-wide", "0x100000000"),
            ("poison-negative", "-0x1"),
        ):
            mutant = root / label
            clone(good, mutant)
            changed = dict(manifest)
            changed["carrier_poison_dword"] = poison
            (mutant / "manifest.json").write_text(json.dumps(changed))
            expect_reject(mutant, have_b3sum, label)

        mutant = root / "non-hex-digest"
        clone(good, mutant)
        changed = dict(manifest)
        changed["ib_blake3"] = "g" * 64
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "non-hex-digest")

        if have_b3sum:
            mutant = root / "stale-digest"
            clone(good, mutant)
            digest = manifest["ib_blake3"]
            changed = dict(manifest)
            changed["ib_blake3"] = ("0" if digest[0] != "0" else "1") + digest[1:]
            (mutant / "manifest.json").write_text(json.dumps(changed))
            expect_reject(mutant, have_b3sum, "stale-digest")
        else:
            print(
                "b3sum absent; stale digest recomputation mutant not run",
                file=sys.stderr,
            )

        mutant = root / "sweep-wrong-stream"
        clone(sweep_good, mutant)
        changed = dict(sweep_manifest)
        changed["stream"] = "reference"
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "sweep-wrong-stream", "fp24-sweep")

        mutant = root / "sweep-reference-oracle"
        clone(sweep_good, mutant)
        changed = dict(sweep_manifest)
        changed["expected_carrier_dwords"] = manifest["expected_carrier_dwords"]
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "sweep-reference-oracle", "fp24-sweep")

        mutant = root / "sweep-stale-digest"
        clone(sweep_good, mutant)
        changed = dict(sweep_manifest)
        digest = sweep_manifest["ib_blake3"]
        changed["ib_blake3"] = ("0" if digest[0] != "0" else "1") + digest[1:]
        (mutant / "manifest.json").write_text(json.dumps(changed))
        expect_reject(mutant, have_b3sum, "sweep-stale-digest", "fp24-sweep")

    print("producer manifest artifacts are consistent and reject calibrated mutants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
