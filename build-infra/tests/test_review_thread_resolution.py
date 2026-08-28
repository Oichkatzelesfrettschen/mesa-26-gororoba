# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import importlib.util
import sys
from argparse import Namespace
from pathlib import Path
from subprocess import CompletedProcess
from typing import Any

import pytest

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = BUILD_INFRA_ROOT / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SCRIPT_PATH = SCRIPT_DIR / "review_thread_resolution.py"
CLASSIFICATION_DIR = (
    BUILD_INFRA_ROOT
    / "docs/review-thread-classifications/merged-thread-frontier-after-QY6A8"
)
FRONTIER_PATH = CLASSIFICATION_DIR / "pre-resolution-frontier.tsv"
JOURNAL_PATH = CLASSIFICATION_DIR / "resolution-observation.json"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_resolution", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_resolution = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_resolution)


def test_graphql_rejects_non_object_payload(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        review_thread_resolution.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 0, "[]", ""),
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="non-object JSON",
    ):
        review_thread_resolution.graphql("query { viewer { login } }")


def retained_state() -> tuple[list[dict[str, str]], dict[str, Any]]:
    rows = review_thread_resolution.read_tsv(
        FRONTIER_PATH, review_thread_resolution.FRONTIER_FIELDS
    )
    journal = review_thread_resolution.load_journal(JOURNAL_PATH)
    return rows, journal


def test_retained_resolution_journal_is_complete() -> None:
    rows, journal = retained_state()
    review_thread_resolution.validate_resolution_frontier(rows)
    review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)
    assert journal["complete"] is True
    assert len(journal["entries"]) == 50


def test_resolution_accepts_variable_frontier() -> None:
    rows, _ = retained_state()
    review_thread_resolution.validate_resolution_frontier(rows[:-1])


def test_resolution_rejects_empty_frontier() -> None:
    with pytest.raises(review_thread_resolution.FrontierError, match="empty"):
        review_thread_resolution.validate_resolution_frontier([])


def test_resolution_rejects_actionable_row_before_authority_lookup() -> None:
    rows, _ = retained_state()
    mutated = copy.deepcopy(rows[:1])
    mutated[0].update(
        {
            "completion_state": "actionable",
            "disposition": "requires-change",
            "merged_evidence_commit": "",
        }
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="not fixed, superseded, or invalid",
    ):
        review_thread_resolution.verify_resolution_authority(mutated, Namespace())


def test_resolution_rejects_multiple_evidence_commits() -> None:
    rows, _ = retained_state()
    mutated = copy.deepcopy(rows[:2])
    mutated[1]["merged_evidence_commit"] = "f" * 40
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="multiple evidence commits",
    ):
        review_thread_resolution.verify_resolution_authority(mutated, Namespace())


def test_resolution_journal_rejects_evidence_commit_drift() -> None:
    rows, _ = retained_state()
    journal = {
        "schema": review_thread_resolution.SCHEMA,
        "frontier_sha256": review_thread_resolution.frontier_hash(FRONTIER_PATH),
        "evidence_commit": "f" * 40,
        "evidence_pr": 1913,
        "merged_at": "2026-08-27T09:04:16Z",
        "started_at": "2026-08-27T09:05:00Z",
        "entries": [],
        "complete": False,
        "post_verified_at": None,
    }
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="evidence commit differs from frontier",
    ):
        review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)


def test_resolution_rejects_malformed_thread_id() -> None:
    rows, _ = retained_state()
    mutated = copy.deepcopy(rows)
    mutated[0]["thread_id"] = 'PRRT_bad") { viewer { login } }'
    with pytest.raises(
        review_thread_resolution.FrontierError, match="invalid thread_id"
    ):
        review_thread_resolution.validate_resolution_frontier(mutated)


def test_resolution_rejects_reordered_entries() -> None:
    rows, journal = retained_state()
    mutated = copy.deepcopy(journal)
    mutated["entries"][0], mutated["entries"][1] = (
        mutated["entries"][1],
        mutated["entries"][0],
    )
    with pytest.raises(
        review_thread_resolution.FrontierError, match="ordered frontier prefix"
    ):
        review_thread_resolution.validate_journal(rows, mutated, FRONTIER_PATH)


def test_resolution_rejects_frontier_hash_drift() -> None:
    rows, journal = retained_state()
    mutated = copy.deepcopy(journal)
    mutated["frontier_sha256"] = "0" * 64
    with pytest.raises(
        review_thread_resolution.FrontierError, match="frontier hash differs"
    ):
        review_thread_resolution.validate_journal(rows, mutated, FRONTIER_PATH)


def test_resolution_rejects_incomplete_complete_journal() -> None:
    rows, journal = retained_state()
    mutated = copy.deepcopy(journal)
    mutated["entries"] = mutated["entries"][:-1]
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="complete journal lacks full postflight evidence",
    ):
        review_thread_resolution.validate_journal(rows, mutated, FRONTIER_PATH)


def recovery_journal(
    rows: list[dict[str, str]], indexes: tuple[int, ...] = (39, 41)
) -> dict[str, Any]:
    entries = []
    for offset, index in enumerate(indexes, start=1):
        entries.append(
            {
                "thread_id": rows[index]["thread_id"],
                "disposition": "fixed",
                "evidence_commit": f"{offset % 16:x}" * 40,
                "evidence_pr": 1900 + offset,
                "merged_at": "2026-08-27T00:01:00Z",
                "resolved_at": "2026-08-27T00:02:00Z",
                "verified_at": "2026-08-27T00:03:00Z",
            }
        )
    return {
        "schema": review_thread_resolution.RECOVERY_SCHEMA,
        "frontier_sha256": review_thread_resolution.frontier_hash(FRONTIER_PATH),
        "started_at": "2026-08-27T00:00:00Z",
        "entries": entries,
        "complete": False,
        "post_verified_at": None,
    }


def test_recovery_accepts_an_ordered_nonprefix_subsequence() -> None:
    rows, _ = retained_state()
    journal = recovery_journal(rows)
    review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)


def test_recovery_rejects_reordered_entries() -> None:
    rows, _ = retained_state()
    journal = recovery_journal(rows)
    journal["entries"][0], journal["entries"][1] = (
        journal["entries"][1],
        journal["entries"][0],
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="ordered frontier subsequence",
    ):
        review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)


def test_recovery_rejects_missing_entry_authority() -> None:
    rows, _ = retained_state()
    journal = recovery_journal(rows)
    del journal["entries"][0]["evidence_pr"]
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="invalid fields",
    ):
        review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)


def test_recovery_rejects_nontext_evidence_commit() -> None:
    rows, _ = retained_state()
    journal = recovery_journal(rows)
    journal["entries"][0]["evidence_commit"] = 1
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="invalid evidence commit",
    ):
        review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)


def test_recovery_rejects_nonboolean_completion_state() -> None:
    rows, _ = retained_state()
    journal = recovery_journal(rows)
    journal["complete"] = "false"
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="complete state is not boolean",
    ):
        review_thread_resolution.validate_journal(rows, journal, FRONTIER_PATH)


def test_recovery_writes_only_recorded_rows(tmp_path: Path) -> None:
    rows, _ = retained_state()
    journal = recovery_journal(rows)
    ledger_path = tmp_path / "resolution-ledger.tsv"
    review_thread_resolution.write_ledger(ledger_path, rows, journal)
    ledger = review_thread_resolution.read_tsv(
        ledger_path, review_thread_resolution.LEDGER_FIELDS
    )
    assert [row["thread_id"] for row in ledger] == [
        rows[39]["thread_id"],
        rows[41]["thread_id"],
    ]
    assert [row["evidence_pr"] for row in ledger] == ["1901", "1902"]
    assert [row["evidence_commit"] for row in ledger] == ["1" * 40, "2" * 40]


def record_arguments(tmp_path: Path, rows: list[dict[str, str]]) -> Namespace:
    return Namespace(
        frontier=FRONTIER_PATH,
        journal=tmp_path / "resolution-observation.json",
        ledger=tmp_path / "resolution-ledger.tsv",
        repo_root=tmp_path,
        main_ref="origin/main",
        thread_id=rows[-1]["thread_id"],
        disposition="fixed",
        evidence_commit="a" * 40,
        evidence_pr=1913,
        merged_at="2026-08-27T09:04:16Z",
    )


def resolution_arguments(tmp_path: Path) -> Namespace:
    return Namespace(
        repo_root=tmp_path,
        main_ref="origin/main",
        evidence_pr=1913,
        merged_at="2026-08-27T09:04:16Z",
    )


def test_resolution_authority_accepts_exact_merged_pr(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rows, _ = retained_state()
    evidence_commit = rows[0]["merged_evidence_commit"]
    arguments = resolution_arguments(tmp_path)
    monkeypatch.setattr(
        review_thread_resolution,
        "run_git",
        lambda *_args: str(tmp_path),
    )
    monkeypatch.setattr(
        review_thread_resolution.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 0, "", ""),
    )
    monkeypatch.setattr(
        review_thread_resolution,
        "evidence_target_identity",
        lambda *_args: "same-owner-content",
    )
    monkeypatch.setattr(
        review_thread_resolution,
        "graphql",
        lambda _query, _variables: {
            "data": {
                "repository": {
                    "pullRequest": {
                        "merged": True,
                        "mergedAt": arguments.merged_at,
                        "mergeCommit": {"oid": evidence_commit},
                    }
                }
            }
        },
    )
    assert (
        review_thread_resolution.verify_resolution_authority(rows, arguments)
        == evidence_commit
    )


def test_resolution_authority_rejects_unmerged_pr(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rows, _ = retained_state()
    arguments = resolution_arguments(tmp_path)
    monkeypatch.setattr(
        review_thread_resolution,
        "run_git",
        lambda *_args: str(tmp_path),
    )
    monkeypatch.setattr(
        review_thread_resolution.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 0, "", ""),
    )
    monkeypatch.setattr(
        review_thread_resolution,
        "evidence_target_identity",
        lambda *_args: "same-owner-content",
    )
    monkeypatch.setattr(
        review_thread_resolution,
        "graphql",
        lambda _query, _variables: {
            "data": {"repository": {"pullRequest": {"merged": False}}}
        },
    )
    with pytest.raises(review_thread_resolution.FrontierError, match="not merged"):
        review_thread_resolution.verify_resolution_authority(rows, arguments)


def test_record_authority_accepts_exact_merged_pr(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rows, _ = retained_state()
    arguments = record_arguments(tmp_path, rows)
    monkeypatch.setattr(
        review_thread_resolution,
        "run_git",
        lambda *_args: str(tmp_path),
    )
    monkeypatch.setattr(
        review_thread_resolution.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 0, "", ""),
    )
    monkeypatch.setattr(
        review_thread_resolution,
        "evidence_target_identity",
        lambda *_args: "same-owner-content",
    )
    monkeypatch.setattr(
        review_thread_resolution,
        "graphql",
        lambda _query, _variables: {
            "data": {
                "repository": {
                    "pullRequest": {
                        "merged": True,
                        "mergedAt": arguments.merged_at,
                        "mergeCommit": {"oid": arguments.evidence_commit},
                    }
                }
            }
        },
    )
    review_thread_resolution.verify_record_authority(rows[-1], arguments)


def test_record_authority_rejects_null_merge_commit(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        review_thread_resolution,
        "graphql",
        lambda _query, _variables: {
            "data": {
                "repository": {
                    "pullRequest": {
                        "merged": True,
                        "mergedAt": "2026-08-27T09:04:16Z",
                        "mergeCommit": None,
                    }
                }
            }
        },
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="merge commit differs",
    ):
        review_thread_resolution.verify_record_pull_request(
            "a" * 40, 1913, "2026-08-27T09:04:16Z"
        )


def test_record_authority_rejects_ancestry_query_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        review_thread_resolution.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 128, "", "bad revision"),
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="evidence ancestry check failed: bad revision",
    ):
        review_thread_resolution.verify_commit_on_main(
            tmp_path, "a" * 40, "missing-main"
        )


def test_record_authority_rejects_nonancestor(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        review_thread_resolution.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 1, "", ""),
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="not an ancestor of main",
    ):
        review_thread_resolution.verify_commit_on_main(
            tmp_path, "a" * 40, "origin/main"
        )


def test_record_authority_rejects_target_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rows, _ = retained_state()
    identities = iter(("evidence-content", "main-content"))
    monkeypatch.setattr(
        review_thread_resolution,
        "evidence_target_identity",
        lambda *_args: next(identities),
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="evidence target differs on main",
    ):
        review_thread_resolution.verify_record_targets(
            rows[-1], tmp_path, "a" * 40, "origin/main"
        )


def test_record_requires_all_thread_postflight_before_completion(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rows, _ = retained_state()
    arguments = record_arguments(tmp_path, rows)
    journal = recovery_journal(rows, tuple(range(49)))
    review_thread_resolution.atomic_json(arguments.journal, journal)
    monkeypatch.setattr(
        review_thread_resolution,
        "verify_record_authority",
        lambda _row, _arguments: None,
    )
    query_sizes: list[int] = []

    def query_with_unresolved_postflight(
        queried_rows: list[dict[str, str]],
    ) -> dict[str, bool]:
        query_sizes.append(len(queried_rows))
        states = {row["thread_id"]: True for row in queried_rows}
        if len(queried_rows) == 50:
            states[queried_rows[0]["thread_id"]] = False
        return states

    monkeypatch.setattr(
        review_thread_resolution,
        "query_rows",
        query_with_unresolved_postflight,
    )
    with pytest.raises(
        review_thread_resolution.FrontierError,
        match="final postflight found unresolved threads",
    ):
        review_thread_resolution.record(rows, arguments)
    assert query_sizes == [1, 1, 50]
    retained = review_thread_resolution.load_journal(arguments.journal)
    assert retained["complete"] is False
    assert len(retained["entries"]) == 49


def test_record_completes_after_all_thread_postflight(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rows, _ = retained_state()
    arguments = record_arguments(tmp_path, rows)
    journal = recovery_journal(rows, tuple(range(49)))
    review_thread_resolution.atomic_json(arguments.journal, journal)
    monkeypatch.setattr(
        review_thread_resolution,
        "verify_record_authority",
        lambda _row, _arguments: None,
    )
    query_sizes: list[int] = []

    def query_resolved(queried_rows: list[dict[str, str]]) -> dict[str, bool]:
        query_sizes.append(len(queried_rows))
        return {row["thread_id"]: True for row in queried_rows}

    monkeypatch.setattr(review_thread_resolution, "query_rows", query_resolved)
    review_thread_resolution.record(rows, arguments)
    assert query_sizes == [1, 1, 50]
    retained = review_thread_resolution.load_journal(arguments.journal)
    assert retained["complete"] is True
    assert len(retained["entries"]) == 50
    assert retained["post_verified_at"] is not None
