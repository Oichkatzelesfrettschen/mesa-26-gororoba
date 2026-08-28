#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Refusal-reason ledger for the direct SPIR-V admitters.

Each admitter names the construct it refuses through a string literal
(`refuse(r, "...")` or `*reason = "..."`).  The ledger beside this script
lists every reason the two sources carry, one per line, so the admitted
grammar's rejection surface is enumerable and a new or removed reason
reports as a ledger movement.  The checker extracts the literals from the
sources, joining adjacent string pieces, and compares the set with the
ledger; `--selftest` proves a missing, extra, and duplicated row each fail.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REFUSE_RE = re.compile(
    r'(?:refuse\(r,|\*reason =)\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', re.S)
PIECE_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

STATUS_OK = 0
STATUS_MISMATCH = 1
STATUS_USAGE = 2


def source_reasons(text: str) -> set[str]:
    reasons: set[str] = set()
    for match in REFUSE_RE.finditer(text):
        reasons.add("".join(PIECE_RE.findall(match.group(1))))
    return reasons


def ledger_rows(text: str) -> tuple[list[str], list[str]]:
    rows: list[str] = []
    failures: list[str] = []
    seen: set[str] = set()
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        if line in seen:
            failures.append(f"ledger line {number}: duplicate row {line!r}")
        seen.add(line)
        rows.append(line)
    return rows, failures


def check(sources: list[str], ledger: str) -> list[str]:
    observed: set[str] = set()
    for text in sources:
        observed |= source_reasons(text)
    rows, failures = ledger_rows(ledger)
    listed = set(rows)
    for reason in sorted(observed - listed):
        failures.append(f"source reason has no ledger row: {reason!r}")
    for reason in sorted(listed - observed):
        failures.append(f"ledger row names no source reason: {reason!r}")
    if not observed:
        failures.append("no refusal reason found in the sources")
    return failures


def selftest(sources: list[str], ledger: str) -> int:
    ok = True

    def expect(label: str, failures: list[str], marker: str) -> None:
        nonlocal ok
        if not any(marker in line for line in failures):
            print(f"selftest {label}: expected a failure naming {marker!r}, "
                  f"got {failures!r}")
            ok = False

    if check(sources, ledger):
        print(f"selftest known-good: {check(sources, ledger)!r}")
        ok = False
    rows, _ = ledger_rows(ledger)
    expect("missing-row", check(sources, "\n".join(rows[1:])),
           "source reason has no ledger row")
    expect("extra-row", check(sources, ledger + "\nnever emitted\n"),
           "ledger row names no source reason")
    expect("duplicate-row", check(sources, ledger + "\n" + rows[0] + "\n"),
           "duplicate row")
    expect("new-source-reason",
           check(sources + ['refuse(r, "fresh reason");'], ledger),
           "'fresh reason'")
    if not ok:
        return STATUS_MISMATCH
    print("r3v_spirv_refusal_reasons: known-good, missing-row, extra-row, "
          "duplicate-row, and new-source-reason verdicts calibrated")
    return STATUS_OK


def main(argv: list[str]) -> int:
    args = list(argv[1:])
    run_selftest = False
    if args and args[0] == "--selftest":
        run_selftest = True
        args = args[1:]
    if len(args) < 2:
        print("usage: r3v_spirv_refusal_reasons.py [--selftest] <ledger> "
              "<source>...", file=sys.stderr)
        return STATUS_USAGE
    ledger = Path(args[0]).read_text(encoding="utf-8")
    sources = [Path(p).read_text(encoding="utf-8") for p in args[1:]]
    if run_selftest:
        return selftest(sources, ledger)
    failures = check(sources, ledger)
    if failures:
        print("\n".join(failures))
        return STATUS_MISMATCH
    print(f"r3v_spirv_refusal_reasons: {len(ledger_rows(ledger)[0])} "
          "reasons match the ledger")
    return STATUS_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv))
