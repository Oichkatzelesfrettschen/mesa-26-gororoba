# SPDX-License-Identifier: MIT
"""Compare raw Z24/S8 images from the two linear stencil-seed scenarios.

The fixed geometry is an independent statement of the declared linear
experiment in r300_zb_depth_discovery.c. Packed words have depth in bits
31:8 and stencil in bits 7:0. The byte scan locates depth changes before
checking their offsets; no tiled address resolver participates.

A classified pair describes these input bytes. It neither authenticates a
hardware execution nor supplies a general stencil transfer function.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


ALLOCATION_BYTES = 24576
BASE = 2048
PITCH_BYTES = 64 * 4
STORAGE_BYTES = 65 * PITCH_BYTES
STORAGE_END = BASE + STORAGE_BYTES
GUARD_BYTES = 2048
SUFFIX_END = STORAGE_END + GUARD_BYTES
PIXEL_OFFSET = BASE + 21 * PITCH_BYTES + 37 * 4
INITIAL_DEPTH = 0x800000
MARKER_DEPTH = 0x400000
GUARD_FILL = 0xA3
WORD = struct.Struct("<I")


class ObservationRefusal(ValueError):
    """The supplied bytes do not satisfy the declared pair experiment."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ObservationRefusal(reason)


def bit_observations(seed_a: int, seed_b: int, out_a: int, out_b: int) -> dict:
    """Partition bit positions by the two observed input/output pairs.

    Complementary seeds expose both input values at each bit position.
    Cross-bit dependencies and behavior on untested bytes remain unknown.
    """
    require(all(type(v) is int and 0 <= v <= 255
                for v in (seed_a, seed_b, out_a, out_b)), "stencil_byte")
    require(seed_a ^ seed_b == 255, "complementary_seeds")
    same = (~(out_a ^ seed_a) & ~(out_b ^ seed_b)) & 255
    inverted = (out_a ^ seed_a) & (out_b ^ seed_b)
    zero = (~out_a & ~out_b) & 255
    one = out_a & out_b
    return {"same_as_input": same, "opposite_to_input": inverted,
            "zero_for_both": zero, "one_for_both": one}


def observe(before: bytes, after: bytes, seed: int) -> dict:
    require(type(seed) is int and 0 <= seed <= 255, "stencil_byte")
    require(len(before) == len(after) == ALLOCATION_BYTES, "allocation_length")
    initial_word = (INITIAL_DEPTH << 8) | seed
    for offset in range(BASE, STORAGE_END, 4):
        require(WORD.unpack_from(before, offset)[0] == initial_word,
                "initial_storage")
    require(all(value == GUARD_FILL for value in before[:BASE]),
            "initial_prefix_guard")
    require(all(value == GUARD_FILL for value in before[STORAGE_END:SUFFIX_END]),
            "initial_suffix_guard")
    require(all(value == GUARD_FILL for value in before[SUFFIX_END:]),
            "initial_unclaimed")

    prefix_changed = sum(a != b for a, b in zip(before[:BASE], after[:BASE]))
    suffix_changed = sum(a != b for a, b in
                         zip(before[STORAGE_END:SUFFIX_END],
                             after[STORAGE_END:SUFFIX_END]))
    slack_changed = sum(a != b for a, b in
                        zip(before[SUFFIX_END:], after[SUFFIX_END:]))
    require(prefix_changed == 0, "prefix_guard_changed")
    require(suffix_changed == 0, "suffix_guard_changed")
    require(slack_changed == 0, "unclaimed_changed")

    classes = {"unchanged": 0, "depth_only": 0, "stencil_only": 0, "both": 0}
    depth_changes = []
    stencil_histogram: dict[int, int] = {}
    stencil_only_offsets = []
    for offset in range(BASE, STORAGE_END, 4):
        old, new = WORD.unpack_from(before, offset)[0], WORD.unpack_from(after, offset)[0]
        depth_changed, stencil_changed = old >> 8 != new >> 8, old & 255 != new & 255
        key = ("both" if stencil_changed else "depth_only") if depth_changed else (
            "stencil_only" if stencil_changed else "unchanged")
        classes[key] += 1
        stencil = new & 255
        stencil_histogram[stencil] = stencil_histogram.get(stencil, 0) + 1
        if depth_changed:
            depth_changes.append((offset, new))
        elif stencil_changed:
            stencil_only_offsets.append(offset)

    require(len(depth_changes) == 1, "one_depth_location")
    offset, word = depth_changes[0]
    require(offset == PIXEL_OFFSET, "linear_address")
    require(word >> 8 == MARKER_DEPTH, "depth_marker")
    return {
        "seed": seed,
        "allocation_bytes": ALLOCATION_BYTES,
        "storage_slots": sum(classes.values()),
        "storage_bytes": STORAGE_BYTES,
        "guard_bytes": BASE + GUARD_BYTES,
        "unclaimed_bytes": ALLOCATION_BYTES - SUFFIX_END,
        "slot_classes": classes,
        "depth_bo_offset": offset,
        "depth_surface_offset": offset - BASE,
        "depth_code": word >> 8,
        "selected_stencil": word & 255,
        "stencil_only_offsets": stencil_only_offsets,
        "after_stencil_histogram": stencil_histogram,
        "changed_bytes": sum(a != b for a, b in zip(before, after)),
        "before_sha256": hashlib.sha256(before).hexdigest(),
        "after_sha256": hashlib.sha256(after).hexdigest(),
    }


def classify_pair(a_before: bytes, a_after: bytes,
                  b_before: bytes, b_after: bytes) -> dict:
    a = observe(a_before, a_after, 0x5A)
    b = observe(b_before, b_after, 0xA5)
    outputs = a["selected_stencil"], b["selected_stencil"]
    if outputs == (0x5A, 0xA5):
        selected = "PRESERVED_FOR_BOTH_SEEDS"
    elif outputs == (0, 0):
        selected = "ZERO_FOR_BOTH_SEEDS"
    elif outputs == (255, 255):
        selected = "ONE_FOR_BOTH_SEEDS"
    else:
        selected = "OTHER_OBSERVED_PAIR"
    return {
        "schema": "r300-zb-linear-stencil-pair/1",
        "status": "CLASSIFIED",
        "scope": "raw_byte_comparison_only",
        "general_stencil_function_established": False,
        "selected_behavior": selected,
        "unwritten_slots_preserved": not (
            a["stencil_only_offsets"] or b["stencil_only_offsets"]),
        "bit_observations": bit_observations(0x5A, 0xA5, *outputs),
        "runs": [a, b],
    }


def read_image(path: Path) -> bytes:
    with path.open("rb") as source:
        data = source.read(ALLOCATION_BYTES + 1)
    require(len(data) == ALLOCATION_BYTES, "allocation_length")
    return data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed-5a", required=True, type=Path,
                        help="directory containing depth_before.bin and depth_after.bin")
    parser.add_argument("--seed-a5", required=True, type=Path,
                        help="directory containing depth_before.bin and depth_after.bin")
    args = parser.parse_args(argv)
    try:
        images = [read_image(root / name)
                  for root in (args.seed_5a, args.seed_a5)
                  for name in ("depth_before.bin", "depth_after.bin")]
        result = classify_pair(*images)
        status = 0
    except ObservationRefusal as exc:
        result = {"status": "REFUSED", "reason": str(exc)}
        status = 1
    except OSError as exc:
        result = {"status": "INPUT_ERROR", "reason": str(exc)}
        status = 2
    try:
        print(json.dumps(result, sort_keys=True))
        sys.stdout.flush()
    except OSError:
        return 2
    return status


if __name__ == "__main__":
    sys.exit(main())
