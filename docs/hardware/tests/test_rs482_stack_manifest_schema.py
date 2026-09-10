#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Calibrate accepted and rejected RS485M stack manifest claims."""

from __future__ import annotations

import copy
import json
import unittest
from itertools import product
from pathlib import Path

from jsonschema import Draft202012Validator

TEST_ROOT = Path(__file__).resolve().parent
FIXTURE_ROOT = TEST_ROOT / "rs482-stack-manifest-schema"
SCHEMA_PATH = TEST_ROOT.parent / "rs482-stack-manifest.schema.json"
MUTATION_FIXTURE_NAME = "invalid-v2-identity-mutations.json"

VALID_FIXTURES = (
    "valid-legacy.json",
    "valid-source-to-payload-v2.json",
    "valid-post-cutover-radeon-ddx-v2.json",
    "valid-rs485m-radeon-ddx-v3.json",
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


def replace_path_value(
    instance: dict[str, object], path: tuple[str, ...], replacement: object
) -> None:
    if not path:
        raise ValueError("mutation path must contain at least one component")

    parent = instance
    for component in path[:-1]:
        child = parent.get(component)
        if not isinstance(child, dict):
            raise TypeError(f"mutation path component {component!r} is not an object")
        parent = child
    parent[path[-1]] = replacement


def load_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise TypeError(f"{path} must contain one JSON object")
    return value


class RS482StackManifestSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = load_json(SCHEMA_PATH)
        Draft202012Validator.check_schema(cls.schema)
        cls.validator = Draft202012Validator(cls.schema)

    def test_accepts_legacy_and_versioned_manifests(self) -> None:
        for fixture_name in VALID_FIXTURES:
            with self.subTest(fixture=fixture_name):
                instance = load_json(FIXTURE_ROOT / fixture_name)
                errors = sorted(
                    self.validator.iter_errors(instance),
                    key=lambda error: list(error.absolute_path),
                )
                self.assertEqual([], errors)

    def test_accepts_modesetting_for_each_contract(self) -> None:
        for contract in ("source-to-payload-v2", "source-to-payload-v3"):
            with self.subTest(contract=contract):
                instance = load_json(FIXTURE_ROOT / "valid-source-to-payload-v2.json")
                instance["provenance_contract"] = contract
                self.assertEqual([], list(self.validator.iter_errors(instance)))

    def test_rejects_each_calibrated_contract_mutation(self) -> None:
        for (fixture_name, expectation), contract in product(
            INVALID_FIXTURES.items(), ("source-to-payload-v2", "source-to-payload-v3")
        ):
            with self.subTest(fixture=fixture_name, contract=contract):
                instance = load_json(FIXTURE_ROOT / fixture_name)
                instance["provenance_contract"] = contract
                errors = list(self.validator.iter_errors(instance))
                self.assertEqual(
                    1,
                    len(errors),
                    [error.message for error in errors],
                )
                error = errors[0]
                self.assertEqual(expectation["path"], tuple(error.absolute_path))
                self.assertEqual(expectation["validator"], error.validator)
                if "message_fragment" in expectation:
                    self.assertIn(expectation["message_fragment"], error.message)

    def test_rejects_identity_value_mutations(self) -> None:
        mutation_fixture = load_json(FIXTURE_ROOT / MUTATION_FIXTURE_NAME)
        base_fixture_name = mutation_fixture.get("base_fixture")
        mutations = mutation_fixture.get("mutations")
        self.assertIsInstance(base_fixture_name, str)
        self.assertIsInstance(mutations, list)
        base_instance = load_json(FIXTURE_ROOT / str(base_fixture_name))

        for mutation, contract in product(
            mutations, ("source-to-payload-v2", "source-to-payload-v3")
        ):
            self.assertIsInstance(mutation, dict)
            mutation_name = mutation.get("name")
            mutation_path = mutation.get("path")
            expected_validator = mutation.get("validator")
            self.assertIsInstance(mutation_name, str)
            self.assertIsInstance(mutation_path, list)
            self.assertTrue(
                all(isinstance(component, str) for component in mutation_path)
            )
            self.assertIsInstance(expected_validator, str)

            with self.subTest(mutation=mutation_name, contract=contract):
                candidate = copy.deepcopy(base_instance)
                candidate["provenance_contract"] = contract
                path = tuple(mutation_path)
                replace_path_value(candidate, path, mutation.get("value"))
                errors = list(self.validator.iter_errors(candidate))
                error_paths = {tuple(error.absolute_path) for error in errors}
                validators = {error.validator for error in errors}
                self.assertTrue(errors)
                self.assertEqual({path}, error_paths)
                self.assertIn(expected_validator, validators)

    def test_repository_pair_matches_manifest_contract(self) -> None:
        base_instance = load_json(
            FIXTURE_ROOT / "valid-post-cutover-radeon-ddx-v2.json"
        )
        contracts = {
            None: "rs482",
            "source-to-payload-v2": "rs482",
            "source-to-payload-v3": "rs485m",
        }
        for (contract, target), source_target, release_target in product(
            contracts.items(),
            ("rs482", "rs485m", "unknown"),
            ("rs482", "rs485m", "unknown"),
        ):
            with self.subTest(
                contract=contract, source=source_target, release=release_target
            ):
                candidate = copy.deepcopy(base_instance)
                if contract is None:
                    del candidate["provenance_contract"]
                else:
                    candidate["provenance_contract"] = contract
                provenance = candidate["xorg"]["radeon_ddx_provenance"]
                provenance["source_repository"] = f"xf86-video-ati-{source_target}"
                provenance["release_repository"] = (
                    f"PKGBUILD_xf86-video-ati-{release_target}"
                )
                expected_paths = {
                    ("xorg", "radeon_ddx_provenance", field)
                    for field, actual_target in (
                        ("source_repository", source_target),
                        ("release_repository", release_target),
                    )
                    if actual_target != target
                }
                errors = list(self.validator.iter_errors(candidate))
                self.assertEqual(len(expected_paths), len(errors))
                self.assertEqual(
                    expected_paths, {tuple(error.absolute_path) for error in errors}
                )
                for error in errors:
                    self.assertEqual("const", error.validator)

    def test_v3_preserves_required_provenance_fields(self) -> None:
        base_instance = load_json(FIXTURE_ROOT / "valid-rs485m-radeon-ddx-v3.json")
        required_paths = [
            ("xorg", "ddx_package"),
            ("xorg", "radeon_ddx_provenance"),
            ("xorg", "server_provenance"),
            ("mesa", "provenance"),
            ("kernel", "provenance"),
        ]
        required_paths.extend(
            ("xorg", "radeon_ddx_provenance", field)
            for field in (
                "source_repository",
                "source_commit",
                "source_tree",
                "release_repository",
                "release_commit",
                "release_input_sha256",
                "package_artifact_sha256",
                "installed_payload_manifest_sha256",
            )
        )
        for path in required_paths:
            with self.subTest(path=path):
                candidate = copy.deepcopy(base_instance)
                parent = candidate
                for component in path[:-1]:
                    parent = parent[component]
                del parent[path[-1]]
                errors = list(self.validator.iter_errors(candidate))
                self.assertEqual(1, len(errors), [error.message for error in errors])
                self.assertEqual(path[:-1], tuple(errors[0].absolute_path))
                self.assertEqual("required", errors[0].validator)
                self.assertIn(f"'{path[-1]}'", errors[0].message)

    def test_contract_version_requires_an_exact_supported_value(self) -> None:
        base_instance = load_json(FIXTURE_ROOT / "valid-source-to-payload-v2.json")
        for contract in ("", "source-to-payload-v4", "source-to-payload-v3\n", None, 3):
            with self.subTest(contract=contract):
                candidate = copy.deepcopy(base_instance)
                candidate["provenance_contract"] = contract
                errors = list(self.validator.iter_errors(candidate))
                self.assertTrue(errors)
                self.assertEqual(
                    {("provenance_contract",)},
                    {tuple(error.absolute_path) for error in errors},
                )
                self.assertIn("enum", {error.validator for error in errors})

    def test_fixture_inventory_is_exact(self) -> None:
        expected = set(VALID_FIXTURES) | set(INVALID_FIXTURES) | {MUTATION_FIXTURE_NAME}
        observed = {path.name for path in FIXTURE_ROOT.glob("*.json")}
        self.assertEqual(expected, observed)


if __name__ == "__main__":
    unittest.main()
