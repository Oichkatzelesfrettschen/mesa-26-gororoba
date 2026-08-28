#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cluster the complete unresolved-review corpus into shared source reads."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import review_thread_corpus
import review_thread_group_history

CLUSTER_SCHEMA = "mesa-review-thread-clusters-v1"
ROUTE_ORDER = {
    "one-change-commit": 0,
    "unchanged-source": 1,
    "shared-change-history": 2,
    "closed-without-merge": 3,
    "missing-current-path": 4,
    "recover-review-source": 5,
}
CLUSTER_FIELDS = (
    "cluster_rank",
    "cluster_id",
    "review_route",
    "review_group_count",
    "thread_count",
    "review_path_count",
    "pull_request_count",
    "oldest_thread_created_at",
    "newest_thread_created_at",
    "validation_gates",
    "history_states",
    "path_change_candidates",
    "representative_work_group_id",
    "next_action",
)
MEMBER_FIELDS = (
    "cluster_rank",
    "cluster_id",
    "work_group_id",
    "oldest_thread_created_at",
    "review_path",
    "source_anchor",
    "thread_count",
    "pull_request_numbers",
    "thread_ids",
    "claim_headings",
    "validation_gate",
    "history_state",
)
STATUS_FIELDS = (
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
STATUS_VALUES = {
    "unassessed",
    "fixed-on-main",
    "superseded-on-main",
    "repair-required",
    "evidence-required",
}
VERIFICATION_RESULTS = {"", "not-run", "pass", "blocked"}
CLOSURE_STATES = {"open", "ready-to-resolve"}


class ClusterError(ValueError):
    """The review clustering or status ledger is invalid."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("build", "check"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--corpus-dir", type=Path, required=True)
        command_parser.add_argument("--history-dir", type=Path, required=True)
        command_parser.add_argument("--repo-root", type=Path, required=True)
        command_parser.add_argument("--revision", required=True)
        command_parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def json_list(values: list[str]) -> str:
    return json.dumps(values, ensure_ascii=True, separators=(",", ":"))


def review_route(history: dict[str, str]) -> str:
    state = history["history_state"]
    candidate_count = int(history["path_change_candidate_count"])
    if state == "changed-since-review":
        if candidate_count == 1:
            return "one-change-commit"
        return "shared-change-history"
    if state == "unchanged-since-review":
        return "unchanged-source"
    if state == "changed-without-merged-anchor":
        return "closed-without-merge"
    if state == "current-path-missing":
        return "missing-current-path"
    if state in ("partial-history", "review-source-unavailable"):
        return "recover-review-source"
    raise ClusterError(f"unsupported history state {state!r}")


def cluster_key(
    history: dict[str, str], records: list[dict[str, Any]]
) -> tuple[str, str]:
    route = review_route(history)
    if route in ("one-change-commit", "shared-change-history"):
        detail = history["path_change_candidates"]
    elif route in ("unchanged-source", "missing-current-path"):
        detail = history["review_path"]
    else:
        sources: set[str] = set()
        for record in records:
            pull_request = record["pull_request"]
            comment = record["thread"]["comments"][0]
            source = comment["commit_oid"] or comment["original_commit_oid"] or "none"
            sources.add(f"{pull_request['number']}:{source}")
        detail = ";".join(sorted(sources))
    if not detail:
        raise ClusterError(f"{history['work_group_id']}: empty cluster key")
    return route, detail


def stable_cluster_id(route: str, detail: str) -> str:
    return review_thread_corpus.stable_identifier(
        "review", f"{route}-{detail}", f"{route}\0{detail}"
    )


def validate_join(
    records: list[dict[str, Any]],
    memberships: list[dict[str, str]],
    work_groups: list[dict[str, str]],
    histories: list[dict[str, str]],
) -> tuple[
    dict[str, dict[str, Any]],
    dict[str, list[dict[str, Any]]],
    dict[str, dict[str, str]],
    dict[str, dict[str, str]],
]:
    records_by_thread = {record["thread_id"]: record for record in records}
    if len(records_by_thread) != len(records):
        raise ClusterError("review corpus repeats a thread ID")
    work_groups_by_id = {row["work_group_id"]: row for row in work_groups}
    histories_by_id = {row["work_group_id"]: row for row in histories}
    if len(work_groups_by_id) != len(work_groups):
        raise ClusterError("review corpus repeats a work-group ID")
    if len(histories_by_id) != len(histories):
        raise ClusterError("history analysis repeats a work-group ID")
    if set(work_groups_by_id) != set(histories_by_id):
        raise ClusterError("corpus and history work-group membership differ")
    records_by_group: dict[str, list[dict[str, Any]]] = {}
    seen_threads: set[str] = set()
    for membership in memberships:
        thread_id = membership["thread_id"]
        group_id = membership["work_group_id"]
        if thread_id in seen_threads:
            raise ClusterError(f"thread {thread_id} repeats in group membership")
        if thread_id not in records_by_thread:
            raise ClusterError(f"group membership names unknown thread {thread_id}")
        if group_id not in work_groups_by_id:
            raise ClusterError(f"group membership names unknown group {group_id}")
        seen_threads.add(thread_id)
        records_by_group.setdefault(group_id, []).append(records_by_thread[thread_id])
    if seen_threads != set(records_by_thread):
        raise ClusterError("group membership does not cover the review corpus")
    for group_id, group in work_groups_by_id.items():
        history = histories_by_id[group_id]
        if group["review_path"] != history["review_path"]:
            raise ClusterError(f"{group_id}: corpus and history paths differ")
        if group["source_anchor"] != history["source_anchor"]:
            raise ClusterError(f"{group_id}: corpus and history anchors differ")
        if int(group["thread_count"]) != len(records_by_group[group_id]):
            raise ClusterError(f"{group_id}: thread count differs")
    return records_by_thread, records_by_group, work_groups_by_id, histories_by_id


def build_rows(
    records: list[dict[str, Any]],
    memberships: list[dict[str, str]],
    work_groups: list[dict[str, str]],
    histories: list[dict[str, str]],
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    (
        _records_by_thread,
        records_by_group,
        work_groups_by_id,
        histories_by_id,
    ) = validate_join(records, memberships, work_groups, histories)
    grouped: dict[tuple[str, str], list[str]] = {}
    for group_id, history in histories_by_id.items():
        key = cluster_key(history, records_by_group[group_id])
        grouped.setdefault(key, []).append(group_id)
    ordered_clusters = sorted(
        grouped.items(),
        key=lambda item: (
            ROUTE_ORDER[item[0][0]],
            min(
                work_groups_by_id[group_id]["oldest_thread_created_at"]
                for group_id in item[1]
            ),
            stable_cluster_id(*item[0]),
        ),
    )
    cluster_rows: list[dict[str, str]] = []
    member_rows: list[dict[str, str]] = []
    for cluster_rank, ((route, detail), group_ids) in enumerate(
        ordered_clusters, start=1
    ):
        cluster_id = stable_cluster_id(route, detail)
        group_ids.sort(
            key=lambda group_id: (
                work_groups_by_id[group_id]["oldest_thread_created_at"],
                group_id,
            )
        )
        cluster_records = [
            record for group_id in group_ids for record in records_by_group[group_id]
        ]
        pull_requests = sorted(
            {record["pull_request"]["number"] for record in cluster_records}
        )
        paths = sorted(
            {work_groups_by_id[group_id]["review_path"] for group_id in group_ids}
        )
        gates = sorted(
            {histories_by_id[group_id]["validation_gate"] for group_id in group_ids}
        )
        states = sorted(
            {histories_by_id[group_id]["history_state"] for group_id in group_ids}
        )
        candidate_sequence = histories_by_id[group_ids[0]]["path_change_candidates"]
        if any(
            histories_by_id[group_id]["path_change_candidates"] != candidate_sequence
            for group_id in group_ids[1:]
        ):
            raise ClusterError(f"{cluster_id}: clustered history sequences differ")
        candidates = [
            candidate for candidate in candidate_sequence.split(";") if candidate
        ]
        next_action = {
            "one-change-commit": "compare-the-review-claim-with-the-single-later-commit",
            "unchanged-source": "compare-each-review-claim-with-current-source",
            "shared-change-history": "read-the-shared-commit-sequence-once-then-check-each-claim",
            "closed-without-merge": "compare-the-closed-pr-source-with-current-main",
            "missing-current-path": "locate-the-replacement-or-prove-removal-resolved-the-claim",
            "recover-review-source": "fetch-the-named-pr-source-before-classification",
        }[route]
        cluster_rows.append(
            {
                "cluster_rank": str(cluster_rank),
                "cluster_id": cluster_id,
                "review_route": route,
                "review_group_count": str(len(group_ids)),
                "thread_count": str(len(cluster_records)),
                "review_path_count": str(len(paths)),
                "pull_request_count": str(len(pull_requests)),
                "oldest_thread_created_at": min(
                    record["thread_created_at"] for record in cluster_records
                ),
                "newest_thread_created_at": max(
                    record["thread_created_at"] for record in cluster_records
                ),
                "validation_gates": json_list(gates),
                "history_states": json_list(states),
                "path_change_candidates": json_list(candidates),
                "representative_work_group_id": group_ids[0],
                "next_action": next_action,
            }
        )
        for group_id in group_ids:
            group = work_groups_by_id[group_id]
            history = histories_by_id[group_id]
            group_records = records_by_group[group_id]
            group_records.sort(
                key=lambda record: (record["thread_created_at"], record["thread_id"])
            )
            member_rows.append(
                {
                    "cluster_rank": str(cluster_rank),
                    "cluster_id": cluster_id,
                    "work_group_id": group_id,
                    "oldest_thread_created_at": group["oldest_thread_created_at"],
                    "review_path": group["review_path"],
                    "source_anchor": group["source_anchor"],
                    "thread_count": group["thread_count"],
                    "pull_request_numbers": json_list(
                        [
                            str(pull_request_number)
                            for pull_request_number in sorted(
                                {
                                    record["pull_request"]["number"]
                                    for record in group_records
                                }
                            )
                        ]
                    ),
                    "thread_ids": json_list(
                        [record["thread_id"] for record in group_records]
                    ),
                    "claim_headings": json_list(
                        [
                            review_thread_corpus.durable_display_label(
                                review_thread_corpus.clean_claim_heading(
                                    record["thread"]["comments"][0]["body"]
                                ),
                                "unlabeled review claim",
                            )
                            for record in group_records
                        ]
                    ),
                    "validation_gate": history["validation_gate"],
                    "history_state": history["history_state"],
                }
            )
    return cluster_rows, member_rows


def build_summary(
    cluster_rows: list[dict[str, str]], member_rows: list[dict[str, str]]
) -> dict[str, Any]:
    route_clusters = Counter(row["review_route"] for row in cluster_rows)
    route_groups = Counter()
    route_threads = Counter()
    route_by_cluster = {row["cluster_id"]: row["review_route"] for row in cluster_rows}
    for row in member_rows:
        route = route_by_cluster[row["cluster_id"]]
        route_groups[route] += 1
        route_threads[route] += int(row["thread_count"])
    return {
        "cluster_count": len(cluster_rows),
        "review_group_count": len(member_rows),
        "thread_count": sum(int(row["thread_count"]) for row in member_rows),
        "route_cluster_counts": dict(sorted(route_clusters.items())),
        "route_review_group_counts": dict(sorted(route_groups.items())),
        "route_thread_counts": dict(sorted(route_threads.items())),
    }


def write_tsv(
    path: Path,
    fields: tuple[str, ...],
    rows: list[dict[str, str]],
    *,
    quote_all: bool = False,
) -> None:
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=fields,
            delimiter="\t",
            lineterminator="\n",
            quoting=csv.QUOTE_ALL if quote_all else csv.QUOTE_MINIMAL,
        )
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path: Path, fields: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.DictReader(input_file, delimiter="\t")
        if tuple(reader.fieldnames or ()) != fields:
            raise ClusterError(f"{path}: header differs from its schema")
        rows = list(reader)
    for row_number, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise ClusterError(f"{path}:{row_number}: malformed TSV row")
    return rows


def status_scaffold(member_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [
        {
            "work_group_id": row["work_group_id"],
            "status": "unassessed",
            "evidence_commit": "",
            "evidence_locator": "",
            "discovery_command": "",
            "verification_command": "",
            "verification_result": "",
            "closure_state": "open",
            "reason": "",
            "falsifier": "",
        }
        for row in member_rows
    ]


def sha256_path(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_directory_membership(directory: Path, expected_names: set[str]) -> None:
    actual_names = {path.name for path in directory.iterdir()}
    if actual_names != expected_names:
        raise ClusterError(f"{directory}: retained entry membership differs")


def json_bytes(payload: Any) -> bytes:
    return (
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("utf-8")


def write_output(
    output_dir: Path,
    corpus_dir: Path,
    history_dir: Path,
    disposition_revision: str,
    cluster_rows: list[dict[str, str]],
    member_rows: list[dict[str, str]],
) -> None:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise ClusterError(f"output directory is not empty: {output_dir}")
    generated_dir = output_dir / "generated"
    generated_dir.mkdir(parents=True, exist_ok=True)
    clusters_path = generated_dir / "review-clusters.tsv"
    members_path = generated_dir / "review-cluster-members.tsv"
    summary_path = generated_dir / "summary.json"
    write_tsv(clusters_path, CLUSTER_FIELDS, cluster_rows)
    write_tsv(members_path, MEMBER_FIELDS, member_rows)
    summary_path.write_bytes(json_bytes(build_summary(cluster_rows, member_rows)))
    write_tsv(
        output_dir / "review-status.tsv",
        STATUS_FIELDS,
        status_scaffold(member_rows),
        quote_all=True,
    )
    manifest = {
        "schema": CLUSTER_SCHEMA,
        "generated_at": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "corpus_manifest_sha256": sha256_path(corpus_dir / "manifest.json"),
        "history_manifest_sha256": sha256_path(history_dir / "manifest.json"),
        "disposition_revision": disposition_revision,
        "files": {
            "review-cluster-members.tsv": sha256_path(members_path),
            "review-clusters.tsv": sha256_path(clusters_path),
            "summary.json": sha256_path(summary_path),
        },
    }
    (generated_dir / "manifest.json").write_bytes(json_bytes(manifest))


def check_status(
    path: Path,
    member_rows: list[dict[str, str]],
    repository_root: Path,
    disposition_revision: str,
) -> None:
    rows = read_tsv(path, STATUS_FIELDS)
    expected_ids = [row["work_group_id"] for row in member_rows]
    actual_ids = [row["work_group_id"] for row in rows]
    if actual_ids != expected_ids:
        raise ClusterError("review status order or work-group membership differs")
    for row_number, row in enumerate(rows, start=2):
        status = row["status"]
        verification = row["verification_result"]
        closure = row["closure_state"]
        if status not in STATUS_VALUES:
            raise ClusterError(
                f"review-status.tsv:{row_number}: unknown status {status!r}"
            )
        if verification not in VERIFICATION_RESULTS:
            raise ClusterError(
                f"review-status.tsv:{row_number}: unknown verification result {verification!r}"
            )
        if closure not in CLOSURE_STATES:
            raise ClusterError(
                f"review-status.tsv:{row_number}: unknown closure state {closure!r}"
            )
        if status == "unassessed":
            if closure != "open" or any(
                row[field]
                for field in STATUS_FIELDS
                if field not in ("work_group_id", "status", "closure_state")
            ):
                raise ClusterError(
                    f"review-status.tsv:{row_number}: unassessed row carries a conclusion"
                )
            continue
        if not row["reason"] or not row["falsifier"] or not row["discovery_command"]:
            raise ClusterError(
                f"review-status.tsv:{row_number}: assessed row lacks reason, falsifier, "
                "or discovery command"
            )
        if status in ("fixed-on-main", "superseded-on-main"):
            if (
                not review_thread_corpus.COMMIT_OID.fullmatch(row["evidence_commit"])
                or not row["evidence_locator"]
            ):
                raise ClusterError(
                    f"review-status.tsv:{row_number}: resolved code status lacks merged proof"
                )
            if not review_thread_group_history.commit_is_reachable(
                repository_root, row["evidence_commit"], disposition_revision
            ):
                raise ClusterError(
                    f"review-status.tsv:{row_number}: evidence commit is not reachable "
                    "from the disposition revision"
                )
            if closure == "ready-to-resolve" and (
                not row["verification_command"] or verification != "pass"
            ):
                raise ClusterError(
                    f"review-status.tsv:{row_number}: closable row lacks a passing check"
                )
        elif closure != "open":
            raise ClusterError(
                f"review-status.tsv:{row_number}: unresolved status cannot close"
            )


def check_output(
    output_dir: Path,
    corpus_dir: Path,
    history_dir: Path,
    repository_root: Path,
    disposition_revision: str,
    cluster_rows: list[dict[str, str]],
    member_rows: list[dict[str, str]],
) -> None:
    review_thread_group_history.check_analysis(
        history_dir,
        corpus_dir,
        repository_root,
        disposition_revision,
    )
    generated_dir = output_dir / "generated"
    manifest = review_thread_corpus.read_json(generated_dir / "manifest.json")
    if not isinstance(manifest, dict) or manifest.get("schema") != CLUSTER_SCHEMA:
        raise ClusterError("cluster manifest has an unexpected schema")
    review_thread_corpus.parse_timestamp(
        manifest.get("generated_at"), "cluster manifest generated_at"
    )
    if manifest.get("corpus_manifest_sha256") != sha256_path(
        corpus_dir / "manifest.json"
    ):
        raise ClusterError("cluster manifest names a different corpus")
    if manifest.get("history_manifest_sha256") != sha256_path(
        history_dir / "manifest.json"
    ):
        raise ClusterError("cluster manifest names a different history analysis")
    if manifest.get("disposition_revision") != disposition_revision:
        raise ClusterError("cluster manifest names a different disposition revision")
    expected_files = {
        "review-cluster-members.tsv": generated_dir / "review-cluster-members.tsv",
        "review-clusters.tsv": generated_dir / "review-clusters.tsv",
        "summary.json": generated_dir / "summary.json",
    }
    declared_files = manifest.get("files")
    if not isinstance(declared_files, dict) or set(declared_files) != set(
        expected_files
    ):
        raise ClusterError("cluster manifest file membership differs")
    check_directory_membership(generated_dir, {"manifest.json", *expected_files})
    check_directory_membership(output_dir, {"generated", "review-status.tsv"})
    for name, path in expected_files.items():
        if declared_files[name] != sha256_path(path):
            raise ClusterError(f"cluster hash differs for {name}")
    if read_tsv(expected_files["review-clusters.tsv"], CLUSTER_FIELDS) != cluster_rows:
        raise ClusterError("retained review clusters differ from source replay")
    if (
        read_tsv(expected_files["review-cluster-members.tsv"], MEMBER_FIELDS)
        != member_rows
    ):
        raise ClusterError("retained cluster members differ from source replay")
    if review_thread_corpus.read_json(expected_files["summary.json"]) != build_summary(
        cluster_rows, member_rows
    ):
        raise ClusterError("retained cluster summary differs from source replay")
    check_status(
        output_dir / "review-status.tsv",
        member_rows,
        repository_root,
        disposition_revision,
    )


def main() -> int:
    arguments = parse_args()
    try:
        repository_root = arguments.repo_root.resolve()
        discovered_root = Path(
            review_thread_group_history.run_git(
                repository_root, "rev-parse", "--show-toplevel"
            ).stdout.strip()
        ).resolve()
        if discovered_root != repository_root:
            raise ClusterError("--repo-root is not the Git worktree root")
        disposition_revision = review_thread_group_history.resolve_commit(
            repository_root, arguments.revision
        )
        review_thread_corpus.check_capture(arguments.corpus_dir)
        histories = review_thread_group_history.read_tsv(
            arguments.history_dir / "work-group-history.tsv"
        )
        records, memberships, work_groups = review_thread_group_history.group_threads(
            arguments.corpus_dir
        )
        cluster_rows, member_rows = build_rows(
            records, memberships, work_groups, histories
        )
        if arguments.command == "build":
            write_output(
                arguments.output_dir,
                arguments.corpus_dir,
                arguments.history_dir,
                disposition_revision,
                cluster_rows,
                member_rows,
            )
        check_output(
            arguments.output_dir,
            arguments.corpus_dir,
            arguments.history_dir,
            repository_root,
            disposition_revision,
            cluster_rows,
            member_rows,
        )
    except (
        ClusterError,
        review_thread_corpus.CorpusError,
        review_thread_group_history.HistoryError,
        OSError,
        ValueError,
    ) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    summary = build_summary(cluster_rows, member_rows)
    print(
        f"OK: grouped {summary['thread_count']} threads and "
        f"{summary['review_group_count']} review groups into "
        f"{summary['cluster_count']} shared source reads"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
