# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path
from subprocess import CompletedProcess

import pytest

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = BUILD_INFRA_ROOT / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SCRIPT_PATH = SCRIPT_DIR / "review_thread_reconciliation.py"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_reconciliation", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_reconciliation = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_reconciliation)

COMMIT_OID = "a" * 40
MERGE_OID = "b" * 40


def merged_pull_request_payload() -> str:
    return json.dumps(
        {
            "data": {
                "repository": {
                    "pullRequest": {
                        "number": 42,
                        "state": "MERGED",
                        "mergedAt": "2026-08-29T00:00:00Z",
                        "mergeCommit": {"oid": MERGE_OID},
                        "title": "Target mechanism",
                    }
                }
            }
        }
    )


def test_merged_pull_request_returns_citable_metadata(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        review_thread_reconciliation.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess(
            [], 0, merged_pull_request_payload(), ""
        ),
    )
    assert review_thread_reconciliation.graphql_merged_pull_request("42") == {
        "evidence_pr": "42",
        "evidence_commit": MERGE_OID,
        "evidence_pr_title": "Target mechanism",
        "evidence_merged_at": "2026-08-29T00:00:00Z",
    }


def test_merged_pull_request_rejects_open_state(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    payload_object = json.loads(merged_pull_request_payload())
    payload_object["data"]["repository"]["pullRequest"]["state"] = "OPEN"
    payload = json.dumps(payload_object)
    monkeypatch.setattr(
        review_thread_reconciliation.subprocess,
        "run",
        lambda *_args, **_kwargs: CompletedProcess([], 0, payload, ""),
    )
    with pytest.raises(
        review_thread_reconciliation.ReconciliationError,
        match="not merged",
    ):
        review_thread_reconciliation.graphql_merged_pull_request("42")


def test_commit_pull_request_query_preserves_title_and_balanced_braces(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    requests: list[tuple[object, ...]] = []
    payload = json.dumps(
        {
            "data": {
                "repository": {
                    "c0": {
                        "oid": COMMIT_OID,
                        "associatedPullRequests": {
                            "nodes": [
                                {
                                    "number": 42,
                                    "state": "MERGED",
                                    "mergedAt": "2026-08-29T00:00:00Z",
                                    "mergeCommit": {"oid": MERGE_OID},
                                    "title": "Target mechanism",
                                }
                            ]
                        },
                    }
                }
            }
        }
    )

    def fake_run(arguments: list[str], **_kwargs: object) -> CompletedProcess[str]:
        requests.append(tuple(arguments))
        return CompletedProcess(arguments, 0, payload, "")

    monkeypatch.setattr(review_thread_reconciliation.subprocess, "run", fake_run)
    assert review_thread_reconciliation.graphql_pull_requests([COMMIT_OID]) == {
        COMMIT_OID: {
            "evidence_pr": "42",
            "evidence_commit": MERGE_OID,
            "evidence_pr_title": "Target mechanism",
            "evidence_merged_at": "2026-08-29T00:00:00Z",
        }
    }
    query = requests[0][-1].removeprefix("query=")
    assert query.count("{") == query.count("}")


def test_build_rejects_override_outside_frontier(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    frontier = [
        {
            "thread_id": "PRRT_fixture",
            "thread_created_at": "2026-08-29T00:00:00Z",
            "review_url": "https://example.invalid/thread",
            "review_path": "src/example.c",
            "original_line": "1",
            "claim_heading": "Claim",
            "status_at_capture": "resolved",
            "falsifier": "A change",
        }
    ]
    monkeypatch.setattr(
        review_thread_reconciliation,
        "graphql_nodes",
        lambda _thread_ids: {"PRRT_fixture": True},
    )
    with pytest.raises(
        review_thread_reconciliation.ReconciliationError,
        match="outside the frontier",
    ):
        review_thread_reconciliation.build_rows(
            tmp_path, COMMIT_OID, frontier, {"PRRT_not_in_frontier": {}}
        )


def test_source_change_subject_rejects_empty_subject(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(review_thread_reconciliation, "run_git", lambda *_args: "")
    with pytest.raises(
        review_thread_reconciliation.ReconciliationError,
        match="missing source change subject",
    ):
        review_thread_reconciliation.source_change_subject(tmp_path, COMMIT_OID)


def test_tsv_writer_quotes_empty_final_field(tmp_path: Path) -> None:
    output_path = tmp_path / "record.tsv"
    review_thread_reconciliation.write_tsv(
        output_path,
        ("thread_id", "falsifier"),
        [{"thread_id": "PRRT_fixture", "falsifier": ""}],
    )
    assert output_path.read_text(encoding="utf-8").splitlines()[-1].endswith('\t""')
