# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = BUILD_INFRA_ROOT / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SCRIPT_PATH = SCRIPT_DIR / "review_thread_classification.py"
CAPTURE_PATH = (
    BUILD_INFRA_ROOT / "docs/review-thread-frontiers/merged-pr93-pr161/frontier.tsv"
)
ASSESSMENT_PATH = (
    BUILD_INFRA_ROOT
    / "docs/review-thread-classifications/merged-pr93-pr161/assessments.tsv"
)
OUTPUT_PATH = BUILD_INFRA_ROOT / "docs/merged-review-thread-action-frontier.tsv"
THIRD_CAPTURE_PATH = (
    BUILD_INFRA_ROOT
    / "docs/review-thread-frontiers/merged-thread-frontier-92b67f719e7b/frontier.tsv"
)
THIRD_ASSESSMENT_PATH = (
    BUILD_INFRA_ROOT
    / "docs/review-thread-classifications/merged-thread-frontier-92b67f719e7b/assessments.tsv"
)
THIRD_OUTPUT_PATH = (
    BUILD_INFRA_ROOT
    / "docs/review-thread-classifications/merged-thread-frontier-92b67f719e7b/action-frontier.tsv"
)
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_classification", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_classification = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_classification)


def retained_inputs() -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    capture_rows = review_thread_classification.read_tsv(
        CAPTURE_PATH, review_thread_classification.CAPTURE_FIELDS
    )
    assessment_rows = review_thread_classification.read_tsv(
        ASSESSMENT_PATH, review_thread_classification.ASSESSMENT_FIELDS
    )
    return capture_rows, assessment_rows


def build_rows(
    capture_rows: list[dict[str, str]], assessment_rows: list[dict[str, str]]
) -> list[dict[str, str]]:
    return review_thread_classification.build_rows(
        capture_rows,
        assessment_rows,
        "merged-review-thread-closure-batch-0002",
        50,
    )


def third_retained_inputs() -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    capture_rows = review_thread_classification.read_tsv(
        THIRD_CAPTURE_PATH, review_thread_classification.CAPTURE_FIELDS
    )
    assessment_rows = review_thread_classification.read_tsv(
        THIRD_ASSESSMENT_PATH, review_thread_classification.ASSESSMENT_FIELDS
    )
    return capture_rows, assessment_rows


def build_third_rows(
    capture_rows: list[dict[str, str]], assessment_rows: list[dict[str, str]]
) -> list[dict[str, str]]:
    return review_thread_classification.build_rows(
        capture_rows,
        assessment_rows,
        "merged-review-thread-closure-batch-0003",
        50,
    )


def test_retained_classification_matches_generated_frontier() -> None:
    capture_rows, assessment_rows = retained_inputs()
    expected = build_rows(capture_rows, assessment_rows)
    retained = review_thread_classification.read_tsv(
        OUTPUT_PATH, review_thread_classification.FRONTIER_FIELDS
    )
    assert retained == expected


def test_third_classification_matches_generated_frontier() -> None:
    capture_rows, assessment_rows = third_retained_inputs()
    expected = build_third_rows(capture_rows, assessment_rows)
    retained = review_thread_classification.read_tsv(
        THIRD_OUTPUT_PATH, review_thread_classification.FRONTIER_FIELDS
    )
    assert retained == expected


def test_third_classification_rejects_missing_assessment() -> None:
    capture_rows, assessment_rows = third_retained_inputs()
    with pytest.raises(
        review_thread_classification.FrontierError, match="membership differs"
    ):
        build_third_rows(capture_rows, assessment_rows[:-1])


def test_classification_rejects_missing_assessment() -> None:
    capture_rows, assessment_rows = retained_inputs()
    with pytest.raises(
        review_thread_classification.FrontierError, match="membership differs"
    ):
        build_rows(capture_rows, assessment_rows[:-1])


def test_classification_rejects_extra_assessment() -> None:
    capture_rows, assessment_rows = retained_inputs()
    extra = dict(assessment_rows[-1])
    extra["thread_id"] = "PRRT_extra_calibration"
    with pytest.raises(
        review_thread_classification.FrontierError, match="membership differs"
    ):
        build_rows(capture_rows, assessment_rows + [extra])


def test_classification_rejects_duplicate_assessment() -> None:
    capture_rows, assessment_rows = retained_inputs()
    with pytest.raises(review_thread_classification.FrontierError, match="duplicate"):
        build_rows(capture_rows, assessment_rows + [dict(assessment_rows[0])])


def test_classification_rejects_state_disposition_mismatch() -> None:
    capture_rows, assessment_rows = retained_inputs()
    mutated = [dict(row) for row in assessment_rows]
    mutated[0]["disposition"] = "requires-change"
    with pytest.raises(
        review_thread_classification.FrontierError,
        match="invalid closed disposition",
    ):
        build_rows(capture_rows, mutated)


def test_classification_rejects_short_merged_evidence_commit() -> None:
    capture_rows, assessment_rows = retained_inputs()
    mutated = [dict(row) for row in assessment_rows]
    mutated[0]["merged_evidence_commit"] = "0fab98b4"
    with pytest.raises(
        review_thread_classification.FrontierError,
        match="lacks a full commit",
    ):
        build_rows(capture_rows, mutated)
