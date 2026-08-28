#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Group unresolved review work by source history and validation gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import review_thread_corpus

ANALYSIS_SCHEMA = "mesa-review-thread-group-history-v1"
HISTORY_FIELDS = (
    "work_group_id",
    "history_group_id",
    "review_path",
    "source_anchor",
    "thread_count",
    "review_source_count",
    "available_review_source_count",
    "unavailable_review_source_count",
    "unchanged_review_source_count",
    "changed_review_source_count",
    "merged_anchor_count",
    "current_path_state",
    "path_change_candidate_count",
    "path_change_candidates",
    "single_change_candidate",
    "validation_gate",
    "history_state",
    "next_action",
)


class HistoryError(ValueError):
    """The source-history grouping or its replay contract is invalid."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("build", "check"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--corpus-dir", type=Path, required=True)
        command_parser.add_argument("--repo-root", type=Path, required=True)
        command_parser.add_argument("--revision", default="HEAD")
        command_parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def run_git(
    repository_root: Path,
    *arguments: str,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if check and completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise HistoryError(f"git {' '.join(arguments)} failed: {diagnostic}")
    return completed


def resolve_commit(repository_root: Path, revision: str) -> str:
    completed = run_git(
        repository_root,
        "rev-parse",
        "--verify",
        f"{revision}^{{commit}}",
    )
    commit = completed.stdout.strip()
    if not review_thread_corpus.COMMIT_OID.fullmatch(commit):
        raise HistoryError(f"revision {revision!r} did not resolve to a commit")
    return commit


def commit_exists(repository_root: Path, commit: str) -> bool:
    existence = run_git(
        repository_root,
        "cat-file",
        "-e",
        f"{commit}^{{commit}}",
        check=False,
    )
    return existence.returncode == 0


def commit_is_reachable(repository_root: Path, ancestor: str, revision: str) -> bool:
    if not commit_exists(repository_root, ancestor):
        return False
    completed = run_git(
        repository_root,
        "merge-base",
        "--is-ancestor",
        ancestor,
        revision,
        check=False,
    )
    if completed.returncode not in (0, 1):
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise HistoryError(f"git ancestry query failed: {diagnostic}")
    return completed.returncode == 0


def blob_identity(repository_root: Path, revision: str, review_path: str) -> str | None:
    completed = run_git(
        repository_root,
        "rev-parse",
        "--verify",
        f"{revision}:{review_path}",
        check=False,
    )
    if completed.returncode != 0:
        return None
    identity = completed.stdout.strip()
    if not re_full_hex(identity):
        raise HistoryError(f"{revision}:{review_path} did not resolve to a Git object")
    return identity


def re_full_hex(value: str) -> bool:
    return len(value) == 40 and all(
        character in "0123456789abcdef" for character in value
    )


def path_change_commits(
    repository_root: Path,
    review_commit: str,
    revision: str,
    review_path: str,
) -> tuple[str, ...]:
    completed = run_git(
        repository_root,
        "log",
        "--format=%H",
        "--reverse",
        "--ancestry-path",
        f"{review_commit}..{revision}",
        "--",
        review_path,
    )
    commits = tuple(line for line in completed.stdout.splitlines() if line)
    if any(not review_thread_corpus.COMMIT_OID.fullmatch(commit) for commit in commits):
        raise HistoryError(f"{review_path}: Git log returned an invalid commit")
    return commits


def select_review_source(
    repository_root: Path,
    review_commit: str,
    original_commit: str,
) -> str | None:
    candidates = []
    for candidate in (review_commit, original_commit):
        if candidate and candidate not in candidates:
            candidates.append(candidate)
    for candidate in candidates:
        if not review_thread_corpus.COMMIT_OID.fullmatch(candidate):
            continue
        if commit_exists(repository_root, candidate):
            return candidate
    return None


def validation_gate(review_path: str) -> str:
    path = Path(review_path)
    suffix = path.suffix.lower()
    basename = path.name.lower()
    if suffix in (".md", ".rst", ".txt"):
        return "documentation-symbol-and-link-check"
    if suffix == ".py":
        return "python-test-black-and-ruff"
    if suffix in (".sh", ".bash"):
        return "shellcheck-and-script-test"
    if (
        basename in ("makefile", "meson.build", "meson_options.txt")
        or suffix == ".meson"
    ):
        return "configure-build-and-artifact-check"
    if suffix in (".c", ".cc", ".cpp", ".h", ".hpp"):
        if any(
            component in review_path
            for component in (
                "/r300/",
                "/r600/",
                "/terascale/",
                "/gallium/drivers/r300/",
                "/gallium/drivers/r600/",
            )
        ):
            return "warning-clean-driver-build-and-focused-test"
        return "warning-clean-build-and-focused-test"
    if suffix in (".json", ".toml", ".tsv", ".yaml", ".yml"):
        return "schema-generator-and-mutation-check"
    return "repository-native-focused-check"


def next_action(history_state: str, path_change_candidate_count: int) -> str:
    if history_state == "current-path-missing":
        return "locate-replacement-or-prove-supersession"
    if history_state == "review-source-unavailable":
        return "fetch-review-source-or-inspect-retained-diff"
    if history_state == "changed-without-merged-anchor":
        return "compare-reviewed-and-current-blobs-without-ancestry-claim"
    if history_state == "partial-history":
        return "complete-review-source-history-before-disposition"
    if history_state == "unchanged-since-review":
        return "compare-current-code-with-review-claim"
    if path_change_candidate_count == 1:
        return "verify-single-path-change-against-review-claims"
    return "inspect-path-change-sequence-against-review-claims"


def group_threads(
    corpus_dir: Path,
) -> tuple[
    list[dict[str, Any]],
    list[dict[str, str]],
    list[dict[str, str]],
]:
    records = review_thread_corpus.validate_records(
        review_thread_corpus.read_json(
            review_thread_corpus.thread_records_path(corpus_dir)
        )
    )
    memberships = review_thread_corpus.read_tsv(
        corpus_dir / "group-members.tsv",
        review_thread_corpus.GROUP_MEMBER_FIELDS,
    )
    work_groups = review_thread_corpus.read_tsv(
        corpus_dir / "work-groups.tsv",
        review_thread_corpus.WORK_GROUP_FIELDS,
    )
    if {record["thread_id"] for record in records} != {
        membership["thread_id"] for membership in memberships
    }:
        raise HistoryError("corpus and work-group membership differ")
    return records, memberships, work_groups


def build_history_rows(
    corpus_dir: Path,
    repository_root: Path,
    revision: str,
) -> list[dict[str, str]]:
    records, memberships, work_groups = group_threads(corpus_dir)
    records_by_thread = {record["thread_id"]: record for record in records}
    memberships_by_group: dict[str, list[dict[str, str]]] = {}
    for membership in memberships:
        memberships_by_group.setdefault(membership["work_group_id"], []).append(
            membership
        )
    path_blob_cache: dict[tuple[str, str], str | None] = {}
    history_cache: dict[tuple[str, str], tuple[str, ...]] = {}
    commit_time_cache: dict[str, int] = {}

    def cached_blob(commit: str, review_path: str) -> str | None:
        key = (commit, review_path)
        if key not in path_blob_cache:
            path_blob_cache[key] = blob_identity(repository_root, commit, review_path)
        return path_blob_cache[key]

    def cached_history(commit: str, review_path: str) -> tuple[str, ...]:
        key = (commit, review_path)
        if key not in history_cache:
            history_cache[key] = path_change_commits(
                repository_root, commit, revision, review_path
            )
        return history_cache[key]

    def commit_time(commit: str) -> int:
        if commit not in commit_time_cache:
            completed = run_git(repository_root, "show", "-s", "--format=%ct", commit)
            value = completed.stdout.strip()
            if not value.isdecimal():
                raise HistoryError(f"commit {commit} has an invalid timestamp")
            commit_time_cache[commit] = int(value)
        return commit_time_cache[commit]

    output: list[dict[str, str]] = []
    for group in work_groups:
        group_id = group["work_group_id"]
        review_path = group["review_path"]
        current_blob = cached_blob(revision, review_path)
        review_sources: set[tuple[str, str]] = set()
        unavailable_sources: set[str] = set()
        for membership in memberships_by_group[group_id]:
            record = records_by_thread[membership["thread_id"]]
            first_comment = record["thread"]["comments"][0]
            source = select_review_source(
                repository_root,
                first_comment["commit_oid"] or "",
                first_comment["original_commit_oid"] or "",
            )
            if source is None:
                unavailable_sources.add(
                    first_comment["commit_oid"]
                    or first_comment["original_commit_oid"]
                    or record["thread_id"]
                )
            else:
                merge_anchor = record["pull_request"]["merge_commit_oid"] or ""
                if merge_anchor and not commit_is_reachable(
                    repository_root, merge_anchor, revision
                ):
                    merge_anchor = ""
                review_sources.add((source, merge_anchor))

        unchanged_sources = 0
        changed_sources = 0
        changed_without_anchor = 0
        path_change_candidates: set[str] = set()
        available_sources = 0
        for review_source, merge_anchor in review_sources:
            review_blob = cached_blob(review_source, review_path)
            if review_blob is None:
                unavailable_sources.add(review_source)
                continue
            available_sources += 1
            if current_blob is None:
                continue
            if review_blob == current_blob:
                unchanged_sources += 1
            else:
                changed_sources += 1
                if not merge_anchor:
                    changed_without_anchor += 1
                    continue
                merge_blob = cached_blob(merge_anchor, review_path)
                if merge_blob != review_blob:
                    path_change_candidates.add(merge_anchor)
                path_change_candidates.update(cached_history(merge_anchor, review_path))
        ordered_candidates = sorted(
            path_change_candidates, key=lambda commit: (commit_time(commit), commit)
        )
        if current_blob is None:
            history_state = "current-path-missing"
            current_path_state = "missing"
        else:
            current_path_state = "present"
            if not available_sources:
                history_state = "review-source-unavailable"
            elif unavailable_sources:
                history_state = "partial-history"
            elif changed_without_anchor:
                if changed_without_anchor == changed_sources:
                    history_state = "changed-without-merged-anchor"
                else:
                    history_state = "partial-history"
            elif changed_sources:
                history_state = "changed-since-review"
            else:
                history_state = "unchanged-since-review"
        gate = validation_gate(review_path)
        history_key = "\0".join((history_state, gate, *ordered_candidates))
        history_group_id = review_thread_corpus.stable_identifier(
            "history", f"{history_state}-{gate}", history_key
        )
        candidate_count = len(ordered_candidates)
        output.append(
            {
                "work_group_id": group_id,
                "history_group_id": history_group_id,
                "review_path": review_path,
                "source_anchor": group["source_anchor"],
                "thread_count": group["thread_count"],
                "review_source_count": str(
                    available_sources + len(unavailable_sources)
                ),
                "available_review_source_count": str(available_sources),
                "unavailable_review_source_count": str(len(unavailable_sources)),
                "unchanged_review_source_count": str(unchanged_sources),
                "changed_review_source_count": str(changed_sources),
                "merged_anchor_count": str(
                    len({anchor for _source, anchor in review_sources if anchor})
                ),
                "current_path_state": current_path_state,
                "path_change_candidate_count": str(candidate_count),
                "path_change_candidates": ";".join(ordered_candidates),
                "single_change_candidate": (
                    ordered_candidates[0] if candidate_count == 1 else ""
                ),
                "validation_gate": gate,
                "history_state": history_state,
                "next_action": next_action(history_state, candidate_count),
            }
        )
    return output


def build_summary(rows: list[dict[str, str]]) -> dict[str, Any]:
    history_states = Counter(row["history_state"] for row in rows)
    validation_gates = Counter(row["validation_gate"] for row in rows)
    return {
        "work_group_count": len(rows),
        "thread_count": sum(int(row["thread_count"]) for row in rows),
        "single_change_candidate_groups": sum(
            bool(row["single_change_candidate"]) for row in rows
        ),
        "history_state_counts": dict(sorted(history_states.items())),
        "validation_gate_counts": dict(sorted(validation_gates.items())),
    }


def json_bytes(payload: Any) -> bytes:
    return (
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("utf-8")


def write_tsv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=HISTORY_FIELDS,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def sha256_path(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_analysis(
    output_dir: Path,
    corpus_dir: Path,
    revision: str,
    rows: list[dict[str, str]],
) -> None:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise HistoryError(f"output directory is not empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    history_path = output_dir / "work-group-history.tsv"
    summary_path = output_dir / "summary.json"
    write_tsv(history_path, rows)
    summary_path.write_bytes(json_bytes(build_summary(rows)))
    manifest = {
        "schema": ANALYSIS_SCHEMA,
        "generated_at": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "corpus_manifest_sha256": sha256_path(corpus_dir / "manifest.json"),
        "revision": revision,
        "work_group_count": len(rows),
        "files": {
            "summary.json": sha256_path(summary_path),
            "work-group-history.tsv": sha256_path(history_path),
        },
    }
    (output_dir / "manifest.json").write_bytes(json_bytes(manifest))


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        if tuple(reader.fieldnames or ()) != HISTORY_FIELDS:
            raise HistoryError(f"{path}: header differs from its schema")
        rows = list(reader)
    for row_number, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise HistoryError(f"{path}:{row_number}: malformed TSV row")
    return rows


def check_analysis(
    output_dir: Path,
    corpus_dir: Path,
    repository_root: Path,
    revision: str,
) -> None:
    manifest = review_thread_corpus.read_json(output_dir / "manifest.json")
    if not isinstance(manifest, dict) or manifest.get("schema") != ANALYSIS_SCHEMA:
        raise HistoryError("history manifest has an unexpected schema")
    review_thread_corpus.parse_timestamp(
        manifest.get("generated_at"), "history manifest generated_at"
    )
    if manifest.get("corpus_manifest_sha256") != sha256_path(
        corpus_dir / "manifest.json"
    ):
        raise HistoryError("history manifest names a different corpus")
    if manifest.get("revision") != revision:
        raise HistoryError("history manifest names a different revision")
    expected_files = {
        "summary.json": output_dir / "summary.json",
        "work-group-history.tsv": output_dir / "work-group-history.tsv",
    }
    declared_files = manifest.get("files")
    if not isinstance(declared_files, dict) or set(declared_files) != set(
        expected_files
    ):
        raise HistoryError("history manifest file membership differs")
    for relative_path, path in expected_files.items():
        if declared_files[relative_path] != sha256_path(path):
            raise HistoryError(f"history hash differs for {relative_path}")
    expected_rows = build_history_rows(corpus_dir, repository_root, revision)
    retained_rows = read_tsv(output_dir / "work-group-history.tsv")
    if retained_rows != expected_rows:
        raise HistoryError("retained history rows differ from source replay")
    expected_summary = build_summary(expected_rows)
    if review_thread_corpus.read_json(output_dir / "summary.json") != expected_summary:
        raise HistoryError("retained history summary differs from source replay")
    if manifest.get("work_group_count") != len(expected_rows):
        raise HistoryError("history manifest work-group count differs")


def main() -> int:
    arguments = parse_args()
    try:
        repository_root = arguments.repo_root.resolve()
        discovered_root = Path(
            run_git(repository_root, "rev-parse", "--show-toplevel").stdout.strip()
        ).resolve()
        if discovered_root != repository_root:
            raise HistoryError("--repo-root is not the Git worktree root")
        revision = resolve_commit(repository_root, arguments.revision)
        review_thread_corpus.check_capture(arguments.corpus_dir)
        if arguments.command == "build":
            rows = build_history_rows(arguments.corpus_dir, repository_root, revision)
            write_analysis(arguments.output_dir, arguments.corpus_dir, revision, rows)
        check_analysis(
            arguments.output_dir,
            arguments.corpus_dir,
            repository_root,
            revision,
        )
    except (
        HistoryError,
        review_thread_corpus.CorpusError,
        OSError,
        ValueError,
    ) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    summary = review_thread_corpus.read_json(arguments.output_dir / "summary.json")
    print(
        f"OK: grouped {summary['thread_count']} threads into "
        f"{summary['work_group_count']} source-history groups"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
