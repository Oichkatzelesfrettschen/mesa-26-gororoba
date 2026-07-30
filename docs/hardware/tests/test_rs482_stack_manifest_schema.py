#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Calibrate the RS482 stack manifest schema on accepted and rejected claims."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator


TEST_ROOT = Path(__file__).resolve().parent
FIXTURE_ROOT = TEST_ROOT / "rs482-stack-manifest-schema"
SCHEMA_PATH = TEST_ROOT.parent / "rs482-stack-manifest.schema.json"

VALID_FIXTURES = (
    "valid-legacy.json",
    "valid-source-to-payload-v2.json",
    "valid-post-cutover-radeon-ddx-v2.json",
)

INVALID_FIXTURES = {
    "invalid-v2-kernel-source-pin.json": {
        "path": ("kernel", "provenance", "source_pin_sha256"),
        "validator": "pattern",
    },
    "invalid-v2-kernel-module-build-id.json": {
        "path": ("kernel", "provenance"),
        "validator": "required",
        "message_fragment": "'module_build_id' is a required property",
    },
    "invalid-v2-radeon-ddx-provenance.json": {
        "path": ("xorg",),
        "validator": "required",
        "message_fragment": "'radeon_ddx_provenance' is a required property",
    },
    "invalid-v2-post-cutover-equivalence.json": {
        "path": ("kernel", "provenance"),
        "validator": "required",
        "message_fragment": "'equivalence' is a required property",
    },
}


def load_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="ascii"))
    if not isinstance(value, dict):
        raise TypeError(f"{path} must contain one JSON object")
    return value


class RS482StackManifestSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = load_json(SCHEMA_PATH)
        Draft202012Validator.check_schema(cls.schema)
        cls.validator = Draft202012Validator(cls.schema)

    def test_accepts_legacy_and_complete_v2_manifests(self) -> None:
        for fixture_name in VALID_FIXTURES:
            with self.subTest(fixture=fixture_name):
                instance = load_json(FIXTURE_ROOT / fixture_name)
                errors = sorted(
                    self.validator.iter_errors(instance),
                    key=lambda error: list(error.absolute_path),
                )
                self.assertEqual([], errors)

    def test_rejects_each_calibrated_contract_mutation(self) -> None:
        for fixture_name, expectation in INVALID_FIXTURES.items():
            with self.subTest(fixture=fixture_name):
                instance = load_json(FIXTURE_ROOT / fixture_name)
                errors = list(self.validator.iter_errors(instance))
                self.assertEqual(1, len(errors), [error.message for error in errors])
                error = errors[0]
                self.assertEqual(expectation["path"], tuple(error.absolute_path))
                self.assertEqual(expectation["validator"], error.validator)
                if "message_fragment" in expectation:
                    self.assertIn(expectation["message_fragment"], error.message)

    def test_fixture_inventory_is_exact(self) -> None:
        expected = set(VALID_FIXTURES) | set(INVALID_FIXTURES)
        observed = {path.name for path in FIXTURE_ROOT.glob("*.json")}
        self.assertEqual(expected, observed)


if __name__ == "__main__":
    unittest.main()
