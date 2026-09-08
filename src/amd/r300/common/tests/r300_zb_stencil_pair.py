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
import re
import shutil
import subprocess
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
    available_before, available_after = before is not None, after is not None
    before = before if available_before else b""
    after = after if available_after else b""
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
    result = {
        "seed": seed,
        "comparison_available": available_before and available_after,
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

    if not result["comparison_available"]:
        for name in ("storage_slots", "slot_classes", "depth_bo_offset", "depth_surface_offset",
                     "depth_code", "selected_stencil", "depth_changes", "stencil_only_offsets",
                     "off_target_stencil_offsets", "after_stencil_histogram", "changed_bytes"):
            result[name] = None
        result["regions"] = {name: {"inspected_bytes": None, "changed_bytes": None}
                             for name in ("prefix_guard", "suffix_guard", "unclaimed")}
        result["errors"] = [error for error in errors
                            if available_before and (error["reason"].startswith("initial_") or
                               (error["reason"] == "allocation_length" and
                                error["artifact"] == "depth_before.bin"))]
        if available_after:
            histogram = {}
            for offset in range(BASE, min(STORAGE_END, len(after) - 3), 4):
                stencil = after[offset]
                histogram[stencil] = histogram.get(stencil, 0) + 1
            result["after_stencil_histogram"] = histogram
        for side, available in (("before", available_before), ("after", available_after)):
            if not available:
                result[side + "_sha256"] = None
                result["image_lengths"][side] = None
                result["errors"].append({"stage": "raw_observation",
                                         "seed_role": f"seed-{seed:02x}",
                                         "artifact": f"depth_{side}.bin", "offset": None,
                                         "reason": "image_unavailable", "expected": "readable image",
                                         "observed": None})
    return result


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
    off_target = [len(run["off_target_stencil_offsets"]) if run["off_target_stencil_offsets"] is not None else None for run in (a, b)]
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


JSON_LIMIT = 65536
CONTEXT_FIELDS = (
    "platform", "boot_id", "kernel_release", "module_srcversion",
    "driver_elf_blake3", "mesa_source_sha", "build_profile",
    "application_sha256", "application_build_id",
    "arming_runner_sha256", "arming_runner_build_id",
)


class EvidenceError(ValueError):
    def __init__(self, code, expected=None, observed=None, infrastructure=False):
        super().__init__(code)
        self.code = code
        self.expected = expected
        self.observed = observed
        self.infrastructure = infrastructure


def exact(value, expected, code):
    if type(value) is not type(expected) or value != expected:
        raise EvidenceError(code, expected, value)


def parse_document(data):
    if len(data) > JSON_LIMIT:
        raise EvidenceError("metadata_length", JSON_LIMIT, len(data))

    def pairs(entries):
        result = {}
        for name, value in entries:
            if name in result:
                raise EvidenceError("duplicate_key", "unique key", name)
            result[name] = value
        return result

    def constant(value):
        raise EvidenceError("nonfinite_constant", "finite JSON", value)

    try:
        result = json.loads(data, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as exc:
        if isinstance(exc, EvidenceError):
            raise
        raise EvidenceError("malformed_json", "JSON object", str(exc)) from exc
    if type(result) is not dict:
        raise EvidenceError("metadata_type", "object", type(result).__name__)
    return result


def blake3_bytes(data):
    executable = shutil.which("b3sum")
    if executable is None:
        raise EvidenceError("hasher_missing", "PATH b3sum", None, True)
    try:
        process = subprocess.run([executable, "--no-names"], input=data,
                                 capture_output=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise EvidenceError("hasher_execution", "completed b3sum", str(exc), True) from exc
    if process.returncode != 0:
        raise EvidenceError("hasher_exit", 0, process.returncode, True)
    if process.stderr or re.fullmatch(rb"[0-9a-f]{64}\n?", process.stdout) is None:
        raise EvidenceError("hasher_output", "one lowercase BLAKE3 digest",
                            repr(process.stdout[:256]), True)
    return process.stdout.decode("ascii").strip()


def validate_outcome(document, before, after, run):
    if not run["comparison_available"]:
        raise EvidenceError("raw_inputs_unavailable", "readable image pair", None)
    expected = {
        "schema": "r3v-native-zb-depth-discovery-outcome/1",
        "verdict": "CONTROL_PASS", "scenario": f"z24-linear-seed-{run['seed']:02x}",
        "arm": "measure", "pixel_x": 37, "pixel_y": 21,
        "initial_depth_code": "0x800000", "initial_stencil": f"0x{run['seed']:02x}",
        "marker_depth_code": "0x400000", "initialization_declared": True,
        "allocation_bytes": 24576, "envelope_offset": 2048, "envelope_bytes": 16640,
        "submit_result": 0, "queue_status": "COMPLETED", "judged": True,
        "slots_inspected": run["storage_slots"],
        "depth_locations": len(run["depth_changes"]),
        "guard_bytes_inspected": sum(run["regions"][name]["inspected_bytes"]
                                     for name in ("prefix_guard", "suffix_guard")),
        "guard_bytes_changed": sum(run["regions"][name]["changed_bytes"]
                                   for name in ("prefix_guard", "suffix_guard")),
        "unclaimed_bytes_inspected": run["regions"]["unclaimed"]["inspected_bytes"],
        "unclaimed_bytes_changed": run["regions"]["unclaimed"]["changed_bytes"],
        "color_judged": True, "color_exact": True,
        "color_inside_colored": 1, "color_inside_samples": 1,
        "color_outside_colored": 0, "color_outside_samples": 4095,
    }
    expected.update({"slots_" + name: count for name, count in run["slot_classes"].items()})
    changes = []
    for offset in range(BASE, min(STORAGE_END, len(before) - 3, len(after) - 3), 4):
        old, new = WORD.unpack_from(before, offset)[0], WORD.unpack_from(after, offset)[0]
        if old != new:
            changes.append({"offset": offset, "before": f"0x{old:08x}",
                            "after": f"0x{new:08x}", "depth_changed": old >> 8 != new >> 8,
                            "stencil_changed": old & 255 != new & 255,
                            "depth_before": f"0x{old >> 8:06x}", "depth_after": f"0x{new >> 8:06x}",
                            "stencil_before": f"0x{old & 255:02x}", "stencil_after": f"0x{new & 255:02x}"})
    expected["change_overflow"] = len(changes) > 64
    for name, value in expected.items():
        if name not in document:
            raise EvidenceError("missing_field:" + name, value, None)
        exact(document[name], value, "outcome:" + name)
    reported = document.get("changes")
    if type(reported) is not list or len(reported) != min(64, len(changes)):
        raise EvidenceError("outcome:changes", changes[:64], reported)
    for actual, wanted in zip(reported, changes):
        if type(actual) is not dict or actual.keys() != wanted.keys():
            raise EvidenceError("outcome:change_fields", list(wanted), actual)
        for name, value in wanted.items():
            exact(actual[name], value, "outcome:changes:" + name)
    digest = document.get("initial_image_blake3")
    if type(digest) is not str or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise EvidenceError("outcome:initial_image_blake3_format", "lowercase digest", digest)
    exact(digest, blake3_bytes(before), "initial_image_digest")
    if document.keys() != expected.keys() | {"changes", "initial_image_blake3"}:
        raise EvidenceError("outcome:fields", sorted(expected.keys() | {"changes", "initial_image_blake3"}),
                            sorted(document))


def exact_keys(value, names, code):
    if type(value) is not dict or set(value) != set(names):
        raise EvidenceError(code, sorted(names), sorted(value) if type(value) is dict else None)


def validate_context(context, role, artifacts):
    if type(context) is not dict:
        raise EvidenceError("context:type", "object", type(context).__name__)
    exact(context.get("schema"), "r300-zb-stencil-pair-context/1", "context:schema")
    exact_keys(context, ("schema", "declaration", "runs"), "context:fields")
    declaration = context.get("declaration")
    runs = context.get("runs")
    if type(declaration) is not dict or type(runs) is not dict:
        raise EvidenceError("context:structure", "declaration and runs objects", None)
    exact_keys(runs, ("seed-5a", "seed-a5"), "context:run_fields")
    record = runs.get(role)
    if type(record) is not dict or type(record.get("identity")) is not dict:
        raise EvidenceError("context:run", role, record)
    exact_keys(record, ("identity", "authority", "seal_sha256", "artifacts"), "context:record_fields")
    identity = record["identity"]
    for name in CONTEXT_FIELDS:
        wanted = declaration.get(name)
        if type(wanted) is not str or not wanted.strip():
            raise EvidenceError("context:declaration:" + name, "nonempty identity", wanted)
        exact(identity.get(name), wanted, "context:identity:" + name)
    exact_keys(declaration, CONTEXT_FIELDS, "context:declaration_fields")
    exact_keys(identity, CONTEXT_FIELDS, "context:identity_fields")
    platform = declaration["platform"]
    if "subsystem" not in platform or "1028:022a" not in platform or "Vostro 1000" not in platform:
        raise EvidenceError("context:platform", "Vostro 1000 with subsystem identity", platform)
    for name, pattern in (("boot_id", r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}"),
                          ("mesa_source_sha", r"[0-9a-f]{40}"),
                          ("driver_elf_blake3", r"[0-9a-f]{64}"),
                          ("application_sha256", r"[0-9a-f]{64}"),
                          ("arming_runner_sha256", r"[0-9a-f]{64}"),
                          ("application_build_id", r"[0-9a-f]{40}"),
                          ("arming_runner_build_id", r"[0-9a-f]{40}")):
        if re.fullmatch(pattern, declaration[name]) is None:
            raise EvidenceError("context:format:" + name, pattern, declaration[name])
    exact(record.get("authority"), "retained-bundle-sha256", "context:authority")
    seal = record.get("seal_sha256")
    if type(seal) is not str or re.fullmatch(r"[0-9a-f]{64}", seal) is None:
        raise EvidenceError("context:seal", "verified seal digest", seal)
    verified = record.get("artifacts")
    if type(verified) is not dict:
        raise EvidenceError("context:artifacts", "verified artifact map", verified)
    exact_keys(verified, ("depth_before.bin", "depth_after.bin", "zb_depth_discovery_outcome.json"),
               "context:artifact_fields")
    for name, data in artifacts.items():
        if data is None:
            raise EvidenceError("context:artifact_unavailable:" + name, "readable artifact", None)
        exact(verified.get(name), hashlib.sha256(data).hexdigest(), "context:artifact:" + name)


def qualify_pair(result, images, outcomes, context):
    """Join bytes to a caller-supplied context from the receipt verifier.

    The caller owns the declaration and receipt-verifier provenance. A
    normalized context is an input contract, rather than hardware attestation.
    """
    errors = result["errors"]
    for index, role in enumerate(("seed-5a", "seed-a5")):
        before, after = images[index * 2:index * 2 + 2]
        data = outcomes[index]
        run = result["runs"][index]
        run["outcome_sha256"] = hashlib.sha256(data).hexdigest() if data is not None and len(data) <= JSON_LIMIT else None
        for stage in ("outcome", "context"):
            try:
                if stage == "outcome":
                    if data is None:
                        raise EvidenceError("outcome_missing", "outcome JSON", None)
                    validate_outcome(parse_document(data), before, after, run)
                else:
                    if context is None:
                        raise EvidenceError("context_missing", "verified pair context", None)
                    validate_context(context, role, {"depth_before.bin": before,
                                     "depth_after.bin": after,
                                     "zb_depth_discovery_outcome.json": data})
            except EvidenceError as exc:
                errors.append({"stage": stage, "seed_role": role,
                               "artifact": "zb_depth_discovery_outcome.json" if stage == "outcome" else "pair_context",
                               "offset": None, "reason": exc.code,
                               "expected": exc.expected, "observed": exc.observed,
                               "infrastructure": exc.infrastructure})
    if result["spatially_isolated"] is not True:
        errors.append({"stage": "qualification", "seed_role": "pair",
                       "artifact": "depth_after.bin", "offset": None,
                       "reason": "spatial_isolation", "expected": True,
                       "observed": result["spatially_isolated"]})
    result["qualification"] = "REFUSED" if errors else "QUALIFIED_ISOLATED_PAIR"
    result["scope"] = "supplied_verified_receipt_context"
    result["status"] = "REFUSED" if errors else "QUALIFIED"
    if any(error.get("infrastructure", False) for error in errors):
        result["status"] = "INPUT_ERROR"
        return 2
    return 1 if errors else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed-5a", required=True, type=Path,
                        help="directory containing depth_before.bin and depth_after.bin")
    parser.add_argument("--seed-a5", required=True, type=Path,
                        help="directory containing depth_before.bin and depth_after.bin")
    parser.add_argument("--pair-context", type=Path)
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
                images.append(None)
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
    outcomes = []
    for root in (args.seed_5a, args.seed_a5):
        try:
            with (root / "zb_depth_discovery_outcome.json").open("rb") as source:
                outcomes.append(source.read(JSON_LIMIT + 1))
        except OSError as exc:
            outcomes.append(None)
            if not isinstance(exc, FileNotFoundError):
                result["errors"].append({"stage": "input", "seed_role": "seed-5a" if root == args.seed_5a else "seed-a5",
                                         "artifact": "zb_depth_discovery_outcome.json", "offset": None,
                                         "reason": "outcome_read", "expected": "readable outcome",
                                         "observed": str(exc), "infrastructure": True})
    context = None
    if args.pair_context is not None:
        try:
            with args.pair_context.open("rb") as source:
                context = parse_document(source.read(JSON_LIMIT + 1))
        except (OSError, EvidenceError) as exc:
            result["errors"].append({"stage": "context", "seed_role": "pair",
                                     "artifact": "pair_context", "offset": None,
                                     "reason": "context_read", "expected": "valid context",
                                     "observed": str(exc), "infrastructure": isinstance(exc, OSError)})
    qualified_status = qualify_pair(result, images, outcomes, context)
    status = 2 if input_errors else qualified_status
    if input_errors:
        result["status"] = "INPUT_ERROR"
    try:
        print(json.dumps(result, sort_keys=True))
        sys.stdout.flush()
    except OSError:
        return 2
    return status


if __name__ == "__main__":
    sys.exit(main())
