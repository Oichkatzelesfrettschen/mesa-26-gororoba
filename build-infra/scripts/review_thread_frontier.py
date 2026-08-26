#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate the finite merged-PR review-thread closure frontier."""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import NamedTuple

FRONTIER_FIELDS = (
    "frontier_id",
    "priority",
    "domain",
    "mechanism",
    "current_evidence",
    "completion_state",
    "discriminating_question",
    "required_observation",
    "execution_class",
    "authority_owner",
    "canonical_data_target",
    "required_generator",
    "probe_declaration",
    "bounded_output_schema",
    "maximum_rows_and_bytes",
    "retention_class",
    "tools",
    "ordering_dependencies",
    "completion_gate",
    "falsification_condition",
    "batch_id",
    "batch_rank",
    "thread_id",
    "pr_number",
    "thread_created_at",
    "is_outdated",
    "review_path",
    "original_line",
    "review_author",
    "review_url",
    "disposition",
    "merged_evidence_commit",
    "resolution_state",
)

LEDGER_FIELDS = (
    "thread_id",
    "disposition",
    "evidence_commit",
    "evidence_pr",
    "merged_at",
    "github_resolved_at",
    "post_resolution_verified_at",
    "closure_note",
)

COMPLETION_DISPOSITION = {
    "pending-evidence": "pending",
    "actionable": "requires-change",
    "fixed-on-main": "fixed",
    "superseded-on-main": "superseded",
    "invalid-on-main": "invalid",
}
CLOSED_DISPOSITIONS = frozenset(("fixed", "superseded", "invalid"))
THREAD_ID_PATTERN = re.compile(r"^PRRT_[A-Za-z0-9_-]+$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
CANONICAL_TARGET_PATTERN = re.compile(
    r"^(?P<path>[A-Za-z0-9_./+-]+)"
    r"(?:#L(?P<first>[1-9][0-9]*)-L(?P<last>[1-9][0-9]*))?$"
)
REVIEW_URL_PATTERN = re.compile(
    r"^https://github[.]com/Oichkatzelesfrettschen/mesa-26-gororoba/"
    r"pull/([1-9][0-9]*)#discussion_r[1-9][0-9]*$"
)


class FrontierError(ValueError):
    """A frontier or ledger invariant failed."""


class EvidenceTarget(NamedTuple):
    """A repository path and optional inclusive source-line slice."""

    declaration: str
    path: str
    first_line: int | None
    last_line: int | None


def read_tsv(path: Path, expected_fields: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        actual_fields = tuple(reader.fieldnames or ())
        if actual_fields != expected_fields:
            raise FrontierError(
                f"{path}: header mismatch: expected {expected_fields!r}, "
                f"found {actual_fields!r}"
            )
        rows = list(reader)
    for row_number, row in enumerate(rows, start=2):
        if None in row:
            raise FrontierError(f"{path}:{row_number}: unexpected extra columns")
        missing_fields = sorted(field for field, value in row.items() if value is None)
        if missing_fields:
            raise FrontierError(
                f"{path}:{row_number}: missing columns {', '.join(missing_fields)}"
            )
    return rows


def parse_timestamp(value: str, context: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise FrontierError(
            f"{context}: invalid ISO 8601 timestamp {value!r}"
        ) from error
    if parsed.tzinfo is None:
        raise FrontierError(f"{context}: timestamp lacks a timezone")
    return parsed


def positive_decimal(value: str, field: str, row_number: int) -> int:
    if not value.isdecimal() or int(value) <= 0:
        raise FrontierError(
            f"frontier row {row_number}: {field} must be a positive decimal"
        )
    return int(value)


def canonical_targets(value: str, row_number: int) -> tuple[EvidenceTarget, ...]:
    """Return normalized repository-relative evidence-owner targets."""
    targets = []
    for declaration in value.split(";"):
        target_match = CANONICAL_TARGET_PATTERN.fullmatch(declaration)
        path = target_match.group("path") if target_match is not None else ""
        path_parts = path.split("/")
        if (
            target_match is None
            or path.startswith("/")
            or any(part in ("", ".", "..") for part in path_parts)
        ):
            raise FrontierError(
                f"frontier row {row_number}: invalid canonical_data_target "
                f"{declaration!r}"
            )
        first_text = target_match.group("first")
        last_text = target_match.group("last")
        first_line = int(first_text) if first_text is not None else None
        last_line = int(last_text) if last_text is not None else None
        if first_line is not None and last_line is not None and first_line > last_line:
            raise FrontierError(
                f"frontier row {row_number}: reversed canonical_data_target range "
                f"{declaration!r}"
            )
        targets.append(EvidenceTarget(declaration, path, first_line, last_line))
    if len(set(targets)) != len(targets):
        raise FrontierError(
            f"frontier row {row_number}: duplicate canonical_data_target"
        )
    return tuple(targets)


def validate_frontier(
    rows: list[dict[str, str]], expected_batch_size: int
) -> dict[str, dict[str, str]]:
    if len(rows) != expected_batch_size:
        raise FrontierError(
            f"frontier has {len(rows)} rows; expected exactly {expected_batch_size}"
        )

    nonempty_fields = FRONTIER_FIELDS[:21] + FRONTIER_FIELDS[26:30]
    frontier_ids: set[str] = set()
    thread_ids: set[str] = set()
    batch_ids: set[str] = set()
    ranks: list[int] = []
    chronological_keys: list[tuple[datetime, str]] = []
    rows_by_thread: dict[str, dict[str, str]] = {}

    for row_number, row in enumerate(rows, start=2):
        for field in nonempty_fields:
            if not row[field].strip():
                raise FrontierError(f"frontier row {row_number}: {field} is empty")
        canonical_targets(row["canonical_data_target"], row_number)

        frontier_id = row["frontier_id"]
        if frontier_id in frontier_ids:
            raise FrontierError(
                f"frontier row {row_number}: duplicate frontier_id {frontier_id}"
            )
        frontier_ids.add(frontier_id)

        thread_id = row["thread_id"]
        if not THREAD_ID_PATTERN.fullmatch(thread_id):
            raise FrontierError(
                f"frontier row {row_number}: invalid thread_id {thread_id!r}"
            )
        if thread_id in thread_ids:
            raise FrontierError(
                f"frontier row {row_number}: duplicate thread_id {thread_id}"
            )
        thread_ids.add(thread_id)
        rows_by_thread[thread_id] = row

        ranks.append(positive_decimal(row["batch_rank"], "batch_rank", row_number))
        pr_number = positive_decimal(row["pr_number"], "pr_number", row_number)
        if not row["original_line"].isdecimal():
            raise FrontierError(
                f"frontier row {row_number}: original_line must be nonnegative"
            )
        if row["is_outdated"] not in ("false", "true"):
            raise FrontierError(
                f"frontier row {row_number}: is_outdated must be false or true"
            )
        review_url_match = REVIEW_URL_PATTERN.fullmatch(row["review_url"])
        if review_url_match is None or int(review_url_match.group(1)) != pr_number:
            raise FrontierError(
                f"frontier row {row_number}: review_url does not identify its PR"
            )

        completion_state = row["completion_state"]
        disposition = row["disposition"]
        resolution_state = row["resolution_state"]
        evidence_commit = row["merged_evidence_commit"]
        expected_disposition = COMPLETION_DISPOSITION.get(completion_state)
        if completion_state == "closed":
            if disposition not in CLOSED_DISPOSITIONS:
                raise FrontierError(
                    f"frontier row {row_number}: closed disposition is invalid"
                )
            if resolution_state != "resolved":
                raise FrontierError(
                    f"frontier row {row_number}: closed requires resolved state"
                )
        elif expected_disposition is None:
            raise FrontierError(
                f"frontier row {row_number}: invalid completion_state "
                f"{completion_state!r}"
            )
        else:
            if disposition != expected_disposition:
                raise FrontierError(
                    f"frontier row {row_number}: {completion_state} requires "
                    f"disposition={expected_disposition}"
                )
            if resolution_state != "unresolved":
                raise FrontierError(
                    f"frontier row {row_number}: only closed rows may be resolved"
                )

        evidence_states = frozenset(
            ("fixed-on-main", "superseded-on-main", "invalid-on-main", "closed")
        )
        if completion_state in evidence_states:
            if not COMMIT_PATTERN.fullmatch(evidence_commit):
                raise FrontierError(
                    f"frontier row {row_number}: {completion_state} requires a "
                    "40-hex merged evidence commit"
                )
        elif evidence_commit:
            raise FrontierError(
                f"frontier row {row_number}: unmerged state carries merged evidence"
            )

        timestamp = parse_timestamp(
            row["thread_created_at"], f"frontier row {row_number}"
        )
        chronological_keys.append((timestamp, thread_id))
        batch_ids.add(row["batch_id"])

    if len(batch_ids) != 1:
        raise FrontierError(f"frontier spans multiple batches: {sorted(batch_ids)}")
    expected_ranks = list(range(1, expected_batch_size + 1))
    if ranks != expected_ranks:
        raise FrontierError("batch_rank must be contiguous and ordered")
    if chronological_keys != sorted(chronological_keys):
        raise FrontierError("frontier is not ordered by thread creation time and ID")
    return rows_by_thread


def validate_ledger(
    rows: list[dict[str, str]], frontier_by_thread: dict[str, dict[str, str]]
) -> None:
    ledger_threads: set[str] = set()
    for row_number, row in enumerate(rows, start=2):
        thread_id = row["thread_id"]
        if thread_id in ledger_threads:
            raise FrontierError(f"ledger row {row_number}: duplicate {thread_id}")
        if thread_id not in frontier_by_thread:
            raise FrontierError(f"ledger row {row_number}: thread is outside frontier")
        frontier_row = frontier_by_thread[thread_id]
        if frontier_row["completion_state"] != "closed":
            raise FrontierError(f"ledger row {row_number}: frontier row is not closed")
        if row["disposition"] != frontier_row["disposition"]:
            raise FrontierError(f"ledger row {row_number}: disposition mismatch")
        if row["evidence_commit"] != frontier_row["merged_evidence_commit"]:
            raise FrontierError(f"ledger row {row_number}: evidence commit mismatch")
        if not COMMIT_PATTERN.fullmatch(row["evidence_commit"]):
            raise FrontierError(f"ledger row {row_number}: invalid evidence commit")
        if not row["evidence_pr"].isdecimal() or int(row["evidence_pr"]) <= 0:
            raise FrontierError(f"ledger row {row_number}: invalid evidence PR")
        if not row["closure_note"].strip():
            raise FrontierError(f"ledger row {row_number}: empty closure note")
        merged_at = parse_timestamp(row["merged_at"], f"ledger row {row_number}")
        resolved_at = parse_timestamp(
            row["github_resolved_at"], f"ledger row {row_number}"
        )
        verified_at = parse_timestamp(
            row["post_resolution_verified_at"], f"ledger row {row_number}"
        )
        if not merged_at <= resolved_at <= verified_at:
            raise FrontierError(
                f"ledger row {row_number}: expected merged <= resolved <= verified"
            )
        ledger_threads.add(thread_id)

    closed_threads = {
        thread_id
        for thread_id, row in frontier_by_thread.items()
        if row["completion_state"] == "closed"
    }
    if ledger_threads != closed_threads:
        raise FrontierError(
            "ledger/frontier closure mismatch: "
            f"missing={sorted(closed_threads - ledger_threads)}, "
            f"extra={sorted(ledger_threads - closed_threads)}"
        )


def run_git(
    repository_root: Path,
    *arguments: str,
    strip: bool = True,
) -> str:
    """Run a read-only Git query and return its standard output."""
    result = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        raise FrontierError(f"git {' '.join(arguments)} failed: {diagnostic}")
    return result.stdout.strip() if strip else result.stdout


def evidence_target_identity(
    repository_root: Path,
    revision: str,
    target: EvidenceTarget,
) -> str:
    """Return a blob ID or exact bounded source slice for one target."""
    if target.first_line is None or target.last_line is None:
        return run_git(
            repository_root,
            "rev-parse",
            "--verify",
            f"{revision}:{target.path}",
        )
    content = run_git(
        repository_root,
        "show",
        f"{revision}:{target.path}",
        strip=False,
    )
    lines = content.splitlines()
    if target.last_line > len(lines):
        raise FrontierError(
            f"{target.declaration}: source slice exceeds {revision} line count "
            f"{len(lines)}"
        )
    return "\n".join(lines[target.first_line - 1 : target.last_line])


def verify_merged_evidence(
    frontier_rows: list[dict[str, str]],
    repository_root: Path,
    main_ref: str,
    candidate_ref: str | None = None,
) -> None:
    """Require merged evidence whose owner content still matches the candidate."""
    main_commit = run_git(
        repository_root, "rev-parse", "--verify", f"{main_ref}^{{commit}}"
    )
    candidate_commit = run_git(
        repository_root,
        "rev-parse",
        "--verify",
        f"{candidate_ref or main_ref}^{{commit}}",
    )
    candidate_ancestry = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "merge-base",
            "--is-ancestor",
            main_commit,
            candidate_commit,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if candidate_ancestry.returncode != 0:
        raise FrontierError(
            f"candidate {candidate_commit} does not contain merged ref {main_commit}"
        )
    target_identities: dict[tuple[str, EvidenceTarget], str] = {}
    checked_commits: set[str] = set()
    for row_number, frontier_row in enumerate(frontier_rows, start=2):
        evidence_commit = frontier_row["merged_evidence_commit"]
        if not evidence_commit:
            continue
        if evidence_commit not in checked_commits:
            ancestry = subprocess.run(
                [
                    "git",
                    "-C",
                    str(repository_root),
                    "merge-base",
                    "--is-ancestor",
                    evidence_commit,
                    main_commit,
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            if ancestry.returncode != 0:
                diagnostic = ancestry.stderr.strip() or "commit is not reachable"
                raise FrontierError(
                    f"evidence commit {evidence_commit} is not merged in {main_ref}: "
                    f"{diagnostic}"
                )
            checked_commits.add(evidence_commit)
        for target in canonical_targets(
            frontier_row["canonical_data_target"], row_number
        ):
            evidence_key = (evidence_commit, target)
            if evidence_key not in target_identities:
                target_identities[evidence_key] = evidence_target_identity(
                    repository_root,
                    evidence_commit,
                    target,
                )
            candidate_key = (candidate_commit, target)
            if candidate_key not in target_identities:
                target_identities[candidate_key] = evidence_target_identity(
                    repository_root,
                    candidate_commit,
                    target,
                )
            if target_identities[evidence_key] != target_identities[candidate_key]:
                thread_id = frontier_row["thread_id"]
                raise FrontierError(
                    f"{thread_id}: evidence owner {target.declaration} changed "
                    f"between {evidence_commit} and candidate {candidate_commit}"
                )


def verify_clean_candidate(repository_root: Path, candidate_ref: str) -> None:
    """Require candidate evidence to identify the clean checked-out commit."""
    candidate_commit = run_git(
        repository_root, "rev-parse", "--verify", f"{candidate_ref}^{{commit}}"
    )
    head_commit = run_git(repository_root, "rev-parse", "--verify", "HEAD^{commit}")
    if candidate_commit != head_commit:
        raise FrontierError(
            f"candidate {candidate_commit} does not identify checked-out HEAD "
            f"{head_commit}"
        )
    status = run_git(
        repository_root,
        "status",
        "--porcelain=v2",
        "--untracked-files=all",
        strip=False,
    )
    if status:
        raise FrontierError("candidate worktree is not clean at the declared SHA")


def validate_live_payload(
    frontier_rows: list[dict[str, str]], payload: dict[str, object]
) -> None:
    """Bind exact thread IDs to discussion URLs and live resolution state."""
    payload_data = payload.get("data")
    if not isinstance(payload_data, dict):
        raise FrontierError("live GraphQL payload has no data object")
    for row_index, frontier_row in enumerate(frontier_rows):
        alias = f"thread_{row_index}"
        thread = payload_data.get(alias)
        if not isinstance(thread, dict):
            raise FrontierError(f"live GraphQL payload has no node for {alias}")
        if thread.get("id") != frontier_row["thread_id"]:
            raise FrontierError(f"live GraphQL node mismatch for {alias}")
        expected_resolved = frontier_row["resolution_state"] == "resolved"
        if thread.get("isResolved") is not expected_resolved:
            raise FrontierError(
                f"{frontier_row['thread_id']}: live resolution state differs"
            )
        expected_outdated = frontier_row["is_outdated"] == "true"
        if thread.get("isOutdated") is not expected_outdated:
            raise FrontierError(
                f"{frontier_row['thread_id']}: live outdated state differs"
            )
        comments = thread.get("comments")
        if not isinstance(comments, dict):
            raise FrontierError(f"{frontier_row['thread_id']}: comments are missing")
        comment_nodes = comments.get("nodes")
        if not isinstance(comment_nodes, list) or len(comment_nodes) != 1:
            raise FrontierError(
                f"{frontier_row['thread_id']}: first comment is not unique"
            )
        first_comment = comment_nodes[0]
        if not isinstance(first_comment, dict):
            raise FrontierError(
                f"{frontier_row['thread_id']}: first comment is malformed"
            )
        if first_comment.get("url") != frontier_row["review_url"]:
            raise FrontierError(
                f"{frontier_row['thread_id']}: discussion URL differs from live node"
            )


def verify_live_threads(frontier_rows: list[dict[str, str]]) -> None:
    """Query all frontier thread nodes in one authenticated GraphQL request."""
    selections = []
    for row_index, frontier_row in enumerate(frontier_rows):
        selections.append(
            f'thread_{row_index}: node(id: "{frontier_row["thread_id"]}") {{ '
            "... on PullRequestReviewThread { id isResolved isOutdated "
            "comments(first: 1) { nodes { url } } } }"
        )
    query = "query { " + " ".join(selections) + " }"
    result = subprocess.run(
        ["gh", "api", "graphql", "-f", f"query={query}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        raise FrontierError(f"live GraphQL query failed: {diagnostic}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise FrontierError("live GraphQL query returned invalid JSON") from error
    if not isinstance(payload, dict):
        raise FrontierError("live GraphQL query returned a non-object")
    if payload.get("errors"):
        raise FrontierError(f"live GraphQL query returned errors: {payload['errors']}")
    validate_live_payload(frontier_rows, payload)


def validate_files(
    frontier_path: Path,
    ledger_path: Path,
    batch_size: int,
    repository_root: Path,
    main_ref: str,
    candidate_ref: str,
    live: bool,
    require_clean_candidate: bool = False,
) -> None:
    if require_clean_candidate or candidate_ref == "HEAD":
        verify_clean_candidate(repository_root, candidate_ref)
    frontier_rows = read_tsv(frontier_path, FRONTIER_FIELDS)
    ledger_rows = read_tsv(ledger_path, LEDGER_FIELDS)
    frontier_by_thread = validate_frontier(frontier_rows, batch_size)
    validate_ledger(ledger_rows, frontier_by_thread)
    verify_merged_evidence(
        frontier_rows,
        repository_root,
        main_ref,
        candidate_ref,
    )
    if live:
        verify_live_threads(frontier_rows)
    print(
        f"OK: {len(frontier_rows)} frontier rows; {len(ledger_rows)} verified closures"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontier", type=Path, required=True)
    parser.add_argument("--ledger", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--main-ref", default="origin/main")
    parser.add_argument("--candidate-ref", default="HEAD")
    parser.add_argument("--require-clean-candidate", action="store_true")
    parser.add_argument("--live", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.batch_size <= 0:
            raise FrontierError("--batch-size must be positive")
        validate_files(
            arguments.frontier,
            arguments.ledger,
            arguments.batch_size,
            arguments.repo_root,
            arguments.main_ref,
            arguments.candidate_ref,
            arguments.live,
            arguments.require_clean_candidate,
        )
    except (FrontierError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
