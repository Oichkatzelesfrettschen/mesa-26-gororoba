# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import importlib.util
import sys
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = BUILD_INFRA_ROOT / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SCRIPT_PATH = SCRIPT_DIR / "review_thread_resolution.py"
CLASSIFICATION_DIR = (
    BUILD_INFRA_ROOT
    / "docs/review-thread-classifications/merged-thread-frontier-after-QY6A8"
)
FRONTIER_PATH = CLASSIFICATION_DIR / "pre-resolution-frontier.tsv"
JOURNAL_PATH = CLASSIFICATION_DIR / "resolution-observation.json"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_resolution", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_resolution = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_resolution)


def retained_state() -> tuple[list[dict[str, str]], dict[str, object]]:
    rows = review_thread_resolution.read_tsv(
        FRONTIER_PATH, review_thread_resolution.FRONTIER_FIELDS
    )
    journal = review_thread_resolution.load_journal(JOURNAL_PATH)
    return rows, journal


def test_retained_resolution_journal_is_complete() -> None:
    rows, journal = retained_state()
    review_thread_resolution.validate_resolution_frontier(rows)
    review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)
    assert journal["complete"] is True
    assert len(journal["entries"]) == 50


def test_resolution_rejects_non_fifty_frontier() -> None:
    rows, _ = retained_state()
    with pytest.raises(
        review_thread_resolution.FrontierError, match="expected exactly 50"
    ):
        review_thread_resolution.validate_resolution_frontier(rows[:-1])


def test_resolution_rejects_malformed_thread_id() -> None:
    rows, _ = retained_state()
    mutated = copy.deepcopy(rows)
    mutated[0]["thread_id"] = 'PRRT_bad") { viewer { login } }'
    with pytest.raises(
        review_thread_resolution.FrontierError, match="invalid thread_id"
    ):
        review_thread_resolution.validate_resolution_frontier(mutated)


def test_resolution_rejects_reordered_entries() -> None:
    rows, journal = retained_state()
    mutated = copy.deepcopy(journal)
    mutated["entries"][0], mutated["entries"][1] = (
        mutated["entries"][1],
        mutated["entries"][0],
    )
    with pytest.raises(
        review_thread_resolution.FrontierError, match="ordered frontier prefix"
    ):
        review_thread_resolution.validate_journal(rows, mutated, FRONTIER_PATH)


def test_resolution_rejects_frontier_hash_drift() -> None:
    rows, journal = retained_state()
    mutated = copy.deepcopy(journal)
    mutated["frontier_sha256"] = "0" * 64
    with pytest.raises(
        review_thread_resolution.FrontierError, match="frontier hash differs"
    ):
        review_thread_resolution.validate_journal(rows, mutated, FRONTIER_PATH)


def test_resolution_rejects_incomplete_complete_journal() -> None:
    rows, journal = retained_state()
    mutated = copy.deepcopy(journal)
    mutated["entries"] = mutated["entries"][:-1]
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="complete journal lacks full postflight evidence",
    ):
        review_thread_resolution.validate_journal(rows, mutated, FRONTIER_PATH)
