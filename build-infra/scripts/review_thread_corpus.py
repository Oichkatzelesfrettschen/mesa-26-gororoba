#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture and verify the complete unresolved review-thread corpus."""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import re
import subprocess
import sys
from collections import defaultdict
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

CORPUS_SCHEMA = "mesa-unresolved-review-thread-corpus-v1"
MAX_PULL_REQUEST_PAGES = 100
MAX_THREAD_PAGES_PER_PULL_REQUEST = 20
MAX_COMMENT_PAGES_PER_THREAD = 20
COMMENTS_PER_PRIMARY_THREAD = 10

THREAD_FIELDS = (
    "corpus_rank",
    "thread_id",
    "pull_request_number",
    "pull_request_state",
    "pull_request_title",
    "pull_request_created_at",
    "pull_request_merged_at",
    "pull_request_merge_commit",
    "thread_created_at",
    "is_outdated",
    "review_path",
    "line",
    "original_line",
    "review_authors",
    "comment_count",
    "first_comment_url",
    "first_comment_sha256",
    "review_commit_oid",
    "original_commit_oid",
    "diff_hunk_sha256",
    "source_anchor",
    "claim_heading",
    "claim_fingerprint",
)

GROUP_MEMBER_FIELDS = (
    "corpus_rank",
    "thread_id",
    "path_group_id",
    "source_group_id",
    "claim_group_id",
    "work_group_id",
)

WORK_GROUP_FIELDS = (
    "work_group_id",
    "review_path",
    "source_anchor",
    "thread_count",
    "claim_group_count",
    "pull_request_count",
    "merged_pull_request_threads",
    "closed_pull_request_threads",
    "open_pull_request_threads",
    "current_threads",
    "outdated_threads",
    "oldest_thread_created_at",
    "newest_thread_created_at",
    "representative_thread_id",
    "classification_state",
    "next_action",
)

PULL_REQUEST_QUERY = r"""
query($owner:String!, $repo:String!, $cursor:String) {
  repository(owner:$owner, name:$repo) {
    defaultBranchRef { name target { ... on Commit { oid } } }
    pullRequests(
      first:100
      after:$cursor
      states:[OPEN,CLOSED,MERGED]
      orderBy:{field:CREATED_AT,direction:ASC}
    ) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        title
        url
        state
        createdAt
        mergedAt
        mergeCommit { oid }
        closedAt
        reviewThreads(first:100) {
          pageInfo { hasNextPage endCursor }
          nodes {
            id
            isResolved
            isOutdated
            path
            line
            originalLine
            comments(first:10) {
              totalCount
              pageInfo { hasNextPage endCursor }
              nodes {
                id
                databaseId
                author { login }
                body
                createdAt
                updatedAt
                url
                diffHunk
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

PULL_REQUEST_THREADS_QUERY = r"""
query($owner:String!, $repo:String!, $number:Int!, $cursor:String!) {
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
          comments(first:10) {
            totalCount
            pageInfo { hasNextPage endCursor }
            nodes {
              id
              databaseId
              author { login }
              body
              createdAt
              updatedAt
              url
              diffHunk
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
query($threadId:ID!, $cursor:String!) {
  node(id:$threadId) {
    ... on PullRequestReviewThread {
      id
      comments(first:100, after:$cursor) {
        totalCount
        pageInfo { hasNextPage endCursor }
        nodes {
          id
          databaseId
          author { login }
          body
          createdAt
          updatedAt
          url
          diffHunk
          commit { oid }
          originalCommit { oid }
        }
      }
    }
  }
}
"""

MEMBERSHIP_QUERY = r"""
query($owner:String!, $repo:String!, $cursor:String) {
  repository(owner:$owner, name:$repo) {
    defaultBranchRef { name target { ... on Commit { oid } } }
    pullRequests(
      first:100
      after:$cursor
      states:[OPEN,CLOSED,MERGED]
      orderBy:{field:CREATED_AT,direction:ASC}
    ) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        state
        reviewThreads(first:100) {
          pageInfo { hasNextPage endCursor }
          nodes { id isResolved isOutdated path }
        }
      }
    }
  }
}
"""

MEMBERSHIP_THREADS_QUERY = r"""
query($owner:String!, $repo:String!, $number:Int!, $cursor:String!) {
  repository(owner:$owner, name:$repo) {
    defaultBranchRef { name target { ... on Commit { oid } } }
    pullRequest(number:$number) {
      state
      reviewThreads(first:100, after:$cursor) {
        pageInfo { hasNextPage endCursor }
        nodes { id isResolved isOutdated path }
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

QUERY_FILES = {
    "pull-requests": PULL_REQUEST_QUERY,
    "pull-request-threads": PULL_REQUEST_THREADS_QUERY,
    "thread-comments": THREAD_COMMENTS_QUERY,
    "membership": MEMBERSHIP_QUERY,
    "membership-threads": MEMBERSHIP_THREADS_QUERY,
    "default-branch": DEFAULT_BRANCH_QUERY,
}

MARKDOWN_IMAGE = re.compile(r"!\[[^]]*\]\([^)]*\)")
MARKDOWN_LINK = re.compile(r"\[([^]]+)\]\([^)]*\)")
HTML_TAG = re.compile(r"<[^>]+>")
TOKEN = re.compile(r"[a-z0-9]+")
THREAD_ID = re.compile(r"^PRRT_[A-Za-z0-9_-]+$")
COMMIT_OID = re.compile(r"^[0-9a-f]{40}$")
RETIRED_NAME_TOKEN = "".join(("goro", "roba"))
RETIRED_NAME_PATTERN = re.compile(re.escape(RETIRED_NAME_TOKEN), re.IGNORECASE)
RETIRED_QUALIFICATION_PATTERN = re.compile(r"\bdecision[-_\s]+grade\b", re.IGNORECASE)


class CorpusError(ValueError):
    """The retained corpus or its exact membership proof is invalid."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    capture_parser = subparsers.add_parser("capture")
    capture_parser.add_argument("--owner", default="Oichkatzelesfrettschen")
    capture_parser.add_argument("--repo", default="mesa-26-gororoba")
    capture_parser.add_argument("--corpus-id", required=True)
    capture_parser.add_argument("--output-dir", type=Path, required=True)
    check_parser = subparsers.add_parser("check")
    check_parser.add_argument("--input-dir", type=Path, required=True)
    compress_parser = subparsers.add_parser("compress")
    compress_parser.add_argument("--input-dir", type=Path, required=True)
    return parser.parse_args()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def json_bytes(payload: Any) -> bytes:
    return (
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("utf-8")


def compressed_json_bytes(payload: Any) -> bytes:
    return gzip.compress(json_bytes(payload), compresslevel=9, mtime=0)


def parse_timestamp(value: Any, context: str) -> datetime:
    if not isinstance(value, str) or not value:
        raise CorpusError(f"{context}: timestamp is missing")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise CorpusError(f"{context}: invalid timestamp {value!r}") from error
    if parsed.tzinfo is None:
        raise CorpusError(f"{context}: timestamp lacks a timezone")
    return parsed


def graphql(query: str, variables: dict[str, str | int]) -> dict[str, Any]:
    command = ["gh", "api", "graphql", "-F", "query=@-"]
    for name, value in variables.items():
        command.extend(("-F", f"{name}={value}"))
    completed = subprocess.run(
        command,
        input=query,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = " ".join((completed.stderr or completed.stdout).split())
        raise CorpusError(f"GitHub GraphQL query failed: {diagnostic}")
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise CorpusError("GitHub GraphQL returned invalid JSON") from error
    if not isinstance(payload, dict):
        raise CorpusError("GitHub GraphQL returned a non-object")
    if payload.get("errors"):
        raise CorpusError(f"GitHub GraphQL returned errors: {payload['errors']}")
    return payload


def repository_from_payload(payload: dict[str, Any], context: str) -> dict[str, Any]:
    repository = payload.get("data", {}).get("repository")
    if not isinstance(repository, dict):
        raise CorpusError(f"{context}: repository is missing")
    return repository


def branch_identity(repository: dict[str, Any], context: str) -> tuple[str, str]:
    branch = repository.get("defaultBranchRef")
    target = branch.get("target") if isinstance(branch, dict) else None
    name = branch.get("name") if isinstance(branch, dict) else None
    oid = target.get("oid") if isinstance(target, dict) else None
    if not isinstance(name, str) or not isinstance(oid, str):
        raise CorpusError(f"{context}: default branch identity is missing")
    if not COMMIT_OID.fullmatch(oid):
        raise CorpusError(f"{context}: default branch commit is invalid")
    return name, oid


def page_info(connection: dict[str, Any], context: str) -> tuple[bool, str | None]:
    information = connection.get("pageInfo")
    if not isinstance(information, dict):
        raise CorpusError(f"{context}: pageInfo is missing")
    has_next_page = information.get("hasNextPage")
    end_cursor = information.get("endCursor")
    if not isinstance(has_next_page, bool):
        raise CorpusError(f"{context}: hasNextPage is invalid")
    if has_next_page and (not isinstance(end_cursor, str) or not end_cursor):
        raise CorpusError(f"{context}: endCursor is required")
    return has_next_page, end_cursor if isinstance(end_cursor, str) else None


def required_boolean(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        raise CorpusError(f"{context}: boolean value is missing")
    return value


def normalize_comment(comment: Any, thread_id: str) -> dict[str, Any]:
    if not isinstance(comment, dict):
        raise CorpusError(f"{thread_id}: malformed review comment")
    comment_id = comment.get("id")
    body = comment.get("body")
    created_at = comment.get("createdAt")
    url = comment.get("url")
    if not isinstance(comment_id, str) or not comment_id:
        raise CorpusError(f"{thread_id}: comment ID is missing")
    if not isinstance(body, str):
        raise CorpusError(f"{thread_id}: comment body is missing")
    parse_timestamp(created_at, f"{thread_id} comment {comment_id}")
    if not isinstance(url, str) or not url:
        raise CorpusError(f"{thread_id}: comment URL is missing")
    author = comment.get("author")
    commit = comment.get("commit")
    original_commit = comment.get("originalCommit")
    commit_oid = commit.get("oid") if isinstance(commit, dict) else None
    original_commit_oid = (
        original_commit.get("oid") if isinstance(original_commit, dict) else None
    )
    for label, oid in (
        ("commit", commit_oid),
        ("original commit", original_commit_oid),
    ):
        if oid is not None and (
            not isinstance(oid, str) or not COMMIT_OID.fullmatch(oid)
        ):
            raise CorpusError(f"{thread_id}: {label} OID is invalid")
    return {
        "id": comment_id,
        "database_id": comment.get("databaseId"),
        "author": (
            author.get("login")
            if isinstance(author, dict) and isinstance(author.get("login"), str)
            else "none"
        ),
        "body": body,
        "created_at": created_at,
        "updated_at": comment.get("updatedAt"),
        "url": url,
        "diff_hunk": comment.get("diffHunk") or "",
        "commit_oid": commit_oid,
        "original_commit_oid": original_commit_oid,
    }


def append_comments(
    thread: dict[str, Any],
    connection: dict[str, Any],
    seen_comment_ids: set[str],
) -> None:
    nodes = connection.get("nodes")
    if not isinstance(nodes, list):
        raise CorpusError(f"{thread['id']}: comment nodes are missing")
    for node in nodes:
        comment = normalize_comment(node, thread["id"])
        if comment["id"] in seen_comment_ids:
            raise CorpusError(f"{thread['id']}: duplicate comment {comment['id']}")
        seen_comment_ids.add(comment["id"])
        thread["comments"].append(comment)


def normalize_thread(
    node: Any, pull_request_number: int
) -> tuple[dict[str, Any], dict[str, Any]]:
    if not isinstance(node, dict):
        raise CorpusError(
            f"pull request {pull_request_number}: malformed review thread"
        )
    thread_id = node.get("id")
    path = node.get("path")
    if not isinstance(thread_id, str) or not THREAD_ID.fullmatch(thread_id):
        raise CorpusError(f"pull request {pull_request_number}: invalid thread ID")
    if not isinstance(path, str) or not path:
        raise CorpusError(f"{thread_id}: review path is missing")
    comments = node.get("comments")
    if not isinstance(comments, dict):
        raise CorpusError(f"{thread_id}: comments connection is missing")
    total_count = comments.get("totalCount")
    if (
        not isinstance(total_count, int)
        or isinstance(total_count, bool)
        or total_count <= 0
    ):
        raise CorpusError(f"{thread_id}: comment totalCount is invalid")
    thread = {
        "id": thread_id,
        "is_resolved": required_boolean(node.get("isResolved"), thread_id),
        "is_outdated": required_boolean(node.get("isOutdated"), thread_id),
        "path": path,
        "line": node.get("line"),
        "original_line": node.get("originalLine"),
        "comment_total_count": total_count,
        "comments": [],
    }
    for field in ("line", "original_line"):
        value = thread[field]
        if value is not None and (
            not isinstance(value, int) or isinstance(value, bool) or value <= 0
        ):
            raise CorpusError(f"{thread_id}: {field} is invalid")
    append_comments(thread, comments, set())
    if not thread["comments"]:
        raise CorpusError(f"{thread_id}: review thread has no comments")
    return thread, comments


def normalize_pull_request(
    node: Any,
) -> tuple[dict[str, Any], dict[str, Any]]:
    if not isinstance(node, dict):
        raise CorpusError("pull-request page contains a malformed node")
    number = node.get("number")
    title = node.get("title")
    url = node.get("url")
    state = node.get("state")
    if not isinstance(number, int) or isinstance(number, bool) or number <= 0:
        raise CorpusError("pull-request page contains an invalid number")
    if not isinstance(title, str) or not isinstance(url, str):
        raise CorpusError(f"pull request {number}: title or URL is missing")
    if state not in ("MERGED", "CLOSED", "OPEN"):
        raise CorpusError(f"pull request {number}: state is invalid")
    parse_timestamp(node.get("createdAt"), f"pull request {number} createdAt")
    merged_at = node.get("mergedAt")
    merge_commit = node.get("mergeCommit")
    merge_commit_oid = (
        merge_commit.get("oid") if isinstance(merge_commit, dict) else None
    )
    if state == "MERGED":
        parse_timestamp(merged_at, f"pull request {number} mergedAt")
        if not isinstance(merge_commit_oid, str) or not COMMIT_OID.fullmatch(
            merge_commit_oid
        ):
            raise CorpusError(f"pull request {number}: merge commit is invalid")
    elif merged_at is not None:
        raise CorpusError(f"pull request {number}: unmerged state carries mergedAt")
    elif merge_commit_oid is not None:
        raise CorpusError(f"pull request {number}: unmerged state carries merge commit")
    thread_connection = node.get("reviewThreads")
    if not isinstance(thread_connection, dict):
        raise CorpusError(f"pull request {number}: reviewThreads is missing")
    thread_nodes = thread_connection.get("nodes")
    if not isinstance(thread_nodes, list):
        raise CorpusError(f"pull request {number}: review-thread nodes are missing")
    pull_request = {
        "number": number,
        "title": title,
        "url": url,
        "state": state,
        "created_at": node.get("createdAt"),
        "merged_at": merged_at,
        "merge_commit_oid": merge_commit_oid,
        "closed_at": node.get("closedAt"),
        "threads": [],
    }
    comment_connections: dict[str, Any] = {}
    for thread_node in thread_nodes:
        thread, comments = normalize_thread(thread_node, number)
        pull_request["threads"].append(thread)
        comment_connections[thread["id"]] = comments
    return pull_request, {
        "threads": thread_connection,
        "comments": comment_connections,
    }


def observe_default_branch(owner: str, repo: str) -> tuple[str, str]:
    payload = graphql(DEFAULT_BRANCH_QUERY, {"owner": owner, "repo": repo})
    repository = repository_from_payload(payload, "default-branch query")
    return branch_identity(repository, "default-branch query")


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
        initial_connection, f"pull request {pull_request['number']} review threads"
    )
    page_number = 1
    while has_next_page:
        page_number += 1
        if page_number > MAX_THREAD_PAGES_PER_PULL_REQUEST:
            raise CorpusError(
                f"pull request {pull_request['number']}: thread pages exceed bound"
            )
        payload = graphql(
            PULL_REQUEST_THREADS_QUERY,
            {
                "owner": owner,
                "repo": repo,
                "number": pull_request["number"],
                "cursor": cursor or "",
            },
        )
        raw_payloads.append(
            (
                f"pull-request-{pull_request['number']}-threads-{page_number:04d}.json",
                {
                    "query": "pull-request-threads",
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
            raise CorpusError("default branch changed during thread pagination")
        pull_request_node = repository.get("pullRequest")
        connection = (
            pull_request_node.get("reviewThreads")
            if isinstance(pull_request_node, dict)
            else None
        )
        if not isinstance(connection, dict):
            raise CorpusError(
                f"pull request {pull_request['number']}: supplemental threads missing"
            )
        nodes = connection.get("nodes")
        if not isinstance(nodes, list):
            raise CorpusError(
                f"pull request {pull_request['number']}: supplemental nodes missing"
            )
        for node in nodes:
            thread, comments = normalize_thread(node, pull_request["number"])
            if thread["id"] in seen_thread_ids:
                raise CorpusError(f"duplicate review thread {thread['id']}")
            seen_thread_ids.add(thread["id"])
            pull_request["threads"].append(thread)
            comment_connections[thread["id"]] = comments
        has_next_page, cursor = page_info(
            connection, f"pull request {pull_request['number']} review threads"
        )
    return page_number


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
            raise CorpusError(f"{thread['id']}: comment pages exceed bound")
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
            raise CorpusError(f"supplemental comment query omitted {thread['id']}")
        connection = node.get("comments")
        if not isinstance(connection, dict):
            raise CorpusError(f"supplemental comments omitted {thread['id']}")
        append_comments(thread, connection, seen_comment_ids)
        has_next_page, cursor = page_info(connection, f"{thread['id']} comments")
    if len(thread["comments"]) != thread["comment_total_count"]:
        raise CorpusError(f"{thread['id']}: comment count differs after pagination")
    return page_number


def capture_detailed_corpus(owner: str, repo: str) -> tuple[
    list[dict[str, Any]],
    list[tuple[str, dict[str, Any]]],
    tuple[str, str],
    dict[str, int],
    dict[str, int],
]:
    pull_requests: list[dict[str, Any]] = []
    raw_payloads: list[tuple[str, dict[str, Any]]] = []
    seen_pull_request_numbers: set[int] = set()
    seen_thread_ids: set[str] = set()
    thread_page_counts: dict[str, int] = {}
    comment_page_counts: dict[str, int] = {}
    expected_branch: tuple[str, str] | None = None
    cursor: str | None = None
    for page_number in range(1, MAX_PULL_REQUEST_PAGES + 1):
        variables: dict[str, str | int] = {"owner": owner, "repo": repo}
        if cursor is not None:
            variables["cursor"] = cursor
        payload = graphql(PULL_REQUEST_QUERY, variables)
        raw_payloads.append(
            (
                f"pull-requests-{page_number:04d}.json",
                {
                    "query": "pull-requests",
                    "variables": variables,
                    "response": payload,
                },
            )
        )
        repository = repository_from_payload(payload, "pull-request query")
        observed_branch = branch_identity(repository, "pull-request query")
        if expected_branch is None:
            expected_branch = observed_branch
        elif observed_branch != expected_branch:
            raise CorpusError("default branch changed during pull-request pagination")
        connection = repository.get("pullRequests")
        if not isinstance(connection, dict):
            raise CorpusError("pull-request connection is missing")
        nodes = connection.get("nodes")
        if not isinstance(nodes, list) or not nodes:
            raise CorpusError("pull-request query returned an unexpected empty page")
        for node in nodes:
            pull_request, connections = normalize_pull_request(node)
            number = pull_request["number"]
            if number in seen_pull_request_numbers:
                raise CorpusError(f"duplicate pull request {number}")
            seen_pull_request_numbers.add(number)
            for thread in pull_request["threads"]:
                if thread["id"] in seen_thread_ids:
                    raise CorpusError(f"duplicate review thread {thread['id']}")
                seen_thread_ids.add(thread["id"])
            thread_page_counts[str(number)] = capture_supplemental_threads(
                owner,
                repo,
                pull_request,
                connections["threads"],
                expected_branch,
                connections["comments"],
                raw_payloads,
                seen_thread_ids,
            )
            for thread in pull_request["threads"]:
                if thread["is_resolved"]:
                    continue
                comment_page_counts[thread["id"]] = capture_supplemental_comments(
                    thread,
                    connections["comments"][thread["id"]],
                    raw_payloads,
                )
            pull_requests.append(pull_request)
        has_next_page, cursor = page_info(connection, "pull-request query")
        if not has_next_page:
            if expected_branch is None:
                raise CorpusError("capture omitted the default branch")
            return (
                pull_requests,
                raw_payloads,
                expected_branch,
                thread_page_counts,
                comment_page_counts,
            )
    raise CorpusError("pull-request pagination exceeds its bound")


def unresolved_records(
    pull_requests: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for pull_request in pull_requests:
        pull_request_identity = {
            key: value for key, value in pull_request.items() if key != "threads"
        }
        for thread in pull_request["threads"]:
            if thread["is_resolved"]:
                continue
            first_comment = thread["comments"][0]
            created_at = first_comment["created_at"]
            parse_timestamp(created_at, f"{thread['id']} first comment")
            records.append(
                {
                    "thread_id": thread["id"],
                    "thread_created_at": created_at,
                    "pull_request": pull_request_identity,
                    "thread": thread,
                }
            )
    return sorted(
        records,
        key=lambda record: (record["thread_created_at"], record["thread_id"]),
    )


def normalize_membership_thread(
    node: Any, pull_request_number: int, pull_request_state: str
) -> dict[str, Any]:
    if not isinstance(node, dict):
        raise CorpusError(
            f"membership pull request {pull_request_number}: malformed thread"
        )
    thread_id = node.get("id")
    path = node.get("path")
    if not isinstance(thread_id, str) or not THREAD_ID.fullmatch(thread_id):
        raise CorpusError(
            f"membership pull request {pull_request_number}: invalid thread ID"
        )
    if not isinstance(path, str) or not path:
        raise CorpusError(f"membership {thread_id}: path is missing")
    return {
        "thread_id": thread_id,
        "pull_request_number": pull_request_number,
        "pull_request_state": pull_request_state,
        "is_resolved": required_boolean(node.get("isResolved"), thread_id),
        "is_outdated": required_boolean(node.get("isOutdated"), thread_id),
        "review_path": path,
    }


def append_membership_threads(
    membership: dict[str, dict[str, Any]],
    seen_thread_ids: set[str],
    nodes: Any,
    pull_request_number: int,
    pull_request_state: str,
) -> None:
    if not isinstance(nodes, list):
        raise CorpusError(
            f"membership pull request {pull_request_number}: thread nodes missing"
        )
    for node in nodes:
        record = normalize_membership_thread(
            node, pull_request_number, pull_request_state
        )
        if record["thread_id"] in seen_thread_ids:
            raise CorpusError(f"membership repeats {record['thread_id']}")
        seen_thread_ids.add(record["thread_id"])
        if not record["is_resolved"]:
            membership[record["thread_id"]] = record


def capture_membership(
    owner: str,
    repo: str,
    expected_branch: tuple[str, str],
) -> tuple[
    dict[str, dict[str, Any]],
    list[tuple[str, dict[str, Any]]],
    dict[str, int],
]:
    membership: dict[str, dict[str, Any]] = {}
    seen_thread_ids: set[str] = set()
    raw_payloads: list[tuple[str, dict[str, Any]]] = []
    thread_page_counts: dict[str, int] = {}
    cursor: str | None = None
    for page_number in range(1, MAX_PULL_REQUEST_PAGES + 1):
        variables: dict[str, str | int] = {"owner": owner, "repo": repo}
        if cursor is not None:
            variables["cursor"] = cursor
        payload = graphql(MEMBERSHIP_QUERY, variables)
        raw_payloads.append(
            (
                f"membership-{page_number:04d}.json",
                {
                    "query": "membership",
                    "variables": variables,
                    "response": payload,
                },
            )
        )
        repository = repository_from_payload(payload, "membership query")
        if branch_identity(repository, "membership query") != expected_branch:
            raise CorpusError("default branch changed during membership verification")
        connection = repository.get("pullRequests")
        if not isinstance(connection, dict):
            raise CorpusError("membership pull-request connection is missing")
        nodes = connection.get("nodes")
        if not isinstance(nodes, list) or not nodes:
            raise CorpusError("membership query returned an unexpected empty page")
        for pull_request_node in nodes:
            if not isinstance(pull_request_node, dict):
                raise CorpusError("membership query returned malformed pull request")
            number = pull_request_node.get("number")
            state = pull_request_node.get("state")
            threads = pull_request_node.get("reviewThreads")
            if (
                not isinstance(number, int)
                or isinstance(number, bool)
                or number <= 0
                or state not in ("MERGED", "CLOSED", "OPEN")
                or not isinstance(threads, dict)
            ):
                raise CorpusError("membership pull-request identity is invalid")
            append_membership_threads(
                membership,
                seen_thread_ids,
                threads.get("nodes"),
                number,
                state,
            )
            has_more_threads, thread_cursor = page_info(
                threads, f"membership pull request {number}"
            )
            thread_page_number = 1
            while has_more_threads:
                thread_page_number += 1
                if thread_page_number > MAX_THREAD_PAGES_PER_PULL_REQUEST:
                    raise CorpusError(
                        f"membership pull request {number}: thread pages exceed bound"
                    )
                thread_payload = graphql(
                    MEMBERSHIP_THREADS_QUERY,
                    {
                        "owner": owner,
                        "repo": repo,
                        "number": number,
                        "cursor": thread_cursor or "",
                    },
                )
                raw_payloads.append(
                    (
                        f"membership-pull-request-{number}-threads-"
                        f"{thread_page_number:04d}.json",
                        {
                            "query": "membership-threads",
                            "variables": {
                                "owner": owner,
                                "repo": repo,
                                "number": number,
                                "cursor": thread_cursor,
                            },
                            "response": thread_payload,
                        },
                    )
                )
                thread_repository = repository_from_payload(
                    thread_payload, "membership supplemental thread query"
                )
                if (
                    branch_identity(
                        thread_repository, "membership supplemental thread query"
                    )
                    != expected_branch
                ):
                    raise CorpusError(
                        "default branch changed during membership thread pagination"
                    )
                thread_pull_request = thread_repository.get("pullRequest")
                if not isinstance(thread_pull_request, dict):
                    raise CorpusError(
                        f"membership pull request {number}: supplemental node missing"
                    )
                if thread_pull_request.get("state") != state:
                    raise CorpusError(
                        f"membership pull request {number}: state changed"
                    )
                thread_connection = thread_pull_request.get("reviewThreads")
                if not isinstance(thread_connection, dict):
                    raise CorpusError(
                        f"membership pull request {number}: threads missing"
                    )
                append_membership_threads(
                    membership,
                    seen_thread_ids,
                    thread_connection.get("nodes"),
                    number,
                    state,
                )
                has_more_threads, thread_cursor = page_info(
                    thread_connection, f"membership pull request {number}"
                )
            thread_page_counts[str(number)] = thread_page_number
        has_next_page, cursor = page_info(connection, "membership query")
        if not has_next_page:
            return membership, raw_payloads, thread_page_counts
    raise CorpusError("membership pull-request pagination exceeds its bound")


def detailed_membership(
    records: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    membership: dict[str, dict[str, Any]] = {}
    for record in records:
        thread = record["thread"]
        pull_request = record["pull_request"]
        thread_id = record["thread_id"]
        if thread_id in membership:
            raise CorpusError(f"detailed corpus repeats {thread_id}")
        membership[thread_id] = {
            "thread_id": thread_id,
            "pull_request_number": pull_request["number"],
            "pull_request_state": pull_request["state"],
            "is_resolved": False,
            "is_outdated": thread["is_outdated"],
            "review_path": thread["path"],
        }
    return membership


def validate_exact_membership(
    records: list[dict[str, Any]], membership: dict[str, dict[str, Any]]
) -> None:
    detailed = detailed_membership(records)
    if set(detailed) != set(membership):
        raise CorpusError(
            "detailed and verification membership differ: "
            f"missing={sorted(set(membership) - set(detailed))}, "
            f"extra={sorted(set(detailed) - set(membership))}"
        )
    for thread_id, record in detailed.items():
        if record != membership[thread_id]:
            raise CorpusError(f"membership state differs for {thread_id}")


def clean_claim_heading(body: str) -> str:
    for source_line in body.splitlines():
        line = MARKDOWN_IMAGE.sub("", source_line)
        line = MARKDOWN_LINK.sub(r"\1", line)
        line = HTML_TAG.sub("", line)
        line = line.replace("**", "").replace("__", "").replace("`", "")
        line = line.strip(" #*_-\t")
        if not line:
            continue
        lowered = line.lower()
        if lowered.startswith("agents.md reference:"):
            continue
        if lowered.startswith("useful? react with"):
            continue
        if lowered in ("p0 badge", "p1 badge", "p2 badge", "p3 badge"):
            continue
        return " ".join(line.split())[:240]
    return "Unlabeled review claim"


def normalized_tokens(value: str) -> tuple[str, ...]:
    return tuple(TOKEN.findall(value.lower()))


def durable_display_label(value: str, fallback: str) -> str:
    without_retired_name = RETIRED_NAME_PATTERN.sub("", value)
    current_wording = RETIRED_QUALIFICATION_PATTERN.sub(
        "verified result", without_retired_name
    )
    tokens = normalized_tokens(current_wording)
    if not tokens:
        return fallback
    return " ".join(tokens)[:240]


def stable_slug(value: str, fallback: str) -> str:
    tokens = normalized_tokens(durable_display_label(value, fallback))
    if not tokens:
        return fallback
    return "-".join(tokens[:10])[:72]


def stable_identifier(prefix: str, label: str, key: str) -> str:
    digest = sha256_bytes(key.encode("utf-8"))[:12]
    return f"{prefix}-{stable_slug(label, prefix)}-{digest}"


def source_anchor(thread: dict[str, Any]) -> str:
    diff_hunk = thread["comments"][0]["diff_hunk"]
    if diff_hunk:
        first_line = diff_hunk.splitlines()[0]
        if first_line.startswith("@@"):
            closing_marker = first_line.find("@@", 2)
            if closing_marker >= 0:
                declaration = first_line[closing_marker + 2 :].strip()
                if declaration:
                    return " ".join(declaration.split())[:240]
    if thread["original_line"] is not None:
        return f"original-line-{thread['original_line']}"
    if thread["line"] is not None:
        return f"line-{thread['line']}"
    return "path-scope"


def normalized_claim_body(body: str) -> str:
    retained_lines = []
    for source_line in body.splitlines():
        lowered = source_line.strip().lower()
        if lowered.startswith("agents.md reference:"):
            continue
        if lowered.startswith("useful? react with"):
            continue
        retained_lines.append(source_line)
    plain = "\n".join(retained_lines)
    plain = MARKDOWN_IMAGE.sub("", plain)
    plain = MARKDOWN_LINK.sub(r"\1", plain)
    plain = HTML_TAG.sub("", plain)
    return " ".join(normalized_tokens(plain))


def group_identities(record: dict[str, Any]) -> dict[str, str]:
    thread = record["thread"]
    path = thread["path"]
    anchor = source_anchor(thread)
    heading = clean_claim_heading(thread["comments"][0]["body"])
    display_heading = durable_display_label(heading, "unlabeled review claim")
    normalized_heading = " ".join(normalized_tokens(heading))
    path_group_id = stable_identifier("path", Path(path).name, path)
    source_key = f"{path}\0{anchor}"
    source_group_id = stable_identifier(
        "source", f"{Path(path).name}-{anchor}", source_key
    )
    claim_key = f"{path}\0{normalized_heading}"
    claim_group_id = stable_identifier(
        "claim", f"{Path(path).name}-{display_heading}", claim_key
    )
    return {
        "path_group_id": path_group_id,
        "source_group_id": source_group_id,
        "claim_group_id": claim_group_id,
        "work_group_id": source_group_id.replace("source-", "work-", 1),
    }


def build_thread_rows(records: list[dict[str, Any]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for rank, record in enumerate(records, start=1):
        thread = record["thread"]
        pull_request = record["pull_request"]
        first_comment = thread["comments"][0]
        heading = durable_display_label(
            clean_claim_heading(first_comment["body"]), "unlabeled review claim"
        )
        claim_body = normalized_claim_body(first_comment["body"])
        authors = sorted({comment["author"] for comment in thread["comments"]})
        rows.append(
            {
                "corpus_rank": str(rank),
                "thread_id": thread["id"],
                "pull_request_number": str(pull_request["number"]),
                "pull_request_state": pull_request["state"],
                "pull_request_title": pull_request["title"],
                "pull_request_created_at": pull_request["created_at"],
                "pull_request_merged_at": pull_request["merged_at"] or "",
                "pull_request_merge_commit": (pull_request["merge_commit_oid"] or ""),
                "thread_created_at": record["thread_created_at"],
                "is_outdated": str(thread["is_outdated"]).lower(),
                "review_path": thread["path"],
                "line": "" if thread["line"] is None else str(thread["line"]),
                "original_line": (
                    ""
                    if thread["original_line"] is None
                    else str(thread["original_line"])
                ),
                "review_authors": ";".join(authors),
                "comment_count": str(len(thread["comments"])),
                "first_comment_url": first_comment["url"],
                "first_comment_sha256": sha256_bytes(
                    first_comment["body"].encode("utf-8")
                ),
                "review_commit_oid": first_comment["commit_oid"] or "",
                "original_commit_oid": first_comment["original_commit_oid"] or "",
                "diff_hunk_sha256": sha256_bytes(
                    first_comment["diff_hunk"].encode("utf-8")
                ),
                "source_anchor": durable_display_label(
                    source_anchor(thread), "path scope"
                ),
                "claim_heading": heading,
                "claim_fingerprint": sha256_bytes(claim_body.encode("utf-8")),
            }
        )
    return rows


def build_group_member_rows(
    records: list[dict[str, Any]],
) -> list[dict[str, str]]:
    output = []
    for rank, record in enumerate(records, start=1):
        output.append(
            {
                "corpus_rank": str(rank),
                "thread_id": record["thread_id"],
                **group_identities(record),
            }
        )
    return output


def build_work_group_rows(
    records: list[dict[str, Any]],
    group_members: list[dict[str, str]],
) -> list[dict[str, str]]:
    records_by_thread = {record["thread_id"]: record for record in records}
    members_by_group: dict[str, list[dict[str, str]]] = defaultdict(list)
    for membership in group_members:
        members_by_group[membership["work_group_id"]].append(membership)
    output = []
    for work_group_id, memberships in members_by_group.items():
        group_records = [
            records_by_thread[membership["thread_id"]] for membership in memberships
        ]
        representative = group_records[0]
        pull_request_states = [
            record["pull_request"]["state"] for record in group_records
        ]
        outdated_states = [record["thread"]["is_outdated"] for record in group_records]
        output.append(
            {
                "work_group_id": work_group_id,
                "review_path": representative["thread"]["path"],
                "source_anchor": durable_display_label(
                    source_anchor(representative["thread"]), "path scope"
                ),
                "thread_count": str(len(group_records)),
                "claim_group_count": str(
                    len({membership["claim_group_id"] for membership in memberships})
                ),
                "pull_request_count": str(
                    len({record["pull_request"]["number"] for record in group_records})
                ),
                "merged_pull_request_threads": str(pull_request_states.count("MERGED")),
                "closed_pull_request_threads": str(pull_request_states.count("CLOSED")),
                "open_pull_request_threads": str(pull_request_states.count("OPEN")),
                "current_threads": str(outdated_states.count(False)),
                "outdated_threads": str(outdated_states.count(True)),
                "oldest_thread_created_at": group_records[0]["thread_created_at"],
                "newest_thread_created_at": group_records[-1]["thread_created_at"],
                "representative_thread_id": representative["thread_id"],
                "classification_state": "unassessed",
                "next_action": "inspect-current-main-and-history",
            }
        )
    return sorted(
        output,
        key=lambda group: (
            group["oldest_thread_created_at"],
            group["work_group_id"],
        ),
    )


def build_summary(
    records: list[dict[str, Any]],
    group_members: list[dict[str, str]],
    work_groups: list[dict[str, str]],
) -> dict[str, Any]:
    state_counts = {state: 0 for state in ("MERGED", "CLOSED", "OPEN")}
    outdated_count = 0
    for record in records:
        state_counts[record["pull_request"]["state"]] += 1
        outdated_count += int(record["thread"]["is_outdated"])
    return {
        "unresolved_thread_count": len(records),
        "merged_pull_request_threads": state_counts["MERGED"],
        "closed_pull_request_threads": state_counts["CLOSED"],
        "open_pull_request_threads": state_counts["OPEN"],
        "current_threads": len(records) - outdated_count,
        "outdated_threads": outdated_count,
        "review_path_count": len({record["thread"]["path"] for record in records}),
        "path_group_count": len(
            {membership["path_group_id"] for membership in group_members}
        ),
        "source_group_count": len(
            {membership["source_group_id"] for membership in group_members}
        ),
        "claim_group_count": len(
            {membership["claim_group_id"] for membership in group_members}
        ),
        "work_group_count": len(work_groups),
    }


def write_tsv(
    path: Path, fieldnames: tuple[str, ...], rows: list[dict[str, str]]
) -> None:
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=fieldnames,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def retained_files(input_dir: Path) -> dict[str, Path]:
    return {
        str(path.relative_to(input_dir)): path
        for path in input_dir.rglob("*")
        if path.is_file() and path.name != "manifest.json"
    }


def write_capture(
    output_dir: Path,
    raw_payloads: list[tuple[str, dict[str, Any]]],
    records: list[dict[str, Any]],
    manifest: dict[str, Any],
) -> None:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise CorpusError(f"output directory is not empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = output_dir / "raw"
    raw_dir.mkdir()
    file_hashes: dict[str, str] = {}
    for name, query in QUERY_FILES.items():
        relative_path = f"{name}.graphql"
        content = query.encode("utf-8")
        (output_dir / relative_path).write_bytes(content)
        file_hashes[relative_path] = sha256_bytes(content)
    for name, payload in raw_payloads:
        relative_path = f"raw/{name}.gz"
        content = compressed_json_bytes(payload)
        (output_dir / relative_path).write_bytes(content)
        file_hashes[relative_path] = sha256_bytes(content)
    thread_payload = compressed_json_bytes(records)
    (output_dir / "threads.json.gz").write_bytes(thread_payload)
    file_hashes["threads.json.gz"] = sha256_bytes(thread_payload)
    thread_rows = build_thread_rows(records)
    group_members = build_group_member_rows(records)
    work_groups = build_work_group_rows(records, group_members)
    summary = build_summary(records, group_members, work_groups)
    derived_files = (
        ("threads.tsv", THREAD_FIELDS, thread_rows),
        ("group-members.tsv", GROUP_MEMBER_FIELDS, group_members),
        ("work-groups.tsv", WORK_GROUP_FIELDS, work_groups),
    )
    for relative_path, fields, rows in derived_files:
        path = output_dir / relative_path
        write_tsv(path, fields, rows)
        file_hashes[relative_path] = sha256_bytes(path.read_bytes())
    summary_content = json_bytes(summary)
    (output_dir / "summary.json").write_bytes(summary_content)
    file_hashes["summary.json"] = sha256_bytes(summary_content)
    manifest["files"] = dict(sorted(file_hashes.items()))
    (output_dir / "manifest.json").write_bytes(json_bytes(manifest))


def read_json(path: Path) -> Any:
    try:
        content = path.read_bytes()
        if path.suffix == ".gz":
            content = gzip.decompress(content)
        return json.loads(content.decode("utf-8"))
    except (
        gzip.BadGzipFile,
        json.JSONDecodeError,
        OSError,
        UnicodeDecodeError,
    ) as error:
        raise CorpusError(f"cannot read JSON from {path}: {error}") from error


def json_artifact_paths(directory: Path, stem_pattern: str) -> list[Path]:
    paths = list(directory.glob(f"{stem_pattern}.json"))
    paths.extend(directory.glob(f"{stem_pattern}.json.gz"))
    logical_names: dict[str, Path] = {}
    for path in paths:
        logical_name = path.name.removesuffix(".gz")
        if logical_name in logical_names:
            raise CorpusError(
                f"retained JSON has duplicate compressed and plain forms: {logical_name}"
            )
        logical_names[logical_name] = path
    return [logical_names[name] for name in sorted(logical_names)]


def thread_records_path(input_dir: Path) -> Path:
    paths = [
        path
        for path in (input_dir / "threads.json", input_dir / "threads.json.gz")
        if path.is_file()
    ]
    if len(paths) != 1:
        raise CorpusError("retained corpus must contain exactly one thread record file")
    return paths[0]


def read_tsv(path: Path, fields: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        if tuple(reader.fieldnames or ()) != fields:
            raise CorpusError(f"{path}: header differs from its schema")
        rows = list(reader)
    for row_number, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise CorpusError(f"{path}:{row_number}: malformed TSV row")
    return rows


def validate_file_manifest(input_dir: Path, manifest: dict[str, Any]) -> None:
    declared = manifest.get("files")
    if not isinstance(declared, dict) or not declared:
        raise CorpusError("manifest lacks retained file hashes")
    actual = retained_files(input_dir)
    if set(declared) != set(actual):
        raise CorpusError(
            "retained file membership differs from manifest: "
            f"missing={sorted(set(declared) - set(actual))}, "
            f"extra={sorted(set(actual) - set(declared))}"
        )
    for relative_path, path in actual.items():
        expected_hash = declared.get(relative_path)
        if not isinstance(expected_hash, str) or len(expected_hash) != 64:
            raise CorpusError(f"manifest hash is invalid for {relative_path}")
        if sha256_bytes(path.read_bytes()) != expected_hash:
            raise CorpusError(f"retained hash differs for {relative_path}")


def validate_query_files(input_dir: Path) -> None:
    for name, query in QUERY_FILES.items():
        if (input_dir / f"{name}.graphql").read_text(encoding="utf-8") != query:
            raise CorpusError(f"retained query differs from generator: {name}")


def raw_wrapper(
    path: Path, expected_query: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    wrapper = read_json(path)
    if not isinstance(wrapper, dict) or wrapper.get("query") != expected_query:
        raise CorpusError(f"{path}: query declaration is invalid")
    variables = wrapper.get("variables")
    response = wrapper.get("response")
    if not isinstance(variables, dict) or not isinstance(response, dict):
        raise CorpusError(f"{path}: request or response is invalid")
    return variables, response


def reconstruct_detailed_corpus(
    input_dir: Path, manifest: dict[str, Any]
) -> tuple[list[dict[str, Any]], tuple[str, str], set[Path]]:
    raw_dir = input_dir / "raw"
    primary_paths = json_artifact_paths(raw_dir, "pull-requests-[0-9][0-9][0-9][0-9]")
    if len(primary_paths) != manifest.get("pull_request_page_count"):
        raise CorpusError("retained pull-request page count differs from manifest")
    owner = manifest.get("owner")
    repo = manifest.get("repository")
    expected_branch = (
        manifest.get("default_branch"),
        manifest.get("default_branch_oid"),
    )
    pull_requests: list[dict[str, Any]] = []
    pull_requests_by_number: dict[int, dict[str, Any]] = {}
    thread_connections: dict[int, dict[str, Any]] = {}
    comment_connections: dict[str, dict[str, Any]] = {}
    threads_by_id: dict[str, dict[str, Any]] = {}
    consumed_paths = set(primary_paths)
    previous_cursor: str | None = None
    has_more_pull_requests = True
    for page_number, path in enumerate(primary_paths, start=1):
        variables, response = raw_wrapper(path, "pull-requests")
        expected_variables: dict[str, Any] = {"owner": owner, "repo": repo}
        if previous_cursor is not None:
            expected_variables["cursor"] = previous_cursor
        if variables != expected_variables:
            raise CorpusError(
                f"retained pull-request page {page_number}: cursor is discontinuous"
            )
        repository = repository_from_payload(
            response, f"retained pull-request page {page_number}"
        )
        if (
            branch_identity(repository, f"retained pull-request page {page_number}")
            != expected_branch
        ):
            raise CorpusError("retained detailed page changed default branch")
        connection = repository.get("pullRequests")
        if not isinstance(connection, dict):
            raise CorpusError(
                f"retained pull-request page {page_number}: connection missing"
            )
        nodes = connection.get("nodes")
        if not isinstance(nodes, list) or not nodes:
            raise CorpusError(
                f"retained pull-request page {page_number}: nodes are missing"
            )
        for node in nodes:
            pull_request, connections = normalize_pull_request(node)
            number = pull_request["number"]
            if number in pull_requests_by_number:
                raise CorpusError(f"retained capture repeats pull request {number}")
            pull_requests.append(pull_request)
            pull_requests_by_number[number] = pull_request
            thread_connections[number] = connections["threads"]
            for thread in pull_request["threads"]:
                thread_id = thread["id"]
                if thread_id in threads_by_id:
                    raise CorpusError(f"retained capture repeats {thread_id}")
                threads_by_id[thread_id] = thread
                comment_connections[thread_id] = connections["comments"][thread_id]
        has_more_pull_requests, previous_cursor = page_info(
            connection, f"retained pull-request page {page_number}"
        )
    if has_more_pull_requests:
        raise CorpusError("retained pull-request pagination is incomplete")

    supplemental_thread_paths = json_artifact_paths(
        raw_dir, "pull-request-*-threads-[0-9][0-9][0-9][0-9]"
    )
    consumed_paths.update(supplemental_thread_paths)
    thread_page_counts = {str(number): 1 for number in pull_requests_by_number}
    for path in supplemental_thread_paths:
        variables, response = raw_wrapper(path, "pull-request-threads")
        number = variables.get("number")
        if not isinstance(number, int) or number not in pull_requests_by_number:
            raise CorpusError(f"{path}: pull request identity is invalid")
        has_next_page, expected_cursor = page_info(
            thread_connections[number], f"retained pull request {number} threads"
        )
        expected_variables = {
            "owner": owner,
            "repo": repo,
            "number": number,
            "cursor": expected_cursor,
        }
        if not has_next_page or variables != expected_variables:
            raise CorpusError(
                f"retained pull request {number}: thread cursor is discontinuous"
            )
        repository = repository_from_payload(
            response, f"retained pull request {number} thread page"
        )
        if (
            branch_identity(repository, f"retained pull request {number} thread page")
            != expected_branch
        ):
            raise CorpusError(f"retained pull request {number}: default branch changed")
        pull_request_node = repository.get("pullRequest")
        connection = (
            pull_request_node.get("reviewThreads")
            if isinstance(pull_request_node, dict)
            else None
        )
        if not isinstance(connection, dict):
            raise CorpusError(
                f"retained pull request {number}: supplemental threads missing"
            )
        nodes = connection.get("nodes")
        if not isinstance(nodes, list):
            raise CorpusError(
                f"retained pull request {number}: supplemental nodes missing"
            )
        for node in nodes:
            thread, comments = normalize_thread(node, number)
            thread_id = thread["id"]
            if thread_id in threads_by_id:
                raise CorpusError(f"retained capture repeats {thread_id}")
            threads_by_id[thread_id] = thread
            pull_requests_by_number[number]["threads"].append(thread)
            comment_connections[thread_id] = comments
        thread_connections[number] = connection
        thread_page_counts[str(number)] += 1
    for number, connection in thread_connections.items():
        has_next_page, _cursor = page_info(
            connection, f"retained pull request {number} final thread page"
        )
        if has_next_page:
            raise CorpusError(
                f"retained pull request {number}: thread pagination is incomplete"
            )
    if thread_page_counts != manifest.get("thread_page_counts"):
        raise CorpusError("retained thread page counts differ from manifest")

    comment_page_counts = {
        thread_id: 1
        for thread_id, thread in threads_by_id.items()
        if not thread["is_resolved"]
    }
    supplemental_comment_paths = json_artifact_paths(
        raw_dir, "thread-*-comments-[0-9][0-9][0-9][0-9]"
    )
    consumed_paths.update(supplemental_comment_paths)
    for path in supplemental_comment_paths:
        variables, response = raw_wrapper(path, "thread-comments")
        thread_id = variables.get("threadId")
        if not isinstance(thread_id, str) or thread_id not in comment_page_counts:
            raise CorpusError(f"{path}: comment thread identity is invalid")
        has_next_page, expected_cursor = page_info(
            comment_connections[thread_id], f"retained {thread_id} comments"
        )
        if not has_next_page or variables != {
            "threadId": thread_id,
            "cursor": expected_cursor,
        }:
            raise CorpusError(f"retained {thread_id}: comment cursor is discontinuous")
        node = response.get("data", {}).get("node")
        if not isinstance(node, dict) or node.get("id") != thread_id:
            raise CorpusError(f"{path}: response omitted {thread_id}")
        connection = node.get("comments")
        if not isinstance(connection, dict):
            raise CorpusError(f"{path}: comments connection is missing")
        seen_comment_ids = {
            comment["id"] for comment in threads_by_id[thread_id]["comments"]
        }
        append_comments(threads_by_id[thread_id], connection, seen_comment_ids)
        comment_connections[thread_id] = connection
        comment_page_counts[thread_id] += 1
    for thread_id in comment_page_counts:
        has_next_page, _cursor = page_info(
            comment_connections[thread_id], f"retained {thread_id} final comments"
        )
        if has_next_page:
            raise CorpusError(f"retained {thread_id}: comments are incomplete")
        thread = threads_by_id[thread_id]
        if len(thread["comments"]) != thread["comment_total_count"]:
            raise CorpusError(f"retained {thread_id}: comment count differs")
    if comment_page_counts != manifest.get("comment_page_counts"):
        raise CorpusError("retained comment page counts differ from manifest")

    chronological_pull_requests = [
        (
            parse_timestamp(
                pull_request["created_at"],
                f"retained pull request {pull_request['number']}",
            ),
            pull_request["number"],
        )
        for pull_request in pull_requests
    ]
    if chronological_pull_requests != sorted(chronological_pull_requests):
        raise CorpusError("retained pull requests are not chronologically ordered")
    if len(pull_requests) != manifest.get("scanned_pull_request_count"):
        raise CorpusError("retained pull-request count differs from manifest")
    if len(threads_by_id) != manifest.get("scanned_review_thread_count"):
        raise CorpusError("retained review-thread count differs from manifest")
    return unresolved_records(pull_requests), expected_branch, consumed_paths


def reconstruct_membership(
    input_dir: Path,
    manifest: dict[str, Any],
    expected_branch: tuple[str, str],
) -> tuple[dict[str, dict[str, Any]], set[Path]]:
    raw_dir = input_dir / "raw"
    primary_paths = json_artifact_paths(raw_dir, "membership-[0-9][0-9][0-9][0-9]")
    if len(primary_paths) != manifest.get("membership_page_count"):
        raise CorpusError("retained membership page count differs from manifest")
    owner = manifest.get("owner")
    repo = manifest.get("repository")
    membership: dict[str, dict[str, Any]] = {}
    seen_thread_ids: set[str] = set()
    thread_connections: dict[int, dict[str, Any]] = {}
    pull_request_states: dict[int, str] = {}
    consumed_paths = set(primary_paths)
    previous_cursor: str | None = None
    has_more_pull_requests = True
    for page_number, path in enumerate(primary_paths, start=1):
        variables, response = raw_wrapper(path, "membership")
        expected_variables: dict[str, Any] = {"owner": owner, "repo": repo}
        if previous_cursor is not None:
            expected_variables["cursor"] = previous_cursor
        if variables != expected_variables:
            raise CorpusError(
                f"retained membership page {page_number}: cursor is discontinuous"
            )
        repository = repository_from_payload(
            response, f"retained membership page {page_number}"
        )
        if (
            branch_identity(repository, f"retained membership page {page_number}")
            != expected_branch
        ):
            raise CorpusError("retained membership page changed default branch")
        connection = repository.get("pullRequests")
        if not isinstance(connection, dict):
            raise CorpusError(
                f"retained membership page {page_number}: connection missing"
            )
        nodes = connection.get("nodes")
        if not isinstance(nodes, list) or not nodes:
            raise CorpusError(
                f"retained membership page {page_number}: nodes are missing"
            )
        for pull_request_node in nodes:
            if not isinstance(pull_request_node, dict):
                raise CorpusError("retained membership pull request is malformed")
            number = pull_request_node.get("number")
            state = pull_request_node.get("state")
            threads = pull_request_node.get("reviewThreads")
            if (
                not isinstance(number, int)
                or isinstance(number, bool)
                or number <= 0
                or state not in ("MERGED", "CLOSED", "OPEN")
                or not isinstance(threads, dict)
                or number in thread_connections
            ):
                raise CorpusError("retained membership pull request is invalid")
            pull_request_states[number] = state
            thread_connections[number] = threads
            append_membership_threads(
                membership,
                seen_thread_ids,
                threads.get("nodes"),
                number,
                state,
            )
        has_more_pull_requests, previous_cursor = page_info(
            connection, f"retained membership page {page_number}"
        )
    if has_more_pull_requests:
        raise CorpusError("retained membership pull-request pagination is incomplete")

    supplemental_paths = json_artifact_paths(
        raw_dir, "membership-pull-request-*-threads-[0-9][0-9][0-9][0-9]"
    )
    consumed_paths.update(supplemental_paths)
    thread_page_counts = {str(number): 1 for number in thread_connections}
    for path in supplemental_paths:
        variables, response = raw_wrapper(path, "membership-threads")
        number = variables.get("number")
        if not isinstance(number, int) or number not in thread_connections:
            raise CorpusError(f"{path}: membership pull request is invalid")
        has_next_page, expected_cursor = page_info(
            thread_connections[number],
            f"retained membership pull request {number}",
        )
        expected_variables = {
            "owner": owner,
            "repo": repo,
            "number": number,
            "cursor": expected_cursor,
        }
        if not has_next_page or variables != expected_variables:
            raise CorpusError(
                f"retained membership pull request {number}: cursor is discontinuous"
            )
        repository = repository_from_payload(
            response, f"retained membership pull request {number} thread page"
        )
        if (
            branch_identity(
                repository,
                f"retained membership pull request {number} thread page",
            )
            != expected_branch
        ):
            raise CorpusError(
                f"retained membership pull request {number}: default branch changed"
            )
        pull_request_node = repository.get("pullRequest")
        if not isinstance(pull_request_node, dict):
            raise CorpusError(
                f"retained membership pull request {number}: node is missing"
            )
        state = pull_request_states[number]
        if pull_request_node.get("state") != state:
            raise CorpusError(
                f"retained membership pull request {number}: state changed"
            )
        connection = pull_request_node.get("reviewThreads")
        if not isinstance(connection, dict):
            raise CorpusError(
                f"retained membership pull request {number}: threads missing"
            )
        append_membership_threads(
            membership,
            seen_thread_ids,
            connection.get("nodes"),
            number,
            state,
        )
        thread_connections[number] = connection
        thread_page_counts[str(number)] += 1
    for number, connection in thread_connections.items():
        has_next_page, _cursor = page_info(
            connection, f"retained membership pull request {number} final page"
        )
        if has_next_page:
            raise CorpusError(
                f"retained membership pull request {number}: threads are incomplete"
            )
    if thread_page_counts != manifest.get("membership_thread_page_counts"):
        raise CorpusError("retained membership thread page counts differ")
    return membership, consumed_paths


def validate_records(records: Any) -> list[dict[str, Any]]:
    if not isinstance(records, list) or not records:
        raise CorpusError("thread corpus is empty or malformed")
    thread_ids: set[str] = set()
    chronological_keys: list[tuple[datetime, str]] = []
    for rank, record in enumerate(records, start=1):
        if not isinstance(record, dict):
            raise CorpusError(f"corpus rank {rank}: record is malformed")
        thread_id = record.get("thread_id")
        thread = record.get("thread")
        pull_request = record.get("pull_request")
        created_at = record.get("thread_created_at")
        if (
            not isinstance(thread_id, str)
            or not THREAD_ID.fullmatch(thread_id)
            or not isinstance(thread, dict)
            or not isinstance(pull_request, dict)
            or thread.get("id") != thread_id
            or thread.get("is_resolved") is not False
        ):
            raise CorpusError(f"corpus rank {rank}: record identity is invalid")
        if thread_id in thread_ids:
            raise CorpusError(f"corpus repeats {thread_id}")
        thread_ids.add(thread_id)
        if not isinstance(thread.get("comments"), list) or not thread["comments"]:
            raise CorpusError(f"{thread_id}: comments are missing")
        if len(thread["comments"]) != thread.get("comment_total_count"):
            raise CorpusError(f"{thread_id}: retained comment count differs")
        if created_at != thread["comments"][0].get("created_at"):
            raise CorpusError(f"{thread_id}: first-comment chronology differs")
        chronological_keys.append((parse_timestamp(created_at, thread_id), thread_id))
    if chronological_keys != sorted(chronological_keys):
        raise CorpusError("thread corpus is not chronologically ordered")
    return records


def validate_derived_files(
    input_dir: Path, records: list[dict[str, Any]], manifest: dict[str, Any]
) -> None:
    thread_rows = build_thread_rows(records)
    group_members = build_group_member_rows(records)
    work_groups = build_work_group_rows(records, group_members)
    retained_thread_rows = read_tsv(input_dir / "threads.tsv", THREAD_FIELDS)
    retained_group_members = read_tsv(
        input_dir / "group-members.tsv", GROUP_MEMBER_FIELDS
    )
    retained_work_groups = read_tsv(input_dir / "work-groups.tsv", WORK_GROUP_FIELDS)
    if retained_thread_rows != thread_rows:
        raise CorpusError("thread TSV differs from retained thread data")
    if retained_group_members != group_members:
        raise CorpusError("group membership differs from retained thread data")
    if retained_work_groups != work_groups:
        raise CorpusError("work groups differ from retained thread data")
    thread_ids = {record["thread_id"] for record in records}
    member_ids = {membership["thread_id"] for membership in group_members}
    if thread_ids != member_ids or len(group_members) != len(records):
        raise CorpusError("group membership is not an exact thread partition")
    summary = build_summary(records, group_members, work_groups)
    if read_json(input_dir / "summary.json") != summary:
        raise CorpusError("summary differs from retained thread data")
    if manifest.get("unresolved_thread_count") != len(records):
        raise CorpusError("manifest unresolved count differs from thread data")
    for field, value in summary.items():
        if manifest.get(field) != value:
            raise CorpusError(f"manifest {field} differs from summary")


def check_capture(input_dir: Path) -> None:
    manifest = read_json(input_dir / "manifest.json")
    if not isinstance(manifest, dict) or manifest.get("schema") != CORPUS_SCHEMA:
        raise CorpusError("manifest has an unexpected schema")
    for field in ("corpus_id", "owner", "repository", "default_branch"):
        if not isinstance(manifest.get(field), str) or not manifest[field]:
            raise CorpusError(f"manifest {field} is missing")
    parse_timestamp(manifest.get("observed_at"), "manifest observed_at")
    parse_timestamp(
        manifest.get("membership_verified_at"), "manifest membership_verified_at"
    )
    if not COMMIT_OID.fullmatch(str(manifest.get("default_branch_oid", ""))):
        raise CorpusError("manifest default-branch commit is invalid")
    validate_file_manifest(input_dir, manifest)
    validate_query_files(input_dir)
    records = validate_records(read_json(thread_records_path(input_dir)))
    reconstructed_records, expected_branch, detailed_paths = (
        reconstruct_detailed_corpus(input_dir, manifest)
    )
    if reconstructed_records != records:
        raise CorpusError("retained thread data differs from raw GraphQL capture")
    membership, membership_paths = reconstruct_membership(
        input_dir, manifest, expected_branch
    )
    validate_exact_membership(records, membership)
    consumed_raw_paths = detailed_paths | membership_paths
    actual_raw_paths = {
        path
        for path in (input_dir / "raw").iterdir()
        if path.is_file()
        and (path.name.endswith(".json") or path.name.endswith(".json.gz"))
    }
    if consumed_raw_paths != actual_raw_paths:
        raise CorpusError(
            "raw response membership differs from reconstructed capture: "
            f"missing={sorted(str(path) for path in actual_raw_paths - consumed_raw_paths)}, "
            f"extra={sorted(str(path) for path in consumed_raw_paths - actual_raw_paths)}"
        )
    validate_derived_files(input_dir, records, manifest)


def compress_capture(input_dir: Path) -> None:
    check_capture(input_dir)
    plain_paths = sorted((input_dir / "raw").glob("*.json"))
    plain_thread_path = input_dir / "threads.json"
    if plain_thread_path.is_file():
        plain_paths.append(plain_thread_path)
    if not plain_paths:
        return
    manifest_path = input_dir / "manifest.json"
    manifest = read_json(manifest_path)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("files"), dict):
        raise CorpusError("manifest lacks a compressible file declaration")
    replacements: list[tuple[Path, Path, bytes]] = []
    for source_path in plain_paths:
        target_path = source_path.with_name(f"{source_path.name}.gz")
        if target_path.exists():
            raise CorpusError(f"compressed target already exists: {target_path}")
        replacements.append(
            (
                source_path,
                target_path,
                gzip.compress(source_path.read_bytes(), compresslevel=9, mtime=0),
            )
        )
    for _source_path, target_path, content in replacements:
        target_path.write_bytes(content)
    declared_files = dict(manifest["files"])
    for source_path, target_path, content in replacements:
        source_relative_path = str(source_path.relative_to(input_dir))
        target_relative_path = str(target_path.relative_to(input_dir))
        if source_relative_path not in declared_files:
            raise CorpusError(f"manifest omits {source_relative_path}")
        declared_files.pop(source_relative_path)
        declared_files[target_relative_path] = sha256_bytes(content)
    for source_path, _target_path, _content in replacements:
        source_path.unlink()
    manifest["files"] = dict(sorted(declared_files.items()))
    manifest_path.write_bytes(json_bytes(manifest))
    check_capture(input_dir)


def capture(owner: str, repo: str, corpus_id: str) -> tuple[
    list[tuple[str, dict[str, Any]]],
    list[dict[str, Any]],
    dict[str, Any],
]:
    (
        pull_requests,
        detailed_payloads,
        branch,
        thread_page_counts,
        comment_page_counts,
    ) = capture_detailed_corpus(owner, repo)
    records = unresolved_records(pull_requests)
    observed_at = datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    membership, membership_payloads, membership_thread_page_counts = capture_membership(
        owner, repo, branch
    )
    validate_exact_membership(records, membership)
    if observe_default_branch(owner, repo) != branch:
        raise CorpusError("default branch changed before capture completed")
    membership_verified_at = datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    group_members = build_group_member_rows(records)
    work_groups = build_work_group_rows(records, group_members)
    summary = build_summary(records, group_members, work_groups)
    manifest = {
        "schema": CORPUS_SCHEMA,
        "corpus_id": corpus_id,
        "owner": owner,
        "repository": repo,
        "observed_at": observed_at,
        "membership_verified_at": membership_verified_at,
        "default_branch": branch[0],
        "default_branch_oid": branch[1],
        "pull_request_page_count": sum(
            name.startswith("pull-requests-") for name, _payload in detailed_payloads
        ),
        "membership_page_count": sum(
            name.startswith("membership-") and "pull-request" not in name
            for name, _payload in membership_payloads
        ),
        "scanned_pull_request_count": len(pull_requests),
        "scanned_review_thread_count": sum(
            len(pull_request["threads"]) for pull_request in pull_requests
        ),
        "thread_page_counts": thread_page_counts,
        "comment_page_counts": comment_page_counts,
        "membership_thread_page_counts": membership_thread_page_counts,
        **summary,
    }
    return detailed_payloads + membership_payloads, records, manifest


def main() -> int:
    arguments = parse_args()
    try:
        if arguments.command == "check":
            check_capture(arguments.input_dir)
            print(f"OK: verified unresolved review-thread corpus {arguments.input_dir}")
            return 0
        if arguments.command == "compress":
            compress_capture(arguments.input_dir)
            print(
                "OK: compressed exact review-thread JSON evidence in "
                f"{arguments.input_dir}"
            )
            return 0
        raw_payloads, records, manifest = capture(
            arguments.owner, arguments.repo, arguments.corpus_id
        )
        write_capture(arguments.output_dir, raw_payloads, records, manifest)
        check_capture(arguments.output_dir)
    except (CorpusError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"OK: captured {manifest['unresolved_thread_count']} unresolved threads in "
        f"{manifest['work_group_count']} work groups at "
        f"{manifest['default_branch_oid']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
