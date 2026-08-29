# SPDX-License-Identifier: MIT
"""Validate the FLOAT_4 + FLOAT_2 tuple manifest as one artifact contract.

The tuple unit test proves the emitter in memory and the replay wrapper proves
kernel-parser admission.  This checker joins the generated IB, vertex bytes,
BO table, and manifest before replay.  It verifies both independent BLAKE3
digests, decodes the PM4 register and packet state, and compares the fetched
vertex stream with the fixed reference records.  Calibrated mutations prove
that stale metadata, malformed bytes, and self-consistent but semantically
wrong command streams fail closed.
"""

from __future__ import annotations

import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


class CheckFailure(Exception):
    """A tuple artifact violates its published contract."""


R300_PACKET3_NOP = 0xC0001000
R300_PACKET3_LOAD_VBPNTR = 0x2F
R300_PACKET3_DRAW_VBUF_2 = 0x34
RADEON_GEM_DOMAIN_GTT = 0x2

R300_RB3D_COLOROFFSET0 = 0x4E28
R300_RB3D_COLORPITCH0 = 0x4E38
R300_RB3D_COLOR_FORMAT_ARGB32323232 = 7 << 21
R300_US_OUT_FMT_0 = 0x46A4
R300_US_OUT_FMT_C4_32_FP = 21
R300_US_OUT_FMT_RGBA = (1 << 8) | (2 << 10) | (3 << 12)

R300_VAP_CNTL_STATUS = 0x2140
R300_VAP_CLIP_CNTL = 0x221C
R300_VAP_VTE_CNTL = 0x20B0
R300_VAP_VTX_SIZE = 0x20B4
R300_VAP_VF_MAX_VTX_INDX = 0x2134
R300_VAP_VF_MIN_VTX_INDX = 0x2138
R300_VAP_PROG_STREAM_CNTL_0 = 0x2150
R300_VAP_PROG_STREAM_CNTL_EXT_0 = 0x21E0
R300_VAP_VTX_STATE_CNTL = 0x2180
R300_VAP_VSM_VTX_ASSM = 0x2184
R300_VAP_OUTPUT_VTX_FMT_0 = 0x2090
R300_VAP_OUTPUT_VTX_FMT_1 = 0x2094

R300_RS_COUNT = 0x4300
R300_RS_INST_COUNT = 0x4304
R300_RS_IP_0 = 0x4310
R300_RS_INST_0 = 0x4330

R300_VAP_TCL_BYPASS = 1 << 8
R300_CLIP_DISABLE = 1 << 16
R300_VTX_XY_FMT = 1 << 8
R300_VTX_Z_FMT = 1 << 9
R300_INPUT_CNTL_POS = 0x1
R300_INPUT_CNTL_TC0 = 0x400

R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES = 16
R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES = 8
R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS = 6
R300_R2VB_PRODUCER_CPP_BYTES = 16
R300_R2VB_PRODUCER_POISON_DWORD = 0xDEADBEEF

# The reference records mirror the pinned binary32 table in
# r300_r2vb_float2_tuple_pass.c.  Keeping the source-order values here makes
# the checker independent of the manifest's own expected_carrier_dwords field.
REFERENCE_RECORD_BITS: tuple[tuple[int, int], ...] = (
    (0x41000000, 0x3F400000),
    (0x42600000, 0x3F800000),
    (0x4479C000, 0x40000000),
)
EXPECTED_CARRIER_DWORDS = tuple(
    word
    for record in REFERENCE_RECORD_BITS
    for word in (*record, 0x00000000, 0x3F800000)
)

# FLOAT_4 slot identity followed by FLOAT_2 model records under the XY01
# selector.  The second LOAD_VBPNTR pointer starts after three 16-byte slots.
EXPECTED_LOAD_VBPNTR = (0x22, 0x02020404, 0, 3 * 16)
EXPECTED_PSC = 0x26010003
EXPECTED_PSC_EXT = 0xFB08F688
EXPECTED_RS_IP_0 = (1 << 16) | (2 << 19) | (3 << 22)
EXPECTED_RS_INST_0 = 1 << 3


def fail(message: str) -> None:
    """Print a stable test prefix and terminate the checker."""
    print(f"tuple manifest check: {message}", file=sys.stderr)
    raise SystemExit(1)


def require_int(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise CheckFailure(f"{name} must be an integer: {value!r}")
    return value


def read_json(path: Path) -> object:
    try:
        return json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CheckFailure(f"{path.name} is unusable: {error}") from error


def read_bytes(path: Path) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise CheckFailure(f"{path.name} is unreadable: {error}") from error
    if not data:
        raise CheckFailure(f"{path.name} is empty")
    return data


def read_words(path: Path) -> tuple[bytes, list[int]]:
    data = read_bytes(path)
    if len(data) % 4:
        raise CheckFailure(f"ib.bin has {len(data)} non-dword bytes")
    return data, [
        int.from_bytes(data[offset : offset + 4], "little")
        for offset in range(0, len(data), 4)
    ]


def b3sum_digest(path: Path) -> str:
    b3sum = shutil.which("b3sum")
    if b3sum is None:
        raise CheckFailure("required independent verifier b3sum is unavailable")
    try:
        result = subprocess.run(
            [b3sum, "--no-names", str(path)],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise CheckFailure(f"b3sum failed for {path.name}: {error}") from error
    digest = result.stdout.strip()
    if re.fullmatch(r"[0-9a-fA-F]{64}", digest) is None:
        raise CheckFailure(f"b3sum returned an invalid digest for {path.name}")
    return digest


def parse_hex_word(value: object, name: str) -> int:
    if not isinstance(value, str) or re.fullmatch(r"0x[0-9a-fA-F]{1,8}", value) is None:
        raise CheckFailure(f"{name} is not an unsigned hexadecimal dword: {value!r}")
    return int(value, 16)


def decode_stream(
    words: list[int],
) -> tuple[dict[int, int], list[tuple[int, int, int, int | None, list[int]]]]:
    """Decode packet boundaries and final PACKET0 register state."""
    registers: dict[int, int] = {}
    packets: list[tuple[int, int, int, int | None, list[int]]] = []
    index = 0
    while index < len(words):
        header = words[index]
        packet_type = header >> 30
        if packet_type == 2:
            packets.append((index, index + 1, packet_type, None, []))
            index += 1
            continue
        if packet_type not in (0, 3):
            raise CheckFailure(
                f"unsupported packet type {packet_type} at dword {index}"
            )

        raw_count = (header >> 16) & 0x3FFF
        payload_count = raw_count + 1
        end = index + 1 + payload_count
        if end > len(words):
            raise CheckFailure(f"packet at dword {index} extends past ib.bin")
        payload = words[index + 1 : end]
        opcode: int | None = None
        if packet_type == 0:
            register = (header & 0x1FFF) << 2
            one_register = (header >> 15) & 1
            for payload_index, value in enumerate(payload):
                current_register = (
                    register if one_register else register + payload_index * 4
                )
                registers[current_register] = value
        else:
            opcode = (header >> 8) & 0xFF
        packets.append((index, end, packet_type, opcode, payload))
        index = end
    return registers, packets


def packet0_register_range(
    words: list[int],
    packet: tuple[int, int, int, int | None, list[int]],
) -> tuple[int, int, bool] | None:
    start, end, packet_type, _, _ = packet
    if packet_type != 0:
        return None
    header = words[start]
    register = (header & 0x1FFF) << 2
    one_register = bool((header >> 15) & 1)
    return register, end - start - 1, one_register


def find_register_packets(
    words: list[int],
    packets: list[tuple[int, int, int, int | None, list[int]]],
    register: int,
) -> list[tuple[int, int, int, int | None, list[int]]]:
    matches = []
    for packet in packets:
        decoded = packet0_register_range(words, packet)
        if decoded is None:
            continue
        base, count, one_register = decoded
        if one_register:
            covers = base == register
        else:
            covers = base <= register < base + count * 4 and (register - base) % 4 == 0
        if covers:
            matches.append(packet)
    return matches


def read_reference_us_block() -> list[int]:
    source_path = Path(__file__).resolve().parents[1] / "r300_r2vb_producer_fs_block.h"
    try:
        source = source_path.read_text()
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


def expected_vertex_bytes() -> bytes:
    slot_bytes = b"".join(
        struct.pack(
            "<4I",
            struct.unpack("<I", struct.pack("<f", index + 0.5))[0],
            0x3F000000,
            0,
            0x3F800000,
        )
        for index in range(len(REFERENCE_RECORD_BITS))
    )
    model_bytes = b"".join(
        struct.pack("<2I", x_bits, y_bits) for x_bits, y_bits in REFERENCE_RECORD_BITS
    )
    return slot_bytes + model_bytes


def validate(outdir: Path) -> dict[str, int]:
    manifest = read_json(outdir / "manifest.json")
    bo_table = read_json(outdir / "bo_table.json")
    _, words = read_words(outdir / "ib.bin")
    vertex_bytes = read_bytes(outdir / "vertex.bin")
    if not isinstance(manifest, dict) or not isinstance(bo_table, dict):
        raise CheckFailure("manifest.json and bo_table.json must be objects")

    if manifest.get("schema") != "r300-r2vb-float2-tuple-pass/1":
        raise CheckFailure(f"unexpected schema: {manifest.get('schema')!r}")
    if manifest.get("emitter") != "r300_r2vb_float2_tuple_pass":
        raise CheckFailure(f"unexpected emitter: {manifest.get('emitter')!r}")

    ib_dwords = require_int(manifest.get("ib_dwords"), "ib_dwords")
    vertex_count = require_int(manifest.get("vertex_count"), "vertex_count")
    vtx_size = require_int(manifest.get("vap_vtx_size_dwords"), "vap_vtx_size_dwords")
    vertex_size = require_int(manifest.get("vertex_size_bytes"), "vertex_size_bytes")
    pitch = require_int(manifest.get("carrier_pitch_pixels"), "carrier_pitch_pixels")
    height = require_int(manifest.get("carrier_height"), "carrier_height")
    cpp = require_int(manifest.get("carrier_cpp_bytes"), "carrier_cpp_bytes")
    carrier_size = require_int(manifest.get("carrier_size_bytes"), "carrier_size_bytes")
    if ib_dwords != len(words):
        raise CheckFailure(f"ib_dwords {ib_dwords} != ib.bin dword count {len(words)}")
    if vertex_count != len(REFERENCE_RECORD_BITS):
        raise CheckFailure(
            f"vertex_count {vertex_count} does not describe the reference stream"
        )
    expected_vertex_size = vertex_count * (
        R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES
        + R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES
    )
    if vtx_size != R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS:
        raise CheckFailure(
            f"vap_vtx_size_dwords {vtx_size} does not match the six-dword fetch"
        )
    if vertex_size != expected_vertex_size or len(vertex_bytes) != vertex_size:
        raise CheckFailure(
            f"vertex size {vertex_size} does not match {len(vertex_bytes)} bytes and "
            f"the {expected_vertex_size}-byte fetch layout"
        )
    if (pitch, height, cpp) != (4, 1, R300_R2VB_PRODUCER_CPP_BYTES):
        raise CheckFailure(
            f"carrier geometry is {(pitch, height, cpp)!r}, expected (4, 1, 16)"
        )
    if carrier_size != pitch * height * cpp:
        raise CheckFailure(
            "carrier_size_bytes does not derive from pitch * height * cpp"
        )
    if carrier_size != 64:
        raise CheckFailure(
            f"carrier_size_bytes {carrier_size} is not the 64-byte reference row"
        )
    if vertex_bytes != expected_vertex_bytes():
        raise CheckFailure(
            "vertex.bin does not match the slot and FLOAT_2 model reference bytes"
        )

    for field, path in (
        ("ib_blake3", outdir / "ib.bin"),
        ("vertex_blake3", outdir / "vertex.bin"),
    ):
        declared = manifest.get(field)
        if (
            not isinstance(declared, str)
            or re.fullmatch(r"[0-9a-fA-F]{64}", declared) is None
        ):
            raise CheckFailure(f"{field} is not a 64-hex digest")
        if b3sum_digest(path).lower() != declared.lower():
            raise CheckFailure(f"{field} does not match b3sum for {path.name}")

    slots = bo_table.get("slots")
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
            "role": "vertex",
            "domain": "GTT",
            "read_domains": RADEON_GEM_DOMAIN_GTT,
            "write_domain": 0,
            "size": vertex_size,
        },
    )
    if not isinstance(slots, list) or len(slots) != len(expected_slots):
        raise CheckFailure(f"BO table must contain carrier and vertex slots: {slots!r}")
    for index, expected in enumerate(expected_slots):
        entry = slots[index]
        if not isinstance(entry, dict) or any(
            entry.get(field) != value for field, value in expected.items()
        ):
            raise CheckFailure(
                f"BO slot {index} does not match the tuple contract: {entry!r}"
            )

    registers, packets = decode_stream(words)
    sites = manifest.get("reloc_sites")
    expected_site_slots = (0, 1, 1)
    if not isinstance(sites, list) or len(sites) != len(expected_site_slots):
        raise CheckFailure(f"reloc_sites must contain three entries: {sites!r}")
    site_indices: list[int] = []
    for position, expected_slot in enumerate(expected_site_slots):
        site = sites[position]
        if not isinstance(site, dict) or site.get("slot") != expected_slot:
            raise CheckFailure(
                f"relocation site {position} names the wrong slot: {site!r}"
            )
        site_index = require_int(
            site.get("ib_index"), f"reloc_sites[{position}].ib_index"
        )
        if not 0 < site_index < len(words) or (
            site_indices and site_index <= site_indices[-1]
        ):
            raise CheckFailure(f"relocation site {position} is out of order: {site!r}")
        if words[site_index - 1] != R300_PACKET3_NOP:
            raise CheckFailure(f"relocation site {position} lacks its NOP header")
        if words[site_index] != expected_slot * 4:
            raise CheckFailure(
                f"relocation site {position} carries the wrong slot payload"
            )
        site_indices.append(site_index)

    color_packets = find_register_packets(words, packets, R300_RB3D_COLOROFFSET0)
    if len(color_packets) != 1 or site_indices[0] != color_packets[0][1] + 1:
        raise CheckFailure(
            "carrier relocation does not immediately follow COLOROFFSET0"
        )
    load_packets = [
        packet
        for packet in packets
        if packet[2] == 3 and packet[3] == R300_PACKET3_LOAD_VBPNTR
    ]
    if len(load_packets) != 1:
        raise CheckFailure(f"expected one LOAD_VBPNTR packet, got {len(load_packets)}")
    load_packet = load_packets[0]
    if load_packet[4] != list(EXPECTED_LOAD_VBPNTR):
        raise CheckFailure(
            f"LOAD_VBPNTR payload is {load_packet[4]!r}, expected {EXPECTED_LOAD_VBPNTR!r}"
        )
    if site_indices[1:] != [load_packet[1] + 1, load_packet[1] + 3]:
        raise CheckFailure("vertex relocations do not immediately follow LOAD_VBPNTR")

    if registers.get(R300_RB3D_COLOROFFSET0) != 0:
        raise CheckFailure("COLOROFFSET0 does not start at carrier offset zero")
    if registers.get(R300_RB3D_COLORPITCH0) != (
        4 | R300_RB3D_COLOR_FORMAT_ARGB32323232
    ):
        raise CheckFailure("COLORPITCH0 does not describe the four-pixel C4_32_FP row")
    if registers.get(R300_US_OUT_FMT_0) != (
        R300_US_OUT_FMT_C4_32_FP | R300_US_OUT_FMT_RGBA
    ):
        raise CheckFailure("US_OUT_FMT_0 does not select straight RGBA C4_32_FP output")
    if registers.get(R300_VAP_CNTL_STATUS) != R300_VAP_TCL_BYPASS:
        raise CheckFailure("VAP_CNTL_STATUS does not enable TCL bypass")
    if registers.get(R300_VAP_CLIP_CNTL) != R300_CLIP_DISABLE:
        raise CheckFailure("VAP_CLIP_CNTL does not disable clipping")
    if registers.get(R300_VAP_VTE_CNTL) != (R300_VTX_XY_FMT | R300_VTX_Z_FMT):
        raise CheckFailure("VAP_VTE_CNTL does not select XY/Z passthrough")
    if registers.get(R300_VAP_PROG_STREAM_CNTL_0) != EXPECTED_PSC:
        raise CheckFailure("PSC CNTL_0 does not declare FLOAT_4 plus FLOAT_2")
    if registers.get(R300_VAP_PROG_STREAM_CNTL_EXT_0) != EXPECTED_PSC_EXT:
        raise CheckFailure("PSC EXT_0 does not declare identity plus XY01")
    for offset in range(1, 8):
        if registers.get(R300_VAP_PROG_STREAM_CNTL_0 + offset * 4) != 0:
            raise CheckFailure("PSC control tail is not zero")
        if registers.get(R300_VAP_PROG_STREAM_CNTL_EXT_0 + offset * 4) != 0:
            raise CheckFailure("PSC EXT control tail is not zero")
    if registers.get(R300_VAP_VTX_SIZE) != R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS:
        raise CheckFailure("VAP_VTX_SIZE does not match the six-dword fetch")
    if registers.get(R300_VAP_VTX_STATE_CNTL) != 0x5555:
        raise CheckFailure("VAP_VTX_STATE_CNTL does not select user colors")
    if registers.get(R300_VAP_VSM_VTX_ASSM) != (
        R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0
    ):
        raise CheckFailure("VAP_VSM_VTX_ASSM does not select position and TEX0")
    if registers.get(R300_VAP_OUTPUT_VTX_FMT_0) != 1:
        raise CheckFailure("VAP_OUTPUT_VTX_FMT_0 is not position-only")
    if registers.get(R300_VAP_OUTPUT_VTX_FMT_1) != 4:
        raise CheckFailure("VAP_OUTPUT_VTX_FMT_1 is not one four-component varying")
    if registers.get(R300_RS_COUNT) != 0x40004:
        raise CheckFailure("RS_COUNT does not declare four interpolated components")
    if registers.get(R300_RS_INST_COUNT) != 0:
        raise CheckFailure("RS_INST_COUNT does not bound routing to instruction zero")
    if registers.get(R300_RS_IP_0) != EXPECTED_RS_IP_0:
        raise CheckFailure("RS_IP_0 does not route TEX0 in component order")
    if registers.get(R300_RS_INST_0) != EXPECTED_RS_INST_0:
        raise CheckFailure("RS_INST_0 does not write US input register zero")
    if registers.get(R300_VAP_VF_MAX_VTX_INDX) != vertex_count - 1:
        raise CheckFailure("VAP_VF_MAX_VTX_INDX does not match the stream count")
    if registers.get(R300_VAP_VF_MIN_VTX_INDX) != 0:
        raise CheckFailure(
            "VAP_VF_MIN_VTX_INDX does not establish the zero lower bound"
        )

    draw_packets = [
        packet
        for packet in packets
        if packet[2] == 3 and packet[3] == R300_PACKET3_DRAW_VBUF_2
    ]
    if len(draw_packets) != 1 or draw_packets[0][4] != [0x30021]:
        raise CheckFailure(f"tuple draw payload is malformed: {draw_packets!r}")
    producer_block = read_reference_us_block()
    if not any(
        words[index : index + len(producer_block)] == producer_block
        for index in range(len(words) - len(producer_block) + 1)
    ):
        raise CheckFailure("complete producer fragment block is absent from ib.bin")

    expected_carrier = manifest.get("expected_carrier_dwords")
    if not isinstance(expected_carrier, list) or len(expected_carrier) != len(
        EXPECTED_CARRIER_DWORDS
    ):
        raise CheckFailure(
            "expected_carrier_dwords does not cover every reference slot"
        )
    parsed_carrier = tuple(
        parse_hex_word(word, f"expected_carrier_dwords[{index}]")
        for index, word in enumerate(expected_carrier)
    )
    if parsed_carrier != EXPECTED_CARRIER_DWORDS:
        raise CheckFailure("expected carrier content does not match the XY01 oracle")
    poison = parse_hex_word(
        manifest.get("carrier_poison_dword"), "carrier_poison_dword"
    )
    if poison != R300_R2VB_PRODUCER_POISON_DWORD:
        raise CheckFailure(
            "carrier poison pattern does not match the producer contract"
        )
    if poison in parsed_carrier:
        raise CheckFailure("carrier poison collides with expected delivered content")

    return {
        "ib_dwords": ib_dwords,
        "vertex_bytes": vertex_size,
        "carrier_bytes": carrier_size,
    }


def clone(good: Path, target: Path) -> None:
    target.mkdir()
    for name in ("ib.bin", "vertex.bin", "bo_table.json", "manifest.json"):
        shutil.copy(good / name, target / name)


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value))


def refresh_ib_digest(manifest: dict[str, object], outdir: Path) -> None:
    ib = outdir / "ib.bin"
    manifest["ib_dwords"] = len(ib.read_bytes()) // 4
    manifest["ib_blake3"] = b3sum_digest(ib)
    write_json(outdir / "manifest.json", manifest)


def expect_reject(outdir: Path, label: str) -> None:
    try:
        validate(outdir)
    except CheckFailure:
        return
    fail(f"calibration mutant '{label}' passed validation")


def generate_manifest(tool: str, outdir: Path) -> None:
    outdir.mkdir()
    result = subprocess.run(
        [tool, str(outdir)], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        fail(f"manifest tool exited {result.returncode}: {result.stderr}")
    try:
        validate(outdir)
    except CheckFailure as error:
        fail(f"generated manifest is invalid: {error}")


def validate_existing(outdir: Path) -> int:
    try:
        report = validate(outdir)
    except CheckFailure as error:
        fail(str(error))
    print(
        "tuple manifest artifacts validated: "
        f"ib_dwords={report['ib_dwords']} "
        f"vertex_bytes={report['vertex_bytes']} "
        f"carrier_bytes={report['carrier_bytes']}"
    )
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--validate":
        if shutil.which("b3sum") is None:
            fail("required independent verifier b3sum is unavailable")
        return validate_existing(Path(sys.argv[2]))
    if len(sys.argv) != 2:
        fail(
            "usage: r300_r2vb_float2_tuple_manifest_check.py <manifest-tool> or "
            "--validate <output-directory>"
        )
    if shutil.which("b3sum") is None:
        fail("required independent verifier b3sum is unavailable")

    tool = sys.argv[1]
    with tempfile.TemporaryDirectory(
        prefix="r300-r2vb-float2-tuple-manifest-"
    ) as temporary:
        root = Path(temporary)
        good = root / "good"
        generate_manifest(tool, good)
        manifest = read_json(good / "manifest.json")
        bo_table = read_json(good / "bo_table.json")
        assert isinstance(manifest, dict)
        assert isinstance(bo_table, dict)

        mutant = root / "stale-ib-digest"
        clone(good, mutant)
        changed = dict(manifest)
        digest = str(changed["ib_blake3"])
        changed["ib_blake3"] = ("0" if digest[0] != "0" else "1") + digest[1:]
        write_json(mutant / "manifest.json", changed)
        expect_reject(mutant, "stale-ib-digest")

        mutant = root / "stale-vertex-digest"
        clone(good, mutant)
        changed = dict(manifest)
        digest = str(changed["vertex_blake3"])
        changed["vertex_blake3"] = ("0" if digest[0] != "0" else "1") + digest[1:]
        write_json(mutant / "manifest.json", changed)
        expect_reject(mutant, "stale-vertex-digest")

        mutant = root / "corrupted-vertex"
        clone(good, mutant)
        vertex = bytearray((mutant / "vertex.bin").read_bytes())
        vertex[0] ^= 1
        (mutant / "vertex.bin").write_bytes(vertex)
        changed = dict(manifest)
        changed["vertex_blake3"] = b3sum_digest(mutant / "vertex.bin")
        write_json(mutant / "manifest.json", changed)
        expect_reject(mutant, "corrupted-vertex")

        mutant = root / "wrong-bo-size"
        clone(good, mutant)
        changed_bo = json.loads(json.dumps(bo_table))
        changed_bo["slots"][1]["size"] = 16
        write_json(mutant / "bo_table.json", changed_bo)
        expect_reject(mutant, "wrong-bo-size")

        mutant = root / "wrong-relocation"
        clone(good, mutant)
        changed = dict(manifest)
        changed["reloc_sites"] = [
            {"slot": 0, "ib_index": 0},
            *changed["reloc_sites"][1:],
        ]
        write_json(mutant / "manifest.json", changed)
        expect_reject(mutant, "wrong-relocation")

        mutant = root / "wrong-psc-with-refreshed-digest"
        clone(good, mutant)
        ib = bytearray((mutant / "ib.bin").read_bytes())
        words = [
            int.from_bytes(ib[offset : offset + 4], "little")
            for offset in range(0, len(ib), 4)
        ]
        registers, packets = decode_stream(words)
        del registers
        psc_packets = find_register_packets(words, packets, R300_VAP_PROG_STREAM_CNTL_0)
        if len(psc_packets) != 1:
            fail("calibration setup could not find PSC packet")
        psc_start = psc_packets[0][0]
        words[psc_start + 1] ^= 1
        (mutant / "ib.bin").write_bytes(
            b"".join(word.to_bytes(4, "little") for word in words)
        )
        changed = dict(manifest)
        refresh_ib_digest(changed, mutant)
        expect_reject(mutant, "wrong-psc-with-refreshed-digest")

        mutant = root / "truncated-ib"
        clone(good, mutant)
        ib = (mutant / "ib.bin").read_bytes()
        (mutant / "ib.bin").write_bytes(ib[:-4])
        expect_reject(mutant, "truncated-ib")

        mutant = root / "wrong-carrier-oracle"
        clone(good, mutant)
        changed = dict(manifest)
        changed["expected_carrier_dwords"] = list(
            reversed(changed["expected_carrier_dwords"])
        )
        write_json(mutant / "manifest.json", changed)
        expect_reject(mutant, "wrong-carrier-oracle")

    print(
        "tuple manifest artifacts are mutually consistent and reject calibrated mutants"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
