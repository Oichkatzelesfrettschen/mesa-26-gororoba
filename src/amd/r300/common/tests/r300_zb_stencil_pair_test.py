# SPDX-License-Identifier: MIT
"""Exercise paired-stencil classification with independent byte fixtures."""

import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import r300_zb_stencil_pair as pair


def image_pair(seed, output=None, offset=7572, code=0x400000):
    # Literal geometry keeps an implementation constant from changing its oracle.
    before = bytearray([0xA3]) * 24576
    for at in range(2048, 18688, 4):
        struct.pack_into("<I", before, at, (0x800000 << 8) | seed)
    after = before.copy()
    struct.pack_into("<I", after, offset,
                     (code << 8) | (seed if output is None else output))
    return bytes(before), bytes(after)


class PairTests(unittest.TestCase):
    def classify(self, out_a=0x5A, out_b=0xA5):
        return pair.classify_pair(*image_pair(0x5A, out_a),
                                  *image_pair(0xA5, out_b))

    def test_preserved_pair(self):
        result = self.classify()
        self.assertEqual(result["selected_slot_behavior"], "PRESERVED_FOR_BOTH_SEEDS")
        self.assertTrue(result["unwritten_slots_preserved"])
        self.assertFalse(result["general_stencil_function_established"])
        self.assertEqual(result["scope"], "raw_byte_comparison_only")
        self.assertEqual(result["bit_observations"]["same_as_input"], 255)
        for seed, run in zip((0x5A, 0xA5), result["runs"]):
            before, after = image_pair(seed)
            self.assertEqual(run["seed"], seed)
            self.assertEqual(run["before_sha256"], hashlib.sha256(before).hexdigest())
            self.assertEqual(run["after_sha256"], hashlib.sha256(after).hexdigest())
            self.assertEqual(run["storage_slots"], 4160)
            self.assertEqual(run["after_stencil_histogram"], {seed: 4160})
            self.assertEqual(run["regions"], {
                "prefix_guard": {"inspected_bytes": 2048, "changed_bytes": 0},
                "suffix_guard": {"inspected_bytes": 2048, "changed_bytes": 0},
                "unclaimed": {"inspected_bytes": 3840, "changed_bytes": 0}})
            self.assertEqual(run["depth_bo_offset"], 7572)
            self.assertEqual(run["depth_surface_offset"], 5524)
            self.assertEqual(run["changed_bytes"], 1)
            self.assertEqual(run["slot_classes"], {
                "unchanged": 4159, "depth_only": 1, "stencil_only": 0, "both": 0})
            self.assertEqual(sum(run["slot_classes"].values()), 4160)
            self.assertEqual(run["storage_bytes"] + run["guard_bytes"] +
                             run["unclaimed_bytes"], 24576)
            self.assertEqual((run["storage_bytes"], run["guard_bytes"],
                              run["unclaimed_bytes"]), (16640, 4096, 3840))
            self.assertEqual(sum(run["after_stencil_histogram"].values()), 4160)

    def test_zero_replacement_is_classified(self):
        result = self.classify(0, 0)
        self.assertEqual(result["selected_slot_behavior"], "ZERO_FOR_BOTH_SEEDS")
        self.assertEqual(result["bit_observations"]["zero_for_both"], 255)
        self.assertEqual(result["runs"][0]["slot_classes"]["both"], 1)
        self.assertEqual(result["runs"][0]["changed_bytes"], 2)

    def test_one_replacement_is_classified(self):
        result = self.classify(255, 255)
        self.assertEqual(result["selected_slot_behavior"], "ONE_FOR_BOTH_SEEDS")
        self.assertEqual(result["bit_observations"]["one_for_both"], 255)

    def test_complement_and_mixed_behavior(self):
        result = self.classify(0xA5, 0x5A)
        self.assertEqual(result["selected_slot_behavior"], "OTHER_OBSERVED_PAIR")
        self.assertEqual(result["bit_observations"]["opposite_to_input"], 255)
        masks = pair.bit_observations(0x5A, 0xA5, 0x50, 0xA0)
        self.assertEqual(masks, {"same_as_input": 0xF0, "opposite_to_input": 0,
                                 "zero_for_both": 0x0F, "one_for_both": 0})

    def test_every_output_pair_partitions_bits(self):
        for a in range(256):
            for b in range(256):
                masks = pair.bit_observations(0x5A, 0xA5, a, b)
                values = list(masks.values())
                self.assertEqual(sum(values), 255)
                for i, value in enumerate(values):
                    self.assertTrue(0 <= value <= 255)
                    self.assertTrue(all((value & other) == 0 for other in values[i + 1:]))

    def test_cross_bit_permutation_remains_unresolved(self):
        # Bits 1 and 3 are both set in 0x5a and clear in 0xa5. Swapping
        # them preserves these seeds but changes the input byte 0x02.
        def swap(value):
            return (value & ~10) | ((value & 2) << 2) | ((value & 8) >> 2)
        self.assertEqual((swap(0x5A), swap(0xA5)), (0x5A, 0xA5))
        self.assertEqual(swap(2), 8)
        self.assertEqual(self.classify(swap(0x5A), swap(0xA5))["bit_observations"],
                         {"same_as_input": 255, "opposite_to_input": 0,
                          "zero_for_both": 0, "one_for_both": 0})
        self.assertFalse(self.classify(swap(0x5A), swap(0xA5))[
            "general_stencil_function_established"])

    def test_invalid_bit_inputs(self):
        for values in ((0, 0, 0, 0), (0x5A, 0, 1, 2)):
            with self.subTest(values=values), self.assertRaisesRegex(
                    pair.ObservationRefusal, "^complementary_seeds$"):
                pair.bit_observations(*values)
        for value in (-1, 256, True, "5a"):
            with self.subTest(value=value), self.assertRaisesRegex(
                    pair.ObservationRefusal, "^stencil_byte$"):
                pair.bit_observations(value, 0xA5, 0, 0)

    def test_stencil_only_changes_are_not_hidden(self):
        before, after = image_pair(0x5A)
        modified = bytearray(after)
        modified[18684] = 0
        result = pair.classify_pair(before, bytes(modified), *image_pair(0xA5))
        self.assertEqual(result["selected_slot_behavior"], "PRESERVED_FOR_BOTH_SEEDS")
        self.assertFalse(result["unwritten_slots_preserved"])
        self.assertEqual(result["runs"][0]["stencil_only_offsets"], [18684])
        self.assertEqual(result["runs"][0]["slot_classes"]["stencil_only"], 1)

    def test_mass_clobber_retains_selected_observation(self):
        for selected in (None, 0):
            images = []
            for seed in (0x5A, 0xA5):
                before, after = image_pair(seed, selected)
                after = bytearray(after)
                for offset in range(2048, 18688, 4):
                    if offset != 7572:
                        after[offset] = 0
                images.extend((before, bytes(after)))
            result = pair.classify_pair(*images)
            self.assertEqual(result["off_target_stencil_changes"], [4159, 4159])
            self.assertFalse(result["spatially_isolated"])
            self.assertFalse(result["unwritten_slots_preserved"])
            self.assertEqual(result["selected_slot_behavior"],
                             "PRESERVED_FOR_BOTH_SEEDS" if selected is None else
                             "ZERO_FOR_BOTH_SEEDS")

    def test_off_target_combined_write_is_counted(self):
        before, after = image_pair(0x5A)
        after = bytearray(after)
        struct.pack_into("<I", after, 18684, 0x40000000)
        result = pair.classify_pair(before, bytes(after), *image_pair(0xA5))
        self.assertEqual(result["off_target_stencil_changes"], [1, 0])
        self.assertFalse(result["unwritten_slots_preserved"])
        self.assertIsNone(result["selected_slot_behavior"])

    def test_missing_coverage_is_unjudged(self):
        result = pair.classify_pair(b"", b"", *image_pair(0xA5))
        self.assertIsNone(result["unwritten_slots_preserved"])
        self.assertIsNone(result["spatially_isolated"])
        self.assertIsNone(result["selected_slot_behavior"])

    def test_refusal_retains_complete_measurements(self):
        before, after = image_pair(0x5A, offset=5524)
        with self.assertRaises(pair.ObservationRefusal) as refusal:
            pair.observe(before, after, 0x5A)
        run = refusal.exception.observation
        self.assertEqual(run["storage_slots"], 4160)
        self.assertEqual(run["depth_bo_offset"], 5524)
        self.assertEqual(run["errors"][0]["offset"], 5524)
        self.assertEqual(run["errors"][0]["expected"], 7572)
        self.assertEqual(run["regions"]["unclaimed"],
                         {"inspected_bytes": 3840, "changed_bytes": 0})

    def test_zero_seed_evidence_cannot_be_reused(self):
        result = pair.classify_pair(*image_pair(0), *image_pair(0))
        self.assertEqual(result["status"], "REFUSED")
        self.assertEqual(result["errors"][0]["reason"], "initial_storage")

    def test_swapped_seed_roles_refuse(self):
        result = pair.classify_pair(*image_pair(0xA5), *image_pair(0x5A))
        self.assertEqual(result["status"], "REFUSED")
        self.assertEqual(result["errors"][0]["reason"], "initial_storage")

    def test_initial_storage_is_checked_even_if_unchanged(self):
        before, after = map(bytearray, image_pair(0x5A))
        before[18686] ^= 1
        after[18686] ^= 1
        with self.assertRaisesRegex(pair.ObservationRefusal, "^initial_storage$"):
            pair.observe(bytes(before), bytes(after), 0x5A)

    def test_initial_nonstorage_classes_are_checked(self):
        for offset, reason in ((2047, "initial_prefix_guard"),
                               (20735, "initial_suffix_guard"),
                               (24575, "initial_unclaimed")):
            before, after = map(bytearray, image_pair(0x5A))
            before[offset] ^= 1
            after[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaisesRegex(
                    pair.ObservationRefusal, f"^{reason}$"):
                pair.observe(bytes(before), bytes(after), 0x5A)

    def test_wrong_linear_origin_refuses_inside_storage(self):
        before, after = image_pair(0x5A, offset=5524)
        with self.assertRaisesRegex(pair.ObservationRefusal, "^linear_address$"):
            pair.observe(before, after, 0x5A)

    def test_wrong_marker_refuses_at_right_address(self):
        before, after = image_pair(0x5A, code=0x400001)
        with self.assertRaisesRegex(pair.ObservationRefusal, "^depth_marker$"):
            pair.observe(before, after, 0x5A)

    def test_no_depth_write_and_stencil_only_refuse(self):
        before, _ = image_pair(0x5A)
        for image in (before, before[:7572] + bytes([0]) + before[7573:]):
            with self.subTest(image=image[7572:7576]), self.assertRaisesRegex(
                    pair.ObservationRefusal, "^one_depth_location$"):
                pair.observe(before, image, 0x5A)

    def test_second_depth_write_refuses_at_final_slot(self):
        before, after = image_pair(0x5A)
        modified = bytearray(after)
        struct.pack_into("<I", modified, 18684, 0x4000005A)
        with self.assertRaisesRegex(pair.ObservationRefusal, "^one_depth_location$"):
            pair.observe(before, bytes(modified), 0x5A)

    def test_each_nonstorage_range_refuses(self):
        for offset, reason in ((0, "prefix_guard_changed"),
                               (2047, "prefix_guard_changed"),
                               (18688, "suffix_guard_changed"),
                               (20735, "suffix_guard_changed"),
                               (20736, "unclaimed_changed"),
                               (24575, "unclaimed_changed")):
            before, after = image_pair(0x5A)
            modified = bytearray(after)
            modified[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaisesRegex(
                    pair.ObservationRefusal, f"^{reason}$"):
                pair.observe(before, bytes(modified), 0x5A)

    def test_both_lengths_are_exact(self):
        before, after = image_pair(0x5A)
        for a, b in ((before[:-1], after), (before, after[:-1]),
                     (before + b"x", after), (before, after + b"x")):
            with self.subTest(lengths=(len(a), len(b))), self.assertRaisesRegex(
                    pair.ObservationRefusal, "^allocation_length$"):
                pair.observe(a, b, 0x5A)

    def test_byte_order_matters(self):
        before, after = image_pair(0x5A)
        modified = bytearray(after)
        struct.pack_into(">I", modified, 7572, 0x4000005A)
        with self.assertRaisesRegex(pair.ObservationRefusal, "^depth_marker$"):
            pair.observe(before, bytes(modified), 0x5A)


def outcome_fixture(seed):
    before, after = image_pair(seed)
    digest = subprocess.run(["b3sum", "--no-names"], input=before,
                            capture_output=True, check=True).stdout.decode().strip()
    return {
        "schema": "r3v-native-zb-depth-discovery-outcome/1", "verdict": "CONTROL_PASS",
        "scenario": f"z24-linear-seed-{seed:02x}", "arm": "measure",
        "pixel_x": 37, "pixel_y": 21, "initial_depth_code": "0x800000",
        "initial_stencil": f"0x{seed:02x}", "marker_depth_code": "0x400000",
        "initial_image_blake3": digest, "initialization_declared": True,
        "allocation_bytes": 24576, "envelope_offset": 2048, "envelope_bytes": 16640,
        "submit_result": 0, "queue_status": "COMPLETED", "judged": True,
        "slots_inspected": 4160, "slots_unchanged": 4159, "slots_depth_only": 1,
        "slots_stencil_only": 0, "slots_both": 0, "depth_locations": 1,
        "guard_bytes_inspected": 4096, "guard_bytes_changed": 0,
        "unclaimed_bytes_inspected": 3840, "unclaimed_bytes_changed": 0,
        "change_overflow": False,
        "changes": [{"offset": 7572, "before": f"0x800000{seed:02x}",
                     "after": f"0x400000{seed:02x}", "depth_changed": True,
                     "stencil_changed": False, "depth_before": "0x800000",
                     "depth_after": "0x400000", "stencil_before": f"0x{seed:02x}",
                     "stencil_after": f"0x{seed:02x}"}],
        "color_judged": True, "color_exact": True, "color_inside_colored": 1,
        "color_inside_samples": 1, "color_outside_colored": 0,
        "color_outside_samples": 4095,
    }


def context_fixture(images, outcomes):
    identity = {"platform": "Dell Vostro 1000 PCI 1002:5974 subsystem 1028:022a",
                "boot_id": "6222574b-495d-411e-9a88-1703bad493c8",
                "kernel_release": "test-kernel", "module_srcversion": "test-module",
                "driver_elf_blake3": "1" * 64, "mesa_source_sha": "2" * 40,
                "build_profile": "profile 4 release", "application_sha256": "3" * 64,
                "application_build_id": "4" * 40, "arming_runner_sha256": "5" * 64,
                "arming_runner_build_id": "6" * 40}
    context = {"schema": "r300-zb-stencil-pair-context/1", "declaration": identity,
               "runs": {}}
    for index, role in enumerate(("seed-5a", "seed-a5")):
        context["runs"][role] = {
            "identity": identity.copy(), "authority": "retained-bundle-sha256",
            "seal_sha256": str(index + 7) * 64,
            "artifacts": dict(zip(("depth_before.bin", "depth_after.bin",
                                   "zb_depth_discovery_outcome.json"),
                                  (hashlib.sha256(data).hexdigest() for data in
                                   (*images[2 * index:2 * index + 2], outcomes[index]))))}
    return context


class EvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.documents = [outcome_fixture(seed) for seed in (0x5A, 0xA5)]

    def setUp(self):
        self.images = (*image_pair(0x5A), *image_pair(0xA5))
        self.outcomes = [json.dumps(document).encode() for document in self.documents]
        self.context = context_fixture(self.images, self.outcomes)

    def qualify(self):
        result = pair.classify_pair(*self.images)
        status = pair.qualify_pair(result, self.images, self.outcomes, self.context)
        return status, result

    def assert_failure(self, code, expected_status=1):
        status, result = self.qualify()
        self.assertEqual(status, expected_status, result)
        self.assertIn(code, [error["reason"] for error in result["errors"]])
        self.assertTrue(all(run["before_sha256"] for run in result["runs"]))
        return result

    def test_real_digest_and_distinct_seed_artifacts(self):
        status, result = self.qualify()
        self.assertEqual(status, 0, result)
        self.assertEqual(result["qualification"], "QUALIFIED_ISOLATED_PAIR")
        self.assertNotEqual(self.documents[0]["initial_image_blake3"],
                            self.documents[1]["initial_image_blake3"])

    def test_metadata_one_fact_mutations(self):
        mutations = {"schema": "unsupported", "arm": "never", "pixel_x": 38,
                     "pixel_y": 20, "initial_depth_code": "0X800000",
                     "marker_depth_code": "0x400001", "initial_stencil": "0xa5",
                     "scenario": "z24-linear", "submit_result": False,
                     "queue_status": "SUBMITTED", "judged": False,
                     "initialization_declared": False, "color_exact": False,
                     "color_judged": False, "color_outside_colored": 1,
                     "allocation_bytes": 24575, "envelope_offset": 0,
                     "envelope_bytes": 16636, "slots_inspected": 4159,
                     "slots_unchanged": 0, "slots_depth_only": 0,
                     "slots_stencil_only": 1, "slots_both": 1,
                     "depth_locations": 0, "guard_bytes_inspected": 0,
                     "guard_bytes_changed": 1, "unclaimed_bytes_inspected": 0,
                     "unclaimed_bytes_changed": 1, "change_overflow": True}
        original = self.outcomes[0]
        for name, value in mutations.items():
            with self.subTest(field=name):
                document = copy.deepcopy(self.documents[0])
                document[name] = value
                self.outcomes[0] = json.dumps(document).encode()
                self.assert_failure("outcome:" + name)
        self.outcomes[0] = original
        self.assertEqual(self.qualify()[0], 0)

    def test_missing_field_and_digest(self):
        document = copy.deepcopy(self.documents[0])
        del document["submit_result"]
        self.outcomes[0] = json.dumps(document).encode()
        self.assert_failure("missing_field:submit_result")
        document = copy.deepcopy(self.documents[0])
        document["initial_image_blake3"] = "0" * 64
        self.outcomes[0] = json.dumps(document).encode()
        self.assert_failure("initial_image_digest")

    def test_document_parser_refusals(self):
        for data, code in ((None, "outcome_missing"), (b"{", "malformed_json"),
                           (b"[]", "metadata_type"), (b'{"a":1,"a":2}', "duplicate_key"),
                           (b'{"a":NaN}', "nonfinite_constant"),
                           (b" " * 65537, "metadata_length")):
            with self.subTest(code=code):
                self.outcomes[0] = data
                self.assert_failure(code)

    def test_context_identity_one_run_and_equally_wrong(self):
        for name in pair.CONTEXT_FIELDS:
            for roles in (("seed-5a",), ("seed-5a", "seed-a5")):
                with self.subTest(field=name, roles=roles):
                    changed = copy.deepcopy(self.context)
                    for role in roles:
                        changed["runs"][role]["identity"][name] = "wrong"
                    with mock.patch.object(self, "context", changed):
                        self.assert_failure("context:identity:" + name)
        self.assertEqual(self.qualify()[0], 0)

    def test_absent_identity_and_board(self):
        del self.context["declaration"]["boot_id"]
        self.assert_failure("context:declaration:boot_id")
        self.context = context_fixture(self.images, self.outcomes)
        self.context["declaration"]["platform"] = "1002:5974"
        for record in self.context["runs"].values():
            record["identity"]["platform"] = "1002:5974"
        self.assert_failure("context:platform")

    def test_sealed_artifact_join(self):
        for name in ("depth_before.bin", "depth_after.bin", "zb_depth_discovery_outcome.json"):
            with self.subTest(artifact=name):
                changed = copy.deepcopy(self.context)
                changed["runs"]["seed-5a"]["artifacts"][name] = "0" * 64
                with mock.patch.object(self, "context", changed):
                    self.assert_failure("context:artifact:" + name)

    def test_hasher_failures(self):
        with mock.patch.object(pair.shutil, "which", return_value=None):
            self.assert_failure("hasher_missing", 2)
        for code, stdout, stderr, reason in ((1, b"", b"failed", "hasher_exit"),
                                            (0, b"bad\n", b"", "hasher_output"),
                                            (0, b"1" * 64 + b"\n", b"warning", "hasher_output")):
            process = subprocess.CompletedProcess([], code, stdout, stderr)
            with mock.patch.object(pair.subprocess, "run", return_value=process):
                self.assert_failure(reason, 2)
        with mock.patch.object(pair.subprocess, "run", side_effect=subprocess.TimeoutExpired("b3sum", 30)):
            self.assert_failure("hasher_execution", 2)


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.a, self.b = Path(self.tmp.name) / "a", Path(self.tmp.name) / "b"
        for root, seed in ((self.a, 0x5A), (self.b, 0xA5)):
            root.mkdir()
            for name, data in zip(("depth_before.bin", "depth_after.bin"), image_pair(seed)):
                (root / name).write_bytes(data)

        images = (*image_pair(0x5A), *image_pair(0xA5))
        outcomes = [json.dumps(outcome_fixture(seed)).encode() for seed in (0x5A, 0xA5)]
        for root, data in zip((self.a, self.b), outcomes):
            (root / "zb_depth_discovery_outcome.json").write_bytes(data)
        self.context_path = Path(self.tmp.name) / "context.json"
        self.context_path.write_text(json.dumps(context_fixture(images, outcomes)))

    def command(self, *args):
        return subprocess.run([sys.executable, str(Path(pair.__file__)), *map(str, args)],
                              capture_output=True, text=True, timeout=10, check=False)

    def run_pair(self):
        return self.command("--seed-5a", self.a, "--seed-a5", self.b,
                            "--pair-context", self.context_path)

    def test_success(self):
        result = self.run_pair()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "QUALIFIED")

    def test_judged_failure_exact_status(self):
        (self.a / "depth_after.bin").write_bytes((self.a / "depth_before.bin").read_bytes())
        result = self.run_pair()
        self.assertEqual(result.returncode, 1)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["status"], "REFUSED")
        self.assertEqual(payload["errors"][0], {
            "stage": "raw_observation", "seed_role": "seed-5a",
            "artifact": "depth_after.bin", "offset": None,
            "reason": "one_depth_location", "expected": 1, "observed": 0})
        self.assertEqual(payload["runs"][0]["storage_slots"], 4160)
        self.assertIsNone(payload["runs"][0]["selected_stencil"])

    def test_input_failure_exact_status(self):
        (self.b / "depth_before.bin").unlink()
        result = self.run_pair()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "INPUT_ERROR")

    def test_oversized_input_refuses(self):
        with (self.a / "depth_before.bin").open("ab") as output:
            output.write(b"x")
        result = self.run_pair()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["errors"][0]["reason"], "allocation_length")

    def test_missing_metadata_refuses_with_raw_hashes(self):
        (self.a / "zb_depth_discovery_outcome.json").unlink()
        result = self.run_pair()
        self.assertEqual(result.returncode, 1, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIn("outcome_missing", [error["reason"] for error in payload["errors"]])
        self.assertEqual(payload["runs"][0]["before_sha256"],
                         hashlib.sha256(image_pair(0x5A)[0]).hexdigest())

    def test_input_failure_retains_other_images(self):
        (self.b / "depth_after.bin").unlink()
        result = self.run_pair()
        self.assertEqual(result.returncode, 2, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["inputs"][0]["sha256"],
                         hashlib.sha256(image_pair(0x5A)[0]).hexdigest())
        self.assertIsNone(payload["runs"][1]["after_sha256"])
        self.assertEqual(payload["errors"][0]["seed_role"], "seed-a5")

    def test_oversized_digest_is_explicit_prefix(self):
        with (self.a / "depth_after.bin").open("ab") as target:
            target.write(b"extra data")
        result = self.run_pair()
        self.assertEqual(result.returncode, 1, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["inputs"][1]["digest_scope"], "bounded_prefix")
        self.assertIsNone(payload["runs"][0]["after_sha256"])

    def test_usage_is_not_a_crash(self):
        result = self.command()
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
