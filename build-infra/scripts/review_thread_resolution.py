#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Resolve an exact classified review-thread frontier with a durable journal."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, cast

from review_thread_frontier import (
    CLOSED_DISPOSITIONS,
    COMMIT_PATTERN,
    FRONTIER_FIELDS,
    LEDGER_FIELDS,
    FrontierError,
    canonical_targets,
    evidence_target_identity,
    parse_timestamp,
    read_tsv,
    run_git,
    validate_frontier,
)

SCHEMA = "mesa-review-thread-resolution-v1"
RECOVERY_SCHEMA = "mesa-review-thread-resolution-v2"
EXPECTED_BATCH_SIZE = 50
REPOSITORY_OWNER = "Oichkatzelesfrettschen"
REPOSITORY_NAME = "mesa-26-gororoba"
RECOVERY_ENTRY_FIELDS = frozenset(
    (
        "thread_id",
        "disposition",
        "evidence_commit",
        "evidence_pr",
        "merged_at",
        "resolved_at",
        "verified_at",
    )
)


def now() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--frontier", type=Path, required=True)
    resolve = subparsers.add_parser("resolve")
    resolve.add_argument("--frontier", type=Path, required=True)
    resolve.add_argument("--journal", type=Path, required=True)
    resolve.add_argument("--ledger", type=Path, required=True)
    resolve.add_argument("--evidence-pr", type=int, required=True)
    resolve.add_argument("--merged-at", required=True)
    record = subparsers.add_parser("record")
    record.add_argument("--frontier", type=Path, required=True)
    record.add_argument("--journal", type=Path, required=True)
    record.add_argument("--ledger", type=Path, required=True)
    record.add_argument("--repo-root", type=Path, required=True)
    record.add_argument("--main-ref", default="origin/main")
    record.add_argument("--thread-id", required=True)
    record.add_argument(
        "--disposition", choices=sorted(CLOSED_DISPOSITIONS), required=True
    )
    record.add_argument("--evidence-commit", required=True)
    record.add_argument("--evidence-pr", type=int, required=True)
    record.add_argument("--merged-at", required=True)
    check = subparsers.add_parser("check")
    check.add_argument("--frontier", type=Path, required=True)
    check.add_argument("--journal", type=Path, required=True)
    check.add_argument("--ledger", type=Path, required=True)
    check.add_argument("--live", action="store_true")
    return parser.parse_args()


def graphql(query: str, variables: dict[str, str] | None = None) -> dict[str, Any]:
    command = ["gh", "api", "graphql", "-f", f"query={query}"]
    for name, value in (variables or {}).items():
        command.extend(("-F", f"{name}={value}"))
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise FrontierError(
            f"GraphQL failed: {(result.stderr or result.stdout).strip()}"
        )
    try:
        payload_object: object = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise FrontierError("GraphQL returned invalid JSON") from error
    if not isinstance(payload_object, dict):
        raise FrontierError("GraphQL returned non-object JSON")
    payload = cast(dict[str, Any], payload_object)
    if payload.get("errors"):
        raise FrontierError(f"GraphQL returned errors: {payload['errors']}")
    return payload


def query_rows(rows: list[dict[str, str]]) -> dict[str, bool]:
    selections = []
    for index, row in enumerate(rows):
        selections.append(
            f't{index}:node(id:"{row["thread_id"]}"){{... on '
            "PullRequestReviewThread{id isResolved isOutdated "
            "comments(first:1){nodes{url}}}}"
        )
    payload = graphql("query{" + " ".join(selections) + "}")
    data = payload.get("data")
    if not isinstance(data, dict):
        raise FrontierError("thread query omitted data")
    states: dict[str, bool] = {}
    for index, row in enumerate(rows):
        node = data.get(f"t{index}")
        if not isinstance(node, dict) or node.get("id") != row["thread_id"]:
            raise FrontierError(f"thread identity differs for {row['thread_id']}")
        expected_outdated = row["is_outdated"] == "true"
        if node.get("isOutdated") is not expected_outdated:
            raise FrontierError(f"outdated state differs for {row['thread_id']}")
        comments = node.get("comments", {}).get("nodes")
        if not isinstance(comments, list) or len(comments) != 1:
            raise FrontierError(f"first comment missing for {row['thread_id']}")
        if comments[0].get("url") != row["review_url"]:
            raise FrontierError(f"discussion URL differs for {row['thread_id']}")
        resolved = node.get("isResolved")
        if not isinstance(resolved, bool):
            raise FrontierError(f"resolution state missing for {row['thread_id']}")
        states[row["thread_id"]] = resolved
    return states


def resolve_one(row: dict[str, str]) -> None:
    mutation = (
        "mutation($id:ID!){resolveReviewThread(input:{threadId:$id})"
        "{thread{id isResolved isOutdated comments(first:1){nodes{url}}}}}"
    )
    payload = graphql(mutation, {"id": row["thread_id"]})
    thread = payload.get("data", {}).get("resolveReviewThread", {}).get("thread")
    if not isinstance(thread, dict) or thread.get("id") != row["thread_id"]:
        raise FrontierError(f"mutation omitted {row['thread_id']}")
    if thread.get("isResolved") is not True:
        raise FrontierError(f"mutation did not resolve {row['thread_id']}")


def frontier_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def load_journal(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FrontierError(f"cannot read resolution journal: {error}") from error
    if not isinstance(payload, dict) or payload.get("schema") not in (
        SCHEMA,
        RECOVERY_SCHEMA,
    ):
        raise FrontierError("resolution journal has an invalid schema")
    return payload


def validate_resolution_frontier(rows: list[dict[str, str]]) -> None:
    """Reject malformed or non-campaign frontiers before GraphQL construction."""
    validate_frontier(rows, EXPECTED_BATCH_SIZE)


def parse_journal_timestamp(value: Any, context: str) -> datetime:
    if not isinstance(value, str):
        raise FrontierError(f"{context}: timestamp is not text")
    return parse_timestamp(value, context)


def write_ledger(
    path: Path, rows: list[dict[str, str]], journal: dict[str, Any]
) -> None:
    entries = {entry["thread_id"]: entry for entry in journal["entries"]}
    if journal["schema"] == SCHEMA:
        ledger_rows = rows
    else:
        rows_by_thread = {row["thread_id"]: row for row in rows}
        ledger_rows = [
            rows_by_thread[entry["thread_id"]] for entry in journal["entries"]
        ]
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=LEDGER_FIELDS, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        for row in ledger_rows:
            entry = entries[row["thread_id"]]
            recovery = journal["schema"] == RECOVERY_SCHEMA
            writer.writerow(
                {
                    "thread_id": row["thread_id"],
                    "disposition": (
                        entry["disposition"] if recovery else row["disposition"]
                    ),
                    "evidence_commit": (
                        entry["evidence_commit"]
                        if recovery
                        else row["merged_evidence_commit"]
                    ),
                    "evidence_pr": str(
                        entry["evidence_pr"] if recovery else journal["evidence_pr"]
                    ),
                    "merged_at": (
                        entry["merged_at"] if recovery else journal["merged_at"]
                    ),
                    "github_resolved_at": entry["resolved_at"],
                    "post_resolution_verified_at": (
                        entry["verified_at"]
                        if recovery
                        else journal["post_verified_at"]
                    ),
                    "closure_note": row["mechanism"],
                }
            )


def validate_journal(
    rows: list[dict[str, str]], journal: dict[str, Any], path: Path
) -> None:
    schema = journal.get("schema")
    if schema not in (SCHEMA, RECOVERY_SCHEMA):
        raise FrontierError("resolution journal has an invalid schema")
    if journal.get("frontier_sha256") != frontier_hash(path):
        raise FrontierError("journal frontier hash differs")
    if not isinstance(journal.get("complete"), bool):
        raise FrontierError("resolution journal complete state is not boolean")
    entries = journal.get("entries")
    if not isinstance(entries, list):
        raise FrontierError("journal entries are missing")
    ids: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("thread_id"), str):
            raise FrontierError("journal entries have invalid thread IDs")
        ids.append(entry["thread_id"])
    expected = [row["thread_id"] for row in rows]
    if len(set(ids)) != len(ids):
        raise FrontierError("journal entries have duplicate thread IDs")
    if schema == SCHEMA:
        if ids != expected[: len(ids)]:
            raise FrontierError("journal entries are not an ordered frontier prefix")
    else:
        positions = {thread_id: index for index, thread_id in enumerate(expected)}
        if any(thread_id not in positions for thread_id in ids) or [
            positions[thread_id] for thread_id in ids
        ] != sorted(positions[thread_id] for thread_id in ids):
            raise FrontierError(
                "recovery journal entries are not an ordered frontier subsequence"
            )
        started_at = parse_journal_timestamp(
            journal.get("started_at"), "recovery journal"
        )
        for entry_number, entry in enumerate(entries, start=1):
            if set(entry) != RECOVERY_ENTRY_FIELDS:
                raise FrontierError(f"recovery entry {entry_number} has invalid fields")
            if entry["disposition"] not in CLOSED_DISPOSITIONS:
                raise FrontierError(
                    f"recovery entry {entry_number} has invalid disposition"
                )
            if not isinstance(
                entry["evidence_commit"], str
            ) or not COMMIT_PATTERN.fullmatch(entry["evidence_commit"]):
                raise FrontierError(
                    f"recovery entry {entry_number} has invalid evidence commit"
                )
            if (
                not isinstance(entry["evidence_pr"], int)
                or isinstance(entry["evidence_pr"], bool)
                or entry["evidence_pr"] <= 0
            ):
                raise FrontierError(
                    f"recovery entry {entry_number} has invalid evidence PR"
                )
            merged_at = parse_journal_timestamp(
                entry["merged_at"], f"recovery entry {entry_number}"
            )
            resolved_at = parse_journal_timestamp(
                entry["resolved_at"], f"recovery entry {entry_number}"
            )
            verified_at = parse_journal_timestamp(
                entry["verified_at"], f"recovery entry {entry_number}"
            )
            if not merged_at <= resolved_at <= verified_at:
                raise FrontierError(
                    f"recovery entry {entry_number} expected merged <= resolved <= verified"
                )
            if started_at > resolved_at:
                raise FrontierError(
                    f"recovery entry {entry_number} predates journal start"
                )
    if journal.get("complete") and (
        ids != expected or not journal.get("post_verified_at")
    ):
        raise FrontierError("complete journal lacks full postflight evidence")
    if schema == RECOVERY_SCHEMA:
        if journal.get("complete"):
            post_verified_at = parse_journal_timestamp(
                journal["post_verified_at"], "recovery journal postflight"
            )
            if entries and post_verified_at < max(
                parse_journal_timestamp(entry["verified_at"], "recovery entry")
                for entry in entries
            ):
                raise FrontierError(
                    "recovery postflight predates an entry verification"
                )
        elif journal.get("post_verified_at") is not None:
            raise FrontierError(
                "incomplete recovery journal carries postflight evidence"
            )


def verify_commit_on_main(
    repository_root: Path, evidence_commit: str, main_ref: str
) -> None:
    ancestry = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "merge-base",
            "--is-ancestor",
            evidence_commit,
            main_ref,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if ancestry.returncode == 1:
        raise FrontierError("record evidence commit is not an ancestor of main")
    if ancestry.returncode != 0:
        diagnostic = ancestry.stderr.strip() or ancestry.stdout.strip()
        raise FrontierError(f"record ancestry check failed: {diagnostic}")


def verify_record_targets(
    row: dict[str, str],
    repository_root: Path,
    evidence_commit: str,
    main_ref: str,
) -> None:
    for target in canonical_targets(row["canonical_data_target"], 1):
        evidence_identity = evidence_target_identity(
            repository_root, evidence_commit, target
        )
        main_identity = evidence_target_identity(repository_root, main_ref, target)
        if evidence_identity != main_identity:
            raise FrontierError(
                f"record evidence target differs on main: {target.declaration}"
            )


def verify_record_pull_request(
    evidence_commit: str, evidence_pr: int, merged_at_text: str
) -> None:
    query = (
        'query($number:Int!){repository(owner:"'
        + REPOSITORY_OWNER
        + '",name:"'
        + REPOSITORY_NAME
        + '"){pullRequest(number:$number){merged mergedAt mergeCommit{oid}}}}'
    )
    payload = graphql(query, {"number": str(evidence_pr)})
    pull_request = payload.get("data", {}).get("repository", {}).get("pullRequest")
    if not isinstance(pull_request, dict) or pull_request.get("merged") is not True:
        raise FrontierError("record evidence PR is not merged")
    merge_commit = pull_request.get("mergeCommit")
    if not isinstance(merge_commit, dict) or merge_commit.get("oid") != evidence_commit:
        raise FrontierError("record evidence PR merge commit differs")
    if pull_request.get("mergedAt") != merged_at_text:
        raise FrontierError("record evidence PR merge time differs")


def verify_record_authority(row: dict[str, str], arguments: argparse.Namespace) -> None:
    if not COMMIT_PATTERN.fullmatch(arguments.evidence_commit):
        raise FrontierError("record evidence commit is not 40-hex")
    if arguments.evidence_pr <= 0:
        raise FrontierError("record evidence PR must be positive")
    merged_at = parse_timestamp(arguments.merged_at, "record merged_at")
    repository_root = arguments.repo_root.resolve()
    discovered_root = Path(
        run_git(repository_root, "rev-parse", "--show-toplevel")
    ).resolve()
    if discovered_root != repository_root:
        raise FrontierError("record repository root is not the Git worktree root")

    verify_commit_on_main(
        repository_root, arguments.evidence_commit, arguments.main_ref
    )
    verify_record_targets(
        row, repository_root, arguments.evidence_commit, arguments.main_ref
    )
    verify_record_pull_request(
        arguments.evidence_commit, arguments.evidence_pr, arguments.merged_at
    )
    if merged_at > datetime.now(UTC):
        raise FrontierError("record evidence PR merge time is in the future")


def record(rows: list[dict[str, str]], arguments: argparse.Namespace) -> None:
    rows_by_thread = {row["thread_id"]: row for row in rows}
    row = rows_by_thread.get(arguments.thread_id)
    if row is None:
        raise FrontierError("record thread is outside the frontier")

    if arguments.journal.exists():
        journal = load_journal(arguments.journal)
    else:
        journal = {
            "schema": RECOVERY_SCHEMA,
            "frontier_sha256": frontier_hash(arguments.frontier),
            "started_at": now(),
            "entries": [],
            "complete": False,
            "post_verified_at": None,
        }
    if journal.get("schema") != RECOVERY_SCHEMA:
        raise FrontierError("record requires a v2 recovery journal")
    validate_journal(rows, journal, arguments.frontier)
    verify_record_authority(row, arguments)

    authority = {
        "thread_id": arguments.thread_id,
        "disposition": arguments.disposition,
        "evidence_commit": arguments.evidence_commit,
        "evidence_pr": arguments.evidence_pr,
        "merged_at": arguments.merged_at,
    }
    existing = next(
        (
            entry
            for entry in journal["entries"]
            if entry["thread_id"] == arguments.thread_id
        ),
        None,
    )
    if existing is not None:
        if any(existing[field] != value for field, value in authority.items()):
            raise FrontierError("record authority differs from existing entry")
        if not query_rows([row])[arguments.thread_id]:
            raise FrontierError("recorded thread is no longer resolved")
        write_ledger(arguments.ledger, rows, journal)
        return

    if not query_rows([row])[arguments.thread_id]:
        raise FrontierError("record thread is not resolved")
    resolved_at = now()
    if not query_rows([row])[arguments.thread_id]:
        raise FrontierError("record postflight found the thread unresolved")
    verified_at = now()
    entry = {
        **authority,
        "resolved_at": resolved_at,
        "verified_at": verified_at,
    }
    positions = {row["thread_id"]: index for index, row in enumerate(rows)}
    journal["entries"].append(entry)
    journal["entries"].sort(key=lambda item: positions[item["thread_id"]])
    if len(journal["entries"]) == len(rows):
        if not all(query_rows(rows).values()):
            raise FrontierError("record final postflight found unresolved threads")
        journal["complete"] = True
        journal["post_verified_at"] = now()
    validate_journal(rows, journal, arguments.frontier)
    atomic_json(arguments.journal, journal)
    write_ledger(arguments.ledger, rows, journal)


def resolve(rows: list[dict[str, str]], arguments: argparse.Namespace) -> None:
    if arguments.journal.exists():
        journal = load_journal(arguments.journal)
    else:
        journal = {
            "schema": SCHEMA,
            "frontier_sha256": frontier_hash(arguments.frontier),
            "evidence_pr": arguments.evidence_pr,
            "merged_at": arguments.merged_at,
            "started_at": now(),
            "entries": [],
            "complete": False,
            "post_verified_at": None,
        }
    validate_journal(rows, journal, arguments.frontier)
    if journal.get("schema") != SCHEMA:
        raise FrontierError("resolve requires a v1 ordered-prefix journal")
    if (
        journal["evidence_pr"] != arguments.evidence_pr
        or journal["merged_at"] != arguments.merged_at
    ):
        raise FrontierError("resolution authority differs from journal")
    resolved_ids = {entry["thread_id"] for entry in journal["entries"]}
    states = query_rows(rows)
    for row in rows:
        expected_resolved = row["thread_id"] in resolved_ids
        if states[row["thread_id"]] is not expected_resolved:
            raise FrontierError(f"preflight state drift for {row['thread_id']}")
    for row in rows:
        if row["thread_id"] in resolved_ids:
            continue
        if query_rows([row])[row["thread_id"]]:
            raise FrontierError(f"thread resolved before mutation: {row['thread_id']}")
        resolve_one(row)
        journal["entries"].append({"thread_id": row["thread_id"], "resolved_at": now()})
        atomic_json(arguments.journal, journal)
    states = query_rows(rows)
    if not all(states.values()):
        raise FrontierError("postflight found unresolved threads")
    journal["complete"] = True
    journal["post_verified_at"] = now()
    atomic_json(arguments.journal, journal)
    write_ledger(arguments.ledger, rows, journal)


def check(rows: list[dict[str, str]], arguments: argparse.Namespace) -> None:
    journal = load_journal(arguments.journal)
    validate_journal(rows, journal, arguments.frontier)
    if journal.get("schema") == SCHEMA and not journal.get("complete"):
        raise FrontierError("resolution journal is incomplete")
    expected_path = arguments.ledger.with_suffix(arguments.ledger.suffix + ".expected")
    write_ledger(expected_path, rows, journal)
    expected = expected_path.read_bytes()
    expected_path.unlink()
    if arguments.ledger.read_bytes() != expected:
        raise FrontierError("resolution ledger differs from journal")
    if arguments.live:
        if journal.get("schema") == SCHEMA:
            live_rows = rows
        else:
            recorded_ids = {entry["thread_id"] for entry in journal["entries"]}
            live_rows = [row for row in rows if row["thread_id"] in recorded_ids]
        if live_rows and not all(query_rows(live_rows).values()):
            raise FrontierError("live postflight found unresolved recorded threads")


def main() -> int:
    arguments = parse_args()
    try:
        rows = read_tsv(arguments.frontier, FRONTIER_FIELDS)
        validate_resolution_frontier(rows)
        if arguments.command == "preflight":
            states = query_rows(rows)
            if any(states.values()):
                raise FrontierError("preflight found already-resolved threads")
        elif arguments.command == "resolve":
            resolve(rows, arguments)
        elif arguments.command == "record":
            record(rows, arguments)
        else:
            check(rows, arguments)
    except (FrontierError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    if arguments.command == "record":
        journal = load_journal(arguments.journal)
        print(f"OK: {len(journal['entries'])} recorded review-thread resolutions")
    else:
        print(f"OK: {len(rows)} exact review-thread resolutions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
