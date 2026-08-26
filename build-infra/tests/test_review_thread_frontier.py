# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import json
import subprocess
from pathlib import Path

import pytest  # type: ignore[import-not-found]

SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "review_thread_frontier.py"
)
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_frontier", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_frontier = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_frontier)


def frontier_row(rank: int) -> dict[str, str]:
    return {
        "frontier_id": f"terakan-calibration-mechanism-{rank}",
        "priority": "P1",
        "domain": "terakan",
        "mechanism": f"calibration mechanism {rank}",
        "current_evidence": "known-good fixture",
        "completion_state": "pending-evidence",
        "discriminating_question": "Does the row satisfy the contract?",
        "required_observation": "known-good acceptance",
        "execution_class": "offline",
        "authority_owner": "mesa-26-gororoba",
        "canonical_data_target": "build-infra/docs/frontier.tsv",
        "required_generator": "review_thread_frontier.py",
        "probe_declaration": "offline fixture",
        "bounded_output_schema": "one TSV row",
        "maximum_rows_and_bytes": "2 rows; 32768 bytes",
        "retention_class": "canonical-ledger",
        "tools": "python3",
        "ordering_dependencies": "previous rank",
        "completion_gate": "merged evidence and exact thread re-query",
        "falsification_condition": "any row invariant fails",
        "batch_id": "calibration",
        "batch_rank": str(rank),
        "thread_id": f"PRRT_calibration_{rank}",
        "pr_number": str(rank),
        "thread_created_at": f"2026-01-0{rank}T00:00:00Z",
        "is_outdated": "false",
        "review_path": "src/calibration.c",
        "original_line": str(rank),
        "review_author": "calibration",
        "review_url": (
            "https://github.com/Oichkatzelesfrettschen/mesa-26-gororoba/"
            f"pull/{rank}#discussion_r{rank}"
        ),
        "disposition": "pending",
        "merged_evidence_commit": "",
        "resolution_state": "unresolved",
    }


def frontier_rows() -> list[dict[str, str]]:
    return [frontier_row(1), frontier_row(2)]


def run_git(repository_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def initialize_evidence_repository(repository_root: Path) -> str:
    run_git(repository_root, "init", "--initial-branch=main")
    run_git(repository_root, "config", "user.name", "Review Frontier Calibration")
    run_git(repository_root, "config", "user.email", "calibration@example.invalid")
    (repository_root / "evidence.txt").write_text("fixed mechanism\n", encoding="utf-8")
    run_git(repository_root, "add", "evidence.txt")
    run_git(repository_root, "commit", "-m", "add fixed mechanism")
    evidence_commit = run_git(repository_root, "rev-parse", "HEAD")
    run_git(
        repository_root,
        "update-ref",
        "refs/remotes/origin/main",
        evidence_commit,
    )
    return evidence_commit


def evidence_row(evidence_commit: str) -> dict[str, str]:
    row = frontier_row(1)
    row["completion_state"] = "fixed-on-main"
    row["disposition"] = "fixed"
    row["canonical_data_target"] = "evidence.txt"
    row["merged_evidence_commit"] = evidence_commit
    return row


def live_payload(rows: list[dict[str, str]]) -> dict[str, object]:
    return {
        "data": {
            f"thread_{row_index}": {
                "id": row["thread_id"],
                "isResolved": row["resolution_state"] == "resolved",
                "isOutdated": row["is_outdated"] == "true",
                "comments": {"nodes": [{"url": row["review_url"]}]},
            }
            for row_index, row in enumerate(rows)
        }
    }


def test_accepts_complete_chronological_batch() -> None:
    rows_by_thread = review_thread_frontier.validate_frontier(frontier_rows(), 2)
    assert tuple(rows_by_thread) == ("PRRT_calibration_1", "PRRT_calibration_2")


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]
    ("field", "value"),
    (
        ("thread_id", "discussion_r123"),
        ("is_outdated", "yes"),
        ("completion_state", "done"),
        ("disposition", "maybe"),
        ("resolution_state", "unknown"),
        ("thread_created_at", "2026-01-01"),
    ),
)
def test_rejects_invalid_field_domain(field: str, value: str) -> None:
    rows = frontier_rows()
    rows[0][field] = value
    with pytest.raises(review_thread_frontier.FrontierError):
        review_thread_frontier.validate_frontier(rows, 2)


def test_rejects_duplicate_thread() -> None:
    rows = frontier_rows()
    rows[1]["thread_id"] = rows[0]["thread_id"]
    with pytest.raises(
        review_thread_frontier.FrontierError, match="duplicate thread_id"
    ):
        review_thread_frontier.validate_frontier(rows, 2)


def test_rejects_noncontiguous_rank() -> None:
    rows = frontier_rows()
    rows[1]["batch_rank"] = "3"
    with pytest.raises(review_thread_frontier.FrontierError, match="contiguous"):
        review_thread_frontier.validate_frontier(rows, 2)


def test_rejects_false_merged_evidence() -> None:
    rows = frontier_rows()
    rows[0]["merged_evidence_commit"] = "a" * 40
    with pytest.raises(review_thread_frontier.FrontierError, match="unmerged state"):
        review_thread_frontier.validate_frontier(rows, 2)


def test_ledger_requires_every_closed_thread() -> None:
    rows = frontier_rows()
    rows[0]["completion_state"] = "closed"
    rows[0]["disposition"] = "fixed"
    rows[0]["merged_evidence_commit"] = "a" * 40
    rows[0]["resolution_state"] = "resolved"
    frontier_by_thread = review_thread_frontier.validate_frontier(rows, 2)
    with pytest.raises(review_thread_frontier.FrontierError, match="closure mismatch"):
        review_thread_frontier.validate_ledger([], frontier_by_thread)


def test_ledger_accepts_merged_then_resolved_then_verified() -> None:
    rows = frontier_rows()
    rows[0]["completion_state"] = "closed"
    rows[0]["disposition"] = "fixed"
    rows[0]["merged_evidence_commit"] = "a" * 40
    rows[0]["resolution_state"] = "resolved"
    frontier_by_thread = review_thread_frontier.validate_frontier(rows, 2)
    ledger_row = {
        "thread_id": "PRRT_calibration_1",
        "disposition": "fixed",
        "evidence_commit": "a" * 40,
        "evidence_pr": "3",
        "merged_at": "2026-01-03T00:00:00Z",
        "github_resolved_at": "2026-01-03T00:01:00Z",
        "post_resolution_verified_at": "2026-01-03T00:02:00Z",
        "closure_note": "known-good closure ordering",
    }
    review_thread_frontier.validate_ledger([ledger_row], frontier_by_thread)


def test_merged_evidence_rejects_divergent_commit_with_same_blob(
    tmp_path: Path,
) -> None:
    evidence_commit = initialize_evidence_repository(tmp_path)
    evidence_tree = run_git(tmp_path, "rev-parse", f"{evidence_commit}^{{tree}}")
    divergent_commit = run_git(
        tmp_path, "commit-tree", evidence_tree, "-m", "divergent evidence"
    )
    with pytest.raises(review_thread_frontier.FrontierError, match="not merged"):
        review_thread_frontier.verify_merged_evidence(
            [evidence_row(divergent_commit)], tmp_path, "origin/main"
        )


def test_merged_evidence_rejects_stale_owner_blob(tmp_path: Path) -> None:
    evidence_commit = initialize_evidence_repository(tmp_path)
    (tmp_path / "evidence.txt").write_text("regressed mechanism\n", encoding="utf-8")
    run_git(tmp_path, "add", "evidence.txt")
    run_git(tmp_path, "commit", "-m", "change evidence owner")
    run_git(tmp_path, "update-ref", "refs/remotes/origin/main", "HEAD")
    with pytest.raises(review_thread_frontier.FrontierError, match="changed after"):
        review_thread_frontier.verify_merged_evidence(
            [evidence_row(evidence_commit)], tmp_path, "origin/main"
        )


def test_merged_evidence_accepts_unrelated_future_commit(tmp_path: Path) -> None:
    evidence_commit = initialize_evidence_repository(tmp_path)
    (tmp_path / "unrelated.txt").write_text("unrelated\n", encoding="utf-8")
    run_git(tmp_path, "add", "unrelated.txt")
    run_git(tmp_path, "commit", "-m", "add unrelated state")
    run_git(tmp_path, "update-ref", "refs/remotes/origin/main", "HEAD")
    review_thread_frontier.verify_merged_evidence(
        [evidence_row(evidence_commit)], tmp_path, "origin/main"
    )


def test_read_tsv_rejects_missing_trailing_cell(tmp_path: Path) -> None:
    frontier_path = tmp_path / "frontier.tsv"
    header = "\t".join(review_thread_frontier.FRONTIER_FIELDS)
    incomplete_row = "\t".join(
        "value" for _ in review_thread_frontier.FRONTIER_FIELDS[:-1]
    )
    frontier_path.write_text(f"{header}\n{incomplete_row}\n", encoding="utf-8")
    with pytest.raises(review_thread_frontier.FrontierError, match="missing columns"):
        review_thread_frontier.read_tsv(
            frontier_path, review_thread_frontier.FRONTIER_FIELDS
        )


def test_live_payload_accepts_exact_thread_identity() -> None:
    rows = frontier_rows()
    review_thread_frontier.validate_live_payload(rows, live_payload(rows))


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]
    ("field", "value", "message"),
    (
        ("id", "PRRT_wrong", "node mismatch"),
        ("isResolved", True, "resolution state differs"),
        ("isOutdated", True, "outdated state differs"),
    ),
)
def test_live_payload_rejects_thread_state_drift(
    field: str, value: object, message: str
) -> None:
    rows = frontier_rows()
    payload = live_payload(rows)
    data = payload["data"]
    assert isinstance(data, dict)
    thread = data["thread_0"]
    assert isinstance(thread, dict)
    thread[field] = value
    with pytest.raises(review_thread_frontier.FrontierError, match=message):
        review_thread_frontier.validate_live_payload(rows, payload)


def test_live_payload_rejects_discussion_url_drift() -> None:
    rows = frontier_rows()
    payload = live_payload(rows)
    data = payload["data"]
    assert isinstance(data, dict)
    thread = data["thread_0"]
    assert isinstance(thread, dict)
    comments = thread["comments"]
    assert isinstance(comments, dict)
    nodes = comments["nodes"]
    assert isinstance(nodes, list)
    first_comment = nodes[0]
    assert isinstance(first_comment, dict)
    first_comment["url"] = rows[1]["review_url"]
    with pytest.raises(review_thread_frontier.FrontierError, match="URL differs"):
        review_thread_frontier.validate_live_payload(rows, payload)


def test_live_query_uses_exact_node_ids(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    rows = frontier_rows()
    observed_arguments: list[str] = []

    def fake_run(
        arguments: list[str], **_kwargs: object
    ) -> subprocess.CompletedProcess[str]:
        observed_arguments.extend(arguments)
        return subprocess.CompletedProcess(
            arguments,
            0,
            stdout=json.dumps(live_payload(rows)),
            stderr="",
        )

    monkeypatch.setattr(review_thread_frontier.subprocess, "run", fake_run)
    review_thread_frontier.verify_live_threads(rows)
    assert observed_arguments[:3] == ["gh", "api", "graphql"]
    query_argument = next(
        argument for argument in observed_arguments if argument.startswith("query=")
    )
    assert all(row["thread_id"] in query_argument for row in rows)


def test_ledger_rejects_resolution_before_merge() -> None:
    rows = frontier_rows()
    rows[0]["completion_state"] = "closed"
    rows[0]["disposition"] = "fixed"
    rows[0]["merged_evidence_commit"] = "a" * 40
    rows[0]["resolution_state"] = "resolved"
    frontier_by_thread = review_thread_frontier.validate_frontier(rows, 2)
    ledger_row = {
        "thread_id": "PRRT_calibration_1",
        "disposition": "fixed",
        "evidence_commit": "a" * 40,
        "evidence_pr": "3",
        "merged_at": "2026-01-03T00:02:00Z",
        "github_resolved_at": "2026-01-03T00:01:00Z",
        "post_resolution_verified_at": "2026-01-03T00:03:00Z",
        "closure_note": "known-bad ordering",
    }
    with pytest.raises(review_thread_frontier.FrontierError, match="merged <="):
        review_thread_frontier.validate_ledger([ledger_row], frontier_by_thread)
