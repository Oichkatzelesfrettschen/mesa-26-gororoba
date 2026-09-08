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

    def __init__(self, reason, observation=None):
        super().__init__(reason)
        self.observation = observation


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


def scan_images(before: bytes, after: bytes, seed: int) -> dict:
    require(type(seed) is int and 0 <= seed <= 255, "stencil_byte")
    errors = []

    def check(condition, reason, artifact, offset, expected, observed):
        if not condition:
            errors.append({"stage": "raw_observation", "seed_role": f"seed-{seed:02x}",
                           "artifact": artifact, "offset": offset, "reason": reason,
                           "expected": expected, "observed": observed})

    for artifact, data in (("depth_before.bin", before), ("depth_after.bin", after)):
        check(len(data) == ALLOCATION_BYTES, "allocation_length", artifact,
              None, ALLOCATION_BYTES, len(data))
    initial_word = (INITIAL_DEPTH << 8) | seed
    for offset in range(BASE, min(STORAGE_END, len(before) - 3), 4):
        observed = WORD.unpack_from(before, offset)[0]
        check(observed == initial_word, "initial_storage", "depth_before.bin",
              offset, initial_word, observed)
    regions = (("prefix_guard", 0, BASE),
               ("suffix_guard", STORAGE_END, SUFFIX_END),
               ("unclaimed", SUFFIX_END, ALLOCATION_BYTES))
    region_counts = {}
    for name, begin, end in regions:
        changed = 0
        for offset in range(begin, min(end, len(before), len(after))):
            check(before[offset] == GUARD_FILL, "initial_" + name,
                  "depth_before.bin", offset, GUARD_FILL, before[offset])
            if before[offset] != after[offset]:
                changed += 1
                check(False, name + "_changed", "depth_after.bin", offset,
                      before[offset], after[offset])
        region_counts[name] = {"inspected_bytes": max(0, min(end, len(before), len(after)) - begin),
                               "changed_bytes": changed}

    classes = {"unchanged": 0, "depth_only": 0, "stencil_only": 0, "both": 0}
    depth_changes = []
    stencil_histogram: dict[int, int] = {}
    stencil_only_offsets = []
    off_target_stencil_offsets = []
    for offset in range(BASE, min(STORAGE_END, len(before) - 3, len(after) - 3), 4):
        old, new = WORD.unpack_from(before, offset)[0], WORD.unpack_from(after, offset)[0]
        depth_changed, stencil_changed = old >> 8 != new >> 8, old & 255 != new & 255
        key = ("both" if stencil_changed else "depth_only") if depth_changed else (
            "stencil_only" if stencil_changed else "unchanged")
        classes[key] += 1
        if stencil_changed and offset != PIXEL_OFFSET:
            off_target_stencil_offsets.append(offset)
        stencil = new & 255
        stencil_histogram[stencil] = stencil_histogram.get(stencil, 0) + 1
        if depth_changed:
            depth_changes.append((offset, new))
        elif stencil_changed:
            stencil_only_offsets.append(offset)

    check(len(depth_changes) == 1, "one_depth_location", "depth_after.bin",
          None, 1, len(depth_changes))
    offset, word = depth_changes[0] if len(depth_changes) == 1 else (None, None)
    if offset is not None:
        check(offset == PIXEL_OFFSET, "linear_address", "depth_after.bin",
              offset, PIXEL_OFFSET, offset)
        check(word >> 8 == MARKER_DEPTH, "depth_marker", "depth_after.bin",
              offset, MARKER_DEPTH, word >> 8)
    return {
        "seed": seed,
        "allocation_bytes": ALLOCATION_BYTES,
        "image_lengths": {"before": len(before), "after": len(after)},
        "storage_slots": sum(classes.values()),
        "storage_bytes": STORAGE_BYTES,
        "guard_bytes": BASE + GUARD_BYTES,
        "unclaimed_bytes": ALLOCATION_BYTES - SUFFIX_END,
        "slot_classes": classes,
        "depth_bo_offset": offset,
        "depth_surface_offset": offset - BASE if offset is not None else None,
        "depth_code": word >> 8 if word is not None else None,
        "selected_stencil": word & 255 if word is not None else None,
        "depth_changes": depth_changes,
        "regions": region_counts,
        "errors": errors,
        "stencil_only_offsets": stencil_only_offsets,
        "off_target_stencil_offsets": off_target_stencil_offsets,
        "after_stencil_histogram": stencil_histogram,
        "changed_bytes": sum(a != b for a, b in zip(before, after)),
        "before_sha256": hashlib.sha256(before).hexdigest(),
        "after_sha256": hashlib.sha256(after).hexdigest(),
    }


def observe(before: bytes, after: bytes, seed: int) -> dict:
    observation = scan_images(before, after, seed)
    if observation["errors"]:
        raise ObservationRefusal(observation["errors"][0]["reason"], observation)
    return observation


def classify_pair(a_before: bytes, a_after: bytes,
                  b_before: bytes, b_after: bytes) -> dict:
    a = scan_images(a_before, a_after, 0x5A)
    b = scan_images(b_before, b_after, 0xA5)
    outputs = a["selected_stencil"], b["selected_stencil"]
    if None in outputs:
        selected = None
    elif outputs == (0x5A, 0xA5):
        selected = "PRESERVED_FOR_BOTH_SEEDS"
    elif outputs == (0, 0):
        selected = "ZERO_FOR_BOTH_SEEDS"
    elif outputs == (255, 255):
        selected = "ONE_FOR_BOTH_SEEDS"
    else:
        selected = "OTHER_OBSERVED_PAIR"
    off_target = [len(run["off_target_stencil_offsets"]) for run in (a, b)]
    complete = all(run["storage_slots"] == 4160 for run in (a, b))
    preserved = False if any(off_target) else True if complete else None
    return {
        "schema": "r300-zb-linear-stencil-pair/2",
        "status": "REFUSED" if a["errors"] or b["errors"] else "OBSERVED",
        "qualification": "UNJUDGED",
        "errors": a["errors"] + b["errors"],
        "off_target_stencil_changes": off_target,
        "spatially_isolated": (False if any(off_target) else
                               None if a["errors"] or b["errors"] else True),
        "scope": "raw_byte_comparison_only",
        "general_stencil_function_established": False,
        "selected_slot_behavior": selected,
        "unwritten_slots_preserved": preserved,
        "bit_observations": bit_observations(0x5A, 0xA5, *outputs) if None not in outputs else None,
        "runs": [a, b],
    }


def read_image(path: Path) -> bytes:
    with path.open("rb") as source:
        data = source.read(ALLOCATION_BYTES + 1)
    return data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed-5a", required=True, type=Path,
                        help="directory containing depth_before.bin and depth_after.bin")
    parser.add_argument("--seed-a5", required=True, type=Path,
                        help="directory containing depth_before.bin and depth_after.bin")
    args = parser.parse_args(argv)
    images = []
    inputs = []
    input_errors = []
    for role, root in (("seed-5a", args.seed_5a), ("seed-a5", args.seed_a5)):
        for name in ("depth_before.bin", "depth_after.bin"):
            try:
                with (root / name).open("rb") as source:
                    data = source.read(ALLOCATION_BYTES + 1)
                images.append(data)
                inputs.append({"seed_role": role, "artifact": name,
                               "read_bytes": len(data),
                               "digest_scope": "bounded_prefix" if len(data) > ALLOCATION_BYTES else "complete_file",
                               "sha256": hashlib.sha256(data).hexdigest()})
            except OSError as exc:
                images.append(b"")
                inputs.append({"seed_role": role, "artifact": name,
                               "read_bytes": None, "sha256": None})
                input_errors.append({"stage": "input", "seed_role": role,
                                     "artifact": name, "reason": "image_read",
                                     "offset": None, "expected": "readable image",
                                     "observed": str(exc)})
    result = classify_pair(*images)
    result["inputs"] = inputs
    for run, offset in zip(result["runs"], (0, 2)):
        for side, record in zip(("before", "after"), inputs[offset:offset + 2]):
            if record.get("digest_scope") != "complete_file":
                run[side + "_sha256"] = None
    result["errors"] = input_errors + result["errors"]
    status = 1 if result["errors"] or result["spatially_isolated"] is not True else 0
    if input_errors:
        result["status"] = "INPUT_ERROR"
        status = 2
    try:
        print(json.dumps(result, sort_keys=True))
        sys.stdout.flush()
    except OSError:
        return 2
    return status


if __name__ == "__main__":
    sys.exit(main())
