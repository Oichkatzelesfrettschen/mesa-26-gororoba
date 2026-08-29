#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bind already-resolved review threads to durable merged source evidence."""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import subprocess
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import Any

import review_thread_corpus

SCHEMA = "mesa-review-thread-reconciliation-v1"
FRONTIER_FIELDS = (
    "thread_id",
    "thread_created_at",
    "review_url",
    "review_path",
    "original_line",
    "claim_heading",
    "status_at_capture",
    "falsifier",
)
OVERRIDE_FIELDS = (
    "thread_id",
    "relationship",
    "current_target",
    "source_change_commit",
    "evidence_kind",
    "evidence_commit",
    "evidence_pr",
    "evidence_merged_at",
)
EVIDENCE_FIELDS = (
    *FRONTIER_FIELDS,
    "resolution_state",
    "relationship",
    "source_change_commit",
    "evidence_kind",
    "evidence_commit",
    "evidence_pr",
    "evidence_pr_title",
    "evidence_merged_at",
    "current_target",
    "current_target_identity",
    "current_owner_change_commit",
    "current_owner_evidence_kind",
    "current_owner_evidence_commit",
    "current_owner_evidence_pr",
    "current_owner_evidence_pr_title",
    "current_owner_evidence_merged_at",
    "current_owner_change_subject",
    "source_change_subject",
    "verification_method",
)
RELATIONSHIPS = frozenset(
    ("same-source", "renamed-source", "moved-source", "removed-source")
)
EVIDENCE_KINDS = frozenset(("merged-pr", "direct-main"))
OWNER = "Oichkatzelesfrettschen"
REPOSITORY = "mesa-26-gororoba"


class ReconciliationError(ValueError):
    """The retained review-thread reconciliation is inconsistent."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("capture", "build", "check"):
        command = commands.add_parser(name)
        command.add_argument("--corpus-dir", type=Path, required=True)
        command.add_argument("--cluster-dir", type=Path, required=True)
        command.add_argument("--repo-root", type=Path, required=True)
        command.add_argument("--revision", required=True)
        command.add_argument("--frontier", type=Path, required=True)
        command.add_argument("--overrides", type=Path, required=True)
        command.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def run_git(repository_root: Path, *arguments: str, strip: bool = True) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise ReconciliationError(f"git {' '.join(arguments)} failed: {diagnostic}")
    return completed.stdout.strip() if strip else completed.stdout


def git_returncode(repository_root: Path, *arguments: str) -> int:
    return subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        capture_output=True,
        text=True,
        check=False,
    ).returncode


def resolve_revision(repository_root: Path, revision: str) -> str:
    value = run_git(repository_root, "rev-parse", "--verify", f"{revision}^{{commit}}")
    if not review_thread_corpus.COMMIT_OID.fullmatch(value):
        raise ReconciliationError(f"revision {revision!r} is not a commit")
    return value


def sha256_path(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_tsv(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=fields,
            delimiter="\t",
            lineterminator="\n",
            quoting=csv.QUOTE_ALL,
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path: Path, fields: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        if tuple(reader.fieldnames or ()) != fields:
            raise ReconciliationError(f"{path}: header differs from the schema")
        rows = list(reader)
    for row_number, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise ReconciliationError(f"{path}:{row_number}: malformed TSV row")
    return rows


def graphql_nodes(thread_ids: list[str]) -> dict[str, bool]:
    states: dict[str, bool] = {}
    query = "query($ids:[ID!]!){nodes(ids:$ids){... on PullRequestReviewThread{id isResolved}}}"
    for offset in range(0, len(thread_ids), 100):
        request = json.dumps(
            {"query": query, "variables": {"ids": thread_ids[offset : offset + 100]}}
        )
        completed = subprocess.run(
            ["gh", "api", "graphql", "--input", "-"],
            input=request,
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode:
            diagnostic = completed.stderr.strip() or completed.stdout.strip()
            raise ReconciliationError(f"GitHub thread query failed: {diagnostic}")
        payload = json.loads(completed.stdout)
        if payload.get("errors"):
            raise ReconciliationError(
                f"GitHub thread query errors: {payload['errors']}"
            )
        nodes = payload.get("data", {}).get("nodes")
        if not isinstance(nodes, list):
            raise ReconciliationError("GitHub thread query omitted nodes")
        for node in nodes:
            if not isinstance(node, dict) or not isinstance(node.get("id"), str):
                raise ReconciliationError(
                    "GitHub thread query returned an unknown node"
                )
            resolved = node.get("isResolved")
            if not isinstance(resolved, bool):
                raise ReconciliationError(
                    f"GitHub resolution state missing for {node['id']}"
                )
            states[node["id"]] = resolved
    if set(states) != set(thread_ids):
        raise ReconciliationError(
            "GitHub thread query membership differs from frontier"
        )
    return states


def graphql_pull_requests(commits: Iterable[str]) -> dict[str, dict[str, str]]:
    commit_list = sorted(set(commits))
    result: dict[str, dict[str, str]] = {}
    for offset in range(0, len(commit_list), 40):
        selections = []
        for index, commit in enumerate(commit_list[offset : offset + 40]):
            selections.append(
                f'c{index}:object(oid:"{commit}"){{... on Commit{{oid '
                "associatedPullRequests(first:10){nodes{number state mergedAt "
                "mergeCommit{oid} title}}}}"
            )
        query = (
            f'query{{repository(owner:"{OWNER}",name:"{REPOSITORY}"){{'
            + " ".join(selections)
            + "}}"
        )
        completed = subprocess.run(
            ["gh", "api", "graphql", "-f", f"query={query}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode:
            diagnostic = completed.stderr.strip() or completed.stdout.strip()
            raise ReconciliationError(f"GitHub commit query failed: {diagnostic}")
        payload = json.loads(completed.stdout)
        if payload.get("errors"):
            raise ReconciliationError(
                f"GitHub commit query errors: {payload['errors']}"
            )
        nodes = payload.get("data", {}).get("repository")
        if not isinstance(nodes, dict):
            raise ReconciliationError("GitHub commit query omitted repository data")
        for node in nodes.values():
            if not isinstance(node, dict) or not isinstance(node.get("oid"), str):
                raise ReconciliationError(
                    "GitHub commit query returned an unknown node"
                )
            merged = [
                pull_request
                for pull_request in node.get("associatedPullRequests", {}).get(
                    "nodes", []
                )
                if isinstance(pull_request, dict)
                and pull_request.get("state") == "MERGED"
                and isinstance(pull_request.get("number"), int)
                and isinstance(pull_request.get("mergedAt"), str)
                and isinstance(pull_request.get("mergeCommit"), dict)
                and isinstance(pull_request["mergeCommit"].get("oid"), str)
                and isinstance(pull_request.get("title"), str)
            ]
            if len(merged) != 1:
                continue
            pull_request = merged[0]
            result[node["oid"]] = {
                "evidence_pr": str(pull_request["number"]),
                "evidence_commit": pull_request["mergeCommit"]["oid"],
                "evidence_pr_title": pull_request["title"],
                "evidence_merged_at": pull_request["mergedAt"],
            }
    return result


def graphql_merged_pull_request(number: str) -> dict[str, str]:
    query = (
        f'query{{repository(owner:"{OWNER}",name:"{REPOSITORY}"){{'
        f"pullRequest(number:{number}){{number state mergedAt mergeCommit{{oid}} title}}}}}}"
    )
    completed = subprocess.run(
        ["gh", "api", "graphql", "-f", f"query={query}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise ReconciliationError(f"GitHub pull request query failed: {diagnostic}")
    payload = json.loads(completed.stdout)
    if payload.get("errors"):
        raise ReconciliationError(
            f"GitHub pull request query errors: {payload['errors']}"
        )
    pull_request = payload.get("data", {}).get("repository", {}).get("pullRequest")
    if not isinstance(pull_request, dict):
        raise ReconciliationError("GitHub pull request query omitted pull request")
    merge_commit = pull_request.get("mergeCommit")
    if (
        pull_request.get("state") != "MERGED"
        or not isinstance(pull_request.get("number"), int)
        or not isinstance(pull_request.get("mergedAt"), str)
        or not isinstance(pull_request.get("title"), str)
        or not isinstance(merge_commit, dict)
        or not isinstance(merge_commit.get("oid"), str)
    ):
        raise ReconciliationError(f"GitHub pull request {number} is not merged")
    return {
        "evidence_pr": str(pull_request["number"]),
        "evidence_commit": merge_commit["oid"],
        "evidence_pr_title": pull_request["title"],
        "evidence_merged_at": pull_request["mergedAt"],
    }


def source_change_subject(repository_root: Path, commit: str) -> str:
    subject = run_git(repository_root, "show", "-s", "--format=%s", commit)
    if not subject:
        raise ReconciliationError(f"{commit}: missing source change subject")
    return subject


def corpus_records(corpus_dir: Path) -> dict[str, dict[str, Any]]:
    with gzip.open(
        corpus_dir / "threads.json.gz", "rt", encoding="utf-8"
    ) as input_file:
        records = review_thread_corpus.validate_records(json.load(input_file))
    result = {record["thread_id"]: record for record in records}
    if len(result) != len(records):
        raise ReconciliationError("review corpus repeats a thread ID")
    return result


def corpus_groups(corpus_dir: Path) -> dict[str, str]:
    rows = review_thread_corpus.read_tsv(
        corpus_dir / "group-members.tsv", review_thread_corpus.GROUP_MEMBER_FIELDS
    )
    result = {row["thread_id"]: row["work_group_id"] for row in rows}
    if len(result) != len(rows):
        raise ReconciliationError("review corpus repeats a thread group")
    return result


def cluster_statuses(cluster_dir: Path) -> dict[str, dict[str, str]]:
    fields = (
        "work_group_id",
        "status",
        "evidence_commit",
        "evidence_locator",
        "discovery_command",
        "verification_command",
        "verification_result",
        "closure_state",
        "reason",
        "falsifier",
    )
    rows = read_tsv(cluster_dir / "review-status.tsv", fields)
    result = {row["work_group_id"]: row for row in rows}
    if len(result) != len(rows):
        raise ReconciliationError("cluster status repeats a work group")
    return result


def capture_rows(corpus_dir: Path, cluster_dir: Path) -> list[dict[str, str]]:
    records = corpus_records(corpus_dir)
    groups = corpus_groups(corpus_dir)
    statuses = cluster_statuses(cluster_dir)
    if set(records) != set(groups) or set(groups.values()) != set(statuses):
        raise ReconciliationError("corpus and cluster membership differs")
    states = graphql_nodes(sorted(records))
    rows = []
    for thread_id, record in records.items():
        status = statuses[groups[thread_id]]
        if not states[thread_id] or status["verification_result"] == "pass":
            continue
        comment = record["thread"]["comments"][0]
        rows.append(
            {
                "thread_id": thread_id,
                "thread_created_at": record["thread_created_at"],
                "review_url": comment["url"],
                "review_path": record["thread"]["path"],
                "original_line": str(record["thread"]["original_line"]),
                "claim_heading": review_thread_corpus.durable_display_label(
                    review_thread_corpus.clean_claim_heading(comment["body"]),
                    "unlabeled review claim",
                ),
                "status_at_capture": status["status"],
                "falsifier": status["falsifier"],
            }
        )
    rows.sort(key=lambda row: (row["thread_created_at"], row["thread_id"]))
    return rows


def commit_changes_path(repository_root: Path, commit: str, path: str) -> bool:
    parent = run_git(repository_root, "rev-parse", f"{commit}^")
    changed = run_git(
        repository_root, "diff", "--name-only", parent, commit, "--", path
    )
    return bool(changed)


def path_identity(repository_root: Path, revision: str, path: str) -> str:
    return run_git(repository_root, "rev-parse", "--verify", f"{revision}:{path}")


def path_exists(repository_root: Path, revision: str, path: str) -> bool:
    return git_returncode(repository_root, "cat-file", "-e", f"{revision}:{path}") == 0


def validate_commit_ancestry(
    repository_root: Path, ancestor: str, revision: str
) -> None:
    if git_returncode(
        repository_root, "merge-base", "--is-ancestor", ancestor, revision
    ):
        raise ReconciliationError(f"{ancestor} is not merged in {revision}")


def build_rows(
    repository_root: Path,
    revision: str,
    frontier: list[dict[str, str]],
    overrides: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    frontier_ids = [row["thread_id"] for row in frontier]
    if len(set(frontier_ids)) != len(frontier_ids):
        raise ReconciliationError("reconciliation frontier repeats a thread ID")
    unknown_overrides = set(overrides).difference(frontier_ids)
    if unknown_overrides:
        raise ReconciliationError(
            f"override table has IDs outside the frontier: {sorted(unknown_overrides)}"
        )
    states = graphql_nodes(frontier_ids)
    unresolved = [thread_id for thread_id, resolved in states.items() if not resolved]
    if unresolved:
        raise ReconciliationError(
            f"frontier contains unresolved thread IDs: {unresolved}"
        )
    current_owners: dict[str, tuple[str, str]] = {}
    override_pull_requests: dict[str, dict[str, str]] = {}
    for row in frontier:
        override = overrides.get(row["thread_id"])
        if override is not None and override["relationship"] == "removed-source":
            continue
        path = (
            override["current_target"] if override is not None else row["review_path"]
        )
        if not path_exists(repository_root, revision, path):
            raise ReconciliationError(f"{row['thread_id']}: current target is missing")
        output = run_git(
            repository_root, "log", "-1", "--format=%H", revision, "--", path
        )
        if not output:
            raise ReconciliationError(f"{row['thread_id']}: no current source owner")
        current_owners[row["thread_id"]] = (output, path)
    associated = graphql_pull_requests(
        commit for commit, _path in current_owners.values()
    )
    result = []
    for row in frontier:
        thread_id = row["thread_id"]
        if thread_id in overrides:
            override = overrides[thread_id]
            relationship = override["relationship"]
            current_target = override["current_target"]
            source_change = override["source_change_commit"]
            evidence_kind = override["evidence_kind"]
            evidence_commit = override["evidence_commit"]
            evidence_pr = override["evidence_pr"]
            merged_at = override["evidence_merged_at"]
            evidence_pr_title = ""
        else:
            source_change, current_target = current_owners[thread_id]
            association = associated.get(source_change)
            if association is None:
                raise ReconciliationError(
                    f"{thread_id}: current source owner has no unique merged PR"
                )
            relationship = "same-source"
            evidence_kind = "merged-pr"
            evidence_commit = association["evidence_commit"]
            evidence_pr = association["evidence_pr"]
            evidence_pr_title = association["evidence_pr_title"]
            merged_at = association["evidence_merged_at"]
        if relationship not in RELATIONSHIPS:
            raise ReconciliationError(f"{thread_id}: invalid source relationship")
        if evidence_kind not in EVIDENCE_KINDS:
            raise ReconciliationError(f"{thread_id}: invalid evidence kind")
        if not review_thread_corpus.COMMIT_OID.fullmatch(source_change):
            raise ReconciliationError(f"{thread_id}: invalid source change commit")
        if not review_thread_corpus.COMMIT_OID.fullmatch(evidence_commit):
            raise ReconciliationError(f"{thread_id}: invalid evidence commit")
        validate_commit_ancestry(repository_root, source_change, evidence_commit)
        validate_commit_ancestry(repository_root, evidence_commit, revision)
        if not commit_changes_path(repository_root, source_change, row["review_path"]):
            raise ReconciliationError(
                f"{thread_id}: source change does not change {row['review_path']}"
            )
        if relationship == "removed-source":
            if path_exists(repository_root, evidence_commit, row["review_path"]):
                raise ReconciliationError(
                    f"{thread_id}: removed source survives evidence"
                )
            if path_exists(repository_root, revision, row["review_path"]):
                raise ReconciliationError(
                    f"{thread_id}: removed source survives current main"
                )
            identity = "removed"
            current_owner_change = ""
            current_owner_kind = ""
            current_owner_evidence_commit = ""
            current_owner_pr = ""
            current_owner_pr_title = ""
            current_owner_merged_at = ""
            current_owner_subject = ""
        else:
            if not current_target:
                raise ReconciliationError(f"{thread_id}: current target is empty")
            current_identity = path_identity(repository_root, revision, current_target)
            identity = current_identity
            current_owner_change, _current_owner_target = current_owners[thread_id]
            validate_commit_ancestry(repository_root, current_owner_change, revision)
            current_owner_association = associated.get(current_owner_change)
            if current_owner_association is None:
                if (
                    evidence_kind != "direct-main"
                    or current_owner_change != evidence_commit
                ):
                    raise ReconciliationError(
                        f"{thread_id}: current source owner has no unique merged PR"
                    )
                current_owner_kind = "direct-main"
                current_owner_evidence_commit = current_owner_change
                current_owner_pr = ""
                current_owner_pr_title = ""
                current_owner_merged_at = ""
            else:
                current_owner_kind = "merged-pr"
                current_owner_evidence_commit = current_owner_association[
                    "evidence_commit"
                ]
                current_owner_pr = current_owner_association["evidence_pr"]
                current_owner_pr_title = current_owner_association["evidence_pr_title"]
                current_owner_merged_at = current_owner_association[
                    "evidence_merged_at"
                ]
                validate_commit_ancestry(
                    repository_root, current_owner_evidence_commit, revision
                )
            current_owner_subject = source_change_subject(
                repository_root, current_owner_change
            )
        if evidence_kind == "merged-pr":
            if not evidence_pr.isdecimal() or int(evidence_pr) <= 0 or not merged_at:
                raise ReconciliationError(
                    f"{thread_id}: merged PR evidence is incomplete"
                )
            if thread_id in overrides:
                if evidence_pr not in override_pull_requests:
                    override_pull_requests[evidence_pr] = graphql_merged_pull_request(
                        evidence_pr
                    )
                merged_pull_request = override_pull_requests[evidence_pr]
                if (
                    merged_pull_request["evidence_commit"] != evidence_commit
                    or merged_pull_request["evidence_merged_at"] != merged_at
                ):
                    raise ReconciliationError(
                        f"{thread_id}: merged PR metadata differs from GitHub"
                    )
                evidence_pr_title = merged_pull_request["evidence_pr_title"]
        elif evidence_pr or merged_at:
            raise ReconciliationError(
                f"{thread_id}: direct-main evidence has PR metadata"
            )
        result.append(
            {
                **row,
                "resolution_state": "resolved",
                "relationship": relationship,
                "source_change_commit": source_change,
                "evidence_kind": evidence_kind,
                "evidence_commit": evidence_commit,
                "evidence_pr": evidence_pr,
                "evidence_pr_title": evidence_pr_title,
                "evidence_merged_at": merged_at,
                "current_target": current_target,
                "current_target_identity": identity,
                "current_owner_change_commit": current_owner_change,
                "current_owner_evidence_kind": current_owner_kind,
                "current_owner_evidence_commit": current_owner_evidence_commit,
                "current_owner_evidence_pr": current_owner_pr,
                "current_owner_evidence_pr_title": current_owner_pr_title,
                "current_owner_evidence_merged_at": current_owner_merged_at,
                "current_owner_change_subject": current_owner_subject,
                "source_change_subject": source_change_subject(
                    repository_root, source_change
                ),
                "verification_method": (
                    "Exact GitHub thread re-query plus resolution-source and "
                    "current-source ancestry at the recorded target identity"
                ),
            }
        )
    return result


def read_overrides(path: Path) -> dict[str, dict[str, str]]:
    rows = read_tsv(path, OVERRIDE_FIELDS)
    result = {row["thread_id"]: row for row in rows}
    if len(result) != len(rows):
        raise ReconciliationError("override table repeats a thread ID")
    return result


def write_output(
    output_dir: Path,
    corpus_dir: Path,
    cluster_dir: Path,
    revision: str,
    frontier_path: Path,
    overrides_path: Path,
    rows: list[dict[str, str]],
) -> None:
    allowed_inputs = {
        frontier_path.resolve(),
        overrides_path.resolve(),
        (output_dir / "README.md").resolve(),
    }
    unexpected_files = (
        [path for path in output_dir.iterdir() if path.resolve() not in allowed_inputs]
        if output_dir.exists()
        else []
    )
    if unexpected_files:
        raise ReconciliationError(
            f"output directory has unexpected files: {sorted(str(path) for path in unexpected_files)}"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    evidence_path = output_dir / "resolution-evidence.tsv"
    write_tsv(evidence_path, EVIDENCE_FIELDS, rows)
    manifest = {
        "schema": SCHEMA,
        "corpus_manifest_sha256": sha256_path(corpus_dir / "manifest.json"),
        "cluster_status_sha256": sha256_path(cluster_dir / "review-status.tsv"),
        "frontier_sha256": sha256_path(frontier_path),
        "overrides_sha256": sha256_path(overrides_path),
        "revision": revision,
        "thread_count": len(rows),
        "files": {"resolution-evidence.tsv": sha256_path(evidence_path)},
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def check_output(
    output_dir: Path,
    corpus_dir: Path,
    cluster_dir: Path,
    revision: str,
    frontier_path: Path,
    overrides_path: Path,
    rows: list[dict[str, str]],
) -> None:
    manifest_path = output_dir / "manifest.json"
    evidence_path = output_dir / "resolution-evidence.tsv"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_manifest = {
        "schema": SCHEMA,
        "corpus_manifest_sha256": sha256_path(corpus_dir / "manifest.json"),
        "cluster_status_sha256": sha256_path(cluster_dir / "review-status.tsv"),
        "frontier_sha256": sha256_path(frontier_path),
        "overrides_sha256": sha256_path(overrides_path),
        "revision": revision,
        "thread_count": len(rows),
        "files": {"resolution-evidence.tsv": sha256_path(evidence_path)},
    }
    if manifest != expected_manifest:
        raise ReconciliationError("reconciliation manifest differs from replay")
    if read_tsv(evidence_path, EVIDENCE_FIELDS) != rows:
        raise ReconciliationError("reconciliation evidence differs from replay")


def validate_frontier_membership(
    frontier: list[dict[str, str]], corpus_dir: Path, cluster_dir: Path
) -> None:
    expected = capture_rows(corpus_dir, cluster_dir)
    if frontier != expected:
        raise ReconciliationError("reconciliation frontier differs from its capture")


def main() -> int:
    arguments = parse_args()
    try:
        repository_root = arguments.repo_root.resolve()
        if (
            Path(run_git(repository_root, "rev-parse", "--show-toplevel")).resolve()
            != repository_root
        ):
            raise ReconciliationError("--repo-root is not the Git worktree root")
        revision = resolve_revision(repository_root, arguments.revision)
        review_thread_corpus.check_capture(arguments.corpus_dir)
        if arguments.command == "capture":
            rows = capture_rows(arguments.corpus_dir, arguments.cluster_dir)
            if arguments.frontier.exists():
                raise ReconciliationError("reconciliation frontier already exists")
            write_tsv(arguments.frontier, FRONTIER_FIELDS, rows)
        else:
            frontier = read_tsv(arguments.frontier, FRONTIER_FIELDS)
            validate_frontier_membership(
                frontier, arguments.corpus_dir, arguments.cluster_dir
            )
            rows = build_rows(
                repository_root, revision, frontier, read_overrides(arguments.overrides)
            )
            if arguments.command == "build":
                write_output(
                    arguments.output_dir,
                    arguments.corpus_dir,
                    arguments.cluster_dir,
                    revision,
                    arguments.frontier,
                    arguments.overrides,
                    rows,
                )
            else:
                check_output(
                    arguments.output_dir,
                    arguments.corpus_dir,
                    arguments.cluster_dir,
                    revision,
                    arguments.frontier,
                    arguments.overrides,
                    rows,
                )
    except (OSError, ValueError, json.JSONDecodeError, ReconciliationError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"OK: reconciled {len(rows)} exact resolved review threads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
