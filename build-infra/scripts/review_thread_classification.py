#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a classified action frontier from retained GitHub capture metadata."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

from review_thread_frontier import (
    FRONTIER_FIELDS,
    FrontierError,
    validate_frontier,
)

CAPTURE_FIELDS = (
    "batch_rank",
    "thread_id",
    "pr_number",
    "pr_title",
    "pr_created_at",
    "pr_merged_at",
    "thread_created_at",
    "is_outdated",
    "review_path",
    "line",
    "original_line",
    "review_authors",
    "comment_count",
    "first_comment_url",
    "first_comment_sha256",
)

ASSESSMENT_FIELDS = (
    "thread_id",
    "frontier_id",
    "priority",
    "domain",
    "mechanism",
    "current_evidence",
    "completion_state",
    "required_observation",
    "execution_class",
    "canonical_data_target",
    "completion_gate",
    "falsification_condition",
    "disposition",
    "merged_evidence_commit",
)

COMPLETION_DISPOSITIONS = {
    "pending-evidence": "pending",
    "actionable": "requires-change",
    "fixed-on-main": "fixed",
    "superseded-on-main": "superseded",
    "invalid-on-main": "invalid",
}
CLOSED_DISPOSITIONS = {"fixed", "superseded", "invalid"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("build", "check"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--capture-frontier", type=Path, required=True)
        command_parser.add_argument("--assessments", type=Path, required=True)
        command_parser.add_argument("--output", type=Path, required=True)
        command_parser.add_argument("--batch-id", required=True)
        command_parser.add_argument("--batch-size", type=int, default=50)
    return parser.parse_args()


def read_tsv(path: Path, expected_fields: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        if tuple(reader.fieldnames or ()) != expected_fields:
            raise FrontierError(f"{path}: header differs from the expected schema")
        rows = list(reader)
    for row_number, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise FrontierError(f"{path}:{row_number}: malformed TSV row")
    return rows


def assessment_map(
    assessments: list[dict[str, str]],
) -> dict[str, dict[str, str]]:
    mapped: dict[str, dict[str, str]] = {}
    for row_number, assessment in enumerate(assessments, start=2):
        for field, value in assessment.items():
            if not value.strip():
                raise FrontierError(
                    f"assessment row {row_number}: {field} must be nonempty"
                )
        thread_id = assessment["thread_id"]
        if thread_id in mapped:
            raise FrontierError(f"assessment row {row_number}: duplicate {thread_id}")
        completion_state = assessment["completion_state"]
        expected_disposition = COMPLETION_DISPOSITIONS.get(completion_state)
        if completion_state == "closed":
            if assessment["disposition"] not in CLOSED_DISPOSITIONS:
                raise FrontierError(
                    f"assessment row {row_number}: invalid closed disposition"
                )
        elif expected_disposition is None:
            raise FrontierError(
                f"assessment row {row_number}: invalid completion_state"
            )
        elif assessment["disposition"] != expected_disposition:
            raise FrontierError(
                f"assessment row {row_number}: disposition does not match state"
            )
        if completion_state in ("pending-evidence", "actionable"):
            if assessment["merged_evidence_commit"] != "none":
                raise FrontierError(
                    f"assessment row {row_number}: unmerged state carries evidence"
                )
        elif len(assessment["merged_evidence_commit"]) != 40:
            raise FrontierError(
                f"assessment row {row_number}: merged state lacks a full commit"
            )
        mapped[thread_id] = assessment
    return mapped


def build_rows(
    capture_rows: list[dict[str, str]],
    assessments: list[dict[str, str]],
    batch_id: str,
    batch_size: int,
) -> list[dict[str, str]]:
    if len(capture_rows) != batch_size:
        raise FrontierError(
            f"capture frontier has {len(capture_rows)} rows; expected {batch_size}"
        )
    assessments_by_thread = assessment_map(assessments)
    capture_ids = {row["thread_id"] for row in capture_rows}
    assessment_ids = set(assessments_by_thread)
    if capture_ids != assessment_ids:
        raise FrontierError(
            "capture/assessment membership differs: "
            f"missing={sorted(capture_ids - assessment_ids)}, "
            f"extra={sorted(assessment_ids - capture_ids)}"
        )
    output: list[dict[str, str]] = []
    seen_capture_ids: set[str] = set()
    for capture in capture_rows:
        thread_id = capture["thread_id"]
        if thread_id in seen_capture_ids:
            raise FrontierError(f"capture frontier repeats {thread_id}")
        seen_capture_ids.add(thread_id)
        assessment = assessments_by_thread[thread_id]
        rank = capture["batch_rank"]
        merged_evidence = assessment["merged_evidence_commit"]
        output.append(
            {
                "frontier_id": assessment["frontier_id"],
                "priority": assessment["priority"],
                "domain": assessment["domain"],
                "mechanism": assessment["mechanism"],
                "current_evidence": assessment["current_evidence"],
                "completion_state": assessment["completion_state"],
                "discriminating_question": (
                    f"Does merged main still exhibit {assessment['mechanism']}?"
                ),
                "required_observation": assessment["required_observation"],
                "execution_class": assessment["execution_class"],
                "authority_owner": "mesa-26-gororoba",
                "canonical_data_target": assessment["canonical_data_target"],
                "required_generator": (
                    "review_thread_batch_capture.py plus source-history audit"
                ),
                "probe_declaration": "static source and Git history",
                "bounded_output_schema": (
                    "one classified frontier row and one verified closure row"
                ),
                "maximum_rows_and_bytes": "50 rows; 262144 bytes",
                "retention_class": "canonical-review-ledger",
                "tools": "gh api graphql; rg; git log; sed",
                "ordering_dependencies": (
                    "batch denominator proof"
                    if rank == "1"
                    else f"batch rank {int(rank) - 1} remains stable"
                ),
                "completion_gate": assessment["completion_gate"],
                "falsification_condition": assessment["falsification_condition"],
                "batch_id": batch_id,
                "batch_rank": rank,
                "thread_id": thread_id,
                "pr_number": capture["pr_number"],
                "thread_created_at": capture["thread_created_at"],
                "is_outdated": capture["is_outdated"],
                "review_path": capture["review_path"],
                "original_line": capture["original_line"],
                "review_author": capture["review_authors"],
                "review_url": capture["first_comment_url"],
                "disposition": assessment["disposition"],
                "merged_evidence_commit": (
                    "" if merged_evidence == "none" else merged_evidence
                ),
                "resolution_state": (
                    "resolved"
                    if assessment["completion_state"] == "closed"
                    else "unresolved"
                ),
            }
        )
    validate_frontier(output, batch_size)
    return output


def write_tsv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=FRONTIER_FIELDS,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    arguments = parse_args()
    try:
        if arguments.batch_size <= 0:
            raise FrontierError("--batch-size must be positive")
        capture_rows = read_tsv(arguments.capture_frontier, CAPTURE_FIELDS)
        assessments = read_tsv(arguments.assessments, ASSESSMENT_FIELDS)
        rows = build_rows(
            capture_rows,
            assessments,
            arguments.batch_id,
            arguments.batch_size,
        )
        if arguments.command == "build":
            write_tsv(arguments.output, rows)
        else:
            retained_rows = read_tsv(arguments.output, FRONTIER_FIELDS)
            if retained_rows != rows:
                raise FrontierError("retained action frontier differs from generator")
    except (FrontierError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"OK: {len(rows)} classified review-thread rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
