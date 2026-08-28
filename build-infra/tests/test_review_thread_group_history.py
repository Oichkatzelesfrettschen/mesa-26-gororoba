# SPDX-License-Identifier: MIT

from __future__ import annotations

import csv
import hashlib
import importlib
import json
import subprocess
import sys
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_ROOT = BUILD_INFRA_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_ROOT))
review_thread_group_history = importlib.import_module("review_thread_group_history")
sys.path.pop(0)


def run_git(repository_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def commit_file(
    repository_root: Path,
    relative_path: str,
    content: str,
    subject: str,
) -> str:
    source_path = repository_root / relative_path
    source_path.parent.mkdir(parents=True, exist_ok=True)
    source_path.write_text(content, encoding="utf-8")
    run_git(repository_root, "add", relative_path)
    run_git(repository_root, "commit", "-m", subject)
    return run_git(repository_root, "rev-parse", "HEAD")


@pytest.fixture
def squash_history_repository(tmp_path: Path) -> dict[str, object]:
    repository_root = tmp_path / "repository"
    repository_root.mkdir()
    run_git(repository_root, "init", "--initial-branch=main")
    run_git(repository_root, "config", "user.name", "Review Corpus Test")
    run_git(repository_root, "config", "user.email", "review-corpus@example.invalid")
    review_path = "src/reviewed.py"
    base_commit = commit_file(
        repository_root,
        review_path,
        "value = 'base'\n",
        "test: establish base source",
    )
    run_git(repository_root, "checkout", "-b", "review-head")
    review_commit = commit_file(
        repository_root,
        review_path,
        "value = 'reviewed'\n",
        "test: create reviewed source",
    )
    run_git(repository_root, "checkout", "main")
    merge_commit = commit_file(
        repository_root,
        review_path,
        "value = 'reviewed'\n",
        "test: squash reviewed source",
    )
    current_commit = commit_file(
        repository_root,
        review_path,
        "value = 'current'\n",
        "test: change reviewed source later",
    )
    return {
        "repository_root": repository_root,
        "review_path": review_path,
        "base_commit": base_commit,
        "review_commit": review_commit,
        "merge_commit": merge_commit,
        "current_commit": current_commit,
    }


def grouped_input(
    review_path: str,
    review_commit: str,
    merge_commit: str,
) -> tuple[
    list[dict[str, object]],
    list[dict[str, str]],
    list[dict[str, str]],
]:
    thread_id = "PRRT_fixture_history"
    work_group_id = "work-reviewed-py-function"
    records: list[dict[str, object]] = [
        {
            "thread_id": thread_id,
            "pull_request": {"merge_commit_oid": merge_commit},
            "thread": {
                "comments": [
                    {
                        "commit_oid": review_commit,
                        "original_commit_oid": "",
                    }
                ]
            },
        }
    ]
    memberships = [{"thread_id": thread_id, "work_group_id": work_group_id}]
    work_groups = [
        {
            "work_group_id": work_group_id,
            "review_path": review_path,
            "source_anchor": "function-reviewed",
            "thread_count": "1",
        }
    ]
    return records, memberships, work_groups


def install_grouped_input(
    monkeypatch: pytest.MonkeyPatch,
    grouped_rows: tuple[
        list[dict[str, object]],
        list[dict[str, str]],
        list[dict[str, str]],
    ],
) -> None:
    monkeypatch.setattr(
        review_thread_group_history,
        "group_threads",
        lambda _corpus_dir: grouped_rows,
    )


def test_squash_merge_anchor_finds_only_later_path_change(
    squash_history_repository: dict[str, object],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    repository_root = squash_history_repository["repository_root"]
    review_path = squash_history_repository["review_path"]
    review_commit = squash_history_repository["review_commit"]
    merge_commit = squash_history_repository["merge_commit"]
    current_commit = squash_history_repository["current_commit"]
    assert isinstance(repository_root, Path)
    assert all(
        isinstance(value, str)
        for value in (review_path, review_commit, merge_commit, current_commit)
    )
    install_grouped_input(
        monkeypatch,
        grouped_input(review_path, review_commit, merge_commit),
    )

    rows = review_thread_group_history.build_history_rows(
        tmp_path / "corpus", repository_root, current_commit
    )

    assert rows[0]["history_state"] == "changed-since-review"
    assert rows[0]["merged_anchor_count"] == "1"
    assert rows[0]["path_change_candidates"] == current_commit
    assert rows[0]["single_change_candidate"] == current_commit


def test_identical_review_and_current_blobs_need_claim_comparison(
    squash_history_repository: dict[str, object],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    repository_root = squash_history_repository["repository_root"]
    review_path = squash_history_repository["review_path"]
    review_commit = squash_history_repository["review_commit"]
    merge_commit = squash_history_repository["merge_commit"]
    assert isinstance(repository_root, Path)
    assert all(
        isinstance(value, str) for value in (review_path, review_commit, merge_commit)
    )
    install_grouped_input(
        monkeypatch,
        grouped_input(review_path, review_commit, merge_commit),
    )

    rows = review_thread_group_history.build_history_rows(
        tmp_path / "corpus", repository_root, merge_commit
    )

    assert rows[0]["history_state"] == "unchanged-since-review"
    assert rows[0]["path_change_candidates"] == ""
    assert rows[0]["next_action"] == "compare-current-code-with-review-claim"


def test_unavailable_review_source_stays_unresolved(
    squash_history_repository: dict[str, object],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    repository_root = squash_history_repository["repository_root"]
    review_path = squash_history_repository["review_path"]
    merge_commit = squash_history_repository["merge_commit"]
    current_commit = squash_history_repository["current_commit"]
    assert isinstance(repository_root, Path)
    assert all(
        isinstance(value, str) for value in (review_path, merge_commit, current_commit)
    )
    unavailable_commit = "f" * 40
    install_grouped_input(
        monkeypatch,
        grouped_input(review_path, unavailable_commit, merge_commit),
    )

    rows = review_thread_group_history.build_history_rows(
        tmp_path / "corpus", repository_root, current_commit
    )

    assert rows[0]["history_state"] == "review-source-unavailable"
    assert rows[0]["available_review_source_count"] == "0"
    assert rows[0]["unavailable_review_source_count"] == "1"
    assert rows[0]["next_action"] == "fetch-review-source-or-inspect-retained-diff"


def test_analysis_replay_rejects_candidate_commit_mutation(
    squash_history_repository: dict[str, object],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    repository_root = squash_history_repository["repository_root"]
    review_path = squash_history_repository["review_path"]
    base_commit = squash_history_repository["base_commit"]
    review_commit = squash_history_repository["review_commit"]
    merge_commit = squash_history_repository["merge_commit"]
    current_commit = squash_history_repository["current_commit"]
    assert isinstance(repository_root, Path)
    assert all(
        isinstance(value, str)
        for value in (
            review_path,
            base_commit,
            review_commit,
            merge_commit,
            current_commit,
        )
    )
    install_grouped_input(
        monkeypatch,
        grouped_input(review_path, review_commit, merge_commit),
    )
    corpus_dir = tmp_path / "corpus"
    corpus_dir.mkdir()
    (corpus_dir / "manifest.json").write_text("{}\n", encoding="utf-8")
    output_dir = tmp_path / "analysis"
    rows = review_thread_group_history.build_history_rows(
        corpus_dir, repository_root, current_commit
    )
    review_thread_group_history.write_analysis(
        output_dir, corpus_dir, current_commit, rows
    )
    history_path = output_dir / "work-group-history.tsv"
    with history_path.open("r", encoding="utf-8", newline="") as input_file:
        retained_rows = list(csv.DictReader(input_file, delimiter="\t"))
    retained_rows[0]["path_change_candidates"] = base_commit
    retained_rows[0]["single_change_candidate"] = base_commit
    review_thread_group_history.write_tsv(history_path, retained_rows)
    manifest_path = output_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["files"]["work-group-history.tsv"] = hashlib.sha256(
        history_path.read_bytes()
    ).hexdigest()
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )

    with pytest.raises(
        review_thread_group_history.HistoryError,
        match="retained history rows differ from source replay",
    ):
        review_thread_group_history.check_analysis(
            output_dir, corpus_dir, repository_root, current_commit
        )
