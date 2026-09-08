# SPDX-License-Identifier: MIT
"""Exercise paired-stencil classification with independent byte fixtures."""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

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
        for run in result["runs"]:
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
        self.assertNotEqual(swap(2), 2)
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


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.a, self.b = Path(self.tmp.name) / "a", Path(self.tmp.name) / "b"
        for root, seed in ((self.a, 0x5A), (self.b, 0xA5)):
            root.mkdir()
            for name, data in zip(("depth_before.bin", "depth_after.bin"), image_pair(seed)):
                (root / name).write_bytes(data)

    def command(self, *args):
        return subprocess.run([sys.executable, str(Path(pair.__file__)), *map(str, args)],
                              capture_output=True, text=True, timeout=10, check=False)

    def run_pair(self):
        return self.command("--seed-5a", self.a, "--seed-a5", self.b)

    def test_success(self):
        result = self.run_pair()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "OBSERVED")

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

    def test_usage_is_not_a_crash(self):
        result = self.command()
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
