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
from typing import Any

from review_thread_frontier import (
    FRONTIER_FIELDS,
    LEDGER_FIELDS,
    FrontierError,
    read_tsv,
    validate_frontier,
)

SCHEMA = "mesa-review-thread-resolution-v1"
EXPECTED_BATCH_SIZE = 50


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
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise FrontierError("GraphQL returned invalid JSON") from error
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
    if not isinstance(payload, dict) or payload.get("schema") != SCHEMA:
        raise FrontierError("resolution journal has an invalid schema")
    return payload


def validate_resolution_frontier(rows: list[dict[str, str]]) -> None:
    """Reject malformed or non-campaign frontiers before GraphQL construction."""
    validate_frontier(rows, EXPECTED_BATCH_SIZE)


def write_ledger(
    path: Path, rows: list[dict[str, str]], journal: dict[str, Any]
) -> None:
    entries = {entry["thread_id"]: entry for entry in journal["entries"]}
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=LEDGER_FIELDS, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        for row in rows:
            entry = entries[row["thread_id"]]
            writer.writerow(
                {
                    "thread_id": row["thread_id"],
                    "disposition": row["disposition"],
                    "evidence_commit": row["merged_evidence_commit"],
                    "evidence_pr": str(journal["evidence_pr"]),
                    "merged_at": journal["merged_at"],
                    "github_resolved_at": entry["resolved_at"],
                    "post_resolution_verified_at": journal["post_verified_at"],
                    "closure_note": row["mechanism"],
                }
            )


def validate_journal(
    rows: list[dict[str, str]], journal: dict[str, Any], path: Path
) -> None:
    if journal.get("frontier_sha256") != frontier_hash(path):
        raise FrontierError("journal frontier hash differs")
    entries = journal.get("entries")
    if not isinstance(entries, list):
        raise FrontierError("journal entries are missing")
    ids = [entry.get("thread_id") for entry in entries if isinstance(entry, dict)]
    expected = [row["thread_id"] for row in rows]
    if ids != expected[: len(ids)] or len(set(ids)) != len(ids):
        raise FrontierError("journal entries are not an ordered frontier prefix")
    if journal.get("complete") and (
        ids != expected or not journal.get("post_verified_at")
    ):
        raise FrontierError("complete journal lacks full postflight evidence")


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
    if not journal.get("complete"):
        raise FrontierError("resolution journal is incomplete")
    expected_path = arguments.ledger.with_suffix(arguments.ledger.suffix + ".expected")
    write_ledger(expected_path, rows, journal)
    expected = expected_path.read_bytes()
    expected_path.unlink()
    if arguments.ledger.read_bytes() != expected:
        raise FrontierError("resolution ledger differs from journal")
    if arguments.live and not all(query_rows(rows).values()):
        raise FrontierError("live postflight found unresolved threads")


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
        else:
            check(rows, arguments)
    except (FrontierError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"OK: {len(rows)} exact review-thread resolutions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
