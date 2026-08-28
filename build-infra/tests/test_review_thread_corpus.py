# SPDX-License-Identifier: MIT

from __future__ import annotations

import gzip
import hashlib
import importlib.util
import json
from pathlib import Path

import pytest  # type: ignore[import-not-found]

BUILD_INFRA_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = BUILD_INFRA_ROOT / "scripts" / "review_thread_corpus.py"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "review_thread_corpus", SCRIPT_PATH
)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
review_thread_corpus = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(review_thread_corpus)

BRANCH_OID = "a" * 40
REVIEW_OID = "b" * 40
ORIGINAL_OID = "c" * 40


def comment_node(
    suffix: str,
    created_at: str,
    heading: str,
    diff_hunk: str = "@@ -10,3 +10,4 @@ def verify_contract(source: str) -> list[str]:",
) -> dict[str, object]:
    return {
        "id": f"PRRC_fixture_{suffix}",
        "databaseId": 100 + ord(suffix),
        "author": {"login": "reviewer"},
        "body": (
            "**<sub>![P2 Badge](https://example.invalid/p2)</sub> "
            f"{heading}**\n\nFinding body.\n\nUseful? React with yes or no."
        ),
        "createdAt": created_at,
        "updatedAt": created_at,
        "url": f"https://github.com/example/repo/pull/1#discussion_{suffix}",
        "diffHunk": diff_hunk,
        "commit": {"oid": REVIEW_OID},
        "originalCommit": {"oid": ORIGINAL_OID},
    }


def thread_node(
    suffix: str,
    created_at: str,
    heading: str,
    diff_hunk: str = "@@ -10,3 +10,4 @@ def verify_contract(source: str) -> list[str]:",
) -> dict[str, object]:
    return {
        "id": f"PRRT_fixture_{suffix}",
        "isResolved": False,
        "isOutdated": False,
        "path": "src/example_contract_test.py",
        "line": 12,
        "originalLine": 11,
        "comments": {
            "totalCount": 1,
            "pageInfo": {"hasNextPage": False, "endCursor": "comment-end"},
            "nodes": [comment_node(suffix, created_at, heading, diff_hunk)],
        },
    }


def pull_request_node() -> dict[str, object]:
    return {
        "number": 1,
        "title": "Validate cleanup control flow",
        "url": "https://github.com/example/repo/pull/1",
        "state": "MERGED",
        "createdAt": "2026-01-01T00:00:00Z",
        "mergedAt": "2026-01-02T00:00:00Z",
        "mergeCommit": {"oid": BRANCH_OID},
        "closedAt": "2026-01-02T00:00:00Z",
        "reviewThreads": {
            "pageInfo": {"hasNextPage": False, "endCursor": "thread-end"},
            "nodes": [
                thread_node(
                    "A",
                    "2026-01-01T01:00:00Z",
                    "Reject direct returns before cleanup goto",
                ),
                thread_node(
                    "B",
                    "2026-01-01T02:00:00Z",
                    "Verify the WSI failure reaches fail_device",
                ),
            ],
        },
    }


def repository_payload(connection: dict[str, object]) -> dict[str, object]:
    return {
        "data": {
            "repository": {
                "defaultBranchRef": {
                    "name": "main",
                    "target": {"oid": BRANCH_OID},
                },
                **connection,
            }
        }
    }


def fixture_capture(tmp_path: Path) -> Path:
    pull_request = pull_request_node()
    detailed_response = repository_payload(
        {
            "pullRequests": {
                "pageInfo": {"hasNextPage": False, "endCursor": "pr-end"},
                "nodes": [pull_request],
            }
        }
    )
    membership_nodes = []
    review_threads = pull_request["reviewThreads"]
    assert isinstance(review_threads, dict)
    for thread in review_threads["nodes"]:
        assert isinstance(thread, dict)
        membership_nodes.append(
            {
                "id": thread["id"],
                "isResolved": thread["isResolved"],
                "isOutdated": thread["isOutdated"],
                "path": thread["path"],
            }
        )
    membership_response = repository_payload(
        {
            "pullRequests": {
                "pageInfo": {"hasNextPage": False, "endCursor": "pr-end"},
                "nodes": [
                    {
                        "number": 1,
                        "state": "MERGED",
                        "reviewThreads": {
                            "pageInfo": {
                                "hasNextPage": False,
                                "endCursor": "thread-end",
                            },
                            "nodes": membership_nodes,
                        },
                    }
                ],
            }
        }
    )
    normalized_pull_request, _connections = review_thread_corpus.normalize_pull_request(
        pull_request
    )
    records = review_thread_corpus.unresolved_records([normalized_pull_request])
    group_members = review_thread_corpus.build_group_member_rows(records)
    work_groups = review_thread_corpus.build_work_group_rows(records, group_members)
    summary = review_thread_corpus.build_summary(records, group_members, work_groups)
    raw_payloads = [
        (
            "pull-requests-0001.json",
            {
                "query": "pull-requests",
                "variables": {"owner": "example", "repo": "repo"},
                "response": detailed_response,
            },
        ),
        (
            "membership-0001.json",
            {
                "query": "membership",
                "variables": {"owner": "example", "repo": "repo"},
                "response": membership_response,
            },
        ),
    ]
    manifest = {
        "schema": review_thread_corpus.CORPUS_SCHEMA,
        "corpus_id": "fixture-review-thread-corpus",
        "owner": "example",
        "repository": "repo",
        "observed_at": "2026-01-03T00:00:00Z",
        "membership_verified_at": "2026-01-03T00:01:00Z",
        "default_branch": "main",
        "default_branch_oid": BRANCH_OID,
        "pull_request_page_count": 1,
        "membership_page_count": 1,
        "scanned_pull_request_count": 1,
        "scanned_review_thread_count": 2,
        "thread_page_counts": {"1": 1},
        "comment_page_counts": {
            "PRRT_fixture_A": 1,
            "PRRT_fixture_B": 1,
        },
        "membership_thread_page_counts": {"1": 1},
        **summary,
    }
    output_dir = tmp_path / "capture"
    review_thread_corpus.write_capture(output_dir, raw_payloads, records, manifest)
    return output_dir


def refresh_declared_hash(capture: Path, relative_path: str) -> None:
    manifest_path = capture / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["files"][relative_path] = hashlib.sha256(
        (capture / relative_path).read_bytes()
    ).hexdigest()
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def rewrite_json(capture: Path, relative_path: str, payload: object) -> None:
    content = (
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("utf-8")
    if relative_path.endswith(".gz"):
        content = gzip.compress(content, compresslevel=9, mtime=0)
    (capture / relative_path).write_bytes(content)
    refresh_declared_hash(capture, relative_path)


def test_fixture_capture_passes(tmp_path: Path) -> None:
    review_thread_corpus.check_capture(fixture_capture(tmp_path))


def test_capture_rejects_thread_omission_with_refreshed_hash(tmp_path: Path) -> None:
    capture = fixture_capture(tmp_path)
    threads_path = capture / "threads.json.gz"
    threads = json.loads(gzip.decompress(threads_path.read_bytes()).decode("utf-8"))
    rewrite_json(capture, "threads.json.gz", threads[:-1])
    with pytest.raises(
        review_thread_corpus.CorpusError,
        match="differs from raw GraphQL capture",
    ):
        review_thread_corpus.check_capture(capture)


def test_capture_rejects_raw_duplicate_thread_with_refreshed_hash(
    tmp_path: Path,
) -> None:
    capture = fixture_capture(tmp_path)
    relative_path = "raw/pull-requests-0001.json.gz"
    wrapper = review_thread_corpus.read_json(capture / relative_path)
    nodes = wrapper["response"]["data"]["repository"]["pullRequests"]["nodes"]
    threads = nodes[0]["reviewThreads"]["nodes"]
    threads[1]["id"] = threads[0]["id"]
    rewrite_json(capture, relative_path, wrapper)
    with pytest.raises(review_thread_corpus.CorpusError, match="repeats"):
        review_thread_corpus.check_capture(capture)


def test_capture_rejects_pagination_loss_with_refreshed_hash(tmp_path: Path) -> None:
    capture = fixture_capture(tmp_path)
    relative_path = "raw/pull-requests-0001.json.gz"
    wrapper = review_thread_corpus.read_json(capture / relative_path)
    page_info = wrapper["response"]["data"]["repository"]["pullRequests"]["pageInfo"]
    page_info["hasNextPage"] = True
    page_info["endCursor"] = "missing-page"
    rewrite_json(capture, relative_path, wrapper)
    with pytest.raises(review_thread_corpus.CorpusError, match="incomplete"):
        review_thread_corpus.check_capture(capture)


def test_capture_rejects_membership_omission_with_refreshed_hash(
    tmp_path: Path,
) -> None:
    capture = fixture_capture(tmp_path)
    relative_path = "raw/membership-0001.json.gz"
    wrapper = review_thread_corpus.read_json(capture / relative_path)
    nodes = wrapper["response"]["data"]["repository"]["pullRequests"]["nodes"]
    nodes[0]["reviewThreads"]["nodes"][1]["isResolved"] = True
    rewrite_json(capture, relative_path, wrapper)
    with pytest.raises(review_thread_corpus.CorpusError, match="membership differ"):
        review_thread_corpus.check_capture(capture)


def test_compression_preserves_exact_json_and_replay(tmp_path: Path) -> None:
    capture = fixture_capture(tmp_path)
    compressed_paths = sorted((capture / "raw").glob("*.json.gz"))
    compressed_paths.append(capture / "threads.json.gz")
    expected_json = {
        str(path.relative_to(capture)).removesuffix(".gz"): gzip.decompress(
            path.read_bytes()
        )
        for path in compressed_paths
    }
    manifest_path = capture / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    declared_files = dict(manifest["files"])
    for compressed_path in compressed_paths:
        compressed_relative_path = str(compressed_path.relative_to(capture))
        plain_path = compressed_path.with_suffix("")
        plain_relative_path = str(plain_path.relative_to(capture))
        plain_content = gzip.decompress(compressed_path.read_bytes())
        plain_path.write_bytes(plain_content)
        compressed_path.unlink()
        declared_files.pop(compressed_relative_path)
        declared_files[plain_relative_path] = hashlib.sha256(plain_content).hexdigest()
    manifest["files"] = dict(sorted(declared_files.items()))
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    review_thread_corpus.check_capture(capture)

    review_thread_corpus.compress_capture(capture)

    review_thread_corpus.check_capture(capture)
    for plain_relative_path, expected_content in expected_json.items():
        compressed_path = capture / f"{plain_relative_path}.gz"
        assert compressed_path.is_file()
        assert gzip.decompress(compressed_path.read_bytes()) == expected_content
        assert not (capture / plain_relative_path).exists()


def test_grouping_keeps_claims_separate_inside_one_source_owner() -> None:
    pull_request = pull_request_node()
    normalized_pull_request, _connections = review_thread_corpus.normalize_pull_request(
        pull_request
    )
    records = review_thread_corpus.unresolved_records([normalized_pull_request])
    memberships = review_thread_corpus.build_group_member_rows(records)
    assert memberships[0]["work_group_id"] == memberships[1]["work_group_id"]
    assert memberships[0]["claim_group_id"] != memberships[1]["claim_group_id"]


def test_grouping_keeps_source_owners_separate_for_one_claim() -> None:
    pull_request = pull_request_node()
    review_threads = pull_request["reviewThreads"]
    assert isinstance(review_threads, dict)
    second_thread = review_threads["nodes"][1]
    assert isinstance(second_thread, dict)
    second_comment_connection = second_thread["comments"]
    assert isinstance(second_comment_connection, dict)
    second_comment = second_comment_connection["nodes"][0]
    assert isinstance(second_comment, dict)
    first_thread = review_threads["nodes"][0]
    assert isinstance(first_thread, dict)
    first_comment_connection = first_thread["comments"]
    assert isinstance(first_comment_connection, dict)
    first_comment = first_comment_connection["nodes"][0]
    assert isinstance(first_comment, dict)
    second_comment["body"] = first_comment["body"]
    second_comment["diffHunk"] = (
        "@@ -40,3 +40,4 @@ def verify_wsi_edge(source: str) -> None:"
    )
    normalized_pull_request, _connections = review_thread_corpus.normalize_pull_request(
        pull_request
    )
    records = review_thread_corpus.unresolved_records([normalized_pull_request])
    memberships = review_thread_corpus.build_group_member_rows(records)
    assert memberships[0]["claim_group_id"] == memberships[1]["claim_group_id"]
    assert memberships[0]["work_group_id"] != memberships[1]["work_group_id"]


def test_group_identifiers_are_stable_per_record() -> None:
    pull_request = pull_request_node()
    normalized_pull_request, _connections = review_thread_corpus.normalize_pull_request(
        pull_request
    )
    records = review_thread_corpus.unresolved_records([normalized_pull_request])
    first = review_thread_corpus.group_identities(records[0])
    second = review_thread_corpus.group_identities(dict(records[0]))
    assert first == second


def test_derived_labels_remove_external_decoration_and_retired_wording() -> None:
    pull_request = pull_request_node()
    review_threads = pull_request["reviewThreads"]
    assert isinstance(review_threads, dict)
    first_thread = review_threads["nodes"][0]
    assert isinstance(first_thread, dict)
    comments = first_thread["comments"]
    assert isinstance(comments, dict)
    first_comment = comments["nodes"][0]
    assert isinstance(first_comment, dict)
    retired_name = "".join(("goro", "roba"))
    retired_qualification = "decision" + "-grade"
    external_heading = (
        "\U0001f4d0 Maintainability " f"{retired_qualification} {retired_name} helper"
    )
    first_comment["body"] = f"**{external_heading}**\n\nFinding body."
    normalized_pull_request, _connections = review_thread_corpus.normalize_pull_request(
        pull_request
    )
    records = review_thread_corpus.unresolved_records([normalized_pull_request])
    thread_rows = review_thread_corpus.build_thread_rows(records)
    identities = review_thread_corpus.group_identities(records[0])

    assert thread_rows[0]["claim_heading"] == ("maintainability verified result helper")
    assert all(retired_name not in identifier for identifier in identities.values())
    assert records[0]["thread"]["comments"][0]["body"].startswith(
        f"**{external_heading}**"
    )
