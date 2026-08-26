#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture one bounded oldest-unresolved merged-PR review-thread frontier."""

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

CAPTURE_SCHEMA = "mesa-merged-review-thread-batch-v1"
FRONTIER_FIELDS = (
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
MAX_PR_PAGES = 100
MAX_THREAD_PAGES_PER_PR = 20
MAX_COMMENT_PAGES_PER_THREAD = 20

MERGED_PR_QUERY = r"""
query($owner:String!, $repo:String!, $cursor:String) {
  repository(owner:$owner, name:$repo) {
    defaultBranchRef { name target { ... on Commit { oid } } }
    pullRequests(
      first:100
      after:$cursor
      states:MERGED
      orderBy:{field:CREATED_AT,direction:ASC}
    ) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        title
        url
        createdAt
        mergedAt
        reviewThreads(first:100) {
          pageInfo { hasNextPage endCursor }
          nodes {
            id
            isResolved
            isOutdated
            path
            line
            originalLine
            comments(first:1) {
              pageInfo { hasNextPage endCursor }
              nodes {
                id
                databaseId
                author { login }
                body
                createdAt
                updatedAt
                url
                commit { oid }
                originalCommit { oid }
              }
            }
          }
        }
      }
    }
  }
}
"""

PR_THREADS_QUERY = r"""
query($owner:String!, $repo:String!, $number:Int!, $cursor:String) {
  repository(owner:$owner, name:$repo) {
    defaultBranchRef { name target { ... on Commit { oid } } }
    pullRequest(number:$number) {
      reviewThreads(first:100, after:$cursor) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id
          isResolved
          isOutdated
          path
          line
          originalLine
          comments(first:1) {
            pageInfo { hasNextPage endCursor }
            nodes {
              id
              databaseId
              author { login }
              body
              createdAt
              updatedAt
              url
              commit { oid }
              originalCommit { oid }
            }
          }
        }
      }
    }
  }
}
"""

THREAD_COMMENTS_QUERY = r"""
query($threadId:ID!, $cursor:String) {
  node(id:$threadId) {
    ... on PullRequestReviewThread {
      id
      comments(first:100, after:$cursor) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id
          databaseId
          author { login }
          body
          createdAt
          updatedAt
          url
          commit { oid }
          originalCommit { oid }
        }
      }
    }
  }
}
"""

DEFAULT_BRANCH_QUERY = r"""
query($owner:String!, $repo:String!) {
  repository(owner:$owner, name:$repo) {
    defaultBranchRef { name target { ... on Commit { oid } } }
  }
}
"""


class CaptureError(ValueError):
    """The live capture or its finite-denominator proof is invalid."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    capture_parser = subparsers.add_parser("capture")
    capture_parser.add_argument("--owner", default="Oichkatzelesfrettschen")
    capture_parser.add_argument("--repo", default="mesa-26-gororoba")
    capture_parser.add_argument("--batch-id", required=True)
    capture_parser.add_argument("--batch-size", type=int, default=50)
    capture_parser.add_argument("--output-dir", type=Path, required=True)
    check_parser = subparsers.add_parser("check")
    check_parser.add_argument("--input-dir", type=Path, required=True)
    return parser.parse_args()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def parse_timestamp(value: Any, label: str) -> datetime:
    if not isinstance(value, str) or not value:
        raise CaptureError(f"{label} is missing")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise CaptureError(f"{label} is not an ISO 8601 timestamp: {value}") from error
    if parsed.tzinfo is None:
        raise CaptureError(f"{label} lacks a timezone")
    return parsed


def graphql(query: str, variables: dict[str, str | int]) -> dict[str, Any]:
    command = ["gh", "api", "graphql", "-F", "query=@-"]
    for name, value in variables.items():
        command.extend(("-F", f"{name}={value}"))
    result = subprocess.run(
        command,
        input=query,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        diagnostic = " ".join((result.stderr or result.stdout).split())
        raise CaptureError(f"GitHub GraphQL query failed: {diagnostic}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise CaptureError("GitHub GraphQL returned invalid JSON") from error
    if not isinstance(payload, dict):
        raise CaptureError("GitHub GraphQL returned a non-object")
    if payload.get("errors"):
        raise CaptureError(f"GitHub GraphQL returned errors: {payload['errors']}")
    return payload


def repository_from_payload(payload: dict[str, Any], label: str) -> dict[str, Any]:
    repository = payload.get("data", {}).get("repository")
    if not isinstance(repository, dict):
        raise CaptureError(f"{label} omitted the repository")
    return repository


def branch_identity(repository: dict[str, Any], label: str) -> tuple[str, str]:
    branch = repository.get("defaultBranchRef")
    target = branch.get("target") if isinstance(branch, dict) else None
    name = branch.get("name") if isinstance(branch, dict) else None
    oid = target.get("oid") if isinstance(target, dict) else None
    if not isinstance(name, str) or not isinstance(oid, str) or len(oid) != 40:
        raise CaptureError(f"{label} omitted the default-branch identity")
    return name, oid


def page_info(connection: dict[str, Any], label: str) -> tuple[bool, str | None]:
    information = connection.get("pageInfo")
    if not isinstance(information, dict):
        raise CaptureError(f"{label} omitted pageInfo")
    has_next_page = information.get("hasNextPage")
    end_cursor = information.get("endCursor")
    if not isinstance(has_next_page, bool):
        raise CaptureError(f"{label} has invalid hasNextPage")
    if has_next_page and (not isinstance(end_cursor, str) or not end_cursor):
        raise CaptureError(f"{label} requires a nonempty endCursor")
    return has_next_page, end_cursor if isinstance(end_cursor, str) else None


def normalize_comment(comment: Any, thread_id: str) -> dict[str, Any]:
    if not isinstance(comment, dict):
        raise CaptureError(f"{thread_id} contains a malformed comment")
    comment_id = comment.get("id")
    author = comment.get("author")
    body = comment.get("body")
    if not isinstance(comment_id, str) or not comment_id:
        raise CaptureError(f"{thread_id} contains a comment without an ID")
    if not isinstance(body, str):
        raise CaptureError(f"{thread_id} comment {comment_id} lacks a body")
    return {
        "id": comment_id,
        "database_id": comment.get("databaseId"),
        "author": (
            author.get("login")
            if isinstance(author, dict) and isinstance(author.get("login"), str)
            else "none"
        ),
        "body": body,
        "created_at": comment.get("createdAt"),
        "updated_at": comment.get("updatedAt"),
        "url": comment.get("url"),
        "commit_oid": (
            comment.get("commit", {}).get("oid")
            if isinstance(comment.get("commit"), dict)
            else None
        ),
        "original_commit_oid": (
            comment.get("originalCommit", {}).get("oid")
            if isinstance(comment.get("originalCommit"), dict)
            else None
        ),
    }


def append_comment_page(
    thread: dict[str, Any],
    connection: dict[str, Any],
    seen_comment_ids: set[str],
) -> None:
    thread_id = thread["id"]
    nodes = connection.get("nodes")
    if not isinstance(nodes, list):
        raise CaptureError(f"{thread_id} omitted comment nodes")
    for node in nodes:
        comment = normalize_comment(node, thread_id)
        comment_id = comment["id"]
        if comment_id in seen_comment_ids:
            raise CaptureError(f"{thread_id} repeated comment {comment_id}")
        seen_comment_ids.add(comment_id)
        thread["comments"].append(comment)


def normalize_thread(
    node: Any, pr_number: int
) -> tuple[dict[str, Any], dict[str, Any]]:
    if not isinstance(node, dict):
        raise CaptureError(f"PR {pr_number} contains a malformed review thread")
    thread_id = node.get("id")
    if not isinstance(thread_id, str) or not thread_id:
        raise CaptureError(f"PR {pr_number} contains a review thread without an ID")
    comments = node.get("comments")
    if not isinstance(comments, dict):
        raise CaptureError(f"{thread_id} omitted comments")
    thread = {
        "id": thread_id,
        "is_resolved": bool(node.get("isResolved")),
        "is_outdated": bool(node.get("isOutdated")),
        "path": node.get("path"),
        "line": node.get("line"),
        "original_line": node.get("originalLine"),
        "comments": [],
    }
    append_comment_page(thread, comments, set())
    if not thread["comments"]:
        raise CaptureError(f"{thread_id} has no comments")
    return thread, comments


def normalize_pr(node: Any) -> tuple[dict[str, Any], dict[str, Any]]:
    if not isinstance(node, dict):
        raise CaptureError("merged-PR page contains a malformed pull request")
    number = node.get("number")
    if not isinstance(number, int) or number <= 0:
        raise CaptureError(f"merged-PR page contains invalid PR number {number}")
    parse_timestamp(node.get("createdAt"), f"PR {number} createdAt")
    parse_timestamp(node.get("mergedAt"), f"PR {number} mergedAt")
    thread_connection = node.get("reviewThreads")
    if not isinstance(thread_connection, dict):
        raise CaptureError(f"PR {number} omitted reviewThreads")
    thread_nodes = thread_connection.get("nodes")
    if not isinstance(thread_nodes, list):
        raise CaptureError(f"PR {number} omitted review-thread nodes")
    normalized = {
        "number": number,
        "title": node.get("title"),
        "url": node.get("url"),
        "created_at": node.get("createdAt"),
        "merged_at": node.get("mergedAt"),
        "threads": [],
    }
    comment_connections: dict[str, Any] = {}
    for thread_node in thread_nodes:
        thread, comments = normalize_thread(thread_node, number)
        normalized["threads"].append(thread)
        comment_connections[thread["id"]] = comments
    return normalized, {
        "threads": thread_connection,
        "comments": comment_connections,
    }


def unresolved_rows(pull_requests: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for pull_request in pull_requests:
        for thread in pull_request["threads"]:
            if thread["is_resolved"]:
                continue
            first_comment = thread["comments"][0]
            created_at = first_comment.get("created_at")
            parse_timestamp(created_at, f"{thread['id']} first comment createdAt")
            rows.append(
                {
                    "thread_created_at": created_at,
                    "thread_id": thread["id"],
                    "pr": pull_request,
                    "thread": thread,
                }
            )
    return sorted(rows, key=lambda row: (row["thread_created_at"], row["thread_id"]))


def denominator_is_bounded(
    pull_requests: list[dict[str, Any]], rows: list[dict[str, Any]], batch_size: int
) -> bool:
    if len(rows) < batch_size or not pull_requests:
        return False
    cutoff = parse_timestamp(rows[batch_size - 1]["thread_created_at"], "rank cutoff")
    last_pr = pull_requests[-1]
    last_pr_created = parse_timestamp(
        last_pr["created_at"], "last scanned PR createdAt"
    )
    return last_pr_created > cutoff


def capture_supplemental_comments(
    thread: dict[str, Any],
    initial_connection: dict[str, Any],
    raw_payloads: list[tuple[str, dict[str, Any]]],
) -> int:
    has_next_page, cursor = page_info(initial_connection, f"{thread['id']} comments")
    seen_comment_ids = {comment["id"] for comment in thread["comments"]}
    page_number = 1
    while has_next_page:
        page_number += 1
        if page_number > MAX_COMMENT_PAGES_PER_THREAD:
            raise CaptureError(f"{thread['id']} comment pagination exceeds its bound")
        payload = graphql(
            THREAD_COMMENTS_QUERY,
            {"threadId": thread["id"], "cursor": cursor or ""},
        )
        raw_payloads.append(
            (
                f"thread-{thread['id']}-comments-{page_number:04d}.json",
                {
                    "query": "thread-comments",
                    "variables": {"threadId": thread["id"], "cursor": cursor},
                    "response": payload,
                },
            )
        )
        node = payload.get("data", {}).get("node")
        if not isinstance(node, dict) or node.get("id") != thread["id"]:
            raise CaptureError(f"supplemental comment query omitted {thread['id']}")
        connection = node.get("comments")
        if not isinstance(connection, dict):
            raise CaptureError(
                f"supplemental comment query omitted {thread['id']} comments"
            )
        append_comment_page(thread, connection, seen_comment_ids)
        has_next_page, cursor = page_info(connection, f"{thread['id']} comments")
    return page_number


def capture_supplemental_threads(
    owner: str,
    repo: str,
    pull_request: dict[str, Any],
    initial_connection: dict[str, Any],
    expected_branch: tuple[str, str],
    comment_connections: dict[str, Any],
    raw_payloads: list[tuple[str, dict[str, Any]]],
    seen_thread_ids: set[str],
) -> int:
    has_next_page, cursor = page_info(
        initial_connection, f"PR {pull_request['number']} threads"
    )
    page_number = 1
    while has_next_page:
        page_number += 1
        if page_number > MAX_THREAD_PAGES_PER_PR:
            raise CaptureError(
                f"PR {pull_request['number']} thread pagination exceeds its bound"
            )
        payload = graphql(
            PR_THREADS_QUERY,
            {
                "owner": owner,
                "repo": repo,
                "number": pull_request["number"],
                "cursor": cursor or "",
            },
        )
        raw_payloads.append(
            (
                f"pr-{pull_request['number']}-threads-{page_number:04d}.json",
                {
                    "query": "pr-review-threads",
                    "variables": {
                        "owner": owner,
                        "repo": repo,
                        "number": pull_request["number"],
                        "cursor": cursor,
                    },
                    "response": payload,
                },
            )
        )
        repository = repository_from_payload(payload, "supplemental thread query")
        if branch_identity(repository, "supplemental thread query") != expected_branch:
            raise CaptureError("default branch changed during thread pagination")
        pull_request_node = repository.get("pullRequest")
        connection = (
            pull_request_node.get("reviewThreads")
            if isinstance(pull_request_node, dict)
            else None
        )
        if not isinstance(connection, dict):
            raise CaptureError(
                f"supplemental thread query omitted PR {pull_request['number']}"
            )
        nodes = connection.get("nodes")
        if not isinstance(nodes, list):
            raise CaptureError(
                f"PR {pull_request['number']} omitted supplemental threads"
            )
        for thread_node in nodes:
            thread, comments = normalize_thread(thread_node, pull_request["number"])
            if thread["id"] in seen_thread_ids:
                raise CaptureError(f"GitHub repeated review thread {thread['id']}")
            seen_thread_ids.add(thread["id"])
            pull_request["threads"].append(thread)
            comment_connections[thread["id"]] = comments
        has_next_page, cursor = page_info(
            connection, f"PR {pull_request['number']} threads"
        )
    return page_number


def observe_default_branch(owner: str, repo: str) -> tuple[str, str]:
    payload = graphql(DEFAULT_BRANCH_QUERY, {"owner": owner, "repo": repo})
    repository = repository_from_payload(payload, "default-branch query")
    return branch_identity(repository, "default-branch query")


def build_frontier_rows(selected: list[dict[str, Any]]) -> list[dict[str, str]]:
    output: list[dict[str, str]] = []
    for rank, row in enumerate(selected, start=1):
        pull_request = row["pr"]
        thread = row["thread"]
        comments = thread["comments"]
        first_comment = comments[0]
        authors = sorted({comment["author"] for comment in comments})
        output.append(
            {
                "batch_rank": str(rank),
                "thread_id": thread["id"],
                "pr_number": str(pull_request["number"]),
                "pr_title": str(pull_request["title"]),
                "pr_created_at": pull_request["created_at"],
                "pr_merged_at": pull_request["merged_at"],
                "thread_created_at": row["thread_created_at"],
                "is_outdated": str(thread["is_outdated"]).lower(),
                "review_path": str(thread["path"] or "none"),
                "line": "" if thread["line"] is None else str(thread["line"]),
                "original_line": (
                    ""
                    if thread["original_line"] is None
                    else str(thread["original_line"])
                ),
                "review_authors": ";".join(authors),
                "comment_count": str(len(comments)),
                "first_comment_url": str(first_comment["url"]),
                "first_comment_sha256": sha256_bytes(
                    first_comment["body"].encode("utf-8")
                ),
            }
        )
    return output


def json_bytes(payload: Any) -> bytes:
    return (
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("utf-8")


def write_capture(
    output_dir: Path,
    query_files: dict[str, str],
    raw_payloads: list[tuple[str, dict[str, Any]]],
    selected: list[dict[str, Any]],
    manifest: dict[str, Any],
) -> None:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise CaptureError(f"output directory is not empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = output_dir / "raw"
    raw_dir.mkdir()
    hashes: dict[str, str] = {}
    for name, query in query_files.items():
        relative_path = f"{name}.graphql"
        content = query.encode("utf-8")
        (output_dir / relative_path).write_bytes(content)
        hashes[relative_path] = sha256_bytes(content)
    for name, payload in raw_payloads:
        relative_path = f"raw/{name}"
        content = json_bytes(payload)
        (output_dir / relative_path).write_bytes(content)
        hashes[relative_path] = sha256_bytes(content)
    selected_content = json_bytes(selected)
    (output_dir / "selected-threads.json").write_bytes(selected_content)
    hashes["selected-threads.json"] = sha256_bytes(selected_content)
    frontier_path = output_dir / "frontier.tsv"
    with frontier_path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=FRONTIER_FIELDS,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(build_frontier_rows(selected))
    hashes["frontier.tsv"] = sha256_bytes(frontier_path.read_bytes())
    manifest["files"] = dict(sorted(hashes.items()))
    (output_dir / "manifest.json").write_bytes(json_bytes(manifest))


def capture(
    owner: str, repo: str, batch_id: str, batch_size: int
) -> tuple[
    list[tuple[str, dict[str, Any]]],
    list[dict[str, Any]],
    dict[str, Any],
]:
    if batch_size <= 0:
        raise CaptureError("--batch-size must be positive")
    cursor: str | None = None
    expected_branch: tuple[str, str] | None = None
    pull_requests: list[dict[str, Any]] = []
    seen_pr_numbers: set[int] = set()
    seen_thread_ids: set[str] = set()
    raw_payloads: list[tuple[str, dict[str, Any]]] = []
    thread_page_counts: dict[str, int] = {}
    comment_page_counts: dict[str, int] = {}
    selected: list[dict[str, Any]] = []
    has_more_prs = True
    for page_number in range(1, MAX_PR_PAGES + 1):
        variables: dict[str, str | int] = {"owner": owner, "repo": repo}
        if cursor is not None:
            variables["cursor"] = cursor
        payload = graphql(MERGED_PR_QUERY, variables)
        raw_payloads.append(
            (
                f"merged-prs-{page_number:04d}.json",
                {
                    "query": "merged-prs",
                    "variables": variables,
                    "response": payload,
                },
            )
        )
        repository = repository_from_payload(payload, "merged-PR query")
        observed_branch = branch_identity(repository, "merged-PR query")
        if expected_branch is None:
            expected_branch = observed_branch
        elif observed_branch != expected_branch:
            raise CaptureError("default branch changed during merged-PR pagination")
        connection = repository.get("pullRequests")
        if not isinstance(connection, dict):
            raise CaptureError("merged-PR query omitted pullRequests")
        nodes = connection.get("nodes")
        if not isinstance(nodes, list) or not nodes:
            raise CaptureError("merged-PR query returned an empty page before closure")
        for node in nodes:
            pull_request, connections = normalize_pr(node)
            if pull_request["number"] in seen_pr_numbers:
                raise CaptureError(f"GitHub repeated PR {pull_request['number']}")
            seen_pr_numbers.add(pull_request["number"])
            thread_connection = connections["threads"]
            comment_connections = connections["comments"]
            for thread in pull_request["threads"]:
                if thread["id"] in seen_thread_ids:
                    raise CaptureError(f"GitHub repeated review thread {thread['id']}")
                seen_thread_ids.add(thread["id"])
            thread_pages = capture_supplemental_threads(
                owner,
                repo,
                pull_request,
                thread_connection,
                expected_branch,
                comment_connections,
                raw_payloads,
                seen_thread_ids,
            )
            thread_page_counts[str(pull_request["number"])] = thread_pages
            for thread in pull_request["threads"]:
                if not thread["is_resolved"]:
                    comment_page_counts[thread["id"]] = capture_supplemental_comments(
                        thread,
                        comment_connections[thread["id"]],
                        raw_payloads,
                    )
            pull_requests.append(pull_request)
        unresolved = unresolved_rows(pull_requests)
        has_more_prs, cursor = page_info(connection, "merged-PR query")
        if denominator_is_bounded(pull_requests, unresolved, batch_size):
            selected = unresolved[:batch_size]
            break
        if not has_more_prs:
            raise CaptureError(
                f"repository contains only {len(unresolved)} unresolved merged-PR threads"
            )
    else:
        raise CaptureError("merged-PR pagination exceeds its bound")
    if expected_branch is None or len(selected) != batch_size:
        raise CaptureError("capture did not establish the requested denominator")
    if observe_default_branch(owner, repo) != expected_branch:
        raise CaptureError("default branch changed before capture completed")
    scanned_unresolved_thread_count = len(unresolved_rows(pull_requests))
    selected_ids = {row["thread_id"] for row in selected}
    for pull_request in pull_requests:
        pull_request["threads"] = [
            thread for thread in pull_request["threads"] if thread["id"] in selected_ids
        ]
    selected_payload = [
        {
            "rank": rank,
            "pr": row["pr"],
            "thread": row["thread"],
            "thread_created_at": row["thread_created_at"],
        }
        for rank, row in enumerate(selected, start=1)
    ]
    last_pr = pull_requests[-1]
    rank_cutoff = selected[-1]
    manifest = {
        "schema": CAPTURE_SCHEMA,
        "batch_id": batch_id,
        "batch_size": batch_size,
        "owner": owner,
        "repository": repo,
        "observed_at": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "default_branch": expected_branch[0],
        "default_branch_oid": expected_branch[1],
        "merged_pr_page_count": sum(
            name.startswith("merged-prs-") for name, _payload in raw_payloads
        ),
        "scanned_pr_count": len(pull_requests),
        "scanned_review_thread_count": len(seen_thread_ids),
        "scanned_unresolved_thread_count": scanned_unresolved_thread_count,
        "rank_1_thread_id": selected[0]["thread_id"],
        "rank_1_created_at": selected[0]["thread_created_at"],
        "rank_cutoff_thread_id": rank_cutoff["thread_id"],
        "rank_cutoff_created_at": rank_cutoff["thread_created_at"],
        "last_scanned_pr_number": last_pr["number"],
        "last_scanned_pr_created_at": last_pr["created_at"],
        "unscanned_prs_remain": has_more_prs,
        "chronological_stop_proof": (
            "Pull requests are ordered by createdAt ascending; a review comment cannot "
            "predate its pull request; last_scanned_pr_created_at is later than "
            "rank_cutoff_created_at."
        ),
        "thread_page_counts": thread_page_counts,
        "comment_page_counts": comment_page_counts,
    }
    return raw_payloads, selected_payload, manifest


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as error:
        raise CaptureError(f"cannot read JSON from {path}: {error}") from error


def retained_files(input_dir: Path) -> dict[str, Path]:
    return {
        str(path.relative_to(input_dir)): path
        for path in input_dir.rglob("*")
        if path.is_file() and path.name != "manifest.json"
    }


def validate_file_manifest(input_dir: Path, manifest: dict[str, Any]) -> None:
    declared = manifest.get("files")
    if not isinstance(declared, dict) or not declared:
        raise CaptureError("manifest lacks retained file hashes")
    actual = retained_files(input_dir)
    if set(declared) != set(actual):
        raise CaptureError(
            "retained file membership differs from manifest: "
            f"missing={sorted(set(declared) - set(actual))}, "
            f"extra={sorted(set(actual) - set(declared))}"
        )
    for relative_path, path in actual.items():
        expected_hash = declared.get(relative_path)
        if not isinstance(expected_hash, str) or len(expected_hash) != 64:
            raise CaptureError(f"manifest has invalid hash for {relative_path}")
        actual_hash = sha256_bytes(path.read_bytes())
        if actual_hash != expected_hash:
            raise CaptureError(f"retained hash differs for {relative_path}")


def validate_query_files(input_dir: Path) -> None:
    expected_queries = {
        "merged-prs.graphql": MERGED_PR_QUERY,
        "pr-review-threads.graphql": PR_THREADS_QUERY,
        "thread-comments.graphql": THREAD_COMMENTS_QUERY,
        "default-branch.graphql": DEFAULT_BRANCH_QUERY,
    }
    for relative_path, expected_query in expected_queries.items():
        path = input_dir / relative_path
        if path.read_text(encoding="utf-8") != expected_query:
            raise CaptureError(
                f"retained query differs from generator: {relative_path}"
            )


def raw_wrapper(
    path: Path, expected_query: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    wrapper = read_json(path)
    if not isinstance(wrapper, dict) or wrapper.get("query") != expected_query:
        raise CaptureError(f"{path} has an invalid query declaration")
    variables = wrapper.get("variables")
    response = wrapper.get("response")
    if not isinstance(variables, dict) or not isinstance(response, dict):
        raise CaptureError(f"{path} has an invalid request or response")
    return variables, response


def validate_selected(
    selected: Any,
    batch_size: int,
    reconstructed_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    if not isinstance(selected, list) or len(selected) != batch_size:
        raise CaptureError(
            f"selected capture does not contain exactly {batch_size} rows"
        )
    if len(reconstructed_rows) < batch_size:
        raise CaptureError("raw capture lacks the declared denominator")
    selected_ids: set[str] = set()
    chronological_keys: list[tuple[datetime, str]] = []
    for expected_rank, (selected_row, raw_row) in enumerate(
        zip(selected, reconstructed_rows[:batch_size], strict=True), start=1
    ):
        if (
            not isinstance(selected_row, dict)
            or selected_row.get("rank") != expected_rank
        ):
            raise CaptureError("selected capture ranks are not contiguous")
        thread = selected_row.get("thread")
        pull_request = selected_row.get("pr")
        if not isinstance(thread, dict) or not isinstance(pull_request, dict):
            raise CaptureError(f"selected rank {expected_rank} is malformed")
        thread_id = thread.get("id")
        if not isinstance(thread_id, str) or thread_id in selected_ids:
            raise CaptureError(
                f"selected rank {expected_rank} has a duplicate thread ID"
            )
        selected_ids.add(thread_id)
        if thread_id != raw_row["thread_id"]:
            raise CaptureError(
                f"selected rank {expected_rank} differs from raw membership"
            )
        if pull_request.get("number") != raw_row["pr"]["number"]:
            raise CaptureError(
                f"selected rank {expected_rank} differs from raw PR identity"
            )
        if thread != raw_row["thread"]:
            raise CaptureError(
                f"selected rank {expected_rank} differs from raw thread state"
            )
        created_at = selected_row.get("thread_created_at")
        if created_at != raw_row["thread_created_at"]:
            raise CaptureError(
                f"selected rank {expected_rank} differs from raw chronology"
            )
        chronological_keys.append(
            (parse_timestamp(created_at, f"selected rank {expected_rank}"), thread_id)
        )
    if chronological_keys != sorted(chronological_keys):
        raise CaptureError("selected capture is not chronologically ordered")
    return selected


def validate_frontier_file(input_dir: Path, selected: list[dict[str, Any]]) -> None:
    frontier_path = input_dir / "frontier.tsv"
    with frontier_path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        if tuple(reader.fieldnames or ()) != FRONTIER_FIELDS:
            raise CaptureError("frontier header differs from its schema")
        actual_rows = list(reader)
    expected_rows = build_frontier_rows(selected)
    if actual_rows != expected_rows:
        raise CaptureError("frontier rows differ from selected-thread capture")


def reconstruct_raw_capture(
    input_dir: Path,
    manifest: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], bool]:
    primary_paths = sorted((input_dir / "raw").glob("merged-prs-*.json"))
    expected_page_count = manifest.get("merged_pr_page_count")
    if len(primary_paths) != expected_page_count:
        raise CaptureError("merged-PR raw page count differs from manifest")
    owner = manifest.get("owner")
    repository_name = manifest.get("repository")
    expected_branch = (
        manifest.get("default_branch"),
        manifest.get("default_branch_oid"),
    )
    pull_requests: list[dict[str, Any]] = []
    pull_requests_by_number: dict[int, dict[str, Any]] = {}
    thread_connections: dict[int, dict[str, Any]] = {}
    comment_connections: dict[str, dict[str, Any]] = {}
    threads_by_id: dict[str, dict[str, Any]] = {}
    previous_cursor: str | None = None
    has_more_prs = True
    for page_number, path in enumerate(primary_paths, start=1):
        variables, response = raw_wrapper(path, "merged-prs")
        expected_variables: dict[str, Any] = {"owner": owner, "repo": repository_name}
        if previous_cursor is not None:
            expected_variables["cursor"] = previous_cursor
        if variables != expected_variables:
            raise CaptureError(
                f"merged-PR page {page_number} request cursor is discontinuous"
            )
        repository = repository_from_payload(response, f"merged-PR page {page_number}")
        if (
            branch_identity(repository, f"merged-PR page {page_number}")
            != expected_branch
        ):
            raise CaptureError("raw page default-branch identity differs from manifest")
        connection = repository.get("pullRequests")
        if not isinstance(connection, dict):
            raise CaptureError(f"merged-PR page {page_number} omitted pull requests")
        nodes = connection.get("nodes")
        if not isinstance(nodes, list) or not nodes:
            raise CaptureError(f"merged-PR page {page_number} is empty")
        for node in nodes:
            pull_request, connections = normalize_pr(node)
            number = pull_request["number"]
            if number in pull_requests_by_number:
                raise CaptureError(f"raw capture repeats PR {number}")
            pull_requests.append(pull_request)
            pull_requests_by_number[number] = pull_request
            thread_connections[number] = connections["threads"]
            for thread in pull_request["threads"]:
                thread_id = thread["id"]
                if thread_id in threads_by_id:
                    raise CaptureError(f"raw capture repeats review thread {thread_id}")
                threads_by_id[thread_id] = thread
                comment_connections[thread_id] = connections["comments"][thread_id]
        has_more_prs, previous_cursor = page_info(
            connection, f"merged-PR page {page_number}"
        )

    supplemental_thread_paths = sorted((input_dir / "raw").glob("pr-*-threads-*.json"))
    thread_page_counts = {str(number): 1 for number in pull_requests_by_number}
    for path in supplemental_thread_paths:
        variables, response = raw_wrapper(path, "pr-review-threads")
        number = variables.get("number")
        if not isinstance(number, int) or number not in pull_requests_by_number:
            raise CaptureError(f"{path} names an unknown pull request")
        has_next, expected_cursor = page_info(
            thread_connections[number], f"PR {number} review threads"
        )
        if not has_next or variables.get("cursor") != expected_cursor:
            raise CaptureError(f"PR {number} thread pagination is discontinuous")
        repository = repository_from_payload(response, f"PR {number} thread page")
        if branch_identity(repository, f"PR {number} thread page") != expected_branch:
            raise CaptureError(f"PR {number} thread page changed default branch")
        pull_request_node = repository.get("pullRequest")
        connection = (
            pull_request_node.get("reviewThreads")
            if isinstance(pull_request_node, dict)
            else None
        )
        if not isinstance(connection, dict):
            raise CaptureError(f"PR {number} thread page omitted review threads")
        nodes = connection.get("nodes")
        if not isinstance(nodes, list):
            raise CaptureError(f"PR {number} thread page omitted nodes")
        for node in nodes:
            thread, comments = normalize_thread(node, number)
            thread_id = thread["id"]
            if thread_id in threads_by_id:
                raise CaptureError(f"raw capture repeats review thread {thread_id}")
            threads_by_id[thread_id] = thread
            pull_requests_by_number[number]["threads"].append(thread)
            comment_connections[thread_id] = comments
        thread_connections[number] = connection
        thread_page_counts[str(number)] += 1
    for number, connection in thread_connections.items():
        has_next, _cursor = page_info(connection, f"PR {number} final thread page")
        if has_next:
            raise CaptureError(f"PR {number} thread pagination is incomplete")

    comment_page_counts: dict[str, int] = {}
    for thread_id, connection in comment_connections.items():
        if not threads_by_id[thread_id]["is_resolved"]:
            comment_page_counts[thread_id] = 1
    supplemental_comment_paths = sorted(
        (input_dir / "raw").glob("thread-*-comments-*.json")
    )
    for path in supplemental_comment_paths:
        variables, response = raw_wrapper(path, "thread-comments")
        thread_id = variables.get("threadId")
        if not isinstance(thread_id, str) or thread_id not in comment_connections:
            raise CaptureError(f"{path} names an unknown review thread")
        has_next, expected_cursor = page_info(
            comment_connections[thread_id], f"{thread_id} comments"
        )
        if not has_next or variables.get("cursor") != expected_cursor:
            raise CaptureError(f"{thread_id} comment pagination is discontinuous")
        node = response.get("data", {}).get("node")
        if not isinstance(node, dict) or node.get("id") != thread_id:
            raise CaptureError(f"{path} response omitted {thread_id}")
        connection = node.get("comments")
        if not isinstance(connection, dict):
            raise CaptureError(f"{path} response omitted comments")
        seen_comment_ids = {
            comment["id"] for comment in threads_by_id[thread_id]["comments"]
        }
        append_comment_page(threads_by_id[thread_id], connection, seen_comment_ids)
        comment_connections[thread_id] = connection
        comment_page_counts[thread_id] += 1
    for thread_id in comment_page_counts:
        has_next, _cursor = page_info(
            comment_connections[thread_id], f"{thread_id} final comment page"
        )
        if has_next:
            raise CaptureError(f"{thread_id} comment pagination is incomplete")

    if thread_page_counts != manifest.get("thread_page_counts"):
        raise CaptureError("thread page counts differ from manifest")
    if comment_page_counts != manifest.get("comment_page_counts"):
        raise CaptureError("comment page counts differ from manifest")
    created_keys = [
        (
            parse_timestamp(pull_request["created_at"], "raw PR createdAt"),
            pull_request["number"],
        )
        for pull_request in pull_requests
    ]
    if created_keys != sorted(created_keys):
        raise CaptureError("raw pull requests are not ordered by creation time")
    if len(pull_requests) != manifest.get("scanned_pr_count"):
        raise CaptureError("scanned PR count differs from manifest")
    if len(threads_by_id) != manifest.get("scanned_review_thread_count"):
        raise CaptureError("scanned review-thread count differs from manifest")
    reconstructed_rows = unresolved_rows(pull_requests)
    if len(reconstructed_rows) != manifest.get("scanned_unresolved_thread_count"):
        raise CaptureError("scanned unresolved-thread count differs from manifest")
    return pull_requests, reconstructed_rows, has_more_prs


def check_capture(input_dir: Path) -> None:
    manifest_path = input_dir / "manifest.json"
    manifest = read_json(manifest_path)
    if not isinstance(manifest, dict) or manifest.get("schema") != CAPTURE_SCHEMA:
        raise CaptureError("manifest has an unexpected schema")
    batch_size = manifest.get("batch_size")
    if not isinstance(batch_size, int) or batch_size <= 0:
        raise CaptureError("manifest has an invalid batch size")
    validate_file_manifest(input_dir, manifest)
    validate_query_files(input_dir)
    pull_requests, reconstructed_rows, has_more_prs = reconstruct_raw_capture(
        input_dir, manifest
    )
    selected = validate_selected(
        read_json(input_dir / "selected-threads.json"),
        batch_size,
        reconstructed_rows,
    )
    validate_frontier_file(input_dir, selected)
    first_row = reconstructed_rows[0]
    cutoff_row = reconstructed_rows[batch_size - 1]
    if (
        manifest.get("rank_1_thread_id") != first_row["thread_id"]
        or manifest.get("rank_1_created_at") != first_row["thread_created_at"]
        or manifest.get("rank_cutoff_thread_id") != cutoff_row["thread_id"]
        or manifest.get("rank_cutoff_created_at") != cutoff_row["thread_created_at"]
    ):
        raise CaptureError("manifest rank witnesses differ from raw capture")
    last_pull_request = pull_requests[-1]
    if manifest.get("last_scanned_pr_number") != last_pull_request["number"]:
        raise CaptureError("manifest last scanned PR differs from raw capture")
    if manifest.get("last_scanned_pr_created_at") != last_pull_request["created_at"]:
        raise CaptureError(
            "manifest last scanned PR timestamp differs from raw capture"
        )
    if manifest.get("unscanned_prs_remain") is not has_more_prs:
        raise CaptureError("manifest unscanned-PR state differs from raw capture")
    if not denominator_is_bounded(
        pull_requests,
        reconstructed_rows,
        batch_size,
    ):
        raise CaptureError("chronological stop proof is not satisfied")


def main() -> int:
    arguments = parse_args()
    try:
        if arguments.command == "check":
            check_capture(arguments.input_dir)
            print(f"OK: verified retained review-thread capture {arguments.input_dir}")
            return 0
        raw_payloads, selected, manifest = capture(
            arguments.owner,
            arguments.repo,
            arguments.batch_id,
            arguments.batch_size,
        )
        write_capture(
            arguments.output_dir,
            {
                "merged-prs": MERGED_PR_QUERY,
                "pr-review-threads": PR_THREADS_QUERY,
                "thread-comments": THREAD_COMMENTS_QUERY,
                "default-branch": DEFAULT_BRANCH_QUERY,
            },
            raw_payloads,
            selected,
            manifest,
        )
    except (CaptureError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"OK: captured {manifest['batch_size']} oldest unresolved threads at "
        f"{manifest['default_branch_oid']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
