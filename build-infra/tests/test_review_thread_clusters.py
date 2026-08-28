# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib
import subprocess
import sys
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_ROOT = BUILD_INFRA_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_ROOT))
review_thread_clusters = importlib.import_module("review_thread_clusters")
sys.path.pop(0)


def fixture_inputs() -> tuple[
    list[dict[str, object]],
    list[dict[str, str]],
    list[dict[str, str]],
    list[dict[str, str]],
]:
    records: list[dict[str, object]] = []
    memberships: list[dict[str, str]] = []
    work_groups: list[dict[str, str]] = []
    histories: list[dict[str, str]] = []

    def add_group(
        suffix: str,
        path: str,
        state: str,
        candidates: str,
        pull_request: int,
    ) -> None:
        thread_id = f"PRRT_fixture_{suffix}"
        group_id = f"work-fixture-{suffix}"
        timestamp = f"2026-08-{int(suffix) + 10:02d}T00:00:00Z"
        records.append(
            {
                "thread_id": thread_id,
                "thread_created_at": timestamp,
                "pull_request": {"number": pull_request, "state": "MERGED"},
                "thread": {
                    "comments": [
                        {
                            "body": f"**Fix mechanism {suffix}**\n\nDetails.",
                            "commit_oid": "1" * 40,
                            "original_commit_oid": "1" * 40,
                        }
                    ]
                },
            }
        )
        memberships.append({"thread_id": thread_id, "work_group_id": group_id})
        work_groups.append(
            {
                "work_group_id": group_id,
                "review_path": path,
                "source_anchor": f"fixture anchor {suffix}",
                "thread_count": "1",
                "oldest_thread_created_at": timestamp,
            }
        )
        histories.append(
            {
                "work_group_id": group_id,
                "review_path": path,
                "source_anchor": f"fixture anchor {suffix}",
                "path_change_candidate_count": str(
                    len(candidates.split(";")) if candidates else 0
                ),
                "path_change_candidates": candidates,
                "validation_gate": "python-test-black-and-ruff",
                "history_state": state,
            }
        )

    add_group("1", "src/shared_a.py", "changed-since-review", "2" * 40, 101)
    add_group("2", "src/shared_b.py", "changed-since-review", "2" * 40, 101)
    add_group("3", "src/unchanged.py", "unchanged-since-review", "", 102)
    add_group("4", "src/removed.py", "current-path-missing", "", 103)
    add_group("5", "src/closed.py", "changed-without-merged-anchor", "", 104)
    return records, memberships, work_groups, histories


def test_build_rows_clusters_shared_commit_once() -> None:
    cluster_rows, member_rows = review_thread_clusters.build_rows(*fixture_inputs())
    assert len(cluster_rows) == 4
    assert len(member_rows) == 5
    assert [row["review_route"] for row in cluster_rows] == [
        "one-change-commit",
        "unchanged-source",
        "closed-without-merge",
        "missing-current-path",
    ]
    assert cluster_rows[0]["review_group_count"] == "2"
    assert cluster_rows[0]["thread_count"] == "2"
    assert cluster_rows[0]["path_change_candidates"] == f'["{"2" * 40}"]'


def test_duplicate_thread_membership_is_rejected() -> None:
    records, memberships, work_groups, histories = fixture_inputs()
    memberships.append(dict(memberships[0]))
    with pytest.raises(review_thread_clusters.ClusterError, match="repeats"):
        review_thread_clusters.build_rows(records, memberships, work_groups, histories)


def test_shared_history_preserves_candidate_chronology() -> None:
    records, memberships, work_groups, histories = fixture_inputs()
    chronological_commits = ["3" * 40, "1" * 40, "2" * 40]
    candidate_sequence = ";".join(chronological_commits)
    for history in histories[:2]:
        history["path_change_candidate_count"] = "3"
        history["path_change_candidates"] = candidate_sequence
    cluster_rows, _member_rows = review_thread_clusters.build_rows(
        records, memberships, work_groups, histories
    )
    shared_history = next(
        row for row in cluster_rows if row["review_route"] == "shared-change-history"
    )
    assert shared_history["path_change_candidates"] == (
        review_thread_clusters.json_list(chronological_commits)
    )


def create_git_history(repository_root: Path) -> tuple[str, str]:
    repository_root.mkdir()

    def run_git(*arguments: str) -> str:
        completed = subprocess.run(
            ["git", "-C", str(repository_root), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
        return completed.stdout.strip()

    run_git("init", "--quiet")
    run_git("config", "user.name", "Review Fixture")
    run_git("config", "user.email", "review-fixture@example.invalid")
    (repository_root / "tracked.txt").write_text("base\n", encoding="utf-8")
    run_git("add", "tracked.txt")
    run_git("commit", "--quiet", "-m", "fixture: add disposition base")
    disposition_revision = run_git("rev-parse", "HEAD")
    run_git("checkout", "--quiet", "-b", "unmerged-evidence")
    (repository_root / "tracked.txt").write_text("unmerged\n", encoding="utf-8")
    run_git("commit", "--quiet", "-am", "fixture: add unmerged evidence")
    unmerged_evidence = run_git("rev-parse", "HEAD")
    return disposition_revision, unmerged_evidence


def test_closable_status_requires_a_passing_check(tmp_path: Path) -> None:
    _cluster_rows, member_rows = review_thread_clusters.build_rows(*fixture_inputs())
    status_rows = review_thread_clusters.status_scaffold(member_rows)
    repository_root = tmp_path / "repository"
    disposition_revision, _unmerged_evidence = create_git_history(repository_root)
    status_rows[0].update(
        {
            "status": "fixed-on-main",
            "closure_state": "ready-to-resolve",
            "evidence_commit": disposition_revision,
            "evidence_locator": "src/shared_a.py:fixture",
            "discovery_command": "rg --fixed-strings fixture src/shared_a.py",
            "reason": "The current implementation satisfies the review claim.",
            "falsifier": "The reviewed failure remains reproducible.",
        }
    )
    status_path = tmp_path / "review-status.tsv"
    review_thread_clusters.write_tsv(
        status_path, review_thread_clusters.STATUS_FIELDS, status_rows
    )
    with pytest.raises(review_thread_clusters.ClusterError, match="passing check"):
        review_thread_clusters.check_status(
            status_path, member_rows, repository_root, disposition_revision
        )


def test_fixed_status_rejects_unmerged_evidence(tmp_path: Path) -> None:
    _cluster_rows, member_rows = review_thread_clusters.build_rows(*fixture_inputs())
    status_rows = review_thread_clusters.status_scaffold(member_rows)
    repository_root = tmp_path / "repository"
    disposition_revision, unmerged_evidence = create_git_history(repository_root)
    status_rows[0].update(
        {
            "status": "fixed-on-main",
            "evidence_commit": unmerged_evidence,
            "evidence_locator": "src/shared_a.py:fixture",
            "discovery_command": "rg --fixed-strings fixture src/shared_a.py",
            "verification_result": "not-run",
            "reason": "The candidate branch carries the requested mechanism.",
            "falsifier": "The evidence commit reaches the disposition revision.",
        }
    )
    status_path = tmp_path / "review-status.tsv"
    review_thread_clusters.write_tsv(
        status_path, review_thread_clusters.STATUS_FIELDS, status_rows
    )
    with pytest.raises(review_thread_clusters.ClusterError, match="not reachable"):
        review_thread_clusters.check_status(
            status_path, member_rows, repository_root, disposition_revision
        )


def test_retained_directory_rejects_extra_entries(tmp_path: Path) -> None:
    (tmp_path / "expected.tsv").write_text("header\n", encoding="utf-8")
    (tmp_path / "stale.tsv").write_text("header\n", encoding="utf-8")
    with pytest.raises(review_thread_clusters.ClusterError, match="membership differs"):
        review_thread_clusters.check_directory_membership(tmp_path, {"expected.tsv"})
