#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify that each RS485M kernel authority axis has an immutable identity."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

TEST_ROOT = Path(__file__).resolve().parent
AUTHORITY_DOCUMENT = TEST_ROOT.parent / "rs482-source-authority.md"
CURRENT_IDENTITIES_MARKER = "The current identities stay on separate axes:"
TABLE_END_MARKER = "<!-- markdownlint-enable MD013 -->"
GIT_OBJECT = r"`[0-9a-f]{40}`"
SHA256 = r"`[0-9a-f]{64}`"

EXPECTED_AXES = (
    "Modified source",
    "Active package recipe",
    "Target deployment runtime",
    "Loaded module byte identity",
    "Parked-device behavior",
)


def load_current_identity_rows() -> tuple[str, dict[str, tuple[str, str]]]:
    document = AUTHORITY_DOCUMENT.read_text(encoding="utf-8")
    _, marker, remaining_document = document.partition(CURRENT_IDENTITIES_MARKER)
    if not marker:
        raise ValueError("current identity table marker is missing")

    table, end_marker, _ = remaining_document.partition(TABLE_END_MARKER)
    if not end_marker:
        raise ValueError("current identity table end marker is missing")

    rows: dict[str, tuple[str, str]] = {}
    for line in table.splitlines():
        if not line.startswith("|"):
            continue
        cells = tuple(cell.strip() for cell in line.strip("|").split("|"))
        if len(cells) != 3 or cells[0] in {"Identity axis", "---"}:
            continue
        if cells[0] in rows:
            raise ValueError(f"duplicate identity axis: {cells[0]}")
        rows[cells[0]] = (cells[1], cells[2])
    return document, rows


class RS482SourceAuthorityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document, cls.rows = load_current_identity_rows()

    def assert_labeled_identity(
        self, evidence: str, label: str, identity_pattern: str
    ) -> None:
        self.assertRegex(evidence, rf"{re.escape(label)} {identity_pattern}")

    def test_identity_axes_are_exact_and_unique(self) -> None:
        self.assertEqual(set(EXPECTED_AXES), set(self.rows))
        self.assertEqual(len(EXPECTED_AXES), len(self.rows))

    def test_modified_source_has_no_implicit_deployment_claim(self) -> None:
        authority, evidence = self.rows["Modified source"]
        self.assertEqual("`linux-radeon-gororoba`", authority)
        self.assert_labeled_identity(evidence, "current main commit", GIT_OBJECT)
        self.assert_labeled_identity(evidence, "driver tree", GIT_OBJECT)
        self.assertIn("carries no package or runtime claim", evidence)

    def test_active_recipe_pins_package_and_source_objects(self) -> None:
        authority, evidence = self.rows["Active package recipe"]
        self.assertRegex(authority, r"^`radeon-custom` [0-9]+\.[0-9]+\.[0-9]+-[0-9]+$")
        for label in (
            "package commit",
            "recipe tree",
            "`PKGBUILD` blob",
            "source identity blob",
            "signed source tag object",
            "source commit",
            "driver tree",
        ):
            self.assert_labeled_identity(evidence, label, GIT_OBJECT)

    def test_target_runtime_pins_bundle_and_loaded_source(self) -> None:
        authority, evidence = self.rows["Target deployment runtime"]
        self.assertRegex(
            authority,
            r"^`steinmarder-r300/results/.+-deployment-runtime/`$",
        )
        for label in ("retaining commit", "source commit", "driver tree"):
            self.assert_labeled_identity(evidence, label, GIT_OBJECT)
        for label in ("manifest SHA-256", "hash ledger SHA-256"):
            self.assert_labeled_identity(evidence, label, SHA256)
        self.assertRegex(evidence, r"[0-9]+\.[0-9]+\.[0-9]+-[0-9]+")
        self.assertRegex(evidence, r"srcversion `[0-9A-F]+`")

    def test_loaded_module_bytes_remain_a_separate_axis(self) -> None:
        authority, evidence = self.rows["Loaded module byte identity"]
        self.assertRegex(authority, r"^`steinmarder-r300/.+-production-identity/`$")
        self.assert_labeled_identity(evidence, "retaining commit", GIT_OBJECT)
        self.assert_labeled_identity(evidence, "manifest SHA-256", SHA256)
        self.assert_labeled_identity(evidence, "hash ledger SHA-256", SHA256)
        self.assert_labeled_identity(evidence, "compressed module SHA-256", SHA256)
        self.assert_labeled_identity(evidence, "GNU Build ID", GIT_OBJECT)
        self.assertIn("no newer retained bundle records", evidence)

    def test_parked_behavior_pins_the_measured_bundle(self) -> None:
        authority, evidence = self.rows["Parked-device behavior"]
        self.assertRegex(authority, r"^`steinmarder-r300/.+parked_entry_contract.+/`$")
        self.assert_labeled_identity(evidence, "retaining commit", GIT_OBJECT)
        self.assert_labeled_identity(evidence, "outcome SHA-256", SHA256)
        self.assert_labeled_identity(evidence, "hash ledger SHA-256", SHA256)
        self.assertRegex(evidence, r"measures the [0-9]+\.[0-9]+-[0-9]+ parked-entry")
        self.assertNotIn("latest attended park verdict", self.document)


if __name__ == "__main__":
    unittest.main()
