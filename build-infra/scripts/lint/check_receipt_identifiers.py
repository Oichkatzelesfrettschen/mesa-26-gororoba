#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hold receipt identifiers in the hardware documents to retained bundles.

A receipt-shaped identifier (``r3v-...-rs482``, ``...-workstation``,
``...-host-model``, or the ``...-vostro1000_rs485m_5974`` scheme) in
``docs/hardware/*.md`` is a claim that a sealed bundle of that name exists
in the steinmarder-r300 evidence checkout.  This check resolves every
such identifier against that checkout's retained roots; a name that does
not resolve fails unless the same line carries the
``implemented_unreceipted`` marker, which states the pending receipt
instead of naming one.

The sibling checkout lives beside this repository or at
``R300_EVIDENCE_ROOT``.  Without it the check reports ``not run`` and
exits 0 in ordinary builds; under ``--require-evidence`` (the
qualification profiles) the absent checkout fails.

Usage:
  check_receipt_identifiers.py [--repo-root DIR] [--evidence-root DIR]
                               [--require-evidence]
  check_receipt_identifiers.py --self-test
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

RECEIPT_IDENTIFIER = re.compile(
    r"\br3v-[a-z0-9-]+?-(?:rs482|workstation|host-model|"
    r"vostro1000_rs485m_5974)(?![\w-])"
)
PENDING_MARKER = "implemented_unreceipted"
RETAINED_ROOTS = (Path("src/re/r300/results"), Path("results"))
DOC_GLOB = "docs/hardware/*.md"


class ReceiptIdentifierError(Exception):
    """A receipt identifier names no retained bundle."""


def evidence_root(repo_root: Path, override: Path | None) -> Path | None:
    candidates = []
    if override is not None:
        candidates.append(override)
    env = os.environ.get("R300_EVIDENCE_ROOT")
    if env:
        candidates.append(Path(env))
    candidates.append(repo_root.parent / "steinmarder-r300")
    # A linked worktree keeps its administrative directory under the
    # primary checkout's .git, so the sibling resolves from there.
    common = subprocess.run(
        ["git", "-C", str(repo_root), "rev-parse", "--git-common-dir"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False)
    if common.returncode == 0 and common.stdout.strip():
        primary = (repo_root / common.stdout.strip()).resolve().parent
        candidates.append(primary.parent / "steinmarder-r300")
    for candidate in candidates:
        if all((candidate / root).is_dir() for root in RETAINED_ROOTS):
            return candidate
    return None


def bundle_names(evidence: Path) -> frozenset[str]:
    names = set()
    for root in RETAINED_ROOTS:
        for entry in (evidence / root).iterdir():
            if entry.is_dir():
                names.add(entry.name)
    return frozenset(names)


def unresolved(text: str, path: str, names: frozenset[str]) -> list[str]:
    findings = []
    for number, line in enumerate(text.splitlines(), start=1):
        if PENDING_MARKER in line:
            continue
        for match in RECEIPT_IDENTIFIER.finditer(line):
            name = match.group(0)
            if name not in names:
                findings.append(f"{path}:{number}: receipt identifier "
                                f"{name!r} names no retained bundle")
    return findings


def check(repo_root: Path, evidence: Path) -> list[str]:
    names = bundle_names(evidence)
    findings: list[str] = []
    for doc in sorted(repo_root.glob(DOC_GLOB)):
        rel = doc.relative_to(repo_root).as_posix()
        findings.extend(unresolved(doc.read_text(encoding="utf-8"), rel, names))
    return findings


def self_test() -> int:
    names = frozenset({"r3v-native-real-receipt-rs482"})
    good = "the bundle r3v-native-real-receipt-rs482 holds it\n"
    bad = "the bundle r3v-native-imagined-receipt-rs482 holds it\n"
    pending = ("| row | implemented_unreceipted: the "
               "r3v-native-imagined-receipt-rs482 name is not reserved |\n")
    new_scheme = "r3v-native-flat-receipt-vostro1000_rs485m_5974\n"
    if unresolved(good, "d.md", names):
        raise ReceiptIdentifierError("self-test refused a retained name")
    if not unresolved(bad, "d.md", names):
        raise ReceiptIdentifierError("self-test admitted an unretained name")
    if unresolved(pending, "d.md", names):
        raise ReceiptIdentifierError("self-test refused the pending marker")
    if not unresolved(new_scheme, "d.md", names):
        raise ReceiptIdentifierError("self-test missed the vostro1000 scheme")
    print("check_receipt_identifiers: self-test OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parents[3])
    parser.add_argument("--evidence-root", type=Path)
    parser.add_argument("--require-evidence", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        evidence = evidence_root(args.repo_root.resolve(), args.evidence_root)
        if evidence is None:
            message = ("check_receipt_identifiers: not run; no steinmarder-r300 "
                       "evidence checkout beside the repository or at "
                       "R300_EVIDENCE_ROOT")
            if args.require_evidence:
                print(message, file=sys.stderr)
                return 1
            print(message)
            return 0
        findings = check(args.repo_root.resolve(), evidence)
        for finding in findings:
            print(finding, file=sys.stderr)
        if findings:
            return 1
        print(f"check_receipt_identifiers: every receipt identifier in "
              f"{DOC_GLOB} resolves under {evidence}")
        return 0
    except (ReceiptIdentifierError, OSError) as error:
        print(f"check_receipt_identifiers: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
