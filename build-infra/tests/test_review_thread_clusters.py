# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib
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


def test_closable_status_requires_a_passing_check(tmp_path: Path) -> None:
    _cluster_rows, member_rows = review_thread_clusters.build_rows(*fixture_inputs())
    status_rows = review_thread_clusters.status_scaffold(member_rows)
    status_rows[0].update(
        {
            "status": "fixed-on-main",
            "closure_state": "ready-to-resolve",
            "evidence_commit": "2" * 40,
            "evidence_locator": "src/shared_a.py:fixture",
            "reason": "The current implementation satisfies the review claim.",
            "falsifier": "The reviewed failure remains reproducible.",
        }
    )
    status_path = tmp_path / "review-status.tsv"
    review_thread_clusters.write_tsv(
        status_path, review_thread_clusters.STATUS_FIELDS, status_rows
    )
    with pytest.raises(review_thread_clusters.ClusterError, match="passing check"):
        review_thread_clusters.check_status(status_path, member_rows)
