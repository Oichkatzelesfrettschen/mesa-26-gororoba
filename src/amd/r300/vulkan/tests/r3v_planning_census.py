#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Census a host-planning pass by first refusal and by nonempty IB.

A planning pass answers one question per case: did the driver build an
indirect buffer, or did something refuse before it could?  A shard receipt
counts dEQP statuses, which answers a different question -- a case that
reports `NotSupported` because the CTS declined a missing extension and a
case that reports `Fail` because `vkCreateImage` refused a format both leave
the same empty capture, and neither status names the predicate that stopped
the driver.

Three artifacts carry the answer, and this tool reads all three:

  run.qpa            the CTS result element: status code and the message
                     naming the predicate and its `file:line` refusal site
  <case>.plan        a transcript: the submissions the driver planned, their
                     IB dword counts, cell kinds, emitters, and BO roles
  <case>.plan.no_nonempty_ib
                     the marker the capture writes when the device destroyed
                     without an executable submission

The per-case table is the join.  The first-refusal census ranks refusal
sites by case population, so the predicate family worth implementing next is
the one at the top rather than the one that happened to be read first.  The
nonempty-IB census separates the cases with a plan to compose from the cases
with nothing to replay.
"""

from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path

CASE_TABLE_HEADER = (
    "slice",
    "shard",
    "case",
    "deqp_status",
    "refusal_site",
    "refusal_predicate",
    "planning_outcome",
    "transcript_sha256",
    "submission_count",
    "ib_dwords",
    "cell_kind",
    "emitter",
    "reloc_count",
    "bo_roles",
    "cs_ioctls",
)

FIRST_REFUSAL_HEADER = (
    "refusal_site",
    "deqp_status",
    "cases",
    "slices",
    "example_case",
    "example_predicate",
)

NONEMPTY_IB_HEADER = ("planning_outcome", "cases", "slices", "example_case")

TRANSCRIPT_SUFFIX = ".plan"
EMPTY_SUFFIX = ".plan.no_nonempty_ib"

RESULT_PATTERN = re.compile(
    r'<Result\s+StatusCode="([^"]+)"\s*>(.*?)</Result>', re.S
)
SITE_PATTERN = re.compile(r"\bat\s+([\w./+-]+:\d+)")


def result_of(qpa):
    """The case's status code and message, or an absent result.

    A case the runner killed mid-flight leaves a QPA without a result
    element; it carries no refusal of its own, so the census reports the
    absence rather than inventing a predicate for it."""
    try:
        text = qpa.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None, None
    match = RESULT_PATTERN.search(text)
    if not match:
        return None, None
    return match.group(1), " ".join(match.group(2).split())


def refusal_site(message):
    """The `file:line` the CTS names, which is the refusal's durable address.

    The message itself carries case-specific values, so the site is the key a
    census groups by and the message is the example it keeps."""
    if not message:
        return "unreported"
    match = SITE_PATTERN.search(message)
    return match.group(1) if match else "unreported"


def parse_transcript(path):
    """Read the plan a capture wrote: submissions, cells, and BO roles."""
    fields = {
        "submission_count": "",
        "ib_dwords": "",
        "cell_kind": "",
        "emitter": "",
        "reloc_count": "",
        "bo_roles": "",
    }
    roles = []
    dwords = []
    kinds = []
    emitters = []
    relocs = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        if parts[0] == "submission_count" and len(parts) > 1:
            fields["submission_count"] = parts[1]
        elif parts[0] == "submission" and len(parts) >= 7:
            dwords.append(parts[3])
            kinds.append(parts[4])
            emitters.append(parts[5])
            relocs += int(parts[6]) if parts[6].isdigit() else 0
        elif parts[0] == "reloc" and len(parts) >= 4:
            roles.append(parts[3])
    fields["ib_dwords"] = " ".join(dwords)
    fields["cell_kind"] = " ".join(sorted(set(kinds)))
    fields["emitter"] = " ".join(sorted(set(emitters)))
    fields["reloc_count"] = str(relocs)
    fields["bo_roles"] = " ".join(roles)
    return fields


def capture_state(capture_dir, case):
    """Classify one case's capture: a transcript, an empty marker, or neither.

    The capture names a file after the case, and a case name longer than the
    plan path ceiling reaches the filesystem shortened with a digest suffix,
    so a name that does not match exactly is searched for by prefix."""
    outcome = "unresolved"
    digest = ""
    fields = {key: "" for key in CASE_TABLE_HEADER[8:14]}
    if capture_dir is None:
        return outcome, digest, fields
    exact = capture_dir / f"{case}{TRANSCRIPT_SUFFIX}"
    empty = capture_dir / f"{case}{EMPTY_SUFFIX}"
    if not exact.is_file() and not empty.is_file():
        stem = case[:96]
        matches = sorted(capture_dir.glob(f"{glob_escape(stem)}*"))
        for candidate in matches:
            if candidate.name.endswith(EMPTY_SUFFIX):
                empty = candidate
                break
            if candidate.name.endswith(TRANSCRIPT_SUFFIX):
                exact = candidate
                break
    if exact.is_file():
        outcome = "transcript"
        digest = hashlib.sha256(exact.read_bytes()).hexdigest()
        fields.update(parse_transcript(exact))
    elif empty.is_file():
        outcome = "no_nonempty_ib"
    return outcome, digest, fields


def glob_escape(text):
    return re.sub(r"([*?\[\]])", r"[\1]", text)


def strace_ioctls(case_dir):
    """Count the CS ioctls the syscall witness saw for this case."""
    trace = case_dir / "ioctl.strace"
    if not trace.is_file():
        return ""
    count = 0
    for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
        if "0xc0206466" in line:
            count += 1
    return str(count)


def shard_rows(out_dir, capture_root):
    """Every case of one shard as a row, joined across its three artifacts."""
    receipt_path = out_dir / "receipt.json"
    receipt = {}
    if receipt_path.is_file():
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        except ValueError:
            receipt = {}
    partition = receipt.get("partition") or {}
    slice_name = partition.get("slice") or "unknown"
    shard = out_dir.name
    directories = receipt.get("case_directories") or {}
    capture_dir = None
    if capture_root is not None:
        candidate = capture_root / shard
        capture_dir = candidate if candidate.is_dir() else capture_root
    cases_root = out_dir / "cases"
    if directories:
        pairs = sorted(directories.items())
    elif cases_root.is_dir():
        pairs = sorted((path.name, path.name) for path in cases_root.iterdir())
    else:
        pairs = []
    for case, directory in pairs:
        case_dir = cases_root / directory
        status, message = result_of(case_dir / "run.qpa")
        outcome, digest, fields = capture_state(capture_dir, case)
        yield {
            "slice": slice_name,
            "shard": shard,
            "case": case,
            "deqp_status": status or "absent",
            "refusal_site": refusal_site(message),
            "refusal_predicate": message or "",
            "planning_outcome": outcome,
            "transcript_sha256": digest,
            "cs_ioctls": strace_ioctls(case_dir),
            **fields,
        }


def write_table(path, header, rows):
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(header)
        for row in rows:
            writer.writerow([row.get(name, "") for name in header])


def census(out_dirs, capture_root, prefix):
    rows = []
    for out_dir in out_dirs:
        rows.extend(shard_rows(out_dir, capture_root))
    rows.sort(key=lambda row: (row["slice"], row["shard"], row["case"]))
    write_table(f"{prefix}-cases.tsv", CASE_TABLE_HEADER, rows)

    refusals = collections.defaultdict(
        lambda: {"cases": 0, "slices": set(), "example": None}
    )
    for row in rows:
        if row["planning_outcome"] == "transcript":
            continue
        entry = refusals[(row["refusal_site"], row["deqp_status"])]
        entry["cases"] += 1
        entry["slices"].add(row["slice"])
        if entry["example"] is None:
            entry["example"] = row
    refusal_rows = [
        {
            "refusal_site": site,
            "deqp_status": status,
            "cases": entry["cases"],
            "slices": " ".join(sorted(entry["slices"])),
            "example_case": entry["example"]["case"],
            "example_predicate": entry["example"]["refusal_predicate"],
        }
        for (site, status), entry in refusals.items()
    ]
    refusal_rows.sort(key=lambda row: (-row["cases"], row["refusal_site"]))
    write_table(f"{prefix}-first-refusal.tsv", FIRST_REFUSAL_HEADER, refusal_rows)

    outcomes = collections.defaultdict(
        lambda: {"cases": 0, "slices": set(), "example": None}
    )
    for row in rows:
        entry = outcomes[row["planning_outcome"]]
        entry["cases"] += 1
        entry["slices"].add(row["slice"])
        if entry["example"] is None:
            entry["example"] = row["case"]
    outcome_rows = [
        {
            "planning_outcome": outcome,
            "cases": entry["cases"],
            "slices": " ".join(sorted(entry["slices"])),
            "example_case": entry["example"],
        }
        for outcome, entry in outcomes.items()
    ]
    outcome_rows.sort(key=lambda row: (-row["cases"], row["planning_outcome"]))
    write_table(f"{prefix}-nonempty-ib.tsv", NONEMPTY_IB_HEADER, outcome_rows)
    return rows, refusal_rows, outcome_rows


def selftest():
    """Calibrate the join against a shard whose every outcome is known."""
    with tempfile.TemporaryDirectory() as work:
        root = Path(work)
        out = root / "s0"
        cases = out / "cases"
        capture = root / "capture" / "s0"
        capture.mkdir(parents=True)
        names = {
            "dEQP-VK.fake.transcript": "Pass",
            "dEQP-VK.fake.declined": "NotSupported",
            "dEQP-VK.fake.declined_too": "NotSupported",
            "dEQP-VK.fake.refused": "Fail",
            "dEQP-VK.fake.silent": None,
        }
        messages = {
            "dEQP-VK.fake.transcript": "ok",
            "dEQP-VK.fake.declined": (
                "VK_FAKE_extension is not supported at vktTestCase.cpp:1389"
            ),
            "dEQP-VK.fake.declined_too": (
                "VK_OTHER_extension is not supported at vktTestCase.cpp:1389"
            ),
            "dEQP-VK.fake.refused": (
                "vk.createImage(...): VK_ERROR_UNKNOWN at vkRefUtilImpl.inl:1"
            ),
        }
        for case, status in names.items():
            case_dir = cases / case
            case_dir.mkdir(parents=True)
            body = '<?xml version="1.0"?>\n'
            if status is not None:
                body += (
                    f'<Result StatusCode="{status}">{messages[case]}</Result>\n'
                )
            (case_dir / "run.qpa").write_text(body)
            (case_dir / "ioctl.strace").write_text("")
        (capture / "dEQP-VK.fake.transcript.plan").write_text(
            "r3v-native-conformance-plan\t1\n"
            "submission_count\t1\n"
            "submission\t0\tdeadbeef\t231\ttriangle\tr3v\t3\n"
            "reloc\t0\t0\tvertex\t2\t0\t4096\tr\n"
            "reloc\t0\t1\tcolor\t0\t2\t263168\tw\n"
            "reloc\t0\t2\tcompletion\t0\t2\t4\tw\n"
        )
        for case in (
            "dEQP-VK.fake.declined",
            "dEQP-VK.fake.declined_too",
            "dEQP-VK.fake.refused",
        ):
            (capture / f"{case}.plan.no_nonempty_ib").write_text("")
        (out / "receipt.json").write_text(
            json.dumps(
                {
                    "partition": {"slice": "fake"},
                    "case_directories": {name: name for name in names},
                }
            )
        )
        rows, refusals, outcomes = census(
            [out], root / "capture", str(root / "census")
        )

        by_case = {row["case"]: row for row in rows}
        transcript = by_case["dEQP-VK.fake.transcript"]
        assert transcript["planning_outcome"] == "transcript", transcript
        assert transcript["ib_dwords"] == "231"
        assert transcript["cell_kind"] == "triangle"
        assert transcript["emitter"] == "r3v"
        assert transcript["reloc_count"] == "3"
        assert transcript["bo_roles"] == "vertex color completion"
        assert len(transcript["transcript_sha256"]) == 64

        declined = by_case["dEQP-VK.fake.declined"]
        assert declined["planning_outcome"] == "no_nonempty_ib"
        assert declined["refusal_site"] == "vktTestCase.cpp:1389"
        refused = by_case["dEQP-VK.fake.refused"]
        assert refused["refusal_site"] == "vkRefUtilImpl.inl:1"
        assert refused["deqp_status"] == "Fail"

        # A case whose QPA carries no result element reports the absence; a
        # census that named a predicate here would be inventing one.
        silent = by_case["dEQP-VK.fake.silent"]
        assert silent["deqp_status"] == "absent"
        assert silent["refusal_site"] == "unreported"
        assert silent["planning_outcome"] == "unresolved"

        # A transcript-bearing case is not a refusal, so it leaves the
        # first-refusal census untouched.
        sites = {row["refusal_site"] for row in refusals}
        assert "vktTestCase.cpp:1389" in sites and len(sites) == 3, sites
        assert sum(row["cases"] for row in refusals) == 4

        counts = {row["planning_outcome"]: row["cases"] for row in outcomes}
        assert counts == {"no_nonempty_ib": 3, "transcript": 1, "unresolved": 1}
        # The census ranks by population, so the family worth implementing
        # next reads off the first row: two cases share one site here and the
        # other two sites carry one case apiece.
        assert refusals[0]["refusal_site"] == "vktTestCase.cpp:1389"
        assert refusals[0]["cases"] == 2
        assert [row["cases"] for row in refusals] == [2, 1, 1]
    print("OK    planning census: transcript, refusal, absent-result, and ranking")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--out-dir", type=Path, action="append", default=[])
    parser.add_argument("--capture-root", type=Path)
    parser.add_argument("--report-prefix")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.out_dir or not args.report_prefix:
        parser.error("--out-dir and --report-prefix are required")
    rows, refusals, outcomes = census(
        args.out_dir, args.capture_root, args.report_prefix
    )
    for row in outcomes:
        print(f"{row['planning_outcome']}\t{row['cases']}\t{row['slices']}")
    print(f"cases {len(rows)}, refusal sites {len(refusals)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
