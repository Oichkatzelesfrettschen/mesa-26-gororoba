# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = BUILD_INFRA_ROOT / "scripts" / "review_thread_batch_capture.py"
CAPTURE_PATH = BUILD_INFRA_ROOT / "docs/review-thread-frontiers/merged-pr93-pr161"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_batch_capture", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_batch_capture = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_batch_capture)


def copied_capture(tmp_path: Path) -> Path:
    destination = tmp_path / "capture"
    shutil.copytree(CAPTURE_PATH, destination)
    return destination


def refresh_declared_hash(capture: Path, relative_path: str) -> None:
    manifest_path = capture / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["files"][relative_path] = hashlib.sha256(
        (capture / relative_path).read_bytes()
    ).hexdigest()
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def test_retained_capture_passes() -> None:
    review_thread_batch_capture.check_capture(CAPTURE_PATH)


def test_capture_rejects_undeclared_file(tmp_path: Path) -> None:
    capture = copied_capture(tmp_path)
    (capture / "unexpected.txt").write_text("unexpected\n", encoding="utf-8")
    with pytest.raises(
        review_thread_batch_capture.CaptureError, match="membership differs"
    ):
        review_thread_batch_capture.check_capture(capture)


def test_capture_rejects_hash_mutation(tmp_path: Path) -> None:
    capture = copied_capture(tmp_path)
    frontier_path = capture / "frontier.tsv"
    frontier_path.write_text(
        frontier_path.read_text(encoding="utf-8") + "mutated\n", encoding="utf-8"
    )
    with pytest.raises(review_thread_batch_capture.CaptureError, match="hash differs"):
        review_thread_batch_capture.check_capture(capture)


def test_capture_rejects_query_mutation_with_refreshed_hash(tmp_path: Path) -> None:
    capture = copied_capture(tmp_path)
    query_path = capture / "merged-prs.graphql"
    query_path.write_text(
        query_path.read_text(encoding="utf-8") + "\n", encoding="utf-8"
    )
    refresh_declared_hash(capture, "merged-prs.graphql")
    with pytest.raises(review_thread_batch_capture.CaptureError, match="query differs"):
        review_thread_batch_capture.check_capture(capture)


def test_capture_rejects_cursor_mutation_with_refreshed_hash(tmp_path: Path) -> None:
    capture = copied_capture(tmp_path)
    relative_path = "raw/merged-prs-0002.json"
    raw_path = capture / relative_path
    wrapper = json.loads(raw_path.read_text(encoding="utf-8"))
    wrapper["variables"]["cursor"] = "forged-cursor"
    raw_path.write_text(
        json.dumps(wrapper, sort_keys=True, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    refresh_declared_hash(capture, relative_path)
    with pytest.raises(review_thread_batch_capture.CaptureError, match="discontinuous"):
        review_thread_batch_capture.check_capture(capture)


def test_capture_rejects_selected_comment_mutation_with_refreshed_hash(
    tmp_path: Path,
) -> None:
    capture = copied_capture(tmp_path)
    relative_path = "selected-threads.json"
    selected_path = capture / relative_path
    selected = json.loads(selected_path.read_text(encoding="utf-8"))
    selected[0]["thread"]["comments"][0]["body"] += " mutated"
    selected_path.write_text(
        json.dumps(selected, sort_keys=True, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    refresh_declared_hash(capture, relative_path)
    with pytest.raises(
        review_thread_batch_capture.CaptureError, match="raw thread state"
    ):
        review_thread_batch_capture.check_capture(capture)


def test_chronological_stop_requires_scanned_pr_after_cutoff() -> None:
    pull_requests = [
        {"created_at": "2026-01-01T00:00:00Z"},
        {"created_at": "2026-01-02T00:00:00Z"},
    ]
    unresolved = [
        {"thread_created_at": "2026-01-01T12:00:00Z"},
        {"thread_created_at": "2026-01-02T00:00:00Z"},
    ]
    assert not review_thread_batch_capture.denominator_is_bounded(
        pull_requests, unresolved, 2
    )
